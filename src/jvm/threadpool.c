/*
 * J2ME Emulator — v36.73 [VM-THREADPOOL] implementation.
 * See threadpool.h for the design rationale.
 *
 * Lock order: g_pool_mu -> worker.mu (never the reverse; workers take only
 * their own mu). vm_pool_shutdown_join() holds g_pool_mu across the poison
 * broadcast so a submit racing teardown observes g_pool_shutdown under the
 * same mutex that guards the claim — no claim can land after the poison.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifdef _WIN32
/* MinGW build: the pthread API emulation from the shim (attrs are ignored
 * by its pthread_create — see worker_setup_attr's Windows short-circuit). */
#include "win_thread_shim.h"
#else
#include <pthread.h>
#endif
#ifdef __SWITCH__
#include <malloc.h>   /* memalign — newlib has no posix_memalign prototype */
/* v36.74 [MMAN-GUARD]: libnx has no <sys/mman.h> and no mprotect(). The
 * v36.73 sources failed to compile on devkitA64 exactly here. On the REAL
 * devkitPro/libnx toolchain the precise stack guard is still possible:
 * heap pages owned by the process can have their permission changed with
 * the Horizon syscall wrapper svcSetMemoryPermission (perm 0 = none) —
 * the same thing mprotect(PROT_NONE) does on POSIX. Our stacks are
 * memalign(4096)-page blocks wholly inside the malloc arena, so flipping
 * the first page to no-access is legal.
 * The host switchui-verify build also defines __SWITCH__ but has no
 * libnx headers (same pattern as [PAD-INFO]): __has_include gate, and a
 * dummy always-failing mprotect so the graceful setstacksize fallback
 * path compiles and is exercised there. NOJME_VM_MPROTECT=0 disables the
 * guard page on the device (A/B knob). */
#if defined(__has_include)
#  if __has_include(<switch.h>)
#    include <switch.h>
#    define NOJME_VM_HAVE_SVC_MPROTECT 1
#  endif
#endif
#define PROT_NONE 0
#ifdef NOJME_VM_HAVE_SVC_MPROTECT
static int switch_mprotect(void* addr, size_t len, int prot) {
    /* svcSetMemoryPermission perm bits: 1=R 2=W 4=X; 0 = inaccessible. */
    u32 perm = (u32)prot;
    return (int)svcSetMemoryPermission(addr, (u64)len, perm);  /* 0 = success */
}
#define mprotect(addr, len, prot) switch_mprotect((addr), (len), (prot))
#else
/* verify build: no libnx — the guard path degrades to "refused", the
 * caller falls back to a guardless (still pool-owned) stack. */
static inline int vm_mprotect_dummy(void* addr, size_t len, int prot) {
    (void)addr; (void)len; (void)prot;
    return -1;
}
#define mprotect(addr, len, prot) vm_mprotect_dummy((addr), (len), (prot))
#endif
#endif

#include "threadpool.h"

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

/* Hard cap. 32 concurrent Java threads covers every J2ME title seen in
 * the field (Bounce Tales needs 4, VmTest ~9 concurrent / ~109 sequential
 * — sequential ones reuse a single parked worker). Beyond the cap the
 * submit fails and Thread.start() runs its old failure path (silent
 * no-op) — identical to the pthread_create-failed behaviour. */
#define VM_POOL_MAX 32

/* Native stack per worker. The interpreter burns ~4.7 KB of native stack
 * per Java frame (interpret -> op_invokeXXX -> execute_method + frame
 * structs — MEASURED: a 450-frame Java recursion exhausted a 2 MB worker
 * stack on the poolstress selftest before the 500-frame StackOverflowError
 * guard could fire; v36.72-phonestacks' 2 MB was sized without that fact).
 * 4 MB gives the guard a 1.7x budget (500 x 4.7 KB = 2.35 MB) — Bounce
 * Tales' 4 workers reserve 16 MB total vs the 128 MB the v36.63 32 MB
 * stacks burned. Host builds keep the 8 MB Linux default.
 * NOJME_VM_STACK_MB (1..64) overrides for field A/B. */
#ifdef __SWITCH__
#define VM_WORKER_STACK_DEFAULT_MB 4
#else
#define VM_WORKER_STACK_DEFAULT_MB 8
#endif

typedef struct VmWorker {
    pthread_t       thread;
    int             started;
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    int             state;          /* VMW_* */
    void          (*fn)(void*);
    void*           arg;
    int             slot;           /* permanent registry slot */
    /* Own-stack path (Switch): base is the PROT_NONE guard page, the
     * usable stack begins at (char*)stack_mem + 4096. */
    void*           stack_mem;
    size_t          stack_total;
} VmWorker;

#define VMW_IDLE     0   /* parked in the pool, no job                      */
#define VMW_CLAIMED  1   /* job assigned, worker not awake yet              */
#define VMW_RUNNING  2   /* executing a job                                 */

static VmWorker g_workers[VM_POOL_MAX];
static int g_worker_count = 0;
static pthread_mutex_t g_pool_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_pool_parked_cv = PTHREAD_COND_INITIALIZER;
static volatile int g_pool_shutdown = 0;

/* Which worker THIS OS thread is (job body reads it at run start/end). */
static __thread int tls_pool_slot = -1;

/* ------------------------------------------------------------------ */
/* Diagnostics (weak — device trace only, compiles away nowhere special)*/
/* ------------------------------------------------------------------ */
extern void sw_trace_force(const char* fmt, ...) __attribute__((weak));

static long worker_stack_bytes(void) {
    static long sz = -1;
    if (sz < 0) {
        long mb = VM_WORKER_STACK_DEFAULT_MB;
        const char* e = getenv("NOJME_VM_STACK_MB");
        if (e && e[0]) {
            long v = atol(e);
            if (v >= 1 && v <= 64) mb = v;
        }
        sz = mb * 1024L * 1024L;
    }
    return sz;
}

int vm_pool_worker_count(void) { return g_worker_count; }

int vm_pool_current_slot(void) { return tls_pool_slot; }

/* v36.74 [MMAN-GUARD]: unguard + free a worker's custom stack. The guard
 * page was flipped to no-access via svcSetMemoryPermission; free() writes
 * dlmalloc's chunk metadata INTO the freed block's first bytes — touching
 * the guarded page would data-abort. Restore RW (perm 3) first; ignore
 * failures (the free is best-effort teardown). */
#ifdef __SWITCH__
static void vm_stack_release(VmWorker* w) {
    if (!w->stack_mem) return;
    mprotect(w->stack_mem, 4096, 3 /* Perm_R|Perm_W */);
    free(w->stack_mem);
    w->stack_mem = NULL;
    w->stack_total = 0;
}
#else
static void vm_stack_release(VmWorker* w) {
    if (!w->stack_mem) return;
    free(w->stack_mem);
    w->stack_mem = NULL;
    w->stack_total = 0;
}
#endif

int vm_pool_idle_count(void) {
    int n = 0;
    for (int i = 0; i < g_worker_count; i++) {
        if (g_workers[i].started && g_workers[i].state == VMW_IDLE) n++;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* Worker main: park -> claim -> run -> park, forever (until poison).  */
/* ------------------------------------------------------------------ */
static void* vm_worker_main(void* arg) {
    VmWorker* w = (VmWorker*)arg;

    pthread_mutex_lock(&w->mu);
    for (;;) {
        while (w->state == VMW_IDLE && !g_pool_shutdown) {
            pthread_cond_wait(&w->cv, &w->mu);
        }
        /* A CLAIMED job must still run during shutdown: the claimer has
         * already published is_alive=true and g_pthread_running[id]=true —
         * dropping it would wedge native_threads_wait_idle forever. The
         * job itself exits at its first dispatch (jvm->running == false),
         * so this is bounded by one interpreter step. */
        if (w->state == VMW_CLAIMED || w->state == VMW_RUNNING) {
            void (*fn)(void*) = w->fn;
            void* jarg = w->arg;
            w->state = VMW_RUNNING;
            tls_pool_slot = w->slot;   /* job body reads vm_pool_current_slot */
            pthread_mutex_unlock(&w->mu);
            /* The job body (native.c pthread_job_body) performs the whole
             * runner lifecycle INCLUDING the TLS reset at its tail — a
             * reused worker enters the next job with clean TLS. */
            fn(jarg);
            pthread_mutex_lock(&w->mu);
            w->fn = NULL;
            w->arg = NULL;
            w->state = VMW_IDLE;
            /* Wake a submitter briefly waiting for a worker to park (see
             * the bounded park-wait in vm_pool_submit): state is already
             * IDLE under w->mu, so the woken rescan cannot miss us. The
             * broadcast is taken AFTER releasing w->mu — the lock order
             * g_pool_mu -> w->mu is never inverted here. */
            pthread_mutex_unlock(&w->mu);
            pthread_mutex_lock(&g_pool_mu);
            pthread_cond_broadcast(&g_pool_parked_cv);
            pthread_mutex_unlock(&g_pool_mu);
            continue;   /* re-check poison */
        }
        /* IDLE + shutdown -> exit */
        break;
    }
    w->state = VMW_IDLE;   /* not EXITING: state is meaningless after join */
    pthread_mutex_unlock(&w->mu);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Stack setup: precise guard on Switch, plain attr elsewhere.         */
/* ------------------------------------------------------------------ */
static int worker_setup_attr(VmWorker* w, pthread_attr_t* attr) {
#ifdef _WIN32
    /* The shim's pthread_create ignores attrs — report "no attr" so the
     * caller passes NULL; the stack size is whatever CreateThread picks
     * (v17 sizing applies on Windows via the win runner path instead). */
    (void)w; (void)attr;
    return -1;
#else
    size_t want = (size_t)worker_stack_bytes();
#ifndef __SWITCH__
    (void)w;   /* stack_mem is only assigned on the Switch guard path */
#endif

    if (pthread_attr_init(attr) != 0) return -1;

#ifdef __SWITCH__
    /* v36.72-phonestacks parity: allocate the stack OURSELVES with a
     * no-access page at the growing edge. libnx pthread stacks come from
     * the malloc arena with NO guard — an interpreter recursion past the
     * 500-frame Java guard would then scribble over the Java heap instead
     * of faulting. Degrade gracefully: if memalign/mprotect/setstack is
     * not available we fall back to setstacksize (still pool-owned).
     * NOJME_VM_NOGUARD=1 skips the custom-stack path entirely (field
     * escape hatch / A-B); NOJME_VM_MPROTECT=0 skips only the guard page
     * (v36.74: svcSetMemoryPermission refusal => plain stack + guardless
     * setstacksize fallback, never a spawn failure). */
    if (!getenv("NOJME_VM_NOGUARD")) {
        size_t body = (want + 4095u) & ~(size_t)4095u;    /* page-rounded  */
        void* base = memalign(4096, body + 4096);
        if (base) {
            static int use_mprotect = -1;
            if (use_mprotect < 0) {
                const char* e = getenv("NOJME_VM_MPROTECT");
                use_mprotect = (e && e[0] == '0') ? 0 : 1;
            }
            int guard_ok = 0;
            if (use_mprotect && mprotect(base, 4096, PROT_NONE) == 0) {
                guard_ok = 1;
            }
            if (guard_ok &&
                pthread_attr_setstack(attr, (char*)base + 4096, body) == 0) {
                w->stack_mem = base;
                w->stack_total = body + 4096;
                return 0;   /* attr owns the precise-guard stack */
            }
            /* Guard page refused (or disabled) but the stack block itself
             * is fine — hand it to pthread_attr_setstack WITHOUT the
             * guard: the first page becomes plain usable stack. */
            if (pthread_attr_setstack(attr, (char*)base + 4096, body) == 0) {
                w->stack_mem = base;
                w->stack_total = body + 4096;
                return 0;
            }
            /* setstack refused: free and fall through */
            free(base);
            base = NULL;
        }
    }
#endif /* __SWITCH__ */

    if (pthread_attr_setstacksize(attr, want) == 0) return 0;
    return -1;
#endif /* !_WIN32 */
}

/* ------------------------------------------------------------------ */
/* Submit                                                              */
/* ------------------------------------------------------------------ */
int vm_pool_submit(void (*fn)(void*), void* arg,
                    void (*on_claim)(void* ctx, int slot), void* claim_ctx) {
    if (!fn) return -1;

    pthread_mutex_lock(&g_pool_mu);
    if (g_pool_shutdown) {
        pthread_mutex_unlock(&g_pool_mu);
        return -1;
    }

    /* Fast path: a parked worker. */
    {
        int waited_us = 0;
    rescan:
        for (int i = 0; i < g_worker_count; i++) {
            VmWorker* w = &g_workers[i];
            if (!w->started) continue;
            pthread_mutex_lock(&w->mu);
            if (w->state == VMW_IDLE) {
                /* Poison check under the SAME w->mu that the worker's exit
                 * decision takes: a worker that already decided to exit
                 * cannot be claimed (its loop breaks only from the IDLE
                 * park, and we hold the mutex across the transition). */
                if (g_pool_shutdown) {
                    pthread_mutex_unlock(&w->mu);
                    pthread_mutex_unlock(&g_pool_mu);
                    return -1;
                }
                w->fn = fn;
                w->arg = arg;
                w->state = VMW_CLAIMED;
                /* Registry re-point while the worker still CANNOT run:
                 * the signal happens strictly after on_claim returns. */
                if (on_claim) on_claim(claim_ctx, w->slot);
                pthread_cond_signal(&w->cv);
                pthread_mutex_unlock(&w->mu);
                pthread_mutex_unlock(&g_pool_mu);
                return 0;
            }
            pthread_mutex_unlock(&w->mu);
        }
        /* BOUNDED PARK-WAIT (v36.73): the classic churn case — start,join,
         * start,join... — races the worker's park: Thread.join() returns on
         * the job tail's cond broadcast, which happens BEFORE the worker
         * takes its pool lock back, so the very next Thread.start found no
         * IDLE worker and GREW the pool on every iteration (the selftest
         * grew it past 1 for 300 sequential starts). Wait a moment for a
         * park broadcast instead: the worker parks within microseconds of
         * the join signal. Under genuine concurrency (a burst) nobody is
         * about to park and the wait expires after 2 ms — then we grow.
         * Skipped entirely for the empty pool (nothing can park). */
        if (g_worker_count > 0 && waited_us < 2000) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 250 * 1000;   /* 250 us slice */
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&g_pool_parked_cv, &g_pool_mu, &ts);
            waited_us += 250;
            if (g_pool_shutdown) {
                pthread_mutex_unlock(&g_pool_mu);
                return -1;
            }
            goto rescan;
        }
    }

    /* Grow path: spawn a new worker (one pthread_create per worker for
     * the WHOLE process lifetime — never per Thread.start). */
    if (g_pool_shutdown) {   /* re-check: shutdown may have started while we
                              * were in the park-wait above */
        pthread_mutex_unlock(&g_pool_mu);
        return -1;
    }
    if (g_worker_count < VM_POOL_MAX) {
        VmWorker* w = &g_workers[g_worker_count];
#ifndef _WIN32
        pthread_attr_t attr;
        int attr_ok = (worker_setup_attr(w, &attr) == 0);
#else
        int attr_ok = 0;   /* shim pthread_create ignores attrs */
        (void)worker_setup_attr; (void)worker_stack_bytes;
#endif

        w->fn = fn;
        w->arg = arg;
        w->state = VMW_CLAIMED;   /* hand it the job before it starts */
        w->slot = g_worker_count; /* worker_setup_attr set stack_mem if the
                                     precise-guard path succeeded */
        pthread_mutex_init(&w->mu, NULL);
        pthread_cond_init(&w->cv, NULL);
        {
            pthread_mutex_lock(&w->mu);   /* hold across create: the newborn
                                             worker blocks on this lock in
                                             vm_worker_main until we release */
            if (pthread_create(&w->thread,
#ifndef _WIN32
                                 attr_ok ? &attr : NULL,
#else
                                 NULL,
#endif
                                 vm_worker_main, w) == 0) {
                w->started = 1;
                g_worker_count++;
                /* Registry re-point only now that creation cannot fail —
                 * the worker still cannot run (we hold w->mu). */
                if (on_claim) on_claim(claim_ctx, w->slot);
                pthread_mutex_unlock(&w->mu);
#ifndef _WIN32
                if (attr_ok) pthread_attr_destroy(&attr);
#endif
                pthread_mutex_unlock(&g_pool_mu);
                return 0;
            }
            /* create failed: undo and fall through to the cap/no-path */
            w->fn = NULL;
            w->arg = NULL;
            w->state = VMW_IDLE;
            pthread_mutex_unlock(&w->mu);
            pthread_mutex_destroy(&w->mu);
            pthread_cond_destroy(&w->cv);
            vm_stack_release(w); /* v36.74 [MMAN-GUARD]: unguard first */
#ifndef _WIN32
            if (attr_ok) pthread_attr_destroy(&attr);
#endif
        }
    }

    pthread_mutex_unlock(&g_pool_mu);
    return -1;   /* pool exhausted / spawn failed: Thread.start no-op (old semantics) */
}

/* ------------------------------------------------------------------ */
/* Teardown                                                            */
/* ------------------------------------------------------------------ */
void vm_pool_shutdown_join(void) {
    pthread_mutex_lock(&g_pool_mu);
    g_pool_shutdown = 1;
    /* Wake any submitter parked in the bounded park-wait — it re-checks
     * the flag and bails instead of claiming an exiting worker. */
    pthread_cond_broadcast(&g_pool_parked_cv);
    for (int i = 0; i < g_worker_count; i++) {
        VmWorker* w = &g_workers[i];
        if (!w->started) continue;
        pthread_mutex_lock(&w->mu);
        pthread_cond_broadcast(&w->cv);   /* wake IDLE parkers (CLAIMED jobs
                                            run once more, then exit) */
        pthread_mutex_unlock(&w->mu);
    }
    pthread_mutex_unlock(&g_pool_mu);

    int joined = 0;
    for (int i = 0; i < g_worker_count; i++) {
        VmWorker* w = &g_workers[i];
        if (!w->started) continue;
        pthread_join(w->thread, NULL);
        joined++;
        pthread_mutex_destroy(&w->mu);
        pthread_cond_destroy(&w->cv);
        vm_stack_release(w); /* v36.74 [MMAN-GUARD]: unguard first */
        w->started = 0;
        w->state = VMW_IDLE;
        w->fn = NULL;
        w->arg = NULL;
    }

    if (joined > 0) {
        if (&sw_trace_force && sw_trace_force) {
            sw_trace_force("[VMPOOL] shutdown: joined %d worker(s), stacks freed",
                           joined);
        }
    }
    g_worker_count = 0;
    g_pool_shutdown = 0;   /* next session starts a fresh pool */
}
