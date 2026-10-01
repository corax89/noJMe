/*
 * J2ME Emulator - Debug runtime variables and logging mutex
 *
 * v34.24: implements the "quiet by default" log policy:
 *   - g_j2me_runtime_debug statically initialized to J2ME_DEBUG (no
 *     explicit init call required anywhere).
 *   - j2me_log_enabled(): NOJME_LOG env (cached) || runtime debug flag.
 *   - j2me_log_ungated(): the ALWAYS-on channel (MISSING_LOG/ALWAYS_LOG).
 *   - j2me_fprintf_gate / j2me_vfprintf_gate: the fprintf intercept that
 *     silences stderr diagnostics in release builds.
 *   - j2me_set_log_sink/j2me_log_ungated (v34.25): optional frontend sink
 *     that mirrors the always-on channel into a host log (Windows DLLs
 *     have no reachable stderr).
 *
 * The intercept macros are #undef'd HERE so this translation unit talks
 * to the real libc fprintf/vfprintf.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "debug.h"

/* Talk to the real libc functions from this unit */
#undef fprintf
#undef vfprintf

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static CRITICAL_SECTION g_log_cs;
static int g_log_initialized = 0;
#else
#include <pthread.h>
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

/* v34.24: statically initialized from the compile-time setting so verbose
 * builds (headless, -DJ2ME_DEBUG=1) log without any init call. */
int g_j2me_runtime_debug = J2ME_DEBUG;

/* v36.28: dedicated MEDIA module gate (NOJME_MEDIA_DEBUG env) — позволяет
 * включить [MEDIA]-трассировку жизненного цикла плееров в sandbox/на
 * устройстве, не включая тяжёлые [EXEC]/[OP] логи verbose-режима. */
int g_nojme_media_debug = 0;

void log_mutex_init(void) {
#ifdef _WIN32
    if (!g_log_initialized) {
        InitializeCriticalSection(&g_log_cs);
        g_log_initialized = 1;
    }
#endif
}

void log_lock(void) {
#ifdef _WIN32
    if (!g_log_initialized) {
        InitializeCriticalSection(&g_log_cs);
        g_log_initialized = 1;
    }
    EnterCriticalSection(&g_log_cs);
#else
    pthread_mutex_lock(&g_log_mutex);
#endif
}

void log_unlock(void) {
#ifdef _WIN32
    LeaveCriticalSection(&g_log_cs);
#else
    pthread_mutex_unlock(&g_log_mutex);
#endif
}

/* ----------------------------------------------
 * Log gate: NOJME_LOG env (cached) || runtime flag
 * ---------------------------------------------- */
int j2me_log_enabled(void) {
    static int env_cache = -1;
    if (env_cache < 0) {
        const char* e = getenv("NOJME_LOG");
        env_cache = (e && e[0] != '\0' && strcmp(e, "0") != 0) ? 1 : 0;
    }
    return env_cache || g_j2me_runtime_debug;
}

/* ----------------------------------------------
 * v34.25: frontend sink for the ALWAYS-ON channel
 * ---------------------------------------------- */
static j2me_log_sink_fn g_log_sink = NULL;

void j2me_set_log_sink(j2me_log_sink_fn sink) {
    log_lock();
    g_log_sink = sink;
    log_unlock();
}

/* ----------------------------------------------
 * ALWAYS-on channel: MISSING_LOG / ALWAYS_LOG
 * ----------------------------------------------
 * v34.25: also forwards each line to the registered sink (if any) so
 * missing-method reports survive on hosts with no usable stderr
 * (Windows DLL). The stderr stream is unchanged (full, undeduplicated);
 * the SINK decides its own filtering (libretro.c dedups by signature). */
/* ----------------------------------------------
 * v34.30 Windows printf-format translator
 * ----------------------------------------------
 * msvcrt-based printf (classic MinGW runtime, and MinGW-w64 without
 * __USE_MINGW_ANSI_STDIO) does not understand the C99 length modifiers:
 *   %zu/%zd/%zx (size_t), %ju (intmax_t), %tu (ptrdiff_t), and (on
 * XP-era msvcrt) %llu/%lld (long long).
 * Every log line in this emulator funnels through exactly THIS file
 * (the fprintf/vfprintf intercept + j2me_log_ungated), so translating
 * the format string here fixes the actual RUNTIME OUTPUT (garbage
 * numbers, argument misalignment — a shifted %s can even crash) on
 * every MinGW flavor in one place. The MS 'I' / 'I64' prefixes are
 * accepted by msvcrt AND by mingw-w64's ANSI printf, so the
 * translation is safe even when the runtime already supports 'z'.
 * Source code keeps writing plain %zu. */
#if defined(_WIN32)
/* v36.38: returns_nonnull — every call site feeds the result straight into
 * vfprintf/vsnprintf as the FORMAT string; GCC's -Wformat-overflow could
 * not prove the translator never returns NULL and warned "null format
 * string" on MinGW builds. The function now NEVER returns NULL (see the
 * !fmt guard below) and the attribute documents it for the compiler. */
#if defined(__GNUC__)
#define NOJME_FMT_RET_NONNULL __attribute__((returns_nonnull))
#else
#define NOJME_FMT_RET_NONNULL
#endif
static const char* win_fmt_translate(char* dst, size_t dstsz, const char* fmt) NOJME_FMT_RET_NONNULL;
static const char* win_fmt_translate(char* dst, size_t dstsz, const char* fmt) {
    if (!fmt) return ""; /* v36.38: NULL format -> empty format (NULL was UB in v*printf) */
    size_t o = 0;
    const char* p = fmt;
    while (*p) {
        if (o + 6 >= dstsz) return fmt;   /* too long: pass through */
        if (*p != '%') { dst[o++] = *p++; continue; }
        dst[o++] = *p++;                       /* '%' */
        if (*p == '%') { dst[o++] = *p++; continue; }  /* '%%' */
        /* flags */
        while (*p && (*p == '-' || *p == '+' || *p == ' ' ||
                      *p == '#' || *p == '0')) {
            if (o + 1 >= dstsz) return fmt;
            dst[o++] = *p++;
        }
        /* width (digits or '*') */
        while (*p >= '0' && *p <= '9') {
            if (o + 1 >= dstsz) return fmt;
            dst[o++] = *p++;
        }
        if (*p == '*') {
            if (o + 1 >= dstsz) return fmt;
            dst[o++] = *p++;
        }
        /* precision */
        if (*p == '.') {
            if (o + 1 >= dstsz) return fmt;
            dst[o++] = *p++;
            while (*p >= '0' && *p <= '9') {
                if (o + 1 >= dstsz) return fmt;
                dst[o++] = *p++;
            }
            if (*p == '*') {
                if (o + 1 >= dstsz) return fmt;
                dst[o++] = *p++;
            }
        }
        /* length modifier -> MS equivalents */
        if (p[0] == 'z' || p[0] == 'j' || p[0] == 't') {
            if (o + 2 >= dstsz) return fmt;
            dst[o++] = 'I';
            p++;
        } else if (p[0] == 'l' && p[1] == 'l') {
            if (o + 4 >= dstsz) return fmt;
            dst[o++] = 'I'; dst[o++] = '6'; dst[o++] = '4';
            p += 2;
        } else if (p[0] == 'l' || p[0] == 'h' || p[0] == 'L') {
            if (o + 1 >= dstsz) return fmt;
            dst[o++] = *p++;
        }
        /* conversion character */
        if (*p) {
            if (o + 1 >= dstsz) return fmt;
            dst[o++] = *p++;
        }
    }
    dst[o] = '\0';
    return dst;
}
#define WIN_FMT(buf, fmt) win_fmt_translate((buf), sizeof(buf), (fmt))
#else
#define WIN_FMT(buf, fmt) (fmt)
#endif

J2ME_PRINTF_ATTR(1, 2)
int j2me_log_ungated(const char* fmt, ...) {
    va_list ap, ap2;
    j2me_log_sink_fn sink = g_log_sink; /* benign read: pointer-sized */
    va_start(ap, fmt);
    va_copy(ap2, ap);
    log_lock();
#ifdef _WIN32
    {
        char tbuf[2048];
        vfprintf(stderr, WIN_FMT(tbuf, fmt), ap);
        fflush(stderr);
    }
#else
    vfprintf(stderr, fmt, ap);
    fflush(stderr);
#endif
    log_unlock();
    va_end(ap);
    if (sink) {
        char buf[2048];
#ifdef _WIN32
        {
            char tbuf[2048];
            vsnprintf(buf, sizeof(buf), WIN_FMT(tbuf, fmt), ap2);
        }
#else
        vsnprintf(buf, sizeof(buf), fmt, ap2);
#endif
        va_end(ap2);
        sink(buf); /* called WITHOUT log_lock: the sink may print */
    } else {
        va_end(ap2);
    }
    return 0;
}

/* ----------------------------------------------
 * fprintf/vfprintf intercept
 * ---------------------------------------------- */
J2ME_PRINTF_ATTR(2, 3)
int j2me_fprintf_gate(FILE* stream, const char* fmt, ...) {
    if (stream != stderr || j2me_log_enabled()) {
        va_list ap;
        va_start(ap, fmt);
#ifdef _WIN32
        char tbuf[2048];
        int r = vfprintf(stream, WIN_FMT(tbuf, fmt), ap);
#else
        int r = vfprintf(stream, fmt, ap);
#endif
        va_end(ap);
        return r;
    }
    return 0;
}

J2ME_PRINTF_ATTR(2, 0)
int j2me_vfprintf_gate(FILE* stream, const char* fmt, va_list ap) {
    if (stream != stderr || j2me_log_enabled()) {
#ifdef _WIN32
        char tbuf[2048];
        return vfprintf(stream, WIN_FMT(tbuf, fmt), ap);
#else
        return vfprintf(stream, fmt, ap);
#endif
    }
    return 0;
}


/* v34.95 EXIT-TRACE (host verification builds): bracket the process exit
 * path — SDL_Quit completes, then main returns; the observed hang with
 * logging ON sits somewhere in exit()/stdio teardown. */
static void j2me_dbg_exit_atexit(void) {
    fprintf(stderr, "[EXITTRACE] atexit-handler\n");
    fflush(stderr);
}
void j2me_dbg_exit_marker(const char* tag) {
    static int registered = 0;
    if (!registered) { registered = 1; atexit(j2me_dbg_exit_atexit); }
    fprintf(stderr, "[EXITTRACE] %s\n", tag ? tag : "?");
    fflush(stderr);
}
