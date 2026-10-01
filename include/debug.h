/*
 * J2ME Emulator - Debug Logging System
 * Conditional compilation for debug output
 *
 * v34.24 LOGGING POLICY ("quiet by default"):
 *   - MISSING_LOG — ALWAYS printed. This is the only always-on channel:
 *     it reports unimplemented Java/native methods ([INVOKE-MISSING],
 *     [NATIVE-MISSING], [M3G-MISSING], [IMG-MISSING]) — the exact signal
 *     a user needs to report an API gap. ALWAYS_LOG is the same function
 *     under a semantic alias (crash backtraces, requested self-tests).
 *   - Everything else (LOG_SAFE, ERROR_LOG, WARN_LOG, INFO_LOG, module
 *     DEBUG macros, and every raw fprintf(stderr)/vfprintf(stderr) call
 *     intercepted via the object-like macro below) is silent unless
 *     enabled by:
 *       * compile-time -DJ2ME_DEBUG=1 (headless dev builds), or
 *       * env NOJME_LOG=1 at runtime (release builds), or
 *       * runtime flag toggle (F12 in the SDL app, --verbose).
 *
 * Implementation notes (IMPORTANT):
 *   - The intercept `#define fprintf j2me_fprintf_gate` is an OBJECT-LIKE
 *     macro, so `(fprintf)(...)` does NOT bypass it (the parenthesis
 *     suppression trick only works for function-like macros). Code that
 *     must print unconditionally calls j2me_log_ungated() directly.
 *   - jvm/debug_var.c #undef's the intercept before implementing the
 *     gates with the real libc vfprintf.
 */

#ifndef DEBUG_H
#define DEBUG_H

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>

/* attribute for format-checked printing (keeps -Wformat working).
 * v34.30: gnu_printf flavor on ALL GNU-compatible compilers (GCC, clang,
 * MinGW). Rationale: on MinGW targets the plain "printf" flavor maps to
 * ms_printf, which REJECTS the C99 'z' length modifier at -Wformat level
 * ("unknown conversion type character 'z'") — every %zu in our ~114 log
 * sites warned, and the "too many arguments" follow-up warnings cascaded
 * from the same misparse. On glibc/musl Linux targets printf == gnu_printf
 * (zero checking change). gnu_printf accepts %zu on every toolchain,
 * including MinGW.org's ancient GCC. The RUNTIME side (msvcrt printf also
 * lacking %z) is handled separately in debug_var.c's win32 translator. */
#if defined(__GNUC__)
#define J2ME_PRINTF_ATTR(fmtidx, argidx) __attribute__((format(gnu_printf, fmtidx, argidx)))
#else
#define J2ME_PRINTF_ATTR(fmtidx, argidx)
#endif

/* Debug level - can be overridden via compiler flag */
#ifndef J2ME_DEBUG
#define J2ME_DEBUG 0
#endif

/* Error prefix for all log messages */
#define LOG_PREFIX "[J2ME]"

#ifdef __cplusplus
extern "C" {
#endif

/* ==============================================
 * THREAD-SAFE LOGGING INFRASTRUCTURE
 * ============================================== */

/* Mutex functions - implemented in debug_var.c */
extern void log_lock(void);
extern void log_unlock(void);
extern void log_mutex_init(void);

/* ==============================================
 * v34.24 LOG GATE + CHANNELS
 * ============================================== */

/* 1 if env NOJME_LOG is set to a non-"0" value OR the runtime debug flag
 * is on (compile-time J2ME_DEBUG, F12 toggle, --verbose). The env part is
 * cached; the runtime flag stays dynamic. */
int j2me_log_enabled(void);

/* UNGATED channel: ALWAYS prints to stderr, thread-safe, format-checked.
 * Backs MISSING_LOG / ALWAYS_LOG. Use ONLY for missing-method reports,
 * crash handlers and explicitly requested diagnostics. */
J2ME_PRINTF_ATTR(1, 2)
int j2me_log_ungated(const char* fmt, ...);

/* v34.25: optional sink for the ALWAYS-ON channel. Frontends where stderr
 * is unreachable (a Windows DLL loaded by a GUI libretro host — the DLL's
 * stderr writes nowhere) register a sink so MISSING_LOG reports, the core
 * build banner and FATAL init failures reach the host log instead.
 * The sink receives one pre-formatted line (may contain a trailing '\n').
 * NULL (default) = stderr only, exactly the v34.24 behavior. */
typedef void (*j2me_log_sink_fn)(const char* line);
void j2me_set_log_sink(j2me_log_sink_fn sink);

/* Gate implementations for the fprintf/vfprintf intercept.
 * stream != stderr always passes through (stdout, files).
 * stderr passes only when j2me_log_enabled(). */
J2ME_PRINTF_ATTR(2, 3)
int j2me_fprintf_gate(FILE* stream, const char* fmt, ...);
J2ME_PRINTF_ATTR(2, 0)
int j2me_vfprintf_gate(FILE* stream, const char* fmt, va_list ap);

/* ==============================================
 * MISSING-METHOD LOG — THE ONLY ALWAYS-ON CHANNEL
 * ==============================================
 * Reports unimplemented Java/native API surface ([INVOKE-MISSING],
 * [NATIVE-MISSING], [M3G-MISSING], [IMG-MISSING] family). */
#ifndef MISSING_LOG
#define MISSING_LOG(fmt, ...) j2me_log_ungated(fmt, ##__VA_ARGS__)
#endif

/* Semantic alias: always-print channel for crash handlers and explicitly
 * requested diagnostics (same function as MISSING_LOG). */
#ifndef ALWAYS_LOG
#define ALWAYS_LOG(fmt, ...) j2me_log_ungated(fmt, ##__VA_ARGS__)
#endif

/* Thread-safe fprintf replacement for stderr — GATED (v34.24).
 * Prints only when j2me_log_enabled() (see above). */
#define LOG_SAFE(fmt, ...) do { \
    if (j2me_log_enabled()) { \
        j2me_log_ungated(fmt, ##__VA_ARGS__); \
    } \
} while(0)

/* Debug-only thread-safe log — only prints when g_j2me_runtime_debug != 0. */
#define VERBOSE_LOG(fmt, ...) do { \
    if (g_j2me_runtime_debug) { \
        j2me_log_ungated(fmt, ##__VA_ARGS__); \
    } \
} while(0)

/* ==============================================
 * RUNTIME DEBUG TOGGLE SUPPORT
 * ============================================== */

/* Global runtime debug flag - initialized from J2ME_DEBUG at link time
 * (see debug_var.c v34.24: static init, no explicit init call required) */
extern int g_j2me_runtime_debug;

/* Initialize runtime debug state (compatibility no-op now: the flag starts
 * at J2ME_DEBUG via static initialization). Also initializes the mutex. */
static inline void j2me_debug_init(void) {
    g_j2me_runtime_debug = J2ME_DEBUG;
    log_mutex_init();
}

/* Toggle runtime debug mode - returns new state. LOG_SAFE / the fprintf
 * gate follow this flag: F12 flips logging at runtime. */
static inline bool j2me_debug_toggle(void) {
    g_j2me_runtime_debug = !g_j2me_runtime_debug;
    MISSING_LOG(LOG_PREFIX " [DEBUG] Debug mode %s\n",
            g_j2me_runtime_debug ? "ENABLED" : "DISABLED");
    return g_j2me_runtime_debug != 0;
}

/* Check if debug mode is enabled (runtime or compile-time) */
static inline bool j2me_debug_enabled(void) {
    return g_j2me_runtime_debug != 0;
}

/* Enable/disable debug mode programmatically */
static inline void j2me_set_debug(bool enabled) {
    g_j2me_runtime_debug = enabled ? 1 : 0;
}

/* ==============================================
 * DEBUG LOGGING MACROS (all GATED, v34.24)
 * ============================================== */

#define DEBUG_LOG(fmt, ...) do { \
    if (g_j2me_runtime_debug) { \
        j2me_log_ungated(LOG_PREFIX " " fmt "\n", ##__VA_ARGS__); \
    } \
} while(0)

#define DEBUG_LOG_RAW(fmt, ...) do { \
    if (g_j2me_runtime_debug) { \
        j2me_log_ungated(fmt, ##__VA_ARGS__); \
    } \
} while(0)

/* Error log - gated like everything else except missing-methods.
 * Enable with NOJME_LOG=1 / J2ME_DEBUG=1 / F12 to see errors. */
#define ERROR_LOG(fmt, ...) do { \
    if (j2me_log_enabled()) { \
        j2me_log_ungated(LOG_PREFIX " [ERROR] " fmt "\n", ##__VA_ARGS__); \
    } \
} while(0)

/* Warning log - gated (see ERROR_LOG) */
#define WARN_LOG(fmt, ...) do { \
    if (j2me_log_enabled()) { \
        j2me_log_ungated(LOG_PREFIX " [WARN] " fmt "\n", ##__VA_ARGS__); \
    } \
} while(0)

/* Info log - checks runtime flag */
#define INFO_LOG(fmt, ...) do { \
    if (g_j2me_runtime_debug) { \
        printf("[INFO] " fmt "\n", ##__VA_ARGS__); \
        fflush(stdout); \
    } \
} while(0)

/* ==============================================
 * Module-specific debug macros (gated)
 * ============================================== */

#define MODULE_LOG(tag, fmt, ...) do { \
    if (g_j2me_runtime_debug) { \
        j2me_log_ungated(tag " " fmt "\n", ##__VA_ARGS__); \
    } \
} while(0)

#define JVM_DEBUG(fmt, ...)    MODULE_LOG("[JVM]", fmt, ##__VA_ARGS__)
#define EXEC_DEBUG(fmt, ...)   MODULE_LOG("[EXEC]", fmt, ##__VA_ARGS__)
#define CLASS_DEBUG(fmt, ...)  MODULE_LOG("[CLASS]", fmt, ##__VA_ARGS__)
#define GC_DEBUG(fmt, ...)     MODULE_LOG("[GC]", fmt, ##__VA_ARGS__)
#define NATIVE_DEBUG(fmt, ...) MODULE_LOG("[NATIVE]", fmt, ##__VA_ARGS__)
#define MIDP_DEBUG(fmt, ...)   MODULE_LOG("[MIDP]", fmt, ##__VA_ARGS__)
#define GFX_DEBUG(fmt, ...)    MODULE_LOG("[GFX]", fmt, ##__VA_ARGS__)
#define DISP_DEBUG(fmt, ...)   MODULE_LOG("[DISP]", fmt, ##__VA_ARGS__)
#define FORM_DEBUG(fmt, ...)   MODULE_LOG("[FORM]", fmt, ##__VA_ARGS__)
#define MEDIA_DEBUG(fmt, ...)  MODULE_LOG("[MEDIA]", fmt, ##__VA_ARGS__)
#define RMS_DEBUG(fmt, ...)    MODULE_LOG("[RMS]", fmt, ##__VA_ARGS__)
#define THREAD_DEBUG(fmt, ...) MODULE_LOG("[THREAD]", fmt, ##__VA_ARGS__)
#define OPCODE_DEBUG(fmt, ...) MODULE_LOG("[OP]", fmt, ##__VA_ARGS__)

/* ==============================================
 * Hex dump utility (gated)
 * ============================================== */
static inline void debug_hexdump(const char* label, const void* data, size_t len) {
    if (!g_j2me_runtime_debug) return;

    const uint8_t* bytes = (const uint8_t*)data;
    log_lock();
    fprintf(stderr, "[HEX] %s (%zu bytes): ", label, len);
    for (size_t i = 0; i < len && i < 64; i++) {
        fprintf(stderr, "%02X ", bytes[i]);
    }
    if (len > 64) fprintf(stderr, "...");
    fprintf(stderr, "\n");
    fflush(stderr);
    log_unlock();
}
#define HEXDUMP(label, data, len) debug_hexdump(label, data, len)

/* ==============================================
 * Memory tracking
 * ============================================== */
#define J2ME_MEM_ALLOC(ptr, size) DEBUG_LOG("ALLOC: %p (%zu bytes)", ptr, size)
#define J2ME_MEM_FREE(ptr) DEBUG_LOG("FREE: %p", ptr)

/* ==============================================
 * Performance timing
 * ============================================== */
#include <time.h>
#define PERF_START() clock_t _perf_start = clock()
#define PERF_END(label) do { \
    if (g_j2me_runtime_debug) { \
        clock_t _perf_end = clock(); \
        double _perf_ms = ((double)(_perf_end - _perf_start) / CLOCKS_PER_SEC) * 1000.0; \
        DEBUG_LOG("[PERF] %s: %.2f ms", label, _perf_ms); \
    } \
} while(0)

/* ==============================================
 * v34.24 fprintf INTERCEPT (object-like macro)
 * ==============================================
 * Every TU that includes this header gets fprintf/vfprintf redirected to
 * the gate: stderr output is silent unless logging is enabled. This covers
 * ~600 legacy raw fprintf(stderr, ...) diagnostic sites without touching
 * them one by one. stdout and other streams pass through untouched.
 *
 * NOTE: this is an OBJECT-LIKE macro — `(fprintf)(...)` does NOT bypass
 * it. Unconditional output goes through j2me_log_ungated()/MISSING_LOG.
 */
#if !defined(J2ME_FPRINTF_GATED) && !defined(NOJME_NO_FPRINTF_GATE)
#define J2ME_FPRINTF_GATED 1
#ifdef fprintf
#undef fprintf
#endif
#ifdef vfprintf
#undef vfprintf
#endif
#define fprintf j2me_fprintf_gate
#define vfprintf j2me_vfprintf_gate
#endif

#ifdef __cplusplus
}
#endif

#endif /* DEBUG_H */
