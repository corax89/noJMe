/*
 * J2ME Emulator - Bytecode Execution Engine
 * Main interpreter loop and method invocation
 */

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <setjmp.h>
#include <time.h>   /* v41: thread-budget clock_gettime/nanosleep */
#include <math.h>   /* v36.37: isnan() in the inline F2I/F2L/D2L/D2I twins */
#include <stdint.h> /* v36.37: INT32_MAX/INT64_MAX in the saturating converts */
#include "cp_caches.h" /* v36.37: field/static-field inline caches shared with opcodes.c */
#ifdef _WIN32
#include <windows.h>  /* v41: Sleep(2) slices for the thread budget */
#endif

#include "jvm.h"
#include "classfile.h"
#include "opcodes.h"
#include "heap.h"
#include "threads.h"
#include "native.h"
/* Performance: direct access to instruction counter for inlined scheduling */
extern volatile uint64_t g_instruction_counter;
#include "debug.h"
#include "debug_macros.h"

/* M3G debug logging - disabled by default */
#ifndef M3G_DEBUG_LOG
#define M3G_DEBUG_LOG 0
#endif

/* Forward declarations */
int execute_method(JVM* jvm, JavaThread* thread, JavaMethod* method, JavaValue* args, JavaValue* result);
static int interpret(JVM* jvm, JavaThread* thread, JavaFrame* frame);

/* v36.57 [SPIN-BREAK]: монотонные миллисунды для брейкера чисто-Java
 * спинов (интерпретатор вызывает это из cold-ветки slow-check — редкий
 * каденс, clock_gettime вне горячего пути). */
int64_t jvm_exec_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Frame pool for fast allocation (avoids repeated malloc/free) */
#define FRAME_POOL_SIZE 256
typedef struct {
        JavaFrame* frames[FRAME_POOL_SIZE];
        JavaValue* locals_blocks[FRAME_POOL_SIZE];
        JavaValue* stack_blocks[FRAME_POOL_SIZE];
        int count;
        int max_locals[FRAME_POOL_SIZE];
        int max_stack[FRAME_POOL_SIZE];
} FramePool;

/* FIX(race): the pool used to be a single process-wide global, but methods
 * execute on real pthreads (Thread.run()). Concurrent frame_create/frame_destroy
 * calls raced on frame_pool.count producing negative/out-of-range indices
 * (global-buffer-overflow, caught by ASAN) and handing THE SAME JavaFrame/
 * locals/stack blocks to two threads at once (later "double free or corruption").
 * Execution of a frame always happens on the owning thread, so make the pool
 * thread-local: zero contention, no locks, identical semantics. */
static _Thread_local FramePool* tls_frame_pool = NULL;

static FramePool* frame_pool_get(void) {
        if (!tls_frame_pool) {
                tls_frame_pool = (FramePool*)calloc(1, sizeof(FramePool));
        }
        return tls_frame_pool;
}

/* === CRITICAL: Recalculate instance_size after superclass is loaded ===
 * This must be called after superclass is guaranteed to be loaded.
 * It properly calculates the total instance size including all inherited fields.
 * 
 * JVM Specification §4.5: Fields are laid out in the order they are declared,
 * with superclass fields appearing before subclass fields.
 */
void jvm_recalculate_instance_size(JVM* jvm, JavaClass* clazz) {
    if (!clazz) return;

    /* GUARD: Skip M3G (JSR-184) stub classes.
     * Their instance_size is managed exclusively by init_m3g_stub_classes()
     * in mobile3d.c. Re-running the calculation here (especially from
     * jvm_init_class at line 902) can produce a different size if the
     * class hierarchy or field count differs from what mobile3d.c set up.
     * This would cause objects allocated with the mobile3d.c size to be
     * accessed with a different size — heap corruption. */
    if (clazz->class_name && strstr(clazz->class_name, "javax/microedition/m3g/") != NULL) {
        return;
    }

    /* DEBUG: Log structure layout once */
    static bool layout_logged = false;
    if (!layout_logged) {
        if (g_j2me_runtime_debug) LOG_SAFE("[LAYOUT DEBUG] sizeof(JavaClass)=%zu, sizeof(ObjectHeader)=%zu\n",
                sizeof(JavaClass), sizeof(ObjectHeader));
        if (g_j2me_runtime_debug) LOG_SAFE("[LAYOUT DEBUG] offset of super_class_name=%zu, super_class=%zu\n",
                offsetof(JavaClass, super_class_name), offsetof(JavaClass, super_class));
        if (g_j2me_runtime_debug) LOG_SAFE("[LAYOUT DEBUG] offset of constant_pool_capacity=%zu\n",
                offsetof(JavaClass, constant_pool_capacity));
        layout_logged = true;
    }
    
    /* DEBUG: Validate clazz pointer */
    if ((uintptr_t)clazz < 0x10000) {
        LOG_SAFE("[FATAL] jvm_recalculate_instance_size: invalid clazz=%p (too low)\n", (void*)clazz);
        return;
    }
    
    /* DEBUG: Check if clazz looks like a string */
    if (clazz->magic != 0xCAFEBABE && clazz->magic != 0xDEADBEEF) {
        LOG_SAFE("[FATAL] jvm_recalculate_instance_size: clazz=%p has invalid magic=0x%08X\n", 
                (void*)clazz, clazz->magic);
        /* Try to detect if this is a string */
        const char* maybe_str = (const char*)clazz;
        LOG_SAFE("[FATAL] Looks like string: %.20s\n", maybe_str);
        return;
    }
    
    /* Note: We used to skip stub classes, but they also need proper instance_size
     * calculation to include inherited fields. The stub class initialization only
     * sets instance_size for its own fields, not inherited ones. */
    
    /* Start with superclass size or ObjectHeader if no superclass */
    size_t size = sizeof(ObjectHeader);  /* Always start with header */
    
    /* CRITICAL: Ensure superclass is loaded and has correct size */
    if (clazz->super_class_name && !clazz->super_class) {
        /* Try to load superclass now */
        clazz->super_class = jvm_load_class(jvm, clazz->super_class_name);
    }
    
    if (clazz->super_class) {
        /* Recursively ensure superclass has correct size */
        jvm_recalculate_instance_size(jvm, clazz->super_class);
        
        /* Superclass should already have correct instance_size including header */
        /* Use >= because a superclass with no fields still has ObjectHeader */
        if (clazz->super_class->instance_size >= sizeof(ObjectHeader)) {
            size = clazz->super_class->instance_size;
        }
    }
    
    /* Add own instance fields */
    int own_field_count = 0;
    if (clazz->fields) {
        for (uint16_t i = 0; i < clazz->fields_count; i++) {
            JavaField* field = &clazz->fields[i];
            
            /* Skip static fields */
            if (field->access_flags & ACC_STATIC) continue;
            
            own_field_count++;
            
            /* Each field takes one JavaValue slot */
            size += sizeof(JavaValue);
            
            /* Long and double take 2 slots */
            if (field->descriptor && 
                (field->descriptor[0] == 'J' || field->descriptor[0] == 'D')) {
                size += sizeof(JavaValue);
            }
        }
    }
    
    /* Log M3G class instance_size recalculations */
    #if M3G_DEBUG_LOG
    if (clazz->class_name && strstr(clazz->class_name, "m3g/") != NULL) {
        LOG_SAFE("[M3G-SIZE] %s: super=%s(%p), old_size=%zu, new_size=%zu, own_fields=%d\n",
                clazz->class_name,
                clazz->super_class ? clazz->super_class->class_name : "NONE",
                (void*)clazz->super_class,
                clazz->instance_size, size, own_field_count);
    }
    #endif
    
    /* Update if different */
    if (clazz->instance_size != size) {
        EXEC_DEBUG("Recalculated instance_size for %s: %zu -> %zu bytes (super=%s, own_fields=%d)",
                   clazz->class_name ? clazz->class_name : "?",
                   clazz->instance_size, size,
                   clazz->super_class ? clazz->super_class->class_name : "none",
                   own_field_count);
        clazz->instance_size = size;
    }
}

/* Create a new frame */
static JavaFrame* frame_create(JVM* jvm, JavaMethod* method, JavaClass* clazz) {
        (void)jvm;
        if (!method || !clazz) {
                ERROR_LOG("frame_create: NULL method or clazz");
                return NULL;
        }
        int max_locals = method->code.max_locals;
        int max_stack = method->code.max_stack + 4;  /* TEMPORARY FIX: Add 4 to max_stack */
        
        /* Try to reuse from pool (thread-local: see FIX(race) above) */
        FramePool* fp = frame_pool_get();
        if (fp && fp->count > 0 && fp->count <= FRAME_POOL_SIZE) {
                int idx = --fp->count;
                JavaFrame* frame = (idx >= 0 && idx < FRAME_POOL_SIZE) ? fp->frames[idx] : NULL;
                if (frame && fp->max_locals[idx] >= max_locals && fp->max_stack[idx] >= max_stack) {
                        frame->locals = fp->locals_blocks[idx];
                        frame->stack = fp->stack_blocks[idx];
                        /* Clear only needed portions (not entire allocation) */
                        memset(frame->locals, 0, max_locals * sizeof(JavaValue));
                        memset(frame->stack, 0, max_stack * sizeof(JavaValue));
                        /* Reset frame state */
                        frame->pc = 0;
                        frame->stack_top = -1;
                        frame->prev = NULL;
                        /* v34.99 MONITOR-UNWIND: pooled frames are reused — the
                         * ownership list MUST start empty or a stale record
                         * from a previous method would release a monitor this
                         * frame never acquired. */
                        frame->mon_owned_n = 0;
                        frame->clazz = method->clazz;
                        frame->method = method;
                        frame->code = method->code.code;
                        frame->code_length = method->code.code_length;
                        frame->max_locals = max_locals;
                        frame->max_stack = max_stack;
                        frame->throwing_pc = 0;
                        frame->exception_table = method->code.exception_table;
                        frame->exception_table_length = method->code.exception_table_length;
                        return frame;
                }
                /* Size mismatch or corrupt entry, put back and fall through to alloc */
                fp->count++;
        }
        
        /* Pool miss or empty - allocate new */
        JavaFrame* frame = (JavaFrame*)calloc(1, sizeof(JavaFrame));
        if (!frame) {
                ERROR_LOG("frame_create: Failed to allocate frame");
                return NULL;
        }

        frame->method = method;
        frame->clazz = clazz;
        frame->pc = 0;
        frame->code = method->code.code;
        frame->code_length = method->code.code_length;

        /* Allocate locals */
        frame->max_locals = max_locals;
        if (frame->max_locals > 0) {
                frame->locals = (JavaValue*)calloc(frame->max_locals, sizeof(JavaValue));
                if (!frame->locals) {
                        ERROR_LOG("frame_create: Failed to allocate locals");
                        free(frame);
                        return NULL;
                }
        }

        /* Allocate operand stack */
        frame->max_stack = max_stack;
        frame->stack_top = -1;
        if (frame->max_stack > 0) {
                /* v36.04 STKDIAG: the frame pool reuses blocks across methods
                 * only when max_stack matches, so a padded allocation must be
                 * carried through the pool as well (pool keys on max_stack). */
#ifdef NOJME_STKDIAG
                frame->stack = (JavaValue*)calloc(frame->max_stack + 24, sizeof(JavaValue));
#else
                frame->stack = (JavaValue*)calloc(frame->max_stack, sizeof(JavaValue));
#endif
                if (!frame->stack) {
                        ERROR_LOG("frame_create: Failed to allocate stack");
                        free(frame->locals);
                        free(frame);
                        return NULL;
                }
        }

        /* Exception table */
        frame->exception_table = method->code.exception_table;
        frame->exception_table_length = method->code.exception_table_length;

        EXEC_DEBUG("Created frame for %s%s (locals: %d, stack: %d, code: %u bytes)",
                   method->name, method->descriptor,
                   frame->max_locals, frame->max_stack, frame->code_length);

        return frame;
}

/* Free a frame */
static void frame_destroy(JavaFrame* frame) {
    if (!frame) return;
    
    /* Return to pool instead of freeing (thread-local: see FIX(race)) */
    FramePool* fp = frame_pool_get();
    if (fp && fp->count >= 0 && fp->count < FRAME_POOL_SIZE) {
        int idx = fp->count++;
        fp->frames[idx] = frame;
        fp->locals_blocks[idx] = frame->locals;
        fp->stack_blocks[idx] = frame->stack;
        fp->max_locals[idx] = frame->max_locals;
        fp->max_stack[idx] = frame->max_stack;
        return;
    }
    
    /* Pool full - actually free */
    free(frame->locals);
    free(frame->stack);
    free(frame);
}

/* v36.12 (malloc-census find): the frame pool is _Thread_local, so every
 * runner pthread that finishes takes its whole pool (up to 256 JavaFrame
 * + locals + stack blocks) to the grave. The JVM spawns several REAL
 * pthreads per session; each session therefore leaked a few dozen frames
 * (measured: 12 blocks / ~1.8 KB per session on TestSpin alone, scaled by
 * thread count and call depth on real games). Call this on the OWNING
 * pthread right before it exits — the pool re-creates lazily on demand. */
void jvm_frame_pool_flush(void) {
    FramePool* fp = tls_frame_pool;
    if (!fp) return;
    tls_frame_pool = NULL;
    for (int i = 0; i < fp->count; i++) {
        free(fp->frames[i]);
        free(fp->locals_blocks[i]);
        free(fp->stack_blocks[i]);
    }
    free(fp);
}

/* v36.48 [DUMP-FENCE]: строчный снапшот текущего Java-сайта потока.
 * Пишется ТОЛЬКО потоком-владельцем (push_frame при входе в exec-окно +
 * каждый 64-й yield в interpret), читается beat-таймером в
 * jvm_threads_dump_snprint. Прежде beat разыменовывал current_frame
 * ДРУГИХ потоков без блокировки: между pop_frame и frame_destroy это
 * heap-use-after-free (ASan: READ cf->method, ys_asan_loop фазы 2/3 —
 * beat-поток против unwind-а главного потока). Кадры теперь НЕ читаются
 * из чужого потока ВООБЩЕ — только стабильная строка в структуре потока. */
void jvm_thread_note_site(JavaThread* thread, JavaFrame* frame) {
    if (!thread || !frame) return;
    snprintf(thread->jsite, sizeof(thread->jsite), "%.22s.%.24s@%u",
             (frame->clazz && frame->clazz->class_name)
                 ? frame->clazz->class_name : "?",
             (frame->method && frame->method->name)
                 ? frame->method->name : "?",
             (unsigned)frame->pc);
}

/* Push frame onto thread stack */
static int push_frame(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm;
    if (!thread || !frame) return -1;

    frame->prev = thread->current_frame;
    thread->current_frame = frame;

    /* [DUMP-FENCE] v36.48: вход в exec-окно (внешний кадр) — обновить
     * снапшот сайта; вложенные вызовы обновит yield-гранулярность ниже. */
    if (!frame->prev) jvm_thread_note_site(thread, frame);

    EXEC_DEBUG("Pushed frame, depth now: %d", 
               thread->current_frame ? 
               (thread->current_frame->prev ? 1 : 0) + 1 : 0);

    return 0;
}

/* Pop frame from thread stack */
static JavaFrame* pop_frame(JVM* jvm, JavaThread* thread) {
    (void)jvm;
    if (!thread || !thread->current_frame) return NULL;

    JavaFrame* frame = thread->current_frame;
    thread->current_frame = frame->prev;

    EXEC_DEBUG("Popped frame, depth now: %d",
               thread->current_frame ? 1 : 0);

    return frame;
}

/* Count arguments in method descriptor - uses native.h version */
/* Forward declaration for readability */
extern int count_args(const char* descriptor);

/* =========================================================================
 * v36.30 [EXIT-FENCE]: frame-thread Java exclusion fence.
 *
 * USER CRASH REPORT (Sys Info (MH)(1.1.0), TWO Atmosphère dumps, both
 * builds): Data Abort (NULL deref) on EXIT; stack = interpreter x2 ->
 * invoke -> native -> deep native chain. The window: destroyApp /
 * commandAction / timer / key Java runs ON THE FRAME THREAD (execute_method
 * with no VM-runner TLS), while pthread VM runners (Thread.start threads)
 * keep interpreting CONCURRENTLY — two interpreters against one heap /
 * scheduler / RMS state. The pause menu lifts the pause BEFORE calling
 * destroyApp (v35.10 deadlock fix), so at exit the VM is fully live — the
 * midlet's helper thread and destroyApp collide exactly once, on the
 * device's slow SD/RMS path. This fence holds every VM runner at its
 * 64-insn poll while frame-thread Java executes (bounded: progress is
 * guaranteed even if a fence window overstayed). */
volatile int g_vm_fiber_fence = 0;

/* v36.30: fast per-OS-thread "am I a VM runner?" flag — written ONCE by the
 * runner thread at startup, read on EVERY execute_method call (the arm
 * check must stay off the hot path's cost radar; the pthread_equal walk in
 * jvm_current_os_thread_is_vm_runner() is far too heavy for that). */
__thread int tls_os_thread_is_vm_runner = 0;

/* Runner-side wait: spin while the frame thread owns the VM. Bounded —
 * after ~2.5 s total we proceed anyway (a wedged fence must never hang
 * the VM; the windows are ms-scale in practice).
 * v36.31 [EXIT-FENCE-EXT]: EXCEPTION — while the frame thread is inside a
 * destroyApp Java window (midp_destroy_app_in_flight()), the cap extends
 * in 2.5 s quanta up to 15 s. Field evidence (v36.31 host-repro knob
 * NOJME_RMS_DELAY_MS): a destroyApp RMS flush on slow SD holds the fence
 * 0.1-4+ s; the old fixed cap dropped the runners back into the heap
 * mid-destroyApp — the exact two-interpreters race this fence exists to
 * close ("Invalid memory access at exit" class). The extension still
 * bails when the VM is killed (running=false from the teardown kill
 * switch) or after the absolute 15 s ceiling (a wedged destroyApp must
 * not pin the VM forever — teardown's strand path takes over). */
void jvm_frame_fence_wait_public(void) {
    int waited_ms = 0;
    int ext_logged = 0;
    while (g_vm_fiber_fence) {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 2 * 1000000L };
        nanosleep(&ts, NULL);
        waited_ms += 2;
        if (waited_ms >= 2500) {
            {
                extern int midp_destroy_app_in_flight(void);
                extern JVM* g_jvm_for_instanceof;
                if (midp_destroy_app_in_flight() &&
                    waited_ms < 15000 &&
                    !(g_jvm_for_instanceof && !g_jvm_for_instanceof->running)) {
                    if (!ext_logged) {
                        ext_logged = 1;
                        LOG_SAFE("[FENCE] 2.5s cap during destroyApp — extending runner hold (slow SD RMS flush?)\n");
                        {
                            extern void sw_trace_force(const char* fmt, ...)
                                __attribute__((weak));
                            if (&sw_trace_force && sw_trace_force)
                                sw_trace_force("[FENCE] destroyApp in flight — runner hold extended");
                        }
                    }
                    continue;
                }
            }
            LOG_SAFE("[FENCE] held >2.5s for a frame-thread Java window - proceeding\n");
            return;
        }
    }
}

/* execute_method scope guard: arm the fence when the caller is the frame/
 * frontend thread (NOT a VM runner, NOT inside the main-fiber stepping
 * window — that window IS the main thread's own bytecode). cleanup runs on
 * every return path of execute_method. */
static int jvm_frame_fence_arm(void) {
    extern __thread int tls_os_thread_is_vm_runner;
    extern volatile int g_jvm_main_thread_executing;
    if (tls_os_thread_is_vm_runner || g_vm_fiber_fence ||
        g_jvm_main_thread_executing) {
        return 0;
    }
    g_vm_fiber_fence = 1;
    memory_barrier();
    return 1;
}
static void jvm_frame_fence_disarm(void *armed_p) {
    if (armed_p && (int)(intptr_t)armed_p) {
        memory_barrier();
        g_vm_fiber_fence = 0;
        memory_barrier();
    }
}

/* ================= [ARGGUARD] v36.49 =================
 * Охота за "es/Yeti " (Ryujinx: Invalid memory access at
 * 0x20697465592F7365 — ASCII-байты ПУТИ ИГРЫ в ref-слоте). Адрес
 * разыменования РОВЕН мусорному значению => читали header.clazz
 * (смещение 0) по мусорной ссылке — см. гварды receiver в opcodes.c
 * (invokevirtual fast/slow, getfield/putfield, invokeinterface,
 * checkcast, aastore). Этот блок ловит мусор У ИСТОКА: на входе в
 * execute_method (аргументы мостов native->Java) и на возврате из
 * native-методов (L/[ возвраты). Хост-детекторы (ASan + pattern-init)
 * весь девайс-джорней чисты — мусор появляется только на устройстве,
 * поэтому валидатор вшит в бинарь и включён по умолчанию
 * (NOJME_ARGGUARD=0 выключает). Репорт: метод, слот, hex+ASCII мусора,
 * адрес вызвавшего моста (addr2line на хост-сборке), первые 24. */
static int argguard_enabled(void) {
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("NOJME_ARGGUARD");
        v = (e && e[0] == '0') ? 0 : 1;
    }
    return v;
}

static void argguard_report(const char* kind, const char* cname,
                            const char* mname, const char* desc,
                            int slot, uint64_t raw, void* callsite) {
    static int count = 0;
    char ascii[9];
    int i;
    for (i = 0; i < 8; i++) {
        unsigned char c = (unsigned char)((raw >> (56 - 8 * i)) & 0xFF);
        ascii[i] = (c >= 0x20 && c <= 0x7E) ? (char)c : '.';
    }
    ascii[8] = '\0';
    if (count < 24) {
        count++;
        LOG_SAFE("[ARGGUARD] %s-слот #%d мусор в %s.%s%s: raw=0x%016llx "
                 "ascii=\"%s\" callsite=%p -> NULL (источник: мост/возврат с C-стека)\n",
                 kind, slot,
                 cname ? cname : "?", mname ? mname : "?", desc ? desc : "?",
                 (unsigned long long)raw, ascii, callsite);
        /* устройство: stderr не попадает в log.txt — дублируем в sw_trace */
        {
            extern void sw_trace_force(const char* fmt, ...) __attribute__((weak));
            if (&sw_trace_force && sw_trace_force) {
                sw_trace_force("[ARGGUARD] %s-слот #%d мусор в %s.%s%s: "
                               "raw=0x%016llx ascii=\"%s\" callsite=%p",
                               kind, slot,
                               cname ? cname : "?", mname ? mname : "?",
                               desc ? desc : "?",
                               (unsigned long long)raw, ascii, callsite);
            }
        }
    }
}

/* Validate-walk аргументов по дескриптору. argc_total = сколько слотов
 * массива реально предоставлено вызывающим (ходим только по нему);
 * has_this = args[0] является receiver. defuse=1 — заменить мусор на
 * NULL (безопасно, если массив >= слотов дескриптора — malloc-массивы
 * op_invoke*; для чужих C-стековых мостов — только defuse=0). */
void argguard_check_array(const char* cname, const char* mname,
                          const char* desc, JavaValue* args,
                          int argc_total, int has_this, void* callsite,
                          int defuse) {
    if (!argguard_enabled() || !args || !desc || argc_total <= 0) return;
    int idx = 0;
    if (has_this) {
        if (argc_total < 1) return;
        if (argguard_bad_ptr((uintptr_t)args[0].ref)) {
            argguard_report("this", cname, mname, desc, 0,
                            args[0].raw, callsite);
            if (defuse) args[0].ref = NULL;
        }
        idx = 1;
    }
    const char* d = desc;
    if (*d == '(') d++;
    while (*d && *d != ')' && idx < argc_total) {
        char c = *d;
        if (c == 'L' || c == '[') {
            if (argguard_bad_ptr((uintptr_t)args[idx].ref)) {
                argguard_report("arg", cname, mname, desc, idx,
                                args[idx].raw, callsite);
                if (defuse) args[idx].ref = NULL;
            }
            idx++;
            if (c == 'L') {
                while (*d && *d != ';') d++;
                if (*d) d++;
            } else {
                while (*d == '[') d++;
                if (*d == 'L') {
                    while (*d && *d != ';') d++;
                    if (*d) d++;
                } else if (*d) {
                    d++;
                }
            }
        } else {
            /* B,C,F,I,S,Z и компактные J/D — один слот, не ref */
            idx++;
            d++;
        }
    }
}

/* Валидация возврата native: L/[ возвращаемый тип с мусорным ref. */
void argguard_check_ret(const char* cname, const char* mname,
                        const char* desc, JavaValue* rv, void* callsite) {
    if (!argguard_enabled() || !rv || !desc) return;
    const char* rp = strchr(desc, ')');
    if (!rp) return;
    rp++;
    if (*rp != 'L' && *rp != '[') return;
    if (argguard_bad_ptr((uintptr_t)rv->ref)) {
        argguard_report("ret", cname, mname, desc, -1, rv->raw, callsite);
        rv->ref = NULL;
    }
}

/* Execute a method */
int execute_method(JVM* jvm, JavaThread* thread, JavaMethod* method,
                   JavaValue* args, JavaValue* result) {
    if (!jvm || !thread || !method) {
        ERROR_LOG("execute_method: NULL parameter");
        return -1;
    }

    /* v36.30 [EXIT-FENCE]: hold VM runners while frame-thread Java runs.
     * Scope guard — armed only by the outermost frame-thread call. */
    int fence_armed = jvm_frame_fence_arm();
    void* fence_guard __attribute__((unused, cleanup(jvm_frame_fence_disarm))) =
        (void*)(intptr_t)fence_armed;

    /* Check recursion depth to prevent stack overflow.
     * BUG FIX: Was 'static int' which is shared across ALL threads in cooperative
     * scheduling, causing premature StackOverflowError when multiple threads are active.
     * Now counts actual frame depth from the thread's call stack.
     * v34.33: depth cap is env-tunable (NOJME_MAX_RECURSION). Every Java call
     * nests two large C frames (execute_method + interpret), so 2048 Java frames
     * can exhaust a 1 MB Windows thread stack BEFORE this guard trips — a raw
     * SIGSEGV with no Java-visible StackOverflowError. Lower the cap if the
     * host stack is small (see FIXES.txt v34.33). */
    static int max_recursion_depth = -1;
    if (max_recursion_depth < 0) {
        max_recursion_depth = 2048;
        const char* env = getenv("NOJME_MAX_RECURSION");
        if (env && *env) {
            int v = atoi(env);
            if (v > 0) max_recursion_depth = v;
        }
    }
    int recursion_depth = 0;
    {
        JavaFrame* f = thread->current_frame;
        while (f) { recursion_depth++; f = f->prev; }
    }
    if (recursion_depth > max_recursion_depth) {
        /* Print call stack to identify which method is recursing */
        JavaThread* cur = jvm_current_thread(jvm);
        if (cur && cur->current_frame) {
            /* v34.33: ALWAYS_LOG — this is a crash-path diagnostic (the very
             * next step without the guard would be a native SIGSEGV); it must
             * be visible with the default "quiet" log policy. */
            ALWAYS_LOG("[RECURSION-ERROR] Call stack (depth=%d):\n", recursion_depth);
            JavaFrame* f = cur->current_frame;
            int frames_shown = 0;
            while (f && frames_shown < 20) {
                ALWAYS_LOG("  [%d] %s.%s%s\n", frames_shown,
                    f->clazz ? (f->clazz->class_name ? f->clazz->class_name : "?") : "?",
                    f->method ? f->method->name : "?",
                    f->method ? f->method->descriptor : "");
                f = f->prev;
                frames_shown++;
            }
        }
        ERROR_LOG("execute_method: Maximum recursion depth exceeded (%d)", recursion_depth);
        jvm_throw_by_name(jvm, "java/lang/StackOverflowError", NULL);
        return -1;
    }

    EXEC_DEBUG("execute_method: %s%s (depth=%d)", method->name, method->descriptor, recursion_depth);

    /* Check if native - first check the flag, then check registry for stub classes */
    if (method->is_native) {
        EXEC_DEBUG("Calling native method: %s%s", method->name, method->descriptor);
        return native_call(jvm, thread, method, args, result);
    }
    
    /* For stub classes, check if there's a registered native handler even if not marked native.
     *
     * v34 FIX (VmTest TimerTests EXEC-UNDERFLOW / lost TimerTask.cancel() result):
     * two conditions were missing here.
     *
     * 1. The shortcut ran for methods that HAVE REAL BYTECODE: native_find()
     *    walks up the inheritance chain, so a JAR class OVERRIDING a
     *    native-backed stub method (TimedTask.cancel() over the
     *    java/util/TimerTask native) was hijacked - the override's bytecode
     *    never executed and super.cancel() side effects were lost.
     *    Restrict the shortcut to methods without bytecode (true stubs).
     *
     * 2. The invoke wrappers (op_invokevirtual/invokespecial/interface) push
     *    the returned value only when method->is_native is set; dispatching a
     *    !is_native method through the registry returned the value in
     *    *result, nothing was pushed, and the caller underflowed
     *    (safe_pop returned 0 - the test only PASSED by coincidence).
     *    Mark the method native so every wrapper pushes the result. */
    extern NativeMethod native_find(JVM* jvm, const char* class_name, const char* method_name,
                                    const char* descriptor);
    if (method->clazz && method->clazz->class_name &&
        (!method->code.code || method->code.code_length == 0) &&
        native_find(jvm, method->clazz->class_name, method->name, method->descriptor)) {
        EXEC_DEBUG("Calling native method (from registry): %s%s", method->name, method->descriptor);
        method->is_native = 1;  /* v34: dispatch is native - wrappers must push *result */
        return native_call(jvm, thread, method, args, result);
    }

    /* Check if method has code */
    if (!method->code.code || method->code.code_length == 0) {
        ERROR_LOG("Method %s has no code", method->name);
        return -1;
    }

    /* Handle synchronized methods - acquire monitor before execution */
    bool is_synchronized = (method->access_flags & ACC_SYNCHRONIZED) != 0;
    bool is_static = (method->access_flags & ACC_STATIC) != 0;
    JavaObject* sync_obj = NULL;
    
    if (is_synchronized) {
        if (is_static) {
            /* For static methods, synchronize on the Class object */
            sync_obj = (JavaObject*)method->clazz;
        } else {
            /* For instance methods, synchronize on 'this' */
            if (args && args[0].ref) {
                sync_obj = (JavaObject*)args[0].ref;
            }
        }
        
        EXEC_DEBUG("[SYNC_METHOD] %s.%s is synchronized, sync_obj=%p",
                method->clazz ? method->clazz->class_name : "?", 
                method->name ? method->name : "?",
                (void*)sync_obj);
        
        if (sync_obj) {
            /* v36.57 [MON-SITE]: сайт ACC_SYNC-входа для [MON-WAIT]. */
            {
                extern void jvm_mon_site_note(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
                jvm_mon_site_note("sync-method %s.%s",
                                  method->clazz ? (method->clazz->class_name ? method->clazz->class_name : "?") : "?",
                                  method->name ? method->name : "?");
            }
            int mon_result = monitor_enter(jvm, sync_obj);
            if (mon_result != JNI_OK) {
                /* v36.57 [PAINT-BUDGET]: бюджет pump-потока истёк — бросаем
                 * RuntimeException (unwind освобождает мониторы, paint()
                 * прерывается, главный цикл жив). Без этого возвращались с
                 * -1 БЕЗ pending-исключения — диспетчер логировал
                 * "OPCODE FAIL ... WITHOUT exception". */
                {
                    extern int jvm_paint_lock_aborted(void);
                    if (jvm_paint_lock_aborted()) {
                        jvm_throw_by_name(jvm, "java/lang/RuntimeException",
                                          "nojme paint-lock budget exceeded");
                    }
                }
                ERROR_LOG("Failed to enter monitor for synchronized method %s", method->name);
                return -1;
            }
            EXEC_DEBUG("[SYNC_METHOD] Entered monitor for %s.%s",
                    method->clazz ? method->clazz->class_name : "?",
                    method->name ? method->name : "?");
        } else {
            WARN_LOG("[SYNC_METHOD] sync_obj is NULL for %s.%s!",
                    method->clazz ? method->clazz->class_name : "?",
                    method->name ? method->name : "?");
        }
    }

    /* Create frame */
    JavaFrame* frame = frame_create(jvm, method, method->clazz);
    if (!frame) {
        ERROR_LOG("Failed to create frame for %s", method->name);
        if (is_synchronized && sync_obj) {
            monitor_exit(jvm, sync_obj);
        }
        return -1;
    }

    /* Copy arguments to locals */
    (void)method_arg_count(method);  /* Verify descriptor is valid and cache count */
    int local_idx = 0;

    if (!is_static && args) {
        /* this reference */
        /* [ARGGUARD] v36.49: receiver от моста может быть мусором
         * (поле: "es/Yeti ") — валидируем ДО попадания в locals:
         * мусор -> NULL (штатный NPE-путь вместо дикого deref [ref+0]). */
        JavaValue this_v = args[0];
        if (argguard_enabled() && argguard_bad_ptr((uintptr_t)this_v.ref)) {
            argguard_report("this",
                            method->clazz ? method->clazz->class_name : NULL,
                            method->name, method->descriptor, 0,
                            this_v.raw, __builtin_return_address(0));
            this_v.ref = NULL;
        }
        frame->locals[local_idx++] = this_v;
    }

    /* Copy remaining arguments from descriptor 
     * For non-static: args[0]=this, args[1..]=method args
     * For static: args[0..]=method args
     * IMPORTANT: double/long take 2 slots in locals but only 1 entry in args[]
     */
    int args_start = is_static ? 0 : 1;
    int arg_idx = args_start;
    const char* desc = method->descriptor;
    
    /* Skip past '(' */
    if (*desc == '(') desc++;
    
    while (*desc && *desc != ')') {
        /* Copy argument value to locals */
        frame->locals[local_idx] = args[arg_idx];
        
        switch (*desc) {
            case 'J': case 'D':
                /* FIX: args[] is a COMPACT list built by every caller
                 * (op_invoke* marshalling, jvm_invoke_*, natives): exactly one
                 * JavaValue per Java argument, J/D included. The previous code
                 * consumed TWO entries here (value + bogus neighbor as high
                 * slot), shifting every following argument and producing
                 * garbage doubles/longs (e.g. Rectangle area tests).
                 * Locals still occupy two slots - fill both with the SAME
                 * value, which is our two-slot representation convention. */
                frame->locals[local_idx] = args[arg_idx];
                frame->locals[local_idx + 1] = args[arg_idx];
                local_idx += 2;
                arg_idx += 1;
                desc++;
                break;
            case 'B': case 'C': case 'F': case 'I': case 'S': case 'Z':
                local_idx++;
                arg_idx++;
                desc++;
                break;
            case 'L':
                /* [ARGGUARD] v36.49: ref-аргумент от моста — валидация
                 * при копировании (значение уже в locals, дефуз там же) */
                if (argguard_enabled() &&
                    argguard_bad_ptr((uintptr_t)frame->locals[local_idx].ref)) {
                    argguard_report("arg",
                                    method->clazz ? method->clazz->class_name : NULL,
                                    method->name, method->descriptor, arg_idx,
                                    frame->locals[local_idx].raw,
                                    __builtin_return_address(0));
                    frame->locals[local_idx].ref = NULL;
                }
                local_idx++;
                arg_idx++;
                while (*desc && *desc != ';') desc++;
                if (*desc == ';') desc++;
                break;
            case '[':
                /* [ARGGUARD] v36.49: массивный ref — та же валидация */
                if (argguard_enabled() &&
                    argguard_bad_ptr((uintptr_t)frame->locals[local_idx].ref)) {
                    argguard_report("arg",
                                    method->clazz ? method->clazz->class_name : NULL,
                                    method->name, method->descriptor, arg_idx,
                                    frame->locals[local_idx].raw,
                                    __builtin_return_address(0));
                    frame->locals[local_idx].ref = NULL;
                }
                local_idx++;
                arg_idx++;
                while (*desc == '[') desc++;
                if (*desc == 'L') while (*desc && *desc != ';') desc++;
                if (*desc) desc++;
                break;
            default:
                desc++;
                break;
        }
    }


    /* Push frame */
    push_frame(jvm, thread, frame);


    /* DEBUG: Log frame relationship */
    EXEC_DEBUG("DEBUG: Pushed frame %p, prev=%p, thread->current_frame=%p",
            (void*)frame, (void*)frame->prev, (void*)thread->current_frame);
    if (frame->prev) {
        EXEC_DEBUG("DEBUG: Caller stack_top before: %d", frame->prev->stack_top);
    }

    /* Execute */
    int exec_result = interpret(jvm, thread, frame);

    /* DEBUG: Log stack state after interpret for create method */
    if (method->name && strcmp(method->name, "create") == 0) {
        if (g_j2me_runtime_debug) LOG_SAFE("[EXECUTE_METHOD] After interpret for %s: result=%d\n", method->name, exec_result);
        if (g_j2me_runtime_debug) LOG_SAFE("[EXECUTE_METHOD] frame=%p, frame->prev=%p\n", (void*)frame, (void*)frame->prev);
        if (frame->prev) {
            if (g_j2me_runtime_debug) LOG_SAFE("[EXECUTE_METHOD] Caller stack_top=%d, stack[top]=%p\n",
                    frame->prev->stack_top,
                    (void*)frame->prev->stack[frame->prev->stack_top].ref);
        }
    }

    /* DEBUG: Log stack state after interpret */
    EXEC_DEBUG("DEBUG: interpret returned %d, frame->prev=%p", exec_result, (void*)frame->prev);
    if (frame->prev) {
        EXEC_DEBUG("DEBUG: Caller stack_top after: %d", frame->prev->stack_top);
        if (frame->prev->stack_top >= 0) {
            EXEC_DEBUG("DEBUG: Caller stack[top] = %p",
                    (void*)frame->prev->stack[frame->prev->stack_top].ref);
        }
    }

    /* Pop frame */
    /* v34.99 MONITOR-UNWIND (the countdown-freeze root fix): release monitors
     * this frame acquired via monitorenter and never released. An exception
     * unwinding out of a synchronized BLOCK used to leak the monitor
     * forever (the interpreter never tracked block ownership), and the next
     * monitor_enter on that object from ANOTHER OS thread — the frontend
     * thread executing paint() — blocked on the entry condvar FOREVER:
     * m frozen at one pc with last_native=bindTarget, t2 'run' spinning,
     * CPU ~100%, all M3G locks free. Normal returns leave the list empty
     * (javac emits monitorexit before every return in the block); the call
     * is a no-op then and costs one predictable branch. */
    if (frame->mon_owned_n > 0) {
        extern void jvm_frame_release_monitors(JVM* jvm, JavaThread* jt,
                                               JavaFrame* fr, int on_exception);
        jvm_frame_release_monitors(jvm, thread, frame, exec_result < 0 ? 1 : 0);
    }
    pop_frame(jvm, thread);

    /* For non-native methods: return value was pushed to caller's stack by
     * op_ireturn/op_areturn/etc. Now that we've popped the method frame,
     * the return value is on the new current_frame's stack.
     *
     * NOTE (FIX): we deliberately do NOT read/consume anything here anymore.
     * The generic path used to read caller->stack[stack_top-1] which grabbed
     * a stale operand, and any consumption here would also break the
     * interpreter-level invoke opcodes that RELY on the pushed value.
     * C-entry wrappers (jvm_invoke_virtual/static/special) snapshot & restore
     * the caller's stack_top around this call and pick up the result from the
     * deposited slots themselves. */

    /* DEBUG: Verify current frame after pop for create method */
    if (method->name && strcmp(method->name, "create") == 0) {
        if (g_j2me_runtime_debug) LOG_SAFE("[EXECUTE_METHOD] After pop_frame: thread->current_frame=%p\n",
                (void*)thread->current_frame);
        if (thread->current_frame) {
            if (g_j2me_runtime_debug) LOG_SAFE("[EXECUTE_METHOD] Current stack_top=%d, stack[top]=%p\n",
                    thread->current_frame->stack_top,
                    (void*)thread->current_frame->stack[thread->current_frame->stack_top].ref);
        }
    }

    /* Get result - already handled above for non-native, 
     * for native methods the result is set by native_call */

    /* Handle synchronized methods - release monitor after execution */
    if (is_synchronized && sync_obj) {
        monitor_exit(jvm, sync_obj);
    }

    /* Cleanup */
    frame_destroy(frame);

    return exec_result;
}

/* v36.04 NESTED-RETURN-CONTAINED — the root fix of the session-48 deferred
 * bugs (a)+(b) ("interpreter frame-stack leak / write past frame stack in
 * ILOAD_n", "nested execute_method segv in class_hash_lookup").
 *
 * MECHANISM (reproduced on host, TestVector + TestConcat): the *return
 * opcode handlers (op_ireturn/lreturn/freturn/dreturn/areturn, opcodes.c)
 * deposit the result onto frame->prev — the RESTORED CALLER frame. That is
 * the correct contract for bytecode->bytecode calls (the invoke handlers
 * popped the args, the deposit lands where the opcode stream expects the
 * value) and for C-entry wrappers that snapshot/restore stack_top
 * themselves. But a NESTED execute_method() issued from inside a NATIVE
 * (Vector.toString -> element toString(), Hashtable.put in
 * System.getProperties, ...) has NO bytecode caller: frame->prev is the
 * nearest OUTER bytecode frame, whose native callee will never pop the
 * deposit. Every nested call PERMANENTLY LEAKED 1 slot (2 for J/D) on that
 * outer frame's operand stack:
 *   * TestVector.startApp (max_stack 5 + 4 pad = 9): 8 GcBomb.toString()
 *     nested calls through one Vector.toString() drove the tracked stack
 *     pointer from 0 to 8 — the very next pushes wrote past the frame
 *     allocation (ASAN heap-buffer-overflow, "write past frame stack,
 *     execute.c ILOAD_n"), and with games' longer loops the corruption
 *     reached the neighbouring malloc heap (field Data Aborts in dtoa /
 *     class-hash — the "random victim" class of crashes);
 *   * the deposited STALE refs also sit in GC-scanned frame stacks — a
 *     later sweep treats recycled garbage as live roots (the "segv in
 *     class_hash_lookup AFTER nested execution" shape).
 * The pre-existing "+4 to max_stack" TEMPORARY FIX in frame_create was a
 * band-aid over the same drift.
 *
 * THIS wrapper: snapshot the caller's stack_top, run the nested method,
 * pick the deposited value up into *result (top slot; J/D high-slot is a
 * full-width copy per the PUSH2 convention) and REWIND the deposit. Void
 * methods and exception unwinds deposit nothing — the rewind is a no-op.
 * The transient +1 write during the deposit itself stays inside the
 * frame's padded allocation (the caller has just been popped-args-clean
 * by the native dispatch, exactly like a normal return). */
int execute_method_nested(JVM* jvm, JavaThread* thread, JavaMethod* method,
                          JavaValue* args, JavaValue* result) {
    if (!jvm || !thread || !method) return -1;

    JavaFrame* caller = thread->current_frame;
    int16_t saved_top = caller ? caller->stack_top : 0;

    int ret = execute_method(jvm, thread, method, args, result);

    if (caller && caller->stack_top > saved_top) {
        /* The method returned a value: op_*return deposited it (1 slot, or
         * 2 identical full-width slots for J/D) on the caller frame. */
        if (result) {
            *result = caller->stack[caller->stack_top];
        }
        caller->stack_top = saved_top;
    }
    return ret;
}

/* v34.26 PERF: sampling-profiler pieces extracted from the interpreter hot
 * loop (see SLOW_CHECK_INTERVAL block inside interpret). Off by default; the
 * loop pays one cached branch per 64 opcodes instead of a branch per opcode. */
#define SLOW_CHECK_INTERVAL 64

/* v34.26 BENCH: calibration counters — real slow-check fire count vs the
 * profiler's fire-count-based instruction estimate (printed by [BENCH]). */
uint64_t g_slowcheck_fires = 0;
uint64_t g_prof_est_instr = 0;

/* v50 DIAG (Asphalt 3 3D stall): one-shot self-stack dump request. The
 * libretro presentation-freeze detector (NOJME_STALL_DIAG) increments this
 * generation counter; every interpreting thread serves it at most once per
 * generation at its 64-instruction slow-check, printing its OWN frame chain
 * (safe — same walker as NOJME_SELF_STACK_EVERY). Threads parked inside
 * natives cannot serve it (that fact alone is diagnostic: a thread missing
 * from [STALLSTACK] output is stuck inside a native call or parked). */
volatile uint32_t g_stall_stack_gen = 0;

/* ============================================================================
 * v41 PERF (Asphalt "freeze every 2 seconds" on armv7): pthread VM-thread
 * budget.
 * ============================================================================
 * Thread.start()-created threads run as real pthreads and previously had NO
 * execution budget: a 2005-era game loop re-hosted on a modern (or
 * NEON-optimized) CPU spins 10-30x faster than real time. Measured on the
 * Asphalt 3 3D race: ~400 MB/s of garbage allocation, the 64 MB heap filling
 * every ~160 ms on x86 (≈ every 2 s on a Cortex-A7), so a stop-the-world GC
 * fires constantly — on the phone-shaped libretro core that surfaces as the
 * reported periodic freeze, and it burns a full core competing with the
 * frontend thread (worse in libretro than headless for exactly that reason).
 * The speedups of v34.24-v34.40 made this WORSE: faster interpreter = faster
 * garbage = more frequent collections.
 *
 * Fix: give runner pthreads the same phone-era pacing the libretro main
 * thread already has (j2me_vm_speed): an instruction allowance per 16.6 ms
 * wall-time window; the excess is slept off in GC-safepoint-aware slices
 * (a sleeping runner still parks promptly for a stop-the-world GC).
 *
 * - Default: 90000 instrs/window ≈ 5.4M/s (the FAST mode of j2me_vm_speed).
 * - NOJME_THREAD_BUDGET=<n>: manual override (0 or negative disables).
 * - libretro.c overrides the default to track the j2me_vm_speed core option
 *   via g_jvm_thread_budget (turbo disables the throttle entirely).
 * Headless: the MIDlet main thread is NOT a runner pthread and is unaffected
 * (bench/verify runs keep their free-run behavior).
 * ============================================================================ */
long g_jvm_thread_budget = -1;  /* -1 = env/default, 0 = off, >0 = per-window */
/* v35.09 [VM-PACED]: ms the runner(s) spent asleep in the budget pacer since
 * the last 10 s diagnostic window reset (see the budget sleep in the
 * slow-check block). Diagnostic aggregate shared across runner threads —
 * benign racy increments, reset by whichever thread crosses the window. */
volatile uint32_t g_vmpace_sleep_ms = 0;
/* v35.13: sleeping time reported for the LAST closed 10 s diagnostic window.
 * Together with g_vmpace_sleep_ms it answers "is the game VM-speed-bound
 * right now" for the frontend loading screen hint (jvm_vmpace_active). */
static volatile uint32_t g_vmpace_sleep_ms_last = 0;

/* v35.13: nonzero while the budget pacer has been eating a large share of
 * the last ~10 s window — the game cannot keep up with its instruction
 * budget ("Скорость ВМ" setting is the remedy). Read by the SDL frontend
 * via a weak extern (same pattern as midp_present_stable_seq). */
int jvm_vmpace_active(void) {
    return (g_vmpace_sleep_ms + g_vmpace_sleep_ms_last) >= 3000;
}

/* ============================================================================
 * v41 CRITICAL (torn-heap fix): the MAIN Java thread does not run as a
 * registered pthread — in the libretro build it is driven by the frontend
 * thread (retro_run budget loop / midp pumps / jvm_run_midlet), in headless
 * by the main process thread. gc_collect()'s stop-the-world census counted
 * ONLY pthread runners, so a runner-triggered collection NEVER asked the
 * main thread to park: the mark/sweep walked the heap while the main
 * mutator kept interpreting mid-instruction (locals holding refs to
 * objects the sweep was freeing). On x86's strongly-ordered memory this
 * mostly "worked"; on armv7's weakly-ordered multi-core it surfaces as
 * random SIGSEGVs, corrupted/black rendering and unexplained hangs.
 *
 * Fix: every driver window that executes main-thread Java brackets itself
 * with jvm_main_thread_exec_begin/end(); the GC census then counts the main
 * thread (it parks at the 64-instruction safepoint poll inside every
 * interpreter loop). Between windows the main thread is quiescent and is
 * correctly NOT counted (nothing to wait for).
 * ============================================================================ */
volatile int g_jvm_main_thread_executing = 0;

void jvm_main_thread_exec_begin(void) {
    extern void jvm_main_thread_exec_nested_inc(void);
    jvm_main_thread_exec_nested_inc();
}

void jvm_main_thread_exec_end(void) {
    extern void jvm_main_thread_exec_nested_dec(void);
    jvm_main_thread_exec_nested_dec();
}

/* Nesting counter (all begin/end pairs run on the same OS thread; a plain
 * int is sufficient — no torn access from other threads matters here
 * because the GC only READS it while every mutator is parked). */
static int s_jvm_main_exec_nesting = 0;
void jvm_main_thread_exec_nested_inc(void) { s_jvm_main_exec_nesting++; g_jvm_main_thread_executing = 1; }
void jvm_main_thread_exec_nested_dec(void) {
    if (s_jvm_main_exec_nesting > 0) s_jvm_main_exec_nesting--;
    if (s_jvm_main_exec_nesting == 0) {
        /* v34.71 CRITICAL (companion to the heap.c TLAB main-thread gate):
         * closing the LAST driver window makes this thread invisible to the
         * GC census (g_jvm_main_thread_executing -> 0 below) while it keeps
         * running pure native C code that never polls safepoints. Any TLAB
         * chunk opened while the window was live MUST be closed HERE —
         * otherwise the next collection's linear sweep walks into the
         * chunk's unflushed tail (stale bytes read as a GCObjectHeader —
         * the "corrupted magic 0x00610063" Nescube forensics). Between
         * windows the allocator's gate keeps this thread on the slow path,
         * whose ticket spin parks it at every GC request. */
        {
            extern void heap_tlab_flush_self(void);
            heap_tlab_flush_self();
        }
        /* v34.67 CRITICAL FIX (GC-safepoint starvation = the "settings screen
         * hang"): closing the LAST driver window means the main thread is
         * quiescent BY CONSTRUCTION until the next window opens. On
         * event-driven screens (a Form rendered natively, key events with no
         * Java method to run) the main thread executes NO bytecode between
         * windows, so the interpreter's 64-instruction safepoint poll NEVER
         * fires — while the GC census counted it (executing==1 all frame).
         * A game thread that trips a collection then sat through the full
         * 2000ms wait_arrivals timeout, ABORTED, retried on the next
         * allocation, timed out again... its UI-builder thread crawled at
         * one GC-abort per allocation (repro: Nescube settings — thread 'o'
         * stuck ~2s+ per collection; the Form's commands were never added
         * in time and BOTH soft keys died with "null-commands-array" — the
         * user pressed "Cancel" and the game appeared frozen). Park HERE,
         * while still counted: arrival + broadcast wakes the collector
         * instantly, the collection completes normally instead of aborting,
         * and the frontend thread loses only the park time it would have
         * spent sleeping between frames anyway. */
        if (g_jvm_main_thread_executing && g_gc_safepoint_request) {
            jvm_gc_safepoint_park();
        }
        g_jvm_main_thread_executing = 0;
    }
}

/* v34.29 DIAG: REAL-RATE sampling profiler (NOJME_PROFILE=1).
 * v34.26's sampler hung off the batched slow-check (every 64 opcodes in ONE
 * frame's interpret() invocation). But execute_method() RECURSES: every Java
 * call gets a fresh interpret() with a fresh countdown, so methods shorter
 * than 64 opcodes never fire the slow check at all (measured: 489M real
 * instructions vs 704 estimated — 99.999% of the workload invisible to the
 * profiler). This sampler keys off the JVM-global instruction counter
 * instead, so every opcode counts regardless of frame depth. */
static void prof_sample_real(JavaFrame* frame, uint64_t real_instr) {
    static uint32_t prof_method_hits[96];
    static const char* prof_method_names[96];
    static int prof_method_n = 0;
    static uint64_t prof_calls = 0;
    static uint64_t prof_next_dump = 1024ULL;  /* first dump comes fast */
    static uint64_t prof_last_real = 0;
    static uint64_t prof_last_ms = 0;

    char buf[80];
    const char* cn = (frame->clazz && frame->clazz->class_name) ? frame->clazz->class_name : "?";
    const char* mn = frame->method ? frame->method->name : "?";
    const char* md = (frame->method && frame->method->descriptor) ? frame->method->descriptor : "";
    snprintf(buf, sizeof(buf), "%.44s.%.22s%.14s", cn, mn, md);
    int found = -1;
    for (int i = 0; i < prof_method_n; i++) {
        if (prof_method_names[i] && strcmp(prof_method_names[i], buf) == 0) { found = i; break; }
    }
    if (found < 0 && prof_method_n < 96) {
        prof_method_names[prof_method_n] = strdup(buf);
        found = prof_method_n++;
    }
    if (found >= 0) prof_method_hits[found]++;

    prof_calls++;
    if (prof_calls >= prof_next_dump) {
        prof_next_dump = prof_calls + 1024ULL;  /* every ~1024*16K = 16M instr */
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t now_ms = (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
        ALWAYS_LOG("[PROF-REAL] instr=%llu (+%llu) elapsed_ms=%llu rate=%.0fK/s\n",
                   (unsigned long long)real_instr,
                   (unsigned long long)(real_instr - prof_last_real),
                   (unsigned long long)(now_ms - prof_last_ms),
                   prof_last_ms ? (double)(real_instr - prof_last_real) / (double)(now_ms - prof_last_ms) : 0.0);
        /* top-12 methods by hits */
        int idx[96];
        for (int i = 0; i < prof_method_n; i++) idx[i] = i;
        for (int i = 0; i < prof_method_n; i++)
            for (int j = i + 1; j < prof_method_n; j++)
                if (prof_method_hits[idx[j]] > prof_method_hits[idx[i]]) { int t = idx[i]; idx[i] = idx[j]; idx[j] = t; }
        for (int i = 0; i < prof_method_n && i < 12; i++)
            if (prof_method_hits[idx[i]] > 0)
                ALWAYS_LOG("[PROF-REAL] %7u %s\n", prof_method_hits[idx[i]], prof_method_names[idx[i]]);
        prof_last_real = real_instr;
        prof_last_ms = now_ms;
    }
}

static void prof_sample(JavaFrame* frame) {
    /* v34.26: self-contained counters. The old version keyed the rate dump
     * off `jvm->instr_count % 10000000 == 0` — with the checks now batched to
     * every 64 opcodes AND several threads incrementing instr_count
     * concurrently, the exact multiple is routinely skipped and the dump
     * never fires. Count our own sampling cadence instead: one call ≈
     * SLOW_CHECK_INTERVAL instructions. */
    static uint64_t prof_calls = 0;
    static uint64_t prof_instr_est = 0;
    static uint64_t prof_next_dump = 10000000ULL;
    static uint64_t prof_last_ms = 0;
    static uint32_t prof_method_hits[64];
    static const char* prof_method_names[64];
    static int prof_method_n = 0;

    uint64_t call = ++prof_calls;
    /* histogram: every 1M instructions ≈ 15625 calls */
    if ((call % 15625ULL) == 0) {
        char buf[80];
        const char* cn = (frame->clazz && frame->clazz->class_name) ? frame->clazz->class_name : "?";
        const char* mn = frame->method ? frame->method->name : "?";
        const char* md = frame->method && frame->method->descriptor ? frame->method->descriptor : "";
        snprintf(buf, sizeof(buf), "%.40s.%.20s%.12s", cn, mn, md);
        int found = -1;
        for (int i = 0; i < prof_method_n; i++) {
            if (prof_method_names[i] && strcmp(prof_method_names[i], buf) == 0) { found = i; break; }
        }
        if (found < 0 && prof_method_n < 64) {
            prof_method_names[prof_method_n] = strdup(buf);
            found = prof_method_n++;
        }
        if (found >= 0) prof_method_hits[found]++;
    }
    /* rate/histogram dump every ~10M instructions. v34.26: ALWAYS_LOG, not
     * fprintf — plain fprintf is intercepted by the log gate and the dumps
     * silently vanish when logging is disabled. */
    prof_instr_est += SLOW_CHECK_INTERVAL;
    g_prof_est_instr = prof_instr_est;
    if (prof_instr_est >= prof_next_dump) {
        prof_next_dump += 10000000ULL;
        struct timespec prof_ts;
        clock_gettime(CLOCK_MONOTONIC, &prof_ts);
        uint64_t now_ms = (uint64_t)prof_ts.tv_sec * 1000U + (uint64_t)prof_ts.tv_nsec / 1000000U;
        ALWAYS_LOG("[PROFILE] instr=%llu elapsed_ms=%llu rate=%.0f K/s\n",
                (unsigned long long)prof_instr_est,
                (unsigned long long)(now_ms - prof_last_ms),
                prof_last_ms ? 10000000.0 / (double)(now_ms - prof_last_ms) : 0.0);
        prof_last_ms = now_ms;
        for (int i = 0; i < prof_method_n; i++)
            if (prof_method_hits[i] > 0)
                ALWAYS_LOG("[PROFILE] %8u %s\n", prof_method_hits[i], prof_method_names[i]);
    }
}

/*
 * v34.35 PERF (ARMv7): fast interpreter core.
 *
 * The v34.34 loop paid, PER BYTECODE: ~29 ARM insns of loop scaffolding
 * (dispatch through the opcode_table function-pointer array: reload
 * jvm/thread/frame from the stack frame, `blx r3` + handler prologue/
 * epilogue push/pop, then reload frame->pc/frame->code/frame->stack_top
 * the call might have dirtied) plus per-opcode memory RMWs (jvm->instr_count,
 * the prof_real_on static, the instr_limit field). Simple opcodes (ILOAD_0)
 * executed ~50 instructions of overhead for ~4 of work.
 *
 * This version keeps the hot interpreter state (pc, code, operand stack top,
 * locals pointer, instruction counter) in C locals — REGISTERS on both
 * x86-64 and ARMv7 — and dispatches through a dense switch whose hot cases
 * (~70 opcodes: const/load/store/int+float arith/branches/array ops/iinc)
 * are INLINED with semantics bit-matched to the opcodes.c handlers:
 *  - IADD/ISUB/IMUL/IAND/IOR/IXOR: identical int expressions.
 *  - ISHL/ISHR/IUSHR: shift counts masked with 0x1F like op_shl/shr/ushr.
 *  - IDIV/IREM: zero divisor DEOPTS to the handler (exact throw path);
 *    INT_MIN/-1 special cases (IDIV->INT_MIN, IREM->0) replicated.
 *  - Array load/store: peek+validate (null, 0<=idx<len, known element type)
 *    then commit; ANY miss deopts to op_array_load/op_array_store with the
 *    operand stack un-popped so the handler re-executes and throws (NPE /
 *    AIOOBE diag + throw / DRM-bypass) exactly as before. The inline path
 *    skips array_get/array_set's __sync_synchronize() full memory barrier:
 *    the GC is stop-the-world and only runs at the 64-opcode safepoint
 *    (state is synced to the frame BEFORE parking), so plain stores are
 *    sufficient; racy cross-thread array reads have no defined Java
 *    semantics either way.
 *  - Branch target arithmetic: op_if and op_goto compute the target as
 *    pc_of_opcode + (int16_t)offset — replicated (NOT pc+3+offset).
 *
 * Cold opcodes (invoke*, get/putfield, ldc, new, conversions, long math...)
 * sync state back to the frame, set throwing_pc, and call the SAME handler
 * through the SAME table as v34.34; on return the locals are re-synced.
 * J2ME_DEBUG builds skip the fast path entirely (checked PUSH/POP macros
 * must not be bypassed). g_j2me_runtime_debug (F12) forces the cold path
 * per-opcode so opcode tracing is unchanged.
 *
 * jvm->instr_count is batched in a register and flushed at every exit,
 * safepoint and yield; the per-opcode limit check uses the register copy
 * (the limit itself is immutable after the first env read), so
 * J2ME_MAX_INSTRUCTIONS semantics are unchanged.
 */

/* Main bytecode interpreter - NO setjmp/longjmp for thread safety
 *
 * Each thread has its own call stack, so exceptions are propagated via return values.
 * This avoids the problem where Thread 2's jmp_buf would corrupt Thread 1's exception handling.
 */
static int interpret(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    if (!frame || !frame->code) {
        ERROR_LOG("interpret: NULL frame or code");
        return -1;
    }

    EXEC_DEBUG("interpret: Starting at PC=0, code_length=%u", frame->code_length);

    /* v34.26 PERF (armv7 stutter): countdown timers held in REGISTERS. */
    uint32_t slow_check_countdown = SLOW_CHECK_INTERVAL;
    uint32_t sched_countdown = YIELD_INTERVAL;
    /* v36.57 [SPIN-BREAK]: отложенный запрос на диспетчеризацию исключения
     * через ШТАТНЫЙ поиск обработчиков текущего кадра (см. exception_search). */
    int tls_sb_inject = 0;

    if (!jvm->instr_env_checked) {
        jvm->instr_env_checked = true;
        const char* max_instr_env = getenv("J2ME_MAX_INSTRUCTIONS");
        if (max_instr_env) {
            jvm->instr_limit = strtoull(max_instr_env, NULL, 10);
        }
    }

    /* v34.29 prof_real_on: resolve the lazy static ONCE per interpret() call
     * (the old per-opcode static load cost 2 ARM insns per bytecode). */
    int prof_real_local = 0;
    {
        static int prof_real_on = -1;
        if (prof_real_on < 0) {
            prof_real_on = getenv("NOJME_PROFILE") ? 1 : 0;
        }
        prof_real_local = prof_real_on;
    }

    /* v34.35 PERF: hot interpreter state cached in locals (registers). */
    uint32_t f_pc = frame->pc;
    uint8_t* code = frame->code;
    uint32_t code_len = frame->code_length;
    JavaValue* stack = frame->stack;
    int st = frame->stack_top;
    JavaValue* locals = frame->locals;
    uint64_t icount = jvm->instr_count;   /* batched; flushed at exits/safepoints */
    uint64_t limit = jvm->instr_limit;    /* immutable after env read */

continue_execution:
    ;  /* declarations above; exception-handler gotos resume here */
    f_pc = frame->pc;
    st = frame->stack_top;
    icount = jvm->instr_count;

    while (f_pc < code_len && jvm->running) {
        if (__builtin_expect(--slow_check_countdown == 0, 0)) {
            slow_check_countdown = SLOW_CHECK_INTERVAL;
            jvm->instr_count = icount;
            frame->pc = f_pc;
            frame->stack_top = (int16_t)st;
            g_slowcheck_fires++;  /* v34.26 BENCH calibration */
            if (g_gc_safepoint_request) {
                jvm_gc_safepoint_park();
                /* v36.57 [SPIN-BREAK]: парковка — НЕ спин; возраст
                 * last_native_ms refresh-им, чтобы STW-пауза коллектора
                 * не воспринималась брейкером как чисто-Java цикл. */
                {
                    JavaThread* sp = thread_current(jvm);
                    if (sp) sp->last_native_ms = jvm_exec_now_ms();
                }
            }
            /* v34.81 FRONTEND PAUSE: одна volatile-читка на 64 инструкции
             * (тот же каденс, что GC-опрос). Латент поддерживает eval-поток
             * и park-циклы wait/sleep; здесь поток только парковки ждёт.
             * В headless/SDL-сборках латент всегда 0 — ноль ветвлений. */
            {
                extern volatile int g_frontend_pause_active;
                if (g_frontend_pause_active) {
                    extern void jvm_frontend_pause_park(void);
                    jvm_frontend_pause_park();
                    /* v36.57 [SPIN-BREAK]: минутное меню паузы не должно
                     * взводить брейкер — после парковки bytecode свежий. */
                    {
                        JavaThread* sp = thread_current(jvm);
                        if (sp) sp->last_native_ms = jvm_exec_now_ms();
                    }
                }
            }
            /* v36.30 [EXIT-FENCE]: пока фрейм-поток исполняет Java
             * (destroyApp/timers/keys/pointer/commandAction), pthread-раннеры
             * держатся на этом опросе — два интерпретатора против одной кучи
             * (краш-репорт пользователя: Data Abort при выходе). Main-файбер
             * и фрейм-поток — один OS-поток, ему ждать не нужно. */
            if (g_vm_fiber_fence && tls_os_thread_is_vm_runner) {
                jvm_frame_fence_wait_public();
                /* v36.57 [SPIN-BREAK]: fence — событийная парковка. */
                {
                    JavaThread* sp = thread_current(jvm);
                    if (sp) sp->last_native_ms = jvm_exec_now_ms();
                }
            }
            /* v41 PERF: pthread VM-thread budget (see the block comment near
             * g_jvm_thread_budget above). Checked every 256 slow-checks =
             * 16k instructions so the clock read costs ~0.002% of the run.
             * Thread-local window state — every runner thread is paced
             * independently. The headless main thread and the libretro
             * frontend-driven main thread are NOT runners and skip this. */
            {
                static _Thread_local uint32_t tl_thr_tick;
                if (++tl_thr_tick >= 256) {
                    tl_thr_tick = 0;
                    long thr_budget = g_jvm_thread_budget;
                    if (thr_budget < 0) {
                        /* one-time env/default resolution (process-wide) */
                        static long s_env_budget = 90000;
                        static int s_env_resolved = 0;
                        if (!s_env_resolved) {
                            const char* e = getenv("NOJME_THREAD_BUDGET");
                            if (e) s_env_budget = atol(e);
                            s_env_resolved = 1;
                        }
                        thr_budget = s_env_budget;
                    }
                    if (thr_budget > 0 && jvm->running) {
                        extern int jvm_current_os_thread_is_vm_runner(void);
                        if (jvm_current_os_thread_is_vm_runner()) {
                            /* v41 DIAG: NOJME_THRDIAG=1 — heartbeat that the
                             * runner throttle is engaged (one line per 500
                             * checks). Silent by default. */
                            {
                                static int s_diag_on = -1;
                                if (s_diag_on < 0) {
                                    const char* e = getenv("NOJME_THRDIAG");
                                    s_diag_on = (e && e[0] && e[0] != '0') ? 1 : 0;
                                }
                                if (s_diag_on) {
                                    static uint32_t s_thr_diag = 0;
                                    if ((++s_thr_diag % 500) == 1) {
                                        char line[96];
                                        int ln = snprintf(line, sizeof(line),
                                            "[THRDIAG] throttle check #%u budget=%ld\n",
                                            s_thr_diag, thr_budget);
                                        if (ln > 0) fwrite(line, 1, (size_t)ln, stderr);
                                    }
                                }
                            }
                            static _Thread_local uint64_t tl_window_us;
                            static _Thread_local uint64_t tl_window_instr;
                            struct timespec tsn;
                            clock_gettime(CLOCK_MONOTONIC, &tsn);
                            uint64_t now_us = (uint64_t)tsn.tv_sec * 1000000ULL +
                                              (uint64_t)tsn.tv_nsec / 1000ULL;
                            if (tl_window_us == 0 || now_us - tl_window_us >= 16600ULL) {
                                tl_window_us = now_us;
                                tl_window_instr = 0;
                            }
                            tl_window_instr += 16384;  /* one check period */
                            if (tl_window_instr > (uint64_t)thr_budget) {
                                /* v35.09 [VM-PACED]: field case (asia rally
                                 * raced at ~1 fps) proved the interpreter
                                 * budget can be the bottleneck for games
                                 * whose bytecode demands more than the
                                 * ceiling (their own Java-side 3D math).
                                 * This counter + throttled forced line make
                                 * that class VISIBLE on device with logging
                                 * off: when a runner spends a third of its
                                 * wall time asleep in THIS loop, the game is
                                 * VM-speed-bound — the remedy is the
                                 * "Скорость ВМ" setting (global or per-game
                                 * via the PLUS menu). */
                                {
                                    static volatile long long g_vmpace_win_t0;
                                    struct timespec vts;
                                    uint64_t vnow_ms;
                                    clock_gettime(CLOCK_MONOTONIC, &vts);
                                    vnow_ms = (uint64_t)vts.tv_sec * 1000ULL +
                                              vts.tv_nsec / 1000000ULL;
                                    if (g_vmpace_win_t0 == 0 ||
                                        vnow_ms - g_vmpace_win_t0 >= 10000) {
                                        uint32_t slept = g_vmpace_sleep_ms;
                                        g_vmpace_sleep_ms = 0;
                                        g_vmpace_sleep_ms_last = slept; /* v35.13 */
                                        g_vmpace_win_t0 = (long long)vnow_ms;
                                        if (slept >= 3000) {
                                            extern void sw_trace_force(const char* fmt, ...)
                                                __attribute__((weak));
                                            if (&sw_trace_force && sw_trace_force) {
                                                sw_trace_force("[VM-PACED] VM runner slept %u ms of the last 10 s pacing to %ld instr/16.6ms — game is VM-speed-bound; raise \"Скорость ВМ\" (global or per-game in PLUS)",
                                                               slept, thr_budget);
                                            }
                                        }
                                    }
                                }
                                /* Sleep off the rest of this window in 2 ms
                                 * slices so a stop-the-world GC request is
                                 * still honored within one slice (the park
                                 * releases when the collector finishes). */
                                /* v34.98: publish the park reason for the td dump. */
                                {
                                    JavaThread* self = thread_current(jvm);
                                    if (self) snprintf(self->park_why, sizeof(self->park_why), "budget");
                                }
                                uint64_t vpace_ms = 0;
                                while (jvm->running) {
                                    uint64_t deadline = tl_window_us + 16600ULL;
                                    clock_gettime(CLOCK_MONOTONIC, &tsn);
                                    uint64_t n2 = (uint64_t)tsn.tv_sec * 1000000ULL +
                                                 (uint64_t)tsn.tv_nsec / 1000ULL;
                                    if (n2 >= deadline) break;
                                    if (g_gc_safepoint_request) {
                                        jvm_gc_safepoint_park();
                                        continue;
                                    }
#ifdef _WIN32
                                    Sleep(2);
#else
                                    struct timespec req = { .tv_sec = 0, .tv_nsec = 2000000 };
                                    nanosleep(&req, NULL);
#endif
                                    vpace_ms += 2;
                                }
                                g_vmpace_sleep_ms += (uint32_t)vpace_ms;
                                {
                                    JavaThread* self = thread_current(jvm);
                                    if (self) self->park_why[0] = '\0';
                                }
                                clock_gettime(CLOCK_MONOTONIC, &tsn);
                                tl_window_us = (uint64_t)tsn.tv_sec * 1000000ULL +
                                               (uint64_t)tsn.tv_nsec / 1000ULL;
                                tl_window_instr = 0;
                            }
                        }
                    }
                }
            }
            {
                extern volatile int midp_game_thread_id;
                JavaThread* self = thread_current(jvm);
                if (self && self->id == midp_game_thread_id) {
                    extern void midp_process_pending_keys(JVM* jvm);
                    midp_process_pending_keys(jvm);
                }
            }
            {
                static int prof_on = -1;
                if (prof_on < 0) prof_on = getenv("NOJME_PROFILE") ? 1 : 0;
                if (prof_on) prof_sample(frame);
            }
            /* v38 DIAG: NOJME_SELF_STACK_EVERY=<n> — every n slow-checks the
             * EXECUTING thread prints its own full Java call stack (safe: own
             * frames only, same walker as GCDIAG). For hang triage: shows the
             * caller chain above a spin/sleep loop per thread. */
            {
                static int ss_every = -1;
                static uint32_t ss_counter = 0;
                if (ss_every < 0) {
                    const char* e = getenv("NOJME_SELF_STACK_EVERY");
                    ss_every = (e && atoi(e) > 0) ? atoi(e) : 0;
                }
                if (ss_every && (++ss_counter % (uint32_t)ss_every) == 0) {
                    JavaThread* self = thread_current(jvm);
                    if (self) {
                        JavaFrame* f = self->current_frame;
                        int d = 0;
                        /* NOTE: no log_lock() here — j2me_log_ungated() takes
                         * the same mutex internally (non-recursive). */
                        j2me_log_ungated("[SELFSTACK] tid=%d depth-begin\n", self->id);
                        while (f && d < 24) {
                            j2me_log_ungated("[SELFSTACK]   [%d] %s.%s pc=%u\n", d,
                                             f->clazz && f->clazz->class_name ? f->clazz->class_name : "?",
                                             f->method && f->method->name ? f->method->name : "?",
                                             (unsigned)f->pc);
                            f = f->prev; d++;
                        }
                        j2me_log_ungated("[SELFSTACK] tid=%d depth-end\n", self->id);
                    }
                }
            }
            /* v50 DIAG (Asphalt 3 3D stall): serve the one-shot freeze-dump
             * request. Each thread dumps at most once per generation; own
             * frames only (safe while running). See g_stall_stack_gen. */
            {
                extern volatile uint32_t g_stall_stack_gen;
                static _Thread_local uint32_t tl_stall_served = 0;
                uint32_t stall_gen = g_stall_stack_gen;
                if (stall_gen && tl_stall_served != stall_gen) {
                    tl_stall_served = stall_gen;
                    JavaThread* self = thread_current(jvm);
                    if (self) {
                        JavaFrame* f = self->current_frame;
                        int d = 0;
                        j2me_log_ungated("[STALLSTACK] tid=%d (gen %u) dump-begin\n",
                                         self->id, stall_gen);
                        while (f && d < 24) {
                            j2me_log_ungated("[STALLSTACK]   [%d] %s.%s pc=%u\n", d,
                                             f->clazz && f->clazz->class_name ? f->clazz->class_name : "?",
                                             f->method && f->method->name ? f->method->name : "?",
                                             (unsigned)f->pc);
                            f = f->prev; d++;
                        }
                        j2me_log_ungated("[STALLSTACK] tid=%d dump-end\n", self->id);
                    }
                }
            }
            /* v36.57 [SPIN-BREAK]: прерыватель чисто-Java спинов.
             *
             * ПОЛЕВОЙ КОРЕНЬ (Gravity Defied 240x320, лог v36.54-goffix1):
             * игровой поток входит в безграничный цикл депенетрации физики
             * (b.u: do { ca(); } while (ba() != 2) — мотоцикл зажат
             * геометрией, нормали коллизии осциллируют) ВНУТРИ
             * synchronized b.do() и крутится 5-10+ СЕКУНД чистого байткода
             * без единого натива (td t2: run nat=currentTimeMillis
             * age=9661), держа монитор физики; pump-поток блокируется в
             * monitor_enter внутри paint() — STUCK stage=repaints, мёртвый
             * ввод, CPU 0%. Игра (мод «pascha») лечится только сама — но
             * ЭМУЛЯТОР обязан такое переживать.
             *
             * ДЕТЕКТОР: время с последней диспетчеризации НАТИВА (поле
             * last_native_ms обновляет каждый натив) больше порога — поток
             * физически не может исполнять корректный игровой цикл 1.5 с
             * БЕЗ единого вызова (рендер/время/аллокация — всё нативы);
             * парковки (GC/пауза/fence) сбрасывают возраст выше.
             *
             * ЛЕКАРСТВО: RuntimeException в СОБСТВЕННОМ потоке на ближайшем
             * slow-check: unwind идёт по кадрам с поиском catch-обработчиков
             * (диспетчер cold-пути), v34.99 MONITOR-UNWIND освобождает
             * мониторы кадров — у GD это catch(Exception) в Micro.run,
             * игра подхватывает и продолжает со следующего кадра (частичный
             * прогресс депенетрации сохраняется — джем сходится за
             * несколько срабатываний). У игр без catch исключение всплывает
             * до uncaught-обработчика — лучше ветка [MIDLET-STUCK], чем
             * вечный фриз. Спин-кадр сам свои catch пропускает (выход -1
             * из interpret минуя поиск в этом кадре) — обработчики ВЫШЕ по
             * стеку ищутся штатно.
             *
             * Порог NOJME_SPIN_BREAK_MS (по умолчанию 1500, 0 = выкл). */
            {
                static long s_spin_break_ms = -1;
                if (s_spin_break_ms < 0) {
                    const char* e = getenv("NOJME_SPIN_BREAK_MS");
                    s_spin_break_ms = (e && atoi(e) >= 0) ? atoi(e) : 1500;
                }
                if (s_spin_break_ms > 0 && jvm->running) {
                    JavaThread* self_sb = thread_current(jvm);
                    if (self_sb && self_sb->last_native_ms) {
                        int64_t sb_now = jvm_exec_now_ms();
                        int64_t sb_age = sb_now - (int64_t)self_sb->last_native_ms;
                        if (sb_age > (int64_t)s_spin_break_ms) {
                            extern void sw_trace_force(const char* fmt, ...)
                                __attribute__((weak));
                            const char* sb_site =
                                self_sb->jsite[0] ? self_sb->jsite : "(?)";
                            if (&sw_trace_force && sw_trace_force) {
                                sw_trace_force("[SPIN-BREAK] tid=%d pure-Java spin %lld ms (site %s) — injecting RuntimeException; unwind releases monitors, catch recovers",
                                               self_sb->id, (long long)sb_age,
                                               sb_site);
                            }
                            LOG_SAFE("[SPIN-BREAK] tid=%d pure-Java spin %lld ms (site %s) — injecting RuntimeException\n",
                                     self_sb->id, (long long)sb_age, sb_site);
                            /* re-arm: следующий выстрел не раньше чем через
                             * порог (catch-обработчик исполняет байткод,
                             * нативов может всё ещё не быть) */
                            self_sb->last_native_ms = sb_now;
                            jvm->instr_count = icount;
                            frame->pc = f_pc;
                            frame->stack_top = (int16_t)st;
                            jvm_throw_by_name(jvm, "java/lang/RuntimeException",
                                              "nojme spin-breaker: pure-Java logic stall");
                            /* ВАЖНО: НЕ return — исключение диспетчеризуется
                             * НА СЛЕДУЮЩЕЙ ИТЕРАЦИИ через штатный поиск
                             * обработчиков ЭТОГО кадра (exception_search):
                             * catch в том же методе, что и спин (как в GD-подобных
                             * циклах и в TestSpinBreak), работает как на JVM. */
                            tls_sb_inject = 1;
                        }
                    }
                }
            }
            f_pc = frame->pc;
            st = frame->stack_top;
            /* v34.35 FIX: re-seed the cached counter — the block above may run
             * NESTED Java (key delivery) which advances jvm->instr_count. */
            icount = jvm->instr_count;
        }

        if (__builtin_expect(tls_sb_inject, 0)) {
            /* v36.57 [SPIN-BREAK]: исключение брейкера диспетчеризуется как
             * обычное athrow-подобное — с поиском catch-обработчиков в ЭТОМ
             * кадре (exception_search). Найден — выполнение продолжится с
             * handler_pc; нет — штатный unwind наверх (следующие кадры
             * ищутся их собственными interpret-ами). */
            tls_sb_inject = 0;
            frame->pc = f_pc + 1;
            frame->stack_top = (int16_t)st;
            frame->throwing_pc = (int)f_pc;
            jvm->instr_count = icount;
            goto exception_search;
        }

        icount++;

        if (__builtin_expect(prof_real_local && (icount & 0x3FFFULL) == 0, 0)) {
            jvm->instr_count = icount;
            frame->pc = f_pc;
            frame->stack_top = (int16_t)st;
            prof_sample_real(frame, icount);
        }
        if (__builtin_expect(limit > 0 && icount > limit, 0)) {
            jvm->instr_count = icount;
            LOG_SAFE("[JVM] Instruction limit reached (%llu instructions). Stopping.\n",
                    (unsigned long long)icount);
            jvm->running = false;
            return -1;
        }

        uint8_t op = code[f_pc];

#ifdef NOJME_STKDIAG
        /* v36.04 STKDIAG: catch operand-stack drift BEFORE an OOB write.
         * The frame allocation is padded (+24 slots) so the drift itself no
         * longer trips ASAN; this check names the opcode stream that walked
         * st past the REAL frame max (code.max_stack + 4). The ring records
         * only ops of the SAME frame that fired (nested frames would
         * otherwise overwrite the history). */
        {
            /* v36.38-BT2: ring enlarged 32 -> 2048 — the Bounce Tales drift
             * in c.a(Lf;)V originates hundreds of instructions before the
             * stack actually overflows; 32 entries only showed the tail. */
            static _Thread_local uint8_t  dg_op[2048];
            static _Thread_local uint32_t dg_pc[2048];
            static _Thread_local int32_t  dg_st[2048];
            static _Thread_local JavaFrame* dg_frame = NULL;
            static _Thread_local int dg_i_local = 0;
            if (dg_frame != frame) { memset(dg_op, 0, sizeof(dg_op)); dg_frame = frame; }
            /* v36.38-BT2: fired at st >= code.max_stack (frame->max_stack-4),
             * NOT st > frame->max_stack — a drift that stops exactly at the
             * +4 padding never tripped the old check, but the padded slots
             * hold garbage the method later pops (Bounce Tales: polygon
             * points corrupted -> ldiv /by zero at c.a PC=1526, ball falls
             * through the ground; ASan caught the write at st==max_stack). */
            if (__builtin_expect(st >= (int)frame->max_stack - 4, 0)) {
                LOG_SAFE("[STK-OVER] %s.%s pc=%u st=%d max=%d (code.max_stack+4)\n",
                         frame->clazz && frame->clazz->class_name ? frame->clazz->class_name : "?",
                         frame->method && frame->method->name ? frame->method->name : "?",
                         (unsigned)f_pc, (int)st, (int)frame->max_stack);
                for (int j = 0; j < 2048; j++) {
                    int idx = (dg_i_local + j) % 2048;
                    if (dg_op[idx]) {
                        LOG_SAFE("[STK-OVER]   op=%02X pc=%u st_in=%d\n",
                                 dg_op[idx], (unsigned)dg_pc[idx], dg_st[idx]);
                    }
                }
                jvm->running = false;
                return -2;
            }
            dg_op[dg_i_local] = op; dg_pc[dg_i_local] = f_pc; dg_st[dg_i_local] = st;
            dg_i_local = (dg_i_local + 1) % 2048;
        }
#endif

        if (__builtin_expect(--sched_countdown == 0, 0)) {
            sched_countdown = YIELD_INTERVAL;
            g_instruction_counter += YIELD_INTERVAL;
            jvm->instr_count = icount;
            frame->pc = f_pc;
            frame->stack_top = (int16_t)st;
            /* [DUMP-FENCE] v36.48: каждые 64 yield (64k инструкций) —
             * обновить строчный снапшот сайта потока. Дёшево (один snprintf
             * ~раз в 1-2 мс реального времени), но закрывает диагностике
             * доступ к кадрам из чужого потока. */
            {
                static _Thread_local uint32_t s_site_div = 0;
                if ((++s_site_div & 63) == 0) {
                    jvm_thread_note_site(thread, frame);
                }
            }
            {
                extern void midp_media_poll_events(JVM* jvm, JavaThread* thread);
                midp_media_poll_events(jvm, thread);
            }
            thread_yield(jvm);
            f_pc = frame->pc;
            st = frame->stack_top;
            /* v34.35 FIX: re-seed — thread_yield may run other threads, and
             * media polling above may run nested Java. */
            icount = jvm->instr_count;
        }

        /* Opcode tracing / debug semantics: force the cold handler path. */
        if (__builtin_expect(g_j2me_runtime_debug, 0)) goto cold_dispatch;


#if !J2ME_DEBUG
        switch (op) {
        /* ---------------- constants ---------------- */
        case OPC_NOP:
            f_pc++;
            break;
        case OPC_ACONST_NULL:
            stack[++st].raw = 0;   /* full 8-byte null ref (high half matters) */
            f_pc++;
            break;
        case OPC_ICONST_M1: case OPC_ICONST_0: case OPC_ICONST_1:
        case OPC_ICONST_2: case OPC_ICONST_3: case OPC_ICONST_4:
        case OPC_ICONST_5:
            stack[++st].raw = (uint64_t)(uint32_t)((jint)op - (jint)OPC_ICONST_0);
            f_pc++;
            break;

        /* ---------------- local loads ---------------- */
        case OPC_ILOAD: case OPC_FLOAD: case OPC_ALOAD:
            stack[++st] = locals[code[f_pc + 1]];
            f_pc += 2;
            break;
        /* Load groups (iload_n/lload_n/fload_n/dload_n/aload_n) all start at
         * an opcode ≡ 2 (mod 4), so (op + 2) & 3 yields the local index 0-3.
         * (Plain `op & 3` would be WRONG: e.g. ALOAD_0 = 0x2A -> 2.) */
        case OPC_ILOAD_0: case OPC_ILOAD_1: case OPC_ILOAD_2: case OPC_ILOAD_3:
        case OPC_FLOAD_0: case OPC_FLOAD_1: case OPC_FLOAD_2: case OPC_FLOAD_3:
        case OPC_ALOAD_0: case OPC_ALOAD_1: case OPC_ALOAD_2: case OPC_ALOAD_3:
            stack[++st] = locals[(op + 2) & 3];
            f_pc++;
            break;

        /* ---------------- local stores ---------------- */
        case OPC_ISTORE: case OPC_FSTORE: case OPC_ASTORE:
            locals[code[f_pc + 1]] = stack[st--];
            f_pc += 2;
            break;
        /* Store groups (istore_n/.../astore_n) start at an opcode ≡ 3 (mod 4):
         * (op + 1) & 3 yields the local index 0-3. */
        case OPC_ISTORE_0: case OPC_ISTORE_1: case OPC_ISTORE_2: case OPC_ISTORE_3:
        case OPC_FSTORE_0: case OPC_FSTORE_1: case OPC_FSTORE_2: case OPC_FSTORE_3:
        case OPC_ASTORE_0: case OPC_ASTORE_1: case OPC_ASTORE_2: case OPC_ASTORE_3:
            locals[(op + 1) & 3] = stack[st--];
            f_pc++;
            break;

        /* ---------------- iinc ---------------- */
        case OPC_IINC: {
            uint8_t idx = code[f_pc + 1];
            int8_t c = (int8_t)code[f_pc + 2];
            locals[idx].i += c;
            f_pc += 3;
            break;
        }

        /* ============ v36.37 PERF: extended inline set ============
         * Alien Shooter 3D (NET Lizard soft-rendered 3D): gprof + PROF-REAL
         * showed the game's Java-side rasterizers burn ~60% of instructions
         * in opcodes that fell to the COLD handler path (function call +
         * frame sync + PUSH/POP through frame pointers):
         *   op_getstatic 2.0M, op_ldc_w 1.96M, op_sipush 1.17M,
         *   op_convert 1.09M, op_lcmp 261K (+ long math in the 3D timing
         *   code: f.a(JJIIII...), lload/lstore pairs).
         * All cases below are bit-matched to the opcodes.c handlers; ANY
         * miss (null receiver, bad CP tag, locals out of range) deopts to
         * the SAME handler with the state un-consumed, so exceptions and
         * DRM bypass behave identically. */
        case OPC_SIPUSH:
            stack[++st].i = (jint)(int16_t)(uint16_t)(((uint16_t)code[f_pc + 1] << 8) | (uint16_t)code[f_pc + 2]);
            f_pc += 3;
            break;

        case OPC_BIPUSH:
            stack[++st].i = (jint)(int8_t)code[f_pc + 1];
            f_pc += 2;
            break;

        /* v36.37 wave 2: gprof (A3D in-game 200 s) — op_bipush 67M,
         * op_mul(DMUL) 17.9M, dconst 5.5M cold calls. FADD/FSUB/FMUL/FDIV
         * were ALREADY inline in v36.36. Same contract: bit-matched, miss
         * -> deopt. */
        case OPC_FCONST_0: case OPC_FCONST_1: case OPC_FCONST_2:
            stack[++st].raw = 0;
            stack[st].f = (jfloat)(op - OPC_FCONST_0);
            f_pc++;
            break;
        case OPC_LCONST_0: case OPC_LCONST_1:
            stack[++st].raw = 0;
            stack[st].j = (jlong)(op - OPC_LCONST_0);
            stack[st + 1] = stack[st];
            st++;
            f_pc++;
            break;
        case OPC_DCONST_0: case OPC_DCONST_1:
            stack[++st].raw = 0;
            stack[st].d = (jdouble)(op - OPC_DCONST_0);
            stack[st + 1] = stack[st];
            st++;
            f_pc++;
            break;

        case OPC_FNEG:
            stack[st].f = -stack[st].f;
            f_pc++;
            break;
        /* v36.38-BT2: d-арифметика читала/писала ТОЛЬКО верхние слоты
         * (stack[st-2], stack[st]) и оставляла нижний (канонический,
         * который читают dcmpl/d2f/invoke-маршаллинг) устаревшим.
         * Приведено к low-канонической инварианте long-семейства. */
        case OPC_DADD:
            stack[st - 3].d = stack[st - 3].d + stack[st - 1].d;
            stack[st - 2] = stack[st - 3];
            st -= 2;
            f_pc++;
            break;
        case OPC_DSUB:
            stack[st - 3].d = stack[st - 3].d - stack[st - 1].d;
            stack[st - 2] = stack[st - 3];
            st -= 2;
            f_pc++;
            break;
        case OPC_DMUL:
            stack[st - 3].d = stack[st - 3].d * stack[st - 1].d;
            stack[st - 2] = stack[st - 3];
            st -= 2;
            f_pc++;
            break;
        case OPC_DDIV:
            stack[st - 3].d = stack[st - 3].d / stack[st - 1].d;
            stack[st - 2] = stack[st - 3];
            st -= 2;
            f_pc++;
            break;
        /* v36.38-BT2: dneg писал только верхний слот — нижний (канонический)
         * оставался устаревшим. Теперь: низ = значение, верх = копия. */
        case OPC_DNEG:
            stack[st - 1].d = -stack[st - 1].d;
            stack[st] = stack[st - 1];
            f_pc++;
            break;
        case OPC_FCMPL: case OPC_FCMPG: {
            jfloat v2 = stack[st].f;
            jfloat v1 = stack[st - 1].f;
            st--;
            if (isnan(v1) || isnan(v2)) stack[st].i = (op == OPC_FCMPL) ? -1 : 1;
            else stack[st].i = (v1 > v2) ? 1 : (v1 < v2) ? -1 : 0;
            f_pc++;
            break;
        }
        case OPC_DCMPL: case OPC_DCMPG: {
            jdouble v2 = stack[st - 1].d;
            jdouble v1 = stack[st - 3].d;
            st -= 3;
            if (isnan(v1) || isnan(v2)) stack[st].i = (op == OPC_DCMPL) ? -1 : 1;
            else stack[st].i = (v1 > v2) ? 1 : (v1 < v2) ? -1 : 0;
            f_pc++;
            break;
        }

        case OPC_LDC_W: {
            /* Integer/Float are pure CP reads; anything else (String/Class)
             * stays on the cold path (interning / class loading). */
            uint16_t ci = (uint16_t)(((uint16_t)code[f_pc + 1] << 8) | (uint16_t)code[f_pc + 2]);
            JavaClass* dc = frame->clazz;
            if (__builtin_expect(dc && dc->constant_pool && ci > 0 &&
                                 ci < dc->constant_pool_count, 1)) {
                ConstantPoolEntry* e = &dc->constant_pool[ci];
                if (e->tag == CONSTANT_Integer) {
                    stack[++st].raw = 0;
                    stack[st].i = e->info.integer.value;
                    f_pc += 3;
                    break;
                }
                if (e->tag == CONSTANT_Float) {
                    stack[++st].raw = 0;
                    stack[st].f = e->info.float_val.value;
                    f_pc += 3;
                    break;
                }
            }
            goto cold_dispatch;
        }

        case OPC_LDC2_W: {
            uint16_t ci = (uint16_t)(((uint16_t)code[f_pc + 1] << 8) | (uint16_t)code[f_pc + 2]);
            JavaClass* dc = frame->clazz;
            if (__builtin_expect(dc && dc->constant_pool && ci > 0 &&
                                 ci < dc->constant_pool_count, 1)) {
                ConstantPoolEntry* e = &dc->constant_pool[ci];
                if (e->tag == CONSTANT_Long) {
                    stack[++st].raw = 0;
                    stack[st].j = e->info.long_val.value;
                    stack[st + 1] = stack[st];  /* high slot = copy */
                    st++;
                    f_pc += 3;
                    break;
                }
                if (e->tag == CONSTANT_Double) {
                    stack[++st].raw = 0;
                    stack[st].d = e->info.double_val.value;
                    stack[st + 1] = stack[st];
                    st++;
                    f_pc += 3;
                    break;
                }
            }
            goto cold_dispatch;
        }

        case OPC_GETSTATIC: {
            uint16_t fi = (uint16_t)(((uint16_t)code[f_pc + 1] << 8) | (uint16_t)code[f_pc + 2]);
            StaticFieldRefCache* sc = static_field_cache_entry(frame, fi);
            if (__builtin_expect(sc && sc->valid && sc->gen == g_vm_cache_gen &&
                                 sc->owner->static_fields &&
                                 sc->slot < sc->owner->static_fields_count, 1)) {
                JavaValue v = sc->owner->static_fields[sc->slot].value;
                stack[++st] = v;
                if (sc->wide) {
                    stack[++st] = v;
                }
                f_pc += 3;
                break;
            }
            goto cold_dispatch;
        }

        case OPC_PUTSTATIC: {
            uint16_t fi = (uint16_t)(((uint16_t)code[f_pc + 1] << 8) | (uint16_t)code[f_pc + 2]);
            StaticFieldRefCache* sc = static_field_cache_entry(frame, fi);
            if (__builtin_expect(sc && sc->valid && sc->gen == g_vm_cache_gen &&
                                 sc->owner->static_fields &&
                                 sc->slot < sc->owner->static_fields_count, 1)) {
                if (sc->wide) st--;
                sc->owner->static_fields[sc->slot].value = stack[st];
                st--;
                f_pc += 3;
                break;
            }
            goto cold_dispatch;
        }

        case OPC_GETFIELD: {
            uint16_t fi = (uint16_t)(((uint16_t)code[f_pc + 1] << 8) | (uint16_t)code[f_pc + 2]);
            FieldRefCache* fc = field_cache_entry(frame, fi);
            if (__builtin_expect(fc && fc->valid && fc->gen == g_vm_cache_gen, 1)) {
                JavaObject* obj = (JavaObject*)stack[st].ref;
                if (__builtin_expect(obj != NULL, 1)) {
                    JavaClass* oc = obj->header.clazz;
                    if (__builtin_expect(oc && (oc == fc->declared ||
                                                jvm_class_is_subclass_of(oc, fc->declared)), 1)) {
                        JavaValue v = obj->fields[fc->slot];
                        stack[st] = v;
                        if (fc->wide) {
                            stack[++st] = v;
                        }
                        f_pc += 3;
                        break;
                    }
                }
            }
            goto cold_dispatch;  /* null receiver: handler throws exactly */
        }

        case OPC_PUTFIELD: {
            uint16_t fi = (uint16_t)(((uint16_t)code[f_pc + 1] << 8) | (uint16_t)code[f_pc + 2]);
            FieldRefCache* fc = field_cache_entry(frame, fi);
            if (__builtin_expect(fc && fc->valid && fc->gen == g_vm_cache_gen, 1)) {
                int voff = fc->wide ? 2 : 1;
                JavaObject* obj = (JavaObject*)stack[st - voff].ref;
                if (__builtin_expect(obj != NULL, 1)) {
                    JavaClass* oc = obj->header.clazz;
                    if (__builtin_expect(oc && (oc == fc->declared ||
                                                jvm_class_is_subclass_of(oc, fc->declared)), 1)) {
                        JavaValue v = stack[st];
                        st -= voff + 1;  /* value (+ high slot) + receiver */
                        obj->fields[fc->slot] = v;
                        f_pc += 3;
                        break;
                    }
                }
            }
            goto cold_dispatch;  /* null receiver: handler throws exactly */
        }

        /* ---------------- conversions (op_convert twins) ---------------- */
        case OPC_I2L:
            stack[st].j = (jlong)stack[st].i;
            stack[st + 1] = stack[st];
            st++;
            f_pc++;
            break;
        case OPC_L2I:
            st--;
            stack[st].i = (jint)stack[st].j;
            f_pc++;
            break;
        case OPC_I2B:
            stack[st].i = (jint)(int8_t)stack[st].i;
            f_pc++;
            break;
        case OPC_I2C:
            stack[st].i = (jint)(uint16_t)stack[st].i;
            f_pc++;
            break;
        case OPC_I2S:
            stack[st].i = (jint)(int16_t)stack[st].i;
            f_pc++;
            break;
        case OPC_I2F:
            stack[st].f = (jfloat)stack[st].i;
            f_pc++;
            break;
        case OPC_F2I: {
            jfloat f_ = stack[st].f;
            /* JVMS saturating (J-2): NaN -> 0, clamp to int range */
            stack[st].i = isnan(f_) ? 0 :
                          (f_ >= 2147483647.0f) ? INT32_MAX :
                          (f_ <= -2147483648.0f) ? INT32_MIN : (jint)f_;
            f_pc++;
            break;
        }
        case OPC_F2L: {
            jfloat f_ = stack[st].f;
            stack[st].j = isnan(f_) ? (jlong)0 :
                          (f_ >= 9223372036854775808.0f) ? INT64_MAX :
                          (f_ <= -9223372036854775808.0f) ? INT64_MIN : (jlong)f_;
            stack[st + 1] = stack[st];
            st++;
            f_pc++;
            break;
        }
        case OPC_L2F:
            st--;
            stack[st].f = (jfloat)stack[st].j;
            f_pc++;
            break;
        /* v36.38-BT2 CONV-SLOTS: long/double занимают 2 слота (low=value,
         * high=copy — см. холодный путь opcodes.c: "pop high (фиктивный),
         * потом low с реальным значением"). Быстрые варианты l2d/d2l/d2i
         * ломали ОБЕ инварианты:
         *   1) СЧЁТ СЛОТОВ: l2d/d2l делали st++ (long<->double обязан быть
         *      footprint-нейтральным 2->2), d2i не делал st-- (2->1). Каждый
         *      вызов утекал +1 слот — дрейф operand-стека до записи за
         *      границей фрейма (ASan: heap-buffer-overflow WRITE of size 8,
         *      execute.c:1497; Bounce Tales c.a(Lf;)V, code.max_stack=9,
         *      st дошёл до 13 на 13-слотовом стеке).
         *   2) ЗНАЧЕНИЕ: чтение шло с ВЕРХНЕГО слота (stack[st]), а быстрая
         *      long-арифметика (ladd/lmul/...) пишет результат только в
         *      НИЖНИЙ. После lmul;lmul;ladd;l2d в Math.sqrt уходил
         *      устаревший слот (dx*dx вместо dx*dx+dy*dy): sqrt(0)=0 для
         *      горизонтальных сегментов полигона -> ldiv "/ by zero" на
         *      c.a PC=1526 каждый тик физики, шарик проваливался сквозь
         *      землю. Теперь: чтение с нижнего слота, запись в оба слота,
         *      счёт слотов как в спецификации JVM. */
        case OPC_L2D:
            stack[st - 1].d = (jdouble)stack[st - 1].j;  /* long low -> double low */
            stack[st] = stack[st - 1];                   /* high = копия */
            f_pc++;
            break;
        case OPC_D2L: {
            jdouble d_ = stack[st - 1].d;                /* double low (канонический) */
            stack[st - 1].j = isnan(d_) ? (jlong)0 :
                              (d_ >= 9223372036854775807.0) ? INT64_MAX :
                              (d_ <= -9223372036854775808.0) ? INT64_MIN : (jlong)d_;
            stack[st] = stack[st - 1];                   /* high = копия */
            f_pc++;
            break;
        }
        case OPC_D2I: {
            jdouble d_ = stack[st - 1].d;                /* double low (канонический) */
            st--;                                         /* 2 слота -> 1, как l2i */
            stack[st].i = isnan(d_) ? 0 :
                          (d_ >= 2147483647.0) ? INT32_MAX :
                          (d_ <= -2147483648.0) ? INT32_MIN : (jint)d_;
            f_pc++;
            break;
        }
        case OPC_D2F:
            st--;
            stack[st].f = (jfloat)stack[st].d;
            f_pc++;
            break;
        case OPC_F2D:
            stack[st].d = (jdouble)stack[st].f;
            stack[st + 1] = stack[st];
            st++;
            f_pc++;
            break;

        case OPC_LCMP: {
            jlong v2 = stack[st - 1].j;
            jlong v1 = stack[st - 3].j;
            st -= 3;
            stack[st].i = (v1 > v2) ? 1 : (v1 < v2) ? -1 : 0;
            f_pc++;
            break;
        }

        /* ---------------- long arithmetic (op_add/op_sub twins) -------- */
        /* v36.38-BT2: арифметика пишет результат в НИЖНИЙ слот пары —
         * теперь и ВЕРХНИЙ обновляется копией, иначе после ladd/lshl
         * верхний слот держал устаревшее значение, и любые читатели
         * верхнего слота (старые l2d/d2i, dcmpl после dadd) получали
         * мусор. Инварианта: оба слота пары всегда валидны. */
        case OPC_LADD:
            stack[st - 3].j = stack[st - 3].j + stack[st - 1].j;
            stack[st - 2] = stack[st - 3];
            st -= 2;
            f_pc++;
            break;
        case OPC_LSUB:
            stack[st - 3].j = stack[st - 3].j - stack[st - 1].j;
            stack[st - 2] = stack[st - 3];
            st -= 2;
            f_pc++;
            break;
        case OPC_LMUL:
            stack[st - 3].j = stack[st - 3].j * stack[st - 1].j;
            stack[st - 2] = stack[st - 3];
            st -= 2;
            f_pc++;
            break;
        case OPC_LAND:
            stack[st - 3].j = stack[st - 3].j & stack[st - 1].j;
            stack[st - 2] = stack[st - 3];
            st -= 2;
            f_pc++;
            break;
        case OPC_LOR:
            stack[st - 3].j = stack[st - 3].j | stack[st - 1].j;
            stack[st - 2] = stack[st - 3];
            st -= 2;
            f_pc++;
            break;
        case OPC_LXOR:
            stack[st - 3].j = stack[st - 3].j ^ stack[st - 1].j;
            stack[st - 2] = stack[st - 3];
            st -= 2;
            f_pc++;
            break;
        case OPC_LSHL:
            stack[st - 2].j = stack[st - 2].j << (stack[st].i & 63);
            stack[st - 1] = stack[st - 2];
            st--;
            f_pc++;
            break;
        case OPC_LSHR:
            stack[st - 2].j = stack[st - 2].j >> (stack[st].i & 63);
            stack[st - 1] = stack[st - 2];
            st--;
            f_pc++;
            break;
        case OPC_LUSHR:
            stack[st - 2].j = (jlong)((uint64_t)stack[st - 2].j >> (stack[st].i & 63));
            stack[st - 1] = stack[st - 2];
            st--;
            f_pc++;
            break;

        /* ---------------- long locals (lload/lstore groups) ------------ */
        case OPC_LLOAD: case OPC_DLOAD: {
            uint8_t li = code[f_pc + 1];
            if (__builtin_expect((uint32_t)(li + 1) >= (uint32_t)frame->max_locals, 0))
                goto cold_dispatch;
            stack[++st] = locals[li];
            stack[++st] = locals[li + 1];
            f_pc += 2;
            break;
        }
        case OPC_LLOAD_0: case OPC_LLOAD_1: case OPC_LLOAD_2: case OPC_LLOAD_3:
        case OPC_DLOAD_0: case OPC_DLOAD_1: case OPC_DLOAD_2: case OPC_DLOAD_3: {
            uint8_t li = (uint8_t)((op - OPC_LLOAD_0) & 3);
            if (__builtin_expect((uint32_t)(li + 1) >= (uint32_t)frame->max_locals, 0))
                goto cold_dispatch;
            stack[++st] = locals[li];
            stack[++st] = locals[li + 1];
            f_pc++;
            break;
        }
        case OPC_LSTORE: case OPC_DSTORE: {
            uint8_t li = code[f_pc + 1];
            if (__builtin_expect((uint32_t)(li + 1) >= (uint32_t)frame->max_locals, 0))
                goto cold_dispatch;
            locals[li + 1] = stack[st--];
            locals[li] = stack[st--];
            f_pc += 2;
            break;
        }
        case OPC_LSTORE_0: case OPC_LSTORE_1: case OPC_LSTORE_2: case OPC_LSTORE_3:
        case OPC_DSTORE_0: case OPC_DSTORE_1: case OPC_DSTORE_2: case OPC_DSTORE_3: {
            uint8_t li = (uint8_t)((op - OPC_LSTORE_0) & 3);
            if (__builtin_expect((uint32_t)(li + 1) >= (uint32_t)frame->max_locals, 0))
                goto cold_dispatch;
            locals[li + 1] = stack[st--];
            locals[li] = stack[st--];
            f_pc++;
            break;
        }
        /* ============ end v36.37 extended inline set ============ */

        /* ---------------- operand stack ---------------- */
        case OPC_POP:
            st--;
            f_pc++;
            break;
        case OPC_POP2:
            st -= 2;
            f_pc++;
            break;
        case OPC_DUP:
            stack[st + 1] = stack[st];
            st++;
            f_pc++;
            break;
        case OPC_DUP2:  /* [a,b] -> [a,b,a,b] */
            stack[st + 1] = stack[st - 1];
            stack[st + 2] = stack[st];
            st += 2;
            f_pc++;
            break;

        /* ---------------- int arithmetic ---------------- */
        case OPC_IADD:
            stack[st - 1].i = stack[st - 1].i + stack[st].i;
            st--;
            f_pc++;
            break;
        case OPC_ISUB:
            stack[st - 1].i = stack[st - 1].i - stack[st].i;
            st--;
            f_pc++;
            break;
        case OPC_IMUL:
            stack[st - 1].i = stack[st - 1].i * stack[st].i;
            st--;
            f_pc++;
            break;
        case OPC_IDIV: {
            jint v2 = stack[st].i;
            jint v1 = stack[st - 1].i;
            if (__builtin_expect(v2 == 0, 0)) goto cold_dispatch;  /* handler throws */
            if (v1 == INT32_MIN && v2 == -1) stack[st - 1].i = INT32_MIN;
            else stack[st - 1].i = v1 / v2;
            st--;
            f_pc++;
            break;
        }
        case OPC_IREM: {
            jint v2 = stack[st].i;
            jint v1 = stack[st - 1].i;
            if (__builtin_expect(v2 == 0, 0)) goto cold_dispatch;  /* handler throws */
            if (v1 == INT32_MIN && v2 == -1) stack[st - 1].i = 0;
            else stack[st - 1].i = v1 % v2;
            st--;
            f_pc++;
            break;
        }
        case OPC_INEG:
            stack[st].i = -stack[st].i;
            f_pc++;
            break;
        case OPC_ISHL:
            stack[st - 1].i = stack[st - 1].i << (stack[st].i & 0x1F);
            st--;
            f_pc++;
            break;
        case OPC_ISHR:
            stack[st - 1].i = stack[st - 1].i >> (stack[st].i & 0x1F);
            st--;
            f_pc++;
            break;
        case OPC_IUSHR:
            /* v34.77 FIX (Spore /-13.bin): C-приоритет! Старая запись
             * (jint)((juint)x) >> y кастовала x обратно в ЗНАКОВЫЙ jint
             * ещё ДО сдвига — сдвиг выходил арифметическим (sarl):
             * отрицательные операнды сохраняли знак (nextInt()>>>1 давал
             * минус -> Spore строил имя ресурса "/-13.bin" и умирал).
             * Скобки обязаны охватывать ВЕСЬ сдвиг. */
            stack[st - 1].i = (jint)(((juint)stack[st - 1].i) >> (stack[st].i & 0x1F));
            st--;
            f_pc++;
            break;
        case OPC_IAND:
            stack[st - 1].i = stack[st - 1].i & stack[st].i;
            st--;
            f_pc++;
            break;
        case OPC_IOR:
            stack[st - 1].i = stack[st - 1].i | stack[st].i;
            st--;
            f_pc++;
            break;
        case OPC_IXOR:
            stack[st - 1].i = stack[st - 1].i ^ stack[st].i;
            st--;
            f_pc++;
            break;

        /* ---------------- float arithmetic ---------------- */
        case OPC_FADD:
            stack[st - 1].f = stack[st - 1].f + stack[st].f;
            st--;
            f_pc++;
            break;
        case OPC_FSUB:
            stack[st - 1].f = stack[st - 1].f - stack[st].f;
            st--;
            f_pc++;
            break;
        case OPC_FMUL:
            stack[st - 1].f = stack[st - 1].f * stack[st].f;
            st--;
            f_pc++;
            break;
        case OPC_FDIV:
            stack[st - 1].f = stack[st - 1].f / stack[st].f;
            st--;
            f_pc++;
            break;

        /* ---------------- branches (target = pc_of_opcode + s16 offset) --- */
        case OPC_IFEQ: case OPC_IFNE: case OPC_IFLT:
        case OPC_IFGE: case OPC_IFGT: case OPC_IFLE: {
            jint v = stack[st--].i;
            int16_t off = (int16_t)(uint16_t)(((uint16_t)code[f_pc + 1] << 8) | (uint16_t)code[f_pc + 2]);  /* JVMS: big-endian s16 */
            bool branch;
            switch (op) {
                case OPC_IFEQ: branch = (v == 0); break;
                case OPC_IFNE: branch = (v != 0); break;
                case OPC_IFLT: branch = (v < 0); break;
                case OPC_IFGE: branch = (v >= 0); break;
                case OPC_IFGT: branch = (v > 0); break;
                default:       branch = (v <= 0); break;  /* OPC_IFLE */
            }
            if (branch) f_pc += (uint32_t)(int32_t)off;
            else f_pc += 3;
            break;
        }
        case OPC_IFNULL: case OPC_IFNONNULL: {
            void* r = stack[st--].ref;
            int16_t off = (int16_t)(uint16_t)(((uint16_t)code[f_pc + 1] << 8) | (uint16_t)code[f_pc + 2]);  /* JVMS: big-endian s16 */
            bool branch = (op == OPC_IFNULL) ? (r == NULL) : (r != NULL);
            if (branch) f_pc += (uint32_t)(int32_t)off;
            else f_pc += 3;
            break;
        }
        case OPC_IF_ICMPEQ: case OPC_IF_ICMPNE: case OPC_IF_ICMPLT:
        case OPC_IF_ICMPGE: case OPC_IF_ICMPGT: case OPC_IF_ICMPLE: {
            jint v2 = stack[st--].i;
            jint v1 = stack[st--].i;
            int16_t off = (int16_t)(uint16_t)(((uint16_t)code[f_pc + 1] << 8) | (uint16_t)code[f_pc + 2]);  /* JVMS: big-endian s16 */
            bool branch;
            switch (op) {
                case OPC_IF_ICMPEQ: branch = (v1 == v2); break;
                case OPC_IF_ICMPNE: branch = (v1 != v2); break;
                case OPC_IF_ICMPLT: branch = (v1 < v2); break;
                case OPC_IF_ICMPGE: branch = (v1 >= v2); break;
                case OPC_IF_ICMPGT: branch = (v1 > v2); break;
                default:            branch = (v1 <= v2); break;  /* OPC_IF_ICMPLE */
            }
            if (branch) f_pc += (uint32_t)(int32_t)off;
            else f_pc += 3;
            break;
        }
        case OPC_IF_ACMPEQ: case OPC_IF_ACMPNE: {
            void* v2 = stack[st--].ref;
            void* v1 = stack[st--].ref;
            int16_t off = (int16_t)(uint16_t)(((uint16_t)code[f_pc + 1] << 8) | (uint16_t)code[f_pc + 2]);  /* JVMS: big-endian s16 */
            bool branch = (op == OPC_IF_ACMPEQ) ? (v1 == v2) : (v1 != v2);
            if (branch) f_pc += (uint32_t)(int32_t)off;
            else f_pc += 3;
            break;
        }
        case OPC_GOTO: {
            int16_t off = (int16_t)(uint16_t)(((uint16_t)code[f_pc + 1] << 8) | (uint16_t)code[f_pc + 2]);  /* JVMS: big-endian s16 */
            f_pc += (uint32_t)(int32_t)off;
            break;
        }

        /* ---------------- array length ---------------- */
        case OPC_ARRAYLENGTH: {
            JavaArray* arr = (JavaArray*)stack[st].ref;
            if (__builtin_expect(arr == NULL, 0)) goto cold_dispatch;  /* NPE / DRM bypass */
            stack[st].i = arr->length;
            f_pc++;
            break;
        }

        /* ---------------- array loads (peek -> validate -> commit) -------- */
        case OPC_IALOAD: case OPC_FALOAD: case OPC_AALOAD: case OPC_BALOAD:
        case OPC_CALOAD: case OPC_SALOAD: {
            JavaArray* arr = (JavaArray*)stack[st - 1].ref;
            jint idx = stack[st].i;
            if (__builtin_expect(arr != NULL && (uint32_t)idx < (uint32_t)arr->length, 1)) {
                void* data = (uint8_t*)arr + sizeof(JavaArray);
                JavaValue v;
                v.raw = 0;
                switch (arr->element_type) {
                    case T_BOOLEAN: v.i = ((uint8_t*)data)[idx]; break;
                    case T_BYTE:    v.i = (jint)((int8_t*)data)[idx]; break;
                    case T_CHAR:    v.i = (jint)((jchar*)data)[idx]; break;
                    case T_SHORT:   v.i = (jint)((jshort*)data)[idx]; break;
                    case T_INT:     v.i = ((jint*)data)[idx]; break;
                    case T_FLOAT:   v.i = ((jint*)data)[idx]; break;  /* bit-identical via .i */
                    case DESC_OBJECT: case DESC_ARRAY: v.ref = ((void**)data)[idx]; break;
                    default: goto cold_dispatch;  /* T_LONG/T_DOUBLE: handler path */
                }
                stack[st - 1] = v;
                st--;
                f_pc++;
            } else {
                goto cold_dispatch;  /* null / OOB: handler throws exactly */
            }
            break;
        }

        /* ---------------- array stores (peek -> validate -> commit) ------ */
        case OPC_IASTORE: case OPC_FASTORE: case OPC_AASTORE: case OPC_BASTORE:
        case OPC_CASTORE: case OPC_SASTORE: {
            JavaArray* arr = (JavaArray*)stack[st - 2].ref;
            jint idx = stack[st - 1].i;
            if (__builtin_expect(arr != NULL && (uint32_t)idx < (uint32_t)arr->length, 1)) {
                void* data = (uint8_t*)arr + sizeof(JavaArray);
                JavaValue v = stack[st];
                switch (arr->element_type) {
                    case T_BOOLEAN: ((uint8_t*)data)[idx] = (uint8_t)v.i; break;
                    case T_BYTE:    ((int8_t*)data)[idx] = (int8_t)v.i; break;
                    case T_CHAR:    ((jchar*)data)[idx] = (jchar)v.i; break;
                    case T_SHORT:   ((jshort*)data)[idx] = (jshort)v.i; break;
                    case T_INT:     ((jint*)data)[idx] = v.i; break;
                    case T_FLOAT:   ((jfloat*)data)[idx] = v.f; break;
                    case DESC_OBJECT: case DESC_ARRAY: ((void**)data)[idx] = v.ref; break;
                    default: goto cold_dispatch;  /* T_LONG/T_DOUBLE: handler path */
                }
                st -= 3;
                f_pc++;
            } else {
                goto cold_dispatch;  /* null / OOB: handler throws exactly */
            }
            break;
        }

        default:
            goto cold_dispatch;
        }  /* end fast switch */
        continue;
#endif /* !J2ME_DEBUG */

cold_dispatch:
        {
            frame->pc = f_pc + 1;
            frame->stack_top = (int16_t)st;
            frame->throwing_pc = (int)f_pc;
            /* v34.35 FIX: flush the cached counter — cold handlers (invoke*,
             * new, getfield on a stub...) can enter NESTED interpret() calls
             * which seed their own counter from this field. */
            jvm->instr_count = icount;

            OpcodeHandler handler = opcode_table[op].handler;
            if (__builtin_expect(handler == NULL, 0)) {
                ERROR_LOG("Unknown opcode: 0x%02X at PC=%d", op, (int)f_pc);
                jvm_throw_by_name(jvm, "java/lang/VirtualMachineError", "Unknown opcode");
                jvm->instr_count = icount;
                return -1;
            }

            int result = handler(jvm, thread, frame);

            f_pc = frame->pc;
            st = frame->stack_top;
            /* v34.35 FIX: re-seed the cached counter with the global total
             * (includes any instructions the nested interpret executed). */
            icount = jvm->instr_count;

            if (result > 0) {
                jvm->instr_count = icount;
                EXEC_DEBUG("Method returned");
                return 0;  /* Normal return */
            }

            if (result < 0) {
                jvm->instr_count = icount;
                if (!thread->pending_exception) {
                    LOG_SAFE("[JVM] OPCODE FAIL: opcode 0x%02X (%s) at PC=%d in %s.%s returned %d WITHOUT exception!\n",
                            op,
                            opcode_table[op].name ? opcode_table[op].name : "?",
                            (int)f_pc - 1,
                            frame->clazz ? (frame->clazz->class_name ? frame->clazz->class_name : "?") : "?",
                            frame->method ? (frame->method->name ? frame->method->name : "?") : "?",
                            result);
                    return -1;
                }
exception_search:
                /* v36.57 [SPIN-BREAK]: точка входа диспетчеризации исключения
                 * брейкера — ТОТ ЖЕ штатный поиск catch-обработчиков текущего
                 * кадра (throwing_pc уже установлен вызывающим goto). */
                {
                    JavaObject* exception = thread->pending_exception;
                    JavaClass* exception_class = exception->header.clazz;

                    if (g_j2me_runtime_debug) LOG_SAFE("[EXCEPTION] Exception %s occurred at PC=%d in %s.%s\n",
                               exception_class ? exception_class->class_name : "??",
                               frame->throwing_pc,
                               frame->clazz ? (frame->clazz->class_name ? frame->clazz->class_name : "?") : "?",
                               frame->method ? (frame->method->name ? frame->method->name : "?") : "?");

                    bool handler_found = false;
                    if (frame->exception_table) {
                        for (uint16_t i = 0; i < frame->exception_table_length; i++) {
                            ExceptionTableEntry* handler = &frame->exception_table[i];

                            if (frame->throwing_pc >= handler->start_pc &&
                                frame->throwing_pc < handler->end_pc) {

                                if (handler->catch_type == 0) {
                                    handler_found = true;
                                } else {
                                    const char* catch_name = classfile_get_class_name(frame->clazz, handler->catch_type);
                                    if (catch_name) {
                                        JavaClass* catch_class = jvm_load_class(jvm, catch_name);
                                        if (catch_class && object_instance_of(exception, catch_class)) {
                                            handler_found = true;
                                        } else {
                                            static int ex_miss_log = 0;
                                            if (ex_miss_log < 40) {
                                                ex_miss_log++;
                                                LOG_SAFE("[EX-MISS] %s not caught by %s in %s.%s "
                                                         "(range %d..%d handler_pc=%d): catch_class=%p "
                                                         "instanceof=%s\n",
                                                         exception_class ? exception_class->class_name : "??",
                                                         catch_name,
                                                         frame->clazz ? frame->clazz->class_name : "?",
                                                         frame->method ? frame->method->name : "?",
                                                         handler->start_pc, handler->end_pc,
                                                         handler->handler_pc,
                                                         (void*)catch_class,
                                                         (catch_class && object_instance_of(exception, catch_class)) ? "true" : "false");
                                            }
                                        }
                                    }
                                }

                                if (handler_found) {
                                    frame->stack_top = 0;
                                    JavaValue v = { .ref = exception };
                                    frame->stack[0] = v;
                                    thread->pending_exception = NULL;

                                    if (thread->exception_stack_trace) {
                                        free(thread->exception_stack_trace);
                                        thread->exception_stack_trace = NULL;
                                    }
                                    if (thread->exception_throw_info) {
                                        free(thread->exception_throw_info);
                                        thread->exception_throw_info = NULL;
                                    }

                                    frame->pc = handler->handler_pc;
                                    goto continue_execution;
                                }
                            }
                        }
                    }

                    if (!handler_found) {
                        static int uncaught_prop_log_count = 0;
                        if (uncaught_prop_log_count < 200) {
                            uncaught_prop_log_count++;
                            char exmsg[96] = "";
                            {
                                extern JavaValue native_get_field_value(JavaObject* obj, const char* field_name);
                                JavaString* m = (JavaString*)native_get_field_value(exception, "detailMessage").ref;
                                if (m) {
                                    extern const char* string_utf8(JVM* jvm, JavaString* str);
                                    const char* u = string_utf8(jvm, m);
                                    if (u) snprintf(exmsg, sizeof(exmsg), " \"%s\"", u);
                                }
                            }
                            LOG_SAFE("[EX-PROP] %s%s: %s.%s PC=%d -> pop frame\n",
                               exception_class ? exception_class->class_name : "??",
                               exmsg,
                               frame->clazz ? (frame->clazz->class_name ? frame->clazz->class_name : "?") : "?",
                               frame->method ? (frame->method->name ? frame->method->name : "?") : "?",
                               frame->throwing_pc);
                        }

                        if (thread->exception_stack_trace == NULL) {
                            char trace_buf[4096];
                            int pos = 0;
                            int frame_count = 0;
                            JavaFrame* f = frame;
                            while (f && frame_count < 20 && pos < (int)sizeof(trace_buf) - 2) {
                                const char* cls = f->clazz && f->clazz->class_name ? f->clazz->class_name : "?";
                                const char* meth = f->method && f->method->name ? f->method->name : "?";
                                const char* desc = f->method && f->method->descriptor ? f->method->descriptor : "";
                                const char* slash = strrchr(cls, '/');
                                const char* short_cls = slash ? slash + 1 : cls;

                                int written;
                                if (frame_count == 0) {
                                    written = snprintf(trace_buf + pos, sizeof(trace_buf) - pos - 1,
                                            ">%s.%s%s at PC=%d\n",
                                            short_cls, meth, desc, f->throwing_pc);
                                } else {
                                    written = snprintf(trace_buf + pos, sizeof(trace_buf) - pos - 1,
                                            "  at %s.%s%s\n",
                                            short_cls, meth, desc);
                                }
                                if (written > 0 && pos + written < (int)sizeof(trace_buf) - 1) {
                                    pos += written;
                                } else {
                                    break;
                                }
                                f = f->prev;
                                frame_count++;
                            }
                            trace_buf[pos] = '\0';

                            thread->exception_stack_trace = strdup(trace_buf);

                            const char* throw_cls = frame->clazz && frame->clazz->class_name ?
                                    frame->clazz->class_name : "?";
                            const char* throw_meth = frame->method && frame->method->name ?
                                    frame->method->name : "?";
                            const char* slash2 = strrchr(throw_cls, '/');
                            throw_cls = slash2 ? slash2 + 1 : throw_cls;
                            char info[256];
                            snprintf(info, sizeof(info), "%s.%s at PC=%d", throw_cls, throw_meth, frame->throwing_pc);
                            thread->exception_throw_info = strdup(info);
                        }

                        return -1;  /* Caller will handle */
                    }
                }
            }
        }
    }  /* end while */

    frame->pc = f_pc;
    frame->stack_top = (int16_t)st;
    jvm->instr_count = icount;
    EXEC_DEBUG("interpret: Reached end of code");
    return 0;  /* Reached end of code */
}

/* Invoke virtual method */
int jvm_invoke_virtual(JVM* jvm, JavaObject* obj, const char* name, 
                       const char* descriptor, JavaValue* args, JavaValue* result) {
    if (!obj || !name || !descriptor) {
        ERROR_LOG("jvm_invoke_virtual: NULL parameter");
        return -1;
    }

    JavaClass* clazz = obj->header.clazz;
    if (!clazz) {
        ERROR_LOG("jvm_invoke_virtual: Object has no class");
        return -1;
    }

    EXEC_DEBUG("jvm_invoke_virtual: %s.%s%s", clazz->class_name, name, descriptor);

    /* Find method in class hierarchy */
    JavaMethod* method = jvm_resolve_method(jvm, clazz, name, descriptor);
    if (!method) {
        /* v34.47: interpreter parity — native registry fallback.
         * op_invokevirtual resolves bytecode methods first and then walks
         * the class chain through native_find(). jvm_invoke_virtual lacked
         * the second half, so virtual calls from C code into native-only
         * classes (java/io/ByteArrayOutputStream etc. — stub classes with
         * no bytecode methods) always failed with rc=-1. This broke the
         * v34.47 DataOutputStream fix: out.write(...) from a native could
         * not reach the BAOS write natives. */
        extern NativeMethod native_find(JVM* jvm, const char* class_name,
                                         const char* method_name,
                                         const char* descriptor);
        JavaClass* search_class = clazz;
        JavaClass* found_class = NULL;
        while (search_class) {
            if (native_find(jvm, search_class->class_name, name, descriptor)) {
                found_class = search_class;
                break;
            }
            search_class = search_class->super_class;
        }
        if (found_class) {
            /* Build a shell JavaMethod so native_call() finds the handler
             * via its own native_find(); the shell is read-only input. */
            extern int count_args(const char* descriptor);
            int narg = count_args(descriptor);
            JavaMethod shell;
            memset(&shell, 0, sizeof(shell));
            shell.name = (char*)name;
            shell.descriptor = (char*)descriptor;
            shell.clazz = found_class;

            JavaValue* full_args = (JavaValue*)malloc((narg + 1) * sizeof(JavaValue));
            if (!full_args) return -1;
            full_args[0].ref = obj;
            for (int i = 0; i < narg; i++) full_args[i + 1] = args[i];

            JavaThread* nthread = jvm_current_thread(jvm);
            JavaValue nresult;
            memset(&nresult, 0, sizeof(nresult));
            /* native_call clears stale pending exceptions itself (same as
             * the is_native dispatch path in execute_method). */
            int ret = native_call(jvm, nthread, &shell, full_args, &nresult);
            free(full_args);
            return ret;
        }
        ERROR_LOG("Method not found: %s%s in %s", name, descriptor, clazz->class_name);
        return -1;
    }

    /* Prepare args with 'this' reference (use cached arg count) */
    int arg_count = method_arg_count(method);
    JavaValue* full_args = (JavaValue*)malloc((arg_count + 1) * sizeof(JavaValue));
    if (!full_args) return -1;

    full_args[0].ref = obj;
    if (args) {
        memcpy(full_args + 1, args, arg_count * sizeof(JavaValue));
    }

    /* FIX: snapshot the CURRENT frame's stack depth. If the callee completes
     * with a normal Xreturn it deposits its result ONTO this stack (op_ireturn
     * pushes into frame->prev). We harvest the deposited slots for the C API
     * caller and restore the previous depth, keeping operand-stack state
     * consistent (previously: stale-value read at stack_top-1 AND permanent
     * phantom operand left on the interpreter frame). */
    JavaThread* pre_thread = jvm_current_thread(jvm);
    JavaFrame* pre_frame = pre_thread ? pre_thread->current_frame : NULL;
    int saved_stack_top = 0;
    bool have_snapshot = false;
    if (pre_frame) {
        saved_stack_top = pre_frame->stack_top;
        have_snapshot = true;
    }

    JavaThread* thread = jvm_current_thread(jvm);
    int ret = execute_method(jvm, thread, method, full_args, result);

    {
        const char* rdesc = method->descriptor ? strchr(method->descriptor, ')') : NULL;
        bool returns_value = rdesc && rdesc[1] != 'V' && rdesc[1] != '\0';
        JavaThread* post_thread = jvm_current_thread(jvm);
        JavaFrame* post_frame = post_thread ? post_thread->current_frame : NULL;

        if (ret == 0 && returns_value && have_snapshot &&
            post_frame == pre_frame && pre_frame->stack_top > saved_stack_top) {
            /* Low word of long/double lives in the first deposited slot */
            *result = pre_frame->stack[saved_stack_top + 1];
            /* Restore caller's operand-stack depth */
            pre_frame->stack_top = saved_stack_top;
        }
    }

    free(full_args);
    return ret;
}

/* Invoke static method */
int jvm_invoke_static(JVM* jvm, JavaClass* clazz, const char* name,
                      const char* descriptor, JavaValue* args, JavaValue* result) {
    if (!clazz || !name || !descriptor) {
        ERROR_LOG("jvm_invoke_static: NULL parameter");
        return -1;
    }

    EXEC_DEBUG("jvm_invoke_static: %s.%s%s", clazz->class_name, name, descriptor);

    /* Find method */
    JavaMethod* method = jvm_resolve_method(jvm, clazz, name, descriptor);
    if (!method) {
        ERROR_LOG("Static method not found: %s%s in %s", name, descriptor, clazz->class_name);
        return -1;
    }

    JavaThread* thread = jvm_current_thread(jvm);
    int ret = execute_method(jvm, thread, method, args, result);

    /* Same deposit-harvest contract as jvm_invoke_virtual (see comment there) */
    {
        const char* rdesc = method->descriptor ? strchr(method->descriptor, ')') : NULL;
        bool returns_value = rdesc && rdesc[1] != 'V' && rdesc[1] != '\0';
        JavaThread* post_thread = jvm_current_thread(jvm);
        JavaFrame* post_frame = post_thread ? post_thread->current_frame : NULL;

        if (ret == 0 && returns_value && post_frame &&
            post_frame->stack_top >= 0) {
            *result = post_frame->stack[post_frame->stack_top];
            post_frame->stack_top--;
        }
    }

    return ret;
}

/* Invoke special method (constructor, private, super) */
int jvm_invoke_special(JVM* jvm, JavaObject* obj, JavaClass* clazz,
                       const char* name, const char* descriptor,
                       JavaValue* args, JavaValue* result) {
    if (!clazz || !name || !descriptor) {
        ERROR_LOG("jvm_invoke_special: NULL parameter");
        return -1;
    }

    EXEC_DEBUG("jvm_invoke_special: %s.%s%s", clazz->class_name, name, descriptor);

    /* Find method */
    JavaMethod* method = jvm_resolve_method(jvm, clazz, name, descriptor);
    if (!method) {
        ERROR_LOG("Special method not found: %s%s in %s", name, descriptor, clazz->class_name);
        return -1;
    }

    /* Prepare args with 'this' reference (use cached arg count) */
    int arg_count = method_arg_count(method);
    JavaValue* full_args = (JavaValue*)malloc((arg_count + 1) * sizeof(JavaValue));
    if (!full_args) return -1;

    full_args[0].ref = obj;
    if (args) {
        memcpy(full_args + 1, args, arg_count * sizeof(JavaValue));
    }

    JavaThread* thread = jvm_current_thread(jvm);
    int ret = execute_method(jvm, thread, method, full_args, result);

    /* Same deposit-harvest contract as jvm_invoke_virtual (see comment there) */
    {
        const char* rdesc = method->descriptor ? strchr(method->descriptor, ')') : NULL;
        bool returns_value = rdesc && rdesc[1] != 'V' && rdesc[1] != '\0';
        JavaThread* post_thread = jvm_current_thread(jvm);
        JavaFrame* post_frame = post_thread ? post_thread->current_frame : NULL;

        if (ret == 0 && returns_value && post_frame &&
            post_frame->stack_top >= 0) {
            *result = post_frame->stack[post_frame->stack_top];
            post_frame->stack_top--;
        }
    }

    free(full_args);
    return ret;
}

/* Create new object and call constructor */
JavaObject* jvm_new_object_with_constructor(JVM* jvm, JavaClass* clazz,
                                            const char* descriptor,
                                            JavaValue* args) {
    if (!clazz) {
        ERROR_LOG("jvm_new_object_with_constructor: NULL class");
        return NULL;
    }

    EXEC_DEBUG("Creating object of class %s", clazz->class_name);

    /* Create object */
    JavaObject* obj = jvm_new_object(jvm, clazz);
    if (!obj) {
        ERROR_LOG("Failed to allocate object");
        return NULL;
    }

    /* Find constructor if descriptor provided */
    if (descriptor) {
        JavaMethod* init = jvm_resolve_method(jvm, clazz, "<init>", descriptor);
        if (init) {
            int arg_count = method_arg_count(init);
            JavaValue* full_args = (JavaValue*)malloc((arg_count + 1) * sizeof(JavaValue));
            if (full_args) {
                full_args[0].ref = obj;
                if (args) {
                    memcpy(full_args + 1, args, arg_count * sizeof(JavaValue));
                }

                JavaThread* thread = jvm_current_thread(jvm);
                JavaValue result;
                execute_method(jvm, thread, init, full_args, &result);

                free(full_args);
            }
        }
    }

    return obj;
}

/* Initialize a class and its superclass */
int jvm_init_class(JVM* jvm, JavaClass* clazz) {
    if (!clazz) return -1;

    /* 1. Если класс уже инициализирован */
    if (clazz->initialized) {
        return 0;
    }

    /* 2. Получаем текущий поток */
    JavaThread* current = jvm_current_thread(jvm);
    if (!current) return -1; /* Safety check */

    /* 3. Проверка на рекурсию (JVMS §5.5)
       Если класс уже инициализируется ЭТИМ потоком, разрешаем выполнение (return 0).
       Это происходит, когда <clinit> вызывает методы своего же класса. */
    if (clazz->initializing && clazz->initializing_thread == current) {
        return 0;
    }
    
    /* 4. Если класс инициализируется другим потоком (в однопоточном варианте не должно случаться) */
    if (clazz->initializing) {
        return 0; 
    }

    EXEC_DEBUG("Initializing class %s", clazz->class_name);
    EXEC_DEBUG("Class %s (is_stub=%d, instance_size=%zu)",
            clazz->class_name ? clazz->class_name : "?", clazz->is_stub ? 1 : 0, clazz->instance_size);

    /* 5. Инициализируем суперкласс */
    if (clazz->super_class && !clazz->super_class->initialized) {
        if (jvm_init_class(jvm, clazz->super_class) != 0) {
            return -1;
        }
    }
    
    /* === CRITICAL FIX: Recalculate instance_size after superclass is initialized ===
     * At class load time, superclass might not have been loaded yet.
     * Now that superclass is guaranteed to be loaded and initialized,
     * we can correctly calculate the total instance size.
     */
    jvm_recalculate_instance_size(jvm, clazz);

    /* === CRITICAL: Initialize static fields from class file ===
     * Static fields are parsed from the class file into clazz->fields[],
     * but need to be copied to clazz->static_fields[] for getstatic/putstatic.
     * This must be done BEFORE <clinit> runs.
     * 
     * IMPORTANT: Fields with same name but different descriptors are DIFFERENT fields!
     * This is valid in Java (obfuscated code often uses this).
     */
    if (clazz->fields && clazz->fields_count > 0) {
        /* Count static fields first */
        int static_count = 0;
        for (uint16_t i = 0; i < clazz->fields_count; i++) {
            if (clazz->fields[i].access_flags & ACC_STATIC) {
                static_count++;
            }
        }
        
        /* Allocate static fields storage if needed */
        if (static_count > 0) {
            if (!clazz->static_fields) {
                int capacity = static_count > 16 ? static_count : 16;
                clazz->static_fields = (JavaStaticField*)calloc(capacity, sizeof(JavaStaticField));
                clazz->static_fields_capacity = capacity;
                clazz->static_fields_count = 0;
            }
            
            /* Add each static field */
            for (uint16_t i = 0; i < clazz->fields_count; i++) {
                JavaField* field = &clazz->fields[i];
                if (!(field->access_flags & ACC_STATIC)) continue;
                
                /* Check if already exists */
                bool exists = false;
                for (int j = 0; j < clazz->static_fields_count; j++) {
                    if (clazz->static_fields[j].name && 
                        strcmp(clazz->static_fields[j].name, field->name) == 0 &&
                        clazz->static_fields[j].descriptor && field->descriptor &&
                        strcmp(clazz->static_fields[j].descriptor, field->descriptor) == 0) {
                        exists = true;
                        break;
                    }
                }
                
                if (!exists) {
                    /* Grow if needed */
                    if (clazz->static_fields_count >= clazz->static_fields_capacity) {
                        int new_cap = clazz->static_fields_capacity + 16;
                        JavaStaticField* new_fields = (JavaStaticField*)realloc(
                            clazz->static_fields, new_cap * sizeof(JavaStaticField));
                        if (!new_fields) continue;
                        memset(new_fields + clazz->static_fields_capacity, 0, 
                               16 * sizeof(JavaStaticField));
                        clazz->static_fields = new_fields;
                        clazz->static_fields_capacity = new_cap;
                    }
                    
                    int slot = clazz->static_fields_count++;
                    clazz->static_fields[slot].name = field->name ? strdup(field->name) : NULL;
                    clazz->static_fields[slot].descriptor = field->descriptor ? strdup(field->descriptor) : NULL;
                    clazz->static_fields[slot].value.raw = 0;  /* Default value */

                    /* === v20 FIX (P0-4): honor the ConstantValue attribute ===
                     * `static final` fields carry their compile-time constant in
                     * the ConstantValue attribute (JVMS §4.7.16). Without this,
                     * Math.PI, Integer.MAX_VALUE, Double.NaN, Boolean.TRUE, ...
                     * all read as 0/null through getstatic, because unlike real
                     * classes the platform stubs have no <clinit> to set them.
                     * The attribute is a 2-byte CP index resolving to
                     * Integer/Float/Long/Double/String. */
                    if (field->attributes) {
                        for (uint16_t a = 0; a < field->attributes_count; a++) {
                            AttributeInfo* attr = &field->attributes[a];
                            if (!attr->info || attr->length < 2) continue;
                            const char* aname = classfile_get_utf8(clazz, attr->name_index);
                            if (!aname || strcmp(aname, "ConstantValue") != 0) continue;

                            uint16_t cp_index = ((uint16_t)attr->info[0] << 8) | attr->info[1];
                            if (cp_index < 1 || cp_index >= clazz->constant_pool_count) break;
                            ConstantPoolEntry* cp = &clazz->constant_pool[cp_index];
                            switch (cp->tag) {
                                case CONSTANT_Integer:
                                    clazz->static_fields[slot].value.i = cp->info.integer.value;
                                    break;
                                case CONSTANT_Float:
                                    clazz->static_fields[slot].value.f = cp->info.float_val.value;
                                    break;
                                case CONSTANT_Long:
                                    clazz->static_fields[slot].value.j = cp->info.long_val.value;
                                    break;
                                case CONSTANT_Double:
                                    clazz->static_fields[slot].value.d = cp->info.double_val.value;
                                    break;
                                case CONSTANT_String: {
                                    uint16_t utf8_idx = cp->info.string_info.string_index;
                                    const char* chars = (utf8_idx < clazz->constant_pool_count &&
                                                         clazz->constant_pool[utf8_idx].tag == CONSTANT_Utf8)
                                                        ? clazz->constant_pool[utf8_idx].info.utf8.bytes : NULL;
                                    if (chars) {
                                        JavaString* s = jvm_new_string(jvm, chars);
                                        clazz->static_fields[slot].value.ref = (JavaObject*)s;
                                    }
                                    break;
                                }
                                default:
                                    break;
                            }
                            EXEC_DEBUG("ConstantValue applied: %s.%s = raw=%lld",
                                    clazz->class_name ? clazz->class_name : "?",
                                    field->name ? field->name : "?",
                                    (long long)clazz->static_fields[slot].value.j);
                            break;
                        }
                    }

                    
                    EXEC_DEBUG("Static field slot %d: %s %s", slot, 
                            field->name ? field->name : "?",
                            field->descriptor ? field->descriptor : "?");
                }
            }
            
            EXEC_DEBUG("Initialized %d static fields for %s", clazz->static_fields_count, 
                    clazz->class_name ? clazz->class_name : "?");
        }
    }

    /* 6. Устанавливаем флаг ПЕРЕД выполнением <clinit> */
    clazz->initializing = true;
    clazz->initializing_thread = current;

    /* 7. Запускаем <clinit> */
    JavaMethod* clinit = jvm_resolve_method(jvm, clazz, "<clinit>", "()V");
    if (clinit) {
        JavaValue result;
        /* execute_method вернет < 0 при исключении */
        int ret = execute_method(jvm, current, clinit, NULL, &result);
        
        if (ret != 0) {
            /* Error during initialization - mark class as initialized to prevent
             * infinite retry loops. Without this, every subsequent op_new/op_getstatic/
             * op_invokestatic would re-trigger <clinit>, consuming recursion depth
             * until the 500 limit is hit. Per JVMS §5.5, a failed initialization makes
             * the class erroneous; subsequent access should throw an error immediately
             * rather than re-running <clinit>. */
            clazz->initializing = false;
            clazz->initializing_thread = NULL;
            clazz->initialized = true;  /* Prevent retries - class is marked erroneous */
            ERROR_LOG("ExceptionInInitializerError for %s (marked as initialized to prevent retries)", clazz->class_name);
            return -1;
        }
    }

    /* 8. Завершаем инициализацию */
    clazz->initializing = false;
    clazz->initializing_thread = NULL;
    clazz->initialized = true;

    /* 9. SPECIAL CASE: Initialize AlertType static fields with instances */
    if (clazz->class_name && strcmp(clazz->class_name, "javax/microedition/lcdui/AlertType") == 0) {
        /* Create AlertType instances for each static field */
        if (clazz->static_fields && clazz->static_fields_count >= 5) {
            for (int i = 0; i < 5; i++) {
                /* Create a new AlertType object for each static field */
                JavaObject* alert_type_obj = jvm_new_object(jvm, clazz);
                if (alert_type_obj) {
                    clazz->static_fields[i].value.ref = alert_type_obj;
                    EXEC_DEBUG("Initialized AlertType.%s = %p", 
                              clazz->static_fields[i].name, (void*)alert_type_obj);
                }
            }
        }
    }

    return 0;
}

/* Main entry point - execute a MIDlet */
int jvm_run_midlet(JVM* jvm, JavaClass* main_class) {
    if (!jvm || !main_class) {
        ERROR_LOG("jvm_run_midlet: NULL parameter");
        return -1;
    }

    /* v36.32 [SESSION-FLAG-RESET]: the MIDlet destroyed/paused flags are
     * PER-SESSION state. midlet_reset_state() used to be dead code, so a
     * notifyDestroyed in ANY earlier session (or — with the v36.32
     * natural-death marker — even a failed launch with no VM threads)
     * raised the flag PROCESS-GLOBALLY and every later session skipped its
     * exit-time destroyApp (silent save loss per relaunch, the badlaunch
     * regression signal). Fresh session -> fresh AMS state. */
    {
        extern void midlet_reset_state(void);
        midlet_reset_state();
    }

    LOG_SAFE("[MIDLET] ========== STARTING MIDLET: %s ==========\n", 
            main_class->class_name ? main_class->class_name : "?");

    EXEC_DEBUG("Running MIDlet: %s", main_class->class_name);

    /* Initialize the class */
    LOG_SAFE("[MIDLET] Initializing class...\n");
    
    if (jvm_init_class(jvm, main_class) != 0) {
        ERROR_LOG("Failed to initialize class %s", main_class->class_name);
        return -1;
    }
    
    LOG_SAFE("[MIDLET] Class initialized OK\n");

    /* Find constructor */
    LOG_SAFE("[MIDLET] Finding constructor...\n");
    JavaMethod* constructor = jvm_resolve_method(jvm, main_class, "<init>", "()V");
    if (!constructor) {
        ERROR_LOG("No default constructor in %s", main_class->class_name);
        return -1;
    }
    
    LOG_SAFE("[MIDLET] Found constructor: %s%s (native=%d)\n", 
            constructor->name ? constructor->name : "?",
            constructor->descriptor ? constructor->descriptor : "?",
            constructor->is_native ? 1 : 0);

    /* Create MIDlet instance */
    LOG_SAFE("[MIDLET] Creating MIDlet instance...\n");
    JavaObject* midlet = jvm_new_object(jvm, main_class);
    if (!midlet) {
        ERROR_LOG("Failed to create MIDlet instance");
        return -1;
    }
    
    LOG_SAFE("[MIDLET] Created MIDlet instance: %p\n", (void*)midlet);

    /* FIX (audit T-1, v18): publish the MIDlet instance so external exit paths
     * (notifyDestroyed, SDL_QUIT, frontend shutdown) can deliver destroyApp()
     * before the VM stops. */
    {
        extern JavaObject* g_midlet_instance;
        g_midlet_instance = midlet;
    }

    /* Call constructor */
    JavaValue this_arg = { .ref = midlet };
    JavaThread* thread = jvm_current_thread(jvm);
    JavaValue result;
    
    LOG_SAFE("[MIDLET] Calling constructor...\n");
    
    if (execute_method(jvm, thread, constructor, &this_arg, &result) != 0) {
        ERROR_LOG("Constructor failed");
        if (thread->pending_exception) {
            JavaClass* exc_class = thread->pending_exception->header.clazz;
            LOG_SAFE("[MIDLET] Exception in constructor: %s\n",
                    exc_class ? exc_class->class_name : "?");
        }
        return -1;
    }
    
    LOG_SAFE("[MIDLET] Constructor completed OK\n");

    EXEC_DEBUG("MIDlet instance created successfully");

    /* Find startApp method */
    LOG_SAFE("[MIDLET] Finding startApp()...\n");
    JavaMethod* startApp = jvm_resolve_method(jvm, main_class, "startApp", "()V");
    if (!startApp) {
        ERROR_LOG("No startApp method in %s", main_class->class_name);
        /* This is not fatal - MIDlet might be abstract */
        return 0;
    }
    
    LOG_SAFE("[MIDLET] Found startApp: %s%s (native=%d, code_len=%u)\n",
            startApp->name ? startApp->name : "?",
            startApp->descriptor ? startApp->descriptor : "?",
            startApp->is_native ? 1 : 0,
            startApp->code.code_length);

    LOG_SAFE("[MIDLET] Calling startApp()...\n");
    
    if (execute_method(jvm, thread, startApp, &this_arg, &result) != 0) {
        ERROR_LOG("startApp failed");
        if (thread->pending_exception) {
            JavaClass* exc_class = thread->pending_exception->header.clazz;
            LOG_SAFE("[MIDLET] Exception in startApp: %s\n",
                    exc_class ? exc_class->class_name : "?");
        }
        /* Print saved exception info and stack trace for debugging */
        if (thread->exception_throw_info) {
            LOG_SAFE("[MIDLET] Exception thrown at: %s\n", thread->exception_throw_info);
        }
        if (thread->exception_stack_trace) {
            LOG_SAFE("[MIDLET] Stack trace at exception:\n%s", thread->exception_stack_trace);
        }
        
        /* COMPATIBILITY: Don't fail if startApp throws an exception.
         * Many J2ME games have DRM/copy protection (e.g., GlomoReg) that throws
         * exceptions when running on non-registered devices. In an emulator,
         * we should log the error but continue running the game.
         * The game's own initialization code after the DRM check will still execute
         * if the exception is properly caught/ignored at the JVM level. */
        LOG_SAFE("[MIDLET] startApp() threw exception - clearing and continuing (DRM bypass)\n");
        if (thread->pending_exception) {
            jvm_exception_clear(jvm);
        }
        thread->pending_exception = NULL;
        return 0;
    }
    
    LOG_SAFE("[MIDLET] startApp() completed OK\n");

    EXEC_DEBUG("MIDlet started successfully");
    LOG_SAFE("[MIDLET] ========== MIDLET STARTED ==========\n");
    return 0;
}

/* Execute one instruction from the current frame (for libretro step-by-step execution)
 * Returns: 0 = continue, >0 = method returned, <0 = error/exception
 * This is designed for step-by-step execution in libretro where we need to
 * periodically return control to the frontend.
 */
int execute_frame(JVM* jvm, JavaThread* thread) {
    if (!jvm || !thread) return -1;
    
    JavaFrame* frame = thread->current_frame;
    if (!frame || !frame->code) {
        return -1;  /* No frame to execute */
    }
    
    /* Check for pending exception */
    if (thread->pending_exception) {
        return -1;
    }
    
    /* Check if we've reached end of code */
    if (frame->pc >= frame->code_length) {
        return 1;  /* End of method */
    }
    
    /* Check if JVM is still running */
    if (!jvm->running) {
        return -1;
    }
    
    /* Save the PC for exception handling */
    frame->throwing_pc = frame->pc;
    
    /* Fetch and execute one instruction */
    uint8_t opcode = frame->code[frame->pc++];
    
    /* Opcode tracing - uses runtime debug flag (toggle with F12) */
    if (g_j2me_runtime_debug) {
        const char* op_name = opcode_table[opcode].name;
        const char* method_name = frame->method ? frame->method->name : "?";
        const char* class_name = (frame->clazz && frame->clazz->class_name) ? frame->clazz->class_name : "?";
        if (g_j2me_runtime_debug) LOG_SAFE("[OPCODE] %s.%s PC=%d: %s (0x%02X) [stack=%d/%d, locals=%d]\n",
                class_name, method_name,
                frame->pc - 1, 
                op_name ? op_name : "unknown", 
                opcode,
                frame->stack_top + 1,
                frame->max_stack,
                frame->max_locals);
    }
    
    /* Get opcode handler */
    OpcodeHandler handler = opcode_table[opcode].handler;
    if (!handler) {
        ERROR_LOG("Unknown opcode: 0x%02X at PC=%d", opcode, frame->pc - 1);
        return -1;
    }
    
    /* Execute opcode */
    int result = handler(jvm, thread, frame);
    
    /* result: 0 = continue, >0 = return, <0 = error/exception */
    return result;
}
