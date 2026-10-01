/*
 * switch_ui.c — Nintendo Switch frontend menus: game browser, settings,
 * main menu. Pure logic + software rendering into the UI canvas (no SDL
 * calls — see switch_glue.h). v34.90: bilingual UI (Russian/English),
 * MINUS toggles the language in every screen, current language + the
 * toggle hint shown as a badge in the top-left corner. UTF-8, rendered
 * with the embedded bitmap font (switch_font.c).
 *
 * Settings persist to <base>/settings.ini where <base> is
 * sdmc:/switch/j2me on real hardware, ./switchui under verification
 * builds (non-Switch hosts).
 */
#include "switch/switch_ui.h"
#include "switch/switch_common.h"
#include "switch/switch_glue.h"
#include "switch/switch_font.h"
#include "switch/switch_trace.h" /* v34.91: freeze triage */
#include "switch/pathguard.h" /* [PATHGUARD] v36.51: slab+n21+ptrscan прибор файловых путей */
#include "utils/battery.h" /* [BATFIX] v36.41: источник процента заряда */
#include "wildguard.h" /* [WILDGUARD] v36.50: детектор мусорных указателей фронтенда */
#include "jar_reader.h" /* v36.58 [JAR-ICONS]: manifest+entry reads for browser icons */
/* stb_image.h НЕ включаем: две нужные функции объявлены ниже weak-символами
 * (white-box тест input линкует switch_ui.c без stb_image_impl.c). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <unistd.h>   /* v35.12: access() in the legacy-root migration */
#include <sys/stat.h>

#define LOG_TAG "[SWITCH-UI] "
#define ui_log(...) do { fprintf(stderr, LOG_TAG __VA_ARGS__); fflush(stderr); } while (0)

/* ================= Settings persistence ================= */

static SwitchSettings g_settings;
static int g_settings_loaded = 0;

static const char* switch_settings_path(char* buf, size_t cap) {
    /* v34.94: the verification dir wins on EVERY platform (host verify
     * builds compile with -D__SWITCH__ but have no sdmc:). */
    const char* env = getenv("NOJME_SWITCH_UI_DIR");
    if (env && env[0]) {
        snprintf(buf, cap, "%s/switchui_settings.ini", env);
        return buf;
    }
#ifdef __SWITCH__
    (void)buf; (void)cap;
    return "sdmc:/switch/j2me/settings.ini";
#else
    snprintf(buf, cap, "%s/switchui_settings.ini", env ? env : ".");
    return buf;
#endif
}

void switch_settings_defaults(SwitchSettings* s) {
    if (!s) return;
    memset(s, 0, sizeof(*s));
    s->scale_mode = NOJME_SCALE_FIT_BLACK;
    s->filter = NOJME_FILTER_NEAREST;
    s->audio_enabled = 1;
    s->vm_speed = NOJME_VM_SPEED_DEFAULT; /* v36.16: Турбо по умолчанию */
    s->lang = NOJME_LANG_RU; /* v34.90: русский по умолчанию */
    s->logging = NOJME_LOGGING_DEFAULT; /* v34.92: лог по умолчанию ВЫКЛ */
    s->flip_mode = NOJME_FLIP_AUTO; /* v35.03: самовосстановление как в v34.88 */
    s->frame_counter = NOJME_FRAME_COUNTER_DEFAULT; /* v35.07: счётчик кадров ВЫКЛ */
    s->tex_filter = NOJME_TEXF_AUTO; /* v36.11: M3G-текстуры как в v34.78 (auto) */
    s->input_swap = NOJME_SWAP_OFF; /* v36.37: стик — цифры, D-pad — стрелки */
    s->stick_deadzone = NOJME_DEADZONE_DEFAULT; /* v36.37: 50% = пороги v36.36 */
    s->battery_display = NOJME_BATT_DEFAULT; /* [BATFIX] v36.41: индикатор заряда выкл */
#ifdef __SWITCH__
    /* v34.92: принятые папки по умолчанию — игры и сохранения живут
     * внутри sdmc:/switch/j2me (браузер всё равно позволяет подняться
     * выше через B). */
    snprintf(s->games_dir, sizeof(s->games_dir), "sdmc:/switch/j2me/games");
    snprintf(s->saves_dir, sizeof(s->saves_dir), "sdmc:/switch/j2me/saves");
#else
    snprintf(s->games_dir, sizeof(s->games_dir), ".");
    snprintf(s->saves_dir, sizeof(s->saves_dir), "saves");
#endif
}

const char* switch_scale_mode_name(int mode) {
    switch (mode) {
        case NOJME_SCALE_STRETCH:   return switch_text(ST_SCALE_STRETCH);
        case NOJME_SCALE_FIT_BLACK: return switch_text(ST_SCALE_FIT_BLACK);
        case NOJME_SCALE_FIT_BLUR:  return switch_text(ST_SCALE_FIT_BLUR);
        default: return "?";
    }
}

const char* switch_filter_name(int filter) {
    switch (filter) {
        case NOJME_FILTER_LINEAR:  return switch_text(ST_FILTER_LINEAR);
        case NOJME_FILTER_SCALE2X: return switch_text(ST_FILTER_SCALE2X);
        default:                   return switch_text(ST_FILTER_NEAREST);
    }
}

const char* switch_vm_speed_name(int speed) {
    switch (speed) {
        case NOJME_VM_SPEED_ORIGINAL: return switch_text(ST_VM_ORIGINAL);
        case NOJME_VM_SPEED_TURBO:    return switch_text(ST_VM_TURBO);
        default:                      return switch_text(ST_VM_FAST);
    }
}

/* v35.03: имя значения настройки "Глобальный флип" (OFF/AUTO/ON). */
const char* switch_flip_mode_name(int mode) {
    switch (mode) {
        case NOJME_FLIP_OFF: return switch_text(ST_VAL_OFF);
        case NOJME_FLIP_ON:  return switch_text(ST_VAL_ON);
        default:             return switch_text(ST_FLIP_AUTO);
    }
}

/* v36.11: имя значения настройки "Сглаживание текстур" (AUTO/NEAREST/SMOOTH). */
const char* switch_texf_name(int mode) {
    switch (mode) {
        case NOJME_TEXF_NEAREST: return switch_text(ST_TEX_NEAREST);
        case NOJME_TEXF_SMOOTH:  return switch_text(ST_TEX_SMOOTH);
        default:                 return switch_text(ST_TEX_AUTO);
    }
}

/* v36.37: имя значения настройки «Стик и D-pad» (Вкл/Выкл) — и глобально
 * (без Наследовать), и в per-game значениях 0/1 (для -1 caller берёт
 * ST_SCALE_INHERIT сам, как для scale/filter). */
const char* switch_swap_name(int mode) {
    return (mode == NOJME_SWAP_ON) ? switch_text(ST_VAL_ON)
                                   : switch_text(ST_VAL_OFF);
}

/* [BATFIX] v36.41: имя значения настройки «Заряд аккумулятора»
 * (не показывать / значок / цифра). */
const char* switch_batt_name(int mode) {
    switch (mode) {
        case NOJME_BATT_ICON: return switch_text(ST_BATT_ICON);
        case NOJME_BATT_NUM:  return switch_text(ST_BATT_NUM);
        default:              return switch_text(ST_BATT_OFF);
    }
}

/* v36.16: поднимается парсером settings_version (см. parse_ini_line).
 * 0 = ini старой схемы (ключа нет) — включается миграция vm_speed.
 * v36.37: + s_ini_version_max — фактическая версия файла (миграции
 * audio/fps привязаны к v3, vm_speed — к v2, а не к текущей схеме). */
static int s_ini_version_seen = 0;
static int s_ini_version_max = 0;

static void trim(char* s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' ||
                 s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = '\0';
    size_t off = 0;
    while (s[off] == ' ' || s[off] == '\t') off++;
    if (off) memmove(s, s + off, n - off + 1);
}

static void parse_ini_line(char* line, SwitchSettings* s) {
    trim(line);
    if (!line[0] || line[0] == '#' || line[0] == ';') return;
    char* eq = strchr(line, '=');
    if (!eq) return;
    *eq = '\0';
    char* key = line;
    char* val = eq + 1;
    trim(key);
    trim(val);

    if (strcmp(key, "scale") == 0) {
        if (strcmp(val, "stretch") == 0) s->scale_mode = NOJME_SCALE_STRETCH;
        else if (strcmp(val, "fit_blur") == 0) s->scale_mode = NOJME_SCALE_FIT_BLUR;
        else s->scale_mode = NOJME_SCALE_FIT_BLACK;
    } else if (strcmp(key, "filter") == 0) {
        /* v36.14: + scale2x (старые ini без него остаются совместимы) */
        if (strcmp(val, "linear") == 0) s->filter = NOJME_FILTER_LINEAR;
        else if (strcmp(val, "scale2x") == 0) s->filter = NOJME_FILTER_SCALE2X;
        else s->filter = NOJME_FILTER_NEAREST;
    } else if (strcmp(key, "audio") == 0) {
        s->audio_enabled = strcmp(val, "off") == 0 ? 0 : 1;
    } else if (strcmp(key, "vm_speed") == 0) {
        if (strcmp(val, "original") == 0) s->vm_speed = NOJME_VM_SPEED_ORIGINAL;
        else if (strcmp(val, "turbo") == 0) s->vm_speed = NOJME_VM_SPEED_TURBO;
        else if (strcmp(val, "fast") == 0) s->vm_speed = NOJME_VM_SPEED_FAST;
        else s->vm_speed = NOJME_VM_SPEED_DEFAULT; /* v36.16: мусор -> дефолт (Турбо) */
    } else if (strcmp(key, "settings_version") == 0) {
        /* v36.16: маркер схемы ini. Присутствует с v36.16; его отсутствие
         * означает старый файл. v36.37: помним МАКСИМАЛЬНУЮ версию файла —
         * миграции привязаны к версии, В КОТОРУЮ менялось поведение
         * (vm_speed — v2, звук/счётчик — v3), а не к текущей схеме. */
        int v = atoi(val);
        if (v > s_ini_version_max) s_ini_version_max = v;
        if (v >= NOJME_SETTINGS_VERSION) s_ini_version_seen = 1;
    } else if (strcmp(key, "games_dir") == 0) {
        /* v35.13: no automatic path rewriting — the value is used as-is.
         * Defaults already live under sdmc:/switch/j2me; an ini with a
         * custom path keeps it (the folder picker re-saves on change). */
        snprintf(s->games_dir, sizeof(s->games_dir), "%s", val);
    } else if (strcmp(key, "saves_dir") == 0) {
        snprintf(s->saves_dir, sizeof(s->saves_dir), "%s", val);
    } else if (strcmp(key, "logging") == 0) {
        s->logging = (strcmp(val, "on") == 0 || strcmp(val, "1") == 0) ? 1 : 0;
    } else if (strcmp(key, "lang") == 0) {
        s->lang = (strcmp(val, "en") == 0) ? NOJME_LANG_EN : NOJME_LANG_RU;
    } else if (strcmp(key, "flip") == 0) {
        /* v35.03: глобальный флип 3D (авто/вкл/выкл) */
        if (strcmp(val, "on") == 0) s->flip_mode = NOJME_FLIP_ON;
        else if (strcmp(val, "off") == 0) s->flip_mode = NOJME_FLIP_OFF;
        else s->flip_mode = NOJME_FLIP_AUTO;
    } else if (strcmp(key, "frame_counter") == 0) {
        /* v35.07: счётчик кадров поверх кадра */
        s->frame_counter = (strcmp(val, "on") == 0 || strcmp(val, "1") == 0) ? 1 : 0;
    } else if (strcmp(key, "tex_filter") == 0) {
        /* v36.11: сглаживание M3G-текстур (те же имена, что у env
         * NOJME_TEXTURE_FILTER: nearest|off, smooth|bilinear|linear|on) */
        if (strcmp(val, "nearest") == 0 || strcmp(val, "off") == 0)
            s->tex_filter = NOJME_TEXF_NEAREST;
        else if (strcmp(val, "smooth") == 0 || strcmp(val, "bilinear") == 0 ||
                 strcmp(val, "linear") == 0 || strcmp(val, "on") == 0)
            s->tex_filter = NOJME_TEXF_SMOOTH;
        else
            s->tex_filter = NOJME_TEXF_AUTO;
    } else if (strcmp(key, "swap") == 0) {
        /* v36.37: поменять местами стик и D-pad (on: стик — стрелки,
         * D-pad — цифры 2/4/6/8; off: стик — цифры, D-pad — стрелки) */
        s->input_swap = strcmp(val, "on") == 0 ? NOJME_SWAP_ON : NOJME_SWAP_OFF;
    } else if (strcmp(key, "deadzone") == 0) {
        /* v36.37: мёртвая зона стика, % полного хода (0..90) */
        int dz = atoi(val);
        if (dz < NOJME_DEADZONE_MIN) dz = NOJME_DEADZONE_MIN;
        if (dz > NOJME_DEADZONE_MAX) dz = NOJME_DEADZONE_MAX;
        s->stick_deadzone = dz;
    } else if (strcmp(key, "battery") == 0) {
        /* [BATFIX] v36.41: индикатор заряда (off|icon|number; числовые
         * формы 0/1/2 тоже принимаются — устойчивость к ручной правке). */
        if (strcmp(val, "icon") == 0 || strcmp(val, "1") == 0)
            s->battery_display = NOJME_BATT_ICON;
        else if (strcmp(val, "number") == 0 || strcmp(val, "2") == 0)
            s->battery_display = NOJME_BATT_NUM;
        else
            s->battery_display = NOJME_BATT_OFF;
    } else if (strcmp(key, "keyhint") == 0) {
        /* v36.48 [KEYHINT-REMOVE]: настройка удалена — ключ только
         * принимается (старые ini на устройствах), значение игнорируется
         * и НЕ пишется обратно при сохранении. */
    } else if (strcmp(key, "dpad") == 0) {
        /* v36.36→v36.37 МИГРАЦИЯ: старый ключ «Раскладка D-pad»
         * переосмыслен как «поменять местами стик и D-pad»
         * (dpad=digits = стик/D-pad шлют цифры == swap=on;
         * dpad=arrows == swap=off). После следующего «Сохранить»
         * файл получает settings_version=4 и ключ swap= — старый
         * ключ dpad= больше не пишется. */
        s->input_swap = (strcmp(val, "digits") == 0) ? NOJME_SWAP_ON
                                                     : NOJME_SWAP_OFF;
    }
}

const SwitchSettings* switch_settings_get(void) {
    if (!g_settings_loaded) {
        g_settings_loaded = 1;
        switch_settings_defaults(&g_settings);
        char pbuf[600];
        const char* path = switch_settings_path(pbuf, sizeof(pbuf));
        FILE* f = nojme_pg_fopen("settings-load", path, "r");
        if (f) {
            char line[1024];
            s_ini_version_seen = 0; /* v36.16: сброс перед разбором файла */
            s_ini_version_max = 0;  /* v36.37: и максимальной версии */
            while (fgets(line, sizeof(line), f)) parse_ini_line(line, &g_settings);
            fclose(f);
            /* v36.16 МИГРАЦИЯ ДЕФОЛТА СКОРОСТИ VM: в ini старой схемы (без
             * settings_version) значение fast — это СТАРЫЙ дефолт, а не выбор
             * пользователя (отличить невозможно), поэтому поднимаем его до
             * нового дефолта Турбо. v36.37: порог ВЕРСИИ ФАЙЛА 3 — v1 и v2
             * мигрируют (семантика v36.36: маркер 3 появился вместе с
             * v3-схемой), файлы v3+ — verbatim. */
            if (s_ini_version_max < 3 && g_settings.vm_speed == NOJME_VM_SPEED_FAST) {
                g_settings.vm_speed = NOJME_VM_SPEED_DEFAULT;
                ui_log("settings: vm_speed fast (v1 default) migrated to turbo\n");
            }
            /* v36.36 МИГРАЦИЯ ДЕФОЛТОВ ЗВУКА/СЧЕТЧИКА: ini старой схемы
             * (< version 3) могла таскать audio=off / frame_counter=on с
             * давних диагностических сессий; по запросу «по умолчанию звук
             * включен, счётчик кадров выключен» оба значения приводятся к
             * дефолту ОДИН РАЗ (v36.37: только для ini с версией < 3 —
             * осознанные выборы, записанные файлом v3+ не трогаются). */
            if (s_ini_version_max < 3) {
                if (!g_settings.audio_enabled || g_settings.frame_counter) {
                    ui_log("settings: v<3 ini — audio=%d frame_counter=%d reset to defaults (on/off)\n",
                           g_settings.audio_enabled, g_settings.frame_counter);
                    g_settings.audio_enabled = 1;
                    g_settings.frame_counter = 0;
                }
            }
            ui_log("settings loaded from %s (scale=%d filter=%d audio=%d vm=%d dir=%s)\n",
                   path, g_settings.scale_mode, g_settings.filter,
                   g_settings.audio_enabled, g_settings.vm_speed, g_settings.games_dir);
        } else {
            ui_log("settings: no %s, using defaults\n", path);
        }
        /* v34.90: deterministic host verification — env beats the ini */
        const char* env_lang = getenv("NOJME_SWITCH_LANG");
        if (env_lang && env_lang[0]) {
            g_settings.lang = (strcmp(env_lang, "en") == 0) ? NOJME_LANG_EN
                                                               : NOJME_LANG_RU;
        }
    }
    return &g_settings;
}

void switch_settings_set(const SwitchSettings* s) {
    if (s) {
        g_settings = *s;
        g_settings_loaded = 1;
    }
}

/* v34.91 test seam (scripts/test_switch_input.c #15): drop the singleton so
 * the next switch_settings_get() re-reads settings.ini — lets one process
 * test several ini files (incl. the poisoned-games_dir case). */
void switch_settings_force_reload_for_test(void) {
    g_settings_loaded = 0;
}

int switch_settings_save(const SwitchSettings* s) {
    if (!s) return -1;
    switch_settings_set(s);
    sw_trace("settings: save"); /* v34.91: proves the ini write COMPLETED */

    char pbuf[600];
    const char* path = switch_settings_path(pbuf, sizeof(pbuf));
    /* parent dir may not exist yet on a fresh SD — best effort mkdir */
#ifdef __SWITCH__
    nojme_pg_mkdir("mkdir-sw", "sdmc:/switch", 0777);
    nojme_pg_mkdir("mkdir-j2me", "sdmc:/switch/j2me", 0777);
#endif
    FILE* f = nojme_pg_fopen("settings-save", path, "w");
    if (!f) {
        ui_log("settings: cannot write %s\n", path);
        return -1;
    }
    static const char* scale_names[] = { "stretch", "fit_black", "fit_blur" };
    static const char* speed_names[] = { "original", "fast", "turbo" };
    fprintf(f, "# nojme Switch frontend settings\n");
    /* v36.16: версия схемы ini — отключает миграцию vm_speed (см.
     * switch_settings_get); важно писать ДО остальных ключей. */
    fprintf(f, "settings_version=%d\n", NOJME_SETTINGS_VERSION);
    fprintf(f, "scale=%s\n", scale_names[s->scale_mode % NOJME_SCALE_MODE_COUNT]);
    fprintf(f, "filter=%s\n",
            s->filter == NOJME_FILTER_LINEAR  ? "linear" :
            s->filter == NOJME_FILTER_SCALE2X ? "scale2x" : "nearest");
    fprintf(f, "audio=%s\n", s->audio_enabled ? "on" : "off");
    fprintf(f, "vm_speed=%s\n", speed_names[s->vm_speed % NOJME_VM_SPEED_COUNT]);
    fprintf(f, "lang=%s\n", s->lang == NOJME_LANG_EN ? "en" : "ru"); /* v34.90 */
    fprintf(f, "games_dir=%s\n", s->games_dir);
    fprintf(f, "saves_dir=%s\n", s->saves_dir); /* v34.92 */
    fprintf(f, "logging=%s\n", s->logging ? "on" : "off"); /* v34.92 */
    /* v35.03: глобальный флип 3D */
    fprintf(f, "flip=%s\n",
            s->flip_mode == NOJME_FLIP_OFF ? "off" :
            s->flip_mode == NOJME_FLIP_ON  ? "on"  : "auto");
    /* v35.07: счётчик кадров */
    fprintf(f, "frame_counter=%s\n", s->frame_counter ? "on" : "off");
    /* v36.11: сглаживание M3G-текстур */
    fprintf(f, "tex_filter=%s\n",
            s->tex_filter == NOJME_TEXF_NEAREST ? "nearest" :
            s->tex_filter == NOJME_TEXF_SMOOTH  ? "smooth"  : "auto");
    /* v36.37: стик/D-pad swap + мёртвая зона (старый ключ dpad= исчезает) */
    fprintf(f, "swap=%s\n", s->input_swap == NOJME_SWAP_ON ? "on" : "off");
    fprintf(f, "deadzone=%d\n", s->stick_deadzone);
    /* [BATFIX] v36.41: индикатор заряда */
    fprintf(f, "battery=%s\n",
            s->battery_display == NOJME_BATT_ICON ? "icon" :
            s->battery_display == NOJME_BATT_NUM  ? "number" : "off");
    fclose(f);
    ui_log("settings saved to %s\n", path);
    return 0;
}

/* ================= v34.92 logging flag + base dir ================= */

int switch_settings_logging_flag(void) {
    return switch_settings_get()->logging ? 1 : 0;
}

const char* switch_settings_base_dir(void) {
    /* v34.94: verification dir overrides even __SWITCH__ builds. */
    const char* env = getenv("NOJME_SWITCH_UI_DIR");
    if (env && env[0]) return env;
#ifdef __SWITCH__
    return "sdmc:/switch/j2me";
#else
    static char buf[600];
    snprintf(buf, sizeof(buf), "%s", env ? env : ".");
    return buf;
#endif
}

/* ================= v34.94 per-game settings =================
 * <base>/pergame/<jar name>.ini — keys: res=auto|WxH, scale=inherit|0..2,
 * filter=inherit|0|1. Only the file NAME of the jar is used (same name in
 * different directories shares settings — deterministic and simple). */

static void pergame_path(char* out, size_t cap, const char* jar_path) {
    const char* base = switch_settings_base_dir();
    const char* slash = strrchr(jar_path, '/');
    const char* name = slash ? slash + 1 : jar_path;
    snprintf(out, cap, "%s/" NOJME_PERGAME_DIR "/%.180s.ini", base, name);
}

void switch_pergame_load(const char* jar_path, SwitchPerGameSettings* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->res_mode = 0;
    out->res_w = 240; out->res_h = 320;
    out->scale_mode = -1;
    out->filter = -1;
    out->flip_mode = -1; /* v35.03: наследовать глобальную настройку */
    out->vm_speed = -1;  /* v35.09: наследовать глобальную настройку */
    out->tex_filter = -1; /* v36.11: наследовать глобальную настройку */
    out->rotation = 0;   /* v36.15: поворота нет и наследовать нечего */
    out->input_swap = -1; /* v36.37: наследовать глобальный стик/D-pad swap */
    if (!jar_path || !jar_path[0]) return;

    char pbuf[1024];
    pergame_path(pbuf, sizeof(pbuf), jar_path);
    FILE* f = nojme_pg_fopen("pergame-load", pbuf, "r");
    if (!f) return; /* defaults: inherit everything */
    out->valid = 1;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        trim(line);
        if (!line[0] || line[0] == '#' || line[0] == ';') continue;
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char* key = line;
        char* val = eq + 1;
        trim(key); trim(val);
        if (strcmp(key, "res") == 0) {
            if (sscanf(val, "%dx%d", &out->res_w, &out->res_h) == 2 &&
                out->res_w >= 96 && out->res_w <= 1280 &&
                out->res_h >= 96 && out->res_h <= 1280) {
                out->res_mode = 1;
            }
        } else if (strcmp(key, "scale") == 0) {
            if (strcmp(val, "stretch") == 0) out->scale_mode = NOJME_SCALE_STRETCH;
            else if (strcmp(val, "fit_black") == 0) out->scale_mode = NOJME_SCALE_FIT_BLACK;
            else if (strcmp(val, "fit_blur") == 0) out->scale_mode = NOJME_SCALE_FIT_BLUR;
            else out->scale_mode = -1;
        } else if (strcmp(key, "filter") == 0) {
            if (strcmp(val, "nearest") == 0) out->filter = NOJME_FILTER_NEAREST;
            else if (strcmp(val, "linear") == 0) out->filter = NOJME_FILTER_LINEAR;
            else if (strcmp(val, "scale2x") == 0) out->filter = NOJME_FILTER_SCALE2X;
            else out->filter = -1;
        } else if (strcmp(key, "flip") == 0) {
            /* v35.03: per-game режим глобального флипа */
            if (strcmp(val, "on") == 0) out->flip_mode = NOJME_FLIP_ON;
            else if (strcmp(val, "off") == 0) out->flip_mode = NOJME_FLIP_OFF;
            else if (strcmp(val, "auto") == 0) out->flip_mode = NOJME_FLIP_AUTO;
            else out->flip_mode = -1;
        } else if (strcmp(key, "vm_speed") == 0) {
            /* v35.09: per-game скорость ВМ (бюджета интерпретатора) */
            if (strcmp(val, "original") == 0) out->vm_speed = NOJME_VM_SPEED_ORIGINAL;
            else if (strcmp(val, "fast") == 0) out->vm_speed = NOJME_VM_SPEED_FAST;
            else if (strcmp(val, "turbo") == 0) out->vm_speed = NOJME_VM_SPEED_TURBO;
            else out->vm_speed = -1;
        } else if (strcmp(key, "tex_filter") == 0) {
            /* v36.11: per-game сглаживание M3G-текстур */
            if (strcmp(val, "auto") == 0) out->tex_filter = NOJME_TEXF_AUTO;
            else if (strcmp(val, "nearest") == 0 || strcmp(val, "off") == 0)
                out->tex_filter = NOJME_TEXF_NEAREST;
            else if (strcmp(val, "smooth") == 0 || strcmp(val, "on") == 0)
                out->tex_filter = NOJME_TEXF_SMOOTH;
            else out->tex_filter = -1;
        } else if (strcmp(key, "rotation") == 0) {
            /* v36.15: per-game поворот экрана (off/right/left; числовые
             * формы 0/1/2 тоже принимаются — имена как у libretro
             * j2me_rotation: off|90 right|90 left) */
            if (strcmp(val, "right") == 0 || strcmp(val, "1") == 0 ||
                strcmp(val, "cw") == 0)
                out->rotation = 1;
            else if (strcmp(val, "left") == 0 || strcmp(val, "2") == 0 ||
                     strcmp(val, "ccw") == 0)
                out->rotation = 2;
            else out->rotation = 0;
        } else if (strcmp(key, "swap") == 0) {
            /* v36.37: per-game стик/D-pad swap (inherit/off/on) */
            if (strcmp(val, "on") == 0) out->input_swap = NOJME_SWAP_ON;
            else if (strcmp(val, "off") == 0) out->input_swap = NOJME_SWAP_OFF;
            else out->input_swap = -1;
        } else if (strcmp(key, "dpad") == 0) {
            /* v36.36→v36.37 МИГРАЦИЯ: pergame dpad=arrows -> swap=off,
             * dpad=digits -> swap=on, inherit -> -1 (см. глобальный парсер) */
            if (strcmp(val, "digits") == 0) out->input_swap = NOJME_SWAP_ON;
            else if (strcmp(val, "arrows") == 0) out->input_swap = NOJME_SWAP_OFF;
            else out->input_swap = -1;
        }
    }
    fclose(f);
    ui_log("pergame settings loaded: %s (res=%s%dx%d scale=%d filter=%d flip=%d vm=%d tex=%d rot=%d)\n",
           pbuf, out->res_mode ? "" : "auto ", out->res_w, out->res_h,
           out->scale_mode, out->filter, out->flip_mode, out->vm_speed,
           out->tex_filter, out->rotation);
}

int switch_pergame_save(const char* jar_path, const SwitchPerGameSettings* s) {
    if (!jar_path || !jar_path[0] || !s) return -1;
    char pbuf[1024];
    pergame_path(pbuf, sizeof(pbuf), jar_path);
    {
        /* ensure the pergame dir exists */
        char dir[1024];
        snprintf(dir, sizeof(dir), "%s/" NOJME_PERGAME_DIR, switch_settings_base_dir());
#ifdef __SWITCH__
        nojme_pg_mkdir("mkdir-sw", "sdmc:/switch", 0777);
        nojme_pg_mkdir("mkdir-j2me", "sdmc:/switch/j2me", 0777);
#endif
        nojme_pg_mkdir("mkdir-pergame", dir, 0777);
    }
    FILE* f = nojme_pg_fopen("pergame-save", pbuf, "w");
    if (!f) {
        ui_log("pergame: cannot write %s\n", pbuf);
        return -1;
    }
    fprintf(f, "# nojme per-game settings\n");
    if (s->res_mode) fprintf(f, "res=%dx%d\n", s->res_w, s->res_h);
    else fprintf(f, "res=auto\n");
    if (s->scale_mode == NOJME_SCALE_STRETCH) fprintf(f, "scale=stretch\n");
    else if (s->scale_mode == NOJME_SCALE_FIT_BLACK) fprintf(f, "scale=fit_black\n");
    else if (s->scale_mode == NOJME_SCALE_FIT_BLUR) fprintf(f, "scale=fit_blur\n");
    else fprintf(f, "scale=inherit\n");
    if (s->filter == NOJME_FILTER_NEAREST) fprintf(f, "filter=nearest\n");
    else if (s->filter == NOJME_FILTER_LINEAR) fprintf(f, "filter=linear\n");
    else if (s->filter == NOJME_FILTER_SCALE2X) fprintf(f, "filter=scale2x\n");
    else fprintf(f, "filter=inherit\n");
    /* v35.03: per-game глобальный флип */
    if (s->flip_mode == NOJME_FLIP_OFF) fprintf(f, "flip=off\n");
    else if (s->flip_mode == NOJME_FLIP_AUTO) fprintf(f, "flip=auto\n");
    else if (s->flip_mode == NOJME_FLIP_ON) fprintf(f, "flip=on\n");
    else fprintf(f, "flip=inherit\n");
    /* v35.09: per-game скорость ВМ */
    if (s->vm_speed == NOJME_VM_SPEED_ORIGINAL) fprintf(f, "vm_speed=original\n");
    else if (s->vm_speed == NOJME_VM_SPEED_FAST) fprintf(f, "vm_speed=fast\n");
    else if (s->vm_speed == NOJME_VM_SPEED_TURBO) fprintf(f, "vm_speed=turbo\n");
    else fprintf(f, "vm_speed=inherit\n");
    /* v36.11: per-game сглаживание M3G-текстур */
    if (s->tex_filter == NOJME_TEXF_AUTO) fprintf(f, "tex_filter=auto\n");
    else if (s->tex_filter == NOJME_TEXF_NEAREST) fprintf(f, "tex_filter=nearest\n");
    else if (s->tex_filter == NOJME_TEXF_SMOOTH) fprintf(f, "tex_filter=smooth\n");
    else fprintf(f, "tex_filter=inherit\n");
    /* v36.15: per-game поворот экрана */
    if (s->rotation == 1) fprintf(f, "rotation=right\n");
    else if (s->rotation == 2) fprintf(f, "rotation=left\n");
    else fprintf(f, "rotation=off\n");
    /* v36.37: per-game стик/D-pad swap (старый ключ dpad= исчезает) */
    if (s->input_swap == NOJME_SWAP_OFF) fprintf(f, "swap=off\n");
    else if (s->input_swap == NOJME_SWAP_ON) fprintf(f, "swap=on\n");
    else fprintf(f, "swap=inherit\n");
    fclose(f);
    ui_log("pergame settings saved to %s\n", pbuf);
    return 0;
}

/* ================= v34.90 UI language (RU/EN) ================= */

/* Every user-visible string, Russian first (the historical UI language),
 * English second. Keep to ASCII + Cyrillic: the embedded 8x16 font has
 * exactly those coverage classes (no em-dash 0x2014 / ellipsis 0x2026 —
 * those silently rendered as blank cells in the old UI). */
static const char* g_ui_texts[ST_TEXT_COUNT][2] = {
    /* ST_APP_TITLE        */ { u8"J2ME Эмулятор",                 "J2ME Emulator" },
    /* ST_MENU_OPEN        */ { u8"Открыть игру",                  "Open game" },
    /* ST_MENU_SETTINGS    */ { u8"Настройки",                     "Settings" },
    /* ST_MENU_TAGLINE     */ { u8"MIDP2 мобильные игры на Nintendo Switch", "MIDP2 mobile games on Nintendo Switch" },
    /*                       ^ v36.17: надпись убрана с главного экрана по
    *                         запросу пользователя (строка пока в таблице,
    *                         чтобы не сдвигать индексы остальных текстов). */
    /* ST_FOOTER_MAIN      */ { u8"A - выбрать   HOME - выход",       "A - select   HOME - exit" },
    /* ST_BROWSER_TITLE    */ { u8"Открыть игру",                  "Open game" },
    /* ST_BROWSER_EMPTY    */ { u8"(пусто или нет доступа)",       "(empty or no access)" },
    /* ST_BROWSER_TRUNC    */ { u8"(показаны первые 2000 записей)", "(first 2000 entries shown)" },
    /* ST_BROWSER_DIR      */ { u8"каталог",                       "folder" },
    /* ST_FOOTER_BROWSER   */ { u8"A - открыть   B - выше   ",     "A - open   B - up   " },
    /* ST_SETTINGS_TITLE   */ { u8"Настройки",                     "Settings" },
    /* ST_SET_SCALE        */ { u8"Масштаб изображения",           "Screen scaling" },
    /* ST_SET_FILTER       */ { u8"Фильтр изображения",            "Filtering" },
    /* ST_SET_TEXF         */ { u8"Сглаживание текстур",           "Texture smoothing" },
    /* ST_TEX_AUTO         */ { u8"Авто",                          "Auto" },
    /* ST_TEX_NEAREST      */ { u8"Пиксели",                       "Pixelated" },
    /* ST_TEX_SMOOTH       */ { u8"Сглаживание",                   "Smooth" },
    /* ST_SET_AUDIO        */ { u8"Звук",                          "Sound" },
    /* ST_SET_VM_SPEED     */ { u8"Скорость VM",                   "VM speed" },
    /* ST_SET_GAMES_DIR    */ { u8"Папка игр",                     "Games folder" },
    /* ST_SET_LOG          */ { u8"Лог (диагностика)",          "Log (diagnostics)" },
    /* ST_SET_SAVE         */ { u8"Сохранить и выйти",             "Save and exit" },
    /* ST_VAL_ON           */ { u8"Вкл",                           "On" },
    /* ST_VAL_OFF          */ { u8"Выкл",                          "Off" },
    /* ST_PREVIEW_LABEL    */ { u8"Предпросмотр:",                 "Preview:" },
    /* ST_FOOTER_SETTINGS  */ { u8"◄►/A - изменить   B - назад",     "◄►/A - change   B - back" },
    /* ST_DIRPICKER_TITLE  */ { u8"Папка игр",                     "Games folder" },
    /* ST_DIRPICKER_THIS   */ { u8"[Эта папка]",                   "[This folder]" },
    /* ST_DIRPICKER_MAKE   */ { u8"сделать стартовой",             "set as start" },
    /* ST_FOOTER_DIRPICKER */ { u8"A - применить/войти   B - выше", "A - apply/enter   B - up" },
    /* ST_PAUSE_TITLE      */ { u8"ПАУЗА",                         "PAUSED" },
    /* v36.19: пункты единого меню паузы (курсор + A), строки без «A -»/«B -» */
    /* ST_PAUSE_RESUME     */ { u8"Продолжить",                    "Resume" },
    /* ST_PAUSE_EXIT       */ { u8"Выйти",                         "Exit" },
    /* ST_PG_TITLE         */ { u8"НАСТРОЙКИ ИГРЫ",              "GAME SETTINGS" },
    /* ST_PG_RES           */ { u8"Разрешение (со след. запуска)", "Resolution (next launch)" },
    /* ST_PG_SCALE         */ { u8"Масштаб",                         "Scaling" },
    /*                       ^ v36.17: «(для этой игры)» убрано по запросу —
    *                         экран и так называется «НАСТРОЙКИ ИГРЫ», а
    *                         момент применения теперь показывает всплывающее
    *                         описание (v36.15). */
    /* ST_PG_FILTER        */ { u8"Фильтр",                         "Filter" },
    /* ST_PG_SAVED         */ { u8"Сохранено",                     "Saved" },
    /* ST_PG_FOOTER        */ { u8"◄►/A - изменить   B - назад к игре", "◄►/A - change   B - back to game" },
    /* ST_RES_AUTO         */ { u8"Как в игре (манифест)",       "Game default (manifest)" },
    /* ST_SCALE_INHERIT    */ { u8"Как в общих настройках",       "From global settings" },
    /* ST_FILTER_INHERIT   */ { u8"Как в общих настройках",       "From global settings" },
    /* ST_SCALE_STRETCH    */ { u8"Растянуть на весь экран",       "Stretch to full screen" },
    /* ST_SCALE_FIT_BLACK  */ { u8"Пропорции (чёрный бордюр)",     "Aspect fit (black bars)" },
    /* ST_SCALE_FIT_BLUR   */ { u8"Пропорции (размытый фон)",      "Aspect fit (blurred fill)" },
    /* ST_FILTER_LINEAR    */ { u8"Сглаженный",                    "Smooth" },
    /* ST_FILTER_NEAREST   */ { u8"Пиксельный",                    "Pixelated" },
    /* ST_VM_ORIGINAL      */ { u8"Обычная (как телефон)",         "Original (phone-like)" },
    /* ST_VM_FAST          */ { u8"Быстрая",                       "Fast" },
    /* ST_VM_TURBO         */ { u8"Турбо (без лимита)",            "Turbo (no limit)" },
    /* ST_SET_FLIP         */ { u8"Глобальный флип 3D",            "Global 3D flip" },
    /* ST_FLIP_AUTO        */ { u8"Авто",                          "Auto" },
    /* ST_PG_FLIP          */ { u8"Глобальный флип",              "Global flip" }, /* v36.17: без «(для игры)» */
    /* ST_SET_FCNT         */ { u8"Счётчик кадров",                "Frame counter" },
    /* ST_PG_VMSPEED       */ { u8"Скорость ВМ",                  "VM speed" }, /* v36.17: без «(для игры)» */
    /* ST_PG_TEXF          */ { u8"Текстуры 3D",                  "3D textures" }, /* v36.17 */
    /* v35.13 loading screen (drawn until the session's first settled frame) */
    /* ST_LOADING          */ { u8"Загрузка",                     "Loading" },
    /* ST_LOADING_SEC      */ { u8"с",                            "s" },
    /* ST_LOADING_VM1      */ { u8"Не хватает скорости ВМ:",      "VM budget is not enough:" },
    /* ST_LOADING_VM2      */ { u8"Скорость ВМ -> Быстрая (PLUS)", "VM speed -> Fast (PLUS)" },
    /* v36.14: пункт меню "Управление" + экран назначенных кнопок */
    /* ST_MENU_KEYS        */ { u8"Управление",                   "Controls" },
    /* ST_KEYS_TITLE       */ { u8"Назначенные кнопки",           "Assigned buttons" },
    /* ST_KEYS_GAME_HDR    */ { u8"В игре:",                      "In game:" },
    /* ST_KEYS_MENU_HDR    */ { u8"В меню:",                      "In menus:" },
    /* ST_KEYS_DPAD        */ { u8"Крестовина / стик",            "D-Pad / stick" },
    /* ST_KEYS_DPAD_V      */ { u8"Стрелки; стик = 2/8/4/6",        "Arrows; stick = 2/8/4/6" },
    /* [KEY-A-PHONE] v36.45: A (и R3) шлёт цифру '5' — ITU-T OK/FIRE.
     * Это работает ВЕЗДЕ: getGameAction('5')=FIRE, LCDUI-списки ('5'=FIRE),
     * getKeyStates ('5' поднимает FIRE-бит) и цифро-панельные порты,
     * где -5/Enter мертвы (таблица клавиш игры знает только '0'..'9',
     * '*','#' — полевой кейс Yeti Sports: A/-5 не выбирала НИЧЕГО). */
    /* ST_KEYS_A           */ { u8"A / R3",                       "A / R3" },
    /* ST_KEYS_A_V         */ { u8"Цифра 5 (OK/огонь)",           "Digit 5 (OK/fire)" },
    /* [YSFIX2] v36.40: имя строки X объединяет все три источника левой
     * софт-клавиши (X / ZL / L3); B — оба источника правой (B / R3).
     * Отдельные строки L3/R3 из справки убраны (мержены сюда) — место
     * освободилось под пометку про одиночный Joy-Con (ST_KEYS_JC). */
    /* ST_KEYS_X           */ { u8"X / ZL / L3",                  "X / ZL / L3" },
    /* ST_KEYS_X_V         */ { u8"Софт-кнопка влево (-6)",       "Left soft key (-6)" },
    /* [KEY-DIGIT5] v36.44: R3 ушёл с правой софт-клавиши (осталась только B)
     * на цифру '5' — см. строку A / R3 выше. */
    /* ST_KEYS_B           */ { u8"B",                            "B" },
    /* ST_KEYS_B_V         */ { u8"Софт-кнопка вправо (-7)",      "Right soft key (-7)" },
    /* ST_KEYS_Y           */ { u8"Y",                            "Y" },
    /* ST_KEYS_Y_V         */ { u8"Очистка (-8, GAME_C)",         "Clear (-8, GAME_C)" },
    /* ST_KEYS_L           */ { u8"L",                            "L" },
    /* ST_KEYS_L_V         */ { u8"Звёздочка (*)",                "Star (*)" },
    /* ST_KEYS_R           */ { u8"R",                            "R" },
    /* ST_KEYS_R_V         */ { u8"Решётка (#)",                  "Pound (#)" },
    /* [YSFIX] v36.39: L3/R3 = софт-кнопки (дубликаты X/B) */
    /* ST_KEYS_L3          */ { u8"L3 (клик стика)",              "L3 (stick click)" },
    /* ST_KEYS_L3_V        */ { u8"Софт-кнопка влево (-6)",       "Left soft key (-6)" },
    /* ST_KEYS_R3          */ { u8"R3 (клик стика)",              "R3 (stick click)" },
    /* ST_KEYS_R3_V        */ { u8"Софт-кнопка вправо (-7)",      "Right soft key (-7)" },
    /* ST_KEYS_TOUCH       */ { u8"Сенсорный экран",              "Touch screen" },
    /* ST_KEYS_TOUCH_V     */ { u8"Pointer (x,y); края = софт",   "Pointer (x,y); edges = soft" },
    /* ST_KEYS_MINUS       */ { u8"MINUS",                        "MINUS" },
    /* v36.19: MINUS и PLUS открывают ОДНО меню паузы */
    /* ST_KEYS_MINUS_V     */ { u8"Меню паузы",                   "Pause menu" },
    /* ST_KEYS_PLUS        */ { u8"PLUS",                         "PLUS" },
    /* ST_KEYS_PLUS_V      */ { u8"Меню паузы",                   "Pause menu" },
    /* ST_KEYS_AM          */ { u8"A / PLUS",                     "A / PLUS" },
    /* ST_KEYS_AM_V        */ { u8"Выбрать",                      "Select" },
    /* ST_KEYS_BM          */ { u8"B",                            "B" },
    /* ST_KEYS_BM_V        */ { u8"Назад",                        "Back" },
    /* ST_KEYS_MINUSM      */ { u8"MINUS",                        "MINUS" },
    /* ST_KEYS_MINUSM_V    */ { u8"Сменить язык",                 "Switch language" },
    /* ST_KEYS_HOME        */ { u8"HOME",                         "HOME" },
    /* ST_KEYS_HOME_V      */ { u8"Выход из приложения",          "Exit the application" },
    /* ST_KEYS_FOOTER      */ { u8"B - назад",                    "B - back" },
    /* ST_FILTER_SCALE2X   */ { u8"Scale2x",                      "Scale2x" },
    /* v36.15: поворот экрана в индивидуальных настройках мидлета */
    /* ST_PG_ROT           */ { u8"Поворот экрана",                "Screen rotation" }, /* v36.17 */
    /* ST_ROT_OFF          */ { u8"Выкл",                          "Off" },
    /* ST_ROT_RIGHT        */ { u8"90 вправо",                     "90 right" },
    /* ST_ROT_LEFT         */ { u8"90 влево",                      "90 left" },
    /* v36.15: всплывающие описания настроек (глобальные) */
    /* ST_HINT_SCALE       */ { u8"Как изображение игры вписывается в экран консоли: растянуть, пропорции с чёрными полями или с размытым фоном по бокам.", "How the game picture fits the console screen: stretch, aspect fit with black bars or blurred fill." },
    /* ST_HINT_FILTER      */ { u8"Фильтр картинки игры: Пиксельный (чёткие пиксели), Сглаженный (мягкий) или Scale2x (чёткое увеличение x2 для пиксель-арта).", "Game picture filter: Pixelated (sharp pixels), Smooth (soft) or Scale2x (crisp x2 upscale for pixel art)." },
    /* ST_HINT_TEXF        */ { u8"Сглаживание 3D-текстур (M3G): Авто - как на старых телефонах, Пиксели - резкие, Сглаживание - мягкие.", "3D texture smoothing (M3G): Auto - like old phones, Pixelated - sharp, Smooth - soft." },
    /* ST_HINT_FLIP        */ { u8"Починка перевёрнутых 3D-моделей: Авто - детектор сам решает, Вкл - всегда переворачивать, Выкл - никогда.", "Fix for upside-down 3D models: Auto - detector decides, On - always invert, Off - never." },
    /* ST_HINT_AUDIO       */ { u8"Звук игры. Выключение немного экономит производительность консоли.", "Game audio. Turning it off saves a bit of console performance." },
    /* ST_HINT_VMSPEED     */ { u8"Скорость виртуальной машины Java: Обычная (как телефон), Быстрая или Турбо (по умолчанию, без лимита - для тяжёлых 3D-игр).", "Java VM speed: Original (phone-like), Fast or Turbo (default, no limit - for heavy 3D games)." },
    /*                       ^ v36.17: длинное тире U+2014 заменено на дефис —
    *                         в шрифте 8x16 нет глифа U+2014, оно рисовалось
    *                         пустой ячейкой (аудит v36.17). */
    /* ST_HINT_GAMESDIR    */ { u8"Стартовая папка браузера игр. Меняется через выбор папки (A на этом пункте).", "Start folder of the game browser. Changed via the folder picker (A on this row)." },
    /* ST_HINT_LOG         */ { u8"Запись диагностического лога в log.txt рядом с настройками. Включайте по просьбе разработчика.", "Writes a diagnostic log to log.txt next to the settings. Enable on developer request." },
    /* ST_HINT_FCNT        */ { u8"FPS в пустой области слева от игры, на самой игре — только если места не хватает. Раз в секунду; FPS зависит от самой игры.", "Shows FPS in the empty area left of the game (over the game only if there is not enough room). Once per second; FPS depends on the game." },
    /* ST_HINT_SAVE        */ { u8"Записать настройки в settings.ini и вернуться в главное меню.", "Save settings to settings.ini and return to the main menu." },
    /* v36.15: всплывающие описания настроек (индивидуальные) */
    /* ST_HINT_PG_RES      */ { u8"Размер экрана телефона для этой игры: Авто (манифест) или явный пресет, например 176x220.", "Phone screen size for this game: Auto (manifest) or an explicit preset, e.g. 176x220." },
    /* ST_HINT_PG_SCALE    */ { u8"Масштаб изображения только для этой игры, вместо общих настроек.", "Picture scaling for this game only, overriding the global settings." },
    /* ST_HINT_PG_FILTER   */ { u8"Фильтр картинки только для этой игры, вместо общих настроек.", "Picture filter for this game only, overriding the global settings." },
    /* ST_HINT_PG_FLIP     */ { u8"Режим починки перевёрнутых 3D-моделей только для этой игры. Применяется сразу, без перезапуска игры.", "Upside-down 3D model fix mode for this game only. Applied immediately, no game restart needed." },
    /* ST_HINT_PG_VMSPEED  */ { u8"Скорость ВМ только для этой игры. Турбо помогает тяжёлым 3D-играм.", "VM speed for this game only. Turbo helps heavy 3D games." },
    /* ST_HINT_PG_TEXF     */ { u8"Сглаживание 3D-текстур только для этой игры.", "3D texture smoothing for this game only." },
    /* ST_HINT_PG_ROT      */ { u8"Поворот картинки на 90 градусов: играйте, повернув консоль набок. Полезно для вертикальных игр на горизонтальном экране.", "Rotate the picture by 90 degrees: play with the console turned sideways. Useful for portrait games on the landscape screen." },
    /* v36.15: пометки о моменте применения */
    /* ST_HINT_ONLAUNCH    */ { u8"Применяется при запуске игры.", "Applied when the game starts." },
    /* ST_HINT_NOW         */ { u8"Применяется сразу.", "Applied immediately." },
    /* ST_HINT_RESTART     */ { u8"Требуется перезапуск игры: выйдите в меню и запустите игру заново.", "Game restart required: exit to the menu and start the game again." },
    /* ST_HINT_LIVE        */ { u8"Применяется сразу после закрытия этого меню.", "Applied as soon as this menu is closed." },
    /* v36.19: единое меню паузы (добавлены В КОНЕЦ таблицы — по порядку enum) */
    /* ST_PAUSE_SETTINGS   */ { u8"Настройки игры",                "Game settings" },
    /* ST_PAUSE_FOOTER     */ { u8"A - выбрать   B - продолжить   MINUS - язык", "A - select   B - resume   MINUS - language" },
    /* v36.25: настройка языка в общих настройках (В КОНЕЦ — по порядку enum) */
    /* ST_SET_LANG         */ { u8"Язык интерфейса",               "Interface language" },
    /* ST_HINT_LANG        */ { u8"Язык всех меню эмулятора. Дублирует переключение кнопкой MINUS; он же задаёт microedition.locale для мидлетов (ru/en).", "Language of every emulator menu. Duplicates the MINUS toggle; it also feeds microedition.locale to midlets (ru/en)." },
    /* v36.36 → v36.37: строки «Стик и D-pad» (id сохранены, тексты переосмыслены) */
    /* ST_SET_DPAD         */ { u8"Поменять местами стик и D-pad", "Swap stick and D-pad" },
    /* ST_DPAD_ARROWS      */ { u8"Стрелки",                      "Arrows" }, /* v36.37: не рисуется */
    /* ST_DPAD_DIGITS      */ { u8"Цифры 2-4-6-8",                "Digits 2-4-6-8" }, /* v36.37: не рисуется */
    /* ST_HINT_DPAD        */ { u8"По умолчанию стик шлёт игре цифры 2/4/6/8, а D-pad — стрелки. Включите, чтобы поменять их местами: стик — стрелки, D-pad — цифры 2/4/6/8.", "By default the stick sends digits 2/4/6/8 and the D-pad sends arrows. Turn on to swap them: stick sends arrows, D-pad sends digits 2/4/6/8." },
    /* ST_PG_DPAD          */ { u8"Стик и D-pad",                 "Stick and D-pad" },
    /* ST_HINT_PG_DPAD     */ { u8"Как в общих настройках, обычная раскладка (стик — цифры, D-pad — стрелки) или поменяна местами. Применяется сразу.", "Inherit the global setting, normal layout (stick digits, D-pad arrows) or swapped. Applied live." },
    /* ST_KEYS_ZR          */ { u8"R2 (ZR)",                      "R2 (ZR)" },
    /* ST_KEYS_ZR_V        */ { u8"Цифра 0",                      "Digit 0" },
    /* v36.37: мёртвая зона стика + per-game «Стик и D-pad» (В КОНЕЦ — по порядку enum) */
    /* ST_SET_DEADZONE     */ { u8"Мёртвая зона стика",           "Stick dead zone" },
    /* ST_HINT_DEADZONE    */ { u8"Порог отклонения стика в %, ниже которого направление не срабатывает. Увеличьте, если стик дрейфует и сам нажимает стрелки/цифры. По умолчанию 50%.", "Stick deflection threshold in percent below which a direction does not engage. Raise it if the stick drifts and presses arrows/digits on its own. Default 50%." },
    /* ST_PG_SWAP          */ { u8"Стик и D-pad",                 "Stick and D-pad" },
    /* ST_HINT_PG_SWAP     */ { u8"Как в общих настройках, обычная раскладка (стик — цифры, D-pad — стрелки) или поменяна местами. Применяется сразу.", "Inherit the global setting, normal layout (stick digits, D-pad arrows) or swapped. Applied live." },
    /* [YSFIX2] v36.40 (в КОНЕЦ — по порядку enum): ZL = дубль левой
     * софт-клавиши + пометка про одиночный Joy-Con */
    /* ST_KEYS_ZL          */ { u8"ZL",                            "ZL" },
    /* ST_KEYS_ZL_V        */ { u8"Софт-кнопка влево (-6)",       "Left soft key (-6)" },
    /* ST_KEYS_JC          */ { u8"Один Joy-Con",                  "Single Joy-Con" },
    /* ST_KEYS_JC_V        */ { u8"SL = софт-влево; края = софт", "SL = L-soft; edges = soft" },
    /* [BATFIX] v36.41 (в КОНЕЦ — по порядку enum): индикатор заряда */
    /* ST_SET_BATT         */ { u8"Заряд аккумулятора",            "Battery level" },
    /* ST_BATT_OFF         */ { u8"Не показывать",                 "Do not show" },
    /* ST_BATT_ICON        */ { u8"Значок",                        "Icon" },
    /* ST_BATT_NUM         */ { u8"Цифра",                         "Number" },
    /* ST_HINT_BATT        */ { u8"Показывает заряд в правом верхнем углу экрана (значок или цифра). Тот же заряд виден играм: System.getProperty(batterylevel) и класс nojme.device.Battery.", "Shows the battery level in the top-right corner of the screen (icon or number). The same level is visible to games: System.getProperty(batterylevel) and the nojme.device.Battery class." },
    /* v36.48 [KEYHINT-REMOVE]: панель «Кнопки в игре» и настройка
     * «Подсказка управления» ([KEY-HINT] v36.45) удалены по запросу
     * пользователя — плашка выскакивала в начале каждой игры. */
};

int switch_lang(void) { return switch_settings_get()->lang == NOJME_LANG_EN ? NOJME_LANG_EN : NOJME_LANG_RU; }

const char* switch_lang_name(int lang) {
    return lang == NOJME_LANG_EN ? "English" : u8"Русский";
}

const char* switch_text(int id) {
    if (id < 0 || id >= ST_TEXT_COUNT) return "?";
    return g_ui_texts[id][switch_lang() == NOJME_LANG_EN ? 1 : 0];
}

int switch_lang_toggle(void) {
    SwitchSettings st = *switch_settings_get();
    st.lang = (st.lang == NOJME_LANG_EN) ? NOJME_LANG_RU : NOJME_LANG_EN;
    switch_settings_set(&st);
    switch_settings_save(&st); /* persists lang= (best effort) */
    ui_log("ui language -> %s\n", switch_lang_name(st.lang));
    sw_trace("lang: %s", switch_lang_name(st.lang)); /* v34.91 */
    return st.lang;
}

/* v36.25: короткий код языка ("ru"/"en") — бейдж + microedition.locale. */
const char* switch_lang_code(int lang) {
    return lang == NOJME_LANG_EN ? "en" : "ru";
}

const char* switch_locale_get(void) {
    return switch_lang_code(switch_lang());
}

/* ================= UI drawing helpers ================= */

#define COL_BG      0xFF10131A  /* тёмный сине-серый фон */
#define COL_BG_ALT  0xFF181C26
#define COL_ACCENT  0xFF2E7CF6  /* синий акцент */
/* COL_ACCENT2 0xFF1B4FA8 удалён в v36.61 [NO-DIRICON]: единственный
 * потребитель — контур значка-папки браузера — убран по полевой жалобе
 * («синие квадраты поверх папок», рисуется поверх имени, т.к. текст
 * папок не сдвигался в колонку иконок). */
#define COL_TEXT    0xFFE8ECF4
#define COL_DIM     0xFF8A93A6
#define COL_SEL_BG  0xFF2E7CF6
#define COL_SEL_FG  0xFFFFFFFF
#define COL_HEADER  0xFF161B2E
#define COL_LINE    0xFF2A3040

static void fill_rect(uint32_t* cv, int cw, int ch, int x, int y, int w, int h, uint32_t col) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > cw) w = cw - x;
    if (y + h > ch) h = ch - y;
    if (w <= 0 || h <= 0) return;
    for (int yy = 0; yy < h; yy++) {
        uint32_t* row = cv + (size_t)(y + yy) * cw + x;
        for (int xx = 0; xx < w; xx++) row[xx] = col;
    }
}

static void hline(uint32_t* cv, int cw, int ch, int x0, int x1, int y, uint32_t col) {
    if (y < 0 || y >= ch) return;
    if (x0 < 0) x0 = 0;
    if (x1 > cw) x1 = cw;
    for (int x = x0; x < x1; x++) cv[(size_t)y * cw + x] = col;
}

/* v36.17: ПОЛУПРОЗРАЧНАЯ заливка прямоугольника (alpha 0..255 поверх того,
 * что уже нарисовано в канве). Фон «Настроек игры» — живой кадр, поверх —
 * затемнение этим хелпером; у fill_rect альфы нет (полная замена пикселя). */
static void blend_rect(uint32_t* cv, int cw, int ch, int x, int y, int w, int h,
                       uint32_t col, int alpha) {
    if (alpha <= 0) return;
    if (alpha > 255) alpha = 255;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > cw) w = cw - x;
    if (y + h > ch) h = ch - y;
    if (w <= 0 || h <= 0) return;
    const uint32_t cr = (col >> 16) & 0xFF, cg = (col >> 8) & 0xFF, cb = col & 0xFF;
    for (int yy = 0; yy < h; yy++) {
        uint32_t* row = cv + (size_t)(y + yy) * cw + x;
        for (int xx = 0; xx < w; xx++) {
            uint32_t p = row[xx];
            uint32_t pr = (p >> 16) & 0xFF, pg = (p >> 8) & 0xFF, pb = p & 0xFF;
            uint32_t nr = (pr * (255u - (uint32_t)alpha) + cr * (uint32_t)alpha) / 255u;
            uint32_t ng = (pg * (255u - (uint32_t)alpha) + cg * (uint32_t)alpha) / 255u;
            uint32_t nb = (pb * (255u - (uint32_t)alpha) + cb * (uint32_t)alpha) / 255u;
            row[xx] = 0xFF000000u | (nr << 16) | (ng << 8) | nb;
        }
    }
}

static void draw_text(uint32_t* cv, int cw, int ch, int x, int y, const char* s, uint32_t col, int scale) {
    switch_font_draw_text(cv, cw, ch, x, y, s, col, 0, scale);
}

/* v36.17: текст с 1-пиксельной чёрной тенью (как у счётчика кадров в игре).
 * Используется строками списков поверх живого кадра игры — белый текст на
 * светлом кадре без тени пропадает. */
static int g_row_text_shadow = 0; /* включается только экраном настроек игры */
/* v36.25: SDL-бейдж левого верхнего угла — нарисованная примитивами кнопка
 * MINUS (рамка + минус-полоса, шрифтовой глиф "−" в 8x16 отсутствует) и
 * рядом короткий код текущего языка ("ru"/"en"). Длинная текстовая строка
 * v34.90 удалена по запросу пользователя. */
void switch_lang_badge_draw(uint32_t* cv, int cw, int ch, int x, int y,
                            uint32_t col) {
    enum { BW = 40, BH = 26, BAR_W = 16, BAR_H = 4, GAP = 10 };
    /* рамка кнопки */
    fill_rect(cv, cw, ch, x, y, BW, BH, col);
    /* тёмная заливка внутри рамки (2px) */
    fill_rect(cv, cw, ch, x + 2, y + 2, BW - 4, BH - 4, COL_BG_ALT);
    /* минус-полоса по центру */
    fill_rect(cv, cw, ch, x + (BW - BAR_W) / 2, y + (BH - BAR_H) / 2,
              BAR_W, BAR_H, col);
    /* короткий код языка рядом (scale 2, глиф 16x32 -> центр по вертикали) */
    draw_text(cv, cw, ch, x + BW + GAP, y - 3, switch_locale_get(), col, 2);
}

static void draw_text_sh(uint32_t* cv, int cw, int ch, int x, int y, const char* s,
                         uint32_t col, int scale) {
    if (g_row_text_shadow)
        draw_text(cv, cw, ch, x + scale, y + scale, s, 0xFF000000u, scale);
    draw_text(cv, cw, ch, x, y, s, col, scale);
}

/* Truncate an UTF-8 string to max_cells glyphs (appending an ellipsis
 * when it does not fit). Works on codepoints, not bytes.
 * v34.91 FIX (freeze triage): the v34.90 walk computed len=4 for INVALID
 * lead bytes (stray 0x80-0xBF continuation, 0xF8-0xFF) and p+=len could
 * JUMP PAST the NUL terminator — an out-of-bounds read over heap garbage
 * (crash / "frozen screen" on emulator filesystems that hand out
 * non-UTF-8 names, e.g. CP1251). The walk is now hard-bounded by the
 * string length and invalid bytes consume exactly one cell. */
static void utf8_trunc(const char* src, char* dst, size_t dst_cap, int max_cells) {
    int cells = 0;
    const char* p = src;
    const char* last_start = src;
    size_t src_len = strlen(src); /* hard bound: the walk never passes the NUL */
    while (*p) {
        unsigned char c = (unsigned char)*p;
        int len;
        if (c < 0x80) len = 1;
        else if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        else len = 1; /* invalid lead byte — one cell, never a wild jump */
        if ((size_t)(p - src) + (size_t)len > src_len) len = 1; /* cut seq */
        last_start = p;
        if (cells == max_cells) {
            /* doesn't fit — cut here and add ellipsis */
            size_t used = (size_t)(last_start - src);
            if (used + 4 > dst_cap) used = dst_cap > 4 ? dst_cap - 4 : 0;
            memcpy(dst, src, used);
            dst[used] = 0;
            strcat(dst, "..."); /* v34.90: U+2026 нет в шрифте 8x16 */
            return;
        }
        p += len;
        cells++;
    }
    snprintf(dst, dst_cap, "%s", src);
}

/* ================= File browser model ================= */

typedef struct {
    char name[256];
    int is_dir;
    int is_jar;
} BrowseEntry;

typedef struct {
    BrowseEntry* items;
    int count;
    int cap;
    int truncated; /* >0 when the directory had more entries than kept */
} BrowseList;

static void browse_free(BrowseList* l) {
    free(l->items);
    memset(l, 0, sizeof(*l));
}

static int entry_cmp(const void* a, const void* b) {
    const BrowseEntry* ea = (const BrowseEntry*)a;
    const BrowseEntry* eb = (const BrowseEntry*)b;
    if (ea->is_dir != eb->is_dir) return ea->is_dir ? -1 : 1; /* dirs first */
    return strcmp(ea->name, eb->name);
}

#define BROWSE_MAX_ENTRIES 2000
/* v34.91 freeze-triage caps: a pathological directory (huge file count,
 * emulator filesystem with slow stat service) must NEVER be able to hang
 * the frontend. The old loop stat()-ed EVERY DT_UNKNOWN entry BEFORE any
 * cap check and skipped non-dir/non-jar names without counting them, so
 * a directory with tens of thousands of files meant tens of thousands of
 * unbounded stat() calls. */
#define BROWSE_MAX_SCANNED 20000 /* total readdir iterations */
#define BROWSE_MAX_STAT    2000  /* stat() fallback calls (DT_UNKNOWN) */
#define BROWSE_MAX_MS      2000  /* wall-clock scan budget */

static void browse_load(BrowseList* l, const char* dir) {
    browse_free(l);
    uint64_t t0 = sdl_switch_ui_ticks_ms(); /* v34.91 */
    int scanned = 0, statted = 0;           /* v34.91 */
    DIR* d = (DIR*)nojme_pg_opendir("browse-scan", dir);
    if (!d) {
        sw_trace("scan: %s FAIL (opendir)", dir); /* v34.91: visible WHY the list is empty */
        return;
    }
    l->cap = 64;
    l->items = (BrowseEntry*)malloc((size_t)l->cap * sizeof(BrowseEntry));
    if (!l->items) { closedir(d); return; }

    struct dirent* de;
    while ((de = readdir(d)) != NULL) {
        if (++scanned > BROWSE_MAX_SCANNED) { l->truncated = 1; break; }
        /* v34.91: live scan progress + wall-clock cap — time checked every
         * 16 entries (cheap), the visible «scanning» screen refreshed every
         * 2048 so the log stays readable on huge directories */
        if ((scanned & 15) == 0) {
            if (sdl_switch_ui_ticks_ms() - t0 > BROWSE_MAX_MS) { l->truncated = 1; break; }
            if ((scanned & 2047) == 0) {
                sw_trace("scan: %s (%d)", dir, l->count);
                sw_trace_flush();
            }
        }
        const char* n = de->d_name;
        /* «.» и «..» не показываем: подъём на уровень — кнопка B
         * (path_parent), дублирование в списке только путает навигацию. */
        if (strcmp(n, ".") == 0 || strcmp(n, "..") == 0) continue;
        size_t nl = strlen(n);
        int is_dir = (de->d_type == DT_DIR);
        if (!is_dir && de->d_type == DT_UNKNOWN) {
            if (statted >= BROWSE_MAX_STAT) {
                is_dir = 0; /* v34.91: over the stat cap — assume file (jars still listed) */
            } else {
                char full[1024];
                snprintf(full, sizeof(full), "%s/%s", dir, n);
                struct stat st;
                statted++;
                is_dir = (nojme_pg_stat("browse-stat", full, &st) == 0 && S_ISDIR(st.st_mode)) ? 1 : 0;
            }
        }
        int is_jar = 0;
        if (!is_dir && nl > 4) {
            is_jar = (strcasecmp(n + nl - 4, ".jar") == 0);
        }
        if (!is_dir && !is_jar) continue; /* показываем только каталоги и .jar */

        if (l->count >= BROWSE_MAX_ENTRIES) { l->truncated = 1; break; }
        if (l->count == l->cap) {
            int nc = l->cap * 2;
            BrowseEntry* ni = (BrowseEntry*)realloc(l->items, (size_t)nc * sizeof(BrowseEntry));
            if (!ni) break;
            l->items = ni;
            l->cap = nc;
        }
        BrowseEntry* e = &l->items[l->count++];
        snprintf(e->name, sizeof(e->name), "%s", n);
        e->is_dir = is_dir;
        e->is_jar = is_jar;
    }
    closedir(d);
    if (l->count > 1) qsort(l->items, (size_t)l->count, sizeof(BrowseEntry), entry_cmp);
    /* v34.91: one line with the outcome — count, cap reason, duration */
    sw_trace("scan: %s -> %d entries%s (%d ms)", dir, l->count,
             l->truncated ? " TRUNCATED" : "",
             (int)(sdl_switch_ui_ticks_ms() - t0));
}

/* ============ v36.58 [JAR-ICONS]: иконки мидлетов в списке игр ============
 * Просьба пользователя: «в список игр добавь показ иконок, которые
 * находятся внутри jar». Каждая запись кэша = путь+mtime+размер (инвалидация
 * при изменении файла) и декодированные пиксели ARGB. Извлечение ЛЕНИВОЕ —
 * не более ОДНОЙ иконки на кадр отрисовки браузера (icons_fill_one):
 * сканирование каталога не тормозит совсем, иконки «появляются» в течение
 * первой секунды; записи без иконки кэшируются как resolved (без повторных
 * попыток каждый кадр).
 *
 * Источник иконки (в порядке приоритета, как в MIDP):
 *   1. MIDlet-Icon: /path.png
 *   2. MIDlet-1: Name,/path.png,Class   (второе поле из трёх)
 *   3. MIDlet-<n>: ...                   (любой номер)
 * Формат — любой, который читает stb_image (PNG/JPEG/BMP/GIF…). */

#define JARICON_CACHE_MAX 160
#define JARICON_MAX_FILE (6 * 1024 * 1024)  /* не читать «jar» больше 6 МБ */

/* v36.58: stb-функции — СЛАБЫЕ символы: white-box тест input линкует
 * switch_ui.c БЕЗ stb_image_impl.c (как jvm_thread_liveness и прочие
 * weak-зависимости там); без иконки просто не извлекаются. */
__attribute__((weak)) unsigned char* stbi_load_from_memory(
    unsigned char const* buffer, int len, int* x, int* y, int* channels_in_file, int desired_channels);
__attribute__((weak)) void stbi_image_free(void* retval_from_stbi_load);
/* v36.58: jar_reader тоже НЕ входит в white-box-линковку теста input. */
__attribute__((weak)) uint8_t* jar_read_file(const void* jar_data, size_t jar_size,
                                             const char* name, size_t* out_size);
__attribute__((weak)) char* jar_manifest_unfold(const uint8_t* data, size_t size);

typedef struct {
    char path[1024];
    long mtime;
    long fsize;
    uint32_t* argb;   /* NULL = иконки нет (resolved-negative) */
    int w, h;
} JarIcon;

static JarIcon g_jaricons[JARICON_CACHE_MAX];
static int g_jaricon_n = 0;
/* v36.60 [ICON-BUDGET]: суммарные байты пикселей всех закэшированных
 * иконок. Кэш больше НЕ сбрасывается на границе игровой сессии
 * (см. [ICON-CACHE-KEEP] в main.c) — вместо этого бюджет с
 * вытеснением самых старых записей: память ограничена навсегда,
 * а браузер после выхода из игры не перечитывает все jar заново. */
#define JARICON_BUDGET_BYTES (8u * 1024 * 1024)
static size_t g_jaricon_bytes = 0;

static void jaricon_free_one(JarIcon* ic) {
    if (ic->argb && ic->w > 0 && ic->h > 0)
        g_jaricon_bytes -= (size_t)ic->w * ic->h * 4;
    free(ic->argb);
    ic->argb = NULL;
    ic->path[0] = '\0';
    ic->mtime = -1;
    ic->fsize = -1;
    ic->w = ic->h = 0;
}

/* v36.60 [ICON-BUDGET]: вытеснить САМУЮ СТАРУЮ запись (индекс 0 — массив
 * в порядке добавления) со сдвигом хвоста. Возвращает 1 если было что
 * вытеснять. Вызов повторяется, пока бюджет не соблюдён. */
static int jaricon_evict_oldest(void) {
    if (g_jaricon_n <= 0) return 0;
    jaricon_free_one(&g_jaricons[0]);
    g_jaricon_n--;
    if (g_jaricon_n > 0)
        memmove(&g_jaricons[0], &g_jaricons[1],
                (size_t)g_jaricon_n * sizeof(JarIcon));
    return 1;
}

/* Полный сброс кэша иконок (реализация ниже по файлу). v36.60: вызов
 * на границе игровой сессии УДАЛЁН (см. [ICON-CACHE-KEEP] в main.c) —
 * память держит бюджет JARICON_BUDGET_BYTES с вытеснением; функция
 * остаётся в API (потенциальный вызов при смене папки игр и т.п.). */
void switch_ui_jaricons_reset(void);

/* Поиск в кэше по пути+признакам; NULL = не в кэше. */
static JarIcon* jaricon_find(const char* path, long mtime, long fsize) {
    for (int i = 0; i < g_jaricon_n; i++) {
        JarIcon* ic = &g_jaricons[i];
        if (ic->path[0] && strcmp(ic->path, path) == 0) {
            if (ic->mtime == mtime && ic->fsize == fsize) return ic;
            /* файл изменился — сбросить запись (новая попытка извлечения) */
            jaricon_free_one(ic);
            return NULL;
        }
    }
    return NULL;
}

/* Разобрать развёрнутый манифест и найти путь иконки. */
static int jaricon_manifest_pick(const char* manifest, char* out, size_t cap) {
    if (!manifest) return 0;
    const char* p = manifest;
    while (*p) {
        const char* eol = strchr(p, '\n');
        size_t llen = eol ? (size_t)(eol - p) : strlen(p);
        /* MIDlet-Icon: <path> */
        if (llen > 12 && strncasecmp(p, "MIDlet-Icon:", 12) == 0) {
            const char* v = p + 12;
            while (v < p + llen && (*v == ' ' || *v == '\t')) v++;
            size_t vl = (size_t)(p + llen - v);
            while (vl > 0 && (v[vl-1] == ' ' || v[vl-1] == '\t' || v[vl-1] == '\r')) vl--;
            if (vl > 0 && vl < cap) {
                memcpy(out, v, vl); out[vl] = '\0';
                return 1;
            }
        }
        /* MIDlet-<n>: Name,icon,class — берём второе поле */
        if (llen > 8 && strncasecmp(p, "MIDlet-", 7) == 0 && p[7] >= '0' && p[7] <= '9') {
            const char* colon = memchr(p, ':', llen);
            if (colon) {
                const char* v = colon + 1;
                const char* end = p + llen;
                while (v < end && *v == ' ') v++; /* skip spaces */
                const char* c1 = memchr(v, ',', (size_t)(end - v));
                if (c1) {                      /* первое поле (имя) пропущено */
                    const char* v2 = c1 + 1;
                    while (v2 < end && *v2 == ' ') v2++;
                    const char* c2 = memchr(v2, ',', (size_t)(end - v2));
                    size_t vl = c2 ? (size_t)(c2 - v2) : (size_t)(end - v2);
                    while (vl > 0 && (v2[vl-1] == ' ' || v2[vl-1] == '\r')) vl--;
                    if (vl > 0 && vl < cap) {
                        memcpy(out, v2, vl); out[vl] = '\0';
                        return 1;
                    }
                }
            }
        }
        if (!eol) break;
        p = eol + 1;
    }
    return 0;
}

/* Извлечь иконку jar-файла; возвращает запись кэша (может с argb==NULL). */
static JarIcon* jaricon_extract(const char* path, long mtime, long fsize) {
    /* место в кэше: первое свободное или самая старая (кольцевой сброс) */
    JarIcon* slot = NULL;
    if (g_jaricon_n < JARICON_CACHE_MAX) {
        slot = &g_jaricons[g_jaricon_n++];
    } else {
        slot = &g_jaricons[0]; /* простая политика: вытеснить нулевой слот */
        jaricon_free_one(slot);
    }
    slot->argb = NULL;
    slot->w = slot->h = 0;

    FILE* f = nojme_pg_fopen("browse-icon", path, "rb");
    if (!f) goto done;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > JARICON_MAX_FILE) { fclose(f); goto done; }
    uint8_t* jar = (uint8_t*)malloc((size_t)sz);
    if (!jar) { fclose(f); goto done; }
    if (fread(jar, 1, (size_t)sz, f) != (size_t)sz) { free(jar); fclose(f); goto done; }
    fclose(f);

    {
        size_t msize = 0;
        uint8_t* mraw = (jar_read_file && jar_manifest_unfold)
            ? jar_read_file(jar, (size_t)sz, "META-INF/MANIFEST.MF", &msize) : NULL;
        char* manifest = mraw ? jar_manifest_unfold(mraw, msize) : NULL;
        free(mraw);
        char iconpath[512];
        if (manifest && jaricon_manifest_pick(manifest, iconpath, sizeof(iconpath))) {
            /* ведущий '/' не входит в имена записей jar */
            const char* entry = iconpath[0] == '/' ? iconpath + 1 : iconpath;
            size_t isize = 0;
            uint8_t* idata = jar_read_file(jar, (size_t)sz, entry, &isize);
            if (idata && stbi_load_from_memory && stbi_image_free) {
                int w = 0, h = 0, ch = 0;
                /* stb: RGBA-выход; конвертация RGBA -> ARGB канвы ниже */
                uint8_t* px = stbi_load_from_memory(idata, (int)isize, &w, &h, &ch, 4);
                free(idata);
                if (px && w > 0 && h > 0 && w <= 512 && h <= 512) {
                    uint32_t* argb = (uint32_t*)malloc((size_t)w * h * 4);
                    if (argb) {
                        const uint8_t* s = px;
                        uint32_t* d = argb;
                        for (int i = 0; i < w * h; i++, s += 4) {
                            uint32_t a = s[3];
                            *d++ = (a << 24) | ((uint32_t)s[0] << 16) |
                                   ((uint32_t)s[1] << 8) | (uint32_t)s[2];
                        }
                        slot->argb = argb;
                        slot->w = w;
                        slot->h = h;
                    }
                }
                stbi_image_free(px);
            }
        }
        free(manifest);
    }
    free(jar);
done:
    snprintf(slot->path, sizeof(slot->path), "%s", path);
    slot->mtime = mtime;
    slot->fsize = fsize;
    /* v36.60 [ICON-BUDGET]: держать суммарные пиксели всех иконок в
     * бюджете, вытесняя самые старые записи. Кэш живёт между игровыми
     * сессиями — это единственный ограничитель его памяти. Гварды:
     * (а) не вытеснять ДО последней записи (одиночная иконка-гигант
     * > бюджета освобождается сама, запись остаётся негативной);
     * (б) сам slot мог уже получить argb=NULL — тогда он ничего не
     * весит и бюджет не давит. */
    if (slot->argb && slot->w > 0 && slot->h > 0)
        g_jaricon_bytes += (size_t)slot->w * slot->h * 4;
    while (g_jaricon_bytes > JARICON_BUDGET_BYTES && g_jaricon_n > 1)
        jaricon_evict_oldest();
    if (g_jaricon_bytes > JARICON_BUDGET_BYTES && g_jaricon_n == 1 &&
        g_jaricons[0].argb) {
        /* единственная запись сама превышает бюджет — пиксели вон,
         * негативная запись (path известен, иконки нет) остаётся */
        jaricon_free_one(&g_jaricons[0]);
    }
    return slot;
}

/* Ленивое заполнение: не более ОДНОЙ незакэшированной иконки за вызов. */
static void path_join(char* dst, size_t cap, const char* dir, const char* name);

static void icons_fill_one(const BrowseList* l, const char* cwd) {
    for (int i = 0; i < l->count; i++) {
        const BrowseEntry* e = &l->items[i];
        if (!e->is_jar) continue;
        char full[1024];
        path_join(full, sizeof(full), cwd, e->name);
        struct stat st;
        long mt = -1, fs = -1;
        if (nojme_pg_stat("icon-stat", full, &st) == 0) {
            mt = (long)st.st_mtime;
            fs = (long)st.st_size;
        }
        if (!jaricon_find(full, mt, fs)) {
            jaricon_extract(full, mt, fs);
            return; /* одна иконка на кадр */
        }
    }
}

/* Блитт иконки: масштабирование nearest-neighbour в квадрат ix..ix+is,
 * по центру по вертикали строки; пиксели с альфой смешиваются с фоном. */
static void jaricon_blit(uint32_t* cv, int cw, int ch,
                         int ix, int iy, int is, const JarIcon* ic) {
    if (!ic || !ic->argb || ic->w <= 0 || ic->h <= 0) return;
    /* вписать с сохранением пропорций */
    int dw = is, dh = is;
    if (ic->w >= ic->h) { dh = is * ic->h / ic->w; }
    else { dw = is * ic->w / ic->h; }
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    int ox = ix + (is - dw) / 2;
    int oy = iy + (is - dh) / 2;
    for (int y = 0; y < dh; y++) {
        int sy = y * ic->h / dh;
        if (oy + y < 0 || oy + y >= ch) continue;
        uint32_t* row = cv + (size_t)(oy + y) * cw;
        const uint32_t* srow = ic->argb + (size_t)sy * ic->w;
        for (int x = 0; x < dw; x++) {
            int sx = x * ic->w / dw;
            int dx = ox + x;
            if (dx < 0 || dx >= cw) continue;
            uint32_t sp = srow[sx];
            uint32_t a = (sp >> 24) & 0xFF;
            if (a == 0) continue;
            if (a == 255) { row[dx] = sp; continue; }
            uint32_t dp = row[dx];
            uint32_t sr = (sp >> 16) & 0xFF, sg = (sp >> 8) & 0xFF, sb = sp & 0xFF;
            uint32_t dr = (dp >> 16) & 0xFF, dg = (dp >> 8) & 0xFF, db = dp & 0xFF;
            row[dx] = 0xFF000000u |
                (((sr * a + dr * (255u - a)) / 255u) << 16) |
                (((sg * a + dg * (255u - a)) / 255u) << 8) |
                 ((sb * a + db * (255u - a)) / 255u);
        }
    }
}

/* Полный сброс кэша иконок (вызывается при завершении игровой сессии). */
void switch_ui_jaricons_reset(void) {
    for (int i = 0; i < g_jaricon_n; i++) jaricon_free_one(&g_jaricons[i]);
    g_jaricon_n = 0;
}

static void path_join(char* dst, size_t cap, const char* dir, const char* name) {
    /* dst may ALIAS dir (cwd updates in place) — build in a temp first:
     * snprintf with overlapping src/dst is undefined behavior. */
    char tmp[1024];
    size_t dl = strlen(dir);
    if (dl && dir[dl - 1] == '/') snprintf(tmp, sizeof(tmp), "%s%s", dir, name);
    else snprintf(tmp, sizeof(tmp), "%s/%s", dir, name);
    snprintf(dst, cap, "%s", tmp);
}

static void path_parent(char* path) {
    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/') n--;
    while (n > 0 && path[n - 1] != '/') n--;
    if (n == 0) { strcpy(path, "/"); return; }
    if (path[n - 1] == '/') n--; /* strip trailing slash of the parent */
    path[n] = '\0';
    if (!path[0]) strcpy(path, "/");
}

static int dir_exists(const char* p) {
    DIR* d = (DIR*)nojme_pg_opendir("dir-exists", p);
    if (!d) return 0;
    closedir(d);
    return 1;
}

/* Root for browsing: settings games_dir, else fallbacks. */
static void browse_root(char* out, size_t cap) {
    const SwitchSettings* st = switch_settings_get();
    const char* env_root = getenv("NOJME_SWITCH_BROWSE_ROOT");
    if (env_root && env_root[0] && dir_exists(env_root)) {
        snprintf(out, cap, "%s", env_root);
        return;
    }
    if (st->games_dir[0] && dir_exists(st->games_dir)) {
        /* games_dir[512] всегда влезает в cwd[1024] у всех вызывающих,
         * но GCC не выводит это из runtime-cap: динамическая точность
         * %.*s делает границу явной (усечение идентично snprintf-норме) */
        snprintf(out, cap, "%.*s", (int)(cap - 1), st->games_dir);
        return;
    }
#ifdef __SWITCH__
    if (dir_exists("sdmc:/")) { snprintf(out, cap, "sdmc:/"); return; }
    snprintf(out, cap, "/");
#else
    snprintf(out, cap, ".");
#endif
}

/* ================= AUTOTEST driver (verification builds) =================
 * NOJME_SWITCH_KEYS="A:300,B:150,UP:100" — press key, hold it for one pump
 * cycle, then advance after the delay (ms). Lets the sandbox drive the
 * whole menu with SDL dummy video. v34.91: the script RESTARTS at every
 * menu session (multi-session E2E); NOJME_SWITCH_AUTOTEST_SESSIONS=<n>
 * caps how many sessions receive synthetic keys (after the cap the menu
 * idles — a deterministic SIGTERM/SDL_QUIT shutdown can end the process). */
static int g_at_enabled = -1;
static char g_at_copy[256];            /* v34.91: OWN copy — env buffers get replaced */
static const char* g_at_script = NULL; /* == g_at_copy or NULL, never a raw env pointer */
static const char* g_at_env_ptr = NULL;
static int g_at_pos = 0;
static uint64_t g_at_next_ms = 0;
static int g_at_sessions = 0;          /* v34.91: menu sessions driven so far */
static int g_at_session_cap = -1;      /* v34.91: NOJME_SWITCH_AUTOTEST_SESSIONS */
/* v36.12 SOAK-EXIT: clean menu exit after the cap (see SOAK-EXIT below) */
static uint64_t s_at_exit_after_ms = 0;
static uint64_t s_at_exit_deadline = 0;

static unsigned at_keys(void) {
    /* v34.91 FIX (found by the multi-session tests): the script pointer was
     * cached once, but SDL_setenv()/setenv() may REPLACE the environment
     * entry (the old buffer is freed/reused) — a second menu session with a
     * different NOJME_SWITCH_KEYS kept reading a DANGLING pointer (garbage
     * tokens or no keys at all, sessions that never ended). The script now
     * lives in an OWN buffer, refreshed whenever the env slot pointer OR its
     * content changes (setenv may also reuse the same address). */
    const char* cur = getenv("NOJME_SWITCH_KEYS");
    if (cur != g_at_env_ptr || (cur && strcmp(cur, g_at_copy) != 0)) {
        g_at_env_ptr = cur;
        snprintf(g_at_copy, sizeof(g_at_copy), "%s", cur ? cur : "");
        g_at_script = g_at_copy[0] ? g_at_copy : NULL;
        g_at_pos = 0;
        g_at_next_ms = 0;
        g_at_enabled = (g_at_script != NULL) ? 1 : 0;
        if (g_at_enabled) ui_log("autotest script: %s\n", g_at_script);
    }
    if (g_at_enabled < 0) g_at_enabled = 0;
    if (!g_at_enabled) return 0;
    if (!g_at_script[g_at_pos]) return 0; /* script exhausted */
    if (sdl_switch_ui_ticks_ms() < g_at_next_ms) return 0;

    /* v36.26 [TOUCH-UI]: dedicated TAP:x,y:delay token — synthetic touch
     * press edge for the sandbox. MUST be tried BEFORE the generic
     * KEY:delay parse (whose %d would eat the x coordinate). */
    {
        int ttx = 0, tty = 0, tdelay = 0, tcons = 0;
        if (sscanf(g_at_script + g_at_pos, " TAP:%d,%d:%d%n",
                   &ttx, &tty, &tdelay, &tcons) >= 3 && tcons > 0) {
            g_at_pos += tcons;
            while (g_at_script[g_at_pos] == ',' || g_at_script[g_at_pos] == ' ') g_at_pos++;
            g_at_next_ms = sdl_switch_ui_ticks_ms() + (uint64_t)(tdelay < 0 ? 0 : tdelay);
            ui_log("autotest tap %d,%d delay=%dms\n", ttx, tty, tdelay);
            sdl_switch_touch_inject(ttx, tty);
            return 0; /* no key bits — screens claim it via sdl_switch_touch_tap */
        }
    }

    /* parse one token: KEY:delay */
    char key[16] = {0};
    int delay = 0;
    int consumed = 0;
    if (sscanf(g_at_script + g_at_pos, "%15[^:]:%d%n", key, &delay, &consumed) >= 2 && consumed > 0) {
        g_at_pos += consumed;
        while (g_at_script[g_at_pos] == ',' || g_at_script[g_at_pos] == ' ') g_at_pos++;
        g_at_next_ms = sdl_switch_ui_ticks_ms() + (uint64_t)(delay < 0 ? 0 : delay);
        ui_log("autotest key=%s delay=%dms\n", key, delay);
        if (strcmp(key, "UP") == 0) return SWK_UP;
        if (strcmp(key, "DOWN") == 0) return SWK_DOWN;
        if (strcmp(key, "LEFT") == 0) return SWK_LEFT;
        if (strcmp(key, "RIGHT") == 0) return SWK_RIGHT;
        if (strcmp(key, "A") == 0) return SWK_A;
        if (strcmp(key, "B") == 0) return SWK_B;
        if (strcmp(key, "MINUS") == 0) return SWK_MINUS; /* v34.90 */
        if (strcmp(key, "EXIT") == 0) return SWK_EXIT;
    } else {
        g_at_pos = (int)strlen(g_at_script); /* malformed — stop */
    }
    return 0;
}

/* ================= Menu screens ================= */

typedef enum {
    MMAIN_MAIN = 0,
    MMAIN_BROWSER,
    MMAIN_SETTINGS,
    MMAIN_DIRPICKER,
    MMAIN_KEYS /* v36.14: экран «Назначенные кнопки» */
} MenuScreen;

#define VISIBLE_ROWS 11 /* v34.90: 96px header (language badge row) */
#define ROW_H 48
#define LIST_Y 134
/* v36.15: компактные строки для экранов настроек (глобальных и per-game):
 * список заканчивается выше, освобождая полосу под всплывающее описание
 * настройки (см. draw_hint_box). 10 строк x 38 = 380 -> до y=514 при LIST_Y.
 * v36.17: 42 -> 38 — освободить место под всплывающее описание ШРИФТОМ
 * SCALE 2 (читаемость в портативном режиме): 2 строки + пометка
 * помещаются в полосу 514..660 без перекрытия списка. */
#define SET_ROW_H 38
#define HINT_STRIP_TOP 514   /* верх полосы описаний (низ на 660, футер на 672) */
#define HINT_STRIP_BOTTOM 660

/* v34.90: top-left corner badge — с v36.25 рисуется switch_lang_badge_draw
 * (кнопка MINUS + короткий код языка). Returns used width (не используется). */
static int draw_lang_badge(uint32_t* cv, int cw) {
    switch_lang_badge_draw(cv, cw, SWITCH_UI_HEIGHT, 32, 8, COL_DIM);
    return 0;
}

/* v36.26 [TOUCH-UI]: hit box of the language badge (см. switch_lang_badge_draw:
 * рамка 40x26 от (32, y_off) + код языка правее). Щедрая рамка под палец.
 * y_off = 8 в шапках меню, 24 в оверлее паузы. */
int switch_ui_badge_hit(int x, int y, int y_off) {
    return x >= 24 && x <= 150 && y >= y_off - 8 && y <= y_off + 34;
}

/* ================= [BATFIX] v36.41: бейдж заряда (правый верхний угол) =================
 * Одна рисовалка на все потребители: канва меню 1280x720 (главное меню /
 * браузер / настройки / справка), оверлей меню паузы и ОТДЕЛЬНАЯ текстура
 * оверлея игрового кадра (sdl_graphics.c) — во всех случаях бейдж
 * рисуется в ПРАВОМ ВЕРХНЕМ углу ПЕРЕДАННОЙ канвы, поэтому маленькая
 * текстура-оверлей даёт тот же угол экрана после RenderCopy к правому
 * краю. mode: NOJME_BATT_OFF ничего не рисует, ICON — значок батареи с
 * заливкой по проценту, NUM — «NN%» (при неизвестном заряде «--%» /
 * пустой корпус).
 *
 * v36.43 [BATT-DEBOLT] (полевой отчёт): ОТДЕЛЬНАЯ молния слева от значка
 * удалена — на экране она читалась как «вертикальное пятно», а не как
 * значок зарядки. Бейдж показывает ТОЛЬКО уровень: заливку корпуса по
 * проценту или цифры «NN%». Зарядка по-прежнему доступна из J2ME
 * (nojme.battery.charging / nojme.device.Battery.isCharging()).
 *
 * Подложка — НЕПРОЗРАЧНАЯ тёмная плашка с рамкой (v36.43: 0xF0 -> 0xFF —
 * читаемость на светлом кадре игры больше не зависит от состояния
 * блендинга рендерера): читаемо и на светлом кадре, и на тёмном меню. */
#define BATT_BADGE_PAD   12  /* внутренние поля плашки */
#define BATT_BADGE_TOP    8  /* отступ от верхнего края канвы */
#define BATT_BADGE_RIGHT 16  /* отступ от правого края канвы */

/* Цвет заливки корпуса по проценту: зелёный > 40, жёлтый 15..40,
 * красный < 15 (пороги как в системных индикаторах). */
static uint32_t switch_batt_fill_color(int pct) {
    if (pct < 0)  return 0xFF8A93A6u; /* неизвестно — серый */
    if (pct <= 15) return 0xFFFF5252u;
    if (pct <= 40) return 0xFFFFC53Du;
    return 0xFF3DDC84u;
}

/* Значок: корпус 44x22 (рамка 2px) + носик 4x10, заливка ширины
 * pct% от 40 внутренних пикселей. v36.43 [BATT-DEBOLT]: встроенная
 * молния поверх заливки удалена вместе с отдельным глифом. */
static void switch_batt_icon_draw(uint32_t* cv, int cw, int ch,
                                  int x, int y, int pct) {
    const uint32_t COL_FRAME = 0xFFE8ECF4u;
    fill_rect(cv, cw, ch, x, y, 44, 22, COL_FRAME);          /* корпус */
    fill_rect(cv, cw, ch, x + 2, y + 2, 40, 18, 0xFF10131Au); /* внутренность */
    fill_rect(cv, cw, ch, x + 44, y + 6, 4, 10, COL_FRAME);   /* носик */
    if (pct > 0) {
        int fw = (pct * 40 + 50) / 100;
        if (fw < 1) fw = 1;
        if (fw > 40) fw = 40;
        fill_rect(cv, cw, ch, x + 2, y + 2, fw, 18,
                  switch_batt_fill_color(pct));
    }
}

void switch_ui_batt_badge_draw(uint32_t* cv, int cw, int ch, int mode) {
    if (!cv || cw < 160 || ch < 64) return;
    if (mode != NOJME_BATT_ICON && mode != NOJME_BATT_NUM) return;

    int pct = nojme_battery_percent();

    /* содержимое плашки: значок 48x22 или текст «NN%» (scale 2 = 32px
     * высоты); v36.43 [BATT-DEBOLT]: молнии больше нет — только уровень */
    char txt[16]; /* «-2147483648%» влезает без -Wformat-truncation */
    if (pct < 0) snprintf(txt, sizeof(txt), "--%%");
    else         snprintf(txt, sizeof(txt), "%d%%", pct);
    int tw = (mode == NOJME_BATT_NUM)
        ? switch_font_text_width(txt, 2) : 48;
    int pill_w = tw + 2 * BATT_BADGE_PAD;
    int pill_h = 44;
    int px = cw - pill_w - BATT_BADGE_RIGHT;
    int py = BATT_BADGE_TOP;

    /* подложка: тёмная НЕПРОЗРАЧНАЯ плашка (v36.43: 0xF0 -> 0xFF)
     * + рамка в стиле UI */
    fill_rect(cv, cw, ch, px, py, pill_w, pill_h, 0xFF10131Au);
    hline(cv, cw, ch, px, px + pill_w, py, 0xFF2A3040u);
    hline(cv, cw, ch, px, px + pill_w, py + pill_h - 1, 0xFF2A3040u);
    fill_rect(cv, cw, ch, px, py, 1, pill_h, 0xFF2A3040u);
    fill_rect(cv, cw, ch, px + pill_w - 1, py, 1, pill_h, 0xFF2A3040u);

    int cx = px + BATT_BADGE_PAD;
    if (mode == NOJME_BATT_ICON) {
        switch_batt_icon_draw(cv, cw, ch, cx, py + (pill_h - 22) / 2, pct);
    } else {
        draw_text(cv, cw, ch, cx, py + (pill_h - 32) / 2, txt,
                  switch_batt_fill_color(pct), 2);
    }
}

#define HEADER_H 96 /* v34.90: 72 -> 96 (badge row on top) */

static void draw_header(uint32_t* cv, int cw, const char* title, const char* right) {
    fill_rect(cv, cw, SWITCH_UI_HEIGHT, 0, 0, cw, HEADER_H, COL_HEADER);
    hline(cv, cw, SWITCH_UI_HEIGHT, 0, cw, HEADER_H, COL_LINE);
    draw_lang_badge(cv, cw);
    draw_text(cv, cw, SWITCH_UI_HEIGHT, 32, 46, title, COL_TEXT, 3);
    if (right) draw_text(cv, cw, SWITCH_UI_HEIGHT, cw - 32 - switch_font_text_width(right, 2), 54, right, COL_DIM, 2);
}

static void draw_footer(uint32_t* cv, int cw, const char* hint) {
    fill_rect(cv, cw, SWITCH_UI_HEIGHT, 0, SWITCH_UI_HEIGHT - 48, cw, 48, COL_HEADER);
    hline(cv, cw, SWITCH_UI_HEIGHT, 0, cw, SWITCH_UI_HEIGHT - 48, COL_LINE);
    draw_text(cv, cw, SWITCH_UI_HEIGHT, 32, SWITCH_UI_HEIGHT - 38, hint, COL_DIM, 2);
}

/* ================= v36.15: всплывающее описание настройки =================
 * Маленькое окно в фиксированной полосе над футером: описание выделенного
 * пункта настроек + пометка о моменте применения (в т.ч. «требуется
 * перезапуск игры» для пунктов, которые печатаются в геометрию сессии).
 * Рисуется при каждом кадре, пока экран настроек открыт — то есть всегда
 * для текущего выделенного пункта («появляется при выделении пункта»). */

#define HINT_TEXT_SCALE 2     /* v36.17: подсказки шрифтом x2 (портативный режим) */
#define HINT_LINE_ADV 38      /* межстрочный интервал при scale 2 (глиф 16*2) */
/* Бюджет ширины строки подсказки в ПИКСЕЛЯХ scale 1 (замер всегда scale 1,
 * отрисовка scale 2 => фактическая ширина x2). Текст от x=48 до рамки 1248
 * с правым полем 16: 1200/2 = 600. */
#define HINT_WRAP_W_PX 600

/* Перенос по словам + отрисовка одной строки описания. Возвращает число
 * занятых строк. Байтовый проход: разбиение только по ASCII-пробелам,
 * безопасно для UTF-8 (пробел не бывает частью мультибайтной серии).
 * NUL-байт никогда не попадает в take — указатель p не проходит мимо
 * конца строки (v36.15 аудит переполнения буфера). */
static int hint_draw_wrapped(uint32_t* cv, int cw, int x, int y,
                             const char* s, uint32_t col) {
    int lines = 0;
    const char* p = s;
    if (!s || !s[0]) return 0;
    while (*p) {
        char chunk[256];
        char tmp[256];
        size_t take = 0;
        /* жадно берём слова, пока влезают в бюджет ширины (scale-1 замер) */
        while (p[take] && take < sizeof(chunk) - 1) {
            size_t cand = take;
            while (p[cand] && p[cand] != ' ' && cand < sizeof(chunk) - 2) cand++;
            if (p[cand] == ' ') cand++; /* пробел включаем, NUL — НЕТ */
            if (cand > sizeof(tmp) - 1) cand = sizeof(tmp) - 1;
            memcpy(tmp, p, cand); tmp[cand] = 0;
            if (switch_font_text_width(tmp, 1) > HINT_WRAP_W_PX && take > 0)
                break; /* не влезает — перенос */
            take = cand;
        }
        if (take == 0) take = 1; /* патология: режем по байту */
        {
            size_t n = take < sizeof(chunk) - 1 ? take : sizeof(chunk) - 1;
            memcpy(chunk, p, n); chunk[n] = 0;
            draw_text(cv, cw, SWITCH_UI_HEIGHT, x, y, chunk, col, HINT_TEXT_SCALE);
            p += n;
        }
        y += HINT_LINE_ADV;
        lines++;
        while (*p == ' ') p++;
    }
    return lines;
}

/* Полоса-окно с описанием: desc обычным цветом, note (если есть) —
 * янтарным акцентом. Anchored: низ HINT_STRIP_BOTTOM, высота по контенту. */
static void draw_hint_box(uint32_t* cv, int cw, const char* desc, const char* note) {
    if (!desc || !desc[0]) return;
    /* v36.17: высота по ширине строки (scale-1 замер) и бюджету
     * HINT_WRAP_W_PX; отрисовка scale 2 => межстрочный HINT_LINE_ADV. */
    int w1 = switch_font_text_width(desc, 1);
    int dlines = (w1 + HINT_WRAP_W_PX - 1) / HINT_WRAP_W_PX;
    if (dlines < 1) dlines = 1;
    int h = 10 + dlines * HINT_LINE_ADV + (note && note[0] ? 4 + HINT_LINE_ADV : 0) + 8;
    int y0 = HINT_STRIP_BOTTOM - h;
    fill_rect(cv, cw, SWITCH_UI_HEIGHT, 32, y0, cw - 64, h, COL_BG_ALT);
    fill_rect(cv, cw, SWITCH_UI_HEIGHT, 32, y0, 4, h, COL_ACCENT);          /* левая полоса-акцент */
    hline(cv, cw, SWITCH_UI_HEIGHT, 32, cw - 32, y0, COL_LINE);             /* рамка сверху */
    hline(cv, cw, SWITCH_UI_HEIGHT, 32, cw - 32, HINT_STRIP_BOTTOM - 1, COL_LINE); /* и снизу */
    hint_draw_wrapped(cv, cw, 48, y0 + 10, desc, COL_TEXT);
    if (note && note[0])
        draw_text(cv, cw, SWITCH_UI_HEIGHT, 48, y0 + 10 + dlines * HINT_LINE_ADV + 4,
                  note, 0xFFE0B040u, HINT_TEXT_SCALE); /* янтарная пометка */
}

/* ================= v36.17: анимация перемещения курсора =================
 * Синяя полоса выделения не «прыгает» со строки на строку, а СКОЛЬЗИТ к
 * новой позиции за CURSOR_ANIM_MS (smoothstep — плавный старт/останов).
 * Экраны списков перед отрисовкой сообщают геометрию (origin_y, row_h) и
 * индекс выделенной строки; обработчики ввода вызывают cursor_anim_arm()
 * при СМЕНЕ выделения (скролл окна и смена экрана анимацию гасят). */
#define CURSOR_ANIM_MS 120
static struct {
    int active;
    float from_idx;      /* дробный индекс строки (для докатки при быстрых нажатиях) */
    float to_idx;
    int origin_y;        /* y первой строки списка */
    int row_h;
    uint64_t t0;
} s_canim;

static void cursor_anim_cancel(void) { s_canim.active = 0; }

/* от: текущая интерполированная позиция (быстрые нажатия не рвут движение) */
static void cursor_anim_arm(float from_idx, int to_idx, int origin_y, int row_h) {
    float f = from_idx;
    if (s_canim.active && s_canim.origin_y == origin_y && s_canim.row_h == row_h) {
        uint64_t el = sdl_switch_ui_ticks_ms() - s_canim.t0;
        if (el < CURSOR_ANIM_MS) {
            float t = (float)el / (float)CURSOR_ANIM_MS;
            t = t * t * (3.0f - 2.0f * t); /* smoothstep */
            f = s_canim.from_idx + (s_canim.to_idx - s_canim.from_idx) * t;
        }
    }
    s_canim.active = 1;
    s_canim.from_idx = f;
    s_canim.to_idx = (float)to_idx;
    s_canim.origin_y = origin_y;
    s_canim.row_h = row_h;
    s_canim.t0 = sdl_switch_ui_ticks_ms();
}

/* Верхняя кромка синей полосы выделения для строки idx_eff (в координатах
 * видимого окна). Вне анимации = origin_y + idx*row_h - 4 (как раньше).
 * Рисующий код кладёт результат в s_hl_y_override и вызывает draw_row. */
static int cursor_anim_highlight_top(int origin_y, int row_h, int idx_eff) {
    const int target = origin_y + idx_eff * row_h - 4;
    if (!s_canim.active) return target;
    if (s_canim.origin_y != origin_y || s_canim.row_h != row_h) {
        s_canim.active = 0;
        return target;
    }
    uint64_t el = sdl_switch_ui_ticks_ms() - s_canim.t0;
    if (el >= CURSOR_ANIM_MS) {
        s_canim.active = 0;
        return target;
    }
    float t = (float)el / (float)CURSOR_ANIM_MS;
    t = t * t * (3.0f - 2.0f * t);
    const float fy = origin_y + s_canim.from_idx * row_h - 4;
    const float ty = origin_y + s_canim.to_idx * row_h - 4;
    return (int)(fy + (ty - fy) * t + 0.5f);
}

/* Переопределение y полосы выделения для ТЕКУЩЕЙ отрисовке списка;
 * -1 = обычное поведение (полоса на самой строке). */
static int s_hl_y_override = -1;

/* v36.17: полоса выделения при анимации рисуется ДО строк списка — она
 * скользит ПОД текстом (строки, мимо которых проходит полоса, остаются
 * читаемыми). При статичном курсоре draw_row рисует полосу сама. */
static void draw_hl_bar_underlay(uint32_t* cv, int cw, int row_h) {
    if (s_hl_y_override >= 0)
        fill_rect(cv, cw, SWITCH_UI_HEIGHT, 16, s_hl_y_override, cw - 32, row_h,
                  COL_SEL_BG);
}

/* Generic list row drawing; returns y of the next row.
 * v36.15: row_h is explicit — the settings screens use compact rows
 * so a description popup strip fits between the list and the footer.
 * v36.58 [JAR-ICONS]: draw_row_at — вариант с явным x текста (колонка
 * иконок браузера сдвигает подписи вправо); draw_row = старый x=40. */
static int draw_row_at(uint32_t* cv, int cw, int y, int text_x, const char* label,
                       const char* value, int selected, int i, int total, int row_h) {
    if (selected && s_hl_y_override < 0) {
        /* статичная полоса (без анимации) — на самой строке */
        fill_rect(cv, cw, SWITCH_UI_HEIGHT, 16, y - 4, cw - 32, row_h, COL_SEL_BG);
    }
    draw_text_sh(cv, cw, SWITCH_UI_HEIGHT, text_x, y + 4, label,
                 selected ? COL_SEL_FG : COL_TEXT, 2);
    if (value) {
        int vw = switch_font_text_width(value, 2);
        draw_text_sh(cv, cw, SWITCH_UI_HEIGHT, cw - 48 - vw, y + 4, value,
                     selected ? COL_SEL_FG : COL_ACCENT, 2);
    }
    (void)i; (void)total;
    return y + row_h;
}

static int draw_row(uint32_t* cv, int cw, int y, const char* label, const char* value,
                    int selected, int i, int total, int row_h) {
    return draw_row_at(cv, cw, y, 40, label, value, selected, i, total, row_h);
}

static void draw_scrollbar(uint32_t* cv, int cw, int count, int sel) {
    if (count <= VISIBLE_ROWS) return;
    int track_y = LIST_Y - 4;
    int track_h = VISIBLE_ROWS * ROW_H;
    fill_rect(cv, cw, SWITCH_UI_HEIGHT, cw - 14, track_y, 6, track_h, COL_LINE);
    int thumb_h = track_h * VISIBLE_ROWS / count;
    if (thumb_h < 30) thumb_h = 30;
    int thumb_y = track_y + (track_h - thumb_h) * sel / (count - 1);
    fill_rect(cv, cw, SWITCH_UI_HEIGHT, cw - 14, thumb_y, 6, thumb_h, COL_ACCENT);
}

/* ================= Browser screen ================= */

typedef struct {
    char cwd[1024];
    BrowseList list;
    int sel;
    int scroll;
    /* возвращение из dirpicker */
    int return_to_settings;
} BrowserState;

static void browser_draw(uint32_t* cv, const BrowserState* b) {
    int cw = SWITCH_UI_WIDTH;
    /* [WILDGUARD] v36.50: вспышка InvalidAccess "tings.in" до запуска
     * мидлета (= браузер, главный поток) — канва и массив записей
     * проверяются до разыменования; мусор уходит в пустой экран кадра. */
    if (WILDGUARD_SKIP(cv, "browse:cv")) return;
    if (b->list.count > 0 && WILDGUARD_SKIP(b->list.items, "browse:items")) {
        char title[512];
        utf8_trunc(b->cwd, title, sizeof(title), 70);
        draw_header(cv, cw, switch_text(ST_BROWSER_TITLE), title);
        draw_text(cv, cw, SWITCH_UI_HEIGHT, 40, LIST_Y + 8,
                  switch_text(ST_BROWSER_EMPTY), COL_DIM, 2);
        return;
    }
    char title[512];
    utf8_trunc(b->cwd, title, sizeof(title), 70);
    draw_header(cv, cw, switch_text(ST_BROWSER_TITLE), title);

    int count = b->list.count;
    if (count == 0) {
        draw_text(cv, cw, SWITCH_UI_HEIGHT, 40, LIST_Y + 8, switch_text(ST_BROWSER_EMPTY), COL_DIM, 2);
        if (b->list.truncated)
            draw_text(cv, cw, SWITCH_UI_HEIGHT, 40, LIST_Y + 48, switch_text(ST_BROWSER_TRUNC), COL_DIM, 2);
    }
    int y = LIST_Y;
    int from = b->scroll;
    int to = from + VISIBLE_ROWS;
    if (to > count) to = count;
    if (count == 0) s_hl_y_override = -1; /* v36.17: пустой список — без полосы */
    draw_hl_bar_underlay(cv, cw, ROW_H); /* v36.17: полоса под текстом */
    /* v36.58 [JAR-ICONS]: одна новая иконка за кадр — список мгновенно
     * готов, иконки догоняют за секунду. */
    if (b->list.count > 0) icons_fill_one(&b->list, b->cwd);
    for (int i = from; i < to; i++) {
        const BrowseEntry* e = &b->list.items[i];
        char label[320];
        if (e->is_dir) snprintf(label, sizeof(label), "[%s]", e->name);
        else snprintf(label, sizeof(label), "%s", e->name);
        char shown[320];
        utf8_trunc(label, shown, sizeof(shown), 60);
        /* v36.58 [JAR-ICONS]: колонка иконок 40x40 слева; текст jar сдвинут
         * вправо. Папки — без иконки ([NO-DIRICON] v36.61, было: значок
         * поверх имени), jar — иконка мидлета (если есть). */
        const int ic_size = 40;
        const int ic_x = 40;
        const int ic_y = y + 4;
        int text_x = 40;
        if (!e->is_dir) {
            text_x = ic_x + ic_size + 16;
            char full[1024];
            path_join(full, sizeof(full), b->cwd, e->name);
            struct stat st;
            long mt = -1, fs = -1;
            if (nojme_pg_stat("icon-stat", full, &st) == 0) {
                mt = (long)st.st_mtime;
                fs = (long)st.st_size;
            }
            const JarIcon* ic = jaricon_find(full, mt, fs);
            if (ic) jaricon_blit(cv, cw, SWITCH_UI_HEIGHT, ic_x, ic_y, ic_size, ic);
        }
        draw_row_at(cv, cw, y, text_x, shown,
                    e->is_dir ? switch_text(ST_BROWSER_DIR) : "JAR",
                    i == b->sel, i, count, ROW_H);
        /* [NO-DIRICON] v36.61: значок-папка для каталогов УБРАН. v36.58
         * рисовал его пятью синими fill_rect в колонке иконок (x=40..80),
         * но текст папок не сдвигался (оставался на x=40) — контур ложился
         * ПОВЕРХ имени, в поле это выглядело «синими квадратами поверх
         * папок». Папки снова без иконки (как до v36.58): «[имя] папка».
         * Колонка иконок остаётся только у jar-файлов. */
        y += ROW_H;
    }
    draw_scrollbar(cv, cw, count, b->sel);

    char footer[256];
    snprintf(footer, sizeof(footer), "%s%d/%d", switch_text(ST_FOOTER_BROWSER),
             count ? b->sel + 1 : 0, count);
    draw_footer(cv, cw, footer);
}

/* Returns: 0 — stay, 1 — jar selected (out_path), 2 — back to main. */
static int browser_handle(BrowserState* b, unsigned keys, char* out_path, size_t cap) {
    int count = b->list.count;
    if (count > 0) {
        if (keys & SWK_UP) { b->sel--; if (b->sel < 0) b->sel = count - 1; }
        if (keys & SWK_DOWN) { b->sel++; if (b->sel >= count) b->sel = 0; }
    }
    /* window scroll */
    if (b->sel < b->scroll) b->scroll = b->sel;
    if (b->sel >= b->scroll + VISIBLE_ROWS) b->scroll = b->sel - VISIBLE_ROWS + 1;
    if (b->scroll < 0) b->scroll = 0;

    if (keys & SWK_B) {
        if (strcmp(b->cwd, "/") == 0 || b->cwd[0] == '\0') return 2; /* уже корень — в главное меню */
        path_parent(b->cwd);
        browse_load(&b->list, b->cwd);
        b->sel = 0;
        b->scroll = 0;
        return 0;
    }
    if (keys & SWK_A && count > 0) {
        const BrowseEntry* e = &b->list.items[b->sel];
        ui_log("browser A: sel=%d/%d '%s' dir=%d\n", b->sel, count, e->name, e->is_dir);
        if (e->is_dir) {
            path_join(b->cwd, sizeof(b->cwd), b->cwd, e->name);
            browse_load(&b->list, b->cwd);
            b->sel = 0;
            b->scroll = 0;
        } else {
            path_join(out_path, cap, b->cwd, e->name);
            ui_log("game selected: %s\n", out_path);
            sw_trace("game: %s", out_path); /* v34.91: the VM session follows this line */
            return 1;
        }
    }
    return 0;
}

/* ================= Settings screen ================= */

typedef enum {
    SET_SCALE = 0,
    SET_FILTER,
    SET_TEXF,     /* v36.11: сглаживание M3G-текстур */
    SET_FLIP,     /* v35.03: глобальный флип 3D */
    SET_AUDIO,
    SET_VM_SPEED,
    SET_GAMES_DIR,
    SET_LOG,
    SET_FCNT,     /* v35.07: счётчик кадров */
    SET_LANG,     /* v36.25: язык интерфейса (дублирует MINUS-тоггл) */
    SET_DPAD,     /* v36.37: «Поменять местами стик и D-pad» (было: раскладка D-pad) */
    SET_DEADZONE, /* v36.37: мёртвая зона стика, % (0..90, шаг 5) */
    SET_BATTERY,  /* [BATFIX] v36.41: индикатор заряда (не показывать/значок/цифра) */
    SET_SAVE,
    SET_COUNT
} SettingsItem;

/* v36.37: видимое окно списка настроек. Полоса подсказок занимает
 * 514..660 (HINT_STRIP_*), список стартует на LIST_Y=134: 514-134=380px
 * = 10 строк по 38px. Пунктов стало 14 ([BATFIX] v36.41: + Заряд
 * аккумулятора) — без скролла справка перекрывала последние строки
 * (репорт пользователя). */
#define SET_VISIBLE 10

static const char* settings_value(const SwitchSettings* s, int item) {
    static char buf[512];
    switch (item) {
        case SET_SCALE:     return switch_scale_mode_name(s->scale_mode);
        case SET_FILTER:    return switch_filter_name(s->filter);
        case SET_TEXF:      return switch_texf_name(s->tex_filter);
        case SET_FLIP:      return switch_flip_mode_name(s->flip_mode);
        case SET_AUDIO:     return s->audio_enabled ? switch_text(ST_VAL_ON)
                                                    : switch_text(ST_VAL_OFF);
        case SET_VM_SPEED:  return switch_vm_speed_name(s->vm_speed);
        case SET_GAMES_DIR:
            utf8_trunc(s->games_dir, buf, sizeof(buf), 30);
            return buf;
        case SET_LOG:       return s->logging ? switch_text(ST_VAL_ON)
                                                : switch_text(ST_VAL_OFF);
        case SET_FCNT:      return s->frame_counter ? switch_text(ST_VAL_ON)
                                                    : switch_text(ST_VAL_OFF);
        case SET_LANG:      return switch_lang_name(switch_lang()); /* v36.25 */
        case SET_DPAD:      return switch_swap_name(s->input_swap); /* v36.37 */
        case SET_DEADZONE:
            /* v36.37: шаг NOJME_DEADZONE_STEP (%), с заворачиванием;
             * A/RIGHT = больше, LEFT = меньше. Читается pump-ом сразу. */
            {
                static char dzbuf[32];
                snprintf(dzbuf, sizeof(dzbuf), "%d%%", s->stick_deadzone);
                return dzbuf;
            }
        case SET_BATTERY:  /* [BATFIX] v36.41 */
            return switch_batt_name(s->battery_display);
        default: return NULL;
    }
}

/* v36.15: описание + пометка о моменте применения для строки настроек.
 * Глобальные настройки доступны только из главного меню (игра не запущена),
 * поэтому «перезапуск» здесь не нужен — игра-зависимые пункты применяются
 * при её запуске, служебные — сразу. */
static void settings_hint(int item, const char** desc, const char** note) {
    switch (item) {
        case SET_SCALE:     *desc = switch_text(ST_HINT_SCALE);    *note = switch_text(ST_HINT_ONLAUNCH); break;
        case SET_FILTER:    *desc = switch_text(ST_HINT_FILTER);   *note = switch_text(ST_HINT_ONLAUNCH); break;
        case SET_TEXF:      *desc = switch_text(ST_HINT_TEXF);     *note = switch_text(ST_HINT_ONLAUNCH); break;
        case SET_FLIP:      *desc = switch_text(ST_HINT_FLIP);     *note = switch_text(ST_HINT_ONLAUNCH); break;
        case SET_AUDIO:     *desc = switch_text(ST_HINT_AUDIO);    *note = switch_text(ST_HINT_ONLAUNCH); break;
        case SET_VM_SPEED:  *desc = switch_text(ST_HINT_VMSPEED);  *note = switch_text(ST_HINT_ONLAUNCH); break;
        case SET_GAMES_DIR: *desc = switch_text(ST_HINT_GAMESDIR); *note = switch_text(ST_HINT_NOW); break;
        case SET_LOG:       *desc = switch_text(ST_HINT_LOG);      *note = switch_text(ST_HINT_NOW); break;
        case SET_FCNT:      *desc = switch_text(ST_HINT_FCNT);     *note = switch_text(ST_HINT_ONLAUNCH); break;
        case SET_LANG:      *desc = switch_text(ST_HINT_LANG);     *note = switch_text(ST_HINT_NOW); break;
        case SET_DPAD:      *desc = switch_text(ST_HINT_DPAD);     *note = switch_text(ST_HINT_NOW); break;
        case SET_DEADZONE:  *desc = switch_text(ST_HINT_DEADZONE); *note = switch_text(ST_HINT_NOW); break;
        case SET_BATTERY:   *desc = switch_text(ST_HINT_BATT);     *note = switch_text(ST_HINT_NOW); break; /* [BATFIX] v36.41 */
        case SET_SAVE:      *desc = switch_text(ST_HINT_SAVE);     *note = NULL; break;
        default:            *desc = NULL; *note = NULL; break;
    }
}

/* v36.37: scroll передан явно (хранится в главном цикле рядом с set_sel).
 * Рисуется окно [scroll, scroll+SET_VISIBLE) — справка (полоса 514..660)
 * больше НЕ перекрывает строки (репорт пользователя). */
static void settings_draw(uint32_t* cv, const SwitchSettings* s, int sel, int scroll) {
    int cw = SWITCH_UI_WIDTH;
    draw_header(cv, cw, switch_text(ST_SETTINGS_TITLE), NULL);
    const char* names[SET_COUNT] = {
        switch_text(ST_SET_SCALE), switch_text(ST_SET_FILTER),
        switch_text(ST_SET_TEXF),
        switch_text(ST_SET_FLIP),
        switch_text(ST_SET_AUDIO), switch_text(ST_SET_VM_SPEED),
        switch_text(ST_SET_GAMES_DIR), switch_text(ST_SET_LOG),
        switch_text(ST_SET_FCNT), switch_text(ST_SET_LANG),
        switch_text(ST_SET_DPAD), /* v36.37: «Поменять местами стик и D-pad» */
        switch_text(ST_SET_DEADZONE), /* v36.37 */
        switch_text(ST_SET_BATT), /* [BATFIX] v36.41: «Заряд аккумулятора» */
        switch_text(ST_SET_SAVE)
    };
    int y = LIST_Y;
    draw_hl_bar_underlay(cv, cw, SET_ROW_H); /* v36.17: полоса под текстом */
    for (int i = scroll; i < SET_COUNT && i < scroll + SET_VISIBLE; i++) {
        y = draw_row(cv, cw, y, names[i], settings_value(s, i), i == sel,
                     i - scroll, SET_VISIBLE, SET_ROW_H); /* v36.37: окно */
    }
    /* v35.12 FIX: живой предпросмотр рамки масштабирования рисуется ТОЛЬКО
     * когда курсор стоит на строке «Масштаб изображения». Раньше блок
     * рисовался всегда и перекрывал нижние строки списка (Лог / Счётчик
     * кадров / Сохранить) — настройки под ним нельзя было прочитать.
     * v36.15: предпросмотр поднят выше (340..540), чтобы не залезать в
     * полосу всплывающего описания. v36.17: предпросмотр рисуется ПЕРЕД
     * всплывающим описанием (окно описания — верхний слой). */
    if (sel == SET_SCALE) {
        int pv_w = 420, pv_h = 200;
        int pv_x = cw - pv_w - 60, pv_y = 340;
        draw_text(cv, cw, SWITCH_UI_HEIGHT, pv_x, pv_y - 32, switch_text(ST_PREVIEW_LABEL), COL_DIM, 2);
        int rx, ry, rw, rh;
        extern void switch_scaling_game_rect(int, int, int, int, int, int*, int*, int*, int*);
        switch_scaling_game_rect(s->scale_mode, 240, 320, pv_w, pv_h, &rx, &ry, &rw, &rh);
        fill_rect(cv, cw, SWITCH_UI_HEIGHT, pv_x, pv_y, pv_w, pv_h, COL_BG_ALT);
        hline(cv, cw, SWITCH_UI_HEIGHT, pv_x, pv_x + pv_w, pv_y, COL_LINE);
        if (s->scale_mode == NOJME_SCALE_FIT_BLUR) {
            /* имитация блюр-фона: тёмно-синяя «размытая» подложка */
            fill_rect(cv, cw, SWITCH_UI_HEIGHT, pv_x, pv_y, pv_w, pv_h, 0xFF1E2A44);
            fill_rect(cv, cw, SWITCH_UI_HEIGHT, pv_x + rx, pv_y + ry, rw, rh, 0xFF3B4C6E);
        } else {
            fill_rect(cv, cw, SWITCH_UI_HEIGHT, pv_x, pv_y, pv_w, pv_h, 0xFF000000);
            fill_rect(cv, cw, SWITCH_UI_HEIGHT, pv_x + rx, pv_y + ry, rw, rh, 0xFF3B4C6E);
        }
        draw_text(cv, cw, SWITCH_UI_HEIGHT, pv_x + rx + 8, pv_y + ry + rh / 2 - 16, "240x320", COL_DIM, 2);
    }
    /* v36.15: всплывающее описание выделенного пункта настроек
     * (v36.17: рисуется последним — поверх предпросмотра масштаба) */
    {
        const char* hd = NULL; const char* hn = NULL;
        settings_hint(sel, &hd, &hn);
        draw_hint_box(cv, cw, hd, hn);
    }
    draw_footer(cv, cw, switch_text(ST_FOOTER_SETTINGS));
}

/* Returns 0 stay, 1 back to main (SAVED — apply), 2 back WITHOUT save.
 * [BATFIX] v36.41 [SET-EXIT]: раньше B и «Сохранить и выйти» возвращали
 * один и тот же код 1, и обработчик меню после ЛЮБОГО выхода звал
 * switch_settings_set(&st) — правки из несохранённого выхода применялись
 * к текущей сессии (репорт пользователя: «если выйти без сохранения они
 * всё равно применяются, только на одну сессию — они не должны
 * применяться»). Теперь B = 2 = ОТКАТ рабочей копии к сохранённым
 * значениям; «Сохранить и выйти» = 1 (файл уже записан внутри). */
static int settings_handle(SwitchSettings* s, int* sel, unsigned keys) {
    if (keys & SWK_UP) { (*sel)--; if (*sel < 0) *sel = SET_COUNT - 1; }
    if (keys & SWK_DOWN) { (*sel)++; if (*sel >= SET_COUNT) *sel = 0; }

    if (keys & SWK_B) return 2; /* [BATFIX] v36.41: выход БЕЗ применения */

    if (keys & (SWK_A | SWK_LEFT | SWK_RIGHT)) {
        int dir = (keys & SWK_LEFT) ? -1 : 1;
        switch (*sel) {
            case SET_SCALE:
                if (keys & SWK_A) s->scale_mode = (s->scale_mode + 1) % NOJME_SCALE_MODE_COUNT;
                else s->scale_mode = (s->scale_mode + dir + NOJME_SCALE_MODE_COUNT) % NOJME_SCALE_MODE_COUNT;
                break;
            case SET_FILTER:
                s->filter = (s->filter + (keys & SWK_A ? 1 : dir) + NOJME_FILTER_COUNT) % NOJME_FILTER_COUNT;
                break;
            case SET_TEXF:
                /* v36.11: Авто -> Пиксели -> Сглаживание -> Авто */
                s->tex_filter = (s->tex_filter + (keys & SWK_A ? 1 : dir) + 3) % 3;
                break;
            case SET_FLIP:
                /* v35.03: OFF -> AUTO -> ON -> OFF */
                s->flip_mode = (s->flip_mode + (keys & SWK_A ? 1 : dir) + 3) % 3;
                break;
            case SET_AUDIO:
                s->audio_enabled = !s->audio_enabled;
                break;
            case SET_LOG:
                /* v36.05: LEFT/RIGHT toggle too (was A-only — inconsistent
                 * with the SCALE/FILTER/FLIP/AUDIO/VM_SPEED rows). */
                s->logging = !s->logging;
                break;
            case SET_FCNT:
                /* v35.07: счётчик кадров (диагностика темпа игры);
                 * v36.05: LEFT/RIGHT toggle too. */
                s->frame_counter = !s->frame_counter;
                break;
            case SET_LANG:
                /* v36.25: A/LEFT/RIGHT — toggle (двухзначная строка).
                 * switch_lang_toggle() обновляет синглтон, сохраняет ini
                 * и перерисовывает ВСЕ экраны на новом языке сразу;
                 * s->lang синхронизируем, чтобы финальный «Сохранить и
                 * выйти» не откатил язык (гард v34.90 ниже). */
                s->lang = switch_lang_toggle();
                break;
            case SET_DPAD:
                /* v36.37: «Поменять местами стик и D-pad» — toggle.
                 * Выкл = стик шлёт цифры 2/4/6/8, D-pad — стрелки;
                 * Вкл = стик — стрелки, D-pad — цифры 2/4/6/8.
                 * Применяется ЖИВО (читается при каждом событии ввода). */
                s->input_swap = !s->input_swap;
                break;
            case SET_DEADZONE: {
                /* v36.37: шаг NOJME_DEADZONE_STEP (%), с заворачиванием;
                 * A/RIGHT = больше, LEFT = меньше. Читается pump-ом сразу. */
                int d = s->stick_deadzone + (keys & SWK_LEFT ? -NOJME_DEADZONE_STEP
                                                             : NOJME_DEADZONE_STEP);
                if (d < NOJME_DEADZONE_MIN)
                    d = NOJME_DEADZONE_MAX - (NOJME_DEADZONE_MAX - NOJME_DEADZONE_MIN) % NOJME_DEADZONE_STEP;
                if (d > NOJME_DEADZONE_MAX)
                    d = NOJME_DEADZONE_MIN;
                s->stick_deadzone = d;
                break;
            }
            case SET_BATTERY:
                /* [BATFIX] v36.41: Не показывать -> Значок -> Цифра -> …
                 * (A/RIGHT — вперёд, LEFT — назад). Применяется ЖИВО:
                 * оверлей в кадре игры и бейдж меню читают настройку
                 * при каждой отрисовке. */
                s->battery_display = (s->battery_display +
                                     (keys & SWK_A ? 1 : dir) +
                                     NOJME_BATT_COUNT) % NOJME_BATT_COUNT;
                break;
            case SET_VM_SPEED:
                if (keys & SWK_A) s->vm_speed = (s->vm_speed + 1) % NOJME_VM_SPEED_COUNT;
                else s->vm_speed = (s->vm_speed + dir + NOJME_VM_SPEED_COUNT) % NOJME_VM_SPEED_COUNT;
                break;
            case SET_GAMES_DIR:
            case SET_SAVE:
                if (keys & SWK_A) {
                    s->lang = switch_lang(); /* v34.90: не откатывать язык, переключённый MINUS-ом */
                    switch_settings_save(s);
                    return 1;
                }
                break;
        }
    }
    return 0;
}

/* ================= v34.94 per-game settings screen (PLUS in game) ================= */

/* In-game overlay: per-game resolution / scaling / filter. Runs on the frame
 * thread from the game pump (same contract as the pause menu): consumes its
 * own key pumps, draws into the UI canvas. Returns when the user presses B.
 * Settings persist to pergame/<jar>.ini immediately on every change
 * (resolution applies on the next launch of this game). */
/* v36.05: direction-aware value cycling for the per-game rows (user
 * request: "переключение настроек по кнопкам влево-вправо, кнопку А тоже
 * оставить"). dir=+1 = next value (was the A-button behavior), dir=-1 =
 * previous value (LEFT). Every override keeps its -1 = inherit slot and
 * wraps through it in BOTH directions. */
static void pg_cycle_value(SwitchPerGameSettings* pg, int sel, int dir,
                           const int (*res_presets)[2], int n_presets) {
    switch (sel) {
        case 0: { /* resolution: AUTO <-> preset cycle (both directions) */
            if (!pg->res_mode) {
                pg->res_mode = 1;
                int i = (dir > 0) ? 0 : n_presets - 1;
                pg->res_w = res_presets[i][0];
                pg->res_h = res_presets[i][1];
                break;
            }
            int idx = -1;
            for (int i = 0; i < n_presets; i++) {
                if (res_presets[i][0] == pg->res_w &&
                    res_presets[i][1] == pg->res_h) { idx = i; break; }
            }
            /* custom (non-preset) size: RIGHT -> first preset, LEFT -> AUTO */
            if (idx < 0) idx = (dir > 0) ? -1 : 0;
            idx += dir;
            if (idx >= n_presets || idx < 0) {
                pg->res_mode = 0; /* wrap through AUTO */
            } else {
                pg->res_w = res_presets[idx][0];
                pg->res_h = res_presets[idx][1];
            }
            break;
        }
        case 1: /* scaling override: -1 (inherit) <-> 0..COUNT-1 */
            pg->scale_mode += dir;
            if (pg->scale_mode > NOJME_SCALE_MODE_COUNT - 1) pg->scale_mode = -1;
            if (pg->scale_mode < -1) pg->scale_mode = NOJME_SCALE_MODE_COUNT - 1;
            break;
        case 2: /* filter override: -1 (inherit) <-> 0..COUNT-1 */
            pg->filter += dir;
            if (pg->filter > NOJME_FILTER_COUNT - 1) pg->filter = -1;
            if (pg->filter < -1) pg->filter = NOJME_FILTER_COUNT - 1;
            break;
        case 3: /* flip override: inherit -> off -> auto -> on -> inherit */
            pg->flip_mode += dir;
            if (pg->flip_mode > NOJME_FLIP_ON) pg->flip_mode = -1;
            if (pg->flip_mode < -1) pg->flip_mode = NOJME_FLIP_ON;
            break;
        case 4: /* vm speed override: inherit -> original -> fast -> turbo */
            pg->vm_speed += dir;
            if (pg->vm_speed > NOJME_VM_SPEED_TURBO) pg->vm_speed = -1;
            if (pg->vm_speed < -1) pg->vm_speed = NOJME_VM_SPEED_TURBO;
            break;
        case 5: /* v36.11 texture filter override: inherit -> auto -> pixelated -> smooth */
            pg->tex_filter += dir;
            if (pg->tex_filter > NOJME_TEXF_SMOOTH) pg->tex_filter = -1;
            if (pg->tex_filter < -1) pg->tex_filter = NOJME_TEXF_SMOOTH;
            break;
        case 6: /* v36.15 rotation: off -> 90 right -> 90 left -> off
                 * (наследовать нечего — глобального поворота нет) */
            pg->rotation = (pg->rotation + dir + 3) % 3;
            break;
        case 7: /* v36.37 stick/D-pad swap: inherit -> normal -> swapped -> inherit
                 * (было: раскладка D-pad inherit -> arrows -> digits) */
            pg->input_swap += dir;
            if (pg->input_swap > NOJME_SWAP_ON) pg->input_swap = -1;
            if (pg->input_swap < -1) pg->input_swap = NOJME_SWAP_ON;
            break;
    }
}

void switch_ui_pergame_screen(const char* jar_path) {
    if (WILDGUARD_SKIP(jar_path, "pgset:jar")) return; /* [WILDGUARD] v36.50 */
    SwitchPerGameSettings pg;
    switch_pergame_load(jar_path, &pg);
    int sel = 0;
    uint64_t last_draw = 0;

    /* resolution presets cycle: auto -> 240x320 -> 320x240 -> 240x240 ->
     * 176x208 -> 176x220 -> 208x208 -> 320x480 -> 640x360 -> 1280x720 ->
     * auto ...
     * v35.08: +640x360 (16:9 small) and +1280x720 (full Switch screen,
     * 16:9) - landscape 3D games look best on them (user request).
     * v36.15: +176x220 (классический Motorola/SonyEricsson candybar,
     * отсутствовал по сравнению с libretro-версией — запрос пользователя). */
    static const int res_presets[][2] = {
        {240, 320}, {320, 240}, {240, 240}, {176, 208}, {176, 220}, {208, 208},
        {320, 480}, {640, 360}, {1280, 720}
    };
    const int n_presets = (int)(sizeof(res_presets) / sizeof(res_presets[0]));

    {
        const char* sl = jar_path ? strrchr(jar_path, '/') : NULL;
        sw_trace("pgset: open %s", sl ? sl + 1 : (jar_path ? jar_path : "?"));
    }
    /* v36.19: раньше экран парковал VM сам (v34.97, begin/end); теперь он
     * вызывается ТОЛЬКО изнутри меню паузы (MINUS/PLUS), где VM УЖЕ
     * запаркована скобками паузы (jvm_frontend_pause_begin). Скобки
     * НЕ вложимы (булев CAS), поэтому здесь НИЧЕГО не паркуем:
     * виртуальное время стоит всё время, пока открыто меню паузы,
     * включая экран настроек игры. Вызов вне паузы запрещён
     * (единственный вызывающий — sdl_switch_pause_menu). */

    for (;;) {
        unsigned keys = sdl_switch_ui_pump_keys();
        if (keys & SWK_MINUS) switch_lang_toggle();
        /* v36.26 [TOUCH-UI]: тап по строке per-game настроек (двухфазно:
         * первый тап выделяет, тап по выделенной листает значение) +
         * бейдж языка = MINUS. */
        {
            int ttx = 0, tty = 0;
            if (sdl_switch_touch_tap(&ttx, &tty)) {
                if (switch_ui_badge_hit(ttx, tty, 8)) {
                    keys |= SWK_MINUS;
                } else if (tty >= LIST_Y && tty < LIST_Y + 8 * SET_ROW_H) {
                    int row = (tty - LIST_Y) / SET_ROW_H;
                    if (row >= 0 && row < 8) {
                        if (sel == row) keys |= SWK_A;
                        else            sel = row;
                    }
                }
            }
        }
        if (keys & SWK_B || keys & SWK_EXIT) break;
        /* v36.17: снимок выделения для анимации курсора */
        int prev_sel = sel;
        if (keys & SWK_UP) { sel--; if (sel < 0) sel = 7; }
        if (keys & SWK_DOWN) { sel++; if (sel > 7) sel = 0; }
        if (sel != prev_sel) cursor_anim_arm((float)prev_sel, sel, LIST_Y, SET_ROW_H);

        /* v36.05: A / RIGHT = next value, LEFT = previous value.
         * (A kept as before — RIGHT is just a duplicate of it.) */
        {
            int dir = 0;
            if (keys & SWK_A) dir = 1;
            else if (keys & SWK_RIGHT) dir = 1;
            else if (keys & SWK_LEFT) dir = -1;
            if (dir != 0) {
                pg_cycle_value(&pg, sel, dir, res_presets, n_presets);
                switch_pergame_save(jar_path, &pg);
                /* v36.11: сглаживание текстур применяется НЕМЕДЛЕННО (в
                 * отличие от разрешения/скорости VM, которым нужен
                 * следующий запуск): сэмплер читает глобальную переменную
                 * на каждую отрисовку, так что перерисовка после B уже с
                 * новым фильтром. Наследование разрешается здесь же.
                 * Слабый символ: хост-сборки без render.c (например
                 * test_switch_input) не линкуются с M3G-сэмплером — там
                 * адрес NULL и запись пропускается; в реальной сборке
                 * render.c определяет переменную сильно. */
                if (sel == 5) {
                    extern int g_m3g_texture_filter_override __attribute__((weak));
                    if (&g_m3g_texture_filter_override) {
                        const SwitchSettings* gst = switch_settings_get();
                        g_m3g_texture_filter_override =
                            (pg.tex_filter >= 0) ? pg.tex_filter : gst->tex_filter;
                    }
                }
                /* v36.15: масштаб per-game тоже применяется ЖИВО (present
                 * читает g_pg_scale каждый кадр) — тот же weak-паттерн,
                 * что и у текстур выше. */
                if (sel == 1) {
                    extern int g_pg_scale __attribute__((weak));
                    if (&g_pg_scale) g_pg_scale = pg.scale_mode;
                }
                /* v36.16: ФЛИП 3D теперь тоже ЖИВОЙ. present пересчитывает
                 * эффективный режим каждый кадр из g_pg_flip (per-game
                 * override) и глобальной настройки, так что достаточно
                 * обновить g_pg_flip здесь — следующий же отрисованный
                 * кадр игры уже с новым режимом (перерисовки не нужны и
                 * при inherit: глобальное значение подхватится само).
                 * Слабый символ — как у g_pg_scale выше (хост-сборки без
                 * sdl_graphics.c не линкуются с ним; там NULL и пропуск). */
                if (sel == 3) {
                    extern int g_pg_flip __attribute__((weak));
                    if (&g_pg_flip) g_pg_flip = pg.flip_mode;
                }
                /* v36.27 [HOT-ROTATION]: строка «Поворот экрана» применяется
                 * ЖИВО — больше НЕ требует перезапуска мидлета (отчёт:
                 * «переворот применяется только после перезагрузки»).
                 * Слабый символ: реальная сборка переставляет staging/
                 * текстуры и пере-поворачивает последний settled-кадр
                 * (sdl_switch_reapply_presentation, sdl_graphics.c);
                 * хост-сборки без sdl_graphics.c — адрес NULL, пропуск.
                 * g_pg_rotation обновляется так же слабо, как scale/flip
                 * выше: в хост-сборках переменной нет. */
                if (sel == 6) {
                    extern int g_pg_rotation __attribute__((weak));
                    if (&g_pg_rotation) g_pg_rotation = pg.rotation;
                    extern void sdl_switch_reapply_presentation(void)
                        __attribute__((weak));
                    if (&sdl_switch_reapply_presentation)
                        sdl_switch_reapply_presentation();
                }
                /* v36.37: стик/D-pad swap применяется ЖИВО — он читается
                 * при каждом событии стика/D-pad (sdl_graphics), так что
                 * следующий же нажатый стрелка/цифра уже с новым кодом.
                 * Слабый символ — как у scale/flip/rotation выше. */
                if (sel == 7) {
                    extern int g_pg_input_swap __attribute__((weak));
                    if (&g_pg_input_swap) g_pg_input_swap = pg.input_swap;
                }
                /* Остальные строки (разрешение, скорость ВМ) печатаются в
                 * геометрию/бюджет сессии и требуют следующего запуска игры.
                 * (Фильтр/масштаб/флип/поворот — живые, см. выше.) */
            }
        }

        uint64_t now = sdl_switch_ui_ticks_ms();
        if (now - last_draw >= 16) {
            last_draw = now;
            uint32_t* cv = sdl_switch_ui_canvas();
            if (cv) {
                int cw = SWITCH_UI_WIDTH;
                /* v36.17: ПРОЗРАЧНЫЙ ФОН — за настройками виден последний кадр
                 * игры (тот же источник/поворот/масштаб, что у обычного
                 * present). Поверх — полупрозрачное затемнение для читаемости;
                 * при недоступности кадра (хост-тесты без бэкенда) — прежний
                 * непрозрачный фон. Смена строки «Масштаб» в этом меню живо
                 * меняет и фон (тот же g_pg_scale, что и у present). */
                int bg_ok = 0;
                {
                    extern int sdl_switch_ui_game_frame_bg(uint32_t*, int, int)
                        __attribute__((weak));
                    if (&sdl_switch_ui_game_frame_bg)
                        bg_ok = sdl_switch_ui_game_frame_bg(cv, cw, SWITCH_UI_HEIGHT);
                }
                if (bg_ok)
                    blend_rect(cv, cw, SWITCH_UI_HEIGHT, 0, 0, cw, SWITCH_UI_HEIGHT,
                               0x10131Au, 0xA0); /* 63% тёмного поверх кадра */
                else
                    fill_rect(cv, cw, SWITCH_UI_HEIGHT, 0, 0, cw, SWITCH_UI_HEIGHT, COL_BG);
                draw_header(cv, cw, switch_text(ST_PG_TITLE), switch_text(ST_PG_SAVED));
                char rv[64];
                if (pg.res_mode) snprintf(rv, sizeof(rv), "%dx%d", pg.res_w, pg.res_h);
                else snprintf(rv, sizeof(rv), "%s", switch_text(ST_RES_AUTO));
                const char* sv = (pg.scale_mode < 0)
                    ? switch_text(ST_SCALE_INHERIT)
                    : switch_scale_mode_name(pg.scale_mode);
                const char* fv = (pg.filter < 0)
                    ? switch_text(ST_FILTER_INHERIT)
                    : switch_filter_name(pg.filter);
                const char* lv = (pg.flip_mode < 0)
                    ? switch_text(ST_SCALE_INHERIT)
                    : switch_flip_mode_name(pg.flip_mode);
                const char* vv = (pg.vm_speed < 0)
                    ? switch_text(ST_SCALE_INHERIT)
                    : switch_vm_speed_name(pg.vm_speed);
                const char* tv = (pg.tex_filter < 0)
                    ? switch_text(ST_SCALE_INHERIT)
                    : switch_texf_name(pg.tex_filter);
                const char* rotv = pg.rotation == 1 ? switch_text(ST_ROT_RIGHT)
                                : pg.rotation == 2 ? switch_text(ST_ROT_LEFT)
                                                   : switch_text(ST_ROT_OFF);
                const char* dpv = pg.input_swap < 0
                    ? switch_text(ST_SCALE_INHERIT)
                    : switch_swap_name(pg.input_swap); /* v36.37 */
                const char* names[8] = {
                    switch_text(ST_PG_RES), switch_text(ST_PG_SCALE),
                    switch_text(ST_PG_FILTER), switch_text(ST_PG_FLIP),
                    switch_text(ST_PG_VMSPEED), switch_text(ST_PG_TEXF),
                    switch_text(ST_PG_ROT), /* v36.15 */
                    switch_text(ST_PG_DPAD) /* v36.37: «Стик и D-pad» */
                };
                const char* vals[8] = { rv, sv, fv, lv, vv, tv, rotv, dpv };
                /* v36.17: строки поверх живого кадра — с тенью; полоса
                 * выделения может анимироваться (cursor_anim) */
                g_row_text_shadow = 1;
                s_hl_y_override = cursor_anim_highlight_top(LIST_Y, SET_ROW_H, sel);
                draw_hl_bar_underlay(cv, cw, SET_ROW_H); /* полоса под текстом */
                int y = LIST_Y;
                for (int i = 0; i < 8; i++)
                    y = draw_row(cv, cw, y, names[i], vals[i], sel == i, i, 8, SET_ROW_H);
                g_row_text_shadow = 0;
                s_hl_y_override = -1;
                /* v36.15: всплывающее описание выделенного пункта + пометка
                 * о моменте применения (перезапуск игры для большинства
                 * строк, «сразу» для масштаба и 3D-текстур) */
                {
                    static const int desc_ids[8] = {
                        ST_HINT_PG_RES, ST_HINT_PG_SCALE, ST_HINT_PG_FILTER,
                        ST_HINT_PG_FLIP, ST_HINT_PG_VMSPEED, ST_HINT_PG_TEXF,
                        ST_HINT_PG_ROT,
                        ST_HINT_PG_DPAD /* v36.36 */
                    };
                    /* 1 = применяется сразу при закрытии меню, 0 = нужен
                     * следующий запуск игры (v36.16: флип стал живым;
                     * v36.36: раскладка D-pad тоже живая) */
                    static const int live_row[8] = { 0, 1, 0, 1, 0, 1, 0, 1 };
                    draw_hint_box(cv, cw, switch_text(desc_ids[sel]),
                                  switch_text(live_row[sel] ? ST_HINT_LIVE
                                                            : ST_HINT_RESTART));
                }
                draw_footer(cv, cw, switch_text(ST_PG_FOOTER));
                /* [BATFIX] v36.41: бейдж заряда и здесь — экран настроек
                 * игры рисуется поверх живого кадра, синглтон-режим (эта
                 * настройка глобальная, per-game override для неё нет) */
                switch_ui_batt_badge_draw(cv, cw, SWITCH_UI_HEIGHT,
                                          switch_settings_get()->battery_display);
                sdl_switch_ui_present();
            }
        } else {
            sdl_switch_ui_wait(4);
        }
    }
    /* v36.19: снятие паузы убрано вместе с self-парковкой — паузу
     * держит и снимает ВЫЗЫВАЮЩЕЕ меню паузы (begin/end вокруг всего
     * хаба). Возврат из этого экрана — обратно в меню паузы. */
    sw_trace("pgset: close");
}

/* ================= Main menu ================= */

/* v36.11: пункт «Выход» УДАЛЁН (запрос пользователя: закрытие из меню
 * всё равно приводит к вылету — HOME закрывает игру корректно силами
 * системы). B на главном экране больше не закрывает приложение тоже:
 * единственный способ выйти из NOJME — кнопка HOME приставки.
 * SWK_EXIT (script/SDL_QUIT) сохранён для E2E-харнесса.
 * v36.14: добавлен пункт «Управление» — назначение кнопок консоли. */
typedef enum {
    MM_OPEN = 0,
    MM_SETTINGS,
    MM_KEYS,
    MM_COUNT
} MainItem;

static void main_draw(uint32_t* cv, int sel) {
    int cw = SWITCH_UI_WIDTH;
    fill_rect(cv, cw, SWITCH_UI_HEIGHT, 0, 0, cw, HEADER_H, COL_HEADER);
    hline(cv, cw, SWITCH_UI_HEIGHT, 0, cw, HEADER_H, COL_LINE);
    draw_lang_badge(cv, cw); /* v34.90: язык + подсказка MINUS в левом верхнем углу */
    draw_text(cv, cw, SWITCH_UI_HEIGHT, 32, 46, switch_text(ST_APP_TITLE), COL_TEXT, 3);
    {
        extern const char* nojme_switch_version_string(void);
        const char* v = nojme_switch_version_string();
        draw_text(cv, cw, SWITCH_UI_HEIGHT, cw - 32 - switch_font_text_width(v, 2), 54, v, COL_DIM, 2);
    }
    int y = LIST_Y + 40;
    const char* names[MM_COUNT] = {
        switch_text(ST_MENU_OPEN), switch_text(ST_MENU_SETTINGS),
        switch_text(ST_MENU_KEYS) /* v36.14 */
    };
    draw_hl_bar_underlay(cv, cw, ROW_H); /* v36.17: полоса под текстом */
    for (int i = 0; i < MM_COUNT; i++) {
        y = draw_row(cv, cw, y, names[i], NULL, i == sel, i, MM_COUNT, ROW_H);
    }
    /* v36.17: надпись «MIDP2 мобильные игры на Nintendo Switch» (ST_MENU_TAGLINE)
     * убрана с главного экрана по запросу пользователя. */
    draw_footer(cv, cw, switch_text(ST_FOOTER_MAIN));
}

/* ================= v36.14: экран «Назначенные кнопки» =================
 * Статичная справка из ЖИВОГО маппинга консоли: кнопка -> действие.
 * Игра-часть повторяет key_map[]/switch_joy_button_to_keycode() из
 * sdl_graphics.c; меню-часть — sdl_switch_ui_pump_keys(). При изменении
 * маппинга обновить и эти строки (см. пометки v36.14 там и здесь).
 * v36.17: шрифт x2 и ДВЕ КОЛОНКИ («В игре» слева, «В меню» справа) —
 * текст scale 1 был слишком мелким в портативном режиме (запрос). */
static void keys_draw(uint32_t* cv) {
    int cw = SWITCH_UI_WIDTH;
    draw_header(cv, cw, switch_text(ST_KEYS_TITLE), NULL);
    const int row_h = 44;      /* строки scale 2 (глиф 32 + воздух) */
    const int y0 = LIST_Y;
    /* колонки: левая "В игре", правая "В меню"; значения прижаты вправо
     * к своей колонке — как в остальных списках (draw_row) */
    const int l_name_x = 40, l_val_right = 620;
    const int r_name_x = 720, r_val_right = cw - 48;

    static const struct { int name_id; int val_id; } game_rows[] = {
        { ST_KEYS_DPAD,  ST_KEYS_DPAD_V  },
        { ST_KEYS_A,     ST_KEYS_A_V     },
        { ST_KEYS_X,     ST_KEYS_X_V     },
        { ST_KEYS_B,     ST_KEYS_B_V     },
        { ST_KEYS_Y,     ST_KEYS_Y_V     },
        { ST_KEYS_L,     ST_KEYS_L_V     },
        { ST_KEYS_R,     ST_KEYS_R_V     },
        /* [YSFIX2] v36.40: L3/R3 смержены в строки X и B (имена «X / ZL /
         * L3» и «B / R3»); освобождённое место заняла пометка про
         * одиночный Joy-Con — на нём стики L3/R3 физически недоступны,
         * а кластер кнопок отдаётся как ВИРТУАЛЬНЫЕ A/B/X/Y (ремап
         * pad_mapping_left/right_joy в switch-SDL2). */
        { ST_KEYS_ZR,    ST_KEYS_ZR_V    }, /* v36.36: R2 = цифра 0 */
        { ST_KEYS_TOUCH, ST_KEYS_TOUCH_V },
        { ST_KEYS_JC,    ST_KEYS_JC_V    }, /* [YSFIX2] v36.40 */
        { ST_KEYS_MINUS, ST_KEYS_MINUS_V },
        { ST_KEYS_PLUS,  ST_KEYS_PLUS_V  },
    };
    static const struct { int name_id; int val_id; } menu_rows[] = {
        { ST_KEYS_AM,     ST_KEYS_AM_V     },
        { ST_KEYS_BM,     ST_KEYS_BM_V     },
        { ST_KEYS_MINUSM, ST_KEYS_MINUSM_V },
        { ST_KEYS_HOME,   ST_KEYS_HOME_V   },
    };

    /* вертикальный разделитель колонок ([YSFIX2] v36.40: 13 строк =
     * заголовок + 12 строк game_rows после мержа L3/R3 в X/B) */
    for (int yy = y0 - 8; yy < y0 + 13 * row_h; yy += 2)
        hline(cv, cw, SWITCH_UI_HEIGHT, 670, 670, yy, COL_LINE);

    int y = y0;
    draw_text(cv, cw, SWITCH_UI_HEIGHT, l_name_x, y, switch_text(ST_KEYS_GAME_HDR), COL_ACCENT, 2);
    y += row_h;
    for (int i = 0; i < (int)(sizeof(game_rows) / sizeof(game_rows[0])); i++) {
        draw_text(cv, cw, SWITCH_UI_HEIGHT, l_name_x, y,
                  switch_text(game_rows[i].name_id), COL_TEXT, 2);
        const char* val = switch_text(game_rows[i].val_id);
        draw_text(cv, cw, SWITCH_UI_HEIGHT, l_val_right - switch_font_text_width(val, 2), y,
                  val, COL_DIM, 2);
        y += row_h;
    }

    y = y0;
    draw_text(cv, cw, SWITCH_UI_HEIGHT, r_name_x, y, switch_text(ST_KEYS_MENU_HDR), COL_ACCENT, 2);
    y += row_h;
    for (int i = 0; i < (int)(sizeof(menu_rows) / sizeof(menu_rows[0])); i++) {
        draw_text(cv, cw, SWITCH_UI_HEIGHT, r_name_x, y,
                  switch_text(menu_rows[i].name_id), COL_TEXT, 2);
        const char* val = switch_text(menu_rows[i].val_id);
        draw_text(cv, cw, SWITCH_UI_HEIGHT, r_val_right - switch_font_text_width(val, 2), y,
                  val, COL_DIM, 2);
        y += row_h;
    }

    draw_footer(cv, cw, switch_text(ST_KEYS_FOOTER));
}

/* ================= Top-level menu session ================= */

int switch_ui_pick_game(char* out_path, size_t path_cap) {

    SwitchSettings st = *switch_settings_get();
    MenuScreen screen = MMAIN_MAIN;
    int sel = 0;
    int set_sel = 0;
    int set_scroll = 0; /* v36.37: верхняя строка видимого окна настроек */
    BrowserState br;
    memset(&br, 0, sizeof(br));
    browse_root(br.cwd, sizeof(br.cwd));
    nojme_pg_watch("ui:cwd", br.cwd, sizeof(br.cwd)); /* [PATHGUARD] v36.51 */

    int result = 0; /* 0 = выход, 1 = игра выбрана */
    uint64_t last_draw = 0;
    unsigned held_last = 0;
    uint64_t repeat_t0 = 0;
    /* v35.11 B-guard history: B на главном экране выходил из приложения
 * (с 500 мс защитой от дожатия) — в v36.11 УДАЛЕН вместе с пунктом
 * «Выход»: закрытие силами NOJME приводит к вылету, HOME закрывает
 * корректно. Случайные дожатия B теперь просто ничего не делают. */

    ui_log("menu session start\n");
    sw_trace("menu: session start"); /* v34.91 */
    /* v34.91 autotest session semantics:
     * DEFAULT (no NOJME_SWITCH_AUTOTEST_SESSIONS) — v34.90 behavior: the
     * script position SURVIVES sessions. A session that ends mid-script
     * (e.g. after the A that selects the jar) leaves the next token armed —
     * the E2E's trailing "B:6000" fires at the next session's first pump
     * and exits the app deterministically.
     * SOAK MODE (NOJME_SWITCH_AUTOTEST_SESSIONS=<n>) — the script RESTARTS
     * from token 0 for the first <n> sessions (drives many game sessions in
     * one process — the multi-session UAF regression harness), then goes
     * idle. */
    {
        const char* cap = getenv("NOJME_SWITCH_AUTOTEST_SESSIONS");
        g_at_session_cap = (cap && cap[0]) ? atoi(cap) : 0;
    }
    /* v36.12 SOAK-EXIT: with the cap reached the menu used to idle forever,
     * so a soak could only end via SIGTERM — valgrind then reports nothing.
     * NOJME_SWITCH_AUTOTEST_EXIT_MS=<ms> breaks the menu loop cleanly that
     * many ms after the cap disables the driver (same path as SWK_EXIT:
     * "menu exit — shutting down"). Sandbox-only: production never sets it. */
    {
        const char* ex = getenv("NOJME_SWITCH_AUTOTEST_EXIT_MS");
        s_at_exit_after_ms = (ex && ex[0]) ? (uint64_t)atoi(ex) : 0;
        if (s_at_exit_after_ms && g_at_session_cap > 0) {
            s_at_exit_deadline = 0; /* armed when the cap is reached */
        } else {
            s_at_exit_after_ms = 0;
        }
    }
    if (g_at_session_cap > 0) {
        if (++g_at_sessions > g_at_session_cap) {
            g_at_enabled = 0; /* cap reached: no more synthetic keys */
            if (s_at_exit_after_ms) {
                s_at_exit_deadline = sdl_switch_ui_ticks_ms() + s_at_exit_after_ms;
                ui_log("autotest cap reached — clean exit in %llu ms\n",
                       (unsigned long long)s_at_exit_after_ms);
            }
        } else {
            g_at_pos = 0;
            g_at_next_ms = 0;
        }
    }

    for (;;) {
        unsigned keys = sdl_switch_ui_pump_keys();
        unsigned held = sdl_switch_ui_held_keys();
        keys |= at_keys();

        /* [PATHGUARD] v36.51: раз в 2 с — скан всех зарегистрированных
         * slab-путей: ядовитые слоты (байты пути в позиции указателя),
         * появившиеся ВНЕ файловых операций, репортятся здесь. */
        {
            static uint64_t s_pg_next = 0;
            uint64_t now = sdl_switch_ui_ticks_ms();
            if (now >= s_pg_next) {
                s_pg_next = now + 2000;
                nojme_pg_scan_all("menu-periodic");
            }
        }

        /* v36.26 [TOUCH-UI]: тап по любому экрану меню. Бейдж языка
         * (верхний левый угол) работает как MINUS во ВСЕХ экранах;
         * остальное разбирает текущий экран. Модель двухфазная — как на
         * реальных тач-списках: первый тап выделяет строку, тап по
         * УЖЕ выделенной — активирует (открывает папку/запускает jar/
         * меняет значение настройки). */
        int ttx = 0, tty = 0;
        int tapped = sdl_switch_touch_tap(&ttx, &tty);
        if (tapped && switch_ui_badge_hit(ttx, tty, 8)) {
            keys |= SWK_MINUS;
            tapped = 0;
        }

        /* v34.90: MINUS — переключение языка в любом экране меню. Текущий
         * язык и подсказка о кнопке всегда видны в левом верхнем углу
         * (draw_lang_badge); следующая отрисовка перерисует всё уже на
         * новом языке. */
        if (keys & SWK_MINUS) {
            switch_lang_toggle();
        }

        /* hold-to-repeat в браузере (долгие списки) */
        if (screen == MMAIN_BROWSER) {
            unsigned newly_held = held & ~held_last;
            if (newly_held & (SWK_UP | SWK_DOWN)) repeat_t0 = sdl_switch_ui_ticks_ms();
            if (held & (SWK_UP | SWK_DOWN) && sdl_switch_ui_ticks_ms() - repeat_t0 > 450) {
                static uint64_t last_rep = 0;
                if (sdl_switch_ui_ticks_ms() - last_rep > 110) {
                    keys |= (held & SWK_UP) ? SWK_UP : 0;
                    keys |= (held & SWK_DOWN) ? SWK_DOWN : 0;
                    last_rep = sdl_switch_ui_ticks_ms();
                }
            }
        }
        held_last = held;

        if (keys & SWK_EXIT) break;
        /* v36.12 SOAK-EXIT: cap reached and the grace period elapsed — the
         * same clean shutdown SWK_EXIT uses (valgrind/soak determinism).
         * NOTE: a synthetic SWK_EXIT via keys|=/continue would be LOST to
         * the next pump_keys() overwrite — break out directly. */
        if (s_at_exit_deadline && sdl_switch_ui_ticks_ms() >= s_at_exit_deadline) {
            sw_trace("autotest: soak exit deadline reached — clean shutdown");
            break;
        }

        /* v36.17: снимки состояния для анимации курсора (ДО обработки ввода) */
        int prev_screen = (int)screen;
        int prev_main_sel = sel, prev_set_sel = set_sel;
        int prev_b_sel = br.sel, prev_b_scroll = br.scroll;

        switch (screen) {
            case MMAIN_MAIN:
                /* v36.26: тап по строке главного меню (двухфазно) */
                if (tapped && tty >= LIST_Y + 40 &&
                    tty < LIST_Y + 40 + MM_COUNT * ROW_H) {
                    int row = (tty - (LIST_Y + 40)) / ROW_H;
                    if (row >= 0 && row < MM_COUNT) {
                        if (sel == row) keys |= SWK_A;
                        else            sel = row;
                    }
                    tapped = 0;
                }
                if (keys & SWK_UP) { sel--; if (sel < 0) sel = MM_COUNT - 1; }
                if (keys & SWK_DOWN) { sel++; if (sel >= MM_COUNT) sel = 0; }
                if (keys & (SWK_A | SWK_RIGHT)) {
                    if (sel == MM_OPEN) {
                        sw_trace("browse: root"); /* v34.91: the root probe itself is filesystem access */
                        browse_root(br.cwd, sizeof(br.cwd));
                        sw_trace("scan: %s", br.cwd);
                        sw_trace_flush(); /* v34.91: present BEFORE the scan — a hang leaves the culprit on screen */
                        browse_load(&br.list, br.cwd);
                        br.sel = 0; br.scroll = 0; br.return_to_settings = 0;
                        ui_log("browser open: %s (%d entries)\n", br.cwd, br.list.count);
                        screen = MMAIN_BROWSER;
                    } else if (sel == MM_KEYS) {
                        /* v36.14: справка о назначенных кнопках консоли */
                        sw_trace("keys: screen");
                        screen = MMAIN_KEYS;
                    } else {
                        set_sel = 0;
                        set_scroll = 0; /* v36.37: окно скролла с начала */
                        sw_trace("settings: screen"); /* v34.91: entering settings is pure memory — if THIS is the last line, the freeze is here */
                        screen = MMAIN_SETTINGS;
                    }
                }
                /* v36.11: B на главном экране — НЕТ действия (приложение
                 * закрывается только кнопкой HOME приставки). */
                break;

            case MMAIN_BROWSER: {
                /* v36.26: drag-прокрутка длинных списков — палец тянет
                 * выделение (окно следует за ним в browser_handle) */
                {
                    int stx = 0, sty = 0, sdown = 0, smoved = 0;
                    sdl_switch_touch_state(&stx, &sty, &sdown, &smoved);
                    static int drag_y = -1, drag_on = 0;
                    if (sdown && smoved && br.list.count > 0) {
                        if (!drag_on) { drag_on = 1; drag_y = sty; }
                        else {
                            int delta = (drag_y - sty) / ROW_H;
                            if (delta) {
                                drag_y -= delta * ROW_H;
                                br.sel += delta;
                                if (br.sel < 0) { br.sel = 0; drag_y = sty; }
                                if (br.sel >= br.list.count) {
                                    br.sel = br.list.count - 1; drag_y = sty;
                                }
                            }
                        }
                    } else if (!sdown) {
                        drag_on = 0;
                    }
                }
                /* v36.26: тап по строке браузера (двухфазно) */
                if (tapped && br.list.count > 0 &&
                    tty >= LIST_Y && tty < LIST_Y + VISIBLE_ROWS * ROW_H) {
                    int idx = br.scroll + (tty - LIST_Y) / ROW_H;
                    if (idx >= 0 && idx < br.list.count) {
                        if (br.sel == idx) keys |= SWK_A;
                        else               br.sel = idx;
                    }
                    tapped = 0;
                }
                int r = browser_handle(&br, keys, out_path, path_cap);
                if (r == 1) { result = 1; goto done; }
                if (r == 2) screen = MMAIN_MAIN;
                break;
            }

            case MMAIN_KEYS:
                /* v36.14: статичный экран-справка — любой из A/B возвращает
                 * в главное меню (SWK_EXIT остаётся глобальным выше).
                 * v36.26: тап по любому месту экрана — тоже назад. */
                if (tapped) { tapped = 0; screen = MMAIN_MAIN; }
                if (keys & (SWK_B | SWK_A)) screen = MMAIN_MAIN;
                break;

            case MMAIN_SETTINGS: {
                /* v36.26: тап по строке настроек (двухфазно: активация = A;
                 * A на «Папке игр» открывает выбор, на «Сохранить» —
                 * сохраняет и выходит, на остальных листает значение)
                 * v36.37: тап считает окно скролла (последняя видимая
                 * строка может быть частично «за» полосой справки). */
                if (tapped && tty >= LIST_Y &&
                    tty < LIST_Y + SET_VISIBLE * SET_ROW_H) {
                    int row = set_scroll + (tty - LIST_Y) / SET_ROW_H;
                    if (row >= set_scroll && row < SET_COUNT) {
                        if (set_sel == row) keys |= SWK_A;
                        else                set_sel = row;
                    }
                    tapped = 0;
                }
                if (keys & SWK_A && set_sel == SET_GAMES_DIR) {
                    /* выбор папки игр через браузер */
                    sw_trace("browse: root"); /* v34.91 */
                    browse_root(br.cwd, sizeof(br.cwd));
                    sw_trace("scan: %s", br.cwd);
                    sw_trace_flush(); /* v34.91 */
                    browse_load(&br.list, br.cwd);
                    br.sel = 0; br.scroll = 0; br.return_to_settings = 1;
                    screen = MMAIN_DIRPICKER;
                    break;
                }
                {
                    int r = settings_handle(&st, &set_sel, keys);
                    if (r == 1) {
                        /* выход ЧЕРЕЗ «Сохранить и выйти»: файл уже записан
                         * внутри settings_handle (switch_settings_save
                         * обновляет и синглтон). Подтверждаем синглтон,
                         * применяем флаг лога немедленно. */
                        st.lang = switch_lang(); /* v34.90: MINUS-переключение живёт в синглтоне */
                        switch_settings_set(&st);
                        /* v34.92: лог-переключатель применяется МГНОВЕННО; сама
                         * строка события всегда попадает в лог (force). */
                        {
                            extern void sw_logging_set(int on);
                            extern const char* j2me_core_build_id(void);
                            sw_logging_set(st.logging);
                            /* [YSFIX2] v36.40: build id в строке события —
                             * баннер «nojme ... trace start» к моменту
                             * включения лога уже съеден тихими вызовами
                             * sw_trace() при выключенном логе (полевые логи
                             * его не показывают вообще), а эта строка
                             * гарантированно первая в каждом логе. */
                            sw_trace_force("settings: logging %s (build %s)",
                                           st.logging ? "enabled" : "disabled",
                                           j2me_core_build_id());
                        }
                        screen = MMAIN_MAIN;
                    } else if (r == 2) {
                        /* [BATFIX] v36.41 [SET-EXIT]: выход БЕЗ сохранения —
                         * рабочая копия st ОТКАТЫВАЕТСЯ к сохранённым
                         * значениям, синглтон НЕ трогаем (применения к
                         * текущей сессии больше нет). Язык — исключение
                         * (v34.90/v36.25): MINUS/A-переключение живёт в
                         * синглтоне и пишется в ini сразу само, откатывать
                         * его нельзя — потому читаем lang из синглтона,
                         * а не из отброшенной копии. Флаг лога тоже не
                         * трогаем: он применяется только при выходе с
                         * сохранением, т.е. остаётся как до входа. */
                        st = *switch_settings_get();
                        ui_log("settings: exit WITHOUT save — reverted to "
                               "stored values\n");
                        sw_trace("settings: exit without save (reverted)");
                        screen = MMAIN_MAIN;
                    }
                }
                /* v36.37: держим выделение внутри видимого окна списка
                 * (окно 10 строк над полосой справки; скролл-прыжок
                 * гасит анимацию курсора, как в браузере). */
                {
                    int ns = set_scroll;
                    if (set_sel < set_scroll) ns = set_sel;
                    if (set_sel >= set_scroll + SET_VISIBLE)
                        ns = set_sel - SET_VISIBLE + 1;
                    if (ns != set_scroll) {
                        set_scroll = ns;
                        cursor_anim_cancel();
                    }
                }
                break;
            }

            case MMAIN_DIRPICKER: {
                /* Выбор «Папки игр»: список = [Эта папка] + подкаталоги.
                 * A на псевдо-строке подтверждает текущую папку, A на
                 * каталоге входит в него, B — на уровень выше (из корня —
                 * назад в настройки без изменения). */
                int count = br.list.count + 1; /* + «[Эта папка]» */
                /* v36.26: тап: слот 0 = «Эта папка», слот s>=1 = запись
                 * окна (scroll + s - 1) -> sel = scroll + s */
                if (tapped && tty >= LIST_Y && tty < LIST_Y + VISIBLE_ROWS * ROW_H) {
                    int slot = (tty - LIST_Y) / ROW_H;
                    int idx = (slot == 0) ? 0 : br.scroll + slot;
                    if (idx >= 0 && idx < count) {
                        if (br.sel == idx) keys |= SWK_A;
                        else               br.sel = idx;
                    }
                    tapped = 0;
                }
                if (keys & SWK_UP) { br.sel--; if (br.sel < 0) br.sel = count - 1; }
                if (keys & SWK_DOWN) { br.sel++; if (br.sel >= count) br.sel = 0; }
                if (br.sel < br.scroll) br.scroll = br.sel;
                if (br.sel >= br.scroll + VISIBLE_ROWS) br.scroll = br.sel - VISIBLE_ROWS + 1;

                if (keys & SWK_B) {
                    if (strcmp(br.cwd, "/") == 0 || strcmp(br.cwd, ".") == 0) {
                        screen = MMAIN_SETTINGS;
                    } else {
                        path_parent(br.cwd);
                        browse_load(&br.list, br.cwd);
                        br.sel = 0; br.scroll = 0;
                    }
                    break;
                }
                if (keys & SWK_A) {
                    if (br.sel == 0) {
                        /* v34.87: bound the source explicitly (cwd is 1024,
                         * games_dir 512) - silent truncation is intended
                         * here, and %.511s keeps -Wformat-truncation quiet */
                        snprintf(st.games_dir, sizeof(st.games_dir), "%.511s", br.cwd);
                        st.lang = switch_lang(); /* v34.90 */
                        switch_settings_set(&st);
                        switch_settings_save(&st);
                        ui_log("games_dir set to %s\n", br.cwd);
                        screen = MMAIN_SETTINGS;
                    } else if (br.list.count > 0) {
                        const BrowseEntry* e = &br.list.items[br.sel - 1];
                        if (e->is_dir) {
                            path_join(br.cwd, sizeof(br.cwd), br.cwd, e->name);
                            browse_load(&br.list, br.cwd);
                            br.sel = 0; br.scroll = 0;
                        }
                    }
                }
                break;
            }
        }

        /* v36.17: заведение/гашение анимации курсора по факту смены выделения */
        if ((int)screen != prev_screen) {
            cursor_anim_cancel();
        } else {
            switch (screen) {
                case MMAIN_MAIN:
                    if (sel != prev_main_sel)
                        cursor_anim_arm((float)prev_main_sel, sel, LIST_Y + 40, ROW_H);
                    break;
                case MMAIN_SETTINGS:
                    if (set_sel != prev_set_sel) {
                        /* v36.37: индексы анимации — В КООРДИНАТАХ ОКНА
                         * (скролл-прыжок уже погасил анимацию выше) */
                        cursor_anim_arm((float)(prev_set_sel - set_scroll),
                                        set_sel - set_scroll, LIST_Y, SET_ROW_H);
                    }
                    break;
                case MMAIN_BROWSER:
                    if (br.sel != prev_b_sel) {
                        /* скролл окна означает прыжок за край — анимацию гасим */
                        if (br.scroll != prev_b_scroll) cursor_anim_cancel();
                        else cursor_anim_arm((float)(prev_b_sel - prev_b_scroll),
                                             br.sel - br.scroll, LIST_Y, ROW_H);
                    }
                    break;
                case MMAIN_DIRPICKER:
                    if (br.sel != prev_b_sel) {
                        if (br.scroll != prev_b_scroll) cursor_anim_cancel();
                        else cursor_anim_arm((float)(prev_b_sel - prev_b_scroll),
                                             br.sel - br.scroll, LIST_Y, ROW_H);
                    }
                    break;
                case MMAIN_KEYS:
                    break; /* статичный экран — выделения нет */
            }
        }

        /* ---- render (trottle to ~60 fps) ---- */
        uint64_t now = sdl_switch_ui_ticks_ms();
        if (now - last_draw >= 16) {
            last_draw = now;
            uint32_t* cv = sdl_switch_ui_canvas();
            if (WILDGUARD_SKIP(cv, "ui:canvas")) cv = NULL; /* [WILDGUARD] v36.50 */
            if (cv) {
                fill_rect(cv, SWITCH_UI_WIDTH, SWITCH_UI_HEIGHT, 0, 0,
                          SWITCH_UI_WIDTH, SWITCH_UI_HEIGHT, COL_BG);
                /* v36.17: y полосы выделения для текущего экрана (анимация) */
                s_hl_y_override = -1;
                switch (screen) {
                    case MMAIN_MAIN:
                        s_hl_y_override = cursor_anim_highlight_top(LIST_Y + 40, ROW_H, sel);
                        main_draw(cv, sel);
                        break;
                    case MMAIN_BROWSER:
                        s_hl_y_override = cursor_anim_highlight_top(LIST_Y, ROW_H,
                                                                    br.sel - br.scroll);
                        browser_draw(cv, &br);
                        break;
                    case MMAIN_SETTINGS:
                        s_hl_y_override = cursor_anim_highlight_top(LIST_Y, SET_ROW_H,
                                                                    set_sel - set_scroll);
                        settings_draw(cv, &st, set_sel, set_scroll); /* v36.37: окно */
                        break;
                    case MMAIN_KEYS:     keys_draw(cv); break; /* v36.14 */
                    case MMAIN_DIRPICKER: {
                        /* своя отрисовка: [Эта папка] + подкаталоги */
                        s_hl_y_override = cursor_anim_highlight_top(LIST_Y, ROW_H,
                                                                    br.sel - br.scroll);
                        draw_header(cv, SWITCH_UI_WIDTH, switch_text(ST_DIRPICKER_TITLE), br.cwd);
                        int y = LIST_Y;
                        draw_hl_bar_underlay(cv, SWITCH_UI_WIDTH, ROW_H); /* v36.17 */
                        y = draw_row(cv, SWITCH_UI_WIDTH, y, switch_text(ST_DIRPICKER_THIS),
                                     switch_text(ST_DIRPICKER_MAKE), br.sel == 0, 0, br.list.count + 1, ROW_H);
                        int from = br.scroll; /* окно по ВИДИМЫМ строкам */
                        for (int i = from; i < br.list.count && y < LIST_Y + VISIBLE_ROWS * ROW_H; i++) {
                            const BrowseEntry* e = &br.list.items[i];
                            char label[320];
                            snprintf(label, sizeof(label), "[%s]", e->name);
                            char shown[320];
                            utf8_trunc(label, shown, sizeof(shown), 52);
                            y = draw_row(cv, SWITCH_UI_WIDTH, y, shown, switch_text(ST_BROWSER_DIR),
                                         br.sel == i + 1, i + 1, br.list.count + 1, ROW_H);
                        }
                        draw_footer(cv, SWITCH_UI_WIDTH, switch_text(ST_FOOTER_DIRPICKER));
                        break;
                    }
                }
                s_hl_y_override = -1;
                /* v34.91: diagnostics status line — right side of the footer
                 * band, dim, scale 1. Always visible; the LAST line before a
                 * freeze names the culprit stage (mirrors the log file). */
                {
                    const char* tl = sw_trace_line();
                    if (tl && tl[0]) {
                        char shown[160];
                        utf8_trunc(tl, shown, sizeof(shown), 85);
                        int w = switch_font_text_width(shown, 1);
                        int x = SWITCH_UI_WIDTH - 32 - w;
                        if (x < 560) x = 560; /* keep clear of the footer hint */
                        draw_text(cv, SWITCH_UI_WIDTH, SWITCH_UI_HEIGHT, x, SWITCH_UI_HEIGHT - 40,
                                  shown, COL_DIM, 1);
                    }
                }
                /* [BATFIX] v36.41: бейдж заряда в правом верхнем углу — на
                 * ВСЕХ экранах меню. В настройках читаем РАБОЧУЮ КОПИЮ st —
                 * переключение строки «Заряд аккумулятора» видно живо ещё
                 * до сохранения (это превью, а не применение). */
                switch_ui_batt_badge_draw(cv, SWITCH_UI_WIDTH, SWITCH_UI_HEIGHT,
                                          st.battery_display);
                sdl_switch_ui_present();
            }
        } else {
            sdl_switch_ui_wait(4);
        }
    }

done:
    browse_free(&br.list);
    ui_log("menu session end (result=%d)\n", result);
    sw_trace("menu: session end (%d)", result); /* v34.91 */
    return result;
}

/* Version string for the main menu header. */
const char* nojme_switch_version_string(void) {
    extern const char* j2me_core_build_id(void);
    static char buf[64];
    if (!buf[0]) snprintf(buf, sizeof(buf), "core %s", j2me_core_build_id());
    return buf;
}
