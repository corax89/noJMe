/*
 * J2ME Emulator - Main Entry Point
 * Cross-platform J2ME/MIDP2 emulator
 */

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L  /* For strdup - only on POSIX systems */
#include <sys/stat.h>
#include <unistd.h>
#else
#include <windows.h>
#endif

#include <stdio.h>
#include <pthread.h>   /* v36.48 [EXIT-RELEASE-ALL]: pthread_create для RMS-сброса в bailout */

/* v36.48 [DUMP-FENCE]: слабая ссылка — headless/app-сборки НЕ линкуют
 * switch_trace.c; файл-скоуп weak-extern (локальный в функции даёт
 * -Wattributes на gcc 12+). */
extern void sw_diag_set_jvm(void* jvm) __attribute__((weak));
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <signal.h>
/* v34.24: musl and some cross toolchains have no execinfo.h (was: __linux__,
 * broke musl). __has_include probes the real availability. */
#if defined(__linux__) && defined(__has_include)
#if __has_include(<execinfo.h>)
#define J2ME_HAVE_EXECINFO 1
#endif
#elif defined(__GLIBC__)
#define J2ME_HAVE_EXECINFO 1
#endif
#if J2ME_HAVE_EXECINFO
#include <execinfo.h>
#endif
#if !defined(_WIN32) && !defined(_WIN64)
#include <malloc.h>   /* v36.06: mallinfo for the per-session [MEM] line */
/* v36.42: glibc >= 2.34 помечает mallinfo() deprecated (newlib на Switch —
 * нет); статистика аллокатора нужна в обоих мирах, а mallinfo2() в newlib
 * отсутствует — гасим предупреждение точечно, вокруг единственной обёртки. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
static inline struct mallinfo nojme_mallinfo(void) { return mallinfo(); }
#pragma GCC diagnostic pop
#endif
/* v36.12 SOAK-VIS: the per-session [MEM] line gains rss= and os_threads=
 * (host/Linux only — /proc does not exist on HOS; there the mallinfo
 * fields already tell the story). The soak harness (scripts/soak_close.sh)
 * parses them to prove threads+memory return to baseline across open/close
 * cycles ("проверь что все потоки и хип память освобождаются корректно"). */
#if defined(__linux__)
#include <dirent.h>
#endif
/* v36.12 MEM-PROFILE: with an ASan build, NOJME_MEM_PROFILE=1 dumps an
 * aggregated allocation-stack profile at every session end — the definitive
 * "who still holds memory after close" answer (reachables included). */
#if defined(__SANITIZE_ADDRESS__)
#include <sanitizer/common_interface_defs.h>
#define J2ME_HAVE_SANITIZER_PROFILE 1
#endif
#include "miniz.h"

/* SDL2 on Windows needs SDL_main for proper initialization */
#ifdef _WIN32
#ifdef __has_include
#if __has_include(<SDL2/SDL_main.h>)
#include <SDL2/SDL_main.h>
#define SDL_MAIN_NEEDED 1
#endif
#endif
#endif

#include "core_version.h"
#include "jvm.h"
#include "classfile.h"
#include "opcodes.h"
#include "heap.h"
#include "native.h"
#include "midp.h"
#include "render/render.h"
#include "sdl_backend.h"
#include "debug.h"
#include "wildguard.h" /* [WILDGUARD] v36.50: мусорные указатели пути загрузки */
#ifdef __SWITCH__
#include "switch/switch_glue.h"
#include "switch/switch_common.h"
#include "switch/switch_ui.h"
#include <sys/stat.h>
/* v36.10 FIX: include the real libnx header only where it exists (the
 * devkitA64 device build). The switchui-verify HOST sandbox compiles this
 * same section with -D__SWITCH__ but has no libnx -> __has_include skips
 * the include and both svcExitProcess call sites below self-guard the same
 * way. (This is also exactly HOW v36.09's svcExitProcess(0) escaped local
 * verification: headless/testspin never parse this section against the
 * real libnx prototypes, and switchui-verify was skipped in session 53.) */
#if __has_include(<switch.h>)
#include <switch.h>
#endif
#endif
/* v34.91 freeze triage: stage breadcrumbs (stderr + sdmc log + on-screen
 * line). Macro no-op on desktop builds — safe to call from shared code. */
#include "switch/switch_trace.h"
#include "switch/pathguard.h" /* [PATHGUARD] v36.51: slab+n21+ptrscan прибор файловых путей */
#include "debug_macros.h"
#include "jar_reader.h"

/* Global runtime debug flag - defined here, declared in debug.h
 * Default is 0 (OFF) for release builds. Press F12 to toggle at runtime.
 * Set to 1 only when you need verbose debug logging. */
/* g_j2me_runtime_debug defined in jvm/debug_var.c */

/* Global context */
static SdlContext* g_sdl_ctx = NULL;
static JVM* g_jvm = NULL;
/* v34.26 BENCH: start of the main event loop (see [BENCH] print in run_midlet) */
static uint64_t g_bench_start_ms = 0;

/* Global JAR data for resource loading */
static uint8_t* g_jar_data = NULL;
static size_t g_jar_size = 0;

/* Get JAR data for resource loading */
const uint8_t* get_jar_data(size_t* size) {
    if (size) *size = g_jar_size;
    return g_jar_data;
}

/* SDL2 detection for logging */
#if defined(HAVE_SDL2) || defined(__has_include)
#  if __has_include(<SDL2/SDL_log.h>)
#    include <SDL2/SDL_log.h>
#  elif __has_include(<SDL_log.h>)
#    include <SDL_log.h>
#  endif
#endif

/* Signal handler for clean shutdown */
__attribute__((unused)) static void signal_handler(int sig) {
    (void)sig;
    DEBUG_LOG("Signal received, shutting down...");
    if (g_jvm) {
        g_jvm->running = false;
    }
    if (g_sdl_ctx) {
        g_sdl_ctx->running = false;
    }
}

/* Print usage */
__attribute__((unused)) static void print_usage(const char* program) {
    printf("J2ME Emulator v%s - MIDP2 Mobile Java Emulator\n\n", J2ME_EMULATOR_VERSION);
    printf("Usage: %s [options] <midlet.jar> [midlet-class]\n\n", program);
    printf("Options:\n");
    printf("  -w, --width <width>      Screen width (default: 240)\n");
    printf("  -h, --height <height>    Screen height (default: 320)\n");
    printf("  -s, --scale <scale>      Display scale factor (default: 2)\n");
    printf("  -c, --classpath <path>   Additional classpath\n");
    printf("  -m, --midlet <class>     MIDlet class name\n");
    printf("  -f, --fullscreen         Start in fullscreen mode\n");
    printf("  -v, --verbose            Verbose output\n");
    printf("  --verbose-class          Verbose class loading\n");
    printf("  --verbose-gc             Verbose garbage collection\n");
    printf("  --heap-size <size>       Heap size in MB (default: 16)\n");
    printf("  --headless               Run without display (for testing)\n");
    printf("  --help                   Show this help\n");
    printf("\nExamples:\n");
    printf("  %s game.jar\n", program);
    printf("  %s -w 320 -h 480 app.jar com.example.MyMIDlet\n", program);
    printf("  %s -s 3 --fullscreen game.jar\n", program);
    printf("  %s --headless game.jar  # For testing without display\n", program);
}

/* Parse command line arguments */
typedef struct {
    const char* jar_file;
    const char* midlet_class;
    int width;
    int height;
    /* v34.46 (Treasure Towers): set when -w/-h were given explicitly —
     * suppresses the manifest-hint auto resolution below. */
    bool explicit_size;
    int scale;
    bool fullscreen;
    bool verbose;
    bool verbose_class;
    bool verbose_gc;
    bool headless;
    size_t heap_size_mb;
    const char* classpath;
} Options;

__attribute__((unused)) static bool parse_args(int argc, char** argv, Options* opts) {
    memset(opts, 0, sizeof(Options));
    opts->width = MIDP_DEFAULT_WIDTH;
    opts->height = MIDP_DEFAULT_HEIGHT;
    opts->scale = 2;
    opts->heap_size_mb = 64;  /* v34.34: 3D-играм (Mortal Kombat 3D) мало 16MB — OOM в загрузке */
    opts->verbose = false;        /* Enable verbose by default for debugging */
    opts->verbose_class = false;  /* Enable class loading info by default */
    
#ifdef J2ME_HEADLESS
    /* Headless binary always runs in headless mode */
    opts->headless = true;
#endif
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-w") == 0 || strcmp(argv[i], "--width") == 0) {
            if (++i >= argc) return false;
            opts->width = atoi(argv[i]);
            opts->explicit_size = true;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--height") == 0) {
            if (++i >= argc) return false;
            opts->height = atoi(argv[i]);
            opts->explicit_size = true;
        } else if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--scale") == 0) {
            if (++i >= argc) return false;
            opts->scale = atoi(argv[i]);
        } else if (strcmp(argv[i], "-m") == 0 || strcmp(argv[i], "--midlet") == 0) {
            if (++i >= argc) return false;
            opts->midlet_class = argv[i];
        } else if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--classpath") == 0) {
            if (++i >= argc) return false;
            opts->classpath = argv[i];
        } else if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--fullscreen") == 0) {
            opts->fullscreen = true;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            opts->verbose = true;
            opts->verbose_class = true;
            opts->verbose_gc = true;
        } else if (strcmp(argv[i], "--verbose-class") == 0) {
            opts->verbose_class = true;
        } else if (strcmp(argv[i], "--verbose-gc") == 0) {
            opts->verbose_gc = true;
        } else if (strcmp(argv[i], "--heap-size") == 0) {
            if (++i >= argc) return false;
            opts->heap_size_mb = (size_t)atoi(argv[i]);
        } else if (strcmp(argv[i], "--headless") == 0) {
            opts->headless = true;
        } else if (strcmp(argv[i], "--help") == 0) {
            return false;
        } else if (argv[i][0] != '-') {
            if (!opts->jar_file) {
                opts->jar_file = argv[i];
            } else if (!opts->midlet_class) {
                opts->midlet_class = argv[i];
            }
        } else {
            LOG_SAFE("Unknown option: %s\n", argv[i]);
            return false;
        }
    }
    
    return opts->jar_file != NULL;
}

/* v19: JAR reading moved to the single canonical miniz-based reader
 * (src/utils/jar_reader.c) — see jvm.c for the full rationale. The
 * hand-rolled central-directory scanners were duplicated in four files and
 * silently diverged. */
static uint8_t* jar_find_file(const uint8_t* jar_data, size_t jar_size, 
                               const char* filename, size_t* out_size) {
    return jar_read_file(jar_data, jar_size, filename, out_size);
}

/* Public function to load a resource from the current JAR */
uint8_t* load_jar_resource(const char* filename, size_t* out_size) {
    return jar_find_file(g_jar_data, g_jar_size, filename, out_size);
}

/* Auto-generate DRM properties from JAR contents
 * This bypasses the need for a JAD file with DCHOC-* properties
 * by scanning the JAR for resource files and generating properties automatically.
 * Supports: Digital Chocolate (DCHOC-*), Siemens (SIE-*), and other DRM schemes.
 *
 * NOTE: intentionally kept as an opt-in feature — the call site in the load
 * path is commented out (see below). Marked unused so -Wall -Wextra stays
 * clean when the feature is disabled.
 */
static void __attribute__((unused)) midlet_generate_drm_properties(const uint8_t* jar_data, size_t jar_size) {
    if (!jar_data || jar_size == 0) return;
    
    INFO_LOG("[MIDlet] Auto-generating DRM properties from JAR contents...");
    
    int dchoc_index = 1;
    char key[32], value[32];
    size_t out_size;
    
    /* Digital Chocolate games: "p" is the properties file, should be DCHOC-1 */
    if (jar_find_file(jar_data, jar_size, "p", &out_size)) {
        snprintf(key, sizeof(key), "DCHOC-%d", dchoc_index++);
        midlet_add_property(key, "p");
        DEBUG_LOG("[MIDlet] Generated %s = p", key);
    }
    
    /* Scan for r1, r2, ..., r99 (resource files - common in Digital Chocolate games) */
    for (int i = 1; i <= 99 && dchoc_index <= 50; i++) {
        snprintf(value, sizeof(value), "r%d", i);
        if (jar_find_file(jar_data, jar_size, value, &out_size)) {
            snprintf(key, sizeof(key), "DCHOC-%d", dchoc_index++);
            midlet_add_property(key, value);
            DEBUG_LOG("[MIDlet] Generated %s = %s", key, value);
        }
    }
    
    /* Scan for l0_0, l1_0, ..., l9_9 (level files) */
    for (int level = 0; level <= 9 && dchoc_index <= 80; level++) {
        for (int sub = 0; sub <= 9; sub++) {
            snprintf(value, sizeof(value), "l%d_%d", level, sub);
            if (jar_find_file(jar_data, jar_size, value, &out_size)) {
                snprintf(key, sizeof(key), "DCHOC-%d", dchoc_index++);
                midlet_add_property(key, value);
                DEBUG_LOG("[MIDlet] Generated %s = %s", key, value);
            }
        }
    }
    
    /* Scan for common resource patterns used by other publishers */
    /* Siemens and others: s1, s2, ... */
    int other_index = 1;
    for (int i = 1; i <= 20; i++) {
        snprintf(value, sizeof(value), "s%d", i);
        if (jar_find_file(jar_data, jar_size, value, &out_size)) {
            snprintf(key, sizeof(key), "SIE-%d", other_index++);
            midlet_add_property(key, value);
        }
    }
    
    /* Gameloft pattern: g1, g2, ... */
    other_index = 1;
    for (int i = 1; i <= 20; i++) {
        snprintf(value, sizeof(value), "g%d", i);
        if (jar_find_file(jar_data, jar_size, value, &out_size)) {
            snprintf(key, sizeof(key), "GL-%d", other_index++);
            midlet_add_property(key, value);
        }
    }
    
    /* EA pattern: ea1, ea2, ... */
    other_index = 1;
    for (int i = 1; i <= 20; i++) {
        snprintf(value, sizeof(value), "ea%d", i);
        if (jar_find_file(jar_data, jar_size, value, &out_size)) {
            snprintf(key, sizeof(key), "EA-%d", other_index++);
            midlet_add_property(key, value);
        }
    }
    
    INFO_LOG("[MIDlet] Generated %d DCHOC properties and other DRM properties", dchoc_index - 1);
}

/* Find MIDlet class from JAR manifest */
static char* find_midlet_class(const uint8_t* jar_data, size_t jar_size) {
    DEBUG_LOG("Looking for MIDlet class in manifest...");
    
    size_t manifest_size;
    uint8_t* manifest = jar_find_file(jar_data, jar_size, "META-INF/MANIFEST.MF", &manifest_size);
    
    if (!manifest) {
        /* Try lowercase */
        manifest = jar_find_file(jar_data, jar_size, "META-INF/manifest.mf", &manifest_size);
    }
    
    if (!manifest) {
        DEBUG_LOG("Manifest not found in JAR");
        return NULL;
    }
    
    DEBUG_LOG("Manifest found, size: %zu", manifest_size);
    
    /* v34.29 FIX (line folding): unfold continuation lines BEFORE parsing —
     * manifests like JBenchmark 3D's split "MIDlet-1: ...Class" across
     * physical lines and strtok("\r\n") truncated the class name. */
    char* unfolded = jar_manifest_unfold(manifest, manifest_size);
    free(manifest);
    if (!unfolded) {
        return NULL;
    }
    
    /* Parse manifest to find MIDlet-n */
    char* result = NULL;
    char* manifest_str = unfolded;
    
    /* Look for MIDlet-1: line */
    char* line = strtok(manifest_str, "\r\n");
    while (line) {
        DEBUG_LOG("Manifest line: '%s'", line);
        
        if (strncmp(line, "MIDlet-1:", 9) == 0 || 
            strncmp(line, "MIDlet-1 :", 10) == 0) {
            /* Format: MIDlet-1: Name, Icon, Class */
            char* start = strchr(line, ':');
            if (start) {
                start++;
                /* Skip to class name (after second comma) */
                char* comma1 = strchr(start, ',');
                if (comma1) {
                    char* comma2 = strchr(comma1 + 1, ',');
                    if (comma2) {
                        comma2++;
                        while (*comma2 == ' ') comma2++;
                        
                        /* Copy class name */
                        char* end = comma2;
                        while (*end && *end != '\r' && *end != '\n' && *end != ',') end++;
                        
                        /* Trim trailing whitespace */
                        while (end > comma2 && (end[-1] == ' ' || end[-1] == '\t')) end--;
                        
                        size_t len = end - comma2;
                        result = (char*)malloc(len + 1);
                        if (result) {
                            memcpy(result, comma2, len);
                            result[len] = '\0';
                        }
                        
                        DEBUG_LOG("Found MIDlet class: '%s'", result);
                        break;
                    }
                }
            }
        }
        
        line = strtok(NULL, "\r\n");
    }
    
    free(manifest_str);   /* == unfolded (manifest itself freed above) */
    return result;
}

/* Load JAR file into memory */
static uint8_t* load_jar_file(const char* filename, size_t* out_size) {
    DEBUG_LOG("Loading JAR file: '%s'", filename);

    /* [PATHGUARD] v36.51: slab-копия + [FILEOP]-бейдж с n21 — полевые
     * InvalidAccess «es/Yeti »/«es/Bounc» появляются именно после
     * загрузки JAR; бейдж этой операции станет главным коррелятом. */
    FILE* f = nojme_pg_fopen("load:jar", filename, "rb");
    if (!f) {
        ERROR_LOG("Cannot open file: '%s'", filename);
        return NULL;
    }
    
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (size <= 0) {
        ERROR_LOG("Invalid file size: %ld", size);
        fclose(f);
        return NULL;
    }
    
    DEBUG_LOG("File size: %ld bytes", size);
    
    uint8_t* data = (uint8_t*)malloc(size);
    if (!data) {
        ERROR_LOG("Cannot allocate %ld bytes", size);
        fclose(f);
        return NULL;
    }
    
    if (fread(data, 1, size, f) != (size_t)size) {
        ERROR_LOG("Failed to read file");
        free(data);
        fclose(f);
        return NULL;
    }
    
    fclose(f);
    *out_size = size;
    
    DEBUG_LOG("JAR loaded successfully");
    return data;
}

/* Initialize emulator */
static bool init_emulator(Options* opts) {
    INFO_LOG("=== Initializing J2ME Emulator ===");
    INFO_LOG("Platform: %s", 
#ifdef _WIN32
        "Windows"
#elif defined(__linux__)
        "Linux"
#elif defined(__APPLE__)
        "macOS"
#else
        "Unknown"
#endif
    );
    INFO_LOG("Configuration: %dx%d, scale=%d, heap=%zuMB, headless=%s", 
             opts->width, opts->height, opts->scale, opts->heap_size_mb,
             opts->headless ? "yes" : "no");
    
    DEBUG_LOG("Step 1: Creating JVM instance...");
    sw_trace("init: jvm_create"); /* v34.91 */
    g_jvm = jvm_create();
    if (!g_jvm) {
        ERROR_LOG("Failed to create JVM - memory allocation failed");
        return false;
    }
    INFO_LOG("JVM instance created");
    
    /* Configure JVM */
    DEBUG_LOG("Step 2: Configuring JVM...");
    g_jvm->config.heap_size = opts->heap_size_mb * 1024 * 1024;
    g_jvm->config.stack_size = JAVA_STACK_SIZE;
    g_jvm->config.max_threads = MAX_JAVA_THREADS;
    g_jvm->config.verbose_class = opts->verbose_class;
    g_jvm->config.verbose_gc = opts->verbose_gc;
    INFO_LOG("JVM configured (heap: %zu bytes)", g_jvm->config.heap_size);
    
    /* Initialize JVM */
    DEBUG_LOG("Step 3: Initializing JVM subsystems...");
    sw_trace("init: jvm_init"); /* v34.91 */
    if (jvm_init(g_jvm) != JNI_OK) {
        ERROR_LOG("Failed to initialize JVM - jvm_init returned error");
        return false;
    }
    INFO_LOG("JVM subsystems initialized (heap, threads, class loader)");
    
    /* Initialize SDL2 backend (or headless framebuffer) */
    DEBUG_LOG("Step 4: Initializing %s...", opts->headless ? "headless mode" : "SDL backend");
    sw_trace("init: sdl_init"); /* v34.91 */
    g_sdl_ctx = malloc(sizeof(SdlContext));
    if (!g_sdl_ctx) {
        ERROR_LOG("Failed to allocate SDL context - memory allocation failed");
        return false;
    }
    memset(g_sdl_ctx, 0, sizeof(SdlContext));
    
    /* IMPORTANT: Set global context BEFORE sdl_init */
    DEBUG_LOG("Setting global SDL context: %p", (void*)g_sdl_ctx);
    sdl_set_global_context(g_sdl_ctx);
    
    if (opts->headless) {
        /* Headless mode: allocate framebuffer without SDL */
        g_sdl_ctx->framebuffer = (uint32_t*)malloc(opts->width * opts->height * sizeof(uint32_t));
        if (!g_sdl_ctx->framebuffer) {
            ERROR_LOG("Failed to allocate headless framebuffer");
            free(g_sdl_ctx);
            g_sdl_ctx = NULL;
            return false;
        }
        /* ИСПРАВЛЕНО: Инициализируем чёрным непрозрачным цветом (ARGB: 0xFF000000) */
        for (int i = 0; i < opts->width * opts->height; i++) {
            g_sdl_ctx->framebuffer[i] = 0xFF000000;
        }
        g_sdl_ctx->width = opts->width;
        g_sdl_ctx->height = opts->height;
        g_sdl_ctx->scale = opts->scale;
        g_sdl_ctx->target_fps = 30;
        g_sdl_ctx->running = true;
        g_sdl_ctx->headless = true;
        /* v34.31 (Duke Nukem 3D): Canvas.getWidth()/getHeight() must match
         * the framebuffer. midp_set_screen_dimensions was only called on
         * the libretro path, so every headless run with non-default -w/-h
         * reported a 240x320 canvas over a differently sized framebuffer
         * (game HUD/projection centers misaligned). */
        midp_set_screen_dimensions(opts->width, opts->height);
        INFO_LOG("Headless mode initialized (%dx%d)", opts->width, opts->height);
    } else {
        int sdl_result = sdl_init(g_jvm, opts->width, opts->height, opts->scale, false);
        if (sdl_result != 0) {
            ERROR_LOG("Failed to initialize SDL - sdl_init returned %d", sdl_result);
            free(g_sdl_ctx);
            g_sdl_ctx = NULL;
            return false;
        }
        
        /* Verify SDL context was properly initialized */
        if (g_sdl_ctx->target_fps <= 0 || g_sdl_ctx->framebuffer == NULL) {
            ERROR_LOG("SDL context not properly initialized after sdl_init()!");
            ERROR_LOG("  target_fps=%d, framebuffer=%p", g_sdl_ctx->target_fps, (void*)g_sdl_ctx->framebuffer);
            return false;
        }
        INFO_LOG("SDL backend initialized (%dx%d, scale: %d, fps: %d)",
                 opts->width, opts->height, opts->scale, g_sdl_ctx->target_fps);
        /* v36.05 FIX (resolution passthrough): the MIDP layer reported
         * 240x320 to the GAME on the Switch/SDL2 path forever —
         * midp_set_screen_dimensions() was only called on the headless
         * (v34.31, Duke Nukem 3D) and libretro paths. Canvas.getWidth()/
         * getHeight(), Display.getWidth/getHeight() and the paint-graphics
         * geometry all read g_midp_screen_* (the 240x320 defaults), while
         * the GameCanvas offscreen buffer was sized from the SDL context
         * (the real opts). A game therefore rendered "in its own
         * resolution" and landed in a corner of the buffer chosen by the
         * manifest hint / per-game manual resolution. One call aligns
         * every consumer on the SAME size (manual per-game resolution
         * wins; manifest hint otherwise). */
        midp_set_screen_dimensions(opts->width, opts->height);
    }
    
    g_sdl_ctx->jvm = g_jvm;
    
    /* CRITICAL FIX: Re-sync global context after framebuffer/dimensions are set.
     * sdl_set_global_context copies by value, so g_headless_ctx.framebuffer was NULL
     * from the first call before allocation. This second call makes the framebuffer
     * available to create_graphics_object() for M3G rendering. */
    sdl_set_global_context(g_sdl_ctx);
    DEBUG_LOG("Re-synced global SDL context: fb=%p, %dx%d", 
              (void*)g_sdl_ctx->framebuffer, g_sdl_ctx->width, g_sdl_ctx->height);
    
    if (opts->fullscreen) {
        sdl_set_fullscreen(g_sdl_ctx, true);
        INFO_LOG("Fullscreen mode enabled");
    }
    
    /* Initialize native methods */
    DEBUG_LOG("Step 5: Registering native methods...");
    sw_trace("init: native_init"); /* v34.91 */
    if (native_init(g_jvm) != JNI_OK) {
        ERROR_LOG("Failed to initialize native methods");
        return false;
    }
    INFO_LOG("Native methods registered (java.lang.*)");
    
    /* Initialize MIDP2 API */
    DEBUG_LOG("Step 6: Initializing MIDP2 API...");
    sw_trace("init: midp_init"); /* v34.91 */
    if (midp_init(g_jvm) != JNI_OK) {
        ERROR_LOG("Failed to initialize MIDP2 API");
        return false;
    }
    INFO_LOG("MIDP2 API initialized");
    
    /* Initialize opcodes */
    DEBUG_LOG("Step 7: Initializing opcode handlers...");
    sw_trace("init: opcodes_init"); /* v34.91 */
    opcodes_init();
    INFO_LOG("Opcode handlers initialized (256 opcodes)");
    
    INFO_LOG("=== Emulator initialized successfully ===");
    return true;
}

/* Load JAR and find/start MIDlet */
static bool run_midlet(Options* opts) {
    INFO_LOG("=== Loading MIDlet ===");
    DEBUG_LOG("run_midlet: Starting...");

    /* v36.24 [SESSION-MANIFEST-RESET]: every session starts with a CLEAN
     * property set. The process-global manifest (getAppProperty) used to
     * survive a session whose jar had no META-INF/MANIFEST.MF (or failed to
     * load) — the NEXT midlet then read the PREVIOUS game's properties
     * (wrong suite values, wrong DRM gates, wrong record-store hints).
     * Setting the new manifest below is NOT enough: a manifest-less jar
     * must yield NULL from getAppProperty, not another game's data. */
    midlet_manifest_reset();

    /* v17: enable RMS persistence for standalone SDL / headless builds.
     * (The libretro core calls midp_rms_set_save_path() itself; everyone
     * else gets a per-user default directory derived from the JAR name,
     * overridable with NOJME_RMS_DIR.) */
    {
        extern void midp_rms_default_save_path(const char* game_name);
        const char* jar_path = opts->jar_file;
        const char* base = jar_path ? strrchr(jar_path, '/') : NULL;
        const char* base2 = jar_path ? strrchr(jar_path, '\\') : NULL;
        if (base2 && (!base || base2 > base)) base = base2;
        base = base ? base + 1 : jar_path;
        char game[256] = "midlet";
        if (base && base[0]) {
            /* явная точность вместо strncpy+ручной NUL: GCC не мог
             * доказать границу (путь до 1023 байт -> буфер 256) и
             * предупреждал -Wstringop-truncation; результат идентичен */
            snprintf(game, sizeof(game), "%.255s", base);
            char* dot = strrchr(game, '.');
            if (dot && (strcasecmp(dot, ".jar") == 0 || strcasecmp(dot, ".jad") == 0)) *dot = '\0';
        }
        midp_rms_default_save_path(game);
    }

    /* Load JAR file */
    DEBUG_LOG("Step 1: Loading JAR file '%s'", opts->jar_file);
    sw_trace("jar: load"); /* v34.91: the FIRST real file read of a session */
    size_t jar_size;
    uint8_t* jar_data = load_jar_file(opts->jar_file, &jar_size);
    if (!jar_data) {
        ERROR_LOG("Failed to load JAR: %s", opts->jar_file);
        return false;
    }
    if (WILDGUARD_SKIP(jar_data, "load:jar")) { /* [WILDGUARD] v36.50 */
        /* мусорный указатель буфера JAR (полевой класс "es/Bounc" при
         * загрузке) — не читаем и не освобождаем; сессия честно падает */
        return false;
    }
    INFO_LOG("JAR loaded: %zu bytes", jar_size);
    
    /* Store JAR data globally for resource loading */
    g_jar_data = jar_data;
    g_jar_size = jar_size;
    
    /* Set JAR data in JVM class loader - enables automatic class loading from JAR */
    jvm_set_jar_data(g_jvm, jar_data, jar_size, opts->jar_file);
    
    /* Load manifest for getAppProperty support */
    sw_trace("jar: manifest"); /* v34.91 */
    size_t manifest_size;
    uint8_t* manifest = jar_find_file(jar_data, jar_size, "META-INF/MANIFEST.MF", &manifest_size);
    if (!manifest) {
        manifest = jar_find_file(jar_data, jar_size, "META-INF/manifest.mf", &manifest_size);
    }
    if (manifest) {
        INFO_LOG("Manifest loaded for getAppProperty: %zu bytes", manifest_size);
        /* v34.29 FIX: store the UNFOLDED manifest so getAppProperty returns
         * complete logical values (folded values previously re-joined with
         * an extra leading space by the continuation logic in native.c). */
        char* unfolded_props = jar_manifest_unfold(manifest, manifest_size);
        if (unfolded_props) {
            midlet_set_manifest(unfolded_props, strlen(unfolded_props));
            free(unfolded_props);
        } else {
            midlet_set_manifest((const char*)manifest, manifest_size);
        }
        free(manifest);
    } else {
        DEBUG_LOG("No manifest found in JAR");
    }
    
    /* DISABLED: Auto-generate DRM properties from JAR contents 
     * This was causing games to fail because they expect DCHOC-* properties to be NULL.
     * KEmulator returns NULL for all DCHOC-* properties and games work fine.
     * Only enable this if you have a specific game that requires auto-generated DRM.
     */
    /* midlet_generate_drm_properties(jar_data, jar_size); */
    
    /* Optional: Load JAD file for additional properties (can override auto-generated) */
    char* jad_path = strdup(opts->jar_file);
    if (jad_path) {
        /* Replace .jar with .jad */
        size_t jad_path_len = strlen(jad_path);
        if (jad_path_len > 4 && strcmp(jad_path + jad_path_len - 4, ".jar") == 0) {
            strcpy(jad_path + jad_path_len - 4, ".jad");
        } else if (jad_path_len > 4 && strcmp(jad_path + jad_path_len - 4, ".JAR") == 0) {
            strcpy(jad_path + jad_path_len - 4, ".JAD");
        } else {
            /* Append .jad */
            char* new_path = (char*)malloc(jad_path_len + 5);
            if (new_path) {
                strcpy(new_path, jad_path);
                strcat(new_path, ".jad");
                free(jad_path);
                jad_path = new_path;
            }
        }
        
        FILE* jad_file = nojme_pg_fopen("jad", jad_path, "r");
        if (jad_file) {
            fseek(jad_file, 0, SEEK_END);
            long jad_size = ftell(jad_file);
            fseek(jad_file, 0, SEEK_SET);
            
            if (jad_size > 0) {
                char* jad_data = (char*)malloc(jad_size + 1);
                if (jad_data) {
                    size_t read_size = fread(jad_data, 1, jad_size, jad_file);
                    jad_data[read_size] = '\0';
                    INFO_LOG("JAD loaded (optional): %s (%ld bytes)", jad_path, jad_size);
                    
                    /* Append JAD properties to manifest for getAppProperty */
                    midlet_append_manifest(jad_data);
                    free(jad_data);
                }
            }
            fclose(jad_file);
        }
        free(jad_path);
    }
    
    /* Find MIDlet class */
    DEBUG_LOG("Step 2: Finding MIDlet class...");
    sw_trace("class: find"); /* v34.91 */
    if (!opts->midlet_class) {
        DEBUG_LOG("No MIDlet class specified, searching manifest...");
        char* found = find_midlet_class(jar_data, jar_size);
        if (found) {
            opts->midlet_class = found;
            INFO_LOG("Found MIDlet class in manifest: %s", opts->midlet_class);
        } else {
            ERROR_LOG("No MIDlet class specified or found in manifest");
            /* v35.12: jar ownership moved to the session teardown (the Switch
             * loop frees g_jar_data after cleanup()) — freeing here left the
             * global pointer dangling and double-freed on relaunch paths. */
            return false;
        }
    } else {
        INFO_LOG("Using specified MIDlet class: %s", opts->midlet_class);
    }
    
    /* Convert class name from dot notation to slash notation */
    char* class_name = strdup(opts->midlet_class);
    for (char* p = class_name; *p; p++) {
        if (*p == '.') *p = '/';
    }
    
    DEBUG_LOG("Step 3: Loading main class '%s'...", class_name);
    sw_trace("class: load %s", class_name); /* v34.91 */
    
    /* Load the main class - now uses jvm_load_class which checks JAR */
    JavaClass* main_class = jvm_load_class(g_jvm, class_name);
    if (!main_class) {
        ERROR_LOG("Failed to load class: %s", class_name);
        free(class_name);
        /* v35.12: jar freed by the session teardown now (see above). */
        return false;
    }
    INFO_LOG("Main class loaded: %s (version %d.%d, %d methods)", 
             main_class->class_name ? main_class->class_name : "(unnamed)",
             main_class->major_version, main_class->minor_version,
             main_class->methods_count);
    /* v36.12 (malloc-census find): the dot->slash conversion string is only
     * needed for the load call — on the success path it used to live on
     * until the process died (one block per session). */
    free(class_name);
    class_name = NULL;
    
    /* Dump class info if verbose */
    if (opts->verbose_class) {
        jvm_dump_class(main_class);
    }
    
    /* Execute the MIDlet */
    INFO_LOG("Step 4: Starting MIDlet execution...");
    sw_trace("midlet: start"); /* v34.91 */
    /* v41 torn-heap fix: the main thread executes Java HERE (and later in
     * the sdl_run pump) — bracket both windows so runner-triggered GCs
     * can park it (see execute.c). */
    {
        extern void jvm_main_thread_exec_begin(void);
        extern void jvm_main_thread_exec_end(void);
        jvm_main_thread_exec_begin();
        int result = jvm_run_midlet(g_jvm, main_class);
        jvm_main_thread_exec_end();
    
    if (result != 0) {
        ERROR_LOG("MIDlet execution failed with code %d", result);
        
        /* Check for pending exception and display error screen */
        JavaThread* main_thread = jvm_current_thread(g_jvm);
        if (main_thread && main_thread->pending_exception) {
            JavaObject* exception = main_thread->pending_exception;
            JavaClass* exc_class = exception->header.clazz;
            
            /* Get exception class name */
            const char* exc_name = exc_class ? exc_class->class_name : "Unknown Exception";
            
            /* Try to get exception message from detailMessage field */
            char message[512] = {0};
            char stack_trace[2048] = {0};
            
            /* Look for detailMessage field in exception object */
            if (exc_class && exc_class->fields) {
                for (uint16_t i = 0; i < exc_class->fields_count; i++) {
                    JavaField* field = &exc_class->fields[i];
                    if (field->name && strcmp(field->name, "detailMessage") == 0) {
                        /* Found message field - get its value */
                        JavaValue* field_val = (JavaValue*)((uint8_t*)exception + sizeof(ObjectHeader) + i * sizeof(JavaValue));
                        if (field_val && field_val->ref) {
                            /* It's a String object - get its UTF8 value */
                            JavaObject* str_obj = (JavaObject*)field_val->ref;
                            if (str_obj->header.clazz && str_obj->header.clazz->class_name &&
                                strcmp(str_obj->header.clazz->class_name, "java/lang/String") == 0) {
                                /* Try to get the char array */
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
            
            /* Build stack trace from current frame */
            if (main_thread->current_frame) {
                JavaFrame* frame = main_thread->current_frame;
                int pos = 0;
                int frame_count = 0;
                
                while (frame && frame_count < 15) {
                    const char* cls_name = frame->clazz && frame->clazz->class_name ? frame->clazz->class_name : "?";
                    const char* method_name = frame->method && frame->method->name ? frame->method->name : "?";
                    
                    int written = snprintf(stack_trace + pos, sizeof(stack_trace) - pos - 1,
                            "  at %s.%s (PC=%d)\n", cls_name, method_name, frame->throwing_pc);
                    if (written > 0 && pos + written < (int)sizeof(stack_trace) - 1) {
                        pos += written;
                    }
                    
                    frame = frame->prev;
                    frame_count++;
                }
            }
            
            /* Set error info for display */
            sdl_set_error_info(exc_name, message[0] ? message : NULL, stack_trace[0] ? stack_trace : NULL);
            
            INFO_LOG("Displaying error screen for: %s: %s", exc_name, message);
        } else {
            /* No exception object, just show generic error */
            sdl_set_error_info("Execution Failed", "MIDlet returned error code", NULL);
        }
        
        /* Don't exit - keep running to show error screen */
        /* Fall through to main loop which will display the error */
    } else {
        INFO_LOG("MIDlet started successfully");
    }
    }  /* end v41 main-thread exec window (jvm_run_midlet) */

    /* Run SDL main loop or headless execution */
    INFO_LOG("Step 5: Entering main event loop...");
    DEBUG_LOG("Starting main loop...");
    DEBUG_LOG("g_sdl_ctx: %p, target_fps: %d, framebuffer: %p", 
              (void*)g_sdl_ctx, g_sdl_ctx ? g_sdl_ctx->target_fps : -1,
              g_sdl_ctx ? (void*)g_sdl_ctx->framebuffer : NULL);
    
    /* CRITICAL DEBUG: Check running status BEFORE sdl_run */
    LOG_SAFE("[MAIN] BEFORE sdl_run: g_sdl_ctx->running=%d, g_jvm->running=%d\n",
            g_sdl_ctx ? g_sdl_ctx->running : -1,
            g_jvm ? g_jvm->running : -1);
    
    /* ИСПРАВЛЕНО: Force running to true if needed */
    if (g_sdl_ctx && !g_sdl_ctx->running) {
        LOG_SAFE("[MAIN] WARNING: g_sdl_ctx->running was false, forcing to true\n");
        g_sdl_ctx->running = true;
    }
    /* v34.72: the old unconditional force-true masked a MIDlet that had
     * ALREADY finished inside startApp/constructor (notifyDestroyed() or
     * System.exit set running=false) — the app then sat on a frozen last
     * frame forever, indistinguishable from a hang. Only revive a VM that
     * is not deliberately finished; the finished one falls through to the
     * explicit black "MIDlet finished" screen. */
    if (g_jvm && !g_jvm->running && !g_jvm->exiting && !midlet_is_destroyed()) {
        LOG_SAFE("[MAIN] WARNING: g_jvm->running was false, forcing to true\n");
        g_jvm->running = true;
    }
    
    LOG_SAFE("[MAIN] AFTER fix: g_sdl_ctx->running=%d, g_jvm->running=%d\n",
            g_sdl_ctx ? g_sdl_ctx->running : -1,
            g_jvm ? g_jvm->running : -1);
    
    if (!g_sdl_ctx || g_sdl_ctx->target_fps <= 0) {
        ERROR_LOG("Context invalid, cannot run main loop");
        free(class_name);
        /* v35.12: jar freed by the session teardown now (see above). */
        return false;
    }
    
    /* v34.26 BENCH: wall-clock + instruction telemetry around the main loop
     * (always-on, two lines; used to measure real interpreter throughput). */
    {
        struct timespec b_ts;
        clock_gettime(CLOCK_MONOTONIC, &b_ts);
        uint64_t b_start_ms = (uint64_t)b_ts.tv_sec * 1000U + (uint64_t)b_ts.tv_nsec / 1000000U;
        g_bench_start_ms = b_start_ms;
    }
    sdl_run(g_sdl_ctx);
    {
        extern void jvm_main_thread_exec_end(void);
        jvm_main_thread_exec_end();
    }
    {
        extern JVM* g_jvm;
        if (g_jvm) {
            struct timespec b_ts;
            clock_gettime(CLOCK_MONOTONIC, &b_ts);
            uint64_t b_end_ms = (uint64_t)b_ts.tv_sec * 1000U + (uint64_t)b_ts.tv_nsec / 1000000U;
            /* v34.26: plain fprintf is intercepted by the log gate — use the
             * always-on channel so the telemetry survives disabled logging.
             * The calibration counters are weak: they exist only in builds
             * with the batched-interpreter execute.c. */
            {
                extern uint64_t g_slowcheck_fires __attribute__((weak));
                extern uint64_t g_prof_est_instr __attribute__((weak));
                uint64_t fires = (&g_slowcheck_fires) ? g_slowcheck_fires : 0;
                uint64_t est = (&g_prof_est_instr) ? g_prof_est_instr : 0;
                ALWAYS_LOG("[BENCH] main loop: %llu ms, instructions=%llu, rate=%.0f K/s | slowcheck_fires=%llu est=%llu (fires*64)\n",
                        (unsigned long long)(b_end_ms - g_bench_start_ms),
                        (unsigned long long)g_jvm->instr_count,
                        g_bench_start_ms ? (double)g_jvm->instr_count / (double)(b_end_ms - g_bench_start_ms) : 0.0,
                        (unsigned long long)fires,
                        (unsigned long long)est);
            }
        }
    }

    free(class_name);
    /* v35.12: jar freed by the session teardown now (Switch loop / desktop
     * main) — run_midlet used to free it here while g_jar_data kept the
     * same pointer, so the next free() of the global would double-free. */
    
    INFO_LOG("=== MIDlet finished ===");
    return true;
}

/* Cleanup */
/* v36.48 [EXIT-RELEASE-ALL]: гарантия ПОЛНОГО освобождения ресурсов при
 * выходе из мидлета — даже когда Java-поток застрял и нормальный teardown
 * невозможен. Все аварийные пути (cleanup() с занятым потоком, «flap»-случай
 * jvm_destroy в цикле сессий) сходятся сюда вместо возврата в меню с
 * утёкшей VM (именно та утечка кормила OOM/«белый экран» повторного
 * запуска в полевом логе v36.46: после teardown не возвращалось ~155 МБ).
 * Что делает:
 *   1) лучший-усилие СБРОС RMS (прогресс игры!) — в потоке-помощнике со
 *      сторожевым таймаутом 1.5 с: застрявший поток мог оставить rms-мьютекс
 *      занятым, обычный flush дедлочил бы выход; помощник умирает вместе с
 *      процессом, дедлока нет НИКОГДА;
 *   2) svcExitProcess() — на HOS убивает ВСЕ потоки атомно с адресным
 *      пространством: ОС возвращает ВСЮ память (хип VM, текстуры, стеки),
 *      ни один поток не исполняет разэмапленный код (класс 2168-0001).
 *      На хосте то же делает _exit(0). Стоимость: консоль оказывается в
 *      HOME/hbmenu вместо меню эмулятора — честная плата за гарантию
 *      чистого следующего запуска в ПАТОЛОГИЧЕСКОМ случае (обычный выход
 *      из мидлета идёт штатным путём teardown T0..T3). */
static volatile int s_bailout_rms_done = 0;

static void* bailout_rms_flush_thread(void* arg) {
    (void)arg;
    /* midp_rms_session_reset = rms_flush_dirty_stores + сброс enum/listener
     * слотов; корни GC трогать безопасно — процесс всё равно умирает. */
    extern void midp_rms_session_reset(void) __attribute__((weak));
    if (midp_rms_session_reset) midp_rms_session_reset();
    s_bailout_rms_done = 1;
    return NULL;
}

static void nojme_bailout_exit(const char* reason) {
    /* sw_trace_force: прототип или no-op из switch_trace.h (v36.48:
     * локальный weak-extern давал -Wattributes на новых gcc). */
    sw_trace_force("[EXIT-RELEASE-ALL] %s", reason);
    fprintf(stderr, "[EXIT-RELEASE-ALL] %s\n", reason);
    fflush(stderr);

    /* 1) RMS: сброс незаписанных сторов, ограничен 1.5 с */
    s_bailout_rms_done = 0;
    pthread_t fl;
    int flush_started = (pthread_create(&fl, NULL, bailout_rms_flush_thread,
                                        NULL) == 0);
    if (flush_started) {
        for (int i = 0; i < 150 && !s_bailout_rms_done; i++) {
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 10 * 1000000L };
            nanosleep(&ts, NULL);
        }
    }
    sw_trace_force("[EXIT-RELEASE-ALL] rms flush %s",
                   (!flush_started || !s_bailout_rms_done)
                       ? "SKIPPED/TIMEOUT (запись могла не доехать)"
                       : "done");

    /* -pg сборки: профильные счётчики на диск до сырого выхода */
    {
        extern void _mcleanup(void) __attribute__((weak));
        if (_mcleanup) _mcleanup();
    }

#if defined(__SWITCH__) && __has_include(<switch.h>)
    /* HOS: ExitProcess-svc без параметров (v36.10); убивает все потоки
     * атомно — никакой код NRO не исполняется после. */
    svcExitProcess();
#endif
    _exit(0);
}

static void cleanup(void) {
    DEBUG_LOG("Cleaning up...");

    /* v36.48 [DUMP-FENCE]: параноидальная очистка и здесь — init-fail путь
     * и desktop-сборки идут через cleanup() без switch-цикла. */
    if (sw_diag_set_jvm) sw_diag_set_jvm(NULL);

    /* FIX(shutdown-race): if a real-pthread Java thread is still mid-run(),
     * tearing down the heap/JVM beneath it segfaults nondeterministically.
     * Exit the process with resources intact instead - the OS reclaims them
     * atomically with thread death.
     * v36.24 BUSY-TEARDOWN-EXIT (the field report: "если при запуске
     * происходит ошибка, или запуск прерван на этапе загрузки — повторный
     * запуск мидлета приводит к вылету; какие-то ресурсы остаются заняты"):
     * the raw _exit(0) here is WRONG on HOS — _exit(0) from an NRO does NOT
     * kill the process, it unmaps the module and returns to hbmenu with the
     * wedged runner STILL EXECUTING and the whole VM arena still mapped
     * (v36.09 field story: "~200+ MB of leaked per-session arena... the
     * next NRO launch inherits that wreckage and Data-Aborts"). In applet
     * mode the relaunch runs in the SAME process — it opens straight into
     * the wreckage (a live runner mid-interpret + a poisoned malloc view)
     * and crashes: the "повторный запуск приводит к вылету" report.
     * Fix, mirroring the v36.07 main-exit guard: one bounded extra grace
     * (a runner inside a long native may still finish); if threads are
     * STILL busy after it, terminate the whole PROCESS (svcExitProcess) so
     * every thread dies atomically with the address space and the next
     * launch starts from a clean process. On the host sandbox there is no
     * libnx: _exit(0) really ends the process, same as before. */
    {
        extern bool vm_threads_busy(void);
        if (vm_threads_busy()) {
            int waited_ms = 0;
            fprintf(stderr, "[EXIT] Java threads still active; extra grace before process exit\n");
            fflush(stderr);
            while (vm_threads_busy() && waited_ms < 3000) {
                struct timespec ts = { .tv_sec = 0, .tv_nsec = 50 * 1000000L };
                nanosleep(&ts, NULL);
                waited_ms += 50;
            }
            if (vm_threads_busy()) {
                /* v34.42 PERF-DIAG: -pg binaries must flush their profile
                 * counters before the raw exit — gmon.out is written by
                 * _mcleanup(). Weak symbol: no-op link when built without
                 * -pg. */
                {
                    extern void _mcleanup(void) __attribute__((weak));
                    if (_mcleanup) _mcleanup();
                }
                /* v36.48 [EXIT-RELEASE-ALL]: сходится в общий аварийный
                 * выход (RMS-сброс со сторожем + svcExitProcess) вместо
                 * сырого _exit — прогресс игры сохраняется лучшим усилием. */
                nojme_bailout_exit("cleanup: Java thread(s) STILL busy after "
                                   "extra 3s — process exit (full release)");
            }
            fprintf(stderr, "[EXIT] threads settled during grace — normal teardown\n");
            fflush(stderr);
        }
    }
    
    if (g_sdl_ctx) {
        sdl_destroy(g_sdl_ctx);
        free(g_sdl_ctx);
        g_sdl_ctx = NULL;
    }
    
    if (g_jvm) {
        jvm_destroy(g_jvm);
        g_jvm = NULL;
    }
    
    DEBUG_LOG("Cleanup complete");
}

/* Main entry point */
/* v29 DIAG: crash backtrace dump (headless debugging aid).
 * On SIGSEGV/SIGBUS/SIGFPE print a raw backtrace so the faulting frames can
 * be symbolized with addr2line; restores the default handler and re-raises.
 * v34.24: J2ME_HAVE_EXECINFO (probe above) — musl and some cross
 * toolchains have no execinfo.h; the guard previously accepted any
 * __linux__, breaking musl builds. MinGW uses SEH anyway. */
#if J2ME_HAVE_EXECINFO
__attribute__((unused)) static void j2me_crash_backtrace(int sig) {
    void* frames[64];
    int n = backtrace(frames, 64);
    /* v34.24: ALWAYS_LOG — crash backtraces must ALWAYS reach stderr. */
    ALWAYS_LOG("\n*** FATAL signal %d, backtrace (%d frames):\n", sig, n);
    backtrace_symbols_fd(frames, n, 2);
    /* Restore default and re-raise for a real core */
    signal(sig, SIG_DFL);
    raise(sig);
}
#define J2ME_CRASH_HANDLER 1
#else
#define J2ME_CRASH_HANDLER 0
#endif

#ifndef __SWITCH__
int main(int argc, char** argv) {
    /* v38: [V37-DIAG] main() breadcrumb removed (quiet-by-default). */
    fflush(stderr);
    /* v34.24: standalone NEON A/B self-test (env-gated; runs and exits
     * before any JAR parsing). NOJME_NEON_SELFTEST=1 j2me-headless [jar] */
    if (getenv("NOJME_NEON_SELFTEST")) {
        int st_fails = render_neon_selftest();
        if (strcmp(getenv("NOJME_NEON_SELFTEST"), "run") != 0) {
            return st_fails ? 1 : 0;
        }
    }

    /* v34.28: standalone 2D micro-benchmark (env-gated). NOJME_2D_BENCH=1
     * j2me-headless — times every optimized 2D primitive scalar vs fast. */
    if (getenv("NOJME_2D_BENCH")) {
        nojme_2d_bench();
        if (strcmp(getenv("NOJME_2D_BENCH"), "run") != 0) {
            return 0;
        }
    }

    /* Initialize logging mutex FIRST */
    log_mutex_init();

#if J2ME_CRASH_HANDLER
    signal(SIGSEGV, j2me_crash_backtrace);
    signal(SIGBUS,  j2me_crash_backtrace);
    signal(SIGFPE,  j2me_crash_backtrace);
#endif

    /* Immediate output for MinGW debugging */
    LOG_SAFE("=== J2ME Emulator v%s ===\n", J2ME_EMULATOR_VERSION);
    LOG_SAFE("Core build: %s (Corax89)\n", CORE_BUILD_ID);
    LOG_SAFE("Platform: ");
#ifdef _WIN32
    LOG_SAFE("Windows");
#ifdef __MINGW32__
    LOG_SAFE(" (MinGW)");
#endif
#elif defined(__linux__)
    LOG_SAFE("Linux");
#elif defined(__APPLE__)
    LOG_SAFE("macOS");
#else
    LOG_SAFE("Unknown");
#endif
    LOG_SAFE("\n");
    
    /* Also print to stdout */
    printf("=== J2ME Emulator v%s ===\n", J2ME_EMULATOR_VERSION);
    printf("Platform: ");
#ifdef _WIN32
    printf("Windows");
#ifdef __MINGW32__
    printf(" (MinGW)");
#endif
#elif defined(__linux__)
    printf("Linux");
#elif defined(__APPLE__)
    printf("macOS");
#else
    printf("Unknown");
#endif
    printf("\n");
    fflush(stdout);
    
    DEBUG_LOG("=== J2ME Emulator Starting ===");
    DEBUG_LOG("argc: %d", argc);
    for (int i = 0; i < argc; i++) {
        DEBUG_LOG("argv[%d]: '%s'", i, argv[i]);
    }
    
    Options opts;
    
    /* Parse arguments */
    if (!parse_args(argc, argv, &opts)) {
        print_usage(argv[0]);
        return 1;
    }
    
    DEBUG_LOG("JAR file: %s", opts.jar_file);
    DEBUG_LOG("MIDlet class: %s", opts.midlet_class ? opts.midlet_class : "(auto-detect)");
    
    /* Enable debug mode if verbose flag is set */
    LOG_SAFE("[DEBUG] opts.verbose = %d\n", opts.verbose);
    if (opts.verbose) {
        g_j2me_runtime_debug = 1;
        LOG_SAFE("[J2ME] Debug mode enabled via --verbose flag\n");
    }
    /* v36.28: dedicated media trace gate (sandbox/on-device diagnostics):
     * NOJME_MEDIA_DEBUG=1 enables the [MEDIA] player-lifecycle log WITHOUT
     * the heavy verbose [EXEC]/[OP] firehose. */
    {
        const char* md = getenv("NOJME_MEDIA_DEBUG");
        if (md && md[0] && strcmp(md, "0") != 0) g_nojme_media_debug = 1;
    }
    
    /* Check if JAR file exists */
    FILE* jar_test = nojme_pg_fopen("jar-test", opts.jar_file, "rb");
    if (!jar_test) {
        ERROR_LOG("JAR file not found: %s", opts.jar_file);
        ERROR_LOG("Current directory: ");
#ifdef _WIN32
        system("cd");
#else
        system("pwd");
#endif
        return 1;
    }
    fclose(jar_test);
    DEBUG_LOG("JAR file exists and is readable");
    
    /* v34.46 (Treasure Towers): auto screen size from the JAR manifest
     * hint (MIDxlet-Application-Range / Nokia-MIDlet-Original-Display-Size)
     * when the caller did not pass an explicit -w/-h. Fixed-resolution
     * builds refuse any other canvas ("Error! Cannot start the game."),
     * so the default 240x320 blocks them; hint-less JARs keep 240x320.
     * This mirrors the libretro core's "auto" resolution mode. */
    if (!opts.explicit_size) {
        size_t peek_size = 0;
        uint8_t* peek = load_jar_file(opts.jar_file, &peek_size);
        if (peek) {
            int hint_w = 0, hint_h = 0;
            if (jar_detect_screen_size(peek, peek_size, &hint_w, &hint_h)) {
                opts.width = hint_w;
                opts.height = hint_h;
                LOG_SAFE("[J2ME] Auto resolution: manifest hint -> %dx%d\n",
                        hint_w, hint_h);
            } else if (jar_detect_screen_size_from_name(opts.jar_file,
                                                        &hint_w, &hint_h)) {
                /* v35.12: manifest carries no display-size attribute — try
                 * the file name ("Game 240x320.jar"). */
                opts.width = hint_w;
                opts.height = hint_h;
                LOG_SAFE("[J2ME] Auto resolution: file name hint -> %dx%d\n",
                        hint_w, hint_h);
            }
            free(peek);
        }
    }
    
    /* Print banner */
    printf("\n");
    printf("╔═══════════════════════════════════════════════════════════╗\n");
    printf("║           J2ME Emulator - MIDP2 Mobile Java               ║\n");
    printf("║                    Version %s                          ║\n", J2ME_EMULATOR_VERSION);
    printf("╠═══════════════════════════════════════════════════════════╣\n");
    printf("║  SDL2 Graphics  │  MIDP2 API  │  Full Opcode Support     ║\n");
    printf("╠═══════════════════════════════════════════════════════════╣\n");
    printf("║  Press F12 to toggle debug mode at runtime                ║\n");
    printf("╚═══════════════════════════════════════════════════════════╝\n");
    printf("\n");
    
    /* Print debug mode status */
    LOG_SAFE("[J2ME] Debug mode: %s (Press F12 to toggle)\n", 
            g_j2me_runtime_debug ? "ON" : "OFF");
    
    /* Install signal handlers */
#ifdef _WIN32
    /* Windows uses signal() directly */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
#else
    /* POSIX systems */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);
#endif
    
    /* Initialize */
    if (!init_emulator(&opts)) {
        ERROR_LOG("Failed to initialize emulator");
        cleanup();
        return 1;
    }
    
    /* Run MIDlet */
    bool success = run_midlet(&opts);
    
    /* Cleanup */
    cleanup();
    /* v35.12: jar ownership lives with the session teardown now. */
    if (g_jar_data) { free(g_jar_data); g_jar_data = NULL; g_jar_size = 0; }
    
    DEBUG_LOG("=== J2ME Emulator Exiting (success: %d) ===", success);
    return success ? 0 : 1;
}

#else /* __SWITCH__ — Nintendo Switch frontend (v34.84) */

/* Core build id for the menu header (switch_ui.c). */
const char* j2me_core_build_id(void) { return CORE_BUILD_ID; }

/*
 * Switch entry point: no command line — an in-app menu (game browser +
 * settings, src/switch/switch_ui.c) drives everything. Each selected JAR
 * runs a full emulator session (init_emulator -> run_midlet -> teardown)
 * on the SHARED SDL window; after the game ends control returns to the
 * menu so the user can open another game without restarting the app.
 */
int main(int argc, char** argv) {
    (void)argc; (void)argv;
    fflush(stderr);
    log_mutex_init();

    if (getenv("NOJME_NEON_SELFTEST")) {
        int st_fails = render_neon_selftest();
        if (strcmp(getenv("NOJME_NEON_SELFTEST"), "run") != 0) {
            return st_fails ? 1 : 0;
        }
    }

    LOG_SAFE("=== J2ME Emulator (Switch) — core %s ===\n", CORE_BUILD_ID);

    /* v36.48 [BUILD-BANNER-STDOUT]: тот же баннер в STDOUT одной строкой.
     * Зачем: stderr на устройстве невидим (известный класс бага [YSFIX2]),
     * а Ryu/Ryujinx показывают stdout homebrew в СВОЁМ логе — строка
     * "nojme-core <id>" рядом с InvalidAccess-ошибками сразу доказывает,
     * какая сборка реально загружена (диагностика «лог от старого NRO»). */
    printf("nojme-core %s\n", CORE_BUILD_ID);
    fflush(stdout);

    /* v36.28: dedicated media trace gate (sandbox/on-device diagnostics):
     * NOJME_MEDIA_DEBUG=1 enables the [MEDIA] player-lifecycle log WITHOUT
     * the heavy verbose [EXEC]/[OP] firehose. Must live in THIS entry —
     * the sandbox/Switch session loop never runs the CLI main() below. */
    {
        const char* md = getenv("NOJME_MEDIA_DEBUG");
        if (md && md[0] && strcmp(md, "0") != 0) g_nojme_media_debug = 1;
    }

    if (sdl_switch_platform_init() != 0) {
        LOG_SAFE("[SWITCH] platform init failed, exiting\n");
        return 1;
    }

#ifdef __SWITCH__
    /* v34.92: default layout — games and saves live under sdmc:/switch/j2me.
     * v35.13: the automatic legacy migration (v35.12) was REMOVED at the
     * user's request — defaults pointing at switch/j2me are enough; no
     * startup moves/renames happen anymore. Create both dirs eagerly so
     * the browser has a sane start and RMS writes never fail on a fresh
     * card. */
    {
        const SwitchSettings* st0 = switch_settings_get();
        nojme_pg_mkdir("mkdir-sw", "sdmc:/switch", 0777);
        nojme_pg_mkdir("mkdir-j2me", "sdmc:/switch/j2me", 0777);
        nojme_pg_mkdir("mkdir-games", st0->games_dir[0] ? st0->games_dir : "sdmc:/switch/j2me/games", 0777);
        nojme_pg_mkdir("mkdir-saves", st0->saves_dir[0] ? st0->saves_dir : "sdmc:/switch/j2me/saves", 0777);
        /* v36.58 [DIAG-FLAGS]: the env-gated diagnostic instruments are
         * host-only by construction (HOS has no env). Empty flag FILES on
         * the SD bridge them: create the file, relaunch, the instrument
         * runs for the whole session. Remove the file to switch off.
         *   textlog.flag  -> NOJME_TEXTLOG=1 (every drawString -> log.txt)
         *   snap.flag     -> NOJME_SNAP_DIR=sdmc:/switch/j2me/snaps,ALL
         *                    (every GameCanvas frame -> frame_N.ppm)
         *   imm.flag      -> NOJME_IMM_TRACE=1 (immediate M3G renders)
         *   selfstack.flag-> NOJME_SELF_STACK_EVERY=2000 (Java stacks)
         *   race.flag     -> NOJME_RACE_PROBE=1 (v36.59 [RACE]: значения
         *                    setViewport/setClip, каждый bindTarget/сид/
         *                    немедленный render/releaseTarget, итог кадра
         *                    на flushGraphics — разбор «гонка Rally 3D:
         *                    только машина»)
         *   neon_scalar.flag -> NOJME_NEON_SCALAR=1 (A/B NEON-растерайзера:
         *                    если с флагом сцена появляется — баг в NEON)
         *   pg.flag       -> NOJME_PG_SCAN=1 (v36.60 [PSCAN-FLAG]: PTRSCAN-
         *                    проходы прибора PATHGUARD — только по флагу;
         *                    без него нулевая цена и нулевой спам.
         *                    Включать, если вернётся InvalidAccess) */
        {
            static const struct { const char* flag; const char* var; const char* val; } k_diag[] = {
                { "sdmc:/switch/j2me/textlog.flag",   "NOJME_TEXTLOG",          "1" },
                { "sdmc:/switch/j2me/imm.flag",       "NOJME_IMM_TRACE",        "1" },
                { "sdmc:/switch/j2me/selfstack.flag", "NOJME_SELF_STACK_EVERY", "2000" },
                { "sdmc:/switch/j2me/snap.flag",      "NOJME_SNAP_DIR",
                  "sdmc:/switch/j2me/snaps,ALL" },
                { "sdmc:/switch/j2me/race.flag",      "NOJME_RACE_PROBE",       "1" },
                { "sdmc:/switch/j2me/neon_scalar.flag", "NOJME_NEON_SCALAR",    "1" },
                { "sdmc:/switch/j2me/pg.flag",          "NOJME_PG_SCAN",          "1" },
            };
            for (size_t i = 0; i < sizeof(k_diag) / sizeof(k_diag[0]); i++) {
                FILE* probe = fopen(k_diag[i].flag, "rb");
                if (probe) {
                    fclose(probe);
                    setenv(k_diag[i].var, k_diag[i].val, 1);
                    LOG_SAFE("[DIAG-FLAG] %s present -> %s=%s\n",
                             k_diag[i].flag, k_diag[i].var, k_diag[i].val);
                }
            }
            /* SNAP needs its output dir */
            {
                FILE* probe = fopen("sdmc:/switch/j2me/snap.flag", "rb");
                if (probe) {
                    fclose(probe);
                    nojme_pg_mkdir("mkdir-snaps", "sdmc:/switch/j2me/snaps", 0777);
                }
            }
        }
    }
#endif

    /* v34.91 freeze triage: breadcrumbs + sdmc:/switch/j2me/log.txt + a
     * 2-second "alive:" heartbeat (SDL timer thread). On an emulator the
     * stderr is invisible; the log file survives hangs and crashes. */
    sw_trace_heartbeat();

    for (;;) {
        char jar_path[1024];
        nojme_pg_watch("main:jar_path", jar_path, sizeof(jar_path)); /* [PATHGUARD] v36.51 */
        int pg_vm_speed = -1; /* v35.09: per-game VM speed override (-1 inherit) */
        if (!switch_ui_pick_game(jar_path, sizeof(jar_path))) {
            LOG_SAFE("[SWITCH] menu exit — shutting down\n");
            break;
        }

        Options opts;
        memset(&opts, 0, sizeof(opts));
        opts.width = MIDP_DEFAULT_WIDTH;
        opts.height = MIDP_DEFAULT_HEIGHT;
        opts.explicit_size = false;
        opts.scale = 1; /* presentation is rect-based on the fixed screen */
        opts.heap_size_mb = 64;
        opts.jar_file = jar_path;

        /* v34.92: RMS saves go to sdmc:/switch/j2me/saves (settings
         * "saves_dir"); NOJME_RMS_DIR wins inside midp_rms_default_save_path. */
        {
            const SwitchSettings* st = switch_settings_get();
            if (st->saves_dir[0]) setenv("NOJME_RMS_DIR", st->saves_dir, 1);
        }

        /* v34.94: per-game settings (PLUS overlay) — resolution override
         * replaces the manifest hint; scale/filter overrides feed the
         * present path; remember the jar for the in-game PLUS menu. */
        {
            extern char g_switch_current_jar[1024];
            extern int g_pg_scale;
            extern int g_pg_filter;
            extern int g_pg_rotation; /* v36.15: defined in sdl_graphics.c */
            extern int g_pg_input_swap; /* v36.37: defined in sdl_graphics.c */
            extern int g_pg_flip;     /* v36.16: defined in sdl_graphics.c */
            extern int g_nojme_flip_mode; /* v35.03: defined in mobile3d.c */
            SwitchPerGameSettings pg;
            switch_pergame_load(jar_path, &pg);
            snprintf(g_switch_current_jar, sizeof(g_switch_current_jar), "%s", jar_path);
            g_pg_scale = pg.scale_mode;
            g_pg_filter = pg.filter;
            /* v36.15: per-game поворот экрана (презентационный, 0/1/2).
             * Печатается в геометрию сессии при game_begin (текстуры под
             * повёрнутый кадр) — применяется с этого запуска игры. */
            g_pg_rotation = (pg.rotation == 1 || pg.rotation == 2) ? pg.rotation : 0;
            if (g_pg_rotation)
                LOG_SAFE("[SWITCH] per-game rotation: %s (canvas unchanged, presentation rotated)\n",
                         g_pg_rotation == 1 ? "90 right" : "90 left");
            /* v36.37: per-game стик/D-pad swap (-1 = глобальная настройка).
             * Читается при каждом событии стика/крестовины, поэтому
             * достаточно проставить здесь один раз; PLUS-меню обновляет
             * ЖИВО. Дефолт: стик — цифры 2/4/6/8, крестовина — стрелки. */
            g_pg_input_swap = pg.input_swap;
            {
                const SwitchSettings* std_ = switch_settings_get();
                int eff_swap = (g_pg_input_swap >= 0) ? g_pg_input_swap
                                                      : std_->input_swap;
                LOG_SAFE("[SWITCH] stick/dpad layout: %s (per-game=%d global=%d)\n",
                         eff_swap ? "stick=arrows dpad=digits 2/4/6/8"
                                  : "stick=digits 2/4/6/8 dpad=arrows",
                         pg.input_swap, std_->input_swap);
                LOG_SAFE("[SWITCH] stick dead zone: %d%%\n",
                         std_->stick_deadzone);
            }
            /* v35.03: эффективный режим глобального флипа 3D — per-game
             * override побеждает глобальную настройку; наследуется в M3G
             * через g_nojme_flip_mode (0=выкл, 1=авто, 2=всегда).
             * v36.16: режим запоминается ещё и в g_pg_flip (per-game
             * кэш), и дальше ПЕРЕСЧИТЫВАЕТСЯ живо в каждом present
             * (sdl_graphics.c): смена флипа в PLUS применяется без
             * перезапуска игры. Запись ниже — начальное значение до
             * первого present. */
            {
                const SwitchSettings* stf = switch_settings_get();
                g_pg_flip = pg.flip_mode;
                g_nojme_flip_mode = (g_pg_flip >= 0) ? g_pg_flip : stf->flip_mode;
                LOG_SAFE("[SWITCH] global 3D flip: %s (per-game=%d global=%d)\n",
                         g_nojme_flip_mode == NOJME_FLIP_ON  ? "on" :
                         g_nojme_flip_mode == NOJME_FLIP_OFF ? "off" : "auto",
                         pg.flip_mode, stf->flip_mode);
                /* v36.11: сглаживание M3G-текстур — per-game override
                 * побеждает глобальную настройку; 0=авто (как в v34.78),
                 * 1=принудительный nearest, 2=принудительный bilinear.
                 * PLUS-оверлей может переопределить на лету (sel==5). */
                {
                    extern int g_m3g_texture_filter_override; /* render.c */
                    g_m3g_texture_filter_override =
                        (pg.tex_filter >= 0) ? pg.tex_filter : stf->tex_filter;
                    LOG_SAFE("[SWITCH] m3g texture filter: %s (per-game=%d global=%d)\n",
                             g_m3g_texture_filter_override == NOJME_TEXF_SMOOTH  ? "smooth" :
                             g_m3g_texture_filter_override == NOJME_TEXF_NEAREST ? "nearest" : "auto",
                             pg.tex_filter, stf->tex_filter);
                }
            }
            if (pg.res_mode) {
                opts.width = pg.res_w;
                opts.height = pg.res_h;
                opts.explicit_size = true;
                LOG_SAFE("[SWITCH] per-game resolution: %dx%d\n", pg.res_w, pg.res_h);
            }
            pg_vm_speed = pg.vm_speed; /* v35.09: carried to the budget block */
        }

        /* manifest resolution hint (mirrors the desktop main); v34.94: a
         * per-game resolution override WINS over the manifest hint. */
        {
            sw_trace("jar: peek"); /* v34.91: the pre-init manifest read */
            size_t peek_size = 0;
            uint8_t* peek = load_jar_file(opts.jar_file, &peek_size);
            if (peek && WILDGUARD_SKIP(peek, "load:peek")) peek = NULL; /* [WILDGUARD] v36.50 */
            int pergame_res = opts.explicit_size;
            if (peek) {
                int hint_w = 0, hint_h = 0;
                if (!pergame_res &&
                    jar_detect_screen_size(peek, peek_size, &hint_w, &hint_h)) {
                    opts.width = hint_w;
                    opts.height = hint_h;
                    LOG_SAFE("[SWITCH] auto resolution: manifest hint -> %dx%d\n",
                             hint_w, hint_h);
                } else if (!pergame_res &&
                           jar_detect_screen_size_from_name(opts.jar_file,
                                                            &hint_w, &hint_h)) {
                    /* v35.12: no display-size attribute in the manifest —
                     * fall back to the file name ("Game 240x320.jar"). */
                    opts.width = hint_w;
                    opts.height = hint_h;
                    LOG_SAFE("[SWITCH] auto resolution: file name hint -> %dx%d\n",
                             hint_w, hint_h);
                }
                free(peek);
            }
        }

        /* settings -> VM knobs (mirrors the libretro j2me_vm_speed
         * semantics: original=15000, fast=400000 (v35.09), turbo=0 budget) */
        {
            extern long g_jvm_thread_budget;
            const SwitchSettings* st = switch_settings_get();
            /* v35.09: fast 90000 -> 400000. Field case (asia rally): the
             * game does its 3D transform math in JAVA bytecode (profile:
             * br.* vector methods dominate) and demands ~7.5M instr/s even
             * idling in the race; the old fast ceiling allowed only
             * 90000/16.6ms = 5.4M/s — the race crawled and the game's
             * dt-catch-up substeps compounded the stall. 400000/16.6ms =
             * 24M/s covers the demand and still sleeps the runner whenever
             * it burns the window early (frontend stays responsive).
             * "original" keeps the phone-faithful 15000; "turbo" remains
             * unlimited. Per-game vm_speed (PLUS menu, v35.09) overrides. */
            #define NOJME_FAST_VM_BUDGET 400000L
            {
                int speed = st->vm_speed;
                if (pg_vm_speed >= 0) { /* v35.09: per-game override wins */
                    speed = pg_vm_speed;
                    LOG_SAFE("[SWITCH] per-game vm_speed: %s\n",
                             speed == NOJME_VM_SPEED_ORIGINAL ? "original" :
                             speed == NOJME_VM_SPEED_TURBO    ? "turbo" : "fast");
                }
                switch (speed) {
                    case NOJME_VM_SPEED_ORIGINAL: g_jvm_thread_budget = 15000; break;
                    case NOJME_VM_SPEED_TURBO:    g_jvm_thread_budget = 0;      break;
                    default:                      g_jvm_thread_budget = NOJME_FAST_VM_BUDGET; break;
                }
            }
            LOG_SAFE("[SWITCH] session: %s (%dx%d), budget=%ld, audio=%d\n",
                     jar_path, opts.width, opts.height, g_jvm_thread_budget,
                     st->audio_enabled);
        }

        /* v36.21 [EXC-RESET]: a new session must start with a CLEAN error
         * state. The uncaught-exception screen lives in PROCESS-GLOBAL
         * statics (g_has_error / g_error_* in sdl_graphics.c), so without
         * this the red "[ERROR] ArithmeticException ..." screen drawn for
         * the PREVIOUS midlet stayed armed forever: closing the crashed
         * midlet and launching another game immediately covered the NEW
         * game with the OLD exception screen (field report). The menu loop
         * itself never draws it, so the stale state was invisible until
         * the next launch. */
        sdl_clear_error();

        if (!init_emulator(&opts)) {
            LOG_SAFE("[SWITCH] init_emulator failed — back to menu\n");
            sw_trace("init: FAILED"); /* v34.91 */
            /* v36.24 [FAILED-INIT-PARITY]: the failure can land AFTER any
             * stage (jvm_init / sdl_init / native_init / midp_init), and
             * this path used to run cleanup() ALONE — the per-session
             * resets the normal teardown performs (media state, diag,
             * frontend-pause latch, manifest properties, midlet_class
             * string) were skipped and leaked into the NEXT launch.
             * cleanup() itself is NULL-guarded and safe on a half-built
             * session (jvm_destroy walks zeroed tables as empty). */
            {
                extern void media_session_reset(void);
                extern void sw_diag_session_reset(void);
                extern void jvm_frontend_pause_enable(int on);
                media_session_reset();
                sw_diag_session_reset();
                jvm_frontend_pause_enable(0);
                midlet_manifest_reset();
            }
            cleanup();
            /* v35.12: the JAR image is a malloc'd buffer owned by the session
             * (class_loader.jar_data); dropping the pointer without free()
             * leaked the whole jar (1-5 MB) EVERY session — fuel for the
             * "relaunch fails to load / crashes" report on memory-tight
             * applet-mode launches. */
            if (g_jar_data) { free(g_jar_data); }
            g_jar_data = NULL;
            g_jar_size = 0;
            /* v36.24: same for the manifest-parsed midlet class string —
             * the normal teardown frees it (v36.12); the init-fail path
             * leaked one block per failed launch. */
            if (opts.midlet_class) { free((void*)opts.midlet_class); opts.midlet_class = NULL; }
            continue;
        }

        {
            extern void sw_diag_set_jvm(void* jvm);
            extern JVM* g_jvm;
            sw_diag_set_jvm((void*)g_jvm);
        }

        /* v34.94 STARTUP-FREEZE FIX: open the audio device EAGERLY here, on
         * the FRONTEND thread, before any Java code runs. Previously the
         * device was opened lazily from inside Player.start() — i.e. from
         * whatever thread the MIDlet used; games that started audio early
         * (or from a runner pthread) froze right at start. Failure degrades
         * to silence, never to a hang. */
        {
            const SwitchSettings* st = switch_settings_get();
            if (st->audio_enabled) {
                extern int sdl_audio_init_simple(uint32_t sample_rate);
                sdl_audio_init_simple(44100);
            }
        }

        /* v34.81 pause watchdog (проводка v34.97): виртуальные часы +
         * eval-поток. Сам цикл кадра больше НЕ снимает active через
         * run_exit (см. sdl_graphics.c) — stall-детектор в игре не взводит
         * паузу вообще; единственные точки взведения — begin/end в
         * оверлеях (меню MINUS, per-game экран PLUS). */
        {
            extern void jvm_frontend_pause_enable(int on);
            jvm_frontend_pause_enable(1);
        }

        (void)run_midlet(&opts);

        /* v36.48 [DUMP-FENCE]: с этой секунды VM умирает — beat-таймер
         * (diag-строки, thread dump) больше не должен трогать ни JVM, ни
         * его кучу/кадры: g_sd_jvm=NULL закрывает ВСЕ g_sd_jvm-гейты в
         * switch_trace.c (heap_get_stats по СВОБОЖДЁННОМУ jvm был тем же
         * классом UAF, что и deep-дамп кадров). */
        {
            extern void sw_diag_set_jvm(void* jvm);
            sw_diag_set_jvm(NULL);
        }

        /* teardown: wait briefly for lingering Java threads, then clean.
         * If threads refuse to die, cleanup() _exit(0)s to HBmenu rather
         * than tearing the heap under a running thread (house rule).
         * v36.13 HARD-KILL: the passive wait is split — after a short
         * grace the GLOBAL REGISTRY signals every survivor (kill switch:
         * the interpreter's jvm->running check + registry wakeup broadcast),
         * then we keep waiting out the remaining window. Survivors after
         * THAT strand the arena in jvm_destroy (leakv=) as before. */
        {
            extern bool vm_threads_busy(void);
            extern void native_threads_kill_all(void);
            int waited_ms = 0;
            int killed_once = 0;
            sw_trace("game: session end"); /* v34.91 */
            /* v36.48 [EXIT-RELEASE-ALL] TESTHOOK: NOJME_TEST_BAILOUT=1 —
             * принудительно пройти аварийный путь выхода (RMS-сброс со
             * сторожем + svcExitProcess/_exit) сразу после конца сессии.
             * Драйвер: scripts/test_exit_release.sh. Ноль эффекта в полях
             * (переменная никогда не ставится). */
            if (getenv("NOJME_TEST_BAILOUT")) {
                nojme_bailout_exit("TESTHOOK: forced bailout after session end");
            }
            while (vm_threads_busy() && waited_ms < 2000) {
                if (!killed_once && waited_ms >= 400) {
                    /* прибить всех выживших через реестр потоков */
                    native_threads_kill_all();
                    killed_once = 1;
                }
                struct timespec ts = { .tv_sec = 0, .tv_nsec = 50 * 1000000L };
                nanosleep(&ts, NULL);
                waited_ms += 50;
            }
            if (vm_threads_busy()) {
                sw_trace("teardown: threads STILL busy after kill+2s (strand expected)");
            }
        }

        /* v36.13 [MEM-STAGE]: per-stage malloc bisection on the teardown
         * path. The v36.12 field log showed uord growing ~+33 MB per
         * session on HOS while the HOST soak converged — with this ladder
         * the next log NAMES the stage that stops returning memory:
         *   memT0 = at session end (runners just dead)
         *   memT1 = after media/diag resets
         *   memT2 = after cleanup() (jvm_destroy -> heap + sdl_destroy)
         *   memT3 = after jar/midlet_class frees (last teardown step)
         * A gap between T2 and T3 that never comes back = VM heap pool
         * not reclaimed by free(); a gap at T1 = media/frontend; growth
         * INTO memT0 across sessions = session-side leak. */
#if !defined(_WIN32) && !defined(_WIN64)
#define NOJME_MEM_STAGE(tag) \
        do { \
            struct mallinfo mi_ = nojme_mallinfo(); \
            sw_trace("mem-%s: arena=%dK uord=%dK ford=%dK", tag, \
                     mi_.arena >> 10, mi_.uordblks >> 10, mi_.fordblks >> 10); \
        } while (0)
#else
#define NOJME_MEM_STAGE(tag) do { } while (0)
#endif
        NOJME_MEM_STAGE("T0");

        /* v34.94 EXIT-CRASH / MIDLET-SWITCH FIX: wipe the process-global
         * media state (players, event queue, dangling roots) BEFORE the
         * JVM heap goes away — the audio thread must never see freed
         * objects, and the next session must start from clean slots. */
        {
            extern void media_session_reset(void);
            media_session_reset();
        }
        {
            extern void sw_diag_session_reset(void);
            sw_diag_session_reset();
        }
        /* v36.24 [SESSION-MANIFEST-RESET]: drop this session's property set
         * together with the rest of the per-session state (mirrors the
         * media/diag resets above; a manifest-less jar must not serve the
         * dead game's getAppProperty values to the next one). */
        midlet_manifest_reset();
        /* v36.21 [EXC-RESET]: drop the uncaught-exception screen together
         * with the rest of the per-session state (mirrors the media/diag
         * resets above). Normal finish, pause-menu exit and ESC-from-
         * error-screen all funnel through this teardown — after it the
         * frontend menu (and every later launch) is guaranteed clean. */
        sdl_clear_error();
        NOJME_MEM_STAGE("T1");
        /* v34.97: скобка паузы — сессионная. Снимаем возможный остаточный
         * взвод и глушим eval-детектор на время меню фронтенда; следующая
         * сессия включит его заново (перед run_midlet). */
        {
            extern void jvm_frontend_pause_enable(int on);
            jvm_frontend_pause_enable(0);
        }
        cleanup();
        /* v36.48 [EXIT-RELEASE-ALL] FLAP-GUARD: редчайший случай — потоки
         * успокоились к проверке в cleanup(), но снова оказались заняты
         * внутри jvm_destroy (чанкованный парк) и тот РАННИЙ return
         * бросил целую VM (heap+классы) «ОС-у», вернув нас в меню с
         * утечкой — топливо OOM/белого экрана СЛЕДУЮЩЕГО запуска.
         * Теперь меню с такой утечкой недостижимо: либо полный teardown,
         * либо аварийный выход из процесса (RMS-сброс + svcExitProcess). */
        {
            extern bool vm_threads_busy(void);
            if (vm_threads_busy()) {
                nojme_bailout_exit("teardown: VM strand after jvm_destroy "
                                   "(flap) — process exit (full release)");
            }
        }
        NOJME_MEM_STAGE("T2");
        /* v35.12: free the session's JAR image (see the init-fail path). */
        if (g_jar_data) { free(g_jar_data); }
        g_jar_data = NULL;
        g_jar_size = 0;
        /* v36.12 (malloc-census find): on THIS loop opts.midlet_class can
         * only come from find_midlet_class (malloc'd manifest parse) — the
         * struct is memset per iteration and the string was dropped on the
         * floor every session (one block per session, e.g. "TestSpin"). */
        if (opts.midlet_class) { free((void*)opts.midlet_class); opts.midlet_class = NULL; }
        NOJME_MEM_STAGE("T3");
#undef NOJME_MEM_STAGE
        /* v36.06 [MEM]: process-malloc visibility at every session boundary.
         * Field case (v36.05 crash report): the user kept the frontend alive
         * for ~24 h (87M ms of trace ticks) opening/closing Doom RPG; every
         * teardown that finds a busy runner strands a whole 32 MB VM arena
         * ("[JVM-DESTROY] ... VM LEAKED"), and newlib fragmentation grows on
         * top. The first symptom of exhaustion is a hard Data Abort deep in
         * the class loader (NULL from an unchecked small allocation) — with
         * this line the log SHOWS the memory curve that led there:
         *   arena    = total malloc'd pages (grows with leaked arenas)
         *   uordblks = bytes in use (should drop back near baseline)
         *   fordblks = bytes in free chunks (fragmentation indicator)
         * newlib (Switch) and glibc/musl (host) all provide mallinfo(); the
         * Win32 path compiles without it. */
#if !defined(_WIN32) && !defined(_WIN64)
        {
            struct mallinfo mi = nojme_mallinfo();
            /* v36.07: sess=N (1-based session number of THIS nojme run) and
             * leakv=N (process-lifetime count of stranded VM arenas) turn
             * the memory curve into a one-line story: "sessions 1..3 fine,
             * then arena/uord step up by 64 MB per [JVM-DESTROY] VM LEAKED"
             * is the exhaustion chain that killed the next launch. */
            static int s_mem_sess_no = 0;
            extern int jvm_strand_count(void);
            /* v36.12 SOAK-VIS: rss= and os_threads= close the loop on the
             * "threads+heap freed at close" question: rss exposes memory
             * mallinfo cannot see (posix_memalign'd VM arena IS counted by
             * mallinfo, but mmap'd frontend buffers are not), and a
             * os_threads count that only ever grows proves leaked runners.
             * Linux-only (/proc); HOS keeps the mallinfo fields. */
            int rss_kb = -1, os_threads = -1;
#if defined(__linux__)
            {
                FILE* f = fopen("/proc/self/statm", "r");
                if (f) {
                    long tot = 0, rs = 0;
                    if (fscanf(f, "%ld %ld", &tot, &rs) == 2) rss_kb = (int)(rs * 4);
                    fclose(f);
                }
                DIR* d = opendir("/proc/self/task");
                if (d) {
                    struct dirent* e;
                    int n = 0;
                    while ((e = readdir(d)) != NULL) {
                        if (e->d_name[0] >= '0' && e->d_name[0] <= '9') n++;
                    }
                    closedir(d);
                    os_threads = n;
                }
            }
#endif
            sw_trace("mem: sess=%d arena=%dK uord=%dK ford=%dK leakv=%d rss=%dK os_threads=%d",
                     ++s_mem_sess_no,
                     mi.arena >> 10, mi.uordblks >> 10, mi.fordblks >> 10,
                     jvm_strand_count(), rss_kb, os_threads);
#if defined(J2ME_HAVE_SANITIZER_PROFILE)
            if (getenv("NOJME_MEM_PROFILE")) {
                ALWAYS_LOG("[MEM-PROFILE] session %d — aggregated live allocs:\n",
                           s_mem_sess_no);
                __sanitizer_print_memory_profile(95, 8);
            }
#endif
        }
#endif
        LOG_SAFE("[SWITCH] session complete — back to menu\n");
        /* v36.60 [ICON-CACHE-KEEP]: кэш иконок браузера БОЛЬШЕ НЕ
         * сбрасывается на границе сессии. Инвалидация и так точная
         * (path+mtime+fsize), а сброс заставлял браузер после КАЖДОЙ
         * игры лениво перечитывать все jar (один целиком на кадр —
         * 30-300 мс на файл) и «подтягивался» первое время после выхода
         * из мидлета. Память ограничена бюджетом пикселей в самом кэше
         * (JARICON_BUDGET_BYTES, вытеснение старых) — вечная жизнь
         * кэша безопасна. */
    }

    /* v34.95: nothing may outlive main() — stop the media audio thread
     * and close the audio device BEFORE the SDL teardown. v35.13: every
     * step leaves a trace breadcrumb (the previous log ended at
     * "menu: session end (0)" with the crash site unnamed). */
    sw_trace("exit: media shutdown begin");
    {
        extern void media_shutdown_full(void);
        media_shutdown_full();
    }
    sw_trace("exit: media shutdown done");
    /* v36.02 EXIT-DAEMON-JOIN: the two process-global daemon threads —
     * pause_eval_thread (threads.c, 100 ms loop, started on the first game)
     * and key_hang_watchdog (display.c, 500 ms loop, started on the first
     * key press) — used to outlive main(). _exit(0) does NOT kill threads
     * atomically on HOS: it returns to HBmenu through the loader unmap, and
     * each daemon's next wake executed unmapped NRO code (2168-0001
     * Instruction Abort, "hbloader", two threads in sleep loops:
     * X0/X1=100000000 and X20=500000000 in the user crash report). Stop and
     * JOIN both here, before anything else tears down. Bounded: <=500 ms
     * total (both sleep in 100 ms chunks and poll their stop flags). */
    sw_trace("exit: daemons shutdown begin");
    {
        extern int jvm_frontend_pause_eval_shutdown(void);
        extern int keydiag_hang_shutdown(void);
        int joined_eval = jvm_frontend_pause_eval_shutdown();
        int joined_wd = keydiag_hang_shutdown();
        sw_trace("exit: daemons joined eval=%d watchdog=%d",
                 joined_eval, joined_wd);
    }
    /* v36.02 census: name any other stragglers while the log still works.
     * VM runners exit at their next dispatch once jvm->running=false
     * (set by jvm_destroy at session end); if one is wedged inside a
     * native, this line is the only evidence — join is not possible for
     * detached runners, so we log and proceed. */
    /* v36.07 EXIT-UNDER-LIVE-THREADS GUARD (the "reopen after hbmenu"
     * crash class): _exit(0) does not kill this process — on HOS it
     * returns through the loader, which UNMAPS this NRO and reloads
     * hbmenu in the SAME process (svcExitProcess is never reached from an
     * NRO). Any VM runner still executing at that moment (wedged in a
     * native that outlived the 3s+4s teardown grace) wakes up inside
     * unmapped code -> 2168-0001 Instruction Abort reported by
     * Atmosphère as "hbloader", i.e. the user's "закрыл nojme, открыл
     * снова — вылет". A dead session that leaves runners alive is exactly
     * the strand case. So: give them one bounded extra wait; if they are
     * STILL alive, terminate the whole PROCESS (svcExitProcess) instead
     * of returning to the loader. Every thread dies atomically with the
     * address space; nothing executes unmapped code. Cost: the console
     * lands on HOME instead of hbmenu (hbmenu is re-opened manually) —
     * a graceful degradation chosen only in the pathological case. */
    {
        extern bool vm_threads_busy(void);
        int busy = vm_threads_busy() ? 1 : 0;
        if (busy) {
            int waited_ms = 0;
            while (vm_threads_busy() && waited_ms < 3000) {
                struct timespec ts = { .tv_sec = 0, .tv_nsec = 50 * 1000000L };
                nanosleep(&ts, NULL);
                waited_ms += 50;
            }
            busy = vm_threads_busy() ? 1 : 0;
        }
        if (busy) {
            sw_trace("exit: vm threads STILL busy after extra 3s — svcExitProcess "
                     "(no NRO unmap under live threads; back to HOME, not hbmenu)");
#ifdef __SWITCH__
            /* weak symbol: the switchui-verify sandbox compiles with
             * -D__SWITCH__ but links host SDL2 (no libnx) — there the
             * address is NULL and the branch is skipped. On the real
             * devkitA64 build libnx resolves it strongly. */
            {
                extern void svcExitProcess(void) __attribute__((weak));
                if (&svcExitProcess && svcExitProcess) {
                    svcExitProcess();
                }
            }
#endif
        } else {
            sw_trace("exit: vm threads idle");
        }
    }
    sdl_switch_platform_shutdown();
    sw_trace("exit: complete, _exit(0)"); /* raw write: lands even at _exit */
    {
        extern void j2me_dbg_exit_marker(const char* tag);
        j2me_dbg_exit_marker("main-returning");
    }
#ifdef __SWITCH__
    /* v35.11 FIX (Atmosphère crash + console reboot on full exit): normal
     * return from main() lets hbl UNMAP the NRO while process-global
     * DETACHED daemon threads are still running inside it —
     * key_hang_watchdog (display.c, 500 ms sleep loop, started once per
     * process on the first keyPressed) and pause_eval_thread (threads.c,
     * 100 ms, started on the first game). Neither is joinable (detached),
     * neither ever exits; their next wake after the unload instruction-
     * aborts on unmapped code (user crash report: "hbloader", Instruction
     * Abort, two threads at the same sleep loop, X0=500000000 ns). The
     * house rule already treats _exit(0) as THE way back to HBmenu (the
     * busy-teardown path); make it unconditional. The trace log uses
     * raw write(), so nothing is lost.
     * v36.02 CORRECTION: _exit(0) does NOT kill threads atomically — it
     * returns to HBmenu through the loader unmap, so any thread still
     * executing our code aborts there (that crash came back with the v36.01
     * build). The real fix for the class is the explicit daemon join above
     * (EXIT-DAEMON-JOIN); _exit(0) remains only as the final jump.
     * v36.09 FINAL WORD (field-proven by the Doom RPG [Rus] trace): even
     * with daemons joined, _exit(0) only unmaps the NRO and returns to
     * HBmenu with the WHOLE process still alive — heap, ~200+ MB of leaked
     * per-session arena, hbloader state. The next NRO launch inherits that
     * wreckage and Data-Aborts ("вылет при повторном запуске", crash at
     * nojme_switch+0x5bbad8). Kill the process itself instead: hbloader
     * restarts HBmenu fresh and every subsequent launch of the core starts
     * with a clean process (user-requested unconditional svcExitProcess).
     * v36.10 COMPILE FIX (field: devkitA64 "error: too many arguments to
     * function 'svcExitProcess'"): the HOS ExitProcess svc takes NO
     * parameter — Horizon has no process exit code. libnx prototype:
     *   void NX_NORETURN svcExitProcess(void);
     * __has_include guard keeps the host sandbox compiling (there it falls
     * through to _exit(0), which on a host OS does kill the process).
     * Must run BEFORE _exit(0); the _exit below stays as an unreachable
     * safety net on the device / the real exit in the sandbox. */
#if defined(__SWITCH__) && __has_include(<switch.h>)
    svcExitProcess();
#endif
    _exit(0);
#endif
    return 0;
}

#endif /* !__SWITCH__ */
