/*
 * switch_common.h — shared settings model for the Nintendo Switch frontend.
 *
 * The Switch port (Makefile: platform=switch) is an SDL2 app: the same
 * core drives a 1280x720 window with an in-app menu system (game browser +
 * settings) implemented in src/switch/switch_ui.c. Settings persist to
 * <sdmc:/switch/j2me/settings.ini> on real hardware (plain file next to
 * the binary in non-Switch verification builds).
 */
#ifndef SWITCH_COMMON_H
#define SWITCH_COMMON_H

#include <stdint.h> /* v36.25: uint32_t в прототипе switch_lang_badge_draw */

#include <stddef.h>

/* ---- Display scaling modes (settings item "Масштаб") ---- */
#define NOJME_SCALE_STRETCH   0  /* растянуть на весь экран (игнор пропорций) */
#define NOJME_SCALE_FIT_BLACK 1  /* пропорции сохранены, чёрный бордюр */
#define NOJME_SCALE_FIT_BLUR  2  /* пропорции сохранены, размытый фон вместо бордюра */
#define NOJME_SCALE_MODE_COUNT 3

/* ---- Game canvas filtering (settings item "Фильтр") ----
 * v36.14: + NOJME_FILTER_SCALE2X — NEON-ускоренный Scale2x (edge-aware
 * целочисленный апскейл x2 в два больших пикселя) с последующим GPU-fit
 * в игровой прямоугольник. Резкие диагонали пиксель-арта без «лестниц»
 * обычного nearest и без мыла полного linear. v36.20: финальный fit —
 * STRICTLY NEAREST (без сглаживания; раньше fit был linear и размывал
 * результат Scale2x — исправлено по репорту). */
#define NOJME_FILTER_NEAREST 0  /* пиксельный (retro) */
#define NOJME_FILTER_LINEAR  1  /* сглаженный */
#define NOJME_FILTER_SCALE2X 2  /* Scale2x (NEON), fit БЕЗ сглаживания (v36.20) */
#define NOJME_FILTER_COUNT   3

/* ---- VM speed presets (settings item "Скорость VM") ----
 * Mirrors the libretro j2me_vm_speed option semantics: the value maps to
 * g_jvm_thread_budget (game-thread instruction budget per 16.6 ms slice):
 * original = 15000 (~0.9M instr/s, «телефонный» темп), fast = 400000
 * (~24M instr/s, v35.09), turbo = 0 (бюджет снят, потоки свободны).
 * v36.16: TURBO — значение по умолчанию (запрос пользователя: тяжёлые
 * 3D-игры не должны ползти из коробки; «Быструю» можно включить вручную). */
#define NOJME_VM_SPEED_ORIGINAL 0
#define NOJME_VM_SPEED_FAST     1
#define NOJME_VM_SPEED_TURBO    2
#define NOJME_VM_SPEED_COUNT    3
#define NOJME_VM_SPEED_DEFAULT  NOJME_VM_SPEED_TURBO

/* ---- v36.16 settings.ini schema version ----
 * Written by switch_settings_save(); used by the loader to migrate old
 * files. Version 1 (any ini WITHOUT this key): vm_speed=fast was the
 * DEFAULT (never a deliberate user choice — indistinguishable), so on
 * first load of a version-1 ini a persisted fast is promoted to the new
 * default (turbo). Version >= 2 inis are taken verbatim: after the first
 * «Сохранить» a deliberate fast survives every future load.
 * v36.36 (version 3): migration to the sound/fps DEFAULTS — a pre-v3 ini
 * gets audio=on and frame_counter=off restored once (запрос пользователя:
 * «по умолчанию звук включен, счётчик кадров выключен»; persisted values
 * from old debugging sessions survived on the SD card). After the next
 * «Сохранить» (version=3 written) deliberate choices stick again.
 * v36.37 (version 4): «Раскладка D-pad» (dpad=arrows|digits) re-interpreted
 * as the new stick/D-pad SWAP (dpad=digits == swap=on) — the parser maps
 * the legacy key once, then settings_version=4 is written and the old key
 * is dropped by the next save. */
#define NOJME_SETTINGS_VERSION 4

/* ---- v35.03 3D facing flip ("global flip") ----
 * Controls the M3G rasterizer facing decision. Field report: the v34.88
 * self-heal (per-PolygonMode facing inversion after sustained 100%
 * back-facing renders) is REQUIRED by some games and BREAKS the 3D of
 * others, so it is now a user setting:
 *   OFF  - never invert (self-heal disabled entirely);
 *   AUTO - v34.88 behavior: self-heal detector per PolygonMode;
 *   ON   - force-invert EVERY facing decision (for games the detector
 *          misses); the detector does not run.
 * NOJME_CULL_HEAL=0 (env) still forces OFF on desktop verification builds. */
#define NOJME_FLIP_OFF  0
#define NOJME_FLIP_AUTO 1
#define NOJME_FLIP_ON   2

/* ---- v36.11 M3G texture smoothing (settings item "Сглаживание текстур") ----
 * Drives g_m3g_texture_filter_override (render.c, v34.79): the per-sample
 * dispatch when sampling M3G textures. AUTO keeps the legacy per-texture
 * behavior (FILTER_LINEAR/TRILINEAR textures get bilinear, the rest nearest
 * — bit-identical to v34.78); NEAREST forces nearest on EVERY texture;
 * SMOOTH forces bilinear on every texture. The same value map the libretro
 * core option j2me_texture_smoothing and the NOJME_TEXTURE_FILTER env use. */
#define NOJME_TEXF_AUTO    0
#define NOJME_TEXF_NEAREST 1
#define NOJME_TEXF_SMOOTH  2

/* ---- v36.37 stick/D-pad layout (settings item «Поменять местами
 * стик и D-pad», per-game override; replaces the v36.36 «Раскладка
 * D-pad» enum) ----
 * Some multi-device J2ME ports (NET Lizard «a2» builds and friends) alias
 * their SOFT-KEY actions onto the NOKIA ARROW keycodes: in Unblock Me
 * [a2] the in-game key translator maps -1 (UP) -> soft1 and -4 (RIGHT) ->
 * soft2, so pressing the D-pad opens menus/exits instead of moving.
 * Those same builds handle ITU-T digits cleanly ('2'/'8' navigate their
 * menus; digits reach getGameAction without soft aliasing). v36.36 made
 * the whole D-pad send digits; v36.37 (запрос пользователя: «пусть стик
 * шлёт цифры а dpad стрелки, в настройках … «поменять местами стик и
 * dpad»») splits the two INPUT PATHS:
 *   OFF (default) - STICK sends ITU-T digits '2'/'8'/'4'/'6' (50/56/52/54),
 *                   D-PAD sends Nokia arrows -1..-4;
 *   ON            - swapped: STICK sends arrows, D-PAD sends digits.
 * The thumb rests on the stick for direction games, the classic arrows
 * stay on the physical cross-key; a2-ports get clean digits from the
 * stick out of the box. Reliable AUTOMATIC detection of aliased builds
 * is impossible without executing the game's obfuscated key translator,
 * so the swap is a global setting + per-game override
 * (-1 inherit / 0 off / 1 on, applied LIVE at input time). */
#define NOJME_SWAP_OFF 0
#define NOJME_SWAP_ON  1
#define NOJME_SWAP_COUNT 2

/* ---- v36.37 stick dead zone (settings item «Мёртвая зона стика») ----
 * Percent (0..90) of the full axis travel the stick must leave before a
 * direction ENGAGES. Joy-Con drift pushes |axis| off zero at rest, so
 * phantom directions fire; raising this radius silences them. Enter
 * threshold = dz, release threshold = dz - 0.15 (hysteresis floor 0) —
 * the v36.36 behavior (enter 0.50 / exit 0.35) is EXACTLY the 50 default,
 * so the default changes nothing until the user tunes it. */
#define NOJME_DEADZONE_MIN  0
#define NOJME_DEADZONE_MAX  90
#define NOJME_DEADZONE_STEP 5
#define NOJME_DEADZONE_DEFAULT 50

/* ---- UI language (v34.90): MINUS toggles RU/EN in every frontend screen.
 * Persisted as lang=ru|en in settings.ini; NOJME_SWITCH_LANG env overrides
 * for host verification builds. ---- */
#define NOJME_LANG_RU 0
#define NOJME_LANG_EN 1

/* ---- [BATFIX] v36.41: индикатор заряда аккумулятора (settings item
 * «Заряд аккумулятора») ----
 * Рисуется в ПРАВОМ ВЕРХНЕМ углу экрана (1280x720) поверх игры и во всех
 * экранах меню. Источник — src/utils/battery.c (libnx PSM на Switch,
 * мок из окружения на хосте). Те же проценты видны из J2ME через
 * System.getProperty("batterylevel" / "com.nokia.mid.batterylevel" /
 * "nojme.batterylevel") и класс nojme.device.Battery (getLevel/isCharging).
 * OFF по умолчанию — как у прочих оверлеев (счётчик кадров, лог). */
#define NOJME_BATT_OFF     0
#define NOJME_BATT_ICON    1
#define NOJME_BATT_NUM     2
#define NOJME_BATT_COUNT   3
#define NOJME_BATT_DEFAULT NOJME_BATT_OFF

/* ---- v34.92 logging toggle (settings item "Лог") ----
 * Trace logging to sdmc:/switch/j2me/log.txt is OFF by default; the
 * settings row enables it. The enable/disable event itself always reaches
 * the log (so "logging enabled" in the log proves WHEN it was turned on). */
#define NOJME_LOGGING_DEFAULT 0

/* ---- v35.07 frame-counter overlay (settings item "Счётчик кадров") ----
 * Draws the settled-frame sequence number + its age INTO the presented
 * frame (the game texture AND the blurred fill are fed from that one
 * buffer, so the same token appears in both). Purpose: an on-device,
 * eye-decisive instrument for the "blur updates more often than the main
 * screen" reports — the counter ticks exactly when the content changes,
 * identically in the sharp image and in the blurred backdrop. */
#define NOJME_FRAME_COUNTER_DEFAULT 0

/* ---- v34.94 per-game settings (PLUS in game) ----
 * Stored per JAR in <settings_base>/pergame/<jar name>.ini. The resolution
 * override replaces the manifest hint for the canvas size; it is applied on
 * the NEXT launch of that game (the canvas is created at session start). */
typedef struct {
    int valid;               /* file existed / values loaded */
    int res_mode;            /* 0 = auto (manifest), 1 = explicit WxH */
    int res_w, res_h;        /* explicit canvas size when res_mode == 1 */
    int scale_mode;          /* -1 = inherit global setting */
    int filter;              /* -1 = inherit global setting */
    int flip_mode;           /* v35.03: -1 = inherit, else NOJME_FLIP_* */
    int vm_speed;            /* v35.09: -1 = inherit, else NOJME_VM_SPEED_*.
                              * Escape hatch for VM-budget-bound 3D games
                              * (asia rally raced at ~1 fps on "fast": the
                              * game's bytecode-side 3D math demands more
                              * instructions/s than the old fast ceiling
                              * allowed; turbo lifts the pacer entirely). */
    int tex_filter;          /* v36.11: -1 = inherit, else NOJME_TEXF_*.
                              * Applies at launch AND live from the PLUS
                              * overlay (writes g_m3g_texture_filter_override
                              * immediately — sampling picks it up per draw). */
    int rotation;            /* v36.15: 0 = off (default), 1 = 90 right (CW),
                              * 2 = 90 left (CCW). Same semantics as the
                              * libretro core option j2me_rotation, but
                              * per-midlet and WITHOUT an inherit slot:
                              * there is no global rotation to inherit.
                              * Presentation-only: the canvas stays W x H,
                              * the PRESENTED frame is rotated (software,
                              * like the libretro v34.48 path), touch
                              * coordinates are mapped back. Applies at
                              * session start (game_begin bakes it into
                              * texture geometry) — next launch of the game. */
    int input_swap;          /* v36.37: -1 = inherit global setting, else
                              * NOJME_SWAP_* . Read at INPUT time (every
                              * stick/D-pad event), so the override applies
                              * LIVE from the PLUS/pause settings screen. */
} SwitchPerGameSettings;

#define NOJME_PERGAME_DIR "pergame"

/* ---- Switch frontend screen geometry ---- */
#define SWITCH_UI_WIDTH  1280
#define SWITCH_UI_HEIGHT 720

typedef struct {
    int scale_mode;          /* NOJME_SCALE_* */
    int filter;              /* NOJME_FILTER_* */
    int audio_enabled;       /* 1 = звук включён */
    int vm_speed;            /* NOJME_VM_SPEED_* */
    int lang;                /* NOJME_LANG_RU / NOJME_LANG_EN */
    char games_dir[512];     /* стартовая папка браузера ("" = корень SD) */
    int logging;             /* v34.92: 1 = писать log.txt (по умолч. выкл) */
    char saves_dir[512];     /* v34.92: база RMS-сохранений ("" = умолчание) */
    int flip_mode;           /* v35.03: NOJME_FLIP_* (default AUTO) */
    int frame_counter;       /* v35.07: счётчик кадров поверх кадра (по умолч. выкл) */
    int tex_filter;          /* v36.11: NOJME_TEXF_* (default AUTO) — M3G
                              * texture smoothing, global base value */
    int input_swap;          /* v36.37: NOJME_SWAP_* (default OFF) — stick
                              * sends digits + D-pad sends arrows when OFF,
                              * swapped when ON (see the block above) */
    int stick_deadzone;      /* v36.37: stick dead-zone radius in percent
                              * (NOJME_DEADZONE_*), default 50 = v36.36
                              * enter/exit thresholds bit-for-bit */
    int battery_display;     /* [BATFIX] v36.41: NOJME_BATT_* — индикатор
                              * заряда в правом верхнем углу (по умолч. выкл) */
} SwitchSettings;

/* Defaults: fit-with-black-borders, pixel filtering, audio on, TURBO VM
 * (v36.16: turbo — новый дефолт скорости VM). */
void switch_settings_defaults(SwitchSettings* s);

/* Singleton: loads settings.ini on first call. NULL-safe (returns defaults
 * if the file is missing/corrupt). */
const SwitchSettings* switch_settings_get(void);

/* Update the singleton (call switch_settings_save right after to persist). */
void switch_settings_set(const SwitchSettings* s);

/* Persist to settings.ini. Returns 0 on success. */
int switch_settings_save(const SwitchSettings* s);

/* Human-readable value names for the settings UI. */
const char* switch_scale_mode_name(int mode);
const char* switch_filter_name(int filter);
const char* switch_vm_speed_name(int speed);
const char* switch_flip_mode_name(int mode); /* v35.03 */
const char* switch_texf_name(int mode);      /* v36.11 */
const char* switch_swap_name(int mode);      /* v36.37: Выкл/Вкл */
const char* switch_batt_name(int mode);      /* [BATFIX] v36.41: не показыв./значок/цифра */

/* v34.92: trace-logging flag for the diag gate (0/1). */
int switch_settings_logging_flag(void);

/* v34.92: base directory that hosts settings.ini (for pergame/ + log.txt).
 * Returns "" when unknown. */
const char* switch_settings_base_dir(void);

/* v34.94 per-game settings: load (or defaults when absent) for one JAR.
 * jar_path may be absolute; only the file name is used (sanitized). */
void switch_pergame_load(const char* jar_path, SwitchPerGameSettings* out);

/* v34.94 per-game settings: persist. Returns 0 on success. */
int switch_pergame_save(const char* jar_path, const SwitchPerGameSettings* s);

/* ================= v34.90 UI language (RU/EN) ================= */

/* Current language (NOJME_LANG_*). Loads with the settings singleton. */
int switch_lang(void);

/* Toggle RU <-> EN, persist to settings.ini, return the NEW value. */
int switch_lang_toggle(void);

/* "Русский" / "English" — the language's own name. */
const char* switch_lang_name(int lang);

/* Localized UI string by id (enum SwitchTextId below). */
const char* switch_text(int id);

/* v36.25: короткий код языка "ru" / "en" — им же питается
 * microedition.locale (запрос пользователя: «locale зависел от настройки
 * языка», бейдж — «только кнопка минус и сокращённый код языка»). */
const char* switch_lang_code(int lang);

/* v36.25: код ТЕКУЩЕГО языка ("ru"/"en"). Определена в switch_ui.c; ядро
 * ВМ (native.c / drm_bypass.c) линкует её слабо — в libretro/headless
 * сборках, где switch_ui.c нет, возвращается "en" (прежнее поведение). */
const char* switch_locale_get(void);

/* v36.25: SDL-бейдж левого верхнего угла — нарисованная примитивами кнопка
 * MINUS (рамка + минус-полоса) и рядом короткий код текущего языка.
 * (x, y) — левый верхний угол кнопки; высота блока ~26px, ширина ~86px. */
void switch_lang_badge_draw(uint32_t* cv, int cw, int ch, int x, int y,
                            uint32_t col);

/* [BATFIX] v36.41: бейдж заряда в ПРАВОМ ВЕРХНЕМ углу переданной канвы
 * (меню 1280x720 / оверлей паузы / маленькая текстура-оверлей кадра —
 * везде один и тот же угол экрана). mode: NOJME_BATT_OFF — ничего не
 * делать, NOJME_BATT_ICON — значок, NOJME_BATT_NUM — «NN%». Значение
 * заряда читает сам (utils/battery.h). */
void switch_ui_batt_badge_draw(uint32_t* cv, int cw, int ch, int mode);

/* String ids for every user-visible frontend string (RU + EN table in
 * switch_ui.c). Drawn with the embedded 8x16 font: ASCII + Cyrillic only
 * (no em-dash/ellipsis — those glyphs are not in the font). */
enum {
    ST_APP_TITLE = 0,
    ST_MENU_OPEN,
    ST_MENU_SETTINGS,
    /* ST_MENU_EXIT removed in v36.11: the in-menu "Выход" item killed the
     * process from inside (field: crash on close) — HOME is THE way out. */
    ST_MENU_TAGLINE,
    ST_FOOTER_MAIN,
    ST_BROWSER_TITLE,
    ST_BROWSER_EMPTY,
    ST_BROWSER_TRUNC,
    ST_BROWSER_DIR,
    ST_FOOTER_BROWSER,      /* counts appended with %d/%d */
    ST_SETTINGS_TITLE,
    ST_SET_SCALE,
    ST_SET_FILTER,
    ST_SET_TEXF,
    ST_TEX_AUTO,
    ST_TEX_NEAREST,
    ST_TEX_SMOOTH,
    ST_SET_AUDIO,
    ST_SET_VM_SPEED,
    ST_SET_GAMES_DIR,
    ST_SET_LOG,
    ST_SET_SAVE,
    ST_VAL_ON,
    ST_VAL_OFF,
    ST_PREVIEW_LABEL,
    ST_FOOTER_SETTINGS,
    ST_DIRPICKER_TITLE,
    ST_DIRPICKER_THIS,
    ST_DIRPICKER_MAKE,
    ST_FOOTER_DIRPICKER,
    ST_PAUSE_TITLE,
    ST_PAUSE_RESUME,
    ST_PAUSE_EXIT,
    ST_PG_TITLE,
    ST_PG_RES,
    ST_PG_SCALE,
    ST_PG_FILTER,
    ST_PG_SAVED,
    ST_PG_FOOTER,
    ST_RES_AUTO,
    ST_SCALE_INHERIT,
    ST_FILTER_INHERIT,
    ST_SCALE_STRETCH,
    ST_SCALE_FIT_BLACK,
    ST_SCALE_FIT_BLUR,
    ST_FILTER_LINEAR,
    ST_FILTER_NEAREST,
    ST_VM_ORIGINAL,
    ST_VM_FAST,
    ST_VM_TURBO,
    ST_SET_FLIP,
    ST_FLIP_AUTO,
    ST_PG_FLIP,
    ST_SET_FCNT,
    ST_PG_VMSPEED,
    ST_PG_TEXF,
    /* v35.13 loading screen (shown until the session's first settled frame) */
    ST_LOADING,
    ST_LOADING_SEC,
    ST_LOADING_VM1,
    ST_LOADING_VM2,
    /* v36.14: главный экран меню "Управление" + значение фильтра Scale2x */
    ST_MENU_KEYS,
    ST_KEYS_TITLE,
    ST_KEYS_GAME_HDR,
    ST_KEYS_MENU_HDR,
    ST_KEYS_DPAD,
    ST_KEYS_DPAD_V,
    ST_KEYS_A,
    ST_KEYS_A_V,
    ST_KEYS_X,
    ST_KEYS_X_V,
    ST_KEYS_B,
    ST_KEYS_B_V,
    ST_KEYS_Y,
    ST_KEYS_Y_V,
    ST_KEYS_L,
    ST_KEYS_L_V,
    ST_KEYS_R,
    ST_KEYS_R_V,
    /* [YSFIX] v36.39: L3/R3 (клики стиков) = дубликаты софт-кнопок */
    ST_KEYS_L3,
    ST_KEYS_L3_V,
    ST_KEYS_R3,
    ST_KEYS_R3_V,
    ST_KEYS_TOUCH,
    ST_KEYS_TOUCH_V,
    ST_KEYS_MINUS,
    ST_KEYS_MINUS_V,
    ST_KEYS_PLUS,
    ST_KEYS_PLUS_V,
    ST_KEYS_AM,
    ST_KEYS_AM_V,
    ST_KEYS_BM,
    ST_KEYS_BM_V,
    ST_KEYS_MINUSM,
    ST_KEYS_MINUSM_V,
    ST_KEYS_HOME,
    ST_KEYS_HOME_V,
    ST_KEYS_FOOTER,
    ST_FILTER_SCALE2X,
    /* v36.15: поворот экрана в индивидуальных настройках (per-game) */
    ST_PG_ROT,
    ST_ROT_OFF,
    ST_ROT_RIGHT,
    ST_ROT_LEFT,
    /* v36.15: всплывающие описания настроек (глобальный экран) */
    ST_HINT_SCALE,
    ST_HINT_FILTER,
    ST_HINT_TEXF,
    ST_HINT_FLIP,
    ST_HINT_AUDIO,
    ST_HINT_VMSPEED,
    ST_HINT_GAMESDIR,
    ST_HINT_LOG,
    ST_HINT_FCNT,
    ST_HINT_SAVE,
    /* v36.15: всплывающие описания настроек (индивидуальные, per-game) */
    ST_HINT_PG_RES,
    ST_HINT_PG_SCALE,
    ST_HINT_PG_FILTER,
    ST_HINT_PG_FLIP,
    ST_HINT_PG_VMSPEED,
    ST_HINT_PG_TEXF,
    ST_HINT_PG_ROT,
    /* v36.15: пометки о моменте применения настройки */
    ST_HINT_ONLAUNCH,
    ST_HINT_NOW,
    ST_HINT_RESTART,
    ST_HINT_LIVE,
    /* v36.19: единое меню паузы (MINUS и PLUS) — пункт «Настройки игры»
     * и подсказка управления (добавлены В КОНЕЦ enum, старые id не
     * сдвинуты — регресс-гарды завязаны на порядок) */
    ST_PAUSE_SETTINGS,
    ST_PAUSE_FOOTER,
    /* v36.25: настройка языка в общих настройках (дублирует MINUS-тоггл;
     * добавлены В КОНЕЦ enum/таблицы — старые id не сдвинуты) */
    ST_SET_LANG,
    ST_HINT_LANG,
    /* v36.36: раскладка D-pad (стрелки или цифры 2/4/6/8) — глобальная
     * и per-game строки + справка «Назначенные кнопки» (R2 = 0).
     * Добавлены В КОНЕЦ enum/таблиц — старые id не сдвинуты.
     * v36.37: ST_SET_DPAD/ST_HINT_DPAD/ST_PG_DPAD/ST_HINT_PG_DPAD
     * ПЕРЕОСМЫСЛЕНЫ как строки «Стик и D-pad» (см. switch_ui.c таблицы);
     * ST_DPAD_ARROWS/ST_DPAD_DIGITS больше не рисуются (id сохранены). */
    ST_SET_DPAD,
    ST_DPAD_ARROWS,
    ST_DPAD_DIGITS,
    ST_HINT_DPAD,
    ST_PG_DPAD,
    ST_HINT_PG_DPAD,
    ST_KEYS_ZR,
    ST_KEYS_ZR_V,
    /* v36.37: мёртвая зона стика (глобальная) + per-game «Стик и D-pad»
     * отдельным пунктом (значения Наследовать/Обычная/Поменять).
     * Добавлены В КОНЕЦ enum/таблиц — старые id не сдвинуты. */
    ST_SET_DEADZONE,
    ST_HINT_DEADZONE,
    ST_PG_SWAP,
    ST_HINT_PG_SWAP,
    /* [YSFIX2] v36.40: справка «Назначенные кнопки» — ZL = дубль левой
     * софт-клавиши (на одиночных Joy-Con слот ZL — это плечевая кнопка
     * SL, см. pad_mapping_left/right_joy в switch-SDL2) + строка-пометка
     * про одиночный Joy-Con. Добавлены В КОНЕЦ enum/таблиц — старые id
     * не сдвинуты. */
    ST_KEYS_ZL,
    ST_KEYS_ZL_V,
    ST_KEYS_JC,
    ST_KEYS_JC_V,
    /* [BATFIX] v36.41: строка «Заряд аккумулятора» в общих настройках
     * + три её значения + всплывающее описание. Добавлены В КОНЕЦ
     * enum/таблиц — старые id не сдвинуты. */
    ST_SET_BATT,
    ST_BATT_OFF,
    ST_BATT_ICON,
    ST_BATT_NUM,
    ST_HINT_BATT,
    /* v36.48 [KEYHINT-REMOVE]: панель «Кнопки в игре» и настройка
     * «Подсказка управления» ([KEY-HINT] v36.45) УДАЛЕНЫ по запросу
     * пользователя — плашка перекрывала игру при старте. Строки
     * справки и поле настроек убраны; ini-ключ keyhint= по-прежнему
     * молча принимается (старые файлы настроек на устройствах). */
    ST_TEXT_COUNT
};

#endif /* SWITCH_COMMON_H */
