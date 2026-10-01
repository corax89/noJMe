/*
 * J2ME Emulator - JVM Core Implementation
 */

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L  /* For strdup - only on POSIX systems */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h> /* v36.05: clock_gettime in the [EX-THROW] throttle */
#include "miniz.h"

#include "jvm.h"
#include "classfile.h"
#include "heap.h"
#include "threads.h"
#include "opcodes.h"
#include "native.h"
#include "debug.h"
#include "debug_macros.h"
#include "jar_reader.h"
/* v34.10: the classload mutex below (g_classload_mutex) compiles on
 * Windows too — include the project's Win32 pthread shim (same pattern
 * as native.c / media.c / sdl_backend_stubs.c). It provides
 * pthread_mutex_t + PTHREAD_MUTEX_INITIALIZER (lazy-init recursive
 * CRITICAL_SECTION) + pthread_mutexattr_*. */
#ifdef _WIN32
#include "win_thread_shim.h"
#else
#include <pthread.h>
#endif

/* v19: pre-allocated OutOfMemoryError instance (created in jvm_init while the
 * heap is empty, rooted in GC). Last-resort throwable when even the emergency
 * reserve is exhausted — guarantees OutOfMemoryError stays catchable. */
static JavaObject* g_emergency_oom = NULL;

/* v36.07 STRAND COUNTER: process-lifetime count of jvm_destroy teardowns
 * that found a busy runner and left the whole VM arena to the OS. Surfaced
 * by the [MEM] line at every session end (main.c) so the memory curve that
 * leads to "relaunch does not load / crash" is visible in one number. */
static volatile int g_vm_strand_count = 0;
int jvm_strand_count(void) { return g_vm_strand_count; }

/* v19: JAR reading moved to the single canonical miniz-based reader
 * (src/utils/jar_reader.c). The old hand-rolled central-directory scan was
 * functionally duplicated in FOUR files (jvm.c, main.c, libretro.c,
 * sdl_backend_stubs.c); divergence between them made resource loading fail
 * on some builds while class loading still worked. miniz also handles
 * data descriptors / ZIP64 / SFX-prefixed archives that the old code did not. */
static uint8_t* jar_find_file_internal(const uint8_t* jar_data, size_t jar_size, 
                                       const char* filename, size_t* out_size) {
    return jar_read_file(jar_data, jar_size, filename, out_size);
}

/* v36.24 [CORRUPT-CLASS-NO-STUB]: set by jvm_load_class_from_jar() when a
 * class file EXISTS in the JAR but classfile_parse() rejected it. The stub
 * fallback in jvm_load_class_impl is designed for ABSENT classes (optional
 * API probes); silently stubbing a PRESENT-but-corrupt class used to mask
 * real damage — the midlet "started" on a hollow stub and died later with
 * a confusing secondary error (field report: a damaged midlet must fail
 * CLEANLY, leaving no poisoned state behind). Class loading is serialized
 * by g_classload_mutex, so a plain flag is race-free here. */
static int s_last_jar_class_corrupt = 0;

/* Load class from JAR */
JavaClass* jvm_load_class_from_jar(JVM* jvm, const char* class_name) {
    if (!jvm || !class_name) return NULL;
    if (!jvm->class_loader.jar_data || jvm->class_loader.jar_size == 0) return NULL;
    
    /* Build filename */
    char filename[512];
    snprintf(filename, sizeof(filename), "%s.class", class_name);
    
    size_t class_size;
    uint8_t* class_data = jar_find_file_internal(jvm->class_loader.jar_data, 
                                                   jvm->class_loader.jar_size, 
                                                   filename, &class_size);
    if (!class_data) {
        fprintf(stderr, "[JAR-DBG] Class '%s' (file='%s') NOT found in JAR (jar_size=%zu)\n",
                class_name, filename, jvm->class_loader.jar_size);
        s_last_jar_class_corrupt = 0;
        return NULL;
    }
    fprintf(stderr, "[JAR-DBG] Class '%s' found in JAR, size=%zu, parsing...\n", class_name, class_size);
    
    JavaClass* clazz = classfile_parse(jvm, class_data, class_size);
    free(class_data);
    
    if (!clazz) {
        fprintf(stderr, "[JAR-DBG] Class '%s': classfile_parse FAILED\n", class_name);
        /* v36.24 [CORRUPT-CLASS-NO-STUB]: the bytes ARE there — the caller
         * must not fall back to a hollow stub for this name. */
        s_last_jar_class_corrupt = 1;
        return NULL;
    }
    s_last_jar_class_corrupt = 0;
    fprintf(stderr, "[JAR-DBG] Class '%s': parsed OK, methods_count=%d, fields_count=%d\n",
            class_name, (int)clazz->methods_count, (int)clazz->fields_count);
    
    /* Resolve super_class reference */
    if (clazz->super_class == NULL && clazz->super_class_name != NULL) {
        JVM_DEBUG("Resolving super_class %s for %s", 
                clazz->super_class_name, clazz->class_name);
        /* Try to load super class */
        JavaClass* super = jvm_load_class(jvm, clazz->super_class_name);
        if (super) {
            clazz->super_class = super;
            JVM_DEBUG("Resolved super_class: %s -> %s", 
                    clazz->class_name, super->class_name);
        } else {
            ERROR_LOG("Failed to resolve super_class %s for %s",
                    clazz->super_class_name, clazz->class_name);
        }
    }
    
    /* === CRITICAL: Recalculate instance_size after superclass is loaded ===
     * This must be done BEFORE any instance is created.
     * The instance_size must include all inherited fields from superclasses.
     */
    jvm_recalculate_instance_size(jvm, clazz);
    
    /* Add to loaded classes */
    if (jvm->class_loader.count >= jvm->class_loader.capacity) {
        /* ИСПРАВЛЕНО: Проверка переполнения capacity */
        size_t new_capacity = jvm->class_loader.capacity * 2;
        if (new_capacity <= jvm->class_loader.capacity) {
            /* Overflow check */
            ERROR_LOG("Class loader capacity overflow");
            return NULL;
        }
        if (new_capacity > 100000) {
            /* Sanity check - max 100K classes */
            ERROR_LOG("Class loader capacity limit reached");
            return NULL;
        }
        
        JavaClass** new_classes = (JavaClass**)realloc(
            jvm->class_loader.classes, 
            new_capacity * sizeof(JavaClass*)
        );
        if (!new_classes) {
            ERROR_LOG("Failed to expand class loader array");
            return NULL;
        }
        jvm->class_loader.classes = new_classes;
        jvm->class_loader.capacity = new_capacity;
    }
    
    jvm->class_loader.classes[jvm->class_loader.count++] = clazz;
    
    /* Add to hash table for fast lookup */
    class_hash_add(clazz);

    /* Initialize header for Class object support */
    /* Find java/lang/Class and set header.clazz to it */
    JavaClass* class_class = NULL;
    for (size_t i = 0; i < jvm->class_loader.count; i++) {
        if (jvm->class_loader.classes[i]->class_name &&
            strcmp(jvm->class_loader.classes[i]->class_name, "java/lang/Class") == 0) {
            class_class = jvm->class_loader.classes[i];
            break;
        }
    }
    if (class_class) {
        clazz->header.clazz = class_class;
        clazz->header.hashcode = (jint)(uintptr_t)clazz ^ 0x5A5A5A5A;
    }

    if (jvm->config.verbose_class) {
        INFO_LOG("Loaded class from JAR: %s (super: %s)]", class_name, 
               clazz->super_class ? clazz->super_class->class_name : 
               (clazz->super_class_name ? clazz->super_class_name : "(none)"));
    }
    
    /* Log all methods for Canvas subclasses to help debug */
    if (clazz->class_name && strstr(clazz->class_name, "Canvas") != NULL) {
        JVM_DEBUG("Canvas Class %s methods (%d total):", 
                clazz->class_name, clazz->methods_count);
        for (int i = 0; i < clazz->methods_count; i++) {
            JavaMethod* m = &clazz->methods[i];
            JVM_DEBUG("  [%d] %s%s (code: %u bytes, native: %s)",
                    i, m->name, m->descriptor, m->code.code_length,
                    m->is_native ? "yes" : "no");
        }
        JVM_DEBUG("Canvas Fields (%d total):", clazz->fields_count);
        for (int i = 0; i < clazz->fields_count; i++) {
            JavaField* f = &clazz->fields[i];
            JVM_DEBUG("  [%d] %s %s", i, f->descriptor, f->name);
        }
    }
    
    return clazz;
}

/* Create JVM instance */
JVM* jvm_create(void) {
    JVM* jvm = (JVM*)calloc(1, sizeof(JVM));
    if (!jvm) return NULL;
    
    /* Set default configuration */
    jvm->config.heap_size = HEAP_INITIAL_SIZE;
    jvm->config.stack_size = JAVA_STACK_SIZE;
    jvm->config.max_threads = MAX_JAVA_THREADS;

    /* FIX (audit T-2, v18): dynamically sized thread registry (grown in
     * threads.c). The old fixed 16-slot array silently dropped threads
     * beyond the cap, making them invisible to GC while running. */
    jvm->threads_capacity = 32;
    jvm->threads = (JavaThread**)calloc((size_t)jvm->threads_capacity, sizeof(JavaThread*));
    if (!jvm->threads) {
        free(jvm);
        return NULL;
    }

    return jvm;
}

/* Destroy JVM instance */
/* v34.91: defined later in this file (next to s_string_class_cache). */
void jvm_class_cache_reset(void);

void jvm_destroy(JVM* jvm) {
    if (!jvm) return;

    jvm->running = false;

    /* v36.13 HARD-KILL: signal every surviving VM runner in the global
     * registry BEFORE the idle wait — the interpreter's jvm->running check
     * is the kill switch; the registry broadcast just makes parked waiters
     * observe it at the earliest chunk. Without this a runner inside a
     * long chunked park added up to its full chunk timeout (250 ms x N)
     * to every session teardown on the device. */
    {
        extern void native_threads_kill_all(void);
        native_threads_kill_all();
    }

    /* v35.12: force-lift the frontend pause latch BEFORE the idle barrier.
     * Threads parked in jvm_frontend_pause_park() break on !running (checked
     * below via scheduler.jvm), but the cheap latch g_frontend_pause_active
     * may still be armed (frontend died mid-pause) — a parked interpreter
     * must never outlive this function or it will wake into a FREED heap
     * (user crash report: Data Abort deep inside the interpreter after a
     * relaunch, registers pointing into the freshly-reused heap region). */
    {
        extern volatile int g_frontend_pause_active;
        g_frontend_pause_active = 0;
    }

    /* v9 FIX (heap-use-after-free at teardown, ASAN-confirmed on Nescube):
     * Thread.start() spawns DETACHED native runners that keep interpreting
     * bytecode while the code below frees JavaThread structs and class
     * memory. interpret() polls jvm->running on every opcode, so live
     * workers exit at their next dispatch — wait for them here. If a thread
     * is stuck (e.g. inside a long native), LEAK the VM instead of crashing
     * with use-after-free; the OS reclaims it at process exit. */
    {
        extern int native_threads_wait_idle(int timeout_ms);
        int still_busy = native_threads_wait_idle(3000);
        /* v36.07 SECOND GRACE: the first 3 s only cover runners that need a
         * few more bytecode dispatches. A runner parked in a chunked
         * Object.wait/sleep wakes on its 250 ms chunk and then sees
         * running=false; a runner inside a slow native (JAR inflate,
         * RMS flush) needs the native to return. Give them a bounded
         * second window before stranding the whole 32-64 MB arena. */
        if (still_busy > 0) {
            LOG_SAFE("[JVM] jvm_destroy: %d still busy after 3s — second grace 4s\n",
                     still_busy);
            still_busy = native_threads_wait_idle(4000);
        }
        if (still_busy > 0) {
            LOG_SAFE("[JVM] jvm_destroy: %d Java thread(s) still active after 3s+4s — "
                     "aborting teardown (memory left to the OS) to avoid UAF\n",
                     still_busy);
            /* v35.12: make the leak VISIBLE in the device trace log — a
             * leaked VM per session is exactly the "relaunch fails to load /
             * crashes" fuel (each abort strands the whole 32 MB heap).
             * v36.07: also NAME the wedged runners in the trace file (state,
             * last native + age, park reason, read-shadow) — previously the
             * strand line said only how many, and the field log could not
             * tell WHICH thread refused to die. Plus a process-lifetime
             * counter surfaced by the [MEM] line at every session end. */
            extern void sw_trace_force(const char* fmt, ...)
                __attribute__((weak));
            if (&sw_trace_force && sw_trace_force) {
                sw_trace_force("[JVM-DESTROY] %d thread(s) BUSY after 3s+4s — VM LEAKED "
                               "(heap+classes left to the OS)", still_busy);
                {
                    extern void jvm_threads_dump_snprint(JVM* jvm, char* buf,
                                                         size_t cap, int deep);
                    static char dump[4096];
                    jvm_threads_dump_snprint(jvm, dump, sizeof(dump), 0);
                    char* p = dump;
                    int lines = 0;
                    while (p && *p && lines < 24) {
                        char* nl = strchr(p, '\n');
                        size_t len = nl ? (size_t)(nl - p) : strlen(p);
                        char one[320]; /* matches the trace layer's line cap */
                        if (len >= sizeof(one)) len = sizeof(one) - 1;
                        memcpy(one, p, len);
                        one[len] = '\0';
                        if (one[0]) sw_trace_force("%s", one);
                        lines++;
                        if (!nl) break;
                        p = nl + 1;
                    }
                }
                g_vm_strand_count++;
            }
            return;
        }
    }

    /* v35.12: every runner has exited — clear the process-global pthread
     * registries so the next session cannot inherit dangling JavaThread*
     * entries (garbage "td t6684680" ids, tid probes into freed memory). */
    {
        extern void native_threads_registry_reset(void);
        native_threads_registry_reset();
    }
    
    /* Cleanup method cache and class hash table */
    method_cache_cleanup();
    
    /* v36.04 TEARDOWN-ORDER (fixes the deferred session-48 (c) UAF): reset
     * the GC roots BEFORE the JavaThread structs are freed. gc_roots_reset_all
     * takes the heap lock, and heap_lock reads thread_current(NULL)->id —
     * with the old order (reset AFTER the thread_destroy loop below) that
     * read dereferenced the ALREADY-FREED current JavaThread (ASAN:
     * heap-use-after-free heap.c heap_lock <- gc_roots_reset_all <-
     * jvm_destroy). Roots only reference heap objects, so clearing them
     * earlier is equivalent; heap_destroy below is still preceded by a
     * fully-reset roots array. */
    {
        extern void gc_roots_reset_all(void);
        gc_roots_reset_all();
        /* v36.12: free the pinned-root SLOTS (see jvm_pinned_roots_reset) —
         * gc_roots_reset_all only clears the registry entries. */
        extern void jvm_pinned_roots_reset(void);
        jvm_pinned_roots_reset();
    }

    /* Destroy all threads */
    for (int i = 0; i < jvm->thread_count; i++) {
        if (jvm->threads[i]) {
            thread_destroy(jvm, jvm->threads[i]);
        }
    }
    /* FIX (audit T-2, v18): registry itself is heap-allocated now */
    free(jvm->threads);
    jvm->threads = NULL;
    jvm->threads_capacity = 0;
    
    /* Free class loader */
    if (jvm->class_loader.classes) {
        for (size_t i = 0; i < jvm->class_loader.count; i++) {
            JavaClass* clazz = jvm->class_loader.classes[i];
            if (!clazz) continue;
            
            /* For stub classes, free only what was allocated */
            if (clazz->is_stub) {
                /* Free class_name and super_class_name allocated by strdup */
                free((void*)clazz->class_name);
                free((void*)clazz->super_class_name);
                
                /* Free constant pool UTF8 entries */
                if (clazz->constant_pool) {
                    for (uint16_t j = 1; j < clazz->constant_pool_count; j++) {
                        if (clazz->constant_pool[j].tag == CONSTANT_Utf8) {
                            free(clazz->constant_pool[j].info.utf8.bytes);
                        }
                    }
                    free(clazz->constant_pool);
                }
                
                /* Free methods - stub methods have allocated name/descriptor */
                if (clazz->methods) {
                    for (uint16_t j = 0; j < clazz->methods_count; j++) {
                        free((void*)clazz->methods[j].name);
                        free((void*)clazz->methods[j].descriptor);
                        free(clazz->methods[j].code.code);
                    }
                    free(clazz->methods);
                }
                
                /* Free fields */
                if (clazz->fields) {
                    for (uint16_t j = 0; j < clazz->fields_count; j++) {
                        free((void*)clazz->fields[j].name);
                        free((void*)clazz->fields[j].descriptor);
                    }
                    free(clazz->fields);
                }
                
                /* v36.12 (malloc-census find): the hand-rolled stub free
                 * list above missed FOUR member groups that runtime stub
                 * injection does allocate:
                 *   1. static_fields — stub_add_static_const strdups
                 *      name/descriptor and calloc/reallocs the array
                 *      (~18 stub classes x 16 slots = the measured
                 *      18 blocks / ~7 KB per session);
                 *   2. interfaces — create_stub_class strdup'd/malloc'd;
                 *   3. CP inline caches (field/invoke/static_field) —
                 *      lazily calloc'd on first getstatic/invoke
                 *      (the measured field_cache_entry growth);
                 *   4. gc_ref_slots — GC mark-layout cache.
                 * Mirror classfile_free's coverage for the stub shape. */
                if (clazz->interfaces) {
                    /* uint16_t* CP indices (not pointers) — the referenced
                     * CP class entries die with the constant_pool above. */
                    free(clazz->interfaces);
                }
                if (clazz->attributes) {
                    for (uint16_t j = 0; j < clazz->attributes_count; j++) {
                        free(clazz->attributes[j].info);
                    }
                    free(clazz->attributes);
                }
                if (clazz->static_fields) {
                    for (int sf_i = 0; sf_i < clazz->static_fields_count; sf_i++) {
                        free(clazz->static_fields[sf_i].name);
                        free(clazz->static_fields[sf_i].descriptor);
                    }
                }
                free(clazz->static_fields);
                free(clazz->field_cache);
                free(clazz->invoke_cache);
                free(clazz->static_field_cache);
                free(clazz->gc_ref_slots);
                
                free(clazz);
            } else {
                /* Use regular classfile_free for non-stub classes */
                classfile_free(clazz);
            }
        }
        free(jvm->class_loader.classes);
    }
    
    if (jvm->class_loader.class_path) {
        for (size_t i = 0; i < jvm->class_loader.class_path_count; i++) {
            free(jvm->class_loader.class_path[i]);
        }
        free(jvm->class_loader.class_path);
    }
    
    /* Free jar_path if allocated */
    free((void*)jvm->class_loader.jar_path);
    
    /* Free string pool - only free utf8 as chars is part of heap allocation */
    if (jvm->string_pool.strings) {
        for (size_t i = 0; i < jvm->string_pool.count; i++) {
            JavaString* str = jvm->string_pool.strings[i];
            if (str) {
                /* utf8 is allocated separately with strdup */
                free(str->utf8);
                /* chars is part of the heap allocation, don't free separately */
                /* The whole JavaString struct is freed by heap_destroy */
            }
        }
        free(jvm->string_pool.strings);
    }
    
    /* Destroy heap - this frees all objects including JavaString structs.
     * v35.08 MULTI-SESSION: NULL every registered GC-root slot FIRST (while
     * the roots array is still alive) - those slots are process-global
     * statics in the native/MIDP layers that pointed into THIS heap; after
     * the free they dangle and the next session's heap often lands at the
     * same address, aliasing foreign objects (field-repro: second game from
     * the frontend menu died in Display.setCurrent on a stale Display
     * singleton). See include/heap.h for the safety argument.
     * v36.04: the reset itself moved ABOVE the thread_destroy loop (see the
     * TEARDOWN-ORDER note there); it stays ahead of heap_destroy. */
    heap_destroy(jvm);

    /* v36.13: the intern pool is process-global but pointed into THIS heap
     * — wipe pool + hash table while the heap pointers are already dead
     * but before any next-session allocation can alias them (the root
     * cause of the field [STR-CHARS-NULL] hits; see native.c). */
    {
        extern void native_intern_pool_session_reset(void);
        native_intern_pool_session_reset();
    }

    /* v36.33 [HT-PEER-SESSION-RESET]: native Hashtable peers (g_hashtables)
     * hold raw JavaObject* keys/values from THIS heap and survived
     * heap_destroy (the sweep that normally frees them never runs on the
     * session-teardown path). The next session's arena usually lands at
     * the same address: hashtable_ensure_peer matches the STALE peer by
     * object pointer (aliasing) and the new Hashtable inherits the dead
     * session's entries — the first get()/put() dereferences a garbage
     * key. ASAN repro: Temple Rush 3D 2, SEGV in hashtable_keys_equal
     * during session-2 class init; field: Invalid memory access
     * 0x736D722E65726F74 (ASCII "tore.rms"). */
    {
        extern void native_hashtable_session_reset(void);
        native_hashtable_session_reset();
    }

    /* v36.33 [SAX-SESSION-RESET]: same staleness class for the SAX layer's
     * per-process Java-object references (AttributesImpl, InputSource ring). */
    {
        extern void sax_session_reset(void);
        sax_session_reset();
    }

    /* Cleanup native fallback buffers */
    native_cleanup_fallbacks();
    
    /* v34.91 MULTI-SESSION FIX: process-global caches (string-class cache in
     * jvm.c, Object/String class caches in heap.c, pinned box caches and
     * singletons in native.c) pointed into THIS jvm's class table / heap —
     * drop them so the next game session (Switch frontend) re-resolves.
     * ASan-confirmed without this: heap-use-after-free in
     * jvm_new_string_utf16 <- init_javax_microedition_lcdui_form. */
    {
        extern void heap_class_cache_reset(void);
        extern void native_object_cache_reset(void);
        extern void threads_scheduler_reset(void); /* v34.91: scheduler.jvm dangled at THIS jvm */
        jvm_class_cache_reset();
        heap_class_cache_reset();
        native_object_cache_reset();
        threads_scheduler_reset();
    }
    /* v35.08 MULTI-SESSION, part 2: the layers the root-slot wipe above
     * cannot reach mechanically.
     *  - native_timers_session_reset(): the java.util.Timer registry held
     *    session objects in static slots AND a "roots registered" latch
     *    that skipped re-registration in the next session (the GC then
     *    swept scheduled tasks mid-flight - the v34.69 Captain-Fatal bug
     *    resurrected for session two).
     *  - midp_session_reset(): display/form/m3g/rms statics the root wipe
     *    does not cover (unrooted pointers, "already rooted" flags, queue
     *    indices, booleans). Weak-linked: white-box builds link jvm.c
     *    without the MIDP layer. */
    {
        extern void native_timers_session_reset(void);
        native_timers_session_reset();
    }
    {
        extern void midp_session_reset(void) __attribute__((weak));
        if (midp_session_reset) midp_session_reset();
    }
    
    /* Cleanup pthread monitor resources */
    cleanup_monitors();
    
    free(jvm);
}

/* Initialize JVM */
/* v35.12: stage diagnostics reach the device trace log even from the core
 * (weak link — headless/libretro builds without switch_trace.c just skip). */
#include <stddef.h>
#include <stdarg.h>
static void jvm_stage_trace(const char* fmt, ...) {
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

int jvm_init(JVM* jvm) {
    if (!jvm) return JNI_ERR;
    
    /* Set global JVM pointer for object_instance_of */
    extern JVM* g_jvm_for_instanceof;
    g_jvm_for_instanceof = jvm;

    /* v34.71: record the OS identity of this (main/frontend) thread for the
     * heap TLAB gate — jvm_init runs on the host loop thread by construction. */
    jvm_record_main_os_thread();
    
    /* Initialize method cache and class hash table */
    method_cache_init();
    
    /* Initialize heap */
    if (heap_init(jvm, jvm->config.heap_size, HEAP_MAX_SIZE) != JNI_OK) {
        ERROR_LOG("Failed to initialize heap");
        /* v35.12: name the failing stage + sizes in the device log — the
         * "relaunch does not load" report was a silent JNI_ERR before. */
        jvm_stage_trace("init: heap FAILED (%u KB + reserve, max %u KB)",
                        (unsigned)(jvm->config.heap_size / 1024),
                        (unsigned)(HEAP_MAX_SIZE / 1024));
        /* v36.07 RETRY LADDER: the requested arena can be unavailable for
         * reasons that only affect THIS size — a previous session's
         * wedged-runner strand in a small (applet-mode) process heap, or
         * mid-session fragmentation. Doom RPG (2.6 MB) and the whole J2ME
         * class of games fit into a fraction of 64 MB, so instead of
         * failing the load ("загружается через раз"), shrink and retry.
         * Each attempt leaves one forced line: the log shows the ladder
         * and where it landed. */
        {
            static const size_t ladder_mb[] = { 32, 24, 16, 12, 8 };
            int ok = 0;
            for (size_t i = 0; i < sizeof(ladder_mb) / sizeof(ladder_mb[0]); i++) {
                size_t sz = ladder_mb[i] * 1024 * 1024;
                if (sz >= jvm->config.heap_size) continue;
                jvm_stage_trace("init: heap retry at %u MB",
                                (unsigned)ladder_mb[i]);
                jvm->config.heap_size = sz;
                if (heap_init(jvm, jvm->config.heap_size, HEAP_MAX_SIZE) == JNI_OK) {
                    ok = 1;
                    jvm_stage_trace("init: heap OK at %u MB (reduced)",
                                    (unsigned)ladder_mb[i]);
                    break;
                }
            }
            if (!ok) return JNI_ERR;
        }
    }
    
    /* Initialize thread system */
    if (threads_init(jvm) != JNI_OK) {
        ERROR_LOG("Failed to initialize threads");
        jvm_stage_trace("init: threads FAILED");
        return JNI_ERR;
    }
    
    /* Create main thread */
    jvm->main_thread = thread_create(jvm, "main", THREAD_PRIORITY_NORM, NULL);
    if (!jvm->main_thread) {
        ERROR_LOG("Failed to create main thread");
        jvm_stage_trace("init: main thread FAILED (stack %u KB)",
                        (unsigned)(JAVA_STACK_SIZE / 1024));
        return JNI_ERR;
    }
    
    jvm->threads[0] = jvm->main_thread;
    jvm->thread_count = 1;
    
    /* Initialize class loader */
    jvm->class_loader.capacity = 256;
    jvm->class_loader.classes = (JavaClass**)calloc(jvm->class_loader.capacity, sizeof(JavaClass*));
    if (!jvm->class_loader.classes) {
        jvm_stage_trace("init: class-loader table FAILED");
        return JNI_ERR;
    }
    
    /* Initialize string pool */
    jvm->string_pool.capacity = 256;
    jvm->string_pool.strings = (JavaString**)calloc(jvm->string_pool.capacity, sizeof(JavaString*));
    if (!jvm->string_pool.strings) {
        jvm_stage_trace("init: string-pool table FAILED");
        return JNI_ERR;
    }
    
    /* Initialize built-in stub classes for J2ME API */
    int stub_count = init_stub_classes(jvm);
    if (jvm->config.verbose_class) {
        printf("[JVM] Initialized %d stub classes\n", stub_count);
    }
    
    /* v19: pre-allocate the emergency OutOfMemoryError singleton NOW, while
     * the heap is empty. If the heap ever becomes so full that even the
     * emergency reserve cannot fit a new exception object, jvm_throw_by_name
     * throws this instance instead of silently failing (which used to stop
     * the VM: "OutOfMemory stops execution"). Rooted in GC so it lives
     * forever. Must run AFTER heap + class loader + stub init. */
    {
        JavaClass* oom_clazz = jvm_load_class(jvm, "java/lang/OutOfMemoryError");
        if (oom_clazz) {
            g_emergency_oom = jvm_new_object(jvm, oom_clazz);
            if (g_emergency_oom) {
                gc_add_root(jvm, (void**)&g_emergency_oom);
            } else {
                ERROR_LOG("Failed to allocate emergency OutOfMemoryError singleton");
            }
        }
    }
    
    jvm->running = true;
    
    return JNI_OK;
}

/* Set JAR data for class loading */
void jvm_set_jar_data(JVM* jvm, uint8_t* data, size_t size, const char* path) {
    if (!jvm) return;
    jvm->class_loader.jar_data = data;
    jvm->class_loader.jar_size = size;
    jvm->class_loader.jar_path = path ? strdup(path) : NULL;
}

/* Load a class by name - OPTIMIZED with hash table */
/* v34.9 THREAD-SAFETY FIX: jvm_load_class mutates shared, non-locked
 * structures (class hash table, class_loader.classes[] array, stub cache).
 * Bounce Tales loads classes from TWO pthreads concurrently (the game
 * thread's render step calls GameCanvas.getGraphics() -> jvm_load_class
 * while the main thread's work loader parses game classes). The unsynchronized
 * race made work items vanish / lookups stall, leaving the chapter loader
 * spinning forever on a white "loading" screen after "New game".
 * Serialize ALL class loading with one recursive mutex (recursive because
 * jvm_load_class recurses for superclasses and stub creation). */
static pthread_mutex_t g_classload_mutex;
static volatile int g_classload_mutex_init_done = 0;
static pthread_mutex_t g_classload_init_mutex = PTHREAD_MUTEX_INITIALIZER;

static void classload_mutex_init(void) {
    if (__sync_fetch_and_add(&g_classload_mutex_init_done, 0) == 1) return;
    pthread_mutex_lock(&g_classload_init_mutex);
    if (g_classload_mutex_init_done == 0) {
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&g_classload_mutex, &attr);
        pthread_mutexattr_destroy(&attr);
        __sync_fetch_and_or(&g_classload_mutex_init_done, 1);
    }
    pthread_mutex_unlock(&g_classload_init_mutex);
}

static JavaClass* jvm_load_class_impl(JVM* jvm, const char* class_name);

/* v34.49 FIX (3D Ferrari / Sumea): JAR-over-stub override for the LINEAR
 * lookup paths.
 *
 * jvm_load_class_impl() Step 1 (hash hit) offers a builtin STUB class one
 * chance to be replaced by the MIDlet's own class from the JAR (v20 P0-2).
 * But init_stub_classes() pre-registers its stubs ONLY in the
 * class_loader.classes[] ARRAY (not in the hash), so the FIRST resolution of
 * such a name goes through the LINEAR search paths — jvm_load_class_impl
 * Step 2 and get_or_create_stub_class() (classfile.c resolves every
 * superclass through the latter) — which returned the stub WITHOUT the JAR
 * probe. 3D Ferrari ships its own Nokia UI shims
 * (com/nokia/mid/ui/FullCanvas extends com/hellomoto/fullscreen/FullCn,
 * DeviceControl, DirectUtils, Sound); the builtin FullCanvas (extends plain
 * Canvas) won, the GameMIDlet<init> checkcast to FullCn threw
 * ClassCastException, the surrounding catch swallowed it, the i.a() text
 * init was skipped, and Ferrari3D.<init> died with an NPE.
 *
 * This helper applies the SAME override rules as the hash path:
 *  - only when the found class is a stub,
 *  - only when the JAR is attached and actually contains the class,
 *  - never for protected names whose layout/natives are wired to the stub.
 * On success it replaces EVERY array slot holding the stub (so future linear
 * searches see the real class too) and updates the hash entry.
 * Returns the JAR class, or NULL to keep the stub.
 *
 * Protected names: the java/lang core + M3G list of Step 1, PLUS the two
 * Nokia UI classes whose builtin stubs carry per-object field state that the
 * registered natives read/write (DirectGraphics.graphics/alphaComponent,
 * SoftNotification.*). Their KEY_* / SOUND_* constants are compile-time
 * inlined by javac, so games never read them at runtime. */
JavaClass* jvm_stub_jar_override_linear(JVM* jvm, const char* class_name, JavaClass* stub) {
    if (!jvm || !class_name || !stub) return NULL;
    if (!(jvm->class_loader.jar_data && jvm->class_loader.jar_size > 0)) return NULL;

    /* Protected names (mirrors jvm_load_class_impl Step 1) */
    if (strncmp(class_name, "java/lang/Object", 16) == 0 ||
        strncmp(class_name, "java/lang/String", 16) == 0 ||
        strncmp(class_name, "java/lang/Class", 15) == 0 ||
        strncmp(class_name, "java/lang/Thread", 16) == 0 ||
        strncmp(class_name, "java/lang/System", 16) == 0 ||
        strncmp(class_name, "java/lang/Runtime", 17) == 0 ||
        strncmp(class_name, "java/lang/Math", 14) == 0 ||
        strncmp(class_name, "javax/microedition/m3g/", 23) == 0 ||
        /* v34.49: native-state stubs — see comment above */
        strcmp(class_name, "com/nokia/mid/ui/DirectGraphics") == 0 ||
        strcmp(class_name, "com/nokia/mid/ui/SoftNotification") == 0) {
        return NULL;
    }

    /* If the hash already resolved this name to a different (real) class,
     * that is the freshest version — hand it out instead of the stub.
     * (The Step-1 override updates the hash but NOT the array, so this
     * state is reachable.) */
    JavaClass* hashed = class_hash_lookup(class_name);
    if (hashed && hashed != stub) {
        return hashed;
    }

    /* Make sure a hash entry exists so the negative cache works */
    if (!hashed) {
        class_hash_add(stub);
    }
    if (class_hash_was_jar_checked(class_name)) {
        return NULL;  /* probed before, JAR has no such class */
    }

    JavaClass* real = jvm_load_class_from_jar(jvm, class_name);
    if (!real) {
        class_hash_mark_jar_checked(class_name);  /* negative cache */
        return NULL;
    }

    /* Replace every array slot that still points at the stub */
    for (size_t i = 0; i < jvm->class_loader.count; i++) {
        if (jvm->class_loader.classes[i] == stub) {
            jvm->class_loader.classes[i] = real;
        }
    }
    /* jvm_load_class_from_jar() APPENDED `real` to the array before we
     * replaced the stub slots, so it may now appear twice — teardown
     * (jvm_destroy frees every array entry) would double-free it.
     * Compact: keep only the FIRST occurrence of `real`. */
    {
        size_t w = 0;
        bool seen = false;
        for (size_t i = 0; i < jvm->class_loader.count; i++) {
            JavaClass* c = jvm->class_loader.classes[i];
            if (c == real) {
                if (seen) continue;  /* drop later duplicates */
                seen = true;
            }
            jvm->class_loader.classes[w++] = c;
        }
        jvm->class_loader.count = w;
    }
    class_hash_add(real);  /* update/insert hash entry */

    static int override_log_count = 0;
    if (override_log_count < 10) {
        override_log_count++;
        LOG_SAFE("[CLASS] JAR overrides builtin stub '%s' (linear-path hierarchy repair)\n",
                 class_name);
    }
    return real;
}


/* v34.9: public entry — serializes class loading across threads.
 * Recursive mutex: the impl recurses for superclasses and stub creation. */
JavaClass* jvm_load_class(JVM* jvm, const char* class_name) {
    if (!jvm || !class_name) return NULL;
    classload_mutex_init();
    pthread_mutex_lock(&g_classload_mutex);
    JavaClass* r = jvm_load_class_impl(jvm, class_name);
    pthread_mutex_unlock(&g_classload_mutex);
    return r;
}

static JavaClass* jvm_load_class_impl(JVM* jvm, const char* class_name) {
    

    /* Step 1: Check hash table first (O(1) average) */
    JavaClass* clazz = class_hash_lookup(class_name);
    if (clazz) {
        /* v20 FIX (P0-2): pre-registered stub classes must not shadow real
         * MIDlet classes. init_stub_classes() populates the hash BEFORE the
         * JAR is attached, and this hash-first lookup returned the empty
         * stub forever — a MIDlet carrying its own Sprite/Stack/... executed
         * the dummy instead of its real bytecode. If a stub is hit after the
         * JAR became available, give the JAR one chance to override.
         *
         * Protected names keep the stub (platform authority): the VM core is
         * wired to their exact layout (String fields, Thread ids, Class
         * objects, System slots), and the javax/microedition/m3g/ stubs carry
         * the native-integrated field names our software rasterizer reads.
         * Per MIDP the AMS would reject suites shipping those anyway. */
        if (clazz->is_stub &&
            jvm->class_loader.jar_data && jvm->class_loader.jar_size > 0 &&
            !class_hash_was_jar_checked(class_name) &&
            strncmp(class_name, "java/lang/Object", 16) != 0 &&
            strncmp(class_name, "java/lang/String", 16) != 0 &&
            strncmp(class_name, "java/lang/Class", 15) != 0 &&
            strncmp(class_name, "java/lang/Thread", 16) != 0 &&
            strncmp(class_name, "java/lang/System", 16) != 0 &&
            strncmp(class_name, "java/lang/Runtime", 17) != 0 &&
            strncmp(class_name, "java/lang/Math", 14) != 0 &&
            strncmp(class_name, "javax/microedition/m3g/", 23) != 0) {
            JavaClass* real = jvm_load_class_from_jar(jvm, class_name);
            if (!real) {
                class_hash_mark_jar_checked(class_name);  /* negative cache */
            }
            if (real) {
                static int override_log_count = 0;
                if (override_log_count++ < 10) {
                    LOG_SAFE("[CLASS] JAR overrides stub class '%s' "
                             "(stub had %d methods, real has %d)\n",
                             class_name, (int)clazz->methods_count,
                             (int)real->methods_count);
                }
                class_hash_add(real);  /* updates the entry in place */
                if (real->super_class == NULL && real->super_class_name != NULL) {
                    real->super_class = jvm_load_class(jvm, real->super_class_name);
                }
                return real;
            }
        }
        /* Resolve super_class if not already done */
        if (clazz->super_class == NULL && clazz->super_class_name != NULL) {
            clazz->super_class = jvm_load_class(jvm, clazz->super_class_name);
        }
        return clazz;
    }
    
    /* Step 2: Fallback to linear search (should rarely happen) */
    for (size_t i = 0; i < jvm->class_loader.count; i++) {
        JavaClass* c = jvm->class_loader.classes[i];
        if (c->class_name && strcmp(c->class_name, class_name) == 0) {
            /* v34.49 FIX (3D Ferrari): this array-first path used to return
             * a builtin STUB without giving the JAR its override chance
             * (only the hash path did). Same rules now. */
            if (c->is_stub) {
                JavaClass* real = jvm_stub_jar_override_linear(jvm, class_name, c);
                if (real) return real;
            }
            /* Add to hash table for future lookups */
            class_hash_add(c);
            return c;
        }
    }
    
    /* Try to load from JAR first if JAR data is available */
    if (jvm->class_loader.jar_data && jvm->class_loader.jar_size > 0) {
        clazz = jvm_load_class_from_jar(jvm, class_name);
        if (clazz) return clazz;
    }
    
    /* Convert class name to path */
    char* path = class_name_to_internal(class_name);
    if (!path) return NULL;
    
    /* Try to load from current directory */
    char filename[1024];
    snprintf(filename, sizeof(filename), "%s.class", path);
    
    clazz = classfile_parse_file(jvm, filename);
    
    free(path);
    
    if (!clazz) {
        /* v36.24 [CORRUPT-CLASS-NO-STUB]: if the class file EXISTS in the
         * JAR but failed to parse, the stub fallback below would create a
         * hollow class (no fields, no methods) and the midlet would die
         * later with a confusing secondary error. Fail the load CLEANLY
         * instead — run_midlet reports "Failed to load class" and the
         * session tears down without poisoning anything. */
        if (s_last_jar_class_corrupt) {
            LOG_SAFE("[CLASS] '%s': corrupt class file in JAR — refusing stub fallback\n",
                     class_name);
            return NULL;
        }
        /* Class not found in filesystem - create or get a stub class */
        fprintf(stderr, "[CLASS-DBG] Class '%s' not found in JAR or filesystem, creating stub\n", class_name);
        clazz = get_or_create_stub_class(jvm, class_name);
        if (!clazz) {
            WARN_LOG("Class not found and stub creation failed: %s", class_name);
            return NULL;
        }
        return clazz;  /* Stub already added to class loader */
    }
    
    /* Add to loaded classes */
    if (jvm->class_loader.count >= jvm->class_loader.capacity) {
        size_t new_capacity = jvm->class_loader.capacity * 2;
        if (new_capacity <= jvm->class_loader.capacity || new_capacity > 100000) {
            ERROR_LOG("Class loader capacity overflow or limit reached");
            free(clazz);
            return NULL;
        }
        
        JavaClass** new_classes = (JavaClass**)realloc(
            jvm->class_loader.classes, 
            new_capacity * sizeof(JavaClass*)
        );
        if (!new_classes) {
            ERROR_LOG("Failed to expand class loader array");
            free(clazz);
            return NULL;
        }
        jvm->class_loader.classes = new_classes;
        jvm->class_loader.capacity = new_capacity;
    }
    
    jvm->class_loader.classes[jvm->class_loader.count++] = clazz;
    
    /* Add to hash table for future lookups */
    class_hash_add(clazz);

    if (jvm->config.verbose_class) {
        INFO_LOG("Loaded class: %s", class_name);
    }
    
    return clazz;
}

/* Define a class from raw bytes */
JavaClass* jvm_define_class(JVM* jvm, const uint8_t* data, size_t length) {
    if (!jvm || !data || length == 0) return NULL;
    
    JavaClass* clazz = classfile_parse(jvm, data, length);
    if (!clazz) return NULL;
    
    /* Resolve class name */
    if (clazz->this_class > 0) {
        clazz->class_name = (char*)classfile_get_class_name(clazz, clazz->this_class);
    }
    
    /* Add to loaded classes */
    if (jvm->class_loader.count >= jvm->class_loader.capacity) {
        /* ИСПРАВЛЕНО: Проверка переполнения capacity */
        size_t new_capacity = jvm->class_loader.capacity * 2;
        if (new_capacity <= jvm->class_loader.capacity || new_capacity > 100000) {
            ERROR_LOG("Class loader capacity overflow or limit reached");
            classfile_free(clazz);
            return NULL;
        }
        
        JavaClass** new_classes = (JavaClass**)realloc(
            jvm->class_loader.classes, 
            new_capacity * sizeof(JavaClass*)
        );
        if (!new_classes) {
            ERROR_LOG("Failed to expand class loader array");
            classfile_free(clazz);
            return NULL;
        }
        jvm->class_loader.classes = new_classes;
        jvm->class_loader.capacity = new_capacity;
    }
    
    jvm->class_loader.classes[jvm->class_loader.count++] = clazz;
    
    /* Recalculate instance size based on fields and superclass */
    jvm_recalculate_instance_size(jvm, clazz);
    
    return clazz;
}

/* Resolve a method in a class - uses optimized cache lookup */
JavaMethod* jvm_resolve_method(JVM* jvm, JavaClass* clazz, const char* name, const char* descriptor) {
    if (!clazz || !name || !descriptor) return NULL;
    
    /* Use fast method cache lookup */
    JavaMethod* method = method_cache_lookup(clazz, name, descriptor);
    if (method) return method;
    
    /* Linear search in class methods */
    for (int i = 0; i < clazz->methods_count; i++) {
        JavaMethod* m = &clazz->methods[i];
        if (m->name && m->descriptor &&
            strcmp(m->name, name) == 0 && 
            strcmp(m->descriptor, descriptor) == 0) {
            /* Cache the result */
            method_cache_store(clazz, name, descriptor, m);
            return m;
        }
    }
    
    /* v36.35 FIX (Bubble Bash 3 eternal loading): <clinit> is NOT inherited.
     * JVMS §5.5: the class initializer runs exactly once for the class that
     * DECLARES it; jvm_resolve_method used to walk the superclass chain for
     * every name — so the first touch of a SUBCLASS without its own <clinit>
     * (obfuscated Gameloft hierarchy: b extends a) resolved and RE-EXECUTED
     * the superclass's <clinit>, wiping ~700 live statics (font tables,
     * game state) mid-load — the loading screen looped forever with NPEs
     * (aaload on the re-nulled font array in a.d/a.a).
     * The guard sits AFTER the declaring-class search above (the class's own
     * <clinit> must still resolve) and only blocks the SUPERCLASS walk.
     * <init> keeps the walk: super() constructor chains resolve through the
     * superclass on purpose. */
    if (name[0] == '<' && name[1] == 'c' && strcmp(name, "<clinit>") == 0) {
        return NULL;
    }
    
    /* Search in superclass */
    if (clazz->super_class) {
        method = jvm_resolve_method(jvm, clazz->super_class, name, descriptor);
        if (method) {
            /* Cache the result for the original class too */
            method_cache_store(clazz, name, descriptor, method);
        }
        return method;
    }
    
    return NULL;
}

/* Resolve a field in a class */
JavaField* jvm_resolve_field(JVM* jvm, JavaClass* clazz, const char* name, const char* descriptor) {
    if (!clazz || !name || !descriptor) return NULL;
    
    /* Search in class fields */
    for (int i = 0; i < clazz->fields_count; i++) {
        JavaField* field = &clazz->fields[i];
        if (field->name && field->descriptor &&
            strcmp(field->name, name) == 0 &&
            strcmp(field->descriptor, descriptor) == 0) {
            return field;
        }
    }
    
    /* Search in superclass */
    if (clazz->super_class) {
        return jvm_resolve_field(jvm, clazz->super_class, name, descriptor);
    }
    
    return NULL;
}

/* Create new object */
JavaObject* jvm_new_object(JVM* jvm, JavaClass* clazz) {
    if (!jvm || !clazz) return NULL;
    
    /* Initialize class if needed - recalculate instance size from superclass chain
     * but do NOT run <clinit> here (we may be in a context where we can't safely
     * execute bytecode, e.g. during exception creation). Just ensure the size is
     * correct so the allocation doesn't corrupt the heap. */
    if (!clazz->initialized && !clazz->initializing) {
        jvm_recalculate_instance_size(jvm, clazz);
        /* Do NOT set clazz->initialized = true without running <clinit>! */
    }
    
    if (jvm->config.verbose_class) {
        printf("[JVM] Creating object of class: %s\n", clazz->class_name);
    }
    
    return heap_alloc_object(jvm, clazz);
}

/* v19: create an object using the emergency reserve. Used exclusively for
 * exception objects when the heap is (nearly) full — see jvm_throw_by_name. */
JavaObject* jvm_new_object_emergency(JVM* jvm, JavaClass* clazz) {
    if (!jvm || !clazz) return NULL;

    if (!clazz->initialized && !clazz->initializing) {
        jvm_recalculate_instance_size(jvm, clazz);
    }

    return heap_alloc_object_emergency(jvm, clazz);
}

/* Create new array */
JavaArray* jvm_new_array(JVM* jvm, uint8_t type, jsize length, JavaClass* element_class) {
    return heap_alloc_array(jvm, type, length, element_class);
}

/* Create new string */
JavaString* jvm_new_string(JVM* jvm, const char* utf8) {
    if (!jvm || !utf8) return NULL;
    
    size_t utf8_len = strlen(utf8);
    
    /* First pass: count UTF-16 code units */
    size_t utf16_len = 0;
    for (size_t i = 0; i < utf8_len; ) {
        uint8_t c = (uint8_t)utf8[i];
        if (c < 0x80) {
            /* ASCII: 1 byte -> 1 UTF-16 code unit */
            utf16_len++;
            i++;
        } else if ((c & 0xE0) == 0xC0) {
            /* 2-byte UTF-8 sequence -> 1 UTF-16 code unit */
            utf16_len++;
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            /* 3-byte UTF-8 sequence -> 1 UTF-16 code unit */
            utf16_len++;
            i += 3;
        } else if ((c & 0xF8) == 0xF0) {
            /* 4-byte UTF-8 sequence -> 2 UTF-16 code units (surrogate pair) */
            utf16_len += 2;
            i += 4;
        } else {
            /* Invalid UTF-8 byte, skip */
            i++;
        }
    }
    
    /* Allocate UTF-16 buffer */
    jchar* chars = (jchar*)malloc(utf16_len * sizeof(jchar));
    if (!chars) return NULL;
    
    /* Second pass: decode UTF-8 to UTF-16.
     * v34.44 HARDENING: truncated sequences (a multi-byte lead byte at the
     * very end of the buffer) are SKIPPED here, so the actually-decoded count
     * (j) can be SMALLER than the first-pass estimate (utf16_len) — passing
     * utf16_len to jvm_new_string_utf16 would publish the uninitialized
     * malloc() tail as string content (mojibake tail / information leak on
     * malformed readUTF data). Use j. */
    size_t j = 0;
    for (size_t i = 0; i < utf8_len; ) {
        uint8_t c = (uint8_t)utf8[i];
        if (c < 0x80) {
            /* ASCII */
            chars[j++] = (jchar)c;
            i++;
        } else if ((c & 0xE0) == 0xC0) {
            /* 2-byte sequence: 110xxxxx 10xxxxxx */
            if (i + 1 < utf8_len) {
                uint32_t cp = ((c & 0x1F) << 6) | ((uint8_t)utf8[i+1] & 0x3F);
                chars[j++] = (jchar)cp;
                i += 2;
            } else {
                i++;
            }
        } else if ((c & 0xF0) == 0xE0) {
            /* 3-byte sequence: 1110xxxx 10xxxxxx 10xxxxxx */
            if (i + 2 < utf8_len) {
                uint32_t cp = ((c & 0x0F) << 12) | 
                             (((uint8_t)utf8[i+1] & 0x3F) << 6) | 
                             ((uint8_t)utf8[i+2] & 0x3F);
                chars[j++] = (jchar)cp;
                i += 3;
            } else {
                i++;
            }
        } else if ((c & 0xF8) == 0xF0) {
            /* 4-byte sequence: 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx */
            if (i + 3 < utf8_len) {
                uint32_t cp = ((c & 0x07) << 18) | 
                             (((uint8_t)utf8[i+1] & 0x3F) << 12) | 
                             (((uint8_t)utf8[i+2] & 0x3F) << 6) | 
                             ((uint8_t)utf8[i+3] & 0x3F);
                /* Encode as UTF-16 surrogate pair */
                cp -= 0x10000;
                chars[j++] = (jchar)(0xD800 | (cp >> 10));
                chars[j++] = (jchar)(0xDC00 | (cp & 0x3FF));
                i += 4;
            } else {
                i++;
            }
        } else {
            /* Invalid UTF-8 byte, skip */
            i++;
        }
    }
    
    JavaString* str = jvm_new_string_utf16(jvm, chars, (jsize)j);
    free(chars);
    
    /* CRITICAL FIX: Only set utf8 cache for native strings (OBJ_TYPE_STRING).
     * For Java String objects (OBJ_TYPE_OBJECT), the JavaString struct fields
     * (length, hash, utf8, chars) overlap with the fields[] array!
     * Setting str->utf8 would corrupt the value/offset/count/hash fields.
     * 
     * How to detect: native strings have clazz=NULL, Java strings have clazz=java/lang/String
     */
    if (str && str->header.clazz == NULL) {
        /* Native string (no java/lang/String class loaded) - safe to use struct fields */
        str->utf8 = strdup(utf8);
    }
    
    return str;
}

/* Create new string from UTF-16
 * 
 * CRITICAL FIX: Правильная модель Java String!
 * Если класс java/lang/String загружен, создаем OBJ_TYPE_OBJECT с полями и отдельный char[] массив для value.
 * Иначе использу inline хранилище как раньше.
 */
/* v34.58 PERF: кэш java/lang/String для jvm_new_string_utf16.
 * Прежний линейный скан массива классов (500-2000+ strcmp) выполнялся
 * на КАЖДОЕ создание строки — при стринг-churn'е игр это самая дорогая
 * константа в пути аллокации строк. Классы в этой VM не выгружаются,
 * поэтому позитивный результат кэшируется навсегда; негативный не
 * кэшируется (ранний запуск, класс может появиться позже из JAR). */
static JavaClass* s_string_class_cache = NULL;

/* v34.91 MULTI-SESSION FIX (Switch frontend runs several games per process):
 * process-global caches hold pointers into a JVM's class table / heap.
 * jvm_destroy frees those — without this reset the NEXT session's
 * jvm_new_string_utf16 read a FREED stub class (ASan: heap-use-after-free
 * at jvm.c:975 <- init_javax_microedition_lcdui_form <- native_init).
 * Called from jvm_destroy — see also heap_class_cache_reset/native_object_cache_reset. */
void jvm_class_cache_reset(void) {
    s_string_class_cache = NULL;
    g_emergency_oom = NULL; /* v19 OOM singleton lives on the dying heap */
}

JavaString* jvm_new_string_utf16(JVM* jvm, const jchar* chars, jsize length) {
    if (!jvm || !chars || length < 0) return NULL;

    /* Проверить, загружен ли класс java/lang/String (v34.58: через кэш) */
    JavaClass* str_class = s_string_class_cache;
    if (!str_class) {
        for (size_t i = 0; i < jvm->class_loader.count; i++) {
            JavaClass* c = jvm->class_loader.classes[i];
            if (c->class_name && strcmp(c->class_name, "java/lang/String") == 0) {
                s_string_class_cache = c;
                str_class = c;
                break;
            }
        }
    }
    
    if (str_class) {
        /* Java String: создаем как OBJ_TYPE_OBJECT с полями value, offset, count, hash
         * КРИТИЧЕСКИ ВАЖНО: Используем OBJ_TYPE_OBJECT, так как у нас есть fields[]
         * OBJ_TYPE_STRING используется только для нативных строк без fields[]
         * 
         * ВАЖНО: Для Java строк мы НЕ можем использовать поля JavaString (length, hash, utf8, chars),
         * так как они перекрываются с fields[]! Мы должны использовать только fields[].
         */
        JavaObject* str = (JavaObject*)heap_alloc(jvm, str_class->instance_size, str_class, OBJ_TYPE_OBJECT);
        if (!str) return NULL;
        
        /* Создаем char[] массив для value field */
        JavaArray* char_array = heap_alloc_array(jvm, T_CHAR, length, NULL);
        if (!char_array) {
            return NULL;
        }
        
        /* Копируем данные в массив */
        jchar* array_data = (jchar*)((uint8_t*)char_array + sizeof(JavaArray));
        memcpy(array_data, chars, length * sizeof(jchar));
        
        /* Устанавливаем поля через функции из native.c */
        int value_slot = native_get_string_value_slot(jvm);
        int offset_slot = native_get_string_offset_slot(jvm);
        int count_slot = native_get_string_count_slot(jvm);
        int hash_slot = native_get_string_hash_slot(jvm);

        /* v36.09: a String object with unset value/count fields is a
         * landmine — string_length() would read recycled heap garbage
         * (bounds checks pass!) while string_chars() yields NULL, which is
         * exactly the silent-NULL substring that built Doom RPG [Rus]
         * "null.str". Trace it loudly; keep the legacy lenient behavior
         * (set fields when the slots resolve) because the slot cache can
         * legitimately report -1 before java/lang/String is fully prepared
         * (early-boot strings take the native-string path elsewhere), and
         * a hard NULL here kills the whole VM at boot. The real corruption
         * net is the native_string_result_guard in native.c. */
        if (value_slot < 0 || count_slot < 0) {
            static int badslot_n = 0;
            if (badslot_n++ < 8) {
                extern void sw_trace_force(const char* fmt, ...) __attribute__((weak));
                if (&sw_trace_force && sw_trace_force) {
                    sw_trace_force("[STR-BADSLOT] value=%d count=%d — String fields "
                                   "unresolved (early boot or stale cache)",
                                   value_slot, count_slot);
                }
            }
        }

        if (value_slot >= 0) {
            JavaValue v = { .ref = char_array };
            str->fields[value_slot] = v;
        }
        if (offset_slot >= 0) {
            JavaValue v = { .i = 0 };
            str->fields[offset_slot] = v;
        }
        if (count_slot >= 0) {
            JavaValue v = { .i = (jint)length };
            str->fields[count_slot] = v;
        }
        if (hash_slot >= 0) {
            JavaValue v = { .i = 0 };
            str->fields[hash_slot] = v;
        }
        
        /* НЕ устанавливаем str->length, str->chars и т.д. - они перекрываются с fields[]!
         * Используем только fields[] для доступа к данным Java строки.
         * string_length() и string_chars() знают как работать с OBJ_TYPE_OBJECT. */
        
        return (JavaString*)str;
    } else {
        /* Нативная строка (класс не загружен) */
        JavaString* str = (JavaString*)heap_alloc_string(jvm, length);
        if (!str) return NULL;
        
        /* Копируем данные inline после JavaString */
        jchar* str_chars = (jchar*)((uint8_t*)str + sizeof(JavaString));
        memcpy(str_chars, chars, length * sizeof(jchar));
        str->length = length;
        str->chars = NULL;  /* Native strings use inline storage */
        
        return str;
    }
}

/* Throw an exception */
void jvm_throw(JVM* jvm, JavaObject* exception) {
    if (!jvm || !exception) return;

    JavaThread* thread = jvm_current_thread(jvm);
    if (thread) {
        thread->pending_exception = exception;
    }

    /* v36.05 [EX-THROW]: every VM-visible exception throw into the Switch
     * trace file (sw_trace_force reaches log.txt; stderr does not). WHY:
     * the v36.04 field trace (Doom RPG [Rus], 2nd launch) died inside the
     * game's CATCH HANDLER (c.toString() NPE while the handler built its
     * log string) — the ORIGINAL exception was caught and therefore
     * invisible: no [EX-PROP] (that channel logs only uncaught
     * propagation), no [VM-THREAD-DEATH] for it. This channel fires at
     * the single choke point every throw passes through (athrow opcode,
     * jvm_throw_by_name, native helpers) and names class + message +
     * throw site. Throttled: the first 32 throws per process verbatim
     * (the interesting ones are always early), then one line per 10 s at
     * most, so exception-driven control flow cannot flood log.txt. */
    {
        static int ex_log_n = 0;
        static long long ex_log_last_ms = 0;
        int allow;
        if (ex_log_n < 32) {
            allow = 1;
        } else {
            /* v36.05: portable monotonic-ish throttle; second granularity
             * is fine for a 10 s rate limit (Win32 audit: no
             * clock_gettime/GetTickCount in this file). */
            time_t now_s = time(NULL);
            allow = (long long)now_s - ex_log_last_ms >= 10;
            if (allow) ex_log_last_ms = (long long)now_s;
        }
        if (allow) {
            const char* excls = (exception->header.clazz &&
                                 exception->header.clazz->class_name)
                                    ? exception->header.clazz->class_name : "?";
            char exmsg[80] = "";
            {
                extern JavaValue native_get_field_value(JavaObject* obj, const char* field_name);
                JavaString* m = (JavaString*)native_get_field_value(exception, "detailMessage").ref;
                if (m) {
                    extern const char* string_utf8(JVM* jvm, JavaString* str);
                    const char* u = string_utf8(jvm, m);
                    if (u) snprintf(exmsg, sizeof(exmsg), " \"%.50s\"", u);
                }
            }
            char exsite[96] = "";
            if (thread && thread->current_frame && thread->current_frame->clazz) {
                JavaFrame* fr = thread->current_frame;
                snprintf(exsite, sizeof(exsite), " at %s.%s PC=%d",
                         fr->clazz && fr->clazz->class_name ? fr->clazz->class_name : "?",
                         fr->method && fr->method->name ? fr->method->name : "?",
                         fr->throwing_pc);
            }
            char exline[256];
            snprintf(exline, sizeof(exline), "[EX-THROW] %s%s%s",
                     excls, exmsg, exsite);
            /* v36.35 DIAG: sandbox/host E2E has no sw_trace sink — mirror the
             * throw trace to stderr when NOJME_EX_TRACE is set, with a
             * deeper budget (500) than the Switch channel (32) and without
             * the 10 s throttle (loader diagnostics need every throw).
             * Costs one cached getenv. */
            {
                static int ex_trace_cache = -1;
                static int ex_trace_n = 0;
                if (ex_trace_cache < 0) {
                    const char* e = getenv("NOJME_EX_TRACE");
                    ex_trace_cache = (e && e[0] != '\0' && strcmp(e, "0") != 0) ? 1 : 0;
                }
                if (ex_trace_cache && ex_trace_n < 500) {
                    fprintf(stderr, "%s\n", exline);
                    ex_trace_n++;
                }
            }
            extern void sw_trace_force(const char* fmt, ...)
                __attribute__((weak));
            if (&sw_trace_force && sw_trace_force) {
                sw_trace_force("%s", exline);
            }
            if (ex_log_n < 32) ex_log_n++;
        }
    }
}

/* Throw exception by class name */
void jvm_throw_by_name(JVM* jvm, const char* class_name, const char* message) {
    if (!jvm || !class_name) return;
    
    JavaClass* clazz = jvm_load_class(jvm, class_name);
    if (!clazz) {
        ERROR_LOG("Exception class not found: %s", class_name);
        return;
    }
    
    /* v19 FIX (OutOfMemory stops execution): allocate the exception object
     * from the EMERGENCY reserve. The very condition that raises
     * OutOfMemoryError (heap full) also made the exception object allocation
     * itself fail, so jvm_throw_by_name returned WITHOUT setting a pending
     * exception; interpret() then treated the opcode failure as fatal and the
     * whole VM stopped. Per J2ME/JVM spec OutOfMemoryError is a normal
     * catchable Throwable, so exception construction must survive a full
     * heap. If even the reserve is exhausted, fall back to the pre-allocated
     * OutOfMemoryError singleton. */
    JavaObject* exception = jvm_new_object_emergency(jvm, clazz);
    if (!exception) {
        if (g_emergency_oom && strcmp(class_name, "java/lang/OutOfMemoryError") == 0) {
            LOG_SAFE("[JVM] throw_by_name: emergency reserve exhausted, "
                     "throwing pre-allocated OutOfMemoryError singleton\n");
            jvm_throw(jvm, g_emergency_oom);
        } else {
            ERROR_LOG("Failed to allocate exception object for %s "
                      "(heap full and no singleton)", class_name);
        }
        return;
    }
    
    /* v20 FIX (P1): deliver the detail message. The stub Throwable
     * constructor stores its argument in `detailMessage` and
     * Throwable.toString() / getMessage() read the same field — an
     * exception created here used to carry NO message at all. */
    if (message) {
        JavaString* msg = jvm_new_string(jvm, message);
        if (msg) {
            JavaValue mv = { .ref = msg };
            native_set_field_value(exception, "detailMessage", mv);
        }
    }
    
    jvm_throw(jvm, exception);
}

/* Check for pending exception */
JavaObject* jvm_exception_pending(JVM* jvm) {
    JavaThread* thread = jvm_current_thread(jvm);
    return thread ? thread->pending_exception : NULL;
}

/* Clear pending exception */
JavaObject* jvm_exception_clear(JVM* jvm) {
    JavaThread* thread = jvm_current_thread(jvm);
    if (!thread) return NULL;
    
    JavaObject* ex = thread->pending_exception;
    thread->pending_exception = NULL;
    return ex;
}

/* Get current thread */
JavaThread* jvm_current_thread(JVM* jvm) {
    if (!jvm) return NULL;
    /* Use the scheduler's current thread instead of always returning main_thread */
    return thread_current(jvm);
}

/* Allocate memory */
void* jvm_alloc(JVM* jvm, size_t size) {
    return heap_alloc(jvm, size, NULL, OBJ_TYPE_OBJECT);
}

/* Run garbage collection */
void jvm_gc(JVM* jvm) {
    gc_collect(jvm);
}

/* Add GC root.
 * v24 CRITICAL FIX (ASAN stack-use-after-return, Brick Breaker): the old
 * implementation stored the address of its own stack PARAMETER:
 *     gc_add_root(jvm, (void**)&obj);
 * After return the GC root pointed into a dead frame; gc_collect() then read
 * garbage from that slot and marked a random pointer as a live object
 * whenever a game called System.gc() after Player creation / Boolean
 * canonicalization. ASAN flagged exactly this on the Brick Breaker run.
 * Value-semantics roots now live in heap-allocated slots tracked in a side
 * table so jvm_remove_root(obj) can find and free the right one. */
typedef struct { JavaObject** slot; JavaObject* obj; } JvmPinnedRoot;
#define JVM_PINNED_ROOTS_MAX 256
static JvmPinnedRoot g_pinned_roots[JVM_PINNED_ROOTS_MAX];
static int g_pinned_root_count = 0;

void jvm_add_root(JVM* jvm, JavaObject* obj) {
    if (!obj) return;
    if (g_pinned_root_count >= JVM_PINNED_ROOTS_MAX) {
        ERROR_LOG("jvm_add_root: pinned-root table full (%d)", JVM_PINNED_ROOTS_MAX);
        return;
    }
    JavaObject** slot = (JavaObject**)malloc(sizeof(JavaObject*));
    if (!slot) return;
    *slot = obj;
    gc_add_root(jvm, (void**)slot);
    g_pinned_roots[g_pinned_root_count].slot = slot;
    g_pinned_roots[g_pinned_root_count].obj = obj;
    g_pinned_root_count++;
}

/* Remove GC root */
void jvm_remove_root(JVM* jvm, JavaObject* obj) {
    for (int i = 0; i < g_pinned_root_count; i++) {
        if (g_pinned_roots[i].obj == obj) {
            gc_remove_root(jvm, (void**)g_pinned_roots[i].slot);
            free(g_pinned_roots[i].slot);
            g_pinned_roots[i] = g_pinned_roots[--g_pinned_root_count];
            return;
        }
    }
}

/* v36.12 (malloc-census find): jvm_add_root mallocs the root SLOT and only
 * jvm_remove_root frees it — roots pinned by Boolean TRUE/FALSE stubs and
 * the Display shell were never removed at session end (6 blocks/session).
 * gc_roots_reset_all (jvm_destroy) wipes the GC's root registry but not
 * these slots; free them here. Call from jvm_destroy AFTER gc_roots_reset_all. */
void jvm_pinned_roots_reset(void) {
    for (int i = 0; i < g_pinned_root_count; i++) {
        free(g_pinned_roots[i].slot);
    }
    g_pinned_root_count = 0;
}

/* Convert string to UTF-8 */
char* jvm_string_to_utf8(JVM* jvm, JavaString* str) {
    (void)jvm;
    if (!str) return NULL;
    
    /* BUG FIX: For Java String objects (OBJ_TYPE_OBJECT with clazz != NULL),
     * the str->utf8 field overlaps with fields[] (value field = char[] pointer).
     * Accessing str->utf8 directly would dereference a JavaArray* as char*,
     * causing a segfault or memory corruption.
     * Use string_utf8() from heap.c instead for proper handling. */
    if (str->header.clazz == NULL && str->utf8) {
        /* Native string (no java/lang/String class) - safe to use struct fields */
        return strdup(str->utf8);
    }
    
    /* For Java String objects, use the proper string_utf8 function */
    extern const char* string_utf8(JVM* jvm, JavaString* str);
    const char* utf8 = string_utf8(jvm, str);
    if (utf8) {
        return strdup(utf8);
    }
    
    /* Fallback: try to get chars from inline storage (native strings only) */
    if (str->header.clazz == NULL) {
        const jchar* chars = (const jchar*)((uint8_t*)str + sizeof(JavaString));
        jsize len = str->length;
        
        /* Compute UTF-8 byte count */
        size_t utf8_len = 0;
        for (jsize i = 0; i < len; i++) {
            jchar c = chars[i];
            if (c < 0x80) utf8_len += 1;
            else if (c < 0x800) utf8_len += 2;
            else utf8_len += 3;
        }
        
        char* result = (char*)malloc(utf8_len + 1);
        if (!result) return NULL;
        
        size_t j = 0;
        for (jsize i = 0; i < len; i++) {
            jchar c = chars[i];
            if (c < 0x80) {
                result[j++] = (char)c;
            } else if (c < 0x800) {
                result[j++] = (char)(0xC0 | (c >> 6));
                result[j++] = (char)(0x80 | (c & 0x3F));
            } else {
                result[j++] = (char)(0xE0 | (c >> 12));
                result[j++] = (char)(0x80 | ((c >> 6) & 0x3F));
                result[j++] = (char)(0x80 | (c & 0x3F));
            }
        }
        result[utf8_len] = '\0';
        
        return result;
    }
    
    return NULL;
}

/* Utility functions */
uint16_t jvm_read_u16(const uint8_t* data) {
    return ((uint16_t)data[0] << 8) | data[1];
}

uint32_t jvm_read_u32(const uint8_t* data) {
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) | data[3];
}

uint64_t jvm_read_u64(const uint8_t* data) {
    return ((uint64_t)data[0] << 56) | ((uint64_t)data[1] << 48) |
           ((uint64_t)data[2] << 40) | ((uint64_t)data[3] << 32) |
           ((uint32_t)((uint64_t)data[4]) <<  24) | ((uint64_t)data[5] << 16) |
           ((uint64_t)data[6] << 8) | data[7];
}

int16_t jvm_read_s16(const uint8_t* data) {
    return (int16_t)jvm_read_u16(data);
}

int32_t jvm_read_s32(const uint8_t* data) {
    return (int32_t)jvm_read_u32(data);
}

int64_t jvm_read_s64(const uint8_t* data) {
    return (int64_t)jvm_read_u64(data);
}

/* Class name conversion */
char* class_name_to_java(const char* internal_name) {
    if (!internal_name) return NULL;
    
    size_t len = strlen(internal_name);
    char* result = (char*)malloc(len + 1);
    if (!result) return NULL;
    
    for (size_t i = 0; i < len; i++) {
        result[i] = (internal_name[i] == '/') ? '.' : internal_name[i];
    }
    result[len] = '\0';
    
    return result;
}

char* class_name_to_internal(const char* java_name) {
    if (!java_name) return NULL;
    
    size_t len = strlen(java_name);
    char* result = (char*)malloc(len + 1);
    if (!result) return NULL;
    
    for (size_t i = 0; i < len; i++) {
        result[i] = (java_name[i] == '.') ? '/' : java_name[i];
    }
    result[len] = '\0';
    
    return result;
}
