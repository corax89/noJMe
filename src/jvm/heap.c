/*
 * J2ME Emulator - Heap and Garbage Collector (Fixed)
 * Mark-Sweep with free list, Coalescing, and 32-bit alignment support
 *
 * CRITICAL FIX: Added thread synchronization for heap operations.
 * Without this, multi-threaded access causes memory corruption on 32-bit Windows.
 *
 * DEBUG HEAP CORRUPTION:
 *   Define DEBUG_HEAP_CORRUPTION=1 to enable detailed logging to heap_debug.log
 *   This helps track down memory corruption issues.
 *
 * GC DEBUG LOGGING:
 *   Set environment variable J2ME_GC_DEBUG=1 to enable GC debug output to stderr
 *   All messages starting with [GC_ are hidden by default.
 */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L  /* For posix_memalign */
#endif

/* Enable detailed heap corruption debugging */
#ifndef DEBUG_HEAP_CORRUPTION
#define DEBUG_HEAP_CORRUPTION 0
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#ifdef MEM_FREE
#undef MEM_FREE
#endif
#else
#include <pthread.h>
#include <sched.h>  /* For sched_yield() */
#include <time.h>    /* v34.9: nanosleep for safepoint wait */
#endif

/* v34.9 GC safepoint API (defined in threads.c, declared in
 * include/threads.h). v34.10: these externs moved OUT of the POSIX-only
 * branch — the Windows build runs VM threads as native threads too, so
 * gc_collect() must request a stop-the-world there as well, and the
 * heap_lock ticket wait loop polls the same request. */
extern volatile int g_gc_safepoint_request;
void jvm_gc_safepoint_park(void);

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "heap.h"
#include "jvm.h"
#include "opcodes.h"
#include "classfile.h"
#include "debug.h"
#include "debug_macros.h"
#include "native.h"  /* For string field slot accessors */
#include "midp.h"    /* For M3G registry GC root support */
#include "threads.h" /* v34.71: jvm_os_thread_is_main_java for the TLAB gate */

/* v34 FIX (VmTest false "UNCAUGHT EXCEPTION"): heap-full is NOT a fatal error.
 * Per JVM spec an allocation failure is delivered to Java as a NORMAL catchable
 * OutOfMemoryError (MemTests/MemBudget in VmTest and many real games probe the
 * heap by allocating until OOME and catching it). The old code called
 * sdl_set_error_info() here, which printed a misleading "[J2ME UNCAUGHT
 * EXCEPTION]" banner and set sdl_has_error() -> the headless loop aborted the
 * run mid-test and libretro/SDL stuck a permanent error screen over a running
 * game that had legitimately recovered. Fatal-OOM reporting now happens ONLY
 * at the MIDlet-death layer (main.c / libretro.c uncaught-exception handlers),
 * which read detailMessage from the actually-thrown exception.
 * These globals keep the sizes of the LAST failed allocation so
 * native_throw_oome() can build the same diagnostic detailMessage. */
static size_t g_last_oom_requested = 0;
static size_t g_last_oom_available = 0;

void heap_get_last_oom_info(size_t* requested, size_t* available) {
    if (requested)  *requested  = g_last_oom_requested;
    if (available)  *available  = g_last_oom_available;
}

/* ============================================================
 * GC DEBUG LOGGING CONTROL
 * ============================================================
 * By default, all [GC_* debug messages are hidden.
 * To enable them, set environment variable: J2ME_GC_DEBUG=1
 * 
 * Example: J2ME_GC_DEBUG=1 ./j2me-emulator game.jar
 */
static int gc_debug_enabled_cache = -1;  /* -1 = not checked, 0 = disabled, 1 = enabled */

static int gc_debug_enabled(void) {
    if (gc_debug_enabled_cache == -1) {
        const char* env = getenv("J2ME_GC_DEBUG");
        gc_debug_enabled_cache = (env != NULL && (env[0] == '1' || env[0] == 'y' || env[0] == 'Y'));
    }
    return gc_debug_enabled_cache;
}

/* Macro for GC debug logging - only prints if J2ME_GC_DEBUG=1 */
#define GC_LOG(fmt, ...) do { \
    if (gc_debug_enabled()) { \
        LOG_SAFE(fmt, ##__VA_ARGS__); \
    } \
} while(0)

/* Macro for GC debug logging without newline (for partial lines) */
#define GC_LOG_PARTIAL(fmt, ...) do { \
    if (gc_debug_enabled()) { \
        LOG_SAFE(fmt, ##__VA_ARGS__); \
    } \
} while(0)

/* ============================================================
 * NATIVE HASHTABLE SUPPORT FOR GC
 * ============================================================
 * The Hashtable implementation uses native memory (g_hashtables)
 * to store key-value pairs. The GC needs to mark these references
 * because they're not stored in Java heap objects.
 * 
 * HashtableEntry: stores key (JavaObject*), value (JavaObject*), hash
 * HashtablePeer: stores entries array, capacity, count
 * g_hashtables[]: global array of HashtablePeer structures
 */
#define GC_HASHTABLE_MAX_HASHTABLES 4096

/* These must match the types in native.c */
typedef struct {
    JavaObject* key;
    JavaObject* value;
    jint hash;
} HashtableEntryGC;

typedef struct {
    JavaObject* ht_obj;     /* Back-reference to Java Hashtable object - MUST MATCH native.c! */
    HashtableEntryGC* entries;
    int capacity;
    int count;
} HashtablePeerGC;

/* External references to native Hashtable storage in native.c
 * Note: The struct types are named differently (HashtablePeer vs HashtablePeerGC)
 * but have identical memory layout, so the extern declarations work correctly.
 */
extern HashtablePeerGC g_hashtables[GC_HASHTABLE_MAX_HASHTABLES];
extern int g_hashtable_count;
extern bool g_hashtable_peer_alive[GC_HASHTABLE_MAX_HASHTABLES];

/* Function to free native peer - implemented in native.c */
extern void hashtable_free_peer(int idx);

/* ============================================================
 * HEAP CORRUPTION DEBUG LOGGING
 * ============================================================
 * When DEBUG_HEAP_CORRUPTION is enabled, all heap operations
 * are logged to heap_debug.log for post-mortem analysis.
 * This helps identify the source of memory corruption.
 */
#if DEBUG_HEAP_CORRUPTION
static FILE* volatile heap_log = NULL;
static volatile int heap_log_initialized = 0;

static void heap_log_init(void) {
    /* CRITICAL FIX: Thread-safe initialization using CAS */
    int expected = 0;
    if (__sync_bool_compare_and_swap(&heap_log_initialized, expected, 1)) {
        /* We won the race - open the log file */
        FILE* f = fopen("heap_debug.log", "w");
        if (!f) {
            f = stderr;
        }
        /* Memory barrier to ensure file is opened before other threads see it */
#ifdef _WIN32
        MemoryBarrier();
#else
        __sync_synchronize();
#endif
        heap_log = f;
        /* Mark as fully initialized */
        heap_log_initialized = 2;
    } else {
        /* Another thread is initializing - wait until complete */
        while (heap_log_initialized != 2) {
#ifdef _WIN32
            Sleep(0);
#else
            sched_yield();
#endif
        }
    }
}
static void heap_log_close(void) {
    if (heap_log && heap_log != stderr) {
        fclose((FILE*)heap_log);  /* Cast to non-volatile for fclose */
        heap_log = NULL;
    }
    heap_log_initialized = 0;
}
#define HEAP_CORRUPTION_LOG(fmt, ...) do { \
    if (!heap_log) heap_log_init(); \
    fprintf((FILE*)heap_log, "[HEAP_CORRUPTION] " fmt "\n", ##__VA_ARGS__); \
    fflush((FILE*)heap_log); \
} while(0)
#else
#define HEAP_CORRUPTION_LOG(fmt, ...) ((void)0)
#endif

/* Global JVM pointer for object_instance_of (set during JVM initialization) */
JVM* g_jvm_for_instanceof = NULL;

/* Global counter for strings fixed during GC (reset each cycle) */
static int gc_strings_fixed_this_cycle = 0;
/* v34.80: recycled headers neutralized by the gc_mark_object class-guard
 * (see the OBJ_TYPE_OBJECT case) — diagnostic counter, same style as
 * gc_strings_fixed_this_cycle; racily read by telemetry. */
static int gc_recycled_headers_skipped = 0;

/* Выравнивание объектов. 
 * На 32-битных системах double/long требуют выравнивания по 8 байт. 
 * Поэтому используем 8 для совместимости. */
#ifndef OBJECT_ALIGNMENT
#define OBJECT_ALIGNMENT 8
#endif

/* FreeBlock is now defined in heap.h for binary compatibility with GCObjectHeader */

/* ============================================================
 * CRITICAL: Heap mutex for thread synchronization
 * Without this, multi-threaded allocation causes memory corruption
 * 
 * NOTE: We use a RECURSIVE mutex because gc_collect() can be
 * called from inside heap_alloc(), and both need the lock.
 * ============================================================ */
#ifdef _WIN32
static CRITICAL_SECTION heap_mutex;
static volatile LONG heap_mutex_initialized = 0;  /* Use LONG for InterlockedCompareExchange */
#else
static pthread_mutex_t heap_mutex;
static pthread_mutexattr_t heap_mutex_attr;
#endif

/* Atomic flag for mutex initialization - used with barrier pattern */
static volatile int heap_mutex_init_done = 0;
static volatile int heap_mutex_ready = 0;  /* Set AFTER initialization complete */

/*
 * CRITICAL FIX: Cross-platform atomic compare-and-swap
 * Supports both GCC/Clang and MSVC compilers
 */
#ifdef _WIN32
/* MSVC: Use InterlockedCompareExchange */
#define ATOMIC_CAS(ptr, expected, desired) \
    (InterlockedCompareExchange((LONG*)(ptr), (desired), (expected)) == (expected))
#else
/* GCC/Clang: Use __sync_bool_compare_and_swap */
#define ATOMIC_CAS(ptr, expected, desired) \
    __sync_bool_compare_and_swap((ptr), (expected), (desired))
#endif

/* Initialize heap mutex (call once, thread-safe) */
static void heap_mutex_init(void) {
    int expected = 0;
    if (ATOMIC_CAS(&heap_mutex_init_done, expected, 1)) {
        /* We won the race - initialize the mutex */
#ifdef _WIN32
        /* CRITICAL_SECTION is recursive by default on Windows */
        InitializeCriticalSection(&heap_mutex);
#else
        /* POSIX: Create a RECURSIVE mutex */
        pthread_mutexattr_init(&heap_mutex_attr);
        pthread_mutexattr_settype(&heap_mutex_attr, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&heap_mutex, &heap_mutex_attr);
#endif
        
        /* CRITICAL FIX: Memory barrier to ensure mutex initialization
         * is visible to other threads BEFORE they can use it.
         * Without this, Thread B might see heap_mutex_init_done=1 but
         * try to enter an uninitialized critical section.
         */
#ifdef _WIN32
        MemoryBarrier();  /* Windows memory barrier */
#else
        __sync_synchronize();  /* GCC/Clang full memory barrier */
#endif
        
        /* Now mark as ready - other threads can proceed */
        heap_mutex_ready = 1;
    } else {
        /* Another thread is initializing - wait until ready */
        while (!heap_mutex_ready) {
#ifdef _WIN32
            Sleep(0);  /* Yield to other threads */
#else
            sched_yield();  /* POSIX yield */
#endif
        }
    }
}

/* ============ v34.9 HEAP LOCK: TICKET (STRICTLY FAIR) ============
 * The heap mutex is a recursive primitive (PTHREAD_MUTEX_RECURSIVE on
 * POSIX, CRITICAL_SECTION on Windows). Under contention between a
 * spinning thread (the Bounce Tales loader choreography calls System.gc()
 * in a busy loop) and a single-shot acquirer (the loader thread inside
 * GameCanvas.getGraphics()), the plain mutex let the spinner re-acquire
 * forever (barge) and starved the loader -> white loading screen hang
 * after "New game". A ticket queue makes acquisition strictly FIFO:
 * every caller takes a ticket and only the serving ticket may touch the
 * mutex. Re-entrant (recursive) callers bypass the ticket queue.
 * The GC safepoint is honored while waiting for a ticket.
 *
 * v34.10: ONE implementation for POSIX and Windows. v34.9 shipped the
 * ticket queue only in the POSIX branch of heap_lock/heap_unlock, so the
 * Windows libretro build kept the unfair plain mutex and was still
 * exposed to the same loader starvation. Thread identity is abstracted
 * through heap_native_tid_t (pthread_t vs GetCurrentThreadId()). */
static volatile int heap_ticket_serving = 0;
static volatile int heap_ticket_next = 0;

/* Native thread identity for the recursive fast-path */
#ifdef _WIN32
typedef DWORD heap_native_tid_t;
#define HEAP_TID_SELF()       GetCurrentThreadId()
#define HEAP_TID_EQUAL(a, b)  ((a) == (b))
#define HEAP_YIELD()          SwitchToThread()
#else
typedef pthread_t heap_native_tid_t;
#define HEAP_TID_SELF()       pthread_self()
#define HEAP_TID_EQUAL(a, b)  pthread_equal((a), (b))
#define HEAP_YIELD()          sched_yield()
#endif

static volatile heap_native_tid_t g_heap_owner_native;  /* primitive-mutex owner */
static volatile int g_heap_owner_valid = 0;
static volatile int g_heap_owner_tid = -1;              /* JavaThread id (diag) */
static volatile int g_heap_lock_stall_reported = 0;
static volatile int g_heap_recursion_count = 0;   /* v34.9: inner (re-entrant) lock depth */

static inline void heap_prim_lock(void) {
#ifdef _WIN32
    EnterCriticalSection(&heap_mutex);
#else
    pthread_mutex_lock(&heap_mutex);
#endif
}
static inline void heap_prim_unlock(void) {
#ifdef _WIN32
    LeaveCriticalSection(&heap_mutex);
#else
    pthread_mutex_unlock(&heap_mutex);
#endif
}

static inline void heap_lock(void) {
    heap_mutex_init();
    {
        heap_native_tid_t self = HEAP_TID_SELF();
        /* Recursive fast-path: we already own the primitive mutex (recursive
         * on both platforms: PTHREAD_MUTEX_RECURSIVE / CRITICAL_SECTION).
         * The matching unlock must NOT advance the ticket queue
         * (v34.9: a lost ticket deadlock). */
        if (g_heap_owner_valid && HEAP_TID_EQUAL(g_heap_owner_native, self)) {
            heap_prim_lock();
            g_heap_recursion_count++;
            return;
        }
        {
            int my = __sync_fetch_and_add(&heap_ticket_next, 1);
            int spins = 0;
            while (heap_ticket_serving != my) {
                if (g_gc_safepoint_request) jvm_gc_safepoint_park();
                HEAP_YIELD();
                /* v34.96: the v34.9 report was ONE-SHOT per process
                 * (g_heap_lock_stall_reported latched) — a SECOND stall in
                 * the same session was silent, and the v34.95 freeze trace
                 * provably lacked the report although a ticket stall was a
                 * candidate. Report the FIRST stall, then re-report every
                 * 5 s while the stall persists, now including next= (the
                 * queued ticket count) — a serving that never advances
                 * across reports is a dead ticket queue, an advancing one
                 * is just heavy contention. */
                if (++spins >= 50000) {
                    static volatile long long s_last_rep_ms = 0;
#ifdef _WIN32
                    long long rep_now = (long long)GetTickCount64();
#else
                    struct timespec rep_ts;
                    clock_gettime(CLOCK_MONOTONIC, &rep_ts);
                    long long rep_now =
                        (long long)rep_ts.tv_sec * 1000LL + rep_ts.tv_nsec / 1000000LL;
#endif
                    if (!g_heap_lock_stall_reported ||
                        rep_now - s_last_rep_ms >= 5000) {
                        g_heap_lock_stall_reported = 1;
                        s_last_rep_ms = rep_now;
                        extern JavaThread* thread_current(JVM*);
                        JavaThread* t = thread_current(NULL);
                        LOG_SAFE("[HEAPDIAG] TICKET STALL: my=%d serving=%d next=%d "
                                 "owner_tid=%d owner_valid=%d waiter_tid=%d\n",
                                 my, heap_ticket_serving, heap_ticket_next,
                                 g_heap_owner_tid,
                                 g_heap_owner_valid, t ? t->id : -1);
                    }
                    spins = 0;
                }
            }
            heap_prim_lock();
        }
        {
            extern JavaThread* thread_current(JVM*);
            JavaThread* t = thread_current(NULL);
            g_heap_owner_tid = t ? t->id : -1;
        }
        g_heap_owner_native = self;
        g_heap_owner_valid = 1;
    }
}

static inline void heap_unlock(void) {
    if (g_heap_recursion_count > 0) {
        /* Inner (re-entrant) release: the primitive mutex is still held by
         * the outer owner — do NOT pass the ticket. */
        g_heap_recursion_count--;
        heap_prim_unlock();
        return;
    }
    g_heap_owner_valid = 0;
    g_heap_owner_tid = -1;
    /* v34.11: advance the ticket BEFORE releasing the primitive mutex.
     * Advancing it after the unlock let the next-ticket thread acquire the
     * primitive mutex and observe a stale serving pointer (harmless spin,
     * but it also allowed an out-of-order mutex handoff window). */
    heap_ticket_serving++;
    heap_prim_unlock();
}

/* v19: Emergency reserve. When the Java heap is FULL, the VM must still be
 * able to allocate the OutOfMemoryError (or any other Throwable) object so
 * the MIDlet can CATCH it (J2ME spec: OOME is a normal Throwable). Without
 * the reserve the exception object allocation itself fails, no exception is
 * pending, and interpret() treats the opcode failure as fatal -> VM stops.
 * The reserve sits BEYOND heap.end and is only used by *_emergency allocs. */
#define HEAP_EMERGENCY_RESERVE (256 * 1024)

/* ============================================================
 * v34.58 PERF: SIZE-CLASS ALLOCATOR (segregated free lists)
 * ============================================================
 * Раньше try_alloc_from_free_list() делал first-fit по ОДНОМУ
 * линейному списку всех свободных блоков. На фрагментированной
 * куче (стринг-churn игр даёт 100k+ блоков) каждый miss — O(n)
 * pointer chases. Теперь:
 *
 *  - Блоки <= SC_SMALL_MAX (1024 байт) раскладываются по
 *    SC_NUM_BUCKETS бакетам: LIFO-пуш при освобождении/split,
 *    pop головы при аллокации — O(1) в общем случае.
 *  - Блоки > 1024 байт живут в одном large-списке (короткий:
 *    крупные буферы игр редки), first-fit по нему остаётся
 *    O(крупные блоки).
 *  - Поиск: сначала СВОЙ бакет, затем бакеты постарше, затем
 *    large-список. Это best-fit-by-class: меньше фрагментации,
 *    чем чистый first-fit по адресам.
 *
 * sc_lut[] — прямая таблица «размер>>3 -> индекс бакета» для всех
 * 8-байтовых шагов до 1024, строится в heap_init (зависит от
 * sizeof(GCObjectHeader) — минимального возможного total_size).
 *
 * ИНВАРИАНТЫ, которые обязан сохранять каждый путь:
 *  1) Каждый свободный регион начинается с валидного
 *     GCObjectHeader/FreeBlock (size по общему смещению,
 *     type=OBJ_TYPE_FREE) — линейные обходы (sweep, presweep,
 *     heap_validate, heap_check_magic) прыгают по size целыми
 *     регионами и не должны спотыкаться о мусор в середине.
 *  2) Блок, взятый из бакета и разрезанный, даёт remainder,
 *     который кладётся в бакет СВОЕГО размера (не в голову
 *     чужого списка) — так мелочь не накапливается.
 *  3) Sweep-фаза rebuild'ит ВСЕ бакеты с нуля (самоочистка от
 *     любого рассинхрона) и заодно coalesce'ит соседние свободные
 *     блоки одним проходом (адреса растут — слияние на лету).
 *     Попутно устранён старый дефект: блоки, уже свободные ДО
 *     этого GC, теперь заново попадают в новые списки (раньше
 *     они просто выпадали из учёта — емкость кучи деградировала).
 * ============================================================ */
#define SC_NUM_BUCKETS 18
static const uint32_t sc_bucket_limit[SC_NUM_BUCKETS] = {
    48,  56,  64,  72,  80,  96,  112, 128, 160, 192,
    224, 256, 320, 384, 512, 640, 768, 1024
};
#define SC_SMALL_MAX  1024
#define SC_LARGE_IDX  SC_NUM_BUCKETS          /* виртуальный индекс large-списка */
#define SC_LUT_SIZE   ((SC_SMALL_MAX >> 3) + 1)
static uint8_t sc_lut[SC_LUT_SIZE];          /* размер>>3 -> бакет */

/* Быстрое определение бакета по total_size (выравнивание 8 гарантировано). */
static inline int size_class_idx(size_t total_size) {
    if (total_size <= SC_SMALL_MAX) return sc_lut[total_size >> 3];
    return SC_LARGE_IDX;
}

/* Построение LUT: для каждого 8-байтового размера — индекс первого
 * бакета, чей limit >= размер. Бакеты упорядочены по возрастанию. */
static void sc_build_lut(void) {
    int b = 0;
    for (size_t i = 0; i < SC_LUT_SIZE; i++) {
        size_t bytes = i << 3;
        while (b + 1 < SC_NUM_BUCKETS && bytes > sc_bucket_limit[b]) b++;
        sc_lut[i] = (uint8_t)b;
    }
}

/* ============================================================
 * v34.59 PERF: TLAB — ПОТОКОВО-ЛОКАЛЬНЫЕ БУФЕРЫ АЛЛОКАЦИИ
 * ============================================================
 * Профиль 3D-игр (Asphalt 3 3D / M3G, см. execute.c v41: ~400 МБ/с
 * мусора): каждый кадр рождает СОТНИ мелких объектов и массивов —
 * векторы координат, float[16] матрицы, boxed-числа. После v34.58
 * (size-классы, O(1)-pop) на одну мелкую аллокацию приходится:
 * ticket-lock (fetch_and_add + ожидание билета + pthread_mutex) +
 * pop из бакета + memset + инициализация заголовка — 100+ тактов И
 * глобальная точка сериализации на КАЖДУЮ мелочь. Многопоточные
 * игры (game thread + render/timer) платят ещё и кохерентностью
 * кэш-линий ticket-счётчиков.
 *
 * TLAB: каждый поток РАЗ в ~350-500 мелких аллокаций (под
 * heap_lock) вырезает себе чанк 16 КБ (сперва из large-списка —
 * переиспользование памяти, собранной GC; затем с топа кучи) и
 * дальше мелкие аллокации (total <= TLAB_MAX_TOTAL) бампаются в
 * чанке ВООБЩЕ без блокировок: ~10-15 тактов на аллокацию.
 *
 * ИНВАРИНТЫ СОВМЕСТИМОСТИ С MARK-SWEEP:
 *  1) Под-объекты чанка — ОБЫЧНЫЕ блоки с валидными GCObjectHeader
 *     (memset + все поля): линейный sweep/presweep ходят по ним как
 *     по обычным объектам; метки живости mark ставит по стекам/полям
 *     владельца — во время STW владелец чанка запаркован, гонок нет.
 *  2) Неиспользованный хвост чанка [top, limit) ВСЕГДА >= 40 байт
 *     (fast path не бампает, если не влезает объект + хвостовой
 *     заголовок; carve даёт чанк >= 16К, total <= 1024). Хвост
 *     закрывается валидным free-заголовком ТОЛЬКО в точках закрытия
 *     (исчерпание/refill, flush); между ними хвост недоступен никому:
 *     sweep/presweep идут строго под stop-the-world ПОСЛЕ flush'а
 *     всех чанков; heap_validate/check_magic в проде не вызываются
 *     (их debug-режимы J2ME_GC_DEBUG / DEBUG_HEAP_CORRUPTION полностью
 *     отключают TLAB — см. условия ниже).
 *  3) Flush (закрытие своего чанка) происходит:
 *       - в jvm_gc_safepoint_park() — ДО arrival-broadcast, т.е. до
 *         того как коллектор начнёт walk (threads.c);
 *       - при входе в gc_collect() — сам коллектор;
 *       - перед выходом pthread/win-раннера (native.c) — смерть
 *         потока с активным чанком.
 *     Остаток остаётся валидным свободным блоком ВНЕ списков —
 *     ближайший sweep втянет его в новые бакеты (v34.58: блоки,
 *     уже свободные до GC, участвуют в rebuild).
 *  4) heap.allocated ведётся атомарно на каждую TLAB-аллокацию;
 *     во время STW конкурентных TLAB-записей нет — вычеты sweep
 *     остаются точными, heap_get_stats() не врёт.
 *  5) safepoint_request читается в fast path ДО бампа: если GC
 *     уже запросил мир, мы уходим в медленный путь (парк там же).
 *     Коллектор физически не может быть в mark/sweep, пока владелец
 *     чанка не запарковался (wait_arrivals), а парк закрывает хвост.
 *  6) Отключение: NOJME_TLAB=0 (env), J2ME_GC_DEBUG=1,
 *     DEBUG_HEAP_CORRUPTION=1 (compile-time) — полный откат на
 *     поведение v34.58.
 * ============================================================ */
#define TLAB_CHUNK_SIZE  (16 * 1024)
#define TLAB_MAX_TOTAL   SC_SMALL_MAX      /* fast path только для total <= 1024 */

typedef struct {
    uint8_t* base;
    uint8_t* top;     /* следующая позиция bump */
    uint8_t* limit;   /* конец чанка */
} TLAB;
static __thread TLAB tlab;   /* нулевой по умолчанию: limit == NULL */

/* Диагностика (racy-инкременты — только для логов/бенчмарков). */
static volatile uint64_t g_tlab_carves = 0;
static volatile uint64_t g_tlab_fast_allocs = 0;
void heap_tlab_stats(uint64_t* carves, uint64_t* fast_allocs) {
    if (carves) *carves = g_tlab_carves;
    if (fast_allocs) *fast_allocs = g_tlab_fast_allocs;
}

/* Страховочный выключатель: NOJME_TLAB=0 отключает (кэш env, читается
 * только в медленном пути при refill — fast path лишь проверяет limit).
 * Отсутствие переменной/любое другое значение — включено. */
static int tlab_enabled_cache = -1;
static int tlab_enabled(void) {
    if (tlab_enabled_cache < 0) {
        const char* e = getenv("NOJME_TLAB");
        tlab_enabled_cache = (e && e[0] == '0') ? 0 : 1;
    }
    return tlab_enabled_cache;
}

/* Валидный free-заголовок, закрывающий хвост чанка [top, limit).
 * Вызывается только владельцем чанка (TLS) или под heap_lock
 * (refill); размер >= sizeof(GCObjectHeader) гарантирован инвариантом
 * №2 (проверяется вызывающими). */
static FILE* heap_diag_alloc_log(void);  /* v34.71 DIAG 2 (defined below) */
static void tlab_write_tail(uint8_t* top, uint8_t* limit) {
    GCObjectHeader* tail = (GCObjectHeader*)top;
    memset(tail, 0, sizeof(GCObjectHeader));
    tail->size  = (uint32_t)(limit - top);
    tail->type  = OBJ_TYPE_FREE;
    tail->magic = GC_HEADER_MAGIC;
    /* v34.71 DIAG 2: tail-write event for the alloc log */
    {
        FILE* alloc_log = heap_diag_alloc_log();
        if (alloc_log) {
            fprintf(alloc_log, "T %p %ld\n", (void*)top, (long)(limit - top));
        }
    }
}

/* Закрыть свой чанк (если активен): хвост — валидный свободный блок,
 * НЕ включённый в списки; sweep втянет его при ближайшем GC. Вызывается
 * БЕЗ heap_lock — трогает только TLS владельца и его чанк. Идемпотентно.
 * Должно быть вызвано ДО arrival-broadcast в jvm_gc_safepoint_park() и
 * при входе в gc_collect(). */
void heap_tlab_flush_self(void) {
    if (tlab.limit == NULL) return;
    if (tlab.top + sizeof(GCObjectHeader) <= tlab.limit) {
        tlab_write_tail(tlab.top, tlab.limit);
    }
    /* top + 40 > limit недостижимо: инвариант №2 (bump всегда оставляет
     * место под хвостовой заголовок); guard оставлен как страховка от
     * внешней порчи — в этом случае блок остаётся невалидным, но и не
     * попадает ни в какие списки. */
    tlab.base = tlab.top = tlab.limit = NULL;
}

/* Heap state */
static struct {
    uint8_t* start;
    uint8_t* current;   /* Bump pointer for new allocations */
    uint8_t* end;       /* Soft limit: normal allocations stop here */
    uint8_t* hard_end;  /* v19: start + size + HEAP_EMERGENCY_RESERVE */
    size_t size;
    size_t allocated;   /* Total bytes currently occupied by live objects */
    size_t freed;       /* Total historical freed bytes */
    
    /* Мы не используем linked list heap.objects для обхода GC, 
       так как линейный проход по памяти надежнее. 
       Но можно оставить для статистики. */

    /* v34.58 PERF (size-class allocator): segregated free lists.
     * БЫЛО: один глобальный free_list, first-fit по линейному списку —
     * O(n) на каждый miss при 100k+ свободных блоках (v34.15 отмечал
     * O(n^2) в sweep; first-fit в alloc страдал тем же).
     * СТАЛО: 18 size-class бакетов для блоков <= 1024 байт (LIFO,
     * head-pop O(1) в общем случае) + один large-список для блокков
     * > 1024 байт (короткий: крупные буферы игр редки). Sweep-фаза
     * rebuild'ит все бакеты с on-the-fly coalescing (см. gc_collect).
     * Остаток (remainder) от split'а уходит в бакет СВОЕГО размера,
     * а не в голову общего списка — упорядоченность больше не
     * разрушается, мелкие блоки не скапливаются перед крупными. */
    FreeBlock* free_lists[SC_NUM_BUCKETS];  /* small buckets (<= 1024) */
    FreeBlock* large_free_list;             /* blocks > 1024, address-ordered by sweep */

    void*** roots;
    size_t root_count;
    size_t root_capacity;
    
    /* GC statistics */
    size_t gc_cycles;
    size_t gc_total_freed;
} heap;

/* Global heap bounds for validation */
void* g_heap_start = NULL;
void* g_heap_end = NULL;

/* v34.59: верхняя граница занятой области кучи (для диагностических
 * обходов: после top-rewind current может опускаться ниже high-water
 * mark аллокаций — обход выше current читает девственную память). */
void* heap_top_ptr(void) { return heap.current; }

/* v34.94 DIAG: arena bump offset in bytes for the frontend diag line
 * ("top=") — the monotonic per-session allocation watermark the user's
 * v34.93 log tracked. Weak fallback in switch_trace.c returns 0 when the
 * heap module is not linked. */
size_t heap_arena_top_bytes(void) {
    return (heap.current && heap.start && heap.current >= heap.start)
        ? (size_t)((uint8_t*)heap.current - (uint8_t*)heap.start) : 0;
}

/* v34.71 DIAG 2: ONE shared unbuffered handle for the allocation log —
 * separate stdio buffers on the same path interleave chunks out of order
 * and made the first forensic attempt unreadable. Opened lazily once. */
static FILE* heap_diag_alloc_log(void) {
    static FILE* log = NULL;
    static int checked = 0;
    if (!checked) {
        checked = 1;
        const char* p = getenv("NOJME_ALLOC_LOG");
        if (p && p[0]) {
            log = fopen(p, "a");
            if (log) setvbuf(log, NULL, _IONBF, 0);
        }
    }
    return log;
}

/* Положить свободный блок в его бакет (LIFO). Возвращает индекс бакета.
 * Вызывается только под heap_lock (alloc slow path / sweep rebuild). */
static int sc_push_free(FreeBlock* fb) {
    int idx;
    if (fb->size > SC_SMALL_MAX) {
        fb->next = heap.large_free_list;
        heap.large_free_list = fb;
        idx = SC_LARGE_IDX;
    } else {
        idx = size_class_idx((size_t)fb->size);
        fb->next = heap.free_lists[idx];
        heap.free_lists[idx] = fb;
    }
    /* v34.71 DIAG 2: push event for the alloc log */
    {
        FILE* alloc_log = heap_diag_alloc_log();
        if (alloc_log) {
            fprintf(alloc_log, "P %p %u %d\n", (void*)fb, fb->size, idx);
        }
    }
    return idx;
}

/* v34.43 PERF-DIAG: total gc_collect() invocations (racy read is fine —
 * used only by the SLOWFRAME telemetry to attribute frame stalls to GC). */
uint64_t g_gc_collections = 0;

/* v35.03: GC pause accounting for the diag line + [GC-SLOW] watchdog.
 * The "1 fps after a lap" field report needs a GC-storm verdict from the
 * log: gms= last pause ms, gxm= worst pause since the last diag tick
 * (reset by diag_emit), gte= monotonic ms stamp of the last GC end (the
 * trace beat checks recency without reaching into GC internals). */
volatile uint32_t g_gc_last_ms = 0;
volatile uint32_t g_gc_worst_ms = 0;
volatile long long g_gc_last_end_ms = 0;

static void gc_pause_account(const struct timespec* t0) {
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long dur_us = (t1.tv_sec - t0->tv_sec) * 1000000L +
                  (t1.tv_nsec - t0->tv_nsec) / 1000;
    uint32_t ms = (uint32_t)(dur_us / 1000);
    g_gc_last_ms = ms;
    if (ms > g_gc_worst_ms) g_gc_worst_ms = ms;
    g_gc_last_end_ms = (long long)t1.tv_sec * 1000LL + t1.tv_nsec / 1000000LL;
}

/* v34.95 SAFEPOINT-BACKOFF: monotonic-ms deadline until which gc_collect()
 * declines to run. Set after a safepoint-timeout abort: the straggler
 * (a VM thread stuck inside a long native that never polls) makes every
 * collection burn its full 2000 ms stop-the-world wait and abort — with
 * an allocation-triggered collector that degenerates into a livelock
 * (each allocation = 2 s world stop; the frontend's paint() stuck in the
 * heap-lock queue is exactly the v34.94 "STUCK: stage=repaints" report).
 * During the window allocations fall through to the catchable
 * OutOfMemoryError path instead; once the straggler reaches a poll the
 * next collection succeeds normally. */
volatile jlong g_gc_backoff_until_ms = 0;

/* ========================================================================
 * v35.04 PERIODIC GC: allocation-budget trigger.
 *
 * FIELD EVIDENCE (v35.03 race trace, 197 s): gc=77..182 during the first
 * ~15 s (class loading garbage), then gc=0 for the ENTIRE race while the
 * heap grew monotonically 1.9 -> 6.8 MB (~150 KB/s of garbage). By design
 * this collector only runs when the free list AND the bump top are
 * exhausted — with a 64 MB heap that means the first gameplay collection
 * happens ~7 minutes in, over ~60 MB of accumulated garbage: a multi-
 * second stop-the-world pause (the "sudden 1 fps after a lap" storm
 * v35.03 built [GC-SLOW] to catch) and, on longer sessions, a real OOM —
 * a J2ME phone with a 1-4 MB heap survives this game, so the 150 KB/s is
 * GARBAGE, not live data, and periodic collection keeps the heap small.
 *
 * Trigger: when the bytes allocated since the last successful collection
 * exceed the budget (default 4 MB, env NOJME_GC_BUDGET_MB, 0 = disable)
 * AND at least 3 s passed since the previous periodic attempt, run a
 * normal collection from the next allocation. Mark/sweep over a small
 * live set costs single-digit ms (loading-phase GCs measured gms=1..2 in
 * the field trace), so a once-per-~30 s pause at 150 KB/s is invisible.
 * ======================================================================== */
static volatile size_t g_gc_alloc_at_last_gc = 0; /* heap.allocated snapshot */
static volatile long long g_gc_periodic_last_ms = 0;
#define NOJME_GC_PERIODIC_MIN_MS 3000

static size_t gc_periodic_budget(void) {
    static size_t budget = 0;
    static int env_read = 0;
    if (!env_read) {
        env_read = 1;
        budget = (size_t)4 << 20; /* 4 MB default */
        const char* e = getenv("NOJME_GC_BUDGET_MB");
        if (e && e[0]) {
            long v = atol(e);
            if (v > 0) budget = (size_t)v << 20;
            else budget = 0;      /* 0 disables the periodic trigger */
        }
    }
    return budget;
}

/* Cheap predicate: allocation budget reached AND the per-session rate
 * limiter allows another attempt. Sets the attempt stamp so concurrent
 * mutators do not stampede (a losing racer no-ops inside gc_collect via
 * gc_in_progress anyway). Racy reads of heap.allocated are fine: the
 * worst case is one 3 s-late or one aggregated collection. */
static int gc_periodic_due(void) {
    size_t budget = gc_periodic_budget();
    if (!budget) return 0;
    size_t alloc = heap.allocated, base = g_gc_alloc_at_last_gc;
    if (alloc < base || alloc - base < budget) return 0;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long long now = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    long long last = g_gc_periodic_last_ms;
    if (last && now - last < NOJME_GC_PERIODIC_MIN_MS) return 0;
    g_gc_periodic_last_ms = now;
    return 1;
}

/* v50 DIAG (Asphalt 3 3D stall): wall ms the LAST collection spent waiting
 * for VM threads to park (0 when the world stopped instantly). Printed by
 * [GCSTAMP] as sp_wait= — a large value pins the stall on a straggler
 * thread executing a long native instead of on mark/sweep itself. */
double g_gc_last_sp_wait_ms = 0.0;

/* Forward declarations */
static void gc_mark_object(void* ptr);
static void* try_alloc_from_free_list(size_t total_size, size_t* actual_size);

/* Проверка, является ли указатель частью кучи
 * v19: include the emergency reserve (hard_end). Exception objects allocated
 * there are reachable through thread stacks / pending_exception and MUST
 * validate as heap pointers for GC marking and heap_java_object_valid().
 * v36.38: parameter is now const void* — callers holding const pointers
 * (e.g. string_chars_null_trace) no longer discard the qualifier
 * (-Wdiscarded-qualifiers under -Wall -Wextra). */
static inline bool is_heap_ptr(const void* ptr) {
    return (const void*)ptr >= (const void*)heap.start &&
           (const void*)ptr <  (const void*)heap.hard_end;
}

/* =====================================================================
 * v34.14: NATIVE-SCOPE AUTO-PINNING (аналог JNI local references)
 *
 * Проблема (Nescube SIGSEGV): нативные методы (M3G Loader.load, медиа,
 * RMS, PNG-декодеры...) аллоцируют Java-объекты (Image2D, пиксельные
 * массивы, строки) и держат их ТОЛЬКО в C-локалах. Если аллокация
 * внутри натива сама триггерит gc_collect(), mark-фаза не видит ни
 * одной Java-ссылки на эти объекты (Java-фреймов ещё нет), sweep их
 * освобождает, heap-rewind затирает память — и натив возвращает
 * мёртвый указатель («field lookup on DEAD object» + SIGSEGV).
 *
 * Фикс: пока на потоке исполняется натив (TLS g_gc_in_native > 0),
 * каждая кучная аллокация рождается PINNED и записывается в
 * per-thread (TLS) auto-pin журнал. При выходе из натива
 * gc_autopin_release(base) снимает pin со всех объектов, записанных
 * после base. Вложенность диспетчей поддерживается: каждый уровень
 * запоминает свой base. Pinned-объекты, кроме того, останавливают
 * heap-top rewind — их память не может быть переиспользована, пока
 * натив их держит.
 * ===================================================================== */
__thread int g_gc_in_native = 0;

__thread void** g_gc_autopin_log = NULL;
__thread size_t g_gc_autopin_len = 0;
__thread size_t g_gc_autopin_cap = 0;

size_t gc_autopin_base(void) { return g_gc_autopin_len; }

void gc_autopin_note(void* obj) {
    if (!g_gc_in_native) return;  /* обычная интерпретаторная аллокация */
    if (g_gc_autopin_len >= g_gc_autopin_cap) {
        size_t ncap = g_gc_autopin_cap ? g_gc_autopin_cap * 2 : 64;
        void** nlog = (void**)realloc(g_gc_autopin_log, ncap * sizeof(void*));
        if (!nlog) return;  /* нет нативной памяти: объект останется unpinned */
        g_gc_autopin_log = nlog;
        g_gc_autopin_cap = ncap;
    }
    g_gc_autopin_log[g_gc_autopin_len++] = obj;
}

void gc_autopin_release(size_t base) {
    if (base > g_gc_autopin_len) base = g_gc_autopin_len;
    for (size_t i = base; i < g_gc_autopin_len; i++) {
        void* o = g_gc_autopin_log[i];
        if (o && is_heap_ptr(o)) {
            GCObjectHeader* h = (GCObjectHeader*)o - 1;
            if (h->magic == GC_HEADER_MAGIC) h->pinned = 0;
        }
    }
    g_gc_autopin_len = base;
}

/* v34.59 TLAB: общая инициализация выделенного блока — memset (Java
 * zero-init!) + все поля GCObjectHeader + ObjectHeader-оверлей для
 * объектных типов. Точная копия хвоста heap_alloc_ex (v34.58), вынесена
 * для TLAB fast path / carve. Возвращает пользовательский указатель.
 * Определена ЗДЕСЬ (после auto-pin TLS) из-за зависимости от
 * g_gc_in_native / gc_autopin_note. */
static inline void* heap_init_block(GCObjectHeader* header, size_t total,
                                    JavaClass* clazz, ObjectType type) {
    /* v34.71 DIAG (heap-overlap forensics): NOJME_ALLOC_TRACE=LO-HI (hex,
     * no 0x) prints every allocation whose header falls in [LO, HI).
     * Ungated on purpose: used exactly when the heap is already suspect.
     * One line per hit; parse-once via a benign race (idempotent result). */
    {
        static uintptr_t tr_lo = 0, tr_hi = 0;
        if (tr_hi == 0 && tr_lo == 0) {
            const char* e = getenv("NOJME_ALLOC_TRACE");
            if (e && e[0]) {
                uintptr_t lo = strtoul(e, (char**)&e, 16);
                if (*e == '-' || *e == ':') {
                    uintptr_t hi = strtoul(e + 1, NULL, 16);
                    if (hi > lo) { tr_lo = lo; tr_hi = hi; }
                }
            }
            if (tr_hi == 0) tr_lo = tr_hi = 1;  /* parsed, no range */
        }
        if (tr_hi > 1 && (uintptr_t)header >= tr_lo && (uintptr_t)header < tr_hi) {
            j2me_log_ungated("[ALLOC-TRACE] hdr=%p total=%zu clazz=%s type=%d tid_in_native=%d\n",
                             (void*)header, total,
                             clazz ? (clazz->class_name ? clazz->class_name : "?") : "NULL",
                             (int)type, g_gc_in_native);
        }
    }
    /* v34.71 DIAG 2: NOJME_ALLOC_LOG=path — append every allocation with its
     * class; gc_collect() appends epoch markers. Offline analysis finds two
     * LIVE allocations with overlapping [hdr, hdr+total) inside one epoch =
     * the overlapping-allocation bug (Nescube heap corruption forensics). */
    {
        FILE* alloc_log = heap_diag_alloc_log();
        if (alloc_log) {
            extern JavaThread* thread_current(JVM*);
            JavaThread* jt = thread_current(NULL);
            fprintf(alloc_log, "A %p %zu %s %d %d\n", (void*)header, total,
                    clazz ? (clazz->class_name ? clazz->class_name : "?") : "NULL",
                    (int)type, jt ? jt->id : 0);
        }
    }
    memset(header, 0, total);
    /* header->next / marked / reserved / _align_pad / _magic_pad —
     * нулевые после memset (как в оригинальном пути) */
    header->clazz = clazz;
    header->size = (uint32_t)total;
    header->type = (uint8_t)type;
    /* v34.14: объект, рождённый внутри натива, пиннится (auto-pin) */
    header->pinned = g_gc_in_native ? 1 : 0;
    if (header->pinned) gc_autopin_note(header + 1);
    header->magic = GC_HEADER_MAGIC;
    if (clazz && (type == OBJ_TYPE_OBJECT || type == OBJ_TYPE_STRING || type == OBJ_TYPE_ARRAY)) {
        ObjectHeader* obj_header = (ObjectHeader*)(header + 1);
        obj_header->clazz = clazz;
        obj_header->hashcode = (jint)(intptr_t)(header + 1);
        /* gc_mark / reserved уже нулевые из memset */
    }
    return (void*)(header + 1);
}

/* Инициализация кучи */
/* v35.12: stage diagnostics reach the device trace log even from the core
 * (weak link — headless/libretro builds without switch_trace.c just skip). */
#include <stddef.h>
#include <stdarg.h>
static void heap_stage_trace(const char* fmt, ...) {
    extern void sw_trace_force(const char* fmt, ...) __attribute__((weak));
    if (&sw_trace_force && sw_trace_force) {
        va_list ap;
        va_start(ap, fmt);
        char buf[160];
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        sw_trace_force("%s", buf);
    }
}

int heap_init(JVM* jvm, size_t initial_size, size_t max_size) {
    (void)jvm; (void)max_size;
    
    /* v19: the underlying buffer also holds the emergency reserve.
     * heap.end stays at start+initial_size (normal/GC watermarks unchanged);
     * heap.hard_end marks the true end of the buffer. */
    size_t buffer_size = initial_size + HEAP_EMERGENCY_RESERVE;
    
    /* Выравниваем начальный адрес кучи */
#if defined(_WIN32) || defined(_WIN64)
    /* Windows: использование _aligned_malloc */
    heap.start = (uint8_t*)_aligned_malloc(buffer_size, OBJECT_ALIGNMENT);
    if (!heap.start) {
        /* v35.12: the device-side "relaunch fails to load" always landed
         * here — make the OOM visible (requested size + the fact that the
         * PROCESS is out of memory, usually a leaked VM from an earlier
         * session whose teardown found a busy thread). */
        heap_stage_trace("init: heap FAILED — %u KB unavailable (process OOM; "
                         "check earlier [JVM-DESTROY] leak lines)",
                         (unsigned)(buffer_size / 1024));
        return JNI_ERR;
    }
#elif defined(__APPLE__) || defined(__linux__) || defined(__unix__)
    /* Unix-like: использование posix_memalign */
    if (posix_memalign((void**)&heap.start, OBJECT_ALIGNMENT, buffer_size) != 0) {
        /* Пробуем обычный malloc как запасной вариант */
        heap.start = (uint8_t*)malloc(buffer_size);
        if (!heap.start) {
            heap_stage_trace("init: heap FAILED — %u KB unavailable (process OOM; "
                             "check earlier [JVM-DESTROY] leak lines)",
                             (unsigned)(buffer_size / 1024));
            return JNI_ERR;
        }
        
        /* Проверяем выравнивание */
        if (((uintptr_t)heap.start & (OBJECT_ALIGNMENT - 1)) != 0) {
            /* CRITICAL FIX: On architectures with strict alignment (SPARC, old ARM, etc.),
             * unaligned access to double/long causes SIGBUS crash, not just performance loss.
             * Make this a FATAL error instead of a warning. */
#if defined(__sparc__) || defined(__sparc_v9__) || defined(__arm__) || defined(__aarch64__)
            ERROR_LOG("FATAL: malloc returned unaligned address %p on strict-alignment architecture!"
                     " This will cause SIGBUS crashes when accessing double/long values.",
                     (void*)heap.start);
            free(heap.start);
            heap.start = NULL;
            return JNI_ERR;
#else
            WARN_LOG("malloc returned unaligned address %p, performance may suffer",
                    (void*)heap.start);
#endif
        }
    }
#else
    /* Для других систем используем malloc и надеемся на лучшее */
    heap.start = (uint8_t*)malloc(buffer_size);
    if (!heap.start) {
        heap_stage_trace("init: heap FAILED — %u KB unavailable (process OOM)",
                         (unsigned)(buffer_size / 1024));
        return JNI_ERR;
    }
    
    /* Проверяем выравнивание */
    if (((uintptr_t)heap.start & (OBJECT_ALIGNMENT - 1)) != 0) {
        ERROR_LOG("FATAL: malloc returned unaligned address %p! "
                 "This may cause crashes on strict-alignment architectures.",
                 (void*)heap.start);
        free(heap.start);
        heap.start = NULL;
        return JNI_ERR;
    }
#endif

    /* v36.13 ZERO-FILL ("хип просто заполнять нулями"): the pool used to be
     * left with whatever malloc returned — typically the PREVIOUS session's
     * heap image, byte for byte (same pool size -> same address on HOS).
     * Any stale pointer a process-global cache had kept then aliased a
     * plausible-looking corpse of session N-1 instead of obviously-dead
     * memory. Fresh sessions now start from deterministic ZERO pool: a
     * stale pointer reads hdr_type=0 / NULL fields and fails validation
     * loudly (is_heap_ptr range + type checks + the [STR-CHARS-NULL]
     * trace), never masquerades as a live object of the past session.
     * Cost: one 32 MB memset per session (~5-10 ms on HOS) — noise next
     * to the session start budget. */
    memset(heap.start, 0, buffer_size);

    heap.current = heap.start;
    heap.end = heap.start + initial_size;
    heap.hard_end = heap.start + buffer_size;
    heap.size = initial_size;
    heap.allocated = 0;
    /* v35.04 PERIODIC GC: rebase the allocation budget with the fresh heap
     * (the static snapshot would otherwise carry the previous session's
     * watermark and delay the first periodic collection). */
    g_gc_alloc_at_last_gc = 0;
    g_gc_periodic_last_ms = 0;
    heap.freed = 0;
    /* v34.58: size-class LUT + чистые бакеты */
    sc_build_lut();
    for (int c = 0; c < SC_NUM_BUCKETS; c++) heap.free_lists[c] = NULL;
    heap.large_free_list = NULL;
    heap.gc_cycles = 0;
    heap.gc_total_freed = 0;
    
    /* Set global heap bounds for validation.
     * v19: use hard_end (buffer end INCLUDING the emergency reserve) so
     * exception objects allocated from the reserve still pass
     * is_heap_ptr_check() when the MIDlet calls methods on a caught
     * OutOfMemoryError (e.getMessage(), printStackTrace(), ...).
     * Normal allocations still stop at heap.end — the reserve is only
     * handed out by heap_alloc_emergency(). */
    g_heap_start = heap.start;
    g_heap_end = heap.hard_end;
    
    heap.roots = (void***)malloc(1024 * sizeof(void**));
    if (!heap.roots) {
        heap_stage_trace("init: heap FAILED — roots table OOM");
        /* CRITICAL FIX: Use _aligned_free on Windows for memory allocated with _aligned_malloc!
         * Using free() on _aligned_malloc memory causes heap corruption and crashes. */
#if defined(_WIN32) || defined(_WIN64)
        _aligned_free(heap.start);
#else
        free(heap.start);
#endif
        /* v36.07: leave NO stale arena pointers behind — the jvm_init retry
         * ladder (v36.07) may call heap_init again immediately, and
         * g_heap_start/heap.start pointing at freed memory would let an
         * is_heap_ptr_check() between the attempts validate garbage. */
        heap.start = NULL;
        g_heap_start = NULL;
        g_heap_end = NULL;
        return JNI_ERR;
    }
    heap.root_capacity = 1024;
    heap.root_count = 0;
    
    return JNI_OK;
}

/* Forward declaration for gc_mark_stack_destroy */
static void gc_mark_stack_destroy(void);

/* Уничтожение кучи */
void heap_destroy(JVM* jvm) {
    (void)jvm;

    /* CRITICAL FIX: Use _aligned_free on Windows for memory allocated with _aligned_malloc!
     * Using free() on _aligned_malloc memory causes heap corruption and crashes.
     * The _aligned_malloc functions use a special header to track alignment,
     * and _aligned_free knows how to find the original allocation address.
     */
#if defined(_WIN32) || defined(_WIN64)
    if (heap.start) {
        _aligned_free(heap.start);
    }
#else
    if (heap.start) {
        /* v36.13 ZERO-FILL before free (the "хип нулями" half): (a) a
         * zombie runner that ignored the kill and still writes through a
         * heap pointer now writes into a visible zero field instead of
         * silently corrupting whatever malloc reuses the block for;
         * (b) the freed pool can never be fingerprinted as "last session's
         * heap" by a stale pointer aliasing into the NEXT session's
         * malloc — zeros fail every object-header validation. */
        memset(heap.start, 0, (size_t)(heap.hard_end - heap.start));
        free(heap.start);
    }
#endif
    
    free(heap.roots);
    gc_mark_stack_destroy();  /* Free the GC mark stack */
    
    heap.start = NULL;
    heap.current = NULL;
    heap.end = NULL;
    heap.hard_end = NULL;
    for (int c = 0; c < SC_NUM_BUCKETS; c++) heap.free_lists[c] = NULL;  /* v34.58 */
    heap.large_free_list = NULL;                                            /* v34.58 */
}

/* [GC-DIAG2] forensics ring state (shared with heap_alloc) */
static void* g_alloc_ring = NULL;
static int g_alloc_ring_idx = 0;

/* Попытка выделить память из size-class бакетов / large-списка.
 * Возвращает указатель на блок и его реальный размер через actual_size.
 *
 * v34.58 PERF: поиск идет по возрастанию классов (свой бакет -> старше
 * -> large), каждый бакет содержит блоки близких размеров, поэтому
 * first-fit внутри бакета почти всегда решается на первом узле.
 * БЫЛО: один линейный список — O(общее число свободных блоков) на
 * каждый промах (стринг-churn игр = 100k+ блоков).
 *
 * Общий хелпер: вырезает блок из цепочки, при избытке отрезает хвост
 * в бакет ЕГО размера (инвариант №2 из блока v34.58 выше). */
static void* sc_try_chain(FreeBlock** head, size_t total_size, size_t* actual_size) {
    FreeBlock** pp = head;
    while (*pp) {
        FreeBlock* block = *pp;
        if (block->size >= total_size) {
            /* Найден подходящий блок — отсоединяем из цепочки */
            *pp = block->next;

            size_t remainder_size = (size_t)block->size - total_size;
            if (remainder_size >= sizeof(GCObjectHeader) + 8) {
                /* Дробим: remainder должен начинаться с валидного
                 * GCObjectHeader для корректного линейного прохода GC
                 * (FreeBlock использует те же поля, что и GCObjectHeader:
                 * size + next, поэтому overlay безопасен). v34.58: также
                 * ставим magic — линейные обходы (presweep-валидация строк,
                 * heap_validate) перестают 8-байтовым шагом сканировать
                 * внутренности свободного блока и прыгают по size. */
                GCObjectHeader* remainder = (GCObjectHeader*)((uint8_t*)block + total_size);
                remainder->size  = (uint32_t)remainder_size;
                remainder->type  = OBJ_TYPE_FREE;
                remainder->marked = 0;
                remainder->pinned = 0;
                remainder->clazz = NULL;
                remainder->next  = NULL;
                remainder->magic = GC_HEADER_MAGIC;
                remainder->_magic_pad = 0;

                /* Хвост — в бакет СВОЕГО размера (не в голову общего списка) */
                sc_push_free((FreeBlock*)remainder);

                *actual_size = total_size;
            } else {
                /* Нет места для remainder - используем ВЕСЬ блок
                 * (иначе образуется "дыра", ломающая линейный sweep). */
                *actual_size = block->size;
            }

            return block;
        }
        pp = &block->next;  /* блок меньше запроса — дальше по цепочке */
    }
    return NULL;
}

static void* try_alloc_from_free_list(size_t total_size, size_t* actual_size) {
    if (total_size <= SC_SMALL_MAX) {
        /* 1. Мелкий запрос: свой бакет, затем старшие мелкие бакеты */
        int cls = sc_lut[total_size >> 3];
        for (int c = cls; c < SC_NUM_BUCKETS; c++) {
            void* p = sc_try_chain(&heap.free_lists[c], total_size, actual_size);
            if (p) return p;
        }
    }
    /* 2. Крупный запрос (или мелкие бакеты пусты): large-список */
    return sc_try_chain(&heap.large_free_list, total_size, actual_size);
}

/* ============================================================
 * v34.71 CRITICAL FIX: TLAB MAIN-THREAD GATE.
 *
 * DIAGNOSED DEFECT (Nescube "corrupted magic 0x00610063" + user crashes
 * on settings-Cancel; reproduced deterministically, see NOJME_ALLOC_LOG
 * forensics): the main Java thread runs on the frontend/headless OS
 * thread. Between driver exec-windows (g_jvm_main_thread_executing == 0)
 * it executes pure native C code (M3G scene setup, form render, RMS,
 * media...) and NEVER polls GC safefoints. The GC census counts it only
 * while a window is open, so a collection triggered by a game thread
 * proceeds while the main thread keeps allocating — and if that thread
 * holds an open TLAB chunk, the sweep's linear walk runs into the
 * chunk's UNFLUSHED tail (bump allocations leave no header beyond top):
 * stale object bytes get read as a GCObjectHeader -> "corrupted magic" ->
 * the recovery resync desynced the walk and pushed free regions over
 * LIVE objects -> overlapping allocations -> SIGSEGV minutes later.
 *
 * FIX: between windows the main OS thread must not use TLAB at all — its
 * allocations take the slow path, whose heap_lock ticket spin parks it at
 * any GC request (and the park flushes the chunk). Runner threads are
 * safe either way: the census counts them (they park, flushing, or the
 * collection times out and aborts without sweeping).
 * ============================================================ */
static int tlab_main_thread_blocked(void) {
    extern volatile int g_jvm_main_thread_executing;   /* execute.c */
    if (g_jvm_main_thread_executing) return 0;         /* counted; parks at polls */
    return jvm_os_thread_is_main_java();               /* invisible between windows */
}

/* Базовое выделение памяти */
/* v19: internal allocator with emergency flag. Emergency allocations may
 * consume the reserve area beyond heap.end (see HEAP_EMERGENCY_RESERVE) and
 * are ONLY used for exception objects so that Throwable delivery works even
 * on a completely full heap. */
static void* heap_alloc_ex(JVM* jvm, size_t size, JavaClass* clazz, ObjectType type,
                           int emergency);

void* heap_alloc(JVM* jvm, size_t size, JavaClass* clazz, ObjectType type) {
    return heap_alloc_ex(jvm, size, clazz, type, 0);
}

/* v19: emergency allocator for exception objects (OutOfMemoryError et al). */
void* heap_alloc_emergency(JVM* jvm, size_t size, JavaClass* clazz, ObjectType type) {
    return heap_alloc_ex(jvm, size, clazz, type, 1);
}

static void* heap_alloc_ex(JVM* jvm, size_t size, JavaClass* clazz, ObjectType type,
                           int emergency) {
    (void)jvm;

    /* v35.04 PERIODIC GC: the exhaustion-only trigger lets garbage pile up
     * to the 64 MB heap top on long sessions (v35.03 field trace: gc=0 for
     * a whole race, 150 KB/s). Run a normal collection once the allocation
     * budget is reached — the pause is single-digit ms over a small live
     * set, and gc_in_progress makes concurrent racers no-op. Emergency
     * allocations (exception objects on a full heap) never trigger. */
    if (__builtin_expect(!emergency && jvm && gc_periodic_due(), 0)) {
        /* v36.03 GC-DEFER-IN-NATIVE: an inline gc_collect() while the VM
         * thread is inside a NATIVE (g_gc_in_native > 0) corrupts the heap
         * and the class loader — the field Data Abort (nojme_switch
         * native_vector_toString / newlib float formatting, 0xd7cf9088+8)
         * and a host segv in class_hash_lookup both reproduce exactly this
         * shape. The v34.14 auto-pin protects objects allocated INSIDE the
         * native, and the v36.03 NATIVE-ARGS-ROOTED mark scan protects the
         * caller's graph — but the safest policy is to not collect at all
         * until the native returns: skip silently (the budget check re-fires
         * on the first interpreter-context allocation). */
        extern __thread int g_gc_in_native;
        if (g_gc_in_native == 0) {
            static volatile int periodic_n = 0;
            int n = __sync_add_and_fetch(&periodic_n, 1);
            LOG_SAFE("[GC-PERIODIC] #%d allocation budget reached (used=%zu KB, heap top=%zu KB) — collecting\n",
                     n, (size_t)(heap.allocated >> 10),
                     (size_t)((heap.current - heap.start) >> 10));
            gc_collect(jvm);
        }
    }

    /* ==== v34.59 TLAB FAST PATH (без блокировок) ====
     * Мелкие аллокации 3D-churn'а бампаются в потоковом чанке.
     * Проверки по возрастанию стоимости: TLS-чанк есть -> GC не
     * запросил мир -> не диагностический режим -> размер мелкий ->
     * влезает (объект + 40-байтовый хвостовой заголовок).
     * v34.71: main-thread gate — см. tlab_main_thread_blocked(). */
#if !DEBUG_HEAP_CORRUPTION
    if (__builtin_expect(!emergency && tlab.limit != NULL &&
                         !tlab_main_thread_blocked() &&
                         !g_gc_safepoint_request &&
                         !gc_debug_enabled(), 1)) {
        size_t fsize = (size + OBJECT_ALIGNMENT - 1) & ~(size_t)(OBJECT_ALIGNMENT - 1);
        size_t total = sizeof(GCObjectHeader) + fsize;
        if (total <= TLAB_MAX_TOTAL) {
            uint8_t* top = tlab.top;
            if (top + total + sizeof(GCObjectHeader) <= tlab.limit) {
                GCObjectHeader* header = (GCObjectHeader*)top;
                tlab.top = top + total;
                __sync_add_and_fetch(&heap.allocated, total);
                g_tlab_fast_allocs++;   /* racy — диагностика */
                return heap_init_block(header, total, clazz, type);
            }
            /* Чанк исчерпан: медленный путь ниже закроет хвост
             * (возврат остатка в бакет) и вырежет новый чанк. */
        }
    }
#endif

    /* CRITICAL: Acquire heap lock BEFORE any heap operations */
    /* This is a RECURSIVE mutex, so calling gc_collect() inside is safe */
    heap_lock();
    
    /* FIX (audit G-1, v18): removed the unconditional 1 MB per-object cap — it
     * ran BEFORE any free-space/GC logic and deterministically rejected
     * legitimate game buffers (int[512*512], multi-MB resource blocks) on a
     * half-empty heap. Negative sizes are already guarded as unsigned here; we
     * only reject sizes larger than the entire heap, which no real object can
     * satisfy and which reliably indicates corrupted length math. */
    if (heap.start && heap.end > heap.start &&
        size > (size_t)(heap.end - heap.start)) {
        ERROR_LOG("CRITICAL: Oversized allocation request: %zu bytes for class=%s type=%d",
                size,
                clazz ? (clazz->class_name ? clazz->class_name : "?") : "NULL",
                type);
        heap_unlock();
        return NULL;
    }
    
    /* HEAP CORRUPTION DEBUG: Log allocation request */
    HEAP_CORRUPTION_LOG("ALLOC_START: size=%zu, clazz=%s, type=%d, heap.current=%p, heap.end=%p",
            size, clazz ? (clazz->class_name ? clazz->class_name : "?") : "NULL",
            type, heap.current, heap.end);
    
    /* Выравниваем размер запроса */
    size = (size + OBJECT_ALIGNMENT - 1) & ~(OBJECT_ALIGNMENT - 1);
    
    /* Добавляем размер GC-заголовка */
    size_t total_size = sizeof(GCObjectHeader) + size;

#if !DEBUG_HEAP_CORRUPTION
    /* ==== v34.59 TLAB REFILL (под heap_lock) ====
     * Мелкий запрос, а чанк отсутствует/исчерпан: вырезаем НОВЫЙ чанк
     * (16 КБ) — сперва из large-списка (переиспользование памяти,
     * собранной GC — чанки умирающих young-объектов сливаются в большие
     * регионы), затем с топа кучи. Текущий запрос обслуживается из
     * начала чанка прямо здесь; последующие уйдут в fast path без
     * блокировок. Один lock на ~350-500 мелких аллокаций.
     *
     * НЕ заходим сюда для больших запросов (> TLAB_MAX_TOTAL) и когда
     * валидный чанк ещё вмещает запрос (мы в slow path из-за гонки с
     * safepoint-запросом или большого размера — обычный путь ниже,
     * чанк не трогаем). */
    if (!emergency && total_size <= TLAB_MAX_TOTAL &&
        !gc_debug_enabled() && tlab_enabled() &&
        !tlab_main_thread_blocked()) {
        int need_carve = (tlab.limit == NULL) ||
                         (tlab.top + total_size + sizeof(GCObjectHeader) > tlab.limit);
        if (need_carve) {
            if (tlab.limit != NULL) {
                /* Остаток старого чанка (>= 40 байт по инварианту №2) —
                 * валидный свободный блок; немедленно возвращаем его в
                 * бакет СВОЕГО размера, чтобы не ждать ближайшего sweep. */
                if (tlab.top + sizeof(GCObjectHeader) <= tlab.limit) {
                    tlab_write_tail(tlab.top, tlab.limit);
                    sc_push_free((FreeBlock*)tlab.top);
                }
                tlab.base = tlab.top = tlab.limit = NULL;
            }
            {
                size_t chunk_actual = 0;
                void* chunk = sc_try_chain(&heap.large_free_list,
                                           TLAB_CHUNK_SIZE, &chunk_actual);
                if (!chunk && heap.current + TLAB_CHUNK_SIZE <= heap.end) {
                    chunk = heap.current;
                    chunk_actual = TLAB_CHUNK_SIZE;
                    heap.current += TLAB_CHUNK_SIZE;
                }
                if (chunk) {
                    tlab.base = (uint8_t*)chunk;
                    tlab.limit = tlab.base + chunk_actual;
                    tlab.top = tlab.base + total_size;   /* хвост — лениво */
                    g_tlab_carves++;                    /* racy — диагностика */
                    /* v34.71 DIAG 2: chunk carve event for the alloc log */
                    {
                        FILE* alloc_log = heap_diag_alloc_log();
                        if (alloc_log) {
                            extern JavaThread* thread_current(JVM*);
                            JavaThread* jt = thread_current(NULL);
                            fprintf(alloc_log, "C %p %zu %d\n", chunk, chunk_actual,
                                    jt ? jt->id : 0);
                        }
                    }
                    /* v34.59 FIX (race): АТОМАРНОЕ добавление — конкурентный
                     * поток в это же время может бампать в своём чанке БЕЗ
                     * heap_lock (fast path тоже атомарен). Обычный += здесь
                     * терял обновления (стресс P8f: allocated уходил в минус
                     * на ~сотни байт за прогон). */
                    __sync_add_and_fetch(&heap.allocated, total_size);
                    void* ptr = heap_init_block((GCObjectHeader*)chunk,
                                                total_size, clazz, type);
                    heap_unlock();
                    return ptr;
                }
                /* Carve не удался (large пуст + топ мал) — обычный путь
                 * ниже: free list -> GC -> OOM. TLAB деградирует до
                 * поведения v34.58 до следующего успешного GC. */
            }
        }
    }
#endif

    /* 1. Пытаемся найти в Free List */
    size_t actual_alloc_size = total_size;  /* По умолчанию - запрошенный размер */
    int from_free_list = 1;
    (void)from_free_list; /* retained for diagnostics */
    GCObjectHeader* header = (GCObjectHeader*)try_alloc_from_free_list(total_size, &actual_alloc_size);
    
    if (!header) {
        /* 2. Если нет в Free List, берем из топа кучи (Bump Pointer) */
        if (heap.current + total_size > heap.end) {
            /* Не хватает места в конце кучи. Запускаем GC. */
            HEAP_CORRUPTION_LOG("ALLOC_GC: triggering GC, need=%zu, available=%zu",
                    total_size, (size_t)(heap.end - heap.current));
            /* v34.15 DIAG (Galaxy on Fire): GC fired while the heap was only
             * half full — only a single huge request can do that. Log the
             * first few so the game-side caller can be identified. */
            if (total_size >= (1 << 20)) {
                static int huge_gc_trigger_count = 0;
                if (huge_gc_trigger_count < 5) {
                    huge_gc_trigger_count++;
                    LOG_SAFE("[HEAP-HUGE] %zu bytes (class=%s type=%d) needs GC: current=%zu end=%zu\n",
                            total_size,
                            clazz ? (clazz->class_name ? clazz->class_name : "?") : "?",
                            type,
                            (size_t)(heap.current - heap.start),
                            (size_t)(heap.end - heap.start));
                }
            }
            /* v36.03 GC-DEFER-IN-NATIVE: same policy as the periodic trigger
             * — never run an inline stop-the-world from inside a native.
             * This allocation falls through to the free-list/emergency paths
             * (a catchable OutOfMemoryError beats heap corruption); the next
             * interpreter-context allocation retries the collection. */
            extern __thread int g_gc_in_native;
            if (g_gc_in_native == 0) {
                gc_collect(jvm);
            }
            header = (GCObjectHeader*)try_alloc_from_free_list(total_size, &actual_alloc_size);
            
            if (!header) {
                /* Все еще нет места. Проверяем топ кучи (GC мог освободить место в середине, но не в конце) */
                if (heap.current + total_size > heap.end) {
                    /* v19: emergency allocations (exception objects) may dip into
                     * the reserve beyond heap.end so the VM can still THROW a
                     * catchable OutOfMemoryError / Throwable on a full heap. */
                    if (emergency && heap.hard_end &&
                        heap.current + total_size <= heap.hard_end) {
                        HEAP_CORRUPTION_LOG("ALLOC_EMERGENCY: requested=%zu, reserve_left=%zu",
                                total_size,
                                (size_t)(heap.hard_end - heap.current - total_size));
                        LOG_SAFE("[HEAP] EMERGENCY allocation: %zu bytes (reserve left: %zu)\n",
                                total_size,
                                (size_t)(heap.hard_end - heap.current - total_size));
                        from_free_list = 0;
                        header = (GCObjectHeader*)heap.current;
                        heap.current += total_size;
                        actual_alloc_size = total_size;
                        /* Skip the normal bump-pointer path below by falling
                         * through with header set. */
                        goto emergency_allocated;
                    }
                    ERROR_LOG("Out of memory! Requested: %zu, Available: %zu",
                            total_size,
                            (size_t)(heap.end > heap.current ? (heap.end - heap.current) : 0));
                    HEAP_CORRUPTION_LOG("ALLOC_OOM: requested=%zu, available=%zu",
                            total_size,
                            (size_t)(heap.end > heap.current ? (heap.end - heap.current) : 0));
                    
                    /* v34 FIX: remember sizes for the OutOfMemoryError
                     * detailMessage (native_throw_oome). Do NOT touch the
                     * error-screen state here: a failed allocation is a
                     * catchable condition, not a VM-fatal one. */
                    g_last_oom_requested = total_size;
                    g_last_oom_available = (heap.end > heap.current)
                                           ? (size_t)(heap.end - heap.current) : 0;
                    /* v34.5 DIAG: one-line live-set summary at OOM */
                    {
                        static int oom_diag_count = 0;
                        if (oom_diag_count < 3) {
                            oom_diag_count++;
                            LOG_SAFE("[HEAP-OOM] live=%zu bytes, gc_cycles=%zu, total_freed=%zu, req=%zu\n",
                                     heap.allocated, heap.gc_cycles, heap.gc_total_freed,
                                     total_size);
                            /* v36.08: mirror into the trace file — stderr is
                             * invisible on Switch, and a JVM-heap OOM inside a
                             * native (GC-DEFER-IN-NATIVE skips the collection)
                             * is exactly the silent-null class the field logs
                             * must explain. */
                            {
                                extern void sw_trace_force(const char* fmt, ...)
                                    __attribute__((weak));
                                if (&sw_trace_force && sw_trace_force) {
                                    sw_trace_force("[HEAP-OOM] live=%zuK gc=%zu req=%zu avail=%zu%s",
                                                   heap.allocated >> 10, heap.gc_cycles,
                                                   total_size,
                                                   (size_t)(heap.end > heap.current ? (heap.end - heap.current) : 0),
                                                   g_gc_in_native != 0 ? " (in-native: GC deferred!)" : "");
                                }
                            }
                        }
                    }
                    heap_unlock();  /* Release lock before returning */
                    return NULL;
                }
                
                /* Если GC освободил место в конце, используем bump pointer */
                from_free_list = 0;
                header = (GCObjectHeader*)heap.current;
                heap.current += total_size;
                actual_alloc_size = total_size;
            }
        } else {
            /* Место есть, выделяем */
            from_free_list = 0;
            header = (GCObjectHeader*)heap.current;
            heap.current += total_size;
            actual_alloc_size = total_size;
        }
    } else {
        HEAP_CORRUPTION_LOG("ALLOC_FREELIST: found block at %p, size=%zu", header, actual_alloc_size);
    }

emergency_allocated:;
    
    /* v34.59 FIX (race): атомарно — см. комментарий в TLAB-carve: */
    /* конкурентный fast-path поток не берёт heap_lock. */
    __sync_add_and_fetch(&heap.allocated, actual_alloc_size);
    
    /* v34.5 DIAG: track big allocations + live-set growth to diagnose
     * full-heap OOMs (Doom RPG [Rus] new-game path). First 40 hits. */
    {
        static int big_alloc_count = 0;
        if (actual_alloc_size >= 32768 && big_alloc_count < 40) {
            big_alloc_count++;
            LOG_SAFE("[HEAP-BIG] alloc #%d: %zu bytes, class=%s type=%d, live=%zu\n",
                     big_alloc_count, actual_alloc_size,
                     clazz ? (clazz->class_name ? clazz->class_name : "?") : "?",
                     type, heap.allocated);
        }
    }
    
    /* FIX-19n: ALWAYS zero the allocated block. The old assumption
     * ("bump-pointer memory is fresh/never written") is wrong since
     * gc_collect() rewinds heap.current into freed space (top
     * defragmentation), so a reused bump region contains stale object
     * bytes. JVMS requires new arrays to be zero-filled. */
    memset(header, 0, actual_alloc_size);

    /* v34.71 DIAG: same range trace as heap_init_block (see there). */
    {
        static uintptr_t tr_lo = 0, tr_hi = 0;
        if (tr_hi == 0 && tr_lo == 0) {
            const char* e = getenv("NOJME_ALLOC_TRACE");
            if (e && e[0]) {
                uintptr_t lo = strtoul(e, (char**)&e, 16);
                if (*e == '-' || *e == ':') {
                    uintptr_t hi = strtoul(e + 1, NULL, 16);
                    if (hi > lo) { tr_lo = lo; tr_hi = hi; }
                }
            }
            if (tr_hi == 0) tr_lo = tr_hi = 1;
        }
        if (tr_hi > 1 && (uintptr_t)header >= tr_lo && (uintptr_t)header < tr_hi) {
            j2me_log_ungated("[ALLOC-TRACE] hdr=%p total=%zu actual=%zu clazz=%s type=%d tid_in_native=%d\n",
                             (void*)header, total_size, actual_alloc_size,
                             clazz ? (clazz->class_name ? clazz->class_name : "?") : "NULL",
                             (int)type, g_gc_in_native);
        }
    }
    
    /* Инициализация заголовка */
    /* Примечание: next здесь не используется для связывания объектов при GC, 
       так как мы используем линейный проход. Поле зарезервировано. */
    header->next = NULL; 
    header->clazz = clazz;
    header->size = (uint32_t)actual_alloc_size;  /* CRITICAL FIX: используем РАЛЬНЫЙ размер! */
    header->type = (uint8_t)type;
    header->marked = 0;
    /* v34.14: объект, рождённый внутри исполняющегося натива, пиннится
     * сразу (C-локалы натива невидимы для GC mark-фазы) и регистрируется
     * в per-thread auto-pin журнале; pin снимется при выходе из натива. */
    header->pinned = g_gc_in_native ? 1 : 0;
    if (header->pinned) gc_autopin_note(header + 1);
    /* reserved[0..1], _align_pad[0..7] already zeroed by memset above */
    header->magic = GC_HEADER_MAGIC;  /* Magic number for corruption detection */
    header->_magic_pad = 0;  /* Padding for 8-byte alignment */

    /* [GC-DIAG2] record allocation in a ring for corruption forensics */
    {
        static struct { void* addr; uint32_t size; int type; const char* name; } alloc_ring[512];
        static int ring_idx = 0;
        alloc_ring[ring_idx].addr = (void*)header;
        alloc_ring[ring_idx].size = (uint32_t)actual_alloc_size;
        alloc_ring[ring_idx].type = (int)type;
        alloc_ring[ring_idx].name = (clazz && clazz->class_name) ? clazz->class_name : "?";
        ring_idx = (ring_idx + 1) % 512;
        g_alloc_ring = (void*)alloc_ring;
        g_alloc_ring_idx = ring_idx;
    }
    /* v34.71 DIAG 2: full allocation log (see heap_init_block for rationale) */
    {
        FILE* alloc_log = heap_diag_alloc_log();
        if (alloc_log) {
            extern JavaThread* thread_current(JVM*);
            JavaThread* jt = thread_current(NULL);
            fprintf(alloc_log, "A %p %zu %s %d %d\n", (void*)header, actual_alloc_size,
                    clazz ? (clazz->class_name ? clazz->class_name : "?") : "NULL",
                    (int)type, jt ? jt->id : 0);
        }
    }
    
    /* Вычисляем указатель на пользовательские данные (сразу после заголовка) */
    void* ptr = (void*)(header + 1);
    
    /* CRITICAL FIX: Also set clazz in ObjectHeader for JavaObject types
     * ObjectHeader.clazz is at offset 0 of the object, while GCObjectHeader.clazz
     * is at offset 24 of the GC header. They are DIFFERENT memory locations!
     * For JavaObject (OBJ_TYPE_OBJECT, OBJ_TYPE_STRING, OBJ_TYPE_ARRAY),
     * we need to set the ObjectHeader.clazz field as well.
     */
    if (clazz && (type == OBJ_TYPE_OBJECT || type == OBJ_TYPE_STRING || type == OBJ_TYPE_ARRAY)) {
        ObjectHeader* obj_header = (ObjectHeader*)ptr;
        obj_header->clazz = clazz;
        obj_header->hashcode = (jint)(intptr_t)ptr;  /* Default hashcode = object address */
        obj_header->gc_mark = 0;
        obj_header->reserved = 0;
    }
    
    /* HEAP CORRUPTION DEBUG: Log successful allocation */
    HEAP_CORRUPTION_LOG("ALLOC_DONE: ptr=%p, header=%p, size=%u, type=%d, clazz=%s",
            ptr, header, header->size, type,
            clazz ? (clazz->class_name ? clazz->class_name : "?") : "NULL");
    
    /* DEBUG: Log heap allocation */
    GC_DEBUG("[HEAP_ALLOC] ptr=%p, gc_header=%p, clazz=%p (%s), type=%d, size=%u",
            ptr, header, clazz, 
            clazz ? (clazz->class_name ? clazz->class_name : "NO_NAME") : "NULL",
            type, header->size);
    
    /* CRITICAL: Release heap lock after all operations complete */
    heap_unlock();
    
    return ptr;
}

/* Выделение объекта */
JavaObject* heap_alloc_object(JVM* jvm, JavaClass* clazz) {
    if (!clazz) {
        ERROR_LOG("heap_alloc_object called with NULL class");
        return NULL;
    }
    
    /* ИСПРАВЛЕНИЕ: clazz->instance_size уже должен включать sizeof(ObjectHeader) + поля.
       Мы не должны добавлять sizeof(ObjectHeader) здесь снова. */
    size_t size = clazz->instance_size;
    
    /* Минимальная защита */
    if (size < sizeof(JavaObject)) {
        size = sizeof(JavaObject);
    }

    /* Выравниваем размер объекта */
    size = (size + OBJECT_ALIGNMENT - 1) & ~(OBJECT_ALIGNMENT - 1);
    
    /* GC_DEBUG("Allocating object: class=%s, size=%zu", clazz->class_name, size); */
    
    JavaObject* obj = (JavaObject*)heap_alloc(jvm, size, clazz, OBJ_TYPE_OBJECT);
    if (obj) {
        /* Инициализируем Java-хедер объекта */
        obj->header.clazz = clazz;
        obj->header.hashcode = (jint)(intptr_t)obj;  /* Identity hash */
        /* gc_mark and reserved already zeroed by heap_alloc's memset */
        
        /* NOTE: heap_alloc already memsets the entire block (including fields) to zero.
         * No need for a second memset of obj->fields here. */
    } else {
        ERROR_LOG("heap_alloc failed for class %s", 
                clazz->class_name ? clazz->class_name : "unknown");
    }
    
    return obj;
}

/* v19: emergency object allocation — uses the reserve area so exception
 * objects can be created even when the heap is completely full. Mirrors
 * heap_alloc_object but never logs the regular "heap_alloc failed" error
 * (the caller decides whether a NULL result is fatal). */
JavaObject* heap_alloc_object_emergency(JVM* jvm, JavaClass* clazz) {
    if (!clazz) {
        return NULL;
    }

    size_t size = clazz->instance_size;
    if (size < sizeof(JavaObject)) {
        size = sizeof(JavaObject);
    }

    size = (size + OBJECT_ALIGNMENT - 1) & ~(OBJECT_ALIGNMENT - 1);

    JavaObject* obj = (JavaObject*)heap_alloc_emergency(jvm, size, clazz, OBJ_TYPE_OBJECT);
    if (obj) {
        obj->header.clazz = clazz;
        obj->header.hashcode = (jint)(intptr_t)obj;
    }

    return obj;
}

/* v34.58 PERF: кэшированный поиск горячих встроенных классов.
 * Прежний линейный скан проходил ВЕСЬ массив классов (у игр это
 * 500-2000+ strcmp) на КАЖДУЮ аллокацию массива/строки — при
 * стринг-churn'е это доминирующая константа на аллокацию.
 * Классы в этой VM никогда не выгружаются, поэтому позитивный
 * результат кэшируется навсегда. Негативный НЕ кэшируется
 * (класс может быть загружен позже — ранний запуск, JAR). */
static JavaClass* heap_cached_class(JVM* jvm, const char* name, JavaClass** cache_slot) {
    if (*cache_slot) return *cache_slot;
    for (size_t i = 0; i < jvm->class_loader.count; i++) {
        JavaClass* c = jvm->class_loader.classes[i];
        if (c->class_name && strcmp(c->class_name, name) == 0) {
            *cache_slot = c;
            return c;
        }
    }
    return NULL;
}
static JavaClass* s_heap_class_object = NULL;  /* java/lang/Object */
static JavaClass* s_heap_class_string = NULL;  /* java/lang/String */

/* v34.91 MULTI-SESSION FIX: same pattern as jvm.c's s_string_class_cache —
 * these caches survive jvm_destroy and would dangle into the next game
 * session (Switch frontend). Called from jvm_destroy. */
void heap_class_cache_reset(void) {
    s_heap_class_object = NULL;
    s_heap_class_string = NULL;
}

/* Выделение массива */
JavaArray* heap_alloc_array(JVM* jvm, uint8_t element_type, jsize length, JavaClass* element_class) {
    /* Verify JavaArray structure size at runtime */
    /* On 64-bit: should be 32 bytes (ObjectHeader 16 + length 4 + element_type 1 + reserved 3 + element_class 8) */
    /* On 32-bit: CRITICAL - ObjectHeader has bit-fields, actual size is 12 bytes, not 8! */
    /*   ObjectHeader = clazz(4) + hashcode(4) + gc_mark+reserved(4) = 12 bytes */
    /*   JavaArray = ObjectHeader(12) + length(4) + element_type(1) + reserved(3) + element_class(4) = 24 bytes */
    
    /* CRITICAL: Log actual structure sizes for debugging */
    static bool sizes_logged = false;
    if (!sizes_logged) {
        LOG_SAFE("[HEAP] Structure sizes: ObjectHeader=%zu, JavaArray=%zu, JavaObject=%zu, JavaValue=%zu, GCObjectHeader=%zu\n",
                sizeof(ObjectHeader), sizeof(JavaArray), sizeof(JavaObject), sizeof(JavaValue), sizeof(GCObjectHeader));
        
        /* Verify ObjectHeader is multiple of 8 for proper field alignment */
        if (sizeof(ObjectHeader) % 8 != 0) {
            LOG_SAFE("[HEAP] WARNING: ObjectHeader size %zu is NOT 8-byte aligned! Fields will be misaligned.\n",
                    sizeof(ObjectHeader));
        }
        
        sizes_logged = true;
    }
    
    /* Размер элемента */
    size_t elem_size;
    switch (element_type) {
        case T_BOOLEAN:
        case T_BYTE:    elem_size = 1; break;
        case T_CHAR:
        case T_SHORT:   elem_size = 2; break;
        case T_INT:
        case T_FLOAT:   elem_size = 4; break;
        case T_LONG:
        case T_DOUBLE:  elem_size = 8; break;
        case DESC_OBJECT:
        case DESC_ARRAY: elem_size = sizeof(void*); break;
        default: elem_size = 4; break;
    }
    
    size_t data_size = elem_size * length;
    size_t total_size = sizeof(JavaArray) + data_size;
    
    /* Поиск класса java/lang/Object для массива (v34.58: через кэш). */
    JavaClass* array_class = heap_cached_class(jvm, "java/lang/Object", &s_heap_class_object);
    
    if (!array_class && jvm->class_loader.count > 0) {
        array_class = jvm->class_loader.classes[0]; /* Fallback */
    }
    
    /* DEBUG: Log what array_class we found */
    GC_DEBUG("[HEAP_ALLOC_ARRAY] array_class=%p (%s), element_class=%p (%s)",
            (void*)array_class, array_class ? (array_class->class_name ? array_class->class_name : "NO_NAME") : "NULL",
            (void*)element_class, element_class ? (element_class->class_name ? element_class->class_name : "NO_NAME") : "NULL");
    
    /* ИСПРАВЛЕНО: Передаём array_class в heap_alloc, чтобы GC header->clazz был установлен */
    JavaArray* array = (JavaArray*)heap_alloc(jvm, total_size, array_class, OBJ_TYPE_ARRAY);
    if (!array) return NULL;
    
    array->header.clazz = array_class;
    array->header.hashcode = (jint)(intptr_t)array;
    array->length = length;
    array->element_type = element_type;
    memset(array->_reserved, 0, sizeof(array->_reserved));
    array->element_class = element_class;  /* Store element class for object arrays */
    
    /* Debug: log array allocation details */
    DEBUG_LOG("heap_alloc_array: array=%p, elem_type=%d, length=%d, elem_class=%s, sizeof(JavaArray)=%zu",
              (void*)array, element_type, length, 
              element_class ? (element_class->class_name ? element_class->class_name : "?") : "NULL",
              sizeof(JavaArray));
    
    return array;
}

/* Выделение строки
 * 
 * CRITICAL FIX: Унификация модели строк!
 * 
 * Если класс java/lang/String загружен, создаем OBJ_TYPE_OBJECT с полями.
 * Это Java String объект, который хранит данные в отдельном char[] через поле 'value'.
 * 
 * Если класс java/lang/String НЕ загружен, создаем OBJ_TYPE_STRING с inline данными.
 * Это нативная строка для внутреннего использования (например, до загрузки классов).
 */
JavaString* heap_alloc_string(JVM* jvm, jsize length) {
    /* Найти класс java/lang/String (v34.58: через кэш) */
    JavaClass* str_class = heap_cached_class(jvm, "java/lang/String", &s_heap_class_string);
    
    /* Если класс java/lang/String найден, создаем как OBJ_TYPE_OBJECT!
     * Это КРИТИЧЕСКИ важно для правильной работы GC.
     * Java String объект имеет поля: value (char[]), offset (int), count (int), hash (int)
     * Данные хранятся в отдельном массиве char[], на который ссылается поле value.
     * 
     * ВАЖНО: НЕ используем поля JavaString (length, hash, utf8, chars) т.к. они
     * перекрываются с fields[]! Используем только fields[].
     */
    if (str_class) {
        /* Выделяем объект с полями */
        size_t obj_size = str_class->instance_size;
        obj_size = (obj_size + OBJECT_ALIGNMENT - 1) & ~(OBJECT_ALIGNMENT - 1);
        
        JavaObject* str = (JavaObject*)heap_alloc(jvm, obj_size, str_class, OBJ_TYPE_OBJECT);
        if (!str) return NULL;
        
        /* Инициализируем заголовок - heap_alloc уже установил clazz, но мы удостоверимся */
        str->header.clazz = str_class;
        
        /* Поля: value, offset, count, hash будут установлены вызывающим кодом
         * (обычно в native String.<init> или jvm_new_string_utf16)
         * ВАЖНО: НЕ устанавливаем str->length, str->chars и т.д. - они перекрываются с fields[]! */
        
        return (JavaString*)str;
    }
    
    /* Класс java/lang/String НЕ найден - создаем нативную строку с inline данными.
     * Это используется только на ранней стадии инициализации JVM. */
    size_t data_size = length * sizeof(jchar);
    size_t total_size = sizeof(JavaString) + data_size;
    
    JavaString* str = (JavaString*)heap_alloc(jvm, total_size, NULL, OBJ_TYPE_STRING);
    if (!str) return NULL;
    
    str->header.clazz = NULL;
    str->header.hashcode = (jint)(intptr_t)str;
    str->header.gc_mark = 0;
    str->header.reserved = 0;
    str->length = length;
    str->hash = 0;
    str->utf8 = NULL;
    str->chars = NULL;  /* Native strings use inline storage, accessed via string_chars */
    
    return str;
}

/* --- Mark and Sweep Implementation --- */

/* ИСПРАВЛЕНО: Динамический размер стека маркировки GC */
#define GC_MARK_STACK_INITIAL_SIZE 4096
#define GC_MARK_STACK_MAX_SIZE (1024 * 1024)  /* Максимум 1M объектов */

typedef struct {
    void* ptr;
} MarkStackEntry;

static MarkStackEntry* gc_mark_stack = NULL;
static int gc_mark_stack_top = 0;
static int gc_mark_stack_capacity = 0;

/* ИСПРАВЛЕНО: Инициализация стека маркировки */
static bool gc_mark_stack_init(void) {
    gc_mark_stack = (MarkStackEntry*)malloc(GC_MARK_STACK_INITIAL_SIZE * sizeof(MarkStackEntry));
    if (!gc_mark_stack) return false;
    gc_mark_stack_capacity = GC_MARK_STACK_INITIAL_SIZE;
    gc_mark_stack_top = 0;
    return true;
}

/* ИСПРАВЛЕНО: Освобождение стека маркировки */
static void gc_mark_stack_destroy(void) {
    if (gc_mark_stack) {
        free(gc_mark_stack);
        gc_mark_stack = NULL;
    }
    gc_mark_stack_capacity = 0;
    gc_mark_stack_top = 0;
}

/* Проверка валидности указателя на GC-объект */
static bool is_valid_gc_object(void* ptr) {
    if (!ptr) return false;
    
    /* CRITICAL FIX: Check if pointer looks like a small integer value
     * These are NOT heap pointers! Common in Java where int values
     * are stored in reference fields before proper initialization. */
    if ((uintptr_t)ptr < 0x10000) {
        /* Almost certainly a small integer, not a pointer */
        return false;
    }
    
    if (!is_heap_ptr(ptr)) {
        return false;
    }
    
    /* Получаем GC заголовок (он находится перед пользовательскими данными) */
    GCObjectHeader* header = (GCObjectHeader*)ptr - 1;
    
    /* Проверка magic number - это самый надежный способ проверить валидность */
    if (header->magic != GC_HEADER_MAGIC) {
        return false;
    }
    
    /* Проверка валидности размера */
    if (header->size == 0 || header->size > heap.size) {
        return false;
    }
    
    /* CRITICAL FIX: Reject objects that are already freed (OBJ_TYPE_FREE = 4)
     * This prevents use-after-free when static fields reference freed objects */
    if (header->type == OBJ_TYPE_FREE) {
        return false;
    }
    
    /* Проверка валидности типа */
    if (header->type > OBJ_TYPE_CLASS) {
        return false;
    }
    
    /* Проверка что объект находится в правильном месте в куче */
    uint8_t* obj_start = (uint8_t*)header;
    uint8_t* obj_end = obj_start + header->size;
    if (obj_end > heap.current) {
        return false;
    }
    
    return true;
}

/* v15: Exported liveness probe for JavaObject* held by native code.
 * See heap.h comment. Same checks as is_valid_gc_object(). */
bool heap_java_object_valid(void* ptr) {
    return is_valid_gc_object(ptr);
}

/* ============================================================
 * v34.59 PERF: ПЕР-КЛАССОВЫЙ КЭШ РАЗМЕТКИ GC (mark/sweep)
 * ============================================================
 * Проблема: gc_mark_object() для КАЖДОГО помечаемого OBJ_TYPE_OBJECT
 * строил массив hierarchy[64], обходил ВСЕ классы иерархии и ВСЕ их
 * поля (проверка access_flags, разыменование descriptor, strcmp
 * "nativePeer", арифметика слотов J/D). Для 3D-игр с большими живыми
 * графами (сцены M3G: Node/Transform/Group/Mesh + Vector/Object[]-
 * контейнеры, сотни тысяч объектов) это была доминирующая константа
 * mark-фазы: 150-400 тактов НА ОБЪЕКТ только на «понять, какие слоты
 * поля — ссылки», при том что разметка класса НЕИЗМЕННА после загрузки.
 *
 * Решение: считаем разметку ОДИН раз на класс и кладём в JavaClass:
 *   - gc_ref_slots[]: индексы слотов полей-ссылок (в порядке иерархии,
 *     nativePeer исключён) — mark-фаза сводится к циклу по готовым
 *     индексам (~15-30 тактов на объект вместо 150-400);
 *   - gc_kind: биты семейств классов (String / Hashtable / Vector /
 *     Entry) — strcmp имён классов выполняется один раз на класс
 *     вместо одного-двух на каждый объект в mark и sweep.
 *
 * Когда строится: ЛЕНИВО, при первой встрече объекта класса в
 * gc_mark_object / sweep / presweep. Все эти места выполняются под
 * heap_lock с остановленным миром (единственный поток-мутатор GC),
 * поэтому гонок на ленивую инициализацию нет. Классы в рантайме не
 * выгружаются (см. v34.58 note), кэш валиден до shutdown.
 *
 * Отказ malloc'а массива слотов НЕ фатален: LAYOUT_READY не
 * выставляется, mark-фаза уходит в резервный медленный проход
 * (gc_mark_fields_slow) — поведение деградирует до v34.58, но не
 * ломается. Биты gc_kind выставляются ДО попытки malloc, поэтому
 * sweep/presweep-пути (нуждающиеся только в битах) работают всегда.
 * ============================================================ */
#define GC_KIND_READY  0x01u   /* gc_ref_slots построен и валиден */
#define GC_KIND_STRING 0x02u   /* имя == "java/lang/String" */
#define GC_KIND_HT     0x04u   /* имя == java/util/Hashtable|HashMap */
#define GC_KIND_VEC    0x08u   /* имя == java/util/Vector|ArrayList */
#define GC_KIND_ENTRY  0x10u   /* имя СОДЕРЖИТ "Hashtable$Entry"/"HashMap$Entry" */

/* gc_mark_fields_slow вызывается до определения gc_push_mark ниже */
static bool gc_push_mark(void* ptr);

/* Резервный (медленный) проход по полям — точная копия логики
 * v34.58: иерархия 64 уровня, static-пропуск, 'L'/'[' дескрипторы,
 * nativePeer-пропуск, 2 слота у J/D. Используется пока разметка
 * класса не построена (или malloc массива слотов не удался). */
static void gc_mark_fields_slow(JavaObject* obj, JavaClass* clazz) {
    JavaClass* hierarchy[64];
    int depth = 0;
    JavaClass* c = clazz;
    while (c && depth < 64) {
        hierarchy[depth++] = c;
        c = c->super_class;
    }

    int slot = 0;
    for (int h = depth - 1; h >= 0; h--) {
        JavaClass* current = hierarchy[h];
        if (!current->fields) continue;

        for (int i = 0; i < current->fields_count; i++) {
            JavaField* field = &current->fields[i];

            /* Skip static fields */
            if (field->access_flags & ACC_STATIC) continue;

            if (field->descriptor) {
                char desc = field->descriptor[0];
                if (desc == 'L' || desc == '[') {
                    void* ref = obj->fields[slot].ref;
                    if (ref) {
                        /* nativePeer — нативный указатель, НЕ Java-ссылка */
                        if (field->name && strcmp(field->name, "nativePeer") == 0) {
                            /* skip */
                        } else {
                            gc_push_mark(ref);
                        }
                    }
                }
            }

            /* Advance slot; long/double занимают 2 слота */
            slot++;
            if (field->descriptor &&
                (field->descriptor[0] == 'J' || field->descriptor[0] == 'D')) {
                slot++;
            }
        }
    }
}

/* Построить (если ещё не построена) разметку класса и вернуть биты
 * GC_KIND_*. Вызывается ТОЛЬКО из GC-контекста (мир остановлен).
 * Идемпотентно; потокобезопасность обеспечивается остановкой мира. */
static uint8_t gc_class_kind(JavaClass* clazz) {
    if (!clazz) return 0;
    /* v34.80: defense in depth for the recycled-header crash (see the
     * gc_mark_object OBJ_TYPE_OBJECT guard) — real classes are malloc'd,
     * never GC-heap residents; nothing real lives below the mmap floor. */
    if (is_heap_ptr((void*)clazz) || (uintptr_t)clazz < 0x10000u) return 0;
    if (clazz->gc_kind & GC_KIND_READY) return clazz->gc_kind;

    /* 1) Биты семейств — strcmp по имени ОДИН раз на класс.
     *    Порядок проверок повторяет прежний код mark/sweep (strcmp
     *    точных имён + strstr для Entry-семейства). */
    uint8_t kind = 0;
    const char* cn = clazz->class_name;
    /* v34.80: real class names are malloc'd constant-pool bytes, strdup'd
     * stub names or .rodata literals — none of them live inside the GC
     * heap or below the mmap floor; both shapes were observed in the
     * recycled-header garbage (0x19 / 0x65). */
    if (cn && !is_heap_ptr((void*)cn) && (uintptr_t)cn >= 0x10000u) {
        if (strcmp(cn, "java/lang/String") == 0) {
            kind |= GC_KIND_STRING;
        }
        if (strcmp(cn, "java/util/Hashtable") == 0 ||
            strcmp(cn, "java/util/HashMap") == 0) {
            kind |= GC_KIND_HT;
        }
        if (strcmp(cn, "java/util/Vector") == 0 ||
            strcmp(cn, "java/util/ArrayList") == 0) {
            kind |= GC_KIND_VEC;
        }
        if (strstr(cn, "Hashtable$Entry") != NULL ||
            strstr(cn, "HashMap$Entry") != NULL) {
            kind |= GC_KIND_ENTRY;
        }
    }
    clazz->gc_kind = (uint8_t)(clazz->gc_kind | kind);

    /* 2) Слоты полей-ссылок (та же арифметика слотов, что в
     *    gc_mark_fields_slow, чтобы индексы совпадали байт-в-байт). */
    JavaClass* hierarchy[64];
    int depth = 0;
    JavaClass* c = clazz;
    while (c && depth < 64) {
        /* v34.80: guard every hop — the entry checks validated clazz
         * itself, but a wild clazz that points at recycled C memory can
         * still carry garbage in super_class (observed under qemu-arm:
         * super_class = 0x11, fault at 0x11+offsetof(super_class)=0x65).
         * Real classes are malloc'd, never GC-heap residents, and nothing
         * real lives below the mmap floor — same invariants as above.
         * Truncating the hierarchy only narrows the slot set; the walk
         * below then reads fields of validated classes only. */
        if (is_heap_ptr((void*)c) || (uintptr_t)c < 0x10000u) break;
        hierarchy[depth++] = c;
        c = c->super_class;
    }

    /* Проход 1: посчитать ссылочные слоты */
    int nslots = 0;
    {
        int slot = 0;
        for (int h = depth - 1; h >= 0; h--) {
            JavaClass* cur = hierarchy[h];
            if (!cur->fields) continue;
            for (int i = 0; i < cur->fields_count; i++) {
                JavaField* field = &cur->fields[i];
                if (field->access_flags & ACC_STATIC) continue;
                if (field->descriptor) {
                    char desc = field->descriptor[0];
                    if (desc == 'L' || desc == '[') {
                        if (!(field->name &&
                              strcmp(field->name, "nativePeer") == 0)) {
                            nslots++;
                        }
                    }
                }
                slot++;
                if (field->descriptor &&
                    (field->descriptor[0] == 'J' || field->descriptor[0] == 'D')) {
                    slot++;
                }
            }
        }
    }

    /* Проход 2: собрать индексы. Максимум полей у J2ME-классов мал
     * (сотни), uint16_t слота хватает с запасом (инвариант: полей
     * < 65536 — Java-ограничение classfile-формата u2). */
    if (nslots > 0) {
        uint16_t* slots = (uint16_t*)malloc((size_t)nslots * sizeof(uint16_t));
        if (slots) {
            int slot = 0;
            int out = 0;
            for (int h = depth - 1; h >= 0; h--) {
                JavaClass* cur = hierarchy[h];
                if (!cur->fields) continue;
                for (int i = 0; i < cur->fields_count; i++) {
                    JavaField* field = &cur->fields[i];
                    if (field->access_flags & ACC_STATIC) continue;
                    if (field->descriptor) {
                        char desc = field->descriptor[0];
                        if (desc == 'L' || desc == '[') {
                            if (!(field->name &&
                                  strcmp(field->name, "nativePeer") == 0) &&
                                out < nslots) {
                                slots[out++] = (uint16_t)slot;
                            }
                        }
                    }
                    slot++;
                    if (field->descriptor &&
                        (field->descriptor[0] == 'J' || field->descriptor[0] == 'D')) {
                        slot++;
                    }
                }
            }
            clazz->gc_ref_slots = slots;
            clazz->gc_ref_slot_count = (uint16_t)out;
            kind |= GC_KIND_READY;
        }
        /* malloc не удался: READY не ставим — mark уйдёт в slow-path */
    } else {
        /* Ссылочных полей нет — разметка "пуста", но ГОТОВА */
        clazz->gc_ref_slots = NULL;
        clazz->gc_ref_slot_count = 0;
        kind |= GC_KIND_READY;
    }

    clazz->gc_kind = (uint8_t)(clazz->gc_kind | kind);
    return clazz->gc_kind;
}


/* ИСПРАВЛЕНО: Push с проверкой и расширением стека */
static bool gc_push_mark(void* ptr) {
    if (!ptr) return true;

    /* CRITICAL FIX: Validate that this is actually a GC-managed object.
     * This prevents primitive values (ints, floats) that happen to look like
     * heap pointers from being treated as references. */
    if (!is_valid_gc_object(ptr)) {
        /* CRITICAL: Log WHY validation failed for debugging.
         * DO NOT try to access header for invalid pointers - it would cause segfault! */
        GC_LOG("[GC_PUSH] SKIPPING invalid ptr=%p (heap=%p-%p, current=%p)\n", 
                ptr, heap.start, heap.end, heap.current);
        /* For small integers (< 0x10000), this is likely a primitive value stored in a reference field.
         * This is usually harmless - the field was not initialized or contains a non-reference value. */
        if ((uintptr_t)ptr < 0x10000) {
            GC_LOG("[GC_PUSH]   ptr looks like a small integer value: %zu\n", (uintptr_t)ptr);
        } else {
            /* CRITICAL FIX: Do NOT try to access header for invalid pointers!
             * The pointer may point to unmapped memory, causing segfault.
             * Just log that it's invalid and skip it. */
            GC_LOG("[GC_PUSH]   ptr is outside heap or invalid - skipping without dereferencing\n");
        }
        /* Return true to continue execution - the object was simply not marked.
         * This is not a fatal error, just means this reference field contains a non-GC value. */
        return true;
    }
    
    /* Log valid object being added */
    GCObjectHeader* header = (GCObjectHeader*)ptr - 1;
    GC_DEBUG("[GC_PUSH] VALID ptr=%p, header=%p, size=%u, type=%d, marked=%d",
            ptr, header, header->size, header->type, header->marked);

    /* v34.59 PERF: объект уже помечен — не кладём на стек вовсе.
     * Прежний порядок (push -> pop -> gc_mark_object -> «уже помечен»)
     * тратил push/pop/разыменования на КАЖДУЮ повторную ссылку, а их
     * в живых графах 3D-игр большинство (Object[]/Vector/Entry-цепочки
     * ссылаются на одни и те же объекты по много раз). Семантика не
     * меняется: marked проверялся в gc_mark_object ДО обработки полей,
     * т.е. повторный pop ничего не делал, кроме этой проверки. Мир
     * остановлен — чтение marked безопасно. */
    if (header->marked) return true;
    
    /* Проверка на переполнение и расширение */
    if (gc_mark_stack_top >= gc_mark_stack_capacity) {
        if (gc_mark_stack_capacity >= GC_MARK_STACK_MAX_SIZE) {
            ERROR_LOG("Mark stack overflow (max %d entries)", GC_MARK_STACK_MAX_SIZE);
            return false;  /* Переполнение */
        }
        
        /* Расширяем стек */
        int new_capacity = gc_mark_stack_capacity * 2;
        if (new_capacity > GC_MARK_STACK_MAX_SIZE) {
            new_capacity = GC_MARK_STACK_MAX_SIZE;
        }
        
        MarkStackEntry* new_stack = (MarkStackEntry*)realloc(gc_mark_stack, 
                                                              new_capacity * sizeof(MarkStackEntry));
        if (!new_stack) {
            ERROR_LOG("Failed to expand mark stack");
            return false;
        }
        
        gc_mark_stack = new_stack;
        gc_mark_stack_capacity = new_capacity;
        GC_DEBUG("Expanded mark stack to %d entries", new_capacity);
    }
    
    gc_mark_stack[gc_mark_stack_top++].ptr = ptr;
    return true;
}

static void gc_mark_object(void* ptr) {
    if (!ptr) return;
    if (!is_heap_ptr(ptr)) return;
    
    /* Получаем GC заголовок (он находится перед пользовательскими данными) */
    GCObjectHeader* header = (GCObjectHeader*)ptr - 1;
    
    /* Проверка валидности */
    if (header->size == 0 || header->size > heap.size) {
        return; /* Коррумпированный указатель */
    }
    
    if (header->marked) return; /* Уже помечен */
    
    /* CRITICAL: Log when we mark an object */
    GC_LOG("[GC_MARK] Marking object %p (header=%p): size=%u, type=%d, clazz=%s\n",
            ptr, header, header->size, header->type,
            header->clazz ? (header->clazz->class_name ? header->clazz->class_name : "?") : "NULL");
    
    header->marked = 1;
    
    /* Рекурсивная пометка ссылок */
    switch (header->type) {
        case OBJ_TYPE_OBJECT: {
            JavaObject* obj = (JavaObject*)ptr;
            JavaClass* clazz = obj->header.clazz;

            /* v34.80 ARMv7 crash fix (reproduced under qemu-arm, Stalker,
             * ~40-60% of runs): a recycled heap block whose stale header
             * still parses as OBJ_TYPE_OBJECT with a plausible size walks
             * into gc_class_kind() with header->clazz pointing at garbage
             * (observed: pointers into the recycled GC-heap block holding
             * small ints 0x19/0x65; then strcmp(clazz->class_name,
             * "java/lang/String") faults in libc). Real JavaClass structs
             * live in C memory — malloc'd by classfile.c/jvm.c (stubs) —
             * and are NEVER allocated inside the GC heap, and no real
             * pointer can be below the mmap floor. Validate the class
             * pointer with exactly those two invariants and treat a
             * failing object as opaque: keep the mark (conservative — the
             * block survives this cycle), skip all class-specific walking.
             * Same philosophy as the String value_ref guard below. */
            if (!clazz) break;
            if (is_heap_ptr((void*)clazz) || (uintptr_t)clazz < 0x10000u) {
                gc_recycled_headers_skipped++;
                break;
            }

            /* v34.59 PERF: разметка класса (слоты ссылочных полей + биты
             * семейств) — строится ОДИН раз на класс. Прежний код делал
             * все strcmp/strstr по имени класса и построение массива
             * hierarchy[64] НА КАЖДЫЙ объект. */
            uint8_t kind = gc_class_kind(clazz);
            
            /* CRITICAL FIX: Special handling for java/lang/String objects.
             * String objects have a 'value' field that points to a char[].
             * We must validate and mark this array FIRST to prevent use-after-free.
             */
            if (kind & GC_KIND_STRING) {
                GC_LOG("[GC_STRING_OBJ] Marking java/lang/String object at %p\n", ptr);
                
                /* Get the value slot for the char array */
                int value_slot = native_get_string_value_slot(g_jvm_for_instanceof);
                
                if (value_slot >= 0) {
                    void* value_ref = obj->fields[value_slot].ref;
                    if (value_ref) {
                        /* Validate the char array before marking */
                        if (is_valid_gc_object(value_ref)) {
                            GC_LOG("[GC_STRING_OBJ]   value field (char[]) = %p (valid), marking\n", value_ref);
                            gc_push_mark(value_ref);
                        } else {
                            /* CRITICAL: Invalid char array reference! */
                            GC_LOG("[GC_STRING_OBJ]   CRITICAL: value field = %p (INVALID! use-after-free detected!)\n", value_ref);
                            
                            if (is_heap_ptr(value_ref)) {
                                GCObjectHeader* array_header = (GCObjectHeader*)value_ref - 1;
                                GC_LOG("[GC_STRING_OBJ]   Array header: magic=0x%08X, type=%d, size=%u\n",
                                        array_header->magic, array_header->type, array_header->size);
                                
                                if (array_header->type == OBJ_TYPE_FREE) {
                                    GC_LOG("[GC_STRING_OBJ]   ERROR: Char array was already freed! Clearing field.\n");
                                    obj->fields[value_slot].ref = NULL;
                                    gc_strings_fixed_this_cycle++;  /* Count this fix */
                                }
                            } else {
                                GC_LOG("[GC_STRING_OBJ]   Pointer outside heap - clearing field\n");
                                obj->fields[value_slot].ref = NULL;
                                gc_strings_fixed_this_cycle++;  /* Count this fix */
                            }
                        }
                    }
                }
                /* Continue to normal field processing for other fields */
            }
            
            /* CRITICAL FIX: Special handling for collection classes
             * These classes store references in internal arrays that may not
             * be properly detected by field scanning. We handle them explicitly.
             * 
             * java/util/Hashtable: table field -> Entry[] -> Entry.key, Entry.value
             * java/util/Vector: elementData field -> Object[]
             * java/util/ArrayList: elementData field -> Object[]
             */
            {
                /* java/util/Hashtable$Entry - has key, value, next fields */
                if (kind & GC_KIND_ENTRY) {
                    /* Entry objects have key (Object), value (Object), next (Entry) fields
                     * Layout: typically key=slot 0, value=slot 1, next=slot 2
                     * We need to mark all three */
                    GC_LOG("[GC_COLLECTION] Marking Entry object %p\n", ptr);
                    for (int e_slot = 0; e_slot < 4 && e_slot < (int)(header->size / sizeof(JavaValue)); e_slot++) {
                        void* ref = obj->fields[e_slot].ref;
                        if (ref) {
                            GC_LOG("[GC_COLLECTION]   Entry field[%d] = %p, marking\n", e_slot, ref);
                            gc_push_mark(ref);
                        }
                    }
                    break;
                }
                
                /* java/util/Hashtable - scan table array and ALL reference fields */
                if (kind & GC_KIND_HT) {
                    GC_LOG("[GC_HASHTABLE] Special handling for %s at %p, size=%u\n", clazz->class_name ? clazz->class_name : "?", ptr, header->size);
                    
                    int num_slots = (int)(header->size / sizeof(JavaValue));
                    GC_LOG("[GC_HASHTABLE]   Scanning %d field slots...\n", num_slots);
                    
                    /* CRITICAL FIX: Scan ALL reference fields, not just arrays!
                     * Hashtable may store entries as:
                     * 1. Entry[] array (OBJ_TYPE_ARRAY with element_type=OBJECT)
                     * 2. Linked list Entry objects (OBJ_TYPE_OBJECT with clazz=Hashtable$Entry)
                     * We need to mark ALL reference fields to catch both cases. */
                    for (int s = 0; s < num_slots; s++) {
                        void* ref = obj->fields[s].ref;
                        GC_LOG("[GC_HASHTABLE]   field[%d] = %p\n", s, ref);
                        
                        if (ref && is_heap_ptr(ref)) {
                            GCObjectHeader* ref_hdr = (GCObjectHeader*)ref - 1;
                            if (ref_hdr->magic == GC_HEADER_MAGIC) {
                                GC_LOG("[GC_HASHTABLE]     -> valid object: type=%d, size=%u, clazz=%s\n",
                                        ref_hdr->type, ref_hdr->size,
                                        ref_hdr->clazz ? (ref_hdr->clazz->class_name ? ref_hdr->clazz->class_name : "?") : "NULL");
                                
                                /* Mark ANY reference field - this handles both arrays and Entry objects */
                                gc_push_mark(ref);
                            } else {
                                GC_LOG("[GC_HASHTABLE]     -> INVALID MAGIC (not a GC object)\n");
                            }
                        } else if (ref) {
                            GC_LOG("[GC_HASHTABLE]     -> not a heap pointer\n");
                        }
                    }
                    /* Continue to normal field processing as well for completeness */
                }
            }
            
            /* v34.59 PERF: маркировка полей по кэшированной разметке класса.
             * БЫЛО (v34.58): построение hierarchy[64] + проход по всем полям
             * всех классов иерархии с разыменованием descriptor и strcmp
             * nativePeer НА КАЖДЫЙ объект. СТАЛО: цикл по готовому массиву
             * ссылочных слотов. Семантика идентична (см. gc_class_kind). */
            if (kind & GC_KIND_READY) {
                const uint16_t* slots = clazz->gc_ref_slots;
                int nslots = clazz->gc_ref_slot_count;
                for (int i = 0; i < nslots; i++) {
                    void* ref = obj->fields[slots[i]].ref;
                    if (ref) gc_push_mark(ref);
                }
            } else {
                /* Резервный медленный проход (разметка не построена:
                 * malloc слотов не удался — ретрай на следующем GC). */
                gc_mark_fields_slow(obj, clazz);
            }

            /* CRITICAL FIX (v15): conservative scan for STUB objects.
             * Stub classes created by the emulator often have NO declared
             * fields, yet native code stores live Java references in their
             * raw fields[] slots (RecordEnumerationImpl holds the ids
             * int[], M3G/LCDUI stubs hold peer objects). Without this scan
             * those references are invisible to the mark phase and the
             * referenced objects get freed while still in use
             * (use-after-free on the next native access). */
            if (clazz->fields_count == 0) {
                GCObjectHeader* obj_hdr = (GCObjectHeader*)ptr - 1;
                if (obj_hdr->magic == GC_HEADER_MAGIC &&
                    obj_hdr->size > sizeof(GCObjectHeader) + sizeof(JavaObject)) {
                    int stub_slots = (int)((obj_hdr->size - sizeof(GCObjectHeader)
                                            - sizeof(JavaObject)) / sizeof(JavaValue));
                    if (stub_slots > 16) stub_slots = 16;  /* sanity cap */
                    for (int s = 0; s < stub_slots; s++) {
                        void* ref = obj->fields[s].ref;
                        if (ref && is_heap_ptr(ref)) {
                            GCObjectHeader* ref_hdr = (GCObjectHeader*)ref - 1;
                            if (ref_hdr->magic == GC_HEADER_MAGIC &&
                                ref_hdr->type != OBJ_TYPE_FREE) {
                                GC_LOG("[GC_STUB] %s@%p field[%d] -> %p (type=%d) marked conservatively\n",
                                       clazz->class_name ? clazz->class_name : "?",
                                       ptr, s, ref, ref_hdr->type);
                                gc_push_mark(ref);
                            }
                        }
                    }
                }
            }
            break;
        }
        
        case OBJ_TYPE_ARRAY: {
            JavaArray* array = (JavaArray*)ptr;
            /* CRITICAL DEBUG: Log array marking details */
            GC_DEBUG("[GC_ARRAY] Marking array %p: length=%d, element_type=%d ('%c'), sizeof(JavaArray)=%zu",
                    ptr, array->length, array->element_type, 
                    (array->element_type >= 32 && array->element_type < 127) ? (char)array->element_type : '?',
                    sizeof(JavaArray));
            
            if (array->element_type == DESC_OBJECT || array->element_type == DESC_ARRAY) {
                void** refs = (void**)((uint8_t*)array + sizeof(JavaArray));
                GC_DEBUG("[GC_ARRAY] Array %p is object/array array, refs at %p, scanning %d elements",
                        ptr, refs, array->length);
                int marked_count = 0;
                for (jsize i = 0; i < array->length; i++) {
                    if (refs[i]) {
                        GC_DEBUG("[GC_ARRAY] Array %p element[%d] = %p, marking...", ptr, i, refs[i]);
                        bool result = gc_push_mark(refs[i]);
                        if (result) marked_count++;
                    }
                }
                GC_DEBUG("[GC_ARRAY] Array %p: marked %d/%d elements", ptr, marked_count, array->length);
            } else {
                GC_DEBUG("[GC_ARRAY] Array %p is primitive (type=%d), no references to mark", 
                        ptr, array->element_type);
            }
            break;
        }
        
        case OBJ_TYPE_CLASS: {
             /* Статические поля классов */
            if (header->clazz && header->clazz->static_fields) {
                JavaClass* clazz = header->clazz;
                for (int i = 0; i < clazz->static_fields_count; i++) {
                    JavaStaticField* sf = &clazz->static_fields[i];
                    if (sf->descriptor && (sf->descriptor[0] == 'L' || sf->descriptor[0] == '[')) {
                        if (sf->value.ref) gc_push_mark(sf->value.ref);
                    }
                }
            }
            break;
        }
            
        case OBJ_TYPE_STRING: {
            /* CRITICAL FIX: OBJ_TYPE_STRING is for NATIVE strings with inline char data.
             * However, some String objects may have been created with OBJ_TYPE_STRING
             * type but still have clazz=java/lang/String and fields!
             * 
             * We need to check if this has clazz=java/lang/String - if so, treat it
             * as an object with fields, NOT as a native string!
             */
            GCObjectHeader* gc_header = (GCObjectHeader*)ptr - 1;
            JavaString* str = (JavaString*)ptr;
            
            /* CRITICAL FIX: If this "string" has a java/lang/String class, it's actually
             * a Java String object with fields (value, offset, count, hash).
             * Treat it as OBJ_TYPE_OBJECT and scan its fields! */
            if (gc_header->clazz && gc_header->clazz->class_name &&
                strcmp(gc_header->clazz->class_name, "java/lang/String") == 0) {
                /* This is a JAVA String object with fields, NOT a native string!
                 * Scan its reference fields. */
                GC_LOG("[GC_STRING_FIX] String object at %p has clazz=java/lang/String, treating as object with fields\n", ptr);
                
                JavaObject* obj = (JavaObject*)ptr;
                JavaClass* clazz = gc_header->clazz;
                
                /* CRITICAL FIX: Get the value slot to access the char array.
                 * We need to validate the char array BEFORE trying to mark it.
                 * This prevents use-after-free when the array was already collected. */
                int value_slot = native_get_string_value_slot(g_jvm_for_instanceof);
                
                if (value_slot >= 0) {
                    void* value_ref = obj->fields[value_slot].ref;
                    if (value_ref) {
                        /* CRITICAL: Validate that the char array is still a valid heap object.
                         * If it's invalid (already freed or corrupted), we have a use-after-free bug!
                         * We should NOT try to mark it - just log the error and clear the field. */
                        if (is_valid_gc_object(value_ref)) {
                            GC_LOG("[GC_STRING_FIX]   field value = %p (valid), marking\n", value_ref);
                            gc_push_mark(value_ref);
                        } else {
                            /* CRITICAL BUG DETECTED: The char array is invalid!
                             * This means the array was freed before the String object.
                             * We cannot recover from this - the String is corrupted.
                             * Log the error and set value to NULL to prevent crashes. */
                            GC_LOG("[GC_STRING_FIX]   CRITICAL: field value = %p (INVALID! use-after-free detected!)\n", value_ref);
                            
                            /* Check if it looks like a freed object (deadbeef pattern) */
                            if (is_heap_ptr(value_ref)) {
                                GCObjectHeader* array_header = (GCObjectHeader*)value_ref - 1;
                                GC_LOG("[GC_STRING_FIX]   Array header: magic=0x%08X, type=%d, size=%u\n",
                                        array_header->magic, array_header->type, array_header->size);
                                
                                /* If the object is already freed (OBJ_TYPE_FREE), we have a serious bug */
                                if (array_header->type == OBJ_TYPE_FREE) {
                                    GC_LOG("[GC_STRING_FIX]   ERROR: Char array was already freed! String object is corrupted!\n");
                                    /* Clear the field to prevent further issues */
                                    obj->fields[value_slot].ref = NULL;
                                }
                            } else {
                                /* Pointer is outside heap - clear it */
                                GC_LOG("[GC_STRING_FIX]   Pointer outside heap - clearing field\n");
                                obj->fields[value_slot].ref = NULL;
                            }
                        }
                    }
                }
                
                /* Build hierarchy for remaining fields */
                JavaClass* hierarchy[64];
                int depth = 0;
                JavaClass* c = clazz;
                while (c && depth < 64) {
                    hierarchy[depth++] = c;
                    c = c->super_class;
                }
                
                /* Process fields from Object down to String (skip value - already handled) */
                int slot = 0;
                for (int h = depth - 1; h >= 0; h--) {
                    JavaClass* current = hierarchy[h];
                    if (!current->fields) continue;
                    
                    for (int i = 0; i < current->fields_count; i++) {
                        JavaField* field = &current->fields[i];
                        if (field->access_flags & ACC_STATIC) continue;
                        
                        if (field->descriptor) {
                            char desc = field->descriptor[0];
                            if (desc == 'L' || desc == '[') {
                                /* Skip the 'value' field - already handled above */
                                if (field->name && strcmp(field->name, "value") == 0) {
                                    slot++;
                                    continue;
                                }
                                void* ref = obj->fields[slot].ref;
                                if (ref && is_valid_gc_object(ref)) {
                                    GC_LOG("[GC_STRING_FIX]   field %s = %p, marking\n", 
                                            field->name ? field->name : "?", ref);
                                    gc_push_mark(ref);
                                }
                            }
                        }
                        
                        slot++;
                        if (field->descriptor && 
                            (field->descriptor[0] == 'J' || field->descriptor[0] == 'D')) {
                            slot++;
                        }
                    }
                }
            }
            
            /* Native strings have inline data, no separate array to mark.
             * The str->utf8 pointer (if set) points to malloc'd memory, not a heap object,
             * so we don't mark it either. */
            (void)str;  /* Suppress unused variable warning */
            break;
        }
        case OBJ_TYPE_FREE:
            /* Free block: never live, nothing to mark */
            break;
    }
}

static void gc_process_mark_stack(void) {
    while (gc_mark_stack_top > 0) {
        void* ptr = gc_mark_stack[--gc_mark_stack_top].ptr;
        gc_mark_object(ptr);
    }
}

/* Запуск сборщика мусора с Линейным Sweep и Коалесцингом */

/* [GC-DIAG] Walk the heap from start and dump each valid header until a
 * failing address, to identify which object's size field broke the linear
 * scan (one-shot, throttled). */
static void gc_diag_dump_alloc_ring(uint8_t* bad_ptr) {
    if (!g_alloc_ring) return;
    typedef struct { void* addr; uint32_t size; int type; const char* name; } RingEnt;
    RingEnt* ring = (RingEnt*)(void*)g_alloc_ring;
    ERROR_LOG("[GC-DIAG2] allocations adjacent to bad ptr %p (full ring scan):", (void*)bad_ptr);
    for (int k = 0; k < 512; k++) {
        RingEnt* e = &ring[k];
        if (!e->addr) continue;
        if (bad_ptr && (uint8_t*)e->addr < bad_ptr && (uint8_t*)e->addr + e->size + 128 > bad_ptr) {
            ERROR_LOG("[GC-DIAG2]   NEAR alloc %p size=%u type=%d clazz=%s (ends 0x%zx)",
                      e->addr, e->size, e->type, e->name,
                      (size_t)((uint8_t*)e->addr) + e->size);
        }
    }
    ERROR_LOG("[GC-DIAG2] layout: sizeof(GCHeader)=%zu size@%zu type@%zu clazz@%zu magic@%zu | sizeof(JavaArray)=%zu sizeof(JavaObject)=%zu sizeof(ObjectHeader)=%zu",
              sizeof(GCObjectHeader),
              (size_t)((uint8_t*)&((GCObjectHeader*)0)->size - (uint8_t*)0),
              (size_t)((uint8_t*)&((GCObjectHeader*)0)->type - (uint8_t*)0),
              (size_t)((uint8_t*)&((GCObjectHeader*)0)->clazz - (uint8_t*)0),
              (size_t)((uint8_t*)&((GCObjectHeader*)0)->magic - (uint8_t*)0),
              sizeof(JavaArray), sizeof(JavaObject), sizeof(ObjectHeader));
}

static void gc_diag_dump_heap_walk(uint8_t* bad_ptr) {
    static int diag_done = 0;
    if (diag_done) return;
    diag_done = 1;
    ERROR_LOG("[GC-DIAG] ===== heap walk dump until %p (heap.start=%p, current=%p) =====",
              (void*)bad_ptr, (void*)heap.start, (void*)heap.current);
    uint8_t* p = heap.start;

    /* circular buffer of the last N walked positions */
    uint8_t* last_pos[24];
    uint32_t last_size[24];
    int last_type[24];
    uint32_t last_magic[24];
    const char* last_name[24];
    int ring = 0;
    int total = 0;
    while (p < heap.current && total < 200000) {
        GCObjectHeader* h = (GCObjectHeader*)p;
        if (h->magic == GC_HEADER_MAGIC || h->type == OBJ_TYPE_FREE) {
            const char* cname = (h->clazz && h->clazz->class_name) ? h->clazz->class_name : "?";
            last_pos[ring] = p;
            last_size[ring] = h->size;
            last_type[ring] = h->type;
            last_magic[ring] = h->magic;
            last_name[ring] = cname;
            ring = (ring + 1) % 24;
            total++;
            p += h->size;
        } else {
            ERROR_LOG("[GC-DIAG] BAD HEADER while walking: @%p magic=0x%08X size=%u type=%d (objects walked OK: %d)",
                      (void*)p, h->magic, h->size, h->type, total);
            break;
        }
    }
    /* print the last 24 walked objects, in order */
    int idx = ring;
    ERROR_LOG("[GC-DIAG] last %d objects walked before failure:", (total < 24) ? total : 24);
    for (int i = 0; i < 24 && i < total; i++) {
        idx = (idx + 1) % 24;
        ERROR_LOG("[GC-DIAG]   obj[-%d] @%p size=%u type=%d magic=0x%08X clazz=%s",
                  (total < 24 ? total : 24) - i,
                  (void*)last_pos[idx], last_size[idx], last_type[idx],
                  last_magic[idx], last_name[idx]);
    }
    ERROR_LOG("[GC-DIAG] walk stopped at %p; bad scan_ptr was %p", (void*)p, (void*)bad_ptr);
    /* hexdump 64 bytes at the stop position */
    uint8_t* dump = p;
    for (int row = 0; row < 4; row++) {
        char line[130]; int pos = 0;
        pos += snprintf(line + pos, sizeof(line) - pos, "[GC-DIAG]   %p:", (void*)(dump + row*16));
        for (int b = 0; b < 16; b++) {
            pos += snprintf(line + pos, sizeof(line) - pos, " %02X", dump[row*16 + b]);
        }
        ERROR_LOG("%s", line);
    }
    ERROR_LOG("[GC-DIAG] ===== end heap walk dump =====");
}

static volatile int gc_diag_active = 0;
static int gc_diag_expected = 0;
static void jvm_gc_safepoint_release_local(void) {
    extern void jvm_gc_safepoint_release(void);
    jvm_gc_safepoint_release();
}

/* v41 PERF-DIAG: NOJME_GCSTAMP=1 — one line per GC with pause duration and
 * bytes freed. Cheap (no frames walked, no heap dump) — safe to keep on in
 * stutter profiling runs where full NOJME_LOG distorts the timings. */
static int gc_stamp_on(void) {
    static int env_cache = -1;
    if (env_cache < 0) {
        const char* e = getenv("NOJME_GCSTAMP");
        env_cache = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    return env_cache;
}

void gc_collect(JVM* jvm) {
    if (!jvm) return;
    /* v34.95 SAFEPOINT-BACKOFF entry gate: skip collections inside the
     * post-abort window (see the comment at g_gc_backoff_until_ms). */
    {
        struct timespec bk_ts;
        clock_gettime(CLOCK_MONOTONIC, &bk_ts);
        jlong bk_now = (jlong)bk_ts.tv_sec * 1000 + bk_ts.tv_nsec / 1000000;
        if (bk_now < g_gc_backoff_until_ms) {
            static int bk_log_n = 0;
            if (bk_log_n < 10 || (bk_log_n % 200) == 0) {
                bk_log_n++;
                LOG_SAFE("[GC-BACKOFF] collection skipped (%lld ms left in window)\n",
                         (long long)(g_gc_backoff_until_ms - bk_now));
            }
            return;
        }
    }
    /* v34.71 DIAG 2: epoch marker for the full allocation log (see
     * heap_init_block). Everything allocated after this point coexists
     * with the post-GC survivors — overlaps inside one epoch are bugs. */
    {
        FILE* alloc_log = heap_diag_alloc_log();
        if (alloc_log) fprintf(alloc_log, "G\n");
    }
    /* v34.59 TLAB: закрыть СВОЙ чанк ДО остановки мира — mark/sweep
     * должны видеть только закрытые чанки (хвост — валидный свободный
     * блок, инвариант №3). TLS-операция, лок не нужен; идемпотентно
     * (парковка потока могла закрыть чанк чуть раньше). */
    heap_tlab_flush_self();
    struct timespec gc_stamp_t0;
    int gc_stamp = gc_stamp_on();
    /* v35.03: t0 captured ALWAYS — the pause accounting feeds the diag
     * fields gms=/gxm= and the [GC-SLOW] watchdog, not just NOJME_GCSTAMP. */
    clock_gettime(CLOCK_MONOTONIC, &gc_stamp_t0);
    /* v34.43 PERF-DIAG: global GC collection counter (read racily by the
     * headless SLOWFRAME telemetry — a plain increment is fine). */
    g_gc_collections++;
    {
        /* v34.9 GC-STALL diag */
        static volatile int gc_diag_n = 0;
        int n = ++gc_diag_n;
        if (n <= 60 || (n % 100) == 0) {
            extern JavaThread* thread_current(JVM*);
            JavaThread* t = thread_current(jvm);
            LOG_SAFE("[GCDIAG] enter gc_collect tid=%d n=%d\n", t ? t->id : -1, n);
            if (t) {
                /* v34.9: walk the CALLER'S OWN frames (safe) */
                JavaFrame* f = t->current_frame;
                int d = 0;
                while (f && d < 6) {
                    LOG_SAFE("[GCDIAG]   [%d] %s.%s pc=%u\n", d,
                             f->clazz && f->clazz->class_name ? f->clazz->class_name : "?",
                             f->method && f->method->name ? f->method->name : "?",
                             (unsigned)f->pc);
                    f = f->prev; d++;
                }
            }
        }
        __sync_fetch_and_add(&gc_diag_active, 1);
    }
    LOG_SAFE("[GCDIAG] pre-lock: heap owner_valid=%d owner_tid=%d serving=%d next=%d\n",
             g_heap_owner_valid, g_heap_owner_tid, heap_ticket_serving, heap_ticket_next);
    /* UNCONDITIONAL LOG: Always log GC start for debugging */
    GC_LOG("[GC_TRIGGER] gc_collect() called! heap.current=%p, heap.end=%p, used=%zu bytes\n",
            (void*)heap.current, heap.end, (size_t)(heap.current - heap.start));
    
    /* CRITICAL: Acquire heap lock for GC
     * This is a RECURSIVE mutex, so if gc_collect is called from
     * heap_alloc (which already holds the lock), this is safe. */
    heap_lock();
    
    GC_DEBUG("========== gc_collect() START ==========");
    GC_DEBUG("heap.start=%p, heap.current=%p, heap.end=%p",
            (void*)heap.start, heap.current, heap.end);
    GC_DEBUG("heap.allocated=%zu, heap.size=%zu", heap.allocated, heap.size);
    
    /* ИСПРАВЛЕНО: Защита от повторного входа в GC */
    static bool gc_in_progress = false; jvm_gc_safepoint_release_local();
    if (gc_in_progress) {
        WARN_LOG("GC already in progress, skipping recursive collection");
        heap_unlock();  /* Release lock before returning */
        gc_pause_account(&gc_stamp_t0);
        return;
    }
    gc_in_progress = true;

    /* v34.9 SAFEPPOINT: stop the (pthread) world before walking the heap.
     * Other VM threads park at their next safepoint poll (interpreter loop,
     * monitor wait/enter, heap_lock wait). Bounded wait: a thread stuck in
     * a long native cannot hang the GC. */
    {
        extern volatile int g_gc_safepoint_request;
        extern int jvm_gc_safepoint_arrived(void);
        extern void jvm_gc_safepoint_release(void);
        extern int jvm_live_vm_thread_count(void);
        extern int jvm_current_os_thread_is_vm_runner(void);
        extern int jvm_gc_safepoint_wait_arrivals(int expected, int timeout_ms);
        /* v34.26 CENSUS FIX: `live - 1` assumed the GC caller is one of the
         * pthread runners. When gc_collect() runs on the FRONTEND thread
         * (libretro retro_run drives the main Java thread via execute_frame
         * on the host's thread), the caller is NOT a runner — every live
         * runner must park, and live-1 let one mutator keep running through
         * the sweep (torn-heap risk). Subtract one ONLY if the caller is
         * actually a registered runner. */
        int expected = jvm_live_vm_thread_count();
        if (jvm_current_os_thread_is_vm_runner()) expected -= 1;  /* self */
        /* v41 CRITICAL (torn-heap fix, see execute.c): the MAIN Java thread
         * runs on the frontend/headless OS thread and is NOT in the pthread
         * registry — runner-triggered collections never counted it, so the
         * mark/sweep ran while the main mutator kept interpreting (freed
         * live objects => random SIGSEGV / black / corrupted rendering on
         * weakly-ordered armv7). Count it whenever a driver window is
         * actively executing it (it parks at the 64-instruction safepoint
         * poll); skip when it is the CALLER (System.gc() from the MIDlet
         * itself — thread_current() resolves to main_thread there) or is
         * quiescent between frontend frames. */
        {
            extern JavaThread* thread_current(JVM* jvm);
            extern volatile int g_jvm_main_thread_executing;
            if (g_jvm_main_thread_executing &&
                jvm->main_thread && jvm->main_thread->current_frame &&
                thread_current(jvm) != jvm->main_thread) {
                expected += 1;
            }
        }
        gc_diag_expected = expected;
        if (expected > 0) {
            g_gc_safepoint_request = 1;
            /* v34.59 PERF: разбудить СПЯЩИХ (Thread.sleep / кусочные ожидания)
             * МГНОВЕННО — раньше поток в 50-мс куске nanosleep'а проверял
             * флаг только на границе куска, и каждый System.gc() 3D-игры
             * с циклом sleep+gc ждал «застреддера» до 50 мс (fmx: ~40 мс
             * sp_wait на каждый вызов — половина кадрового бюджета). */
            {
                extern void jvm_gc_safepoint_wake_sleepers(void);
                jvm_gc_safepoint_wake_sleepers();
                /* v34.65: wake MONITOR waiters too (Object.wait chunks on
                 * mon->wait_cond) — same rationale as the sleepers above,
                 * but these were only noticed at chunk boundaries: up to
                 * 50ms of pause padding per waiting thread on every
                 * collection in v34.61 (longer with the v34.65 default
                 * 250ms chunk — which is why this wake is mandatory). */
                extern void jvm_gc_safepoint_wake_monitors(void);
                jvm_gc_safepoint_wake_monitors();
            }
            /* v34.11 SAFEPOINT-INTEGRITY FIX.
             * The old code waited only 300ms for the world to stop and then
             * ran the mark-sweep ANYWAY while stragglers (a thread inside a
             * long native: Image.createImage/PNG decode, Thread.sleep,
             * Thread.join, RMS/media I/O — none of which used to poll the
             * safepoint) were still mutating JVM frames and the heap.
             * The sweep then freed objects those threads still referenced
             * -> use-after-free -> the "random hangs in random places"
             * reported on the Windows build (and occasionally on POSIX).
             *
             * New policy: wait up to 2s (generous; stragglers now poll from
             * sleep/join too), then ABORT this collection instead of
             * corrupting the heap. The allocation that triggered the GC
             * simply re-runs it on the next failure once the straggler
             * reaches a poll — worst case a fully-starved heap throws the
             * spec-mandated (and catchable) OutOfMemoryError.
             *
             * v34.26 PERF (armv7 stutter): the wait itself was a 5ms
             * nanosleep poll loop — EVERY GC paused >= 5ms even when all
             * mutators parked in microseconds (30% of a 60fps frame budget
             * burnt on sleep granularity = the periodic stutter / frame
             * drops). Now waits on the safepoint condition variable: park()
             * broadcasts on each arrival, so the collector wakes the instant
             * the last mutator parks. */
            int arrived = jvm_gc_safepoint_wait_arrivals(expected, 2000);
            if (gc_stamp) {
                /* v50 DIAG (Asphalt 3 3D stall): split the GC pause into
                 * "waiting for the world to park" vs actual mark/sweep — a
                 * long sp_wait means one mutator is stuck inside a native
                 * (M3G render / image decode / I/O) while everyone else is
                 * already parked; surfaced via [GCSTAMP] sp_wait. */
                struct timespec spw_t1;
                clock_gettime(CLOCK_MONOTONIC, &spw_t1);
                g_gc_last_sp_wait_ms =
                    (spw_t1.tv_sec - gc_stamp_t0.tv_sec) * 1000.0 +
                    (spw_t1.tv_nsec - gc_stamp_t0.tv_nsec) / 1e6;
            }
            if (arrived < expected) {
                /* SAFE ABORT: never sweep with live mutators. */
                LOG_SAFE("[GC-SAFEPOINT-TIMEOUT] only %d/%d VM threads reached "
                         "the safepoint after 2000ms — GC ABORTED (no sweep, "
                         "heap untouched; the allocation will retry).\n",
                         arrived, expected);
                /* v34.95: arm the backoff window so the immediate retry
                 * storm cannot turn every allocation into a 2 s stop-the-
                 * world livelock while the straggler sits in its native. */
                {
                    struct timespec ab_ts;
                    clock_gettime(CLOCK_MONOTONIC, &ab_ts);
                    jlong ab_now = (jlong)ab_ts.tv_sec * 1000 + ab_ts.tv_nsec / 1000000;
                    g_gc_backoff_until_ms = ab_now + 400;
                }
                /* v34.11: name the stragglers via the last-native tracker */
                {
                    extern void jvm_dump_all_threads(void);
                    jvm_dump_all_threads();
                }
                gc_in_progress = false;
                jvm_gc_safepoint_release_local();
                heap_unlock();
                {
                    int n2 = __sync_fetch_and_add(&gc_diag_active, -1);
                    (void)n2;
                }
                gc_pause_account(&gc_stamp_t0);
                return;
            }
        }
    }
    
    /* Reset string fix counter for this GC cycle */
    gc_strings_fixed_this_cycle = 0;
    
    /* ИСПРАВЛЕНО: Инициализация стека маркировки при первом вызове */
    if (!gc_mark_stack) {
        if (!gc_mark_stack_init()) {
            ERROR_LOG("Failed to initialize mark stack");
            gc_in_progress = false; jvm_gc_safepoint_release_local();
            heap_unlock();  /* Release lock before returning */
            gc_pause_account(&gc_stamp_t0);
            return;
        }
    }
    
    gc_mark_stack_top = 0;
    
    GC_DEBUG("root_count=%zu, thread_count=%d, class_count=%zu",
            heap.root_count, jvm->thread_count, jvm->class_loader.count);
    
    LOG_SAFE("[GCSTAGE] n=%d pre-mark heap_used=%zu\n", __sync_fetch_and_add(&gc_diag_active, 0), (size_t)(heap.current - heap.start));
    /* 1. Mark Phase */
    /* Mark from Roots */
    int roots_pushed = 0;
    for (size_t i = 0; i < heap.root_count; i++) {
        if (heap.roots[i] && *heap.roots[i]) {
            GC_DEBUG("[GC_ROOT] root[%zu] = %p", i, *heap.roots[i]);
            if (gc_push_mark(*heap.roots[i])) {
                roots_pushed++;
            }
        }
    }
    
    /* Mark from Stacks */
    int stack_refs_pushed = 0;
    for (int i = 0; i < jvm->thread_count; i++) {
        JavaThread* thread = jvm->threads[i];
        if (!thread) {
            GC_LOG("[GC_THREAD] thread[%d] is NULL, skipping\n", i);
            continue;
        }
        
        GC_LOG("[GC_THREAD] thread[%d]: pending_exception=%p, thread_object=%p, current_frame=%p\n",
                i, thread->pending_exception, thread->thread_object, (void*)thread->current_frame);
        
        if (thread->pending_exception) {
            if (gc_push_mark(thread->pending_exception)) stack_refs_pushed++;
        }
        if (thread->thread_object) {
            if (gc_push_mark(thread->thread_object)) stack_refs_pushed++;
        }
        /* FIX (audit S-9, v18): thread_group was never marked and could be
         * collected out from under live threads */
        if (thread->thread_group) {
            if (gc_push_mark(thread->thread_group)) stack_refs_pushed++;
        }
        
        JavaFrame* frame = thread->current_frame;
        int frame_num = 0;
        if (!frame) {
            GC_LOG("[GC_FRAME] thread[%d] has NO current_frame!\n", i);
        }
        while (frame) {
            GC_LOG("[GC_FRAME] thread[%d] frame[%d]: method=%s, max_locals=%d, stack_top=%d\n",
                    i, frame_num, 
                    (frame->method && frame->method->name) ? frame->method->name : "?",
                    frame->max_locals, frame->stack_top);
            
            /* ИСПРАВЛЕНО: Сканируем locals с учетом long/double (2 слота)
             * Long и double занимают 2 слота, но это ОДНО значение.
             * Второй слот (index+1) не является отдельной ссылкой.
             * Для locals это менее критично, т.к. .ref будет 0 для high-word,
             * но для корректности пропускаем второй слот long/double.
             */
            for (uint16_t j = 0; j < frame->max_locals; j++) {
                if (frame->locals[j].ref) {
                    GC_DEBUG("[GC_LOCAL] thread[%d] frame[%d] local[%d] = %p",
                            i, frame_num, j, frame->locals[j].ref);
                    if (gc_push_mark(frame->locals[j].ref)) {
                        stack_refs_pushed++;
                    }
                }
            }
            
            /* ИСПРАВЛЕНО: Сканируем стек с учетом long/double (2 слота)
             * JVM Spec: long и double занимают 2 слота стека.
             * Второй слот содержит continuation (high word), а не ссылку.
             * Когда long/double пушится на стек, оба слота содержат данные value,
             * поэтому .ref в high-word не будет валидным указателем.
             * Но для безопасности пропускаем второй слот явно.
             */
            for (int16_t j = 0; j <= frame->stack_top; j++) {
                if (frame->stack[j].ref) {
                    GC_DEBUG("[GC_STACK] thread[%d] frame[%d] stack[%d] = %p",
                            i, frame_num, j, frame->stack[j].ref);
                    if (gc_push_mark(frame->stack[j].ref)) {
                        stack_refs_pushed++;
                    }
                }
            }
            frame = frame->prev;
            frame_num++;
        }

        /* v36.03 NATIVE-ARGS-ROOTED: mark the receiver+args of every native
         * call currently in progress on this thread. The invoke wrappers
         * popped those slots from the java frame before dispatching, so the
         * frame scan above cannot see them; without this a GC triggered
         * INSIDE a native (bytecode toString, jvm_new_string, jvm_new_array,
         * ...) could sweep the receiver's whole object graph while the
         * native still walks it through C locals. gc_push_mark validates
         * every slot against the heap range, so primitive arguments
         * (ints/floats sharing the JavaValue union) are skipped safely. */
        for (int d = 0; d < thread->native_arg_depth; d++) {
            if (d >= NATIVE_ARG_FRAMES_MAX) break;
            JavaValue* nargs = thread->native_arg_frames[d].args;
            int ncnt = thread->native_arg_frames[d].count;
            if (!nargs || ncnt <= 0) continue;
            for (int k = 0; k < ncnt; k++) {
                if (nargs[k].ref) {
                    if (gc_push_mark(nargs[k].ref)) {
                        stack_refs_pushed++;
                    }
                }
            }
        }
    }
    
    /* Mark from Statics */
    int static_refs_pushed = 0;
    if (jvm->config.verbose_gc) {
        LOG_SAFE("[GC] Scanning %zu classes for static fields\n", jvm->class_loader.count);
    }
    for (size_t i = 0; i < jvm->class_loader.count; i++) {
        JavaClass* clazz = jvm->class_loader.classes[i];
        if (!clazz) {
            continue;
        }
        
        /* CRITICAL: Check if this class has static_fields allocated */
        if (!clazz->static_fields) {
            /* No static fields storage allocated - skip */
            continue;
        }
        
        if (clazz->static_fields_count == 0) {
            /* Has storage but no fields yet - skip */
            continue;
        }
        
        /* DEBUG: Log classes with static fields */
        if (jvm->config.verbose_gc) {
            LOG_SAFE("[GC]   class[%zu] = %s has %d static fields\n", 
                    i, clazz->class_name ? clazz->class_name : "?", clazz->static_fields_count);
        }
        
        for (int j = 0; j < clazz->static_fields_count; j++) {
            JavaStaticField* sf = &clazz->static_fields[j];
            
            /* DEBUG: Log all static fields to see descriptors */
            if (jvm->config.verbose_gc) {
                LOG_SAFE("[GC]   field %s.%s descriptor='%s' value.ref=%p\n",
                        clazz->class_name ? clazz->class_name : "?",
                        sf->name ? sf->name : "?",
                        sf->descriptor ? sf->descriptor : "NULL",
                        sf->value.ref);
            }
            
            if (sf->descriptor && (sf->descriptor[0] == 'L' || sf->descriptor[0] == '[')) {
                if (sf->value.ref) {
                    GC_LOG("[GC_STATIC] %s.%s = %p (attempting to mark)\n",
                            clazz->class_name ? clazz->class_name : "?",
                            sf->name ? sf->name : "?",
                            sf->value.ref);
                    if (gc_push_mark(sf->value.ref)) {
                        static_refs_pushed++;
                        GC_LOG("[GC_STATIC]   -> marked successfully (count=%d)\n", static_refs_pushed);
                    } else {
                        GC_LOG("[GC_STATIC]   -> FAILED to mark!\n");
                    }
                }
            }
        }
    }
    
    /* CRITICAL: Mark objects stored in native Hashtable entries!
     * The Hashtable implementation uses native memory (g_hashtables) to store
     * key-value pairs. These are NOT Java heap objects, so normal field scanning
     * won't find them. We must explicitly mark them here.
     * 
     * This fixes the use-after-free bug where strings stored as Hashtable keys
     * were being garbage collected because GC didn't see them.
     */
    int native_ht_refs = 0;
    GC_LOG("[GC_NATIVE_HT] Marking native Hashtable entries (g_hashtable_count=%d)...\n", 
            g_hashtable_count);
    for (int i = 0; i < g_hashtable_count && i < GC_HASHTABLE_MAX_HASHTABLES; i++) {
        /* CRITICAL: Skip dead peers! When a Hashtable is freed, its peer is marked
         * as not alive. We must not scan freed memory (deadbeef pattern). */
        if (!g_hashtable_peer_alive[i]) {
            GC_LOG("[GC_NATIVE_HT]   Skipping dead Hashtable[%d]\n", i);
            continue;
        }
        
        HashtablePeerGC* peer = &g_hashtables[i];
        if (!peer->entries || peer->count <= 0) continue;
        
        GC_LOG("[GC_NATIVE_HT]   Hashtable[%d]: capacity=%d, count=%d\n", 
                i, peer->capacity, peer->count);
        
        for (int j = 0; j < peer->capacity; j++) {
            HashtableEntryGC* entry = &peer->entries[j];
            if (!entry->key && !entry->value) continue;
            
            GC_LOG("[GC_NATIVE_HT]     entry[%d]: key=%p, value=%p\n",
                    j, entry->key, entry->value);
            
            if (entry->key) {
                if (gc_push_mark(entry->key)) {
                    native_ht_refs++;
                    GC_LOG("[GC_NATIVE_HT]       marked key\n");
                }
            }
            if (entry->value) {
                if (gc_push_mark(entry->value)) {
                    native_ht_refs++;
                    GC_LOG("[GC_NATIVE_HT]       marked value\n");
                }
            }
        }
    }
    GC_LOG("[GC_NATIVE_HT] Marked %d native Hashtable references\n", native_ht_refs);
    
    /* CRITICAL: Mark interned strings from String.intern() pool!
     * The intern_pool in native.c holds references to interned strings.
     * These strings must NOT be garbage collected, or String.intern() will
     * return freed objects causing crashes.
     * 
     * This fixes the crash in GCTest during OOM recovery where interned
     * strings like "key", "value" were being collected.
     */
    extern JavaString* intern_pool[];
    extern int intern_pool_count;
    int intern_refs = 0;
    #define INTERN_POOL_SIZE_MAX 1024
    GC_LOG("[GC_INTERN] Marking interned strings (intern_pool_count=%d)...\n", intern_pool_count);
    for (int i = 0; i < intern_pool_count && i < INTERN_POOL_SIZE_MAX; i++) {
        JavaString* str = intern_pool[i];
        if (str && is_heap_ptr(str)) {
            /* Verify the header is valid before marking */
            GCObjectHeader* header = (GCObjectHeader*)((uint8_t*)str - sizeof(GCObjectHeader));
            if (header->magic == GC_HEADER_MAGIC && header->type != OBJ_TYPE_FREE) {
                if (gc_push_mark(str)) {
                    intern_refs++;
                    GC_LOG("[GC_INTERN]   marked intern_pool[%d] = %p\n", i, (void*)str);
                }
            } else {
                /* Invalid entry - clear it from the pool */
                GC_LOG("[GC_INTERN]   WARNING: intern_pool[%d] = %p has invalid header, clearing\n", i, (void*)str);
                intern_pool[i] = NULL;
            }
        }
    }
    GC_LOG("[GC_INTERN] Marked %d interned string references\n", intern_refs);
    
    /* CRITICAL: Mark objects stored in M3G object registry!
     * The M3G (Mobile 3D Graphics) system stores loaded 3D objects in a global
     * registry for find() lookup. These must be treated as GC roots or they
     * will be collected while still referenced by the registry.
     */
    int m3g_count = 0;
    int m3g_refs = 0;
    JavaObject** m3g_objects = m3g_registry_get_objects(&m3g_count);
    GC_LOG("[GC_M3G] Marking M3G registry objects (count=%d)...\n", m3g_count);
    if (m3g_objects && m3g_count > 0) {
        for (int i = 0; i < m3g_count; i++) {
            JavaObject* obj = m3g_objects[i];
            if (obj && is_heap_ptr(obj)) {
                GCObjectHeader* header = (GCObjectHeader*)((uint8_t*)obj - sizeof(GCObjectHeader));
                if (header->magic == GC_HEADER_MAGIC && header->type != OBJ_TYPE_FREE) {
                    if (gc_push_mark(obj)) {
                        m3g_refs++;
                        GC_LOG("[GC_M3G]   marked registry[%d] = %p\n", i, (void*)obj);
                    }
                } else {
                    /* Invalid/corrupted object - clear from registry */
                    GC_LOG("[GC_M3G]   registry[%d] = %p has invalid header, clearing\n", i, (void*)obj);
                    m3g_objects[i] = NULL;
                }
            }
        }
    }
    GC_LOG("[GC_M3G] Marked %d M3G registry references\n", m3g_refs);

    /* v34.18: extra native M3G GC roots — the pending-render queue
     * (nodes/transforms captured before bindTarget), the bound camera and
     * target image, the cached last World and the active Light objects.
     * These are raw JavaObject* slots in C globals the GC previously could
     * not see; a collection turned them into dangling pointers (the v15
     * heap_java_object_valid() guards only caught the symptoms). Mark the
     * real roots now and clear slots that point at dead objects. */
    {
        int extra_n = 0;
        JavaObject*** extra_slots = m3g_gc_extra_roots(&extra_n);
        int extra_marked = 0;
        int extra_cleared = 0;
        for (int i = 0; i < extra_n; i++) {
            JavaObject* obj = *(extra_slots[i]);
            if (obj && is_heap_ptr(obj)) {
                GCObjectHeader* header = (GCObjectHeader*)((uint8_t*)obj - sizeof(GCObjectHeader));
                if (header->magic == GC_HEADER_MAGIC && header->type != OBJ_TYPE_FREE) {
                    if (gc_push_mark(obj)) {
                        extra_marked++;
                        GC_LOG("[GC_M3G]   marked native root slot[%d] = %p\n", i, (void*)obj);
                    }
                } else {
                    *extra_slots[i] = NULL;
                    extra_cleared++;
                }
            } else if (obj) {
                *extra_slots[i] = NULL;
                extra_cleared++;
            }
        }
        GC_LOG("[GC_M3G] Marked %d extra native roots (cleared %d dead)\n",
               extra_marked, extra_cleared);
    }
    
    GC_DEBUG("Mark phase: roots_pushed=%d, stack_refs=%d, static_refs=%d, native_ht_refs=%d, intern_refs=%d, m3g_refs=%d, total_on_stack=%d",
            roots_pushed, stack_refs_pushed, static_refs_pushed, native_ht_refs, intern_refs, m3g_refs, gc_mark_stack_top);
    
    if (jvm->config.verbose_gc) {
        LOG_SAFE("[GC] Mark phase complete: roots=%d, stack_refs=%d, static_refs=%d, native_ht_refs=%d, intern_refs=%d, m3g_refs=%d, mark_stack_top=%d\n",
                roots_pushed, stack_refs_pushed, static_refs_pushed, native_ht_refs, intern_refs, m3g_refs, gc_mark_stack_top);
    }
    
    gc_process_mark_stack();
    
    if (jvm->config.verbose_gc) {
        LOG_SAFE("[GC] Mark stack processed, starting sweep...\n");
    }
    
    /* 1.5 PRE-SWEEP VALIDATION: Check all live String objects have marked char arrays.
     * This catches use-after-free bugs where the char array was freed before the String.
     * If we find an unmarked array, we must mark it to prevent crashes.
     *
     * v34.59 PERF (АДАПТИВНЫЙ ЗАПУСК): это ПОЛНЫЙ ВТОРОЙ обход кучи
     * (одна кэш-линия на каждый объект) — на 64МБ-куче Asphalt-профиля
     * с ~800k объектов это добавляло ~50% к стоимости sweep. Начиная
     * с v34.x mark-фаза САМА помечает value-массив живой строки
     * (gc_mark_object: GC_KIND_STRING-блок пушит value_ref до обхода
     * остальных полей), поэтому «живая строка с непомеченным массивом»
     * в текущей архитектуре невозможно — оба обхода идут под одним
     * heap_lock с остановленным миром, между ними ничего не меняется.
     * Политика: первые 3 GC и каждый 8-й — полный аудит (страховка от
     * регрессий и непредвиденных путей); любая найденная проблема
     * возвращает ежецикловый режим. Остальные GC пропускают обход.
     */
    static uint32_t presweep_clean_streak = 0;
    static uint32_t presweep_serial = 0;
    int strings_fixed = 0;
    int presweep_skipped = 0;
    presweep_serial++;
    if (presweep_clean_streak < 3 || (presweep_serial & 7u) == 0u) {
    GC_LOG("[GC_PRESWEEP] Validating String objects...\n");
    uint8_t* validate_ptr = heap.start;
    while (validate_ptr < (heap.end < heap.current ? heap.end : heap.current)) {
        GCObjectHeader* header = (GCObjectHeader*)validate_ptr;
        
        /* Skip invalid/corrupted headers */
        if (header->magic != GC_HEADER_MAGIC || header->size == 0 || header->size > heap.size) {
            validate_ptr += 8;
            continue;
        }
        
        /* Only check live (marked) objects that might be Strings.
         * v34.59: бит GC_KIND_STRING вместо strcmp на каждый объект.
         * (живой объект помечен => gc_mark_object => разметка класса
         * гарантированно построена). */
        if (header->marked && (header->type == OBJ_TYPE_OBJECT || header->type == OBJ_TYPE_STRING)) {
            if (header->clazz && (gc_class_kind(header->clazz) & GC_KIND_STRING)) {
                
                JavaObject* str_obj = (JavaObject*)(header + 1);
                int value_slot = native_get_string_value_slot(g_jvm_for_instanceof);
                
                if (value_slot >= 0) {
                    void* value_ref = str_obj->fields[value_slot].ref;
                    if (value_ref) {
                        /* Check if the char array is properly marked */
                        if (is_heap_ptr(value_ref)) {
                            GCObjectHeader* array_header = (GCObjectHeader*)value_ref - 1;
                            
                            /* Check if array header is valid */
                            if (array_header->magic == GC_HEADER_MAGIC &&
                                array_header->type != OBJ_TYPE_FREE) {
                                
                                if (!array_header->marked) {
                                    /* CRITICAL: Char array is not marked but String is live!
                                     * This is a bug - the array would be freed while the String survives.
                                     * We must mark it now to prevent use-after-free. */
                                    GC_LOG("[GC_PRESWEEP] FIXING: String %p has unmarked char array %p - marking it!\n",
                                            (void*)str_obj, value_ref);
                                    
                                    /* Mark the array */
                                    array_header->marked = 1;
                                    strings_fixed++;
                                    
                                    /* Also process its references if it's an object array */
                                    if (array_header->type == OBJ_TYPE_ARRAY) {
                                        JavaArray* arr = (JavaArray*)value_ref;
                                        if (arr->element_type == DESC_OBJECT || arr->element_type == DESC_ARRAY) {
                                            void** refs = (void**)((uint8_t*)arr + sizeof(JavaArray));
                                            for (jsize i = 0; i < arr->length; i++) {
                                                if (refs[i] && is_valid_gc_object(refs[i])) {
                                                    gc_push_mark(refs[i]);
                                                }
                                            }
                                        }
                                    }
                                }
                            } else {
                                /* Array header is invalid or already freed */
                                GC_LOG("[GC_PRESWEEP] WARNING: String %p has invalid char array %p (magic=0x%08X, type=%d)\n",
                                        (void*)str_obj, value_ref, array_header->magic, array_header->type);
                                
                                /* Clear the invalid reference */
                                str_obj->fields[value_slot].ref = NULL;
                            }
                        }
                    }
                }
            }
        }
        
        validate_ptr += header->size;
    }
    } else {
        presweep_skipped = 1;
    }
    
    /* Process any new objects pushed onto mark stack */
    if (gc_mark_stack_top > 0) {
        GC_LOG("[GC_PRESWEEP] Processing %d additional objects from fixup\n", gc_mark_stack_top);
        gc_process_mark_stack();
    }
    
    if (presweep_skipped) {
        presweep_clean_streak++;   /* пропуск = доверяем mark-фазе */
        GC_LOG("[GC_PRESWEEP] SKIPPED full pass (streak=%u)\n", presweep_clean_streak);
    } else {
        presweep_clean_streak = (strings_fixed > 0) ? 0 : presweep_clean_streak + 1;
    }
    GC_LOG("[GC_PRESWEEP] Validation complete: fixed %d String objects (streak=%u)\n",
           strings_fixed, presweep_clean_streak);
    
    LOG_SAFE("[GCSTAGE] pre-sweep\n");
    /* 2. Sweep Phase (Linear Scan + Coalescing) */
    size_t freed_this_cycle = 0;
    size_t objects_freed = 0;
    
    /* v34.58 PERF/FIX: чистим ВСЕ size-class бакеты и large-список —
     * sweep rebuild'ит их с нуля (инвариант №3). ВАЖНОЕ исправление
     * старого дефекта: прежний код блоки, УЖЕ свободные к моменту GC
     * (type==OBJ_TYPE_FREE), просто пропускал — но список-то он к этому
     * моменту уже обнулил. Такие блоки навсегда выпадали из учёта: память
     * в середине кучи становилась недоступной, ёмкость деградировала от
     * GC к GC (маскировалось top-rewind'ом). Теперь старые свободные
     * блоки вместе с новыми мёртвыми объектами попадают в новые
     * бакеты, соседние — coalesce'ятся на лету (адреса растут). */
    for (int c = 0; c < SC_NUM_BUCKETS; c++) heap.free_lists[c] = NULL;
    heap.large_free_list = NULL;
    
    uint8_t* scan_ptr = heap.start;
    
    /* v34 FIX (VmTest heap death after OOM stress): objects allocated from the
     * v19 EMERGENCY RESERVE legitimately live beyond heap.end (up to
     * heap.hard_end) - e.g. the pre-allocated OutOfMemoryError thrown by the
     * memory tests. The old sweep flagged any object crossing heap.end as
     * corruption, FAILED TO RECOVER (its recovery scan also refused objects
     * past heap.end) and ABORTED the whole sweep, leaving heap.current stuck
     * beyond heap.end forever: the next allocation computed
     * "Available: (size_t)-1024" and the VM was dead (every ldc/alloc failed).
     *
     * Sweep bounds: walk ONLY [heap.start, heap.end). Objects in the reserve
     * are emergency exception allocations - tiny, bounded by the reserve size,
     * and reclaimed nowhere (by design: they must stay valid while the heap is
     * saturated; the pre-allocated OOM singleton covers exhaustion). Walking
     * the reserve is NOT possible anyway: the linear header chain breaks at
     * every emergency free/alloc cycle, so any walk past heap.end eventually
     * reads a zeroed/garbage header ("corrupted magic 0x00000000"). A LIVE
     * object crossing heap.end is visited (mark bit cleared) and then the
     * loop stops cleanly at the boundary. */
    uint8_t* sweep_limit = (heap.end < heap.current) ? heap.end : heap.current;
    
    /* Absolute object bound for the size sanity check: a crossing emergency
     * object must fit into the real buffer. */
    uint8_t* gc_abs_end = (heap.hard_end && heap.hard_end > heap.end)
                          ? heap.hard_end : heap.end;
    
    /* v34.15 FIX (Galaxy on Fire eternal freeze at ~16 MB heap):
       the old code inserted every freed block into a SORTED list by walking
       it from the head (O(n) per insert). With a heap full of small dead
       objects (string churn: ~150k blocks at 16 MB) that is O(n^2) ~ 10^10
       pointer chases — the sweep "never" returns and the whole VM freezes
       mid-GC with no log output (THREADDUMP: running, native age growing).
       v34.58: region-логика — свободные блоки объединяются в регионы
       на лету (одним проходом, без второй фазы coalescing), каждый
       закрытый регион O(1) пушится в свой size-class бакет. */
    FreeBlock* region = NULL;        /* первый блок текущего свободного региона */
    size_t region_size = 0;          /* накопленный размер региона */
    FreeBlock* last_pushed = NULL;   /* последний ЗАКРЫТЫЙ регион (для top-rewind) */
    int last_pushed_cls = -1;
    
    while (scan_ptr < sweep_limit) {
        GCObjectHeader* header = (GCObjectHeader*)scan_ptr;
        
        /* Проверка magic number для обнаружения corruption */
        if (header->magic != GC_HEADER_MAGIC && header->type != OBJ_TYPE_FREE) {
            /* v34.2 FIX (VmTest OOM-loop log spam): under an exhausted heap the
             * repaint timer re-enters paint() every 250 ms, every failed alloc
             * triggers a GC, and every GC walked into the same recovered
             * zeroed-header region - dumping the FULL forensics ring each time
             * (hundreds of [GC-DIAG2] lines per second, the log grows without
             * bound). The corruption report itself is one-shot information:
             * keep the single-line warning for EVERY occurrence, but emit the
             * heavy heap-walk + alloc-ring dumps only for the first 3. */
            static int gc_diag_dump_budget = 3;
            ERROR_LOG("GC: Object at %p has corrupted magic: expected 0x%08X, got 0x%08X",
                     (void*)scan_ptr, GC_HEADER_MAGIC, header->magic);
            ERROR_LOG("  header->size=%u, type=%d, marked=%d", 
                     header->size, header->type, header->marked);
            if (gc_diag_dump_budget > 0) {
                gc_diag_dump_budget--;
                gc_diag_dump_heap_walk(scan_ptr);
                gc_diag_dump_alloc_ring(scan_ptr);
            } else if (gc_diag_dump_budget == 0) {
                gc_diag_dump_budget--;
                ERROR_LOG("[GC-DIAG2] further corruption dumps suppressed (forensics budget spent)");
            }
            
            HEAP_CORRUPTION_LOG("CORRUPTION_MAGIC: addr=%p, expected=0x%08X, got=0x%08X, size=%u, type=%d",
                    (void*)scan_ptr, GC_HEADER_MAGIC, header->magic, header->size, header->type);

            /* v34.71 CRITICAL FIX: SAFE ABORT instead of the recovery resync.
             *
             * The old "resync forward to the next valid-looking header" was
             * fundamentally unsound: after a desync the walk no longer knows
             * the block boundaries, so every free region it pushed afterwards
             * could cover LIVE objects. Empirically (Nescube, NOJME_ALLOC_LOG
             * forensics) that produced overlapping allocations — real objects
             * carved over live ones — and SIGSEGVs minutes later (the
             * user-reported settings-Cancel crash). Aborting is safe: the
             * regions pushed SO FAR are exact (chains of verified-dead
             * blocks); everything beyond the corruption point keeps its state
             * and is reclaimed by the next collection (which re-marks and
             * re-walks). The worst case is a bounded capacity leak, never a
             * structural corruption. */
            ERROR_LOG("  [GC] sweep SAFE-ABORT at corruption (no resync; "
                     "remainder reclaimed next collection)");
            if (region) {
                region->size = (uint32_t)region_size;
                region->next = NULL;
                last_pushed_cls = sc_push_free(region);
                last_pushed = region;
                region = NULL;
                region_size = 0;
            }
            break;
        }
        
        /* Защита от порчи памяти в куче */
        /* v34: объекты из emergency-резерва могут выходить за heap.end - это
         * НЕ corruption, если они укладываются в hard_end (см. комментарий выше). */
        if (header->size == 0 || header->size > heap.size ||
            scan_ptr + header->size > gc_abs_end) {
            ERROR_LOG("Corruption detected at %p, attempting recovery", scan_ptr);
            ERROR_LOG("  header->size=%u, heap.size=%zu", header->size, heap.size);
            ERROR_LOG("  scan_ptr=%p, heap.end=%p", scan_ptr, heap.end);
            ERROR_LOG("  header->type=%d, header->marked=%d", header->type, header->marked);
            
            HEAP_CORRUPTION_LOG("CORRUPTION_SIZE: addr=%p, size=%u, heap.size=%zu, type=%d, magic=0x%08X",
                    (void*)scan_ptr, header->size, heap.size, header->type, header->magic);
            
            if (header->clazz) {
                ERROR_LOG("  header->clazz=%p, class_name=%s", 
                         (void*)header->clazz, 
                         header->clazz->class_name ? header->clazz->class_name : "(null)");
                HEAP_CORRUPTION_LOG("CORRUPTION_CLAZZ: clazz=%p, name=%s",
                        (void*)header->clazz, header->clazz->class_name ? header->clazz->class_name : "(null)");
            }
            
            /* Попытка найти предыдущий объект для диагностики */
            uint8_t* prev_scan = heap.start;
            uint8_t* prev_obj_end = NULL;
            while (prev_scan < scan_ptr) {
                GCObjectHeader* prev_header = (GCObjectHeader*)prev_scan;
                if (prev_header->size == 0 || prev_header->size > heap.size) break;
                prev_obj_end = prev_scan + prev_header->size;
                prev_scan += prev_header->size;
            }
            if (prev_obj_end && prev_obj_end <= scan_ptr) {
                ERROR_LOG("  Previous object ended at %p, gap to corruption: %ld bytes",
                         (void*)prev_obj_end, (long)(scan_ptr - prev_obj_end));
                HEAP_CORRUPTION_LOG("CORRUPTION_GAP: prev_end=%p, gap=%ld bytes",
                        (void*)prev_obj_end, (long)(scan_ptr - prev_obj_end));
            }

            /* v34.71: SAFE ABORT — same rationale as the magic-corruption path
             * above (the resync could push free regions over live objects).
             * Close the open region (exact, verified-dead blocks only) and
             * stop sweeping; the remainder is reclaimed next collection. */
            ERROR_LOG("  [GC] sweep SAFE-ABORT at size corruption (no resync)");
            if (region) {
                region->size = (uint32_t)region_size;
                region->next = NULL;
                last_pushed_cls = sc_push_free(region);
                last_pushed = region;
                region = NULL;
                region_size = 0;
            }
            break;
        }
        
        /* v34.58: блок, уже свободный ДО этого GC, тоже попадает в новые
         * бакеты (раньше выпадал из учёта навсегда — см. комментарий в
         * начале sweep). Он может слиться с текущим свободным регионом. */
        if (header->type == OBJ_TYPE_FREE) {
            if (scan_ptr + header->size <= heap.end) {
                if (region && (uint8_t*)region + region_size == scan_ptr) {
                    region_size += header->size;   /* слияние на лету */
                } else {
                    if (region) {
                        region->size = (uint32_t)region_size;
                        region->next = NULL;
                        last_pushed_cls = sc_push_free(region);
                        last_pushed = region;
                    }
                    region = (FreeBlock*)header;
                    region_size = header->size;
                }
            }
            scan_ptr += header->size;
            continue;
        }
        
        if (header->marked || header->pinned) {
            /* Объект жив: сброс метки + закрыть свободный регион */
            header->marked = 0; /* Сброс метки */
            if (region) {
                region->size = (uint32_t)region_size;
                region->next = NULL;
                last_pushed_cls = sc_push_free(region);
                last_pushed = region;
                region = NULL;
                region_size = 0;
            }
        } else {
            /* Объект мертв - логируем для отладки */
            GC_LOG("[GC_SWEEP] FREEING object at %p (header=%p): size=%u, type=%d, clazz=%s\n",
                    (void*)(header + 1), header, header->size, header->type,
                    header->clazz ? (header->clazz->class_name ? header->clazz->class_name : "?") : "NULL");
            
            freed_this_cycle += header->size;
            objects_freed++;
            
            /* Очистка строкового кэша */
            if (header->type == OBJ_TYPE_STRING) {
                JavaString* str = (JavaString*)(header + 1);
                if (str->utf8) {
                    free(str->utf8);
                    str->utf8 = NULL;
                }
            }
            
            /* CRITICAL: Clean up native Hashtable peer when Hashtable is freed!
             * When a Java Hashtable object is garbage collected, we must free its
             * native peer (g_hashtables entry) to prevent:
             * 1. Memory leak of native entries array
             * 2. Use-after-free when GC scans freed native memory (deadbeef pattern)
             *
             * v34.59 PERF: бит GC_KIND_HT (strcmp имени класса — ОДИН раз
             * на класс при первой встрече) вместо strcmp НА КАЖДЫЙ мёртвый
             * объект: на churn-профилях умирают сотни тысяч объектов за GC.
             * gc_class_kind() здесь безопасен: sweep идёт под heap_lock с
             * остановленным миром; для класса без слотов это только
             * установка битов. */
            if (header->clazz && (gc_class_kind(header->clazz) & GC_KIND_HT)) {
                
                JavaObject* ht = (JavaObject*)(header + 1);
                
                /* v36.33 [HT-PEER-SWEEP]: free the peer OWNED by this object
                 * (ht_obj pointer match). The old code read the object's
                 * "threshold" field as a peer index — but "threshold" is a
                 * live Java field the class itself may set (initialCapacity
                 * semantics), so a random value freed ANOTHER live
                 * hashtable's peer (entries NULLed mid-flight, its future
                 * put/get silently dataless) while THIS object's own peer
                 * survived with dangling keys into the swept arena. */
                for (int pi = 0; pi < g_hashtable_count && pi < GC_HASHTABLE_MAX_HASHTABLES; pi++) {
                    if (g_hashtable_peer_alive[pi] && g_hashtables[pi].ht_obj == ht) {
                        GC_LOG("[GC_SWEEP] Freeing native Hashtable peer idx %d (pointer match)\n", pi);
                        hashtable_free_peer(pi);
                        break;
                    }
                }
            }
            
            /* Помечаем как свободный */
            header->type = OBJ_TYPE_FREE;
            /* v34.58: валидный magic у свободного блока — линейные обходы
             * (presweep-валидация String, heap_validate, heap_check_magic)
             * прыгают по size целыми регионами, а не сканируют внутренности
             * 8-байтовым шагом. Recovery-сканы отличают свободные блоки по
             * type > OBJ_TYPE_CLASS, так что они не путают их с живыми. */
            header->magic = GC_HEADER_MAGIC;
            
            /* v34: блоки, уходящие за heap.end (пересечение с emergency-
             * резервом), в free list НЕ добавляются - обычные аллокации
             * должны жить строго ниже heap.end (см. комментарий к sweep_limit). */
            if (scan_ptr + header->size <= heap.end) {
                if (region && (uint8_t*)region + region_size == scan_ptr) {
                    region_size += header->size;   /* слияние с предыдущим свободным */
                } else {
                    if (region) {
                        region->size = (uint32_t)region_size;
                        region->next = NULL;
                        last_pushed_cls = sc_push_free(region);
                        last_pushed = region;
                    }
                    region = (FreeBlock*)header;
                    region_size = header->size;
                }
            }
        }
        
        scan_ptr += header->size;
    }
    
    /* v34.58: закрываем хвостовой свободный регион (если остался) */
    if (region) {
        region->size = (uint32_t)region_size;
        region->next = NULL;
        last_pushed_cls = sc_push_free(region);
        last_pushed = region;
        region = NULL;
    }
    
    /* 3. Coalescing — НЕ нужен отдельным проходом: v34.58 сливает соседние
     * свободные блоки на лету прямо в sweep-цикле (регионы). Все бакеты
     * уже заполнены sc_push_free(). */
    
    /* Статистика. v34.59 FIX (race): вычитание атомарно для симметрии —
     * во время sweep мир остановлен (конкурентных fast-path добавлений
     * нет), но атомарность здесь дешёвая и страхует будущие изменения. */
    __sync_sub_and_fetch(&heap.allocated, freed_this_cycle);
    __sync_add_and_fetch(&heap.freed, freed_this_cycle);
    heap.gc_cycles++;
    heap.gc_total_freed += freed_this_cycle;
    
    /* Если мы освободили блок в конце кучи, можем подвинуть current назад
     * (Defragmentation of top).
     * v34.58: последний закрытый регион — самый высокий по адресу
     * (walk восходящий, он был запушен последним => голова своего
     * бакета), поэтому никакого хождения до хвоста списка не нужно:
     * просто проверяем, кончается ли он ровно на heap.current. */
    if (last_pushed && last_pushed_cls >= 0 &&
        (uint8_t*)last_pushed + last_pushed->size == heap.current) {
        int popped = 0;
        if (last_pushed_cls == SC_LARGE_IDX) {
            if (heap.large_free_list == last_pushed) {
                heap.large_free_list = last_pushed->next;
                popped = 1;
            }
        } else if (heap.free_lists[last_pushed_cls] == last_pushed) {
            heap.free_lists[last_pushed_cls] = last_pushed->next;
            popped = 1;
        }
        if (popped) {
            /* Память возвращена bump-области (указатель current уменьшен) */
            heap.current = (uint8_t*)last_pushed;
        }
        /* Если голова бакета не совпала (не должно случаться — пуш был
         * последним) — оставляем блок в бакете, rewind не делаем. */
    }

    if (freed_this_cycle > 0) {
        GC_DEBUG("Cycle %zu: freed %zu bytes (%zu objects), %zu bytes allocated",
                heap.gc_cycles, freed_this_cycle, objects_freed, heap.allocated);
    }
    
    /* Notify native code about GC completion.
     * This allows native fallback buffers (e.g., StringBuffer) to clean up
     * entries for objects that were garbage collected.
     */
    extern void native_gc_notify(void);
    native_gc_notify();
    
    GC_DEBUG("========== gc_collect() END ==========");

    /* Сброс флага GC */
    gc_in_progress = false; jvm_gc_safepoint_release_local();
    /* v34.11: the counter was decremented twice here (once in the diag block
     * and once after heap_unlock), driving it negative over time. Decrement
     * exactly once and keep the single diagnostic line. */
    {
        int n = __sync_fetch_and_add(&gc_diag_active, -1);
        extern JavaThread* thread_current(JVM*);
        JavaThread* t2 = thread_current(jvm);
        LOG_SAFE("[GCDIAG] exit  gc_collect tid=%d n=%d\n", t2 ? t2->id : -1, n);
        if (gc_stamp) {
            struct timespec gc_stamp_t1;
            clock_gettime(CLOCK_MONOTONIC, &gc_stamp_t1);
            long dur_us = (gc_stamp_t1.tv_sec - gc_stamp_t0.tv_sec) * 1000000L +
                          (gc_stamp_t1.tv_nsec - gc_stamp_t0.tv_nsec) / 1000;
            char line[160];
            int ln = snprintf(line, sizeof(line),
                    "[GCSTAMP] dur=%.1fms sp_wait=%.1fms used=%zukb freed=%zukb cycles=%zu heap=%zukb\n",
                    dur_us / 1000.0, g_gc_last_sp_wait_ms,
                    (size_t)(heap.current - heap.start) / 1024,
                    freed_this_cycle / 1024, heap.gc_cycles,
                    (size_t)(heap.end - heap.start) / 1024);
            if (ln > 0) fwrite(line, 1, (size_t)ln, stderr);
        }
    }
    
    /* CRITICAL: Release heap lock after GC completes */
    /* v35.04 PERIODIC GC: rebase the allocation budget on the post-GC
     * watermark. Only the MAIN successful path rebases; the backoff /
     * recursive-skip early returns must not (they collected nothing). */
    g_gc_alloc_at_last_gc = heap.allocated;
    gc_pause_account(&gc_stamp_t0);
    heap_unlock();
}

/* --- Roots and Utilities --- */

void gc_add_root(JVM* jvm, void** root) {
    (void)jvm;
    
    /* CRITICAL FIX: Acquire heap lock before modifying roots array
     * Without this, concurrent GC could read the roots array while
     * we're reallocating it, causing memory corruption. */
    heap_lock();
    
    /* v35.08: idempotent registration - the same slot registered twice
     * within one heap lifetime must not create two entries (marking twice
     * is harmless, but callers relied on "registered" flags instead, which
     * broke re-registration after the per-session roots reset). Linear scan
     * is fine: the live root list is small (dozens, rarely hundreds). */
    for (size_t i = 0; i < heap.root_count; i++) {
        if (heap.roots[i] == root) {
            heap_unlock();
            return;
        }
    }
    
    if (heap.root_count >= heap.root_capacity) {
        /* ИСПРАВЛЕНО: Проверка успешности realloc */
        size_t new_capacity = heap.root_capacity * 2;
        if (new_capacity == 0) new_capacity = 1024;  /* Защита от нуля */
        
        void*** new_roots = (void***)realloc(heap.roots, new_capacity * sizeof(void**));
        if (!new_roots) {
            ERROR_LOG("Failed to expand roots array");
            heap_unlock();
            return;
        }
        heap.roots = new_roots;
        heap.root_capacity = new_capacity;
    }
    heap.roots[heap.root_count++] = root;
    
    heap_unlock();
}

void gc_remove_root(JVM* jvm, void** root) {
    (void)jvm;
    
    /* CRITICAL FIX: Acquire heap lock before modifying roots array */
    heap_lock();
    
    for (size_t i = 0; i < heap.root_count; i++) {
        if (heap.roots[i] == root) {
            heap.roots[i] = heap.roots[--heap.root_count];
            heap_unlock();
            return;
        }
    }
    
    heap_unlock();
}

/* v35.08 MULTI-SESSION: see include/heap.h for the contract. NULLs every
 * registered root slot (all of them live in static storage - see the
 * header comment for why that is safe), then empties the list. The roots
 * ARRAY itself stays allocated (heap_destroy frees it; heap_init
 * re-creates it on the next session). */
void gc_roots_reset_all(void) {
    heap_lock();
    
    for (size_t i = 0; i < heap.root_count; i++) {
        if (heap.roots[i]) {
            *heap.roots[i] = NULL;
        }
    }
    heap.root_count = 0;
    
    heap_unlock();
}

void gc_pin(JVM* jvm, void* object) {
    (void)jvm;
    if (!object || !is_heap_ptr(object)) return;
    GCObjectHeader* header = (GCObjectHeader*)object - 1;
    header->pinned = 1;
}

void gc_unpin(JVM* jvm, void* object) {
    (void)jvm;
    if (!object || !is_heap_ptr(object)) return;
    GCObjectHeader* header = (GCObjectHeader*)object - 1;
    header->pinned = 0;
}

HeapStats heap_get_stats(JVM* jvm) {
    HeapStats stats = {0};
    (void)jvm;
    stats.total_size = heap.size;
    stats.used_size = heap.allocated;
    stats.free_size = heap.size - heap.allocated;
    stats.object_count = 0; /* Сложно считать точно без счетчика при аллокации, можно добавить */
    stats.gc_cycles = heap.gc_cycles;
    stats.gc_time_ms = 0;
    return stats;
}

/* Объектные утилиты */

JavaClass* object_get_class(void* object) {
    if (!object) return NULL;
    
    /* Check if pointer is in heap */
    if (!is_heap_ptr(object)) return NULL;
    
    GCObjectHeader* header = (GCObjectHeader*)object - 1;
    
    /* Validate header before accessing */
    if (header->size == 0 || header->size > 64 * 1024 * 1024) {
        return NULL;  /* Invalid object */
    }
    if (header->type > OBJ_TYPE_CLASS) {
        return NULL;  /* Invalid type */
    }
    
    return header->clazz;
}

size_t object_get_size(void* object) {
    if (!object || !is_heap_ptr(object)) return 0;
    GCObjectHeader* header = (GCObjectHeader*)object - 1;
    return header->size - sizeof(GCObjectHeader);
}

jint object_hash_code(void* object) {
    if (!object) return 0;
    JavaObject* obj = (JavaObject*)object;
    return obj->header.hashcode;
}

bool object_is_array(void* object) {
    if (!object || !is_heap_ptr(object)) return false;
    GCObjectHeader* header = (GCObjectHeader*)object - 1;
    return header->type == OBJ_TYPE_ARRAY;
}

bool object_is_string(void* object) {
    if (!object || !is_heap_ptr(object)) return false;
    GCObjectHeader* header = (GCObjectHeader*)object - 1;
    return header->type == OBJ_TYPE_STRING;
}

/*
 * CRITICAL FIX: Avoid malloc/strdup in hot path of object_instance_of
 * 
 * Problem: object_instance_of can be called very frequently (every checkcast, instanceof).
 * Allocating memory in this hot path kills performance and risks memory leaks.
 * 
 * Solution: Use static buffers for temporary class name construction.
 * These buffers are NOT thread-safe, but neither is the rest of the heap code.
 */

/* Static buffer for computed array class name - sized for max reasonable class name */
#define INSTANCEOF_BUFFER_SIZE 512
static char instanceof_buffer[INSTANCEOF_BUFFER_SIZE];

bool object_instance_of(void* object, JavaClass* clazz) {
    if (!object || !clazz) return false;
    
    /* CRITICAL: Check if pointer is in heap before accessing GC header */
    if (!is_heap_ptr(object)) {
        /* Not a heap object - could be a Class object or invalid pointer 
         * For safety, just return false for any non-heap pointer.
         * Class objects are handled separately in checkcast/instanceof. */
        return false;
    }
    
    /* Check GC header to determine object type */
    GCObjectHeader* gc_header = (GCObjectHeader*)object - 1;
    
    /* Validate GC header before using it */
    if (gc_header->size == 0 || gc_header->size > 64 * 1024 * 1024) {
        /* Invalid GC header - corrupted object */
        return false;
    }
    if (gc_header->type > OBJ_TYPE_CLASS) {
        /* Invalid object type */
        return false;
    }
    
    bool obj_is_array = (gc_header->type == OBJ_TYPE_ARRAY);
    
    /* Get the object's class */
    JavaClass* obj_class = object_get_class(object);
    
    /* Null check for class names */
    if (!clazz->class_name) return false;
    
    /* For arrays, check element type against expected array class name */
    if (obj_is_array) {
        /* All arrays are instanceof java/lang/Object */
        if (strcmp(clazz->class_name, "java/lang/Object") == 0) {
            return true;
        }
        
        /* Arrays implement Cloneable and Serializable interfaces */
        if (strcmp(clazz->class_name, "java/lang/Cloneable") == 0 ||
            strcmp(clazz->class_name, "java/io/Serializable") == 0) {
            return true;
        }
        
        /* Check if target class is an array type (starts with '[') */
        if (clazz->class_name[0] == '[') {
            JavaArray* array = (JavaArray*)object;
            
            /* For primitive arrays, do exact type matching */
            const char* expected_name = NULL;
            switch (array->element_type) {
                case T_BOOLEAN: expected_name = "[Z"; break;
                case T_BYTE:    expected_name = "[B"; break;
                case T_CHAR:    expected_name = "[C"; break;
                case T_SHORT:   expected_name = "[S"; break;
                case T_INT:     expected_name = "[I"; break;
                case T_LONG:    expected_name = "[J"; break;
                case T_FLOAT:   expected_name = "[F"; break;
                case T_DOUBLE:  expected_name = "[D"; break;
                case DESC_OBJECT:
                case DESC_ARRAY: {
                    /* For object arrays, we need to check covariance */
                    /* clazz->class_name is like "[Ljava/lang/String;" or "[[I" */
                    
                    /* Case 1: Exact match - check if element_class matches */
                    if (array->element_class && array->element_class->class_name) {
                        /* Build expected array class name from element class using static buffer
                         * CRITICAL FIX: No malloc - use static buffer instead */
                        const char* elem_name = array->element_class->class_name;
                        size_t elem_name_len = strlen(elem_name);
                        /* v24 FIX (Brick Breaker load hang): when the element is
                         * ITSELF an array, element_class->class_name is already a
                         * descriptor ("[I", "[Ljava/lang/String;", ...). The
                         * enclosing array's type name is "[" + elem_name ("[[I"),
                         * NOT "[L" + elem_name + ";" (which produced the nonsense
                         * "[L[I;" and failed every checkcast/instanceof on
                         * int[][]/String[][] walls -> infinite exception cascade
                         * that killed the game's loader ~1/3 through). */
                        if (elem_name[0] == '[') {
                            if (elem_name_len + 2 <= INSTANCEOF_BUFFER_SIZE) {
                                instanceof_buffer[0] = '[';
                                memcpy(instanceof_buffer + 1, elem_name, elem_name_len + 1);
                                if (strcmp(clazz->class_name, instanceof_buffer) == 0) {
                                    return true;
                                }
                            }
                        } else if (elem_name_len + 4 <= INSTANCEOF_BUFFER_SIZE) {
                            instanceof_buffer[0] = '[';
                            instanceof_buffer[1] = 'L';
                            memcpy(instanceof_buffer + 2, elem_name, elem_name_len);
                            instanceof_buffer[elem_name_len + 2] = ';';
                            instanceof_buffer[elem_name_len + 3] = '\0';
                            
                            if (strcmp(clazz->class_name, instanceof_buffer) == 0) {
                                return true;
                            }
                        }
                        
                        /* Case 2: Covariance - Object[] can hold any reference array */
                        if (strcmp(clazz->class_name, "[Ljava/lang/Object;") == 0) {
                            return true;
                        }
                        
                        /* Case 3: Check if element type is subtype of target element type */
                        const char* target_class_name = clazz->class_name;
                        if (target_class_name[0] == '[' && target_class_name[1] == 'L') {
                            /* Extract element class name into static buffer */
                            size_t target_len = strlen(target_class_name + 2);
                            if (target_len < INSTANCEOF_BUFFER_SIZE) {
                                memcpy(instanceof_buffer, target_class_name + 2, target_len);
                                char* semicolon = strchr(instanceof_buffer, ';');
                                if (semicolon) *semicolon = '\0';
                                
                                /* Load target element class and check assignability */
                                extern JVM* g_jvm_for_instanceof;
                                JavaClass* target_elem_class = g_jvm_for_instanceof ? 
                                    jvm_load_class(g_jvm_for_instanceof, instanceof_buffer) : NULL;
                                
                                if (target_elem_class && array->element_class) {
                                    /* Check if array's element class is assignable to target element class */
                                    JavaClass* check = array->element_class;
                                    while (check) {
                                        if (check == target_elem_class ||
                                            (check->class_name && target_elem_class->class_name &&
                                             strcmp(check->class_name, target_elem_class->class_name) == 0)) {
                                            return true;
                                        }
                                        check = check->super_class;
                                    }
                                }
                            }
                        }
                    } else {
                        /* No element_class stored - assume generic Object[] for DESC_OBJECT arrays */
                        if (strcmp(clazz->class_name, "[Ljava/lang/Object;") == 0) {
                            return true;
                        }
                    }
                    break;
                }
            }
            
            /* Exact match for primitive array types */
            if (expected_name && strcmp(clazz->class_name, expected_name) == 0) {
                return true;
            }
        }
        
        /* Array type doesn't match */
        return false;
    }
    
    if (!obj_class) return false;
    
    /* Walk up the class hierarchy and compare by name */
    JavaClass* current = obj_class;
    while (current) {
        /* Compare by class name instead of pointer */
        if (current->class_name && clazz->class_name) {
            if (strcmp(current->class_name, clazz->class_name) == 0) {
                return true;
            }
        }
        
        /* Also check pointer equality as fast path */
        if (current == clazz) return true;
        
        current = current->super_class;
    }
    
    /* Check if clazz is an interface and obj_class implements it */
    /* We need to check all interfaces in the hierarchy */
    current = obj_class;
    while (current) {
        /* Check interfaces implemented by this class */
        for (int i = 0; i < current->interfaces_count; i++) {
            const char* iface_name = classfile_get_class_name(current, current->interfaces[i]);
            if (iface_name && clazz->class_name && strcmp(iface_name, clazz->class_name) == 0) {
                return true;
            }
            /* Also check if the interface extends the target interface */
            if (iface_name) {
                /* Get JVM from global context - need to pass it or use a different approach */
                /* For now, use a helper function that takes JVM parameter */
                extern JVM* g_jvm_for_instanceof;
                JavaClass* iface_class = g_jvm_for_instanceof ? jvm_load_class(g_jvm_for_instanceof, iface_name) : NULL;
                if (iface_class && iface_class != clazz) {
                    /* Recursively check parent interfaces */
                    JavaClass* iface_check = iface_class;
                    while (iface_check) {
                        for (int j = 0; j < iface_check->interfaces_count; j++) {
                            const char* parent_iface = classfile_get_class_name(iface_check, iface_check->interfaces[j]);
                            if (parent_iface && clazz->class_name && strcmp(parent_iface, clazz->class_name) == 0) {
                                return true;
                            }
                        }
                        /* Interfaces can only extend other interfaces, not classes */
                        iface_check = iface_check->super_class;  /* This would be Object or another interface */
                    }
                }
            }
        }
        
        current = current->super_class;
    }
    
    return false;
}

/* Массивы */

jsize array_length(JavaArray* array) {
    return array ? array->length : 0;
}

uint8_t array_element_type(JavaArray* array) {
    return array ? array->element_type : 0;
}

static inline void* array_data_ptr(JavaArray* array) {
    return (void*)((uint8_t*)array + sizeof(JavaArray));
}

JavaValue array_get(JavaArray* array, jsize index) {
    JavaValue v = { .raw = 0 };
    if (!array || index < 0 || index >= array->length) return v;
    
    /* Memory barrier for thread visibility - ensure we see writes from other threads */
    __sync_synchronize();
    
    void* data = array_data_ptr(array);
    switch (array->element_type) {
        case T_BOOLEAN:
            v.i = ((uint8_t*)data)[index]; 
            break;
        case T_BYTE:    
            /* CRITICAL FIX: Sign-extend byte to int for Java semantics */
            v.i = (jint)((int8_t*)data)[index]; 
            break;
        case T_CHAR:    v.i = ((jchar*)data)[index]; break;
        case T_SHORT:   v.i = ((jshort*)data)[index]; break;
        case T_INT:     v.i = ((jint*)data)[index]; break;
        case T_LONG:    v.j = ((jlong*)data)[index]; break;
        case T_FLOAT:   v.f = ((jfloat*)data)[index]; break;
        case T_DOUBLE:  v.d = ((jdouble*)data)[index]; break;
        case DESC_OBJECT:
        case DESC_ARRAY: v.ref = ((void**)data)[index]; break;
    }
    return v;
}

void array_set(JavaArray* array, jsize index, JavaValue value) {
    if (!array) {
        ERROR_LOG("array_set: NULL array");
        return;
    }
    if (index < 0 || index >= array->length) {
        ERROR_LOG("array_set: index %d out of bounds (length=%d)", index, array->length);
        return;
    }
    
    /* Проверка magic number для обнаружения corruption */
    GCObjectHeader* gc_hdr = (GCObjectHeader*)array - 1;
    if (gc_hdr->magic != GC_HEADER_MAGIC) {
        ERROR_LOG("array_set: FATAL - array at %p has corrupted magic (0x%08X), skipping write", 
                 (void*)array, gc_hdr->magic);
        return;
    }
    
    void* data = array_data_ptr(array);
    switch (array->element_type) {
        case T_BOOLEAN:
        case T_BYTE:    ((uint8_t*)data)[index] = (uint8_t)value.i; break;
        case T_CHAR:    ((jchar*)data)[index] = (jchar)value.i; break;
        case T_SHORT:   ((jshort*)data)[index] = (jshort)value.i; break;
        case T_INT:     ((jint*)data)[index] = value.i; break;
        case T_LONG:    ((jlong*)data)[index] = value.j; break;
        case T_FLOAT:   ((jfloat*)data)[index] = value.f; break;
        case T_DOUBLE:  ((jdouble*)data)[index] = value.d; break;
        case DESC_OBJECT:
        case DESC_ARRAY: ((void**)data)[index] = value.ref; break;
    }
    
    /* Memory barrier for thread visibility - ensure other threads see our writes */
    __sync_synchronize();
}

void* array_get_ref(JavaArray* array, jsize index) {
    if (!array || index < 0 || index >= array->length) return NULL;
    return ((void**)array_data_ptr(array))[index];
}

void array_set_ref(JavaArray* array, jsize index, void* ref) {
    if (!array || index < 0 || index >= array->length) return;
    ((void**)array_data_ptr(array))[index] = ref;
}

/* Строки */

/* Helper to check if object is a valid GC-managed string (native or JavaObject) */
static inline bool is_valid_string_object(void* ptr) {
    if (!ptr) return false;
    if (!is_heap_ptr(ptr)) return false;
    
    GCObjectHeader* header = (GCObjectHeader*)ptr - 1;
    ObjectType type = header->type;
    
    /* CRITICAL: Reject freed objects and invalid types */
    if (type == OBJ_TYPE_FREE) {
        DEBUG_LOG("is_valid_string_object: REJECTED - object at %p was freed (type=OBJ_TYPE_FREE)", ptr);
        return false;
    }
    if (type > OBJ_TYPE_CLASS) {
        DEBUG_LOG("is_valid_string_object: REJECTED - invalid type %d for ptr=%p", type, ptr);
        return false;
    }
    
    return true;
}

/* Helper to check if object is a native string (OBJ_TYPE_STRING) */
static inline bool is_native_string(void* ptr) {
    if (!ptr) return false;
    if (!is_heap_ptr(ptr)) return false;
    
    GCObjectHeader* header = (GCObjectHeader*)ptr - 1;
    ObjectType type = header->type;
    
    /* CRITICAL: Freed objects should not be treated as strings */
    if (type == OBJ_TYPE_FREE) {
        DEBUG_LOG("is_native_string: REJECTED - object at %p was freed!", ptr);
        return false;
    }
    
    DEBUG_LOG("is_native_string: ptr=%p, header=%p, type=%d, is_string=%d",
              ptr, header, type, type == OBJ_TYPE_STRING);
    return type == OBJ_TYPE_STRING;
}

/* String field indices are now dynamically looked up via native.c */

jsize string_length(JavaString* str) {
    if (!str) return 0;
    
    /* CRITICAL: Check if this is a valid heap pointer */
    if (!is_heap_ptr(str)) {
        return 0;
    }
    
    GCObjectHeader* gc_header = (GCObjectHeader*)str - 1;
    
    /* CRITICAL: Check if object was freed by GC */
    if (gc_header->type == OBJ_TYPE_FREE) {
        return 0;
    }
    
    /* Check if this is a native string (inline chars) or JavaObject with fields */
    if (gc_header->type == OBJ_TYPE_STRING) {
        return str->length;
    }
    
    /* It's a JavaObject - get count from field using proper slot lookup */
    if (gc_header->type == OBJ_TYPE_OBJECT) {
        JavaObject* obj = (JavaObject*)str;
        if (obj->header.clazz && g_jvm_for_instanceof) {
            int count_slot = native_get_string_count_slot(g_jvm_for_instanceof);
            if (count_slot >= 0 && count_slot < (int)(gc_header->size / sizeof(JavaValue))) {
                return obj->fields[count_slot].i;
            }
        }
    }
    
    return 0;
}

/* v36.09: every NULL this function hands out is a real field bug — stale
 * pointer, freed block, broken value field. This is the class of failure
 * that became the silent Doom RPG [Rus] "null.str" corruption (substring
 * -> NULL -> concat -> bogus resource name -> readFully spin). Name the
 * poison in the device trace (rate-limited: first 8 hits verbatim). */
static void string_chars_null_trace(const JavaString* str, const char* why) {
    static int n = 0;
    if (n >= 8) return;
    n++;
    {
        extern void sw_trace_force(const char* fmt, ...) __attribute__((weak));
        if (&sw_trace_force && sw_trace_force) {
            int in_heap = str ? (int)is_heap_ptr((const void*)str) : -1;
            int hdr_type = -1;
            if (in_heap == 1) {
                GCObjectHeader* h = ((GCObjectHeader*)str) - 1;
                hdr_type = (int)h->type;
            }
            sw_trace_force("[STR-CHARS-NULL] #%d str=%p why=%s in_heap=%d hdr_type=%d",
                           n, (const void*)str, why, in_heap, hdr_type);
        }
    }
}

const jchar* string_chars(JavaString* str) {
    if (!str) return NULL;
    
    /* CRITICAL: First check if this is a valid object (not freed) */
    if (!is_heap_ptr(str)) {
        string_chars_null_trace(str, "not-heap-ptr");
        return NULL;
    }
    
    GCObjectHeader* gc_header = (GCObjectHeader*)str - 1;
    
    /* CRITICAL: Check if object was freed by GC */
    if (gc_header->type == OBJ_TYPE_FREE) {
        string_chars_null_trace(str, "freed-block");
        return NULL;
    }
    
    /* Check if this is a native string with inline chars */
    if (gc_header->type == OBJ_TYPE_STRING) {
        return (const jchar*)((uint8_t*)str + sizeof(JavaString));
    }
    
    /* It's a JavaObject (type=OBJ_TYPE_OBJECT) - get char array from value field */
    if (gc_header->type == OBJ_TYPE_OBJECT) {
        JavaObject* obj = (JavaObject*)str;
        if (obj->header.clazz && g_jvm_for_instanceof) {
            int value_slot = native_get_string_value_slot(g_jvm_for_instanceof);
            if (value_slot >= 0 && value_slot < (int)(gc_header->size / sizeof(JavaValue))) {
                JavaArray* char_array = (JavaArray*)obj->fields[value_slot].ref;
                /* v34.80 ARMv7 crash fix (reproduced under qemu-arm, Stalker,
                 * ~60% of runs, SIGSEGV in libc with r0=0x65): a STALE str
                 * pointer (object moved/reused by a GC that ran while this
                 * C frame held the raw pointer) sails through the is_heap_ptr
                 * + non-FREE checks above, and fields[value_slot].ref then
                 * reads whatever now lives in that slot — observed 0x45,
                 * returned as (jchar*)(0x45 + 32) = 0x65 and dereferenced
                 * by the caller's libc string call. str itself is already
                 * validity-checked above; extend the SAME discipline to the
                 * char array before handing out array_data(): heap range,
                 * live non-FREE block, array type, T_CHAR elements. Valid
                 * strings take the same path as before; garbage now yields
                 * NULL, which every caller already handles ("if (!chars ...)"). */
                if (char_array && is_heap_ptr(char_array)) {
                    GCObjectHeader* arr_hdr = (GCObjectHeader*)char_array - 1;
                    if (arr_hdr->type == OBJ_TYPE_ARRAY &&
                        char_array->element_type == T_CHAR) {
                        return (const jchar*)array_data(char_array);
                    }
                }
            }
        }
    }
    
    string_chars_null_trace(str, "value-field-invalid");
    return NULL;
}

/*
 * CRITICAL FIX: Unified string_utf8 API
 * 
 * Previously, this function had ambiguous ownership:
 * - For native strings (OBJ_TYPE_STRING): returned internal cached buffer (caller MUST NOT free)
 * - For Java objects (OBJ_TYPE_OBJECT): returned malloc'd buffer (caller MUST free)
 * 
 * NEW BEHAVIOR:
 * - string_utf8(): Returns a pointer that caller MUST NOT free (callee owns).
 *   For Java objects, this uses a thread-local buffer that is reused on next call.
 *   This is safe for immediate use but NOT for storing the pointer.
 * 
 * - string_utf8_copy(): Always returns a malloc'd copy that caller MUST free.
 *   Use this when you need to store the string for later use.
 */

/* Thread-local buffer for string_utf8 — each thread gets its own buffer.
 * v34.29 FIX (JBenchmark 3D "text drawn twice"): the single reusable TLS
 * buffer made every returned pointer alias the PREVIOUS call's result.
 * Real MIDP renderers legitimately hold several strings at once — e.g.
 * render_stringitem reads Item label and Item text back-to-back and drew
 * the TEXT twice (label draw used the clobbered buffer). A ring of four
 * buffers keeps the documented "valid until next call" contract for
 * single-use callers while making the common label+text / title+text /
 * old+insert patterns safe. Callers that need a pointer to outlive four
 * further conversions must still use string_utf8_copy(). */
#define STRING_UTF8_TLS_SLOTS 4
static __thread char* string_utf8_tls_buffers[STRING_UTF8_TLS_SLOTS];
static __thread size_t string_utf8_tls_buffer_sizes[STRING_UTF8_TLS_SLOTS];
static __thread unsigned string_utf8_tls_ring = 0;

const char* string_utf8(JVM* jvm, JavaString* str) {
    (void)jvm;
    if (!str) return NULL;
    
    /* For native strings, use cached utf8 if available */
    if (is_native_string(str) && str->utf8) return str->utf8;
    
    jsize len = string_length(str);
    const jchar* chars = string_chars(str);
    if (!chars && len > 0) return NULL;
    
    /* Calculate required buffer size (max 3 bytes per UTF-16 code unit) */
    size_t required_size = (size_t)len * 3 + 1;
    
    /* For native strings, allocate permanent cache */
    if (is_native_string(str)) {
        char* result = (char*)malloc(required_size);
        if (!result) return NULL;
        
        char* p = result;
        for (jsize i = 0; i < len; i++) {
            jchar c = chars[i];
            if (c < 0x80) {
                *p++ = (char)c;
            } else if (c < 0x800) {
                *p++ = (char)(0xC0 | (c >> 6));
                *p++ = (char)(0x80 | (c & 0x3F));
            } else {
                *p++ = (char)(0xE0 | (c >> 12));
                *p++ = (char)(0x80 | ((c >> 6) & 0x3F));
                *p++ = (char)(0x80 | (c & 0x3F));
            }
        }
        *p = '\0';
        
        /* Cache for future use - caller MUST NOT free this */
        str->utf8 = result;
        return result;
    }
    
    /* For Java objects, use a thread-local ring buffer: the returned
     * pointer stays valid until FOUR more string_utf8 calls on this
     * thread (see the block comment above for why more than one is
     * needed). */
    unsigned slot = (string_utf8_tls_ring++) % STRING_UTF8_TLS_SLOTS;
    if (string_utf8_tls_buffer_sizes[slot] < required_size) {
        /* Grow the buffer */
        size_t new_size = required_size * 2;
        if (new_size < 256) new_size = 256;
        char* new_buffer = (char*)realloc(string_utf8_tls_buffers[slot], new_size);
        if (!new_buffer) return NULL;
        string_utf8_tls_buffers[slot] = new_buffer;
        string_utf8_tls_buffer_sizes[slot] = new_size;
    }
    
    char* p = string_utf8_tls_buffers[slot];
    for (jsize i = 0; i < len; i++) {
        jchar c = chars[i];
        if (c < 0x80) {
            *p++ = (char)c;
        } else if (c < 0x800) {
            *p++ = (char)(0xC0 | (c >> 6));
            *p++ = (char)(0x80 | (c & 0x3F));
        } else {
            *p++ = (char)(0xE0 | (c >> 12));
            *p++ = (char)(0x80 | ((c >> 6) & 0x3F));
            *p++ = (char)(0x80 | (c & 0x3F));
        }
    }
    *p = '\0';
    
    return string_utf8_tls_buffers[slot];
}

/* 
 * string_utf8_copy - Always returns a malloc'd copy that caller MUST free.
 * Use this when you need to store the string for later use.
 */
char* string_utf8_copy(JVM* jvm, JavaString* str) {
    const char* cached = string_utf8(jvm, str);
    if (!cached) return NULL;
    return strdup(cached);
}

/* Cleanup thread-local buffers (call at JVM shutdown) */
void string_utf8_cleanup(void) {
    /* Only clean current thread's buffers */
    for (int i = 0; i < STRING_UTF8_TLS_SLOTS; i++) {
        free(string_utf8_tls_buffers[i]);
        string_utf8_tls_buffers[i] = NULL;
        string_utf8_tls_buffer_sizes[i] = 0;
    }
    string_utf8_tls_ring = 0;
}

bool string_equals(JavaString* a, JavaString* b) {
    if (a == b) return true;
    if (!a || !b) return false;
    
    jsize len_a = string_length(a);
    jsize len_b = string_length(b);
    if (len_a != len_b) return false;
    
    const jchar* chars_a = string_chars(a);
    const jchar* chars_b = string_chars(b);
    if (!chars_a || !chars_b) return false;
    
    return memcmp(chars_a, chars_b, len_a * sizeof(jchar)) == 0;
}

jint string_hash(JavaString* str) {
    if (!str) return 0;
    
    /* For native strings, use cached hash if available */
    if (is_native_string(str) && str->hash) return str->hash;
    
    /* For JavaObject strings, check hash field using proper slot lookup */
    if (!is_native_string(str)) {
        JavaObject* obj = (JavaObject*)str;
        if (obj->header.clazz && g_jvm_for_instanceof) {
            int hash_slot = native_get_string_hash_slot(g_jvm_for_instanceof);
            if (hash_slot >= 0) {
                jint cached_hash = obj->fields[hash_slot].i;
                if (cached_hash != 0) return cached_hash;
            }
        }
    }
    
    jsize len = string_length(str);
    const jchar* chars = string_chars(str);
    if (!chars) return 0;
    
    /* Java String.hashCode(): int overflow is INTENTIONAL (spec wraps).
     * Compute in uint32_t to avoid C signed-overflow UB (gcc-15 -O2 can
     * miscompile the signed version — observed crash on MinGW). */
    uint32_t hash32 = 0;
    for (jsize i = 0; i < len; i++) {
        hash32 = 31u * hash32 + chars[i];
    }
    jint hash = (jint)hash32;
    
    /* Cache hash for native strings */
    if (is_native_string(str)) {
        str->hash = hash;
    }
    
    return hash;
}

void heap_dump(JVM* jvm) {
    (void)jvm;
    /* v34.30: route through the fprintf intercept (gate + gnu_printf
     * checking) — the raw printf() call bypassed both and warned on MinGW
     * (ms_printf header decl rejects %zu) and printed wrong values on
     * msvcrt at runtime. stdout passes the gate unconditionally. */
    fprintf(stdout, "=== Heap Dump ===\n");
    fprintf(stdout, "Total: %zu bytes\n", heap.size);
    fprintf(stdout, "Used: %zu bytes\n", heap.allocated);
    fprintf(stdout, "Free List Blocks: ");
    
    /* v34.58: перебираем size-class бакеты + large-список */
    int count = 0;
    for (int c = 0; c < SC_NUM_BUCKETS && count < 10; c++) {
        FreeBlock* fb = heap.free_lists[c];
        while (fb && count < 10) {
            printf("[%p:%u] ", fb, (unsigned int)fb->size);
            fb = fb->next;
            count++;
        }
    }
    for (FreeBlock* fb = heap.large_free_list; fb && count < 10; fb = fb->next) {
        printf("[%p:%u] ", fb, (unsigned int)fb->size);
        count++;
    }
    printf("\n");
    
    printf("Live objects scan:\n");
    uint8_t* ptr = heap.start;
    int obj_count = 0;
    while (ptr < heap.current && obj_count < 20) {
        GCObjectHeader* h = (GCObjectHeader*)ptr;
        const char* type_name = "?";
        if (h->type == OBJ_TYPE_OBJECT && h->clazz) type_name = h->clazz->class_name;
        else if (h->type == OBJ_TYPE_ARRAY) type_name = "Array";
        else if (h->type == OBJ_TYPE_STRING) type_name = "String";
        
        printf("  [%p] %s sz=%u m=%d\n", (h+1), type_name, h->size, h->marked);
        ptr += h->size;
        obj_count++;
    }
    printf("==================\n");
}

void heap_validate(JVM* jvm) {
    (void)jvm;
    size_t computed_allocated = 0;
    size_t object_count = 0;
    uint8_t* ptr = heap.start;

    /* v34.59 TLAB: линейный обход требует ЗАКРЫТЫХ чанков. Свой чанк
     * закрываем здесь (TLAB-остаток — валидный свободный блок, но НЕ в
     * списках, поэтому computed_allocated его не считает — корректно:
     * он и не прибавлялся). Чанки ДРУГИХ потоков видны только под
     * stop-the-world — вызывайте heap_validate из GC-контекста или с
     * NOJME_TLAB=0. */
    heap_tlab_flush_self();

    while (ptr < heap.current) {
        GCObjectHeader* h = (GCObjectHeader*)ptr;
        if (h->size == 0 || h->size > heap.size || ptr + h->size > heap.end) {
            ERROR_LOG("Corruption at %p", ptr);
            return;
        }
        
        /* ИСПРАВЛЕНО: Считаем все НЕ свободные объекты, а не только marked/pinned.
         * После GC marked сбрасывается в 0, поэтому проверка marked/pinned
         * давала бы всегда 0. Правильная проверка: type != OBJ_TYPE_FREE.
         */
        if (h->type != OBJ_TYPE_FREE) {
            computed_allocated += h->size;
            object_count++;
        }
        ptr += h->size;
    }
    
    if (computed_allocated != heap.allocated) {
        ERROR_LOG("Alloc mismatch: calc=%zu vs stats=%zu (objects=%zu)", 
                  computed_allocated, heap.allocated, object_count);
    }
}

/* Проверка magic number всех объектов - для раннего обнаружения corruption */
int heap_check_magic(JVM* jvm) {
    (void)jvm;
    int corrupt_count = 0;
    uint8_t* ptr = heap.start;

    /* v34.59 TLAB: аналогично heap_validate — только закрытые чанки
     * (см. комментарий там). */
    heap_tlab_flush_self();

    while (ptr < heap.current) {
        GCObjectHeader* h = (GCObjectHeader*)ptr;
        
        /* Защита от бесконечного цикла */
        if (h->size == 0 || h->size > heap.size || ptr + h->size > heap.end) {
            ERROR_LOG("heap_check_magic: Invalid object size at %p", ptr);
            break;
        }
        
        /* Проверяем magic только для НЕ свободных объектов */
        if (h->type != OBJ_TYPE_FREE) {
            if (h->magic != GC_HEADER_MAGIC) {
                const char* type_name = "?";
                if (h->clazz && h->clazz->class_name) {
                    type_name = h->clazz->class_name;
                }
                ERROR_LOG("heap_check_magic: Object at %p has corrupted magic 0x%08X (class=%s, type=%d, size=%u)", 
                         (void*)ptr, h->magic, type_name, h->type, h->size);
                corrupt_count++;
            }
        }
        
        ptr += h->size;
    }
    
    return corrupt_count;
}