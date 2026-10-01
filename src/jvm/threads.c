/*
 * J2ME Emulator - Cooperative Multithreading Implementation
 * User-space thread scheduling with real context switching
 * 
 * Uses ucontext/makecontext/swapcontext for proper thread switching.
 * Each thread has its own stack, allowing true cooperative multitasking.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#ifdef MEM_FREE
#undef MEM_FREE
#endif
/* v34.10: the GC safepoint sync below (g_sp_mutex/g_sp_cond) uses
 * pthread_mutex_t / pthread_cond_t on BOTH platforms now — on Windows via
 * the project shim (lazy-init CRITICAL_SECTION / CONDITION_VARIABLE,
 * PTHREAD_COND_INITIALIZER). Same pattern as native.c / media.c. */
#include "win_thread_shim.h"
#else
#include <pthread.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <errno.h>
#include <stdbool.h>

#ifndef _WIN32
#if defined(__SWITCH__) && defined(__aarch64__)
/* v34.87: newlib/libnx have no <ucontext.h> (glibc-only API). The
 * cooperative scheduler below uses exactly the get/set/make/swapcontext
 * subset that src/switch/switch_ucontext.[ch] provides (aarch64 asm;
 * functionally tested under qemu-aarch64 - scripts/switch_ucontext_test.sh).
 * Semantics match the glibc build: no signal masks (libnx has no POSIX
 * signals) and no thread-pointer switching (all cooperative contexts run
 * on one native thread, so __thread state is shared - same as glibc, where
 * swapcontext restores the very same TP value).
 * v34.90: the guard now matches switch_ucontext.h's own (aarch64) - the
 * HOST switchui-verify build also defines __SWITCH__ but compiles on
 * x86-64 glibc, which HAS <ucontext.h>; picking the vendored header there
 * left ucontext_t undefined (latent breakage since v34.87). */
#include "switch/switch_ucontext.h"
#else
#include <ucontext.h>
#endif
#include <sched.h>
#endif

#include "threads.h"
#include "jvm.h"
#include "debug.h"
#include "debug_macros.h"
#include "native.h"

/* v34.9 GC safepoint (defined in this file, used by monitor/heap paths) */
extern volatile int g_gc_safepoint_request;
void jvm_gc_safepoint_park(void);
int  jvm_gc_safepoint_arrived(void);
void jvm_gc_safepoint_release(void);

/* 
 * Global instruction counter for time-slicing 
 */
volatile uint64_t g_instruction_counter = 0;

/* v34.26 PERF: pending-terminated counter. reap_terminated_threads() used to
 * scan the whole scheduler table on EVERY yield (every 1000 opcodes) even when
 * zero threads had finished — an O(n) walk per yield for nothing. Terminating
 * threads bump this; the reaper (and only the reaper) clears it. */
static int g_terminated_pending = 0;

#ifdef _WIN32
/* Windows Thread-Local Storage for current JavaThread */
static DWORD g_win_tls_index = TLS_OUT_OF_INDEXES;
static bool g_win_tls_initialized = false;

static void win_tls_init(void) {
    if (!g_win_tls_initialized) {
        g_win_tls_index = TlsAlloc();
        g_win_tls_initialized = true;
    }
}
#endif

/* Yield interval - switch threads after this many opcodes */
#define YIELD_INTERVAL 1000

/* Forward declarations */
static void memory_barrier_acquire(void);
static void memory_barrier_release(void);

/* Maximum number of cooperative threads.
 * v34: raised 32 -> 128. The old 32-slot table filled with TERMINATED
 * corpses (they were never removed) and thread_create() then failed forever:
 * VmTest's thread group creates ~109 threads (100-thread batch + torn-test
 * writer/reader) and died exactly at the cap. The v34 reaper
 * (reap_terminated_threads) now frees finished threads' slots and stacks at
 * every yield/sleep/create-retry, so steady-state usage stays small; the
 * raised cap only bounds SIMULTANEOUSLY alive threads (worst case ~128 x
 * ~160KB native = ~20MB, reached only by thread-stress MIDlets). */
#define MAX_COOP_THREADS 128

/* Stack size for each thread (64KB) */
#define THREAD_STACK_SIZE (64 * 1024)

/* Thread control block */
typedef struct CoopThread {
    JavaThread* java_thread;
    struct CoopThread* next;
    ThreadState state;
    jlong wake_time_ms;
    int priority;
    /* v34.10: removed the v34.9 pthread_id/pthread_alive fields — they were
     * POSIX-only types in an unguarded struct and were never read anywhere
     * (the pthread watchdog lives in native.c's g_live registry). */

#ifndef _WIN32
    ucontext_t context;       /* Thread context */
    void* stack;              /* Thread stack */
    int context_valid;        /* Whether context is initialized */
#endif
} CoopThread;

/* Forward declaration for thread wrapper */
static void thread_entry_wrapper(void);
static void init_monitors(void);

/* Forward declaration for pthread_set_current_thread (used on Windows too) */
extern void pthread_set_current_thread(JavaThread* thread);

/* Global flag for uncaught exception - checked by main loop */
bool g_has_uncaught_exception = false;

bool thread_has_uncaught_exception(void) {
    return g_has_uncaught_exception;
}

/* Global scheduler state */
static struct {
    CoopThread* threads[MAX_COOP_THREADS];
    int thread_count;
    
    CoopThread* run_queue_head;
    CoopThread* run_queue_tail;
    
    CoopThread* sleep_list;
    
    CoopThread* current;
    JavaThread* main_thread;
    
    JVM* jvm;
    bool initialized;
    
#ifndef _WIN32
    ucontext_t main_context;    /* Main/scheduler context */
    ucontext_t* return_context; /* Context to return to after thread exits */
#endif
    
    /* Entry point for new threads */
    JavaMethod* entry_method;
    JavaObject* entry_obj;
    JavaThread* entry_java_thread;
} scheduler = {0};

/*
 * Thread-local storage to identify if current thread is a pthread
 * Used by monitor functions to determine synchronization method
 */
#ifndef _WIN32
static __thread int g_is_pthread = -1;  /* -1 = not set, 0 = cooperative, 1 = pthread */
#endif

/* Get current time in milliseconds */
static jlong get_time_ms(void) {
#ifdef _WIN32
    return (jlong)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (jlong)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

/* Initialize thread system */
int threads_init(JVM* jvm) {
    if (scheduler.initialized) return JNI_OK;
    
#ifdef _WIN32
    /* Initialize Windows TLS */
    win_tls_init();
#endif
    
    scheduler.jvm = jvm;
    scheduler.thread_count = 0;
    scheduler.run_queue_head = NULL;
    scheduler.run_queue_tail = NULL;
    scheduler.sleep_list = NULL;
    scheduler.current = NULL;
    scheduler.main_thread = NULL;
    scheduler.initialized = true;
    
    return JNI_OK;
}

/* v34.91 MULTI-SESSION FIX (Switch frontend runs several games per process):
 * the scheduler is a PROCESS-GLOBAL static, but jvm_destroy frees the JVM it
 * points at (and thread_destroy already freed the JavaThread structs). Two
 * consequences for the NEXT session, both ASan-confirmed:
 *   1) threads_init() early-returns on scheduler.initialized, leaving
 *      scheduler.jvm dangling at the DESTROYED JVM;
 *   2) reap_terminated_threads() walks stale scheduler.threads[] entries and
 *      WRITES into freed JavaThread memory (threads.c UAF).
 * jvm_destroy therefore calls this reset: frees the remaining CoopThread
 * shells + stacks (JavaThreads themselves are freed by thread_destroy),
 * zeroes the whole scheduler (initialized=false -> threads_init re-runs)
 * and clears the pending-reap / uncaught-exception flags. */
void threads_scheduler_reset(void) {
    for (int i = 0; i < scheduler.thread_count; i++) {
        CoopThread* c = scheduler.threads[i];
        if (!c) continue;
#ifndef _WIN32
        if (c->stack) { free(c->stack); c->stack = NULL; }
#endif
        free(c);
        scheduler.threads[i] = NULL;
    }
    memset(&scheduler, 0, sizeof(scheduler));
    g_terminated_pending = 0;
    g_has_uncaught_exception = false;
}

/* Check if thread is in run queue */
static bool is_in_run_queue(CoopThread* coop) {
    CoopThread* p = scheduler.run_queue_head;
    while (p) {
        if (p == coop) return true;
        p = p->next;
    }
    return false;
}

/* Add thread to run queue */
static void add_to_run_queue(CoopThread* coop) {
    if (!coop) return;
    
    if (is_in_run_queue(coop)) {
        return;
    }
    
    coop->state = THREAD_STATE_RUNNABLE;
    coop->next = NULL;
    
    if (!scheduler.run_queue_head) {
        scheduler.run_queue_head = coop;
        scheduler.run_queue_tail = coop;
    } else {
        scheduler.run_queue_tail->next = coop;
        scheduler.run_queue_tail = coop;
    }
}

/* Remove thread from run queue head */
static CoopThread* remove_from_run_queue(void) {
    if (!scheduler.run_queue_head) return NULL;
    
    CoopThread* coop = scheduler.run_queue_head;
    scheduler.run_queue_head = coop->next;
    
    if (!scheduler.run_queue_head) {
        scheduler.run_queue_tail = NULL;
    }
    
    coop->next = NULL;
    return coop;
}

/* Remove specific thread from queue */
__attribute__((unused))
static bool remove_specific_from_queue(CoopThread* target) {
    if (!target || !scheduler.run_queue_head) return false;
    
    if (scheduler.run_queue_head == target) {
        scheduler.run_queue_head = target->next;
        if (!scheduler.run_queue_head) {
            scheduler.run_queue_tail = NULL;
        } else if (scheduler.run_queue_tail == target) {
            scheduler.run_queue_tail = NULL;
        }
        target->next = NULL;
        return true;
    }
    
    CoopThread* prev = scheduler.run_queue_head;
    CoopThread* curr = prev->next;
    
    while (curr) {
        if (curr == target) {
            prev->next = curr->next;
            if (scheduler.run_queue_tail == target) {
                scheduler.run_queue_tail = prev;
            }
            target->next = NULL;
            return true;
        }
        prev = curr;
        curr = curr->next;
    }
    
    return false;
}

/* Wake up sleeping threads.
 * v34.26 PERF (armv7 stutter): this runs on EVERY interpreter yield (default
 * every 1000 opcodes). get_time_ms() is a clock_gettime call — a real syscall
 * on ARM kernels without a fast vDSO path (~1-10us). With an EMPTY sleep list
 * that syscall was pure waste, and at 20k yields/sec it dominated the hot
 * loop. Skip everything when nobody is sleeping. */
static void wakeup_sleeping_threads(void) {
    if (!scheduler.sleep_list) return;  /* fast path: no sleepers, no clock */
    
    jlong now = get_time_ms();
    
    while (scheduler.sleep_list && scheduler.sleep_list->wake_time_ms <= now) {
        CoopThread* coop = scheduler.sleep_list;
        scheduler.sleep_list = coop->next;
        coop->next = NULL;
        add_to_run_queue(coop);
    }
}

/* v34 FIX (VmTest "100 short-lived threads all complete" + "torn-test reads: 0"):
 * the cooperative scheduler NEVER removed TERMINATED threads from
 * scheduler.threads[], so the fixed MAX_COOP_THREADS (32) table filled up with
 * corpses after ~32 thread creations and thread_create() returned NULL forever
 * (Thread.start() silently failing). VmTest's thread group creates ~109
 * threads: the 100-thread batch ran out of slots and the torn-test
 * writer/reader could not be created at all (reads=0).
 *
 * This reaper compacts TERMINATED threads out of the scheduler table and
 * frees their native resources (ucontext stack, Java stack arena, thread-local
 * allocator). The JavaThread shell stays registered in jvm->threads[] so
 * native isAlive()/join() lookups and the GC registry keep working
 * (current_frame/pending_exception are cleared, making the shell GC-inert —
 * same state as a freshly created, never-started thread).
 *
 * Safety: a terminated coop thread stopped executing the moment
 * thread_entry_wrapper() did setcontext() away from it, so its stack is
 * inactive when this runs (and we never reap scheduler.current). */
static void reap_terminated_threads(void) {
    int dropped = 0;
    for (int i = 0; i < scheduler.thread_count; ) {
        CoopThread* coop = scheduler.threads[i];
        if (coop && coop != scheduler.current &&
            coop->state == THREAD_STATE_TERMINATED) {
            JavaThread* jt = coop->java_thread;
#ifndef _WIN32
            if (coop->stack) {
                free(coop->stack);
                coop->stack = NULL;
            }
            coop->context_valid = 0;
#endif
            if (jt) {
                jt->exec_jmp_buf_valid = 0;
                jt->current_frame = NULL;   /* frames lived in the freed arena */
                jt->pending_exception = NULL;
                jt->is_alive = false;
                if (jt->stack_base) {
                    free(jt->stack_base);
                    jt->stack_base = NULL;
                    jt->stack_used = 0;
                }
                if (jt->allocator.heap) {
                    free(jt->allocator.heap);
                    jt->allocator.heap = NULL;
                    jt->allocator.heap_used = 0;
                }
            }
            for (int j = i + 1; j < scheduler.thread_count; j++) {
                scheduler.threads[j - 1] = scheduler.threads[j];
            }
            scheduler.thread_count--;
            scheduler.threads[scheduler.thread_count] = NULL;
            free(coop);
            dropped++;
        } else {
            i++;
        }
    }
    if (dropped > 0) {
        g_terminated_pending = 0;
        LOG_SAFE("[THREADS] Reaped %d terminated thread(s), scheduler table %d/%d\n",
                 dropped, scheduler.thread_count, MAX_COOP_THREADS);
    }
}

/* Create new Java thread */
JavaThread* thread_create(JVM* jvm, const char* name, jint priority, JavaObject* thread_obj) {
    (void)jvm;
    
    if (scheduler.thread_count >= MAX_COOP_THREADS) {
        /* v34: try to free slots held by TERMINATED threads before giving up */
        reap_terminated_threads();
        if (scheduler.thread_count >= MAX_COOP_THREADS) {
            return NULL;
        }
    }
    
    JavaThread* java_thread = (JavaThread*)calloc(1, sizeof(JavaThread));
    if (!java_thread) return NULL;
    
    java_thread->id = scheduler.thread_count + 1;
    java_thread->name = name ? strdup(name) : NULL;
    java_thread->priority = priority;
    java_thread->is_daemon = false;
    java_thread->is_alive = false;
    java_thread->thread_object = thread_obj;
    java_thread->current_frame = NULL;
    java_thread->pending_exception = NULL;
    java_thread->stack_size = JAVA_STACK_SIZE;
    java_thread->exec_jmp_buf = NULL;
    java_thread->exec_jmp_buf_valid = 0;
    /* v36.05: creation timestamp for the liveness age fallback (see the
     * created_ms comment in jvm.h). */
    {
        struct timespec cts;
        clock_gettime(CLOCK_MONOTONIC, &cts);
        java_thread->created_ms = (jlong)cts.tv_sec * 1000 + cts.tv_nsec / 1000000;
    }
    
    /* Allocate stack for thread-local allocator */
    java_thread->stack_base = malloc(JAVA_STACK_SIZE);
    if (!java_thread->stack_base) {
        free(java_thread->name);
        free(java_thread);
        return NULL;
    }
    java_thread->stack_used = 0;
    
    java_thread->allocator.heap_size = 64 * 1024;
    java_thread->allocator.heap = malloc(java_thread->allocator.heap_size);
    java_thread->allocator.heap_used = 0;
    
    /* Create CoopThread wrapper */
    CoopThread* coop = (CoopThread*)calloc(1, sizeof(CoopThread));
    if (!coop) {
        free(java_thread->stack_base);
        free(java_thread->name);
        free(java_thread);
        return NULL;
    }
    
    coop->java_thread = java_thread;
    coop->state = THREAD_STATE_NEW;
    coop->priority = priority;
    coop->wake_time_ms = 0;
    coop->next = NULL;
    
#ifndef _WIN32
    /* Allocate stack for context switching */
    coop->stack = malloc(THREAD_STACK_SIZE);
    if (!coop->stack) {
        free(coop);
        free(java_thread->stack_base);
        free(java_thread->name);
        free(java_thread);
        return NULL;
    }
    coop->context_valid = 0;
#endif
    
    scheduler.threads[scheduler.thread_count++] = coop;
    
    /* CRITICAL FIX: Also add to jvm->threads[] so GC can see this thread's frames!
     * Without this, threads created after the main thread are invisible to GC,
     * and objects stored in their local variables will be incorrectly freed.
     * FIX (audit T-2, v18): the registry now grows dynamically — previously,
     * threads beyond the fixed 16-slot cap were never registered (invisible to
     * GC) while still executing. If we cannot allocate the registry slot at
     * all, refuse to create the thread entirely so Thread.start() surfaces a
     * failure instead of running an untracked thread. */
    if (jvm) {
        if (jvm->thread_count >= jvm->threads_capacity) {
            jint new_cap = jvm->threads_capacity * 2;
            JavaThread** grown = (JavaThread**)realloc(jvm->threads,
                    (size_t)new_cap * sizeof(JavaThread*));
            if (!grown) {
                LOG_SAFE("[THREADS] ERROR: cannot grow JVM thread registry\n");
                scheduler.thread_count--;
#ifndef _WIN32
                free(coop->stack);
#endif
                free(coop);
                free(java_thread->allocator.heap);
                free(java_thread->stack_base);
                free(java_thread->name);
                free(java_thread);
                return NULL;
            }
            jvm->threads = grown;
            jvm->threads_capacity = new_cap;
        }
        jvm->threads[jvm->thread_count++] = java_thread;
    }
    
    /* Store main thread reference */
    if (name && strcmp(name, "main") == 0) {
        scheduler.main_thread = java_thread;
        scheduler.current = coop;
        coop->state = THREAD_STATE_RUNNABLE;
        java_thread->is_alive = true;
        
        scheduler.run_queue_head = coop;
        scheduler.run_queue_tail = coop;
        
#ifdef _WIN32
        /* Set TLS for main thread on Windows */
        pthread_set_current_thread(java_thread);
#else
        /* Mark this thread as cooperative (not pthread) */
        g_is_pthread = 0;
#endif
    }
    
    return java_thread;
}

/* Start a thread */
int thread_start(JVM* jvm, JavaThread* thread) {
    (void)jvm;
    if (!thread) return JNI_ERR;
    
    CoopThread* coop = NULL;
    for (int i = 0; i < scheduler.thread_count; i++) {
        if (scheduler.threads[i]->java_thread == thread) {
            coop = scheduler.threads[i];
            break;
        }
    }
    
    if (!coop) {
        return JNI_ERR;
    }
    
    if (coop->state != THREAD_STATE_NEW) {
        return JNI_ERR;
    }
    
    thread->is_alive = true;
    add_to_run_queue(coop);
    
    return JNI_OK;
}

#ifndef _WIN32
/* Thread-local storage for current JavaThread in pthread threads */
static __thread JavaThread* tls_current_thread = NULL;

/* Set current thread for pthread */
void pthread_set_current_thread(JavaThread* thread) {
    tls_current_thread = thread;
}
#else
/* Windows TLS-based thread current setting */
void pthread_set_current_thread(JavaThread* thread) {
    win_tls_init();
    if (g_win_tls_index != TLS_OUT_OF_INDEXES) {
        TlsSetValue(g_win_tls_index, thread);
    }
}
#endif

/* ============================================================
 * v34.71 CRITICAL FIX (heap corruption — see heap.c TLAB gate).
 * The MAIN Java thread runs on the frontend/headless OS thread.
 * Between driver exec-windows it executes pure native C code and
 * never polls GC safepoints; the GC census only counts it while
 * g_jvm_main_thread_executing != 0. An open TLAB chunk on that
 * thread therefore survives a whole mark/sweep and the sweep's
 * linear walk runs into the chunk's unflushed tail (stale object
 * bytes read as a GCObjectHeader -> "corrupted magic"). The heap
 * allocator needs to know "is the caller the main OS thread" to
 * disable TLAB for it between windows.
 * ============================================================ */
#ifndef _WIN32
static pthread_t g_main_os_thread;
#else
static DWORD g_main_os_thread = 0;
#endif
static volatile int g_main_os_thread_known = 0;

/* Called once from jvm_init(), which by construction runs on the
 * main/frontend OS thread. Idempotent. */
void jvm_record_main_os_thread(void) {
    if (!g_main_os_thread_known) {
#ifndef _WIN32
        g_main_os_thread = pthread_self();
#else
        g_main_os_thread = GetCurrentThreadId();
#endif
        g_main_os_thread_known = 1;
    }
}

int jvm_os_thread_is_main_java(void) {
    if (!g_main_os_thread_known) return 0;
#ifndef _WIN32
    return pthread_equal(pthread_self(), g_main_os_thread);
#else
    return GetCurrentThreadId() == g_main_os_thread;
#endif
}

/* Get current thread */
JavaThread* thread_current(JVM* jvm) {
    (void)jvm;
#ifdef _WIN32
    /* Windows: check TLS first for native threads */
    win_tls_init();
    if (g_win_tls_index != TLS_OUT_OF_INDEXES) {
        JavaThread* tls_thread = (JavaThread*)TlsGetValue(g_win_tls_index);
        if (tls_thread) {
            return tls_thread;
        }
    }
    /* Fallback to scheduler.current for cooperative threads */
    return scheduler.current ? scheduler.current->java_thread : scheduler.main_thread;
#else
    /* POSIX: First check TLS for pthread threads */
    if (tls_current_thread) {
        return tls_current_thread;
    }
#endif
    return scheduler.current ? scheduler.current->java_thread : scheduler.main_thread;
}

/* Set current thread */
JavaThread* thread_set_current(JavaThread* thread) {
    JavaThread* prev = scheduler.current ? scheduler.current->java_thread : NULL;
    
    for (int i = 0; i < scheduler.thread_count; i++) {
        if (scheduler.threads[i]->java_thread == thread) {
            scheduler.current = scheduler.threads[i];
            return prev;
        }
    }
    
    return prev;
}

/* Schedule next thread - cooperative context switch */
void thread_schedule(JVM* jvm) {
    (void)jvm;
    
    wakeup_sleeping_threads();
    
    /* v34: reclaim slots/stacks of finished threads (see reap_terminated_threads)
     * v34.26 PERF: skip the O(n) scan when nothing terminated. */
    if (g_terminated_pending > 0) reap_terminated_threads();
    
    /* If current thread is still runnable, put it back in queue */
    if (scheduler.current && 
        scheduler.current->state == THREAD_STATE_RUNNABLE &&
        scheduler.current->java_thread->is_alive) {
        add_to_run_queue(scheduler.current);
    }
    
    /* Get next thread from run queue */
    CoopThread* next = remove_from_run_queue();
    
    /* Skip if it's the same thread and there are others */
    if (next && next == scheduler.current && scheduler.run_queue_head) {
        add_to_run_queue(next);
        next = remove_from_run_queue();
    }
    
    if (next && next != scheduler.current) {
#ifndef _WIN32
        /* Capture the OLD current BEFORE overwriting: it's the outgoing frame */
        CoopThread* prev = scheduler.current;
#endif
        scheduler.current = next;
        
#ifndef _WIN32
        if (prev && prev->context_valid) {
            /* Switch to next thread */
            /* JMM FIX: Memory barrier before context switch to ensure all writes are visible */
            memory_barrier();
            swapcontext(&prev->context, &next->context);
            /* JMM FIX: Memory barrier after context switch to see all writes from other thread */
            memory_barrier();
        } else if (next->context_valid) {
            /* First time switching from main */
            memory_barrier();
            setcontext(&next->context);
        }
#endif
    }
}

/* v36.04 VM-THREAD-DEATH: forced-trace channel for VM thread termination.
 *
 * WHY: the DoomRPG-relaunch field trace (v36.03) showed a loader thread
 * ("t2") dying ~1.6 s into the second session while the main thread spun
 * forever — and the log had NOTHING: the existing [THREAD-EXIT]/[THREAD]
 * lines go to stderr (LOG_SAFE), which is invisible in the Switch log.txt;
 * only sw_trace_force lines reach it. A silently dead game thread is the
 * single most valuable signal in "game hangs" reports: its exit reason
 * (clean run() return vs uncaught exception class + message + stack) names
 * the failing subsystem immediately.
 *
 * This helper is frontend-agnostic (weak sw_trace_force — compiles away on
 * desktop) and is called from BOTH thread-exit paths (coop wrapper below,
 * pthread runner in native.c). Throttled per name so a respawn loop cannot
 * flood the log. */
void jvm_thread_death_forced(const char* name, JavaObject* pending_ex,
                             const char* stack_trace, const char* site) {
    extern void sw_trace_force(const char* fmt, ...) __attribute__((weak));
    if (&sw_trace_force && sw_trace_force) {
        char line[320];
        int pos = 0;
        const char* cls = "?";
        char msg[96] = "";
        if (pending_ex) {
            if (pending_ex->header.clazz && pending_ex->header.clazz->class_name)
                cls = pending_ex->header.clazz->class_name;
            {
                extern JavaValue native_get_field_value(JavaObject* obj, const char* field_name);
                JavaString* m = (JavaString*)native_get_field_value(pending_ex, "detailMessage").ref;
                if (m) {
                    extern const char* string_utf8(JVM* jvm, JavaString* str);
                    const char* u = string_utf8(NULL, m);
                    if (u) snprintf(msg, sizeof(msg), " \"%.60s\"", u);
                }
            }
        }
        pos = snprintf(line, sizeof(line), "[VM-THREAD-DEATH] '%s' (%s): %s",
                       name ? name : "(unnamed)", site ? site : "?",
                       pending_ex ? "uncaught exception" : "run() returned");
        if (pending_ex && pos > 0 && pos < (int)sizeof(line))
            pos += snprintf(line + pos, sizeof(line) - pos, " %s%s", cls, msg);
        if (stack_trace && stack_trace[0] && pos > 0 && pos < (int)sizeof(line))
            pos += snprintf(line + pos, sizeof(line) - pos, " | %.100s",
                            stack_trace);
        if (pos > 0 && pos < (int)sizeof(line))
            sw_trace_force("%s", line);

        /* v36.05 FULL STACK: the v36.04 field trace proved the value — the
         * NPE stack named the failing game method instantly — but the
         * single-line cap (100 chars) cut the chain after 3 frames, and
         * the caller of DoomRPG.a(...) (i.e. WHERE in the load path the
         * game died) was lost. Emit the captured trace as ONE TRACE LINE
         * PER FRAME (the capture already limits itself to 20 frames):
         * nothing is truncated, the whole unwind is in log.txt. */
        if (stack_trace && stack_trace[0]) {
            const char* cur = stack_trace;
            int frames = 0;
            while (cur && *cur && frames < 24) {
                const char* nl = strchr(cur, '\n');
                size_t len = nl ? (size_t)(nl - cur) : strlen(cur);
                while (len > 0 && (cur[len - 1] == '\r' || cur[len - 1] == ' '))
                    len--;
                if (len == 0) { /* blank segment */
                    if (!nl) break;
                    cur = nl + 1;
                    continue;
                }
                char fl[320]; /* TRACE_LINE_CAP (switch_trace.h not included here) */
                int fp = snprintf(fl, sizeof(fl), "[VM-THREAD-DEATH]  %.*s",
                                  (len > sizeof(fl) - 32) ? (int)(sizeof(fl) - 32) : (int)len,
                                  cur);
                (void)fp;
                sw_trace_force("%s", fl);
                frames++;
                if (!nl) break;
                cur = nl + 1;
            }
        }
    }
}

/* Thread entry wrapper - calls run() method */
#ifndef _WIN32
static void thread_entry_wrapper(void) {
#else
__attribute__((unused))
static void thread_entry_wrapper(void) {
#endif
    if (!scheduler.current) return;
    
    JavaThread* java_thread = scheduler.current->java_thread;
    JavaMethod* method = scheduler.entry_method;
    JavaObject* obj = scheduler.entry_obj;
    
    if (method && obj) {
        extern int execute_method(JVM* jvm, JavaThread* thread, JavaMethod* method, 
                                  JavaValue* args, JavaValue* result);
        JavaValue this_arg = { .ref = obj };
        JavaValue result;
        execute_method(scheduler.jvm, java_thread, method, &this_arg, &result);
    }
    
    /* Thread finished - mark as terminated */
    java_thread->is_alive = false;
    scheduler.current->state = THREAD_STATE_TERMINATED;
    g_terminated_pending++;  /* v34.26 PERF: tell the reaper there is work */
    /* v36.12: this Java thread's frames were interpreted on THIS pthread's
     * frame pool; the pool is _Thread_local — flush it when the run()
     * finishes so a dead thread never takes pooled blocks with it
     * (re-creates lazily; see jvm_frame_pool_flush in execute.c). */
    {
        extern void jvm_frame_pool_flush(void);
        jvm_frame_pool_flush();
    }
    /* v34.19 DIAG (Worms Forts 3D silent freeze): one-line always-on log so
     * "game thread vanished without an exception" is distinguishable from
     * "game thread stuck in sleep/wait" in production logs. */
    LOG_SAFE("[THREAD-EXIT] Thread '%s' finished (run() returned%s)\n",
             java_thread->name ? java_thread->name : "(unnamed)",
             java_thread->pending_exception ? ", uncaught exception" : "");
    /* v36.04 VM-THREAD-DEATH: the same signal into the Switch trace file
     * (with the exception class/message/stack — stderr never reaches it). */
    jvm_thread_death_forced(java_thread->name, java_thread->pending_exception,
                            java_thread->exception_stack_trace, "coop");
    
    /* Check for uncaught exception - in J2ME, this should NOT stop the app!
     * Only the thread dies, the rest of the application continues.
     * This matches the behavior of real J2ME implementations.
     */
    if (java_thread->pending_exception) {
        DEBUG_LOG("[THREAD] Thread '%s' terminated with uncaught exception: %s (app continues)",
                java_thread->name ? java_thread->name : "(unnamed)",
                java_thread->pending_exception->header.clazz ? 
                java_thread->pending_exception->header.clazz->class_name : "?");
        /* DO NOT set g_has_uncaught_exception = true - app should continue */
    }
    
    /* Check if any threads are still runnable */
    bool has_runnable = false;
    for (int i = 0; i < scheduler.thread_count; i++) {
        if (scheduler.threads[i]->state == THREAD_STATE_RUNNABLE ||
            scheduler.threads[i]->state == THREAD_STATE_WAITING ||
            scheduler.threads[i]->state == THREAD_STATE_TIMED_WAITING) {
            has_runnable = true;
            break;
        }
    }
    
    if (!has_runnable) {
        if (g_j2me_runtime_debug) LOG_SAFE("[THREAD] No runnable threads remaining, stopping JVM\n");
        /* v36.32 [DIRECT-DESTROY-GUARD]: cooperative/pthread runner path —
         * the midlet finished naturally; close destroyApp delivery so no
         * later system-side call re-runs save bytecode on the dead session */
        midlet_mark_destroyed();
        if (scheduler.jvm) {
            scheduler.jvm->running = false;
        }
    }
    
    /* Switch back to scheduler/main context */
#ifndef _WIN32
    if (scheduler.return_context) {
        setcontext(scheduler.return_context);
    }
#endif
}

/* Execute a thread's run method in its own context */
void thread_execute_run(JVM* jvm, JavaThread* java_thread, JavaMethod* run_method, JavaObject* run_obj) {
    (void)jvm;
#ifndef _WIN32
    CoopThread* coop = NULL;
    for (int i = 0; i < scheduler.thread_count; i++) {
        if (scheduler.threads[i]->java_thread == java_thread) {
            coop = scheduler.threads[i];
            break;
        }
    }
    
    if (!coop) return;
    
    /* Set up entry point */
    scheduler.entry_method = run_method;
    scheduler.entry_obj = run_obj;
    scheduler.entry_java_thread = java_thread;
    
    /* Initialize thread context */
    getcontext(&coop->context);
    
    coop->context.uc_stack.ss_sp = coop->stack;
    coop->context.uc_stack.ss_size = THREAD_STACK_SIZE;
    coop->context.uc_link = &scheduler.main_context;  /* Return to main when done */
    
    makecontext(&coop->context, thread_entry_wrapper, 0);
    coop->context_valid = 1;
    
    /* Save current context and switch to new thread */
    scheduler.return_context = &scheduler.main_context;
    
    /* CRITICAL: Set current to the new thread BEFORE swapcontext,
     * so thread_entry_wrapper sees the correct current thread */
    scheduler.current = coop;
    
    swapcontext(&scheduler.main_context, &coop->context);
    
    /* Restore current thread pointer */
    for (int i = 0; i < scheduler.thread_count; i++) {
        if (scheduler.threads[i]->java_thread == scheduler.main_thread) {
            scheduler.current = scheduler.threads[i];
            break;
        }
    }
#else
    /* Windows: just execute synchronously */
    extern int execute_method(JVM* jvm, JavaThread* thread, JavaMethod* method, 
                              JavaValue* args, JavaValue* result);
    JavaValue this_arg = { .ref = run_obj };
    JavaValue result;
    execute_method(jvm, java_thread, run_method, &this_arg, &result);
    java_thread->is_alive = false;
    
    /* Check for uncaught exception on Windows - in J2ME, this should NOT stop the app! */
    if (java_thread->pending_exception) {
        DEBUG_LOG("[THREAD] Thread '%s' terminated with uncaught exception: %s (app continues)",
                java_thread->name ? java_thread->name : "(unnamed)",
                java_thread->pending_exception->header.clazz ? 
                java_thread->pending_exception->header.clazz->class_name : "?");
        /* DO NOT set g_has_uncaught_exception = true - app should continue */
    }
    
    /* Check if any threads are still runnable on Windows */
    bool has_runnable = false;
    for (int i = 0; i < scheduler.thread_count; i++) {
        if (scheduler.threads[i] && 
            (scheduler.threads[i]->state == THREAD_STATE_RUNNABLE ||
             scheduler.threads[i]->state == THREAD_STATE_WAITING ||
             scheduler.threads[i]->state == THREAD_STATE_TIMED_WAITING)) {
            has_runnable = true;
            break;
        }
    }
    
    if (!has_runnable) {
        if (g_j2me_runtime_debug) LOG_SAFE("[THREAD] No runnable threads remaining, stopping JVM\n");
        /* v36.32 [DIRECT-DESTROY-GUARD]: same as the POSIX path — close
         * destroyApp delivery when the VM finishes naturally (Windows). */
        midlet_mark_destroyed();
        jvm->running = false;
    }
#endif
}

/* Yield to other threads */
void thread_yield(JVM* jvm) {
    (void)jvm;
    
#ifdef _WIN32
    /* Windows: use native threads only, no cooperative switching */
    /* Just yield to OS scheduler */
    Sleep(0);
    return;
#else
    /* POSIX: cooperative threading with ucontext */
    if (!scheduler.initialized || !scheduler.current) return;
    
    wakeup_sleeping_threads();
    
    /* v34: reclaim slots/stacks of finished threads at every real context
     * switch point. thread_schedule() is legacy/dead on this path - ALL
     * cooperative switching goes through thread_yield().
     * v34.26 PERF: skip the O(n) scan when nothing terminated. */
    if (g_terminated_pending > 0) reap_terminated_threads();

    /* v34.9: deliver deferred key events on the MAIN VM thread only (the
     * game thread that runs o.p()'s key drain). Delivering on any yielding
     * thread (e.g. the loading thread) re-introduced the queue race with
     * the game thread's o.p() drain. Foreign frontend threads only POST. */
    {
        /* v34.9: key delivery moved to the interpreter loop (execute.c) on
         * the display-owning thread. */
    }
    
    /* Process SDL events */
    /* FIX(reentrant-paint): skip event/repaint pumping while THIS thread is
     * already inside midp_process_repaints() -> paint(); pumping here recursed
     * paint<->repaint until the interpreter frame cap (SU-30 abort trace:
     * j.a(Graphics) x501). Extern in display.c; safe no-op when unset. */
    {
        extern int midp_is_inside_repaint_pump(void);
        if (!midp_is_inside_repaint_pump()) {
            extern void sdl_process_events_minimal(void);
            sdl_process_events_minimal();
        }
    }
    
    /* Handle TIMED_WAITING threads - add to sleep list for wakeup */
    if (scheduler.current->state == THREAD_STATE_TIMED_WAITING && 
        scheduler.current->wake_time_ms > 0) {
        /* Add to sleep list sorted by wake time */
        CoopThread* coop = scheduler.current;
        coop->next = NULL;
        
        if (!scheduler.sleep_list || coop->wake_time_ms < scheduler.sleep_list->wake_time_ms) {
            coop->next = scheduler.sleep_list;
            scheduler.sleep_list = coop;
        } else {
            CoopThread* prev = scheduler.sleep_list;
            while (prev->next && prev->next->wake_time_ms <= coop->wake_time_ms) {
                prev = prev->next;
            }
            coop->next = prev->next;
            prev->next = coop;
        }
    }
    
    /* Check if there are other runnable threads */
    bool has_other = false;
    CoopThread* p = scheduler.run_queue_head;
    while (p) {
        if (p != scheduler.current && p->java_thread->is_alive) {
            has_other = true;
            break;
        }
        p = p->next;
    }
    
    if (!has_other) {
        /* No other cooperative threads, but yield to OS scheduler for pthreads */
        /* This is critical for busy-wait patterns where main thread waits for child pthread.
         * v34.26 PERF (armv7): sched_yield() is a REAL syscall (~1-10us on
         * Cortex-A7). thread_yield() fires every YIELD_INTERVAL opcodes, so
         * calling sched_yield() every time meant ~20k syscalls/sec and
         * scheduling jitter = the reported stutter. The pthread runners only
         * need OS fairness occasionally: rate-limit to every 16th yield
         * (≈16k opcodes ≈ 1ms at 16 MIPS — still far tighter than any Java
         * code can observe). */
        static __thread int yield_os_counter = 0;
        if ((++yield_os_counter & 15) == 0) {
        #ifdef __linux__
            sched_yield();
        #elif defined(_WIN32)
            /* Win32: yield to OS scheduler */
            SwitchToThread();
        #elif defined(__APPLE__)
            pthread_yield_np();
        #elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
            pthread_yield();
        #else
            /* Fallback to nanosleep for minimal yield */
            struct timespec ts = {0, 1};
            nanosleep(&ts, NULL);
        #endif
        }
        return;
    }
    
    /* Save current context */
    if (scheduler.current->context_valid) {
        /* Put current thread back in queue ONLY if it's not blocked */
        if (scheduler.current->java_thread->is_alive && 
            scheduler.current->state != THREAD_STATE_BLOCKED) {
            scheduler.current->state = THREAD_STATE_RUNNABLE;
            add_to_run_queue(scheduler.current);
        }
        
        /* Get next thread */
        CoopThread* next = remove_from_run_queue();
        if (next && next != scheduler.current) {
            /* CRITICAL: Save prev BEFORE updating current, so we save the correct context */
            CoopThread* prev = scheduler.current;
            scheduler.current = next;
            
            /* Main thread's context is in scheduler.main_context, not CoopThread.context */
            ucontext_t* next_ctx = (next->java_thread == scheduler.main_thread) 
                                   ? &scheduler.main_context 
                                   : &next->context;
            
            /* JMM FIX: Memory barrier before context switch */
            memory_barrier();
            swapcontext(&prev->context, next_ctx);
            /* JMM FIX: Memory barrier after context switch */
            memory_barrier();
        }
    } else {
        /* First yield - save context and then switch */
        CoopThread* prev = scheduler.current;
        if (getcontext(&prev->context) == 0) {
            prev->context_valid = 1;
            
            /* Now do the actual context switch */
            /* Put current thread back in queue ONLY if not blocked */
            if (prev->java_thread->is_alive && 
                prev->state != THREAD_STATE_BLOCKED) {
                prev->state = THREAD_STATE_RUNNABLE;
                add_to_run_queue(prev);
            }
            
            /* Get next thread */
            CoopThread* next = remove_from_run_queue();
            if (next && next != prev) {
                scheduler.current = next;
                
                /* Main thread's context is in scheduler.main_context */
                ucontext_t* next_ctx = (next->java_thread == scheduler.main_thread) 
                                       ? &scheduler.main_context 
                                       : &next->context;
                
                /* JMM FIX: Memory barrier before context switch */
                memory_barrier();
                setcontext(next_ctx);
            }
        }
    }
#endif
}

/* ============================================================================
 * v34.45 FIX (Gameloft frame-limiter spin): java.lang.Thread.yield().
 * ============================================================================
 * The Gameloft main-loop idiom (both Assassin's Creed 1/2, Asphalt family):
 *
 *     while (System.currentTimeMillis() - t0 < FRAME_PERIOD) Thread.yield();
 *
 * used to return without resting the CPU: thread_yield()'s pthread-model
 * fast path is (at best) a rate-limited sched_yield(). Measured on AC2:
 * h.run(), re-queued each frame via callSerially, spun ~1.5M iterations per
 * 40 ms frame ON THE FRONTEND THREAD — every retro_run blocked 40 ms at
 * 100% CPU and starved RetroArch's audio/video work (worst on 1-2 core
 * armv7). AC1's runner thread burned its whole 5.4M instr/s budget spinning
 * in `while (g < 66) yield();` before every frame, leaving less for real
 * game work. On a real KVM phone yield() handed the single CPU back to the
 * green-thread scheduler (ms-granular), so the same loop cost ~nothing.
 *
 * Fix: adaptive per-OS-thread back-off, applied ONLY to the EXPLICIT Java
 * yield (the interpreter's YIELD_INTERVAL cadence must stay cheap — a
 * compute-heavy thread yields implicitly every 1000 opcodes and must not
 * be punished for it). Explicit yields spaced >2ms apart (normal loop
 * fairness, hand-offs, event pumps) behave exactly as before; yields
 * arriving <200us apart (a spin) escalate after 64 consecutive hits to a
 * 1ms KVM-style rest. Wall-clock deadline loops still exit on time — they
 * poll System.currentTimeMillis() — they just stop burning a core.
 * The 200us..2ms band is neutral on purpose: the 1ms spin-sleep itself must
 * not reset the escalation (else the loop degenerates into 64 free spins +
 * 1 sleep cycles).
 * ========================================================================== */
/* v36.37 DIAG: spin-rest счётчик thread_yield_explicit (NOJME_SCHED_TRACE). */
static __thread int s_trace_rest_n = 0;

void thread_yield_explicit(JVM* jvm) {
    static __thread uint64_t s_yield_last_ns;
    static __thread int s_yield_spin_hot;
    struct timespec yts;
    clock_gettime(CLOCK_MONOTONIC, &yts);
    uint64_t ynow_ns = (uint64_t)yts.tv_sec * 1000000000ULL + (uint64_t)yts.tv_nsec;
    /* v36.37 DIAG (Alien Shooter 3D 4-fps report): NOJME_SCHED_TRACE=1 —
     * счётчик явных Thread.yield() и 1 мс spin-rest'ов (см. выше), печать
     * раз в 5 с вместе с [SCHED] sleep-строками. */
    {
        static int s_trace_on = -1;
        if (s_trace_on < 0)
            s_trace_on = getenv("NOJME_SCHED_TRACE") ? 1 : 0;
        if (s_trace_on) {
            static __thread int s_n = 0, s_rest = 0;
            static __thread uint64_t s_t0 = 0;
            uint64_t now = ynow_ns / 1000000ULL;
            if (!s_t0) s_t0 = now;
            s_n++;
            if (now - s_t0 >= 5000) {
                fprintf(stderr,
                        "[SCHED] yield: calls=%d rests=%d wall=%llu ms\n",
                        s_n, s_rest, (unsigned long long)(now - s_t0));
                s_n = 0; s_rest = 0; s_t0 = now;
            }
        }
    }
    if (ynow_ns - s_yield_last_ns < 200000ULL) {
        if (s_yield_spin_hot < 4096) s_yield_spin_hot++;
    } else if (!s_yield_last_ns || ynow_ns - s_yield_last_ns > 2000000ULL) {
        s_yield_spin_hot = 0;
    }
    s_yield_last_ns = ynow_ns;
    if (s_yield_spin_hot >= 64) {
#ifdef _WIN32
        Sleep(1);
#else
        struct timespec yrq = { .tv_sec = 0, .tv_nsec = 1000000 };
        nanosleep(&yrq, NULL);
        {
            static __thread int s_rest_trace = -1;
            if (s_rest_trace < 0)
                s_rest_trace = getenv("NOJME_SCHED_TRACE") ? 1 : 0;
            if (s_rest_trace) s_trace_rest_n++;
        }
#endif
    }
    /* Always run the full cooperative path afterwards — the pumps (media
     * event delivery, SDL minimal events, coop wakeups) keep their exact
     * legacy semantics; we only ADD the rest before them. */
    thread_yield(jvm);
}

/* Check if it's time to yield */
bool thread_should_yield(void) {
    return (g_instruction_counter % YIELD_INTERVAL) == 0;
}

/* Increment counter and yield if needed */
void thread_tick(JVM* jvm) {
    g_instruction_counter++;
    
    if ((g_instruction_counter % YIELD_INTERVAL) == 0) {
        thread_yield(jvm);
    }
}

/* Get instruction counter */
uint64_t thread_get_instruction_counter(void) {
    return g_instruction_counter;
}

/* Sleep for milliseconds - COOPERATIVE VERSION
 * 
 * CRITICAL FIX: This is now cooperative! Instead of blocking nanosleep,
 * we mark the thread as TIMED_WAITING and yield to other threads.
 * The thread will be woken up after the timeout by the scheduler.
 */
int thread_sleep(JVM* jvm, jlong millis) {
    (void)jvm;
    
    if (millis < 0) millis = 0;
    if (millis > 10000) millis = 10000;
    
    wakeup_sleeping_threads();
    
    extern void sdl_process_events_minimal(void);
    sdl_process_events_minimal();
    
    if (millis > 0) {
        /* COOPERATIVE FIX: Mark current thread as TIMED_WAITING and set wake time */
        if (scheduler.current) {
            CoopThread* coop = scheduler.current;
            coop->state = THREAD_STATE_TIMED_WAITING;
            coop->wake_time_ms = get_time_ms() + millis;
            
            /* Yield to other threads - they can run while we "sleep" */
            thread_yield(jvm);
            
            /* When we return, the sleep time has elapsed */
            coop->state = THREAD_STATE_RUNNABLE;
        } else {
            /* Fallback for non-cooperative context (main thread before scheduler init) */
#ifdef _WIN32
            Sleep((DWORD)millis);
#else
            struct timespec ts = {
                .tv_sec = millis / 1000,
                .tv_nsec = (millis % 1000) * 1000000
            };
            nanosleep(&ts, NULL);
#endif
        }
    }
    
    return JNI_OK;
}

/* Check if thread is alive */
bool thread_is_alive(JavaThread* thread) {
    if (!thread) return false;
    return thread->is_alive;
}

/* Get thread state */
ThreadState thread_get_state(JavaThread* thread) {
    if (!thread) return THREAD_STATE_NEW;
    
    for (int i = 0; i < scheduler.thread_count; i++) {
        if (scheduler.threads[i]->java_thread == thread) {
            return scheduler.threads[i]->state;
        }
    }
    
    return THREAD_STATE_TERMINATED;
}

/* Set thread priority */
void thread_set_priority(JavaThread* thread, jint priority) {
    if (!thread) return;
    
    thread->priority = priority;
    
    for (int i = 0; i < scheduler.thread_count; i++) {
        if (scheduler.threads[i]->java_thread == thread) {
            scheduler.threads[i]->priority = priority;
            break;
        }
    }
}

/* Check if thread is interrupted */
bool thread_is_interrupted(JavaThread* thread, bool clear_flag) {
    if (!thread) return false;
    bool interrupted = thread->interrupted;
    if (clear_flag) thread->interrupted = false;
    return interrupted;
}

/* Find thread by thread object */
JavaThread* thread_find_by_object(JavaObject* thread_obj) {
    if (!thread_obj) return NULL;
    
    for (int i = 0; i < scheduler.thread_count; i++) {
        JavaThread* jt = scheduler.threads[i]->java_thread;
        if (jt && jt->thread_object == thread_obj) {
            return jt;
        }
    }
    return NULL;
}

/* Destroy a thread */
void thread_destroy(JVM* jvm, JavaThread* thread) {
    (void)jvm;
    if (!thread) return;
    
    for (int i = 0; i < scheduler.thread_count; i++) {
        if (scheduler.threads[i]->java_thread == thread) {
            CoopThread* coop = scheduler.threads[i];
            coop->state = THREAD_STATE_TERMINATED;
            g_terminated_pending++;  /* v34.26 PERF: work for the reaper */
            thread->is_alive = false;
            
#ifndef _WIN32
            if (coop->stack) {
                free(coop->stack);
                coop->stack = NULL;
            }
#endif
            break;
        }
    }
    
    /* Check if any threads are still runnable */
    bool has_runnable = false;
    for (int i = 0; i < scheduler.thread_count; i++) {
        if (scheduler.threads[i] &&
            (scheduler.threads[i]->state == THREAD_STATE_RUNNABLE ||
             scheduler.threads[i]->state == THREAD_STATE_WAITING ||
             scheduler.threads[i]->state == THREAD_STATE_TIMED_WAITING)) {
            has_runnable = true;
            break;
        }
    }
    
    if (!has_runnable && jvm) {
        if (g_j2me_runtime_debug) LOG_SAFE("[THREAD] thread_destroy: No runnable threads, stopping JVM\n");
        /* v36.32 [DIRECT-DESTROY-GUARD]: the midlet finished NATURALLY (its
         * last thread died). If it destroyed itself via a direct Java call
         * (this.destroyApp — invisible to our natives), the v36.31
         * double-delivery flag never went up; raise it now so a later
         * system-side delivery can never run save-state bytecode on the
         * dead session ("выход с ошибкой" fuel). */
        midlet_mark_destroyed();
        jvm->running = false;
    }
    
    free(thread->name);
    free(thread->stack_base);
    if (thread->allocator.heap) {
        free(thread->allocator.heap);
    }
    if (thread->exception_stack_trace) {
        free(thread->exception_stack_trace);
    }
    if (thread->exception_throw_info) {
        free(thread->exception_throw_info);
    }
    free(thread);
}

/* Check if there are runnable threads */
bool threads_has_runnable(void) {
    wakeup_sleeping_threads();
    return scheduler.run_queue_head != NULL;
}

/* Get current CoopThread */
void* thread_current_coop(void) {
    return scheduler.current;
}

/* Thread preparation data for deferred execution */
typedef struct {
    JavaMethod* run_method;
    JavaObject* run_obj;
    JavaThread* java_thread;
    JVM* jvm;
} ThreadRunData;

static ThreadRunData g_thread_run_data[MAX_COOP_THREADS];

/* Prepare a thread for execution (stores run info for later) */
void thread_prepare_run(JVM* jvm, JavaThread* java_thread, 
                        JavaMethod* run_method, JavaObject* run_obj) {
    if (!java_thread) return;
    
    int idx = -1;
    for (int i = 0; i < scheduler.thread_count; i++) {
        if (scheduler.threads[i]->java_thread == java_thread) {
            idx = i;
            break;
        }
    }
    
    if (idx < 0 || idx >= MAX_COOP_THREADS) return;
    
    g_thread_run_data[idx].run_method = run_method;
    g_thread_run_data[idx].run_obj = run_obj;
    g_thread_run_data[idx].java_thread = java_thread;
    g_thread_run_data[idx].jvm = jvm;
}

/* Get thread run data */
ThreadRunData* thread_get_run_data(JavaThread* java_thread) {
    for (int i = 0; i < scheduler.thread_count; i++) {
        if (scheduler.threads[i]->java_thread == java_thread) {
            return &g_thread_run_data[i];
        }
    }
    return NULL;
}

/*
 * Monitor functions
 * Supports both pthread-based threads and cooperative threads
 */

typedef struct CoopThread CoopThread;

typedef struct {
    JavaObject* owner;
    JavaThread* owner_thread;       /* For cooperative threads */
#ifndef _WIN32
    pthread_t owner_pthread;        /* For pthread threads */
    pthread_mutex_t mutex;          /* Mutex for pthread synchronization */
    pthread_cond_t entry_cond;      /* Condition variable for monitor entry/exit */
    pthread_cond_t wait_cond;       /* Condition variable for wait/notify */
    int wait_count;                 /* Number of pthreads waiting on wait_cond */
    int entry_wait_count;           /* Number of pthreads waiting on entry_cond */
    JavaThread* waiting_thread;     /* Thread currently waiting (for interrupt) */
    /* v34.65 WAIT-HANDSHAKE: bumped by notify/notifyAll under this monitor's
     * native lock BEFORE signalling. A waiter compares its entry snapshot
     * against it to distinguish a REAL notify from spurious wakes,
     * GC-safepoint wakes and interrupt signals — and to recover a notify
     * that landed while the waiter was transiently outside the condvar
     * (the GC-park lost-wakeup window). */
    jint wait_seq;
#else
    /* Windows synchronization primitives */
    CRITICAL_SECTION cs;            /* Critical section for mutex */
    CONDITION_VARIABLE entry_cond;  /* Condition variable for monitor entry/exit */
    CONDITION_VARIABLE wait_cond;   /* Condition variable for wait/notify */
    DWORD owner_thread_id;         /* Owner thread ID (GetCurrentThreadId()) */
    volatile int wait_count;        /* Number of threads waiting on wait_cond */
    volatile int entry_wait_count;  /* Number of threads waiting on entry_cond */
    JavaThread* waiting_thread;     /* Thread currently waiting (for interrupt) */
    jint wait_seq;                  /* v34.65 WAIT-HANDSHAKE (POSIX twin above) */
#endif
    jint entry_count;
    volatile jint spinlock;         /* Fast spinlock for cooperative threads */
    int is_pthread;                 /* True if owned by pthread/native thread */
    CoopThread* wait_list;          /* Wait list for cooperative threads */
} CoopMonitor;

/* ==== v20 FIX (P0-5): per-object monitors ====
 * The old table was 256 fixed CoopMonitor slots keyed by
 * ((uintptr_t)obj >> 4) & 0xFF - ANY two objects whose low address bits
 * collided shared ONE monitor: synchronized blocks falsely serialized and
 * wait()/notify() woke waiters of a DIFFERENT object. The table is now an
 * open-addressing hash keyed by the EXACT object pointer; CoopMonitor
 * structs are heap-allocated and never move after creation (so a held
 * mon->mutex stays valid while the entry array grows). Entries live until
 * shutdown; games synchronize on a bounded set of objects, and a dead
 * object's stale entry is harmless because nobody re-locks through it. */
typedef struct {
    JavaObject* owner;
    CoopMonitor* mon;
} MonitorEntry;

static MonitorEntry* g_mon_table = NULL;
static size_t g_mon_capacity = 0;   /* power of two */
static size_t g_mon_count = 0;
static volatile jint g_monitors_initialized = 0;

/* v34.65 PERF: polling chunk (ms) for Object.wait()'s interrupt safety-net.
 * Latency for the events that matter is NOT polling-based anymore:
 *   notify    -> seq handshake + always-signal (mutex-ordered),
 *   interrupt -> thread_interrupt() signals this monitor's wait_cond,
 *   GC        -> jvm_gc_safepoint_wake_monitors() broadcasts wait_cond.
 * The chunk only bounds the worst case if an event window is missed, so it
 * was raised 50ms -> 250ms: idle waiter wakeups drop 20/s -> 4/s (fewer
 * syscall pairs, less mon->mutex churn against the monitor owner).
 * NOJME_WAIT_CHUNK_MS=50 restores the v34.61 cadence for A/B testing. */
static jlong g_wait_chunk_ms = 250;

/* v34.26 PERF (armv7): acquire/release accessors for the monitor table's
 * lock-free read path. On GCC/Clang use atomic builtins (ARM is weakly
 * ordered — the mon pointer must be observed only after the owner slot that
 * publishes it); on MSVC/x86 plain accesses suffice (TSO). */
#if !defined(_MSC_VER)
#define MON_LOAD_ACQ(p)        __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define MON_STORE_REL(p, v)    __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#else
/* MSVC targets x86/x64 only (TSO): aligned pointer/size loads and stores are
 * already acquire/release at the hardware level; plain accesses suffice. */
#define MON_LOAD_ACQ(p)        (*(p))
#define MON_STORE_REL(p, v)    (*(p) = (v))
#endif

/* Small guard for table access; on Windows it also serializes the one-time
 * CRITICAL_SECTION init inside MON_TABLE_LOCK(). */
static volatile jint g_mon_table_lock_flag = 0;
#ifndef _WIN32
static pthread_mutex_t g_mon_table_mutex = PTHREAD_MUTEX_INITIALIZER;
#define MON_TABLE_LOCK()   pthread_mutex_lock(&g_mon_table_mutex)
#define MON_TABLE_UNLOCK() pthread_mutex_unlock(&g_mon_table_mutex)
#else
static CRITICAL_SECTION g_mon_table_cs;
static int g_mon_table_cs_ready = 0;
static void mon_table_lock_win(void) {
    while (atomic_cas_int(&g_mon_table_lock_flag, 0, 1) == false) {
        YieldProcessor();
    }
    if (!g_mon_table_cs_ready) {
        InitializeCriticalSection(&g_mon_table_cs);
        g_mon_table_cs_ready = 1;
    }
}
static void mon_table_unlock_win(void) {
    memory_barrier_release();
    g_mon_table_lock_flag = 0;
}
#define MON_TABLE_LOCK()   mon_table_lock_win()
#define MON_TABLE_UNLOCK() mon_table_unlock_win()
#endif

/* v34.99: monotonic ms helper for the bounded monitor wait + the
 * mon_enter_ms timestamps read by the per-second td dump. */
static long long mon_wait_mono_ms(void) {
#ifdef _WIN32
    return (long long)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
#endif
}

static CoopMonitor* monitor_create(void) {
    CoopMonitor* mon = (CoopMonitor*)calloc(1, sizeof(CoopMonitor));
    if (!mon) return NULL;
#ifndef _WIN32
    pthread_mutex_init(&mon->mutex, NULL);
    pthread_cond_init(&mon->entry_cond, NULL);
    pthread_cond_init(&mon->wait_cond, NULL);
#else
    InitializeCriticalSection(&mon->cs);
    InitializeConditionVariable(&mon->entry_cond);
    InitializeConditionVariable(&mon->wait_cond);
#endif
    return mon;
}

static void mon_table_insert_raw(MonitorEntry* tab, size_t cap,
                                 JavaObject* owner, CoopMonitor* mon) {
    size_t mask = cap - 1;
    size_t idx = ((size_t)owner >> 4) & mask;
    while (tab[idx].owner != NULL) {
        idx = (idx + 1) & mask;
    }
    tab[idx].owner = owner;
    tab[idx].mon = mon;
}

static void mon_table_grow(void) {
    size_t new_cap = g_mon_capacity ? g_mon_capacity * 2 : 1024;
    MonitorEntry* ntab = (MonitorEntry*)calloc(new_cap, sizeof(MonitorEntry));
    if (!ntab) return; /* keep old table; insert path re-checks space */
    for (size_t i = 0; i < g_mon_capacity; i++) {
        if (g_mon_table[i].owner) {
            mon_table_insert_raw(ntab, new_cap, g_mon_table[i].owner, g_mon_table[i].mon);
        }
    }
    /* v34.26 PERF: the OLD table is intentionally NOT freed. Lock-free
     * readers (get_monitor fast path) may still be walking it; freeing under
     * the table lock does not stop them. The leak is bounded (doubling
     * sizes: 16KB + 32KB + ...) and only grows with distinct synchronized
     * objects — a few dozen KB for a typical game. */
    MON_STORE_REL(&g_mon_table, ntab);
    MON_STORE_REL(&g_mon_capacity, new_cap);
}

static void init_monitors(void) {
    if (atomic_cas_int(&g_monitors_initialized, 0, 1)) {
        mon_table_grow();  /* allocates the initial 1024-entry table */
        {
            const char* e = getenv("NOJME_WAIT_CHUNK_MS");
            if (e && e[0] >= '0' && e[0] <= '9') {
                long v = atol(e);
                if (v >= 1 && v <= 10000) g_wait_chunk_ms = (jlong)v;
            }
        }
    }
    memory_barrier_acquire();
}

/* Interrupt a thread */
void thread_interrupt(JavaThread* thread) {
    if (!thread) return;
    
    /* Set interrupted flag with memory barrier for cross-thread visibility */
    thread->interrupted = true;
    memory_barrier();  /* Ensure interrupted flag is visible to other threads */
    
    /* Find the CoopThread and wake it if it's waiting */
    for (int i = 0; i < scheduler.thread_count; i++) {
        if (scheduler.threads[i]->java_thread == thread) {
            CoopThread* coop = scheduler.threads[i];
            
            /* If thread is waiting or timed waiting, add to run queue */
            if (coop->state == THREAD_STATE_WAITING || 
                coop->state == THREAD_STATE_TIMED_WAITING) {
                coop->state = THREAD_STATE_RUNNABLE;
                add_to_run_queue(coop);
            }
            break;
        }
    }
    
    /* For native threads: find monitor where this thread is waiting and signal it */
    init_monitors();
    CoopMonitor* to_signal = NULL;
    MON_TABLE_LOCK();
    for (size_t i = 0; i < g_mon_capacity; i++) {
        if (g_mon_table[i].owner && g_mon_table[i].mon &&
            g_mon_table[i].mon->waiting_thread == thread) {
            to_signal = g_mon_table[i].mon;
            break;
        }
    }
    MON_TABLE_UNLOCK();
    if (to_signal) {
#ifndef _WIN32
        /* v34.65: re-check waiting_thread under the monitor's own lock —
         * the table walk above reads it WITHOUT mon->mutex, so the thread
         * may have stopped waiting by now; the re-check also pairs the
         * flag store (full barrier above) with the mutex release/acquire
         * so the waiter reliably observes the flag. */
        pthread_mutex_lock(&to_signal->mutex);
        if (to_signal->waiting_thread == thread) {
            pthread_cond_signal(&to_signal->wait_cond);
        }
        pthread_mutex_unlock(&to_signal->mutex);
#else
        EnterCriticalSection(&to_signal->cs);
        if (to_signal->waiting_thread == thread) {
            WakeConditionVariable(&to_signal->wait_cond);
        }
        LeaveCriticalSection(&to_signal->cs);
#endif
    }
}

static void monitor_spinlock_acquire(CoopMonitor* mon) {
    while (atomic_cas_int(&mon->spinlock, 0, 1) == false) {
#ifdef _WIN32
        YieldProcessor();
#elif defined(__i386__) || defined(__x86_64__)
        __builtin_ia32_pause();
#elif defined(__arm__) || defined(__aarch64__)
        /* v34.24: ARM yield hint — the x86 pause builtin does not exist on ARM
         * (found by the armv7 NEON cross-build). */
        __asm__ volatile ("yield" ::: "memory");
#endif
    }
    memory_barrier_acquire();
}

static void monitor_spinlock_release(CoopMonitor* mon) {
    memory_barrier_release();
    mon->spinlock = 0;
}

static JavaObject* mondiag_watched_obj = NULL;
static int mondiag_obj_is_watched(JavaObject* obj) {
    return mondiag_watched_obj != NULL && obj == mondiag_watched_obj;
}
static void mondiag_set_watched(JavaObject* obj) {
    if (!mondiag_watched_obj && obj) mondiag_watched_obj = obj;
}

static CoopMonitor* get_monitor(JavaObject* obj) {
    init_monitors();
    /* v34.26 PERF (armv7): lock-free lookup. monitor_enter/exit run on every
     * synchronized block and synchronized method; taking the GLOBAL table
     * mutex twice per block (enter + exit) serialized all VM threads through
     * one cacheline even when uncontended. Entries are never removed, so a
     * reader can probe the open-addressing table without the lock:
     *   - hit:  owner published only AFTER mon (release-stores), so the mon
     *           pointer is fully initialized when observed (acquire-loads).
     *   - miss (owner==NULL or a half-published entry): fall through to the
     *     locked slow path, which re-probes and inserts exactly as before.
     * A concurrent GROW may swap the table mid-probe: the reader either
     * matches by exact owner pointer (impossible to match wrongly) or misses
     * and takes the slow path. Old tables are never freed (see grow). */
    {
        size_t cap = MON_LOAD_ACQ(&g_mon_capacity);
        if (cap != 0) {
            MonitorEntry* tab = MON_LOAD_ACQ(&g_mon_table);
            size_t mask = cap - 1;
            size_t idx = ((size_t)obj >> 4) & mask;
            for (size_t probe = 0; probe < cap; probe++) {
                MonitorEntry* e = &tab[idx];
                if (e->owner == obj) {
                    CoopMonitor* mon = e->mon;
                    if (mon) return mon;
                    break;  /* entry being published right now: use the lock */
                }
                if (e->owner == NULL) break;  /* not present: slow path */
                idx = (idx + 1) & mask;
            }
        }
    }
    MON_TABLE_LOCK();
    size_t mask = g_mon_capacity - 1;
    size_t idx = ((size_t)obj >> 4) & mask;
    for (size_t probe = 0; probe < g_mon_capacity; probe++) {
        MonitorEntry* e = &g_mon_table[idx];
        if (e->owner == obj) {
            MON_TABLE_UNLOCK();
            return e->mon;
        }
        if (e->owner == NULL) {
            /* Grow at 75% load so a free slot is always found. */
            if ((g_mon_count + 1) * 4 >= g_mon_capacity * 3) {
                mon_table_grow();
                mask = g_mon_capacity - 1;
                idx = ((size_t)obj >> 4) & mask;
                while (g_mon_table[idx].owner != NULL) {
                    idx = (idx + 1) & mask;
                }
            }
            CoopMonitor* mon = monitor_create();
            if (!mon) {
                MON_TABLE_UNLOCK();
                return NULL;
            }
            /* v34.26: publish mon BEFORE owner so lock-free readers never
             * observe a set owner with an uninitialized mon. */
            g_mon_table[idx].mon = mon;
            MON_STORE_REL(&g_mon_table[idx].owner, obj);
            g_mon_count++;
            MON_TABLE_UNLOCK();
            return mon;
        }
        idx = (idx + 1) & mask;
    }

    MON_TABLE_UNLOCK();
    return NULL; /* unreachable: growth keeps load under 75% */
}

/* v36.57 [PAINT-BUDGET]: ограниченное ожидание монитора для PUMP-потока.
 * Корень фриза Gravity Defied (полевой лог v36.54-goffix1): игровой поток
 * (Thread-2) заклинивает в чисто-Java цикле депенетрации физики ВНУТРИ
 * synchronized-метода b.do() и держит монитор физики ~10 с; фронтенд-поток,
 * исполняющий paint() в стадии repaints, блокируется в monitor_enter на всё
 * это время — STUCK stage=repaints, вход/звук/картинка мертвы, CPU 0%.
 * Решение в два слоя: (1) [SPIN-BREAKER] (execute.c) прерывает сам спин
 * исключением; (2) ЗДЕСЬ — пока спин ещё не сломан, monitor_enter потока-
 * пампера не ждёт вечно: по истечении бюджета (NOJME_PAINT_LOCK_MS, по
 * умолчанию 400 мс) он выходит с JNI_ERR и tls_paint_lock_aborted=1,
 * вызывающая сторона (op_monitorenter / ACC_SYNC вход execute_method)
 * бросает InterruptedException-подобный RuntimeException, paint()
 * распаковывается (v34.99 MONITOR-UNWIND освобождает oc/H/l), насос
 * возвращается, главный цикл ЖИВЁТ (ввод/пауза/выход/презентация последнего
 * кадра), следующий насос повторяет попытку. Бюджет активен ТОЛЬКО между
 * arm()/disarm() — их ставит midp_process_repaints_impl вокруг вызова
 * paint() и ТОЛЬКО на потоке фронтенда (не на VM-раннерах: их блокировки —
 * игровая логика, см. serviceRepaints). */
static __thread int64_t tls_paint_lock_deadline_ms = 0;
static __thread int    tls_paint_lock_aborted = 0;

/* v36.57 [MON-SITE]: сайт входа в monitor_enter (метод/класс кадра + тип
 * входа: блок или ACC_SYNC-метод) — пишут op_monitorenter/execute_method
 * ДО monitor_enter, читает mon_wait_report в [MON-WAIT]: полевой лог сразу
 * называет ГДЕ ждал жертва, а не только чей монитор она ждала. */
static __thread char tls_mon_site[96] = "";
void jvm_mon_site_note(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tls_mon_site, sizeof(tls_mon_site), fmt, ap);
    va_end(ap);
}

void jvm_paint_lock_arm(uint32_t budget_ms) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t now = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    tls_paint_lock_deadline_ms = now + (int64_t)budget_ms;
    tls_paint_lock_aborted = 0;
}
void jvm_paint_lock_disarm(void) {
    tls_paint_lock_deadline_ms = 0;
}
int jvm_paint_lock_aborted(void) {
    return tls_paint_lock_aborted;
}
void jvm_paint_lock_reset(void) {
    tls_paint_lock_aborted = 0;
    tls_paint_lock_deadline_ms = 0;
}

/* v36.57 [KEY-TRYLOCK]: неблокирующий вход в монитор — для доставки
 * отложенных клавиш на игровом потоке (см. midp_process_pending_keys).
 * Возвращает JNI_OK при захвате, JNI_ERR если монитор занят другим
 * потоком. РЕКУРСИВНЫЙ для владельца (как monitor_enter). */
int monitor_tryenter(JVM* jvm, JavaObject* obj) {
    (void)jvm;
    if (!obj) return JNI_ERR;
    CoopMonitor* mon = get_monitor(obj);
    if (!mon) return JNI_ERR;

#ifndef _WIN32
    pthread_t self = pthread_self();
    pthread_mutex_lock(&mon->mutex);
    /* reentrant fast path */
    if (mon->is_pthread && pthread_equal(mon->owner_pthread, self)) {
        mon->entry_count++;
        pthread_mutex_unlock(&mon->mutex);
        return JNI_OK;
    }
    if (mon->entry_count > 0) {
        pthread_mutex_unlock(&mon->mutex);
        return JNI_ERR;  /* занят другим потоком — НЕ ждём */
    }
    mon->owner = obj;
    mon->owner_pthread = self;
    {
        JavaThread* self_jt = thread_current(jvm);
        mon->owner_thread = self_jt;
    }
    mon->is_pthread = 1;
    mon->entry_count = 1;
    pthread_mutex_unlock(&mon->mutex);
    return JNI_OK;
#else
    DWORD self_id = GetCurrentThreadId();
    EnterCriticalSection(&mon->cs);
    if (mon->is_pthread && mon->owner_thread_id == self_id) {
        mon->entry_count++;
        LeaveCriticalSection(&mon->cs);
        return JNI_OK;
    }
    if (mon->entry_count > 0) {
        LeaveCriticalSection(&mon->cs);
        return JNI_ERR;
    }
    mon->owner = obj;
    mon->owner_thread = thread_current(jvm);
    mon->owner_thread_id = self_id;
    mon->is_pthread = 1;
    mon->entry_count = 1;
    LeaveCriticalSection(&mon->cs);
    return JNI_OK;
#endif
}

/* v36.57 [MON-WAIT-CLASS]: общий хелпер журналирования долгих ожиданий
 * монитора (POSIX- и Windows-пути monitor_enter печатали дублирующийся
 * блок). Добавлено: имя КЛАССА блокируемого объекта и дублирование в
 * log.txt через sw_trace_force — полевой лог теперь сам называет жертву.
 * Мотивация: разбор фриза Gravity Defied (полевой лог v36.54-goffix1,
 * STUCK stage=repaints ~10 с, td m monenter=9s) показал, что [MON-WAIT]
 * уходил ТОЛЬКО в stderr, которого у полевого пользователя нет — класс
 * фриза был невидим в log.txt. */
static void mon_wait_report(void* obj, long long mon_waited,
                            CoopMonitor* mon, JavaThread* self_jt,
                            const char* kind) {
    extern JavaClass* object_get_class(void* object);
    extern void sw_trace_force(const char* fmt, ...) __attribute__((weak));
    JavaThread* owner_jt = mon ? mon->owner_thread : NULL;
    const char* mon_cls = "?";
    {
        JavaClass* oc = obj ? object_get_class(obj) : NULL;
        if (oc && oc->class_name) mon_cls = oc->class_name;
    }
    if (strcmp(kind, "WAIT") == 0) {
        LOG_SAFE("[MON-WAIT] obj=%p class=%s site=%s waited=%lld ms entry_count=%d is_pthread=%d owner_tid=%d owner_nat=%.40s waiter_tid=%d\n",
                 obj, mon_cls,
                 tls_mon_site[0] ? tls_mon_site : "(?)",
                 mon_waited, mon ? mon->entry_count : -1,
                 mon ? mon->is_pthread : -1,
                 owner_jt ? owner_jt->id : -1,
                 (owner_jt && owner_jt->last_native[0]) ? owner_jt->last_native : "(none)",
                 self_jt ? self_jt->id : -1);
        if (&sw_trace_force && sw_trace_force) {
            sw_trace_force("[MON-WAIT] class=%s site=%s waited=%lld ms owner_tid=%d owner_nat=%.40s waiter_tid=%d",
                           mon_cls,
                           tls_mon_site[0] ? tls_mon_site : "(?)",
                           mon_waited,
                           owner_jt ? owner_jt->id : -1,
                           (owner_jt && owner_jt->last_native[0]) ? owner_jt->last_native : "(none)",
                           self_jt ? self_jt->id : -1);
        }
    } else {
        LOG_SAFE("[MON-STEAL] obj=%p class=%s waited=%lld ms — forcing release of monitor owned by tid=%d (owner leaked it via exception unwind or hung inside the block); freeze converted to a glitch\n",
                 obj, mon_cls, mon_waited,
                 owner_jt ? owner_jt->id : -1);
        if (&sw_trace_force && sw_trace_force) {
            sw_trace_force("[MON-STEAL] class=%s waited=%lld ms — forcing release of monitor owned by tid=%d",
                           mon_cls, mon_waited,
                           owner_jt ? owner_jt->id : -1);
        }
    }
}

int monitor_enter(JVM* jvm, JavaObject* obj) {
    (void)jvm;
    if (!obj) return JNI_ERR;
    
    CoopMonitor* mon = get_monitor(obj);
    
#ifndef _WIN32
    pthread_t self = pthread_self();
    
    /* Always use pthread mutex for synchronization */
    pthread_mutex_lock(&mon->mutex);
    
    /* Check if we already own it (reentrant) */
    if (mon->is_pthread && pthread_equal(mon->owner_pthread, self)) {
        mon->entry_count++;
        if (mondiag_obj_is_watched(obj)) {
            JavaThread* cur = thread_current(jvm);
            LOG_SAFE("[MONDIAG] ENTER-RE(obj=%p) tid=%d count=%d\n", (void*)obj,
                     cur ? cur->id : -1, mon->entry_count);
        }
        pthread_mutex_unlock(&mon->mutex);
        return JNI_OK;
    }
    
    /* Wait until monitor is free - use entry_cond for monitor entry contention */
    /* v34.95: mark the blocker for the per-second trace-log thread dump —
     * "monenter" on a thread whose monitor owner is itself stuck is the
     * exact freeze signature this diagnostic hunts for. */
    JavaThread* self_jt = thread_current(jvm);
    if (self_jt) { self_jt->mon_enter_wait = 1; self_jt->mon_enter_ms = mon_wait_mono_ms(); }
    mon->entry_wait_count++;
    /* v34.99 BOUNDED MONITOR WAIT: this was the LAST unbounded wait in the
     * JVM. The v34.98 field freeze parked the frontend thread here forever
     * (monitor leaked by an exception unwind on a game thread) while the
     * td m line could not even SHOW it. Policy mirrors the v34.96 M3G
     * bounded lock: after 2 s log [MON-WAIT] (throttled 5 s) naming the
     * owner; after 20 s [MON-STEAL] force-releases the monitor so the
     * freeze converts into a recoverable glitch. The timed wait (50 ms
     * slice) also re-arms the GC-safepoint branch promptly. */
    long long mon_t0 = mon_wait_mono_ms();
    long long mon_last_log = 0;
    int mon_steals = 0;
    while (mon->entry_count > 0) {
        if (g_gc_safepoint_request) {
            pthread_mutex_unlock(&mon->mutex);
            jvm_gc_safepoint_park();
            pthread_mutex_lock(&mon->mutex);
        } else {
            struct timespec mt_abstime;
            clock_gettime(CLOCK_REALTIME, &mt_abstime);
            mt_abstime.tv_nsec += 50000000; /* 50 ms slice */
            if (mt_abstime.tv_nsec >= 1000000000L) {
                mt_abstime.tv_sec++;
                mt_abstime.tv_nsec -= 1000000000L;
            }
            pthread_cond_timedwait(&mon->entry_cond, &mon->mutex, &mt_abstime);
        }
        if (mon->entry_count > 0) {
            long long mon_waited = mon_wait_mono_ms() - mon_t0;
            long long mon_now = mon_wait_mono_ms();
            /* v36.57 [PAINT-BUDGET]: истёк бюджет pump-потока — уходим без
             * монитора (вызывающая сторона бросит исключение и прервёт
             * paint). Не логируем как MON-WAIT — это штатный отказ. */
            if (tls_paint_lock_deadline_ms && mon_now >= tls_paint_lock_deadline_ms) {
                tls_paint_lock_aborted = 1;
                pthread_mutex_unlock(&mon->mutex);
                mon->entry_wait_count--;
                if (self_jt) { self_jt->mon_enter_wait = 0; self_jt->mon_enter_ms = 0; }
                return JNI_ERR;
            }
            if (mon_waited >= 2000 && mon_now - mon_last_log >= 5000) {
                mon_last_log = mon_now;
                mon_wait_report((void*)obj, mon_waited, mon, self_jt, "WAIT");
            }
            if (mon_waited >= 20000 && mon_steals < 8) {
                mon_steals++;
                mon_wait_report((void*)obj, mon_waited, mon, self_jt, "STEAL");
                mon->entry_count = 0;
                mon->owner = NULL;
                mon->owner_thread = NULL;
                mon->owner_pthread = (pthread_t)0;
                mon->is_pthread = 0;
                pthread_cond_broadcast(&mon->entry_cond);
                /* loop re-checks: entry_count==0 -> fall through and acquire */
            }
        }
    }
    mon->entry_wait_count--;
    if (self_jt) { self_jt->mon_enter_wait = 0; self_jt->mon_enter_ms = 0; }
    
    /* Now we own the monitor */
    mon->owner = obj;
    mon->owner_pthread = self;
    mon->owner_thread = self_jt;   /* v34.99: name the owner in [MON-WAIT] */
    mon->is_pthread = 1;
    mon->entry_count = 1;
    if (mondiag_obj_is_watched(obj)) {
        JavaThread* cur = thread_current(jvm);
        LOG_SAFE("[MONDIAG] ENTER(obj=%p) owner tid=%d\n", (void*)obj,
                 cur ? cur->id : -1);
    }
    
    pthread_mutex_unlock(&mon->mutex);
    return JNI_OK;
#else
    /* Windows: use CRITICAL_SECTION for native threads */
    EnterCriticalSection(&mon->cs);
    
    DWORD self_id = GetCurrentThreadId();
    
    /* Check if we already own it (reentrant) */
    if (mon->is_pthread && mon->owner_thread_id == self_id) {
        mon->entry_count++;
        if (mondiag_obj_is_watched(obj)) {
            JavaThread* cur = thread_current(jvm);
            LOG_SAFE("[MONDIAG] ENTER-RE(obj=%p) tid=%d count=%d\n", (void*)obj,
                     cur ? cur->id : -1, mon->entry_count);
        }
        LeaveCriticalSection(&mon->cs);
        return JNI_OK;
    }
    
    /* Wait until monitor is free - use entry_cond for monitor entry contention */
    /* v34.95: Windows twin of the POSIX monenter dump marker. */
    JavaThread* self_jt = thread_current(jvm);
    if (self_jt) { self_jt->mon_enter_wait = 1; self_jt->mon_enter_ms = mon_wait_mono_ms(); }
    mon->entry_wait_count++;
    /* v34.99 BOUNDED MONITOR WAIT (Windows twin): same 2 s log / 20 s steal
     * policy as the POSIX path above. */
    long long mon_t0 = mon_wait_mono_ms();
    long long mon_last_log = 0;
    int mon_steals = 0;
    while (mon->entry_count > 0) {
        /* v34.10 GC safepoint poll (Windows twin of the POSIX branch):
         * release the monitor CS while parking so gc_collect() can run. */
        if (g_gc_safepoint_request) {
            LeaveCriticalSection(&mon->cs);
            jvm_gc_safepoint_park();
            EnterCriticalSection(&mon->cs);
        } else {
            /* Use entry_cond for monitor entry, NOT wait_cond; 50 ms slice
             * keeps the log/steal checks live (v34.99). */
            SleepConditionVariableCS(&mon->entry_cond, &mon->cs, 50);
        }
        if (mon->entry_count > 0) {
            long long mon_waited = mon_wait_mono_ms() - mon_t0;
            long long mon_now = mon_wait_mono_ms();
            /* v36.57 [PAINT-BUDGET] (Windows twin) */
            if (tls_paint_lock_deadline_ms && mon_now >= tls_paint_lock_deadline_ms) {
                tls_paint_lock_aborted = 1;
                LeaveCriticalSection(&mon->cs);
                mon->entry_wait_count--;
                if (self_jt) { self_jt->mon_enter_wait = 0; self_jt->mon_enter_ms = 0; }
                return JNI_ERR;
            }
            if (mon_waited >= 2000 && mon_now - mon_last_log >= 5000) {
                mon_last_log = mon_now;
                mon_wait_report((void*)obj, mon_waited, mon, self_jt, "WAIT");
            }
            if (mon_waited >= 20000 && mon_steals < 8) {
                mon_steals++;
                mon_wait_report((void*)obj, mon_waited, mon, self_jt, "STEAL");
                mon->entry_count = 0;
                mon->owner = NULL;
                mon->owner_thread = NULL;
                mon->owner_thread_id = 0;
                mon->is_pthread = 0;
                WakeAllConditionVariable(&mon->entry_cond);
            }
        }
    }
    mon->entry_wait_count--;
    if (self_jt) { self_jt->mon_enter_wait = 0; self_jt->mon_enter_ms = 0; }
    
    /* Now we own the monitor */
    mon->owner = obj;
    mon->owner_thread = self_jt;   /* v34.99: name the owner in [MON-WAIT] */
    mon->owner_thread_id = self_id;
    mon->is_pthread = 1;
    mon->entry_count = 1;
    if (mondiag_obj_is_watched(obj)) {
        JavaThread* cur = thread_current(jvm);
        LOG_SAFE("[MONDIAG] ENTER(obj=%p) owner tid=%d\n", (void*)obj,
                 cur ? cur->id : -1);
    }
    
    LeaveCriticalSection(&mon->cs);
    return JNI_OK;
#endif
}

int monitor_exit(JVM* jvm, JavaObject* obj) {
    (void)jvm;
    if (!obj) return JNI_ERR;
    
    CoopMonitor* mon = get_monitor(obj);
    
#ifndef _WIN32
    pthread_t self = pthread_self();
    
    pthread_mutex_lock(&mon->mutex);
    
    /* Verify we own the monitor */
    if (!mon->is_pthread || !pthread_equal(mon->owner_pthread, self)) {
        pthread_mutex_unlock(&mon->mutex);
        return JNI_ERR;
    }
    
    mon->entry_count--;
    if (mon->entry_count == 0) {
        /* Release the monitor */
        mon->owner = NULL;
        mon->owner_thread = NULL;   /* v34.99: keep [MON-WAIT] honest */
        mon->owner_pthread = (pthread_t)0;
        mon->is_pthread = 0;
        if (mondiag_obj_is_watched(obj)) {
            JavaThread* cur = thread_current(jvm);
            LOG_SAFE("[MONDIAG] EXIT(obj=%p) tid=%d\n", (void*)obj,
                     cur ? cur->id : -1);
        }
        
        /* Wake up one thread waiting to enter the monitor - use entry_cond */
        if (mon->entry_wait_count > 0) {
            pthread_cond_signal(&mon->entry_cond);
        }
    }
    
    pthread_mutex_unlock(&mon->mutex);
    return JNI_OK;
#else
    /* Windows: use CRITICAL_SECTION for native threads */
    DWORD self_id = GetCurrentThreadId();
    
    EnterCriticalSection(&mon->cs);
    
    /* Verify we own the monitor */
    if (!mon->is_pthread || mon->owner_thread_id != self_id) {
        LeaveCriticalSection(&mon->cs);
        return JNI_ERR;
    }
    
    mon->entry_count--;
    if (mon->entry_count == 0) {
        /* Release the monitor */
        mon->owner = NULL;
        mon->owner_thread = NULL;   /* v34.99: keep [MON-WAIT] honest */
        mon->owner_thread_id = 0;
        mon->is_pthread = 0;
        if (mondiag_obj_is_watched(obj)) {
            JavaThread* cur = thread_current(jvm);
            LOG_SAFE("[MONDIAG] EXIT(obj=%p) tid=%d\n", (void*)obj,
                     cur ? cur->id : -1);
        }
        
        /* Wake up one thread waiting to enter the monitor - use entry_cond */
        if (mon->entry_wait_count > 0) {
            WakeConditionVariable(&mon->entry_cond);
        }
    }
    
    LeaveCriticalSection(&mon->cs);
    return JNI_OK;
#endif
}

/* v34.9: live progress counters for the watchdog (which loop is a thread
 * parked in inside monitor_wait). */
volatile int mondiag_dbg_state[16] = {0};   /* per tid: 0=idle 1=timedloop 2=reacquire 3=done */
volatile int mondiag_dbg_iter[16] = {0};

/* ===================== v34.99 MONITOR-UNWIND =====================
 * The root fix for the standalone-only "freeze right after the countdown"
 * family (v34.95..v34.98 field logs, identical signature every time):
 *   td m: nat=...Graphics3D.bindTarget age=<grows> | g.b@18 | g.a@12 | g.k@14
 *   td t2: run (game thread keeps spinning, CPU ~100%)
 *   td m3g: ui/pipe both locked=0  (m NOT inside an M3G native)
 * m was blocked in monitor_enter on a monitor LEAKED by an exception that
 * unwound out of a synchronized BLOCK on a game thread: the interpreter
 * never tracked block-level monitor ownership, so entry_count stayed >0
 * forever and the frontend/paint thread waited on the entry condvar
 * forever. libretro was immune because its single-threaded retro_run
 * makes every monitor re-entry reentrant (same pthread).
 *
 * jvm_frame_release_monitors: drain a frame's mon_owned[] list (reverse
 * acquisition order) — called from execute_method's frame-exit path.
 * on_exception=1 releases due to unwind (the leak fix itself, logged);
 * on_exception=0 covers non-standard bytecode that returns while still
 * inside a block (defensive, logged separately). */
void jvm_frame_release_monitors(JVM* jvm, JavaThread* jt, JavaFrame* fr, int on_exception) {
    (void)jt;  /* reserved: owner tid in future [MON-*] enrichments */
    if (!fr || fr->mon_owned_n <= 0) return;
    int released = 0;
    const char* cls = fr->clazz && fr->clazz->class_name ? fr->clazz->class_name : "?";
    const char* mth = fr->method && fr->method->name ? fr->method->name : "?";
    for (int i = fr->mon_owned_n - 1; i >= 0; i--) {
        JavaObject* obj = fr->mon_owned[i];
        fr->mon_owned[i] = NULL;
        if (obj) {
            monitor_exit(jvm, obj);   /* rc ignored: best-effort unwind */
            released++;
        }
    }
    fr->mon_owned_n = 0;
    if (released > 0) {
        static int mon_unwind_logs = 0;
        if (mon_unwind_logs < 20) {
            mon_unwind_logs++;
            LOG_SAFE("[MON-%s] %s.%s: released %d monitor(s) %s (v34.99 leak fix)\n",
                     on_exception ? "UNWIND" : "LEFTOVER",
                     cls, mth, released,
                     on_exception ? "during exception unwind" : "on frame exit");
        }
    }
}

/* ================= v34.9 GC SAFEPPOINT =================
 * The pthread build runs VM threads as native pthreads. gc_collect()
 * walks/moves the heap WHILE other pthread threads allocate and mutate
 * Java objects concurrently -> torn graphs, runaway mark loops, the
 * loader thread stuck forever inside an exception allocation
 * (Bounce Tales: white loading screen after "New game").
 *
 * A GC now requests a safepoint; VM threads poll the request in their
 * interpreter loop, in monitor_wait / monitor_enter / heap_lock wait
 * loops, and park until the GC releases them. Park arrival is
 * best-effort (bounded wait) so a thread stuck in a long native call
 * cannot hang the GC forever. */
extern volatile int g_gc_safepoint_request; /* defined below */
volatile int g_gc_safepoint_request = 0;
static pthread_mutex_t g_sp_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_sp_cond = PTHREAD_COND_INITIALIZER;
static int g_sp_arrived = 0;

void jvm_gc_safepoint_park(void) {
    /* v34.59 TLAB: закрыть СВОЙ аллокационный чанк ДО arrival-count.
     * mark/sweep коллектора должны видеть только закрытые чанки
     * (неиспользованный хвост остаётся валидным свободным блоком —
     * инвариант №3 в heap.c). TLS-операция без локов; идемпотентно.
     * Проверка request — чтобы ложный вызов park (запроса нет) не
     * выбрасывал живой чанк. */
    if (g_gc_safepoint_request) {
        extern void heap_tlab_flush_self(void);
        heap_tlab_flush_self();
    }
    /* v34.98: publish the park reason so the per-second td dump can tell
     * "state=run but frozen at a safepoint" from a genuine spin. */
    {
        extern JavaThread* thread_current(JVM* jvm);
        JavaThread* self = thread_current(NULL);
        if (self) snprintf(self->park_why, sizeof(self->park_why), "gc-sp");
    }
    pthread_mutex_lock(&g_sp_mutex);
    if (g_gc_safepoint_request) {
        g_sp_arrived++;
        pthread_cond_broadcast(&g_sp_cond);
        while (g_gc_safepoint_request) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 50000000; /* 50ms */
            if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
            pthread_cond_timedwait(&g_sp_cond, &g_sp_mutex, &ts);
        }
        g_sp_arrived--;
    }
    pthread_mutex_unlock(&g_sp_mutex);
    {
        extern JavaThread* thread_current(JVM* jvm);
        JavaThread* self = thread_current(NULL);
        if (self) self->park_why[0] = '\0';
    }
}

int jvm_gc_safepoint_arrived(void) {
    return g_sp_arrived;
}

/* v34.26 PERF (armv7 stutter — the big one): event-driven safepoint wait.
 * gc_collect() used to poll `nanosleep(5ms); re-check;` for mutator arrival:
 * even when every VM thread parked within microseconds, the collector STILL
 * slept a full 5ms slice before noticing — at 60fps (16.6ms/frame) that is
 * ~30% of the frame budget burnt on sleep granularity for EVERY collection,
 * which surfaces exactly as the periodic stutter / dropped frames reported
 * on armv7. jvm_gc_safepoint_park() broadcasts g_sp_cond on each arrival, so
 * waiting ON the condition wakes the collector the instant the LAST thread
 * parks. The 2ms timed slices only guard against a lost-wakeup edge case.
 * Returns the arrival count observed when the wait ended. */
int jvm_gc_safepoint_wait_arrivals(int expected, int timeout_ms) {
    if (expected <= 0 || timeout_ms <= 0) {
        return g_sp_arrived;
    }
    pthread_mutex_lock(&g_sp_mutex);
    int waited_ms = 0;
    while (g_sp_arrived < expected && waited_ms < timeout_ms) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 2 * 1000000;  /* 2ms slice */
        if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
        pthread_cond_timedwait(&g_sp_cond, &g_sp_mutex, &ts);
        waited_ms += 2;
    }
    int arrived = g_sp_arrived;
    pthread_mutex_unlock(&g_sp_mutex);
    return arrived;
}

void jvm_gc_safepoint_release(void) {
    pthread_mutex_lock(&g_sp_mutex);
    g_gc_safepoint_request = 0;
    pthread_cond_broadcast(&g_sp_cond);
    pthread_mutex_unlock(&g_sp_mutex);
}

/* ==========================================================================
 * v34.81: FRONTEND PAUSE — «пока я в настройках, игра продолжает работать»
 * ==========================================================================
 * Пользовательский отчёт: при открытии меню RetroArch игра продолжала
 * работать «возможно не основным потоком». Forensics: когда фронтенд
 * ставит ядро на паузу (меню/пауза RetroArch), он просто ПЕРЕСТАЁТ
 * вызывать retro_run() — и всё, что качается из retro_run (бюджет
 * главного потока, jvm_process_timers, repaint-насос, callSerially,
 * синхронный аудио-микс libretro_process_audio), замирает корректно.
 * Но Java-потоки — НАСТОЯЩИЕ pthread (Thread.start -> native.c), они
 * продолжают исполнять байткод: игровая логика живёт, System.currentTime-
 * Millis идёт, Object.wait-таймауты сгорают. После закрытия меню игра
 * «перепрыгивала» вперёд на всю длительность меню.
 *
 * РЕШЕНИЕ — watchdog по каденции retro_run + виртуализация времени:
 *   - retro_run() на входе/выходе обновляет g_frontend_last_frame_ms /
 *     g_frontend_run_active (libretro.c);
 *   - eval-поток (запускается из libretro-ядра, 100 мс) и park-циклы
 *     вычисляют stall: нет завершённого retro_run дольше PAUSE_STALL_MS
 *     (по умолчанию 300 мс; NOJME_PAUSE_STALL_MS, 0 = выключить) — пауза;
 *   - интерпретатор опрашивает дешёвый латент g_frontend_pause_active на
 *     том же 64-инструкционном slow-check, что и GC-safepoint (одна
 *     volatile-читка на 64 инструкции);
 *   - jvm_frontend_pause_park(): поток спит чанками по 15 мс до снятия
 *     паузы, ПРИ ЭТОМ уважает GC-safepoint-запрос (in-flight коллектор
 *     не зависает);
 *   - ВРЕМЯ ВИРТУАЛИЗИРУЕТСЯ: System.currentTimeMillis и таймеры
 *     вычитают накопленную паузу (jvm_pause_offset_ms), Thread.sleep и
 *     Object.wait считают остаток по виртуальным монотонным часам
 *     (jvm_virt_mono_ms) — после снятия паузы игра продолжает ровно с
 *     того мгновения, когда меню открылось. Когда пауза ни разу не
 *     срабатывала, offset == 0 и поведение бит-в-бит прежнее.
 *
 * Headless/SDL-сборки не вызывают jvm_frontend_pause_enable() — фича
 * выключена по умолчанию, их тайминги не затронуты.
 * ========================================================================== */

/* Состояние паузы (см. блок-комментарий выше). volatile: пишут retro_run
 * (frontend-поток) и eval-поток, читают VM-потоки без локов. */
volatile uint64_t g_frontend_last_frame_ms = 0; /* последний вход retro_run */
volatile int      g_frontend_run_active = 0;    /* retro_run выполняется */
volatile int      g_frontend_pause_active = 0;  /* латет для дешёвых опросов */
static volatile int g_frontend_pause_enabled = 0;
static volatile int g_pause_state = 0;          /* 0=run 1=paused (CAS) */
static uint64_t g_pause_started_ms = 0;         /* старт текущей паузы */
static uint64_t g_pause_total_ms = 0;           /* накоплено за все паузы */
static uint32_t g_pause_stall_ms = 300;         /* порог (env NOJME_PAUSE_STALL_MS) */
/* v34.97 FORCE-LIFT страховка: эпизод паузы не может длиться дольше
 * g_pause_force_lift_ms — если никто не снял паузу (единственные
 * «сниматели» живут на потоке фронтенда — именно он завис в trace v34.96:
 * STUCK stage=repaints, VM запаркован внутри paint()), аварийно снимаем и
 * на g_pause_suppress_ms запрещаем stall-детектору немедленно взводить
 * снова (иначе был бы лифлодк: lift -> eval через 100 мс -> arm -> park). */
static uint32_t g_pause_force_lift_ms = 20000;  /* 20 с */
static uint32_t g_pause_suppress_ms   = 5000;   /* 5 с без re-arm */
static uint64_t g_pause_suppress_until_ms = 0;
static int g_pause_eval_thread_started = 0;

/* v36.02 EXIT-DAEMON-JOIN: the eval thread used to be created DETACHED and
 * never exited — it outlived the process-exit path. On HOS _exit(0) does
 * NOT kill threads atomically: control returns to HBmenu through the
 * loader unmap, and this thread's next 100 ms wake executed unmapped NRO
 * code (2168-0001 Instruction Abort, "hbloader", two daemon sleep loops in
 * the user crash report: X0/X1=100000000 here, X20=500000000 = the
 * key_hang_watchdog twin). Now the thread is JOINABLE and stops on
 * request; main() joins it before the raw exit. */
static volatile int g_pause_eval_quit = 0;
static pthread_t g_pause_eval_pthread;
static int g_pause_eval_have = 0;

static uint64_t pause_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

/* Полная проверка stall + переходы состояния. Вызывается eval-потоком и
 * park-циклами (не из горячего пути интерпретатора). Переходы защищены
 * CAS: двойного начисления g_pause_total_ms при гонке двух вызывателей
 * не бывает; микросекундный разброс старта (два потока одновременно
 * увидели 0->1) влияет только на косметику виртуальных часов. */
static void jvm_frontend_pause_eval(uint64_t now) {
    if (!g_frontend_pause_enabled) {
        if (g_pause_state == 1) {
            if (__sync_bool_compare_and_swap(&g_pause_state, 1, 0)) {
                g_frontend_pause_active = 0;
            }
        }
        return;
    }
    /* v34.97 FORCE-LIFT: пауза не может длиться вечно. См. блок у статиков. */
    if (g_pause_state == 1 && g_pause_started_ms &&
        now - g_pause_started_ms > (uint64_t)g_pause_force_lift_ms) {
        if (__sync_bool_compare_and_swap(&g_pause_state, 1, 0)) {
            g_pause_total_ms += now - g_pause_started_ms;
            g_frontend_pause_active = 0;
            g_pause_suppress_until_ms = now + (uint64_t)g_pause_suppress_ms;
            LOG_SAFE("[FE-PAUSE-FORCE-LIFT] pause episode lasted %llu ms with no lifter on the frontend thread — lifting, stall re-arm suppressed for %u s\n",
                     (unsigned long long)(now - g_pause_started_ms),
                     (unsigned)(g_pause_suppress_ms / 1000));
        }
    }
    int stalled = (g_frontend_last_frame_ms != 0 &&
                   !g_frontend_run_active &&
                   now - g_frontend_last_frame_ms > (uint64_t)g_pause_stall_ms);
    /* v34.97: после force-lift'а не взводим паузу заново до конца окна
     * подавления — иначе lift превращается в лифлодк lift->arm->park. */
    if (stalled && now < g_pause_suppress_until_ms) stalled = 0;
    if (stalled && g_pause_state == 0) {
        if (__sync_bool_compare_and_swap(&g_pause_state, 0, 1)) {
            g_pause_started_ms = now;   /* пишем ДО взвода латча */
            g_frontend_pause_active = 1;
        }
    } else if (!stalled && g_pause_state == 1) {
        if (__sync_bool_compare_and_swap(&g_pause_state, 1, 0)) {
            g_pause_total_ms += now - g_pause_started_ms;
            g_frontend_pause_active = 0;
        }
    }
}

int jvm_frontend_paused(void) {
    /* быстрый путь для wait/sleep-циклов: без latчa они бы не обновляли
     * переходы, если eval-поток не успел — дешёвая полная проверка здесь */
    jvm_frontend_pause_eval(pause_now_ms());
    return g_pause_state;
}

/* Парковка вызывающего потока до снятия паузы. GC-safepoint уважается:
 * запаркованный поток остаётся видимым коллектору (park по его протоколу),
 * in-flight GC завершается, а не ждёт 2с-таймаута. */
void jvm_frontend_pause_park(void) {
    int printed = 0;
    /* v34.98: publish the park reason for the td dump (see gc-sp). */
    {
        extern JavaThread* thread_current(JVM* jvm);
        JavaThread* self = thread_current(NULL);
        if (self) snprintf(self->park_why, sizeof(self->park_why), "fe-pause");
    }
    while (g_frontend_pause_active || jvm_frontend_paused()) {
        /* v34.84 FIX (slow close): VM shutdown lifts the park — otherwise a
         * thread parked here (menu was open at close time) never exits and
         * jvm_destroy leaks the VM after its 3 s wait. */
        if (scheduler.initialized && scheduler.jvm && !scheduler.jvm->running) {
            break;
        }
        if (g_gc_safepoint_request) {
            jvm_gc_safepoint_park();
            continue;
        }
        if (!printed) {
            printed = 1;
            LOG_SAFE("[FE-PAUSE] tid parking (frontend stall > %u ms)\n",
                     (unsigned)g_pause_stall_ms);
        }
#ifdef _WIN32
        Sleep(15);
#else
        struct timespec req = { .tv_sec = 0, .tv_nsec = 15 * 1000000L };
        nanosleep(&req, NULL);
#endif
    }
    {
        extern JavaThread* thread_current(JVM* jvm);
        JavaThread* self = thread_current(NULL);
        if (self) self->park_why[0] = '\0';
    }
}

/* v34.98: STUCK-forensics snapshot for the trace heartbeat (bit0 = pause
 * state armed, bit1 = pause latch active). Weak-linked by switch_trace.c. */
int jvm_frontend_pause_snapshot(void) {
    return (g_pause_state & 1) | ((g_frontend_pause_active ? 1 : 0) << 1);
}

/* Сколько времени ядро провело в паузе (включая текущий активный отрезок).
 * Вычитается из System.currentTimeMillis и таймеров — «часы игры стоят,
 * пока открыто меню». Никогда не уменьшается в пределах сессии контента. */
uint64_t jvm_pause_offset_ms(void) {
    uint64_t now = pause_now_ms();
    if (g_pause_state == 1 && now > g_pause_started_ms) {
        return g_pause_total_ms + (now - g_pause_started_ms);
    }
    return g_pause_total_ms;
}

/* Виртуальные монотонные часы: real_monotonic - pause_offset. Основа
 * остатков Thread.sleep и Object.wait — сон не сгорает за время меню. */
uint64_t jvm_virt_mono_ms(void) {
    return pause_now_ms() - jvm_pause_offset_ms();
}

/* Вызывается из retro_run (frontend-поток) на ВХОДЕ: свежий таймстамп +
 * признак «выполняется». Активный флаг защищает от ложного stall, когда
 * один retro_run сам длится дольше порога (тяжёлый GC): часы тикают на
 * входе, а параллельные VM-потоки не должны видеть «паузу». */
void jvm_frontend_frame_tick(void) {
    g_frontend_last_frame_ms = pause_now_ms();
    g_frontend_run_active = 1;
}

/* Вызывается из retro_run на КАЖДОМ выходе (exec-window cleanup в
 * libretro.c): снимает активный флаг и переоценивает stall — пауза
 * замечается не позже чем через один poll, а не через 100 мс eval-потока. */
void jvm_frontend_run_exit(void) {
    /* v34.97: часы stall перезапускаются на ВЫХОДЕ из exec-окна, а не на
     * его входе. Раньше одна тяжёлая итерация (GC-шторм + медленный paint —
     * во trace v34.96 fl поднялся 50->84->160 прямо перед фризом) сама по
     * себе делала now - last_frame > порога на выходе: пауза взводилась
     * против ЖИВОГО фронтенда, который собирался начать следующую итерацию,
     * и никто её уже не снимал. Для libretro семантика остаётся «фронтенд
     * перестал вызывать retro_run», просто длительность самого retro_run
     * больше не считается затишьем. */
    g_frontend_last_frame_ms = pause_now_ms();
    g_frontend_run_active = 0;
    jvm_frontend_pause_eval(pause_now_ms());
}

/* v34.97: детерминированные скобки паузы для фронтендов, которые ЗНАЮТ,
 * когда их главный цикл блокируется (Switch: меню MINUS и per-game экран
 * PLUS блокируют цикл кадра под-циклом оверлея). Прежняя проводка
 * полагалась на stall-детектор (300 мс без тика), но детектор не умеет
 * отличать «цикл стоит в меню» от «цикл занят тяжёлым кадром»: одна
 * итерация длиннее порога взводила паузу посреди игры, фронтенд парковался
 * внутри paint() на первом же slow-check — и снимать паузу было уже
 * некому (trace v34.96: STUCK stage=repaints, td m в Graphics3D-нативе,
 * sp=0, локи m3g свободны, t2 run с растущим age). begin()/end() убирают
 * эвристику: пауза существует ровно между вызовами. Оба идемпотентны и
 * но-опы, пока фича выключена (enable не вызывался). */
void jvm_frontend_pause_begin(void) {
    if (!g_frontend_pause_enabled) return;
    uint64_t now = pause_now_ms();
    if (__sync_bool_compare_and_swap(&g_pause_state, 0, 1)) {
        g_pause_started_ms = now;     /* ДО взвода латча — как в eval */
        g_frontend_last_frame_ms = now;
        g_frontend_run_active = 0;
        g_frontend_pause_active = 1;
        LOG_SAFE("[FE-PAUSE] begin (explicit overlay)\n");
    }
}

void jvm_frontend_pause_end(void) {
    if (!g_frontend_pause_enabled) return;
    if (__sync_bool_compare_and_swap(&g_pause_state, 1, 0)) {
        uint64_t now = pause_now_ms();
        g_pause_total_ms += (now > g_pause_started_ms) ? now - g_pause_started_ms : 0;
        g_frontend_pause_active = 0;
        LOG_SAFE("[FE-PAUSE] end (explicit overlay)\n");
    }
    /* Цикл сейчас продолжится: свежий таймстамп кадра + active=1, чтобы
     * stall-детектор не мог взвести паузу от зазора закрытия меню. */
    jvm_frontend_frame_tick();
}

/* forward decls: eval-поток стартуется из enable() ниже */
#ifndef _WIN32
static void* pause_eval_thread(void* arg);
#else
static DWORD WINAPI pause_eval_thread_win(LPVOID arg);
#endif

/* Включение фичи (только libretro-сборка; headless/app не вызывают).
 * Сбрасывает накопленное время паузы — каждый загруженный контент
 * начинает с чистыми виртуальными часами (остаток прошлой сессии не
 * должен сдвигать currentTimeMillis). Запускает eval-поток один раз. */
void jvm_frontend_pause_enable(int on) {
    g_frontend_pause_enabled = on ? 1 : 0;
    if (!on) {
        jvm_frontend_pause_eval(pause_now_ms());
        return;
    }
    /* env: NOJME_PAUSE_STALL_MS=<ms>; 0 — выключить фичу целиком */
    {
        static int env_read = 0;
        if (!env_read) {
            env_read = 1;
            const char* e = getenv("NOJME_PAUSE_STALL_MS");
            if (e && e[0]) {
                long v = atol(e);
                if (v <= 0) {
                    g_frontend_pause_enabled = 0;
                } else {
                    if (v < 100) v = 100;
                    g_pause_stall_ms = (uint32_t)v;
                }
            }
        }
    }
    g_pause_total_ms = 0;
    g_pause_started_ms = 0;
    if (__sync_bool_compare_and_swap(&g_pause_state, 1, 0)) {
        g_frontend_pause_active = 0;
    }
    if (__sync_bool_compare_and_swap(&g_pause_eval_thread_started, 0, 1)) {
#ifdef _WIN32
        HANDLE h = CreateThread(NULL, 0, pause_eval_thread_win, NULL, 0, NULL);
        if (h) CloseHandle(h);
#else
        /* v36.02 EXIT-DAEMON-JOIN: keep the handle joinable (was detached).
         * The thread is reused across sessions; it is joined once, at the
         * process-exit path (jvm_frontend_pause_eval_shutdown). */
        pthread_t t;
        if (pthread_create(&t, NULL, pause_eval_thread, NULL) == 0) {
            g_pause_eval_pthread = t;
            g_pause_eval_have = 1;
        } else {
            g_pause_eval_thread_started = 0;
        }
#endif
    }
}

/* Eval-поток: единственный обязан всегда переоценивать stall, даже если
 * ни один VM-поток не спит и не интерпретирует (курсор латча для
 * интерпретатора). 100 мс — компромисс: латент взвода паузы после
 * открытия меню не заметен на фоне задержки фронтенда, цена — ноль. */
#ifndef _WIN32
static void* pause_eval_thread(void* arg) {
    (void)arg;
    /* v36.02 EXIT-DAEMON-JOIN: bounded lifetime — quits when main() asks. */
    while (!g_pause_eval_quit) {
        jvm_frontend_pause_eval(pause_now_ms());
        struct timespec req = { .tv_sec = 0, .tv_nsec = 100 * 1000000L };
        nanosleep(&req, NULL);
    }
    return NULL;
}
#else
static DWORD WINAPI pause_eval_thread_win(LPVOID arg) {
    (void)arg;
    while (!g_pause_eval_quit) {
        jvm_frontend_pause_eval(pause_now_ms());
        Sleep(100);
    }
    return 0;
}
#endif

/* v36.02 EXIT-DAEMON-JOIN: process-exit hook (called from main.c, Switch
 * frontend, BEFORE the SDL teardown and the raw _exit(0)). Sets the stop
 * flag and joins the eval thread. The thread wakes at 100 ms granularity,
 * so the join is bounded by that plus one eval pass. Idempotent. Returns
 * 1 if a live thread was joined, 0 if it was never started. */
int jvm_frontend_pause_eval_shutdown(void) {
    int joined = 0;
    g_pause_eval_quit = 1;
#ifndef _WIN32
    if (g_pause_eval_have &&
        !pthread_equal(g_pause_eval_pthread, pthread_self())) {
        pthread_join(g_pause_eval_pthread, NULL);
        joined = 1;
    }
    g_pause_eval_have = 0;
#endif
    return joined;
}


/* v34.59 PERF: событийное пробуждение спящих (см. threads.h). Спящий
 * поток ждёт chunk мс НА КОНДИЦИОНАЛЕ; broadcast при постановке
 * safepoint-запроса будит его мгновенно (раньше — до 50 мс до границы
 * nanosleep-куска: каждый System.gc() в 3D-играх с циклом
 * «sleep(50) + gc()» стоил ~40 мс паузы, [GCSTAMP] sp_wait).
 * Lock-порядок: этот мьютекс НЕ удерживается никем, кто берёт
 * heap/sp-мьютексы, — инверсии нет. */
static pthread_mutex_t g_sleep_wake_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_sleep_wake_cond  = PTHREAD_COND_INITIALIZER;

void jvm_gc_safepoint_wake_sleepers(void) {
    pthread_mutex_lock(&g_sleep_wake_mutex);
    pthread_cond_broadcast(&g_sleep_wake_cond);
    pthread_mutex_unlock(&g_sleep_wake_mutex);
}

/* v34.65: GC twin of jvm_gc_safepoint_wake_sleepers() for MONITOR waiters.
 * A waiter parked in pthread_cond_timedwait(mon->wait_cond) notices
 * g_gc_safepoint_request only at its chunk boundary — v34.61: up to 50ms,
 * the v34.65 default chunk would be up to 250ms — and every gc_collect()
 * blocks in jvm_gc_safepoint_wait_arrivals() until the LAST monitor waiter
 * arrives, so the chunk boundary directly padded every GC pause (GC-heavy
 * 3D games => FPS loss). Broadcasting each waiter's condvar directly
 * closes the gap: the waiter wakes, sees the flag, parks in
 * jvm_gc_safepoint_park(), and resumes its chunk afterwards.
 * Correctness with the v34.65 seq handshake: this wake does NOT bump
 * wait_seq, so waiters treat it as "not a notify" and keep waiting — the
 * handshake is exactly what makes aggressive waking safe (waking a waiter
 * WITHOUT it made wait() return spuriously: broken game choreography,
 * spin-repaint loops, CPU rise — the v34.64 regression pattern).
 * Lock-free snapshot walk (get_monitor fast-path pattern): entries are
 * never removed, old tables never freed. mon->mutex is never held across
 * allocation or parking, so taking it here cannot deadlock against the
 * heap lock the collector already holds. */
void jvm_gc_safepoint_wake_monitors(void) {
    size_t cap = MON_LOAD_ACQ(&g_mon_capacity);
    if (cap == 0) return;
    MonitorEntry* tab = MON_LOAD_ACQ(&g_mon_table);
    if (!tab) return;
    for (size_t i = 0; i < cap; i++) {
        JavaObject* owner = MON_LOAD_ACQ(&tab[i].owner);
        if (!owner) continue;
        CoopMonitor* mon = tab[i].mon;  /* published before owner (release) */
        if (!mon) continue;
#ifndef _WIN32
        pthread_mutex_lock(&mon->mutex);
        if (mon->wait_count > 0) {
            pthread_cond_broadcast(&mon->wait_cond);
        }
        pthread_mutex_unlock(&mon->mutex);
#else
        EnterCriticalSection(&mon->cs);
        if (mon->wait_count > 0) {
            WakeAllConditionVariable(&mon->wait_cond);
        }
        LeaveCriticalSection(&mon->cs);
#endif
    }
}

/* Спать chunk_ms, но мгновенно проснуться при safepoint-broadcast.
 * Возврат: 1 = разбужен broadcast'ом (или ложное срабатывание —
 * вызывающий цикл перепроверит время и флаг), 0 = кусок истёк. */
int jvm_sleep_chunk_ms(long chunk_ms) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += chunk_ms / 1000;
    deadline.tv_nsec += (chunk_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&g_sleep_wake_mutex);
    int rc = pthread_cond_timedwait(&g_sleep_wake_cond, &g_sleep_wake_mutex, &deadline);
    pthread_mutex_unlock(&g_sleep_wake_mutex);
    return (rc == 0);
}

/* v34.9 forward declarations for the blocked-thread registry */
static void mondiag_blocked_register(JavaThread* t, JavaObject* obj);
static void mondiag_blocked_unregister(JavaThread* t);

static int monitor_wait_impl(JVM* jvm, JavaObject* obj, jlong timeout, bool timed) {
    (void)jvm;
    if (!obj) return JNI_ERR;

    /* [MONDIAG] v34.9: log every monitor wait/notify with the calling Java
     * method to diagnose loading-thread choreography hangs (Bounce Tales). */
    JavaThread* mondiag_self = thread_current(jvm);
    {
        static int mondiag_n = 0;
        int n = ++mondiag_n;
        if (n <= 80 || (n % 200) == 0) {
            const char* cls = "?";
            const char* mth = "?";
            if (mondiag_self && mondiag_self->current_frame && mondiag_self->current_frame->clazz) {
                cls = mondiag_self->current_frame->clazz->class_name ?
                      mondiag_self->current_frame->clazz->class_name : "?";
                mth = (mondiag_self->current_frame->method && mondiag_self->current_frame->method->name) ?
                      mondiag_self->current_frame->method->name : "?";
            }
            LOG_SAFE("[MONDIAG] wait(obj=%p timeout=%lld timed=%d) from %s.%s tid=%d\n",
                     (void*)obj, (long long)timeout, timed ? 1 : 0, cls, mth,
                     mondiag_self ? mondiag_self->id : -1);
        }
    }

    /* v34.9: register as blocked — the watchdog dump walks frames ONLY of
     * threads parked here (stable frame chains, safe to read). */
    mondiag_blocked_register(mondiag_self, obj);
    /* Watch the first loader-ish wait object to trace its ownership */
    if (mondiag_self && mondiag_self->id >= 3 && !mondiag_watched_obj) {
        mondiag_set_watched(obj);
        LOG_SAFE("[MONDIAG] watching obj=%p (first loader wait, tid=%d)\n",
                 (void*)obj, mondiag_self->id);
    }

    CoopMonitor* mon = get_monitor(obj);
    
#ifndef _WIN32
    /* Pthread-based wait for Linux/Unix */
    pthread_t self = pthread_self();
    
    pthread_mutex_lock(&mon->mutex);
    
    /* Verify we own the monitor */
    if (!mon->is_pthread || !pthread_equal(mon->owner_pthread, self)) {
        pthread_mutex_unlock(&mon->mutex);
        return JNI_ERR;
    }
    
    /* Get current JavaThread for interrupt checking */
    extern JavaThread* thread_current(JVM* jvm);
    JavaThread* current_java_thread = thread_current(jvm);
    
    /* Check if already interrupted */
    if (current_java_thread && current_java_thread->interrupted) {
        /* Don't clear the flag - let caller handle it */
        pthread_mutex_unlock(&mon->mutex);
        return JNI_ERR;  /* Caller should throw InterruptedException */
    }
    
    /* Save entry count and release monitor */
    jint saved_count = mon->entry_count;
    mon->owner = NULL;
    mon->owner_pthread = (pthread_t)0;
    mon->is_pthread = 0;
    mon->entry_count = 0;
    mon->wait_count++;
    
    /* Signal entry_cond so threads waiting to enter the monitor can proceed */
    if (mon->entry_wait_count > 0) {
        pthread_cond_signal(&mon->entry_cond);
    }
    
    /* Store waiting thread for interrupt support */
    mon->waiting_thread = current_java_thread;
    
    /* Wait on condition variable - use timed wait with periodic interrupt checks */
    int wait_result = 0;
    jlong remaining = timeout;
    /* v34.65 WAIT-HANDSHAKE: snapshot the notify sequence NOW (we hold
     * mon->mutex and are registered: wait_count>0, waiting_thread set).
     * Every wake below re-compares: changed => a real notify() arrived at
     * ANY point since registration — including while we were parked
     * outside the condvar for a GC safepoint (waiting_thread briefly
     * NULL: the lost-wakeup window that hung the Asphalt loader at 7%
     * CPU) — unchanged => spurious wake / GC wake / interrupt signal:
     * keep waiting per the Object.wait() contract (v34.61 returned on ANY
     * cond signal: spurious wakes escaped wait() early). */
    jint my_seq = mon->wait_seq;
    /* v34.65 PERF: event paths carry the real latency (seq handshake,
     * thread_interrupt signal, GC wake_monitors); the chunk is only the
     * interrupt safety-net => 50ms -> g_wait_chunk_ms (default 250). */
    jlong check_interval = g_wait_chunk_ms;
    if (mondiag_self && mondiag_self->id >= 0 && mondiag_self->id < 16) {
        mondiag_dbg_state[mondiag_self->id] = 1;
        mondiag_dbg_iter[mondiag_self->id] = 0;
    }
    
    while (1) {
        if (timed && timeout > 0) {
            if (mondiag_self && mondiag_self->id >= 0 && mondiag_self->id < 16) mondiag_dbg_iter[mondiag_self->id]++;
            /* v34.81 FRONTEND PAUSE: freeze the timeout budget. While the
             * frontend menu holds the core paused, Object.wait does not
             * burn `remaining`: we park WITHOUT the monitor (wait_count /
             * waiting_thread bookkeeping mirrors the GC block below --
             * the collector must never block on mon->mutex). */
            {
                extern volatile int g_frontend_pause_active;
                if (g_frontend_pause_active) {
                    extern void jvm_frontend_pause_park(void);
                    mon->waiting_thread = NULL;
                    mon->wait_count--;
                    pthread_mutex_unlock(&mon->mutex);
                    jvm_frontend_pause_park();
                    pthread_mutex_lock(&mon->mutex);
                    mon->waiting_thread = current_java_thread;
                    mon->wait_count++;
                }
            }
            jlong wait_time = (remaining < check_interval) ? remaining : check_interval;
            /* v34.59 PERF: GC ждёт наш паркинг — ужимаем кусок ожидания
             * с 50 до 10 мс (тот же приём, что и Thread.sleep; полное
             * событийное пробуждение здесь невозможно: ждём на cond
             * КОНКРЕТНОГО монитора, который коллектор не знает). */
            if (g_gc_safepoint_request && wait_time > 10) wait_time = 10;
            if (wait_time <= 0) {
                wait_result = ETIMEDOUT;
                break;
            }
            /* v34.84 FIX (slow close): VM shutdown aborts the wait —
             * see the matching note in native.c Thread.sleep. */
            if (jvm && !jvm->running) {
                wait_result = ETIMEDOUT;
                break;
            }
            
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += wait_time / 1000;
            ts.tv_nsec += (wait_time % 1000) * 1000000;
            if (ts.tv_nsec >= 1000000000) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000;
            }
            wait_result = pthread_cond_timedwait(&mon->wait_cond, &mon->mutex, &ts);
            /* v34.9 safepoint poll: release mon->mutex while parking */
            if (g_gc_safepoint_request) {
                mon->waiting_thread = NULL;
                mon->wait_count--;
                pthread_mutex_unlock(&mon->mutex);
                jvm_gc_safepoint_park();
                pthread_mutex_lock(&mon->mutex);
                mon->waiting_thread = current_java_thread;
                mon->wait_count++;
            }
            
            /* ALWAYS check for interrupt after waking */
            if (current_java_thread && current_java_thread->interrupted) {
                mon->waiting_thread = NULL;
                mon->wait_count--;
                
                if (current_java_thread && current_java_thread->id >= 0 && current_java_thread->id < 16) { mondiag_dbg_state[current_java_thread->id] = 2; mondiag_dbg_iter[current_java_thread->id] = 0; }
    if (mondiag_self && mondiag_self->id >= 0 && mondiag_self->id < 16) { mondiag_dbg_state[mondiag_self->id] = 2; mondiag_dbg_iter[mondiag_self->id] = 0; }
                /* Re-acquire monitor */
                while (mon->entry_count > 0) {
                    if (current_java_thread && current_java_thread->id >= 0 && current_java_thread->id < 16) mondiag_dbg_iter[current_java_thread->id]++;
                    struct timespec ts2;
                    clock_gettime(CLOCK_REALTIME, &ts2);
                    ts2.tv_nsec += 10000000;  /* 10ms */
                    if (ts2.tv_nsec >= 1000000000) {
                        ts2.tv_sec++;
                        ts2.tv_nsec -= 1000000000;
                    }
                    pthread_cond_timedwait(&mon->entry_cond, &mon->mutex, &ts2);
                }
                
                mon->owner = obj;
                mon->owner_pthread = self;
                mon->is_pthread = 1;
                mon->entry_count = saved_count;
                pthread_mutex_unlock(&mon->mutex);
                
                return JNI_ERR;  /* Interrupted */
            }
            
            /* v34.65 WAIT-HANDSHAKE: real notify => sequence moved.
             * Covers the GC-park window (notify landed while we were
             * outside the condvar with wait_count briefly 0 — its signal
             * evaporated, but the bump did not). */
            if (mon->wait_seq != my_seq) {
                wait_result = 0;
                break;
            }
            if (wait_result == ETIMEDOUT) {
                remaining -= wait_time;
                if (remaining <= 0) {
                    break;  /* Full timeout elapsed */
                }
                /* Continue waiting */
                continue;
            }
            
            /* v34.65: wait_result==0 with UNCHANGED sequence = spurious
             * wake or GC-safepoint wake of this condvar — NOT a notify;
             * keep waiting (real notifies broke above). EINTR also lands
             * here: v34.61 let it fall out of the loop entirely (wait()
             * returned early as if notified). */
            continue;
        } else {
            /* Non-timed wait - use timed wait with interrupt checks */
            /* v34.84 FIX (slow close): untimed Object.wait() would park in
             * 250 ms chunks until a notify that never comes during VM
             * teardown — jvm_destroy timed out after 3 s and leaked the
             * whole VM. Abort on shutdown; the interpreter exits the
             * thread at its next opcode check. */
            if (jvm && !jvm->running) {
                wait_result = ETIMEDOUT;
                break;
            }
            jlong wait_time = check_interval;  /* v34.65: default 250ms */
            if (g_gc_safepoint_request && wait_time > 10) wait_time = 10;  /* v34.59 */
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += wait_time * 1000000;  /* chunked wait */
            if (ts.tv_nsec >= 1000000000) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000;
            }
            wait_result = pthread_cond_timedwait(&mon->wait_cond, &mon->mutex, &ts);
            /* v34.9 safepoint poll: release mon->mutex while parking */
            if (g_gc_safepoint_request) {
                mon->waiting_thread = NULL;
                mon->wait_count--;
                pthread_mutex_unlock(&mon->mutex);
                jvm_gc_safepoint_park();
                pthread_mutex_lock(&mon->mutex);
                mon->waiting_thread = current_java_thread;
                mon->wait_count++;
            }
            
            /* Check for interrupt */
            if (current_java_thread && current_java_thread->interrupted) {
                /* Don't clear interrupted flag - let caller handle it */
                mon->waiting_thread = NULL;
                mon->wait_count--;
                
                if (current_java_thread && current_java_thread->id >= 0 && current_java_thread->id < 16) { mondiag_dbg_state[current_java_thread->id] = 2; mondiag_dbg_iter[current_java_thread->id] = 0; }
    if (mondiag_self && mondiag_self->id >= 0 && mondiag_self->id < 16) { mondiag_dbg_state[mondiag_self->id] = 2; mondiag_dbg_iter[mondiag_self->id] = 0; }
                /* Re-acquire monitor */
                while (mon->entry_count > 0) {
                    if (current_java_thread && current_java_thread->id >= 0 && current_java_thread->id < 16) mondiag_dbg_iter[current_java_thread->id]++;
                    struct timespec ts2;
                    clock_gettime(CLOCK_REALTIME, &ts2);
                    ts2.tv_nsec += 10000000;  /* 10ms */
                    if (ts2.tv_nsec >= 1000000000) {
                        ts2.tv_sec++;
                        ts2.tv_nsec -= 1000000000;
                    }
                    pthread_cond_timedwait(&mon->entry_cond, &mon->mutex, &ts2);
                }
                
                mon->owner = obj;
                mon->owner_pthread = self;
                mon->is_pthread = 1;
                mon->entry_count = saved_count;
                pthread_mutex_unlock(&mon->mutex);
                
                return JNI_ERR;  /* Interrupted */
            }
            
            /* If we got a real signal (not timeout), we're done */
            /* v34.65 WAIT-HANDSHAKE: real notify => sequence moved; any
             * other wake (chunk timeout / spurious / GC) just re-polls
             * the interrupt flag on the next chunk. */
            if (mon->wait_seq != my_seq) {
                wait_result = 0;
                break;
            }
            /* ETIMEDOUT means we just need to check again */
            continue;
        }
        break;
    }
    
    mon->waiting_thread = NULL;
    mon->wait_count--;
    
    if (mondiag_self && mondiag_self->id >= 0 && mondiag_self->id < 16) { mondiag_dbg_state[mondiag_self->id] = 2; mondiag_dbg_iter[mondiag_self->id] = 0; }
    if (mondiag_self && mondiag_self->id >= 0 && mondiag_self->id < 16) { mondiag_dbg_state[mondiag_self->id] = 2; mondiag_dbg_iter[mondiag_self->id] = 0; }
    /* Re-acquire monitor */
    while (mon->entry_count > 0) {
        if (current_java_thread && current_java_thread->id >= 0 && current_java_thread->id < 16) mondiag_dbg_iter[current_java_thread->id]++;
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 10000000;  /* 10ms */
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
        }
        pthread_cond_timedwait(&mon->entry_cond, &mon->mutex, &ts);
        if (g_gc_safepoint_request) {
            pthread_mutex_unlock(&mon->mutex);
            jvm_gc_safepoint_park();
            pthread_mutex_lock(&mon->mutex);
        }
    }
    if (mondiag_self && mondiag_self->id >= 0 && mondiag_self->id < 16) mondiag_dbg_state[mondiag_self->id] = 3;
    if (current_java_thread && current_java_thread->id >= 0 && current_java_thread->id < 16) mondiag_dbg_state[current_java_thread->id] = 3;
    
    mon->owner = obj;
    mon->owner_pthread = self;
    mon->is_pthread = 1;
    mon->entry_count = saved_count;
    
    pthread_mutex_unlock(&mon->mutex);
    
    return JNI_OK;
#else
    /* Windows: use CRITICAL_SECTION and CONDITION_VARIABLE for native threads */
    DWORD self_id = GetCurrentThreadId();
    
    EnterCriticalSection(&mon->cs);
    
    /* Verify we own the monitor */
    if (!mon->is_pthread || mon->owner_thread_id != self_id) {
        LeaveCriticalSection(&mon->cs);
        return JNI_ERR;
    }
    
    /* Get current JavaThread for interrupt checking */
    extern JavaThread* thread_current(JVM* jvm);
    JavaThread* current_java_thread = thread_current(jvm);
    
    /* Find CoopThread for state updates */
    CoopThread* current_coop = NULL;
    for (int i = 0; i < scheduler.thread_count; i++) {
        if (scheduler.threads[i]->java_thread == current_java_thread) {
            current_coop = scheduler.threads[i];
            break;
        }
    }
    
    /* Check if already interrupted */
    if (current_java_thread && current_java_thread->interrupted) {
        LeaveCriticalSection(&mon->cs);
        return JNI_ERR;  /* Caller should throw InterruptedException */
    }
    
    /* Update thread state to WAITING or TIMED_WAITING */
    if (current_coop) {
        current_coop->state = timed ? THREAD_STATE_TIMED_WAITING : THREAD_STATE_WAITING;
    }
    
    /* Save entry count and release monitor */
    jint saved_count = mon->entry_count;
    mon->owner = NULL;
    mon->owner_thread_id = 0;
    mon->is_pthread = 0;
    mon->entry_count = 0;
    mon->wait_count++;
    
    /* Signal entry_cond so threads waiting to enter the monitor can proceed */
    if (mon->entry_wait_count > 0) {
        WakeConditionVariable(&mon->entry_cond);
    }
    
    /* Store waiting thread for interrupt support */
    mon->waiting_thread = current_java_thread;
    
    /* Wait on condition variable - use timed wait with periodic interrupt checks.
     * v34.66: the old `BOOL wait_result` accumulator was set but never read
     * (exit criteria are the seq handshake / interrupt flag / elapsed
     * deadline) — removed to keep -Wunused-but-set-variable clean on MinGW. */
    /* v34.65 WAIT-HANDSHAKE (Windows twin of the POSIX branch): notify
     * exits ONLY on a sequence change; spurious / GC / interrupt wakes
     * re-poll the interrupt flag and deadline at the loop top. Chunk
     * raised 50ms -> g_wait_chunk_ms (default 250) — wake latency for
     * notify/interrupt/GC is event-driven, the chunk is only the
     * interrupt safety-net. */
    jint my_seq = mon->wait_seq;
    jlong check_interval = g_wait_chunk_ms;
    DWORD timeout_ms = INFINITE;
    
    /* Track elapsed time for timed waits */
    jlong start_time = 0;
    /* v34.81 FRONTEND PAUSE: pause offset snapshot + short-chunk flag */
    uint64_t poff_at_entry = 0;
    int win_pause_chunk = 0;
    if (timed && timeout > 0) {
        {
            extern uint64_t jvm_pause_offset_ms(void);
            poff_at_entry = jvm_pause_offset_ms();
        }
        start_time = (jlong)GetTickCount64();
    }
    
    while (1) {
        /* v34.10 GC safepoint poll (Windows twin of the POSIX branch):
         * release the monitor CS while parking so gc_collect() can run.
         * v34.65: GC wakes us via jvm_gc_safepoint_wake_monitors() now, so
         * this poll is only a backstop for a wake missed mid-registration. */
        if (g_gc_safepoint_request) {
            LeaveCriticalSection(&mon->cs);
            jvm_gc_safepoint_park();
            EnterCriticalSection(&mon->cs);
            /* v34.65: a notify may have landed while we were parked
             * outside the condvar (lost-signal window) — the seq
             * handshake catches it here instead of after the next
             * chunk. */
            if (mon->wait_seq != my_seq) {
                break;
            }
        }

        /* Check for interrupt at the start of each iteration */
        if (current_java_thread && current_java_thread->interrupted) {
            mon->waiting_thread = NULL;
            mon->wait_count--;
            
            /* Re-acquire monitor - use entry_cond */
            while (mon->entry_count > 0) {
                SleepConditionVariableCS(&mon->entry_cond, &mon->cs, 10);
            }
            
            mon->owner = obj;
            mon->owner_thread_id = self_id;
            mon->is_pthread = 1;
            mon->entry_count = saved_count;
            LeaveCriticalSection(&mon->cs);
            
            return JNI_ERR;  /* Interrupted */
        }
        
        if (timed && timeout > 0) {
            /* Calculate remaining time (v34.81 FRONTEND PAUSE: subtract
             * the pause accumulated AFTER we entered wait -- the timeout
             * budget must not burn while the frontend menu holds the core;
             * while paused we keep waiting in short 15ms chunks with the CS
             * released by SleepConditionVariableCS, so the seq/interrupt
             * handshake checks below still run on every wake). */
            jlong elapsed = (jlong)GetTickCount64() - start_time;
            {
                extern uint64_t jvm_pause_offset_ms(void);
                extern volatile int g_frontend_pause_active;
                uint64_t pnow = jvm_pause_offset_ms();
                if (pnow > poff_at_entry) elapsed -= (jlong)(pnow - poff_at_entry);
                win_pause_chunk = g_frontend_pause_active ? 1 : 0;
            }
            jlong remaining = timeout - elapsed;
            
            if (remaining <= 0) {
                /* Timeout has elapsed */
                break;
            }
            
            /* Wait for the shorter of remaining time or check interval */
            jlong wait_time = (remaining < check_interval) ? remaining : check_interval;
            /* v34.65: Windows twin of the POSIX safepoint shrink (was
             * missing here — during a GC safepoint request a Windows
             * waiter kept sleeping full 50ms chunks, padding every GC
             * pause by up to a chunk per waiting thread). */
            if (g_gc_safepoint_request && wait_time > 10) wait_time = 10;
            if (win_pause_chunk && wait_time > 15) wait_time = 15;
            timeout_ms = (DWORD)wait_time;
            
            SleepConditionVariableCS(&mon->wait_cond, &mon->cs, timeout_ms);
            
            /* v34.65 WAIT-HANDSHAKE: real notify => sequence moved. */
            if (mon->wait_seq != my_seq) {
                break;
            }
            
            /* Spurious / GC / interrupt-signal wake (woken without a
             * sequence change) or timeout: the loop top re-checks the
             * interrupt flag and the elapsed deadline. v34.61 broke on ANY
             * wake — spurious wakes escaped wait() early. */
            continue;
        } else {
            /* Non-timed wait - chunked with interrupt checks */
            jlong wait_time = check_interval;
            if (g_gc_safepoint_request && wait_time > 10) wait_time = 10;
            SleepConditionVariableCS(&mon->wait_cond, &mon->cs, (DWORD)wait_time);
            
            /* v34.65 WAIT-HANDSHAKE: real notify => sequence moved; any
             * other wake re-polls the interrupt flag at the loop top. */
            if (mon->wait_seq != my_seq) {
                break;
            }
            /* ERROR_TIMEOUT / spurious: just check again for interrupt */
            continue;
        }
    }
    
    mon->waiting_thread = NULL;
    mon->wait_count--;
    
    /* Re-acquire monitor - use entry_cond */
    while (mon->entry_count > 0) {
        SleepConditionVariableCS(&mon->entry_cond, &mon->cs, 10);
    }
    
    mon->owner = obj;
    mon->owner_thread_id = self_id;
    mon->is_pthread = 1;
    mon->entry_count = saved_count;
    
    /* Restore thread state to RUNNABLE */
    if (current_coop) {
        current_coop->state = THREAD_STATE_RUNNABLE;
    }
    
    LeaveCriticalSection(&mon->cs);
    
    /* CRITICAL: Check if we were interrupted after breaking out of wait loop.
     * The interrupt may have arrived after we broke out but before we returned.
     * This ensures InterruptedException is thrown when interrupted.
     */
    if (current_java_thread && current_java_thread->interrupted) {
        return JNI_ERR;
    }
    
    return JNI_OK;
#endif
}

/* ============ v34.9 blocked-thread registry (safe watchdog dumps) ============ */
#define MONDIAG_MAX 16
typedef struct {
    JavaThread* thread;
    JavaObject* obj;
    int in_use;
} MonDiagBlocked;
static MonDiagBlocked mondiag_blocked[MONDIAG_MAX];

static void mondiag_blocked_register(JavaThread* t, JavaObject* obj) {
    if (!t) return;
    for (int i = 0; i < MONDIAG_MAX; i++) {
        if (!mondiag_blocked[i].in_use) {
            mondiag_blocked[i].thread = t;
            mondiag_blocked[i].obj = obj;
            mondiag_blocked[i].in_use = 1;
            return;
        }
    }
}

static void mondiag_blocked_unregister(JavaThread* t) {
    if (!t) return;
    for (int i = 0; i < MONDIAG_MAX; i++) {
        if (mondiag_blocked[i].in_use && mondiag_blocked[i].thread == t) {
            mondiag_blocked[i].in_use = 0;
            mondiag_blocked[i].thread = NULL;
            mondiag_blocked[i].obj = NULL;
        }
    }
}

/* Dump stacks of threads parked in monitor_wait. Frame chains of blocked
 * threads are stable, so walking them here is safe (unlike live threads). */
void jvm_dump_blocked_threads(void) {
    int any = 0;
    for (int i = 0; i < MONDIAG_MAX; i++) {
        if (!mondiag_blocked[i].in_use) continue;
        JavaThread* t = mondiag_blocked[i].thread;
        if (!t) continue;
        any = 1;
        JavaObject* obj = mondiag_blocked[i].obj;
        LOG_SAFE("[THREADDUMP] tid=%d BLOCKED on monitor(obj=%p):\n", t->id, (void*)obj);
        JavaFrame* f = t->current_frame;
        int depth = 0;
        while (f && depth < 14) {
            LOG_SAFE("[THREADDUMP]   [%d] %s.%s pc=%u\n", depth,
                     f->clazz ? (f->clazz->class_name ? f->clazz->class_name : "?") : "?",
                     f->method ? (f->method->name ? f->method->name : "?") : "?",
                     (unsigned)f->pc);
            f = f->prev;
            depth++;
        }
    }
    if (!any) {
        LOG_SAFE("[THREADDUMP] no threads parked in monitor_wait\n");
    }
    fflush(stderr);
}

/* Public entry: wrapper that registers/unregisters around the impl so every
 * return path (normal, timeout, interrupt, error) unregisters exactly once. */
int monitor_wait(JVM* jvm, JavaObject* obj, jlong timeout, bool timed) {
    JavaThread* self = thread_current(jvm);
    mondiag_blocked_register(self, obj);
    int rc = monitor_wait_impl(jvm, obj, timeout, timed);
    mondiag_blocked_unregister(self);
    return rc;
}

int monitor_notify(JVM* jvm, JavaObject* obj) {
    (void)jvm;
    if (!obj) return JNI_ERR;

    {
        static int mondiag_nn = 0;
        int n = ++mondiag_nn;
        if (n <= 80 || (n % 200) == 0) {
            const char* cls = "?";
            const char* mth = "?";
            JavaThread* cur = thread_current(jvm);
            if (cur && cur->current_frame && cur->current_frame->clazz) {
                cls = cur->current_frame->clazz->class_name ?
                      cur->current_frame->clazz->class_name : "?";
                mth = (cur->current_frame->method && cur->current_frame->method->name) ?
                      cur->current_frame->method->name : "?";
            }
            LOG_SAFE("[MONDIAG] notify(obj=%p) from %s.%s tid=%d\n",
                     (void*)obj, cls, mth, cur ? cur->id : -1);
        }
    }

    CoopMonitor* mon = get_monitor(obj);
    
#ifndef _WIN32
    /* Pthread-based notify for Linux/Unix */
    pthread_t self = pthread_self();
    
    pthread_mutex_lock(&mon->mutex);
    
    /* Verify we own the monitor */
    if (!mon->is_pthread || !pthread_equal(mon->owner_pthread, self)) {
        pthread_mutex_unlock(&mon->mutex);
        return JNI_ERR;
    }
    
    /* v34.65 WAIT-HANDSHAKE: bump the sequence FIRST, then ALWAYS signal.
     * Waiters decide "real notify" purely by the sequence change, so a
     * signal with nobody in the condvar is a cheap no-op — while SKIPPING
     * the signal when wait_count transiently reads 0 (a waiter parked for
     * a GC safepoint unregisters before parking) is exactly the
     * lost-wakeup window that hung the Asphalt loader at 7% CPU. */
    mon->wait_seq++;
    pthread_cond_signal(&mon->wait_cond);
    
    pthread_mutex_unlock(&mon->mutex);
    return JNI_OK;
#else
    /* Windows: use CRITICAL_SECTION for native threads */
    DWORD self_id = GetCurrentThreadId();
    
    EnterCriticalSection(&mon->cs);
    
    /* Verify we own the monitor */
    if (!mon->is_pthread || mon->owner_thread_id != self_id) {
        LeaveCriticalSection(&mon->cs);
        return JNI_ERR;
    }
    
    /* Signal one waiting thread - use wait_cond */
    /* v34.65 WAIT-HANDSHAKE (Windows twin): seq bump + always wake. */
    mon->wait_seq++;
    WakeConditionVariable(&mon->wait_cond);
    
    LeaveCriticalSection(&mon->cs);
    return JNI_OK;
#endif
}

int monitor_notify_all(JVM* jvm, JavaObject* obj) {
    (void)jvm;
    if (!obj) return JNI_ERR;
    
    CoopMonitor* mon = get_monitor(obj);
    
#ifndef _WIN32
    /* Pthread-based notify all for Linux/Unix */
    pthread_t self = pthread_self();
    
    pthread_mutex_lock(&mon->mutex);
    
    /* Verify we own the monitor */
    if (!mon->is_pthread || !pthread_equal(mon->owner_pthread, self)) {
        pthread_mutex_unlock(&mon->mutex);
        return JNI_ERR;
    }
    
    /* v34.65 WAIT-HANDSHAKE: seq bump + always broadcast (see notify). */
    mon->wait_seq++;
    pthread_cond_broadcast(&mon->wait_cond);
    
    pthread_mutex_unlock(&mon->mutex);
    return JNI_OK;
#else
    /* Windows: use CRITICAL_SECTION for native threads */
    DWORD self_id = GetCurrentThreadId();
    
    EnterCriticalSection(&mon->cs);
    
    /* Verify we own the monitor */
    if (!mon->is_pthread || mon->owner_thread_id != self_id) {
        LeaveCriticalSection(&mon->cs);
        return JNI_ERR;
    }
    
    /* Signal all waiting threads - use wait_cond */
    /* v34.65 WAIT-HANDSHAKE (Windows twin): seq bump + always wake-all. */
    mon->wait_seq++;
    WakeAllConditionVariable(&mon->wait_cond);
    
    LeaveCriticalSection(&mon->cs);
    return JNI_OK;
#endif
}

JavaMonitor* monitor_get(JVM* jvm, JavaObject* obj) {
    (void)jvm; (void)obj;
    return NULL;
}

JavaThread* monitor_get_owner(JavaObject* obj) {
    if (!obj) return NULL;
    CoopMonitor* mon = get_monitor(obj);
#ifndef _WIN32
    pthread_mutex_lock(&mon->mutex);
    JavaThread* owner = mon->is_pthread ? NULL : mon->owner_thread;
    pthread_mutex_unlock(&mon->mutex);
#else
    EnterCriticalSection(&mon->cs);
    JavaThread* owner = mon->is_pthread ? NULL : mon->owner_thread;
    LeaveCriticalSection(&mon->cs);
#endif
    return owner;
}

jint monitor_get_entry_count(JavaObject* obj) {
    if (!obj) return 0;
    CoopMonitor* mon = get_monitor(obj);
    monitor_spinlock_acquire(mon);
    jint count = mon->entry_count;
    monitor_spinlock_release(mon);
    return count;
}

/*
 * Thread-local storage
 */
int thread_local_create(ThreadLocalKey* key) {
#ifdef _WIN32
    DWORD idx = TlsAlloc();
    if (idx == TLS_OUT_OF_INDEXES) {
        return JNI_ERR;
    }
    *key = (ThreadLocalKey)idx;
#else
    static int next_key = 0;
    *key = ++next_key;
#endif
    return JNI_OK;
}

int thread_local_set(ThreadLocalKey key, void* value) {
#ifdef _WIN32
    TlsSetValue((DWORD)key, value);
#else
    (void)key; (void)value;
    /* For POSIX, use pthread_setspecific if needed */
#endif
    return JNI_OK;
}

void* thread_local_get(ThreadLocalKey key) {
#ifdef _WIN32
    return TlsGetValue((DWORD)key);
#else
    (void)key;
    return NULL;
#endif
}

void thread_local_delete(ThreadLocalKey key) {
#ifdef _WIN32
    TlsFree((DWORD)key);
#else
    (void)key;
#endif
}

/*
 * Atomic operations
 */
void memory_barrier(void) {
#ifdef _WIN32
    MemoryBarrier();
#elif defined(__GNUC__)
    __sync_synchronize();
#endif
}

void memory_barrier_acquire(void) {
    memory_barrier();
}

void memory_barrier_release(void) {
    memory_barrier();
}

bool atomic_cas_int(volatile jint* ptr, jint expected, jint newval) {
#ifdef _WIN32
    return InterlockedCompareExchange((LONG volatile*)ptr, newval, expected) == expected;
#elif defined(__GNUC__)
    return __sync_bool_compare_and_swap(ptr, expected, newval);
#else
    if (*ptr == expected) {
        *ptr = newval;
        return true;
    }
    return false;
#endif
}

bool atomic_cas_long(volatile jlong* ptr, jlong expected, jlong newval) {
#ifdef _WIN32
    return InterlockedCompareExchange64((LONG64 volatile*)ptr, newval, expected) == expected;
#elif defined(__GNUC__)
    return __sync_bool_compare_and_swap(ptr, expected, newval);
#else
    if (*ptr == expected) {
        *ptr = newval;
        return true;
    }
    return false;
#endif
}

bool atomic_cas_ptr(void* volatile* ptr, void* expected, void* newval) {
#ifdef _WIN32
    return InterlockedCompareExchangePointer(ptr, newval, expected) == expected;
#elif defined(__GNUC__)
    return __sync_bool_compare_and_swap(ptr, expected, newval);
#else
    if (*ptr == expected) {
        *ptr = newval;
        return true;
    }
    return false;
#endif
}

jint atomic_increment(volatile jint* ptr) {
#ifdef _WIN32
    return InterlockedIncrement((LONG volatile*)ptr);
#elif defined(__GNUC__)
    return __sync_add_and_fetch(ptr, 1);
#else
    return ++(*ptr);
#endif
}

jint atomic_decrement(volatile jint* ptr) {
#ifdef _WIN32
    return InterlockedDecrement((LONG volatile*)ptr);
#elif defined(__GNUC__)
    return __sync_sub_and_fetch(ptr, 1);
#else
    return --(*ptr);
#endif
}

jint atomic_add(volatile jint* ptr, jint value) {
#ifdef _WIN32
    return InterlockedAdd((LONG volatile*)ptr, value);
#elif defined(__GNUC__)
    return __sync_add_and_fetch(ptr, value);
#else
    *ptr += value;
    return *ptr;
#endif
}

/*
 * Lock-free queue
 */
void lfqueue_init(LFQueue* queue) {
    if (queue) {
        queue->head = NULL;
        queue->tail = NULL;
    }
}

int lfqueue_push(LFQueue* queue, void* data) {
    if (!queue) return -1;
    
    LFQueueNode* node = (LFQueueNode*)malloc(sizeof(LFQueueNode));
    if (!node) return -1;
    
    node->data = data;
    node->next = NULL;
    
    if (queue->tail) {
        queue->tail->next = node;
    } else {
        queue->head = node;
    }
    queue->tail = node;
    
    return 0;
}

void* lfqueue_pop(LFQueue* queue) {
    if (!queue || !queue->head) return NULL;
    
    LFQueueNode* node = queue->head;
    void* data = node->data;
    queue->head = node->next;
    
    if (!queue->head) {
        queue->tail = NULL;
    }
    
    free(node);
    return data;
}

bool lfqueue_empty(LFQueue* queue) {
    return !queue || queue->head == NULL;
}

void lfqueue_destroy(LFQueue* queue) {
    if (!queue) return;
    
    while (queue->head) {
        LFQueueNode* node = queue->head;
        queue->head = node->next;
        free(node);
    }
    queue->tail = NULL;
}

/*
 * Cleanup all monitor resources on JVM shutdown
 * This prevents resource leaks with pthread mutex and condition variables
 */
void cleanup_monitors(void) {
    if (!g_monitors_initialized) return;

    MON_TABLE_LOCK();
    /* v20 (P0-5): destroy every lazily created monitor, then the table. */
    for (size_t i = 0; i < g_mon_capacity; i++) {
        CoopMonitor* mon = g_mon_table[i].mon;
        if (!mon) continue;
#ifndef _WIN32
        pthread_mutex_destroy(&mon->mutex);
        pthread_cond_destroy(&mon->entry_cond);
        pthread_cond_destroy(&mon->wait_cond);
#else
        DeleteCriticalSection(&mon->cs);
#endif
        free(mon);
        g_mon_table[i].owner = NULL;
        g_mon_table[i].mon = NULL;
    }
    free(g_mon_table);
    g_mon_table = NULL;
    g_mon_capacity = 0;
    g_mon_count = 0;
    MON_TABLE_UNLOCK();

    g_monitors_initialized = 0;
}