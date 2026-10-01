/*
 * win_thread_shim.h — minimal Windows shim for the POSIX threading subset
 * used by this project (media.c, mobile3d.c, libretro/sdl_backend_stubs.c).
 *
 * Why: plain MinGW/MSYS2 targets have no <pthread.h> by default and we do not
 * want a hard winpthreads dependency for the libretro core. This header
 * implements exactly the primitives those translation units use, on top of
 * native Win32 facilities:
 *   - recursive mutex            -> CRITICAL_SECTION (recursive by design)
 *   - condition variable         -> CONDITION_VARIABLE / SleepConditionVariableCS
 *   - once-init                  -> InterlockedCompareExchange state machine
 *   - thread create/join         -> CreateThread / WaitForSingleObject
 *
 * Usage in a TU that previously did:  #include <pthread.h>
 *   #if defined(_WIN32)
 *   #include "win_thread_shim.h"
 *   #else
 *   #include <pthread.h>
 *   #endif
 */
#ifndef J2ME_WIN_THREAD_SHIM_H
#define J2ME_WIN_THREAD_SHIM_H

#ifndef _WIN32
#error "win_thread_shim.h must only be included on Windows targets"
#endif

#if defined(__GLIBC__) || defined(__gnu_linux__)
/*
 * Cross-check mode: the Windows code path is being compiled on a glibc host
 * (CI smoke test). glibc's <sys/types.h> has already injected pthread types,
 * so simply forward to the real pthread API — it is API-compatible with
 * everything this shim promises (recursive mutexes, absolute-timeout
 * condvars, once-init). No shim symbols are defined in this mode.
 */
#include <pthread.h>
#include <unistd.h>

#else /* real Windows / MinGW target */

#include <windows.h>
#include <time.h>
#include <stdlib.h>
#include <errno.h>
/* MinGW's <unistd.h> declares POSIX usleep()/sleep(). It MUST be included
 * BEFORE the usleep() override macro below: when a translation unit includes
 * <unistd.h> after this shim, the macro would mangle the real prototype into
 * "wp_usleep_useconds_t((unsigned long)(...))" in a declaration context ->
 * "error: expected declaration specifiers or '...' before '(' token".
 * time.h (above) plays the same role for the clock_gettime() override. */
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Mutex (CRITICAL_SECTION, always recursive)                          */
/* ------------------------------------------------------------------ */

typedef struct {
    /* ready: 0 = uninitialized, 1 = initializing in progress, 2 = ready */
    CRITICAL_SECTION cs;
    volatile LONG    ready;
} pthread_mutex_t;

/* Zero-initialised CRITICAL_SECTION + ready=0 lets us do lazy init.
 * NOTE: use the universal `{ 0 }` form — nested braces trip -Wbraces
 * because the first CRITICAL_SECTION member is a scalar pointer. */
#define PTHREAD_MUTEX_INITIALIZER { 0 }

typedef int pthread_mutexattr_t;   /* ignored: CS is always recursive */
#define PTHREAD_MUTEX_RECURSIVE 1

static inline int pthread_mutexattr_init(pthread_mutexattr_t* a) {
    if (a) *a = PTHREAD_MUTEX_RECURSIVE;
    return 0;
}
static inline int pthread_mutexattr_settype(pthread_mutexattr_t* a, int type) {
    if (a) *a = type;
    return 0;
}
static inline int pthread_mutexattr_destroy(pthread_mutexattr_t* a) {
    (void)a;
    return 0;
}

static inline void wp_mutex_lazy_init(pthread_mutex_t* m) {
    if (!m->ready) {
        if (InterlockedCompareExchange(&m->ready, 1, 0) == 0) {
            InitializeCriticalSection(&m->cs);
            InterlockedExchange(&m->ready, 2);
        } else {
            /* Another thread is initializing; wait until it finishes */
            while (InterlockedCompareExchange(&m->ready, 0, 0) != 2)
                SwitchToThread();
        }
    }
}

static inline int pthread_mutex_init(pthread_mutex_t* m,
                                     const pthread_mutexattr_t* attr) {
    (void)attr;
    m->ready = 0;
    InitializeCriticalSection(&m->cs);
    InterlockedExchange(&m->ready, 2);
    return 0;
}
static inline int pthread_mutex_destroy(pthread_mutex_t* m) {
    if (m && m->ready == 2) DeleteCriticalSection(&m->cs);
    return 0;
}
static inline int pthread_mutex_lock(pthread_mutex_t* m) {
    wp_mutex_lazy_init(m);
    EnterCriticalSection(&m->cs);
    return 0;
}
static inline int pthread_mutex_unlock(pthread_mutex_t* m) {
    LeaveCriticalSection(&m->cs);
    return 0;
}
static inline int pthread_mutex_trylock(pthread_mutex_t* m) {
    wp_mutex_lazy_init(m);
    return TryEnterCriticalSection(&m->cs) ? 0 : EBUSY;
}

/* ------------------------------------------------------------------ */
/* Condition variable                                                  */
/* ------------------------------------------------------------------ */

/* v34.10: like the mutex, the cond carries a `ready` flag so that the
 * static initializer PTHREAD_COND_INITIALIZER (used by threads.c's GC
 * safepoint sync) lazily initializes on first use. A zero-initialized
 * CONDITION_VARIABLE is NOT valid for SleepConditionVariableCS, so every
 * cond entry point runs wp_cond_lazy_init() first — same state machine
 * as wp_mutex_lazy_init(). */
typedef struct {
    CONDITION_VARIABLE cv;
    volatile LONG      ready;   /* 0 = uninit, 1 = initializing, 2 = ready */
} pthread_cond_t;

#define PTHREAD_COND_INITIALIZER { 0 }

#ifndef ETIMEDOUT
#define ETIMEDOUT 138   /* same value as MSVC errno.h */
#endif

static inline void wp_cond_lazy_init(pthread_cond_t* c) {
    if (!c->ready) {
        if (InterlockedCompareExchange(&c->ready, 1, 0) == 0) {
            InitializeConditionVariable(&c->cv);
            InterlockedExchange(&c->ready, 2);
        } else {
            while (InterlockedCompareExchange(&c->ready, 0, 0) != 2)
                SwitchToThread();
        }
    }
}

static inline int pthread_cond_init(pthread_cond_t* c, const void* attr) {
    (void)attr;
    if (!c) return EINVAL;
    c->ready = 0;
    InitializeConditionVariable(&c->cv);
    InterlockedExchange(&c->ready, 2);
    return 0;
}
static inline int pthread_cond_destroy(pthread_cond_t* c) {
    (void)c; /* no resources to release on CONDITION_VARIABLE */
    return 0;
}
static inline int pthread_cond_signal(pthread_cond_t* c) {
    if (!c) return EINVAL;
    wp_cond_lazy_init(c);
    WakeConditionVariable(&c->cv);
    return 0;
}
static inline int pthread_cond_broadcast(pthread_cond_t* c) {
    if (!c) return EINVAL;
    wp_cond_lazy_init(c);
    WakeAllConditionVariable(&c->cv);
    return 0;
}
static inline int pthread_cond_wait(pthread_cond_t* c, pthread_mutex_t* m) {
    if (!c || !m) return EINVAL;
    wp_cond_lazy_init(c);
    wp_mutex_lazy_init(m);
    return SleepConditionVariableCS(&c->cv, &m->cs, INFINITE) ? 0 : EINVAL;
}

/* Absolute realtime deadline in abstime, like POSIX pthread_cond_timedwait.
 * Returns 0 on wake-up signal, ETIMEDOUT when the deadline expired first. */
static inline int pthread_cond_timedwait(pthread_cond_t* c,
                                         pthread_mutex_t* m,
                                         const struct timespec* abstime) {
    ULARGE_INTEGER now100;
    FILETIME ft;
    struct timespec now;
    long long delta_ms;

    if (!c || !m) return EINVAL;
    wp_cond_lazy_init(c);

    /* pthread semantics: NULL/mutated clock not supported here; all project
     * call sites use absolute CLOCK_REALTIME deadlines derived from
     * gettimeofday()/clock_gettime(CLOCK_REALTIME). */
    GetSystemTimeAsFileTime(&ft);
    now100.LowPart  = ft.dwLowDateTime;
    now100.HighPart = ft.dwHighDateTime;
    /* FILETIME epoch (1601-01-01 UTC) -> UNIX epoch seconds */
    now.tv_sec  = (long long)(now100.QuadPart - 116444736000000000LL) / 10000000LL;
    now.tv_nsec = (long long)((now100.QuadPart % 10000000LL)) * 100LL;

    if (!abstime) {
        wp_mutex_lazy_init(m);
        return SleepConditionVariableCS(&c->cv, &m->cs, INFINITE) ? 0 : EINVAL;
    }
    delta_ms = ((long long)abstime->tv_sec  - (long long)now.tv_sec) * 1000LL
             + (((long long)abstime->tv_nsec - (long long)now.tv_nsec) / 1000000LL);
    if (delta_ms < 0) {
        /* Deadline already passed: match pthreads behaviour — still claim the
         * mutex briefly so callers observe it held while checking state. */
        delta_ms = 0;
    }
    wp_mutex_lazy_init(m);
    return SleepConditionVariableCS(&c->cv, &m->cs, (DWORD)delta_ms)
               ? 0 : ETIMEDOUT;
}

/* ------------------------------------------------------------------ */
/* pthread_once                                                        */
/* ------------------------------------------------------------------ */

/* 0 = idle, 1 = running, 2 = done */
typedef volatile LONG pthread_once_t;
#define PTHREAD_ONCE_INIT 0

static inline int pthread_once(volatile pthread_once_t* once, void (*fn)(void)) {
    if (InterlockedCompareExchange(once, 1, 0) == 0) {
        fn();
        InterlockedExchange(once, 2);
    } else {
        while (InterlockedCompareExchange(once, 0, 0) != 2)
            SwitchToThread();
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Threads                                                             */
/* ------------------------------------------------------------------ */

typedef HANDLE pthread_t;

typedef struct {
    void* (*start_routine)(void*);
    void* arg;
} wp_run_arg;

static DWORD WINAPI wp_thread_trampoline(LPVOID p) {
    wp_run_arg ra = *(wp_run_arg*)p;
    free(p);
    ra.start_routine(ra.arg);
    return 0;
}

static inline int pthread_create(pthread_t* t, const void* attr,
                                 void* (*start_routine)(void*), void* arg) {
    wp_run_arg* ra;
    HANDLE h;
    (void)attr;
    if (!t || !start_routine) return EINVAL;
    ra = (wp_run_arg*)malloc(sizeof(wp_run_arg));
    if (!ra) return EAGAIN;
    ra->start_routine = start_routine;
    ra->arg           = arg;
    h = CreateThread(NULL, 0, wp_thread_trampoline, ra, 0, NULL);
    if (!h) {
        free(ra);
        return EAGAIN;
    }
    *t = h;
    return 0;
}

static inline int pthread_join(pthread_t t, void** retval) {
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    if (retval) *retval = NULL;
    return 0;
}

/* v34.60 (merge of upstream v58): detached-thread support for the key-hang
 * watchdog (display.c). The handle is closed immediately; the thread itself
 * keeps running - Windows threads are independent of their handles. */
static inline int pthread_detach(pthread_t t) {
    CloseHandle((HANDLE)t);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Time helpers (plain MinGW lacks these under -std=c11 strict mode)   */
/* ------------------------------------------------------------------ */

#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#endif
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif

static inline int wp_clock_gettime(int clk_id, struct timespec* ts) {
    ULARGE_INTEGER u;
    FILETIME ft;
    LARGE_INTEGER freq, counter;
    if (!ts) return EINVAL;
    switch (clk_id) {
    case CLOCK_MONOTONIC:
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&counter);
        ts->tv_sec  = (time_t)(counter.QuadPart / freq.QuadPart);
        ts->tv_nsec = (long)(((counter.QuadPart % freq.QuadPart) * 1000000000ULL)
                             / (unsigned long long)freq.QuadPart);
        return 0;
    default: /* CLOCK_REALTIME */
        GetSystemTimeAsFileTime(&ft);
        u.LowPart  = ft.dwLowDateTime;
        u.HighPart = ft.dwHighDateTime;
        /* FILETIME epoch (1601-01-01 UTC) -> UNIX epoch seconds */
        ts->tv_sec  = (time_t)((long long)(u.QuadPart - 116444736000000000LL)
                               / 10000000LL);
        ts->tv_nsec = (long)((u.QuadPart % 10000000LL) * 100LL);
        return 0;
    }
}
#define clock_gettime(clk_id, ts) wp_clock_gettime((clk_id), (ts))

static inline void wp_usleep_useconds_t(unsigned long usec) {
    /* Round up to whole milliseconds; sub-ms sleeps keep spinning minimal */
    Sleep((DWORD)((usec + 999UL) / 1000UL));
}
#define usleep(us) wp_usleep_useconds_t((unsigned long)(us))

#endif /* __GLIBC__ cross-check mode */

#endif /* J2ME_WIN_THREAD_SHIM_H */
