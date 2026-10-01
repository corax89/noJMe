/*
 * J2ME Emulator - SDL2 Backend
 * Cross-platform graphics, input, and audio using SDL2
 * Falls back to stub implementation if SDL2 is not available
 */

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <limits.h> /* v35.09 GAMEKEYS sentinel INT_MIN */
#include <string.h>
#include <stdint.h>
#ifndef _WIN32
#include <unistd.h>
#else
#include <windows.h>
#endif
#include <time.h>

/* Try to include SDL2 - support multiple header locations */
#if defined(HAVE_SDL2) || \
    defined(__has_include)
#  if __has_include(<SDL2/SDL.h>)
#    define SDL2_AVAILABLE 1
#    include <SDL2/SDL.h>
#  elif __has_include(<SDL.h>)
#    define SDL2_AVAILABLE 1
#    include <SDL.h>
#  else
#    define SDL2_AVAILABLE 0
#  endif
#else
/* Assume SDL2 might be available, try to include */
#  if defined(_WIN32) || defined(__MINGW32__)
#    define SDL2_AVAILABLE 1
#    include <SDL.h>
#  else
#    define SDL2_AVAILABLE 0
#  endif
#endif

/* v34.86 FIX (pre-existing since the v34.84 key-event refactor): with
 * SDL2_AVAILABLE == 0 the shared helper sdl_handle_key_event() still
 * referenced the SDL_Keycode type in its signature, so `make app` on a
 * host without SDL2 dev headers died with "unknown type name
 * 'SDL_Keycode'" (line ~802). Give the no-SDL no-op backend a dummy
 * typedef so the documented "build WITHOUT SDL" path compiles again. */
#if !SDL2_AVAILABLE
typedef int SDL_Keycode;
#endif

/* v34.88 FIX (devkitA64 "implicit declaration of 'sdl_switch_game_begin'"
* at the sdl_init call site + "'sdl_handle_key_event' defined but not used"):
* both diagnostics together mean this file was compiled WITHOUT any SDL2
* headers on the include path, so the __SWITCH__ glue section (which pulls
* switch/switch_glue.h — the declaration) was compiled out while the
* #ifdef __SWITCH__ call sites stayed. On the Switch, SDL2 is MANDATORY
* (the whole frontend is SDL2): fail loud and early with the actual cause
* instead of a cascade of unrelated errors. See the Makefile v34.88 note
* (PORTLIBS must point at $(DEVKITPRO)/portlibs/switch). */
#if defined(__SWITCH__) && !SDL2_AVAILABLE
#  error "__SWITCH__ build: SDL2 headers not found on the include path. The Switch frontend requires switch-SDL2 (dkp-pacman -S switch-SDL2). Check the PORTLIBS/SDL2 discovery in the Makefile (v34.88)."
#endif

#include "sdl_backend.h"
#include "midp.h"
#include "midi.h"
#include "debug.h"
#include "debug_macros.h"
/* v34.91 freeze triage: stage breadcrumbs — audio-open is a classic
 * emulator hang point, so the shared audio path traces too. Macro no-op
 * when neither __SWITCH__ nor NOJME_SWITCH_TRACE is defined. */
#include "switch/switch_trace.h"

/* Global context */
static SdlContext* g_sdl_ctx_ptr = NULL;

/* Mutex for thread-safe SDL access (only needed when the SDL2 backend is
 * active; without SDL2 the lock macros are never expanded, and defining the
 * mutex unconditionally trips -Wunused-variable) */
#ifdef _WIN32
#include <windows.h>
#endif

#if SDL2_AVAILABLE
#ifdef _WIN32
static CRITICAL_SECTION g_sdl_mutex;
static bool g_sdl_mutex_initialized = false;
#define SDL_LOCK() EnterCriticalSection(&g_sdl_mutex)
#define SDL_UNLOCK() LeaveCriticalSection(&g_sdl_mutex)
#else
#include <pthread.h>
static pthread_mutex_t g_sdl_mutex = PTHREAD_MUTEX_INITIALIZER;
#define SDL_LOCK() pthread_mutex_lock(&g_sdl_mutex)
#define SDL_UNLOCK() pthread_mutex_unlock(&g_sdl_mutex)
#endif
#endif /* SDL2_AVAILABLE */

#if SDL2_AVAILABLE
static SDL_Window* g_window = NULL;
static SDL_Renderer* g_renderer = NULL;
static SDL_Texture* g_texture = NULL;
/* v36.14: Scale2x-текстура (NEON-апскейл x2 кадра на CPU). Создаётся
 * только когда эффективный фильтр сессии = NOJME_FILTER_SCALE2X;
 * финальный fit в g_game_rect остаётся на GPU (linear — итоговая
 * пропорция обычно нецелая, nearest на 2x-картинке мерцает). */
static SDL_Texture* g_s2x_tex = NULL;
static uint32_t*    g_s2x_buf = NULL;
static int          g_s2x_w = 0, g_s2x_h = 0;
static SDL_AudioDeviceID g_audio_dev = 0;

/* v34.94: audio-queue backpressure cap + device handle lock (defined here
 * so every audio function below sees them; see sdl_audio_queue_samples for
 * the freeze rationale). */
#include <pthread.h>
#define NOJME_AUDIO_QUEUE_CAP_BYTES (192 * 1024)  /* ~0.55 s @44.1 kHz stereo S16 */
static pthread_mutex_t g_audio_dev_mutex = PTHREAD_MUTEX_INITIALIZER;

/* v34.94: current game jar (set by main.c at session start) — the PLUS
 * per-game settings overlay keys off this; per-game overrides
 * (-1 = inherit the global setting). */
char g_switch_current_jar[1024] = {0};
int g_pg_scale = -1;
int g_pg_filter = -1;
/* v36.16: per-game режим флипа 3D (-1 = наследовать глобальную настройку;
 * иначе NOJME_FLIP_*). Ставится main.c при старте сессии, обновляется
 * ЖИВО из PLUS-меню (строка "Флип 3D"); эффективный режим пересчитывается
 * в каждом present (см. sdl_switch_present_game) и пишется в
 * g_nojme_flip_mode (mobile3d.c) — так переключение флипа влияет на
 * рендер немедленно, без перезапуска игры. */
int g_pg_flip = -1;
/* v36.15: per-game поворот экрана (0 = выкл, 1 = 90 вправо/CW,
 * 2 = 90 влево/CCW). Ставится main.c при старте сессии из
 * SwitchPerGameSettings.rotation; наследовать нечего. */
int g_pg_rotation = 0;
/* v36.37: per-game стик/D-pad swap (-1 = наследовать глобальную настройку,
 * иначе NOJME_SWAP_*). Ставится main.c при старте сессии, обновляется ЖИВО
 * из PLUS-меню (строка «Стик и D-pad»); читается при КАЖДОМ событии
 * стика/D-pad (sdl_dpad_path_digits ниже), поэтому применяется без
 * перезапуска. (Ранее: v36.36 g_pg_dpad — единая раскладка обоих путей.) */
int g_pg_input_swap = -1;

/* v36.15: состояние поворота сессии. g_rot_mode печатаются в геометрию
 * текстур при game_begin (следующий запуск игры), g_rot_buf — staging
 * буфер кадра (w x h, как канва). Ротация — презентационная: канва игры
 * не меняется, кадр поворачивается софтом (как в libretro v34.48),
 * touch-координаты отображаются обратно.
 * v36.27: g_rot_mode теперь применяется ЖИВО — sdl_switch_reapply_
 * presentation() переставляет staging/текстуры прямо из меню паузы
 * (VM запаркована, present с frame-потоком не соревнуется). */
static int g_rot_mode = 0;           /* 0/1/2 — эффективный режим сессии */
static uint32_t* g_rot_buf = NULL;   /* w*h px staging (NULL = ротация выкл) */

/* v36.27: размеры канвы АКТИВНОЙ сессии. game_begin получает их
 * параметрами; горячий поворот (reapply) вызывает БЕЗ параметров —
 * берёт отсюда. Обнуляются в game_end. */
static int s_sess_fb_w = 0, s_sess_fb_h = 0;

/* Key mapping tables
 * 
 * ИСПРАВЛЕНО: Теперь отправляем правильные keyCode по стандарту Nokia FullCanvas:
 * - UP: keyCode = -1 (FULL_UP)
 * - DOWN: keyCode = -2 (FULL_DOWN)
 * - LEFT: keyCode = -3 (FULL_LEFT)
 * - RIGHT: keyCode = -4 (FULL_RIGHT)
 * - FIRE: keyCode = -5 (FULL_FIRE)
 * 
 * game_action используется для getGameAction() конверсии.
 * Многие игры (например Bobby Carrot) проверяют keyCode напрямую,
 * а не через getGameAction(), поэтому keyCode должен быть правильным.
 */
static const struct {
    SDL_Keycode sdl_key;
    int midp_key;       /* keyCode отправляемый в keyPressed() */
    int game_action;    /* game action для getGameAction() */
} key_map[] = {
    /* Цифры - отправляем ASCII коды */
    {SDLK_0, '0', 0}, {SDLK_1, '1', 0}, {SDLK_2, '2', 0},
    {SDLK_3, '3', 0}, {SDLK_4, '4', 0}, {SDLK_5, '5', 0},
    {SDLK_6, '6', 0}, {SDLK_7, '7', 0}, {SDLK_8, '8', 0},
    {SDLK_9, '9', 0}, {SDLK_ASTERISK, '*', 0}, {SDLK_HASH, '#', 0},
    /* Game keys - отправляем keyCode по стандарту Nokia FullCanvas */
    /* Игра Bobby Carrot проверяет: var1 == -5 || var1 == 5 || var1 == 53 для FIRE */
    {SDLK_UP, -1, GAME_UP},      /* keyCode=-1 (Nokia FULL_UP), gameAction=1 */
    {SDLK_DOWN, -2, GAME_DOWN},  /* keyCode=-2 (Nokia FULL_DOWN), gameAction=6 */
    {SDLK_LEFT, -3, GAME_LEFT},  /* keyCode=-3 (Nokia FULL_LEFT), gameAction=2 */
    {SDLK_RIGHT, -4, GAME_RIGHT},/* keyCode=-4 (Nokia FULL_RIGHT), gameAction=5 */
    {SDLK_RETURN, -5, GAME_FIRE},/* keyCode=-5 (Nokia FULL_FIRE), gameAction=8 */
    {SDLK_SPACE, -5, GAME_FIRE}, /* keyCode=-5 (Nokia FULL_FIRE), gameAction=8 */
    /* Soft buttons - F1 = Left Soft, F2 = Right Soft (по стандарту Nokia) */
    /* F1 отправляет keyCode=-6 (стандартный код левой софт-кнопки) */
    /* F2 отправляет keyCode=-7 (стандартный код правой софт-кнопки) */
    {SDLK_F1, -6, 0},            /* Left Soft Key: keyCode=-6 */
    {SDLK_F2, -7, 0},            /* Right Soft Key: keyCode=-7 */
    /* Game A/B/C/D - альтернативные клавиши (используются реже) */
    {SDLK_a, -6, GAME_A},        /* keyCode=-6, gameAction=9 */
    {SDLK_b, -7, GAME_B},        /* keyCode=-7, gameAction=10 */
    {SDLK_c, -8, GAME_C},        /* keyCode=-8, gameAction=11 */
    {SDLK_d, -9, GAME_D},        /* keyCode=-9, gameAction=12 */
    {SDLK_ESCAPE, 0, 0},
};
#define KEY_MAP_SIZE (sizeof(key_map) / sizeof(key_map[0]))

/* [KEYIN-MIDP] v36.45: SDL keycode -> MIDP keyCode, который РЕАЛЬНО уйдёт
 * игре (тот же key_map, что в sdl_handle_key_event). Для полевой трассы
 * диспетчеризации: «btn=0 -> key=13 midp=-5» без домыслов, что именно
 * увидела игра (до этого трейс показывал только SDL-код). */
static int switch_sdl_key_to_midp(SDL_Keycode key) {
    for (size_t i = 0; i < KEY_MAP_SIZE; i++)
        if (key_map[i].sdl_key == key) return key_map[i].midp_key;
    return 0;
}

/* Software key repeat for SDL build.
 * SDL2 doesn't generate key repeat events by default.
 * Many J2ME Canvas games rely on repeated keyPressed events for
 * continuous movement. This implements the same repeat behavior as
 * real J2ME phones: initial delay ~400ms, then repeat at ~80ms interval.
 */
#define SDL_KEY_REPEAT_DELAY_MS   400
#define SDL_KEY_REPEAT_INTERVAL_MS 80

/* v36.23 FIX ("в играх залипает кнопка вправо"): the repeat state was
 * indexed by (int)key + 128 clamped to [0,255]. SDL2 extended keycodes
 * (0x40000000 range) ALL clamped to the SAME slot 255 — the arrows
 * UP/DOWN/LEFT/RIGHT (and F1/F2/ESC) shared one repeat slot: holding
 * RIGHT generated keyRepeated(-1..-4) all together, a soft-key press
 * latched phantom arrow repeats, and a release swallowed elsewhere left
 * the slot latched = eternal repeats. The state is now indexed by
 * key_map POSITION — one private slot per mapped key. */
static struct {
    uint64_t press_time[KEY_MAP_SIZE];      /* When key was first pressed */
    uint64_t last_repeat_time[KEY_MAP_SIZE]; /* Last repeat event time */
    bool is_held[KEY_MAP_SIZE];             /* Key is currently held */
} g_sdl_key_repeat = {{0}, {0}, {false}};

/* v36.23: key_map slots held at pause-menu open (see the pause-menu key
 * flush block before sdl_switch_pause_menu). File-scope so that
 * sdl_switch_game_end can wipe it together with the repeat state. */
static uint32_t g_pause_held;

/* Map an SDL keycode to its key_map slot (-1 = unmapped: no repeat tracking) */
static int sdl_key_to_repeat_idx(SDL_Keycode key) {
    for (size_t i = 0; i < KEY_MAP_SIZE; i++)
        if (key_map[i].sdl_key == key) return (int)i;
    return -1;
}

/* Get current time in milliseconds */
static uint64_t sdl_get_time_ms(void) {
    return SDL_GetTicks();
}

/* Process software key repeat - call this after event handling */
static void sdl_process_key_repeat(SdlContext* ctx) {
    if (!ctx || !ctx->jvm) return;
    
    uint64_t now = sdl_get_time_ms();
    
    for (size_t i = 0; i < KEY_MAP_SIZE; i++) {
        SDL_Keycode key = key_map[i].sdl_key;
        int midp_key = key_map[i].midp_key;
        int game_action = key_map[i].game_action;
        
        /* Skip soft keys, escape, etc. - they should not repeat */
        if (key == SDLK_F1 || key == SDLK_F2 || key == SDLK_ESCAPE || key == SDLK_F12) {
            continue;
        }
        
        /* Skip if no valid key to send */
        if (!midp_key && !game_action) continue;
        
        
        if (g_sdl_key_repeat.is_held[i]) {
            uint64_t hold_duration = now - g_sdl_key_repeat.press_time[i];
            uint64_t since_last = now - g_sdl_key_repeat.last_repeat_time[i];
            
            if (hold_duration >= SDL_KEY_REPEAT_DELAY_MS &&
                since_last >= SDL_KEY_REPEAT_INTERVAL_MS) {
                /* Generate repeat event.
                 * FIX (audit M-3, v18): deliver as keyRepeated() on canvases
                 * that implement it, not as a phantom extra keyPressed(). */
                g_sdl_key_repeat.last_repeat_time[i] = now;
                int keycode = midp_key ? midp_key : game_action;
                midp_call_keyRepeated(ctx->jvm, keycode);
            }
        }
    }
}

/* Note: Audio uses SDL_QueueAudio() for queue-based playback.
 * No callback needed - samples are generated in media.c audio thread. */

#endif /* SDL2_AVAILABLE */

/*
 * switch_glue_impl.c.txt — the __SWITCH__ section of sdl_graphics.c.
 * Inserted by scripts/switch_patch_sdl.py after the key-repeat block.
 * Everything here is compiled ONLY when __SWITCH__ is defined (real
 * devkitPro build) or in the Linux verification build (same define).
 */
#if defined(__SWITCH__) && SDL2_AVAILABLE

#include "switch/switch_glue.h"
#include "switch/switch_common.h"
#include "switch/switch_scaling.h"
#include "switch/switch_font.h"
#include "switch/switch_ui.h" /* v34.94: per-game settings overlay (PLUS) */
#include "switch/switch_trace.h" /* v34.91: freeze triage (real impl: __SWITCH__/verify) */
#include "wildguard.h" /* [WILDGUARD] v36.50: мусорные указатели (Ryujinx InvalidAccess) */
#include "utils/battery.h" /* [BATFIX] v36.41: заряд для оверлея/трассы */

extern void midlet_call_destroy_app(JVM* jvm, bool unconditional);

/* defined below (extracted v34.84): shared keyboard-event handler the
 * gamepad path re-dispatches through */
static void sdl_handle_key_event(SdlContext* ctx, SDL_Keycode key, bool pressed,
                                 bool is_repeat);

/* ============================ state ============================ */

static SDL_Texture* g_ui_tex = NULL;      /* 1280x720 menu canvas */
static uint32_t* g_ui_canvas = NULL;      /* CPU side of the same */
static SDL_Texture* g_bg_tex = NULL;      /* small blurred background */
static uint32_t* g_bg_buf = NULL;
static int g_bg_w = 0, g_bg_h = 0;
static SDL_Rect g_game_rect = {0, 0, 0, 0};  /* last presented game rect */

/* ================= v36.18: счётчик кадров = полоса FPS =================
 * Показывает ТОЛЬКО FPS (номер кадра и возраст убраны по запросу),
 * обновляется ОДИН РАЗ В СЕКУНДУ. Значение — смены номера settled-кадра
 * за окно ~1000 мс (истинный темп игры, а не present-цикла 60 Гц);
 * FPS = кадры * 1000 / фактическая длительность окна. Замороженная
 * игра честно показывает 0 fps. Рисуется ОТДЕЛЬНОЙ текстурой в точке
 * (4,4) ПОЛНОГО экрана 1280x720 — пустая область СЛЕВА от игры (левый
 * верхний угол); если игра занимает угол и пустого места нет, полоса
 * оказывается на экране игры — допустимый фолбэк по требованию.
 * Между обновлениями текстура просто перерисовывается RenderCopy —
 * дёшево, без пересборки текста. */
#define FPS_STRIP_SCALE 2 /* 16px-шрифт x2: читаемо в левом поле 720p */
static uint32_t s_fps_last_seq = 0;      /* последний виденный settled-seq */
static int      s_fps_seq_seen = 0;      /* хоть один settled-кадр виден */
static uint64_t s_fps_win_start = 0;     /* мс начала текущего окна */
static uint32_t s_fps_win_count = 0;     /* settled-кадров в окне */
static int      s_fps_win_active = 0;    /* окно измерения запущено */
static unsigned s_fps_display = 0;       /* показанное значение FPS */
static int      s_fps_have_display = 0;  /* первое окно уже закрылось */
static SDL_Texture* s_fps_tex = NULL;    /* полоса с текстом FPS */
static uint32_t*    s_fps_buf = NULL;    /* CPU-канва полосы (ARGB8888) */
static int      s_fps_bw = 0, s_fps_bh = 0;
static int      s_fps_logged = 0;        /* одноразовый лог включения */

/* Сброс измерения (на стыке игровых сессий); текстуру не трогает. */
static void sdl_switch_fps_state_reset(void) {
    s_fps_last_seq = 0; s_fps_seq_seen = 0;
    s_fps_win_start = 0; s_fps_win_count = 0; s_fps_win_active = 0;
    s_fps_display = 0; s_fps_have_display = 0;
    s_fps_logged = 0;
}

/* Ленивое создание канвы/текстуры полосы (здесь уже есть g_renderer).
 * Размер фиксирован по самой широкой строке формата ("8888 fps") —
 * пересоздания при смене значения не нужны. */
static void sdl_switch_fps_strip_ensure(void) {
    if (s_fps_tex || !g_renderer) return;
    s_fps_bw = switch_font_text_width("8888 fps", FPS_STRIP_SCALE) + 10;
    s_fps_bh = switch_font_h() * FPS_STRIP_SCALE + 10;
    if (s_fps_bw < 32) s_fps_bw = 32;
    if (s_fps_bh < 32) s_fps_bh = 32;
    s_fps_buf = (uint32_t*)malloc(
        (size_t)s_fps_bw * s_fps_bh * sizeof(uint32_t));
    s_fps_tex = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ARGB8888,
                                  SDL_TEXTUREACCESS_STREAMING,
                                  s_fps_bw, s_fps_bh);
    if (s_fps_tex) SDL_SetTextureBlendMode(s_fps_tex, SDL_BLENDMODE_BLEND);
    if (!s_fps_buf || !s_fps_tex) {
        LOG_SAFE("[SWITCH] fps strip alloc failed (%dx%d) — counter off\n",
                 s_fps_bw, s_fps_bh);
        if (s_fps_tex) { SDL_DestroyTexture(s_fps_tex); s_fps_tex = NULL; }
        free(s_fps_buf); s_fps_buf = NULL;
    }
}

/* Перерисовать текст полосы (вызывается только при смене значения —
 * раз в секунду): прозрачный фон, 1px чёрная тень + ярко-зелёные глифы
 * — читаемо на любом содержимом (та же схема, что у старого оверлея). */
static void sdl_switch_fps_strip_draw(void) {
    if (!s_fps_tex || !s_fps_buf) return;
    if (!s_fps_logged) {
        s_fps_logged = 1;
        LOG_SAFE("[SWITCH] fps strip on: %dx%d at (4,4) full screen, "
                 "scale %d, update 1/s\n",
                 s_fps_bw, s_fps_bh, FPS_STRIP_SCALE);
    }
    memset(s_fps_buf, 0, (size_t)s_fps_bw * s_fps_bh * sizeof(uint32_t));
    char fbuf[16];
    snprintf(fbuf, sizeof(fbuf), "%u fps", s_fps_display);
    switch_font_draw_text(s_fps_buf, s_fps_bw, s_fps_bh,
                          3, 3, fbuf, 0xFF000000u, 0, FPS_STRIP_SCALE);
    switch_font_draw_text(s_fps_buf, s_fps_bw, s_fps_bh,
                          2, 2, fbuf, 0xFF40FF40u, 0, FPS_STRIP_SCALE);
    SDL_UpdateTexture(s_fps_tex, NULL, s_fps_buf,
                      s_fps_bw * sizeof(uint32_t));
}

/* v36.48 [KEYHINT-REMOVE]: панель «Кнопки в игре» (всплывала 8 секунд
 * в начале каждой игры, [KEY-HINT] v36.45) УДАЛЕНА по запросу
 * пользователя. Вся реализация убрана: BLEND-текстура + буфер,
 * ensure/redraw/reset/dismiss, отсчёт 8 с от первого кадра, гашение по
 * первой игровой клавише, перерисовка при смене языка. */

/* ================= v36.43 [PRESENT-GLASS]: стеклянка для полевых фризов ============
 * Полевой случай v36.42 (Yeti Sports): «основной экран завис, изображение
 * видно только в блюре за экраном, софт-клавиши не реагируют» — при ПОЛНОСТЬЮ
 * здоровых счётчиках эмулятора (fl>0 кадры оседают, lvc>0 канва живёт,
 * loop≈60fps, клавиши доставляются, td m/td t2 работают). Все прежние
 * инструменты ([NO-FRAME], STUCK, td) видят только ДО-SDL слой; отказ жил
 * между «кадр собран» и «кадр показан» — в SDL-рендерере/драйвере, и был
 * невидим. Три новых прибора закрывают этот слой (все дешёвые, без аллокаций
 * на горячем пути):
 *   1) rc-проверки каждого SDL_UpdateTexture/RenderCopy/RenderPresent в
 *      sdl_switch_present_game: utf=/rcf= в diag-строке (число отказов за
 *      период) + тротлинг-трейс [PRESENT-FAIL] с SDL_GetError(). Отказ
 *      загрузки текстуры = «вид игры замер на старом кадре»;
 *   2) [COMPOSITE-PROBE] heartbeat: раз в 5 с читаем ОДНУ строку готового
 *      композита (1280x1, y=360 — пересекает и блюр-поля, и вид игры) через
 *      SDL_RenderReadPixels и хэшируем; ch= в diag = сколько раз за период
 *      КАРТИНКА НА ЭКРАНЕ реально менялась, chf= = отказы чтения. При
 *      фризе: fl>0 && ch=0 => композит собирался, но на экран не попал —
 *      SDL/драйвер; fl>0 && ch>0 => менялся и на экране — отказ ещё ниже
 *      (свитч буферов/VI), эмулятор и SDL ни при чём;
 *   3) трейсы для stderr-только событий стадии (создание текстур,
 *      [SURFACE-RECREATE]) — на устройстве stderr не попадает в log.txt
 *      (класс баги [YSFIX2] v36.40), теперь они дублируются в sw_trace. */
extern volatile uint32_t g_sd_utf, g_sd_rcf, g_sd_ch, g_sd_chf;
extern volatile uint32_t g_sd_chg, g_sd_chb; /* v36.44 [SPLIT-PROBE] */

static void sdl_present_fail_note(const char* site, int rc, int is_update) {
    if (rc == 0) return;
    if (is_update) __sync_fetch_and_add(&g_sd_utf, 1);
    else           __sync_fetch_and_add(&g_sd_rcf, 1);
    static uint64_t s_last_note_ms = 0;
    uint64_t now = sdl_switch_ui_ticks_ms();
    if (now - s_last_note_ms >= 5000 || !s_last_note_ms) {
        s_last_note_ms = now;
        sw_trace_force("[PRESENT-FAIL] %s rc=%d: %.120s",
                       site, rc, SDL_GetError());
    }
}

/* [COMPOSITE-PROBE]: чтение готового композита (ДО RenderPresent — после
 * него задний буфер не определён). Строка y=360 покрывает левый блюр,
 * вид игры и правый блюр при любом масштабе. Чтение раз в 5 с — на
 * GPU-рендерере это один синк, на software — memcpy строки.
 * v36.44 [SPLIT-PROBE]: те же 5 строк хэшируются ДВАЖДЫ — колонки ВНУТРИ
 * g_game_rect (вид игры) и колонки ВНЕ его (блюр-поля/оверлеи). Полевой
 * фриз Yeti («вид замер, блюр жив») теперь различим побайтово:
 *   chg>0 — вид игры менялся;  chb>0 — фон/поля менялись;
 *   chg=0 chb>0 при fl>0 — контент игровой текстуры застрял (слой
 *   между present_src и текстурой: аплоад/Scale2x/snapshot);
 *   chg=0 chb=0 — композит целиком статичен (игра сама не рисует). */
static void sdl_composite_probe(uint64_t now_ms) {
    static uint64_t s_probe_last_ms = 0;
    if (!g_renderer) return;
    if (s_probe_last_ms && now_ms - s_probe_last_ms < 5000) return;
    s_probe_last_ms = now_ms;
    /* 5 строк, равномерно по вертикали: верх/низ вида игры и блюр-поля —
     * одна центральная строка пропускала контент, меняющийся только в
     * верхней трети канвы (репро Yeti: ch=0 при живой анимации). */
    uint32_t h = 2166136261u, hg = 2166136261u, hb = 2166136261u;
    uint32_t row[SWITCH_UI_WIDTH];
    int fails = 0;
    /* границы вида игры в колонках пробы (гейм-рект мог обновиться
     * домножением до целых пикселей — сверяемся с текущим g_game_rect) */
    const int gx0 = g_game_rect.x;
    const int gx1 = g_game_rect.x + g_game_rect.w;
    for (int k = 0; k < 5; k++) {
        SDL_Rect r = { 0, (SWITCH_UI_HEIGHT / 5) * k + SWITCH_UI_HEIGHT / 10,
                       SWITCH_UI_WIDTH, 1 };
        if (SDL_RenderReadPixels(g_renderer, &r, SDL_PIXELFORMAT_ARGB8888,
                                 row, sizeof(row)) != 0) {
            fails++;
            continue;
        }
        for (int i = 0; i < SWITCH_UI_WIDTH; i++) {
            uint32_t px = row[i];
            h ^= px; h *= 16777619u;
            /* v36.44: раздельные хэши «внутри вида» и «вне вида» */
            if (i >= gx0 && i < gx1) { hg ^= px; hg *= 16777619u; }
            else                     { hb ^= px; hb *= 16777619u; }
        }
    }
    if (fails == 5) {
        __sync_fetch_and_add(&g_sd_chf, 1);
        return;
    }
    static uint32_t s_prev_hash = 0, s_prev_hash_g = 0, s_prev_hash_b = 0;
    static int s_have = 0;
    if (s_have && h != s_prev_hash)
        __sync_fetch_and_add(&g_sd_ch, 1);
    if (s_have && hg != s_prev_hash_g)
        __sync_fetch_and_add(&g_sd_chg, 1);
    if (s_have && hb != s_prev_hash_b)
        __sync_fetch_and_add(&g_sd_chb, 1);
    s_prev_hash = h; s_prev_hash_g = hg; s_prev_hash_b = hb;
    s_have = 1;
}

/* [COMPOSITE-DUMP]: полный дамп композита в PPM (средство песочницы,
 * NOJME_COMPOSITE_DUMP_DIR; 1/с, лимит NOJME_COMPOSITE_DUMP_MAX=60) —
 * доказывает пикселями, что бейдж заряда/блюр/вид игры действительно
 * попали в ФИНАЛЬНЫЙ кадр (E2E-тест test_switch_battgame.sh). */
static void sdl_composite_dump_maybe(uint64_t now_ms) {
    static int s_dump_n = 0;
    static uint64_t s_dump_last_ms = 0;
    static uint32_t* s_dump_buf = NULL;
    const char* dd = getenv("NOJME_COMPOSITE_DUMP_DIR");
    if (!dd || !dd[0] || !g_renderer) return;
    int max_n = 60;
    {
        const char* mx = getenv("NOJME_COMPOSITE_DUMP_MAX");
        if (mx && mx[0]) max_n = atoi(mx);
    }
    if (s_dump_n >= max_n) return;
    if (s_dump_last_ms && now_ms - s_dump_last_ms < 1000) return;
    s_dump_last_ms = now_ms;
    if (!s_dump_buf) {
        s_dump_buf = (uint32_t*)malloc((size_t)SWITCH_UI_WIDTH *
                                       SWITCH_UI_HEIGHT * sizeof(uint32_t));
        if (!s_dump_buf) return;
    }
    /* v36.43: читаем ПОСТРОЧНО (720 чтений 1280x1), а не одним полным
     * прямоугольником: сплошной RenderReadPixels(NULL) на связке
     * dummy-видео + software-рендерер СЕГФОЛТИТСЯ внутри конверсии SDL
     * (воспроизведено на стенде; построчные чтения — тот же механизм,
     * что и рабочая [COMPOSITE-PROBE], каждое ограничено одной строкой).
     * Стоимость: 720 дешёвых вызовов раз в секунду — только в песочнице. */
    for (int y = 0; y < SWITCH_UI_HEIGHT; y++) {
        SDL_Rect rowr = { 0, y, SWITCH_UI_WIDTH, 1 };
        if (SDL_RenderReadPixels(g_renderer, &rowr, SDL_PIXELFORMAT_ARGB8888,
                                 s_dump_buf + (size_t)y * SWITCH_UI_WIDTH,
                                 SWITCH_UI_WIDTH * (int)sizeof(uint32_t)) != 0) {
            return;
        }
    }
    char p[512];
    snprintf(p, sizeof(p), "%s/comp_%04d.ppm", dd, s_dump_n++);
    FILE* f = fopen(p, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", SWITCH_UI_WIDTH, SWITCH_UI_HEIGHT);
    for (int i = 0; i < SWITCH_UI_WIDTH * SWITCH_UI_HEIGHT; i++) {
        uint32_t pxl = s_dump_buf[i];
        unsigned char rgb[3] = {
            (unsigned char)(pxl >> 16), (unsigned char)(pxl >> 8),
            (unsigned char)(pxl)
        };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static volatile int g_switch_game_active = 0; /* between game_begin/game_end */
static unsigned g_menu_held = 0;          /* held menu-key bitmask */

/* ================= [BATFIX] v36.41: индикатор заряда в кадре игры =================
 * Отдельная SDL-текстура (как полоса FPS), но ПРИЖАТАЯ К ПРАВОМУ ВЕРХНЕМУ
 * углу ПОЛНОГО экрана 1280x720 (запрос пользователя: «реализуй показ
 * заряда в правом верхнем углу»). Рисует в неё общий бейдж
 * switch_ui_batt_badge_draw (правый верхний угол ПЕРЕДАННОЙ канвы), поэтому
 * вид индикатора одинаков в игре, в меню и в паузе. Перерисовка — только
 * при смене значения (PSM кэшируется 2 c в utils/battery.c); между
 * обновлениями — дешёвый RenderCopy. Текстура живёт с первого включения
 * настройки до конца процесса (как s_fps_tex).
 * v36.43 [BATT-GAME] (полевой отчёт «в игре не виден»): RenderCopy теперь
 * прижат к САМОМУ углу экрана (x = 1280-W, y = 0) — бейдж внутри текстуры
 * и так отступает на свои 16/8px, поэтому в кадре игры он стоит РОВНО на
 * тех же пикселях экрана, что и в меню (раньше сдвиг (4,4) текстуры
 * смещал его к углу — на ТВ с небольшим оверсканом край экрана может
 * срезаться). Молния удалена ([BATT-DEBOLT]) — плашка стала уже. */
/* v36.43 [BATT-GAME] КОРЕНЬ «в игре не виден»: текстура была 150x60, а
 * switch_ui_batt_badge_draw СТРОГИЙ к размеру канвы (>=160x64 — защита
 * fill_rect от выхода за границы) и ТИХО возвращался, не нарисовав НИ
 * ПИКСЕЛЯ — трасса «battery: overlay ...» при этом честно срабатывала
 * (redraw вызван), и с v36.41 индикатор в игре был невидим. 160x64
 * вмещает оба режима с запасом: значок 72, «100%» 96. */
#define BATT_OVERLAY_W 160
#define BATT_OVERLAY_H 64
static SDL_Texture* s_batt_tex = NULL;
static uint32_t*    s_batt_buf = NULL;
static int          s_batt_bw = 0, s_batt_bh = 0;
static int          s_batt_mode = -1;  /* последний нарисованный режим */
static int          s_batt_pct = -2;   /* ...и процент/зарядка */
static int          s_batt_chg = -2;
static int          s_batt_logged = 0; /* одноразовая трасса включения */

/* Ленивое создание текстуры-оверлея (g_renderer уже есть). */
static void sdl_switch_batt_ensure(void) {
    if (s_batt_tex || !g_renderer) return;
    s_batt_bw = BATT_OVERLAY_W;
    s_batt_bh = BATT_OVERLAY_H;
    s_batt_buf = (uint32_t*)malloc(
        (size_t)s_batt_bw * s_batt_bh * sizeof(uint32_t));
    s_batt_tex = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ARGB8888,
                                  SDL_TEXTUREACCESS_STREAMING,
                                  s_batt_bw, s_batt_bh);
    if (s_batt_tex) SDL_SetTextureBlendMode(s_batt_tex, SDL_BLENDMODE_BLEND);
    if (!s_batt_buf || !s_batt_tex) {
        LOG_SAFE("[SWITCH] battery overlay alloc failed (%dx%d) — overlay off\n",
                 s_batt_bw, s_batt_bh);
        /* v36.43 [PRESENT-GLASS]: stderr не попадает в log.txt устройства —
         * дублируем в трейс, чтобы отказ был видён в поле */
        sw_trace_force("[BATT-OVERLAY] alloc failed (%dx%d) — индикатор в игре выключен",
                       s_batt_bw, s_batt_bh);
        if (s_batt_tex) { SDL_DestroyTexture(s_batt_tex); s_batt_tex = NULL; }
        free(s_batt_buf); s_batt_buf = NULL;
    }
}

/* Перерисовать содержимое текстуры (вызывается ТОЛЬКО при смене
 * режима/процента/зарядки). Фон — прозрачный, бейдж рисует свою плашку. */
static void sdl_switch_batt_redraw(int mode) {
    if (!s_batt_tex || !s_batt_buf) return;
    if (!s_batt_logged) {
        s_batt_logged = 1;
        LOG_SAFE("[SWITCH] battery overlay on: %s mode, %d%% charging=%d "
                 "(src %s), top-right corner, redraw on change\n",
                 mode == NOJME_BATT_ICON ? "icon" : "number",
                 nojme_battery_percent(), nojme_battery_charging(),
                 nojme_battery_source());
        sw_trace("battery: overlay %s %d%% charging=%d src=%s",
                 mode == NOJME_BATT_ICON ? "icon" : "number",
                 nojme_battery_percent(), nojme_battery_charging(),
                 nojme_battery_source());
    }
    memset(s_batt_buf, 0, (size_t)s_batt_bw * s_batt_bh * sizeof(uint32_t));
    switch_ui_batt_badge_draw(s_batt_buf, s_batt_bw, s_batt_bh, mode);

    SDL_UpdateTexture(s_batt_tex, NULL, s_batt_buf,
                      s_batt_bw * sizeof(uint32_t));
}

/* v34.89: RAW-JOYSTICK input replaces the GameController API.
 * switch-SDL2 exposes the pads as plain joysticks and often ships NO
 * GameController mapping: SDL_IsGameController() returns false, no
 * SDL_CONTROLLER* events are ever delivered and the frontend looks
 * completely dead (user report: controls do not work in an emulator).
 * Raw SDL_JOY* events are the devkitPro-example pattern that works on
 * real hardware AND in emulators. Joystick events only flow for OPENED
 * devices, so every pad is opened right after SDL_Init. */
#define SWITCH_JOY_SLOTS 4
static SDL_Joystick* g_joy[SWITCH_JOY_SLOTS];         /* opened pads */
static SDL_JoystickID g_joy_instance[SWITCH_JOY_SLOTS] = {-1,-1,-1,-1};
static volatile int g_switch_pause_req = 0;  /* v34.90: vestigial latch (nothing sets it
                                              * anymore - MINUS is handled inline by
                                              * the frame pump); kept for ABI/log parity */
static volatile int g_switch_pause_open = 0; /* pause menu is on screen */

/* Raw button indices of the switch-SDL2 joystick driver — libnx HID bit
 * order (A..MINUS are bits 0..11, D-pad L,U,R,D are bits 12..15). */
enum {
    SWJB_A = 0,  SWJB_B = 1,  SWJB_X = 2,  SWJB_Y = 3,
    SWJB_LSTICK = 4, SWJB_RSTICK = 5,
    SWJB_L = 6,  SWJB_R = 7,  SWJB_ZL = 8, SWJB_ZR = 9,
    SWJB_PLUS = 10, SWJB_MINUS = 11,
    SWJB_LEFT = 12, SWJB_UP = 13, SWJB_RIGHT = 14, SWJB_DOWN = 15
};

/* [YSFIX2] v36.40 [PAD-INFO]: identify the pad TYPE the same way the
 * switch-SDL2 driver itself does (hidGetNpadDeviceType/hidGetNpadStyleSet
 * — copied verbatim from its SDL_sysjoystick.c so the API surface is
 * identical to what the portlib already compiles against). WHY: single
 * Joy-Cons get a REMAPPED button table inside switch-SDL2
 * (pad_mapping_left_joy / pad_mapping_right_joy): the four cluster
 * buttons arrive as VIRTUAL A/B/X/Y (physical labels do NOT match the
 * virtual ids), SL/SR land in the ZL/ZR slots, and stick clicks/L/R are
 * dead (BIT(31)). A user holding one Joy-Con horizontally presses the
 * button LABELED "X"/"B" and the game receives FIRE/GAME_C instead of
 * the soft keys — exactly the "soft keys are dead" field report, while
 * pause (PLUS reports as MINUS) keeps working. The type string makes the
 * user's pad situation visible in every device log; the host-verify
 * build (no libnx) reports "host-verify".
 * Guard: only the REAL NRO build has <switch.h> — switchui-verify also
 * defines __SWITCH__ but runs on the host without libnx headers. */
#if defined(__SWITCH__) && defined(__has_include)
#  if __has_include(<switch.h>)
#    include <switch.h>
#    define NOJME_PADINFO_LIBNX 1
#  endif
#endif

static const char* switch_joy_pad_type_str(int device_index) {
#ifdef NOJME_PADINFO_LIBNX
    if (device_index >= 0 && device_index < 8) {
        /* Same idioms as the driver's SWITCH_JoystickInit/Update. */
        /* v36.42 [BATFIX2]: hid.h возвращает из обеих функций u32
         * (сверено с реальным libnx); приём в HidNpadStyleTag был
         * неявной int->enum конверсией. Битовые флаги HidNpadStyleTag_*
         * и HidDeviceTypeBits_* — константы enum, с u32 сочетаются. */
        u32 style = hidGetNpadStyleSet((HidNpadIdType)device_index);
        u32 type = hidGetNpadDeviceType((HidNpadIdType)device_index);
        if (!(style & HidNpadStyleTag_NpadJoyDual)) {
            if (type & HidDeviceTypeBits_JoyLeft)
                return "joycon-L-single(REMAPPED: cluster=A/B/X/Y, SL/ZL)";
            if (type & HidDeviceTypeBits_JoyRight)
                return "joycon-R-single(REMAPPED: cluster=A/B/X/Y, SL/ZL)";
        }
        if (style & HidNpadStyleTag_NpadJoyDual)
            return "joycon-pair";
        return "standard";
    }
    return "idx?";
#else
    (void)device_index;
    return "host-verify";
#endif
}

/* ---------------- v34.90 central input state ----------------
 * Single source of truth for ALL Switch input (see the full design comment
 * at switch_input_pump below). Declared before every consumer, filled only
 * by switch_input_pump() on the main/frame context. */
typedef struct {
    uint16_t held[SWITCH_JOY_SLOTS];    /* live button state (bit = SWJB_*) */
    uint16_t prev[SWITCH_JOY_SLOTS];    /* state at the previous pump */
    uint16_t latch[SWITCH_JOY_SLOTS];   /* JOYBUTTONDOWN events seen this pump */
    int      dir[SWITCH_JOY_SLOTS];     /* stick->dpad mask: 1=u 2=d 4=l 8=r */
    int      dir_prev[SWITCH_JOY_SLOTS];
    unsigned kb_edge;                   /* keyboard press edges this pump */
    unsigned kb_held;                   /* keyboard held bits */
    int      touch_x, touch_y;          /* window pixels (SWITCH_UI_*) */
    int      touch_down;
    int      touch_moved;               /* FINGERMOTION seen while down */
    /* v36.26 [TOUCH-UI]: pending tap edge (FINGERDOWN). Survives pumps
     * until a UI screen consumes it via sdl_switch_touch_tap() — the menu
     * pumps at its own cadence, so the edge cannot be tied to one pump. */
    int      touch_tap;
    int      touch_tap_x, touch_tap_y;
    int      quit;                      /* SDL_QUIT seen this pump */
} SwitchInputState;

static SwitchInputState g_sw_in;

static void switch_input_reset(void) {
    memset(&g_sw_in, 0, sizeof(g_sw_in));
}

/* Acknowledge the CURRENT held state without firing edges (pause-menu
 * open/close): a button held THROUGH the menu must not re-press when the
 * game resumes. */
static void switch_input_settle(void) {
    for (int i = 0; i < SWITCH_JOY_SLOTS; i++) {
        g_sw_in.prev[i] = g_sw_in.held[i];
        g_sw_in.latch[i] = 0;
        g_sw_in.dir_prev[i] = g_sw_in.dir[i];
    }
    g_sw_in.kb_edge = 0;
    g_sw_in.touch_moved = 0;
    g_sw_in.touch_tap = 0; /* v36.26: a settled press must not click */
}

/* Event instance-id -> slot (events only arrive for OPENED pads). */
static int switch_joy_slot(SDL_JoystickID id) {
    for (int i = 0; i < SWITCH_JOY_SLOTS; i++)
        if (g_joy[i] && g_joy_instance[i] == id) return i;
    return -1;
}

/* Open device `idx' into a free slot (idempotent for already-open pads). */
static void switch_joy_open_index(int idx) {
    if (idx < 0 || idx >= SDL_NumJoysticks()) return;
#if SDL_VERSION_ATLEAST(2, 0, 14)
    {   /* SDL2 queues SDL_JOYDEVICEADDED for pads present at SDL_Init —
         * skip the duplicate open when the direct init-time open won. */
        SDL_JoystickID iid = SDL_JoystickGetDeviceInstanceID(idx);
        if (iid >= 0 && switch_joy_slot(iid) >= 0) return;
    }
#endif
    int slot = -1;
    for (int i = 0; i < SWITCH_JOY_SLOTS; i++) if (!g_joy[i]) { slot = i; break; }
    if (slot < 0) return;
    g_joy[slot] = SDL_JoystickOpen(idx);
    if (g_joy[slot]) {
        SDL_JoystickID new_iid = SDL_JoystickInstanceID(g_joy[slot]);
        /* v34.90: if ANOTHER slot already holds this instance, the same
         * physical pad is being opened a second time (the init-time open
         * won and the queued SDL_JOYDEVICEADDED was processed on an SDL
         * older than 2.0.14, where the GetDeviceInstanceID guard above is
         * compiled out). A pad opened twice double-fires every event;
         * close the duplicate immediately. */
        for (int i = 0; i < SWITCH_JOY_SLOTS; i++) {
            if (i != slot && g_joy[i] && g_joy_instance[i] == new_iid) {
                SDL_JoystickClose(g_joy[slot]);
                g_joy[slot] = NULL;
                LOG_SAFE("[SWITCH] joystick device %d already open in slot %d - duplicate closed\n",
                         idx, i);
                return;
            }
        }
        g_joy_instance[slot] = new_iid;
        const char* nm = SDL_JoystickName(g_joy[slot]);
        /* [YSFIX2] v36.40 [PAD-INFO]: pad inventory INTO THE DEVICE LOG
         * (sw_trace; the LOG_SAFE twin below is host-stderr only). The
         * type= classification is the single-Joy-Con detector — see
         * switch_joy_pad_type_str(). */
        sw_trace("[PAD] slot=%d inst=%d type=%s name=%s",
                 slot, (int)new_iid, switch_joy_pad_type_str(idx),
                 nm ? nm : "(unnamed)");
        LOG_SAFE("[SWITCH] joystick slot %d (device %d) open: %s\n",
                 slot, idx, nm ? nm : "(unnamed)");
    } else {
        LOG_SAFE("[SWITCH] joystick slot %d (device %d) open FAILED\n", slot, idx);
    }
}

/* SDL_JOYDEVICEADDED / SDL_JOYDEVICEREMOVED (rails attach/detach, pads).
 * ADDED carries a device INDEX, REMOVED an instance id (SDL2 semantics). */
static void switch_joy_hotplug(const SDL_Event* e) {
    if (e->type == SDL_JOYDEVICEADDED) {
        switch_joy_open_index(e->jdevice.which);
        return;
    }
    if (e->type == SDL_JOYDEVICEREMOVED) {
        int slot = switch_joy_slot(e->jdevice.which);
        if (slot >= 0) {
            SDL_JoystickClose(g_joy[slot]);
            g_joy[slot] = NULL;
            g_joy_instance[slot] = -1;
            /* v34.90: clear the slot in the central input state as well */
            g_sw_in.held[slot] = g_sw_in.prev[slot] = g_sw_in.latch[slot] = 0;
            g_sw_in.dir[slot] = g_sw_in.dir_prev[slot] = 0;
            sw_trace("[PAD] slot=%d removed", slot); /* [YSFIX2] v36.40 */
            LOG_SAFE("[SWITCH] joystick slot %d removed\n", slot);
        }
    }
}

/* ======================= platform bootstrap ======================= */

int sdl_switch_platform_init(void) {
    static int done = 0;
    if (done) return g_window ? 0 : -1;
    done = 1;

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1"); /* linear filtering */
    /* v34.89: SDL_INIT_JOYSTICK, not GAMECONTROLLER — raw SDL_JOY* events
     * are the only input path that works on every switch-SDL2 setup.
     * v34.91: + SDL_INIT_TIMER — the freeze-triage heartbeat (see
     * switch_trace.c) rides an SDL timer thread. */
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS |
                 SDL_INIT_JOYSTICK | SDL_INIT_TIMER) != 0) {
        LOG_SAFE("[SWITCH] SDL_Init failed: %s\n", SDL_GetError());
        return -1;
    }
/* v36.43 [SANDBOX-WINDOW-FIX]: реальное NRO и так определяет __SWITCH__ от
 * devkitPro-тулчейна, а verify-сборка — явным -D__SWITCH__ на ХОСТОВОМ
 * компиляторе; прежняя проверка #ifdef __SWITCH__ отдавала verify-окну
 * SDL_WINDOW_FULLSCREEN, и на dummy-видео окно разворачивалось в размер
 * десктопа (1024x768) вместо 1280x720: правый верхний угол (бейдж заряда,
 * x>=1120) клипался вьюпортом и пропадал из композита, чтение полного
 * композита выходило за границы поверхности. Девайс отличаем по
 * архитектуре тулчейна: devkitPro всегда aarch64/arm, хост — нет. */
#if defined(__SWITCH__) && (defined(__aarch64__) || defined(__arm__) || defined(_ARM_))
    g_window = SDL_CreateWindow("J2ME Emulator",
                                SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                SWITCH_UI_WIDTH, SWITCH_UI_HEIGHT,
                                SDL_WINDOW_FULLSCREEN);
#else
    /* verification build: windowed, resizable off, same geometry */
    g_window = SDL_CreateWindow("nojme Switch UI (verify)",
                                SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                SWITCH_UI_WIDTH, SWITCH_UI_HEIGHT,
                                SDL_WINDOW_SHOWN);
#endif
    if (!g_window) {
        LOG_SAFE("[SWITCH] window failed: %s\n", SDL_GetError());
        return -1;
    }
    g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED);
    if (!g_renderer) {
        g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!g_renderer) {
        LOG_SAFE("[SWITCH] renderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(g_window);
        g_window = NULL;
        SDL_Quit();
        return -1;
    }
    /* v36.43 [PRESENT-GLASS]: ИМЯ рендерера — в поле. GPU-ускоренный и
     * software пути имеют РАЗНЫЕ классы отказов композита; до сих пор
     * лог устройства не сообщал, какой из них реально работает. */
    {
        SDL_RendererInfo ri;
        memset(&ri, 0, sizeof(ri));
        if (SDL_GetRendererInfo(g_renderer, &ri) == 0 && ri.name[0])
            sw_trace("[RND] renderer=%s", ri.name);
        else
            sw_trace("[RND] renderer=(unknown: %s)", SDL_GetError());
    }
    g_ui_tex = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ARGB8888,
                                 SDL_TEXTUREACCESS_STREAMING,
                                 SWITCH_UI_WIDTH, SWITCH_UI_HEIGHT);
    if (!g_ui_tex) {
        LOG_SAFE("[SWITCH] UI texture failed: %s\n", SDL_GetError());
        return -1;
    }
    SDL_SetTextureBlendMode(g_ui_tex, SDL_BLENDMODE_BLEND);
    g_ui_canvas = (uint32_t*)malloc((size_t)SWITCH_UI_WIDTH * SWITCH_UI_HEIGHT * 4);
    if (!g_ui_canvas) return -1;

    /* v34.89: open up to SWITCH_JOY_SLOTS raw pads — joystick events are
     * delivered only for OPENED devices (1 combined pad in handheld
     * mode, 2 when the joy-cons are paired separately, up to 4 pads). */
    for (int i = 0; i < SDL_NumJoysticks() && i < SWITCH_JOY_SLOTS; i++) {
        switch_joy_open_index(i);
    }
    if (SDL_NumJoysticks() <= 0) {
        LOG_SAFE("[SWITCH] no joystick detected (keyboard fallback active)\n");
    }
    LOG_SAFE("[SWITCH] platform init OK (%s renderer)\n",
             SDL_GetCurrentVideoDriver());
    return 0;
}

void sdl_switch_platform_shutdown(void) {
    /* v35.13 EXIT BREADCRUMBS: the user's device log ends at the last
     * session line when the exit path crashes — every teardown step now
     * names itself in the trace (sw_trace = raw write, survives crashes),
     * so the NEXT report pinpoints the faulting stage exactly. The
     * heartbeat timer stays alive until just before SDL_Quit() (the trace
     * sink is a raw fd, safe during the destroys; the v34.95 host-build
     * wedge involved stdio, not this path). */
    sw_trace("exit: sdl begin");
#define ET_MARK(name) do { sw_trace("exit: %s", name); } while (0)
    for (int i = 0; i < SWITCH_JOY_SLOTS; i++) {
        if (g_joy[i]) { SDL_JoystickClose(g_joy[i]); g_joy[i] = NULL; }
    }
    ET_MARK("joysticks closed");
    if (g_ui_tex) { SDL_DestroyTexture(g_ui_tex); g_ui_tex = NULL; }
    free(g_ui_canvas); g_ui_canvas = NULL;
    ET_MARK("texture freed");
    if (g_renderer) { SDL_DestroyRenderer(g_renderer); g_renderer = NULL; }
    ET_MARK("renderer destroyed");
    if (g_window) { SDL_DestroyWindow(g_window); g_window = NULL; }
    ET_MARK("window destroyed");
    sw_trace("exit: sdl teardown ok, quitting");
    sw_trace_shutdown();
    SDL_Quit();
#undef ET_MARK
}

/* ======================= menu canvas / input ======================= */

uint32_t* sdl_switch_ui_canvas(void) { return g_ui_canvas; }

static unsigned kb_key_bit(const SDL_Event* e) {
    switch (e->key.keysym.sym) {
        case SDLK_UP:    return SWK_UP;
        case SDLK_DOWN:  return SWK_DOWN;
        case SDLK_LEFT:  return SWK_LEFT;
        case SDLK_RIGHT: return SWK_RIGHT;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        case SDLK_SPACE: return SWK_A;
        case SDLK_ESCAPE:
        case SDLK_BACKSPACE: return SWK_B;
        case SDLK_q:     return SWK_EXIT;
        default: return 0;
    }
}

/* ==================== v34.90 SINGLE-PUMP input ====================
 * FORENSICS (user report: «кнопки сразу работали, но затем перестали, я так
 * и не смог попасть ни в один из пунктов меню — возможно ВМ перехватывает
 * события»): v34.89 still drained the SDL queue from TWO places during a
 * game — the frame pump (sdl_process_events) AND the game-thread pump
 * (sdl_process_events_minimal, called from thread_yield() every ~16k
 * opcodes MID-BYTECODE on a yielding VM fiber, plus from Thread.sleep).
 * The game-thread pump re-dispatched soft keys (X/B ->
 * midp_handle_soft_button), FIRE while a command menu was open and EVERY
 * touch event through paths that execute JAVA directly
 * (call_command_action / pointerPressed -> execute_method) RE-ENTRANTLY
 * from the middle of the yield — corrupting interpreter state and/or
 * leaving the command menu stuck open, after which it swallows every
 * FIRE/UP/DOWN («не смог попасть ни в один из пунктов меню»). That is
 * exactly «ВМ перехватывает события».
 *
 * FIX (the user's own suggestion — «лучше забирать их из одного места»):
 * ONE pump owns the SDL queue on __SWITCH__. switch_input_pump() runs only
 * on the main/frame context: the frontend menu loop, the frame pump, the
 * pause menu and the finished screen. The game thread (Thread.sleep /
 * thread_yield) NEVER touches SDL events — sdl_process_events_minimal()
 * is a no-op on __SWITCH__, so mid-bytecode re-entrant Java execution is
 * impossible by construction. Input reaches a game with at most one frame
 * (~16 ms) of latency.
 *
 * ROBUSTNESS (the «then they stopped» insurance): press edges are
 * EVENT-FIRST (SDL_JOYBUTTONDOWN latches — a tap shorter than one pump
 * interval still registers) plus STATE-REPAIR (SDL_JoystickGetButton
 * ground truth on every pump — lost, duplicated or swallowed events
 * self-heal on the next pump). Stick->dpad uses hysteresis (enter past
 * 0.50, exit below 0.35) so threshold jitter cannot spam direction edges. */
/* THE pump: drain the queue, refresh ground-truth pad state, derive edges.
 * Press edge  per button = (held | latch) & ~prev   — a full tap inside one
 * pump interval still counts (latch), a lost event still counts (state).
 * Release edge per button = ~held & (prev | latch).  All state lives in one
 * struct updated ONLY from the main/frame context — no locking needed. */
static void switch_frontend_keys_tick(void); /* v35.10 fwd: sandbox FRONT_KEYS */

/* v36.23: FRONT_KEYS script state — named type declared BEFORE the pump:
 * the ground-truth loop now seeds `held' with the synthetic hold mask. */
struct FkState {
    int      armed;       /* env seen and parsed */
    int      done;        /* script exhausted / malformed */
    int      pending_bit; /* parsed token waiting for its due time (-1 = none) */
    int      pending_hold; /* 0 = press edge, 1 = hold-ON, 2 = hold-OFF */
    uint16_t hold_bits;   /* v36.23: synthetic HELD mask (sandbox: no real pads) */
    uint64_t next_at;     /* when pending_bit fires */
    int      pos;         /* parse cursor */
    char     script[512]; /* own copy of the env (v36.26: TAP scripts are long) */
    /* v36.26 [TOUCH-UI]: pending TAP:x,y token (sandbox driver) */
    int      pending_tap;   /* 0 = none, 1 = tap token scheduled */
    int      pending_tx, pending_ty;
    int      tap_release;   /* touch_down=1 was injected — release on next tick */
};
static struct FkState g_fk;

static void switch_input_pump(void) {
    /* v36.60 [PUMP-GAP]: сторож «слепых окон» ввода. На Switch-SDL2
     * события геймпада ГЕНЕРИРУЮТСЯ только внутри SDL_PumpEvents/
     * PollEvent (выборка HID-состояния живёт в SDL_JoystickUpdate) —
     * если главный поток не качает очередь N миллисекунд, нажатие-
     * отпускание, ЦЕЛИКОМ попавшее в такое окно, НЕ становится
     * событием ВООБЩЕ (полевой случай v36.59: [PTRSCAN]-проход
     * PATHGUARD блокировал цикл меню на 1-3+ с раз в 2 с — «меню
     * тормозит и пропускает нажатия», в логе это пропажи [KEYIN]
     * между живыми сериями). Этот сторож превращает ЛЮБОЕ будущее
     * слепое окно >250 мс в именованную строку лога — до него такие
     * окна были невидимы. Рейт-лимит 1 строка / 10 с, чтобы потоп
     * сообщений сам не стал спамом. */
    {
        static uint64_t s_prev_pump_ms = 0;
        static uint64_t s_last_gap_rep_ms = 0;
        uint64_t nowp = (uint64_t)SDL_GetTicks();
        if (s_prev_pump_ms && nowp > s_prev_pump_ms &&
            nowp - s_prev_pump_ms > 250 &&
            (s_last_gap_rep_ms == 0 ||
             nowp - s_last_gap_rep_ms >= 10000)) {
            s_last_gap_rep_ms = nowp;
            sw_trace("[PUMP-GAP] %llu ms between input pumps — main-loop "
                     "stall (short press+release pairs may be LOST)",
                     (unsigned long long)(nowp - s_prev_pump_ms));
        }
        s_prev_pump_ms = nowp;
    }
    __sync_fetch_and_add(&g_sd_pump, 1); /* v34.94 diag */
    g_sw_in.quit = 0;
    g_sw_in.touch_moved = 0;
    g_sw_in.kb_edge = 0;
    for (int i = 0; i < SWITCH_JOY_SLOTS; i++) g_sw_in.latch[i] = 0;

    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
            case SDL_QUIT:
                g_sw_in.quit = 1;
                break;
            case SDL_JOYDEVICEADDED:
            case SDL_JOYDEVICEREMOVED:
                switch_joy_hotplug(&e);
                break;
            case SDL_JOYBUTTONDOWN:
                if (e.jbutton.button < 16) {
                    for (int i = 0; i < SWITCH_JOY_SLOTS; i++)
                        if (g_joy[i] && g_joy_instance[i] == e.jbutton.which)
                            g_sw_in.latch[i] |= (uint16_t)(1u << e.jbutton.button);
                    /* [YSFIX2] v36.40 [KEYIN-ALL]: raw SDL ground truth for
                     * EVERY button press, via sw_trace (the ysfix1 fprintf(stderr)
                     * traces NEVER reached log.txt on device — stderr is not
                     * redirected there, only the sw_trace sink writes the file;
                     * a full field log proved it: zero [KEYIN] lines while the
                     * user was pressing the soft keys). All-button coverage is
                     * required precisely because single Joy-Cons REMAP the
                     * button ids inside switch-SDL2: the press the user thinks
                     * is "X" can arrive as btn=0 (FIRE) — the trace makes the
                     * real pad layout visible. Press-gated, one line per press. */
                    sw_trace("[KEYIN] SDL joy down btn=%d which=%d",
                             (int)e.jbutton.button, (int)e.jbutton.which);
                }
                break;
            case SDL_KEYDOWN:
            case SDL_KEYUP: {
                unsigned bit = kb_key_bit(&e);
                if (bit) {
                    if (e.type == SDL_KEYDOWN) {
                        if (!e.key.repeat) g_sw_in.kb_edge |= bit;
                        g_sw_in.kb_held |= bit;
                    } else {
                        g_sw_in.kb_held &= ~bit;
                    }
                }
                break;
            }
            case SDL_FINGERDOWN:
            case SDL_FINGERUP:
            case SDL_FINGERMOTION:
                g_sw_in.touch_x = (int)(e.tfinger.x * SWITCH_UI_WIDTH);
                g_sw_in.touch_y = (int)(e.tfinger.y * SWITCH_UI_HEIGHT);
                if (e.type == SDL_FINGERDOWN) {
                    g_sw_in.touch_down = 1;
                    /* v36.26 [TOUCH-UI]: every press edge is a TAP candidate;
                     * screens claim it through sdl_switch_touch_tap(). */
                    g_sw_in.touch_tap = 1;
                    g_sw_in.touch_tap_x = g_sw_in.touch_x;
                    g_sw_in.touch_tap_y = g_sw_in.touch_y;
                }
                if (e.type == SDL_FINGERUP)   g_sw_in.touch_down = 0;
                if (e.type == SDL_FINGERMOTION && g_sw_in.touch_down)
                    g_sw_in.touch_moved = 1;
                break;
            default:
                break; /* drained and ignored (MOUSE*, AXIS, WINDOWEVENT...) */
        }
    }

    for (int i = 0; i < SWITCH_JOY_SLOTS; i++) {
        /* v36.23: seed with the synthetic FRONT_KEYS hold mask FIRST — the
         * sandbox has NO real pads, and a hold token must behave exactly
         * like a physically held button (press edge on set, release edge on
         * clear, visible to the pause-menu flush). SLOT 0 only — the same
         * pad the script's latch edges go to; production: hold=0, so
         * NULL-pad slots still wipe to 0 exactly as before. */
        uint16_t h = (i == 0) ? g_fk.hold_bits : 0;
        int dir = g_sw_in.dir[i];
        if (g_joy[i]) {
            for (int b = 0; b < 16; b++)
                if (SDL_JoystickGetButton(g_joy[i], b)) h |= (uint16_t)(1u << b);

            /* v36.37: stick -> dpad с мёртвой зоной (настройка «Мёртвая
             * зона стика», % полного хода; читается КАЖДЫЙ pump —
             * применяется живо). Порог входа = dz, отпускание = dz-0.15
             * (гистерезис, пол 0). Дефолт 50% = пороги v36.36
             * (enter 0.50 / exit 0.35) бит-в-бит; больше — глушит дрейф
             * Joy-Con, меньше — отзывчивее. */
            float dz = (float)switch_settings_get()->stick_deadzone / 100.0f;
            float enter = dz;
            float exit_ = dz - 0.15f; if (exit_ < 0.0f) exit_ = 0.0f;
            float ax = (float)SDL_JoystickGetAxis(g_joy[i], 0) / 32767.0f;
            float ay = (float)SDL_JoystickGetAxis(g_joy[i], 1) / 32767.0f;
            if (ax < -enter) dir |= 4; else if (ax > -exit_) dir &= ~4;
            if (ax >  enter) dir |= 8; else if (ax <  exit_) dir &= ~8;
            if (ay < -enter) dir |= 1; else if (ay > -exit_) dir &= ~1;
            if (ay >  enter) dir |= 2; else if (ay <  exit_) dir &= ~2;
        }
        g_sw_in.prev[i] = g_sw_in.held[i];
        g_sw_in.held[i] = h;
        g_sw_in.dir_prev[i] = g_sw_in.dir[i];
        g_sw_in.dir[i] = dir;
    }

    /* v35.10: synthetic frontend-button edges (sandbox repro harness,
     * no-op unless NOJME_FRONT_KEYS is set). Runs AFTER the SDL drain so
     * the injected latch is seen by this pump's consumers. */
    switch_frontend_keys_tick();
}

/* ================= v35.10 sandbox frontend-button script =================
 * NOJME_FRONT_KEYS="MINUS:12000,B:800" — real frontend button EDGES
 * (SWJB_* bits) injected into the central pump state on a wall-clock
 * schedule: drives the in-game MINUS pause menu, the PLUS per-game screen
 * and their A/B buttons exactly like a thumb would (NOJME_SWITCH_KEYS only
 * reaches the frontend menu, NOJME_GAME_KEYS only reaches the MIDP key
 * queue). Token "BIT:delay" = press `delay` ms AFTER the previous token
 * (the first token is delayed from process start), so a script is stable
 * regardless of when the first pump happens. Sandbox-only repro driver
 * for the v35.10 exit deadlock; zero effect unless the env var is set
 * (production never sets it). */
/* (v36.23: struct FkState / g_fk moved above switch_input_pump) */

static int fk_key_bit(const char* key) {
    if      (!strcmp(key, "A"))     return SWJB_A;
    else if (!strcmp(key, "B"))     return SWJB_B;
    else if (!strcmp(key, "X"))     return SWJB_X;
    else if (!strcmp(key, "Y"))     return SWJB_Y;
    else if (!strcmp(key, "L"))     return SWJB_L;
    else if (!strcmp(key, "R"))     return SWJB_R;
    else if (!strcmp(key, "LSTICK")) return SWJB_LSTICK; /* [YSFIX] L3 soft-key dup */
    else if (!strcmp(key, "RSTICK")) return SWJB_RSTICK; /* [YSFIX] R3 soft-key dup */
    else if (!strcmp(key, "ZL"))    return SWJB_ZL;    /* [YSFIX2] v36.40: ZL soft-key dup (sandbox E2E) */
    else if (!strcmp(key, "ZR"))    return SWJB_ZR; /* v36.36: R2 = цифра 0 */
    else if (!strcmp(key, "PLUS"))  return SWJB_PLUS;
    else if (!strcmp(key, "MINUS")) return SWJB_MINUS;
    else if (!strcmp(key, "UP"))    return SWJB_UP;
    else if (!strcmp(key, "DOWN"))  return SWJB_DOWN;
    else if (!strcmp(key, "LEFT"))  return SWJB_LEFT;
    else if (!strcmp(key, "RIGHT")) return SWJB_RIGHT;
    return -1;
}

static void switch_frontend_keys_tick(void) {
    if (!g_fk.armed) {
        const char* env = getenv("NOJME_FRONT_KEYS");
        g_fk.armed = 1;
        g_fk.pending_bit = -1;
        if (env && env[0]) {
            snprintf(g_fk.script, sizeof(g_fk.script), "%s", env);
            g_fk.pos = 0;
            g_fk.next_at = 0;
            LOG_SAFE("[FRONTKEYS] script active: %.64s\n", g_fk.script);
        } else {
            g_fk.done = 1; /* no script */
        }
    }
    if (g_fk.done) return;
    /* v36.26 [TOUCH-UI]: a TAP injected last tick releases its finger now
     * (one-pump press: the tap EDGE is what UI screens consume; the
     * pointer-dispatch path sees a clean down/up pair). */
    if (g_fk.tap_release) {
        g_sw_in.touch_down = 0;
        g_fk.tap_release = 0;
    }
    uint64_t now = sdl_get_time_ms();
    if (g_fk.pending_bit < 0 && !g_fk.pending_tap) {
        /* parse the next token and schedule it */
        /* v36.26: dedicated TAP:x,y:delay token (sandbox touch driver) —
         * MUST be tried before the generic KEY:delay parse, whose %d
         * would otherwise eat the x coordinate. */
        {
            int tx = 0, ty = 0, tdelay = 0, tcons = 0;
            if (sscanf(g_fk.script + g_fk.pos, " TAP:%d,%d:%d%n",
                       &tx, &ty, &tdelay, &tcons) >= 3 && tcons > 0) {
                g_fk.pos += tcons;
                while (g_fk.script[g_fk.pos] == ',' || g_fk.script[g_fk.pos] == ' ') g_fk.pos++;
                if (tdelay < 20) tdelay = 20;
                g_fk.pending_tap = 1;
                g_fk.pending_tx = tx;
                g_fk.pending_ty = ty;
                g_fk.next_at = now + (uint64_t)tdelay;
                return;
            }
        }
        char key[16] = {0};
        int delay = 0, consumed = 0;
        if (sscanf(g_fk.script + g_fk.pos, "%15[^:]:%d%n", key, &delay, &consumed) >= 2 &&
            consumed > 0) {
            g_fk.pos += consumed;
            while (g_fk.script[g_fk.pos] == ',' || g_fk.script[g_fk.pos] == ' ') g_fk.pos++;
            if (delay < 20) delay = 20;
            int bit = fk_key_bit(key);
            int hold = 0;
            if (bit < 0) {
                /* v36.23: KEYON / KEYOFF tokens — synthetic HELD state for
                 * the sandbox (which has NO real pads, so a one-pump latch
                 * self-releases within the same pump and can never model a
                 * held button). A hold bit joins/leaves the ground-truth
                 * `held' mask: press/release EDGES derive exactly like for
                 * a physically held button (and the pause-menu flush sees
                 * it as one). Production never sets the env. */
                size_t L = strlen(key);
                if (L > 2 && !strcmp(key + L - 2, "ON")) {
                    key[L - 2] = 0; hold = 1;
                } else if (L > 3 && !strcmp(key + L - 3, "OFF")) {
                    key[L - 3] = 0; hold = 2;
                }
                if (hold) bit = fk_key_bit(key);
            }
            if (bit < 0) {
                LOG_SAFE("[FRONTKEYS] unknown key '%s' - stop\n", key);
                g_fk.done = 1;
                return;
            }
            g_fk.pending_bit = bit;
            g_fk.pending_hold = hold;
            g_fk.next_at = now + (uint64_t)delay;
        } else {
            g_fk.done = 1; /* exhausted / malformed */
        }
        return;
    }
    if (now < g_fk.next_at) return;
    /* v36.26: due TAP token — inject a press edge at (x, y). The finger
     * lifts on the NEXT tick (tap_release above). */
    if (g_fk.pending_tap) {
        LOG_SAFE("[FRONTKEYS] tap %d,%d at +%llu ms\n",
                 g_fk.pending_tx, g_fk.pending_ty,
                 (unsigned long long)(now - g_fk.next_at));
        g_sw_in.touch_x = g_fk.pending_tx;
        g_sw_in.touch_y = g_fk.pending_ty;
        g_sw_in.touch_down = 1;
        g_sw_in.touch_moved = 0;
        g_sw_in.touch_tap = 1;
        g_sw_in.touch_tap_x = g_fk.pending_tx;
        g_sw_in.touch_tap_y = g_fk.pending_ty;
        g_fk.pending_tap = 0;
        g_fk.tap_release = 1;
        g_fk.pending_bit = -1;
        return;
    }
    if (g_fk.pending_hold == 1) {
        LOG_SAFE("[FRONTKEYS] hold-ON bit %d at +%llu ms\n",
                 g_fk.pending_bit, (unsigned long long)(now - g_fk.next_at));
        g_fk.hold_bits |= (uint16_t)(1u << g_fk.pending_bit);
    } else if (g_fk.pending_hold == 2) {
        LOG_SAFE("[FRONTKEYS] hold-OFF bit %d at +%llu ms\n",
                 g_fk.pending_bit, (unsigned long long)(now - g_fk.next_at));
        g_fk.hold_bits &= (uint16_t)~(1u << g_fk.pending_bit);
    } else {
        /* due: one-pump press edge (latch is cleared at the next pump start) */
        LOG_SAFE("[FRONTKEYS] press bit %d at +%llu ms\n",
                 g_fk.pending_bit, (unsigned long long)(now - g_fk.next_at));
        g_sw_in.latch[0] |= (uint16_t)(1u << g_fk.pending_bit);
    }
    g_fk.pending_bit = -1;
}

/* ================= v35.09 sandbox in-game key script =================
 * NOJME_GAME_KEYS="-5:300,-1:150,-5:2000" — raw MIDP keycodes driven from
 * the frame pump through the SAME deferred queue real pad keys use
 * (midp_call_keyPressed/Released). Token "code:delay" = press, hold for
 * `delay` ms, release; the next token starts after that. Lets the
 * switchui-verify sandbox drive a game's OWN menu/race (the NOJME_SWITCH_KEYS
 * driver only reaches the frontend menu). Zero effect unless the env var is
 * set (production never sets it). */
static struct {
    int          armed;        /* env seen and parsed */
    int          hold_code;    /* key currently held by the script (0 = none) */
    uint64_t     hold_until;   /* release time for hold_code */
    uint64_t     next_at;      /* when the next token may fire */
    int          pos;          /* parse cursor */
    char         script[512];  /* own copy of the env */
} g_gk;

static void switch_game_keys_tick(SdlContext* ctx) {
    if (!ctx || !ctx->jvm) return;
    if (!g_gk.armed) {
        const char* env = getenv("NOJME_GAME_KEYS");
        g_gk.armed = 1;
        if (env && env[0]) {
            snprintf(g_gk.script, sizeof(g_gk.script), "%s", env);
            g_gk.pos = 0;
            g_gk.next_at = 0;
            LOG_SAFE("[GAMEKEYS] script active: %.64s\n", g_gk.script);
        }
        if (!g_gk.script[0]) { g_gk.hold_code = INT_MIN; } /* mark: nothing to do */
    }
    if (g_gk.hold_code == INT_MIN) return; /* no script */

    uint64_t now = sdl_get_time_ms();
    if (g_gk.hold_code != 0 && now >= g_gk.hold_until) {
        extern void midp_call_keyReleased(JVM* jvm, int keycode);
        midp_call_keyReleased(ctx->jvm, g_gk.hold_code);
        LOG_SAFE("[GAMEKEYS] release %d\n", g_gk.hold_code);
        g_gk.hold_code = 0;
        g_gk.next_at = now; /* next token immediately after release */
    }
    if (g_gk.hold_code != 0 || now < g_gk.next_at) return;

    /* v36.36: W:ms token — pure wait (no key sent). Needed to choreograph
     * presses against a game's own boot/menu timeline (the previous
     * workaround — holding a filler digit — sent unwanted keyRepeated
     * events). Sandbox-only: the whole driver is env-gated. */
    {
        int wdelay = 0, wcons = 0;
        if (sscanf(g_gk.script + g_gk.pos, "W:%d%n", &wdelay, &wcons) >= 1 &&
            wcons > 0) {
            g_gk.pos += wcons;
            while (g_gk.script[g_gk.pos] == ',' || g_gk.script[g_gk.pos] == ' ') g_gk.pos++;
            if (wdelay < 20) wdelay = 20;
            g_gk.next_at = now + (uint64_t)wdelay;
            LOG_SAFE("[GAMEKEYS] wait %d ms\n", wdelay);
            return;
        }
    }

    int code = 0, delay = 0, consumed = 0;
    if (sscanf(g_gk.script + g_gk.pos, "%d:%d%n", &code, &delay, &consumed) >= 2 &&
        consumed > 0 && code != 0) {
        g_gk.pos += consumed;
        while (g_gk.script[g_gk.pos] == ',' || g_gk.script[g_gk.pos] == ' ') g_gk.pos++;
        if (delay < 20) delay = 20; /* one frame minimum so the queue sees it */
        extern void midp_call_keyPressed(JVM* jvm, int keycode);
        midp_call_keyPressed(ctx->jvm, code);
        LOG_SAFE("[GAMEKEYS] press %d for %d ms\n", code, delay);
        g_gk.hold_code = code;
        g_gk.hold_until = now + (uint64_t)delay;
    } else {
        g_gk.hold_code = INT_MIN; /* script exhausted / malformed: stop */
    }
}

unsigned sdl_switch_ui_pump_keys(void) {
    switch_input_pump();
    unsigned pressed = 0;
    unsigned held = 0;
    for (int i = 0; i < SWITCH_JOY_SLOTS; i++) {
        uint16_t pe = (uint16_t)((g_sw_in.held[i] | g_sw_in.latch[i]) & ~g_sw_in.prev[i]);
        uint16_t h  = g_sw_in.held[i];
        int de = g_sw_in.dir[i] & ~g_sw_in.dir_prev[i]; /* stick edges navigate too */
        if (pe & (1u << SWJB_UP))    pressed |= SWK_UP;
        if (pe & (1u << SWJB_DOWN))  pressed |= SWK_DOWN;
        if (pe & (1u << SWJB_LEFT))  pressed |= SWK_LEFT;
        if (pe & (1u << SWJB_RIGHT)) pressed |= SWK_RIGHT;
        if (pe & ((1u << SWJB_A) | (1u << SWJB_PLUS))) pressed |= SWK_A;
        if (pe & (1u << SWJB_B))     pressed |= SWK_B;
        if (pe & (1u << SWJB_MINUS)) pressed |= SWK_MINUS; /* v34.90: язык */
        if (de & 1) pressed |= SWK_UP;
        if (de & 2) pressed |= SWK_DOWN;
        if (de & 4) pressed |= SWK_LEFT;
        if (de & 8) pressed |= SWK_RIGHT;
        if (h & (1u << SWJB_UP))    held |= SWK_UP;
        if (h & (1u << SWJB_DOWN))  held |= SWK_DOWN;
        if (h & (1u << SWJB_LEFT))  held |= SWK_LEFT;
        if (h & (1u << SWJB_RIGHT)) held |= SWK_RIGHT;
        if (h & ((1u << SWJB_A) | (1u << SWJB_PLUS))) held |= SWK_A;
        if (h & (1u << SWJB_B))     held |= SWK_B;
    }
    pressed |= g_sw_in.kb_edge;
    if (g_sw_in.quit) pressed |= SWK_EXIT;
    /* v34.90: held is the LIVE pad state — valid on EVERY call (v34.89 kept
     * it only in the pump that happened to see the DOWN event, so browser
     * hold-to-repeat could never engage). */
    g_menu_held = held | g_sw_in.kb_held;
    return pressed;
}

unsigned sdl_switch_ui_held_keys(void) { return g_menu_held; }

/* =================== v36.26 [TOUCH-UI] touch API ===================
 * Every UI surface (frontend menus, pause menu, and the MIDP high-level
 * UI: soft keys, command menu, List/Form/TextBox/Alert, virtual
 * keyboard) consumes taps through this seam. A tap is the FINGERDOWN
 * edge; it stays PENDING until a screen claims it, because the menu
 * pumps at its own cadence (16 ms throttle) and must not lose presses.
 * Coordinates are SWITCH_UI_WIDTH x SWITCH_UI_HEIGHT window pixels —
 * the same space every screen already draws in. */

/* Sandbox script hook (NOJME_FRONT_KEYS "TAP:x,y:d" / NOJME_SWITCH_KEYS
 * "TAP:x,y:d"): inject a press edge exactly like a FINGERDOWN event. */
void sdl_switch_touch_inject(int x, int y) {
    g_sw_in.touch_x = x;
    g_sw_in.touch_y = y;
    g_sw_in.touch_down = 1;
    g_sw_in.touch_moved = 0;
    g_sw_in.touch_tap = 1;
    g_sw_in.touch_tap_x = x;
    g_sw_in.touch_tap_y = y;
}

/* Consume the pending tap edge. Returns 1 exactly once per press and
 * fills x/y with the press coordinates. */
int sdl_switch_touch_tap(int* x, int* y) {
    if (!g_sw_in.touch_tap) return 0;
    g_sw_in.touch_tap = 0;
    if (x) *x = g_sw_in.touch_tap_x;
    if (y) *y = g_sw_in.touch_tap_y;
    return 1;
}

/* Live finger state (for drag scrolling in long lists). */
int sdl_switch_touch_state(int* x, int* y, int* down, int* moved) {
    if (x) *x = g_sw_in.touch_x;
    if (y) *y = g_sw_in.touch_y;
    if (down) *down = g_sw_in.touch_down;
    if (moved) *moved = g_sw_in.touch_moved;
    return g_sw_in.touch_down;
}

/* v34.98 SDL RENDER-THREAD GUARD — "вся отрисовка SDL только в основном
 * потоке". SDL's renderer API is officially single-threaded; the reference
 * thread is armed by sdl_init()/sdl_run() (the main loop thread) and every
 * render entry point verifies the caller. A violation is throttled to one
 * line per 10 s so a looping offender cannot flood the log. */
#ifdef _WIN32
static volatile long g_sdl_render_tid = 0;
static volatile int  g_sdl_render_tid_valid = 0;
static void sdl_render_thread_arm(void) {
    g_sdl_render_tid = (long)GetCurrentThreadId();
    g_sdl_render_tid_valid = 1;
}
static void sdl_render_thread_check(const char* site) {
    if (!g_sdl_render_tid_valid) return;
    if ((long)GetCurrentThreadId() != g_sdl_render_tid) {
        static volatile long long s_last_viol_ms = 0;
        long long now = (long long)GetTickCount64();
        if (now - s_last_viol_ms >= 10000) {
            s_last_viol_ms = now;
            LOG_SAFE("[SDL-THREAD-VIOLATION] %s called OUTSIDE the render thread "
                     "(SDL rendering must stay on the main thread)\n", site);
        }
    }
}
#else
#include <pthread.h>
static pthread_t g_sdl_render_pt;
static volatile int g_sdl_render_pt_valid = 0;
static void sdl_render_thread_arm(void) {
    g_sdl_render_pt = pthread_self();
    g_sdl_render_pt_valid = 1;
}
static void sdl_render_thread_check(const char* site) {
    if (!g_sdl_render_pt_valid) return;
    if (!pthread_equal(pthread_self(), g_sdl_render_pt)) {
        static volatile long long s_last_viol_ms = 0;
        long long now = (long long)SDL_GetTicks();
        if (now - s_last_viol_ms >= 10000) {
            s_last_viol_ms = now;
            LOG_SAFE("[SDL-THREAD-VIOLATION] %s called OUTSIDE the render thread "
                     "(SDL rendering must stay on the main thread)\n", site);
        }
    }
}
#endif

void sdl_switch_ui_present(void) {
    if (!g_ui_tex || !g_renderer || !g_ui_canvas) return;
    sdl_render_thread_check("ui_present");
    /* v36.17: verification hook — dump the menu canvas as PPM when
     * NOJME_UI_DUMP_DIR is set (sandbox visual tests only; production
     * never sets it). Rate-limited to 4 dumps/sec, capped by
     * NOJME_UI_DUMP_MAX (default 40) so a soak cannot fill the disk. */
    {
        const char* dd = getenv("NOJME_UI_DUMP_DIR");
        if (dd && dd[0]) {
            static int s_dump_n = 0;
            static uint64_t s_dump_last_ms = 0;
            int max_n = 40;
            {
                const char* mx = getenv("NOJME_UI_DUMP_MAX");
                if (mx && mx[0]) max_n = atoi(mx);
            }
            uint64_t nowm = (uint64_t)SDL_GetTicks();
            if (s_dump_n < max_n && nowm - s_dump_last_ms >= 250) {
                s_dump_last_ms = nowm;
                char p[512];
                snprintf(p, sizeof(p), "%s/ui_%04d.ppm", dd, s_dump_n++);
                FILE* f = fopen(p, "wb");
                if (f) {
                    fprintf(f, "P6\n%d %d\n255\n", SWITCH_UI_WIDTH, SWITCH_UI_HEIGHT);
                    for (int i = 0; i < SWITCH_UI_WIDTH * SWITCH_UI_HEIGHT; i++) {
                        uint32_t pxl = g_ui_canvas[i];
                        unsigned char rgb[3] = {
                            (unsigned char)(pxl >> 16),
                            (unsigned char)(pxl >> 8),
                            (unsigned char)(pxl)
                        };
                        fwrite(rgb, 1, 3, f);
                    }
                    fclose(f);
                }
            }
        }
    }
    SDL_UpdateTexture(g_ui_tex, NULL, g_ui_canvas,
                      SWITCH_UI_WIDTH * sizeof(uint32_t));
    SDL_RenderClear(g_renderer);
    SDL_RenderCopy(g_renderer, g_ui_tex, NULL, NULL);
    SDL_RenderPresent(g_renderer);
}

/* ================= v36.17: ЖИВОЙ ФОН «НАСТРОЕК ИГРЫ» =================
 * Экран per-game настроек (PLUS в игре) раньше рисовал непрозрачный
 * тёмный фон — кадр игры полностью пропадал. Теперь перед отрисовкой
 * меню переключатель просит нас СКОПИРОВАТЬ последний кадр игры прямо
 * в канву меню (CPU nearest-скейл в тот же игровой прямоугольник, что
 * и у обычного present), а поверх уже рисует полупрозрачное затемнение.
 * Пользователь видит игру за настройками; смена строки «Масштаб» живо
 * меняет и фон (тот же g_pg_scale/эффективный режим, что у present).
 *
 * Безопасность потоков: экран настроек выполняется НА кадром-потоке из
 * игрового насоса (VM запаркована jvm_frontend_pause_begin), обычный
 * present в это время не вызывается — гонки за g_rot_buf/текстуры нет.
 * Источник кадра тот же single-source, что и у sdl_switch_present_game:
 * последний settled-снимок (midp_present_stable_copy), иначе живой
 * фреймбуфер как bootstrap. Возвращает 1 если кадр нарисован. */
int sdl_switch_ui_game_frame_bg(uint32_t* dst, int dst_w, int dst_h) {
    SdlContext* ctx = g_sdl_ctx_ptr;
    if (!dst || dst_w <= 0 || dst_h <= 0) return 0;
    if (!ctx || !ctx->framebuffer || !g_switch_game_active) return 0;
    const int fw = ctx->width, fh = ctx->height;
    const int px = fw * fh;
    if (px <= 0 || px > 8192 * 8192) return 0;

    /* тот же источник, что и у present (settled-снимок приоритетнее) */
    static uint32_t* s_bg_scan = NULL;
    static int s_bg_scan_px = 0;
    const uint32_t* src = ctx->framebuffer;
    {
        extern uint32_t midp_present_stable_copy(uint32_t* dst, int dst_px);
        if (px > s_bg_scan_px) {
            uint32_t* nb = (uint32_t*)realloc(s_bg_scan,
                                              (size_t)px * sizeof(uint32_t));
            if (nb) { s_bg_scan = nb; s_bg_scan_px = px; }
        }
        if (s_bg_scan && midp_present_stable_copy(s_bg_scan, px) != 0)
            src = s_bg_scan;
    }

    /* эффективный поворот сессии — тот же staging-путь, что у present */
    const int rot = (g_rot_mode && g_rot_buf) ? g_rot_mode : 0;
    const int rw = rot ? fh : fw;
    const int rh = rot ? fw : fh;
    if (rot) {
        static uint32_t* s_bg_rot = NULL;
        static int s_bg_rot_px = 0;
        if (rw * rh > s_bg_rot_px) {
            uint32_t* nb = (uint32_t*)realloc(s_bg_rot,
                            (size_t)rw * rh * sizeof(uint32_t));
            if (nb) { s_bg_rot = nb; s_bg_rot_px = rw * rh; }
        }
        if (!s_bg_rot) return 0;
        switch_scaling_rotate90(src, fw, fh, s_bg_rot, rot);
        src = s_bg_rot;
    }

    /* игровой прямоугольник с ТЕКУЩИМ эффективным масштабом (включая
     * живой per-game override g_pg_scale — фон предпросматривает смену
     * масштаба прямо в открытом меню) */
    const SwitchSettings* st = switch_settings_get();
    const int eff_scale = (g_pg_scale >= 0) ? g_pg_scale : st->scale_mode;
    int x, y, w, h;
    switch_scaling_game_rect(eff_scale, rw, rh, dst_w, dst_h, &x, &y, &w, &h);
    if (w <= 0 || h <= 0) return 0;

    /* фон за пределами кадра: чёрный (fit_black/stretch); для fit_blur —
     * приглушённый средний цвет кадра (дешёвая замена GPU-блюра в меню) */
    uint32_t fill = 0xFF000000u;
    if (eff_scale == NOJME_SCALE_FIT_BLUR) {
        uint64_t ar = 0, ag = 0, ab = 0;
        int n = 0;
        for (int sy = 0; sy < rh; sy += 8) {
            const uint32_t* row = src + (size_t)sy * rw;
            for (int sx = 0; sx < rw; sx += 8) {
                uint32_t p = row[sx];
                ar += (p >> 16) & 0xFF;
                ag += (p >> 8) & 0xFF;
                ab += p & 0xFF;
                n++;
            }
        }
        if (n > 0) {
            uint32_t r = (uint32_t)(ar / n / 2), g = (uint32_t)(ag / n / 2),
                     b = (uint32_t)(ab / n / 2);
            fill = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
    for (int i = 0; i < dst_w * dst_h; i++) dst[i] = fill;

    /* CPU nearest-скейл кадра в игровой прямоугольник */
    for (int dy = 0; dy < h; dy++) {
        const int sy = (int)((int64_t)dy * rh / h);
        const uint32_t* srow = src + (size_t)sy * rw;
        uint32_t* drow = dst + (size_t)(y + dy) * dst_w + x;
        for (int dx = 0; dx < w; dx++) {
            const int sx = (int)((int64_t)dx * rw / w);
            drow[dx] = srow[sx];
        }
    }
    return 1;
}

static uint64_t switch_ticks_ms(void) {
    static Uint64 freq = 0;
    if (!freq) freq = SDL_GetPerformanceFrequency();
    Uint64 c = SDL_GetPerformanceCounter();
    return freq ? (uint64_t)(c * 1000ULL / freq) : SDL_GetTicks();
}

uint64_t sdl_switch_ui_ticks_ms(void) { return switch_ticks_ms(); }

void sdl_switch_ui_wait(uint32_t ms) { SDL_Delay(ms); }

/* ================= v35.13 LOADING SCREEN =================
 * A fresh session has NO settled frame until the midlet completes its
 * first paint. On a VM-paced loader (field case: 3D Asia Rally at
 * "Обычная" speed) that takes tens of seconds; before v35.13 the
 * presenter kept scanning out the PREVIOUS midlet's last settled frame
 * (the session-reset in display.c now also clears that snapshot), which
 * looked exactly like "the game did not load". Instead we compose a
 * private black frame: "Загрузка <имя> (N с)" +, when the budget pacer
 * is eating the VM wall time, the remedy hint. The screen disappears by
 * itself on the first settled frame of the session. */
static uint64_t s_loadscr_begin_ms = 0;    /* session begin tick (0 = none) */
static int s_loadscr_on = 0;               /* last present composed loading */
static int s_loadscr_traced_on = 0;
static int s_loadscr_traced_off = 0;

static void sdl_switch_compose_loading_frame(uint32_t* dst, int w, int h) {
    char title[96];
    char name[160];
    const char* hint1 = NULL;
    const char* hint2 = NULL;
    unsigned secs;
    int cx;

    if (WILDGUARD_SKIP(dst, "loadscr:dst")) return; /* [WILDGUARD] v36.50 */
    memset(dst, 0, (size_t)w * (size_t)h * sizeof(uint32_t));
    if (w < 96 || h < 96) return; /* degenerate fb — plain black is fine */

    secs = (unsigned)((switch_ticks_ms() - s_loadscr_begin_ms) / 1000u);
    snprintf(title, sizeof(title), "%s... (%u %s)", switch_text(ST_LOADING),
             secs, switch_text(ST_LOADING_SEC));

    /* short jar name from the session path set by main.c */
    {
        const char* jar = g_switch_current_jar;
        const char* slash = jar[0] ? strrchr(jar, '/') : NULL;
        const char* nm = slash ? slash + 1 : jar;
        snprintf(name, sizeof(name), "%.150s", nm); /* explicit clamp */
    }

    /* v35.13: the VM-paced remedy hint (weak extern — same pattern as
     * the settled-frame primitives; absent in white-box test links). */
    {
        extern int jvm_vmpace_active(void) __attribute__((weak));
        if (jvm_vmpace_active && jvm_vmpace_active()) {
            hint1 = switch_text(ST_LOADING_VM1);
            hint2 = switch_text(ST_LOADING_VM2);
        }
    }

    /* title at 2x, centered */
    cx = (w - switch_font_text_width(title, 2)) / 2;
    if (cx < 0) cx = 0;
    switch_font_draw_text(dst, w, h, cx, h / 2 - 48, title, 0xFFFFFFFFu, 0, 2);
    /* jar name at 1x, centered, clipped to the screen */
    cx = (w - switch_font_text_width(name, 1)) / 2;
    if (cx < 0) cx = 0;
    switch_font_draw_text(dst, w, h, cx, h / 2 + 8, name, 0xFF80C8FFu, 0, 1);
    /* pacing hint (only while the pacer dominates the VM wall time) */
    if (hint1) {
        cx = (w - switch_font_text_width(hint1, 1)) / 2;
        if (cx < 0) cx = 0;
        switch_font_draw_text(dst, w, h, cx, h / 2 + 40, hint1, 0xFFB0B0B0u, 0, 1);
    }
    if (hint2) {
        cx = (w - switch_font_text_width(hint2, 1)) / 2;
        if (cx < 0) cx = 0;
        switch_font_draw_text(dst, w, h, cx, h / 2 + 60, hint2, 0xFFB0B0B0u, 0, 1);
    }
}

/* v36.27: staging презентации сессии — поворотный буфер + все три
 * текстуры (игра, Scale2x 2x, blur-фон) под ПОВЁРНУТУЮ геометрию.
 * Выделена из sdl_switch_game_begin, чтобы ГОРЯЧИЙ поворот
 * (sdl_switch_reapply_presentation) мог переставить всё то же самое
 * прямо из меню паузы, без перезапуска мидлета.
 * Возвращает 0 ok / -1 если игровая текстура не создалась.
 * Вызывать только на frame-потоке (game_begin / меню паузы). */
static int sdl_switch_stage_presentation(int fb_w, int fb_h) {
    if (!g_renderer) return -1;
    if (g_texture) { SDL_DestroyTexture(g_texture); g_texture = NULL; }

    /* v34.94: per-game overrides — эффективный фильтр (per-game > глобальный). */
    const SwitchSettings* st = switch_settings_get();
    int eff_filter = (g_pg_filter >= 0) ? g_pg_filter : st->filter;
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY,
                eff_filter == NOJME_FILTER_LINEAR ? "1" : "0");

    /* v36.15: per-game поворот экрана (презентационный, как в libretro
     * v34.48). Канва игры остаётся fb_w x fb_h; текстуры создаются под
     * ПОВЁРНУТЫЙ кадр (dw x dh), и весь downstream (UpdateTexture, blur,
     * Scale2x, game rect, pause-меню) живёт в повёрнутых координатах.
     * Ротация софтовая, в staging-буфер канвы (тот же размер px).
     * v36.27: g_pg_rotation читается ЗДЕСЬ (а не только при старте
     * сессии) — горячий поворот обновляет её до этого вызова. */
    g_rot_mode = (g_pg_rotation == 1 || g_pg_rotation == 2) ? g_pg_rotation : 0;
    const int dw = g_rot_mode ? fb_h : fb_w; /* display (повёрнутая) ширина */
    const int dh = g_rot_mode ? fb_w : fb_h; /* display (повёрнутая) высота */
    free(g_rot_buf); g_rot_buf = NULL;
    if (g_rot_mode) {
        g_rot_buf = (uint32_t*)malloc((size_t)fb_w * (size_t)fb_h *
                                      sizeof(uint32_t));
        if (!g_rot_buf) {
            LOG_SAFE("[SWITCH] rotation staging malloc FAILED — rot off\n");
            sw_trace_force("[STAGE] rotation staging malloc FAILED — ротация выключена"); /* v36.43 */
            g_rot_mode = 0; /* честный fallback: без ротации, без краха */
        }
    }

    g_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ARGB8888,
                                  SDL_TEXTUREACCESS_STREAMING, dw, dh);
    if (!g_texture) {
        LOG_SAFE("[SWITCH] game texture %dx%d failed: %s\n",
                 dw, dh, SDL_GetError());
        sw_trace_force("[STAGE] game texture %dx%d FAILED: %.120s",
                       dw, dh, SDL_GetError()); /* v36.43 */
        return -1;
    }

    /* v36.14: Scale2x (NEON) — отдельная 2x-текстура + буфер кадра.
     * Эффективный фильтр сессии уже разрешён выше (per-game > глобальный). */
    if (g_s2x_tex) { SDL_DestroyTexture(g_s2x_tex); g_s2x_tex = NULL; }
    free(g_s2x_buf); g_s2x_buf = NULL; g_s2x_w = g_s2x_h = 0;
    if (eff_filter == NOJME_FILTER_SCALE2X &&
        fb_w > 0 && fb_w <= 2048 && fb_h > 0 && fb_h <= 2048) {
        /* v36.20 FIX: раньше текстура создавалась с hint "1" (linear),
         * и GPU-растяжение 2x-буфера до game_rect ДОБАВЛЯЛО билинейное
         * сглаживание ПОВЕРХ результата Scale2x — пользователь видел
         * «или не работает, или размыто». Scale2x обязан выводиться
         * БЕЗ сглаживания: hint "0" при создании + явный
         * SDL_SetTextureScaleMode(Nearest) (не зависит от hint'ов и
         * порядка вызовов; API есть с SDL 2.0.12, devkitPro SDL2
         * новее). Итог: чёткие пиксели, честный пиксель-арт. */
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
        g_s2x_tex = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ARGB8888,
                                      SDL_TEXTUREACCESS_STREAMING,
                                      dw * 2, dh * 2);
#if SDL_VERSION_ATLEAST(2, 0, 12)
        if (g_s2x_tex) {
            if (SDL_SetTextureScaleMode(g_s2x_tex,
                                        SDL_ScaleModeNearest) != 0) {
                LOG_SAFE("[SWITCH] scale2x: SetTextureScaleMode failed: %s\n",
                         SDL_GetError());
            }
        }
#endif
        if (g_s2x_tex) {
            g_s2x_buf = (uint32_t*)malloc((size_t)dw * 2 * dh * 2 *
                                          sizeof(uint32_t));
            if (!g_s2x_buf) {
                SDL_DestroyTexture(g_s2x_tex); g_s2x_tex = NULL;
            }
        }
        if (g_s2x_tex && g_s2x_buf) {
            g_s2x_w = dw; g_s2x_h = dh; /* v36.15: повёрнутые размеры */
            {
                extern const char* switch_scaling_scale2x_backend(void);
                LOG_SAFE("[SWITCH] scale2x: %dx%d -> %dx%d (%s, fit=nearest "
                         "v36.20)\n",
                         dw, dh, dw * 2, dh * 2,
                         switch_scaling_scale2x_backend());
            }
        } else {
            LOG_SAFE("[SWITCH] scale2x: unavailable (%s) - texture filter fallback\n",
                     SDL_GetError());
            sw_trace("[STAGE] scale2x unavailable: %.120s — фильтр-фолбэк", /* v36.43 */
                     SDL_GetError());
        }
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY,
                    eff_filter == NOJME_FILTER_LINEAR ? "1" : "0");
    }
    switch_scaling_bg_size(dw, dh, &g_bg_w, &g_bg_h); /* v36.15: повёрнутые */
    if (g_bg_tex) { SDL_DestroyTexture(g_bg_tex); g_bg_tex = NULL; }
    /* v36.20: фон-заливка (FIT_BLUR) — НАМЕРЕННО размытый фон за
     * letterbox; present-путь всегда рассчитывал на «GPU stretches it
     * linearly». Раньше текстура создавалась с унаследованным hint'ом
     * (nearest при фильтрах nearest/scale2x) — фон становился блочным.
     * Явно линейный для фона; на саму картинку игры не влияет. */
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    g_bg_tex = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ARGB8888,
                                 SDL_TEXTUREACCESS_STREAMING, g_bg_w, g_bg_h);
#if SDL_VERSION_ATLEAST(2, 0, 12)
    if (g_bg_tex) SDL_SetTextureScaleMode(g_bg_tex, SDL_ScaleModeLinear);
#endif
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY,
                eff_filter == NOJME_FILTER_LINEAR ? "1" : "0");
    free(g_bg_buf);
    g_bg_buf = (uint32_t*)malloc((size_t)g_bg_w * g_bg_h * sizeof(uint32_t));
    if (!g_bg_buf) {
        LOG_SAFE("[SWITCH] blur bg buffer malloc FAILED (%dx%d) — blur disabled\n",
                 g_bg_w, g_bg_h);
        sw_trace_force("[STAGE] blur bg buffer malloc FAILED (%dx%d) — блюр-фон выключен",
                       g_bg_w, g_bg_h); /* v36.43 */
    }
    return 0;
}

int sdl_switch_game_begin(int fb_w, int fb_h) {
    if (!g_renderer) return -1;
    sdl_render_thread_check("game_begin");
    {
        /* [YSFIX2] v36.40: определена в main.c (CORE_BUILD_ID); тот же
         * extern-паттерн, что в switch_trace.c. */
        extern const char* j2me_core_build_id(void);
        sw_trace("game: begin %dx%d build=%s", fb_w, fb_h, j2me_core_build_id()); /* v34.91; [YSFIX2] v36.40: + build id — полевой лог теперь самопрезентирует сборку */
    }
    /* v35.13: loading-screen bookkeeping — the session starts with NO
     * settled frame; until the first one lands we compose a black
     * "Загрузка ..." frame instead of scanning out stale content. */
    s_loadscr_begin_ms = switch_ticks_ms();
    s_loadscr_on = 0;
    s_loadscr_traced_on = 0;
    s_loadscr_traced_off = 0;
    if (g_switch_game_active) {
        /* v34.98: a mid-session recreation is the prime suspect behind
         * corrupted-frame moments; make it LOUD if it ever happens. */
        LOG_SAFE("[SURFACE-RECREATE] game_begin while session active: %dx%d\n", fb_w, fb_h);
        /* v36.43 [PRESENT-GLASS]: на устройстве stderr не попадает в log.txt
         * — ПЕРЕСОЗДАНИЕ поверхностей посреди сессии обязано быть в трейсе
         * (оно пересоздаёт ВСЕ текстуры сессии). */
        sw_trace_force("[SURFACE-RECREATE] game_begin при живой сессии: %dx%d",
                       fb_w, fb_h);
    }
    /* v36.27: канва сессии запоминается для горячего поворота */
    s_sess_fb_w = fb_w;
    s_sess_fb_h = fb_h;

    if (sdl_switch_stage_presentation(fb_w, fb_h) != 0) return -1;

    const SwitchSettings* st = switch_settings_get();
    g_switch_game_active = 1;
    switch_input_reset(); /* v34.90: no edges leak across the menu->game seam */
    g_game_rect.x = g_game_rect.y = g_game_rect.w = g_game_rect.h = 0;
    sdl_switch_fps_state_reset(); /* v36.18: FPS-окно измерения с нуля */
    LOG_SAFE("[SWITCH] game session %dx%d (scale=%d filter=%d bg=%dx%d rot=%s)\n",
             fb_w, fb_h, st->scale_mode, st->filter, g_bg_w, g_bg_h,
             g_rot_mode == 1 ? "right" : g_rot_mode == 2 ? "left" : "off");
    return 0;
}

/* v36.27 HOT ROTATION («переворот экрана применяется не тут же, а только
 * после перезагрузки мидлета» — report): изменение per-game строки
 * «Поворот экрана» в меню паузы (Настройки игры) теперь применяется
 * НЕМЕДЛЕННО. Staging (rot-буфер, игровая/2x/фоновая текстуры)
 * переставляется под новый угол, и последний settled-кадр
 * пере-поворачивается в новую геометрию и заливается в новую текстуру —
 * меню паузы и первый же present после возобновления показывают картинку
 * в новом повороте. Канва игры, VM и сам мидлет НЕ трогаются: перезапуск
 * больше не нужен.
 * Вызывается из switch_ui_pergame_screen (слабый символ — хост-сборки без
 * sdl_graphics.c не линкуются с ним; там адрес NULL и запись пропускается).
 * Контракт: frame-поток, VM запаркована скобками паузы (как и весь
 * pergame-экран) — races с present/UpdateTexture невозможны. */
void sdl_switch_reapply_presentation(void) {
    if (!g_renderer || !g_switch_game_active ||
        s_sess_fb_w <= 0 || s_sess_fb_h <= 0) return;

    const int old_mode = g_rot_mode;
    const int new_mode =
        (g_pg_rotation == 1 || g_pg_rotation == 2) ? g_pg_rotation : 0;
    if (new_mode == old_mode &&
        (new_mode == 0 || g_rot_buf) && g_texture) {
        return; /* угол не менялся (или повторный вызов) — нечего переставлять */
    }

    /* полный restage: g_rot_mode пересчитывается из g_pg_rotation внутри */
    if (sdl_switch_stage_presentation(s_sess_fb_w, s_sess_fb_h) != 0) return;

    /* Пере-поворот последнего settled-кадра в новую геометрию и заливка
     * в новую текстуру — фон меню паузы сразу корректен, первого «моргания
     * чёрным» после возобновления нет. Нет settled-кадра (пауза на
     * загрузочном экране) — просто пропускаем заливку, кадр придёт сам. */
    {
        extern uint32_t midp_present_stable_copy(uint32_t* dst, int dst_px)
            __attribute__((weak));
        if (midp_present_stable_copy) {
            const int px = s_sess_fb_w * s_sess_fb_h;
            uint32_t* src = (uint32_t*)malloc((size_t)px * sizeof(uint32_t));
            if (src && midp_present_stable_copy(src, px)) {
                const int rot = (g_rot_mode && g_rot_buf) ? g_rot_mode : 0;
                uint32_t* disp = src;
                int rw = s_sess_fb_w;
                if (rot) {
                    switch_scaling_rotate90(src, s_sess_fb_w, s_sess_fb_h,
                                            g_rot_buf, rot);
                    disp = g_rot_buf;
                    rw = s_sess_fb_h;
                }
                if (g_texture)
                    SDL_UpdateTexture(g_texture, NULL, disp,
                                      (size_t)rw * sizeof(uint32_t));
            }
            free(src);
        }
    }
    LOG_SAFE("[SWITCH] hot rotation: %s -> %s\n",
             old_mode == 1 ? "right" : old_mode == 2 ? "left" : "off",
             g_rot_mode == 1 ? "right" : g_rot_mode == 2 ? "left" : "off");
    sw_trace("hot rotation -> %s",
             g_rot_mode == 1 ? "right" : g_rot_mode == 2 ? "left" : "off");
}

void sdl_switch_game_end(void) {
    if (g_texture) { SDL_DestroyTexture(g_texture); g_texture = NULL; }
    if (g_s2x_tex) { SDL_DestroyTexture(g_s2x_tex); g_s2x_tex = NULL; }
    free(g_s2x_buf); g_s2x_buf = NULL; g_s2x_w = g_s2x_h = 0; /* v36.14 */
    free(g_rot_buf); g_rot_buf = NULL; g_rot_mode = 0; /* v36.15 */
    /* v36.18: полоса FPS уходит вместе с игровой сессией */
    if (s_fps_tex) { SDL_DestroyTexture(s_fps_tex); s_fps_tex = NULL; }
    free(s_fps_buf); s_fps_buf = NULL; s_fps_bw = s_fps_bh = 0;
    sdl_switch_fps_state_reset();
    if (g_bg_tex) { SDL_DestroyTexture(g_bg_tex); g_bg_tex = NULL; }
    free(g_bg_buf); g_bg_buf = NULL;
    g_switch_game_active = 0;
    s_sess_fb_w = 0; s_sess_fb_h = 0; /* v36.27 */
    s_loadscr_begin_ms = 0; /* v35.13 */
    s_loadscr_on = 0;
    switch_input_reset(); /* v34.90: the frontend menu starts from clean edges */
    /* v36.23: wipe the repeat/pause-flush statics — a latched slot must not
     * leak into the next game (phantom keyRepeated right from launch). */
    memset(&g_sdl_key_repeat, 0, sizeof(g_sdl_key_repeat));
    g_pause_held = 0;
    /* close any game audio so the menu is silent
     * [AUDIO-HARDEN] v36.50: под мьютексом — sdl_switch_game_end зовётся
     * главным потоком на выходе из игры, пока микшерный поток может быть
     * внутри SDL_QueueAudio (sdl_audio_queue_samples берёт тот же мьютекс). */
    pthread_mutex_lock(&g_audio_dev_mutex);
    if (g_audio_dev) { SDL_CloseAudioDevice(g_audio_dev); g_audio_dev = 0; }
    pthread_mutex_unlock(&g_audio_dev_mutex);
    LOG_SAFE("[SWITCH] game session end\n");
}

/* ======================= present with scaling ======================= */

/* v34.94 diag: settled-frame version bookkeeping for skip=/fl= counters. */
static uint32_t s_switch_frame_version_seen = 0;
static int s_switch_frame_version_advanced = 0;

static void sdl_switch_present_game(SdlContext* ctx) {
    if (!ctx || !ctx->framebuffer || !g_texture || !g_renderer) return;
    if (WILDGUARD_SKIP(ctx->framebuffer, "present:fb")) return; /* [WILDGUARD] v36.50 */
    sdl_render_thread_check("present_game");

    /* v36.28 DEBUG: sandbox game-canvas dump (NOJME_GAME_DUMP_DIR),
     * rate-limited 1/sec, capped by NOJME_GAME_DUMP_MAX (default 200). */
    {
        const char* gd = getenv("NOJME_GAME_DUMP_DIR");
        if (gd && gd[0]) {
            static int s_gdump_n = 0;
            static uint64_t s_gdump_last_ms = 0;
            int gmax = 200;
            {
                const char* gmx = getenv("NOJME_GAME_DUMP_MAX");
                if (gmx && gmx[0]) gmax = atoi(gmx);
            }
            uint64_t nowg = (uint64_t)SDL_GetTicks();
            if (s_gdump_n < gmax && nowg - s_gdump_last_ms >= 1000) {
                s_gdump_last_ms = nowg;
                char pth[512];
                snprintf(pth, sizeof(pth), "%s/game_%04d.ppm", gd, s_gdump_n++);
                sdl_save_framebuffer_to_file(ctx, pth);
            }
        }
    }

    /* v35.01: present-cost telemetry — the v35.00 trace froze with the
     * frontend loop alive but over budget (stl 0 -> 1299 ms/period, loop
     * 300 -> 222) and the blur border visibly updating while the game view
     * froze. pv/pk/pw split the loop cost into RenderPresent vs the whole
     * present so the next trace names the stalled stage directly. */
    struct timespec pw_t0, pv_t1, pv_t2;
    clock_gettime(CLOCK_MONOTONIC, &pw_t0);

    const SwitchSettings* st = switch_settings_get();
    int eff_scale = (g_pg_scale >= 0) ? g_pg_scale : st->scale_mode;
    /* v36.16 LIVE 3D FLIP: эффективный режим пересчитывается КАЖДЫЙ present:
     * per-game override (g_pg_flip, обновляется живо из PLUS-меню) > глобальная
     * настройка (синглтон настроек). M3G-растеризатор читает
     * g_nojme_flip_mode на каждое решение о лицевости, поэтому запись здесь
     * меняет рендер немедленно — без перезапуска игры. Слабый символ: хост
     * -сборки без mobile3d.c (test_switch_input) не линкуются с M3G-ядром —
     * там адрес NULL и запись пропускается; в реальной сборке mobile3d.c
     * определяет переменную сильно. */
    {
        extern int g_nojme_flip_mode __attribute__((weak));
        if (&g_nojme_flip_mode)
            g_nojme_flip_mode = (g_pg_flip >= 0) ? g_pg_flip : st->flip_mode;
    }
    /* v36.15: эффективный поворот сессии (может быть ослаблен при OOM
     * staging-буфера в game_begin — g_rot_mode тогда уже 0). Все размеры
     * ниже — ПОВЁРНУТЫЕ (rw x rh), канва игры не трогается. */
    const int rot = (g_rot_mode && g_rot_buf) ? g_rot_mode : 0;
    const int rw = rot ? ctx->height : ctx->width;  /* display width */
    const int rh = rot ? ctx->width  : ctx->height; /* display height */
    int x, y, w, h;
    switch_scaling_game_rect(eff_scale, rw, rh,
                             SWITCH_UI_WIDTH, SWITCH_UI_HEIGHT, &x, &y, &w, &h);
    g_game_rect.x = x; g_game_rect.y = y; g_game_rect.w = w; g_game_rect.h = h;

    /* v36.44 [PRESENT-MODE]: одноразовый трейс КОНФИГУРАЦИИ ВЫВОДА сессии
     * (рендерер/масштаб/фильтр/s2x/рект) — полевой разбор фриза «вид игры
     * замер, блюр жив» упирался в незнание, каким фильтром и режимом
     * пользуется устройство: путь Scale2x (отдельная текстура+NEON на
     * железе) и fit_blur дают РАЗНЫЕ наборы подозреваемых. Срабатывает на
     * первом present сессии и при смене конфигурации (горячий поворот или
     * смена фильтра из меню паузы). */
    {
        static int s_pm_last_scale = -1, s_pm_last_filter = -1,
                   s_pm_last_rot = -1, s_pm_last_s2x = -1;
        int s2x_on = (g_s2x_tex && g_s2x_buf &&
                      g_s2x_w == rw && g_s2x_h == rh) ? 1 : 0;
        int eff_filter = (g_pg_filter >= 0) ? g_pg_filter : st->filter;
        if (eff_scale != s_pm_last_scale || eff_filter != s_pm_last_filter ||
            rot != s_pm_last_rot || s2x_on != s_pm_last_s2x) {
            s_pm_last_scale = eff_scale;
            s_pm_last_filter = eff_filter;
            s_pm_last_rot = rot;
            s_pm_last_s2x = s2x_on;
            const char* rnd = "software";
            {
                SDL_RendererInfo ri;
                if (SDL_GetRendererInfo(g_renderer, &ri) == 0 && ri.name)
                    rnd = ri.name;
            }
            sw_trace("[PRESENT-MODE] renderer=%s scale=%d filter=%d rot=%d "
                     "s2x=%d rect=%d,%d %dx%d canvas=%dx%d",
                     rnd, eff_scale, eff_filter, rot, s2x_on,
                     g_game_rect.x, g_game_rect.y, g_game_rect.w, g_game_rect.h,
                     ctx->width, ctx->height);
        }
    }

    /* v35.06 PHONE-FAITHFUL DIRECT-RENDER DELIVERY ("кадры не отбрасываются
     * при финальном выводе"): before resolving the scanout source, deliver
     * any content the game drew into the live framebuffer OUTSIDE the
     * repaint pump (a Graphics cached from an earlier paint / direct
     * rendering). On a real phone that drawing hits the LCD immediately;
     * the settled-frame model used to withhold it until the next pump
     * paint. Gated by midp_canvas_mid_frame() — never fires inside a
     * paint() or an open M3G bind, so the torn-3D-frame class stays
     * impossible; at most one extra settle per frontend frame.
     *   dset = how often this path delivered (0 on pump-paced games);
     *   lvc  = how often the live framebuffer changed at all, so the diag
     *          can prove lvc == fl ("nothing dropped") per period. */
    {
        extern int midp_present_settle_if_stale(void) __attribute__((weak));
        extern int midp_present_live_changed(void) __attribute__((weak));
        if (midp_present_settle_if_stale && midp_present_settle_if_stale())
            __sync_fetch_and_add(&g_sd_dset, 1);
        if (midp_present_live_changed && midp_present_live_changed())
            __sync_fetch_and_add(&g_sd_lvc, 1);
    }

    /* v35.01 SINGLE-SOURCE PRESENT ("вывод на экран из того же источника,
     * что и блюр"): resolve the frame source ONCE and feed BOTH consumers —
     * the game texture and the blurred border fill. Since v34.98 both read
     * the settled snapshot, but each ran its own stable_copy: two lock
     * round-trips per frame and a theoretical divergence window when a
     * settle lands between them. Now one copy per frame, one decision,
     * zero divergence — and the sharp image can never lag the blur.
     * Semantics unchanged (v34.61): the latest SETTLED snapshot when one
     * exists; the live framebuffer only as the nothing-settled bootstrap. */
    static uint32_t* s_scanout = NULL;
    static int s_scanout_px = 0;
    const uint32_t* present_src = ctx->framebuffer;
    {
        extern uint32_t midp_present_stable_copy(uint32_t* dst, int dst_px);
        int px = ctx->width * ctx->height;
        if (px > 0 && px <= 8192 * 8192) {
            if (px > s_scanout_px) {
                uint32_t* nb = (uint32_t*)realloc(s_scanout,
                                                  (size_t)px * sizeof(uint32_t));
                if (nb) { s_scanout = nb; s_scanout_px = px; }
            }
            if (s_scanout && midp_present_stable_copy(s_scanout, px) != 0)
                present_src = s_scanout;
        }
    }

    /* v36.15: флаг источника ДО ротации (дальше present_src перезаписывается
     * повёрнутым буфером, и сравнивать будет уже не с чем).
     * v36.19: нечитаемый флаг settled убран (предупреждение -Wunused;
     * settled-статистику ведёт fps-блок ниже по s_fps_*). */
    const int bootstrapping = (present_src == ctx->framebuffer);

    /* v36.15 ROTATION: поворачиваем кадр в staging-буфер и дальше весь
     * downstream (счётчик кадров, загрузочный экран, текстура, блюр,
     * Scale2x) питается ПОВЁРНУТЫМ кадром — как в libretro push_frame.
     * Канва игры (ctx->framebuffer) не изменяется. */
    if (rot) {
        switch_scaling_rotate90(present_src, ctx->width, ctx->height,
                                g_rot_buf, rot);
        present_src = g_rot_buf;
    }
    /* буфер для оверлеев (счётчик кадров / загрузочный экран): приватный
     * записываемый буфер текущего present_src */
    uint32_t* overlay_buf = rot ? g_rot_buf : s_scanout;

    /* v35.13 LOADING SCREEN: while THIS session has not completed a single
     * settled frame (stable_copy returned 0 => present_src is still the
     * live bootstrap), present a private black "Загрузка <jar> (N s)"
     * frame. The stale-frame class this kills:
     *   - the previous midlet's last frame leaking into the new session
     *     (settled snapshot is now also cleared on the session reset), and
     *   - a VM-paced loader silently showing that stale content for tens
     *     of seconds with no feedback ("the game did not load").
     * The screen goes away by itself when the first settled frame lands. */
    if (bootstrapping && s_loadscr_begin_ms != 0 &&
        s_scanout && s_scanout_px >= ctx->width * ctx->height) {
        s_loadscr_on = 1;
        if (!s_loadscr_traced_on) {
            s_loadscr_traced_on = 1;
            sw_trace("load-screen: on (no settled frame yet)");
        }
        /* v36.15: рисуется в overlay_buf уже в ПОВЁРНУТОЙ геометрии —
         * текст «Загрузка ...» стоит горизонтально на экране консоли */
        sdl_switch_compose_loading_frame(overlay_buf, rw, rh);
        present_src = overlay_buf;
    } else if (s_loadscr_on) {
        s_loadscr_on = 0;
        if (!s_loadscr_traced_off) {
            s_loadscr_traced_off = 1;
            sw_trace("load-screen: off (first frame settled)");
        }
    }

    /* v35.07 FRAME COUNTER (settings row "Счётчик кадров").
     * v36.18 REWORK по запросу: счётчик больше НЕ рисуется поверх игры
     * (старый способ — вписывать текст в present_src игрового кадра).
     * Теперь это отдельная полоса FPS в точке (4,4) ПОЛНОГО экрана:
     * пустая область слева от игры (левый верхний угол); на экран игры
     * полоса заходит только если пустого места не хватает (игра
     * масштабирована на весь экран). Из показаний убраны номер кадра и
     * возраст, осталось ТОЛЬКО FPS, и обновляется он ОДИН РАЗ В СЕКУНДУ:
     * в окно ~1000 мс считаются смены номера settled-кадра (истинный
     * темп игры, не present-цикла 60 Гц), затем FPS = кадры * 1000 /
     * фактическая длительность окна; замороженная игра честно показывает
     * 0 fps. Текст перерисовывается только при смене значения. */
    if (!s_loadscr_on && st->frame_counter) {
        extern uint32_t midp_present_stable_seq(void) __attribute__((weak));
        if (midp_present_stable_seq) {
            sdl_switch_fps_strip_ensure();
            uint32_t seq = midp_present_stable_seq();
            uint64_t now = sdl_switch_ui_ticks_ms();
            if (seq != s_fps_last_seq) {
                s_fps_last_seq = seq;
                s_fps_seq_seen = 1;
                s_fps_win_count++;
            }
            if (!s_fps_win_active) {
                s_fps_win_start = now;
                s_fps_win_count = 0;
                s_fps_win_active = 1;
            } else if (now >= s_fps_win_start + 1000u) {
                uint64_t el = now - s_fps_win_start;
                if (el < 1) el = 1;
                s_fps_display = (unsigned)
                    (((uint64_t)s_fps_win_count * 1000u + el / 2) / el);
                s_fps_have_display = 1;
                s_fps_win_start = now;
                s_fps_win_count = 0;
                sdl_switch_fps_strip_draw(); /* раз в секунду */
            }
        }
    }

    /* v36.15: pitch повёрнутой ширины; v36.43 [PRESENT-GLASS]: раньше rc
     * не проверялся — отказ загрузки текстуры (драйвер/GPU) оставлял бы на
     * экране СТАРЫЙ кадр при здоровых fl/lvc; теперь это видно в utf=/rcf=
     * + трейс [PRESENT-FAIL] с текстом ошибки SDL. */
    sdl_present_fail_note("UpdateTexture(game)",
        SDL_UpdateTexture(g_texture, NULL, present_src,
                          rw * (int)sizeof(uint32_t)), 1);

    if (eff_scale == NOJME_SCALE_FIT_BLUR && g_bg_tex && g_bg_buf) {
        /* 1/8-size box downsample + blur, GPU stretches it linearly.
         * v34.98 CRITICAL FIX: the stable-copy check was INVERTED here —
         * midp_present_stable_copy returns the snapshot SEQUENCE number
         * (0 = nothing settled yet), and the old `== 0` branch used the
         * scanout copy only while NOTHING was settled, i.e. the blur was
         * computed from the LIVE framebuffer on every normal frame while
         * the main texture got the settled snapshot. During the slow-bind
         * degradation the live framebuffer holds a half-painted/garbage
         * frame: the user saw the blur turn into a garbage blob ("зеленое
         * пятно") while the main image stayed correct — exactly the field
         * report. v35.01: the blur consumes the SAME single-source
         * decision as the game texture (present_src above). */
        switch_scaling_blur_bg(present_src, rw, rh, /* v36.15: повёрнутый кадр */
                               g_bg_buf, g_bg_w, g_bg_h, 2);
        sdl_present_fail_note("UpdateTexture(bg)",
            SDL_UpdateTexture(g_bg_tex, NULL, g_bg_buf,
                              g_bg_w * (int)sizeof(uint32_t)), 1);
        sdl_present_fail_note("RenderCopy(bg)",
            SDL_RenderCopy(g_renderer, g_bg_tex, NULL, NULL), 0); /* stretched full screen */
    } else {
        SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
        SDL_RenderClear(g_renderer);
    }

    /* v36.14: Scale2x — NEON-апскейл x2 settles-кадра в g_s2x_buf, выгрузка
     * в 2x-текстуру, финальный fit в игровой прямоугольник — на GPU.
     * Перезапись 1x-текстуры НЕ пропускается: pause-меню и все остальные
     * пути продолжают читать её без изменений. */
    if (g_s2x_tex && g_s2x_buf &&
        g_s2x_w == rw && g_s2x_h == rh) { /* v36.15: повёрнутые размеры */
        switch_scaling_scale2x(present_src, rw, rh,
                               g_s2x_buf);
        sdl_present_fail_note("UpdateTexture(s2x)",
            SDL_UpdateTexture(g_s2x_tex, NULL, g_s2x_buf,
                              (size_t)rw * 2 * sizeof(uint32_t)) == 0 ? 0 : -1, 1);
        sdl_present_fail_note("RenderCopy(s2x)",
            SDL_RenderCopy(g_renderer, g_s2x_tex, NULL, &g_game_rect), 0);
    } else {
        sdl_present_fail_note("RenderCopy(game)",
            SDL_RenderCopy(g_renderer, g_texture, NULL, &g_game_rect), 0);
    }
    /* v36.18: полоса FPS — самый верхний слой, точка (4,4) ПОЛНОГО экрана:
     * пустое поле слева от игры (верхний левый угол); если игра занимает
     * угол (масштаб «растянуть»/крупный fit), полоса оказывается на экране
     * игры — допустимый фолбэк. Текстура уже содержит готовый текст. */
    if (st->frame_counter && !s_loadscr_on && s_fps_have_display &&
        s_fps_tex) {
        SDL_Rect fps_rect = { 4, 4, s_fps_bw, s_fps_bh };
        SDL_RenderCopy(g_renderer, s_fps_tex, NULL, &fps_rect);
    }
    /* [BATFIX] v36.41: индикатор заряда — правый верхний угол ПОЛНОГО
     * экрана, зеркально полосе FPS (та же модель «отдельная текстура
     * поверх кадра»). Показывается и на загрузочном экране (заряд —
     * не диагностика кадра, ждать первую оседлавшуюся копию незачем).
     * Перерисовка текстуры — ТОЛЬКО при смене режима/процента/зарядки. */
    if (st->battery_display != NOJME_BATT_OFF) {
        sdl_switch_batt_ensure();
        if (s_batt_tex && s_batt_buf) {
            int bpct = nojme_battery_percent();
            int bchg = nojme_battery_charging();
            if (st->battery_display != s_batt_mode ||
                bpct != s_batt_pct || bchg != s_batt_chg) {
                s_batt_mode = st->battery_display;
                s_batt_pct = bpct;
                s_batt_chg = bchg;
                sdl_switch_batt_redraw(s_batt_mode);
            }
            /* v36.43 [BATT-GAME]: текстура прижата к САМОМУ углу (а не к
             * (4,4)-отступу) — бейдж внутри неё уже несёт свои поля
             * 16/8px, поэтому в кадре игры он стоит на ТЕХ ЖЕ пикселях
             * экрана, что и в меню/паузе, и не срезается оверсканом ТВ. */
            SDL_Rect batt_rect = { SWITCH_UI_WIDTH - s_batt_bw, 0,
                                   s_batt_bw, s_batt_bh };
            sdl_present_fail_note("RenderCopy(batt)",
                SDL_RenderCopy(g_renderer, s_batt_tex, NULL, &batt_rect), 0);

        }
    }
    /* v36.43 [PRESENT-GLASS]: проба композита — ДО RenderPresent (после
     * него задний буфер не определён). Заодно env-дамп полного кадра для
     * песочных пиксель-тестов (test_switch_battgame.sh). */
    {
        uint64_t nowp = sdl_switch_ui_ticks_ms();
        sdl_composite_dump_maybe(nowp);
        sdl_composite_probe(nowp);
    }
    clock_gettime(CLOCK_MONOTONIC, &pv_t1);
    SDL_RenderPresent(g_renderer);
    clock_gettime(CLOCK_MONOTONIC, &pv_t2);
    {
        int64_t rp_ms = (int64_t)(pv_t2.tv_sec - pv_t1.tv_sec) * 1000 +
                        (int64_t)(pv_t2.tv_nsec - pv_t1.tv_nsec) / 1000000;
        int64_t pw_ms = (int64_t)(pv_t2.tv_sec - pw_t0.tv_sec) * 1000 +
                        (int64_t)(pv_t2.tv_nsec - pw_t0.tv_nsec) / 1000000;
        if (rp_ms > 0) __sync_fetch_and_add(&g_sd_pv_ms, (uint32_t)rp_ms);
        if (rp_ms > (int64_t)g_sd_pv_ms_max) g_sd_pv_ms_max = (uint32_t)rp_ms;
        if (pw_ms > (int64_t)g_sd_pw_ms_max) g_sd_pw_ms_max = (uint32_t)pw_ms;
        /* A RenderPresent over 250 ms is the display-pipeline stall
         * signature (vsync fence / VI hiccup) — log it throttled, forced
         * (bypasses the logging gate) so the frozen-state log names it. */
        if (rp_ms > 250) {
            static uint64_t s_last_pslow = 0;
            uint64_t nowm = sdl_switch_ui_ticks_ms();
            if (nowm - s_last_pslow >= 5000) {
                s_last_pslow = nowm;
                sw_trace_force("[PRESENT-SLOW] SDL_RenderPresent %lld ms "
                               "(whole present %lld ms) — display pipeline stall?",
                               (long long)rp_ms, (long long)pw_ms);
            }
        }
    }
    /* v34.94 diag: pr/skip/fl — track whether the game actually produced a
     * new settled frame since the last present. Weak: the white-box input
     * test links this file without the JVM/midp objects. */
    {
        extern uint32_t midp_present_stable_seq(void) __attribute__((weak));
        if (midp_present_stable_seq) {
            uint32_t v = midp_present_stable_seq();
            if (v != s_switch_frame_version_seen) {
                s_switch_frame_version_seen = v;
                s_switch_frame_version_advanced = 1;
                __sync_fetch_and_add(&g_sd_fl, 1);
            }
        }
    }
    __sync_fetch_and_add(&g_sd_pr, 1);
    if (!s_switch_frame_version_advanced) {
        __sync_fetch_and_add(&g_sd_skip, 1); /* stale content re-presented */
    }
    s_switch_frame_version_advanced = 0;
}

/* ======================= gamepad -> MIDP keys ======================= */

/* v36.27: D-pad remap по углу поворота презентации (второй пункт отчёта
 * про поворот — «при повороте стрелки должны крутиться вместе с экраном»).
 * rot=1 (90 вправо/CW): верх канвы смотрит вправо экрана; пользователь
 * поворачивает консоль против часовой — кнопка, ставшая «верхней»,
 * это физический RIGHT, и она обязана слать канве UP:
 *   UP->LEFT, LEFT->DOWN, DOWN->RIGHT, RIGHT->UP
 * rot=2 (90 влево/CCW) — зеркально. Канва игры не поворачивается
 * (поворот презентационный), поэтому маппинг делается на уровне
 * keycode'ов игры: и getGameAction(), и прямые проверки keyCode
 * (Bobby Carrot) получают согласованную с картинкой стрелку.
 * Применяется ТОЛЬКО в игровых путях (dir_dispatch + joy-кнопки);
 * меню фронтенда читают сырые SWK_* и не перекашиваются. */
static SDL_Keycode sdl_rot_map_key(SDL_Keycode k) {
    if (!g_rot_mode) return k;
    switch (k) {
        case SDLK_UP:    return (g_rot_mode == 1) ? SDLK_LEFT  : SDLK_RIGHT;
        case SDLK_DOWN:  return (g_rot_mode == 1) ? SDLK_RIGHT : SDLK_LEFT;
        case SDLK_LEFT:  return (g_rot_mode == 1) ? SDLK_DOWN  : SDLK_UP;
        case SDLK_RIGHT: return (g_rot_mode == 1) ? SDLK_UP    : SDLK_DOWN;
        default:         return k;
    }
}

/* Raw joystick button index -> synthetic keycode. Re-dispatch through
 * sdl_handle_key_event: feeds the SAME soft-button, menu-navigation
 * and key-repeat bookkeeping as the keyboard path. */

/* v36.37: эффективная раскладка ПУТИ ввода — per-game override побеждает
 * глобальную настройку «Поменять местами стик и D-pad» (NOJME_SWAP_*).
 * ВАЖНО (смена семантики против v36.36): пути СТИКА и КРЕСТОВИНЫ теперь
 * РАЗДЕЛЬНЫ:
 *   OFF (default): крестовина шлёт стрелки (-1..-4), стик — ITU-T-цифры
 *                  '2'/'8'/'4'/'6' (50/56/52/54);
 *   ON:            наоборот (стик — стрелки, крестовина — цифры).
 * В обоих случаях цифры дают чистые направления без софт-алиасинга в
 * мульт,девайс-портах (NET Lizard «a2» и т.п.), а стрелки остаются на
 * физической крестовине — запрос пользователя. is_stick=1 — событие
 * пришло от осей стика (sdl_switch_dir_dispatch), 0 — от кнопок
 * крестовины (SWJB_UP/DOWN/LEFT/RIGHT в switch_joy_button_to_keycode). */
static int sdl_input_swap_active(void) {
    if (g_pg_input_swap >= 0) return g_pg_input_swap == NOJME_SWAP_ON;
    return switch_settings_get()->input_swap == NOJME_SWAP_ON;
}

/* Цифры активны для ПУТИ, если путь «перевёрнут» относительно дефолта:
 * стик по умолчанию цифровой (цифры активны при swap OFF), крестовина —
 * стрелочная (цифры активны при swap ON). */
static int sdl_dpad_path_digits(int is_stick) {
    int swapped = sdl_input_swap_active();
    return is_stick ? !swapped : swapped;
}

/* Стрелка -> ITU-T-цифра ('2'/'8'/'4'/'6'). Прямо переводит SDL-кейкод:
 * key_map[] шлёт их игре как 50/56/52/54 — тот же путь, что у физической
 * клавиатуры, включая key-repeat bookkeeping. */
static SDL_Keycode sdl_arrow_to_digit(SDL_Keycode arrow) {
    switch (arrow) {
        case SDLK_UP:    return SDLK_2;
        case SDLK_DOWN:  return SDLK_8;
        case SDLK_LEFT:  return SDLK_4;
        case SDLK_RIGHT: return SDLK_6;
        default:         return arrow;
    }
}

/* v36.27 remap (поворот презентации) + v36.37 раскладка пути ввода.
 * is_stick: 1 — событие от стика, 0 — от крестовины. */
static SDL_Keycode sdl_dpad_translate(SDL_Keycode arrow, int is_stick) {
    arrow = sdl_rot_map_key(arrow); /* v36.27: поворот презентации */
    if (sdl_dpad_path_digits(is_stick)) return sdl_arrow_to_digit(arrow);
    return arrow;
}

static SDL_Keycode switch_joy_button_to_keycode(int b) {
    switch (b) {
        /* v36.37: крестовина — путь СТРЕЛОК (цифры только при swap ON) */
        case SWJB_UP:    return sdl_dpad_translate(SDLK_UP, 0);
        case SWJB_DOWN:  return sdl_dpad_translate(SDLK_DOWN, 0);
        case SWJB_LEFT:  return sdl_dpad_translate(SDLK_LEFT, 0);
        case SWJB_RIGHT: return sdl_dpad_translate(SDLK_RIGHT, 0);
        case SWJB_A:     return SDLK_5;      /* [KEY-A-PHONE] v36.45: '5' = ITU-T
                                       * OK/FIRE — работает во ВСЕХ слоях:
                                       * getGameAction('5')=FIRE (display.c),
                                       * LCDUI-списки ('5'→FIRE), getKeyStates
                                       * ('5' поднимает FIRE-бит) и
                                       * цифро-панельные порты (Yeti Sports:
                                       * таблица клавиш знает ТОЛЬКО
                                       * '0'..'9','*','#' — старый -5 от A
                                       * был мёртв во ВСЕХ экранах игры).
                                       * -5 остаётся на SPACE (клавиатура).
                                       * Bounce Tales проверяет обе
                                       * (q==2: '5'(15)→7 == -5→7). */
        case SWJB_B:     return SDLK_F2;     /* right soft */
        case SWJB_X:     return SDLK_F1;     /* left soft */
        case SWJB_Y:     return SDLK_c;      /* GAME_C/clear */
        case SWJB_L:     return SDLK_ASTERISK;
        case SWJB_R:     return SDLK_HASH;
        case SWJB_ZR:    return SDLK_0;      /* v36.36: R2 = цифра 0 (запрос пользователя) */
        /* [YSFIX2] v36.40 [ZL-SOFT]: ZL = левая софт-клавиша (дубль X/L3).
         * До сих пор ZL в игре не была назначена ВООБЩЕ. Ключевой бенефициар
         * — одиночные Joy-Con: в их РЕМАПЕ в switch-SDL2 (pad_mapping_left/
         * right_joy) плечевая кнопка SL приходит именно в слоте ZL (SDL 8),
         * так что софт-клавиша получает физическую плечевую кнопку на любом
         * типе пада. На стандартных падах это безвредный третий дубль —
         * F1/F2-слоты повтора игнорируются (см. ysfix1), софт-кнопки не
         * повторяются. ZR остаётся «0» (запрос v36.36). */
        case SWJB_ZL:    return SDLK_F1;    /* ZL (на одиночных Joy-Con — SL) = left soft (-6) */
        /* [YSFIX] v36.39: клики стиков (L3/R3) раньше не были назначены
         * ВООБЩЕ — мёртвые кнопки на любом падe. Дублируем софт-клавиши:
         * у Yeti Sports (и целого класса Canvas-игр с menu-driven UI)
         * софт-кнопки — единственный способ навигации, X/B уже заняты
         * той же ролью, но третья/четвёртая кнопка-дубль бесплатна и
         * ничего не ломает (софт-кнопки не повторяются, F1/F2-слоты
         * повтора игнорируются). */
        case SWJB_LSTICK: return SDLK_F1;    /* L3 = left soft (-6) */
        /* [KEY-DIGIT5] v36.44: R3 = ITU-T цифра '5' (keycode 53). Полевой
         * разбор Yeti Sports: игра ПОЛНОСТЬЮ цифро-панельная — её таблица
         * клавиш (res.bin, запись 0, 2x17) знает ТОЛЬКО '0'..'9','*','#'
         * и софт-клавиши; ветка getGameAction в o.f(I) недостижима для
         * 0/1 (o.d:I = ориентация канвы), поэтому Enter/-5 в таких играх
         * МЁРТВ. При этом getGameAction('5')=FIRE у нас (и на реальных
         * телефонах) — значит '5' работает ВЕЗДЕ: и в таблицах ITU-T,
         * и в getGameAction-играх, и в проверках сырого n==53. Правая
         * софт (-7) остаётся на B — R3 был лишь третьим дублем. */
        case SWJB_RSTICK: return SDLK_5;     /* R3 = '5' (ITU-T FIRE) */
        case SWJB_PLUS:  return 0; /* v34.94: PLUS = per-game settings (was 2nd FIRE) */
        default: return 0;
    }
}

/* v34.90: stick->dpad direction edges from the CENTRAL pump state
 * (hysteresis-protected — see switch_input_pump). Dispatched only by the
 * frame pump.
 * v36.37: путь СТИКА — по умолчанию ЦИФРЫ (swap OFF), стрелки при swap ON. */
static void sdl_switch_dir_dispatch(SdlContext* ctx, int slot) {
    struct { int bit; SDL_Keycode key; } dirs[4] = {
        {1, SDLK_UP}, {2, SDLK_DOWN}, {4, SDLK_LEFT}, {8, SDLK_RIGHT},
    };
    int d = g_sw_in.dir[slot], dp = g_sw_in.dir_prev[slot];
    for (int i = 0; i < 4; i++) {
        int was = (dp >> i) & 1, now = (d >> i) & 1;
        if (was != now)
            sdl_handle_key_event(ctx, sdl_dpad_translate(dirs[i].key, 1), /* v36.37: стик */
                                 now ? true : false, false);
    }
}

/* ======================= in-game pause menu ======================= */

/* Runs while the main frame loop is blocked. v34.97: the VM is parked
 * EXPLICITLY by jvm_frontend_pause_begin() below (virtual time freezes,
 * all interpreter threads park at their next 64-instruction poll) — the
 * v34.81 stall probe no longer participates: with the loop holding
 * run_active=1 it could not tell "blocked in the menu" from "busy in a
 * heavy frame" and froze the game mid-run (trace v34.96). v36.19:
 * три пункта (Продолжить/Настройки игры/Выйти), см. функцию ниже. */
/* v36.19: заливка прямоугольника с клипом (полоса выделения пункта меню
 * паузы; свой мини-хелпер — fill_rect в switch_ui.c статический). */
static void pause_menu_fill(uint32_t* cv, int cw, int ch,
                            int x, int y, int w, int h, uint32_t col) {
    for (int yy = y; yy < y + h; yy++) {
        if (yy < 0 || yy >= ch) continue;
        uint32_t* row = cv + (size_t)yy * (size_t)cw;
        for (int xx = x; xx < x + w; xx++) {
            if (xx < 0 || xx >= cw) continue;
            row[xx] = col;
        }
    }
}

/* ================= v36.23: pause-menu key flush =================
 * FORENSICS (user: «в играх часто залипает кнопка вправо»): the pause
 * menu (and the per-game settings screen inside it) consumed game-key
 * events WITHOUT dispatching them to the VM — a game key RELEASED inside
 * the menu (the natural flow: holding RIGHT, pressing MINUS, letting go
 * of RIGHT while the overlay is up) never reached the game.
 * game_canvas.key_level then kept the bit set forever: getKeyStates()
 * reported the key held on every frame and the game drove itself (the
 * classic «залипает вправо»), and the SDL repeat slot stayed latched —
 * phantom keyRepeated forever, surviving even the next game (file-scope
 * statics). FIX: capture the held keys at menu open (local repeat state
 * cleared at once) and DELIVER their keyReleased to the game right after
 * the pause ends — the same Java call the normal release path makes on
 * the frame thread. The exit path skips the delivery (the session is
 * over) and sdl_switch_game_end wipes the statics either way. */

static void sdl_pause_capture_held(void) {
    g_pause_held = 0;
    for (size_t i = 0; i < KEY_MAP_SIZE && i < 32; i++) {
        if (g_sdl_key_repeat.is_held[i]) {
            g_pause_held |= (1u << i);
            g_sdl_key_repeat.is_held[i] = false; /* stop repeats at once */
        }
    }
    if (g_pause_held)
        LOG_SAFE("[KEYFLUSH] captured %u held game key(s) at pause open\n",
                 (unsigned)__builtin_popcount(g_pause_held));
}

static void sdl_pause_release_captured(SdlContext* ctx, int deliver) {
    extern void midp_call_keyReleased(JVM* jvm, int keycode);
    unsigned dropped = 0, delivered = 0;
    for (size_t i = 0; i < KEY_MAP_SIZE && i < 32; i++) {
        if (!(g_pause_held & (1u << i))) continue;
        g_pause_held &= ~(1u << i);
        int keycode = key_map[i].midp_key ? key_map[i].midp_key
                                          : key_map[i].game_action;
        if (deliver && ctx->jvm && keycode) {
            LOG_SAFE("[KEYFLUSH] pause-flush keyReleased %d\n", keycode);
            midp_call_keyReleased(ctx->jvm, keycode);
            delivered++;
        } else {
            dropped++; /* exit path / no VM / unmapped: state is wiped anyway */
        }
    }
    if (dropped)
        LOG_SAFE("[KEYFLUSH] exit: %u captured key(s) dropped (session over)\n",
                 dropped);
    (void)delivered;
}

/* v34.90: меню паузы (MINUS). v36.19: ЕДИНОЕ меню паузы — его открывают
 * ОБЕ кнопки, MINUS и PLUS; три пункта (Продолжить / Настройки игры /
 * Выйти), курсор вверх/вниз, A (или PLUS) — выбрать, B — продолжить,
 * MINUS — язык, HOME — немедленный выход. Пункт «Настройки игры»
 * открывает прежний экран per-game настроек: VM остаётся запаркованной
 * скобками паузы всё время (экран настроек с v36.19 сам не паркует —
 * скобки begin/end не вложимы), поэтому виртуальное время стоит и в
 * настройках. Возврат из настроек — обратно в это меню. */
static void sdl_switch_pause_menu(SdlContext* ctx) {
    LOG_SAFE("[SWITCH] pause menu open\n");
    sw_trace("pause: open"); /* v34.91 */
    g_switch_pause_open = 1;
    g_switch_pause_req = 0; /* drop duplicate MINUS/PLUS requests */
    sdl_pause_capture_held(); /* v36.23: game keys held at open — see the block comment */
    switch_input_settle(); /* v34.90: drop the opening button latch */
    {
        /* v34.97: паркуем VM ЯВНО и сразу — прежний stall-детектор (300 мс
         * без тика кадра) больше не участвует: цикл держит active=1, и
         * единственный путь взведения паузы — эти скобки begin/end. */
        extern void jvm_frontend_pause_begin(void);
        jvm_frontend_pause_begin();
    }
    uint32_t* cv = sdl_switch_ui_canvas();
    if (!cv) {
        /* v34.97: begin уже взведён — ранний выход обязан его снять. */
        extern void jvm_frontend_pause_end(void);
        jvm_frontend_pause_end();
        g_switch_pause_open = 0;
        sdl_pause_release_captured(ctx, 0); /* v36.23: nothing was swallowed */
        return;
    }

    /* v36.19: три пункта меню паузы */
    static const int PAUSE_ITEMS = 3;
    int sel = 0;
    int exit_game = 0;

    for (;;) {
        unsigned keys = sdl_switch_ui_pump_keys();
        /* v34.90: MINUS switches the UI language here too; the badge in the
         * top-left corner shows the current language and the hint. */
        if (keys & SWK_MINUS) switch_lang_toggle();
        /* v36.26 [TOUCH-UI]: тап по меню паузы. Та же двухфазная модель,
         * что во всех списках фронтенда: первый тап переносит курсор,
         * тап по УЖЕ выделенному пункту активирует его. Тап по бейджу
         * языка (верхний левый угол) = MINUS (переключить язык). */
        {
            int tx = 0, ty = 0;
            if (sdl_switch_touch_tap(&tx, &ty)) {
                if (switch_ui_badge_hit(tx, ty, 24)) {
                    keys |= SWK_MINUS;
                } else {
                    const int prow_step = 48, py0 = 300;
                    if (ty >= py0 && ty < py0 + 3 * prow_step) {
                        int row = (ty - py0) / prow_step;
                        if (row >= 0 && row < 3) {
                            if (sel == row) keys |= SWK_A;
                            else            sel = row;
                        }
                    }
                }
            }
        }
        if (keys & SWK_UP)   sel = (sel + PAUSE_ITEMS - 1) % PAUSE_ITEMS;
        if (keys & SWK_DOWN) sel = (sel + 1) % PAUSE_ITEMS;
        if (keys & SWK_EXIT) { exit_game = 1; break; } /* HOME — немедленный выход */
        if (keys & SWK_B) break;                       /* B — продолжить (назад) */
        if (keys & SWK_A) {
            if (sel == 0) break;                       /* Продолжить */
            if (sel == 2) { exit_game = 1; break; }    /* Выйти в меню */
            /* sel == 1: Настройки игры — прежний экран per-game
             * настроек. VM уже запаркована скобками паузы (v36.19:
             * экран сам не паркует); по B экран вернётся СЮДА. */
            if (g_switch_current_jar[0]) {
                sw_trace("pause: pgset");
                switch_ui_pergame_screen(g_switch_current_jar);
                switch_input_settle(); /* нажатие не должно протечь в меню */
            }
        }

        /* отрисовка: последний кадр игры + полупрозрачное затемнение */
        if (g_texture && g_renderer) {
            const SwitchSettings* st = switch_settings_get();
            int eff_scale = (g_pg_scale >= 0) ? g_pg_scale : st->scale_mode;
            /* v36.15: текстура сессии содержит ПОВЁРНУТЫЙ кадр — считаем
             * прямоугольник в повёрнутой геометрии (та же математика,
             * что в sdl_switch_present_game). */
            int rot = (g_rot_mode && g_rot_buf) ? g_rot_mode : 0;
            int dwr = rot ? ctx->height : ctx->width;
            int dhr = rot ? ctx->width  : ctx->height;
            int x, y, w, h;
            switch_scaling_game_rect(eff_scale, dwr, dhr,
                                     SWITCH_UI_WIDTH, SWITCH_UI_HEIGHT, &x, &y, &w, &h);
            SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
            SDL_RenderClear(g_renderer);
            if (eff_scale == NOJME_SCALE_FIT_BLUR && g_bg_tex && g_bg_buf) {
                SDL_RenderCopy(g_renderer, g_bg_tex, NULL, NULL);
            }
            SDL_RenderCopy(g_renderer, g_texture, NULL,
                           &(SDL_Rect){x, y, w, h});
        }
        /* overlay */
        for (int i = 0; i < SWITCH_UI_WIDTH * SWITCH_UI_HEIGHT; i++)
            cv[i] = 0x90000000u;
        {
            /* v36.19: заголовок по центру + три пункта с полосой выделения
             * (синий акцент и цвета — как в списках switch_ui.c) */
            const char* items[3] = {
                switch_text(ST_PAUSE_RESUME),    /* Продолжить */
                switch_text(ST_PAUSE_SETTINGS),  /* Настройки игры */
                switch_text(ST_PAUSE_EXIT)       /* Выйти */
            };
            const int row_h = 44, row_step = 48, y0 = 300;
            const uint32_t COL_BAR   = 0xFF2E7CF6u; /* COL_SEL_BG switch_ui.c */
            const uint32_t COL_FG    = 0xFFE8ECF4u; /* COL_TEXT */
            const uint32_t COL_SELFG = 0xFFFFFFFFu;
            const char* title = switch_text(ST_PAUSE_TITLE);
            int tw = switch_font_text_width(title, 4);
            switch_font_draw_text(cv, SWITCH_UI_WIDTH, SWITCH_UI_HEIGHT,
                                  (SWITCH_UI_WIDTH - tw) / 2, 196, title,
                                  0xFFFFFFFFu, 0, 4);
            for (int i = 0; i < PAUSE_ITEMS; i++) {
                int iw = switch_font_text_width(items[i], 2);
                int bw = iw + 72;
                if (bw < 320) bw = 320;
                int bx = (SWITCH_UI_WIDTH - bw) / 2;
                int by = y0 + i * row_step;
                if (i == sel)
                    pause_menu_fill(cv, SWITCH_UI_WIDTH, SWITCH_UI_HEIGHT,
                                    bx, by, bw, row_h, COL_BAR);
                switch_font_draw_text(cv, SWITCH_UI_WIDTH, SWITCH_UI_HEIGHT,
                                      (SWITCH_UI_WIDTH - iw) / 2, by + 6,
                                      items[i], i == sel ? COL_SELFG : COL_FG, 0, 2);
            }
            /* v36.19: подсказка управления внизу */
            {
                const char* foot = switch_text(ST_PAUSE_FOOTER);
                int fw = switch_font_text_width(foot, 1);
                switch_font_draw_text(cv, SWITCH_UI_WIDTH, SWITCH_UI_HEIGHT,
                                      (SWITCH_UI_WIDTH - fw) / 2, 676, foot,
                                      0xFFB9C2D4u, 0, 1);
            }
        }
        /* v34.90: language badge in the top-left corner of the pause overlay.
         * v36.25: только нарисованная SDL-кнопка MINUS + короткий код языка
         * («ru»/«en») — длинная текстовая строка удалена. */
        switch_lang_badge_draw(cv, SWITCH_UI_WIDTH, SWITCH_UI_HEIGHT,
                               32, 24, 0xFFB9C2D4);
        /* [BATFIX] v36.41: бейдж заряда в правом верхнем углу оверлея —
         * тот же угол, что у индикатора в обычном кадре игры */
        switch_ui_batt_badge_draw(cv, SWITCH_UI_WIDTH, SWITCH_UI_HEIGHT,
                                  switch_settings_get()->battery_display);
        if (g_ui_tex && g_renderer) {
            SDL_UpdateTexture(g_ui_tex, NULL, cv, SWITCH_UI_WIDTH * sizeof(uint32_t));
            SDL_RenderCopy(g_renderer, g_ui_tex, NULL, NULL);
            SDL_RenderPresent(g_renderer);
        }
        SDL_Delay(16);
    }
    if (exit_game) {
        /* v35.10 FIX: destroyApp исполняет байткод — под ещё активной
         * паузой оверлея он вечно паркуется на первом 64-инструкционном
         * опросе: взаимная блокировка (STUCK stage=events 12-20 с,
         * AsiaRally trace v35.09 — «при выходе не работает
         * управление»). Снимаем паузу ДО вызова; страховка
         * продублирована внутри midlet_call_destroy_app (native.c). */
        {
            extern void jvm_frontend_pause_end(void);
            jvm_frontend_pause_end();
        }
        if (ctx->jvm) midlet_call_destroy_app(ctx->jvm, true);
        ctx->running = false;
        if (ctx->jvm) ctx->jvm->running = false;
        LOG_SAFE("[SWITCH] pause menu -> exit game\n");
    }
    g_switch_pause_open = 0;
    g_switch_pause_req = 0; /* a MINUS/PLUS stolen during the menu must not reopen it */
    switch_input_settle(); /* v34.90: held-through buttons must not re-press */
    {
        /* v34.97: снимаем паузу ДО возврата в цикл кадра — иначе VM остался
         * бы запаркован до первого frame_tick следующей итерации.
         * Идемпотентно: после exit_game пауза уже снята (CAS 1->0). */
        extern void jvm_frontend_pause_end(void);
        jvm_frontend_pause_end();
    }
    /* v36.23: DELIVER the keyReleased events the menu swallowed — the game
     * learns the keys are up (key_level bits clear) and the repeat slots
     * cannot stay latched. Only on resume; on exit the session is over. */
    sdl_pause_release_captured(ctx, !exit_game);
    sw_trace("pause: close"); /* v34.91 */
    LOG_SAFE("[SWITCH] pause menu close\n");
}

/* ======================= touch -> pointer ======================= */

/* v34.90: touch dispatch from the CENTRAL pump state (one
 * pressed/released/dragged transition per frame — motion floods collapse
 * by design). Runs only on the frame thread: pointerPressed/
 * pointerReleased execute Java (execute_method), so delivering them from
 * the game-thread pump was the re-entrancy hole. */
static void sdl_switch_pointer_dispatch(SdlContext* ctx) {
    if (!ctx->jvm) return;
    extern void midp_call_pointerPressed(JVM* jvm, int x, int y);
    extern void midp_call_pointerReleased(JVM* jvm, int x, int y);
    extern void midp_call_pointerDragged(JVM* jvm, int x, int y);

    static int prev_down = 0;
    static int prev_x = -1, prev_y = -1;
    static int ui_owner = 0; /* v36.26: this touch sequence belongs to the MIDP UI */

    /* [YSFIX] v36.39 SOFT-KEY SCREEN MARGINS: a tap in the blank margin
     * LEFT/RIGHT of the game canvas acts as the phone's under-screen soft
     * key (left bar = -6, right bar = -7). Yeti Sports (and a whole class
     * of Canvas games) are soft-key-driven; on a 240x320 canvas presented
     * on 1280x720 the margins are ~370px each — big, comfortable targets
     * that never collide with the game's own pointer input (they are
     * OUTSIDE the canvas rect). Delivery rides the SAME dispatch as the
     * X/B buttons (sdl_handle_key_event): command interception for
     * high-level screens, keyPressed(-6/-7) for canvases, deferred-queue
     * semantics included. A press inside the canvas never enters this
     * path, and a margin press never leaks a pointer event (claimed here). */
    static int margin_soft_key = 0; /* 0=none, 1=left(-6) held, 2=right(-7) held */
    if (g_game_rect.w > 0 && g_game_rect.h > 0) {
        if (g_sw_in.touch_down && !prev_down && !ui_owner) {
            if (g_sw_in.touch_x < g_game_rect.x) {
                margin_soft_key = 1;
                /* [YSFIX2] v36.40: sw_trace — fprintf(stderr) не попадает
                 * в log.txt на устройстве (см. [KEYIN-ALL] выше) */
                sw_trace("[KEYIN] margin tap (left) -> soft key -6");
                sdl_handle_key_event(ctx, SDLK_F1, true, false);
            } else if (g_sw_in.touch_x >= g_game_rect.x + g_game_rect.w) {
                margin_soft_key = 2;
                sw_trace("[KEYIN] margin tap (right) -> soft key -7"); /* [YSFIX2] v36.40 */
                sdl_handle_key_event(ctx, SDLK_F2, true, false);
            }
        }
        if (margin_soft_key) {
            if (!g_sw_in.touch_down) {
                /* release the margin soft key on finger-up */
                sdl_handle_key_event(ctx, margin_soft_key == 1 ? SDLK_F1 : SDLK_F2,
                                     false, false);
                margin_soft_key = 0;
            }
            /* a margin sequence is fully consumed here: no pointer
             * events, no canvas coordinates */
            prev_down = g_sw_in.touch_down;
            prev_x = g_sw_in.touch_x; prev_y = g_sw_in.touch_y;
            return;
        }
    } else {
        margin_soft_key = 0; /* no game rect: feature off */
    }

    /* map window -> game canvas through the CURRENT game rect.
     * v36.15: с поворотом game rect считается в ПОВЁРНУТОЙ геометрии
     * (display DW x DH = ctx->height x ctx->width), так что сначала
     * получаем точку экрана игры в дисплейных координатах, затем
     * ОБРАТНО поворачиваем в координаты канвы — инверсия
     * switch_scaling_rotate90:
     *   rot=1 (90 вправо/CW):  display(x',y') показывает канву (y', H-1-x')
     *   rot=2 (90 влево/CCW):  display(x',y') показывает канву (W-1-y', x')
     * где W x H — размеры канвы. */
    int gx = 0, gy = 0;
    if (g_game_rect.w > 0 && g_game_rect.h > 0) {
        int rot = (g_rot_mode && g_rot_buf) ? g_rot_mode : 0;
        if (rot) {
            int DW = ctx->height, DH = ctx->width; /* дисплейные размеры */
            int dx = (g_sw_in.touch_x - g_game_rect.x) * DW / g_game_rect.w;
            int dy = (g_sw_in.touch_y - g_game_rect.y) * DH / g_game_rect.h;
            if (dx < 0) dx = 0;
            if (dy < 0) dy = 0;
            if (dx >= DW) dx = DW - 1;
            if (dy >= DH) dy = DH - 1;
            if (rot == 1) { gx = dy; gy = (DW - 1) - dx; }
            else          { gx = (DH - 1) - dy; gy = dx; }
            if (gx < 0) gx = 0;
            if (gy < 0) gy = 0;
            if (gx >= ctx->width) gx = ctx->width - 1;
            if (gy >= ctx->height) gy = ctx->height - 1;
        } else {
            gx = (g_sw_in.touch_x - g_game_rect.x) * ctx->width / g_game_rect.w;
            gy = (g_sw_in.touch_y - g_game_rect.y) * ctx->height / g_game_rect.h;
            if (gx < 0) gx = 0;
            if (gy < 0) gy = 0;
            if (gx >= ctx->width) gx = ctx->width - 1;
            if (gy >= ctx->height) gy = ctx->height - 1;
        }
    }
    ctx->input.pointer_x = gx;
    ctx->input.pointer_y = gy;

    /* v36.26 [TOUCH-UI]: high-level MIDP UI (soft keys, command menu,
     * List/Form/TextBox/Alert, virtual keyboard) claims a press BEFORE it
     * can reach the Canvas as a pointer event. The touch SEQUENCE owner is
     * decided on the DOWN edge: a press the UI consumed never leaks a
     * release/drag into the game canvas (the canvas never saw it). */
    {
        extern int midp_ui_touch_hit(JVM* jvm, int x, int y);
        extern int midp_ui_touch_hold(JVM* jvm, int x, int y);
        if (g_sw_in.touch_down && !prev_down) {
            ui_owner = midp_ui_touch_hit(ctx->jvm, gx, gy);
        } else if (ui_owner) {
            ui_owner = midp_ui_touch_hold(ctx->jvm, gx, gy);
        }
        if (ui_owner && !g_sw_in.touch_down) ui_owner = 0; /* sequence over */
    }
    if (!ui_owner) {
        if (g_sw_in.touch_down && !prev_down) {
            ctx->input.pointer_pressed = true;
            midp_call_pointerPressed(ctx->jvm, gx, gy);
        } else if (!g_sw_in.touch_down && prev_down) {
            ctx->input.pointer_pressed = false;
            midp_call_pointerReleased(ctx->jvm, gx, gy);
        } else if (g_sw_in.touch_down && prev_down && g_sw_in.touch_moved &&
                   (gx != prev_x || gy != prev_y)) {
            midp_call_pointerDragged(ctx->jvm, gx, gy);
        }
    }
    prev_down = g_sw_in.touch_down;
    prev_x = gx; prev_y = gy;
}

#endif /* __SWITCH__ && SDL2_AVAILABLE */

/* v36.30: the render-thread guard (arm/check) is defined inside the
 * __SWITCH__ section above, but sdl_init()/sdl_run() call it in ALL
 * builds — the plain app target (host SDL2, no __SWITCH__) failed to
 * compile with an implicit declaration. Provide inert fallbacks for
 * builds outside that section. */
#if !(defined(__SWITCH__) && SDL2_AVAILABLE)
static void sdl_render_thread_arm(void) { (void)0; }
#define sdl_render_thread_check(site) ((void)(site))
#endif


/* Initialize SDL */
int sdl_init(JVM* jvm, int width, int height, int scale, bool headless) {
    /* v34.98: the thread that initializes SDL is THE render thread; every
     * render entry point verifies its caller against it from now on. */
    sdl_render_thread_arm();
    DEBUG_LOG("[SDL] Initializing %dx%d (scale: %d, headless: %s)", width, height, scale, headless ? "yes" : "no");

#ifdef _WIN32
    /* Initialize mutex for Windows */
    if (!g_sdl_mutex_initialized) {
        InitializeCriticalSection(&g_sdl_mutex);
        g_sdl_mutex_initialized = true;
    }
#endif

    /* Use the global context pointer set by main.c */
    if (!g_sdl_ctx_ptr) {
        DEBUG_LOG("[SDL] ERROR: g_sdl_ctx_ptr is NULL - must be set before sdl_init");
        return -1;
    }
    
    SdlContext* ctx = g_sdl_ctx_ptr;
    memset(ctx, 0, sizeof(SdlContext));
    ctx->width = width;
    ctx->height = height;
    ctx->scale = scale;
    ctx->jvm = jvm;
    ctx->running = true;
    ctx->target_fps = 60;  /* Default FPS */
    ctx->headless = headless;  /* Store headless flag */
    ctx->input.pointer_x = 0;
    ctx->input.pointer_y = 0;
    ctx->input.pointer_pressed = false;
    memset(ctx->input.key_states, 0, sizeof(ctx->input.key_states));

    /* v35.02 SINGLE-PUMP: the SDL-family frontends (Switch UI, Windows/SDL2,
     * headless) pump repaints from THIS thread every iteration — arm the
     * protocol so game threads in serviceRepaints() wait for the frontend
     * to paint instead of racing it for the UI lock (the lost-logic-tick
     * machine behind the "race crawls ~1 Hz then freezes" field report).
     * libretro never arms it -> legacy synchronous behavior there. */
    {
        extern void midp_set_external_pump_active(int on);
        midp_set_external_pump_active(1);
    }

    /* Allocate framebuffer */
    ctx->framebuffer = (uint32_t*)malloc(width * height * sizeof(uint32_t));
    if (!ctx->framebuffer) {
        return -1;
    }
    
    /* ИСПРАВЛЕНО: Инициализируем framebuffer чёрным непрозрачным цветом.
     * calloc инициализирует нулями, что означает alpha=0 (прозрачный).
     * Но для J2ME игр нужен чёрный фон с alpha=255 (непрозрачный).
     * Формат ARGB: 0xFF000000 = чёрный, непрозрачный
     */
    for (int i = 0; i < width * height; i++) {
        ctx->framebuffer[i] = 0xFF000000;  /* Чёрный, непрозрачный */
    }

    /* ИСПРАВЛЕНО: В headless режиме не создаём SDL окно */
    if (headless) {
        DEBUG_LOG("[SDL] Headless mode - skipping SDL window creation");
        return 0;
    }

#ifdef __SWITCH__
    /* v34.84 Switch frontend: the window/renderer/UI texture already
     * exist (sdl_switch_platform_init, called from the menu bootstrap);
     * a game session only adds the canvas texture (+ blur bg). */
    if (sdl_switch_game_begin(width, height) != 0) {
        free(ctx->framebuffer);
        ctx->framebuffer = NULL;
        return -1;
    }
    return 0;
#endif

#if SDL2_AVAILABLE
    /* ИСПРАВЛЕНИЕ: Включаем VSync для устранения мерцания и tearing */
    /* Используем OpenGL или DirectX вместо software renderer */
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengl");
    SDL_SetHint(SDL_HINT_RENDER_VSYNC, "1");
    
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) != 0) {
        DEBUG_LOG("[SDL] Failed to initialize: %s", SDL_GetError());
        free(ctx->framebuffer);
        ctx->framebuffer = NULL;
        return -1;

    /* v17: bridge the Nokia Clipboard to the OS clipboard */
    {
        extern void sdl_setup_clipboard_hooks(void);
        sdl_setup_clipboard_hooks();
    }
    }

    g_window = SDL_CreateWindow(
        "J2ME Emulator",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        width * scale, height * scale,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
    );

    if (!g_window) {
        DEBUG_LOG("[SDL] Failed to create window: %s", SDL_GetError());
        SDL_Quit();
        free(ctx->framebuffer);
        ctx->framebuffer = NULL;
        return -1;
    }

    /* ИСПРАВЛЕНО: Пробуем hardware-accelerated renderer с VSync */
    g_renderer = SDL_CreateRenderer(g_window, -1, 
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);

    if (!g_renderer) {
        /* Fallback: try without VSync */
        g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED);
    }
    
    if (!g_renderer) {
        /* Last resort: software renderer */
        DEBUG_LOG("[SDL] Hardware acceleration not available, using software renderer");
        g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_SOFTWARE);
    }

    if (!g_renderer) {
        DEBUG_LOG("[SDL] Failed to create renderer: %s", SDL_GetError());
        SDL_DestroyWindow(g_window);
        SDL_Quit();
        free(ctx->framebuffer);
        ctx->framebuffer = NULL;
        return -1;
    }

    SDL_RenderSetLogicalSize(g_renderer, width, height);

    g_texture = SDL_CreateTexture(g_renderer,
        SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
        width, height);

    if (!g_texture) {
        DEBUG_LOG("[SDL] Failed to create texture: %s", SDL_GetError());
        SDL_DestroyRenderer(g_renderer);
        SDL_DestroyWindow(g_window);
        SDL_Quit();
        free(ctx->framebuffer);
        ctx->framebuffer = NULL;
        return -1;
    }

    DEBUG_LOG("[SDL] Initialized with real SDL2");
#else
    DEBUG_LOG("[SDL] Running in headless mode (no SDL2)");
    DEBUG_LOG("[SDL] Press Ctrl+C to exit");
#endif

    DEBUG_LOG("[SDL] Context: %p, target_fps: %d", (void*)ctx, ctx->target_fps);
    return 0;
}

/* Destroy SDL */
void sdl_destroy(SdlContext* ctx) {
#if SDL2_AVAILABLE
#ifdef __SWITCH__
    /* v34.84: per-GAME teardown only — the shared window/renderer/UI
     * texture live until sdl_switch_platform_shutdown() at app exit. */
    sdl_switch_game_end();
#else
    if (g_audio_dev) {
        SDL_CloseAudioDevice(g_audio_dev);
        g_audio_dev = 0;
    }
    if (g_texture) { SDL_DestroyTexture(g_texture); g_texture = NULL; }
    if (g_renderer) { SDL_DestroyRenderer(g_renderer); g_renderer = NULL; }
    if (g_window) { SDL_DestroyWindow(g_window); g_window = NULL; }
    SDL_Quit();
#endif
#endif

    if (ctx && ctx->framebuffer) {
        free(ctx->framebuffer);
        ctx->framebuffer = NULL;
    }
}

/* v34.72: finished-MIDlet probe (native.c) — see the revive guard in
 * sdl_run(). */
extern bool midlet_is_destroyed(void);

/* v34.89: declared for EVERY SDL2 build (was __SWITCH__-only since
 * v34.84): the SDL_QUIT handler calls it unconditionally, so a desktop
 * GCC 14 build with SDL2 died on an implicit declaration. */
extern void midlet_call_destroy_app(JVM* jvm, bool unconditional);

/* v34.86: the shared key-event handler uses the SDL-only key_map /
 * repeat bookkeeping, and both of its callers (the SDL event loop and
 * the __SWITCH__&&SDL2 gamepad path) exist only when SDL2_AVAILABLE —
 * compile the body only for SDL builds, stub otherwise (no-SDL hosts
 * have no keyboard events by construction). */
#if SDL2_AVAILABLE
/*
 * sdl_handle_key_event — the shared SDL_KEYDOWN/UP body (extracted v34.84
 * so the __SWITCH__ gamepad path can re-dispatch synthesized keyboard
 * events through the identical logic: repeat bookkeeping, soft buttons,
 * command-menu navigation, ESC/F12 handling).
 */
static void sdl_handle_key_event(SdlContext* ctx, SDL_Keycode key, bool pressed,
                                 bool is_repeat) {
    
    /* Track key hold state for software key repeat.
     * On real J2ME phones, holding a key generates repeated
     * keyPressed events. We track hold state here and
     * generate repeats in sdl_process_key_repeat(). */
    /* v17: refresh DeviceControl inactivity timer */
    {
        extern void midp_input_activity_mark(void);
        midp_input_activity_mark();
    }

    /* [KEY-HINT] v36.45: любое нажатие игровой клавиши гасит панель
     * подсказки (до таймаута). Здесь сходятся ВСЕ источники: кнопки пада
     * (диспетчер SWJB_*), стик/тапы краёв (F1/F2/цифры), клавиатура. */

    int repeat_idx = sdl_key_to_repeat_idx(key);
    if (repeat_idx >= 0) { /* v36.23: -1 = unmapped key, no repeat slot */
    if (pressed && !is_repeat) {
        /* Initial press - start tracking for repeat */
        uint64_t now_ts = sdl_get_time_ms();
        g_sdl_key_repeat.is_held[repeat_idx] = true;
        g_sdl_key_repeat.press_time[repeat_idx] = now_ts;
        g_sdl_key_repeat.last_repeat_time[repeat_idx] = now_ts;
    } else if (!pressed) {
        /* Key released - clear repeat state */
        g_sdl_key_repeat.is_held[repeat_idx] = false;
    }
    }
    
    /* Ignore SDL native repeat events - we handle repeat ourselves */
    if (pressed && is_repeat) {
        return;
    }
    
    int midp_key = 0;
    int game_action = 0;

    for (size_t i = 0; i < KEY_MAP_SIZE; i++) {
        if (key_map[i].sdl_key == key) {
            midp_key = key_map[i].midp_key;
            game_action = key_map[i].game_action;
            
            /* Сохраняем состояние клавиши для game_action (положительное) */
            /* midp_key не используем для key_states, т.к. он может быть отрицательным */
            if (game_action > 0 && game_action < 256) {
                ctx->input.key_states[game_action] = pressed;
                GFX_DEBUG("[SDL] Key %s: SDL=%d -> keyCode=%d, gameAction=%d",
                        pressed ? "DOWN" : "UP", key, midp_key, game_action);
            }
            break;
        }
    }

    /* ИСПРАВЛЕНО: Soft button handling (F1/F2) делаем ПЕРВЫМ!
     * По стандарту MIDP:
     * - Если displayable имеет Commands, вызываем commandAction()
     * - Если нет Commands, передаём keyPressed(-6/-7) в игру
     * F1 = -6 (Left Soft Key), F2 = -7 (Right Soft Key)
     */
    if (pressed && ctx->jvm && (key == SDLK_F1 || key == SDLK_F2)) {
        int soft_button = (key == SDLK_F1) ? 0 : 1;
        int soft_keycode = (key == SDLK_F1) ? -6 : -7;
        
        GFX_DEBUG("[SDL] Soft button: key=%s -> button=%d, keyCode=%d",
                key == SDLK_F1 ? "F1" : "F2", soft_button, soft_keycode);
        
        extern bool midp_handle_soft_button(JVM* jvm, int button_index);
        if (!midp_handle_soft_button(ctx->jvm, soft_button)) {
            /* No commands handled - pass keyPressed to game */
            GFX_DEBUG("[SDL] No commands, passing keyPressed(%d) to game", soft_keycode);
            /* [YSFIX] field trace: unconditional but PRESS-gated (one line
             * per actual press — no per-frame spam). Shows the soft key
             * left the frontend layer; if a field log has THIS line but no
             * [KEYQ-SOFT] line, the event died in the deferred queue.
             * [YSFIX2] v36.40: sw_trace — fprintf(stderr) не попадает в
             * log.txt на устройстве, полевая трасса была невидима. */
            sw_trace("[KEYIN] soft press %d -> keyPressed(%d) to game",
                     soft_button, soft_keycode);
            midp_call_keyPressed(ctx->jvm, soft_keycode);
        } else {
            /* [KEYIN]: the command dispatcher consumed the soft key (a
             * Form/List/TextBox with Commands is current) — the game
             * Canvas deliberately does NOT see keyPressed(). */
            sw_trace("[KEYIN] soft press %d -> command dispatched (displayable has Commands)",
                     soft_button); /* [YSFIX2] v36.40: sw_trace */
        }
        return; /* Обработано, не передаём дальше */
    }

    /* Call keyPressed/keyReleased on Canvas */
    if (ctx->jvm && (midp_key || game_action)) {
        /* Check if menu navigation should be handled first */
        if (pressed && game_action) {
            extern bool midp_is_command_menu_open(void);
            extern bool midp_handle_menu_navigation(int direction);
            extern bool midp_handle_soft_button(JVM* jvm, int button_index);
            
            if (midp_is_command_menu_open()) {
                if (game_action == 1) { /* UP */
                    midp_handle_menu_navigation(-1);
                    GFX_DEBUG("[SDL] Menu navigation UP");
                    return; /* Don't pass to game */
                } else if (game_action == 6) { /* DOWN */
                    midp_handle_menu_navigation(1);
                    GFX_DEBUG("[SDL] Menu navigation DOWN");
                    return; /* Don't pass to game */
                } else if (game_action == 8) { /* FIRE - confirm selection */
                    /* Right button (index 1) confirms selection */
                    midp_handle_soft_button(ctx->jvm, 1);
                    GFX_DEBUG("[SDL] Menu FIRE - confirm selection");
                    return; /* Don't pass to game */
                }
            }
        }
        
        /* ИСПРАВЛЕНО: Отправляем keyCode по стандарту Nokia FullCanvas:
         * - UP = -1, DOWN = -2, LEFT = -3, RIGHT = -4, FIRE = -5
         * Игра сама вызовет getGameAction() для конвертации в game action
         */
        int keycode = midp_key ? midp_key : game_action;
        
        if (pressed) {
            DEBUG_LOG("[SDL] Key pressed: SDL=%d -> MIDP keyCode=%d (gameAction=%d)", key, keycode, game_action);
            midp_call_keyPressed(ctx->jvm, keycode);
        } else {
            midp_call_keyReleased(ctx->jvm, keycode);
        }
    }

    if (key == SDLK_ESCAPE && pressed) {
        ctx->running = false;
        if (ctx->jvm) ctx->jvm->running = false;
    }
    
    /* F12: Toggle debug mode at runtime */
    if (key == SDLK_F12 && pressed) {
        bool new_state = j2me_debug_toggle();
        /* Also update the window title to show debug state */
        char title[256];
        snprintf(title, sizeof(title), "J2ME Emulator [Debug: %s]", 
                 new_state ? "ON" : "OFF");
        sdl_set_title(ctx, title);
    }}

/* Process events */
void sdl_process_events(SdlContext* ctx) {
    (void)ctx;  /* used only in the SDL2 backend path */
#if SDL2_AVAILABLE
    /* Forward declarations for MIDP input functions */
    extern void midp_call_keyPressed(JVM* jvm, int keycode);
    extern void midp_call_keyReleased(JVM* jvm, int keycode);
    extern void midp_call_pointerPressed(JVM* jvm, int x, int y);
    extern void midp_call_pointerReleased(JVM* jvm, int x, int y);
    extern void midp_call_pointerDragged(JVM* jvm, int x, int y);
#ifndef __SWITCH__
    SDL_Event event;
    /* Track previous pointer state for drag detection (desktop loop) */
    static int prev_pointer_x = -1;
    static int prev_pointer_y = -1;
    static bool prev_pointer_pressed = false;
#endif

#ifdef __SWITCH__
    /* v34.90 SINGLE-PUMP dispatch: this is the ONLY place SDL events are
     * consumed during a game (see the state-block comment above
     * switch_input_pump). Everything below the desktop loop is bypassed. */
    if (g_switch_pause_req) { /* vestigial latch — kept for log parity */
        g_switch_pause_req = 0;
        sdl_switch_pause_menu(ctx);
    }
    switch_input_pump();
    switch_game_keys_tick(ctx); /* v35.09 sandbox in-game key script (env-gated) */
    /* v34.91: the first frame of the first game session proves the VM is
     * actually RENDERING (missing line = stuck before the frame pump). */
    {
        static int s_first_frame = 1;
        if (s_first_frame) {
            s_first_frame = 0;
            sw_trace("frame: first");
        }
    }
    if (g_sw_in.quit) {
        if (ctx->jvm) midlet_call_destroy_app(ctx->jvm, true);
        ctx->running = false;
        if (ctx->jvm) ctx->jvm->running = false;
    }
    for (int i = 0; i < SWITCH_JOY_SLOTS; i++) {
        uint16_t press = (uint16_t)((g_sw_in.held[i] | g_sw_in.latch[i]) & ~g_sw_in.prev[i]);
        uint16_t release = (uint16_t)(~g_sw_in.held[i] & (g_sw_in.prev[i] | g_sw_in.latch[i]));
        if (press & (1u << SWJB_MINUS)) {
            /* MINUS — in-game pause menu (VM parks via the v34.81 watchdog;
             * virtual time freezes). Handled HERE on the frame thread —
             * never mid-bytecode on a VM fiber. */
            sdl_switch_pause_menu(ctx);
        }
        if (press & (1u << SWJB_PLUS)) {
            /* v36.19: PLUS — ТО ЖЕ ЕДИНОЕ меню паузы, что и MINUS
             * (Продолжить / Настройки игры / Выйти). Раньше PLUS напрямую
             * открывал per-game настройки; теперь они доступны пунктом
             * «Настройки игры» внутри меню паузы. Same frame-thread
             * contract as before. */
            sdl_switch_pause_menu(ctx);
        }
        for (int b = 0; b < 16; b++) {
            if (b == SWJB_MINUS || b == SWJB_PLUS) continue; /* handled above */
            uint16_t bit = (uint16_t)(1u << b);
            if (press & bit) {
                SDL_Keycode mapped = switch_joy_button_to_keycode(b);
                /* [YSFIX2] v36.40: per-press dispatch trace — connects the
                 * raw SDL button id to the emulated key on ANY pad layout
                 * (single Joy-Cons remap the ids, so "what button was that"
                 * is only answerable with this line next to [KEYIN] SDL joy
                 * down). Fires for latch edges of every origin, including
                 * the sandbox FRONT_KEYS injections (E2E coverage). */
                sw_trace("[KEYIN] dispatch btn=%d -> key=%d midp=%d",
                         b, (int)mapped, switch_sdl_key_to_midp(mapped));
                /* re-dispatch through the shared keyboard path: soft buttons,
                 * menu navigation and key repeat bookkeeping all come along. */
                if (mapped) sdl_handle_key_event(ctx, mapped, true, false);
            }
            if (release & bit) {
                SDL_Keycode mapped = switch_joy_button_to_keycode(b);
                if (mapped) sdl_handle_key_event(ctx, mapped, false, false);
            }
        }
        sdl_switch_dir_dispatch(ctx, i);
    }
    sdl_switch_pointer_dispatch(ctx);
    sdl_process_key_repeat(ctx);
    return; /* the desktop event loop below does not run on __SWITCH__ */
#endif /* __SWITCH__ */

#ifndef __SWITCH__
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_QUIT:
                /* FIX (audit T-1, v18): give the MIDlet a chance to run
                 * destroyApp (save progress) before the interpreter stops. */
                if (ctx->jvm) midlet_call_destroy_app(ctx->jvm, true);
                ctx->running = false;
                if (ctx->jvm) ctx->jvm->running = false;
                break;

            case SDL_KEYDOWN:
            case SDL_KEYUP:
                sdl_handle_key_event(ctx, event.key.keysym.sym,
                                     event.type == SDL_KEYDOWN,
                                     event.key.repeat ? true : false);
                break;

            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP:
                {
                    bool pressed = (event.type == SDL_MOUSEBUTTONDOWN);
                    int x = event.button.x / ctx->scale;
                    int y = event.button.y / ctx->scale;
                    
                    ctx->input.pointer_x = x;
                    ctx->input.pointer_y = y;
                    ctx->input.pointer_pressed = pressed;
                    
                    /* Call pointer event handlers */
                    if (ctx->jvm) {
                        if (pressed && !prev_pointer_pressed) {
                            /* pointerPressed: transition from not pressed to pressed */
                            DEBUG_LOG("[SDL] Pointer pressed at (%d, %d)", x, y);
                            midp_call_pointerPressed(ctx->jvm, x, y);
                        } else if (!pressed && prev_pointer_pressed) {
                            /* pointerReleased: transition from pressed to not pressed */
                            DEBUG_LOG("[SDL] Pointer released at (%d, %d)", x, y);
                            midp_call_pointerReleased(ctx->jvm, x, y);
                        }
                    }
                    
                    prev_pointer_x = x;
                    prev_pointer_y = y;
                    prev_pointer_pressed = pressed;
                }
                break;

            case SDL_MOUSEMOTION:
                {
                    int x = event.motion.x / ctx->scale;
                    int y = event.motion.y / ctx->scale;
                    
                    ctx->input.pointer_x = x;
                    ctx->input.pointer_y = y;
                    
                    /* Check for drag: pointer is pressed and moving */
                    if (ctx->jvm && prev_pointer_pressed && 
                        (x != prev_pointer_x || y != prev_pointer_y)) {
                        DEBUG_LOG("[SDL] Pointer dragged to (%d, %d)", x, y);
                        midp_call_pointerDragged(ctx->jvm, x, y);
                    }
                    
                    prev_pointer_x = x;
                    prev_pointer_y = y;
                }
                break;

            case SDL_WINDOWEVENT:
                if (event.window.event == SDL_WINDOWEVENT_RESIZED) {
                    int new_w = event.window.data1;
                    int new_h = event.window.data2;
                    ctx->scale = (new_w / ctx->width < new_h / ctx->height)
                        ? new_w / ctx->width : new_h / ctx->height;
                    if (ctx->scale < 1) ctx->scale = 1;
                }
                break;
        }
    }
#endif /* !__SWITCH__ */
    
    /* Process software key repeat after all events are handled.
     * Generates repeated keyPressed events for held game keys. */
    sdl_process_key_repeat(ctx);
#endif
}

#else /* !SDL2_AVAILABLE */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((unused))
#endif
static void sdl_handle_key_event(SdlContext* ctx, SDL_Keycode key, bool pressed,
                                 bool is_repeat) {
    (void)ctx; (void)key; (void)pressed; (void)is_repeat; /* no-SDL no-op */
}
#endif /* SDL2_AVAILABLE */

/* Minimal event processing for cooperative threading - called from Thread.sleep() */
void sdl_process_events_minimal(void) {
#if SDL2_AVAILABLE
    if (!g_sdl_ctx_ptr) return;
    
    /* Only process events if we have a valid SDL window */
    if (!g_window) return;

#ifdef __SWITCH__
    /* v34.90 SINGLE-PUMP RULE: the game thread NEVER drains the SDL queue.
     * This pump used to run from thread_yield()/Thread.sleep — MID-BYTECODE
     * on a yielding VM fiber — and re-dispatched soft keys / FIRE with the
     * command menu open / touches through paths that execute Java
     * re-entrantly (execute_method from the middle of the yield): the
     * «buttons work, then stop; cannot enter any menu item» bug. Input
     * now flows exclusively through the frame pump (<= 1 frame latency). */
    return;
#endif
    
    /* ИСПРАВЛЕНО: Кооперативные потоки выполняются в контексте главного потока
     * через ucontext, поэтому SDL_PollEvent безопасен для вызова.
     * Обрабатываем события клавиш, чтобы игры могли реагировать на ввод
     * даже во время выполнения своих игровых циклов. */
    SDL_Event event;
    
    /* Forward declarations for MIDP input functions */
    extern void midp_call_keyPressed(JVM* jvm, int keycode);
    extern void midp_call_keyReleased(JVM* jvm, int keycode);
    
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_QUIT:
                g_sdl_ctx_ptr->running = false;
                if (g_sdl_ctx_ptr->jvm) g_sdl_ctx_ptr->jvm->running = false;
                break;

            case SDL_KEYDOWN:
            case SDL_KEYUP:
                {
                    bool pressed = (event.type == SDL_KEYDOWN);
                    SDL_Keycode key = event.key.keysym.sym;
                    
                    /* Track key hold state for software key repeat */
                    int repeat_idx2 = sdl_key_to_repeat_idx(key);
                    if (repeat_idx2 >= 0) { /* v36.23: -1 = unmapped key */
                    if (pressed && !event.key.repeat) {
                        uint64_t now_ts2 = sdl_get_time_ms();
                        g_sdl_key_repeat.is_held[repeat_idx2] = true;
                        g_sdl_key_repeat.press_time[repeat_idx2] = now_ts2;
                        g_sdl_key_repeat.last_repeat_time[repeat_idx2] = now_ts2;
                    } else if (!pressed) {
                        g_sdl_key_repeat.is_held[repeat_idx2] = false;
                    }
                    }
                    
                    /* Ignore SDL native repeat events - we handle repeat ourselves */
                    if (pressed && event.key.repeat) {
                        break;
                    }
                    
                    int midp_key = 0;
                    int game_action = 0;

                    for (size_t i = 0; i < KEY_MAP_SIZE; i++) {
                        if (key_map[i].sdl_key == key) {
                            midp_key = key_map[i].midp_key;
                            game_action = key_map[i].game_action;
                            break;
                        }
                    }

                    /* ИСПРАВЛЕНО: Soft button handling (F1/F2) делаем ПЕРВЫМ! */
                    if (pressed && g_sdl_ctx_ptr->jvm && (key == SDLK_F1 || key == SDLK_F2)) {
                        int soft_button = (key == SDLK_F1) ? 0 : 1;
                        int soft_keycode = (key == SDLK_F1) ? -6 : -7;
                        
                        extern bool midp_handle_soft_button(JVM* jvm, int button_index);
                        if (!midp_handle_soft_button(g_sdl_ctx_ptr->jvm, soft_button)) {
                            midp_call_keyPressed(g_sdl_ctx_ptr->jvm, soft_keycode);
                        }
                        break; /* Обработано, не передаём дальше */
                    }

                    /* Call keyPressed/keyReleased on Canvas */
                    /* Используем keyCode (midp_key) по стандарту Nokia FullCanvas */
                    if (g_sdl_ctx_ptr->jvm && (midp_key || game_action)) {
                        int keycode = midp_key ? midp_key : game_action;
                        
                        if (pressed) {
                            DEBUG_LOG("[SDL] Minimal: Key pressed SDL=%d -> keyCode=%d", key, keycode);
                            midp_call_keyPressed(g_sdl_ctx_ptr->jvm, keycode);
                        } else {
                            midp_call_keyReleased(g_sdl_ctx_ptr->jvm, keycode);
                        }
                    }

                    if (key == SDLK_ESCAPE && pressed) {
                        g_sdl_ctx_ptr->running = false;
                        if (g_sdl_ctx_ptr->jvm) g_sdl_ctx_ptr->jvm->running = false;
                    }
                }
                break;

        }
    }
    
    /* Process software key repeat for minimal event loop too */
    sdl_process_key_repeat(g_sdl_ctx_ptr);
#endif
}

/* Set global context (called by main.c before sdl_init) */
void sdl_set_global_context(SdlContext* ctx) {
    g_sdl_ctx_ptr = ctx;
    DEBUG_LOG("[SDL] Global context set to %p", (void*)ctx);
}

/* Get global context */
SdlContext* sdl_get_global_context(void) {
    return g_sdl_ctx_ptr;
}

/* ИСПРАВЛЕНО: Thread-safe запрос на перерисовку экрана */
void sdl_request_redraw(void) {
    if (g_sdl_ctx_ptr) {
        atomic_store_explicit(&g_sdl_ctx_ptr->needs_redraw, true, memory_order_release);
        /* DEBUG: Log first 20 calls */
        static int redraw_count = 0;
        redraw_count++;
        if (redraw_count <= 20) {
            LOG_SAFE("[SDL] sdl_request_redraw() #%d: set needs_redraw=true, ptr=%p\n", 
                    redraw_count, (void*)g_sdl_ctx_ptr);
        }
    } else {
        LOG_SAFE("[SDL] WARNING: sdl_request_redraw() called but g_sdl_ctx_ptr is NULL!\n");
    }
}

/* Check if redraw is needed */
bool sdl_needs_redraw(SdlContext* ctx) {
    return ctx ? atomic_load_explicit(&ctx->needs_redraw, memory_order_acquire) : false;
}

/* Clear redraw flag */
void sdl_clear_redraw(SdlContext* ctx) {
    if (ctx) {
        atomic_store_explicit(&ctx->needs_redraw, false, memory_order_release);
    }
}

/* v35.04 stage-cost gauge: keep the worst single elapsed (ms) since t0 in
 * the per-period slot. Worst-value only (no sum) — one giant paint must
 * stand out the same way mx= does for whole iterations. Racy write is
 * acceptable (diagnostics). */
/* v36.42: static inline — в сборках без SDL2_AVAILABLE (app) блоки-
 * вызывающие внутри sdl_run компилируются в ноль, и GCC previously
 * предупреждал -Wunused-function; inline-хелпер этого не вызывает. */
static inline void sd_stage_max(volatile uint32_t* slot, const struct timespec* t0) {
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    int64_t ms = (int64_t)(t1.tv_sec - t0->tv_sec) * 1000 +
                 (int64_t)(t1.tv_nsec - t0->tv_nsec) / 1000000;
    if (ms > 0 && (uint32_t)ms > *slot) *slot = (uint32_t)ms;
}

/* Run main loop */
void sdl_run(SdlContext* ctx) {
    if (!ctx) {
        DEBUG_LOG("[SDL] ERROR: sdl_run called with NULL context");
        return;
    }
    /* v34.98: sdl_run owns the frame loop — (re)arm the render-thread
     * reference here in case the loop thread differs from sdl_init's. */
    sdl_render_thread_arm();
    
    /* Check if context was properly initialized */
    if (ctx->target_fps <= 0 || ctx->framebuffer == NULL) {
        DEBUG_LOG("[SDL] ERROR: Context not properly initialized");
        DEBUG_LOG("[SDL] target_fps=%d, framebuffer=%p, width=%d, height=%d",
                ctx->target_fps, (void*)ctx->framebuffer, ctx->width, ctx->height);
        DEBUG_LOG("[SDL] This usually means sdl_init() failed or was not called");
        return;
    }

    DEBUG_LOG("[SDL] Running main loop (FPS: %d, %dx%d)", ctx->target_fps, ctx->width, ctx->height);

    /* External function from display.c for callSerially processing */
    extern void midp_process_call_serially_queue(JVM* jvm);
    extern void jvm_process_timers(JVM* jvm);
    extern bool midp_check_alert_timeout(JVM* jvm);
    
    /* ИСПРАВЛЕНО: Функция для вызова paint() на текущем displayable */
    extern void midp_repaint_current(JVM* jvm);

    /* ИСПРАВЛЕНО: Если running=false, принудительно устанавливаем true */
    if (!ctx->running) {
        LOG_SAFE("[HEADLESS] WARNING: ctx->running was false, forcing to true\n");
        ctx->running = true;
    }
    /* v34.72: revive the VM ONLY when it did not deliberately finish
     * (notifyDestroyed/System.exit inside startApp). Reviving a finished
     * VM left a dead pump on a frozen screen forever — the finished
     * MIDlet now falls through to the explicit black finished screen. */
    if (ctx->jvm && !ctx->jvm->running &&
        !ctx->jvm->exiting && !midlet_is_destroyed()) {
        LOG_SAFE("[HEADLESS] WARNING: ctx->jvm->running was false, forcing to true\n");
        ctx->jvm->running = true;
    }

#if SDL2_AVAILABLE
    /* Headless-style mode: SDL2 available but no window (headless or headless-like) */
    /* ИСПРАВЛЕНО: Проверяем g_window ПЕРЕД основным циклом, а не после */
    if (!g_window) {
        /* Headless mode - simple delay loop (SDL2 available but no window created) */
        
        LOG_SAFE("[HEADLESS] Starting headless main loop (SDL2 available, no window)\n");
        LOG_SAFE("[HEADLESS] ctx=%p, ctx->running=%d, ctx->jvm=%p, ctx->jvm->running=%d\n",
                (void*)ctx, ctx->running, (void*)ctx->jvm, ctx->jvm ? ctx->jvm->running : -1);
        
        /* ИСПРАВЛЕНО: Используем clock_gettime вместо SDL_GetTicks, т.к. SDL не инициализирован */
        uint64_t last_auto_redraw = 0;
        const uint64_t auto_redraw_interval = 16;  /* ~60 FPS */
        int headless_loop_count = 0;
        
        /* Get current time using clock_gettime */
        struct timespec start_ts;
        clock_gettime(CLOCK_MONOTONIC, &start_ts);
        last_auto_redraw = (uint64_t)start_ts.tv_sec * 1000 + start_ts.tv_nsec / 1000000;
        
        while (ctx->running && ctx->jvm && ctx->jvm->running) {
            headless_loop_count++;
            
            /* Check for error display mode */
            if (sdl_has_error()) {
                sdl_draw_error_screen(ctx);
                sdl_present(ctx);
                
                /* Short delay in error mode */
#ifdef _WIN32
                Sleep(16);
#else
                struct timespec ts = { .tv_sec = 0, .tv_nsec = 16000000 };
                nanosleep(&ts, NULL);
#endif
                continue;
            }
            
            /* Process timers */
            jvm_process_timers(ctx->jvm);
            
            /* v35: age key queue + fallback delivery (event-driven menus) */
            {
                extern void midp_frame_tick(void);
                extern void midp_pump_deferred_keys(JVM* jvm);
                midp_frame_tick();
                midp_pump_deferred_keys(ctx->jvm);
            }

            /* Process callSerially queue */
            midp_process_call_serially_queue(ctx->jvm);
            
            /* Check Alert timeout */
            midp_check_alert_timeout(ctx->jvm);
            
            /* ИСПРАВЛЕНО: Автоматическая перерисовка в headless режиме */
            struct timespec current_ts;
            clock_gettime(CLOCK_MONOTONIC, &current_ts);
            uint64_t current_time = (uint64_t)current_ts.tv_sec * 1000 + current_ts.tv_nsec / 1000000;
            uint64_t elapsed = current_time - last_auto_redraw;
            
            if (elapsed >= auto_redraw_interval) {
                ctx->needs_redraw = true;
                last_auto_redraw = current_time;
                if (headless_loop_count <= 10) {
                    LOG_SAFE("[HEADLESS] Auto-redraw at iteration %d (elapsed=%lu ms)\n", 
                            headless_loop_count, (unsigned long)elapsed);
                }
            }
            
            /* Process pending repaints */
            if (ctx->needs_redraw) {
                /* v35.02: conditional clear — only a pump that ran may
                 * consume the pending state (see the Switch main loop). */
                int painted = midp_process_repaints(ctx->jvm);
                ctx->needs_redraw = false;

                /* ИСПРАВЛЕНО: Сигнализируем что repaint обработан */
                if (painted) {
                    extern void midp_clear_pending_repaint(void);
                    midp_clear_pending_repaint();
                }
                
                /* В J2ME, поток с необработанным исключением НЕ останавливает приложение.
                 * Только этот поток завершается, остальные продолжают работать.
                 */
                
                /* ИСПРАВЛЕНО: Сохраняем кадр в файл для отладки */
                if (headless_loop_count <= 5 || headless_loop_count % 30 == 0) {
                    char filename[256];
                    snprintf(filename, sizeof(filename), "/tmp/j2me_frame_%d.ppm", headless_loop_count);
                    sdl_save_framebuffer_to_file(ctx, filename);
                }
            }
            
            /* ИСПРАВЛЕНО: В headless режиме SDL не инициализирован, используем nanosleep */
#ifdef _WIN32
            Sleep(16);
#else
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 16000000 };
            nanosleep(&ts, NULL);
#endif
            
            /* Limit headless execution time */
            if (headless_loop_count > 6000) {
                LOG_SAFE("[HEADLESS] Maximum iterations reached\n");
                break;
            }
        }
        return;  /* Headless mode done */
    }
    
    /* Normal SDL2 mode with window - main loop */
    uint64_t last_time = SDL_GetTicks();
    const uint64_t frame_time = 1000 / ctx->target_fps;
    
    ctx->needs_redraw = true;
    uint64_t last_auto_redraw = SDL_GetTicks();
    const uint64_t auto_redraw_interval = 16;

    int loop_count = 0;
    while (ctx->running && ctx->jvm && ctx->jvm->running) {
        loop_count++;
        struct timespec sd_iter_t0;
        clock_gettime(CLOCK_MONOTONIC, &sd_iter_t0);
#ifdef __SWITCH__
        /* v34.97 FREEZE FIX: только frame_tick — стоявший следом run_exit
         * обнулял g_frontend_run_active на ВСЮ оставшуюся часть итерации,
         * из-за чего stall-детектор мерил ПОЛНОЕ время итерации и взводил
         * паузу посреди игры, как только одна итерация превышала 300 мс
         * (тяжёлый кадр + GC-шторм: фриз из trace v34.96, fl 50->84->160
         * прямо перед STUCK stage=repaints). Пока цикл жив, active всё
         * время TRUE — детектор больше не может взвести паузу сам; оверлеи
         * (меню MINUS, per-game экран PLUS) делают это детерминированно
         * через jvm_frontend_pause_begin/end (v34.97). */
        {
            extern void jvm_frontend_frame_tick(void);
            jvm_frontend_frame_tick();
        }
        SW_DIAG_STAGE("watchdog");
#endif
        
        /* Check for error display mode */
        if (sdl_has_error()) {
            sdl_draw_error_screen(ctx);
            
            SDL_LOCK();
            sdl_update_texture(ctx);
            SDL_RenderClear(g_renderer);
            SDL_RenderCopy(g_renderer, g_texture, NULL, NULL);
            SDL_RenderPresent(g_renderer);
            SDL_UNLOCK();
            
            /* Still process events so ESC works */
            sdl_process_events(ctx);
            
            SDL_Delay(16);  /* ~60 FPS in error mode */
            continue;
        }
        
        if (loop_count % 60 == 0) {
            DEBUG_LOG("[MAIN_LOOP] Still running, iteration=%d, needs_redraw=%d", loop_count, ctx->needs_redraw);
        }
        
        SW_DIAG_STAGE("events");
        sdl_process_events(ctx);
        /* v36.05 [MIDLET-STUCK] self-recovery consumer: the heartbeat
         * watchdog raised g_midp_stuck_recover_req after ~15 s of VM dead
         * air (loader thread died uncaught — see [VM-THREAD-DEATH] — and
         * the midlet thread spins forever; the Doom RPG [Rus] 2nd-launch
         * hang). Perform the SAME graceful exit as the pause-menu
         * "exit to menu" (destroyApp -> running=false), so the user lands
         * in the frontend menu instead of a dead loading screen. Runs on
         * THIS thread (the session owner) — never from the timer thread. */
        if (g_midp_stuck_recover_req) {
            g_midp_stuck_recover_req = 0; /* consume once */
            sw_trace("[MIDLET-STUCK] consuming recovery request: graceful exit to menu");
            LOG_SAFE("[SWITCH] midlet stuck (VM dead air) -> graceful exit to menu\n");
            if (ctx->jvm) midlet_call_destroy_app(ctx->jvm, true);
            ctx->running = false;
            if (ctx->jvm) ctx->jvm->running = false;
        }
        /* v35.04 STAGE-COST GAUGES: worst single ms per diag period for the
         * three VM-hosting stages (fields tj=/rj=/sj= in the diag line).
         * The v35.03 race trace showed 19 ms iterations with present at
         * 1-3 ms — loop/stl/mx cannot say WHERE the time went; these three
         * gauges name the stage directly. Cheap: one clock_gettime pair per
         * stage, worst-value only, no allocation. */
        SW_DIAG_STAGE("timers");
        {
            struct timespec sd_tj_t0;
            clock_gettime(CLOCK_MONOTONIC, &sd_tj_t0);
            /* v35.05 SHOWNOTIFY PUMP (Bobby Carrot 4 freeze-on-load fix):
             * the deferred showNotify from Display.setCurrent (v34.33) was
             * delivered by the headless and libretro loops but NEVER by this
             * Switch/SDL2 frame loop — on Switch, Canvas.showNotify() did
             * not fire AT ALL. Games that gate their paint loop on a
             * showNotify-set flag (BC4: `if (this.d && ...) repaint()`,
             * d=false forever) painted the first frame and then froze with
             * a live game thread and music playing: [NO-FRAME] fl=0,
             * skip=loop, td m stale at the last drawImage. Deliver here,
             * once per iteration, BEFORE timers/repaints/callSerially —
             * the same position the headless loop uses. */
            {
                extern void midp_pump_pending_shownotify(JVM* jvm);
                midp_pump_pending_shownotify(ctx->jvm);
            }
            jvm_process_timers(ctx->jvm);
            {
                /* v35: age key queue + fallback delivery (event-driven menus) */
                extern void midp_frame_tick(void);
                extern void midp_pump_deferred_keys(JVM* jvm);
                midp_frame_tick();
                midp_pump_deferred_keys(ctx->jvm);
            }
            sd_stage_max(&g_sd_tj_ms_max, &sd_tj_t0);
        }
        uint64_t current_time = SDL_GetTicks();
        bool current_needs_redraw = atomic_load_explicit(&ctx->needs_redraw, memory_order_acquire);
        if (!current_needs_redraw && (current_time - last_auto_redraw) >= auto_redraw_interval) {
            atomic_store_explicit(&ctx->needs_redraw, true, memory_order_release);
            last_auto_redraw = current_time;
        }

        /* DEBUG: Log first 30 checks */
        if (loop_count <= 30) {
            bool needs_redraw_val = atomic_load_explicit(&ctx->needs_redraw, memory_order_acquire);
            LOG_SAFE("[MAIN_LOOP] iteration=%d, needs_redraw=%d\n", loop_count, needs_redraw_val);
        }

        bool needs_redraw_val = atomic_load_explicit(&ctx->needs_redraw, memory_order_acquire);
        if (needs_redraw_val) {
            /* Clear the flag BEFORE processing to avoid race condition */
            atomic_store_explicit(&ctx->needs_redraw, false, memory_order_release);

            struct timespec sd_rj_t0;
            clock_gettime(CLOCK_MONOTONIC, &sd_rj_t0);
            SW_DIAG_STAGE("repaints");
            /* v35.02: pump FIRST, then drain the game-logic queue. MIDP
             * ordering: a queued Runnable (the game's frame logic) runs
             * after the repaint it requested has been painted. The old
             * order (drain before pump) made the drain's repaint gate
             * defer the logic on EVERY iteration where the game thread had
             * a paint in flight — under load that collapsed the logic
             * cadence to ~1 Hz (the race crawl) and then to 0 (freeze). */
            {
                extern int midp_process_repaints(JVM* jvm);
                int painted = midp_process_repaints(ctx->jvm);
                /* v35.02 CONDITIONAL CLEAR: only a pump that actually ran
                 * may consume the pending state. The old unconditional
                 * clear also fired after SKIPPED pumps (UI lock busy) and
                 * (a) dropped g_dirty_valid = lost frame content,
                 * (b) told the game thread's serviceRepaints a paint had
                 * completed when none had — both desynchronize the
                 * handshake and starve the logic queue. */
                if (painted) {
                    extern void midp_clear_pending_repaint(void);
                    midp_clear_pending_repaint();
                }
            }
            sd_stage_max(&g_sd_rj_ms_max, &sd_rj_t0);

            /* В J2ME, поток с необработанным исключением НЕ останавливает приложение.
             * Только этот поток завершается, остальные продолжают работать.
             */
        }

        SW_DIAG_STAGE("callserially");
        {
            struct timespec sd_sj_t0;
            clock_gettime(CLOCK_MONOTONIC, &sd_sj_t0);
            midp_process_call_serially_queue(ctx->jvm);
            midp_check_alert_timeout(ctx->jvm);
            sd_stage_max(&g_sd_sj_ms_max, &sd_sj_t0);
        }

        SW_DIAG_STAGE("present");
#ifdef __SWITCH__
        /* v34.94 ASPECT FIX: the old inline RenderCopy used a NULL dst rect —
         * SDL stretched the game texture over the WHOLE screen regardless of
         * the Масштаб setting (the pause menu drew with the correct rect,
         * which is exactly the user's "только в паузе пропорции верные").
         * Present through the same path the pause menu uses. */
        SDL_LOCK();
        sdl_switch_present_game(ctx);
        SDL_UNLOCK();
#else
        SDL_LOCK();
        sdl_update_texture(ctx);
        SDL_RenderClear(g_renderer);
        SDL_RenderCopy(g_renderer, g_texture, NULL, NULL);
        SDL_RenderPresent(g_renderer);
        SDL_UNLOCK();
#endif

        /* v50 (Asphalt image freeze): end-of-frame vsync tick — game-thread
         * serviceRepaints() paces itself on this (KVM vblank semantics). */
        SW_DIAG_STAGE("vsync");
        {
            extern void midp_vsync_tick(void);
            midp_vsync_tick();
            __sync_fetch_and_add(&g_sd_vs, 1);
        }

        SW_DIAG_STAGE("pace");
#ifdef __SWITCH__
        /* v34.94 diag: iteration bookkeeping (loop/stl) + 5 s diag line. */
        {
            struct timespec sd_iter_t1;
            clock_gettime(CLOCK_MONOTONIC, &sd_iter_t1);
            int64_t iter_ms = (int64_t)(sd_iter_t1.tv_sec - sd_iter_t0.tv_sec) * 1000 +
                             (int64_t)(sd_iter_t1.tv_nsec - sd_iter_t0.tv_nsec) / 1000000;
            int64_t over_ms = iter_ms - 17;
            if (over_ms > 0) __sync_fetch_and_add(&g_sd_stl_ms, (uint32_t)over_ms);
            /* v35.01: worst single iteration this period (mx=) — splits
             * "uniformly slow" (many mid-size stalls) from "one giant
             * stall" (a blocked present / a hundred-ms paint); stl alone
             * cannot tell 78 x 17 ms from 1 x 1300 ms. */
            if (iter_ms > (int64_t)g_sd_iter_ms_max)
                g_sd_iter_ms_max = (uint32_t)iter_ms;
            __sync_fetch_and_add(&g_sd_loop, 1);
            sw_diag_tick();
        }
#endif
        current_time = SDL_GetTicks();
        uint64_t elapsed = current_time - last_time;
        if (elapsed < frame_time) {
            SDL_Delay(1);
        }
        last_time = SDL_GetTicks();
    }

#ifdef __SWITCH__
    /* v34.94: game loop left — disarm the STUCK watchdog (the frontend
     * menu runs its own pump loop and must not be reported as stuck). */
    SW_DIAG_STAGE(NULL);
#endif

    /* v34.72: the loop above exited because the MIDlet finished
     * (ctx->jvm->running == false while ctx->running is still true —
     * the window-close path clears BOTH). Show the explicit "finished"
     * screen and keep the window responsive until the user closes it;
     * without this the window vanished without any indication that the
     * MIDlet had ended normally. */
    if (ctx->running && ctx->jvm && !ctx->jvm->running && !sdl_has_error()) {
        LOG_SAFE("[SDL] MIDlet finished — showing finished screen until window close\n");
        sdl_draw_midlet_finished_screen(ctx);
        SDL_LOCK();
        sdl_update_texture(ctx);
        SDL_RenderClear(g_renderer);
        SDL_RenderCopy(g_renderer, g_texture, NULL, NULL);
        SDL_RenderPresent(g_renderer);
        SDL_UNLOCK();
#ifdef __SWITCH__
        /* Switch: show the finished screen until A/B or 3 s, then fall
         * through — control returns to the frontend menu (switch main).
         * v34.90: same single pump as everything else (hotplug included). */
        {
            uint64_t t0 = SDL_GetTicks();
            switch_input_reset();
            for (;;) {
                unsigned keys = sdl_switch_ui_pump_keys();
                if (keys & (SWK_A | SWK_B) || keys & SWK_EXIT) break;
                if (SDL_GetTicks() - t0 > 3000) break;
                SDL_Delay(16);
            }
        }
#else
        for (;;) {
            SDL_Event ev;
            bool quit = false;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT) quit = true;
                if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE) quit = true;
            }
            if (quit) break;
            SDL_Delay(16);
        }
#endif
    }
#else
    /* Headless mode - simple delay loop (No SDL2) */
    uint64_t last_auto_redraw = 0;
    const uint64_t auto_redraw_interval = 16;  /* ~60 FPS */
    int headless_loop_count = 0;
    
    LOG_SAFE("[HEADLESS] Starting headless main loop (No SDL2)\n");
    
    /* ИСПРАВЛЕНО: Устанавливаем начальную перерисовку */
    ctx->needs_redraw = true;
    last_auto_redraw = 0;
    
    while (ctx->running && ctx->jvm && ctx->jvm->running) {
        headless_loop_count++;
        
        /* Process timers */
        jvm_process_timers(ctx->jvm);
        
        /* Process callSerially queue */
        midp_process_call_serially_queue(ctx->jvm);
        
        /* Check Alert timeout */
        midp_check_alert_timeout(ctx->jvm);
        
        /* ИСПРАВЛЕНО: Автоматическая перерисовка в headless режиме */
        uint64_t current_time = sdl_get_ticks(ctx);
        uint64_t elapsed = current_time - last_auto_redraw;
        
        if (elapsed >= auto_redraw_interval) {
            ctx->needs_redraw = true;
            last_auto_redraw = current_time;
            if (headless_loop_count <= 10) {
                LOG_SAFE("[HEADLESS] Auto-redraw at iteration %d (elapsed=%lu ms)\n", 
                        headless_loop_count, (unsigned long)elapsed);
            }
        }
        
        /* Process pending repaints */
        if (ctx->needs_redraw) {
            /* v35.02: conditional clear — only a pump that ran may consume
             * the pending state (see the Switch main loop). */
            int painted = midp_process_repaints(ctx->jvm);
            ctx->needs_redraw = false;

            /* ИСПРАВЛЕНО: Сигнализируем что repaint обработан */
            if (painted) {
                extern void midp_clear_pending_repaint(void);
                midp_clear_pending_repaint();
            }
            
            /* В J2ME, поток с необработанным исключением НЕ останавливает приложение.
             * Только этот поток завершается, остальные продолжают работать.
             */
        }
        
#ifdef _WIN32
        Sleep(16);  /* ~60 FPS on Windows */
#else
        usleep(16000);  /* ~60 FPS on POSIX */
#endif

        /* v50 (Asphalt image freeze): vsync tick for this fallback loop too. */
        {
            extern void midp_vsync_tick(void);
            midp_vsync_tick();
        }

        /* Limit headless execution time */
        if (headless_loop_count > 6000) {
            LOG_SAFE("[HEADLESS] Maximum iterations reached\n");
            break;
        }
    }
#endif
}

/* Stop */
void sdl_stop(SdlContext* ctx) {
    if (ctx) ctx->running = false;
}

/* Get framebuffer */
uint32_t* sdl_get_framebuffer(SdlContext* ctx) {
    return ctx ? ctx->framebuffer : NULL;
}

/* Clear */
void sdl_clear(SdlContext* ctx, uint32_t color) {
    if (!ctx || !ctx->framebuffer) return;
    for (int i = 0; i < ctx->width * ctx->height; i++) {
        ctx->framebuffer[i] = color;
    }
}

/* Save framebuffer to file (for headless mode debugging) */
void sdl_save_framebuffer_to_file(SdlContext* ctx, const char* filename) {
    if (!ctx || !ctx->framebuffer) return;
    
    FILE* f = fopen(filename, "wb");
    if (!f) {
        LOG_SAFE("[SDL] Failed to open %s for writing\n", filename);
        return;
    }
    
    /* Write PPM header (P6 = RGB binary) */
    fprintf(f, "P6\n%d %d\n255\n", ctx->width, ctx->height);
    
    /* Write pixels (convert ARGB to RGB) */
    for (int i = 0; i < ctx->width * ctx->height; i++) {
        uint32_t pixel = ctx->framebuffer[i];
        uint8_t r = (pixel >> 16) & 0xFF;
        uint8_t g = (pixel >> 8) & 0xFF;
        uint8_t b = pixel & 0xFF;
        fwrite(&r, 1, 1, f);
        fwrite(&g, 1, 1, f);
        fwrite(&b, 1, 1, f);
    }
    
    fclose(f);
    LOG_SAFE("[SDL] Saved framebuffer to %s (%dx%d)\n", filename, ctx->width, ctx->height);
}

/* Present */
void sdl_present(SdlContext* ctx) {
    (void)ctx;  /* used only in the SDL2 backend path */
#if SDL2_AVAILABLE
    sdl_render_thread_check("present");
    if (!ctx || !ctx->framebuffer || !g_texture || !g_renderer) {
        GFX_DEBUG("[SDL] present: invalid context or texture");
        return;
    }
#ifdef __SWITCH__
    SDL_LOCK();
    sdl_switch_present_game(ctx);
    SDL_UNLOCK();
    return;
#endif
    
    static int present_count = 0;
    present_count++;
    if (present_count <= 10) {
        /* Check pixels at position where title.png should be (y=53) */
        int y = 53;
        GFX_DEBUG("[SDL] present #%d: pixel at (0,0)=0x%08X, pixel at (120,53)=0x%08X, pixel at (0,53)=0x%08X", 
                present_count, ctx->framebuffer[0], 
                ctx->framebuffer[y * ctx->width + 120],
                ctx->framebuffer[y * ctx->width + 0]);
    }
    
    SDL_LOCK();
    sdl_update_texture(ctx);
    SDL_RenderClear(g_renderer);
    SDL_RenderCopy(g_renderer, g_texture, NULL, NULL);
    SDL_RenderPresent(g_renderer);
    SDL_UNLOCK();
#endif
}

/* Update texture */
void sdl_update_texture(SdlContext* ctx) {
    (void)ctx;  /* used only in the SDL2 backend path */
#if SDL2_AVAILABLE
    sdl_render_thread_check("update_texture");
    if (ctx && ctx->framebuffer && g_texture) {
        /* Debug: show first few pixels */
        static int update_count = 0;
        update_count++;
        if (update_count <= 5) {
            GFX_DEBUG("[SDL] update_texture #%d: first pixels = 0x%08X 0x%08X 0x%08X", 
                    update_count, ctx->framebuffer[0], ctx->framebuffer[1], ctx->framebuffer[2]);
        }
        
        /* v34.61 (Asphalt 3 3D "rarely delivers 3D frames" - same fix as
         * libretro_end_frame): upload the latest SETTLED frame, not the live
         * framebuffer. The game thread paints asynchronously into
         * ctx->framebuffer; uploading it mid-paint showed torn/partial
         * frames (background only). A settled snapshot exists only after a
         * COMPLETE frame, so tearing is impossible by construction. */
        {
            extern uint32_t midp_present_stable_copy(uint32_t* dst, int dst_px);
            static uint32_t* s_scanout = NULL;
            static int s_scanout_px = 0;
            int px = ctx->width * ctx->height;
            if (px > 0 && px <= 8192 * 8192) {
                if (px > s_scanout_px) {
                    uint32_t* nb = (uint32_t*)realloc(s_scanout,
                                                      (size_t)px * sizeof(uint32_t));
                    if (nb) { s_scanout = nb; s_scanout_px = px; }
                }
                if (s_scanout &&
                    midp_present_stable_copy(s_scanout, s_scanout_px) != 0) {
                    SDL_UpdateTexture(g_texture, NULL, s_scanout,
                                      ctx->width * sizeof(uint32_t));
                    return;
                }
            }
        }
        /* Bootstrap / fallback (nothing settled yet): live framebuffer,
         * pre-v34.61 behavior */
        SDL_UpdateTexture(g_texture, NULL, ctx->framebuffer,
            ctx->width * sizeof(uint32_t));
    }
#endif
}

/* Key mapping */
int sdl_key_to_midp(int sdl_key) {
#if SDL2_AVAILABLE
    for (size_t i = 0; i < KEY_MAP_SIZE; i++) {
        if (key_map[i].sdl_key == sdl_key) return key_map[i].midp_key;
    }
#endif
    return sdl_key;
}

int sdl_key_to_game_action(int sdl_key) {
    (void)sdl_key;  /* used only in the SDL2 backend path */
#if SDL2_AVAILABLE
    for (size_t i = 0; i < KEY_MAP_SIZE; i++) {
        if (key_map[i].sdl_key == sdl_key) return key_map[i].game_action;
    }
#endif
    return 0;
}

/* Key state - uses SDL_GetKeyboardState for real-time state */
bool sdl_key_pressed(SdlContext* ctx, int keycode) {
    (void)ctx;  /* Not needed when using SDL_GetKeyboardState */
    
#if SDL2_AVAILABLE
    /* Pump events to ensure keyboard state is current */
    SDL_PumpEvents();
    
    /* Get the current keyboard state directly from SDL */
    const Uint8* keyboard_state = SDL_GetKeyboardState(NULL);
    if (!keyboard_state) {
        return false;
    }
    
    /* Map Nokia FullCanvas keyCode to SDL scancode */
    /* -1=UP, -2=DOWN, -3=LEFT, -4=RIGHT, -5=FIRE */
    SDL_Scancode scancode = 0;
    switch (keycode) {
        case -1:  /* UP (Nokia FULL_UP) */
        case GAME_UP:
            scancode = SDL_SCANCODE_UP; 
            break;
        case -2:  /* DOWN (Nokia FULL_DOWN) */
        case GAME_DOWN:
            scancode = SDL_SCANCODE_DOWN; 
            break;
        case -3:  /* LEFT (Nokia FULL_LEFT) */
        case GAME_LEFT:
            scancode = SDL_SCANCODE_LEFT; 
            break;
        case -4:  /* RIGHT (Nokia FULL_RIGHT) */
        case GAME_RIGHT:
            scancode = SDL_SCANCODE_RIGHT; 
            break;
        case -5:  /* FIRE (Nokia FULL_FIRE) */
        case GAME_FIRE:
            /* FIRE can be Return or Space */
            if (keyboard_state[SDL_SCANCODE_RETURN] || keyboard_state[SDL_SCANCODE_SPACE]) {
                GFX_DEBUG("[sdl_key_pressed] FIRE (keycode=%d) -> pressed", keycode);
                return true;
            }
            return false;
        case -6:  /* GAME_A */
        case GAME_A:
            scancode = SDL_SCANCODE_A; 
            break;
        case -7:  /* GAME_B */
        case GAME_B:
            scancode = SDL_SCANCODE_B; 
            break;
        case -8:  /* GAME_C */
        case GAME_C:
            scancode = SDL_SCANCODE_C; 
            break;
        case -9:  /* GAME_D */
        case GAME_D:
            scancode = SDL_SCANCODE_D; 
            break;
        default:
            /* For other keycodes (regular keys), check keyboard state directly */
            if (keycode >= 0 && keycode < 256) {
                /* Try to map ASCII to scancode */
                if (keycode >= '0' && keycode <= '9') {
                    scancode = SDL_SCANCODE_0 + (keycode - '0');
                } else if (keycode >= 'a' && keycode <= 'z') {
                    scancode = SDL_SCANCODE_A + (keycode - 'a');
                } else {
                    return false;
                }
            } else {
                return false;
            }
            break;
    }
    
    bool state = keyboard_state[scancode];
    if (state) {
        GFX_DEBUG("[sdl_key_pressed] keycode=%d -> pressed (scancode=%d)", keycode, scancode);
    }
    return state;
#else
    (void)keycode;
    return false;
#endif
}

/* Pointer */
void sdl_get_pointer(SdlContext* ctx, int* x, int* y) {
    if (!ctx) { if (x) *x = 0; if (y) *y = 0; return; }
    if (x) *x = ctx->input.pointer_x;
    if (y) *y = ctx->input.pointer_y;
}

bool sdl_pointer_pressed(SdlContext* ctx) {
    return ctx ? ctx->input.pointer_pressed : false;
}

/* Audio */
int sdl_audio_init(SdlContext* ctx, int frequency, int channels, int samples) {
#if defined(__SWITCH__) && SDL2_AVAILABLE
    if (!switch_settings_get()->audio_enabled) return -1; /* Звук: Выкл */
#endif
    (void)ctx; (void)frequency; (void)channels; (void)samples;
#if SDL2_AVAILABLE
    /* [AUDIO-HARDEN] v36.50: close+open под тем же мьютексом, что и
     * sdl_audio_queue_samples (микшерный поток). Прежний беззамочный
     * SDL_CloseAudioDevice здесь мог закрываться ПАРАЛЛЕЛЬНО микшерному
     * SDL_QueueAudio — TOCTOU класс «преждевременно освобождённые
     * ресурсы» (гипотеза пользователя), на Ryu видно как всплеск
     * InvalidAccess. */
    pthread_mutex_lock(&g_audio_dev_mutex);
    if (g_audio_dev) { SDL_CloseAudioDevice(g_audio_dev); g_audio_dev = 0; }

    sw_trace("audio: open %d Hz %d ch", frequency, channels); /* v34.91: emulator hang point #1 */

    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof(want));
    want.freq = frequency;
    want.format = AUDIO_S16;
    want.channels = channels;
    want.samples = samples;
    want.callback = NULL;  /* Use SDL_QueueAudio instead of callback */
    want.userdata = ctx;

    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, &have,
        SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_CHANNELS_CHANGE);

    if (dev == 0) {
        pthread_mutex_unlock(&g_audio_dev_mutex);
        DEBUG_LOG("[SDL] Failed to open audio: %s", SDL_GetError());
        return -1;
    }
    g_audio_dev = dev;
    pthread_mutex_unlock(&g_audio_dev_mutex);

    SDL_PauseAudioDevice(dev, 0);
    DEBUG_LOG("[SDL] Audio: %d Hz, %d channels, buffer=%d samples", have.freq, have.channels, have.samples);
    return 0;
#else
    return 0;
#endif
}

void sdl_audio_close(SdlContext* ctx) {
    (void)ctx;
#if SDL2_AVAILABLE
    pthread_mutex_lock(&g_audio_dev_mutex);
    if (g_audio_dev) { SDL_CloseAudioDevice(g_audio_dev); g_audio_dev = 0; }
    pthread_mutex_unlock(&g_audio_dev_mutex);
#endif
}

int sdl_audio_queue(SdlContext* ctx, const void* data, size_t length) {
    (void)ctx; (void)data; (void)length;
#if SDL2_AVAILABLE
    /* [AUDIO-HARDEN] v36.50: атомарно с close/init — как sdl_audio_queue_samples. */
    pthread_mutex_lock(&g_audio_dev_mutex);
    SDL_AudioDeviceID dev = g_audio_dev;
    pthread_mutex_unlock(&g_audio_dev_mutex);
    if (dev) return SDL_QueueAudio(dev, data, length);
#endif
    return -1;
}

uint32_t sdl_audio_queued_size(SdlContext* ctx) {
    (void)ctx;
#if SDL2_AVAILABLE
    pthread_mutex_lock(&g_audio_dev_mutex);
    SDL_AudioDeviceID dev = g_audio_dev;
    pthread_mutex_unlock(&g_audio_dev_mutex);
    if (dev) return SDL_GetQueuedAudioSize(dev); /* [AUDIO-HARDEN] v36.50 */
#endif
    return 0;
}

void sdl_audio_clear(SdlContext* ctx) {
    (void)ctx;
#if SDL2_AVAILABLE
    pthread_mutex_lock(&g_audio_dev_mutex);
    SDL_AudioDeviceID dev = g_audio_dev; /* [AUDIO-HARDEN] v36.50 */
    pthread_mutex_unlock(&g_audio_dev_mutex);
    if (dev) SDL_ClearQueuedAudio(dev);
#endif
}

/* Simple audio init without context */
int sdl_audio_init_simple(uint32_t sample_rate) {
#if SDL2_AVAILABLE
    /* [AUDIO-HARDEN] v36.50: прежняя схема «unlock -> open -> lock+assign»
     * позволяла ДВУМ потокам открыть по устройству (double-open: одно
     * утекало, второе могло закрыться под ногами микшера). Теперь проверка
     * и открытие идут под одним мьютексом; порядок блокировок
     * наш-мьютекс -> SDL-замки одинаков во всех путях (queue_samples),
     * дедлока нет. */
    pthread_mutex_lock(&g_audio_dev_mutex);
    if (g_audio_dev) { pthread_mutex_unlock(&g_audio_dev_mutex); return 0; }  /* Already initialized */
#ifdef __SWITCH__
    if (!switch_settings_get()->audio_enabled) { pthread_mutex_unlock(&g_audio_dev_mutex); return -1; } /* Звук: Выкл */
#endif

    sw_trace("audio: open %lu Hz stereo", (unsigned long)sample_rate); /* v34.91 */

    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof(want));
    want.freq = sample_rate;
    want.format = AUDIO_S16;
    want.channels = 2;  /* Stereo */
    want.samples = 2048;  /* Larger buffer for queue-based audio */
    want.callback = NULL;  /* Use SDL_QueueAudio instead of callback */
    want.userdata = NULL;

    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, &have,
        SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_CHANNELS_CHANGE);

    if (dev == 0) {
        pthread_mutex_unlock(&g_audio_dev_mutex);
        DEBUG_LOG("[SDL] Failed to open audio: %s", SDL_GetError());
        sw_trace("audio: open FAILED"); /* v34.94: name the failure in the log */
        return -1;
    }

    g_audio_dev = dev;
    pthread_mutex_unlock(&g_audio_dev_mutex);
    SDL_PauseAudioDevice(dev, 0);  /* Start audio */
    DEBUG_LOG("[SDL] Audio initialized: %d Hz, %d channels, buffer=%d samples",
              have.freq, have.channels, have.samples);
    return 0;
#else
    (void)sample_rate;
    return 0;
#endif
}

void sdl_audio_shutdown(void) {
#if SDL2_AVAILABLE
    pthread_mutex_lock(&g_audio_dev_mutex);
    if (g_audio_dev) {
        SDL_CloseAudioDevice(g_audio_dev);
        g_audio_dev = 0;
    }
    pthread_mutex_unlock(&g_audio_dev_mutex);
#endif
}

/* Timing */
uint64_t sdl_get_ticks(SdlContext* ctx) {
    (void)ctx;
#if SDL2_AVAILABLE
    return SDL_GetTicks();
#else
#ifdef _WIN32
    return GetTickCount();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
#endif
}

void sdl_sleep(uint32_t ms) {
#if SDL2_AVAILABLE
    SDL_Delay(ms);
#else
#ifdef _WIN32
    Sleep(ms);
#else
    usleep(ms * 1000);
#endif
#endif
}

/* Window */
void sdl_set_title(SdlContext* ctx, const char* title) {
    (void)ctx;
    (void)title;  /* used only in the SDL2 backend path */
#if SDL2_AVAILABLE
    if (g_window && title) SDL_SetWindowTitle(g_window, title);
#endif
}

void sdl_set_fullscreen(SdlContext* ctx, bool fullscreen) {
    (void)ctx; (void)fullscreen;
#if SDL2_AVAILABLE
    if (g_window) {
        SDL_SetWindowFullscreen(g_window,
            fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
        /* v36.43 [SANDBOX-WINDOW-FIX]: на dummy-видео песочницы
         * FULLSCREEN_DESKTOP разворачивает окно в размер ДЕСКТОПА
         * (1024x768) — а весь UI/композит фронта рассчитан на 1280x720:
         * бейдж заряда (правый верхний угол, x>=1120) клипался вьюпортом
         * и навсегда пропадал из композита, полноэкранное чтение композита
         * уходило за границы поверхности рендерера (сегфолт в
         * test_switch_battgame). На реальном железе окно Switch всегда
         * 1280x720 (фиксированный VI-слой) — самолечение там не срабатывает
         * и ни на что не влияет. */
        int ww = 0, wh = 0;
        SDL_GetWindowSize(g_window, &ww, &wh);
        if (ww != SWITCH_UI_WIDTH || wh != SWITCH_UI_HEIGHT) {
            SDL_SetWindowFullscreen(g_window, 0);
            SDL_SetWindowSize(g_window, SWITCH_UI_WIDTH, SWITCH_UI_HEIGHT);
            LOG_SAFE("[SWITCH] window self-heal %dx%d -> %dx%d (sandbox desktop "
                     "fullscreen would clip the 1280x720 composite)\n",
                     ww, wh, SWITCH_UI_WIDTH, SWITCH_UI_HEIGHT);
        }
    }
#endif
}

void sdl_toggle_fullscreen(SdlContext* ctx) {
    (void)ctx;
#if SDL2_AVAILABLE
    if (g_window) {
        Uint32 flags = SDL_GetWindowFlags(g_window);
        SDL_SetWindowFullscreen(g_window,
            (flags & SDL_WINDOW_FULLSCREEN_DESKTOP) ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
    }
#endif
}

int sdl_resize(SdlContext* ctx, int width, int height) {
    if (!ctx || width <= 0 || height <= 0) return -1;

    /* ИСПРАВЛЕНО: Сначала выделяем новую память, потом освобождаем старую */
    uint32_t* new_framebuffer = (uint32_t*)malloc(width * height * sizeof(uint32_t));
    if (!new_framebuffer) {
        ERROR_LOG("[SDL] Failed to allocate new framebuffer in sdl_resize");
        return -1;
    }
    
    /* Инициализируем чёрным непрозрачным цветом */
    for (int i = 0; i < width * height; i++) {
        new_framebuffer[i] = 0xFF000000;
    }

#if SDL2_AVAILABLE
    SDL_Texture* new_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING, width, height);
    if (!new_texture) {
        ERROR_LOG("[SDL] Failed to create new texture in sdl_resize: %s", SDL_GetError());
        free(new_framebuffer);
        return -1;
    }
#endif

    /* Only free old resources after successful allocation */
    free(ctx->framebuffer);
    ctx->framebuffer = new_framebuffer;
    ctx->width = width;
    ctx->height = height;

#if SDL2_AVAILABLE
    if (g_texture) SDL_DestroyTexture(g_texture);
    g_texture = new_texture;
    SDL_RenderSetLogicalSize(g_renderer, width, height);
#endif
    return 0;
}

int sdl_screenshot(SdlContext* ctx, const char* filename) {
    if (!ctx || !filename) return -1;
#if SDL2_AVAILABLE
    if (g_renderer) {
        SDL_Surface* surface = SDL_CreateRGBSurface(0,
            ctx->width * ctx->scale, ctx->height * ctx->scale, 32,
            0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);
        if (surface) {
            SDL_RenderReadPixels(g_renderer, NULL, SDL_PIXELFORMAT_ARGB8888,
                surface->pixels, surface->pitch);
            int result = SDL_SaveBMP(surface, filename);
            SDL_FreeSurface(surface);
            return result;
        }
    }
#endif
    return -1;
}

/* Global graphics context for screen - used by display.c */
static MidpGraphics g_sdl_graphics;

MidpGraphics* sdl_get_graphics(SdlContext* ctx) {
    if (!ctx) {
        DEBUG_LOG("[SDL] WARNING: sdl_get_graphics called with NULL context");
        return NULL;
    }
    if (!ctx->framebuffer) {
        DEBUG_LOG("[SDL] WARNING: sdl_get_graphics - framebuffer is NULL");
        return NULL;
    }
    /* Only re-init if dimensions changed */
    if (g_sdl_graphics.width != ctx->width || g_sdl_graphics.height != ctx->height ||
        g_sdl_graphics.pixels != ctx->framebuffer) {
        midp_graphics_init(&g_sdl_graphics, ctx->framebuffer, ctx->width, ctx->height);
    }
    return &g_sdl_graphics;
}

/* Check if the given graphics context is the screen graphics */
bool sdl_is_screen_graphics(MidpGraphics* gfx) {
    return gfx == &g_sdl_graphics;
}

MidpPlatformCallbacks* sdl_get_platform_callbacks(SdlContext* ctx) {
    (void)ctx;
    return NULL;
}

void sdl_dump_info(SdlContext* ctx) {
    if (!ctx) return;
    DEBUG_LOG("[SDL] Window: %dx%d (scale: %d)", ctx->width, ctx->height, ctx->scale);
#if SDL2_AVAILABLE
    DEBUG_LOG("[SDL] Renderer: %s", SDL_GetCurrentVideoDriver());
    if (g_window) {
        int w, h;
        SDL_GetWindowSize(g_window, &w, &h);
        DEBUG_LOG("[SDL] Window size: %dx%d", w, h);
    }
#else
    DEBUG_LOG("[SDL] Running in headless mode");
#endif
}

/* Queue audio samples directly
 * v34.94 FREEZE FIX: the media audio thread produced ~realtime slices with
 * blind 5 ms pacing; any production/consumption drift accumulated in the
 * SDL queue forever (the user's v34.93 diag: sb 0 -> 812+ and climbing,
 * stl/sr climbing with it, then a full frontend freeze). Two guards now:
 *  1. HARD CAP — when the queue already holds more than
 *     NOJME_AUDIO_QUEUE_CAP_BYTES, the fresh slice is DROPPED (never
 *     queued): backlog can no longer grow unboundedly whatever the drift.
 *  2. DEVICE LOCK — g_audio_dev is read/written from the media thread and
 *     closed from the frontend thread; a small mutex keeps check+queue
 *     atomic against SDL_CloseAudioDevice.
 * sr (sound-related stall ms per diag period) is accumulated around the
 * SDL_QueueAudio call so the next diag line SHOWS if this path stalls. */
void sdl_audio_queue_samples(const int16_t* samples, size_t count) {
    (void)samples; (void)count;  /* used only in the SDL2 backend path */
#if SDL2_AVAILABLE
    if (!samples || count == 0) return;
    if (WILDGUARD_SKIP(samples, "audio:queue")) return; /* [WILDGUARD] v36.50 */
    struct timespec ts0, ts1;
    int stamped = 0;
    pthread_mutex_lock(&g_audio_dev_mutex);
    if (g_audio_dev) {
        size_t queued = SDL_GetQueuedAudioSize(g_audio_dev);
        if (queued < NOJME_AUDIO_QUEUE_CAP_BYTES) {
            clock_gettime(CLOCK_MONOTONIC, &ts0); stamped = 1;
            SDL_QueueAudio(g_audio_dev, samples, count * sizeof(int16_t));
            if (stamped) {
                clock_gettime(CLOCK_MONOTONIC, &ts1);
                uint32_t ms = (uint32_t)((ts1.tv_sec - ts0.tv_sec) * 1000 +
                                         (ts1.tv_nsec - ts0.tv_nsec) / 1000000);
                if (ms) __sync_fetch_and_add(&g_sd_sr_ms, ms);
            }
        }
        /* else: queue over the cap — drop the slice (backpressure) */
    }
    pthread_mutex_unlock(&g_audio_dev_mutex);
#endif
}

/* Get queued audio size (samples) — also used by the v34.94 diag (sb). */
size_t sdl_audio_get_queued_size(void) {
#if SDL2_AVAILABLE
    pthread_mutex_lock(&g_audio_dev_mutex);
    SDL_AudioDeviceID dev = g_audio_dev; /* [AUDIO-HARDEN] v36.50 */
    pthread_mutex_unlock(&g_audio_dev_mutex);
    if (dev) {
        return SDL_GetQueuedAudioSize(dev) / sizeof(int16_t);
    }
#endif
    return 0;
}

/* v34.94 diag + media pacing: raw queued BYTES (sb= in the diag line) and
 * a device-alive probe (the media thread idles when no device is open). */
size_t sdl_audio_queued_bytes(void) {
#if SDL2_AVAILABLE
    pthread_mutex_lock(&g_audio_dev_mutex);
    SDL_AudioDeviceID dev = g_audio_dev;
    pthread_mutex_unlock(&g_audio_dev_mutex);
    if (dev) return SDL_GetQueuedAudioSize(dev);
#endif
    return 0;
}

int sdl_audio_device_alive(void) {
#if SDL2_AVAILABLE
    pthread_mutex_lock(&g_audio_dev_mutex);
    int alive = (g_audio_dev != 0);
    pthread_mutex_unlock(&g_audio_dev_mutex);
    return alive;
#else
    return 0;
#endif
}

/* ============================================================
 * Error Screen Display - Shows unhandled exception info
 * ============================================================ */

/* Include bitmap font for error display */
#include "midp/bitmap_font.h"

/* Draw a single character at position (uses bitmap font) */
static void draw_error_char(uint32_t* framebuffer, int fb_width, int fb_height,
                            int codepoint, int x, int y, uint32_t color) {
    if (!framebuffer || x < 0 || y < 0) return;
    if (x + FONT_WIDTH > fb_width || y + FONT_HEIGHT > fb_height) return;
    
    const uint8_t* char_data = get_char_data_unicode(codepoint);
    if (!char_data) return;
    
    for (int row = 0; row < FONT_HEIGHT; row++) {
        uint8_t row_data = char_data[row];
        for (int col = 0; col < FONT_WIDTH; col++) {
            if (row_data & (1 << (4 - col))) {  /* MSB is leftmost */
                int px = x + col;
                int py = y + row;
                if (px >= 0 && px < fb_width && py >= 0 && py < fb_height) {
                    framebuffer[py * fb_width + px] = color;
                }
            }
        }
    }
}

/* Draw a UTF-8 string at position */
static void draw_error_string(uint32_t* framebuffer, int fb_width, int fb_height,
                              const char* str, int x, int y, uint32_t color) {
    if (!framebuffer || !str) return;
    
    int cur_x = x;
    int cur_y = y;
    int len = 0;
    while (str[len]) len++;
    
    int i = 0;
    while (i < len) {
        int codepoint = utf8_decode(str, &i, len);
        if (codepoint < 0) break;
        
        /* Handle newline */
        if (codepoint == '\n') {
            cur_x = x;
            cur_y += FONT_HEIGHT + 2;
            continue;
        }
        
        /* Handle tab */
        if (codepoint == '\t') {
            cur_x += (FONT_WIDTH + 1) * 4;
            continue;
        }
        
        /* Skip if outside screen */
        if (cur_x + FONT_WIDTH > fb_width) {
            cur_x = x;
            cur_y += FONT_HEIGHT + 2;
        }
        
        if (cur_y + FONT_HEIGHT > fb_height) break;
        
        draw_error_char(framebuffer, fb_width, fb_height, codepoint, cur_x, cur_y, color);
        cur_x += FONT_WIDTH + 1;  /* +1 for spacing */
    }
}

/* Word-wrap text to fit width */
static int wrap_text_line(const char* text, int max_chars_per_line, char* buffer, int buffer_size) {
    if (!text || !buffer || buffer_size <= 0) return 0;
    
    int text_len = 0;
    while (text[text_len]) text_len++;
    
    int chars_written = 0;
    int line_pos = 0;
    int i = 0;
    
    while (i < text_len && chars_written < buffer_size - 1) {
        int codepoint = utf8_decode(text, &i, text_len);
        if (codepoint < 0) break;
        
        /* Count character width (simplified - all chars are 1 char width) */
        line_pos++;
        
        if (line_pos > max_chars_per_line && codepoint == ' ') {
            buffer[chars_written++] = '\n';
            line_pos = 0;
        } else {
            /* Convert codepoint to UTF-8 for buffer */
            if (codepoint < 0x80) {
                if (chars_written < buffer_size - 1) {
                    buffer[chars_written++] = (char)codepoint;
                }
            } else if (codepoint < 0x800) {
                if (chars_written < buffer_size - 2) {
                    buffer[chars_written++] = (char)(0xC0 | (codepoint >> 6));
                    buffer[chars_written++] = (char)(0x80 | (codepoint & 0x3F));
                }
            } else if (codepoint < 0x10000) {
                if (chars_written < buffer_size - 3) {
                    buffer[chars_written++] = (char)(0xE0 | (codepoint >> 12));
                    buffer[chars_written++] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
                    buffer[chars_written++] = (char)(0x80 | (codepoint & 0x3F));
                }
            }
        }
    }
    
    buffer[chars_written] = '\0';
    return chars_written;
}

/* Global error message storage */
static char g_error_title[512] = {0};
static char g_error_message[2048] = {0};
static char g_error_stack[8192] = {0};
static char g_error_extra[2048] = {0};
static bool g_has_error = false;

/* Set error information for display */
void sdl_set_error_info(const char* title, const char* message, const char* stack_trace) {
    g_has_error = true;
    
    if (title) {
        strncpy(g_error_title, title, sizeof(g_error_title) - 1);
        g_error_title[sizeof(g_error_title) - 1] = '\0';
    } else {
        strcpy(g_error_title, "Error");
    }
    
    if (message) {
        strncpy(g_error_message, message, sizeof(g_error_message) - 1);
        g_error_message[sizeof(g_error_message) - 1] = '\0';
    } else {
        g_error_message[0] = '\0';
    }
    
    if (stack_trace) {
        strncpy(g_error_stack, stack_trace, sizeof(g_error_stack) - 1);
        g_error_stack[sizeof(g_error_stack) - 1] = '\0';
    } else {
        g_error_stack[0] = '\0';
    }
    
    /* Log to stderr */
    LOG_SAFE("\n========================================\n");
    LOG_SAFE("  [J2ME UNCAUGHT EXCEPTION]\n");
    LOG_SAFE("========================================\n");
    LOG_SAFE("  Exception: %s\n", g_error_title);
    if (g_error_message[0]) LOG_SAFE("  Message:   %s\n", g_error_message);
    if (g_error_extra[0]) LOG_SAFE("  Details:   %s\n", g_error_extra);
    if (g_error_stack[0]) LOG_SAFE("  Stack:\n%s", g_error_stack);
    LOG_SAFE("========================================\n\n");
}

/* Set extra detail info */
void sdl_set_error_extra(const char* extra) {
    if (extra) {
        strncpy(g_error_extra, extra, sizeof(g_error_extra) - 1);
        g_error_extra[sizeof(g_error_extra) - 1] = '\0';
    } else {
        g_error_extra[0] = '\0';
    }
}

/* Check if there's an error to display */
bool sdl_has_error(void) {
    return g_has_error;
}

/* Clear error state */
void sdl_clear_error(void) {
    g_has_error = false;
    g_error_title[0] = '\0';
    g_error_message[0] = '\0';
    g_error_stack[0] = '\0';
    g_error_extra[0] = '\0';
}

/* Draw the error screen */
void sdl_draw_error_screen(SdlContext* ctx) {
    if (!ctx || !ctx->framebuffer) return;
    
    int width = ctx->width;
    int height = ctx->height;
    uint32_t* fb = ctx->framebuffer;
    
    /* Colors: ARGB format */
    const uint32_t bg_color = 0xFF800000;      /* Dark red background */
    const uint32_t title_color = 0xFFFFFFFF;   /* White title */
    const uint32_t msg_color = 0xFFFFFF00;     /* Yellow message */
    const uint32_t stack_color = 0xFFCCCCCC;   /* Light gray stack trace */
    const uint32_t border_color = 0xFFFF0000;  /* Red border */
    
    /* Fill background */
    for (int i = 0; i < width * height; i++) {
        fb[i] = bg_color;
    }
    
    /* Draw border */
    int border_width = 2;
    for (int y = 0; y < border_width; y++) {
        for (int x = 0; x < width; x++) {
            fb[y * width + x] = border_color;
            fb[(height - 1 - y) * width + x] = border_color;
        }
    }
    for (int x = 0; x < border_width; x++) {
        for (int y = 0; y < height; y++) {
            fb[y * width + x] = border_color;
            fb[y * width + (width - 1 - x)] = border_color;
        }
    }
    
    /* Calculate character width for wrapping */
    int char_width = FONT_WIDTH + 1;
    int max_chars = (width - 20) / char_width;
    if (max_chars < 10) max_chars = 10;
    if (max_chars > 60) max_chars = 60;
    
    /* Draw title with prefix */
    char title_line[300];
    /* %.280s caps the copy: g_error_title is 512 bytes, title_line is 300 */
    snprintf(title_line, sizeof(title_line), "[ERROR] %.280s", g_error_title);
    draw_error_string(fb, width, height, title_line, 10, 10, title_color);
    
    /* Draw separator */
    int sep_y = 10 + FONT_HEIGHT + 4;
    for (int x = 10; x < width - 10; x++) {
        fb[sep_y * width + x] = border_color;
    }
    
    /* Draw message */
    int msg_y = sep_y + 10;
    if (g_error_message[0]) {
        char wrapped_msg[3000];
        wrap_text_line(g_error_message, max_chars, wrapped_msg, sizeof(wrapped_msg));
        draw_error_string(fb, width, height, wrapped_msg, 10, msg_y, msg_color);
        msg_y += FONT_HEIGHT * 3;  /* Move down for stack trace */
    }
    
    /* Draw stack trace header */
    if (g_error_stack[0]) {
        draw_error_string(fb, width, height, "Stack trace:", 10, msg_y, title_color);
        msg_y += FONT_HEIGHT + 4;
        
        /* Draw separator */
        for (int x = 10; x < width - 10; x++) {
            fb[msg_y * width + x] = border_color;
        }
        msg_y += 4;
        
        /* Draw stack trace with wrapping */
        char wrapped_stack[6000];
        wrap_text_line(g_error_stack, max_chars, wrapped_stack, sizeof(wrapped_stack));
        draw_error_string(fb, width, height, wrapped_stack, 10, msg_y, stack_color);
    }
    
    /* Draw instructions */
    int inst_y = height - FONT_HEIGHT - 15;
    draw_error_string(fb, width, height, "Press ESC to exit", 10, inst_y, stack_color);
    
    /* v34.61: the error screen is drawn straight into the framebuffer by
     * the main thread; make it the settled frame so the scanout presenter
     * (which no longer reads the live framebuffer) actually shows it. */
    {
        extern void midp_present_settled_snapshot(void);
        midp_present_settled_snapshot();
    }
    
    /* Force redraw flag */
    ctx->needs_redraw = true;
}

/* ============================================
 * v34.72: "MIDlet finished" screen (SDL window app).
 * Same rationale as the libretro twin in sdl_backend_stubs.c: a finished
 * MIDlet must be visibly finished, not a frozen last frame.
 * ============================================ */
static int finished_text_width_px(const char* s) {
    int n = 0, i = 0, len = 0;
    if (!s) return 0;
    while (s[len]) len++;
    while (i < len) {
        int codepoint = utf8_decode(s, &i, len);
        if (codepoint < 0) break;
        n++;
    }
    return n > 0 ? n * (FONT_WIDTH + 1) - 1 : 0;
}

void sdl_draw_midlet_finished_screen(SdlContext* ctx) {
    if (!ctx || !ctx->framebuffer) return;

    int width = ctx->width;
    int height = ctx->height;
    uint32_t* fb = ctx->framebuffer;

    /* Black background */
    for (int i = 0; i < width * height; i++) {
        fb[i] = 0xFF000000;
    }

    const uint32_t title_color = 0xFFFFFFFF;  /* white */
    const uint32_t sub_color   = 0xFFA8A8A8;  /* light gray */
    const uint32_t dim_color   = 0xFF707070;  /* dim gray */
    const uint32_t frame_color = 0xFF303030;  /* thin frame */

    /* Thin frame */
    for (int x = 0; x < width; x++) {
        fb[x] = frame_color;
        fb[(height - 1) * width + x] = frame_color;
    }
    for (int y = 1; y < height - 1; y++) {
        fb[y * width] = frame_color;
        fb[y * width + width - 1] = frame_color;
    }

    int center_y = (height / 2) - 24;

    const char* title = "MIDlet finished";
    {
        int x = (width - finished_text_width_px(title)) / 2;
        if (x < 1) x = 1;
        draw_error_string(fb, width, height, title, x, center_y, title_color);
    }
    {
        const char* sub = "\xD0\x9C\xD0\xB8\xD0\xB4\xD0\xBB\xD0\xB5\xD1\x82 \xD0\xB7\xD0\xB0\xD0\xB2\xD0\xB5\xD1\x80\xD1\x88\xD1\x91\xD0\xBD";  /* "Мидлет завершён" */
        int x = (width - finished_text_width_px(sub)) / 2;
        if (x < 1) x = 1;
        draw_error_string(fb, width, height, sub, x, center_y + FONT_HEIGHT + 6, sub_color);
    }
    {
        int y = center_y + 2 * (FONT_HEIGHT + 6) + 2;
        for (int x = width / 4; x < width - width / 4; x++) {
            fb[y * width + x] = frame_color;
        }
    }
    {
        const char* hint = "The application has ended";
        int x = (width - finished_text_width_px(hint)) / 2;
        if (x < 1) x = 1;
        draw_error_string(fb, width, height, hint, x, center_y + 3 * (FONT_HEIGHT + 6), dim_color);
    }
    {
        const char* hint = "Close the window or press ESC";
        int x = (width - finished_text_width_px(hint)) / 2;
        if (x < 1) x = 1;
        draw_error_string(fb, width, height, hint, x, height - 15, dim_color);
    }

    /* v34.61 semantics: make it the settled frame for the scanout. */
    {
        extern void midp_present_settled_snapshot(void);
        midp_present_settled_snapshot();
    }

    ctx->needs_redraw = true;
}

/* =========================================================================
 * v17: SDL clipboard bridge for com.nokia.mid.ui.Clipboard
 * v34.21: SDL_SetClipboardText/SDL_GetClipboardText need SDL >= 2.26; older
 * distros and mock-SDL builds don't have them — compile-time guard keeps
 * every configuration (real SDL2 new/old, mock, none) warning-free.
 * ========================================================================= */
#include <stdlib.h>
#if defined(__has_include)
#  if __has_include(<SDL2/SDL_version.h>)
#    include <SDL2/SDL_version.h>
#  elif __has_include(<SDL_version.h>)
#    include <SDL_version.h>
#  endif
#endif
#ifndef SDL_VERSION_ATLEAST
#  define SDL_VERSION_ATLEAST(X, Y, Z) 0
#endif

static void sdl_clipboard_set(const char* text) {
#if SDL_VERSION_ATLEAST(2, 26, 0)
    if (text && text[0]) {
        SDL_SetClipboardText(text);
    }
#else
    (void)text; /* clipboard unsupported on this SDL2 */
#endif
}

static char* sdl_clipboard_get(void) {
#if SDL_VERSION_ATLEAST(2, 26, 0)
    char* sys = SDL_GetClipboardText();
    if (!sys || !sys[0]) {
        if (sys) SDL_free(sys);
        return NULL;
    }
    /* Caller frees with free(); SDL requires SDL_free, so copy out */
    char* out = strdup(sys);
    SDL_free(sys);
    return out;
#else
    return NULL; /* clipboard unsupported on this SDL2 */
#endif
}

void sdl_setup_clipboard_hooks(void) {
    extern void midp_set_clipboard_hooks(void (*set_fn)(const char*), char* (*get_fn)(void));
    midp_set_clipboard_hooks(sdl_clipboard_set, sdl_clipboard_get);
}
