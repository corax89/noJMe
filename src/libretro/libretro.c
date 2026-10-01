/*
 * J2ME Emulator - Libretro Core Implementation
 * Compatible with MinArch frontend
 */

/* BUILD_ID - increment this when making changes to verify correct version is running */
#define J2ME_BUILD_ID "2024.01.30.V15-REAL-TIME"

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdarg.h>
#include <time.h>
#include <sys/time.h>

#include "debug.h"
#include "debug_macros.h"
#include "core_version.h"
#include "libretro.h"
#include "libretro_shared.h"
#include "jvm.h"
#include "midp.h"
#include "render/render.h"
#include "native.h"
#include "opcodes.h"
#include "sdl_backend.h"
#include "miniz.h"  /* For JAR decompression */
#include "jar_reader.h" /* v19: canonical JAR reader */

/* Forward declaration for step-by-step execution */
extern int execute_frame(JVM* jvm, JavaThread* thread);

/* v34.81: localized core-option registration (core_options.c) */
extern bool j2me_register_core_options(retro_environment_t cb,
                                       const struct retro_variable* legacy_vars);

/* Forward declaration for audio processing */
extern void libretro_set_sample_rate(uint32_t rate);
extern void libretro_set_fps(int fps);
extern void libretro_process_audio(void);

/* Forward declaration for frame management */
extern void libretro_begin_frame(void);
extern void libretro_end_frame(void);
extern bool libretro_get_display_buffer(uint32_t** buffer, int* width, int* height);

/* Forward declarations for MIDP key events */
extern void midp_call_keyPressed(JVM* jvm, int keycode);
extern void midp_call_keyReleased(JVM* jvm, int keycode);

/* Forward declarations for RMS persistence */
extern void midp_rms_set_save_path(const char* save_dir, const char* game_name);
extern void midp_rms_save_all(void);

/* v34.4: manifest storage for MIDlet.getAppProperty (defined in jvm/native.c;
 * headless main.c already calls it, the libretro path never did) */
extern void midlet_set_manifest(const char* manifest_data, size_t size);

/* CRITICAL: External functions for J2ME event loop */
extern int midp_process_repaints(JVM* jvm);

/* ============================================
 * JAR/ZIP reading functions
 * ============================================ */

/* v19: JAR reading moved to the single canonical miniz-based reader
 * (src/utils/jar_reader.c) — see jvm.c for the full rationale. */
static uint8_t* jar_extract_file(const uint8_t* jar_data, size_t jar_size,
                                  const char* filename, size_t* out_size) {
    return jar_read_file(jar_data, jar_size, filename, out_size);
}

static char* find_midlet_class_in_jar(const uint8_t* jar_data, size_t jar_size) {
    size_t manifest_size;
    uint8_t* manifest = jar_extract_file(jar_data, jar_size, "META-INF/MANIFEST.MF", &manifest_size);
    
    if (!manifest) {
        manifest = jar_extract_file(jar_data, jar_size, "META-INF/manifest.mf", &manifest_size);
    }
    
    if (!manifest) return NULL;
    
    /* v34.29 FIX (line folding): JBenchmark 3D's manifest folds the main
     * class name across physical lines ("...JBenchm\r\n ark3D"); the old
     * strtok("\r\n") parse truncated it to "...JBenchm" and the midlet
     * could not launch. Unfold once and use the result for BOTH the class
     * lookup and getAppProperty storage (native.c's own continuation
     * joiner also adds a spurious leading space; the unfolded form makes
     * that path inert). */
    char* unfolded = jar_manifest_unfold(manifest, manifest_size);
    if (!unfolded) {
        free(manifest);
        return NULL;
    }
    free(manifest);
    
    /* v34.4 FIX: feed the manifest to getAppProperty. The headless entry
     * (main.c) calls midlet_set_manifest(), but this libretro path only used
     * the manifest to locate the MIDlet class and dropped it - so in the
     * libretro build MIDlet.getAppProperty() ALWAYS returned NULL
     * (user log: "[GAPP-DIAG] getAppProperty called, manifest=NULL").
     * Games that read config/URLs/DRM keys from the manifest/JAD got NULL
     * and skipped initialisation (suspected contributor to game 1's null
     * renderer NPE at b.paint). */
    midlet_set_manifest(unfolded, strlen(unfolded));
    LOG_SAFE("[J2ME] Manifest stored for getAppProperty (%zu bytes, unfolded)\n", strlen(unfolded));
    
    char* result = NULL;
    char* manifest_str = unfolded;
    
    char* line = strtok(manifest_str, "\r\n");
    while (line) {
        if (strncmp(line, "MIDlet-1:", 9) == 0 || strncmp(line, "MIDlet-1 :", 10) == 0) {
            char* start = strchr(line, ':');
            if (start) {
                start++;
                char* comma1 = strchr(start, ',');
                if (comma1) {
                    /* Skip whitespace after first comma */
                    char* after_comma1 = comma1 + 1;
                    while (*after_comma1 == ' ') after_comma1++;
                    
                    /* Try to find a second comma (format: Name, Icon, Class)
                     * If not found, use text after first comma (format: Name, Class) */
                    char* comma2 = strchr(after_comma1, ',');
                    char* class_start;
                    if (comma2) {
                        /* 3-field format: Name, Icon, Class */
                        class_start = comma2 + 1;
                        while (*class_start == ' ') class_start++;
                    } else {
                        /* 2-field format: Name, Class (no icon) */
                        class_start = after_comma1;
                    }
                    
                    char* end = class_start;
                    while (*end && *end != '\r' && *end != '\n' && *end != ',') end++;
                    while (end > class_start && (end[-1] == ' ' || end[-1] == '\t')) end--;
                    
                    size_t len = end - class_start;
                    if (len > 0) {
                        result = (char*)malloc(len + 1);
                        if (result) {
                            memcpy(result, class_start, len);
                            result[len] = '\0';
                        }
                    }
                    break;
                }
            }
        }
        line = strtok(NULL, "\r\n");
    }
    
    free(unfolded);   /* manifest itself was freed right after unfolding */
    return result;
}

/* Define missing constants */
#ifndef RETRO_REGION_NTSC
#define RETRO_REGION_NTSC 0
#endif

#ifndef RETRO_REGION_PAL
#define RETRO_REGION_PAL 1
#endif

/* ============================================
 * Global State
 * ============================================ */

JVM* g_jvm = NULL;  /* Non-static for sdl_backend_stubs.c */
static uint8_t* g_jar_data = NULL;
static size_t g_jar_size = 0;
static char g_midlet_class[256] = {0};
static char g_game_name[256] = {0};  /* Base name of loaded JAR for RMS saves */

/* Framebuffer - dynamic allocation for flexible screen sizes */
static uint32_t* g_framebuffer = NULL;  /* XRGB8888 format (internal) */
static uint16_t* g_rgb565_buffer = NULL;  /* RGB565 format (for frontend) */
static int g_screen_width = 240;
static int g_screen_height = 320;
/* v34.46 (Treasure Towers): resolution mode "auto" — derive the canvas
 * size from the JAR manifest hint (MIDxlet-Application-Range /
 * Nokia-MIDlet-Original-Display-Size) at game load; fall back to 240x320
 * when the manifest has no hint. Explicit resolutions below keep working
 * and always win over "auto". Defaults to 1: this mirrors the option
 * list default ("auto" is the first value), so a frontend that answers
 * GET_VARIABLE with false/no-value (lr_run, minimal hosts) gets the same
 * behavior as a fresh RetroArch. */
static int g_resolution_auto = 1;   /* set by update_variables from j2me_resolution */
static int g_screen_pitch = 240 * 4;  /* 240 * 4 bytes per pixel (XRGB8888) */
static size_t g_framebuffer_size = 0;
/* v34.41 FIX: фактический размер ВЫДЕЛЕННОГО rgb565-буфера (в байтах).
 * Прежние точки реаллокации сравнивали требуемый размер 565-буфера
 * (2 байта/пиксель) с g_framebuffer_size (4 байта/пиксель, XRGB8888) —
 * условие было почти всегда ложно, и при размере display-буфера больше
 * экранного конвертер писал за границей выделенной памяти (порча кучи,
 * класс «иногда SIGSEGV на ARMv7»). Считаем собственную ёмкость. */
static size_t g_rgb565_alloc_size = 0;
static bool g_use_rgb565 = true;  /* Default to RGB565 for MinArch */
/* v34.50: user preference from j2me_pixel_format (parsed in
 * update_variables): 0 = RGB565 (default, legacy), 1 = RGB888 (XRGB8888).
 * The effective g_use_rgb565 is negotiated at load: preferred format
 * first, the other as fallback (frontends may support only one). */
static int g_pixel_format_pref = 0;
/* v34.48: screen rotation — core option j2me_rotation.
 * 0 = off, 1 = 90° right (clockwise), 2 = 90° left (counter-clockwise).
 * Presentation-only: the MIDlet canvas keeps W x H (games see the same
 * getWidth()/getHeight()), the frame is rotated on the video_cb path and
 * the reported geometry (av_info / SET_GEOMETRY) swaps W/H. Pointer
 * input is inverse-mapped so stylus games stay playable. */
static int g_rotation = 0;
static uint8_t* g_rot_buffer = NULL;  /* staging buffer for the rotated frame */
static size_t g_rot_alloc_size = 0;
/* v34.50: frontend-native rotation active — RETRO_ENVIRONMENT_SET_ROTATION
 * was accepted by the frontend (RetroArch D3D10/11/12 + Vulkan rotate the
 * output GPU-side; RetroArch on GL returns false — its driver has no
 * set_rotation — and we fall back to the software path below). In this
 * mode the core sends UNROTATED canvas frames and reports base = TRUE
 * canvas with aspect = H/W (the DISPLAYED ratio): RetroArch does NOT
 * flip the core-provided aspect for core-requested rotations
 * (video_driver_get_core_aspect — the flip there covers the USER's video
 * rotation only), while its content dimensions ARE swapped — so the
 * rotated picture lands in a correctly-shaped viewport. This is the
 * proper fix for "rotation must change the aspect ratio": the frontend's
 * own viewport machinery owns it, refreshes included. */
static int g_rot_frontend = 0;

/* Runtime settings - configurable */
int g_target_fps = 30;  /* Non-static for sdl_backend_stubs.c */
static int g_audio_sample_rate = 22050;

/* Keys */
static int g_key_states = 0;
static int g_prev_key_states = 0;  /* Previous frame key states for edge detection */

/* Keyboard key states for numeric/special keys not mapped to joypad */
static int g_kb_key_states = 0;       /* Current keyboard extra keys */
static int g_kb_prev_key_states = 0;  /* Previous frame */

/* Key repeat support - on real J2ME phones, holding a key generates repeated
 * keyPressed events after an initial delay. Many Canvas games rely on this
 * for movement instead of polling getKeyStates(). */
#define KEY_REPEAT_INITIAL_DELAY_MS  400   /* Delay before first repeat (ms) */
#define KEY_REPEAT_INTERVAL_MS        80   /* Interval between repeats (ms) */
#define KEY_REPEAT_MAX_BIT           17    /* Highest bit number in key_map */

static struct {
    uint64_t press_time[KEY_REPEAT_MAX_BIT + 1];     /* Timestamp of initial press */
    uint64_t last_repeat_time[KEY_REPEAT_MAX_BIT + 1]; /* Timestamp of last repeat */
    bool is_held[KEY_REPEAT_MAX_BIT + 1];            /* Key is currently held down */
} g_key_repeat = {{0}, {0}, {false}};

/* Reset key repeat state for a specific bit */
static void key_repeat_reset(int bit) {
    if (bit >= 0 && bit <= KEY_REPEAT_MAX_BIT) {
        g_key_repeat.is_held[bit] = false;
        g_key_repeat.press_time[bit] = 0;
        g_key_repeat.last_repeat_time[bit] = 0;
    }
}

/* Keyboard extra key mapping: bit position -> {retro_key, midp_keycode} */
static const struct {
    int bit;
    unsigned retro_key;   /* RETROK_* constant */
    int keycode;           /* MIDP keyCode (ASCII value) */
} kb_key_map[] = {
    { 0, RETROK_0,       48},  /* '0' */
    { 1, RETROK_6,       54},  /* '6' */
    { 2, RETROK_7,       55},  /* '7' */
    { 3, RETROK_8,       56},  /* '8' */
    { 4, RETROK_9,       57},  /* '9' */
    { 5, RETROK_ASTERISK, 42},  /* '*' */
    { 6, RETROK_HASH,     35},  /* '#' */
};
#define KB_KEY_MAP_SIZE (sizeof(kb_key_map) / sizeof(kb_key_map[0]))

/* Key mapping: bit position -> MIDP keyCode */
static const struct {
    int bit;
    int keycode;
    int game_action;
} key_map[] = {
    { 1,  -1, 1},   /* UP -> keyCode -1, gameAction UP=1 */
    { 2,  -3, 2},   /* LEFT -> keyCode -3, gameAction LEFT=2 */
    { 5,  -4, 5},   /* RIGHT -> keyCode -4, gameAction RIGHT=5 */
    { 6,  -2, 6},   /* DOWN -> keyCode -2, gameAction DOWN=6 */
    { 8,  -5, 8},   /* FIRE -> keyCode -5, gameAction FIRE=8 */
    { 9,  -6, 0},   /* SELECT -> Left soft key -6 */
    { 10, -7, 0},   /* START -> Right soft key -7 */
    { 11, -8, 0},   /* B button -> GAME_C -8 */
    { 12, -9, 0},   /* X button -> GAME_D -9 */
    { 13, 49, 0},   /* Y button -> Key '1' (keyCode 49) */
    { 14, 50, 0},   /* L1 button -> Key '2' (keyCode 50) */
    { 15, 51, 0},   /* R1 button -> Key '3' (keyCode 51) */
    { 16, 52, 0},   /* L2 button -> Key '4' (keyCode 52) */
    { 17, 53, 0},   /* R2 button -> Key '5' (keyCode 53) */
};
#define KEY_MAP_SIZE (sizeof(key_map) / sizeof(key_map[0]))

/* Libretro callbacks */
retro_environment_t environ_cb = NULL;
retro_video_refresh_t video_cb = NULL;
retro_audio_sample_batch_t audio_batch_cb = NULL;
retro_input_poll_t input_poll_cb = NULL;
retro_input_state_t input_state_cb = NULL;
static retro_log_printf_t log_cb = NULL;

/* ============================================
 * v34.25 ALWAYS-ON frontend sink (MISSING_LOG forwarding)
 * ============================================
 * A Windows DLL loaded by a GUI libretro host has NO reachable stderr:
 * the v34.24 "quiet by default" policy left missing-method reports
 * ([INVOKE-MISSING] & co — the one channel we KEEP) invisible exactly
 * where the user asked for it. j2me_log_ungated() now mirrors every
 * always-on line into this sink; the sink forwards it to the host log
 * (log_cb) with per-signature dedup so a per-frame missing method
 * cannot flood the frontend log (stderr keeps the full stream — run
 * from a terminal or set NOJME_LOG=1 for everything). */
#define SINK_DEDUP_SLOTS 512u
#define SINK_SIG_LEN     64
static uint64_t g_sink_dedup[SINK_DEDUP_SLOTS];

static uint64_t sink_signature(const char* line) {
    /* FNV-1a over the first SINK_SIG_LEN chars: the prefix carries the
     * class.method descriptor (stable), volatile suffixes (pc=, caller,
     * addresses) collapse — one entry per emission site. */
    uint64_t h = 1469598103934665603ULL;
    for (int i = 0; line[i] != '\0' && i < SINK_SIG_LEN; i++) {
        h ^= (uint64_t)(unsigned char)line[i];
        h *= 1099511628211ULL;
    }
    return h ? h : 1; /* 0 marks an empty slot */
}

static void j2me_frontend_log_sink(const char* line) {
    if (!log_cb) return;
    const uint64_t sig = sink_signature(line);
    const unsigned slot = (unsigned)(sig % SINK_DEDUP_SLOTS);
    if (g_sink_dedup[slot] == sig) return;           /* already forwarded */
    if (g_sink_dedup[slot] == 0) g_sink_dedup[slot] = sig; /* else: rare
        hash collision — keep forwarding without storing, never drop */
    /* hosts append their own newline: strip ours, then emit one line */
    char clean[1024];
    size_t n = strlen(line);
    if (n >= sizeof(clean)) n = sizeof(clean) - 1;
    memcpy(clean, line, n);
    clean[n] = '\0';
    while (n > 0 && (clean[n - 1] == '\n' || clean[n - 1] == '\r')) clean[--n] = '\0';
    if (n == 0) return;
    log_cb(RETRO_LOG_WARN, "%s\n", clean);
}

/* v34.25: one-time terminal failures (JVM/class/MIDlet startup, midlet
 * death) stay on the ALWAYS-ON channel — a silent black screen is
 * undebuggable. Each fires at most once per run. Reaches stderr AND the
 * frontend log through the ungated sink. */
#define CORE_FATAL(fmt, ...) j2me_log_ungated("[J2ME][FATAL] " fmt, ##__VA_ARGS__)

/* ============================================
 * Option Values Local Cache
 * ============================================ */
#define MAX_OPTIONS 16
typedef struct {
    char key[64];
    char value[128];
} StoredOption;

static StoredOption g_stored_options[MAX_OPTIONS];
static int g_stored_options_count = 0;

/* Store an option value locally */
static void store_option_value(const char* key, const char* value) {
    if (!key || !value) return;
    
    LOG_SAFE("[J2ME] CACHE: Storing '%s' = '%s'\n", key, value);
    
    /* Check if key already exists */
    for (int i = 0; i < g_stored_options_count; i++) {
        if (strcmp(g_stored_options[i].key, key) == 0) {
            strncpy(g_stored_options[i].value, value, sizeof(g_stored_options[i].value) - 1);
            g_stored_options[i].value[sizeof(g_stored_options[i].value) - 1] = '\0';
            return;
        }
    }
    
    /* Add new option */
    if (g_stored_options_count < MAX_OPTIONS) {
        strncpy(g_stored_options[g_stored_options_count].key, key, sizeof(g_stored_options[0].key) - 1);
        g_stored_options[g_stored_options_count].key[sizeof(g_stored_options[0].key) - 1] = '\0';
        strncpy(g_stored_options[g_stored_options_count].value, value, sizeof(g_stored_options[0].value) - 1);
        g_stored_options[g_stored_options_count].value[sizeof(g_stored_options[0].value) - 1] = '\0';
        g_stored_options_count++;
    }
}

/* Get a stored option value */
static const char* get_stored_option_value(const char* key) {
    if (!key) return NULL;
    
    for (int i = 0; i < g_stored_options_count; i++) {
        if (strcmp(g_stored_options[i].key, key) == 0) {
            return g_stored_options[i].value;
        }
    }
    return NULL;
}

/* Time */
static uint64_t last_frame_time = 0;
static uint64_t frame_accumulator = 0;

/* ============================================
 * RGB565 Conversion
 * ============================================ */

static void convert_xrgb8888_to_rgb565(const uint32_t* src, uint16_t* dst, int width, int height) {
    /* v34.28: NEON conversion lives in render.c
     * (nojme_convert_xrgb8888_to_rgb565) — 8 pixels per iteration, pure
     * bit ops, bit-identical to the scalar formula. This runs on the FULL
     * screen every frame (RGB565 is the default frontend format on ARM). */
    nojme_convert_xrgb8888_to_rgb565(src, dst, width, height);
}

/* ============================================
 * v34.48: Screen Rotation (j2me_rotation)
 * ============================================ */

/* Rotate src (w x h, pitch w*bpp) by 90 degrees into dst (pitch h*bpp).
 * dir=1: clockwise ("90 right"), dir=2: counter-clockwise ("90 left").
 * Generic over the two frontend pixel formats: bpp=2 (RGB565),
 * bpp=4 (XRGB8888). Source row-major scan, destination column writes. */
static void rotate_frame_90(const void* src, void* dst, int w, int h, int bpp, int dir) {
    if (bpp == 2) {
        const uint16_t* s = (const uint16_t*)src;
        uint16_t* d = (uint16_t*)dst;
        if (dir == 1) {
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++)
                    d[x * h + (h - 1 - y)] = s[y * w + x];
        } else {
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++)
                    d[(w - 1 - x) * h + y] = s[y * w + x];
        }
    } else {
        const uint32_t* s = (const uint32_t*)src;
        uint32_t* d = (uint32_t*)dst;
        if (dir == 1) {
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++)
                    d[x * h + (h - 1 - y)] = s[y * w + x];
        } else {
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++)
                    d[(w - 1 - x) * h + y] = s[y * w + x];
        }
    }
}

/* Push the final frame to the frontend, applying j2me_rotation when set.
 * The canvas itself is NOT rotated (getWidth()/getHeight() are unchanged)
 * — only the outgoing frame and the reported geometry swap W/H.
 * v34.50: when the frontend accepted SET_ROTATION (g_rot_frontend), the
 * frontend rotates the output itself — hand the canvas frame over as-is. */
static void push_frame(void* src, int w, int h, int bpp) {
    if (!video_cb || w <= 0 || h <= 0) return;
    if (!g_rotation || g_rot_frontend) {
        /* no rotation, or frontend-native rotation — unrotated canvas frame */
        video_cb(src, w, h, (size_t)w * (size_t)bpp);
        return;
    }
    size_t need = (size_t)w * (size_t)h * (size_t)bpp;
    if (need > g_rot_alloc_size) {
        uint8_t* nb = (uint8_t*)malloc(need);
        if (!nb) {
            /* OOM: deliver the frame unrotated rather than drop it */
            video_cb(src, w, h, (size_t)w * (size_t)bpp);
            return;
        }
        free(g_rot_buffer);
        g_rot_buffer = nb;
        g_rot_alloc_size = need;
    }
    rotate_frame_90(src, g_rot_buffer, w, h, bpp, g_rotation);
    video_cb(g_rot_buffer, h, w, (size_t)h * (size_t)bpp);
}

/* v34.50: ask the frontend to rotate the output (SET_ROTATION, cmd 1).
 * Per spec the value is counter-clockwise: "90 right" (clockwise) = 3
 * (270 deg CCW), "90 left" = 1 (90 deg CCW). Returns 1 iff the frontend
 * accepted AND g_rotation is active.
 *
 * ROLLBACK QUORK: RetroArch records system.rotation BEFORE consulting the
 * video driver — on GL (no set_rotation in the driver) the env returns
 * false but the recorded rotation would swap RetroArch's content dims and
 * corrupt our software fallback. A follow-up SET_ROTATION(0) re-runs the
 * handler and resets that state, so the failed negotiation leaves the
 * frontend clean. MinArch-style hosts that never handle cmd 1 return false
 * without any state — the rollback is a harmless no-op there. */
static int lr_request_frontend_rotation(void) {
    if (!environ_cb) return 0;
    unsigned rot = 0;
    if (g_rotation == 1) rot = 3;        /* "90 right"  (CW) = 270 deg CCW */
    else if (g_rotation == 2) rot = 1;    /* "90 left"  (CCW) =  90 deg CCW */
    if (environ_cb(RETRO_ENVIRONMENT_SET_ROTATION, &rot))
        return g_rotation ? 1 : 0;        /* accepted (or rotation off) */
    if (g_rotation) {
        unsigned zero = 0;                /* rollback the partial state */
        environ_cb(RETRO_ENVIRONMENT_SET_ROTATION, &zero);
    }
    return 0;
}

/* v34.50: push the CURRENT geometry to the frontend (SET_GEOMETRY is only
 * legal from within retro_run — the option-change path in
 * update_variables runs there via check_variable_updates). Both rotation
 * modes need it on a live rotation change: software mode swaps base W/H;
 * frontend mode keeps the canvas base but flips the aspect (RetroArch
 * refreshes its core-aspect LUT only on SET_GEOMETRY / av_info queries). */
static void lr_push_geometry(void) {
    if (!environ_cb) return;
    struct retro_system_av_info avi;
    retro_get_system_av_info(&avi);
    environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &avi);
}

/* ============================================
 * Time Functions
 * ============================================ */

static uint64_t millis(void) {
    struct timeval time;
    gettimeofday(&time, NULL);
    return time.tv_sec * 1000 + time.tv_usec / 1000;
}

/* ============================================
 * Logging
 * ============================================ */

__attribute__((unused))
static void fallback_log(enum retro_log_level level, const char *fmt, ...) {
    (void)level;
    va_list va;
    va_start(va, fmt);
    vfprintf(stderr, fmt, va);
    va_end(va);
}

static void log_message(enum retro_log_level level, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    
    /* v34.25 "quiet by default", take 2: EVERY level is gated, including
     * RETRO_LOG_ERROR/WARN. v34.24 let ERROR/WARN reach the frontend log
     * unconditionally — on Windows (DLL: no stderr, log_cb is the only
     * visible channel) that kept flooding the host log, which is exactly
     * what the user still saw. The ONLY always-on channel now is
     * j2me_log_ungated(): missing-method reports + build banner +
     * CORE_FATAL. Enable everything with NOJME_LOG=1 / J2ME_DEBUG=1. */
    if (!j2me_log_enabled()) {
        va_end(args);
        return;
    }
    
    if (log_cb) {
        char buffer[1024];
        vsnprintf(buffer, sizeof(buffer), fmt, args);
        log_cb(level, "%s", buffer);
    } else {
        vfprintf(stderr, fmt, args);
    }
    
    va_end(args);
}

/* ============================================
 * Functions for stubs access
 * ============================================ */

static bool g_touch_input_enabled = true;   /* core option j2me_touch_input (v34.34) */

/* v34.81: touch emulation from the gamepad (core options j2me_touch_mode /
 * j2me_touch_button / j2me_touch_speed -- see core_options.c for the
 * localized descriptions).
 *   g_touch_mode:   0 = auto (stick wakes the cursor, 1.5 s idle timeout
 *                   -- the v34.34 behavior), 1 = analog stick, cursor
 *                   always on, 2 = D-Pad, cursor always on
 *   g_touch_btn_id / g_touch_btn_bit: the gamepad button that taps the
 *                   cursor; while the cursor is active its key bit is
 *                   suppressed so touch-only midlets see no double input
 *   g_touch_speed_slow / _fast: px per frame (auto mode keeps the
 *                   deflection-based 2-level speed; normal == 5/11 is the
 *                   v34.34 bit-identical default) */
static int      g_touch_mode = 0;                  /* 0 auto | 1 stick | 2 dpad */
static unsigned g_touch_btn_id = 8;                /* RETRO_DEVICE_ID_JOYPAD_A */
static int      g_touch_btn_bit = 8;               /* key_map bit of the button */
static int      g_touch_speed_slow = 5;            /* VCUR_SPEED_SLOW default */
static int      g_touch_speed_fast = 11;           /* VCUR_SPEED_FAST default */

/* виртуальный курсор (аналоговый стик / крестовина, v34.81 — режимы) */
static int   g_vcur_x = -1, g_vcur_y = -1;
static bool  g_vcur_armed = false;          /* курсор активен */
static uint64_t g_vcur_last_move_ms = 0;

static void update_variables(const char* game_path);

uint32_t* libretro_get_framebuffer(void) {
    return g_framebuffer;
}

int libretro_get_screen_width(void) {
    return g_screen_width;
}

int libretro_get_screen_height(void) {
    return g_screen_height;
}

void libretro_set_key_states(int states) {
    g_key_states = states;
}

int libretro_get_key_states(void) {
    return g_key_states;
}

/* ============================================
 * JVM Initialization
 * ============================================ */

static bool init_emulator(void) {
    log_message(RETRO_LOG_INFO, "[J2ME] Initializing emulator...\n");
    
    g_jvm = jvm_create();
    LOG_SAFE("[J2ME] init_emulator: jvm_create() returned %p\n", (void*)g_jvm);
    if (!g_jvm) {
        CORE_FATAL("Failed to create JVM\n");
        return false;
    }
    
    /* v34.34: 64MB по умолчанию (16MB мало для 3D-игр: Mortal Kombat Mobile 3D
     * умирал OutOfMemoryError'ом в загрузочном потоке ещё до главного меню).
     * Переопределяется через NOJME_HEAP_MB (для малопамятных устройств). */
    {
        const char* heap_env = getenv("NOJME_HEAP_MB");
        long heap_mb = 64;
        if (heap_env && atoi(heap_env) >= 4) heap_mb = atoi(heap_env);
        g_jvm->config.heap_size = (size_t)heap_mb * 1024 * 1024;
    }
    g_jvm->config.stack_size = 64 * 1024;
    g_jvm->config.max_threads = 8;
    g_jvm->config.verbose_class = false;
    
    LOG_SAFE("[J2ME] init_emulator: calling jvm_init()...\n");
    if (jvm_init(g_jvm) != JNI_OK) {
        CORE_FATAL("JVM init failed\n");
        jvm_destroy(g_jvm);
        g_jvm = NULL;
        return false;
    }
    LOG_SAFE("[J2ME] init_emulator: jvm_init() OK\n");
    
    LOG_SAFE("[J2ME] Calling sdl_init with %dx%d\n", g_screen_width, g_screen_height);
    if (sdl_init(g_jvm, g_screen_width, g_screen_height, 1, true) != 0) {
        CORE_FATAL("sdl_init failed\n");
        jvm_destroy(g_jvm);
        g_jvm = NULL;
        return false;
    }
    LOG_SAFE("[J2ME] sdl_init completed\n");
    
    LOG_SAFE("[J2ME] init_emulator: calling native_init()...\n");
    if (native_init(g_jvm) != JNI_OK) {
        CORE_FATAL("native_init failed\n");
        jvm_destroy(g_jvm);
        g_jvm = NULL;
        return false;
    }
    LOG_SAFE("[J2ME] init_emulator: native_init() OK\n");
    
    LOG_SAFE("[J2ME] init_emulator: calling midp_init()...\n");
    if (midp_init(g_jvm) != JNI_OK) {
        CORE_FATAL("midp_init failed\n");
        jvm_destroy(g_jvm);
        g_jvm = NULL;
        return false;
    }
    LOG_SAFE("[J2ME] init_emulator: midp_init() OK\n");
    
    LOG_SAFE("[J2ME] init_emulator: calling opcodes_init()...\n");
    opcodes_init();
    LOG_SAFE("[J2ME] init_emulator: opcodes_init() OK\n");
    
    log_message(RETRO_LOG_INFO, "[J2ME] Emulator initialized\n");
    return true;
}

/* ============================================
 * MIDlet Execution
 * ============================================ */

/* Forward declarations for error display */
extern void sdl_set_error_info(const char* title, const char* message, const char* stack_trace);
extern void sdl_set_error_extra(const char* extra);

/* Build an informative exception title with full class hierarchy */
static void build_exception_title(JavaObject* exception, char* out, int out_size) {
    if (!exception || !exception->header.clazz) {
        strncpy(out, "Unknown Exception", out_size - 1);
        out[out_size - 1] = '\0';
        return;
    }
    
    JavaClass* cls = exception->header.clazz;
    const char* name = cls->class_name ? cls->class_name : "Unknown";
    
    /* Convert java/lang/NullPointerException to NullPointerException */
    const char* slash = strrchr(name, '/');
    const char* short_name = slash ? slash + 1 : name;
    
    /* Try to walk superclass chain for additional context */
    char chain[512] = {0};
    JavaClass* super = cls->super_class;
    int depth = 0;
    while (super && depth < 3) {
        if (super->class_name && strcmp(super->class_name, "java/lang/Object") != 0 &&
            strcmp(super->class_name, "java/lang/Throwable") != 0 &&
            strcmp(super->class_name, "java/lang/Exception") != 0 &&
            strcmp(super->class_name, "java/lang/Error") != 0 &&
            strcmp(super->class_name, "java/lang/Runtime") != 0) {
            const char* ss = strrchr(super->class_name, '/');
            const char* sn = ss ? ss + 1 : super->class_name;
            if (chain[0]) strncat(chain, " > ", sizeof(chain) - strlen(chain) - 1);
            strncat(chain, sn, sizeof(chain) - strlen(chain) - 1);
            depth++;
            super = super->super_class;
        } else {
            break;
        }
    }
    
    if (chain[0]) {
        snprintf(out, out_size, "%s [%s]", short_name, chain);
    } else {
        strncpy(out, short_name, out_size - 1);
        out[out_size - 1] = '\0';
    }
}

/* Build detailed stack trace with PC and descriptor info */
static int build_stack_trace(JavaThread* thread, char* out, int out_size) {
    if (!thread || !thread->current_frame) {
        out[0] = '\0';
        return 0;
    }
    
    JavaFrame* frame = thread->current_frame;
    int pos = 0;
    int frame_count = 0;
    int max_frames = 25;
    
    /* First frame is the one where exception was thrown - mark it with >>> */
    while (frame && frame_count < max_frames && pos < out_size - 2) {
        const char* cls_name = frame->clazz && frame->clazz->class_name ? frame->clazz->class_name : "?";
        const char* method_name = frame->method && frame->method->name ? frame->method->name : "?";
        const char* descriptor = frame->method && frame->method->descriptor ? frame->method->descriptor : "";
        
        /* Skip internal/vm methods for clarity */
        if (cls_name[0] == '?' && method_name[0] == '?') {
            frame = frame->prev;
            continue;
        }
        
        /* Convert class name: java/lang/String -> String */
        const char* slash = strrchr(cls_name, '/');
        const char* short_cls = slash ? slash + 1 : cls_name;
        
        int written;
        if (frame_count == 0) {
            /* Throwing frame - highlight */
            written = snprintf(out + pos, out_size - pos - 1,
                    ">%s.%s(%s) PC=%d\n",
                    short_cls, method_name, descriptor, frame->throwing_pc);
        } else {
            written = snprintf(out + pos, out_size - pos - 1,
                    "  at %s.%s(%s)\n",
                    short_cls, method_name, descriptor);
        }
        
        if (written > 0 && pos + written < out_size - 1) {
            pos += written;
        } else {
            break;
        }
        
        frame = frame->prev;
        frame_count++;
    }
    
    out[pos] = '\0';
    return frame_count;
}

static bool run_midlet(void) {
    if (!g_jvm || !g_jar_data) return false;
    
    log_message(RETRO_LOG_INFO, "[J2ME] Starting MIDlet: %s\n", g_midlet_class);
    
    LOG_SAFE("[J2ME] run_midlet: calling jvm_set_jar_data() (jar_size=%zu)...\n", g_jar_size);
    jvm_set_jar_data(g_jvm, g_jar_data, g_jar_size, "game.jar");
    LOG_SAFE("[J2ME] run_midlet: jvm_set_jar_data() OK\n");
    
    char class_name[256];
    strncpy(class_name, g_midlet_class, sizeof(class_name) - 1);
    class_name[sizeof(class_name) - 1] = '\0';
    
    for (char* p = class_name; *p; p++) {
        if (*p == '.') *p = '/';
    }
    
    LOG_SAFE("[J2ME] run_midlet: loading class '%s'...\n", class_name);
    JavaClass* main_class = jvm_load_class(g_jvm, class_name);
    LOG_SAFE("[J2ME] run_midlet: jvm_load_class() returned %p\n", (void*)main_class);
    if (!main_class) {
        CORE_FATAL("Failed to load class %s\n", class_name);
        sdl_set_error_info("Class Not Found", class_name, "The MIDlet main class could not be loaded from the JAR file.");
        return false;
    }
    
    LOG_SAFE("[J2ME] run_midlet: calling jvm_run_midlet()...\n");
    /* v41 torn-heap fix: bracket the main-thread Java window (constructor +
     * startApp execute HERE on the frontend thread) so a runner-triggered
     * GC knows the main mutator is live and must park for the collection. */
    {
        extern void jvm_main_thread_exec_begin(void);
        extern void jvm_main_thread_exec_end(void);
        jvm_main_thread_exec_begin();
        int result = jvm_run_midlet(g_jvm, main_class);
        jvm_main_thread_exec_end();
        LOG_SAFE("[J2ME] run_midlet: jvm_run_midlet() returned %d\n", result);
        if (result != 0) {
        CORE_FATAL("MIDlet execution failed: %d\n", result);
        
        /* Check for pending exception and extract error info */
        JavaThread* main_thread = g_jvm->main_thread;
        if (main_thread && main_thread->pending_exception) {
            JavaObject* exception = main_thread->pending_exception;
            char exc_title[512] = {0};
            char message[512] = {0};
            char stack_trace[8192] = {0};
            
            /* Build informative exception title with hierarchy */
            build_exception_title(exception, exc_title, sizeof(exc_title));
            
            /* Look for detailMessage field in exception object */
            JavaClass* exc_class = exception->header.clazz;
            if (exc_class && exc_class->fields) {
                for (uint16_t i = 0; i < exc_class->fields_count; i++) {
                    JavaField* field = &exc_class->fields[i];
                    if (field->name && strcmp(field->name, "detailMessage") == 0) {
                        JavaValue* field_val = (JavaValue*)((uint8_t*)exception + sizeof(ObjectHeader) + i * sizeof(JavaValue));
                        if (field_val && field_val->ref) {
                            JavaObject* str_obj = (JavaObject*)field_val->ref;
                            if (str_obj->header.clazz && str_obj->header.clazz->class_name &&
                                strcmp(str_obj->header.clazz->class_name, "java/lang/String") == 0) {
                                JavaValue* value_field = (JavaValue*)((uint8_t*)str_obj + sizeof(ObjectHeader));
                                JavaValue* count_field = (JavaValue*)((uint8_t*)str_obj + sizeof(ObjectHeader) + sizeof(JavaValue));
                                if (value_field->ref && count_field->i > 0) {
                                    JavaObject* char_array = (JavaObject*)value_field->ref;
                                    jchar* chars = (jchar*)((uint8_t*)char_array + sizeof(ObjectHeader));
                                    int len = count_field->i < 250 ? count_field->i : 250;
                                    for (int j = 0; j < len; j++) {
                                        message[j] = (char)(chars[j] & 0xFF);
                                    }
                                    message[len] = '\0';
                                }
                            }
                        }
                        break;
                    }
                }
            }
            
            /* Build detailed stack trace - prefer saved trace over current frame chain */
            int frames = 0;
            if (main_thread->exception_stack_trace && main_thread->exception_stack_trace[0]) {
                /* Use the saved stack trace from when exception was first thrown */
                strncpy(stack_trace, main_thread->exception_stack_trace, sizeof(stack_trace) - 1);
                stack_trace[sizeof(stack_trace) - 1] = '\0';
                /* Count frames in the trace */
                for (const char* p = stack_trace; *p; p++) {
                    if (*p == '\n') frames++;
                }
            } else {
                /* Fallback: try to build from current frame chain */
                frames = build_stack_trace(main_thread, stack_trace, sizeof(stack_trace));
            }
            
            /* Set extra info: thread, frame count, and throw location */
            char extra[512];
            if (main_thread->exception_throw_info && main_thread->exception_throw_info[0]) {
                snprintf(extra, sizeof(extra), "Thread: main | %d frames | thrown at %s", 
                         frames, main_thread->exception_throw_info);
            } else {
                snprintf(extra, sizeof(extra), "Thread: main | %d frames", frames);
            }
            sdl_set_error_extra(extra);
            
            /* v34.25: the midlet itself died — terminal, one-time: keep on
             * the always-on channel (stderr + frontend log via sink). */
            CORE_FATAL("Uncaught %s: %s\n", exc_title, message[0] ? message : "(no message)");
            
            /* Log the stack trace to stderr for debugging */
            if (stack_trace[0]) {
                CORE_FATAL("Stack trace:\n%s", stack_trace);
            }
            
            sdl_set_error_info(exc_title, message[0] ? message : NULL, stack_trace[0] ? stack_trace : NULL);
        } else {
            sdl_set_error_info("Execution Failed", "MIDlet returned error code", NULL);
            CORE_FATAL("MIDlet failed with code %d (no exception object)\n", result);
        }
        
        return false;
    }
    }  /* end v41 main-thread exec window */

    log_message(RETRO_LOG_INFO, "[J2ME] MIDlet started\n");
    return true;
}

/* ============================================
 * Libretro API
 * ============================================ */

unsigned retro_api_version(void) {
    return RETRO_API_VERSION;
}

static void lr_vibra_start(int freq, int duration_ms);
static void lr_vibra_stop(void);

/* v23: adapter — Display.vibrate passes duration only */
static void lr_vibrate_cb(int duration_ms) {
    lr_vibra_start(0, duration_ms);
}

void retro_init(void) {
    /* Initialize logging mutex FIRST (before any LOG_SAFE calls) */
    log_mutex_init();
    
    /* v34.25: arm the frontend sink + reset its dedup table (fresh core
     * instance -> banner and missing-method reports forward once again). */
    memset(g_sink_dedup, 0, sizeof(g_sink_dedup));
    j2me_set_log_sink(j2me_frontend_log_sink);
    
    /* v34.24: ARM NEON A/B self-test (env-gated, always prints its result —
     * an explicitly requested diagnostic must not be silenced). */
    if (getenv("NOJME_NEON_SELFTEST")) {
        (void)render_neon_selftest();
    }
    
    /* v34.28: 2D micro-benchmark (env-gated, ALWAYS_LOG — explicitly
     * requested diagnostics are never silenced by the log gate). */
    if (getenv("NOJME_2D_BENCH")) {
        (void)nojme_2d_bench();
    }
    
    LOG_SAFE("[J2ME] ========================================\n");
    LOG_SAFE("[J2ME] BUILD_ID: %s\n", J2ME_BUILD_ID);
    LOG_SAFE("[J2ME] ========================================\n");
    LOG_SAFE("[J2ME] retro_init\n");
    
    g_framebuffer_size = g_screen_width * g_screen_height * sizeof(uint32_t);
    g_framebuffer = (uint32_t*)malloc(g_framebuffer_size);
    if (!g_framebuffer) {
        LOG_SAFE("[J2ME] Failed to allocate framebuffer\n");
        return;
    }
    memset(g_framebuffer, 0, g_framebuffer_size);
    g_screen_pitch = g_screen_width * 4;
    
    size_t rgb565_size = g_screen_width * g_screen_height * sizeof(uint16_t);
    g_rgb565_buffer = (uint16_t*)malloc(rgb565_size);
    if (!g_rgb565_buffer) {
        LOG_SAFE("[J2ME] Failed to allocate RGB565 buffer\n");
        free(g_framebuffer);
        g_framebuffer = NULL;
        return;
    }
    g_rgb565_alloc_size = rgb565_size; /* v34.41 */
    memset(g_rgb565_buffer, 0, rgb565_size);
    
    LOG_SAFE("[J2ME] Buffers allocated: XRGB8888=%p, RGB565=%p\n", 
            (void*)g_framebuffer, (void*)g_rgb565_buffer);
    
    uint64_t current_time = millis();
    last_frame_time = current_time;
    frame_accumulator = 0;

    /* v23 FIX (audit #29): wire the MIDP platform callbacks (vibrate /
     * flashBacklight) to the libretro backend. midp_set_platform_callbacks
     * used to receive NULL from every backend, so Display.vibrate() and
     * Display.flashBacklight() were silent no-ops everywhere. */
    {
        MidpPlatformCallbacks cb;
        memset(&cb, 0, sizeof(cb));
        cb.vibrate = lr_vibrate_cb;
        cb.flash_backlight = NULL;  /* no backlight control in libretro */
        midp_set_platform_callbacks(&cb);
    }
}

void retro_deinit(void) {
    /* v34.25: the frontend callbacks die with the core — drop the sink
     * so a stale log_cb can never be called after deinit. */
    j2me_set_log_sink(NULL);

    LOG_SAFE("[J2ME] retro_deinit\n");
    
    if (g_jvm) {
        jvm_destroy(g_jvm);
        g_jvm = NULL;
    }
    
    if (g_jar_data) {
        free(g_jar_data);
        g_jar_data = NULL;
    }
    
    if (g_framebuffer) {
        free(g_framebuffer);
        g_framebuffer = NULL;
    }
    
    if (g_rgb565_buffer) {
        free(g_rgb565_buffer);
        g_rgb565_buffer = NULL;
        g_rgb565_alloc_size = 0; /* v34.41 */
    }
    
    if (g_rot_buffer) {  /* v34.48: rotation staging buffer */
        free(g_rot_buffer);
        g_rot_buffer = NULL;
        g_rot_alloc_size = 0;
    }
    
    g_framebuffer_size = 0;
}

void retro_set_controller_port_device(unsigned port, unsigned device) {
    (void)port;
    (void)device;
}

void retro_get_system_info(struct retro_system_info *info) {
    memset(info, 0, sizeof(*info));
    info->library_name = "J2ME";
    info->library_version = "1.0";
    info->need_fullpath = true;
    info->valid_extensions = "jar";
}

void retro_get_system_av_info(struct retro_system_av_info *info) {
    info->timing.fps = (double)g_target_fps;
    info->timing.sample_rate = (double)g_audio_sample_rate;
    
    /* v34.48/v34.50: rotation geometry per mode — the outgoing frame must
     * match what the frontend expects and the DISPLAYED aspect must be
     * the rotated one:
     *  - software mode (v34.48): the core pushes rotated frames, so base
     *    W/H are swapped and aspect = H/W;
     *  - frontend mode (v34.50, SET_ROTATION accepted): the core pushes
     *    UNROTATED canvas frames; RetroArch swaps its content dims itself
     *    but does NOT flip the core-provided aspect for core-requested
     *    rotations (video_driver_get_core_aspect) — so we report base =
     *    true canvas with aspect = H/W (the DISPLAYED ratio).
     * max 800x800: a rotated 480x800 canvas is 800 pixels wide. */
    int vw = g_screen_width, vh = g_screen_height;
    if (g_rotation && !g_rot_frontend) { vw = g_screen_height; vh = g_screen_width; }
    info->geometry.base_width = vw;
    info->geometry.base_height = vh;
    info->geometry.max_width = 800;
    info->geometry.max_height = 800;
    info->geometry.aspect_ratio = g_rotation
        ? (float)g_screen_height / (float)g_screen_width   /* displayed = rotated */
        : (float)g_screen_width / (float)g_screen_height;
}

/* ============================================
 * Core Options - Legacy Format for MinArch
 * ============================================ */

/* Legacy format for MinArch frontend */
/* Added support for 240x136 and 480x272 resolutions */
static struct retro_variable g_core_variables[] = {
    /* v34.46: "auto" (default) picks the size from the JAR manifest hint
     * (Treasure Towers [128x160] refuses any other canvas with "Error!
     * Cannot start the game."); no hint -> 240x320. Added the missing
     * classic candybar sizes 128x160 / 132x176 / 176x220 as explicit
     * overrides for hint-less fixed-resolution builds. */
    /* v58: landscape + square sizes appended. Landscape J2ME builds
     * (motorola/SE widescreen ports, 3D racers) canvas-match only when
     * width>height; squares (208x208, 176x176) serve Siemens-class builds
     * like "3D Coaster Rush [208x208]". */
    { "j2me_resolution", "Screen Resolution; auto|240x320|240x136|480x272|320x480|360x640|480x800|176x220|176x208|132x176|128x160|128x128|320x240|640x360|640x480|800x480|240x160|220x176|208x176|176x132|160x128|208x208|176x176" },
    { "j2me_fps", "Frame Rate; 30|60|15|20" },
    { "j2me_audio_rate", "Audio Sample Rate; 22050|44100|11025" },
    { "j2me_scaling", "Screen Scaling; Aspect|Integer|Stretch" },
    /* v34.26 PERF (armv7): VM execution budget per frontend frame.
     * The old fixed budget (15k-60k instructions/frame = ~0.9M bytecodes/s)
     * is 10-50x slower than a 2005-era phone — the main Java thread could
     * not finish its game tick, so repaint deadlines slipped and frames
     * dropped. A real KVM-class device executes ~2-10M bytecodes/s.
     *   original: legacy ~0.9M/s (compatibility with old timing)
     *   normal:   ~2.7M/s
     *   fast:     ~5.4M/s (default — close to a mid-range phone)
     *   turbo:    8ms time-slice per frontend frame, no instruction cap
     *             (v34.83: the old 600k cap bound before the slice on
     *             strong hosts and contradicted the "no cap" label), for
     *             strong hosts */
    { "j2me_vm_speed", "VM Speed; fast|original|normal|turbo" },
    /* v41: NEON renderer master switch (see update_variables). */
    { "j2me_neon", "NEON Renderer (arm); on|off" },
    /* v34.79: M3G texture smoothing override (see update_variables).
     *   auto:   per-texture request — v34.78 behavior, bit-identical
     *           default (most games sample nearest; FILTER_LINEAR
     *           textures keep the legacy float bilinear)
     *   nearest: force crisp texels everywhere — fastest (the SIMD span
     *           path becomes eligible for EVERY texture)
     *   smooth: force integer bilinear everywhere — smoothed magnified
     *           textures via the fixed-point sampler + the bit-exact
     *           NEON/SSE2 span bilinear (armv7-friendly: 16-bit lanes,
     *           no float math per texel). Live-switchable, no rebuilds. */
    { "j2me_texture_smoothing", "Texture Smoothing (3D); auto|nearest|smooth" },
    /* v34.34: указатель/тач — для тачфоновых мидлетов (Block 3D 2 и т.п.) */
    { "j2me_touch_input", "Touch Input (stylus games); on|off" },
    /* v34.48: поворот кадра на 90° — для портретных candybar-игр на
     * альбомных экранах (и наоборот). Чисто презентационный: канвас
     * мидлета не меняется, меняются исходящий кадр и геометрия;
     * оси тача обратно отображаются на канвас. v34.50: сначала просим
     * фронтенд вращать самому (SET_ROTATION — аспект меняет фронтенд),
     * при отказе — программный поворот (см. g_rot_frontend). */
    { "j2me_rotation", "Screen Rotation; off|90 right|90 left" },
    /* v34.50: выходной формат пикселей. RGB565 (по умолчанию — как всегда:
     * в 2 раза меньше шины/памяти, конвертер NEON/скалярный уже есть) или
     * RGB888/XRGB8888 (полный диапазон цвета — для дизеринг-чувствительных
     * экранов/шейдеров). Выбранный формат запрашивается первым, второй —
     * фоллбек для фронтендов, поддерживающих только один. */
    /* v34.81: touch-cursor options for gamepad-driven touch emulation.
     * See process_pointer_input(): mode selects the cursor source
     * (auto-arming analog stick / always-on analog stick / always-on
     * D-Pad), button picks the tap key, speed scales movement. */
    { "j2me_touch_mode",   "Touch Cursor Mode; auto|stick|dpad" },
    { "j2me_touch_button", "Touch Tap Button; a|b|x|y|l|r|l2|r2" },
    { "j2me_touch_speed",  "Touch Cursor Speed; slow|normal|fast" },
    { "j2me_pixel_format", "Pixel Format; RGB565|RGB888" },
    { NULL, NULL }
};

/* v34.26: resolved VM speed mode — see j2me_vm_speed above. */
typedef enum { VM_SPEED_ORIGINAL = 0, VM_SPEED_NORMAL, VM_SPEED_FAST, VM_SPEED_TURBO } VmSpeedMode;
static VmSpeedMode g_vm_speed_mode = VM_SPEED_FAST;

/* Controller info */
static const struct retro_controller_description g_controllers[] = {
    { "RetroPad", RETRO_DEVICE_JOYPAD },
    { "Mouse",    RETRO_DEVICE_MOUSE },
    { "Touchscreen", RETRO_DEVICE_POINTER },
    { NULL, 0 }
};

static const struct retro_controller_info g_ports[] = {
    { g_controllers, 1 },
    { NULL, 0 }
};

/* Input descriptors */
static const struct retro_input_descriptor g_input_desc[] = {
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "Up" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "Down" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "Left" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "Right" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "Fire" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "Game C" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "Game D" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "Key 1" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Left Soft" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Right Soft" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      "Key 2" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,      "Key 3" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,     "Key 4" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,     "Key 5" },
    { 0, 0, 0, 0, NULL }
};

/* Helper function to set core options.
 * v34.81: registration moved to core_options.c — official core-options
 * API with Russian localization (SET_CORE_OPTIONS_INTL when the frontend
 * UI language is Russian; SET_CORE_OPTIONS otherwise; legacy
 * SET_VARIABLES as the last-resort fallback for MinArch-class hosts).
 * The v1 structs in include/libretro.h match the official layout. */
static void libretro_set_core_options(retro_environment_t cb) {
    if (!cb) return;

    if (j2me_register_core_options(cb, g_core_variables)) {
        LOG_SAFE("[J2ME] Core options registered (localized v1/intl path)\n");
    } else {
        LOG_SAFE("[J2ME] WARNING: core option registration failed!\n");
    }
}

/* ---- v17: rumble (vibra) + user-activity support ---- */
#ifndef RETRO_RUMBLE_STRONG
#define RETRO_RUMBLE_WEAK 0
#define RETRO_RUMBLE_STRONG 1
typedef void (*retro_set_rumble_state_t)(unsigned port, unsigned effect, uint16_t strength);
struct retro_rumble_interface {
    retro_set_rumble_state_t set_rumble_state;
};
#endif

extern void midp_set_vibra_hooks(void (*start_fn)(int freq, int duration_ms),
                                 void (*stop_fn)(void));
extern void midp_input_activity_mark(void);

static retro_set_rumble_state_t g_rumble_set = NULL;
static uint64_t g_rumble_until_ms = 0;

static uint64_t lr_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static void lr_vibra_start(int freq, int duration_ms) {
    (void)freq;
    if (!g_rumble_set) return;
    /* Map the requested duration onto the strong motor; games that never
     * call stopVibra() get an automatic expiry in retro_run(). */
    if (duration_ms <= 0) duration_ms = 200;
    if (duration_ms > 60000) duration_ms = 60000;
    g_rumble_until_ms = lr_now_ms() + (uint64_t)duration_ms;
    g_rumble_set(0, RETRO_RUMBLE_STRONG, 0xFFFF);
    g_rumble_set(0, RETRO_RUMBLE_WEAK, 0x6666);
}

static void lr_vibra_stop(void) {
    g_rumble_until_ms = 0;
    if (g_rumble_set) {
        g_rumble_set(0, RETRO_RUMBLE_STRONG, 0);
        g_rumble_set(0, RETRO_RUMBLE_WEAK, 0);
    }
}

/* Auto-expire rumble (call from retro_run) */
static void lr_vibra_tick(void) {
    if (g_rumble_set && g_rumble_until_ms != 0 && lr_now_ms() >= g_rumble_until_ms) {
        lr_vibra_stop();
    }
}

void retro_set_environment(retro_environment_t cb) {
    environ_cb = cb;

    LOG_SAFE("[J2ME] retro_set_environment called, cb=%p\n", (void*)cb);

    /* 1. Get log interface */
    struct retro_log_callback logging;
    if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging)) {
        log_cb = logging.log;
        LOG_SAFE("[J2ME] Log interface acquired\n");
    }

    /* v34.25: mirror the ALWAYS-ON channel (missing-method reports, build
     * banner, FATAL failures) into the host log — on Windows the DLL's
     * stderr is unreachable, log_cb is the only visible channel there.
     * Registered here (earliest callback) and re-armed in retro_init. */
    j2me_set_log_sink(j2me_frontend_log_sink);

    /* 1b. v17: acquire the rumble interface for DeviceControl.startVibra */
    {
        struct retro_rumble_interface rumble;
        memset(&rumble, 0, sizeof(rumble));
        if (cb(RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE, &rumble) && rumble.set_rumble_state) {
            g_rumble_set = rumble.set_rumble_state;
            LOG_SAFE("[J2ME] Rumble interface acquired (vibra supported)\n");
        } else {
            LOG_SAFE("[J2ME] No rumble interface (vibra reports unsupported)\n");
        }
        midp_set_vibra_hooks(lr_vibra_start, lr_vibra_stop);
    }

    /* 2. Set core options */
    libretro_set_core_options(cb);
    LOG_SAFE("[J2ME] Core options set\n");

    /* 3. Set controller info */
    cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void*)g_ports);
    LOG_SAFE("[J2ME] Controller info set\n");

    /* 4. Set input descriptors */
    cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, (void*)g_input_desc);
    LOG_SAFE("[J2ME] Input descriptors set\n");

    /* 5. No content mode */
    bool no_content = false;
    cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_content);

    LOG_SAFE("[J2ME] retro_set_environment complete\n");
}

/* Update variables from frontend */
static void update_variables(const char* game_path) {
    struct retro_variable var = {0};
    (void)game_path;
    int old_width = g_screen_width;
    int old_height = g_screen_height;
    int old_fps = g_target_fps;
    int old_audio = g_audio_sample_rate;

    if (!environ_cb) {
        LOG_SAFE("[J2ME] ERROR: environ_cb is NULL!\n");
        return;
    }

    /* Resolution */
    var.key = "j2me_resolution";
    var.value = NULL;

    bool result = environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var);
    LOG_SAFE("[J2ME] GET_VARIABLE(j2me_resolution) returned %d, value='%s'\n",
            result ? 1 : 0, var.value ? var.value : "(null)");

    /* Check cache if GET_VARIABLE failed */
    if (!var.value) {
        var.value = get_stored_option_value("j2me_resolution");
        LOG_SAFE("[J2ME] Using cached value: '%s'\n", var.value ? var.value : "(null)");
    }

    if (var.value) {
        if (strcmp(var.value, "auto") == 0) {
            /* v34.46: keep the current session size here — the manifest hint
             * is applied in retro_load_game() once the JAR is in memory.
             * Mid-run option refreshes with "auto" therefore change nothing
             * (no revert dance needed below). */
            g_resolution_auto = 1;
        } else {
            g_resolution_auto = 0;
            if (strcmp(var.value, "240x136") == 0) {
                g_screen_width = 240; g_screen_height = 136;
            } else if (strcmp(var.value, "480x272") == 0) {
                g_screen_width = 480; g_screen_height = 272;
            } else if (strcmp(var.value, "320x480") == 0) {
                g_screen_width = 320; g_screen_height = 480;
            } else if (strcmp(var.value, "360x640") == 0) {
                g_screen_width = 360; g_screen_height = 640;
            } else if (strcmp(var.value, "480x800") == 0) {
                g_screen_width = 480; g_screen_height = 800;
            } else if (strcmp(var.value, "176x220") == 0) {
                g_screen_width = 176; g_screen_height = 220;
            } else if (strcmp(var.value, "176x208") == 0) {
                g_screen_width = 176; g_screen_height = 208;
            } else if (strcmp(var.value, "132x176") == 0) {
                g_screen_width = 132; g_screen_height = 176;
            } else if (strcmp(var.value, "128x160") == 0) {
                g_screen_width = 128; g_screen_height = 160;
            } else if (strcmp(var.value, "128x128") == 0) {
                g_screen_width = 128; g_screen_height = 128;
            }
            /* v58 landscape + square sizes (see option list comment) */
            else if (strcmp(var.value, "320x240") == 0) {
                g_screen_width = 320; g_screen_height = 240;
            } else if (strcmp(var.value, "640x360") == 0) {
                g_screen_width = 640; g_screen_height = 360;
            } else if (strcmp(var.value, "640x480") == 0) {
                g_screen_width = 640; g_screen_height = 480;
            } else if (strcmp(var.value, "800x480") == 0) {
                g_screen_width = 800; g_screen_height = 480;
            } else if (strcmp(var.value, "240x160") == 0) {
                g_screen_width = 240; g_screen_height = 160;
            } else if (strcmp(var.value, "220x176") == 0) {
                g_screen_width = 220; g_screen_height = 176;
            } else if (strcmp(var.value, "208x176") == 0) {
                g_screen_width = 208; g_screen_height = 176;
            } else if (strcmp(var.value, "176x132") == 0) {
                g_screen_width = 176; g_screen_height = 132;
            } else if (strcmp(var.value, "160x128") == 0) {
                g_screen_width = 160; g_screen_height = 128;
            } else if (strcmp(var.value, "208x208") == 0) {
                g_screen_width = 208; g_screen_height = 208;
            } else if (strcmp(var.value, "176x176") == 0) {
                g_screen_width = 176; g_screen_height = 176;
            } else {
                g_screen_width = 240; g_screen_height = 320;
            }
        }
        store_option_value("j2me_resolution", var.value);
    }

    /* Frame rate */
    var.key = "j2me_fps";
    var.value = NULL;
    result = environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var);
    LOG_SAFE("[J2ME] GET_VARIABLE(j2me_fps) = %d, value='%s'\n",
            result ? 1 : 0, var.value ? var.value : "(null)");

    if (!var.value) var.value = get_stored_option_value("j2me_fps");

    if (var.value) {
        g_target_fps = atoi(var.value);
        if (g_target_fps < 15) g_target_fps = 15;
        if (g_target_fps > 60) g_target_fps = 60;
        store_option_value("j2me_fps", var.value);
    }

    /* v34.26 PERF: VM speed mode (instructions-per-frame budget) */
    var.key = "j2me_vm_speed";
    var.value = NULL;
    result = environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var);
    if (!var.value) var.value = get_stored_option_value("j2me_vm_speed");

    if (var.value) {
        if (strcmp(var.value, "original") == 0) {
            g_vm_speed_mode = VM_SPEED_ORIGINAL;
        } else if (strcmp(var.value, "normal") == 0) {
            g_vm_speed_mode = VM_SPEED_NORMAL;
        } else if (strcmp(var.value, "turbo") == 0) {
            g_vm_speed_mode = VM_SPEED_TURBO;
        } else {
            g_vm_speed_mode = VM_SPEED_FAST;
        }
        store_option_value("j2me_vm_speed", var.value);
    }

    /* v41 PERF: pthread VM-thread budget tracks the same VM speed mode
     * (see execute.c — the runner-thread throttle). Turbo deliberately
     * disables the throttle: strong hosts asking for raw speed accept the
     * garbage-rate consequences.
     * v34.83 FIX (turbo audit): an explicitly set NOJME_THREAD_BUDGET is a
     * MANUAL override per execute.c's documented contract — it used to be
     * silently stomped here on every option (re)read, so the documented
     * knob only ever worked in headless builds. Env now WINS over the
     * option-derived value; 0/negative = off, exactly as documented.
     * Resolved once, logged once (A/B harnesses and user triage rely on
     * it). Unset/empty -> option-driven value as before. */
    {
        extern long g_jvm_thread_budget;
        static long s_thr_env = -2;   /* -2 unresolved, -1 unset, else value */
        if (s_thr_env == -2) {
            const char* e = getenv("NOJME_THREAD_BUDGET");
            if (e && e[0]) {
                long v = atol(e);
                s_thr_env = (v > 0) ? v : 0;   /* "<=0 disables" per contract */
                LOG_SAFE("[J2ME] NOJME_THREAD_BUDGET='%s' -> manual override"
                         " %ld (j2me_vm_speed budget ignored)\n", e, s_thr_env);
            } else {
                s_thr_env = -1;
            }
        }
        if (s_thr_env >= 0) {
            g_jvm_thread_budget = s_thr_env;
        } else switch (g_vm_speed_mode) {
            case VM_SPEED_ORIGINAL: g_jvm_thread_budget = 15000; break;
            case VM_SPEED_NORMAL:   g_jvm_thread_budget = 45000; break;
            case VM_SPEED_TURBO:    g_jvm_thread_budget = 0;      break;
            case VM_SPEED_FAST:
            default:                g_jvm_thread_budget = 90000; break;
        }
    }

    /* v41 DIAG: NEON renderer master switch (3D span rasterizer + 2D
     * helpers + RGB565 converter). "off" forces the scalar paths — an
     * A/B escape hatch for NEON issues on specific toolchains/devices
     * (Nescube black screen triage on armv7). Env NOJME_NEON_SCALAR=1 /
     * NOJME_2D_SCALAR=1 keep working when the option is left "on". */
    var.key = "j2me_neon";
    var.value = NULL;
    result = environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var);
    if (!var.value) var.value = get_stored_option_value("j2me_neon");
    if (var.value) {
        int neon_off = (strcmp(var.value, "off") == 0);
        {
            extern int g_m3g_neon_force_scalar;  /* render.c */
            extern int g_2d_force_scalar;        /* render.c */
            g_m3g_neon_force_scalar = neon_off ? 1 : 0;
            g_2d_force_scalar = neon_off ? 1 : 0;
        }
        store_option_value("j2me_neon", var.value);
    }

    /* v34.79: M3G texture smoothing (j2me_texture_smoothing) — the global
     * texture-filter override in render.c (g_m3g_texture_filter_override).
     * Live-switchable: the value is read per mesh/span, no texture cache
     * rebuilds, takes effect on the next rendered frame. */
    var.key = "j2me_texture_smoothing";
    var.value = NULL;
    result = environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var);
    if (!var.value) var.value = get_stored_option_value("j2me_texture_smoothing");
    if (var.value) {
        extern int g_m3g_texture_filter_override;  /* render.c */
        int new_fov = (strcmp(var.value, "smooth") == 0) ? 2
                    : (strcmp(var.value, "nearest") == 0) ? 1 : 0;
        if (new_fov != g_m3g_texture_filter_override) {
            LOG_SAFE("[J2ME] Texture smoothing -> %s (%s)\n", var.value,
                     new_fov == 2 ? "integer bilinear, SIMD span"
                 : new_fov == 1 ? "forced nearest"
                                : "per-texture (game request)");
        }
        g_m3g_texture_filter_override = new_fov;
        store_option_value("j2me_texture_smoothing", var.value);
    }

    /* v34.34: pointer/touch input for stylus-only midlets */
    var.key = "j2me_touch_input";
    var.value = NULL;
    result = environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var);
    if (!var.value) var.value = get_stored_option_value("j2me_touch_input");
    if (var.value) {
        g_touch_input_enabled = (strcmp(var.value, "off") != 0);
        store_option_value("j2me_touch_input", var.value);
    }

    /* v34.81: touch cursor mode (auto / stick always-on / dpad always-on).
     * A mode switch resets the cursor position so the new mode starts
     * from the screen center; "auto" restores the v34.34 arming window. */
    var.key = "j2me_touch_mode";
    var.value = NULL;
    result = environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var);
    if (!var.value) var.value = get_stored_option_value("j2me_touch_mode");
    if (var.value) {
        int new_mode = (strcmp(var.value, "stick") == 0) ? 1
                     : (strcmp(var.value, "dpad") == 0) ? 2 : 0;
        if (new_mode != g_touch_mode) {
            LOG_SAFE("[J2ME] Touch cursor mode -> %s\n",
                     new_mode == 0 ? "auto (stick, 1.5s idle timeout)"
                     : new_mode == 1 ? "analog stick, always on"
                                     : "D-Pad, always on");
            g_touch_mode = new_mode;
            /* re-arm cleanly: the position re-centers on first movement */
            g_vcur_x = -1; g_vcur_y = -1; g_vcur_armed = 0;
        }
        store_option_value("j2me_touch_mode", var.value);
    }

    /* v34.81: which gamepad button taps the virtual cursor. */
    var.key = "j2me_touch_button";
    var.value = NULL;
    result = environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var);
    if (!var.value) var.value = get_stored_option_value("j2me_touch_button");
    if (var.value) {
        unsigned new_id = 8;   /* A */
        int new_bit = 8;
        if (strcmp(var.value, "b") == 0)  { new_id = 0;  new_bit = 11; }
        else if (strcmp(var.value, "x") == 0)  { new_id = 9;  new_bit = 12; }
        else if (strcmp(var.value, "y") == 0)  { new_id = 1;  new_bit = 13; }
        else if (strcmp(var.value, "l") == 0)  { new_id = 10; new_bit = 14; }
        else if (strcmp(var.value, "r") == 0)  { new_id = 11; new_bit = 15; }
        else if (strcmp(var.value, "l2") == 0) { new_id = 12; new_bit = 16; }
        else if (strcmp(var.value, "r2") == 0) { new_id = 13; new_bit = 17; }
        if (new_id != g_touch_btn_id) {
            LOG_SAFE("[J2ME] Touch tap button -> %s (key bit %d suppressed while cursor active)\n",
                     var.value, new_bit);
        }
        g_touch_btn_id = new_id;
        g_touch_btn_bit = new_bit;
        store_option_value("j2me_touch_button", var.value);
    }

    /* v34.81: cursor movement speed (px/frame @60fps; full analog
     * deflection doubles it). "normal" keeps the v34.34 5/11 defaults. */
    var.key = "j2me_touch_speed";
    var.value = NULL;
    result = environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var);
    if (!var.value) var.value = get_stored_option_value("j2me_touch_speed");
    if (var.value) {
        if (strcmp(var.value, "slow") == 0)      { g_touch_speed_slow = 4;  g_touch_speed_fast = 9;  }
        else if (strcmp(var.value, "fast") == 0) { g_touch_speed_slow = 8;  g_touch_speed_fast = 17; }
        else                                     { g_touch_speed_slow = 5;  g_touch_speed_fast = 11; }
        store_option_value("j2me_touch_speed", var.value);
    }

    /* v34.48: screen rotation — presentation-only, safe to change while
     * running (unlike resolution): the canvas size does not move, only
     * the outgoing frame and the reported geometry swap W/H. v34.50:
     * two execution modes — frontend-native (SET_ROTATION accepted: the
     * frontend rotates the output GPU-side and swaps its content dims,
     * aspect refresh via SET_GEOMETRY) and the software fallback (v34.48
     * pixel transpose + swapped base geometry). update_variables runs
     * inside retro_run when the game is live, so SET_ROTATION/
     * SET_GEOMETRY calls here are API-legal. */
    var.key = "j2me_rotation";
    var.value = NULL;
    result = environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var);
    if (!var.value) var.value = get_stored_option_value("j2me_rotation");
    if (var.value) {
        int new_rot = (strcmp(var.value, "90 right") == 0) ? 1
                    : (strcmp(var.value, "90 left") == 0) ? 2 : 0;
        if (new_rot != g_rotation) {
            g_rotation = new_rot;
            LOG_SAFE("[J2ME] Screen rotation -> %s\n",
                     g_rotation == 0 ? "off" : g_rotation == 1 ? "90 right" : "90 left");
            if (g_jvm && g_jvm->running && environ_cb) {
                /* live switch: renegotiate the frontend rotation first */
                g_rot_frontend = lr_request_frontend_rotation();
                LOG_SAFE("[J2ME]   mode: %s (outgoing %s, geometry %dx%d, aspect %.3f)\n",
                         g_rot_frontend ? "frontend SET_ROTATION" : "software rotate",
                         g_rot_frontend ? "unrotated canvas" : "rotated frame",
                         g_rotation && !g_rot_frontend ? g_screen_height : g_screen_width,
                         g_rotation && !g_rot_frontend ? g_screen_width : g_screen_height,
                         g_rotation
                             ? (float)g_screen_height / (float)g_screen_width
                             : (float)g_screen_width / (float)g_screen_height);
                /* refresh the viewport geometry in BOTH modes */
                lr_push_geometry();
            }
        }
        store_option_value("j2me_rotation", var.value);
    }

    /* v34.50: output pixel format preference. Applied at the next
     * retro_load_game (SET_PIXEL_FORMAT is only legal before the first
     * video_cb); a live change is noted but not renegotiated. */
    var.key = "j2me_pixel_format";
    var.value = NULL;
    result = environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var);
    if (!var.value) var.value = get_stored_option_value("j2me_pixel_format");
    if (var.value) {
        int new_pf = (strcmp(var.value, "RGB888") == 0) ? 1 : 0;
        if (new_pf != g_pixel_format_pref) {
            g_pixel_format_pref = new_pf;
            LOG_SAFE("[J2ME] Pixel format preference -> %s\n",
                     new_pf ? "RGB888 (XRGB8888)" : "RGB565");
            if (g_jvm && g_jvm->running) {
                LOG_SAFE("[J2ME] NOTE: pixel format change takes effect on next game load\n");
            }
        }
        store_option_value("j2me_pixel_format", var.value);
    }

    /* Audio sample rate */
    var.key = "j2me_audio_rate";
    var.value = NULL;
    result = environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var);
    LOG_SAFE("[J2ME] GET_VARIABLE(j2me_audio_rate) = %d, value='%s'\n",
            result ? 1 : 0, var.value ? var.value : "(null)");

    if (!var.value) var.value = get_stored_option_value("j2me_audio_rate");

    if (var.value) {
        g_audio_sample_rate = atoi(var.value);
        if (g_audio_sample_rate < 11025) g_audio_sample_rate = 11025;
        if (g_audio_sample_rate > 44100) g_audio_sample_rate = 44100;
        store_option_value("j2me_audio_rate", var.value);
    }

    g_screen_pitch = g_screen_width * 4;

    /* Update MIDP screen dimensions so getWidth()/getHeight() return correct values */
    midp_set_screen_dimensions(g_screen_width, g_screen_height);

    LOG_SAFE("[J2ME] Final settings: %dx%d, fps=%d, audio=%d\n",
            g_screen_width, g_screen_height, g_target_fps, g_audio_sample_rate);

    /* Note: Resolution changes during runtime require game restart */
    if (g_jvm && g_jvm->running && (g_screen_width != old_width || g_screen_height != old_height)) {
        LOG_SAFE("[J2ME] NOTE: Resolution change from %dx%d to %dx%d will take effect on next game load\n",
                old_width, old_height, g_screen_width, g_screen_height);
        /* Revert to old resolution for this session - cannot resize while running */
        g_screen_width = old_width;
        g_screen_height = old_height;
        midp_set_screen_dimensions(old_width, old_height);
    }

    /* Handle FPS/audio change */
    if (old_fps != g_target_fps || old_audio != g_audio_sample_rate) {
        libretro_set_sample_rate(g_audio_sample_rate);
        libretro_set_fps(g_target_fps);
        /* v34.74 (F-1): the FRONTEND paces retro_run to the fps we reported
         * in retro_get_system_av_info — pacing is frontend-driven (that is
         * why libretro_set_fps is a no-op here). A live j2me_fps change
         * used to re-scale the per-frame VM budget WITHOUT telling the
         * frontend, so the emulated CPU speed diverged from wall clock by
         * old_fps/new_fps (2x for a 30<->60 switch: the game ran at double
         * or half speed). Push the new timing via SET_SYSTEM_AV_INFO — the
         * libretro-legal runtime update — so the frontend re-paces and the
         * budget * rate product stays ~constant. */
        if (g_jvm && g_jvm->running && environ_cb) {
            struct retro_system_av_info avi;
            memset(&avi, 0, sizeof(avi));
            retro_get_system_av_info(&avi);
            if (environ_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &avi)) {
                LOG_SAFE("[J2ME] SET_SYSTEM_AV_INFO ok: fps=%d audio=%d (frontend re-pacing)\n",
                         g_target_fps, g_audio_sample_rate);
            } else {
                LOG_SAFE("[J2ME] NOTE: SET_SYSTEM_AV_INFO rejected; frontend "
                         "keeps old pacing (fps=%d) — VM budget follows the new setting\n",
                         g_target_fps);
            }
        }
    }
}

void retro_set_audio_sample(retro_audio_sample_t cb) {
    (void)cb;
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) {
    audio_batch_cb = cb;
}

void retro_set_input_poll(retro_input_poll_t cb) {
    LOG_SAFE("[J2ME] retro_set_input_poll called, cb=%p\n", (void*)cb);
    input_poll_cb = cb;
}

void retro_set_input_state(retro_input_state_t cb) {
    LOG_SAFE("[J2ME] retro_set_input_state called, cb=%p\n", (void*)cb);
    input_state_cb = cb;
}

void retro_set_video_refresh(retro_video_refresh_t cb) {
    video_cb = cb;
}

void retro_reset(void) {
    LOG_SAFE("[J2ME] retro_reset\n");
    g_key_states = 0;
    g_prev_key_states = 0;
    /* Clear key repeat state on reset */
    memset(&g_key_repeat, 0, sizeof(g_key_repeat));
}

bool retro_load_game(const struct retro_game_info *info) {
    LOG_SAFE("[J2ME] retro_load_game: %s\n", info ? info->path : "NULL");

    /* v36.21 [EXC-RESET]: same process can load another content — never
     * inherit the uncaught-exception screen of the previous midlet. */
    sdl_clear_error();

    if (!info || !info->path) {
        LOG_SAFE("[J2ME] No game path\n");
        return false;
    }
    
    /* Read settings */
    update_variables(info->path);
    
    /* v34.46 (Treasure Towers): the JAR must be in memory BEFORE the
     * framebuffer is (re)allocated — mode "auto" derives the canvas size
     * from the manifest hint and can change the required buffer size. */
    
    /* Load JAR file */
    FILE* f = fopen(info->path, "rb");
    if (!f) {
        CORE_FATAL("Cannot open file '%s'\n", info->path);
        return false;
    }
    
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    g_jar_data = malloc(size);
    if (!g_jar_data) {
        fclose(f);
        CORE_FATAL("Out of memory reading '%s' (%ld bytes)\n", info->path, size);
        return false;
    }
    
    if (fread(g_jar_data, 1, size, f) != (size_t)size) {
        free(g_jar_data);
        g_jar_data = NULL;
        fclose(f);
        CORE_FATAL("Short read on '%s'\n", info->path);
        return false;
    }
    fclose(f);
    g_jar_size = size;
    
    LOG_SAFE("[J2ME] JAR loaded: %ld bytes\n", size);
    
    /* Find MIDlet class */
    char* midlet_class = find_midlet_class_in_jar(g_jar_data, g_jar_size);
    if (midlet_class) {
        strncpy(g_midlet_class, midlet_class, sizeof(g_midlet_class) - 1);
        g_midlet_class[sizeof(g_midlet_class) - 1] = '\0';
        free(midlet_class);
        LOG_SAFE("[J2ME] Found MIDlet class: %s\n", g_midlet_class);
    } else {
        strcpy(g_midlet_class, "MIDlet");
        LOG_SAFE("[J2ME] Warning: Could not find MIDlet class in manifest\n");
    }
    
    /* v34.46 (Treasure Towers): "auto" resolution — take the canvas size
     * from the manifest hint. Fixed-resolution builds (Treasure Towers
     * [128x160] hard-gates on getWidth()==128 && getHeight()==160 and
     * shows "Error! Cannot start the game." otherwise) start correctly
     * without the user digging up the right size; hint-less JARs keep
     * 240x320. An explicitly chosen resolution bypasses this block. */
    if (g_resolution_auto) {
        int hint_w = 0, hint_h = 0;
        if (jar_detect_screen_size(g_jar_data, g_jar_size, &hint_w, &hint_h)) {
            g_screen_width = hint_w;
            g_screen_height = hint_h;
            g_screen_pitch = g_screen_width * 4;
            midp_set_screen_dimensions(g_screen_width, g_screen_height);
            LOG_SAFE("[J2ME] Auto resolution: manifest hint -> %dx%d\n",
                    g_screen_width, g_screen_height);
        } else {
            LOG_SAFE("[J2ME] Auto resolution: no manifest hint, keeping %dx%d\n",
                    g_screen_width, g_screen_height);
        }
    }
    
    LOG_SAFE("[J2ME] Settings from frontend: resolution=%dx%d%s, fps=%d, audio=%d\n",
            g_screen_width, g_screen_height, g_resolution_auto ? " (auto)" : "",
            g_target_fps, g_audio_sample_rate);
    
    /* Reallocate buffers if resolution changed */
    size_t required_size = g_screen_width * g_screen_height * sizeof(uint32_t);
    if (g_framebuffer_size != required_size && g_framebuffer) {
        LOG_SAFE("[J2ME] Reallocating buffers for %dx%d\n", g_screen_width, g_screen_height);
        
        free(g_framebuffer);
        free(g_rgb565_buffer);
        
        g_framebuffer_size = required_size;
        g_framebuffer = (uint32_t*)malloc(g_framebuffer_size);
        if (!g_framebuffer) {
            LOG_SAFE("[J2ME] Failed to reallocate framebuffer\n");
            return false;
        }
        memset(g_framebuffer, 0, g_framebuffer_size);
        g_screen_pitch = g_screen_width * 4;
        
        size_t rgb565_size = g_screen_width * g_screen_height * sizeof(uint16_t);
        g_rgb565_buffer = (uint16_t*)malloc(rgb565_size);
        if (!g_rgb565_buffer) {
            LOG_SAFE("[J2ME] Failed to reallocate RGB565 buffer\n");
            free(g_framebuffer);
            g_framebuffer = NULL;
            return false;
        }
        g_rgb565_alloc_size = rgb565_size; /* v34.41 */
        memset(g_rgb565_buffer, 0, rgb565_size);
        
        LOG_SAFE("[J2ME] Buffers reallocated\n");
    }
    
    /* Extract game name from JAR path for RMS save directory */
    {
        const char* path = info->path;
        const char* base = strrchr(path, '/');
        if (!base) base = strrchr(path, '\\');
        if (base) base++; else base = path;
        
        strncpy(g_game_name, base, sizeof(g_game_name) - 1);
        g_game_name[sizeof(g_game_name) - 1] = '\0';
        
        /* Strip .jar extension if present */
        char* dot = strrchr(g_game_name, '.');
        if (dot) *dot = '\0';
        
        LOG_SAFE("[J2ME] Game name for RMS: '%s'\n", g_game_name);
    }
    
    /* Get save directory from libretro frontend and set up RMS persistence */
    if (environ_cb) {
        const char* save_dir = NULL;
        if (environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &save_dir) && save_dir) {
            LOG_SAFE("[J2ME] Save directory: '%s'\n", save_dir);
            midp_rms_set_save_path(save_dir, g_game_name);
        } else {
            LOG_SAFE("[J2ME] No save directory available, RMS disk persistence disabled\n");
        }
    }
    
    /* Set pixel format — v34.50: the j2me_pixel_format preference picks the
     * negotiation order; the OTHER format stays the fallback so the core
     * keeps running on frontends that support only one of the two. */
    {
        enum retro_pixel_format fmt;
        bool got565 = false;
        if (g_pixel_format_pref == 1) {
            /* RGB888 (XRGB8888) preferred */
            fmt = RETRO_PIXEL_FORMAT_XRGB8888;
            if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) {
                LOG_SAFE("[J2ME] RGB888 not supported, trying RGB565\n");
                fmt = RETRO_PIXEL_FORMAT_RGB565;
                if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) {
                    LOG_SAFE("[J2ME] Neither RGB565 nor XRGB8888 supported\n");
                    return false;
                }
                got565 = true;
            }
        } else {
            /* RGB565 preferred (default — legacy MinArch path) */
            fmt = RETRO_PIXEL_FORMAT_RGB565;
            if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) {
                LOG_SAFE("[J2ME] RGB565 not supported, trying XRGB8888\n");
                fmt = RETRO_PIXEL_FORMAT_XRGB8888;
                if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) {
                    LOG_SAFE("[J2ME] Neither RGB565 nor XRGB8888 supported\n");
                    return false;
                }
            } else {
                got565 = true;
            }
        }
        g_use_rgb565 = got565;
        LOG_SAFE("[J2ME] Using %s format (preference: %s)\n",
                 got565 ? "RGB565" : "XRGB8888",
                 g_pixel_format_pref == 1 ? "RGB888" : "RGB565");
    }

    /* v34.50: rotation — try the FRONTEND-NATIVE path first: the frontend
     * (RetroArch D3D10/11/12, Vulkan) rotates the output GPU-side and
     * swaps its content dimensions itself; our reported aspect (see
     * retro_get_system_av_info) already carries the DISPLAYED (rotated)
     * ratio, so the frontend's own viewport machinery keeps the aspect
     * correct — this is the "rotation changes the aspect ratio" fix.
     * Frontends without SET_ROTATION (or with allow-rotate disabled, or
     * GL which has no driver-level rotation) fall back to the v34.48
     * software path: core-side pixel transpose + swapped base geometry. */
    g_rot_frontend = lr_request_frontend_rotation();
    if (g_rotation) {
        LOG_SAFE("[J2ME] Screen rotation %s: %s mode (canvas %dx%d, displayed aspect %.3f)\n",
                 g_rotation == 1 ? "90 right" : "90 left",
                 g_rot_frontend ? "frontend-native (SET_ROTATION)" : "software fallback",
                 g_screen_width, g_screen_height,
                 (float)g_screen_height / (float)g_screen_width);
    }
    
    libretro_set_sample_rate(g_audio_sample_rate);
    libretro_set_fps(g_target_fps);

    /* Initialize emulator */
    /* v34.24: single-line always-on banner — proves which core build is
     * actually loaded (stale-DLL defense, see CORE_VERSION.h). */
    MISSING_LOG("[J2ME] nojme libretro core BUILD: %s (Corax89)\n", CORE_BUILD_ID);
    LOG_SAFE("[J2ME] retro_load_game: calling init_emulator()...\n");
    if (!init_emulator()) {
        LOG_SAFE("[J2ME] Emulator init failed\n");
        return false;
    }
    LOG_SAFE("[J2ME] retro_load_game: init_emulator() OK\n");
    
    /* Run MIDlet */
    LOG_SAFE("[J2ME] retro_load_game: calling run_midlet()...\n");
    run_midlet();
    LOG_SAFE("[J2ME] retro_load_game: run_midlet() returned\n");
    
    LOG_SAFE("[J2ME] Game loaded successfully\n");

    /* v34.81 FRONTEND PAUSE: arm the retro_run-stall watchdog (RetroArch
     * menu). Libretro build only -- headless/app never call this, their
     * timing is untouched. NOJME_PAUSE_STALL_MS=0 disables. */
    {
        extern void jvm_frontend_pause_enable(int on);
        jvm_frontend_pause_enable(1);
        LOG_SAFE("[J2ME] Frontend-pause watchdog armed (stall threshold: see NOJME_PAUSE_STALL_MS)\n");
    }
    return true;
}

bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num) {
    (void)type;
    (void)info;
    (void)num;
    return false;
}

void retro_unload_game(void) {
    LOG_SAFE("[J2ME] retro_unload_game\n");
    
    /* v34.50: clear any frontend-native rotation we requested — RetroArch
     * resets core state per content load, but hosts that keep the core
     * instance (or reuse the video driver) would otherwise rotate the
     * NEXT content too. Harmless no-op where cmd 1 is unsupported. */
    if (g_rot_frontend && environ_cb) {
        unsigned zero = 0;
        environ_cb(RETRO_ENVIRONMENT_SET_ROTATION, &zero);
        g_rot_frontend = 0;
    }
    
    /* Save all RMS record stores to disk before unloading */
    midp_rms_save_all();
}

/* Key states */
static int get_key_states(void) {
    int keys = 0;
    lr_vibra_tick();
    if (input_state_cb) {
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP))    keys |= (1 << 1);
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN))  keys |= (1 << 6);
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT))  keys |= (1 << 2);
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT)) keys |= (1 << 5);
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A)) keys |= (1 << 8);
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B)) keys |= (1 << 11);
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X)) keys |= (1 << 12);
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y)) keys |= (1 << 13);
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT)) keys |= (1 << 9);
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START))  keys |= (1 << 10);
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L)) keys |= (1 << 14);
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R)) keys |= (1 << 15);
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2)) keys |= (1 << 16);
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2)) keys |= (1 << 17);

        /* v17: any pressed key refreshes the DeviceControl inactivity timer */
        if (keys != 0) {
            midp_input_activity_mark();
        }
    }
    return keys;
}

/* Poll keyboard for extra J2ME keys (0, 6-9, *, #) via RETRO_DEVICE_KEYBOARD */
static int get_kb_key_states(void) {
    int keys = 0;
    if (input_state_cb) {
        for (size_t i = 0; i < KB_KEY_MAP_SIZE; i++) {
            if (input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, kb_key_map[i].retro_key))
                keys |= (1 << kb_key_map[i].bit);
        }
    }
    return keys;
}

/* Process key events with key repeat support.
 * On real J2ME phones, holding a key generates repeated keyPressed events
 * after an initial delay (~400ms) at ~12Hz rate. Many Canvas-based games
 * rely on this for continuous movement instead of polling getKeyStates().
 * GameCanvas games with suppressKeyEvents=true use getKeyStates() polling
 * and don't need repeat, but the repeat events are harmlessly suppressed. */
/* ========================================================================
 * v34.34: POINTER / TOUCH INPUT
 * Block 3D 2 (Cocoasoft) и другие «тачфоновые» мидлеты управляются
 * ИСКЛЮЧИТЕЛЬНО через MIDP pointerPressed/Released/Dragged — клавиатурный
 * ввод у них мёртвый код. Libretro-фронтенд раньше доставлял только
 * клавиши джойпада, поэтому такие игры застывали на выборе языка.
 * Три источника указателя:
 *   1) RETRO_DEVICE_POINTER (тачскрин, абсолютные координаты);
 *   2) RETRO_DEVICE_MOUSE (левая кнопка, относительные дельты);
 *   3) виртуальный курсор: левый аналоговый стик двигает, A — тап.
 *      Активируется только когда стик реально двигали за последние
 *      1.5 с — обычные клавиатурные игры не затрагиваются.
 * ======================================================================== */
static int   g_ptr_x = -1;                  /* текущая позиция указателя */
static int   g_ptr_y = -1;
static int   g_ptr_x_prev_emit = -1;       /* последняя доставленная позиция */
static int   g_ptr_y_prev_emit = -1;
static bool  g_ptr_down = false;            /* кнопка/палец нажаты */
static bool  g_mouse_lb_prev = false;       /* прошлое состояние левой кнопки мыши */
static bool  g_touch_prev = false;          /* прошлое состояние тача */

/* (v34.81: состояние виртуального курсора перенесено наверх — его
 * читает update_variables при смене j2me_touch_mode) */
#define VCUR_ACTIVE_WINDOW_MS 1500
#define VCUR_SIZE 13                        /* размер перекрестия */

static uint32_t g_vcur_backup[VCUR_SIZE * VCUR_SIZE];
static int   g_vcur_saved_x = -1, g_vcur_saved_y = -1;

static void process_pointer_input(void) {
    if (!g_jvm || !g_jvm->running) return;
    if (!input_state_cb || !g_touch_input_enabled) return;

    extern void midp_call_pointerPressed(JVM* jvm, int x, int y);
    extern void midp_call_pointerReleased(JVM* jvm, int x, int y);
    extern void midp_call_pointerDragged(JVM* jvm, int x, int y);

    int   new_x = g_ptr_x, new_y = g_ptr_y;
    bool  new_down = g_ptr_down;
    bool  have_source = false;
    bool  suppress_fire = false;

    /* --- 1) Тачскрин (RETRO_DEVICE_POINTER): абсолютные координаты --- */
    int16_t tp_x = input_state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_X);
    int16_t tp_y = input_state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_Y);
    bool    tp_down = input_state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_PRESSED);
    if (tp_down || g_touch_prev) {
        /* координаты тача: [-32768..32767] -> [0..width/height) */
        if (g_rotation) {
            /* v34.48: фронтенд даёт координаты тача в ПОВЁРНУТОМ кадре
             * (w'=H, h'=W) — обратно отображаем их на канвас, чтобы
             * тач-игры видели тап там, где игрок его видит. */
            int px = ((int)tp_x + 0x8000) * g_screen_height / 0x10000;  /* 0..H-1 */
            int py = ((int)tp_y + 0x8000) * g_screen_width / 0x10000;   /* 0..W-1 */
            if (g_rotation == 1) {          /* кадр повёрнут вправо (CW) */
                new_x = py;
                new_y = g_screen_height - 1 - px;
            } else {                        /* влево (CCW) */
                new_x = g_screen_width - 1 - py;
                new_y = px;
            }
        } else {
            new_x = ((int)tp_x + 0x8000) * g_screen_width / 0x10000;
            new_y = ((int)tp_y + 0x8000) * g_screen_height / 0x10000;
        }
        /* v34.73: split the one-line double-if pairs (-Wmisleading-indentation). */
        if (new_x < 0) new_x = 0;
        if (new_x >= g_screen_width) new_x = g_screen_width - 1;
        if (new_y < 0) new_y = 0;
        if (new_y >= g_screen_height) new_y = g_screen_height - 1;
        new_down = tp_down;
        have_source = true;
        g_ptr_x = new_x; g_ptr_y = new_y;
    }
    g_touch_prev = tp_down;

    /* --- 2) Мышь (RETRO_DEVICE_MOUSE): относительные дельты + ЛКМ --- */
    int16_t m_dx = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_X);
    int16_t m_dy = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_Y);
    int     m_lb  = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_LEFT);
    if (!have_source && (m_dx || m_dy || m_lb || g_mouse_lb_prev)) {
        if (g_ptr_x < 0) { g_ptr_x = g_screen_width / 2; g_ptr_y = g_screen_height / 2; }
        int dx = m_dx, dy = m_dy;
        if (g_rotation) {
            /* v34.48: дельты мыши приходят в координатах повёрнутого
             * кадра — поворачиваем вектор на те же 90°. */
            if (g_rotation == 1) { int t = dx; dx = dy; dy = -t; }      /* CW */
            else                 { int t = dx; dx = -dy; dy = t; }      /* CCW */
        }
        new_x = g_ptr_x + dx;
        new_y = g_ptr_y + dy;
        if (new_x < 0) new_x = 0;
        if (new_x >= g_screen_width) new_x = g_screen_width - 1;
        if (new_y < 0) new_y = 0;
        if (new_y >= g_screen_height) new_y = g_screen_height - 1;
        new_down = m_lb != 0;
        have_source = true;
        g_ptr_x = new_x; g_ptr_y = new_y;
    }
    g_mouse_lb_prev = (m_lb != 0);

    /* --- 3) Виртуальный курсор: режимы j2me_touch_mode (v34.81) ---
     * auto:  стик будит курсор, гаснет через 1.5 с бездействия (v34.34);
     * stick: стик двигает, курсор всегда активен (тач-игры);
     * dpad:  крестовина двигает, курсор всегда активен (пады без стика).
     * Движение вектором в КООРДИНАТАХ ЭКРАНА: при повороте кадра вектор
     * поворачивается тем же правилом, что и дельты мыши выше, — курсор
     * ходит так, как его видит игрок. */
    int16_t ax = input_state_cb(0, RETRO_DEVICE_ANALOG, 0, RETRO_DEVICE_ID_ANALOG_X);
    int16_t ay = input_state_cb(0, RETRO_DEVICE_ANALOG, 0, RETRO_DEVICE_ID_ANALOG_Y);
    uint64_t now = millis();
    bool suppress_dpad = false;
    int mdx = 0, mdy = 0;    /* движение в экранных координатах */

    if (g_touch_mode == 2) {
        /* крестовина: постоянная скорость из j2me_touch_speed */
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT))  mdx -= g_touch_speed_slow;
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT)) mdx += g_touch_speed_slow;
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP))    mdy -= g_touch_speed_slow;
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN))  mdy += g_touch_speed_slow;
        if (mdx || mdy) g_vcur_last_move_ms = now;
        g_vcur_armed = true;                 /* всегда активен */
    } else {
        /* аналоговый стик: двухуровневая скорость по отклонению */
        if (abs(ax) > 0x1800 || abs(ay) > 0x1800) {
            int speed = (abs(ax) > 0x6000 || abs(ay) > 0x6000)
                        ? g_touch_speed_fast : g_touch_speed_slow;
            if (abs(ax) > 0x1800) mdx = (int)ax * speed / 0x8000;
            if (abs(ay) > 0x1800) mdy = (int)ay * speed / 0x8000;
            g_vcur_last_move_ms = now;
            g_vcur_armed = true;
        } else if (g_touch_mode == 0 && g_vcur_armed &&
                   now - g_vcur_last_move_ms > VCUR_ACTIVE_WINDOW_MS) {
            g_vcur_armed = false;   /* auto: стик давно не двигали — курсор гаснет */
        }
    }

    if (g_vcur_armed && !have_source) {
        if (g_vcur_x < 0) { g_vcur_x = g_screen_width / 2; g_vcur_y = g_screen_height / 2; }
        /* поворот вектора движения в координаты канваса (зеркало мыши) */
        int dx = mdx, dy = mdy;
        if (g_rotation == 1)      { int t = dx; dx = dy;  dy = -t; }   /* CW */
        else if (g_rotation == 2) { int t = dx; dx = -dy; dy = t; }   /* CCW */
        g_vcur_x += dx;
        g_vcur_y += dy;
        if (g_vcur_x < 0) g_vcur_x = 0;
        if (g_vcur_x >= g_screen_width) g_vcur_x = g_screen_width - 1;
        if (g_vcur_y < 0) g_vcur_y = 0;
        if (g_vcur_y >= g_screen_height) g_vcur_y = g_screen_height - 1;
        /* кнопка касания (j2me_touch_button, по умолчанию A/Fire) тапает
         * по курсору; её keyPressed гасим, чтобы тач-игры не получали
         * двойной ввод. В режиме крестовины гасим и саму крестовину. */
        new_x = g_vcur_x; new_y = g_vcur_y;
        new_down = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, g_touch_btn_id) != 0;
        have_source = true;
        suppress_fire = true;
        if (g_touch_mode == 2) suppress_dpad = true;
        g_ptr_x = new_x; g_ptr_y = new_y;
    }

    /* --- доставляем события в MIDP --- */
    if (have_source) {
        if (new_down && !g_ptr_down) {
            midp_call_pointerPressed(g_jvm, new_x, new_y);
        } else if (!new_down && g_ptr_down) {
            midp_call_pointerReleased(g_jvm, new_x, new_y);
        } else if (new_down && g_ptr_down && (new_x != g_ptr_x_prev_emit || new_y != g_ptr_y_prev_emit)) {
            midp_call_pointerDragged(g_jvm, new_x, new_y);
        }
        g_ptr_x_prev_emit = new_x; g_ptr_y_prev_emit = new_y;
    }
    g_ptr_down = new_down;

    /* пока активен виртуальный курсор — не слать кнопку касания как
     * клавишу (это тап), а в режиме крестовины — и саму крестовину */
    if (suppress_fire) g_key_states &= ~(1 << g_touch_btn_bit);
    if (suppress_dpad) g_key_states &= ~((1 << 1) | (1 << 2) | (1 << 5) | (1 << 6));
}

/* Перерисовка виртуального курсора поверх кадра (с бэкапом региона).
 * Вызывается ПЕРЕД video_cb; восстанавливает прошлый регион и рисует
 * новое перекрестие. Игра перекрашивает область при своём repaint. */
static void draw_virtual_cursor(uint32_t* buf, int w, int h) {
    if (!buf || !g_touch_input_enabled) return;
    /* восстановить прошлый регион */
    if (g_vcur_saved_x >= 0) {
        for (int y = 0; y < VCUR_SIZE; y++) {
            int dy = g_vcur_saved_y - VCUR_SIZE / 2 + y;
            if (dy < 0 || dy >= h) continue;
            for (int x = 0; x < VCUR_SIZE; x++) {
                int dx = g_vcur_saved_x - VCUR_SIZE / 2 + x;
                if (dx < 0 || dx >= w) continue;
                buf[dy * w + dx] = g_vcur_backup[y * VCUR_SIZE + x];
            }
        }
        g_vcur_saved_x = -1;
    }
    /* нарисовать курсор, если активен */
    if (g_vcur_armed && g_vcur_x >= 0) {
        for (int y = 0; y < VCUR_SIZE; y++) {
            int dy = g_vcur_y - VCUR_SIZE / 2 + y;
            if (dy < 0 || dy >= h) continue;
            for (int x = 0; x < VCUR_SIZE; x++) {
                int dx = g_vcur_x - VCUR_SIZE / 2 + x;
                if (dx < 0 || dx >= w) continue;
                g_vcur_backup[y * VCUR_SIZE + x] = buf[dy * w + dx];
                /* перекрестие: крест белого с чёрной обводкой */
                int rel = x - VCUR_SIZE / 2, rel_y = y - VCUR_SIZE / 2;
                bool cross = (rel == 0 || rel_y == 0) && abs(rel) <= VCUR_SIZE / 2 && abs(rel_y) <= VCUR_SIZE / 2;
                bool hole = (abs(rel) <= 1 && abs(rel_y) <= 1);
                if (cross && !hole) buf[dy * w + dx] = 0xFFFFFFFF;
                else if (hole)      buf[dy * w + dx] = g_ptr_down ? 0xFF00FF00 : 0xFFFF0000;
            }
        }
        g_vcur_saved_x = g_vcur_x; g_vcur_saved_y = g_vcur_y;
    }
}

static void process_key_events(void) {
    if (!g_jvm || !g_jvm->running) return;
    
    int pressed = g_key_states & ~g_prev_key_states;
    int released = ~g_key_states & g_prev_key_states;
    int held = g_key_states & g_prev_key_states;  /* Keys still held from last frame */
    uint64_t now = millis();
    
    extern bool midp_handle_soft_button(JVM* jvm, int button_index);
    
    for (size_t i = 0; i < KEY_MAP_SIZE; i++) {
        int bit = key_map[i].bit;
        int keycode = key_map[i].keycode;
        
        if (pressed & (1 << bit)) {
            /* New key press - record time and fire initial keyPressed */
            if (bit <= KEY_REPEAT_MAX_BIT) {
                g_key_repeat.is_held[bit] = true;
                g_key_repeat.press_time[bit] = now;
                g_key_repeat.last_repeat_time[bit] = now;
            }
            
            /* Intercept soft keys (SELECT=-6, START=-7) for Command handling */
            if (keycode == -6) {
                if (midp_handle_soft_button(g_jvm, 0)) continue;
            } else if (keycode == -7) {
                if (midp_handle_soft_button(g_jvm, 1)) continue;
            }
            midp_call_keyPressed(g_jvm, keycode);
        } else if (released & (1 << bit)) {
            /* Key released - clear repeat state and fire keyReleased */
            key_repeat_reset(bit);
            midp_call_keyReleased(g_jvm, keycode);
        } else if ((held & (1 << bit)) && bit <= KEY_REPEAT_MAX_BIT && g_key_repeat.is_held[bit]) {
            /* Key is still held - check if repeat should fire */
            uint64_t hold_duration = now - g_key_repeat.press_time[bit];
            uint64_t since_last = now - g_key_repeat.last_repeat_time[bit];
            
            if (hold_duration >= KEY_REPEAT_INITIAL_DELAY_MS &&
                since_last >= KEY_REPEAT_INTERVAL_MS) {
                /* Generate repeat event (v23 FIX, audit #27: repeats must go
                 * through keyRepeated(), not a fresh keyPressed() — matches
                 * the SDL backend and the MIDP spec; keyPressed() on repeat
                 * caused double-firing of game actions). */
                g_key_repeat.last_repeat_time[bit] = now;

                /* Skip repeat for soft keys - they should not repeat */
                if (keycode == -6 || keycode == -7) continue;

                extern void midp_call_keyRepeated(JVM* jvm, int keycode);
                midp_call_keyRepeated(g_jvm, keycode);
            }
        }
    }
    
    g_prev_key_states = g_key_states;
    
    /* Process keyboard extra keys (0, 6-9, *, #) */
    int kb_pressed = g_kb_key_states & ~g_kb_prev_key_states;
    int kb_released = ~g_kb_key_states & g_kb_prev_key_states;
    
    for (size_t i = 0; i < KB_KEY_MAP_SIZE; i++) {
        int bit = kb_key_map[i].bit;
        int keycode = kb_key_map[i].keycode;
        
        if (kb_pressed & (1 << bit)) {
            midp_call_keyPressed(g_jvm, keycode);
        }
        if (kb_released & (1 << bit)) {
            midp_call_keyReleased(g_jvm, keycode);
        }
        /* Note: keyboard extra keys don't need repeat since they are
         * typically used for text input where repeat is handled differently */
    }
    
    g_kb_prev_key_states = g_kb_key_states;
}

/* Check for variable updates */
static void check_variable_updates(void) {
    if (!environ_cb) return;
    
    bool updated = false;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated) {
        LOG_SAFE("[J2ME] Variables updated, re-reading...\n");
        update_variables(NULL);
    }
}

/* v41 torn-heap fix: retro_run exec-window marker (see jvm_main_thread_exec_begin) */
static int g_retro_run_exec_window = 0;

/* v41 PERF-DIAG sub-stamps for the video section (file scope so the
 * slow-frame report can read them after the section completes). */
static double s_prof_v1a = 0.0, s_prof_v1b = 0.0;

void retro_run(void) {
    static int first_run = 1;
    /* v34.73 DIAG: NOJME_AUDDBG=1 — branch trace (which early-return path
     * retro_run takes; correlates with the audio pump being skipped). */
    {
        static int s_adbg = -1;
        static uint32_t s_calls = 0;
        if (s_adbg < 0) {
            const char* e = getenv("NOJME_AUDDBG");
            s_adbg = (e && e[0] == '1') ? 1 : 0;
        }
        s_calls++;
        if (s_adbg && (s_calls % 50) == 1) {
            fprintf(stderr, "[AUDDBG] retro_run: call=%u running=%d err=%d\n",
                    s_calls, g_jvm ? (int)g_jvm->running : -1, (int)sdl_has_error());
        }
    }
    /* v41 PERF-DIAG: NOJME_FRAMEPROF=1 — per-phase frame timing printed when
     * a frame exceeds NOJME_FRAMEPROF_MS (default 30ms). Zero overhead when
     * off; one fprintf line when a slow frame is caught. */
    static int prof_on = -1;
    static double prof_thresh_ms = 30.0;
    struct timespec prof_t0, prof_t1;
    double t_budget = 0.0, t_timers = 0.0, t_audio = 0.0, t_total = 0.0;
    if (prof_on < 0) {
        const char* e = getenv("NOJME_FRAMEPROF");
        prof_on = (e && e[0] && e[0] != '0') ? 1 : 0;
        const char* th = getenv("NOJME_FRAMEPROF_MS");
        if (th && atof(th) > 0) prof_thresh_ms = atof(th);
        if (prof_on) {
            char l0[96];
            int ln = snprintf(l0, sizeof(l0), "[FRAMEPROF] init: on=%d thresh=%.1f\n",
                              prof_on, prof_thresh_ms);
            if (ln > 0) fwrite(l0, 1, (size_t)ln, stderr);
        }
    }
    if (prof_on) clock_gettime(CLOCK_MONOTONIC, &prof_t0);

    if (first_run) {
        LOG_SAFE("[J2ME] retro_run started, screen=%dx%d, fps=%d\n",
                g_screen_width, g_screen_height, g_target_fps);
        first_run = 0;
    }
    /* v41 torn-heap fix: retro_run is a main-thread Java window (budget
     * loop + key/serially/repaint pumps all execute the main thread on
     * THIS OS thread) — a runner-triggered GC must be able to park it. */
    {
        extern void jvm_main_thread_exec_begin(void);
        jvm_main_thread_exec_begin();
        g_retro_run_exec_window = 1;  /* cleared at every exit below */
        /* v34.81 FRONTEND PAUSE: heartbeat retro_run (see threads.c). */
        {
            extern void jvm_frontend_frame_tick(void);
            jvm_frontend_frame_tick();
        }
    }

    /* Check for variable updates */
    check_variable_updates();
    
    /* Poll input */
    if (input_poll_cb) {
        input_poll_cb();
    }
    g_key_states = get_key_states();
    g_kb_key_states = get_kb_key_states();
    
    /* Begin frame */
    libretro_begin_frame();
    
    /* Check for error display mode */
    extern bool sdl_has_error(void);
    extern void sdl_draw_error_screen(SdlContext* ctx);
    extern SdlContext* sdl_get_global_context(void);
    
    if (sdl_has_error()) {
        /* Draw error screen on the framebuffer */
        SdlContext* ctx = sdl_get_global_context();
        if (ctx && ctx->framebuffer) {
            sdl_draw_error_screen(ctx);
        }
        
        /* End frame and send video */
        libretro_end_frame();
        
        if (video_cb) {
            uint32_t* display_buffer = NULL;
            int fb_width = 0, fb_height = 0;
            
            libretro_get_display_buffer(&display_buffer, &fb_width, &fb_height);
            
            if (display_buffer && fb_width > 0 && fb_height > 0) {
                if (g_use_rgb565) {
                    size_t rgb565_size = fb_width * fb_height * sizeof(uint16_t);
                    if (!g_rgb565_buffer || rgb565_size > g_rgb565_alloc_size) {
                        /* v34.41 FIX: сравнение с ФАКТИЧЕСКОЙ ёмкостью 565-буфера
                         * (раньше сравнивали с g_framebuffer_size — 4 байта/пиксель
                         * XRGB8888, из-за чего рост буфера молча пропускался). */
                        if (g_rgb565_buffer) free(g_rgb565_buffer);
                        g_rgb565_buffer = (uint16_t*)malloc(rgb565_size);
                        g_rgb565_alloc_size = g_rgb565_buffer ? rgb565_size : 0;
                    }
                    if (g_rgb565_buffer) {
                        convert_xrgb8888_to_rgb565(display_buffer, g_rgb565_buffer, fb_width, fb_height);
                        push_frame(g_rgb565_buffer, fb_width, fb_height, 2);
                    } else {
                        push_frame(display_buffer, fb_width, fb_height, 4);
                    }
                } else {
                    push_frame(display_buffer, fb_width, fb_height, 4);
                }
            } else if (g_framebuffer && g_screen_width > 0 && g_screen_height > 0) {
                if (g_use_rgb565) {
                    convert_xrgb8888_to_rgb565(g_framebuffer, g_rgb565_buffer, g_screen_width, g_screen_height);
                    push_frame(g_rgb565_buffer, g_screen_width, g_screen_height, 2);
                } else {
                    push_frame(g_framebuffer, g_screen_width, g_screen_height, 4);
                }
            } else {
                video_cb(NULL, 1, 1, 0);
            }
        }
        
        /* Check for ESC to exit error screen */
        if (g_key_states & (1 << 9)) { /* SELECT button acts as ESC */
            g_jvm->running = false;
        }
        
        if (g_retro_run_exec_window) {
            extern void jvm_main_thread_exec_end(void);
            jvm_main_thread_exec_end();
            g_retro_run_exec_window = 0;
        }
        {
            extern void jvm_frontend_run_exit(void);
            jvm_frontend_run_exit();
        }
        return;
    }

    /* v34.72: "MIDlet finished" screen. After notifyDestroyed()/System.exit
     * the VM is stopped, but the frontend keeps calling retro_run() forever;
     * the last game frame stayed on screen and looked exactly like a hang
     * (user report: "it is unclear that the MIDlet has ended, it just seems
     * frozen"). Show an explicit black finished screen instead. The error
     * screen (if any) keeps priority — it is checked above. */
    if (!sdl_has_error() && g_jvm && !g_jvm->running) {
        extern void sdl_draw_midlet_finished_screen(SdlContext* ctx);
        SdlContext* ctx = sdl_get_global_context();
        if (ctx && ctx->framebuffer) {
            sdl_draw_midlet_finished_screen(ctx);
        }

        /* End frame and send video */
        libretro_end_frame();

        if (video_cb) {
            uint32_t* display_buffer = NULL;
            int fb_width = 0, fb_height = 0;

            libretro_get_display_buffer(&display_buffer, &fb_width, &fb_height);

            if (display_buffer && fb_width > 0 && fb_height > 0) {
                if (g_use_rgb565) {
                    size_t rgb565_size = fb_width * fb_height * sizeof(uint16_t);
                    if (!g_rgb565_buffer || rgb565_size > g_rgb565_alloc_size) {
                        if (g_rgb565_buffer) free(g_rgb565_buffer);
                        g_rgb565_buffer = (uint16_t*)malloc(rgb565_size);
                        g_rgb565_alloc_size = g_rgb565_buffer ? rgb565_size : 0;
                    }
                    if (g_rgb565_buffer) {
                        convert_xrgb8888_to_rgb565(display_buffer, g_rgb565_buffer, fb_width, fb_height);
                        push_frame(g_rgb565_buffer, fb_width, fb_height, 2);
                    } else {
                        push_frame(display_buffer, fb_width, fb_height, 4);
                    }
                } else {
                    push_frame(display_buffer, fb_width, fb_height, 4);
                }
            } else if (g_framebuffer && g_screen_width > 0 && g_screen_height > 0) {
                if (g_use_rgb565) {
                    convert_xrgb8888_to_rgb565(g_framebuffer, g_rgb565_buffer, g_screen_width, g_screen_height);
                    push_frame(g_rgb565_buffer, g_screen_width, g_screen_height, 2);
                } else {
                    push_frame(g_framebuffer, g_screen_width, g_screen_height, 4);
                }
            } else {
                video_cb(NULL, 1, 1, 0);
            }
        }

        /* v34.72: SELECT on the finished screen asks the frontend to close
         * the content gracefully (RETRO_ENVIRONMENT_SHUTDOWN; frontends
         * without support simply ignore it). */
        if (g_key_states & (1 << 9)) { /* SELECT */
            static bool shutdown_sent = false;
            if (!shutdown_sent && environ_cb) {
                log_message(RETRO_LOG_INFO, "[J2ME] MIDlet finished: requesting frontend shutdown\n");
                environ_cb(RETRO_ENVIRONMENT_SHUTDOWN, NULL);
                shutdown_sent = true;
            }
        }

        if (g_retro_run_exec_window) {
            extern void jvm_main_thread_exec_end(void);
            jvm_main_thread_exec_end();
            g_retro_run_exec_window = 0;
        }
        {
            extern void jvm_frontend_run_exit(void);
            jvm_frontend_run_exit();
        }
        return;
    }

    /* v34.34: pointer/touch input (Block 3D 2 & Co) — ДО ключей,
     * чтобы виртуальный курсор мог подавить бит кнопки касания и биты
     * крестовины В ТЕКУЩЕМ КАДРЕ. v34.81 FIX: код исторически стоял
     * ПОСЛЕ process_key_events (вопреки собственному комментарию), и
     * переход «нажато» подавляемой кнопки успевал выстрелить
     * keyPressed ДО маскирования — тач-игры получали двойной ввод на
     * каждый тап (press протекал всегда, release — через кадр). */
    process_pointer_input();

    /* Process key events */
    process_key_events();
    
    /* Execute JVM */
    if (prof_on) clock_gettime(CLOCK_MONOTONIC, &prof_t1);
    t_budget = (prof_t1.tv_sec - prof_t0.tv_sec) * 1000.0 + (prof_t1.tv_nsec - prof_t0.tv_nsec) / 1e6;
    if (g_jvm && g_jvm->running) {
        if (g_jvm->main_thread && g_jvm->main_thread->current_frame) {
            JavaThread* thread = g_jvm->main_thread;
            /* v34.26 PERF (armv7 frame drops): the old fixed budget ran the
             * main Java thread at ~0.9M bytecodes/s — 10-50x slower than the
             * 2005-era phones these games were tuned for, so the game tick
             * could not finish inside a frontend frame and repaint deadlines
             * slipped. The budget is now mode-driven (core option
             * j2me_vm_speed, default "fast" ≈ 5.4M/s ≈ mid-range phone);
             * turbo switches to a time slice. */
            int64_t instructions_per_frame;
            bool time_sliced = false;
            uint64_t slice_deadline_us = 0;
            switch (g_vm_speed_mode) {
                case VM_SPEED_ORIGINAL:
                    instructions_per_frame = 30000;
                    if (g_target_fps == 60) instructions_per_frame = 15000;
                    else if (g_target_fps == 20) instructions_per_frame = 45000;
                    else if (g_target_fps == 15) instructions_per_frame = 60000;
                    break;
                case VM_SPEED_NORMAL:
                    instructions_per_frame = 90000;
                    if (g_target_fps == 60) instructions_per_frame = 45000;
                    else if (g_target_fps == 20) instructions_per_frame = 135000;
                    else if (g_target_fps == 15) instructions_per_frame = 180000;
                    break;
                case VM_SPEED_TURBO:
                    /* time-slice: 8ms of interpretation per frontend frame.
                     * v34.83 FIX (turbo audit): the old 600k-instruction cap
                     * contradicted the option's own labels ("8 ms slices, no
                     * cap" / «8 мс на кадр, без лимита») and bound BEFORE the
                     * slice on strong hosts — an interpreter above 75M instr/s
                     * hits 600k in under 8ms, so the cap throttled turbo to a
                     * THIRD of what the slice allows. The 8ms deadline is now
                     * the only bound: checked every 16k instructions, so the
                     * loop still returns control to the frontend promptly and
                     * GC safepoints stay honored (64-instr poll below). */
                    instructions_per_frame = INT64_MAX;
                    time_sliced = true;
                    {
                        struct timespec ts;
                        clock_gettime(CLOCK_MONOTONIC, &ts);
                        slice_deadline_us = (uint64_t)ts.tv_sec * 1000000ULL +
                                            (uint64_t)ts.tv_nsec / 1000ULL + 8000ULL;
                    }
                    break;
                case VM_SPEED_FAST:
                default:
                    instructions_per_frame = 180000;
                    if (g_target_fps == 60) instructions_per_frame = 90000;
                    else if (g_target_fps == 20) instructions_per_frame = 270000;
                    else if (g_target_fps == 15) instructions_per_frame = 360000;
                    break;
            }
            
            /* v34.26: main thread now participates in GC stop-the-world.
             * The step-by-step path (execute_frame) never polled the GC
             * safepoint: when a pthread runner triggered a collection, the
             * main thread kept mutating the heap while the collector waited
             * (census off-by-one masked it), tearing the object graph. Park
             * here when a GC is requested — checked at the same 64-instruction
             * cadence as the interpreter loop, one cached branch otherwise. */
            extern volatile int g_gc_safepoint_request;
            uint32_t sp_countdown = 64;
            for (int64_t i = 0; i < instructions_per_frame && g_jvm->running; i++) {
                if (--sp_countdown == 0) {
                    sp_countdown = 64;
                    if (g_gc_safepoint_request) {
                        extern void jvm_gc_safepoint_park(void);
                        jvm_gc_safepoint_park();
                    }
                }
                if (time_sliced && (i & 0x3FFF) == 0) {
                    /* check the 8ms deadline every 16k instructions */
                    struct timespec ts;
                    clock_gettime(CLOCK_MONOTONIC, &ts);
                    uint64_t now_us = (uint64_t)ts.tv_sec * 1000000ULL +
                                      (uint64_t)ts.tv_nsec / 1000ULL;
                    if (now_us >= slice_deadline_us) break;
                }
                if (execute_frame(g_jvm, thread) != 0) {
                    /* Exception occurred during execution */
                    if (thread->pending_exception) {
                        /* Extract exception info using shared helpers */
                        JavaObject* exception = thread->pending_exception;
                        char exc_title[512] = {0};
                        char message[512] = {0};
                        char stack_trace[8192] = {0};
                        
                        /* Build informative exception title */
                        build_exception_title(exception, exc_title, sizeof(exc_title));
                        
                        /* Get message from exception detailMessage field */
                        JavaClass* exc_class = exception->header.clazz;
                        if (exc_class && exc_class->fields) {
                            for (uint16_t fi = 0; fi < exc_class->fields_count; fi++) {
                                JavaField* field = &exc_class->fields[fi];
                                if (field->name && strcmp(field->name, "detailMessage") == 0) {
                                    JavaValue* field_val = (JavaValue*)((uint8_t*)exception + sizeof(ObjectHeader) + fi * sizeof(JavaValue));
                                    if (field_val && field_val->ref) {
                                        JavaObject* str_obj = (JavaObject*)field_val->ref;
                                        if (str_obj->header.clazz && str_obj->header.clazz->class_name &&
                                            strcmp(str_obj->header.clazz->class_name, "java/lang/String") == 0) {
                                            JavaValue* value_field = (JavaValue*)((uint8_t*)str_obj + sizeof(ObjectHeader));
                                            JavaValue* count_field = (JavaValue*)((uint8_t*)str_obj + sizeof(ObjectHeader) + sizeof(JavaValue));
                                            if (value_field->ref && count_field->i > 0) {
                                                JavaObject* char_array = (JavaObject*)value_field->ref;
                                                jchar* chars = (jchar*)((uint8_t*)char_array + sizeof(ObjectHeader));
                                                int msg_len = count_field->i < 250 ? count_field->i : 250;
                                                for (int j = 0; j < msg_len; j++) {
                                                    message[j] = (char)(chars[j] & 0xFF);
                                                }
                                                message[msg_len] = '\0';
                                            }
                                        }
                                    }
                                    break;
                                }
                            }
                        }
                        
                        /* Build detailed stack trace - prefer saved trace */
                        int frames = 0;
                        if (thread->exception_stack_trace && thread->exception_stack_trace[0]) {
                            strncpy(stack_trace, thread->exception_stack_trace, sizeof(stack_trace) - 1);
                            stack_trace[sizeof(stack_trace) - 1] = '\0';
                            for (const char* p = stack_trace; *p; p++) {
                                if (*p == '\n') frames++;
                            }
                        } else {
                            frames = build_stack_trace(thread, stack_trace, sizeof(stack_trace));
                        }
                        
                        /* Set extra info with thread name, frame count, and throw location */
                        const char* thread_name = thread->name ? thread->name : "main";
                        char extra[512];
                        if (thread->exception_throw_info && thread->exception_throw_info[0]) {
                            snprintf(extra, sizeof(extra), "Thread: %s | %d frames | thrown at %s", 
                                     thread_name, frames, thread->exception_throw_info);
                        } else {
                            snprintf(extra, sizeof(extra), "Thread: %s | %d frames", thread_name, frames);
                        }
                        sdl_set_error_extra(extra);
                        
                        /* Log via libretro logging system */
                        log_message(RETRO_LOG_ERROR, "[J2ME] Uncaught %s in thread '%s': %s\n",
                                   exc_title, thread_name, message[0] ? message : "(no message)");
                        log_message(RETRO_LOG_ERROR, "[J2ME] Stack trace (%d frames):\n%s\n", frames, stack_trace);
                        
                        /* Set error info for display */
                        sdl_set_error_info(exc_title, message[0] ? message : NULL, stack_trace[0] ? stack_trace : NULL);
                    }
                    break;
                }
            }
        }
        
        /* Process timers (e.g. java.util.Timer, TimerTask) */
        extern void jvm_process_timers(JVM* jvm);
        jvm_process_timers(g_jvm);
        
        /* v35: age key queue + deliver keys nobody else will drain
         * (event-driven Form/List menus never re-enter the interpreter). */
        {
            extern void midp_frame_tick(void);
            extern void midp_pump_deferred_keys(JVM* jvm);
            extern void midp_pump_pending_shownotify(JVM* jvm);
            midp_pump_pending_shownotify(g_jvm);  /* v34.33: deferred setCurrent, before callSerially/repaints */
            midp_frame_tick();
            midp_pump_deferred_keys(g_jvm);
        }

        /* Process callSerially queue */
        extern void midp_process_call_serially_queue(JVM* jvm);
        midp_process_call_serially_queue(g_jvm);
        
        /* Check Alert timeout */
        extern bool midp_check_alert_timeout(JVM* jvm);
        midp_check_alert_timeout(g_jvm);
        
        midp_process_repaints(g_jvm);
    }

    if (prof_on) clock_gettime(CLOCK_MONOTONIC, &prof_t1);
    t_timers = (prof_t1.tv_sec - prof_t0.tv_sec) * 1000.0 + (prof_t1.tv_nsec - prof_t0.tv_nsec) / 1e6 - t_budget;

    /* End frame */
    libretro_end_frame();
    double t_v1 = 0.0;
    if (prof_on) {
        clock_gettime(CLOCK_MONOTONIC, &prof_t1);
        t_v1 = (prof_t1.tv_sec - prof_t0.tv_sec) * 1000.0 + (prof_t1.tv_nsec - prof_t0.tv_nsec) / 1e6 - t_budget - t_timers;
    }
    
    /* Send video */
    if (video_cb) {
        uint32_t* display_buffer = NULL;
        int fb_width = 0, fb_height = 0;
        libretro_get_display_buffer(&display_buffer, &fb_width, &fb_height);
        double t_v1a = 0.0, t_v1b = 0.0;
        if (prof_on) {
            clock_gettime(CLOCK_MONOTONIC, &prof_t1);
            t_v1a = (prof_t1.tv_sec - prof_t0.tv_sec) * 1000.0 + (prof_t1.tv_nsec - prof_t0.tv_nsec) / 1e6 - t_budget - t_timers;
        }

        /* v50 DIAG (Asphalt 3 3D stall): NOJME_STALL_DIAG=1 — presentation-
         * freeze detector. FNV-hashes a sampled pixel grid of the outgoing
         * frame on every retro_run; while the VM is running and the CONTENT
         * has not changed for > NOJME_STALL_MS (default 400ms) it reports the
         * episode once, requests a one-shot self-stack dump from every
         * interpreting thread (g_stall_stack_gen++, served at the 64-instr
         * slow-check — own frames only, safe) and calls jvm_dump_all_threads.
         * Prints the episode END (with total frozen time) when content moves
         * again, so freeze episodes line up with [GCSTAMP]/[PAINTTIME]. */
        {
            static int stall_diag_on = -1;
            static double stall_thresh_ms = 400.0;
            static unsigned stall_frames = 0;
            if (stall_diag_on < 0) {
                const char* e = getenv("NOJME_STALL_DIAG");
                stall_diag_on = (e && e[0] && e[0] != '0') ? 1 : 0;
                const char* t = getenv("NOJME_STALL_MS");
                if (t && atof(t) > 0) stall_thresh_ms = atof(t);
            }
            if (stall_diag_on) stall_frames++;
            if (stall_diag_on && display_buffer && fb_width > 0 && fb_height > 0 &&
                g_jvm && g_jvm->running) {
                extern volatile uint32_t g_stall_stack_gen;
                extern uint64_t g_gc_collections;
                extern void jvm_dump_all_threads(void);
                static uint64_t s_last_hash = 0;
                static int s_hash_armed = 0;
                static double s_last_change_ms = 0.0;
                static int s_stall_active = 0;
                static double s_stall_start_ms = 0.0;
                static uint64_t s_stall_gc0 = 0;
                static unsigned s_stall_fr0 = 0;
                struct timespec st_ts;
                clock_gettime(CLOCK_MONOTONIC, &st_ts);
                double now_ms = st_ts.tv_sec * 1000.0 + st_ts.tv_nsec / 1e6;
                uint64_t h = 1469598103934665603ULL;
                int step_y = fb_height / 48 + 1;
                int step_x = fb_width / 48 + 1;
                for (int y = 0; y < fb_height; y += step_y)
                    for (int x = 0; x < fb_width; x += step_x)
                        h = (h ^ (uint64_t)display_buffer[y * fb_width + x]) *
                            1099511628211ULL;
                if (!s_hash_armed || h != s_last_hash) {
                    if (s_stall_active) {
                        char line[160];
                        int ln = snprintf(line, sizeof(line),
                                "[STALL-DIAG] episode END: frozen %.0fms (frame %u..%u, gc=%llu..%llu)\n",
                                now_ms - s_stall_start_ms, s_stall_fr0, stall_frames,
                                (unsigned long long)s_stall_gc0,
                                (unsigned long long)g_gc_collections);
                        if (ln > 0) fwrite(line, 1, (size_t)ln, stderr);
                    }
                    s_last_hash = h;
                    s_hash_armed = 1;
                    s_last_change_ms = now_ms;
                    s_stall_active = 0;
                } else {
                    double frozen_ms = now_ms - s_last_change_ms;
                    if (!s_stall_active && frozen_ms > stall_thresh_ms) {
                        char line[192];
                        int ln;
                        s_stall_active = 1;
                        s_stall_start_ms = s_last_change_ms;
                        s_stall_gc0 = g_gc_collections;
                        s_stall_fr0 = stall_frames;
                        ln = snprintf(line, sizeof(line),
                                "[STALL-DIAG] episode START: frame content frozen %.0fms (frame %u, gc_total=%llu) - requesting thread stacks\n",
                                frozen_ms, stall_frames,
                                (unsigned long long)g_gc_collections);
                        if (ln > 0) fwrite(line, 1, (size_t)ln, stderr);
                        g_stall_stack_gen++;
                        jvm_dump_all_threads();
                    }
                }
            }
        }

        /* v34.34: виртуальный курсор поверх кадра (только если активен) */
        if (display_buffer && fb_width > 0 && fb_height > 0) {
            draw_virtual_cursor(display_buffer, fb_width, fb_height);
            if (g_use_rgb565) {
                size_t rgb565_size = fb_width * fb_height * sizeof(uint16_t);
                if (!g_rgb565_buffer || rgb565_size > g_rgb565_alloc_size) {
                    /* v34.41 FIX: та же корректная проверка ёмкости (см. выше). */
                    if (g_rgb565_buffer) free(g_rgb565_buffer);
                    g_rgb565_buffer = (uint16_t*)malloc(rgb565_size);
                    g_rgb565_alloc_size = g_rgb565_buffer ? rgb565_size : 0;
                }
                if (prof_on) {
                    clock_gettime(CLOCK_MONOTONIC, &prof_t1);
                    t_v1b = (prof_t1.tv_sec - prof_t0.tv_sec) * 1000.0 + (prof_t1.tv_nsec - prof_t0.tv_nsec) / 1e6 - t_budget - t_timers;
                }
                if (g_rgb565_buffer) {
                    convert_xrgb8888_to_rgb565(display_buffer, g_rgb565_buffer, fb_width, fb_height);
                    push_frame(g_rgb565_buffer, fb_width, fb_height, 2);
                } else {
                    push_frame(display_buffer, fb_width, fb_height, 4);
                }
            } else {
                push_frame(display_buffer, fb_width, fb_height, 4);
            }
        } else if (g_framebuffer && g_screen_width > 0 && g_screen_height > 0) {
            if (g_use_rgb565) {
                convert_xrgb8888_to_rgb565(g_framebuffer, g_rgb565_buffer, g_screen_width, g_screen_height);
                push_frame(g_rgb565_buffer, g_screen_width, g_screen_height, 2);
            } else {
                push_frame(g_framebuffer, g_screen_width, g_screen_height, 4);
            }
        } else {
            video_cb(NULL, 1, 1, 0);
        }
        if (prof_on) {
            /* stash the sub-stamps for the slow-frame report below */
            s_prof_v1a = t_v1a; s_prof_v1b = t_v1b;
        }
    }
    
    /* Send audio */
    double t_v2 = 0.0;
    if (prof_on) {
        clock_gettime(CLOCK_MONOTONIC, &prof_t1);
        t_v2 = (prof_t1.tv_sec - prof_t0.tv_sec) * 1000.0 + (prof_t1.tv_nsec - prof_t0.tv_nsec) / 1e6 - t_budget - t_timers;
    }
    libretro_process_audio();

    if (g_retro_run_exec_window) {
        extern void jvm_main_thread_exec_end(void);
        jvm_main_thread_exec_end();
        g_retro_run_exec_window = 0;
        {
            extern void jvm_frontend_run_exit(void);
            jvm_frontend_run_exit();
        }
    }

    if (prof_on) {
        clock_gettime(CLOCK_MONOTONIC, &prof_t1);
        t_total = (prof_t1.tv_sec - prof_t0.tv_sec) * 1000.0 + (prof_t1.tv_nsec - prof_t0.tv_nsec) / 1e6;
        t_audio = t_total - t_v2;
        if (t_total > prof_thresh_ms) {
            char line[256];
            int ln = snprintf(line, sizeof(line),
                    "[FRAMEPROF] total=%.1fms budget=%.1f repaints=%.1f endf=%.1f getdisp=%.1f convert=%.1f push=%.1f audio=%.1f\n",
                    t_total, t_budget, t_timers, t_v1, s_prof_v1a - t_v1,
                    s_prof_v1b - s_prof_v1a, t_v2 - s_prof_v1b, t_audio);
            if (ln > 0) fwrite(line, 1, (size_t)ln, stderr);
        }
    }
}

/* Save states
 * ===========
 * VERIFIED v34.26: this core deliberately reports "no save state support"
 * via the standard libretro protocol (serialize_size == 0 / serialize ==
 * false): the frontend then disables its savestate menu entries instead of
 * storing broken files.
 *
 * WHY a faithful snapshot is impossible in this architecture:
 *  1. The Java object heap IS a contiguous arena and could be copied — but
 *     every object header stores a raw JavaClass* into malloc'd class
 *     metadata that lives OUTSIDE the arena. A restored process rebuilds
 *     classes at different malloc addresses, so every restored clazz pointer
 *     would dangle.
 *  2. VM threads run as real OS pthreads mid-interpretation (native C stack
 *     between execute_method frames) — their resumption state cannot be
 *     captured by a memcpy snapshot.
 *  3. Native-side state (decoded images, M3G scene registries, media
 *     players, timers, monitor owners) is pointer-rich malloc data that no
 *     serializer walks.
 * Persistent progress that DOES work: RMS record stores (high scores,
 * options) are saved automatically by midp_rms_save_all() — see
 * retro_unload_game / retro_run teardown. */
size_t retro_serialize_size(void) {
    return 0;
}

bool retro_serialize(void *data, size_t size) {
    (void)data; (void)size;
    /* One always-on diagnostic so users understand why the frontend refuses
     * to save, instead of suspecting a silent bug. Throttled to once. */
    static bool warned = false;
    if (!warned) {
        warned = true;
        MISSING_LOG("[J2ME] Save states are not supported by this VM design "
                    "(malloc-scattered class metadata + pthread stacks cannot "
                    "be snapshotted). Game progress persists via RMS.\n");
    }
    return false;
}

bool retro_unserialize(const void *data, size_t size) {
    (void)data; (void)size;
    static bool warned = false;
    if (!warned) {
        warned = true;
        MISSING_LOG("[J2ME] Save states are not supported by this VM design "
                    "(see retro_serialize note). Load ignored.\n");
    }
    return false;
}

void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned index, bool enabled, const char* code) {
    (void)index;
    (void)enabled;
    (void)code;
}

unsigned retro_get_region(void) {
    return RETRO_REGION_NTSC;
}

void *retro_get_memory_data(unsigned id) {
    (void)id;
    return NULL;
}

size_t retro_get_memory_size(unsigned id) {
    (void)id;
    return 0;
}
