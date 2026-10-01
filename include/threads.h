/*
 * J2ME Emulator - Thread Management (Simplified/Single-threaded)
 * All threading operations are stubs - no actual threads created
 */

#ifndef THREADS_H
#define THREADS_H

#include "jvm.h"

/*
 * Thread priorities
 */
#define THREAD_PRIORITY_MIN     1
#define THREAD_PRIORITY_NORM    5
#define THREAD_PRIORITY_MAX     10

/* Yield interval - switch threads after this many opcodes */
#define YIELD_INTERVAL 1000

/*
 * Thread states (matches java.lang.Thread.State)
 */
typedef enum {
    THREAD_STATE_NEW,
    THREAD_STATE_RUNNABLE,
    THREAD_STATE_BLOCKED,
    THREAD_STATE_WAITING,
    THREAD_STATE_TIMED_WAITING,
    THREAD_STATE_TERMINATED
} ThreadState;

/*
 * Monitor (synchronization object) - stub
 */
typedef struct JavaMonitor {
    JavaObject* owner;          /* For compatibility with object monitor protocol */
    JavaThread* owner_thread;   /* Actual thread owner - use this for thread checks */
    jint entry_count;
} JavaMonitor;

/*
 * Thread management functions (all simplified)
 */

/* Initialize thread system */
int threads_init(JVM* jvm);

/* Create new Java thread */
JavaThread* thread_create(JVM* jvm, const char* name, jint priority, 
                          JavaObject* thread_obj);

/* Start a thread */
int thread_start(JVM* jvm, JavaThread* thread);

/* Join a thread */
int thread_join(JVM* jvm, JavaThread* thread);

/* v34.71: OS identity of the main/frontend Java thread (captured once in
 * jvm_init). Used by the heap's TLAB gate — see heap.c "TLAB main-thread
 * gate" for why allocations on this thread between exec-windows must not
 * hold a TLAB chunk. */
void jvm_record_main_os_thread(void);
int jvm_os_thread_is_main_java(void);

/* Get current thread */
JavaThread* thread_current(JVM* jvm);

/* Yield to other threads */
void thread_yield(JVM* jvm);

/* v34.45: java.lang.Thread.yield() entry — adaptive KVM-style rest for
 * busy-wait frame limiters (`while (now - t0 < period) yield();`), then
 * the full cooperative path. See threads.c for the rationale. */
void thread_yield_explicit(JVM* jvm);

/* Check if it's time to yield based on global instruction counter */
bool thread_should_yield(void);

/* Increment instruction counter and yield if needed */
void thread_tick(JVM* jvm);

/* Get global instruction counter */
uint64_t thread_get_instruction_counter(void);

/* Schedule next thread (cooperative context switch) */
void thread_schedule(JVM* jvm);

/* Check if there are runnable threads */
bool threads_has_runnable(void);

/* Sleep for milliseconds */
int thread_sleep(JVM* jvm, jlong millis);

/* Check if thread is alive */
bool thread_is_alive(JavaThread* thread);

/* Check if any thread terminated with uncaught exception */
bool thread_has_uncaught_exception(void);

/* Get thread state */
ThreadState thread_get_state(JavaThread* thread);

/* Set thread priority */
void thread_set_priority(JavaThread* thread, jint priority);

/* Interrupt a thread */
void thread_interrupt(JavaThread* thread);

/* Check if thread is interrupted */
bool thread_is_interrupted(JavaThread* thread, bool clear_flag);

/* Find thread by thread object */
JavaThread* thread_find_by_object(JavaObject* thread_obj);

/* Destroy a thread */
void thread_destroy(JVM* jvm, JavaThread* thread);

/*
 * Monitor functions (synchronization) - all stubs
 */

int monitor_enter(JVM* jvm, JavaObject* obj);
int monitor_tryenter(JVM* jvm, JavaObject* obj);  /* v36.57 [KEY-TRYLOCK] */
int monitor_exit(JVM* jvm, JavaObject* obj);
int monitor_wait(JVM* jvm, JavaObject* obj, jlong timeout, bool timed);
int monitor_notify(JVM* jvm, JavaObject* obj);
int monitor_notify_all(JVM* jvm, JavaObject* obj);
JavaMonitor* monitor_get(JVM* jvm, JavaObject* obj);
JavaThread* monitor_get_owner(JavaObject* obj);
jint monitor_get_entry_count(JavaObject* obj);

/* v36.57 [PAINT-BUDGET]: ограниченное ожидание монитора для pump-потока
 * (см. блок-комментарий у реализации в threads.c). arm/disarm ставит
 * midp_process_repaints_impl вокруг вызова paint() ТОЛЬКО на потоке
 * фронтенда; monitor_enter между arm/disarm выходит с JNI_ERR по истечении
 * бюджета, вызывающая сторона читает jvm_paint_lock_aborted() и бросает
 * RuntimeException, paint() распаковывается (мониторы освобождаются
 * v34.99 MONITOR-UNWIND), главный цикл продолжает жить. */
void jvm_paint_lock_arm(uint32_t budget_ms);
void jvm_paint_lock_disarm(void);
int  jvm_paint_lock_aborted(void);
void jvm_paint_lock_reset(void);
/* v36.57 [MON-SITE]: сайт входа в monitor_enter для [MON-WAIT] (класс/метод
 * кадра вызывающей стороны) — пишут op_monitorenter и ACC_SYNC-вход
 * execute_method ДО monitor_enter. */
void jvm_mon_site_note(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

/*
 * Thread-local storage - simplified
 */
typedef jint ThreadLocalKey;

int thread_local_create(ThreadLocalKey* key);
int thread_local_set(ThreadLocalKey key, void* value);
void* thread_local_get(ThreadLocalKey key);
void thread_local_delete(ThreadLocalKey key);

/*
 * Atomic operations - simplified without actual atomics
 */

bool atomic_cas_int(volatile jint* ptr, jint expected, jint newval);
bool atomic_cas_long(volatile jlong* ptr, jlong expected, jlong newval);
bool atomic_cas_ptr(void* volatile* ptr, void* expected, void* newval);
jint atomic_increment(volatile jint* ptr);
jint atomic_decrement(volatile jint* ptr);
jint atomic_add(volatile jint* ptr, jint value);
void memory_barrier(void);

/*
 * Lock-free data structures
 */

typedef struct LFQueueNode {
    void* data;
    struct LFQueueNode* next;
} LFQueueNode;

typedef struct {
    LFQueueNode* head;
    LFQueueNode* tail;
} LFQueue;

void lfqueue_init(LFQueue* queue);
int lfqueue_push(LFQueue* queue, void* data);
void* lfqueue_pop(LFQueue* queue);
bool lfqueue_empty(LFQueue* queue);
void lfqueue_destroy(LFQueue* queue);

/*
 * Cleanup functions
 */

/* Destroy all monitor resources (pthread mutex/cond) - call on JVM shutdown */
void cleanup_monitors(void);

#endif /* THREADS_H */

/* v34.9 GC safepoint API (see threads.c) */
extern volatile int g_gc_safepoint_request;
void jvm_gc_safepoint_park(void);
int  jvm_gc_safepoint_arrived(void);
void jvm_gc_safepoint_release(void);
/* v34.26 PERF: event-driven wait used by gc_collect() — wakes on the last
 * mutator's arrival broadcast instead of polling in 5ms sleep slices. */
int  jvm_gc_safepoint_wait_arrivals(int expected, int timeout_ms);
int jvm_live_vm_thread_count(void);
/* v34.26: 1 if the CALLING OS thread is one of the pthread/CreateThread VM
 * runners (native.c registry). gc_collect() uses this to compute the exact
 * safepoint census when triggered from the frontend's retro_run thread. */
int jvm_current_os_thread_is_vm_runner(void);

/* v34.59 PERF (3D GC-пауза): событийное пробуждение СПЯЩИХ потоков при
 * запросе stop-the-world. Thread.sleep() игр спал 50-мс кусками nanosleep'ом
 * и проверял g_gc_safepoint_request ТОЛЬКО на границе куска — каждый GC
 * (в т.ч. System.gc() из игрового цикла, fmx: ~40 мс) ждал «застревателя»
 * до 50 мс (sp_wait в [GCSTAMP]). Теперь спящие ждут на глобальном
 * cond, который коллектор broadcasts при постановке запроса: поток
 * просыпается МГНОВЕННО и запаркивается.
 *   jvm_gc_safepoint_wake_sleepers() — broadcast (вызывает gc_collect).
 *   jvm_sleep_chunk_ms(chunk)       — спать chunk мс, но проснуться при
 *                                     broadcast; возвращает 1, если разбужен
 *                                     (возможно ложное срабатывание —
 *                                     безвредно: цикл сна перепроверит
 *                                     время/флаг). */
void jvm_gc_safepoint_wake_sleepers(void);
int  jvm_sleep_chunk_ms(long chunk_ms);
