/*
 * NOJME Sonivox integration: stub for AOSP <log/log.h>
 *
 * AOSP's sonivox uses ALOGE/ALOGI/etc. for logging. NOJME doesn't
 * have Android's logging framework — provide a minimal no-op shim.
 * If verbose Sonivox logging is needed, set the env var or route
 * these macros to MEDIA_DEBUG instead.
 */
#ifndef _NOJME_ANDROID_LOG_LOG_H
#define _NOJME_ANDROID_LOG_LOG_H

#include <stdio.h>
#include <stdlib.h>

/* Stub all ALOG* macros — silently drop the message */
#define ALOGE(...) do {} while (0)
#define ALOGW(...) do {} while (0)
#define ALOGI(...) do {} while (0)
#define ALOGD(...) do {} while (0)
#define ALOGV(...) do {} while (0)

#define LOG_ALWAYS_FATAL_IF(cond, ...) do { if (cond) { fprintf(stderr, "sonivox fatal: " __VA_ARGS__); abort(); } } while (0)
#define LOG_ALWAYS_FATAL(...) do { fprintf(stderr, "sonivox fatal: " __VA_ARGS__); abort(); } while (0)

#define LOG_FATAL_IF   LOG_ALWAYS_FATAL_IF
#define LOG_FATAL(...) LOG_ALWAYS_FATAL(__VA_ARGS__)

#define CONDITION(cond) (cond)

/* AOSP logging function used in safety checks (b/NNNNNNNN bug refs).
 * NOJME has no AOSP logger — drop the report silently. */
static inline void android_errorWriteLog(int tag, const char *msg) {
    (void)tag; (void)msg;
}

/* Also stub android_errorWriteWithTag (used by some variants) */
static inline void android_errorWriteWithLog(int tag, const char *subtag,
                                              const char *msg, int len) {
    (void)tag; (void)subtag; (void)msg; (void)len;
}

#endif /* _NOJME_ANDROID_LOG_LOG_H */
