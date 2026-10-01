/*
 * J2ME Emulator - Thread-safe debug macros
 *
 * CRITICAL: This file MUST be included AFTER debug.h.
 * It forcibly overrides ALL debug macros with versions that route through
 * j2me_log_ungated() (thread-safe, format-checked) to prevent interleaved
 * output.
 *
 * v34.24 "quiet by default" policy (see debug.h):
 *   - MISSING_LOG / ALWAYS_LOG are the only always-on channels.
 *   - LOG_SAFE / ERROR_LOG / WARN_LOG are gated by j2me_log_enabled().
 *   - fprintf/vfprintf stay intercepted by debug.h's object-like macros.
 *
 *   #include "debug.h"
 *   #include "debug_macros.h"
 */

#ifndef J2ME_DEBUG_MACROS_H
#define J2ME_DEBUG_MACROS_H

#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>

/* Runtime debug flag (extern - defined in jvm/debug_var.c) */
extern int g_j2me_runtime_debug;
extern int g_nojme_media_debug; /* v36.28: NOJME_MEDIA_DEBUG env gate */

/* v34.24 log gate + channels (extern - jvm/debug_var.c) */
extern int j2me_log_enabled(void);
/* v34.30: gnu_printf flavor (MinGW ms_printf rejects the C99 'z' modifier
 * used by %zu log sites) — must match debug.h's J2ME_PRINTF_ATTR. */
#if defined(__GNUC__)
__attribute__((format(gnu_printf, 1, 2)))
#endif
extern int j2me_log_ungated(const char* fmt, ...);
extern int j2me_fprintf_gate(FILE* stream, const char* fmt, ...);
extern int j2me_vfprintf_gate(FILE* stream, const char* fmt, va_list ap);

/* Thread-safe logging mutex (extern - defined in jvm/debug_var.c) */
extern void log_lock(void);
extern void log_unlock(void);
extern void log_mutex_init(void);

/* MISSING-METHOD LOG — the ONLY always-on diagnostic channel. */
#undef MISSING_LOG
#define MISSING_LOG(fmt, ...) j2me_log_ungated(fmt, ##__VA_ARGS__)

/* Semantic alias for crash handlers / requested diagnostics. */
#undef ALWAYS_LOG
#define ALWAYS_LOG(fmt, ...) j2me_log_ungated(fmt, ##__VA_ARGS__)

/* Thread-safe fprintf replacement for stderr — GATED (v34.24). */
#undef LOG_SAFE
#define LOG_SAFE(fmt, ...) do { \
    if (j2me_log_enabled()) { \
        j2me_log_ungated(fmt, ##__VA_ARGS__); \
    } \
} while(0)

/* Debug-only thread-safe log — only prints when g_j2me_runtime_debug != 0 */
#undef VERBOSE_LOG
#define VERBOSE_LOG(fmt, ...) do { \
    if (g_j2me_runtime_debug) { \
        j2me_log_ungated(fmt, ##__VA_ARGS__); \
    } \
} while(0)

/* ===== Core logging macros ===== */

#undef DEBUG_LOG
#define DEBUG_LOG(fmt, ...) do { \
    if (g_j2me_runtime_debug) { \
        j2me_log_ungated("[J2ME] " fmt "\n", ##__VA_ARGS__); \
    } \
} while(0)

#undef DEBUG_LOG_RAW
#define DEBUG_LOG_RAW(fmt, ...) do { \
    if (g_j2me_runtime_debug) { \
        j2me_log_ungated(fmt, ##__VA_ARGS__); \
    } \
} while(0)

#undef ERROR_LOG
#define ERROR_LOG(fmt, ...) do { \
    if (j2me_log_enabled()) { \
        j2me_log_ungated("[J2ME] [ERROR] " fmt "\n", ##__VA_ARGS__); \
    } \
} while(0)

#undef WARN_LOG
#define WARN_LOG(fmt, ...) do { \
    if (j2me_log_enabled()) { \
        j2me_log_ungated("[J2ME] [WARN] " fmt "\n", ##__VA_ARGS__); \
    } \
} while(0)

#undef INFO_LOG
#define INFO_LOG(fmt, ...) do { \
    if (g_j2me_runtime_debug) { \
        printf("[INFO] " fmt "\n", ##__VA_ARGS__); \
        fflush(stdout); \
    } \
} while(0)

/* ===== Module-specific debug macros (gated) ===== */

#define MODULE_LOG_TS(tag, fmt, ...) do { \
    if (g_j2me_runtime_debug) { \
        j2me_log_ungated(tag " " fmt "\n", ##__VA_ARGS__); \
    } \
} while(0)

#undef JVM_DEBUG
#define JVM_DEBUG(fmt, ...)    MODULE_LOG_TS("[JVM]", fmt, ##__VA_ARGS__)
#undef EXEC_DEBUG
#define EXEC_DEBUG(fmt, ...)   MODULE_LOG_TS("[EXEC]", fmt, ##__VA_ARGS__)
#undef CLASS_DEBUG
#define CLASS_DEBUG(fmt, ...)  MODULE_LOG_TS("[CLASS]", fmt, ##__VA_ARGS__)
#undef GC_DEBUG
#define GC_DEBUG(fmt, ...)     MODULE_LOG_TS("[GC]", fmt, ##__VA_ARGS__)
#undef NATIVE_DEBUG
#define NATIVE_DEBUG(fmt, ...) MODULE_LOG_TS("[NATIVE]", fmt, ##__VA_ARGS__)
#undef MIDP_DEBUG
#define MIDP_DEBUG(fmt, ...)   MODULE_LOG_TS("[MIDP]", fmt, ##__VA_ARGS__)
#undef GFX_DEBUG
#define GFX_DEBUG(fmt, ...)    MODULE_LOG_TS("[GFX]", fmt, ##__VA_ARGS__)
#undef DISP_DEBUG
#define DISP_DEBUG(fmt, ...)   MODULE_LOG_TS("[DISP]", fmt, ##__VA_ARGS__)
#undef FORM_DEBUG
#define FORM_DEBUG(fmt, ...)   MODULE_LOG_TS("[FORM]", fmt, ##__VA_ARGS__)
#undef MEDIA_DEBUG
#define MEDIA_DEBUG(fmt, ...) do { \
    if (g_j2me_runtime_debug || g_nojme_media_debug) { \
        j2me_log_ungated("[MEDIA] " fmt "\n", ##__VA_ARGS__); \
    } \
} while(0)
#undef RMS_DEBUG
#define RMS_DEBUG(fmt, ...)    MODULE_LOG_TS("[RMS]", fmt, ##__VA_ARGS__)
#undef THREAD_DEBUG
#define THREAD_DEBUG(fmt, ...) MODULE_LOG_TS("[THREAD]", fmt, ##__VA_ARGS__)
#undef OPCODE_DEBUG
#define OPCODE_DEBUG(fmt, ...) MODULE_LOG_TS("[OP]", fmt, ##__VA_ARGS__)

/* ===== Utility macros ===== */

#undef WH_DEBUG
#define WH_DEBUG(...) do { } while(0)

#undef SM_DEBUG
#define SM_DEBUG(...) do { } while(0)

/* Hex dump utility (gated) */
#ifndef DEBUG_HEXDUMP_DEFINED
#define DEBUG_HEXDUMP_DEFINED
#undef HEXDUMP
static inline void _debug_hexdump(const char* label, const void* data, size_t len) {
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
#define HEXDUMP(label, data, len) _debug_hexdump(label, data, len)
#endif

/* ==============================================
 * v34.24 fprintf INTERCEPT (mirror of debug.h)
 * ============================================== */
#if !defined(J2ME_FPRINTF_GATED) && !defined(NOJME_NO_FPRINTF_GATE)
#define J2ME_FPRINTF_GATED 1
#undef fprintf
#undef vfprintf
#define fprintf j2me_fprintf_gate
#define vfprintf j2me_vfprintf_gate
#endif

#endif /* J2ME_DEBUG_MACROS_H */
