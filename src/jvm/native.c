/*
 * J2ME Emulator - Native Methods
 */

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L  /* For strdup, clock_gettime, pthread - only on POSIX */
#define _DEFAULT_SOURCE   /* For usleep */
#endif

#include <stdio.h>
#include "debug.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <errno.h>
#include <stdint.h>  /* For uint32_t, uint8_t */
#include <inttypes.h>  /* For PRIu64 portable format macros */
#include "utils/battery.h" /* [BATFIX] v36.41: заряд для системных свойств */

#ifdef _WIN32
#include <windows.h>
/* v34.1: the timers-pump mutex in jvm_process_timers() needs pthread_mutex_t
 * on Windows too. Plain MinGW targets have no <pthread.h> — use the project's
 * Win32 shim (same pattern as media.c / sdl_backend_stubs.c). It provides
 * pthread_mutex_t + PTHREAD_MUTEX_INITIALIZER (lazy-init CRITICAL_SECTION) +
 * trylock/unlock. NOTE: <time.h> is already included above, satisfying the
 * shim's ordering requirement for its clock_gettime() override. */
#include "win_thread_shim.h"
#else
#include <pthread.h>
#include <unistd.h>
#ifdef __SWITCH__
/* v34.86 FIX (user build report: "fatal error: sys/syscall.h: No such file
 * or directory" from devkitA64): devkitPro's newlib + libnx have NO
 * <sys/syscall.h> (a glibc-only header), no syscall(2) and no SYS_gettid;
 * <sys/resource.h> exists but declares only getrusage (no setpriority, no
 * PRIO_PROCESS). The per-thread nice offset below is an NPTL/Linux concept —
 * on Switch, thread priorities belong to libnx (svcSetThreadPriority), so
 * the whole nice block is compiled out. pthread.h/unistd.h themselves are
 * fine: newlib's pthread layer is implemented by libnx (newlib.c:
 * __syscall_thread_create/join/cond/lock), verified against the devkitPro
 * newlib fork (branch devkitPro) and libnx master. */
#else
#include <sys/syscall.h>   /* v34.43: SYS_gettid for per-thread setpriority */
#include <sys/resource.h>   /* v34.43: setpriority for game-thread nice offset */
#endif
#endif

#include "native.h"
#include "jvm.h"
#include "heap.h"
#include "charset.h"   /* v34.44: String(byte[], charset) / getBytes(charset) */

/* v34.44 forward declarations — the shared charset helpers live next to the
 * String methods (~line 6300) but are also used by the stream classes
 * (ByteArrayOutputStream.toString, ~line 2140). */
static JCharsetId string_default_charset(void);
static int string_construct_from_bytes(JVM* jvm, JavaObject* str,
                                       JavaArray* byte_array,
                                       jint offset, jint count,
                                       JCharsetId cs);
#include "threads.h"
#include "opcodes.h"
#include "classfile.h"
#include "sdl_backend.h"
#include "midp.h"  /* For load_jar_resource */

/* String field indices for JavaObject-based strings - must match heap.c */
#define STRING_FIELD_VALUE  0   /* char[] array */
#define STRING_FIELD_OFF   1   /* int offset */
#define STRING_FIELD_COUNT  2   /* int length */
#define STRING_FIELD_HASH   3   /* int hash */

/* Forward declarations */
void init_java_util_random(JVM* jvm);
void init_java_util_timer(JVM* jvm);
void init_java_lang_stringbuffer(JVM* jvm);
void init_java_lang_stringbuilder(JVM* jvm);
void init_java_util_vector(JVM* jvm);
void init_java_util_stack(JVM* jvm);
/* v20 batch B forward declarations */
static JavaValue native_string_lastIndexOf_string(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count);
static JavaValue native_string_lastIndexOf_string_from(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count);
static JavaValue native_printstream_println_long(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count);
static JavaValue native_printstream_println_char(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count);
static JavaValue native_printstream_println_float(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count);
static JavaValue native_printstream_println_double(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count);
static JavaValue native_printstream_println_object(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count);
static JavaValue native_printstream_print_long(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count);
static JavaValue native_printstream_print_char(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count);
static JavaValue native_printstream_print_float(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count);
static JavaValue native_printstream_print_double(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count);
static JavaValue native_printstream_print_object(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count);
static JavaValue native_timezone_getID(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count);
static JavaValue native_timezone_getRawOffset(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count);
static JavaValue native_timezone_getOffset7(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count);
static JavaValue native_timezone_useDaylightTime(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count);
static JavaValue native_boolean_toString(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count);
static JavaValue native_integer_toString_instance(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count);
static JavaValue native_byte_byteValue(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count);
static JavaValue native_short_shortValue(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count);
static JavaValue native_character_charValue(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count);
static JavaString* util_to_string_of(JVM* jvm, JavaThread* thread, JavaObject* obj);
static int char_is_unicode_letter(jint c);
/* v20 batch A forward declarations (used by earlier registration tables) */
static JavaValue native_system_exit(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count);
static JavaValue native_runtime_exit(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count);
static JavaValue native_system_identityHashCode(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count);
static JavaValue native_vector_firstElement_spec(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count);
static JavaValue native_vector_lastElement_spec(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count);
static JavaValue native_vector_capacity(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count);
static JavaValue native_vector_toString(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count);
static JavaValue native_thread_sleep_2(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count);
static JavaValue native_thread_join_millis(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count);
static JavaValue native_thread_join_millis_nanos(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count);
static JavaValue native_stack_push(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count);
static JavaValue native_stack_pop(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count);
static JavaValue native_stack_peek(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count);
static JavaValue native_stack_empty(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count);
static JavaValue native_stack_search(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count);
void init_java_util_stack(JVM* jvm);
void init_java_util_enumeration(JVM* jvm);
void init_java_util_hashtable(JVM* jvm);
void init_java_io_bytearrayinputstream(JVM* jvm);
void init_java_io_bytearrayoutputstream(JVM* jvm);
void init_java_io_printstream(JVM* jvm);
void init_java_io_dataoutputstream(JVM* jvm);
void init_java_io_datainputstream(JVM* jvm);
void init_java_io_inputstreamreader(JVM* jvm);
void init_java_io_bufferedreader(JVM* jvm);
void init_nokia_sound(JVM* jvm);
void init_nokia_ui(JVM* jvm);
void init_nokia_direct_graphics(JVM* jvm);
void init_nokia_m3d_impl(JVM* jvm);
void init_nokia_misc(JVM* jvm);
void init_javax_microedition_midlet_MIDlet(JVM* jvm);
void init_javax_microedition_rms(JVM* jvm);
void init_javax_microedition_lcdui_graphics(JVM* jvm);
void init_javax_microedition_lcdui_image(JVM* jvm);
void init_javax_microedition_lcdui_display(JVM* jvm);
void init_javax_microedition_lcdui_form(JVM* jvm);
void init_javax_microedition_media(JVM* jvm);
void init_java_lang_integer(JVM* jvm);
void init_java_lang_long(JVM* jvm);
void init_java_lang_boolean(JVM* jvm);
void init_javax_microedition_io(JVM* jvm);
void init_java_util_calendar(JVM* jvm);
void init_java_util_date(JVM* jvm);
/* v36.33 [SAX-XML]: javax.xml.parsers + org.xml.sax (implemented in sax.c) */
void init_javax_xml_sax(JVM* jvm);
void init_java_util_timezone(JVM* jvm);
void init_java_lang_character(JVM* jvm);
void init_java_lang_byte_short(JVM* jvm);
void init_java_lang_float_double(JVM* jvm);
void init_javax_microedition_media_manager(JVM* jvm);
void init_vendor_extensions(JVM* jvm);
void init_javax_microedition_m3g(JVM* jvm);  /* JSR 184 Mobile 3D Graphics */
void init_nojme_battery(JVM* jvm); /* [BATFIX] v36.41: nojme.device.Battery */
void init_com_mascotcapsule_micro3d_v3(JVM* jvm);  /* MascotCapsule micro3d v3 */

/* Native method registry.
 * v14: 1024 OVERFLOWED — the MIDP/graphics/media tables already consume
 * most of it, so the TAIL of the M3G table (KeyframeSequence accessors,
 * Object3D.animate, addAnimationTrack, ...) was silently dropped
 * (native_register returned JNI_ERR without any log), killing retained-mode
 * animation in every JSR-184 game. Raise the cap and surface overflows. */
#define MAX_NATIVE_METHODS 4096

static struct {
    NativeMethodEntry entries[MAX_NATIVE_METHODS];
    int count;
} native_registry;

/* Native method hash table for O(1) lookup */
#define NATIVE_HASH_SIZE 1024  /* Must be power of 2 */
#define NATIVE_HASH_MASK (NATIVE_HASH_SIZE - 1)

typedef struct NativeHashEntry {
    const char* class_name;
    const char* method_name;
    const char* descriptor;
    NativeMethod handler;
    struct NativeHashEntry* next;  /* For chaining */
} NativeHashEntry;

static NativeHashEntry* native_hash_table[NATIVE_HASH_SIZE];
static int native_hash_initialized = 0;

static uint32_t native_method_hash(const char* class_name, const char* method_name, const char* descriptor) {
    uint32_t h = 2166136261u;
    while (*class_name) { h ^= (uint8_t)*class_name++; h *= 16777619u; }
    h ^= (uint8_t)'/';
    while (*method_name) { h ^= (uint8_t)*method_name++; h *= 16777619u; }
    h ^= (uint8_t)'(';
    while (*descriptor) { h ^= (uint8_t)*descriptor++; h *= 16777619u; }
    return h;
}

static void native_hash_free(void) {
    for (int i = 0; i < NATIVE_HASH_SIZE; i++) {
        NativeHashEntry* he = native_hash_table[i];
        while (he) {
            NativeHashEntry* nx = he->next;
            free(he);
            he = nx;
        }
        native_hash_table[i] = NULL;
    }
    native_hash_initialized = 0;
}

static void native_hash_build(void) {
    /* v36.12 (malloc-census find): native_init runs on EVERY session
     * (jvm_create), resets native_registry.count and re-registers the same
     * ~1670 methods, then rebuilt this table over the OLD one — memset
     * without free leaked one NativeHashEntry per method per session
     * (measured: 1676 blocks / ~67 KB per open/close cycle, the dominant
     * per-session leak of the whole frontend). Free the previous table
     * first: the entries are pure malloc'd nodes over static strings. */
    native_hash_free();
    for (int i = 0; i < native_registry.count; i++) {
        NativeMethodEntry* e = &native_registry.entries[i];
        uint32_t idx = native_method_hash(e->class_name, e->method_name, e->descriptor) & NATIVE_HASH_MASK;
        NativeHashEntry* he = (NativeHashEntry*)malloc(sizeof(NativeHashEntry));
        he->class_name = e->class_name;
        he->method_name = e->method_name;
        he->descriptor = e->descriptor;
        he->handler = e->handler;
        he->next = native_hash_table[idx];
        native_hash_table[idx] = he;
    }
    native_hash_initialized = 1;
}

/* Forward declaration for execute_method (used by Class.newInstance) */
extern int execute_method(JVM* jvm, JavaThread* thread, JavaMethod* method, 
                          JavaValue* args, JavaValue* result);

/* Initialize native methods */
int native_init(JVM* jvm) {
    if (g_j2me_runtime_debug) {
        fprintf(stderr, "### NATIVE_INIT CALLED ###\n");
        fflush(stderr);
    }
    
    /* Initialize DRM bypass system (like KEmulator) */
    /* v38: [V37-DIAG] native_init breadcrumb removed (quiet-by-default). */
    extern void drm_bypass_init(void);
    drm_bypass_init();
    
    native_registry.count = 0;
    
    /* Initialize standard Java native methods */
    init_java_lang_object(jvm);
    init_java_lang_class(jvm);
    init_java_lang_string(jvm);
    init_java_lang_system(jvm);
    init_java_lang_runtime(jvm);
    init_java_lang_math(jvm);
    init_java_lang_thread(jvm);
    init_java_lang_throwable(jvm);
    
    /* Initialize MIDlet lifecycle */
    init_javax_microedition_midlet_MIDlet(jvm);

    /* [BATFIX] v36.41: nojme.device.Battery — класс-API заряда */
    init_nojme_battery(jvm);
    
    /* Initialize StringBuffer and StringBuilder */
    init_java_lang_stringbuffer(jvm);
    init_java_lang_stringbuilder(jvm);
    
    /* Initialize java.util collection classes */
    init_java_util_random(jvm);
    init_java_util_vector(jvm);
    init_java_util_stack(jvm);
    init_java_util_enumeration(jvm);
    init_java_util_hashtable(jvm);
    
    /* Initialize java.util.Timer */
    init_java_util_timer(jvm);
    
    /* Initialize ByteArrayInputStream */
    init_java_io_bytearrayinputstream(jvm);
    
    /* Initialize ByteArrayOutputStream */
    init_java_io_bytearrayoutputstream(jvm);
    
    /* Initialize PrintStream (System.out/err) */
    init_java_io_printstream(jvm);
    
    /* Initialize DataOutputStream */
    init_java_io_dataoutputstream(jvm);
    
    /* Initialize DataInputStream */
    init_java_io_datainputstream(jvm);
    
    /* Initialize InputStreamReader and BufferedReader */
    init_java_io_inputstreamreader(jvm);
    init_java_io_bufferedreader(jvm);
    
    /* Initialize Nokia Sound */
    init_nokia_sound(jvm);
    
    /* Initialize Nokia UI API */
    init_nokia_ui(jvm);
    init_nokia_direct_graphics(jvm);
    
    /* Initialize Nokia M3D software 3D renderer */
    init_nokia_m3d_impl(jvm);

    /* Initialize Nokia misc APIs */
    init_nokia_misc(jvm);
    
    /* Initialize RMS (RecordStore) */
    init_javax_microedition_rms(jvm);
    
    /* Initialize LCDUI */
    init_javax_microedition_lcdui_graphics(jvm);
    init_javax_microedition_lcdui_image(jvm);
    init_javax_microedition_lcdui_display(jvm);
    init_javax_microedition_lcdui_form(jvm);
    
    /* Initialize Font (Sprite/GameCanvas/LayerManager/TiledLayer) */
    init_javax_microedition_lcdui_font(jvm);
    init_javax_microedition_lcdui_game_gamecanvas(jvm);
    
    /* Initialize Media - REAL implementations in media.c */
    init_javax_microedition_media(jvm);
    
    /* NOTE: init_javax_microedition_media_manager() was REMOVED here because it
     * registered empty stubs that overrode the real implementations from media.c.
     * media.c has working createPlayer, start, stop, close, playTone, etc. */
    
    /* Initialize wrapper classes */
    init_java_lang_integer(jvm);
    init_java_lang_long(jvm);
    init_java_lang_boolean(jvm);
    init_java_lang_character(jvm);
    init_java_lang_byte_short(jvm);
    init_java_lang_float_double(jvm);
    
    /* Initialize J2ME IO (GCF) */
    init_javax_microedition_io(jvm);
    
    /* Initialize Calendar, TimeZone and Date */
    init_java_util_timezone(jvm);
    init_java_util_calendar(jvm);
    init_java_util_date(jvm);
    /* v36.33 [SAX-XML]: Speedx 3D & co parse font descriptors via SAX */
    init_javax_xml_sax(jvm);
    
    /* Media Manager stubs removed - real implementations are in media.c
     * init_javax_microedition_media_manager(jvm) was registering empty stubs
     * that overrode the real createPlayer/start/stop/close implementations. */
    
    /* Initialize vendor-specific extensions */
    init_vendor_extensions(jvm);
    
    /* Initialize JSR 184 Mobile 3D Graphics API */
    init_javax_microedition_m3g(jvm);

    /* Initialize MascotCapsule micro3d v3 API (Duke Nukem 3D etc.) */
    init_com_mascotcapsule_micro3d_v3(jvm);
    
    /* Build hash table for O(1) native method lookup */
    native_hash_build();
    
    return JNI_OK;
}

/* Register a native method */
int native_register(JVM* jvm, const char* class_name, const char* method_name,
                    const char* descriptor, NativeMethod handler) {
    (void)jvm;
    
    if (native_registry.count >= MAX_NATIVE_METHODS) {
        fprintf(stderr, "[NATIVE-REG] ERROR: registry full (%d), dropping %s.%s%s\n",
                MAX_NATIVE_METHODS, class_name ? class_name : "?",
                method_name ? method_name : "?", descriptor ? descriptor : "?");
        return JNI_ERR;
    }
    
    NativeMethodEntry* entry = &native_registry.entries[native_registry.count++];
    entry->class_name = class_name;
    entry->method_name = method_name;
    entry->descriptor = descriptor;
    entry->handler = handler;
    
    return JNI_OK;
}

/* Register multiple native methods */
int native_register_methods(JVM* jvm, const NativeMethodEntry* methods, int count) {
    for (int i = 0; i < count; i++) {
        if (native_register(jvm, methods[i].class_name, methods[i].method_name,
                           methods[i].descriptor, methods[i].handler) != JNI_OK) {
            return JNI_ERR;
        }
    }
    return JNI_OK;
}

/* Find native method - ИСПРАВЛЕНО: ищем в родительских классах, КРОМЕ конструкторов */
/* v37 DIAG: NOJME_INVOKE_TRACE=<prefix> — counts native dispatches per
 * class.method whose class starts with <prefix>, dumped every 200k lookups
 * and at exit. Used to identify the live render pipeline (M3G vs software). */
#define V37_TRACE_MAX 512
static struct { char name[96]; uint32_t count; } v37_trace_tab[V37_TRACE_MAX];
static int v37_trace_n = 0;
static uint64_t v37_trace_total = 0;
static int v37_trace_on = -1;
static char v37_trace_prefix[64] = "";
static void v37_trace_dump(void) {
    j2me_log_ungated("[V37-TRACE] native dispatch counts (total lookups %llu):\n",
            (unsigned long long)v37_trace_total);
    for (int i = 0; i < v37_trace_n; i++) {
        j2me_log_ungated("[V37-TRACE]   %8u  %s\n", v37_trace_tab[i].count, v37_trace_tab[i].name);
    }
}
void v37_note_dispatch(const char* class_name, const char* method_name);
static void v37_trace_tick(const char* class_name, const char* method_name) {
    v37_note_dispatch(class_name, method_name);
}
void v37_note_dispatch(const char* class_name, const char* method_name) {
    if (v37_trace_on < 0) {
        const char* e = getenv("NOJME_INVOKE_TRACE");
        v37_trace_on = (e && e[0]) ? 1 : 0;
        if (v37_trace_on) {
            snprintf(v37_trace_prefix, sizeof(v37_trace_prefix), "%s", e);
            atexit(v37_trace_dump);
            j2me_log_ungated("[V37-TRACE] active, prefix='%s'\n", v37_trace_prefix);
        }
    }
    if (!v37_trace_on) return;
    v37_trace_total++;
    if (strncmp(class_name, v37_trace_prefix, strlen(v37_trace_prefix)) != 0) return;
    char full[96];
    snprintf(full, sizeof(full), "%s.%s", class_name, method_name);
    for (int i = 0; i < v37_trace_n; i++) {
        if (strcmp(v37_trace_tab[i].name, full) == 0) {
            v37_trace_tab[i].count++;
            if ((v37_trace_tab[i].count % 200000) == 0) {
                j2me_log_ungated("[V37-TRACE] %s hit %u (total %llu)\n", full,
                        v37_trace_tab[i].count, (unsigned long long)v37_trace_total);
            }
            return;
        }
    }
    if (v37_trace_n < V37_TRACE_MAX) {
        snprintf(v37_trace_tab[v37_trace_n].name, sizeof(v37_trace_tab[0].name), "%s", full);
        v37_trace_tab[v37_trace_n].count = 1;
        v37_trace_n++;
    }
}
NativeMethod native_find(JVM* jvm, const char* class_name, const char* method_name,
                         const char* descriptor) {
    if (class_name && method_name) v37_trace_tick(class_name, method_name);
    /* Fast path: hash table lookup for exact match */
    if (native_hash_initialized) {
        uint32_t idx = native_method_hash(class_name, method_name, descriptor) & NATIVE_HASH_MASK;
        for (NativeHashEntry* he = native_hash_table[idx]; he; he = he->next) {
            if (strcmp(he->class_name, class_name) == 0 &&
                strcmp(he->method_name, method_name) == 0 &&
                strcmp(he->descriptor, descriptor) == 0) {
                return he->handler;
            }
        }
    } else {
        /* Fallback: linear scan (before hash table is built) */
        for (int i = 0; i < native_registry.count; i++) {
            NativeMethodEntry* entry = &native_registry.entries[i];
            if (strcmp(entry->class_name, class_name) == 0 &&
                strcmp(entry->method_name, method_name) == 0 &&
                strcmp(entry->descriptor, descriptor) == 0) {
                return entry->handler;
            }
        }
    }
    
    /* ВАЖНО: Для конструкторов (<init>) и статических инициализаторов (<clinit>) 
     * НЕ ищем в родительских классах!
     * Каждый класс имеет свой конструктор/инициализатор, который должен выполняться.
     * Поиск в суперклассах нужен только для обычных методов (например, notifyDestroyed).
     */
    if (method_name && (strcmp(method_name, "<init>") == 0 || strcmp(method_name, "<clinit>") == 0)) {
        return NULL;
    }
    
    /* ИСПРАВЛЕНО: Если не нашли, ищем в родительских классах */
    /* Ищем класс в загруженных классах JVM */
    if (jvm) {
        JavaClass* clazz = NULL;
        
        /* Search in loaded classes */
        for (int i = 0; i < (int)jvm->class_loader.count; i++) {
            JavaClass* c = jvm->class_loader.classes[i];
            if (c && c->class_name && strcmp(c->class_name, class_name) == 0) {
                clazz = c;
                break;
            }
        }
        
        /* Walk up the inheritance chain */
        while (clazz && clazz->super_class) {
            clazz = clazz->super_class;
            if (clazz->class_name) {
                if (native_hash_initialized) {
                    uint32_t idx = native_method_hash(clazz->class_name, method_name, descriptor) & NATIVE_HASH_MASK;
                    for (NativeHashEntry* he = native_hash_table[idx]; he; he = he->next) {
                        if (strcmp(he->class_name, clazz->class_name) == 0 &&
                            strcmp(he->method_name, method_name) == 0 &&
                            strcmp(he->descriptor, descriptor) == 0) {
                            return he->handler;
                        }
                    }
                } else {
                    for (int i = 0; i < native_registry.count; i++) {
                        NativeMethodEntry* entry = &native_registry.entries[i];
                        if (strcmp(entry->class_name, clazz->class_name) == 0 &&
                            strcmp(entry->method_name, method_name) == 0 &&
                            strcmp(entry->descriptor, descriptor) == 0) {
                            return entry->handler;
                        }
                    }
                }
            }
        }
    }

    return NULL;
}

/* v34.76: descriptor-relaxed native lookup (legacy-game compatibility).
 *
 * Roboros (1.0.2, mob.ua 2006) was compiled against a non-standard API
 * stub where LayerManager.append(Layer) is declared VOID, while JSR-118
 * declares it as returning int. Our resolver matches name+descriptor, so
 * the call fell to the [INVOKE-MISSING] no-op stub: layers were never
 * added to the LayerManager and the whole game world (TiledLayer + 14
 * sprites) stayed invisible - "HUD on black screen". The same class of
 * mismatch already bit us as Doom RPG's 7-vs-8-arg drawRGB (v34.6).
 *
 * This fallback tolerates RETURN-TYPE-ONLY differences: the argument
 * section of the two descriptors must be byte-identical (so the caller's
 * stack layout and the native's args[] indexing agree exactly), only the
 * return type may differ. The invoke opcodes push the result according
 * to the CALLER's descriptor, so a native registered as ")I" invoked
 * through a ")V" caller simply has its result discarded - exactly the
 * Java semantics of ignoring a return value. Argument-count mismatches
 * are deliberately NOT tolerated.
 *
 * Precedence: callers must run the exact native_find() chain FIRST and
 * only fall back here on a full miss (an exact match in a superclass
 * always wins over a relaxed match in a subclass).
 */
static int native_desc_args_equal(const char* a, const char* b) {
    if (!a || !b) return 0;
    /* Compare the "(...)" argument sections byte-for-byte, up to ')'. */
    while (*a && *b && *a != ')' && *b != ')') {
        if (*a != *b) return 0;
        a++; b++;
    }
    /* Both must have arrived at the ')' at the same position. */
    return *a == ')' && *b == ')';
}

NativeMethod native_find_relaxed(JVM* jvm, const char* class_name,
                                 const char* method_name, const char* descriptor) {
    (void)jvm;  /* the registry is global; jvm kept for signature symmetry */
    if (!class_name || !method_name || !descriptor) return NULL;
    /* Constructors / static initializers are never relaxed: each class
     * owns its own <init>/<clinit> and the arg layout must be exact. */
    if (method_name[0] == '<') return NULL;
    if (descriptor[0] != '(') return NULL;

    for (int i = 0; i < native_registry.count; i++) {
        NativeMethodEntry* entry = &native_registry.entries[i];
        if (entry->class_name && entry->method_name && entry->descriptor &&
            strcmp(entry->class_name, class_name) == 0 &&
            strcmp(entry->method_name, method_name) == 0 &&
            native_desc_args_equal(entry->descriptor, descriptor)) {
            static int relax_log_count = 0;
            if (relax_log_count < 40) {
                relax_log_count++;
                j2me_log_ungated(
                    "[INVOKE-RELAX] %s.%s%s -> matched registered %s.%s%s (return-type mismatch tolerated)\n",
                    class_name, method_name, descriptor,
                    entry->class_name, entry->method_name, entry->descriptor);
            }
            return entry->handler;
        }
    }
    return NULL;
}

/* Utility functions */
const char* native_get_string_utf8(JVM* jvm, JavaValue* args, int index) {
    JavaString* str = (JavaString*)args[index].ref;
    if (!str) return NULL;
    return string_utf8(jvm, str);
}

/* Exception helpers */
void native_throw_npe(JVM* jvm, JavaThread* thread) {
    static int npe_log_count = 0;
    if (npe_log_count < 5) {
        JavaFrame* frame = thread && thread->current_frame ? thread->current_frame : NULL;
        if (frame && frame->method && frame->clazz) {
            int pc = (int)frame->pc;
            fprintf(stderr, "[NPE] %s.%s at PC=%d last_native=%s\n",
                    frame->clazz->class_name ? frame->clazz->class_name : "?",
                    frame->method->name ? frame->method->name : "?", pc,
                    thread && thread->last_native[0] ? thread->last_native : "(none)");
            /* v34.17 DIAG: dump the full frame chain once so the exact
             * caller path of the NPE is visible (Saboteur 3D texture blit) */
            if (npe_log_count == 0 && thread) {
                JavaFrame* f = frame;
                int depth = 0;
                fprintf(stderr, "[NPE-CHAIN] ");
                while (f && depth < 12) {
                    fprintf(stderr, "%s.%s%s@%d ",
                            f->clazz && f->clazz->class_name ? f->clazz->class_name : "?",
                            f->method && f->method->name ? f->method->name : "?",
                            f->method && f->method->descriptor ? f->method->descriptor : "",
                            f->pc);
                    f = f->prev; depth++;
                }
                fprintf(stderr, "\n");
            }
        }
        npe_log_count++;
    }
    jvm_throw_by_name(jvm, "java/lang/NullPointerException", NULL);
    thread->pending_exception = jvm_exception_pending(jvm);
}

void native_throw_aioobe(JVM* jvm, JavaThread* thread, jint index) {
    char msg[64];
    snprintf(msg, sizeof(msg), "Array index out of range: %d", index);
    jvm_throw_by_name(jvm, "java/lang/ArrayIndexOutOfBoundsException", msg);
    thread->pending_exception = jvm_exception_pending(jvm);
}

/* FIX-19c: String/StringBuffer methods must throw the SPEC exception
 * java.lang.StringIndexOutOfBoundsException (a RuntimeException distinct
 * from ArrayIndexOutOfBoundsException). Games and tests do
 * 'catch (StringIndexOutOfBoundsException e)' — throwing AIOOBE there
 * escapes the handler and aborts whole threads/midlets. */
void native_throw_sioobe(JVM* jvm, JavaThread* thread, jint index) {
    /* v34.3 DIAG: report (throttled) who hit the bad index, so a game log
     * pinpoints the exact call site - e.g. a game parsing text data with
     * substring()/charAt() on a shorter string than expected. The frame
     * below the native is the Java caller of the failing String method. */
    static int sioobe_diag_count = 0;
    if (sioobe_diag_count < 5) {
        JavaThread* cur = thread ? thread : jvm_current_thread(jvm);
        JavaFrame* f = cur ? cur->current_frame : NULL;
        if (f && f->clazz && f->clazz->class_name) {
            /* v34.4: native entry runs AFTER the invoke opcode advanced f->pc
             * past the instruction (invokevirtual = 3 bytes). EX-PROP later
             * reports frame->throwing_pc (the invoke START). Print the same
             * value so [SIOOBE] and [EX-PROP] lines match in game logs
             * (user log showed PC=36 vs PC=33 for one and the same throw). */
            int invoke_pc = (int)f->pc - 3;
            if (invoke_pc < 0) invoke_pc = (int)f->pc;
            fprintf(stderr, "[SIOOBE] index=%d at %s.%s PC=%d\n",
                    index,
                    f->clazz->class_name,
                    (f->method && f->method->name) ? f->method->name : "?",
                    invoke_pc);
            sioobe_diag_count++;
        }
    }
    char msg[64];
    snprintf(msg, sizeof(msg), "String index out of range: %d", index);
    jvm_throw_by_name(jvm, "java/lang/StringIndexOutOfBoundsException", msg);
    if (thread) thread->pending_exception = jvm_exception_pending(jvm);
}

void native_throw_cnfe(JVM* jvm, JavaThread* thread, const char* name) {
    jvm_throw_by_name(jvm, "java/lang/ClassNotFoundException", name);
    thread->pending_exception = jvm_exception_pending(jvm);
}

/* =====================================================================
 * FIX-19g: Java-compatible float/double formatting and parsing.
 *
 * Old code used C "%g" everywhere: it prints "0" for 0.0 (Java: "0.0"),
 * "1e+10" for 1e10f (Java: "1.0E10"), and only 6 significant digits, so
 * Float.toString(Float.MAX_VALUE) = "3.40282e+38" did not round-trip
 * back through parseFloat (game save/config formats broke silently).
 *
 * java_format_double reproduces Double.toString(double) per JLS:
 *   - NaN / Infinity / -Infinity / -0.0 special forms
 *   - plain decimal form when 1e-3 <= |m| < 1e7 ("123.456", "0.001")
 *   - computerized scientific form otherwise ("1.0E10", "3.4028235E38")
 *   - always at least one digit after '.'
 *   - shortest digit string that round-trips to the same value
 * java_format_float is the float-space variant (Float.toString).
 * ===================================================================== */

static void java_format_double(char* out, size_t outsz, double v) {
    if (isnan(v)) { snprintf(out, outsz, "NaN"); return; }
    if (v == INFINITY)  { snprintf(out, outsz, "Infinity"); return; }
    if (v == -INFINITY) { snprintf(out, outsz, "-Infinity"); return; }
    if (v == 0.0) { snprintf(out, outsz, signbit(v) ? "-0.0" : "0.0"); return; }

    double a = fabs(v);
    char mbuf[48];
    int prec = 17;
    for (int p = 1; p <= 17; p++) {
        snprintf(mbuf, sizeof(mbuf), "%.*e", p - 1, a);
        if (strtod(mbuf, NULL) == a) { prec = p; break; }
    }
    if (prec > 17) prec = 17;
    snprintf(mbuf, sizeof(mbuf), "%.*e", prec - 1, a);

    char* epos = strchr(mbuf, 'e');
    int exp10 = epos ? atoi(epos + 1) : 0;
    if (epos) *epos = '\0';

    char digits[24];
    int nd = 0;
    for (char* p = mbuf; *p && nd < 23; p++) {
        if (*p >= '0' && *p <= '9') digits[nd++] = *p;
    }
    while (nd > 1 && digits[nd - 1] == '0') nd--;
    digits[nd] = '\0';

    const char* sign = (v < 0) ? "-" : "";
    if (exp10 >= -3 && exp10 < 7) {
        char body[40];
        int o = 0;
        if (exp10 >= 0) {
            int intlen = exp10 + 1;
            for (int i = 0; i < intlen && o < (int)sizeof(body) - 2; i++) {
                body[o++] = (i < nd) ? digits[i] : '0';
            }
            body[o++] = '.';
            if (nd > intlen) {
                for (int i = intlen; i < nd && o < (int)sizeof(body) - 1; i++) {
                    body[o++] = digits[i];
                }
            } else {
                body[o++] = '0';
            }
            body[o] = '\0';
        } else {
            body[o++] = '0';
            body[o++] = '.';
            for (int i = 0; i < -exp10 - 1 && o < (int)sizeof(body) - 1; i++) {
                body[o++] = '0';
            }
            for (int i = 0; i < nd && o < (int)sizeof(body) - 1; i++) {
                body[o++] = digits[i];
            }
            body[o] = '\0';
        }
        snprintf(out, outsz, "%s%s", sign, body);
    } else {
        char body[32];
        int o = 0;
        body[o++] = digits[0];
        body[o++] = '.';
        if (nd > 1) {
            for (int i = 1; i < nd && o < (int)sizeof(body) - 1; i++) {
                body[o++] = digits[i];
            }
        } else {
            body[o++] = '0';
        }
        body[o] = '\0';
        snprintf(out, outsz, "%s%sE%d", sign, body, exp10);
    }
}

static void java_format_float(char* out, size_t outsz, float v) {
    if (isnan(v)) { snprintf(out, outsz, "NaN"); return; }
    if (v == INFINITY)  { snprintf(out, outsz, "Infinity"); return; }
    if (v == -INFINITY) { snprintf(out, outsz, "-Infinity"); return; }
    if (v == 0.0f) { snprintf(out, outsz, signbit(v) ? "-0.0" : "0.0"); return; }

    float a = fabsf(v);
    char mbuf[40];
    int prec = 9;
    for (int p = 1; p <= 9; p++) {
        snprintf(mbuf, sizeof(mbuf), "%.*e", p - 1, (double)a);
        if (strtof(mbuf, NULL) == a) { prec = p; break; }
    }
    if (prec > 9) prec = 9;
    snprintf(mbuf, sizeof(mbuf), "%.*e", prec - 1, (double)a);

    char* epos = strchr(mbuf, 'e');
    int exp10 = epos ? atoi(epos + 1) : 0;
    if (epos) *epos = '\0';

    char digits[16];
    int nd = 0;
    for (char* p = mbuf; *p && nd < 15; p++) {
        if (*p >= '0' && *p <= '9') digits[nd++] = *p;
    }
    while (nd > 1 && digits[nd - 1] == '0') nd--;
    digits[nd] = '\0';

    const char* sign = (v < 0) ? "-" : "";
    if (exp10 >= -3 && exp10 < 7) {
        char body[32];
        int o = 0;
        if (exp10 >= 0) {
            int intlen = exp10 + 1;
            for (int i = 0; i < intlen && o < (int)sizeof(body) - 2; i++) {
                body[o++] = (i < nd) ? digits[i] : '0';
            }
            body[o++] = '.';
            if (nd > intlen) {
                for (int i = intlen; i < nd && o < (int)sizeof(body) - 1; i++) {
                    body[o++] = digits[i];
                }
            } else {
                body[o++] = '0';
            }
            body[o] = '\0';
        } else {
            body[o++] = '0';
            body[o++] = '.';
            for (int i = 0; i < -exp10 - 1 && o < (int)sizeof(body) - 1; i++) {
                body[o++] = '0';
            }
            for (int i = 0; i < nd && o < (int)sizeof(body) - 1; i++) {
                body[o++] = digits[i];
            }
            body[o] = '\0';
        }
        snprintf(out, outsz, "%s%s", sign, body);
    } else {
        char body[24];
        int o = 0;
        body[o++] = digits[0];
        body[o++] = '.';
        if (nd > 1) {
            for (int i = 1; i < nd && o < (int)sizeof(body) - 1; i++) {
                body[o++] = digits[i];
            }
        } else {
            body[o++] = '0';
        }
        body[o] = '\0';
        snprintf(out, outsz, "%s%sE%d", sign, body, exp10);
    }
}

/* FIX-19h: strict java.lang.FloatingDecimal-style parser.
 * Grammar: [whitespace] [+-] ( digits [. digits*] | . digits+ ) [eE [+-] digits+] [fFdD]? [whitespace]
 * plus exact forms "NaN" and "[+-]Infinity". Whole string must match,
 * otherwise the caller throws NumberFormatException (old atof() silently
 * returned 0.0 for garbage like "abc"). Returns 1 and fills out_d/out_f
 * (either may be NULL) on success. */
static int java_floating_from_string(const char* in, double* out_d, float* out_f) {
    if (!in) return 0;
    while (*in == ' ' || *in == '\t') in++;
    size_t len = strlen(in);
    while (len > 0 && (in[len - 1] == ' ' || in[len - 1] == '\t')) len--;
    if (len == 0 || len > 63) return 0;

    char buf[64];
    memcpy(buf, in, len);
    buf[len] = '\0';

    const char* body = buf;
    int sign = 1;
    if (*body == '+') { body++; }
    else if (*body == '-') { sign = -1; body++; }

    if (strcmp(body, "NaN") == 0) {
        if (out_d) *out_d = (double)NAN;
        if (out_f) *out_f = NAN;
        return 1;
    }
    if (strcmp(body, "Infinity") == 0) {
        double v = (sign > 0) ? INFINITY : -INFINITY;
        if (out_d) *out_d = v;
        if (out_f) *out_f = (float)v;
        return 1;
    }

    const char* p = body;
    int digits_before = 0, digits_after = 0;
    while (*p >= '0' && *p <= '9') { p++; digits_before++; }
    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9') { p++; digits_after++; }
    }
    if (digits_before + digits_after == 0) return 0;
    if (*p == 'e' || *p == 'E') {
        p++;
        if (*p == '+' || *p == '-') p++;
        if (!(*p >= '0' && *p <= '9')) return 0;
        while (*p >= '0' && *p <= '9') p++;
    }
    /* trailing type suffix accepted by FloatingDecimal (parseDouble("1.5f")) */
    if (*p == 'f' || *p == 'F' || *p == 'd' || *p == 'D') p++;
    if (*p != '\0') return 0;

    double v = strtod(buf, NULL);  /* full string incl. sign */
    if (out_d) *out_d = v;
    if (out_f) *out_f = (float)v;
    return 1;
}

void native_throw_oome(JVM* jvm, JavaThread* thread) {
    /* v34: attach "Requested/Available" diagnostics to the exception message.
     * heap.c no longer feeds the error screen directly (a failed allocation is
     * a catchable condition, not a fatal one); if this OutOfMemoryError DOES
     * turn out to be uncaught, the MIDlet-death handlers (main.c / libretro.c)
     * read detailMessage and display the same info the old heap-level call
     * used to print. */
    size_t oom_req = 0, oom_avail = 0;
    heap_get_last_oom_info(&oom_req, &oom_avail);
    char oom_msg[128];
    snprintf(oom_msg, sizeof(oom_msg),
             "Requested: %zu bytes, Available: %zu bytes",
             oom_req, oom_avail);
    /* v34.5 DIAG: pinpoint which bytecode site requested the OOM throw
     * (first 5 occurrences) - g_last_oom stays 0/0 when the OOM comes from
     * a native path (malloc/jvm_new_* fail) rather than the heap bump path. */
    {
        static int oome_site_count = 0;
        if (oome_site_count < 5) {
            oome_site_count++;
            JavaFrame* fr = thread ? thread->current_frame : NULL;
            if (fr && fr->method) {
                LOG_SAFE("[OOM-THROW] site %d: %s.%s%s PC=%d (req=%zu avail=%zu)\n",
                         oome_site_count,
                         (fr->clazz && fr->clazz->class_name) ? fr->clazz->class_name : "?",
                         fr->method->name ? fr->method->name : "?",
                         fr->method->descriptor ? fr->method->descriptor : "",
                         (int)fr->pc, oom_req, oom_avail);
            } else {
                LOG_SAFE("[OOM-THROW] site %d: (no frame) req=%zu avail=%zu\n",
                         oome_site_count, oom_req, oom_avail);
            }
        }
    }
    jvm_throw_by_name(jvm, "java/lang/OutOfMemoryError", oom_msg);
    thread->pending_exception = jvm_exception_pending(jvm);
}

void native_throw_iae(JVM* jvm, JavaThread* thread, const char* message) {
    jvm_throw_by_name(jvm, "java/lang/IllegalArgumentException", message);
    thread->pending_exception = jvm_exception_pending(jvm);
}

/* Throw NegativeArraySizeException */
void native_throw_negative_array_size(JVM* jvm, JavaThread* thread) {
    jvm_throw_by_name(jvm, "java/lang/NegativeArraySizeException", NULL);
    thread->pending_exception = jvm_exception_pending(jvm);
}

/* v18 (audit): generic helper — throw an exception of an arbitrary class by
 * internal name (e.g. "java/lang/IllegalMonitorStateException"). Used for
 * MIDP/JVMS strict exception contracts that have no dedicated wrapper. */
void native_throw_named_exception(JVM* jvm, JavaThread* thread, const char* class_name, const char* message) {
    if (!class_name) return;
    jvm_throw_by_name(jvm, class_name, message);
    thread->pending_exception = jvm_exception_pending(jvm);
}

/* v18 (audit): Throw IllegalStateException (MIDP state-machine contracts) */
void native_throw_illegal_state(JVM* jvm, JavaThread* thread, const char* message) {
    jvm_throw_by_name(jvm, "java/lang/IllegalStateException", message);
    thread->pending_exception = jvm_exception_pending(jvm);
}

/* v18 (audit): Throw IllegalMonitorStateException */
void native_throw_illegal_monitor_state(JVM* jvm, JavaThread* thread) {
    jvm_throw_by_name(jvm, "java/lang/IllegalMonitorStateException", NULL);
    thread->pending_exception = jvm_exception_pending(jvm);
}

/* Throw ArrayStoreException */
void native_throw_array_store_exception(JVM* jvm, JavaThread* thread) {
    jvm_throw_by_name(jvm, "java/lang/ArrayStoreException", NULL);
    thread->pending_exception = jvm_exception_pending(jvm);
}

/* Throw IOException */
void native_throw_ioe(JVM* jvm, JavaThread* thread, const char* message) {
    jvm_throw_by_name(jvm, "java/io/IOException", message);
    thread->pending_exception = jvm_exception_pending(jvm);
}

/* Throw InterruptedException */
void native_throw_interrupted(JVM* jvm, JavaThread* thread, const char* message) {
    jvm_throw_by_name(jvm, "java/lang/InterruptedException", message);
    thread->pending_exception = jvm_exception_pending(jvm);
}

/* === КРИТИЧЕСКОЕ ИСПРАВЛЕНИЕ: Правильный расчет слотов полей ===
 * 
 * Проблема: native-код использовал индекс поля в clazz->fields[] напрямую
 * как слот в obj->fields[], но это неправильно потому что:
 * 1. Статические поля не хранятся в экземпляре
 * 2. Поля суперкласса должны быть учтены (они идут первыми)
 * 3. Long/double занимают 2 слота
 * 
 * Эта функция вычисляет правильный слот для поля по его имени.
 * Возвращает -1 если поле не найдено.
 */

/* Helper: count instance fields in a class (excluding static) */
static int native_count_instance_fields(JavaClass* clazz) {
    int count = 0;
    if (clazz->fields) {
        for (int i = 0; i < clazz->fields_count; i++) {
            if (!(clazz->fields[i].access_flags & ACC_STATIC)) {
                count++;
                /* Long and double take 2 slots */
                if (clazz->fields[i].descriptor &&
                    (clazz->fields[i].descriptor[0] == 'J' || clazz->fields[i].descriptor[0] == 'D')) {
                    count++;
                }
            }
        }
    }
    return count;
}

/* Helper: count instance fields before a given field index */
static int native_count_instance_fields_before(JavaClass* clazz, int field_index) {
    int count = 0;
    if (clazz->fields) {
        for (int i = 0; i < field_index; i++) {
            if (!(clazz->fields[i].access_flags & ACC_STATIC)) {
                count++;
                /* Long and double take 2 slots */
                if (clazz->fields[i].descriptor &&
                    (clazz->fields[i].descriptor[0] == 'J' || clazz->fields[i].descriptor[0] == 'D')) {
                    count++;
                }
            }
        }
    }
    return count;
}

/* Build class hierarchy array from Object to obj_class */
static int native_build_hierarchy(JavaClass* obj_class, JavaClass** hierarchy, int max_depth) {
    int depth = 0;
    JavaClass* c = obj_class;
    
    while (c && depth < max_depth) {
        hierarchy[depth++] = c;
        c = c->super_class;
    }
    
    /* Reverse to get Object first */
    for (int i = 0; i < depth / 2; i++) {
        JavaClass* tmp = hierarchy[i];
        hierarchy[i] = hierarchy[depth - 1 - i];
        hierarchy[depth - 1 - i] = tmp;
    }
    
    return depth;
}

/* Find field slot in object instance - CORRECT VERSION */
typedef struct {
    JavaClass* defining_class;
    int field_index;
    int slot;
} NativeFieldLookupResult;

static NativeFieldLookupResult native_find_field_slot(JavaClass* obj_class, 
                                                       const char* field_name,
                                                       const char* descriptor) {
    NativeFieldLookupResult result = { NULL, -1, -1 };
    
    if (!obj_class || !field_name) return result;
    
    /* Build hierarchy from Object to obj_class */
    JavaClass* hierarchy[64];
    int depth = native_build_hierarchy(obj_class, hierarchy, 64);
    
    /* Search for field in hierarchy (from Object to obj_class) */
    for (int h = 0; h < depth; h++) {
        JavaClass* current_class = hierarchy[h];
        
        if (current_class->fields) {
            for (int i = 0; i < current_class->fields_count; i++) {
                JavaField* field = &current_class->fields[i];
                
                /* Skip static fields */
                if (field->access_flags & ACC_STATIC) continue;
                
                /* Match by name */
                if (!field->name || strcmp(field->name, field_name) != 0) continue;
                
                /* Match by descriptor if provided */
                if (descriptor && field->descriptor) {
                    if (strcmp(field->descriptor, descriptor) != 0) continue;
                }
                
                /* Found the field! */
                result.defining_class = current_class;
                result.field_index = i;
                
                /* Calculate slot */
                result.slot = 0;
                
                /* Add fields from all classes above current_class */
                for (int j = 0; j < h; j++) {
                    result.slot += native_count_instance_fields(hierarchy[j]);
                }
                
                /* Add fields before this one in current_class */
                result.slot += native_count_instance_fields_before(current_class, i);
                
                return result;
            }
        }
    }
    
    return result;
}

/* Convenience function to get field slot by name only */
int native_get_field_slot(JavaClass* clazz, const char* field_name) {
    NativeFieldLookupResult result = native_find_field_slot(clazz, field_name, NULL);
    if (result.slot < 0) {
        NATIVE_DEBUG("native_get_field_slot: field '%s' not found in class %s",
                field_name, clazz && clazz->class_name ? clazz->class_name : "?");
    }
    return result.slot;
}

/* Convenience function to get field value from object by name */
JavaValue native_get_field_value(JavaObject* obj, const char* field_name) {
    JavaValue v = { .raw = 0 };
    if (!obj || !obj->header.clazz || !field_name) return v;
    
    int slot = native_get_field_slot(obj->header.clazz, field_name);
    if (slot < 0) return v;
    
    int max_slots = (obj->header.clazz->instance_size - sizeof(ObjectHeader)) / sizeof(JavaValue);
    if (slot >= max_slots) {
        NATIVE_DEBUG("ERROR: field '%s' slot %d out of bounds (max %d)",
                field_name, slot, max_slots);
        return v;
    }
    
    return obj->fields[slot];
}

/* Convenience function to set field value in object by name */
void native_set_field_value(JavaObject* obj, const char* field_name, JavaValue value) {
    if (!obj || !obj->header.clazz || !field_name) return;
    
    int slot = native_get_field_slot(obj->header.clazz, field_name);
    if (slot < 0) {
        NATIVE_DEBUG("WARNING: field '%s' not found in %s",
                field_name, obj->header.clazz->class_name ? obj->header.clazz->class_name : "?");
        return;
    }
    
    int max_slots = (obj->header.clazz->instance_size - sizeof(ObjectHeader)) / sizeof(JavaValue);
    if (slot >= max_slots) {
        NATIVE_DEBUG("ERROR: field '%s' slot %d out of bounds (max %d)",
                field_name, slot, max_slots);
        return;
    }
    
    obj->fields[slot] = value;
}

/*
 * java.lang.Object native methods
 */

static JavaValue native_object_getClass(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = NATIVE_ARG_OBJECT(args, 0);
    return NATIVE_RETURN_OBJECT(obj ? obj->header.clazz : NULL);
}

static JavaValue native_object_hashCode(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = NATIVE_ARG_OBJECT(args, 0);
    return NATIVE_RETURN_INT(obj ? obj->header.hashcode : 0);
}

static JavaValue native_object_clone(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = NATIVE_ARG_OBJECT(args, 0);
    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_NULL();
    }
    
    JavaClass* clazz = obj->header.clazz;
    if (!clazz) {
        return NATIVE_RETURN_NULL();
    }
    
    /* Check if class implements Cloneable - use resolved interface_classes */
    bool implements_cloneable = false;
    
    /* Check interface_classes array (resolved interfaces) */
    if (clazz->interface_classes) {
        for (int i = 0; i < clazz->interfaces_count; i++) {
            JavaClass* iface = clazz->interface_classes[i];
            if (iface && iface->class_name && 
                strcmp(iface->class_name, "java/lang/Cloneable") == 0) {
                implements_cloneable = true;
                break;
            }
        }
    }
    
    /* Also check superclasses for Cloneable */
    JavaClass* super_check = clazz->super_class;
    while (!implements_cloneable && super_check) {
        if (super_check->interface_classes) {
            for (int i = 0; i < super_check->interfaces_count; i++) {
                JavaClass* iface = super_check->interface_classes[i];
                if (iface && iface->class_name &&
                    strcmp(iface->class_name, "java/lang/Cloneable") == 0) {
                    implements_cloneable = true;
                    break;
                }
            }
        }
        super_check = super_check->super_class;
    }
    
    if (!implements_cloneable) {
        jvm_throw_by_name(jvm, "java/lang/CloneNotSupportedException", 
                          "Class does not implement Cloneable");
        return NATIVE_RETURN_NULL();
    }
    
    /* Check if object is an array */
    if (object_is_array(obj)) {
        JavaArray* arr = (JavaArray*)obj;
        jsize len = arr->length;
        uint8_t elem_type = arr->element_type;
        JavaClass* elem_class = arr->element_class;
        
        /* Allocate new array */
        JavaArray* new_arr = heap_alloc_array(jvm, elem_type, len, elem_class);
        if (!new_arr) {
            jvm_throw_by_name(jvm, "java/lang/OutOfMemoryError", "Failed to clone array");
            return NATIVE_RETURN_NULL();
        }
        
        /* Copy array data */
        size_t elem_size = 1;
        switch (elem_type) {
            case DESC_BYTE:
            case DESC_BOOLEAN: elem_size = 1; break;
            case DESC_CHAR:
            case DESC_SHORT: elem_size = 2; break;
            case DESC_INT:
            case DESC_FLOAT: elem_size = 4; break;
            case DESC_LONG:
            case DESC_DOUBLE: elem_size = 8; break;
            case DESC_OBJECT:
            case DESC_ARRAY: elem_size = sizeof(void*); break;
        }
        
        memcpy(array_data(new_arr), array_data(arr), len * elem_size);
        
        return NATIVE_RETURN_OBJECT(new_arr);
    }
    
    /* Regular object clone */
    JavaObject* new_obj = heap_alloc_object(jvm, clazz);
    if (!new_obj) {
        jvm_throw_by_name(jvm, "java/lang/OutOfMemoryError", "Failed to clone object");
        return NATIVE_RETURN_NULL();
    }
    
    /* Copy all instance fields */
    int num_fields = OBJECT_NUM_FIELDS(obj);
    for (int i = 0; i < num_fields; i++) {
        new_obj->fields[i] = obj->fields[i];
    }
    
    /* Generate new hashcode for cloned object */
    new_obj->header.hashcode = (jint)(uintptr_t)new_obj ^ 0x12345678;
    
    return NATIVE_RETURN_OBJECT(new_obj);
}

static JavaValue native_object_notify(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)arg_count;
    JavaObject* obj = NATIVE_ARG_OBJECT(args, 0);
    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    
    int result = monitor_notify(jvm, obj);
    if (result == JNI_ERR) {
        /* Thread doesn't own the monitor - throw IllegalMonitorStateException */
        jvm_throw_by_name(jvm, "java/lang/IllegalMonitorStateException", NULL);
    }
    return NATIVE_RETURN_VOID();
}

static JavaValue native_object_notifyAll(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)arg_count;
    JavaObject* obj = NATIVE_ARG_OBJECT(args, 0);
    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    
    int result = monitor_notify_all(jvm, obj);
    if (result == JNI_ERR) {
        /* Thread doesn't own the monitor - throw IllegalMonitorStateException */
        jvm_throw_by_name(jvm, "java/lang/IllegalMonitorStateException", NULL);
    }
    return NATIVE_RETURN_VOID();
}

static JavaValue native_object_wait(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    for (int i = 0; i < arg_count && i < 5; i++) {
    }
    
    JavaObject* obj = NATIVE_ARG_OBJECT(args, 0);
    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    
    jlong timeout = 0;
    bool timed = false;
    
    /* For wait(J)V: arg_count=3 (this + 2 long slots) on most platforms
     * For wait()V: arg_count=1 (just this)
     * The long value is in args[1] (first slot) when timed wait
     * 
     * CRITICAL: Long values occupy 2 stack slots, but when passed to native
     * methods, we receive them as a single jlong in args[1]
     */
    if (arg_count >= 2) {
        /* Timed wait - long is in args[1] as a jlong */
        timeout = args[1].j;
        timed = true;
    }
    
    int result = monitor_wait(jvm, obj, timeout, timed);
    
    /* Check if interrupted - need to throw InterruptedException */
    if (result == JNI_ERR) {
        if (thread && thread->interrupted) {
            /* Clear the interrupted flag */
            thread->interrupted = false;
            
            /* Create and throw InterruptedException */
            extern JavaObject* jvm_new_object(JVM* jvm, JavaClass* clazz);
            extern JavaClass* jvm_load_class(JVM* jvm, const char* name);
            JavaClass* exc_class = jvm_load_class(jvm, "java/lang/InterruptedException");
            if (exc_class) {
                JavaObject* exc = jvm_new_object(jvm, exc_class);
                if (exc) {
                    thread->pending_exception = exc;
                }
            }
        } else {
            /* Thread doesn't own the monitor - throw IllegalMonitorStateException */
            jvm_throw_by_name(jvm, "java/lang/IllegalMonitorStateException", NULL);
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* Object.<init>() - base constructor, does nothing */
static JavaValue native_object_init(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    /* Object constructor does nothing - just return */
    return NATIVE_RETURN_VOID();
}

/* Object.equals(Object) - reference equality comparison */
static JavaValue native_object_equals(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = NATIVE_ARG_OBJECT(args, 0);
    JavaObject* other_obj = NATIVE_ARG_OBJECT(args, 1);
    
    /* Default Object.equals() returns true only if same reference */
    return NATIVE_RETURN_INT(this_obj == other_obj ? 1 : 0);
}

/* Object.toString() - returns getClass().getName() + "@" + Integer.toHexString(hashCode()) */
static JavaValue native_object_toString(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = NATIVE_ARG_OBJECT(args, 0);
    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_NULL();
    }
    
    /* Get class name */
    JavaClass* clazz = obj->header.clazz;
    if (!clazz || !clazz->class_name) {
        return NATIVE_RETURN_NULL();
    }
    
    /* Convert to Java format (dots instead of slashes) */
    char* name = class_name_to_java(clazz->class_name);
    
    /* Create result: className@hexHash */
    char result[256];
    snprintf(result, sizeof(result), "%s@%x", name, obj->header.hashcode);
    free(name);
    
    JavaString* str = jvm_new_string(jvm, result);
    return NATIVE_RETURN_OBJECT(str);
}

/* Throwable.toString() - returns getClass().getName() + ": " + getMessage() if message != null */
static JavaValue native_throwable_toString(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = NATIVE_ARG_OBJECT(args, 0);
    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_NULL();
    }
    
    /* Get class name */
    JavaClass* clazz = obj->header.clazz;
    if (!clazz || !clazz->class_name) {
        return NATIVE_RETURN_NULL();
    }
    
    /* Convert to Java format */
    char* name = class_name_to_java(clazz->class_name);
    
    /* Get detailMessage field */
    JavaString* msg = (JavaString*)native_get_field_value(obj, "detailMessage").ref;
    
    char* result;
    if (msg) {
        const char* msg_utf8 = string_utf8(jvm, msg);
        size_t len = strlen(name) + 2 + strlen(msg_utf8 ? msg_utf8 : "") + 1;
        result = (char*)malloc(len);
        snprintf(result, len, "%s: %s", name, msg_utf8 ? msg_utf8 : "");
    } else {
        result = strdup(name);
    }
    free(name);
    
    JavaString* str = jvm_new_string(jvm, result);
    free(result);
    
    return NATIVE_RETURN_OBJECT(str);
}

void init_java_lang_object(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"java/lang/Object", "<init>", "()V", native_object_init},
        {"java/lang/Object", "getClass", "()Ljava/lang/Class;", native_object_getClass},
        {"java/lang/Object", "hashCode", "()I", native_object_hashCode},
        {"java/lang/Object", "clone", "()Ljava/lang/Object;", native_object_clone},
        {"java/lang/Object", "notify", "()V", native_object_notify},
        {"java/lang/Object", "notifyAll", "()V", native_object_notifyAll},
        {"java/lang/Object", "wait", "()V", native_object_wait},      /* No-arg wait */
        {"java/lang/Object", "wait", "(J)V", native_object_wait},    /* Timed wait */
        {"java/lang/Object", "toString", "()Ljava/lang/String;", native_object_toString},
        {"java/lang/Object", "equals", "(Ljava/lang/Object;)Z", native_object_equals},
        /* Throwable.toString() - all exceptions inherit from Throwable */
        {"java/lang/Throwable", "toString", "()Ljava/lang/String;", native_throwable_toString},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
}

/*
 * java.lang.Class native methods
 */

static JavaValue native_class_forName(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)arg_count;
    /* STATIC METHOD - no 'this' argument!
     * args[0] = class name (String)
     */
    const char* name = native_get_string_utf8(jvm, args, 0);
    if (!name) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_NULL();
    }
    
    /* Convert dots to slashes */
    char* internal_name = strdup(name);
    for (char* p = internal_name; *p; p++) {
        if (*p == '.') *p = '/';
    }
    
    JavaClass* clazz = jvm_load_class(jvm, internal_name);
    free(internal_name);
    
    if (!clazz) {
        native_throw_cnfe(jvm, thread, name);
        return NATIVE_RETURN_NULL();
    }
    
    return NATIVE_RETURN_OBJECT(clazz);
}

static JavaValue native_class_getName(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaClass* clazz = (JavaClass*)args[0].ref;
    if (!clazz || !clazz->class_name) return NATIVE_RETURN_NULL();
    
    char* name = class_name_to_java(clazz->class_name);
    JavaString* str = jvm_new_string(jvm, name);
    free(name);
    
    return NATIVE_RETURN_OBJECT(str);
}

static JavaValue native_class_isInstance(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaClass* clazz = (JavaClass*)args[0].ref;
    JavaObject* obj = (JavaObject*)args[1].ref;
    
    return NATIVE_RETURN_INT(object_instance_of(obj, clazz) ? 1 : 0);
}

/* Class.getResourceAsStream(String) - load resource from JAR */
static JavaValue native_class_getResourceAsStream(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    /* v36.08: every failure path now ALSO lands in the device trace file.
     * The [RESOURCE] stderr lines above never reach log.txt on Switch, so a
     * null return was completely invisible in the field (Doom RPG session 3:
     * a null entities.str stream sent the game's readFully helper into the
     * EOF spin — see native_stm_eof_tick). The reason tag distinguishes:
     *   jar-miss   = the entry is not in the JAR (or jar extract failed —
     *                see [JAR-READ] lines, also trace-visible now)
     *   oom-array  = JVM heap could not fit the resource byte[] (full heap;
     *                GC-DEFER-IN-NATIVE means no collection runs inside this
     *                native — the next interpreter allocation collects)
     *   oom-class  = BAIS class load failed (process-malloc pressure)
     *   oom-object = BAIS object allocation failed (JVM heap) */
    #define RS_TRACE(...) do { \
        extern void sw_trace_force(const char* fmt, ...) __attribute__((weak)); \
        if (&sw_trace_force && sw_trace_force) sw_trace_force(__VA_ARGS__); \
    } while (0)
    
    /* args[0] is the Class object (which is a JavaClass* in our implementation) */
    JavaClass* clazz = (JavaClass*)args[0].ref;
    JavaString* name_str = (JavaString*)args[1].ref;
    
    /* ALWAYS LOG: Track resource loading for game debugging. These calls are
     * rare (a dozen per app) and pinpoint missing-atlas class bugs (SU-30
     * menu.dat was silently never requested). */
    fprintf(stderr, "[RESOURCE] getResourceAsStream called (clazz=%s)\n",
            (clazz && clazz->class_name) ? clazz->class_name : "?");
    fflush(stderr);
    
    if (!name_str) {
        fprintf(stderr, "[RESOURCE] FAILED: name_str is NULL\n");
        fflush(stderr);
        RS_TRACE("[RESOURCE] FAILED: name is NULL (clazz=%s)",
                 (clazz && clazz->class_name) ? clazz->class_name : "?");
        return NATIVE_RETURN_NULL();
    }
    
    /* Get the resource name */
    const char* name = string_utf8(jvm, name_str);
    if (!name) {
        fprintf(stderr, "[RESOURCE] FAILED: string_utf8 returned NULL\n");
        fflush(stderr);
        RS_TRACE("[RESOURCE] FAILED: string_utf8 NULL (clazz=%s)",
                 (clazz && clazz->class_name) ? clazz->class_name : "?");
        return NATIVE_RETURN_NULL();
    }
    
    fprintf(stderr, "[RESOURCE] Looking for: '%s'\n", name);
    fflush(stderr);
    
    /* Skip leading slash if present */
    const char* resource_name = name;
    if (resource_name[0] == '/') {
        resource_name++;
    }
    
    /* Load the resource from JAR */
    size_t data_size;
    uint8_t* data = load_jar_resource(resource_name, &data_size);
    
    if (!data) {
        fprintf(stderr, "[RESOURCE] NOT FOUND: '%s'\n", resource_name);
        fflush(stderr);
        RS_TRACE("[RESOURCE] FAILED '%s': jar-miss (not in JAR or extract/malloc "
                 "failed — see [JAR-READ] lines)", resource_name);
        return NATIVE_RETURN_NULL();
    }
    
    fprintf(stderr, "[RESOURCE] LOADED: '%s' (%zu bytes)\n", resource_name, data_size);
    fflush(stderr);
    RS_TRACE("[RESOURCE] '%s' -> %zu bytes", resource_name, data_size);
    
    /* Create a byte array to hold the data */
    JavaArray* byte_array = jvm_new_array(jvm, T_BYTE, (jsize)data_size, NULL);
    if (!byte_array) {
        free(data);
        RS_TRACE("[RESOURCE] FAILED '%s': oom-array (%zu bytes, JVM heap full; "
                 "no GC in native — next interpreter alloc collects)",
                 resource_name, data_size);
        return NATIVE_RETURN_NULL();
    }
    
    /* Copy data into byte array */
    memcpy(array_data(byte_array), data, data_size);
    free(data);
    
    /* Create ByteArrayInputStream object */
    JavaClass* bais_class = jvm_load_class(jvm, "java/io/ByteArrayInputStream");
    if (!bais_class) {
        CLASS_DEBUG("Failed to load ByteArrayInputStream class");
        RS_TRACE("[RESOURCE] FAILED '%s': oom-class (BAIS class load failed)",
                 resource_name);
        return NATIVE_RETURN_NULL();
    }
    
    JavaObject* bais_obj = jvm_new_object(jvm, bais_class);
    if (!bais_obj) {
        CLASS_DEBUG("Failed to create ByteArrayInputStream object");
        RS_TRACE("[RESOURCE] FAILED '%s': oom-object (BAIS object alloc failed)",
                 resource_name);
        return NATIVE_RETURN_NULL();
    }
    
    /* Set fields: buf = byte_array, pos = 0, count = data_size
     * ИСПРАВЛЕНО: Используем native_set_field_value вместо прямого доступа по индексу */
    JavaValue buf_val = { .ref = byte_array };
    JavaValue pos_val = { .i = 0 };
    JavaValue count_val = { .i = (jint)data_size };
    
    native_set_field_value(bais_obj, "buf", buf_val);
    native_set_field_value(bais_obj, "pos", pos_val);
    native_set_field_value(bais_obj, "count", count_val);
    
    CLASS_DEBUG("Created ByteArrayInputStream: buf=%p, pos=0, count=%zu",
            (void*)byte_array, data_size);
    
    return NATIVE_RETURN_OBJECT(bais_obj);
    #undef RS_TRACE
}

/* ByteArrayInputStream.<init>(byte[]) - constructor with byte array */
static JavaValue native_bytearrayinputstream_init_bytes(JVM* jvm, JavaThread* thread,
                                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaArray* buf = (JavaArray*)args[1].ref;
    
    if (g_j2me_runtime_debug) fprintf(stderr, "[BAIS_INIT] obj=%p, buf=%p, buf_len=%d\n", 
            (void*)obj, (void*)buf, buf ? (int)buf->length : -1);
    
    if (!obj) {
        return NATIVE_RETURN_VOID();
    }
    
    
    /* ИСПРАВЛЕНО: Используем native_set_field_value */
    JavaValue buf_val = { .ref = buf };
    JavaValue pos_val = { .i = 0 };
    JavaValue count_val = { .i = buf ? (jint)buf->length : 0 };
    
    native_set_field_value(obj, "buf", buf_val);
    native_set_field_value(obj, "pos", pos_val);
    native_set_field_value(obj, "count", count_val);
    
    /* Verify fields were set correctly */
    JavaValue v_buf = native_get_field_value(obj, "buf");
    JavaValue v_pos = native_get_field_value(obj, "pos");
    JavaValue v_count = native_get_field_value(obj, "count");
    if (g_j2me_runtime_debug) fprintf(stderr, "[BAIS_INIT] Verify: buf=%p, pos=%d, count=%d\n", 
            (void*)v_buf.ref, v_pos.i, v_count.i);
    
    return NATIVE_RETURN_VOID();
}

/* ByteArrayInputStream.<init>(byte[], int, int) - constructor with offset and length */
static JavaValue native_bytearrayinputstream_init_bytes_offset(JVM* jvm, JavaThread* thread,
                                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaArray* buf = (JavaArray*)args[1].ref;
    jint offset = args[2].i;
    jint length = args[3].i;
    
    if (!obj) {
        return NATIVE_RETURN_VOID();
    }
    
    /* ИСПРАВЛЕНО: Используем native_set_field_value. v34.49: mark = offset
     * per J2SE ByteArrayInputStream(byte[], int, int) — reset() before any
     * mark() must return to the buffer slice start, not to 0. */
    JavaValue buf_val = { .ref = buf };
    JavaValue pos_val = { .i = offset };
    JavaValue count_val = { .i = offset + length };
    JavaValue mark_val = { .i = offset };
    
    native_set_field_value(obj, "buf", buf_val);
    native_set_field_value(obj, "pos", pos_val);
    native_set_field_value(obj, "count", count_val);
    native_set_field_value(obj, "mark", mark_val);
    
    return NATIVE_RETURN_VOID();
}

/* v36.08 STM-SPIN GUARD =====================================================
 * Field case (Doom RPG [Rus], v36.05/36.06/36.07 traces): the game's readFully
 * helper is written as
 *     for (int i = n; i > 0; i -= in.read(buf, off, i)) {}
 * — at EOF read() returns -1, so i -= (-1) INCREMENTS the remaining count and
 * the loop spins on the exhausted stream forever. Signature in every field
 * trace: t2 in InputStream.read with age=0 (keeps RE-entering the native, so
 * T-STALL/MIDLET-STUCK see a "healthy" runner), heap frozen (~176 KB — the
 * spin allocates nothing, so even the deferred GC never runs), r= climbing
 * into the millions, ZERO [EX-THROW] lines. The session hangs on the loading
 * screen until the watchdog gives up 30 s later.
 *
 * Defense: count CONSECUTIVE EOF reads per thread (any successful byte
 * resets the streak). A healthy parser observes EOF once; a spin reaches the
 * limit in a few seconds. At the limit inject a CATCHABLE IOException: the
 * game's own error handling takes over (thread exits cleanly -> THREAD-EXIT
 * -> MIDLET-STUCK graceful recovery in ~15 s instead of an eternal hang),
 * and the [STM-SPIN] line in the trace names the exhausted stream size —
 * the previously invisible failure becomes a one-line diagnosis. The read
 * natives are re-entered per call, so the interpreter still polls
 * jvm->running between calls — teardown stays safe. */
#define NOJME_STM_SPIN_EOF_LIMIT 131072LL

static void native_stm_eof_reset(JavaThread* thread) {
    if (thread && thread->last_stm_eof_streak) thread->last_stm_eof_streak = 0;
}

static void native_stm_eof_tick(JVM* jvm, JavaThread* thread, jint pos, jint count, jint len) {
    if (!thread) return;
    thread->last_stm_eof_streak++;
    if (thread->last_stm_eof_streak < NOJME_STM_SPIN_EOF_LIMIT) return;

    /* Fire: log to the device trace file (NOT stderr — stderr is invisible
     * on Switch), then throw a catchable IOException and re-arm so a
     * catch-and-retry game loop cannot spin silently again afterwards. */
    thread->last_stm_eof_streak = 0;
    {
        extern void sw_trace_force(const char* fmt, ...) __attribute__((weak));
        if (&sw_trace_force && sw_trace_force) {
            sw_trace_force("[STM-SPIN] t%d: %lld consecutive EOF reads on an "
                           "exhausted %dB stream (pos=count=%d) — readFully spin; "
                           "injecting IOException to break the hang",
                           thread->id, (long long)NOJME_STM_SPIN_EOF_LIMIT,
                           len ? len : -1, pos);
        }
    }
    LOG_SAFE("[STM-SPIN] t%d: read-spin at EOF (pos=%d count=%d len=%d) — "
             "injecting IOException\n", thread->id, pos, count, len);
    jvm_throw_by_name(jvm, "java/io/IOException",
                      "stream exhausted (NOJME read-spin guard)");
    thread->pending_exception = jvm_exception_pending(jvm);
}

/* v36.09 STR-NULL GUARD ======================================================
 * A String-returning native must NEVER hand out NULL without a pending
 * exception. Field proof (Doom RPG [Rus], 3rd session, v36.08 trace):
 * string_chars() hit a stale/freed heap block, substring(II) silently
 * returned NULL, the game's `substring(...) + ".str"` concat built the
 * literal resource name "null.str", the entities.str lookup missed and the
 * EOF-unsafe readFully spun on the exhausted 922-byte entities.db (the
 * v36.08 STM-SPIN guard had to break that hang). Convert every silent NULL
 * here into a loud, catchable OutOfMemoryError plus one [STR-NULL] trace
 * line naming the op and the source pointer, so the next field log points
 * straight at the poisoned object instead of a corrupted resource name. */
static JavaString* native_string_result_guard(JVM* jvm, JavaThread* thread,
                                              JavaString* result, const char* op,
                                              const JavaString* src) {
    if (result) return result;
    if (jvm && !jvm_exception_pending(jvm)) {
        {
            extern void sw_trace_force(const char* fmt, ...) __attribute__((weak));
            if (&sw_trace_force && sw_trace_force) {
                sw_trace_force("[STR-NULL] %s produced a NULL String (src=%p) — "
                               "throwing OutOfMemoryError", op, (const void*)src);
            }
        }
        LOG_SAFE("[STR-NULL] %s produced a NULL String (src=%p)\n",
                 op, (const void*)src);
        jvm_throw_by_name(jvm, "java/lang/OutOfMemoryError",
                          "string allocation/data failed (NOJME string-native guard)");
        if (thread) thread->pending_exception = jvm_exception_pending(jvm);
    }
    return NULL;
}

/* ByteArrayInputStream.read() - read single byte */
static JavaValue native_bytearrayinputstream_read(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    if (!obj) {
        return NATIVE_RETURN_INT(-1);
    }
    
    /* ИСПРАВЛЕНО: Используем native_get_field_value */
    JavaArray* buf = (JavaArray*)native_get_field_value(obj, "buf").ref;
    jint pos = native_get_field_value(obj, "pos").i;
    jint count = native_get_field_value(obj, "count").i;
    
    /* v36.06: stream-read shadow (td lines print stm=pos/count/len r=N) —
     * EOF is RECORDED too: a thread spinning on an exhausted stream shows
     * stm=count/count/len with r climbing — instantly classifiable. */
    if (thread) {
        thread->last_stm_pos = pos;
        thread->last_stm_count = count;
        thread->last_stm_len = buf ? (jint)buf->length : -1;
        thread->last_stm_reads++;
    }
    
    if (!buf || pos >= count) {
        /* v36.08: EOF — feed the spin detector (resets on any later success) */
        native_stm_eof_tick(jvm, thread, pos, count, buf ? (jint)buf->length : -1);
        return NATIVE_RETURN_INT(-1);  /* EOF */
    }
    
    /* Read byte from buffer */
    uint8_t* data = (uint8_t*)array_data(buf);
    jbyte val = (jbyte)data[pos];
    
    /* Update position */
    JavaValue new_pos = { .i = pos + 1 };
    native_set_field_value(obj, "pos", new_pos);
    if (thread) thread->last_stm_pos = pos + 1;
    native_stm_eof_reset(thread);
    
    return NATIVE_RETURN_INT(val & 0xFF);  /* Return as unsigned */
}

/* ByteArrayInputStream.read(byte[], int, int) - read into buffer */
static JavaValue native_bytearrayinputstream_read_array(JVM* jvm, JavaThread* thread,
                                                         JavaValue* args, int arg_count) {
    (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaArray* dest = (JavaArray*)args[1].ref;
    jint offset = args[2].i;
    jint len = args[3].i;
    
    if (!obj) {
        return NATIVE_RETURN_INT(-1);
    }
    
    /* ИСПРАВЛЕНО: Используем native_get_field_value */
    JavaArray* buf = (JavaArray*)native_get_field_value(obj, "buf").ref;
    jint pos = native_get_field_value(obj, "pos").i;
    jint count = native_get_field_value(obj, "count").i;
    
    /* v36.06: stream-read shadow (see the single-byte read note above) */
    if (thread) {
        thread->last_stm_pos = pos;
        thread->last_stm_count = count;
        thread->last_stm_len = buf ? (jint)buf->length : -1;
        thread->last_stm_reads++;
    }
    
    if (!buf || !dest) {
        /* v36.08: broken stream/buffer — feed the spin detector too (the
         * game-side readFully loop treats every -1 the same way) */
        native_stm_eof_tick(jvm, thread, pos, count, buf ? (jint)buf->length : -1);
        return NATIVE_RETURN_INT(-1);
    }
    
    if (pos >= count) {
        /* v36.08: EOF — THE readFully-spin site (Doom RPG r.a():
         * "i -= read(...)" increments i on -1). Feeding the detector makes
         * the VM break the loop with a catchable IOException after
         * NOJME_STM_SPIN_EOF_LIMIT consecutive EOF reads. */
        native_stm_eof_tick(jvm, thread, pos, count, (jint)buf->length);
        return NATIVE_RETURN_INT(-1);  /* EOF */
    }
    
    /* Calculate bytes to read */
    jint available = count - pos;
    jint to_read = (len < available) ? len : available;
    
    /* Check bounds */
    if (offset < 0 || len < 0 || offset + len > (jint)dest->length) {
        /* v36.08: the game's readFully loop (r.a: "i -= read(buf, n-i, i)")
         * walks offset OUT OF RANGE after the first EOF (-1): the next call
         * has offset=-1, len=n+1 and lands HERE, not in the EOF branch.
         * Every -1 return of this native must feed the spin detector or the
         * guard never fires (first field repro: 2.3G instructions, zero
         * ticks). The streak stays safe: any successful byte resets it. */
        native_stm_eof_tick(jvm, thread, pos, count, (jint)buf->length);
        return NATIVE_RETURN_INT(-1);
    }
    
    /* Copy data */
    uint8_t* src_data = (uint8_t*)array_data(buf);
    uint8_t* dest_data = (uint8_t*)array_data(dest);
    memcpy(dest_data + offset, src_data + pos, to_read);
    
    /* Update position */
    JavaValue new_pos = { .i = pos + to_read };
    native_set_field_value(obj, "pos", new_pos);
    native_stm_eof_reset(thread);
    
    return NATIVE_RETURN_INT(to_read);
}

/* ByteArrayInputStream.read(byte[]) - read into entire buffer */
static JavaValue native_bytearrayinputstream_read_array_full(JVM* jvm, JavaThread* thread,
                                                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaArray* dest = (JavaArray*)args[1].ref;
    
    if (!obj || !dest) {
        return NATIVE_RETURN_INT(-1);
    }
    
    /* Call read(byte[], 0, length) */
    JavaValue new_args[4];
    new_args[0].ref = args[0].ref;  /* this */
    new_args[1].ref = args[1].ref;  /* buffer */
    new_args[2].i = 0;               /* offset */
    new_args[3].i = dest->length;    /* length */
    
    return native_bytearrayinputstream_read_array(jvm, thread, new_args, 4);
}

/* ByteArrayInputStream.available() - bytes available to read */
static JavaValue native_bytearrayinputstream_available(JVM* jvm, JavaThread* thread,
                                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    if (!obj) {
        return NATIVE_RETURN_INT(0);
    }
    
    /* ИСПРАВЛЕНО: Используем native_get_field_value */
    jint pos = native_get_field_value(obj, "pos").i;
    jint count = native_get_field_value(obj, "count").i;
    
    jint available = count - pos;
    return NATIVE_RETURN_INT(available > 0 ? available : 0);
}

/* Class.newInstance() - create a new instance of the class */
static JavaValue native_class_newInstance(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    
    /* args[0] is the Class object (JavaClass*) */
    JavaClass* clazz = (JavaClass*)args[0].ref;
    
    if (!clazz) {
        CLASS_DEBUG("newInstance: clazz is NULL");
        return NATIVE_RETURN_NULL();
    }
    
    CLASS_DEBUG("newInstance: creating instance of %s", clazz->class_name);
    
    /* Check if class is abstract or interface */
    if (clazz->access_flags & (ACC_ABSTRACT | ACC_INTERFACE)) {
        CLASS_DEBUG("newInstance: cannot instantiate abstract class or interface %s", clazz->class_name);
        return NATIVE_RETURN_NULL();
    }
    
    /* Create new instance */
    JavaObject* obj = jvm_new_object(jvm, clazz);
    if (!obj) {
        CLASS_DEBUG("newInstance: failed to create object for %s", clazz->class_name);
        return NATIVE_RETURN_NULL();
    }
    
    /* Call default constructor if it exists */
    JavaMethod* constructor = jvm_resolve_method(jvm, clazz, "<init>", "()V");
    if (constructor) {
        CLASS_DEBUG("newInstance: calling default constructor for %s", clazz->class_name);
        JavaValue ctor_args[1];
        ctor_args[0].ref = obj;
        JavaValue result;
        execute_method(jvm, thread, constructor, ctor_args, &result);
    } else {
        CLASS_DEBUG("newInstance: no default constructor for %s (using default init)", clazz->class_name);
    }
    
    return NATIVE_RETURN_OBJECT(obj);
}

/* Class.isArray() - check if class is an array class */
static JavaValue native_class_isArray(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    
    JavaClass* clazz = (JavaClass*)args[0].ref;
    if (!clazz || !clazz->class_name) {
        return NATIVE_RETURN_INT(0);
    }
    
    /* Array class names start with '[' */
    return NATIVE_RETURN_INT(clazz->class_name[0] == '[' ? 1 : 0);
}

/* Class.isAssignableFrom(Class) - check if this class is assignable from another */
static JavaValue native_class_isAssignableFrom(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    
    JavaClass* this_class = (JavaClass*)args[0].ref;
    JavaClass* other_class = (JavaClass*)args[1].ref;
    
    if (!this_class || !other_class) {
        return NATIVE_RETURN_INT(0);
    }
    
    /* Simple check: if classes are the same */
    if (this_class == other_class) {
        return NATIVE_RETURN_INT(1);
    }
    
    /* Check if other_class extends this_class */
    JavaClass* current = other_class;
    while (current) {
        if (current == this_class) {
            return NATIVE_RETURN_INT(1);
        }
        current = current->super_class;
    }
    
    return NATIVE_RETURN_INT(0);
}

/* Class.getSuperclass() - get the superclass */
static JavaValue native_class_getSuperclass(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    
    JavaClass* clazz = (JavaClass*)args[0].ref;
    if (!clazz) {
        return NATIVE_RETURN_NULL();
    }
    
    /* Return superclass (for Object, this will be NULL) */
    return NATIVE_RETURN_OBJECT(clazz->super_class);
}

/* Class.getComponentType() - get component type for array classes */
static JavaValue native_class_getComponentType(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    
    JavaClass* clazz = (JavaClass*)args[0].ref;
    if (!clazz || !clazz->class_name || clazz->class_name[0] != '[') {
        return NATIVE_RETURN_NULL();
    }
    
    /* Get component type name (skip the '[' prefix) */
    const char* component_name = clazz->class_name + 1;
    
    /* Handle primitive array types */
    if (component_name[0] != 'L') {
        /* Primitive type - return special class */
        JavaClass* component_class = NULL;
        switch (component_name[0]) {
            case 'Z': component_class = jvm_load_class(jvm, "java/lang/Boolean"); break;
            case 'B': component_class = jvm_load_class(jvm, "java/lang/Byte"); break;
            case 'C': component_class = jvm_load_class(jvm, "java/lang/Character"); break;
            case 'S': component_class = jvm_load_class(jvm, "java/lang/Short"); break;
            case 'I': component_class = jvm_load_class(jvm, "java/lang/Integer"); break;
            case 'J': component_class = jvm_load_class(jvm, "java/lang/Long"); break;
            case 'F': component_class = jvm_load_class(jvm, "java/lang/Float"); break;
            case 'D': component_class = jvm_load_class(jvm, "java/lang/Double"); break;
        }
        return NATIVE_RETURN_OBJECT(component_class);
    }
    
    /* Object array - component name is like "Ljava/lang/Object;" */
    /* Skip 'L' and ';' */
    char* name_copy = strdup(component_name + 1);
    size_t len = strlen(name_copy);
    if (len > 0 && name_copy[len - 1] == ';') {
        name_copy[len - 1] = '\0';
    }
    
    JavaClass* component_class = jvm_load_class(jvm, name_copy);
    free(name_copy);
    
    return NATIVE_RETURN_OBJECT(component_class);
}

void init_java_lang_class(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"java/lang/Class", "forName", "(Ljava/lang/String;)Ljava/lang/Class;", native_class_forName},
        {"java/lang/Class", "getName", "()Ljava/lang/String;", native_class_getName},
        {"java/lang/Class", "isInstance", "(Ljava/lang/Object;)Z", native_class_isInstance},
        {"java/lang/Class", "getResourceAsStream", "(Ljava/lang/String;)Ljava/io/InputStream;", native_class_getResourceAsStream},
        {"java/lang/Class", "newInstance", "()Ljava/lang/Object;", native_class_newInstance},
        {"java/lang/Class", "isArray", "()Z", native_class_isArray},
        {"java/lang/Class", "isAssignableFrom", "(Ljava/lang/Class;)Z", native_class_isAssignableFrom},
        {"java/lang/Class", "getSuperclass", "()Ljava/lang/Class;", native_class_getSuperclass},
        {"java/lang/Class", "getComponentType", "()Ljava/lang/Class;", native_class_getComponentType},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
}

/* ByteArrayInputStream.mark(int) - set the mark position
 * v34.49: was NOT REGISTERED — games calling it hit [INVOKE-MISSING].
 * J2SE/CLDC semantics: mark = pos (the readAheadLimit is ignored). */
static JavaValue native_bytearrayinputstream_mark(JVM* jvm, JavaThread* thread,
                                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count; (void)args;
    JavaObject* obj = (JavaObject*)args[0].ref;
    if (!obj) return NATIVE_RETURN_VOID();
    JavaValue m = { .i = native_get_field_value(obj, "pos").i };
    native_set_field_value(obj, "mark", m);
    return NATIVE_RETURN_VOID();
}

/* ByteArrayInputStream.reset() - reposition to the mark
 * v34.49: was NOT REGISTERED. J2SE: pos = mark (never throws for BAIS;
 * mark defaults to 0 / the constructor offset). */
static JavaValue native_bytearrayinputstream_reset(JVM* jvm, JavaThread* thread,
                                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    if (!obj) return NATIVE_RETURN_VOID();
    jint mark = native_get_field_value(obj, "mark").i;
    jint count = native_get_field_value(obj, "count").i;
    if (mark < 0) mark = 0;
    if (mark > count) mark = count;
    JavaValue p = { .i = mark };
    native_set_field_value(obj, "pos", p);
    return NATIVE_RETURN_VOID();
}

/* ByteArrayInputStream.markSupported() - always true
 * v34.49: was NOT REGISTERED — [INVOKE-MISSING] logs from several games
 * (obfuscated resource loaders check it before using mark/reset). The
 * no-op stub pushed 0 (false) and games silently took degraded paths. */
static JavaValue native_bytearrayinputstream_markSupported(JVM* jvm, JavaThread* thread,
                                                              JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    return NATIVE_RETURN_INT(1);
}

/* ByteArrayInputStream.skip(long) - skips n bytes */
static JavaValue native_bytearrayinputstream_skip(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jlong n = args[1].j;
    
    if (!obj || n <= 0) {
        return NATIVE_RETURN_LONG(0);
    }
    
    /* Get current position and count */
    jint pos = native_get_field_value(obj, "pos").i;
    jint count = native_get_field_value(obj, "count").i;
    
    /* Calculate how many bytes we can actually skip */
    jlong available = count - pos;
    jlong to_skip = (n < available) ? n : available;
    
    if (to_skip < 0) to_skip = 0;
    
    /* Update position */
    JavaValue new_pos = { .i = pos + (jint)to_skip };
    native_set_field_value(obj, "pos", new_pos);
    
    NATIVE_DEBUG("ByteArrayInputStream.skip(%ld): skipped %ld bytes, pos=%d->%d, count=%d",
            (long)n, (long)to_skip, pos, new_pos.i, count);
    
    return NATIVE_RETURN_LONG(to_skip);
}

/* ByteArrayOutputStream.size() - v34.6: Stalker queries the written-byte
 * count; previously a silent stub returning 0. */
static JavaValue native_bytearrayoutputstream_size(JVM* jvm, JavaThread* thread,
                                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    if (!obj) return NATIVE_RETURN_INT(0);
    return NATIVE_RETURN_INT(native_get_field_value(obj, "count").i);
}

/* ByteArrayOutputStream.reset() - v34.33: sets count to zero so the already
 * allocated buffer is reused (spec). Gumball 3000's resource loader (ai.j)
 * streams every resource through ONE reused ByteArrayOutputStream; with the
 * old no-op stub the stream kept growing and every toByteArray() after the
 * first returned the concatenation of ALL previously read chunks — the
 * level/track data was garbage and the loading thread died. */
static JavaValue native_bytearrayoutputstream_reset(JVM* jvm, JavaThread* thread,
                                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    if (!obj) return NATIVE_RETURN_VOID();
    JavaValue zero = { .i = 0 };
    native_set_field_value(obj, "count", zero);
    return NATIVE_RETURN_VOID();
}

/* ByteArrayOutputStream.close() - v34.33: no-op per spec (nothing to close;
 * Treasure Towers calls close() after each resource read and the missing
 * registration spammed INVOKE-MISSING). */
static JavaValue native_bytearrayoutputstream_close(JVM* jvm, JavaThread* thread,
                                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    return NATIVE_RETURN_VOID();
}

/* ByteArrayInputStream.close() - no-op per spec (v34.6: Doom RPG calls it
 * after every level resource load; the missing registration spammed the new
 * [INVOKE-MISSING] diag and, being a stub, left pos/count fields intact,
 * which is exactly the spec behavior for BAIS.close()). */
static JavaValue native_bytearrayinputstream_close(JVM* jvm, JavaThread* thread,
                                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    return NATIVE_RETURN_VOID();
}

/* ByteArrayInputStream native methods */
void init_java_io_bytearrayinputstream(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"java/io/ByteArrayInputStream", "<init>", "([B)V", native_bytearrayinputstream_init_bytes},
        {"java/io/ByteArrayInputStream", "<init>", "([BII)V", native_bytearrayinputstream_init_bytes_offset},
        {"java/io/ByteArrayInputStream", "read", "()I", native_bytearrayinputstream_read},
        {"java/io/ByteArrayInputStream", "read", "([B)I", native_bytearrayinputstream_read_array_full},
        {"java/io/ByteArrayInputStream", "read", "([BII)I", native_bytearrayinputstream_read_array},
        {"java/io/ByteArrayInputStream", "available", "()I", native_bytearrayinputstream_available},
        {"java/io/ByteArrayInputStream", "skip", "(J)J", native_bytearrayinputstream_skip},
        {"java/io/ByteArrayInputStream", "close", "()V", native_bytearrayinputstream_close},
        /* v34.49: mark/reset/markSupported were unregistered — games calling
         * them hit [INVOKE-MISSING] no-op stubs (markSupported returned false,
         * reset did nothing). */
        {"java/io/ByteArrayInputStream", "mark", "(I)V", native_bytearrayinputstream_mark},
        {"java/io/ByteArrayInputStream", "reset", "()V", native_bytearrayinputstream_reset},
        {"java/io/ByteArrayInputStream", "markSupported", "()Z", native_bytearrayinputstream_markSupported},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
}

/*
 * java.io.ByteArrayOutputStream native methods
 */

/* ByteArrayOutputStream buffer is stored in a native peer pointer */

static JavaValue native_bytearrayoutputstream_init(JVM* jvm, JavaThread* thread,
                                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    if (!obj) return NATIVE_RETURN_VOID();
    
    /* Allocate a dynamic buffer - start with 32 bytes */
    int capacity = 32;
    
    /* Create a Java byte array to hold our data */
    JavaArray* buf_array = jvm_new_array(jvm, T_BYTE, capacity, NULL);
    if (buf_array) {
        /* ИСПРАВЛЕНО: Используем native_set_field_value */
        JavaValue buf_val = { .ref = buf_array };
        JavaValue count_val = { .i = 0 };
        native_set_field_value(obj, "buf", buf_val);
        native_set_field_value(obj, "count", count_val);
    }
    
    return NATIVE_RETURN_VOID();
}

/* ByteArrayOutputStream.<init>(int size) - constructor with initial buffer size */
static JavaValue native_bytearrayoutputstream_init_size(JVM* jvm, JavaThread* thread,
                                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    if (!obj) return NATIVE_RETURN_VOID();
    
    /* Use specified initial size, minimum 32 */
    int capacity = args[1].i;
    if (capacity < 32) capacity = 32;
    
    /* Create a Java byte array to hold our data */
    JavaArray* buf_array = jvm_new_array(jvm, T_BYTE, capacity, NULL);
    if (buf_array) {
        JavaValue buf_val = { .ref = buf_array };
        JavaValue count_val = { .i = 0 };
        native_set_field_value(obj, "buf", buf_val);
        native_set_field_value(obj, "count", count_val);
    }
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_bytearrayoutputstream_tobytearray(JVM* jvm, JavaThread* thread,
                                                          JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    if (!obj) return NATIVE_RETURN_NULL();
    
    /* ИСПРАВЛЕНО: Используем native_get_field_value */
    JavaArray* buf_array = (JavaArray*)native_get_field_value(obj, "buf").ref;
    jint count = native_get_field_value(obj, "count").i;
    
    if (g_j2me_runtime_debug) fprintf(stderr, "[BAOS_TOARRAY] obj=%p, buf=%p, count=%d, buf_len=%d\n",
            (void*)obj, (void*)buf_array, count, buf_array ? (int)buf_array->length : -1);
    
    /* Print first bytes for debug */
    if (buf_array && count > 0 && g_j2me_runtime_debug) {
        uint8_t* data = (uint8_t*)array_data(buf_array);
        fprintf(stderr, "[BAOS_TOARRAY] first bytes: ");
        for (int i = 0; i < count && i < 20; i++) {
            fprintf(stderr, "%02x ", data[i]);
        }
        fprintf(stderr, "\n");
    }
    
    if (!buf_array || count <= 0) {
        /* Return empty array */
        JavaArray* empty = jvm_new_array(jvm, T_BYTE, 0, NULL);
        return NATIVE_RETURN_OBJECT(empty);
    }
    
    /* Create result array with actual size */
    JavaArray* result = jvm_new_array(jvm, T_BYTE, count, NULL);
    if (result && buf_array) {
        void* src_data = array_data(buf_array);
        void* dst_data = array_data(result);
        if (src_data && dst_data) {
            memcpy(dst_data, src_data, count);
        }
    }
    
    return NATIVE_RETURN_OBJECT(result);
}

static JavaValue native_bytearrayoutputstream_write(JVM* jvm, JavaThread* thread,
                                                     JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint b = args[1].i;
    
    if (!obj) return NATIVE_RETURN_VOID();
    
    /* ИСПРАВЛЕНО: Используем native_get/set_field_value */
    JavaArray* buf_array = (JavaArray*)native_get_field_value(obj, "buf").ref;
    jint count = native_get_field_value(obj, "count").i;
    
    /* Need to expand buffer? */
    if (!buf_array || count >= (jint)buf_array->length) {
        int new_capacity = buf_array ? buf_array->length * 2 : 32;
        if (new_capacity < count + 1) new_capacity = count + 32;
        
        JavaArray* new_array = jvm_new_array(jvm, T_BYTE, new_capacity, NULL);
        if (new_array) {
            void* src = array_data(buf_array);
            void* dst = array_data(new_array);
            /* v34.47: a subclass may have advanced 'count' past the old
             * buffer's length (TT's PNG writer skips zero bytes with
             * count += n) — copy only the real old contents, never more. */
            jint copy_n = buf_array ? (jint)buf_array->length : 0;
            if (count < copy_n) copy_n = count;
            if (buf_array && src && dst && copy_n > 0) {
                memcpy(dst, src, copy_n);
            }
            buf_array = new_array;
            JavaValue buf_val = { .ref = buf_array };
            native_set_field_value(obj, "buf", buf_val);
        }
    }
    
    /* Write byte */
    if (buf_array) {
        uint8_t* data = (uint8_t*)array_data(buf_array);
        if (data) {
            data[count] = (uint8_t)b;
            count++;
        
            /* Update count field */
            JavaValue count_val = { .i = count };
            native_set_field_value(obj, "count", count_val);
        }
    }
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_bytearrayoutputstream_write_array(JVM* jvm, JavaThread* thread,
                                                          JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaArray* data = (JavaArray*)args[1].ref;
    jint off = args[2].i;
    jint len = args[3].i;
    
    if (!obj || !data) return NATIVE_RETURN_VOID();
    
    /* ИСПРАВЛЕНО: Используем native_get/set_field_value */
    JavaArray* buf_array = (JavaArray*)native_get_field_value(obj, "buf").ref;
    jint count = native_get_field_value(obj, "count").i;
    
    /* Need to expand buffer? */
    int needed = count + len;
    if (!buf_array || needed > (jint)buf_array->length) {
        int new_capacity = buf_array ? buf_array->length * 2 : 32;
        while (new_capacity < needed) new_capacity *= 2;
        
        JavaArray* new_array = jvm_new_array(jvm, T_BYTE, new_capacity, NULL);
        if (new_array) {
            void* src = array_data(buf_array);
            void* dst = array_data(new_array);
            /* v34.47: copy only the real old contents (count may exceed
             * the old length after a subclass's count-only skip). */
            jint copy_n = buf_array ? (jint)buf_array->length : 0;
            if (count < copy_n) copy_n = count;
            if (buf_array && src && dst && copy_n > 0) {
                memcpy(dst, src, copy_n);
            }
            buf_array = new_array;
            JavaValue buf_val = { .ref = buf_array };
            native_set_field_value(obj, "buf", buf_val);
        }
    }
    
    /* Write bytes */
    if (buf_array && data) {
        uint8_t* dst = (uint8_t*)array_data(buf_array);
        uint8_t* src = (uint8_t*)array_data(data);
        if (dst && src) {
            memcpy(dst + count, src + off, len);
            count += len;
        
            JavaValue count_val = { .i = count };
            native_set_field_value(obj, "count", count_val);
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* ByteArrayOutputStream.toString() - v34.44: construct a String from the
 * written bytes using the platform default charset (CLDC 1.1 spec).
 * Previously NOT REGISTERED at all — callers hit [INVOKE-MISSING]. */
static JavaValue native_bytearrayoutputstream_tostring(JVM* jvm, JavaThread* thread,
                                                        JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;

    if (!obj) return NATIVE_RETURN_NULL();

    JavaArray* buf_array = (JavaArray*)native_get_field_value(obj, "buf").ref;
    jint count = native_get_field_value(obj, "count").i;

    if (!buf_array || count <= 0) {
        return NATIVE_RETURN_OBJECT(jvm_new_string(jvm, ""));
    }
    if (count > (jint)buf_array->length) count = (jint)buf_array->length;

    JCharsetId cs = string_default_charset();
    jsize max_units = jcharset_decode_max_units(cs, (jsize)count);
    jchar* units = (jchar*)malloc((size_t)max_units * sizeof(jchar));
    if (!units) {
        native_throw_oome(jvm, thread);
        return NATIVE_RETURN_NULL();
    }

    jsize n = jcharset_decode(cs, (uint8_t*)array_data(buf_array),
                              (jsize)count, units, max_units);
    JavaString* result = jvm_new_string_utf16(jvm, units, n);
    free(units);

    return NATIVE_RETURN_OBJECT(native_string_result_guard(jvm, thread, result,
                                                           "ByteArrayOutputStream.toString()", NULL));
}

void init_java_io_bytearrayoutputstream(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"java/io/ByteArrayOutputStream", "<init>", "()V", native_bytearrayoutputstream_init},
        {"java/io/ByteArrayOutputStream", "<init>", "(I)V", native_bytearrayoutputstream_init_size},
        {"java/io/ByteArrayOutputStream", "toByteArray", "()[B", native_bytearrayoutputstream_tobytearray},
        {"java/io/ByteArrayOutputStream", "write", "(I)V", native_bytearrayoutputstream_write},
        {"java/io/ByteArrayOutputStream", "write", "([BII)V", native_bytearrayoutputstream_write_array},
        {"java/io/ByteArrayOutputStream", "size", "()I", native_bytearrayoutputstream_size},
        {"java/io/ByteArrayOutputStream", "reset", "()V", native_bytearrayoutputstream_reset},
        {"java/io/ByteArrayOutputStream", "close", "()V", native_bytearrayoutputstream_close},
        {"java/io/ByteArrayOutputStream", "toString", "()Ljava/lang/String;", native_bytearrayoutputstream_tostring},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
}

/*
 * java.io.DataOutputStream native methods
 * DataOutputStream wraps an OutputStream and writes data to it
 */

/* Forward declaration */
static void dos_write_byte(JVM* jvm, JavaObject* dos_obj, uint8_t b);
static void dos_write_bytes(JVM* jvm, JavaObject* dos_obj, uint8_t* data, int len);

static JavaValue native_dataoutputstream_init(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaObject* out = (JavaObject*)args[1].ref;
    
    if (g_j2me_runtime_debug) fprintf(stderr, "[DOS_INIT] obj=%p, out=%p\n", (void*)obj, (void*)out);
    
    if (!obj) return NATIVE_RETURN_VOID();
    
    /* Debug: print class info */
    if (obj->header.clazz) {
        if (g_j2me_runtime_debug) fprintf(stderr, "[DOS_INIT] class=%s, fields_count=%d, instance_size=%d\n",
                obj->header.clazz->class_name ? obj->header.clazz->class_name : "?",
                obj->header.clazz->fields_count,
                (int)obj->header.clazz->instance_size);
        
        /* Print all fields */
        for (int i = 0; i < obj->header.clazz->fields_count; i++) {
            JavaField* f = &obj->header.clazz->fields[i];
            if (g_j2me_runtime_debug) fprintf(stderr, "[DOS_INIT] field[%d]: name='%s', descriptor='%s'\n",
                    i, f->name ? f->name : "?", f->descriptor ? f->descriptor : "?");
        }
    }
    
    /* ИСПРАВЛЕНО: Используем native_set_field_value */
    JavaValue out_val = { .ref = out };
    native_set_field_value(obj, "out", out_val);
    
    /* Verify */
    JavaValue verify = native_get_field_value(obj, "out");
    if (g_j2me_runtime_debug) fprintf(stderr, "[DOS_INIT] Set 'out' field, verify: ref=%p (expected %p)\n",
            (void*)verify.ref, (void*)out);
    
    return NATIVE_RETURN_VOID();
}

/* Write a single byte to the underlying output stream.
 *
 * v34.47 CRITICAL FIX (Treasure Towers invisible menu): this used to
 * special-case out's class name to EXACTLY "java/io/ByteArrayOutputStream"
 * and write buf/count fields directly. Any SUBCLASS of
 * ByteArrayOutputStream (TT's PNG writer class 'b' extends it and
 * overrides write(int) with a zero-skip trick: 0-bytes only ++count,
 * relying on the exact-size pre-zeroed buffer) failed the name check and
 * had EVERY DataOutputStream byte silently dropped — the whole PNG
 * structure (signature/IHDR/IDAT headers/CRCs) never reached the buffer,
 * Image.createImage(byte[]) threw, and the game's text fallback rendered
 * red 10x10 X squares instead of all menu/HUD text (menu looked dead and
 * key navigation was invisible). Per JDK/CLDC semantics DataOutputStream
 * routes each byte through out.write(int) VIRTUALLY, so overrides run. */
static void dos_write_byte(JVM* jvm, JavaObject* dos_obj, uint8_t b) {
    if (!dos_obj) return;

    /* ИСПРАВЛЕНО: Используем native_get_field_value */
    JavaObject* out = (JavaObject*)native_get_field_value(dos_obj, "out").ref;

    static int debug_count = 0;
    if (debug_count < 20) {
        if (g_j2me_runtime_debug) fprintf(stderr, "[DOS_WRITE_BYTE] dos=%p, out=%p, byte=%02x\n",
                (void*)dos_obj, (void*)out, b);
        debug_count++;
    }

    if (!out) return;

    /* GC safety: 'out' lives only in this C local while the nested native
     * runs (it may allocate and collect). Pin it for the duration — same
     * pattern as op_invokevirtual's native registry path. */
    gc_pin(jvm, out);

    /* Virtual dispatch: out.write(int). Resolves the override when the
     * stream is a subclass (game PNG writers), or the native when it is a
     * plain ByteArrayOutputStream. Re-entering the interpreter from a native
     * is the same pattern rms.c uses for RecordListener callbacks. */
    JavaValue wargs[1];
    wargs[0].i = (jint)b;
    JavaValue wres;
    memset(&wres, 0, sizeof(wres));
    (void)jvm_invoke_virtual(jvm, out, "write", "(I)V", wargs, &wres);
    gc_unpin(jvm, out);
}

static void dos_write_bytes(JVM* jvm, JavaObject* dos_obj, uint8_t* data, int len) {
    for (int i = 0; i < len; i++) {
        dos_write_byte(jvm, dos_obj, data[i]);
    }
}

static JavaValue native_dataoutputstream_write(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint b = args[1].i;
    
    dos_write_byte(jvm, obj, (uint8_t)b);
    return NATIVE_RETURN_VOID();
}

/* DataOutputStream.write(byte[]) - v34.6: Stalker serializes its data with
 * write(byte[]); the missing registration silently dropped the whole buffer
 * (stub no-op) - visible in the new [INVOKE-MISSING] diagnostics.
 * v34.47: route through out.write(byte[],int,int) VIRTUALLY (one call for
 * the whole array) so subclasses of ByteArrayOutputStream see the write —
 * same fix as dos_write_byte; the old per-byte path special-cased the
 * exact BAOS class name and dropped everything for subclasses. */
static JavaValue native_dataoutputstream_write_array(JVM* jvm, JavaThread* thread,
                                                      JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaArray* data = (JavaArray*)args[1].ref;

    if (!obj || !data) return NATIVE_RETURN_VOID();

    JavaObject* out = (JavaObject*)native_get_field_value(obj, "out").ref;
    if (!out) return NATIVE_RETURN_VOID();

    /* GC safety: pin both the stream and the array for the nested call. */
    gc_pin(jvm, out);
    gc_pin(jvm, data);

    /* The array is GC-reachable via the caller's operand stack while this
     * native runs, so passing it straight through is safe even if the
     * callee (or a GC inside it) runs Java code. */
    JavaValue wargs[3];
    wargs[0].ref = data;
    wargs[1].i = 0;
    wargs[2].i = (jint)data->length;
    JavaValue wres;
    memset(&wres, 0, sizeof(wres));
    int wrc = jvm_invoke_virtual(jvm, out, "write", "([BII)V", wargs, &wres);
    gc_unpin(jvm, data);
    gc_unpin(jvm, out);
    if (wrc != 0) {
        /* Dispatch failure is a bug indicator (native registry gap);
         * capped, ungated like NATIVE-MISSING. */
        static int warnc = 0;
        if (warnc < 10) {
            warnc++;
            fprintf(stderr, "[DOS-WA] write_array virtual failed rc=%d (len=%d) class=%s\n",
                    wrc, (int)data->length,
                    out->header.clazz && out->header.clazz->class_name ?
                    out->header.clazz->class_name : "?");
        }
    }
    return NATIVE_RETURN_VOID();
}

/* DataOutputStream.write(byte[],int,int) - v34.84: was NOT REGISTERED —
 * games calling the 3-arg write (user's video-frame midlet: caller u.a
 * pc=37, 240x320 RGB565 frames = 153 600 bytes per call) hit
 * [INVOKE-MISSING] and EVERY frame was silently dropped by the no-op
 * stub. Per JDK/CLDC FilterOutputStream semantics write(b,off,len)
 * routes the WHOLE range to out.write(b,off,len) VIRTUALLY — one nested
 * call for the whole buffer, so subclasses of ByteArrayOutputStream see
 * the write (same virtual-routing contract as dos_write_byte and
 * native_dataoutputstream_write_array) and 150 KB frames do NOT decay
 * into ~150 000 per-byte interpreter re-entries. */
static JavaValue native_dataoutputstream_write_array_offset(JVM* jvm, JavaThread* thread,
                                                             JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaArray* data = (JavaArray*)args[1].ref;
    jint off = args[2].i;
    jint len = args[3].i;

    if (!obj || !data) return NATIVE_RETURN_VOID();

    /* CLDC bounds contract: negative off/len or off+len > b.length is an
     * IndexOutOfBoundsException in the reference implementation; house
     * natives stay non-throwing — clamp defensively instead of crashing
     * the VM (a real OOB means a bug in the game or in our plumbing,
     * the log line pinpoints it). */
    if (off < 0) off = 0;
    if (len < 0) len = 0;
    if (off > (jint)data->length) off = (jint)data->length;
    if (off + len > (jint)data->length) len = (jint)data->length - off;
    if (len == 0) return NATIVE_RETURN_VOID();

    JavaObject* out = (JavaObject*)native_get_field_value(obj, "out").ref;
    if (!out) return NATIVE_RETURN_VOID();

    /* GC safety: pin the stream and the array for the nested virtual call
     * (the callee may run Java code and collect) — same pattern as
     * native_dataoutputstream_write_array; the opcode layer's own arg
     * pinning covers this native's frame as well, the explicit pair here
     * protects the jvm_invoke_virtual window. */
    gc_pin(jvm, out);
    gc_pin(jvm, data);

    JavaValue wargs[3];
    wargs[0].ref = data;
    wargs[1].i = off;
    wargs[2].i = len;
    JavaValue wres;
    memset(&wres, 0, sizeof(wres));
    int wrc = jvm_invoke_virtual(jvm, out, "write", "([BII)V", wargs, &wres);
    gc_unpin(jvm, data);
    gc_unpin(jvm, out);
    if (wrc != 0) {
        /* Dispatch failure is a registry-gap bug indicator; capped. */
        static int warnc = 0;
        if (warnc < 10) {
            warnc++;
            fprintf(stderr, "[DOS-WA] write_array_offset virtual failed rc=%d (off=%d len=%d) class=%s\n",
                    wrc, (int)off, (int)len,
                    out->header.clazz && out->header.clazz->class_name ?
                    out->header.clazz->class_name : "?");
        }
    }
    return NATIVE_RETURN_VOID();
}

static JavaValue native_dataoutputstream_writeint(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint v = args[1].i;
    
    uint8_t bytes[4];
    bytes[0] = (v >> 24) & 0xFF;
    bytes[1] = (v >> 16) & 0xFF;
    bytes[2] = (v >> 8) & 0xFF;
    bytes[3] = v & 0xFF;
    
    dos_write_bytes(jvm, obj, bytes, 4);
    return NATIVE_RETURN_VOID();
}

static JavaValue native_dataoutputstream_writelong(JVM* jvm, JavaThread* thread,
                                                    JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jlong v = args[1].j;
    
    uint8_t bytes[8];
    bytes[0] = (v >> 56) & 0xFF;
    bytes[1] = (v >> 48) & 0xFF;
    bytes[2] = (v >> 40) & 0xFF;
    bytes[3] = (v >> 32) & 0xFF;
    bytes[4] = (v >> 24) & 0xFF;
    bytes[5] = (v >> 16) & 0xFF;
    bytes[6] = (v >> 8) & 0xFF;
    bytes[7] = v & 0xFF;
    
    dos_write_bytes(jvm, obj, bytes, 8);
    return NATIVE_RETURN_VOID();
}

static JavaValue native_dataoutputstream_writebyte(JVM* jvm, JavaThread* thread,
                                                    JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint v = args[1].i;
    
    dos_write_byte(jvm, obj, (uint8_t)v);
    return NATIVE_RETURN_VOID();
}

static JavaValue native_dataoutputstream_writeshort(JVM* jvm, JavaThread* thread,
                                                     JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint v = args[1].i;
    
    uint8_t bytes[2];
    bytes[0] = (v >> 8) & 0xFF;
    bytes[1] = v & 0xFF;
    
    dos_write_bytes(jvm, obj, bytes, 2);
    return NATIVE_RETURN_VOID();
}

static JavaValue native_dataoutputstream_writeboolean(JVM* jvm, JavaThread* thread,
                                                       JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jboolean v = args[1].i;
    
    dos_write_byte(jvm, obj, v ? 1 : 0);
    return NATIVE_RETURN_VOID();
}

static JavaValue native_dataoutputstream_writeutf(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaString* str = (JavaString*)args[1].ref;
    
    if (!obj) return NATIVE_RETURN_VOID();
    
    if (!str) {
        /* Write 0 length for null string */
        dos_write_byte(jvm, obj, 0);
        dos_write_byte(jvm, obj, 0);
        return NATIVE_RETURN_VOID();
    }
    
    /* v34.44: proper MODIFIED UTF-8 (DataOutput contract): U+0000 is
     * encoded as the 2-byte sequence C0 80 (string_utf8() emits a raw NUL
     * byte, which both lengthens wrong and terminates the C string early),
     * and each UTF-16 unit is encoded independently (surrogate pairs end up
     * as CESU-8 3+3 bytes — exactly what a real KVM writes and readUTF
     * expects). The encoded length must fit an unsigned 16-bit prefix,
     * otherwise UTFDataFormatException is thrown per spec. */
    jsize len = string_length(str);
    const jchar* chars = string_chars(str);

    /* Pass 1: encoded byte length */
    uint32_t enc_len = 0;
    for (jsize i = 0; i < len; i++) {
        enc_len += (uint32_t)jcharset_modified_utf8_len(chars[i]);
        if (enc_len > 0xFFFF) break;
    }
    if (enc_len > 0xFFFF) {
        jvm_throw_by_name(jvm, "java/io/UTFDataFormatException", NULL);
        if (thread) thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_VOID();
    }

    /* Length prefix, big-endian */
    dos_write_byte(jvm, obj, (enc_len >> 8) & 0xFF);
    dos_write_byte(jvm, obj, enc_len & 0xFF);

    /* Pass 2: encode byte by byte (buffered writes are unnecessary —
     * dos_write_byte already goes through the BAOS native peer fast path) */
    for (jsize i = 0; i < len; i++) {
        uint8_t enc[3];
        jsize n = jcharset_modified_utf8_one((uint32_t)chars[i], enc, sizeof(enc));
        for (jsize b = 0; b < n; b++) {
            dos_write_byte(jvm, obj, enc[b]);
        }
    }
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_dataoutputstream_writefloat(JVM* jvm, JavaThread* thread,
                                                     JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jfloat v = args[1].f;
    
    /* Convert float to int bits, then write as 4 bytes (aliasing-safe) */
    jint bits;
    memcpy(&bits, &v, sizeof(bits));
    
    uint8_t bytes[4];
    bytes[0] = (bits >> 24) & 0xFF;
    bytes[1] = (bits >> 16) & 0xFF;
    bytes[2] = (bits >> 8) & 0xFF;
    bytes[3] = bits & 0xFF;
    
    dos_write_bytes(jvm, obj, bytes, 4);
    return NATIVE_RETURN_VOID();
}

static JavaValue native_dataoutputstream_writedouble(JVM* jvm, JavaThread* thread,
                                                      JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jdouble v = args[1].d;
    
    /* Convert double to long bits, then write as 8 bytes (aliasing-safe) */
    jlong bits;
    memcpy(&bits, &v, sizeof(bits));
    
    uint8_t bytes[8];
    bytes[0] = (bits >> 56) & 0xFF;
    bytes[1] = (bits >> 48) & 0xFF;
    bytes[2] = (bits >> 40) & 0xFF;
    bytes[3] = (bits >> 32) & 0xFF;
    bytes[4] = (bits >> 24) & 0xFF;
    bytes[5] = (bits >> 16) & 0xFF;
    bytes[6] = (bits >> 8) & 0xFF;
    bytes[7] = bits & 0xFF;
    
    dos_write_bytes(jvm, obj, bytes, 8);
    return NATIVE_RETURN_VOID();
}

static JavaValue native_dataoutputstream_writechar(JVM* jvm, JavaThread* thread,
                                                    JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint v = args[1].i;
    
    /* Write char as 2 bytes in big-endian order (UTF-16) */
    uint8_t bytes[2];
    bytes[0] = (v >> 8) & 0xFF;
    bytes[1] = v & 0xFF;
    
    dos_write_bytes(jvm, obj, bytes, 2);
    return NATIVE_RETURN_VOID();
}

static JavaValue native_dataoutputstream_writebytes(JVM* jvm, JavaThread* thread,
                                                     JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaString* str = (JavaString*)args[1].ref;
    
    if (!str) return NATIVE_RETURN_VOID();
    
    /* Write string as bytes (high byte discarded) */
    const jchar* chars = string_chars(str);
    if (chars) {
        for (int i = 0; i < string_length(str); i++) {
            uint16_t ch = chars[i];
            dos_write_byte(jvm, obj, ch & 0xFF);
        }
    }
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_dataoutputstream_flush(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    /* No-op for now */
    return NATIVE_RETURN_VOID();
}

static JavaValue native_dataoutputstream_close(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    /* No-op for now */
    return NATIVE_RETURN_VOID();
}

/* v58: DataOutputStream.writeChars(String).
 * Was INVOKE-MISSING (caller ac.a pc=36, 3D Coaster Rush \u00d72):
 * spec = "Writes every character in the string s, to the output stream,
 * in order, two bytes per character" \u2014 big-endian UTF-16 code units,
 * NO length prefix (that is writeUTF). The stub wrote nothing, so the
 * game's save/RMS round-trip serialized empty records. */
static JavaValue native_dataoutputstream_writechars(JVM* jvm, JavaThread* thread,
                                                    JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaString* str = (JavaString*)args[1].ref;

    if (!str) return NATIVE_RETURN_VOID();

    int len = string_length(str);
    const jchar* chars = string_chars(str);
    if (!chars || len <= 0) return NATIVE_RETURN_VOID();

    /* chunked big-endian copy, 2 bytes per UTF-16 code unit */
    uint8_t buf[512];
    int i = 0;
    while (i < len) {
        int n = 0;
        while (n < (int)sizeof(buf) - 1 && i < len) {
            jchar c = chars[i++];
            buf[n++] = (uint8_t)((c >> 8) & 0xFF);
            buf[n++] = (uint8_t)(c & 0xFF);
        }
        if (n > 0) dos_write_bytes(jvm, obj, buf, n);
    }
    return NATIVE_RETURN_VOID();
}

void init_java_io_dataoutputstream(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"java/io/DataOutputStream", "writeChars", "(Ljava/lang/String;)V", native_dataoutputstream_writechars},
        {"java/io/DataOutputStream", "<init>", "(Ljava/io/OutputStream;)V", native_dataoutputstream_init},
        {"java/io/DataOutputStream", "write", "(I)V", native_dataoutputstream_write},
        {"java/io/DataOutputStream", "write", "([B)V", native_dataoutputstream_write_array},
        {"java/io/DataOutputStream", "write", "([BII)V", native_dataoutputstream_write_array_offset},
        {"java/io/DataOutputStream", "writeInt", "(I)V", native_dataoutputstream_writeint},
        {"java/io/DataOutputStream", "writeLong", "(J)V", native_dataoutputstream_writelong},
        {"java/io/DataOutputStream", "writeByte", "(I)V", native_dataoutputstream_writebyte},
        {"java/io/DataOutputStream", "writeShort", "(I)V", native_dataoutputstream_writeshort},
        {"java/io/DataOutputStream", "writeBoolean", "(Z)V", native_dataoutputstream_writeboolean},
        {"java/io/DataOutputStream", "writeFloat", "(F)V", native_dataoutputstream_writefloat},
        {"java/io/DataOutputStream", "writeDouble", "(D)V", native_dataoutputstream_writedouble},
        {"java/io/DataOutputStream", "writeChar", "(I)V", native_dataoutputstream_writechar},
        {"java/io/DataOutputStream", "writeUTF", "(Ljava/lang/String;)V", native_dataoutputstream_writeutf},
        {"java/io/DataOutputStream", "writeBytes", "(Ljava/lang/String;)V", native_dataoutputstream_writebytes},
        {"java/io/DataOutputStream", "flush", "()V", native_dataoutputstream_flush},
        {"java/io/DataOutputStream", "close", "()V", native_dataoutputstream_close},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
}

/*
 * DataInputStream native methods
 */

/* REMOVED: native_datainputstream_read_byte_array - unused, see native_datainputstream_read_array below */

/* REMOVED: find_field_index - use native_get_field_slot instead */

/* DataInputStream.<init>(InputStream) - constructor */
static JavaValue native_datainputstream_init(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaObject* in_stream = (JavaObject*)args[1].ref;
    
    if (g_j2me_runtime_debug) fprintf(stderr, "[DIS_INIT] obj=%p, in_stream=%p\n", (void*)obj, (void*)in_stream);
    
    if (!obj) return NATIVE_RETURN_VOID();
    
    /* ИСПРАВЛЕНО: Используем native_set_field_value */
    JavaValue in_val = { .ref = in_stream };
    native_set_field_value(obj, "in", in_val);
    
    /* v22 DIAG: always-on (rare) - which streams get wrapped by DIS */
    {
        const char* wrapped = "?";
        if (in_stream && in_stream->header.clazz && in_stream->header.clazz->class_name) {
            wrapped = in_stream->header.clazz->class_name;
        }
        fprintf(stderr, "[DIS-INIT] wrap %s obj=%p in=%p\n", wrapped,
                (void*)obj, (void*)in_stream);
    }
    
    /* Verify the field was set correctly */
    JavaValue verify = native_get_field_value(obj, "in");
    if (g_j2me_runtime_debug) fprintf(stderr, "[DIS_INIT] Set 'in' field, verify: ref=%p (expected %p)\n", 
            (void*)verify.ref, (void*)in_stream);
    
    return NATIVE_RETURN_VOID();
}

/* Helper to read a single byte from underlying InputStream with proper position tracking */
static jint dis_read_byte(JVM* jvm, JavaObject* dis_obj) {
    (void)jvm;
    if (!dis_obj) {
        if (g_j2me_runtime_debug) fprintf(stderr, "[DIS_READ] dis_obj is NULL\n");
        return -1;
    }
    
    /* ИСПРАВЛЕНО: Используем native_get_field_value для доступа к полю "in" */
    JavaObject* in_stream = (JavaObject*)native_get_field_value(dis_obj, "in").ref;
    
    if (!in_stream) {
        if (g_j2me_runtime_debug) fprintf(stderr, "[DIS_READ] in_stream is NULL\n");
        return -1;
    }
    
    /* v32 CRITICAL FIX (SU-30 crash): games routinely WRAP streams —
     *   new DataInputStream(new DataInputStream(bais))
     * (SU-30's ah tokenizer) — so the immediate "in" of a DataInputStream
     * can be another DataInputStream, not the ByteArrayInputStream the old
     * code demanded. The single-level class check made every such read
     * return EOF: menu.dat parsed to an EMPTY table, the border image
     * wrapper got width=0, and am.a() later did `x % 0` → ArithmeticException
     * → the game thread died after the first 3D frame (the "only fire
     * visible, then freeze" report). Chase the "in" chain (max 4 hops)
     * until the ByteArrayInputStream, then read from ITS buf/pos/count. */
    JavaObject* stream = in_stream;
    JavaArray* buf = NULL;
    jint pos = 0, count = 0;
    for (int hop = 0; hop < 4 && stream; hop++) {
        JavaClass* sc = stream->header.clazz;
        const char* sname = (sc && sc->class_name) ? sc->class_name : NULL;
        if (!sname) break;
        if (strcmp(sname, "java/io/ByteArrayInputStream") == 0) {
            buf = (JavaArray*)native_get_field_value(stream, "buf").ref;
            pos = native_get_field_value(stream, "pos").i;
            count = native_get_field_value(stream, "count").i;
            in_stream = stream;
            break;
        }
        /* Filter stream (DataInputStream etc.): follow its "in" field */
        stream = (JavaObject*)native_get_field_value(stream, "in").ref;
    }
    
    if (!buf) {
        if (g_j2me_runtime_debug) fprintf(stderr, "[DIS_READ] no ByteArrayInputStream in stream chain\n");
        /* v36.08: no BAIS backing — every call returns -1 forever; feed the
         * spin detector (len=-1 marks the chainless case in the log) */
        native_stm_eof_tick(jvm, thread_current(jvm), 0, 0, -1);
        return -1;
    }
    
    /* v22 SELF-HEAL: a wrong-class allocation (rare VM resolver bug seen with
     * obfuscated SU-30 classes: `new DataInputStream(x)` yielded a
     * ByteArrayInputStream-classed wrapper) leaves buf/pos/count unset on the
     * wrapper while the REAL stream is one hop down its "in" field. Chase the
     * chain (max 4 hops) until a BAIS with a real buffer appears, then read
     * from it (position updates land on the object actually read). */
    int heal_hops = 0;
    while ((!buf || !is_heap_ptr_check(buf)) && heal_hops < 4) {
        JavaObject* inner = (JavaObject*)native_get_field_value(in_stream, "in").ref;
        if (!inner || inner == in_stream) break;
        JavaClass* ic = inner->header.clazz;
        if (!ic || !ic->class_name ||
            strcmp(ic->class_name, "java/io/ByteArrayInputStream") != 0) break;
        JavaArray* ib = (JavaArray*)native_get_field_value(inner, "buf").ref;
        if (ib && is_heap_ptr_check(ib)) {
            in_stream = inner;
            buf = ib;
            pos = native_get_field_value(inner, "pos").i;
            count = native_get_field_value(inner, "count").i;
            break;
        }
        in_stream = inner;
        heal_hops++;
    }
    
    if (!buf) {
        if (g_j2me_runtime_debug) fprintf(stderr, "[DIS_READ] buf is NULL\n");
        return -1;
    }
    
    if (pos >= count) {
        if (g_j2me_runtime_debug) fprintf(stderr, "[DIS_READ] EOF: pos(%d) >= count(%d)\n", pos, count);
        /* v36.06: record the EOF too (see the BAIS shadow note) */
        {
            JavaThread* stm_self = thread_current(jvm);
            if (stm_self) {
                stm_self->last_stm_pos = pos;
                stm_self->last_stm_count = count;
                stm_self->last_stm_len = buf ? (jint)buf->length : -1;
                stm_self->last_stm_reads++;
            }
        }
        /* v36.08: EOF — feed the spin detector (readInt/readBoolean translate
         * the first -1 into EOFException, so a streak here means the caller
         * keeps reading past EOF — the readFully-spin pattern) */
        native_stm_eof_tick(jvm, thread_current(jvm), pos, count, (jint)buf->length);
        return -1;  /* EOF */
    }
    
    /* Read single byte */
    uint8_t* data = (uint8_t*)array_data(buf);
    jbyte val = (jbyte)data[pos];
    
    /* CRITICAL: Update position in the underlying stream */
    JavaValue new_pos = { .i = pos + 1 };
    native_set_field_value(in_stream, "pos", new_pos);
    
    /* v36.06: stream-read shadow — records pos/count/underlying-buffer-len
     * + a cumulative read counter per thread; the per-second td dump prints
     * it as "stm=pos/count/len r=N" so a read-spin hang (Doom RPG v36.05
     * trace: t2 in InputStream.read, age=0, heap frozen) is instantly
     * classifiable: pos advancing or not, how big the stream is, how many
     * reads were issued. thread_current() is a cheap TLS lookup; the read
     * natives already pay a full native dispatch per call. */
    {
        JavaThread* stm_self = thread_current(jvm);
        if (stm_self) {
            stm_self->last_stm_pos = pos + 1;
            stm_self->last_stm_count = count;
            stm_self->last_stm_len = buf ? (jint)buf->length : -1;
            stm_self->last_stm_reads++;
            native_stm_eof_reset(stm_self);   /* v36.08: successful byte */
        }
    }
    
    /* v22 DIAG: env-gated byte-stream trace - tracks the atlas tokenizer's
     * progress through .dat resources (SU-30 menu.dat parsed to an empty
     * table despite loading fine). */
    {
        static int dis_trace = -1;
        if (dis_trace < 0) dis_trace = getenv("NOJME_DIS_TRACE") ? 1 : 0;
        if (dis_trace) {
            static int dis_logged = 0;
            if (dis_logged < 60000) {
                dis_logged++;
                fprintf(stderr, "[DIS-BYTE] pos=%d/%d byte=0x%02x '%c'\n",
                        pos, count,
                        (unsigned)(val & 0xFF),
                        (val >= 32 && val < 127) ? (char)(val & 0xFF) : '.');
            } else if (dis_logged == 60000) {
                dis_logged++;
                fprintf(stderr, "[DIS-BYTE] (further reads suppressed)\n");
            }
        }
    }
    
    return val & 0xFF;
}

/* DataInputStream.read() - reads a single byte */
static JavaValue native_datainputstream_read(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_INT(-1);
    }
    
    jint val = dis_read_byte(jvm, obj);
    
    if (val < 0) {
        return NATIVE_RETURN_INT(-1);  /* EOF */
    }
    
    return NATIVE_RETURN_INT(val);
}

/* DataInputStream.read(byte[]) - reads into byte array */
static JavaValue native_datainputstream_read_array(JVM* jvm, JavaThread* thread,
                                                    JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaArray* buffer = (JavaArray*)args[1].ref;
    
    if (!obj || !buffer) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_INT(-1);
    }
    
    uint8_t* data = (uint8_t*)array_data(buffer);
    int max_read = buffer->length;
    int bytes_read = 0;
    
    for (int i = 0; i < max_read; i++) {
        jint b = dis_read_byte(jvm, obj);
        if (b < 0) {
            break;
        }
        data[i] = (uint8_t)b;
        bytes_read++;
    }
    
    return NATIVE_RETURN_INT(bytes_read > 0 ? bytes_read : -1);
}

/* DataInputStream.read(byte[], int, int) - reads with offset/length */
static JavaValue native_datainputstream_read_offset(JVM* jvm, JavaThread* thread,
                                                     JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaArray* buffer = (JavaArray*)args[1].ref;
    jint offset = args[2].i;
    jint length = args[3].i;
    
    if (!obj || !buffer) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_INT(-1);
    }
    
    /* Bounds checking */
    if (offset < 0 || length < 0 || offset + length > (jint)buffer->length) {
        native_throw_aioobe(jvm, thread, offset);
        return NATIVE_RETURN_INT(-1);
    }
    
    uint8_t* data = (uint8_t*)array_data(buffer);
    int bytes_read = 0;
    
    for (int i = 0; i < length; i++) {
        jint b = dis_read_byte(jvm, obj);
        if (b < 0) {
            break;
        }
        data[offset + i] = (uint8_t)b;
        bytes_read++;
    }
    
    return NATIVE_RETURN_INT(bytes_read > 0 ? bytes_read : -1);
}

/* DataInputStream.skip(int) - skips bytes */
static JavaValue native_datainputstream_skip(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jlong n = args[1].j;
    
    if (!obj || n <= 0) {
        return NATIVE_RETURN_LONG(0);
    }
    
    long skipped = 0;
    for (int i = 0; i < n; i++) {
        jint b = dis_read_byte(jvm, obj);
        if (b < 0) break;
        skipped++;
    }
    
    return NATIVE_RETURN_LONG(skipped);
}

/* DataInputStream.available() - returns bytes available */
static JavaValue native_datainputstream_available(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    if (!obj) {
        return NATIVE_RETURN_INT(0);
    }
    
    /* ИСПРАВЛЕНО: Используем native_get_field_value */
    JavaObject* in_stream = (JavaObject*)native_get_field_value(obj, "in").ref;
    
    if (!in_stream) {
        return NATIVE_RETURN_INT(0);
    }
    
    /* Check if it's ByteArrayInputStream */
    JavaClass* in_class = in_stream->header.clazz;
    if (in_class && in_class->class_name && 
        strcmp(in_class->class_name, "java/io/ByteArrayInputStream") == 0) {
        
        /* ИСПРАВЛЕНО: Используем native_get_field_value */
        jint pos = native_get_field_value(in_stream, "pos").i;
        jint count = native_get_field_value(in_stream, "count").i;
        return NATIVE_RETURN_INT(count - pos);
    }
    
    return NATIVE_RETURN_INT(0);
}

/* DataInputStream.close() */
static JavaValue native_datainputstream_close(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    /* Nothing to do - underlying stream will be GC'd */
    return NATIVE_RETURN_VOID();
}

/* DataInputStream.readBoolean() */
static JavaValue native_datainputstream_readBoolean(JVM* jvm, JavaThread* thread,
                                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    jint b = dis_read_byte(jvm, obj);
    if (b < 0) {
        jvm_throw_by_name(jvm, "java/io/EOFException", NULL);
        thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_INT(0);
    }
    
    return NATIVE_RETURN_INT(b != 0 ? 1 : 0);
}

/* DataInputStream.readUnsignedShort() */
static JavaValue native_datainputstream_readUnsignedShort(JVM* jvm, JavaThread* thread,
                                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    jint b1 = dis_read_byte(jvm, obj);
    jint b2 = dis_read_byte(jvm, obj);
    
    if (b1 < 0 || b2 < 0) {
        jvm_throw_by_name(jvm, "java/io/EOFException", NULL);
        thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_INT(0);
    }
    
    return NATIVE_RETURN_INT((b1 << 8) | b2);
}

/* DataInputStream.readChar() */
static JavaValue native_datainputstream_readChar(JVM* jvm, JavaThread* thread,
                                                  JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    jint b1 = dis_read_byte(jvm, obj);
    jint b2 = dis_read_byte(jvm, obj);
    
    if (b1 < 0 || b2 < 0) {
        jvm_throw_by_name(jvm, "java/io/EOFException", NULL);
        thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_INT(0);
    }
    
    return NATIVE_RETURN_INT((b1 << 8) | b2);
}

/* DataInputStream.readUTF() - reads a UTF-8 encoded string */
static JavaValue native_datainputstream_readUTF(JVM* jvm, JavaThread* thread,
                                                 JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_NULL();
    }
    
    /* Read length (unsigned short) */
    jint b1 = dis_read_byte(jvm, obj);
    jint b2 = dis_read_byte(jvm, obj);
    
    if (b1 < 0 || b2 < 0) {
        jvm_throw_by_name(jvm, "java/io/EOFException", NULL);
        thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_NULL();
    }
    
    jint utf_length = (b1 << 8) | b2;
    
    if (utf_length == 0) {
        JavaString* empty = jvm_new_string(jvm, "");
        return NATIVE_RETURN_OBJECT(empty);
    }
    
    /* Read UTF-8 bytes - allocate extra byte for null terminator */
    uint8_t* utf_bytes = (uint8_t*)malloc(utf_length + 1);
    if (!utf_bytes) {
        native_throw_oome(jvm, thread);
        return NATIVE_RETURN_NULL();
    }
    
    for (int i = 0; i < utf_length; i++) {
        jint b = dis_read_byte(jvm, obj);
        if (b < 0) {
            free(utf_bytes);
            jvm_throw_by_name(jvm, "java/io/EOFException", NULL);
            thread->pending_exception = jvm_exception_pending(jvm);
            return NATIVE_RETURN_NULL();
        }
        utf_bytes[i] = (uint8_t)b;
    }
    
    /* Null-terminate the string! */
    utf_bytes[utf_length] = '\0';

    /* v34.8 DIAG: log decoded readUTF strings (lang pipeline check) */
    {
        static int utf_trace = -1;
        if (utf_trace < 0) utf_trace = getenv("NOJME_DIS_TRACE") ? 1 : 0;
        if (utf_trace) {
            static int utf_logged = 0;
            if (utf_logged < 600) {
                utf_logged++;
                char prev[48];
                snprintf(prev, sizeof(prev), "%s", (char*)utf_bytes);
                fprintf(stderr, "[DIS-UTF] len=%d str='%s'\n", utf_length, prev);
            }
        }
    }

    /* Convert to Java String */
    JavaString* str = jvm_new_string(jvm, (char*)utf_bytes);
    free(utf_bytes);
    
    return NATIVE_RETURN_OBJECT(str);
}

/* DataInputStream.readUTF(DataInput) - static version */
static JavaValue native_datainputstream_readUTF_static(JVM* jvm, JavaThread* thread,
                                                        JavaValue* args, int arg_count) {
    (void)arg_count;
    /* This is a static method that takes a DataInput */
    JavaObject* input = (JavaObject*)args[0].ref;
    
    if (!input) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_NULL();
    }
    
    /* Delegate to readUTF on the DataInput object */
    /* But since DataInput is an interface, we need to call the method dynamically */
    /* For simplicity, we'll assume it's a DataInputStream */
    
    /* Reuse the instance method implementation */
    JavaValue new_args[1] = { { .ref = input } };
    return native_datainputstream_readUTF(jvm, thread, new_args, 1);
}

/* DataInputStream.mark(int) - mark current position */
static JavaValue native_datainputstream_mark(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    /* v20 FIX (P1): contract repair — markSupported() already returns true,
     * so mark/reset MUST work. Store the position on the underlying
     * ByteArrayInputStream (its stub gained a `mark` field). */
    JavaObject* dis = (JavaObject*)args[0].ref;
    if (!dis) return NATIVE_RETURN_VOID();
    JavaObject* in = (JavaObject*)native_get_field_value(dis, "in").ref;
    if (!in || !in->header.clazz || !in->header.clazz->class_name) return NATIVE_RETURN_VOID();
    if (strcmp(in->header.clazz->class_name, "java/io/ByteArrayInputStream") == 0) {
        JavaValue m = { .i = native_get_field_value(in, "pos").i };
        native_set_field_value(in, "mark", m);
    }
    return NATIVE_RETURN_VOID();
}

/* DataInputStream.reset() - reset to marked position */
static JavaValue native_datainputstream_reset(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)args; (void)arg_count;
    /* v20 FIX (P1): restore pos from the BAIS mark (IOException if invalid). */
    JavaObject* dis = (JavaObject*)args[0].ref;
    if (!dis) { native_throw_npe(jvm, thread); return NATIVE_RETURN_VOID(); }
    JavaObject* in = (JavaObject*)native_get_field_value(dis, "in").ref;
    if (!in || !in->header.clazz || !in->header.clazz->class_name) return NATIVE_RETURN_VOID();
    if (strcmp(in->header.clazz->class_name, "java/io/ByteArrayInputStream") == 0) {
        jint mark = native_get_field_value(in, "mark").i;
        if (mark < 0 || mark > native_get_field_value(in, "count").i) {
            jvm_throw_by_name(jvm, "java/io/IOException", "Mark invalid");
            thread->pending_exception = jvm_exception_pending(jvm);
            return NATIVE_RETURN_VOID();
        }
        JavaValue p = { .i = mark };
        native_set_field_value(in, "pos", p);
    } else {
        jvm_throw_by_name(jvm, "java/io/IOException", "Reset not supported");
        thread->pending_exception = jvm_exception_pending(jvm);
    }
    return NATIVE_RETURN_VOID();
}

/* DataInputStream.markSupported() */
static JavaValue native_datainputstream_markSupported(JVM* jvm, JavaThread* thread,
                                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    /* ByteArrayInputStream supports mark/reset */
    return NATIVE_RETURN_INT(1);
}

/* DataInputStream.readByte() - reads a single byte */
static JavaValue native_datainputstream_readbyte(JVM* jvm, JavaThread* thread,
JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_INT(0);
    }
    
    jint val = dis_read_byte(jvm, obj);
    
    //fprintf(stderr, "[DIS.readByte] Result: %d\n", val);
    
    if (val < 0) {
        /* Throw EOFException - критически важно для остановки цикла! */
        jvm_throw_by_name(jvm, "java/io/EOFException", NULL);
        thread->pending_exception = jvm_exception_pending(jvm);
        
        /* Return zero value but exception will be detected by native_call() */
        return NATIVE_RETURN_INT(0);
    }
    
    return NATIVE_RETURN_INT((jbyte)val);
}

/* DataInputStream.readUnsignedByte() - reads an unsigned byte */
static JavaValue native_datainputstream_readunsignedbyte(JVM* jvm, JavaThread* thread,
                                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_INT(0);
    }
    
    jint val = dis_read_byte(jvm, obj);
    if (val < 0) {
        jvm_throw_by_name(jvm, "java/io/EOFException", NULL);
        thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_INT(0);
    }
    
    return NATIVE_RETURN_INT(val);
}

/* DataInputStream.readShort() - reads 2 bytes as short */
static JavaValue native_datainputstream_readshort(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_INT(0);
    }
    
    jint b1 = dis_read_byte(jvm, obj);
    jint b2 = dis_read_byte(jvm, obj);
    
    if (b1 < 0 || b2 < 0) {
        jvm_throw_by_name(jvm, "java/io/EOFException", NULL);
        thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_INT(0);
    }
    
    jshort val = (jshort)((b1 << 8) | b2);
    return NATIVE_RETURN_INT(val);
}

/* DataInputStream.readInt() - reads 4 bytes as int */
static JavaValue native_datainputstream_readint(JVM* jvm, JavaThread* thread,
JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_INT(0);
    }
    
    /* Read 4 bytes sequentially, each call advances position */
    jint b1 = dis_read_byte(jvm, obj);
    if (b1 < 0) {
        /* Throw EOFException на первом байте */
        jvm_throw_by_name(jvm, "java/io/EOFException", NULL);
        thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_INT(0);
    }
    
    jint b2 = dis_read_byte(jvm, obj);
    jint b3 = dis_read_byte(jvm, obj);
    jint b4 = dis_read_byte(jvm, obj);
    
    if (b2 < 0 || b3 < 0 || b4 < 0) {
        jvm_throw_by_name(jvm, "java/io/EOFException", NULL);
        thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_INT(0);
    }
    
    jint val = ((uint32_t)(b1) <<  24) | (b2 << 16) | (b3 << 8) | b4;
    return NATIVE_RETURN_INT(val);
}


/* DataInputStream.readLong() - reads 8 bytes as long */
static JavaValue native_datainputstream_readlong(JVM* jvm, JavaThread* thread,
                                                  JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_LONG(0);
    }
    
    jlong val = 0;
    for (int i = 0; i < 8; i++) {
        jint b = dis_read_byte(jvm, obj);
        if (b < 0) {
            jvm_throw_by_name(jvm, "java/io/EOFException", NULL);
            thread->pending_exception = jvm_exception_pending(jvm);
            return NATIVE_RETURN_LONG(0);
        }
        val = (val << 8) | b;
    }
    
    return NATIVE_RETURN_LONG(val);
}

/* DataInputStream.readFully([B)V - reads bytes to fill array */
static JavaValue native_datainputstream_readfully(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaArray* buffer = (JavaArray*)args[1].ref;
    
    if (!obj || !buffer) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    
    uint8_t* data = (uint8_t*)array_data(buffer);
    if (!data) {
        return NATIVE_RETURN_VOID();
    }
    
    for (int i = 0; i < (int)buffer->length; i++) {
        jint b = dis_read_byte(jvm, obj);
        if (b < 0) {
            jvm_throw_by_name(jvm, "java/io/EOFException", NULL);
            thread->pending_exception = jvm_exception_pending(jvm);
            return NATIVE_RETURN_VOID();
        }
        data[i] = (uint8_t)b;
    }
    
    return NATIVE_RETURN_VOID();
}

/* DataInputStream.readFully([BII)V - with bounds checking */
static JavaValue native_datainputstream_readfully_offset(JVM* jvm, JavaThread* thread,
                                                          JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaArray* buffer = (JavaArray*)args[1].ref;
    jint offset = args[2].i;
    jint len = args[3].i;
    
    if (!obj || !buffer) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    
    /* Bounds checking */
    if (offset < 0 || len < 0 || offset + len > (jint)buffer->length) {
        native_throw_aioobe(jvm, thread, offset);
        return NATIVE_RETURN_VOID();
    }
    
    uint8_t* data = (uint8_t*)array_data(buffer);
    if (!data) {
        return NATIVE_RETURN_VOID();
    }
    
    for (int i = 0; i < len; i++) {
        jint b = dis_read_byte(jvm, obj);
        if (b < 0) {
            /* Throw EOFException if we couldn't read all bytes */
            jvm_throw_by_name(jvm, "java/io/EOFException", NULL);
            thread->pending_exception = jvm_exception_pending(jvm);
            return NATIVE_RETURN_VOID();
        }
        data[offset + i] = (uint8_t)b;
    }
    
    return NATIVE_RETURN_VOID();
}

/* DataInputStream.skipBytes(int) - skips n bytes */
static JavaValue native_datainputstream_skipbytes(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint n = args[1].i;
    
    if (!obj || n <= 0) {
        return NATIVE_RETURN_INT(0);
    }
    
    int skipped = 0;
    for (int i = 0; i < n; i++) {
        jint b = dis_read_byte(jvm, obj);
        if (b < 0) break;
        skipped++;
    }
    
    return NATIVE_RETURN_INT(skipped);
}

/* DataInputStream.readFloat() */
static JavaValue native_datainputstream_readfloat(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    /* Read 4 bytes as int, then convert to float */
    jint bits = 0;
    for (int i = 0; i < 4; i++) {
        jint b = dis_read_byte(jvm, obj);
        if (b < 0) {
            jvm_throw_by_name(jvm, "java/io/EOFException", NULL);
            thread->pending_exception = jvm_exception_pending(jvm);
            return NATIVE_RETURN_FLOAT(0.0f);
        }
        bits = (bits << 8) | b;
    }
    
    jfloat val;
    memcpy(&val, &bits, sizeof(val));   /* aliasing-safe int->float punning */
    return NATIVE_RETURN_FLOAT(val);
}

/* DataInputStream.readDouble() */
static JavaValue native_datainputstream_readdouble(JVM* jvm, JavaThread* thread,
                                                    JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    /* Read 8 bytes as long, then convert to double */
    jlong bits = 0;
    for (int i = 0; i < 8; i++) {
        jint b = dis_read_byte(jvm, obj);
        if (b < 0) {
            jvm_throw_by_name(jvm, "java/io/EOFException", NULL);
            thread->pending_exception = jvm_exception_pending(jvm);
            return NATIVE_RETURN_DOUBLE(0.0);
        }
        bits = (bits << 8) | b;
    }
    
    jdouble val;
    memcpy(&val, &bits, sizeof(val));   /* aliasing-safe long->double punning */
    return NATIVE_RETURN_DOUBLE(val);
}

void init_java_io_datainputstream(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"java/io/DataInputStream", "<init>", "(Ljava/io/InputStream;)V", native_datainputstream_init},
        {"java/io/DataInputStream", "read", "()I", native_datainputstream_read},
        {"java/io/DataInputStream", "read", "([B)I", native_datainputstream_read_array},
        {"java/io/DataInputStream", "read", "([BII)I", native_datainputstream_read_offset},
        {"java/io/DataInputStream", "readByte", "()B", native_datainputstream_readbyte},
        {"java/io/DataInputStream", "readUnsignedByte", "()I", native_datainputstream_readunsignedbyte},
        {"java/io/DataInputStream", "readShort", "()S", native_datainputstream_readshort},
        {"java/io/DataInputStream", "readUnsignedShort", "()I", native_datainputstream_readUnsignedShort},
        {"java/io/DataInputStream", "readChar", "()C", native_datainputstream_readChar},
        {"java/io/DataInputStream", "readInt", "()I", native_datainputstream_readint},
        {"java/io/DataInputStream", "readLong", "()J", native_datainputstream_readlong},
        {"java/io/DataInputStream", "readFloat", "()F", native_datainputstream_readfloat},
        {"java/io/DataInputStream", "readDouble", "()D", native_datainputstream_readdouble},
        {"java/io/DataInputStream", "readBoolean", "()Z", native_datainputstream_readBoolean},
        {"java/io/DataInputStream", "readUTF", "()Ljava/lang/String;", native_datainputstream_readUTF},
        {"java/io/DataInputStream", "readUTF", "(Ljava/io/DataInput;)Ljava/lang/String;", native_datainputstream_readUTF_static},
        {"java/io/DataInputStream", "readFully", "([B)V", native_datainputstream_readfully},
        {"java/io/DataInputStream", "readFully", "([BII)V", native_datainputstream_readfully_offset},
        {"java/io/DataInputStream", "skipBytes", "(I)I", native_datainputstream_skipbytes},
        {"java/io/DataInputStream", "skip", "(J)J", native_datainputstream_skip},
        {"java/io/DataInputStream", "available", "()I", native_datainputstream_available},
        {"java/io/DataInputStream", "close", "()V", native_datainputstream_close},
        {"java/io/DataInputStream", "mark", "(I)V", native_datainputstream_mark},
        {"java/io/DataInputStream", "reset", "()V", native_datainputstream_reset},
        {"java/io/DataInputStream", "markSupported", "()Z", native_datainputstream_markSupported},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
    NATIVE_DEBUG("Registered java/io/DataInputStream native methods (%zu)", 
            sizeof(methods) / sizeof(methods[0]));
}

/*
 * com.nokia.mid.sound.Sound native methods
 * Nokia's sound API for playing audio
 */

/* Sound stores audio data and native handle */
static JavaValue native_sound_init(JVM* jvm, JavaThread* thread,
                                   JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaArray* data = (JavaArray*)args[1].ref;
    jint type = args[2].i;
    
    if (!obj) return NATIVE_RETURN_VOID();
    
    /* ИСПРАВЛЕНО: Используем native_set_field_value */
    JavaValue data_val = { .ref = data };
    JavaValue type_val = { .i = type };
    native_set_field_value(obj, "data", data_val);
    native_set_field_value(obj, "soundType", type_val);
    JavaValue state_val = { .i = 1 };  /* SOUND_STOPPED */
    native_set_field_value(obj, "state", state_val);
    JavaValue gain_def = { .i = 100 };  /* default = max gain */
    native_set_field_value(obj, "gain", gain_def);
    JavaValue id_def = { .i = 0 };  /* channel allocated on first play */
    native_set_field_value(obj, "soundId", id_def);
    
    return NATIVE_RETURN_VOID();
}

/* ---- v17: Nokia Sound natives with real audio backend (see media.c) ---- */

extern int  nokia_sound_prepare(int id, const uint8_t* data, int len, int format);
extern int  nokia_sound_play(int id, int loop_count);
extern void nokia_sound_stop(int id);
extern void nokia_sound_resume(int id);
extern void nokia_sound_release(int id);
extern int  nokia_sound_get_state(int id);
extern void nokia_sound_set_gain(int id, int gain);
extern int  nokia_sound_get_gain(int id);
extern int  nokia_sound_get_duration_ms(int id);
extern bool nokia_sound_poll_finished(int id);

/* Get (lazily allocate) the native channel id stored in the soundId field */
static int sound_channel_id(JVM* jvm, JavaObject* obj) {
    JavaValue id_val = native_get_field_value(obj, "soundId");
    int id = id_val.i;
    if (id == 0) {
        id = nokia_sound_prepare(0, NULL, 0, 0);
        JavaValue id_new = { .i = id };
        native_set_field_value(obj, "soundId", id_new);
    }
    (void)jvm;
    return id;
}

/* Deliver soundStarted/soundStopped to the SoundListener (if registered) */
static void sound_notify_listener(JVM* jvm, JavaObject* obj, const char* method) {
    if (!obj) return;
    JavaObject* listener = (JavaObject*)native_get_field_value(obj, "soundListener").ref;
    if (!listener || !heap_java_object_valid(listener)) return;

    JavaValue cb_args[1];
    cb_args[0].ref = obj;
    JavaValue cb_result;
    memset(&cb_result, 0, sizeof(cb_result));
    jvm_invoke_virtual(jvm, listener, method,
        "(Lcom/nokia/mid/sound/Sound;)V", cb_args, &cb_result);
}

/* Sound.play() - play the prepared sound once */
static JavaValue native_sound_play(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    if (!obj) return NATIVE_RETURN_VOID();

    JavaArray* data = (JavaArray*)native_get_field_value(obj, "data").ref;
    jint sound_type = native_get_field_value(obj, "soundType").i;
    int id = sound_channel_id(jvm, obj);

    /* (Re)prepare current data (data may have changed since ctor) */
    const uint8_t* bytes = NULL;
    int len = 0;
    if (data && array_data(data)) {
        bytes = (const uint8_t*)array_data(data);
        len = (int)data->length;
    }
    nokia_sound_prepare(id, bytes, len, sound_type);

    int rc = nokia_sound_play(id, 0);
    JavaValue state_val = { .i = (rc == 0) ? 0 : 1 };  /* SOUND_PLAYING / STOPPED */
    native_set_field_value(obj, "state", state_val);

    if (rc == 0) {
        sound_notify_listener(jvm, obj, "soundStarted");
    }

    return NATIVE_RETURN_VOID();
}

/* Sound.play(int) - play with loop count (-1 = repeat forever) */
static JavaValue native_sound_play_loop(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint loop = args[1].i;
    if (!obj) return NATIVE_RETURN_VOID();

    JavaArray* data = (JavaArray*)native_get_field_value(obj, "data").ref;
    jint sound_type = native_get_field_value(obj, "soundType").i;
    int id = sound_channel_id(jvm, obj);

    const uint8_t* bytes = NULL;
    int len = 0;
    if (data && array_data(data)) {
        bytes = (const uint8_t*)array_data(data);
        len = (int)data->length;
    }
    nokia_sound_prepare(id, bytes, len, sound_type);

    int rc = nokia_sound_play(id, loop);
    JavaValue state_val = { .i = (rc == 0) ? 0 : 1 };
    native_set_field_value(obj, "state", state_val);

    if (rc == 0) {
        sound_notify_listener(jvm, obj, "soundStarted");
    }

    return NATIVE_RETURN_VOID();
}

static JavaValue native_sound_stop(JVM* jvm, JavaThread* thread,
                                   JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    if (!obj) return NATIVE_RETURN_VOID();

    int id = native_get_field_value(obj, "soundId").i;
    if (id != 0) nokia_sound_stop(id);

    JavaValue state_val = { .i = 1 };  /* SOUND_STOPPED */
    native_set_field_value(obj, "state", state_val);
    JavaValue gain_def = { .i = 100 };  /* default = max gain */
    native_set_field_value(obj, "gain", gain_def);
    JavaValue id_def = { .i = 0 };  /* channel allocated on first play */
    native_set_field_value(obj, "soundId", id_def);
    sound_notify_listener(jvm, obj, "soundStopped");

    return NATIVE_RETURN_VOID();
}

static JavaValue native_sound_getstate(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    if (!obj) return NATIVE_RETURN_INT(1);  /* SOUND_STOPPED */

    int id = native_get_field_value(obj, "soundId").i;
    int state = (id != 0) ? nokia_sound_get_state(id) : 1;

    /* Deliver soundStopped if playback finished naturally */
    if (id != 0 && nokia_sound_poll_finished(id)) {
        JavaValue state_val = { .i = 1 };
        native_set_field_value(obj, "state", state_val);
        sound_notify_listener(jvm, obj, "soundStopped");
        state = 1;
    } else {
        JavaValue state_val = { .i = state };
        native_set_field_value(obj, "state", state_val);
    }

    /* Nokia Sound states: SOUND_PLAYING=0, SOUND_STOPPED=1, SOUND_UNINITIALIZED=3 */
    return NATIVE_RETURN_INT(state);
}

static JavaValue native_sound_setgain(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint gain = args[1].i;
    if (!obj) return NATIVE_RETURN_VOID();

    int id = native_get_field_value(obj, "soundId").i;
    if (id != 0) nokia_sound_set_gain(id, gain);

    JavaValue gain_val = { .i = gain };
    native_set_field_value(obj, "gain", gain_val);
    return NATIVE_RETURN_VOID();
}

static JavaValue native_sound_release(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    if (!obj) return NATIVE_RETURN_VOID();

    int id = native_get_field_value(obj, "soundId").i;
    if (id != 0) nokia_sound_release(id);

    JavaValue state_val = { .i = 3 };  /* SOUND_UNINITIALIZED */
    native_set_field_value(obj, "state", state_val);
    JavaValue id_val = { .i = 0 };
    native_set_field_value(obj, "soundId", id_val);

    return NATIVE_RETURN_VOID();
}

/* Sound.getGain() - returns current gain level */
static JavaValue native_sound_getGain(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    if (!obj) return NATIVE_RETURN_INT(-1);
    int id = native_get_field_value(obj, "soundId").i;
    if (id == 0) {
        /* Not prepared yet: report the stored field (default = max gain) */
        return NATIVE_RETURN_INT(native_get_field_value(obj, "gain").i);
    }
    return NATIVE_RETURN_INT(nokia_sound_get_gain(id));
}

/* Sound.resume() - resume paused playback */
static JavaValue native_sound_resume(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)jvm;
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    if (!obj) return NATIVE_RETURN_VOID();

    int id = native_get_field_value(obj, "soundId").i;
    if (id != 0) nokia_sound_resume(id);

    return NATIVE_RETURN_VOID();
}

/* Sound.getDuration() - get duration in milliseconds */
static JavaValue native_sound_getDuration(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    if (!obj) return NATIVE_RETURN_INT(-1);
    int id = native_get_field_value(obj, "soundId").i;
    if (id == 0) return NATIVE_RETURN_INT(-1);
    return NATIVE_RETURN_INT(nokia_sound_get_duration_ms(id));
}

/* Sound.setSoundListener(SoundListener) - set event listener */
static JavaValue native_sound_setSoundListener(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaObject* listener = (JavaObject*)args[1].ref;
    
    if (obj) {
        JavaValue listener_val = { .ref = listener };
        native_set_field_value(obj, "soundListener", listener_val);
    }
    
    return NATIVE_RETURN_VOID();
}

/* Sound.init(byte[], int, int) - initialize with data, type and frames */
static JavaValue native_sound_init_frames(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaArray* data = (JavaArray*)args[1].ref;
    jint type = args[2].i;
    jint frames = args[3].i;
    
    if (!obj) return NATIVE_RETURN_VOID();
    
    /* ИСПРАВЛЕНО: Используем native_set_field_value */
    JavaValue data_val = { .ref = data };
    JavaValue type_val = { .i = type };
    native_set_field_value(obj, "data", data_val);
    native_set_field_value(obj, "soundType", type_val);
    JavaValue state_val = { .i = 1 };  /* SOUND_STOPPED */
    native_set_field_value(obj, "state", state_val);
    JavaValue gain_def = { .i = 100 };  /* default = max gain */
    native_set_field_value(obj, "gain", gain_def);
    JavaValue id_def = { .i = 0 };  /* channel allocated on first play */
    native_set_field_value(obj, "soundId", id_def);
    
    (void)frames;
    return NATIVE_RETURN_VOID();
}

/* Sound default constructor ()V */
static JavaValue native_sound_init_default(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    if (!obj) return NATIVE_RETURN_VOID();
    
    /* ИСПРАВЛЕНО: Используем native_set_field_value */
    JavaValue null_val = { .ref = NULL };
    JavaValue zero_val = { .i = 0 };
    native_set_field_value(obj, "data", null_val);
    native_set_field_value(obj, "soundType", zero_val);
    JavaValue state_val = { .i = 1 };  /* SOUND_STOPPED */
    native_set_field_value(obj, "state", state_val);
    JavaValue gain_def = { .i = 100 };  /* default = max gain */
    native_set_field_value(obj, "gain", gain_def);
    JavaValue id_def = { .i = 0 };  /* channel allocated on first play */
    native_set_field_value(obj, "soundId", id_def);
    
    return NATIVE_RETURN_VOID();
}

/* Sound constructor with byte array only ([B)V */
static JavaValue native_sound_init_bytes(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaArray* data = (JavaArray*)args[1].ref;
    
    if (!obj) return NATIVE_RETURN_VOID();
    
    /* ИСПРАВЛЕНО: Используем native_set_field_value */
    JavaValue data_val = { .ref = data };
    JavaValue zero_val = { .i = 0 };
    native_set_field_value(obj, "data", data_val);
    native_set_field_value(obj, "soundType", zero_val);
    JavaValue state_val = { .i = 1 };  /* SOUND_STOPPED */
    native_set_field_value(obj, "state", state_val);
    JavaValue gain_def = { .i = 100 };  /* default = max gain */
    native_set_field_value(obj, "gain", gain_def);
    JavaValue id_def = { .i = 0 };  /* channel allocated on first play */
    native_set_field_value(obj, "soundId", id_def);
    
    return NATIVE_RETURN_VOID();
}

/* Sound constructor with int and long (IJ)V - for tone sounds */
static JavaValue native_sound_init_int_long(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint type = args[1].i;
    jlong data = args[2].j;
    (void)data;  /* tone data ignored, stored as null */
    
    if (!obj) return NATIVE_RETURN_VOID();
    
    /* ИСПРАВЛЕНО: Используем native_set_field_value */
    JavaValue null_val = { .ref = NULL };
    JavaValue type_val = { .i = type };
    native_set_field_value(obj, "data", null_val);
    native_set_field_value(obj, "soundType", type_val);
    JavaValue state_val = { .i = 1 };  /* SOUND_STOPPED */
    native_set_field_value(obj, "state", state_val);
    JavaValue gain_def = { .i = 100 };  /* default = max gain */
    native_set_field_value(obj, "gain", gain_def);
    JavaValue id_def = { .i = 0 };  /* channel allocated on first play */
    native_set_field_value(obj, "soundId", id_def);
    
    return NATIVE_RETURN_VOID();
}

/* Sound constructor with int and double (ID)V - for tone sounds */
static JavaValue native_sound_init_int_double(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint type = args[1].i;
    jdouble data = args[2].d;
    (void)data;  /* tone data ignored, stored as null */
    
    if (!obj) return NATIVE_RETURN_VOID();
    
    /* ИСПРАВЛЕНО: Используем native_set_field_value */
    JavaValue null_val = { .ref = NULL };
    JavaValue type_val = { .i = type };
    native_set_field_value(obj, "data", null_val);
    native_set_field_value(obj, "soundType", type_val);
    JavaValue state_val = { .i = 1 };  /* SOUND_STOPPED */
    native_set_field_value(obj, "state", state_val);
    JavaValue gain_def = { .i = 100 };  /* default = max gain */
    native_set_field_value(obj, "gain", gain_def);
    JavaValue id_def = { .i = 0 };  /* channel allocated on first play */
    native_set_field_value(obj, "soundId", id_def);
    
    return NATIVE_RETURN_VOID();
}

/* Sound constructor with int only (I)V */
static JavaValue native_sound_init_int(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint type = args[1].i;
    
    if (!obj) return NATIVE_RETURN_VOID();
    
    /* ИСПРАВЛЕНО: Используем native_set_field_value */
    JavaValue null_val = { .ref = NULL };
    JavaValue type_val = { .i = type };
    native_set_field_value(obj, "data", null_val);
    native_set_field_value(obj, "soundType", type_val);
    JavaValue state_val = { .i = 1 };  /* SOUND_STOPPED */
    native_set_field_value(obj, "state", state_val);
    JavaValue gain_def = { .i = 100 };  /* default = max gain */
    native_set_field_value(obj, "gain", gain_def);
    JavaValue id_def = { .i = 0 };  /* channel allocated on first play */
    native_set_field_value(obj, "soundId", id_def);
    
    return NATIVE_RETURN_VOID();
}

/* Sound.getConcurrentSoundCount(int) - returns max concurrent sounds for a type */
static JavaValue native_sound_getConcurrentSoundCount(JVM* jvm, JavaThread* thread,
                                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    /* Return 1 - only one sound at a time supported */
    return NATIVE_RETURN_INT(1);
}

/* Sound.getSupportedFormats() - returns array of supported format constants */
static JavaValue native_sound_getSupportedFormats(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)thread; (void)args; (void)arg_count;
    /* Return int array {FORMAT_TONE=1, FORMAT_WAV=5} */
    JavaArray* arr = jvm_new_array(jvm, T_INT, 2, NULL);
    if (arr) {
        jint* elems = (jint*)array_data(arr);
        if (elems) {
            elems[0] = 1;  /* FORMAT_TONE */
            elems[1] = 5;  /* FORMAT_WAV */
        }
    }
    JavaValue result = { .ref = arr };
    return result;
}

void init_nokia_sound(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"com/nokia/mid/sound/Sound", "<init>", "()V", native_sound_init_default},
        {"com/nokia/mid/sound/Sound", "<init>", "([B)V", native_sound_init_bytes},
        {"com/nokia/mid/sound/Sound", "<init>", "([BI)V", native_sound_init},
        {"com/nokia/mid/sound/Sound", "<init>", "([BII)V", native_sound_init_frames},
        {"com/nokia/mid/sound/Sound", "<init>", "(I)V", native_sound_init_int},
        {"com/nokia/mid/sound/Sound", "<init>", "(IJ)V", native_sound_init_int_long},
        {"com/nokia/mid/sound/Sound", "<init>", "(ID)V", native_sound_init_int_double},
        {"com/nokia/mid/sound/Sound", "play", "()V", native_sound_play},
        {"com/nokia/mid/sound/Sound", "play", "(I)V", native_sound_play_loop},
        {"com/nokia/mid/sound/Sound", "stop", "()V", native_sound_stop},
        {"com/nokia/mid/sound/Sound", "getState", "()I", native_sound_getstate},
        {"com/nokia/mid/sound/Sound", "setGain", "(I)V", native_sound_setgain},
        {"com/nokia/mid/sound/Sound", "getGain", "()I", native_sound_getGain},
        {"com/nokia/mid/sound/Sound", "resume", "()V", native_sound_resume},
        {"com/nokia/mid/sound/Sound", "getDuration", "()I", native_sound_getDuration},
        {"com/nokia/mid/sound/Sound", "setSoundListener", "(Lcom/nokia/mid/sound/SoundListener;)V", native_sound_setSoundListener},
        {"com/nokia/mid/sound/Sound", "release", "()V", native_sound_release},
        {"com/nokia/mid/sound/Sound", "getConcurrentSoundCount", "(I)I", native_sound_getConcurrentSoundCount},
        {"com/nokia/mid/sound/Sound", "getSupportedFormats", "()[I", native_sound_getSupportedFormats},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
    NATIVE_DEBUG("Registered Nokia Sound native methods (%zu)", sizeof(methods) / sizeof(methods[0]));
}

/*
 * com.nokia.mid.ui.DirectUtils native methods
 * Nokia's utility class for creating images and getting DirectGraphics
 */

/* DirectUtils.createImage(int width, int height) - creates a mutable image */
static JavaValue native_directutils_createImage(JVM* jvm, JavaThread* thread,
                                                 JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    jint width = args[0].i;
    jint height = args[1].i;
    
    NATIVE_DEBUG("DirectUtils.createImage(%d, %d)", width, height);
    
    /* Create native image using midp_image_create from display.c */
    extern MidpImage* midp_image_create(int width, int height, bool mutable);
    MidpImage* img = midp_image_create(width, height, true);
    if (!img) {
        NATIVE_DEBUG("Failed to create native image");
        return NATIVE_RETURN_NULL();
    }
    
    /* Diagnostic: track image creation */
    if (g_j2me_runtime_debug) fprintf(stderr, "[DirectUtils.createImage] Created %dx%d mutable image, native=%p, pixels=%p\n",
            width, height, (void*)img, (void*)img->pixels);
    fflush(stderr);
    
    /* Create Image object using native factory */
    JavaClass* image_class = jvm_load_class(jvm, "javax/microedition/lcdui/Image");
    if (!image_class) {
        /* v36.12: img came from midp_image_create (REGISTERED peer) — the
         * bare free(img) left a stale registry entry (sweep double-free)
         * and leaked the pixel buffer. Use the unregistering destroyer. */
        midp_image_destroy(img);
        return NATIVE_RETURN_NULL();
    }
    
    /* Ensure class has space for nativePeer */
    extern void ensure_native_peer_field(JavaClass* clazz);
    ensure_native_peer_field(image_class);
    
    JavaObject* image = jvm_new_object(jvm, image_class);
    if (!image) {
        midp_image_destroy(img); /* v36.12: unregistering destroy */
        return NATIVE_RETURN_NULL();
    }
    
    /* Store the MidpImage* in the nativePeer field */
    extern void set_object_field_ref(JavaObject* obj, const char* name, JavaObject* value);
    set_object_field_ref(image, "nativePeer", (JavaObject*)img);
    
    if (g_j2me_runtime_debug) fprintf(stderr, "[DirectUtils.createImage] Java Image obj=%p -> native=%p\n", (void*)image, (void*)img);
    fflush(stderr);
    
    return NATIVE_RETURN_OBJECT(image);
}

/* DirectUtils.createImage(int width, int height, int color) - creates image with background */
static JavaValue native_directutils_createImage_color(JVM* jvm, JavaThread* thread,
                                                       JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    jint width = args[0].i;
    jint height = args[1].i;
    jint color = args[2].i;
    
    /* Create native image using midp_image_create from display.c */
    extern MidpImage* midp_image_create(int width, int height, bool mutable);
    MidpImage* img = midp_image_create(width, height, true);
    if (!img) return NATIVE_RETURN_NULL();
    
    /* Fill with color if specified */
    if (img->pixels) {
        uint32_t argb = (uint32_t)color;
        uint8_t a = (argb >> 24) & 0xFF;
        
        if (a == 0) {
            for (int i = 0; i < width * height; i++) {
                img->pixels[i] = 0;
            }
        } else {
            for (int i = 0; i < width * height; i++) {
                img->pixels[i] = argb;
            }
        }
    }
    
    JavaClass* image_class = jvm_load_class(jvm, "javax/microedition/lcdui/Image");
    if (!image_class) {
        midp_image_destroy(img); /* v36.12: unregistering destroy */
        return NATIVE_RETURN_NULL();
    }
    
    /* Ensure class has space for nativePeer */
    extern void ensure_native_peer_field(JavaClass* clazz);
    ensure_native_peer_field(image_class);
    
    JavaObject* image = jvm_new_object(jvm, image_class);
    if (!image) {
        midp_image_destroy(img); /* v36.12: unregistering destroy */
        return NATIVE_RETURN_NULL();
    }
    
    /* Store the MidpImage* in the nativePeer field */
    extern void set_object_field_ref(JavaObject* obj, const char* name, JavaObject* value);
    set_object_field_ref(image, "nativePeer", (JavaObject*)img);
    
    return NATIVE_RETURN_OBJECT(image);
}

/* DirectUtils.getDirectGraphics(Graphics g) - returns DirectGraphics adapter */
static JavaValue native_directutils_getDirectGraphics(JVM* jvm, JavaThread* thread,
                                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* graphics = (JavaObject*)args[0].ref;
    
    /* VERY FIRST - unbuffered marker */
    if (g_j2me_runtime_debug) {
        fprintf(stderr, "=== GET_DIRECT_GRAPHICS_CALLED ===\n");
        fflush(stderr);
    }
    
    if (g_j2me_runtime_debug) fprintf(stderr, "[getDirectGraphics] graphics_obj=%p\n", (void*)graphics);
    fflush(stderr);
    
    if (!graphics) {
        if (g_j2me_runtime_debug) fprintf(stderr, "[getDirectGraphics] RETURN NULL: graphics is NULL\n");
        fflush(stderr);
        return NATIVE_RETURN_NULL();
    }
    
    /* Get the native MidpGraphics to log its details */
    extern MidpGraphics* get_graphics_from_object(JavaObject* obj);
    MidpGraphics* gfx = get_graphics_from_object(graphics);
    if (g_j2me_runtime_debug) fprintf(stderr, "[getDirectGraphics] native gfx=%p (%dx%d, pixels=%p)\n",
            (void*)gfx, gfx ? gfx->width : 0, gfx ? gfx->height : 0, gfx ? (void*)gfx->pixels : NULL);
    fflush(stderr);
    
    /* Create DirectGraphics object */
    JavaClass* dg_class = jvm_load_class(jvm, "com/nokia/mid/ui/DirectGraphics");
    if (!dg_class) {
        if (g_j2me_runtime_debug) fprintf(stderr, "[getDirectGraphics] RETURN NULL: failed to load DirectGraphics class\n");
        fflush(stderr);
        return NATIVE_RETURN_NULL();
    }
    
    /* NOTE: DirectGraphics class already has 'graphics' field from stubs.c - don't call ensure_native_peer_field */
    
    JavaObject* dg = jvm_new_object(jvm, dg_class);
    
    if (dg) {
        /* Store the Graphics object in 'graphics' field (as defined in stubs.c) */
        extern void set_object_field_ref(JavaObject* obj, const char* name, JavaObject* value);
        set_object_field_ref(dg, "graphics", graphics);
        if (g_j2me_runtime_debug) fprintf(stderr, "[getDirectGraphics] Created DirectGraphics obj=%p wrapping Graphics %p\n",
                (void*)dg, (void*)graphics);
        fflush(stderr);
    } else {
        if (g_j2me_runtime_debug) fprintf(stderr, "[getDirectGraphics] RETURN NULL: failed to create DirectGraphics object\n");
        fflush(stderr);
    }
    
    return NATIVE_RETURN_OBJECT(dg);
}

/* DeviceControl.setLights(int num, int level) - control device lights */
/* ---- v17: DeviceControl with backend hooks (see display.c) ---- */

extern void midp_input_activity_mark(void);
extern jlong midp_input_inactivity_ms(void);
extern void midp_input_inactivity_reset(void);
extern bool midp_vibra_start(int freq, int duration_ms);
extern bool midp_vibra_stop(void);
extern int midp_lights_set(int num, int level);

/* DeviceControl.setLights(int num, int level) - backlight control.
 * Spec: IllegalArgumentException if num < 0 or level outside 0..100. */
static JavaValue native_devicecontrol_setLights(JVM* jvm, JavaThread* thread,
                                                  JavaValue* args, int arg_count) {
    (void)jvm; (void)arg_count;
    /* STATIC METHOD - args[0] = num, args[1] = level */
    jint num = args[0].i;
    jint level = args[1].i;

    if (num < 0 || level < 0 || level > 100) {
        jvm_throw_by_name(jvm, "java/lang/IllegalArgumentException", "setLights out of range");
        if (thread) thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_VOID();
    }

    midp_lights_set(num, level);
    return NATIVE_RETURN_VOID();
}

/* DeviceControl.startVibra(int frequency, long duration) -> boolean.
 * Real vibration where the platform provides it (libretro rumble),
 * false when unsupported so games can fall back gracefully. */
static JavaValue native_devicecontrol_startVibra(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jint frequency = args[1].i;
    jlong duration = args[2].j;

    /* Spec: IllegalArgumentException for negative values */
    if (frequency < 0 || duration < 0) {
        jvm_throw_by_name(jvm, "java/lang/IllegalArgumentException", "startVibra out of range");
        if (thread) thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_INT(0);
    }

    int ok = midp_vibra_start(frequency, (int)(duration > 60000 ? 60000 : duration));
    return NATIVE_RETURN_INT(ok ? 1 : 0);
}

/* DeviceControl.stopVibra() -> boolean */
static JavaValue native_devicecontrol_stopVibra(JVM* jvm, JavaThread* thread,
                                                  JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count; (void)args;
    int ok = midp_vibra_stop();
    return NATIVE_RETURN_INT(ok ? 1 : 0);
}

/* DeviceControl.flashLights(long duration) - flash lights + vibra */
static JavaValue native_devicecontrol_flashLights(JVM* jvm, JavaThread* thread,
                                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jlong duration = args[1].j;

    if (duration < 0) {
        jvm_throw_by_name(jvm, "java/lang/IllegalArgumentException", "flashLights out of range");
        if (thread) thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_VOID();
    }

    /* Best-effort: a short rumble doubles as the "flash" notification */
    midp_vibra_start(0, (int)(duration > 60000 ? 60000 : duration));
    midp_lights_set(0, 100);
    return NATIVE_RETURN_VOID();
}

/* DeviceControl.getUserInactivityTime() - seconds since last user input */
static JavaValue native_devicecontrol_getUserInactivityTime(JVM* jvm, JavaThread* thread,
                                                              JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count; (void)args;
    jlong ms = midp_input_inactivity_ms();
    return NATIVE_RETURN_INT((jint)(ms / 1000));
}

/* DeviceControl.resetUserInactivityTime() */
static JavaValue native_devicecontrol_resetUserInactivityTime(JVM* jvm, JavaThread* thread,
                                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count; (void)args;
    midp_input_inactivity_reset();
    return NATIVE_RETURN_VOID();
}

/* ---- v17: Clipboard with real buffer (see display.c) ---- */

extern void midp_clipboard_copy(const char* text);
extern const char* midp_clipboard_paste(void);

/* Clipboard.copyToClipboard(String) */
static JavaValue native_clipboard_copyTo(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaString* text = (JavaString*)args[1].ref;
    if (!text) {
        jvm_throw_by_name(jvm, "java/lang/NullPointerException", NULL);
        if (thread) thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_VOID();
    }
    const char* utf8 = string_utf8(jvm, text);
    midp_clipboard_copy(utf8);
    return NATIVE_RETURN_VOID();
}

/* Clipboard.copyFromClipboard() -> String (null when empty) */
static JavaValue native_clipboard_copyFrom(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count; (void)args;
    const char* text = midp_clipboard_paste();
    if (!text || !text[0]) return NATIVE_RETURN_NULL();
    return NATIVE_RETURN_OBJECT((JavaObject*)jvm_new_string(jvm, text));
}

void init_nokia_ui(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"com/nokia/mid/ui/DirectUtils", "createImage", "(II)Ljavax/microedition/lcdui/Image;", native_directutils_createImage},
        {"com/nokia/mid/ui/DirectUtils", "createImage", "(III)Ljavax/microedition/lcdui/Image;", native_directutils_createImage_color},
        {"com/nokia/mid/ui/DirectUtils", "getDirectGraphics", "(Ljavax/microedition/lcdui/Graphics;)Lcom/nokia/mid/ui/DirectGraphics;", native_directutils_getDirectGraphics},
        {"com/nokia/mid/ui/DeviceControl", "setLights", "(II)V", native_devicecontrol_setLights},
        {"com/nokia/mid/ui/DeviceControl", "startVibra", "(IJ)Z", native_devicecontrol_startVibra},
        {"com/nokia/mid/ui/DeviceControl", "startVibra", "(IJ)V", native_devicecontrol_startVibra},
        {"com/nokia/mid/ui/DeviceControl", "stopVibra", "()Z", native_devicecontrol_stopVibra},
        {"com/nokia/mid/ui/DeviceControl", "stopVibra", "()V", native_devicecontrol_stopVibra},
        {"com/nokia/mid/ui/DeviceControl", "flashLights", "(J)V", native_devicecontrol_flashLights},
        {"com/nokia/mid/ui/DeviceControl", "getUserInactivityTime", "()I", native_devicecontrol_getUserInactivityTime},
        {"com/nokia/mid/ui/DeviceControl", "resetUserInactivityTime", "()V", native_devicecontrol_resetUserInactivityTime},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
    NATIVE_DEBUG("Registered Nokia UI native methods");
}

/* =========================================================================
 * v17: com.nokia.mid.ui.SoftNotification - real implementation
 *
 * State lives in the Java object (id/text/image/listener fields) plus a
 * native posted-flag table. Behavior in this emulator: post() marks the
 * notification active and delivers notificationSelected(SoftNotification)
 * to the registered listener immediately (auto-confirm), since there is no
 * system notification center to route through. remove() clears the state.
 * SoftNotificationException is thrown when posting invalid state.
 * ========================================================================= */

#define SOFTNOTIF_MAX_IDS 16
static int g_softnotif_posted[SOFTNOTIF_MAX_IDS];
static int g_softnotif_next_id = 1;

static JavaValue native_softnotification_getInstance(JVM* jvm, JavaThread* thread,
                                                     JavaValue* args, int arg_count) {
    (void)args; (void)arg_count;
    JavaClass* clazz = jvm_load_class(jvm, "com/nokia/mid/ui/SoftNotification");
    if (!clazz) {
        jvm_throw_by_name(jvm, "com/nokia/mid/ui/SoftNotificationException", "class not found");
        if (thread) thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_NULL();
    }
    JavaObject* obj = jvm_new_object(jvm, clazz);
    if (!obj) return NATIVE_RETURN_NULL();
    /* id stays 0 until post() assigns one */
    return NATIVE_RETURN_OBJECT(obj);
}

static JavaValue native_softnotification_getInstance_id(JVM* jvm, JavaThread* thread,
                                                        JavaValue* args, int arg_count) {
    (void)arg_count;
    JavaClass* clazz = jvm_load_class(jvm, "com/nokia/mid/ui/SoftNotification");
    if (!clazz) {
        jvm_throw_by_name(jvm, "com/nokia/mid/ui/SoftNotificationException", "class not found");
        if (thread) thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_NULL();
    }
    JavaObject* obj = jvm_new_object(jvm, clazz);
    if (!obj) return NATIVE_RETURN_NULL();
    JavaValue id_val = { .i = args[1].i };
    native_set_field_value(obj, "id", id_val);
    return NATIVE_RETURN_OBJECT(obj);
}

static JavaValue native_softnotification_post(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    if (!obj) {
        jvm_throw_by_name(jvm, "java/lang/NullPointerException", NULL);
        if (thread) thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_VOID();
    }

    /* Assign an id on first post */
    jint id = native_get_field_value(obj, "id").i;
    if (id <= 0) {
        id = g_softnotif_next_id++;
        if (g_softnotif_next_id > SOFTNOTIF_MAX_IDS) g_softnotif_next_id = 1;
        JavaValue id_val = { .i = id };
        native_set_field_value(obj, "id", id_val);
    }
    g_softnotif_posted[id % SOFTNOTIF_MAX_IDS] = 1;

    /* Deliver notificationSelected immediately (auto-confirm) */
    JavaObject* listener = (JavaObject*)native_get_field_value(obj, "listener").ref;
    if (listener && heap_java_object_valid(listener)) {
        JavaValue cb_args[1];
        cb_args[0].ref = obj;
        JavaValue cb_result;
        memset(&cb_result, 0, sizeof(cb_result));
        jvm_invoke_virtual(jvm, listener, "notificationSelected",
            "(Lcom/nokia/mid/ui/SoftNotification;)V", cb_args, &cb_result);
    }

    return NATIVE_RETURN_VOID();
}

static JavaValue native_softnotification_remove(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)jvm;
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    if (!obj) return NATIVE_RETURN_VOID();
    jint id = native_get_field_value(obj, "id").i;
    if (id > 0) {
        g_softnotif_posted[id % SOFTNOTIF_MAX_IDS] = 0;
    }
    return NATIVE_RETURN_VOID();
}

static JavaValue native_softnotification_setListener(JVM* jvm, JavaThread* thread,
                                                     JavaValue* args, int arg_count) {
    (void)jvm;
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaObject* listener = (JavaObject*)args[1].ref;
    if (!obj) return NATIVE_RETURN_VOID();
    JavaValue l_val = { .ref = listener };
    native_set_field_value(obj, "listener", l_val);
    return NATIVE_RETURN_VOID();
}

static JavaValue native_softnotification_setText(JVM* jvm, JavaThread* thread,
                                                 JavaValue* args, int arg_count) {
    (void)jvm;
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaObject* text = (JavaObject*)args[1].ref;
    if (!obj) return NATIVE_RETURN_VOID();
    JavaValue t_val = { .ref = text };
    native_set_field_value(obj, "text", t_val);
    return NATIVE_RETURN_VOID();
}

static JavaValue native_softnotification_setImage(JVM* jvm, JavaThread* thread,
                                                  JavaValue* args, int arg_count) {
    (void)jvm;
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaObject* image = (JavaObject*)args[1].ref;
    if (!obj) return NATIVE_RETURN_VOID();
    JavaValue i_val = { .ref = image };
    native_set_field_value(obj, "image", i_val);
    return NATIVE_RETURN_VOID();
}

static JavaValue native_softnotification_getId(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    if (!obj) return NATIVE_RETURN_INT(0);
    return NATIVE_RETURN_INT(native_get_field_value(obj, "id").i);
}

/* Nokia misc API stubs: Clipboard */
void init_nokia_misc(JVM* jvm) {
    /* v17: Clipboard with a real buffer (optionally bridged to the OS
     * clipboard by the platform backend via midp_set_clipboard_hooks) */
    NativeMethodEntry methods[] = {
        {"com/nokia/mid/ui/Clipboard", "copyToClipboard", "(Ljava/lang/String;)V", native_clipboard_copyTo},
        {"com/nokia/mid/ui/Clipboard", "copyFromClipboard", "()Ljava/lang/String;", native_clipboard_copyFrom},

        /* v17: SoftNotification (com.nokia.mid.ui) - real implementation.
         * Notifications are tracked natively; the listener is notified
         * right after post() (auto-confirm semantics of this emulator). */
        {"com/nokia/mid/ui/SoftNotification", "getInstance",
         "()Lcom/nokia/mid/ui/SoftNotification;", native_softnotification_getInstance},
        {"com/nokia/mid/ui/SoftNotification", "getInstance",
         "(I)Lcom/nokia/mid/ui/SoftNotification;", native_softnotification_getInstance_id},
        {"com/nokia/mid/ui/SoftNotification", "post", "()V", native_softnotification_post},
        {"com/nokia/mid/ui/SoftNotification", "post", "([B)V", native_softnotification_post},
        {"com/nokia/mid/ui/SoftNotification", "remove", "()V", native_softnotification_remove},
        {"com/nokia/mid/ui/SoftNotification", "setListener",
         "(Lcom/nokia/mid/ui/SoftNotificationListener;)V", native_softnotification_setListener},
        {"com/nokia/mid/ui/SoftNotification", "setSoftNotificationText",
         "(Ljava/lang/String;)V", native_softnotification_setText},
        {"com/nokia/mid/ui/SoftNotification", "setSoftNotificationImage",
         "(Ljavax/microedition/lcdui/Image;)V", native_softnotification_setImage},
        {"com/nokia/mid/ui/SoftNotification", "getId", "()I", native_softnotification_getId},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
    
    /* Nokia M3D is now handled by init_nokia_m3d_impl() in nokia_m3d.c */
    
    NATIVE_DEBUG("Registered Nokia misc native methods");
}

/*
 * com.nokia.mid.ui.DirectGraphics native methods
 * Nokia's extended graphics API
 */

/* DirectGraphics.drawImage(Image img, int x, int y, int anchor, int manipulation) */
static JavaValue native_directgraphics_drawImage(JVM* jvm, JavaThread* thread,
                                                  JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    
    /* Debug marker to verify function is called */
    if (g_j2me_runtime_debug) {
        fprintf(stderr, "=== DG_DRAW_IMAGE_CALLED ===\n");
        fflush(stderr);
    }
    
    JavaObject* dg = (JavaObject*)args[0].ref;
    JavaObject* image = (JavaObject*)args[1].ref;
    jint x = args[2].i;
    jint y = args[3].i;
    jint anchor = args[4].i;
    jint manipulation = args[5].i;
    
    if (g_j2me_runtime_debug) fprintf(stderr, "[DG.drawImage] dg=%p, image=%p, x=%d, y=%d, anchor=%d, manip=%d\n",
            (void*)dg, (void*)image, x, y, anchor, manipulation);
    fflush(stderr);
    
    if (!dg || !image) {
        if (g_j2me_runtime_debug) fprintf(stderr, "[DG.drawImage] EARLY RETURN: dg or image is NULL\n");
        fflush(stderr);
        return NATIVE_RETURN_VOID();
    }
    
    /* Get the wrapped Graphics object from DirectGraphics.graphics field */
    JavaObject* graphics_obj = get_object_field_ref(dg, "graphics");
    if (g_j2me_runtime_debug) fprintf(stderr, "[DG.drawImage] graphics_obj (from 'graphics' field) = %p\n", (void*)graphics_obj);
    if (!graphics_obj) {
        if (g_j2me_runtime_debug) fprintf(stderr, "[DG.drawImage] EARLY RETURN: graphics_obj is NULL\n");
        return NATIVE_RETURN_VOID();
    }
    
    /* Get the native MidpGraphics from Graphics.nativePeer */
    extern MidpGraphics* get_graphics_from_object(JavaObject* obj);
    MidpGraphics* gfx = get_graphics_from_object(graphics_obj);
    if (g_j2me_runtime_debug) fprintf(stderr, "[DG.drawImage] gfx=%p (width=%d, height=%d, pixels=%p)\n",
            (void*)gfx, gfx ? gfx->width : 0, gfx ? gfx->height : 0, gfx ? (void*)gfx->pixels : NULL);
    
    /* Check if this is screen graphics or offscreen image */
    extern MidpGraphics* sdl_get_graphics(SdlContext* ctx);
    SdlContext* sdl_ctx = sdl_get_global_context();
    MidpGraphics* screen_gfx = sdl_ctx ? sdl_get_graphics(sdl_ctx) : NULL;
    bool is_screen = (gfx == screen_gfx);
    
    if (!gfx || !gfx->pixels) {
        if (g_j2me_runtime_debug) fprintf(stderr, "[DG.drawImage] EARLY RETURN: gfx or gfx->pixels is NULL\n");
        return NATIVE_RETURN_VOID();
    }
    
    /* Get the MidpImage from Image.nativePeer */
    extern MidpImage* get_image_from_object(JavaObject* obj);
    MidpImage* img = get_image_from_object(image);
    if (g_j2me_runtime_debug) fprintf(stderr, "[DG.drawImage] img=%p (%dx%d, pixels=%p)\n",
            (void*)img, img ? img->width : 0, img ? img->height : 0, img ? (void*)img->pixels : NULL);
    if (!img || !img->pixels) {
        if (g_j2me_runtime_debug) fprintf(stderr, "[DG.drawImage] EARLY RETURN: img or img->pixels is NULL\n");
        return NATIVE_RETURN_VOID();
    }
    
    if (g_j2me_runtime_debug) fprintf(stderr, "[DG.drawImage] Drawing %dx%d image to %dx%d graphics at (%d,%d)\n",
            img->width, img->height, gfx->width, gfx->height, x, y);
    
    /* Nokia DirectGraphics manipulation values:
     * 0 = NONE
     * 90 = ROTATE_90 (clockwise)
     * 180 = ROTATE_180
     * 270 = ROTATE_270 (clockwise, or 90 counter-clockwise)
     * 0x2000 (8192) = FLIP_HORIZONTAL
     * 0x4000 (16384) = FLIP_VERTICAL
     * 
     * MIDP Sprite transforms:
     * 0 = TRANS_NONE
     * 1 = TRANS_MIRROR_ROT180 (mirror horizontal)
     * 2 = TRANS_MIRROR (mirror vertical)
     * 3 = TRANS_ROT180
     * 4 = TRANS_MIRROR_ROT270
     * 5 = TRANS_ROT90
     * 6 = TRANS_ROT270
     * 7 = TRANS_MIRROR_ROT90
     */
    int transform = 0;
    switch (manipulation) {
        case 0:     transform = 0; break;  /* NONE */
        case 90:    transform = 5; break;  /* ROTATE_90 -> TRANS_ROT90 */
        case 180:   transform = 3; break;  /* ROTATE_180 -> TRANS_ROT180 */
        case 270:   transform = 6; break;  /* ROTATE_270 -> TRANS_ROT270 */
        case 8192:  transform = 2; break;  /* FLIP_HORIZONTAL -> TRANS_MIRROR (vertical flip in MIDP) */
        case 16384: transform = 1; break;  /* FLIP_VERTICAL -> TRANS_MIRROR_ROT180 (horizontal flip in MIDP) */
        case 24576: transform = 3; break;  /* FLIP_HORIZONTAL | FLIP_VERTICAL = ROTATE_180 */
        default:    
            if (g_j2me_runtime_debug) fprintf(stderr, "[DG.drawImage] Unknown manipulation %d, using NONE\n", manipulation);
            transform = 0; 
            break;
    }
    
    if (g_j2me_runtime_debug) fprintf(stderr, "[DG.drawImage] manip %d -> MIDP transform %d\n", manipulation, transform);
    
    /* Use midp_graphics_draw_region with full image */
    extern void midp_graphics_draw_region(MidpGraphics* gfx, MidpImage* img,
                                          int x_src, int y_src, int w, int h,
                                          int transform, int x_dest, int y_dest, int anchor);
    midp_graphics_draw_region(gfx, img, 0, 0, img->width, img->height, transform, x, y, anchor);
    
    /* If drawing to screen, request redraw */
    if (is_screen) {
        extern void sdl_request_redraw(void);
        sdl_request_redraw();
    }
    
    return NATIVE_RETURN_VOID();
}

/* DirectGraphics.setARGBColor(int argb) */
static JavaValue native_directgraphics_setARGBColor(JVM* jvm, JavaThread* thread,
                                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* dg = (JavaObject*)args[0].ref;
    jint argb = args[1].i;
    
    if (!dg) return NATIVE_RETURN_VOID();
    
    /* Get the wrapped Graphics object from DirectGraphics.graphics field */
    JavaObject* graphics_obj = get_object_field_ref(dg, "graphics");
    if (!graphics_obj) return NATIVE_RETURN_VOID();
    
    /* Get the native graphics context */
    extern MidpGraphics* get_graphics_from_object(JavaObject* obj);
    MidpGraphics* gfx = get_graphics_from_object(graphics_obj);
    if (!gfx) return NATIVE_RETURN_VOID();
    
    /* Set ARGB color - extract RGB and alpha */
    gfx->rgb_color = argb & 0x00FFFFFF;  /* RGB part */
    gfx->alpha = (argb >> 24) & 0xFF;     /* Alpha part */
    
    /* Store alpha component for getAlphaComponent() */
    JavaValue alpha_val = { .i = (argb >> 24) & 0xFF };
    native_set_field_value(dg, "alphaComponent", alpha_val);
    
    return NATIVE_RETURN_VOID();
}

/* DirectGraphics.getAlphaComponent() - returns alpha channel */
static JavaValue native_directgraphics_getAlphaComponent(JVM* jvm, JavaThread* thread,
                                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* dg = (JavaObject*)args[0].ref;
    if (!dg) return NATIVE_RETURN_INT(255);
    
    /* Return stored alpha component */
    JavaValue alpha_val = native_get_field_value(dg, "alphaComponent");
    return NATIVE_RETURN_INT(alpha_val.i);
}

/* DirectGraphics.getNativePixelFormat() - returns pixel format */
static JavaValue native_directgraphics_getNativePixelFormat(JVM* jvm, JavaThread* thread,
                                                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count; (void)args;
    /* Return TYPE_INT_8888_ARGB = 8888 (Nokia DirectGraphics constant) */
    return NATIVE_RETURN_INT(8888);
}

/* ---- v17: DirectGraphics pixel format conversion helpers ----
 *
 * Nokia DirectGraphics formats (numeric constants used across games):
 *   8888 = TYPE_INT_8888_ARGB (0xAARRGGBB in an int)
 *   888  = TYPE_INT_888_RGB   (alpha forced opaque)
 *   4444 = TYPE_USHORT_4444_ARGB (AAAA RRRR GGGG BBBB in a short)
 *   444  = TYPE_USHORT_444_RGB
 *   555  = TYPE_USHORT_555_RGB
 *   565  = TYPE_USHORT_565_RGB
 *   1    = TYPE_BYTE_1_GRAY: 1 bit per pixel, 8 pixels per byte, MSB first
 */

static uint32_t dg_int_to_argb(uint32_t px, jint format) {
    switch (format) {
        case 888:  return 0xFF000000u | (px & 0x00FFFFFFu);
        case 565: {
            uint32_t r = (px >> 11) & 0x1F, g = (px >> 5) & 0x3F, b = px & 0x1F;
            return 0xFF000000u | ((r * 255 / 31) << 16) | ((g * 255 / 63) << 8) | (b * 255 / 31);
        }
        case 555: {
            uint32_t r = (px >> 10) & 0x1F, g = (px >> 5) & 0x1F, b = px & 0x1F;
            return 0xFF000000u | ((r * 255 / 31) << 16) | ((g * 255 / 31) << 8) | (b * 255 / 31);
        }
        case 4444: {
            uint32_t a = (px >> 12) & 0xF, r = (px >> 8) & 0xF, g = (px >> 4) & 0xF, b = px & 0xF;
            return ((a * 255 / 15) << 24) | ((r * 255 / 15) << 16) | ((g * 255 / 15) << 8) | (b * 255 / 15);
        }
        case 444: {
            uint32_t r = (px >> 8) & 0xF, g = (px >> 4) & 0xF, b = px & 0xF;
            return 0xFF000000u | ((r * 255 / 15) << 16) | ((g * 255 / 15) << 8) | (b * 255 / 15);
        }
        case 8888:
        default:   return px;
    }
}

static uint32_t dg_argb_to_short(uint32_t argb, jint format) {
    uint32_t r = (argb >> 16) & 0xFF, g = (argb >> 8) & 0xFF, b = argb & 0xFF, a = (argb >> 24) & 0xFF;
    switch (format) {
        case 565: return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
        case 555: return ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
        case 4444: return ((a >> 4) << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4);
        case 444: return ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4);
        default: return argb & 0xFFFF;
    }
}

/* Destination coordinate with Nokia manipulation (rotation + flips) */
static void dg_manip_xy(jint manipulation, int px, int py, int width, int height,
                        int* dx, int* dy) {
    int rotation = manipulation & 0x0FFF;
    int flip_h = (manipulation & 0x2000) != 0;
    int flip_v = (manipulation & 0x4000) != 0;

    switch (rotation) {
        case 90:  *dx = height - 1 - py; *dy = px; break;
        case 180: *dx = width - 1 - px;  *dy = height - 1 - py; break;
        case 270: *dx = py;              *dy = width - 1 - px; break;
        default:  *dx = px;              *dy = py; break;
    }
    if (flip_h) *dx = width - 1 - *dx;
    if (flip_v) *dy = height - 1 - *dy;
}

/* Draw an ARGB pixel with alpha blending into the graphics buffer */
static void dg_blend_pixel(MidpGraphics* gfx, int dst_x, int dst_y,
                           uint32_t src_color, bool transparency) {
    uint8_t alpha = (src_color >> 24) & 0xFF;
    if (transparency && alpha == 0) return;

    if (dst_x < gfx->clip_x || dst_x >= gfx->clip_x + gfx->clip_width) return;
    if (dst_y < gfx->clip_y || dst_y >= gfx->clip_y + gfx->clip_height) return;
    if (dst_x < 0 || dst_x >= gfx->width || dst_y < 0 || dst_y >= gfx->height) return;

    if (alpha == 255 || !transparency) {
        gfx->pixels[dst_y * gfx->width + dst_x] = src_color | 0xFF000000u;
    } else {
        uint32_t dst_color = gfx->pixels[dst_y * gfx->width + dst_x];
        uint8_t inv_alpha = 255 - alpha;
        uint8_t r = ((src_color >> 16) & 0xFF) * alpha / 255 + ((dst_color >> 16) & 0xFF) * inv_alpha / 255;
        uint8_t g = ((src_color >> 8) & 0xFF) * alpha / 255 + ((dst_color >> 8) & 0xFF) * inv_alpha / 255;
        uint8_t b = (src_color & 0xFF) * alpha / 255 + (dst_color & 0xFF) * inv_alpha / 255;
        gfx->pixels[dst_y * gfx->width + dst_x] = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
}

/* Fetch the MidpGraphics wrapped by a DirectGraphics object */
static MidpGraphics* dg_get_gfx(JavaObject* dg) {
    if (!dg) return NULL;
    JavaObject* graphics_obj = get_object_field_ref(dg, "graphics");
    if (!graphics_obj) return NULL;
    extern MidpGraphics* get_graphics_from_object(JavaObject* obj);
    MidpGraphics* gfx = get_graphics_from_object(graphics_obj);
    if (!gfx || !gfx->pixels) return NULL;
    return gfx;
}

/* DirectGraphics.drawPixels(int[] pixels, boolean transparency, int offset, int scanlength, 
                              int x, int y, int width, int height, int manipulation, int format) */
static JavaValue native_directgraphics_drawPixels(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    /* v38: [V37-DIAG] drawPixels breadcrumb removed — use
     * NOJME_INVOKE_TRACE=DirectGraphics to see the software pipeline. */
    JavaObject* dg = (JavaObject*)args[0].ref;
    JavaArray* pixels = (JavaArray*)args[1].ref;
    jboolean transparency = args[2].i;
    jint offset = args[3].i;
    jint scanlength = args[4].i;
    jint x = args[5].i;
    jint y = args[6].i;
    jint width = args[7].i;
    jint height = args[8].i;
    jint manipulation = args[9].i;
    jint format = args[10].i;
    /* v17: the format parameter is now honored (8888/888/565/555/4444/444) */

    MidpGraphics* gfx = dg_get_gfx(dg);
    if (!gfx || !pixels || width <= 0 || height <= 0) return NATIVE_RETURN_VOID();

    /* Get pixel data from array */
    if (pixels->element_type != T_INT) return NATIVE_RETURN_VOID();
    jint* pixel_data = (jint*)array_data(pixels);
    if (!pixel_data) return NATIVE_RETURN_VOID();
    
    /* Default scanlength to width if not specified */
    if (scanlength <= 0) scanlength = width;
    
    /* Draw pixels with manipulation support */
    for (int py = 0; py < height; py++) {
        for (int px = 0; px < width; px++) {
            int src_idx = offset + py * scanlength + px;
            if (src_idx < 0 || src_idx >= (int)pixels->length) continue;
            
            uint32_t src_color = dg_int_to_argb((uint32_t)pixel_data[src_idx], format);
            
            /* Handle transparency */
            uint8_t alpha = (src_color >> 24) & 0xFF;
            if (transparency && alpha == 0) continue;
            
            /* Calculate destination coordinates with Nokia manipulation constants */
            int rotation = manipulation & 0x0FFF;
            int flip_h = (manipulation & 0x2000) != 0;  /* FLIP_HORIZONTAL = 8192 = 0x2000 */
            int flip_v = (manipulation & 0x4000) != 0;  /* FLIP_VERTICAL = 16384 = 0x4000 */
            
            int dx, dy;
            /* Nokia rotation is counter-clockwise, convert to coordinate mapping */
            switch (rotation) {
                case 0:   dx = px; dy = py; break;
                case 90:  dx = height - 1 - py; dy = px; break;
                case 180: dx = width - 1 - px; dy = height - 1 - py; break;
                case 270: dx = py; dy = width - 1 - px; break;
                default:  dx = px; dy = py; break;
            }
            
            if (flip_h) dx = width - 1 - dx;
            if (flip_v) dy = height - 1 - dy;
            
            int dst_x = x + dx + gfx->translate_x;
            int dst_y = y + dy + gfx->translate_y;
            
            /* Clip check */
            if (dst_x < gfx->clip_x || dst_x >= gfx->clip_x + gfx->clip_width) continue;
            if (dst_y < gfx->clip_y || dst_y >= gfx->clip_y + gfx->clip_height) continue;
            if (dst_x < 0 || dst_x >= gfx->width || dst_y < 0 || dst_y >= gfx->height) continue;
            
            /* Alpha blending */
            if (alpha == 255 || !transparency) {
                gfx->pixels[dst_y * gfx->width + dst_x] = src_color | 0xFF000000;
            } else if (alpha > 0) {
                uint32_t dst_color = gfx->pixels[dst_y * gfx->width + dst_x];
                uint8_t inv_alpha = 255 - alpha;
                
                uint8_t r = ((src_color >> 16) & 0xFF) * alpha / 255 + 
                           ((dst_color >> 16) & 0xFF) * inv_alpha / 255;
                uint8_t g = ((src_color >> 8) & 0xFF) * alpha / 255 +
                           ((dst_color >> 8) & 0xFF) * inv_alpha / 255;
                uint8_t b = (src_color & 0xFF) * alpha / 255 +
                           (dst_color & 0xFF) * inv_alpha / 255;
                
                gfx->pixels[dst_y * gfx->width + dst_x] = ((uint32_t)(0xFF) <<  24) | (r << 16) | (g << 8) | b;
            }
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* DirectGraphics.drawPolygon(int[] xPoints, int xOffset, int[] yPoints, int yOffset, 
                               int nPoints, int argbColor) */
static JavaValue native_directgraphics_drawPolygon(JVM* jvm, JavaThread* thread,
                                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* dg = (JavaObject*)args[0].ref;
    JavaArray* xPoints = (JavaArray*)args[1].ref;
    jint xOffset = args[2].i;
    JavaArray* yPoints = (JavaArray*)args[3].ref;
    jint yOffset = args[4].i;
    jint nPoints = args[5].i;
    jint argbColor = args[6].i;
    
    if (!dg || !xPoints || !yPoints || nPoints < 2) return NATIVE_RETURN_VOID();
    
    /* Get graphics context */
    JavaObject* graphics_obj = get_object_field_ref(dg, "graphics");
    if (!graphics_obj) return NATIVE_RETURN_VOID();
    
    extern MidpGraphics* get_graphics_from_object(JavaObject* obj);
    MidpGraphics* gfx = get_graphics_from_object(graphics_obj);
    if (!gfx || !gfx->pixels) return NATIVE_RETURN_VOID();
    
    /* Get point arrays */
    if (xPoints->element_type != T_INT || yPoints->element_type != T_INT) return NATIVE_RETURN_VOID();
    jint* x_data = (jint*)array_data(xPoints);
    jint* y_data = (jint*)array_data(yPoints);
    if (!x_data || !y_data) return NATIVE_RETURN_VOID();
    
    /* [BT-CRASH-FIX] offset/len guard: unbounded x_data[xOffset+i] reads
     * walked past the Java int[] into adjacent heap blocks (garbage
     * coordinates / arena-exit reads). Clamp nPoints to what both arrays
     * actually hold from the given offsets. */
    {
        jint max_n = xPoints->length - xOffset;
        if (max_n > (jint)(yPoints->length - yOffset)) max_n = (jint)(yPoints->length - yOffset);
        if (xOffset < 0 || yOffset < 0 || max_n < 2) return NATIVE_RETURN_VOID();
        if (nPoints > (int)max_n) nPoints = (int)max_n;
        if (nPoints < 2) return NATIVE_RETURN_VOID();
    }
    
    /* Draw polygon edges using line drawing */
    extern void midp_graphics_draw_line(MidpGraphics* gfx, int x1, int y1, int x2, int y2);
    
    /* Store original color and set new color */
    uint32_t orig_color = gfx->rgb_color;
    uint8_t orig_alpha = gfx->alpha;
    gfx->rgb_color = argbColor & 0x00FFFFFF;
    gfx->alpha = (argbColor >> 24) & 0xFF;
    if (gfx->alpha == 0) gfx->alpha = 255;
    
    for (int i = 0; i < nPoints - 1; i++) {
        int x1 = x_data[xOffset + i] + gfx->translate_x;
        int y1 = y_data[yOffset + i] + gfx->translate_y;
        int x2 = x_data[xOffset + i + 1] + gfx->translate_x;
        int y2 = y_data[yOffset + i + 1] + gfx->translate_y;
        midp_graphics_draw_line(gfx, x1, y1, x2, y2);
    }
    /* Close the polygon */
    if (nPoints > 2) {
        int x1 = x_data[xOffset + nPoints - 1] + gfx->translate_x;
        int y1 = y_data[yOffset + nPoints - 1] + gfx->translate_y;
        int x2 = x_data[xOffset] + gfx->translate_x;
        int y2 = y_data[yOffset] + gfx->translate_y;
        midp_graphics_draw_line(gfx, x1, y1, x2, y2);
    }
    
    /* Restore color */
    gfx->rgb_color = orig_color;
    gfx->alpha = orig_alpha;
    
    return NATIVE_RETURN_VOID();
}

/* DirectGraphics.fillPolygon(int[] xPoints, int xOffset, int[] yPoints, int yOffset, 
                               int nPoints, int argbColor) */
static JavaValue native_directgraphics_fillPolygon(JVM* jvm, JavaThread* thread,
                                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* dg = (JavaObject*)args[0].ref;
    JavaArray* xPoints = (JavaArray*)args[1].ref;
    jint xOffset = args[2].i;
    JavaArray* yPoints = (JavaArray*)args[3].ref;
    jint yOffset = args[4].i;
    jint nPoints = args[5].i;
    jint argbColor = args[6].i;
    
    if (!dg || !xPoints || !yPoints || nPoints < 3) return NATIVE_RETURN_VOID();
    
    /* Get graphics context */
    JavaObject* graphics_obj = get_object_field_ref(dg, "graphics");
    if (!graphics_obj) return NATIVE_RETURN_VOID();
    
    extern MidpGraphics* get_graphics_from_object(JavaObject* obj);
    MidpGraphics* gfx = get_graphics_from_object(graphics_obj);
    if (!gfx || !gfx->pixels) return NATIVE_RETURN_VOID();
    
    /* Get point arrays */
    if (xPoints->element_type != T_INT || yPoints->element_type != T_INT) return NATIVE_RETURN_VOID();
    jint* x_data = (jint*)array_data(xPoints);
    jint* y_data = (jint*)array_data(yPoints);
    if (!x_data || !y_data) return NATIVE_RETURN_VOID();
    
    /* [BT-CRASH-FIX] offset/len guard (see drawPolygon above). */
    {
        jint max_n = xPoints->length - xOffset;
        if (max_n > (jint)(yPoints->length - yOffset)) max_n = (jint)(yPoints->length - yOffset);
        if (xOffset < 0 || yOffset < 0 || max_n < 3) return NATIVE_RETURN_VOID();
        if (nPoints > (int)max_n) nPoints = (int)max_n;
        if (nPoints < 3) return NATIVE_RETURN_VOID();
    }
    
    /* Simple scanline fill algorithm */
    /* Find bounding box */
    int min_y = y_data[yOffset], max_y = y_data[yOffset];
    for (int i = 1; i < nPoints; i++) {
        int y = y_data[yOffset + i];
        if (y < min_y) min_y = y;
        if (y > max_y) max_y = y;
    }
    
    uint32_t fill_color = (uint32_t)argbColor;  /* Preserve full ARGB including alpha */
    
    /* Scanline fill */
    for (int y = min_y; y <= max_y; y++) {
        int intersections[32];  /* Max 32 intersections per scanline */
        int num_intersections = 0;
        
        /* Find intersections with all edges */
        for (int i = 0; i < nPoints; i++) {
            int j = (i + 1) % nPoints;
            int y1 = y_data[yOffset + i];
            int y2 = y_data[yOffset + j];
            int x1 = x_data[xOffset + i];
            int x2 = x_data[xOffset + j];
            
            if ((y1 <= y && y < y2) || (y2 <= y && y < y1)) {
                /* Edge intersects scanline */
                int x = x1 + (y - y1) * (x2 - x1) / (y2 - y1);
                if (num_intersections < 32) {
                    intersections[num_intersections++] = x;
                }
            }
        }
        
        /* Sort intersections */
        for (int i = 0; i < num_intersections - 1; i++) {
            for (int j = i + 1; j < num_intersections; j++) {
                if (intersections[i] > intersections[j]) {
                    int tmp = intersections[i];
                    intersections[i] = intersections[j];
                    intersections[j] = tmp;
                }
            }
        }
        
        /* Fill between pairs of intersections */
        for (int i = 0; i < num_intersections - 1; i += 2) {
            int x_start = intersections[i] + gfx->translate_x;
            int x_end = intersections[i + 1] + gfx->translate_x;
            int draw_y = y + gfx->translate_y;
            
            for (int x = x_start; x <= x_end; x++) {
                if (x >= gfx->clip_x && x < gfx->clip_x + gfx->clip_width &&
                    draw_y >= gfx->clip_y && draw_y < gfx->clip_y + gfx->clip_height &&
                    x >= 0 && x < gfx->width && draw_y >= 0 && draw_y < gfx->height) {
                    gfx->pixels[draw_y * gfx->width + x] = fill_color;
                }
            }
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* DirectGraphics.setPixels(int[] pixels, int offset, int scanlength, int x, int y, 
                             int width, int height, int format) */
static JavaValue native_directgraphics_setPixels(JVM* jvm, JavaThread* thread,
                                                  JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* dg = (JavaObject*)args[0].ref;
    JavaArray* pixels = (JavaArray*)args[1].ref;
    jint offset = args[2].i;
    jint scanlength = args[3].i;
    jint x = args[4].i;
    jint y = args[5].i;
    jint width = args[6].i;
    jint height = args[7].i;
    jint format = args[8].i;
    /* v17: format honored (8888/888/565/555/4444/444) */

    MidpGraphics* gfx = dg_get_gfx(dg);
    if (!gfx || !pixels || width <= 0 || height <= 0) return NATIVE_RETURN_VOID();
    
    /* Get pixel data */
    if (pixels->element_type != T_INT) return NATIVE_RETURN_VOID();
    jint* pixel_data = (jint*)array_data(pixels);
    if (!pixel_data) return NATIVE_RETURN_VOID();
    
    if (scanlength <= 0) scanlength = width;
    
    /* Copy pixels directly to graphics buffer */
    for (int py = 0; py < height; py++) {
        int dst_y = y + py + gfx->translate_y;
        if (dst_y < gfx->clip_y || dst_y >= gfx->clip_y + gfx->clip_height) continue;
        if (dst_y < 0 || dst_y >= gfx->height) continue;
        
        for (int px = 0; px < width; px++) {
            int src_idx = offset + py * scanlength + px;
            if (src_idx < 0 || src_idx >= (int)pixels->length) continue;
            
            int dst_x = x + px + gfx->translate_x;
            if (dst_x < gfx->clip_x || dst_x >= gfx->clip_x + gfx->clip_width) continue;
            if (dst_x < 0 || dst_x >= gfx->width) continue;
            
            uint32_t color = dg_int_to_argb((uint32_t)pixel_data[src_idx], format);
            gfx->pixels[dst_y * gfx->width + dst_x] = color | 0xFF000000;
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* DirectGraphics.getPixels(int[] pixels, int offset, int scanlength, int x, int y, 
                             int width, int height, int format) */
static JavaValue native_directgraphics_getPixels(JVM* jvm, JavaThread* thread,
                                                  JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* dg = (JavaObject*)args[0].ref;
    JavaArray* pixels = (JavaArray*)args[1].ref;
    jint offset = args[2].i;
    jint scanlength = args[3].i;
    jint x = args[4].i;
    jint y = args[5].i;
    jint width = args[6].i;
    jint height = args[7].i;
    jint format = args[8].i;
    /* v17: format honored on output (8888/888/565/555/4444/444) */

    if (!dg || !pixels || width <= 0 || height <= 0) return NATIVE_RETURN_VOID();

    /* Get graphics context */
    JavaObject* graphics_obj = get_object_field_ref(dg, "graphics");
    if (!graphics_obj) return NATIVE_RETURN_VOID();
    
    extern MidpGraphics* get_graphics_from_object(JavaObject* obj);
    MidpGraphics* gfx = get_graphics_from_object(graphics_obj);
    if (!gfx || !gfx->pixels) return NATIVE_RETURN_VOID();
    
    /* Get pixel data array */
    if (pixels->element_type != T_INT) return NATIVE_RETURN_VOID();
    jint* pixel_data = (jint*)array_data(pixels);
    if (!pixel_data) return NATIVE_RETURN_VOID();
    
    if (scanlength <= 0) scanlength = width;
    
    /* Copy pixels from graphics buffer to array */
    for (int py = 0; py < height; py++) {
        int src_y = y + py + gfx->translate_y;
        
        for (int px = 0; px < width; px++) {
            int dst_idx = offset + py * scanlength + px;
            if (dst_idx < 0 || dst_idx >= (int)pixels->length) continue;
            
            int src_x = x + px + gfx->translate_x;
            
            if (src_x >= 0 && src_x < gfx->width && src_y >= 0 && src_y < gfx->height) {
                pixel_data[dst_idx] = (jint)dg_argb_to_short(
                    gfx->pixels[src_y * gfx->width + src_x], format);
            } else {
                pixel_data[dst_idx] = 0;
            }
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* DirectGraphics.drawTriangle(int x1, int y1, int x2, int y2, int x3, int y3, int argbColor) */
static JavaValue native_directgraphics_drawTriangle(JVM* jvm, JavaThread* thread,
                                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* dg = (JavaObject*)args[0].ref;
    jint x1 = args[1].i;
    jint y1 = args[2].i;
    jint x2 = args[3].i;
    jint y2 = args[4].i;
    jint x3 = args[5].i;
    jint y3 = args[6].i;
    jint argbColor = args[7].i;
    
    if (!dg) return NATIVE_RETURN_VOID();
    
    /* Get graphics context */
    JavaObject* graphics_obj = get_object_field_ref(dg, "graphics");
    if (!graphics_obj) return NATIVE_RETURN_VOID();
    
    extern MidpGraphics* get_graphics_from_object(JavaObject* obj);
    MidpGraphics* gfx = get_graphics_from_object(graphics_obj);
    if (!gfx || !gfx->pixels) return NATIVE_RETURN_VOID();
    
    /* Draw triangle edges */
    extern void midp_graphics_draw_line(MidpGraphics* gfx, int x1, int y1, int x2, int y2);
    
    /* Store and set color and alpha */
    uint32_t orig_color = gfx->rgb_color;
    uint8_t orig_alpha = gfx->alpha;
    gfx->rgb_color = argbColor & 0x00FFFFFF;
    gfx->alpha = (argbColor >> 24) & 0xFF;
    if (gfx->alpha == 0) gfx->alpha = 255;
    
    x1 += gfx->translate_x; y1 += gfx->translate_y;
    x2 += gfx->translate_x; y2 += gfx->translate_y;
    x3 += gfx->translate_x; y3 += gfx->translate_y;
    
    midp_graphics_draw_line(gfx, x1, y1, x2, y2);
    midp_graphics_draw_line(gfx, x2, y2, x3, y3);
    midp_graphics_draw_line(gfx, x3, y3, x1, y1);
    
    gfx->rgb_color = orig_color;
    gfx->alpha = orig_alpha;
    
    return NATIVE_RETURN_VOID();
}

/* DirectGraphics.fillTriangle(int x1, int y1, int x2, int y2, int x3, int y3, int argbColor) */
static JavaValue native_directgraphics_fillTriangle(JVM* jvm, JavaThread* thread,
                                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* dg = (JavaObject*)args[0].ref;
    jint x1 = args[1].i;
    jint y1 = args[2].i;
    jint x2 = args[3].i;
    jint y2 = args[4].i;
    jint x3 = args[5].i;
    jint y3 = args[6].i;
    jint argbColor = args[7].i;
    
    if (!dg) return NATIVE_RETURN_VOID();
    
    /* Get graphics context */
    JavaObject* graphics_obj = get_object_field_ref(dg, "graphics");
    if (!graphics_obj) return NATIVE_RETURN_VOID();
    
    extern MidpGraphics* get_graphics_from_object(JavaObject* obj);
    MidpGraphics* gfx = get_graphics_from_object(graphics_obj);
    if (!gfx || !gfx->pixels) return NATIVE_RETURN_VOID();
    
    uint32_t fill_color = (uint32_t)argbColor;  /* Preserve full ARGB including alpha */
    
    x1 += gfx->translate_x; y1 += gfx->translate_y;
    x2 += gfx->translate_x; y2 += gfx->translate_y;
    x3 += gfx->translate_x; y3 += gfx->translate_y;
    
    /* Sort vertices by Y coordinate */
    if (y1 > y2) { int tmp; tmp = x1; x1 = x2; x2 = tmp; tmp = y1; y1 = y2; y2 = tmp; }
    if (y2 > y3) { int tmp; tmp = x2; x2 = x3; x3 = tmp; tmp = y2; y2 = y3; y3 = tmp; }
    if (y1 > y2) { int tmp; tmp = x1; x1 = x2; x2 = tmp; tmp = y1; y1 = y2; y2 = tmp; }
    
    /* Fill triangle using scanline algorithm */
    for (int y = y1; y <= y3; y++) {
        int xl, xr;
        
        if (y < y2) {
            /* Upper half */
            if (y2 != y1) {
                xl = x1 + (y - y1) * (x2 - x1) / (y2 - y1);
            } else {
                xl = x1;
            }
            if (y3 != y1) {
                xr = x1 + (y - y1) * (x3 - x1) / (y3 - y1);
            } else {
                xr = x3;
            }
        } else {
            /* Lower half */
            if (y3 != y2) {
                xl = x2 + (y - y2) * (x3 - x2) / (y3 - y2);
            } else {
                xl = x2;
            }
            if (y3 != y1) {
                xr = x1 + (y - y1) * (x3 - x1) / (y3 - y1);
            } else {
                xr = x3;
            }
        }
        
        if (xl > xr) { int tmp = xl; xl = xr; xr = tmp; }
        
        /* Draw scanline */
        for (int x = xl; x <= xr; x++) {
            if (x >= gfx->clip_x && x < gfx->clip_x + gfx->clip_width &&
                y >= gfx->clip_y && y < gfx->clip_y + gfx->clip_height &&
                x >= 0 && x < gfx->width && y >= 0 && y < gfx->height) {
                gfx->pixels[y * gfx->width + x] = fill_color;
            }
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* ---- drawPixels(byte[] pixels, byte[] transparencyMask, int offset, int scanlength,
 *                 int x, int y, int width, int height, int manipulation, int format)
 * 1-bit-per-pixel image (Nokia bitmap fonts): 8 pixels per byte, MSB first.
 * A set pixel bit means "pixel present" (drawn with the current color);
 * a clear transparencyMask bit (when the mask is provided) means transparent. */
static JavaValue native_directgraphics_drawPixels_byte(JVM* jvm, JavaThread* thread,
                                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* dg = (JavaObject*)args[0].ref;
    JavaArray* pixels = (JavaArray*)args[1].ref;
    JavaArray* mask = (JavaArray*)args[2].ref;
    jint offset = args[3].i;
    jint scanlength = args[4].i;
    jint x = args[5].i;
    jint y = args[6].i;
    jint width = args[7].i;
    jint height = args[8].i;
    jint manipulation = args[9].i;
    jint format = args[10].i;

    MidpGraphics* gfx = dg_get_gfx(dg);
    if (!gfx || !pixels || width <= 0 || height <= 0) return NATIVE_RETURN_VOID();
    if (pixels->element_type != T_BYTE) return NATIVE_RETURN_VOID();
    if (scanlength <= 0) scanlength = width;

    /* Only the 1-bit format is defined for byte[] */
    (void)format; /* TYPE_BYTE_1_GRAY == 1, other byte formats unsupported */

    uint32_t draw_color = 0xFF000000u | gfx->rgb_color |
                          ((uint32_t)(gfx->alpha ? gfx->alpha : 255) << 24);
    uint8_t* pdata = (uint8_t*)array_data(pixels);
    uint8_t* mdata = (mask && mask->element_type == T_BYTE && mask->length > 0)
                     ? (uint8_t*)array_data(mask) : NULL;

    for (int py = 0; py < height; py++) {
        for (int px = 0; px < width; px++) {
            int src_idx = offset + py * scanlength + px / 8;
            if (src_idx < 0 || src_idx >= (int)pixels->length) continue;

            uint8_t bit = (uint8_t)(0x80u >> (px & 7));
            bool set = (pdata[src_idx] & bit) != 0;
            if (!set) continue;

            if (mdata) {
                int m_idx = offset + py * scanlength + px / 8;
                if (m_idx < 0 || m_idx >= (int)mask->length) continue;
                if (!(mdata[m_idx] & bit)) continue; /* masked out -> transparent */
            }

            int dx, dy;
            dg_manip_xy(manipulation, px, py, width, height, &dx, &dy);
            dg_blend_pixel(gfx, x + dx + gfx->translate_x, y + dy + gfx->translate_y,
                           draw_color, true);
        }
    }

    return NATIVE_RETURN_VOID();
}

/* ---- drawPixels(short[] pixels, boolean transparency, int offset, int scanlength,
 *                 int x, int y, int width, int height, int manipulation, int format)
 * Packed 16-bit formats: 565 / 555 / 4444 / 444. */
static JavaValue native_directgraphics_drawPixels_short(JVM* jvm, JavaThread* thread,
                                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* dg = (JavaObject*)args[0].ref;
    JavaArray* pixels = (JavaArray*)args[1].ref;
    jboolean transparency = args[2].i;
    jint offset = args[3].i;
    jint scanlength = args[4].i;
    jint x = args[5].i;
    jint y = args[6].i;
    jint width = args[7].i;
    jint height = args[8].i;
    jint manipulation = args[9].i;
    jint format = args[10].i;

    MidpGraphics* gfx = dg_get_gfx(dg);
    if (!gfx || !pixels || width <= 0 || height <= 0) return NATIVE_RETURN_VOID();
    if (pixels->element_type != T_SHORT) return NATIVE_RETURN_VOID();
    if (scanlength <= 0) scanlength = width;

    int16_t* pdata = (int16_t*)array_data(pixels);

    for (int py = 0; py < height; py++) {
        for (int px = 0; px < width; px++) {
            int src_idx = offset + py * scanlength + px;
            if (src_idx < 0 || src_idx >= (int)pixels->length) continue;

            uint32_t argb = dg_int_to_argb((uint32_t)(uint16_t)pdata[src_idx], format);
            int dx, dy;
            dg_manip_xy(manipulation, px, py, width, height, &dx, &dy);
            dg_blend_pixel(gfx, x + dx + gfx->translate_x, y + dy + gfx->translate_y,
                           argb, transparency != 0);
        }
    }

    return NATIVE_RETURN_VOID();
}

/* ---- getPixels(byte[] pixels, byte[] transparencyMask, int offset, int scanlength,
 *                int x, int y, int width, int height, int format)
 * Reads 1bpp data back: pixel bit = luminance >= 128, mask bit = opaque. */
static JavaValue native_directgraphics_getPixels_byte(JVM* jvm, JavaThread* thread,
                                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* dg = (JavaObject*)args[0].ref;
    JavaArray* pixels = (JavaArray*)args[1].ref;
    JavaArray* mask = (JavaArray*)args[2].ref;
    jint offset = args[3].i;
    jint scanlength = args[4].i;
    jint x = args[5].i;
    jint y = args[6].i;
    jint width = args[7].i;
    jint height = args[8].i;
    (void)args[9]; /* format: TYPE_BYTE_1_GRAY */

    MidpGraphics* gfx = dg_get_gfx(dg);
    if (!gfx || !pixels || width <= 0 || height <= 0) return NATIVE_RETURN_VOID();
    if (pixels->element_type != T_BYTE) return NATIVE_RETURN_VOID();
    if (scanlength <= 0) scanlength = width;

    uint8_t* pdata = (uint8_t*)array_data(pixels);
    uint8_t* mdata = (mask && mask->element_type == T_BYTE && mask->length > 0)
                     ? (uint8_t*)array_data(mask) : NULL;

    for (int py = 0; py < height; py++) {
        int src_y = y + py + gfx->translate_y;
        for (int px = 0; px < width; px++) {
            int bit_idx = offset + py * scanlength + px;
            int byte_idx = bit_idx / 8;
            if (byte_idx < 0 || byte_idx >= (int)pixels->length) continue;
            uint8_t bit = (uint8_t)(0x80u >> (bit_idx & 7));

            int src_x = x + px + gfx->translate_x;
            uint32_t argb = 0;
            bool opaque = false;
            if (src_x >= 0 && src_x < gfx->width && src_y >= 0 && src_y < gfx->height) {
                argb = gfx->pixels[src_y * gfx->width + src_x];
                opaque = ((argb >> 24) & 0xFF) != 0;
            }

            uint32_t lum = ((argb >> 16) & 0xFF) * 30 + ((argb >> 8) & 0xFF) * 59 +
                           (argb & 0xFF) * 11;
            if (lum / 100 >= 128) pdata[byte_idx] |= bit;
            else pdata[byte_idx] &= (uint8_t)~bit;

            if (mdata && byte_idx < (int)mask->length) {
                if (opaque) mdata[byte_idx] |= bit;
                else mdata[byte_idx] &= (uint8_t)~bit;
            }
        }
    }

    return NATIVE_RETURN_VOID();
}

/* ---- getPixels(short[] pixels, int offset, int scanlength, int x, int y,
 *                int width, int height, int format) */
static JavaValue native_directgraphics_getPixels_short(JVM* jvm, JavaThread* thread,
                                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* dg = (JavaObject*)args[0].ref;
    JavaArray* pixels = (JavaArray*)args[1].ref;
    jint offset = args[2].i;
    jint scanlength = args[3].i;
    jint x = args[4].i;
    jint y = args[5].i;
    jint width = args[6].i;
    jint height = args[7].i;
    jint format = args[8].i;

    MidpGraphics* gfx = dg_get_gfx(dg);
    if (!gfx || !pixels || width <= 0 || height <= 0) return NATIVE_RETURN_VOID();
    if (pixels->element_type != T_SHORT) return NATIVE_RETURN_VOID();
    if (scanlength <= 0) scanlength = width;

    int16_t* pdata = (int16_t*)array_data(pixels);

    for (int py = 0; py < height; py++) {
        int src_y = y + py + gfx->translate_y;
        for (int px = 0; px < width; px++) {
            int dst_idx = offset + py * scanlength + px;
            if (dst_idx < 0 || dst_idx >= (int)pixels->length) continue;

            int src_x = x + px + gfx->translate_x;
            uint32_t argb = 0xFF000000u;
            if (src_x >= 0 && src_x < gfx->width && src_y >= 0 && src_y < gfx->height) {
                argb = gfx->pixels[src_y * gfx->width + src_x];
            }
            pdata[dst_idx] = (int16_t)dg_argb_to_short(argb, format);
        }
    }

    return NATIVE_RETURN_VOID();
}

/* ---- fillTriangle(x1,y1,x2,y2,x3,y3) without color - uses current color ---- */
static JavaValue native_directgraphics_fillTriangle_nocolor(JVM* jvm, JavaThread* thread,
                                                              JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* dg = (JavaObject*)args[0].ref;
    MidpGraphics* gfx = dg_get_gfx(dg);
    if (!gfx) return NATIVE_RETURN_VOID();

    jint x1 = args[1].i, y1 = args[2].i, x2 = args[3].i, y2 = args[4].i, x3 = args[5].i, y3 = args[6].i;
    uint32_t fill_color = ((uint32_t)(gfx->alpha ? gfx->alpha : 255) << 24) | gfx->rgb_color;

    x1 += gfx->translate_x; y1 += gfx->translate_y;
    x2 += gfx->translate_x; y2 += gfx->translate_y;
    x3 += gfx->translate_x; y3 += gfx->translate_y;

    if (y1 > y2) { int t; t=x1; x1=x2; x2=t; t=y1; y1=y2; y2=t; }
    if (y2 > y3) { int t; t=x2; x2=x3; x3=t; t=y2; y2=y3; y3=t; }
    if (y1 > y2) { int t; t=x1; x1=x2; x2=t; t=y1; y1=y2; y2=t; }

    for (int y = y1; y <= y3; y++) {
        int xl, xr;
        if (y < y2) {
            xl = (y2 != y1) ? x1 + (y - y1) * (x2 - x1) / (y2 - y1) : x1;
            xr = (y3 != y1) ? x1 + (y - y1) * (x3 - x1) / (y3 - y1) : x3;
        } else {
            xl = (y3 != y2) ? x2 + (y - y2) * (x3 - x2) / (y3 - y2) : x2;
            xr = (y3 != y1) ? x1 + (y - y1) * (x3 - x1) / (y3 - y1) : x3;
        }
        if (xl > xr) { int t = xl; xl = xr; xr = t; }
        for (int x = xl; x <= xr; x++) {
            if (x >= gfx->clip_x && x < gfx->clip_x + gfx->clip_width &&
                y >= gfx->clip_y && y < gfx->clip_y + gfx->clip_height &&
                x >= 0 && x < gfx->width && y >= 0 && y < gfx->height) {
                gfx->pixels[y * gfx->width + x] = fill_color;
            }
        }
    }
    return NATIVE_RETURN_VOID();
}

/* ---- drawTriangle(x1,y1,x2,y2,x3,y3) without color - outline, current color.
 * v17: was a TODO stub - triangle outlines now actually draw. ---- */
static JavaValue native_directgraphics_drawTriangle_nocolor(JVM* jvm, JavaThread* thread,
                                                              JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* dg = (JavaObject*)args[0].ref;
    MidpGraphics* gfx = dg_get_gfx(dg);
    if (!gfx) return NATIVE_RETURN_VOID();

    extern void midp_graphics_draw_line(MidpGraphics* gfx, int x1, int y1, int x2, int y2);

    jint x1 = args[1].i, y1 = args[2].i, x2 = args[3].i, y2 = args[4].i, x3 = args[5].i, y3 = args[6].i;

    /* Draw with the graphics context's current color/alpha */
    x1 += gfx->translate_x; y1 += gfx->translate_y;
    x2 += gfx->translate_x; y2 += gfx->translate_y;
    x3 += gfx->translate_x; y3 += gfx->translate_y;

    midp_graphics_draw_line(gfx, x1, y1, x2, y2);
    midp_graphics_draw_line(gfx, x2, y2, x3, y3);
    midp_graphics_draw_line(gfx, x3, y3, x1, y1);

    return NATIVE_RETURN_VOID();
}

/* com.nokia.mid.m3d.Texture.<init>(int format, Image img) - v17:
 * stores the image reference and format so M3D.bindTexture can sample it */
static JavaValue native_m3d_texture_init(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint format = args[1].i;
    JavaObject* image = (JavaObject*)args[2].ref;
    if (!obj) return NATIVE_RETURN_VOID();

    JavaValue fmt_val = { .i = format };
    native_set_field_value(obj, "format", fmt_val);
    JavaValue img_val = { .ref = image };
    native_set_field_value(obj, "image", img_val);

    return NATIVE_RETURN_VOID();
}

void init_nokia_direct_graphics(JVM* jvm) {
    /* MARKER: This proves the code was recompiled */
    if (g_j2me_runtime_debug) {
        fprintf(stderr, "### DIRECTGRAPHICS NATIVE METHODS REGISTERING (v2) ###\n");
        fflush(stderr);
    }
    
    NativeMethodEntry methods[] = {
        {"com/nokia/mid/ui/DirectGraphics", "drawImage", "(Ljavax/microedition/lcdui/Image;IIII)V", native_directgraphics_drawImage},
        {"com/nokia/mid/ui/DirectGraphics", "setARGBColor", "(I)V", native_directgraphics_setARGBColor},
        {"com/nokia/mid/ui/DirectGraphics", "getAlphaComponent", "()I", native_directgraphics_getAlphaComponent},
        {"com/nokia/mid/ui/DirectGraphics", "getNativePixelFormat", "()I", native_directgraphics_getNativePixelFormat},
        {"com/nokia/mid/ui/DirectGraphics", "drawPixels", "([IZIIIIIIII)V", native_directgraphics_drawPixels},
        {"com/nokia/mid/ui/DirectGraphics", "drawPolygon", "([II[IIII)V", native_directgraphics_drawPolygon},
        {"com/nokia/mid/ui/DirectGraphics", "fillPolygon", "([II[IIII)V", native_directgraphics_fillPolygon},
        {"com/nokia/mid/ui/DirectGraphics", "setPixels", "([IIIIIIII)V", native_directgraphics_setPixels},
        {"com/nokia/mid/ui/DirectGraphics", "getPixels", "([IIIIIIII)V", native_directgraphics_getPixels},
        {"com/nokia/mid/ui/DirectGraphics", "drawTriangle", "(IIIIIII)V", native_directgraphics_drawTriangle},
        {"com/nokia/mid/ui/DirectGraphics", "fillTriangle", "(IIIIIII)V", native_directgraphics_fillTriangle},

        /* DirectGraphics.drawPixels with byte[] pixels - Nokia format 1/2 */
        {"com/nokia/mid/ui/DirectGraphics", "drawPixels", "([B[BIIIIIIII)V", native_directgraphics_drawPixels_byte},
        /* DirectGraphics.drawPixels with short[] pixels - Nokia format 4444/565 etc. */
        {"com/nokia/mid/ui/DirectGraphics", "drawPixels", "([SZIIIIIIII)V", native_directgraphics_drawPixels_short},
        /* DirectGraphics.getPixels with byte[] pixels */
        {"com/nokia/mid/ui/DirectGraphics", "getPixels", "([B[BIIIIII)V", native_directgraphics_getPixels_byte},
        /* DirectGraphics.getPixels with short[] pixels */
        {"com/nokia/mid/ui/DirectGraphics", "getPixels", "([SIIIIII)V", native_directgraphics_getPixels_short},

        /* DirectGraphics.fillTriangle without argbColor (uses current color) */
        {"com/nokia/mid/ui/DirectGraphics", "fillTriangle", "(IIIIII)V", native_directgraphics_fillTriangle_nocolor},
        /* DirectGraphics.drawTriangle without argbColor (uses current color) */
        {"com/nokia/mid/ui/DirectGraphics", "drawTriangle", "(IIIIII)V", native_directgraphics_drawTriangle_nocolor},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
    NATIVE_DEBUG("Registered DirectGraphics native methods (%zu)", 
            sizeof(methods) / sizeof(methods[0]));
}

/*
 * java.lang.String native methods
 */

/* Forward declaration for case conversion helper */
static jchar to_lower_for_compare(jchar c);

/* === String field access helpers using proper field slot calculation === */

/* Cache for String field slots */
static int string_field_value_slot = -1;
static int string_field_offset_slot = -1;
static int string_field_count_slot = -1;
static int string_field_hash_slot = -1;
static bool string_fields_initialized = false;

/* Initialize String field slot cache */
static void string_init_field_slots(JavaClass* string_class) {
    if (string_fields_initialized || !string_class) return;
    
    string_field_value_slot = native_get_field_slot(string_class, "value");
    string_field_offset_slot = native_get_field_slot(string_class, "offset");
    string_field_count_slot = native_get_field_slot(string_class, "count");
    string_field_hash_slot = native_get_field_slot(string_class, "hash");
    
    NATIVE_DEBUG("String field slots: value=%d, offset=%d, count=%d, hash=%d",
            string_field_value_slot, string_field_offset_slot, 
            string_field_count_slot, string_field_hash_slot);
    
    string_fields_initialized = true;
}

/* Get String class and initialize field slots */
static JavaClass* string_get_class(JVM* jvm) {
    JavaClass* string_class = jvm_load_class(jvm, "java/lang/String");
    if (string_class && !string_fields_initialized) {
        string_init_field_slots(string_class);
    }
    return string_class;
}

/* Helper: set String value field (char[]) */
static void string_set_value(JavaObject* str, JavaArray* value) {
    if (!str || string_field_value_slot < 0) return;
    JavaValue v = { .ref = value };
    str->fields[string_field_value_slot] = v;
}

/* Helper: set String offset field (int) */
static void string_set_offset(JavaObject* str, jint offset) {
    if (!str || string_field_offset_slot < 0) return;
    JavaValue v = { .i = offset };
    str->fields[string_field_offset_slot] = v;
}

/* Helper: set String count field (int) */
static void string_set_count(JavaObject* str, jint count) {
    if (!str || string_field_count_slot < 0) return;
    JavaValue v = { .i = count };
    str->fields[string_field_count_slot] = v;
}

/* Helper: set String hash field (int) */
static void string_set_hash(JavaObject* str, jint hash) {
    if (!str || string_field_hash_slot < 0) return;
    JavaValue v = { .i = hash };
    str->fields[string_field_hash_slot] = v;
}

/* REMOVED: string_get_value - unused helper */

/* REMOVED: string_get_count - unused helper */

/* Public accessor functions for heap.c to use */
int native_get_string_value_slot(JVM* jvm) {
    string_get_class(jvm);  /* Initialize slots if needed */
    return string_field_value_slot;
}

int native_get_string_count_slot(JVM* jvm) {
    string_get_class(jvm);  /* Initialize slots if needed */
    return string_field_count_slot;
}

int native_get_string_hash_slot(JVM* jvm) {
    string_get_class(jvm);  /* Initialize slots if needed */
    return string_field_hash_slot;
}

int native_get_string_offset_slot(JVM* jvm) {
    string_get_class(jvm);  /* Initialize slots if needed */
    return string_field_offset_slot;
}

static JavaValue native_string_hashCode(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    return NATIVE_RETURN_INT(str ? string_hash(str) : 0);
}

/* === String Intern Pool (hash-based for O(1) lookup) === */
#define INTERN_POOL_SIZE 1024
#define INTERN_HASH_SIZE 1024
#define INTERN_HASH_MASK (INTERN_HASH_SIZE - 1)

/* NON-STATIC: intern_pool must be accessible to GC in heap.c for marking */
JavaString* intern_pool[INTERN_POOL_SIZE];
int intern_pool_count = 0;

/* Hash table for fast intern lookup (keyed by string hash + length) */
typedef struct InternHashEntry {
    juint hash;
    jsize length;
    struct InternHashEntry* next;
    JavaString* str;
} InternHashEntry;

static InternHashEntry* intern_hash_table[INTERN_HASH_SIZE];

/* CRITICAL: Check if a string is still valid (not freed by GC) */
static bool is_valid_intern_string(JavaString* str) {
    if (!str) return false;
    
    /* Check if pointer is in heap range */
    extern void* g_heap_start;
    extern void* g_heap_end;
    if ((void*)str < g_heap_start || (void*)str >= g_heap_end) {
        return false;
    }
    
    /* Check GC header type - must be OBJ_TYPE_STRING (2) or OBJ_TYPE_OBJECT (1) for Java Strings */
    /* GCObjectHeader is defined in heap.h which is already included */
    GCObjectHeader* header = (GCObjectHeader*)((uint8_t*)str - sizeof(GCObjectHeader));
    
    /* Check if type is STRING (2) or OBJECT (1) - both are valid for intern pool */
    /* OBJ_TYPE_STRING = native strings, OBJ_TYPE_OBJECT = Java String objects */
    if (header->type != OBJ_TYPE_STRING && header->type != OBJ_TYPE_OBJECT) {
        /* Silently return false - no spam in logs */
        return false;
    }
    
    /* Check if object was freed */
    if (header->type == OBJ_TYPE_FREE) {
        return false;
    }
    
    return true;
}

/* Clean up freed strings from intern pool - called from native_gc_notify */
static void cleanup_intern_pool(void) {
    /* Remove freed strings from pool and rebuild hash table */
    int write_idx = 0;
    for (int read_idx = 0; read_idx < intern_pool_count; read_idx++) {
        if (is_valid_intern_string(intern_pool[read_idx])) {
            intern_pool[write_idx++] = intern_pool[read_idx];
        }
    }
    intern_pool_count = write_idx;

    /* Rebuild hash table */
    memset(intern_hash_table, 0, sizeof(intern_hash_table));
    for (int i = 0; i < intern_pool_count; i++) {
        JavaString* str = intern_pool[i];
        jsize len = string_length(str);
        const jchar* chars = string_chars(str);
        juint h = 0;
        if (chars && len > 0) {
            for (jsize j = 0; j < len; j++) h = h * 31 + chars[j];
        }
        InternHashEntry* he = (InternHashEntry*)malloc(sizeof(InternHashEntry));
        if (he) {
            he->hash = (juint)h;
            he->length = len;
            he->str = str;
            uint32_t idx = (juint)h & INTERN_HASH_MASK;
            he->next = intern_hash_table[idx];
            intern_hash_table[idx] = he;
        }
    }
}

/* v36.13 MULTI-SESSION: the intern pool is PROCESS-GLOBAL while its entries
 * point into the PER-SESSION VM heap. Nothing reset it at the session
 * boundary, so session N+1 inherited session N's dead pointers. Two field
 * effects (Asphalt 3 3D device log, sessions 2-3):
 *   1. Every intern lookup walked stale entries — fresh-heap zeros read as
 *      hdr_type=0 == OBJ_TYPE_OBJECT, which is_valid_intern_string() ACCEPTS
 *      (its type check intends "STRING or OBJECT"), so string_equals_intern
 *      called string_chars() on zeroed memory -> the 8 [STR-CHARS-NULL]
 *      "value-field-invalid hdr_type=0" hits per session, right at class
 *      init (op_new -> jvm_init_class -> ldc/putstatic intern path).
 *   2. heap.c's GC root-marking walks intern_pool[] — stale pointers rooted
 *      the new heap's arbitrary objects as permanent live set (heap top
 *      retention grew every session: 755K -> 1101K -> 1190K in the log).
 * Wipe BOTH the flat pool and the hash table here (frees the malloc'd
 * entries), at the session boundary inside jvm_destroy. */
void native_intern_pool_session_reset(void) {
    for (int i = 0; i < INTERN_HASH_SIZE; i++) {
        InternHashEntry* e = intern_hash_table[i];
        while (e) {
            InternHashEntry* next = e->next;
            free(e);
            e = next;
        }
        intern_hash_table[i] = NULL;
    }
    memset(intern_pool, 0, sizeof(intern_pool));
    if (intern_pool_count > 0) {
        LOG_SAFE("[INTERN] pool reset: %d stale entr%s dropped (session boundary)\n",
                 intern_pool_count, intern_pool_count == 1 ? "y" : "ies");
    }
    intern_pool_count = 0;
}

/* Check if two strings are equal */
static bool string_equals_intern(JavaString* s1, JavaString* s2) {
    if (s1 == s2) return true;
    if (!s1 || !s2) return false;
    
    jsize len1 = string_length(s1);
    jsize len2 = string_length(s2);
    if (len1 != len2) return false;
    
    const jchar* c1 = string_chars(s1);
    const jchar* c2 = string_chars(s2);
    
    /* CRITICAL FIX: Check for NULL pointers before accessing char data */
    if (!c1 || !c2) {
        return false;
    }
    
    for (jsize i = 0; i < len1; i++) {
        if (c1[i] != c2[i]) return false;
    }
    return true;
}

/* Intern a string - returns the canonical representation (public API) */
JavaString* native_intern_string(JVM* jvm, JavaString* str) {
    (void)jvm;
    if (!str) return NULL;

    jsize len = string_length(str);
    const jchar* chars = string_chars(str);
    
    /* Compute hash from UTF-16 chars */
    juint h = 0;
    if (chars && len > 0) {
        for (jsize i = 0; i < len; i++) {
            h = h * 31 + chars[i];
        }
    }
    
    /* Fast path: check hash table first */
    uint32_t idx = h & INTERN_HASH_MASK;
    for (InternHashEntry* e = intern_hash_table[idx]; e; e = e->next) {
        if (e->hash == (juint)h && e->length == len && is_valid_intern_string(e->str)) {
            if (string_equals_intern(str, e->str)) {
                return e->str;
            }
        }
    }
    
    /* Fallback: also check the flat array for any entries not in hash table */
    for (int i = 0; i < intern_pool_count; i++) {
        if (!is_valid_intern_string(intern_pool[i])) continue;
        if (string_equals_intern(str, intern_pool[i])) {
            return intern_pool[i];
        }
    }
    
    /* Add to pool and hash table if space available */
    if (intern_pool_count < INTERN_POOL_SIZE) {
        intern_pool[intern_pool_count++] = str;
        
        /* Add to hash table */
        InternHashEntry* he = (InternHashEntry*)malloc(sizeof(InternHashEntry));
        if (he) {
            he->hash = (juint)h;
            he->length = len;
            he->str = str;
            he->next = intern_hash_table[idx];
            intern_hash_table[idx] = he;
        }
    }
    
    return str;
}

/* Look up an interned string by UTF-8 content — returns existing interned string or NULL */
JavaString* native_intern_find_by_utf8(const char* utf8, jsize utf8_len) {
    if (!utf8 || utf8_len <= 0) return NULL;
    
    /* Decode UTF-8 to compute hash and compare */
    juint h = 0;
    int pos = 0;
    while (pos < utf8_len) {
        unsigned char c = (unsigned char)utf8[pos];
        jchar ch;
        if (c < 0x80) {
            ch = c;
            pos++;
        } else if ((c & 0xE0) == 0xC0) {
            ch = ((c & 0x1F) << 6) | ((unsigned char)utf8[pos+1] & 0x3F);
            pos += 2;
        } else {
            ch = ((c & 0x0F) << 12) | (((unsigned char)utf8[pos+1] & 0x3F) << 6) | ((unsigned char)utf8[pos+2] & 0x3F);
            pos += 3;
        }
        h = h * 31 + ch;
    }
    
    /* Compute char length */
    jsize char_len = 0;
    pos = 0;
    while (pos < utf8_len) {
        unsigned char c = (unsigned char)utf8[pos];
        if (c < 0x80) pos++;
        else if ((c & 0xE0) == 0xC0) pos += 2;
        else pos += 3;
        char_len++;
    }
    
    /* Search hash table */
    uint32_t idx = h & INTERN_HASH_MASK;
    for (InternHashEntry* e = intern_hash_table[idx]; e; e = e->next) {
        if (e->hash == (juint)h && e->length == char_len && is_valid_intern_string(e->str)) {
            /* Compare content */
            const jchar* ech = string_chars(e->str);
            if (!ech) continue;
            
            pos = 0;
            int match = 1;
            for (jsize i = 0; i < char_len; i++) {
                unsigned char c = (unsigned char)utf8[pos];
                jchar ch;
                if (c < 0x80) { ch = c; pos++; }
                else if ((c & 0xE0) == 0xC0) { ch = ((c & 0x1F) << 6) | ((unsigned char)utf8[pos+1] & 0x3F); pos += 2; }
                else { ch = ((c & 0x0F) << 12) | (((unsigned char)utf8[pos+1] & 0x3F) << 6) | ((unsigned char)utf8[pos+2] & 0x3F); pos += 3; }
                if (ech[i] != ch) { match = 0; break; }
            }
            if (match) return e->str;
        }
    }
    
    return NULL;
}

static JavaValue native_string_intern(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;

    if (!str) return NATIVE_RETURN_NULL();

    /* Use the public intern function */
    JavaString* interned = native_intern_string(jvm, str);
    return NATIVE_RETURN_OBJECT(interned);
}

/* String.toString() - returns the string itself */
static JavaValue native_string_toString(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;

    /* toString() returns the string itself (this) */
    return NATIVE_RETURN_OBJECT(str);
}

/* String.length() - returns the length of the string */
static JavaValue native_string_length(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    
    if (!str) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_INT(0);
    }
    
    return NATIVE_RETURN_INT(string_length(str));
}

/* String.charAt(int) - returns character at index */
static JavaValue native_string_charAt(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    jint index = args[1].i;
    
    if (!str) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_INT(0);
    }
    
    /* FIX-19c: spec exception is StringIndexOutOfBoundsException */
    if (index < 0 || index >= string_length(str)) {
        native_throw_sioobe(jvm, thread, index);
        return NATIVE_RETURN_INT(0);
    }
    
    const jchar* chars = string_chars(str);
    return NATIVE_RETURN_INT((jint)chars[index]);
}

/* String.isEmpty() - returns true if length is 0 */
static JavaValue native_string_isEmpty(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    
    return NATIVE_RETURN_INT(str ? (string_length(str) == 0 ? 1 : 0) : 1);
}

/* String.startsWith(String) - check if string starts with prefix */
static JavaValue native_string_startsWith(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    JavaString* prefix = (JavaString*)args[1].ref;
    
    if (!str || !prefix) {
        return NATIVE_RETURN_INT(0);
    }
    
    if (string_length(prefix) > string_length(str)) {
        return NATIVE_RETURN_INT(0);
    }
    
    const jchar* str_chars = string_chars(str);
    const jchar* prefix_chars = string_chars(prefix);
    
    for (jsize i = 0; i < string_length(prefix); i++) {
        if (str_chars[i] != prefix_chars[i]) {
            return NATIVE_RETURN_INT(0);
        }
    }
    
    return NATIVE_RETURN_INT(1);
}

/* String.startsWith(String, int) - check if string starts with prefix at offset */
static JavaValue native_string_startsWith_offset(JVM* jvm, JavaThread* thread,
                                                  JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    JavaString* prefix = (JavaString*)args[1].ref;
    jint offset = args[2].i;
    
    if (!str || !prefix) {
        return NATIVE_RETURN_INT(0);
    }
    
    jsize str_len = string_length(str);
    jsize prefix_len = string_length(prefix);
    
    /* Check if offset is valid */
    if (offset < 0 || offset > str_len) {
        return NATIVE_RETURN_INT(0);
    }
    
    /* Check if there's enough room for prefix after offset */
    if (prefix_len > str_len - offset) {
        return NATIVE_RETURN_INT(0);
    }
    
    const jchar* str_chars = string_chars(str);
    const jchar* prefix_chars = string_chars(prefix);
    
    /* Compare characters starting at offset */
    for (jsize i = 0; i < prefix_len; i++) {
        if (str_chars[offset + i] != prefix_chars[i]) {
            return NATIVE_RETURN_INT(0);
        }
    }
    
    return NATIVE_RETURN_INT(1);
}

/* String.endsWith(String) - check if string ends with suffix */
static JavaValue native_string_endsWith(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    JavaString* suffix = (JavaString*)args[1].ref;
    
    if (!str || !suffix) {
        return NATIVE_RETURN_INT(0);
    }
    
    if (string_length(suffix) > string_length(str)) {
        return NATIVE_RETURN_INT(0);
    }
    
    const jchar* str_chars = string_chars(str);
    const jchar* suffix_chars = string_chars(suffix);
    
    jsize start = string_length(str) - string_length(suffix);
    for (jsize i = 0; i < string_length(suffix); i++) {
        if (str_chars[start + i] != suffix_chars[i]) {
            return NATIVE_RETURN_INT(0);
        }
    }
    
    return NATIVE_RETURN_INT(1);
}

/* String.regionMatches(boolean, int, String, int, int) - compare regions of strings */
static JavaValue native_string_regionMatches(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    jboolean ignoreCase = (jboolean)args[1].i;
    jint toffset = args[2].i;
    JavaString* other = (JavaString*)args[3].ref;
    jint ooffset = args[4].i;
    jint len = args[5].i;
    
    /* Null check */
    if (!str || !other) {
        return NATIVE_RETURN_INT(0);
    }
    
    jsize str_len = string_length(str);
    jsize other_len = string_length(other);
    
    /* Bounds check */
    if (toffset < 0 || ooffset < 0 || len < 0 ||
        toffset + len > str_len || ooffset + len > other_len) {
        return NATIVE_RETURN_INT(0);
    }
    
    const jchar* str_chars = string_chars(str);
    const jchar* other_chars = string_chars(other);
    
    /* Compare regions */
    for (jint i = 0; i < len; i++) {
        jchar c1 = str_chars[toffset + i];
        jchar c2 = other_chars[ooffset + i];
        
        if (ignoreCase) {
            /* Case-insensitive comparison */
            c1 = to_lower_for_compare(c1);
            c2 = to_lower_for_compare(c2);
        }
        
        if (c1 != c2) {
            return NATIVE_RETURN_INT(0);
        }
    }
    
    return NATIVE_RETURN_INT(1);
}

/* String.indexOf(int) - find first occurrence of character */
static JavaValue native_string_indexOf(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    jchar ch = (jchar)args[1].i;
    
    if (!str) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_INT(-1);
    }
    
    const jchar* chars = string_chars(str);
    
    for (jsize i = 0; i < string_length(str); i++) {
        if (chars[i] == ch) {
            return NATIVE_RETURN_INT((jint)i);
        }
    }
    
    return NATIVE_RETURN_INT(-1);
}

/* String.indexOf(int, int) - find character starting from index */
static JavaValue native_string_indexOf_from(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    jchar ch = (jchar)args[1].i;
    jint fromIndex = args[2].i;
    
    if (!str) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_INT(-1);
    }
    
    if (fromIndex < 0) fromIndex = 0;
    
    const jchar* chars = string_chars(str);
    
    for (jsize i = fromIndex; i < string_length(str); i++) {
        if (chars[i] == ch) {
            return NATIVE_RETURN_INT((jint)i);
        }
    }
    
    return NATIVE_RETURN_INT(-1);
}

/* String.indexOf(String) - find first occurrence of substring */
static JavaValue native_string_indexOf_string(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    JavaString* sub = (JavaString*)args[1].ref;
    
    if (!str) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_INT(-1);
    }
    
    /* indexOf(null) should throw NPE */
    if (!sub) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_INT(-1);
    }
    
    if (string_length(sub) == 0) {
        return NATIVE_RETURN_INT(0);
    }
    
    if (string_length(sub) > string_length(str)) {
        return NATIVE_RETURN_INT(-1);
    }
    
    const jchar* str_chars = string_chars(str);
    const jchar* sub_chars = string_chars(sub);
    
    /* Simple substring search */
    for (jsize i = 0; i <= string_length(str) - string_length(sub); i++) {
        bool found = true;
        for (jsize j = 0; j < string_length(sub); j++) {
            if (str_chars[i + j] != sub_chars[j]) {
                found = false;
                break;
            }
        }
        if (found) {
            return NATIVE_RETURN_INT((jint)i);
        }
    }
    
    return NATIVE_RETURN_INT(-1);
}

/* String.indexOf(String, int) - find substring starting from index */
static JavaValue native_string_indexOf_string_from(JVM* jvm, JavaThread* thread,
                                                    JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    JavaString* sub = (JavaString*)args[1].ref;
    jint fromIndex = args[2].i;
    
    if (!str) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_INT(-1);
    }
    
    /* indexOf(null, from) should throw NPE */
    if (!sub) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_INT(-1);
    }
    
    if (fromIndex < 0) fromIndex = 0;
    
    if (string_length(sub) == 0) {
        return NATIVE_RETURN_INT(fromIndex < string_length(str) ? fromIndex : string_length(str));
    }
    
    if (string_length(sub) > string_length(str)) {
        return NATIVE_RETURN_INT(-1);
    }
    
    const jchar* str_chars = string_chars(str);
    const jchar* sub_chars = string_chars(sub);
    
    for (jsize i = fromIndex; i <= string_length(str) - string_length(sub); i++) {
        bool found = true;
        for (jsize j = 0; j < string_length(sub); j++) {
            if (str_chars[i + j] != sub_chars[j]) {
                found = false;
                break;
            }
        }
        if (found) {
            return NATIVE_RETURN_INT((jint)i);
        }
    }
    
    return NATIVE_RETURN_INT(-1);
}

/* String.lastIndexOf(int) - find last occurrence of character */
static JavaValue native_string_lastIndexOf(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    jchar ch = (jchar)args[1].i;
    
    if (!str) {
        return NATIVE_RETURN_INT(-1);
    }
    
    const jchar* chars = string_chars(str);
    
    for (jsize i = string_length(str) - 1; i >= 0; i--) {
        if (chars[i] == ch) {
            return NATIVE_RETURN_INT((jint)i);
        }
    }
    
    return NATIVE_RETURN_INT(-1);
}

/* String.lastIndexOf(int, int) - find last occurrence of character from index */
static JavaValue native_string_lastIndexOf_from(JVM* jvm, JavaThread* thread,
                                                 JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    jchar ch = (jchar)args[1].i;
    jint fromIndex = args[2].i;
    
    if (!str) {
        return NATIVE_RETURN_INT(-1);
    }
    
    jsize len = string_length(str);
    
    /* Clamp fromIndex to valid range */
    if (fromIndex < 0) {
        fromIndex = 0;
    } else if (fromIndex >= len) {
        fromIndex = len - 1;
    }
    
    const jchar* chars = string_chars(str);
    
    /* Search backwards from fromIndex */
    for (jint i = fromIndex; i >= 0; i--) {
        if (chars[i] == ch) {
            return NATIVE_RETURN_INT((jint)i);
        }
    }
    
    return NATIVE_RETURN_INT(-1);
}

/* String.substring(int) - extract substring from start index */
static JavaValue native_string_substring(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    jint start = args[1].i;
    
    if (!str) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_OBJECT(NULL);
    }
    
    /* FIX-19c: spec exception is StringIndexOutOfBoundsException */
    if (start < 0 || start > string_length(str)) {
        native_throw_sioobe(jvm, thread, start);
        return NATIVE_RETURN_OBJECT(NULL);
    }
    
    const jchar* chars = string_chars(str);
    jsize len = string_length(str) - start;
    JavaString* result = jvm_new_string_utf16(jvm, chars + start, len);

    /* v36.09: silent NULL here became Doom RPG [Rus] "null.str" */
    return NATIVE_RETURN_OBJECT(native_string_result_guard(jvm, thread, result,
                                                           "substring(I)", str));
}

/* String.substring(int, int) - extract substring between indices */
static JavaValue native_string_substring_range(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    jint start = args[1].i;
    jint end = args[2].i;

    /* v38 DIAG: NOJME_TRACE_SUBSTR=1 — log substring(II) calls with a short
     * repr of the source string (BlackShark .hm tokenizer triage). */
    {
        static int trace_on = -1;
        if (trace_on < 0) {
            const char* e = getenv("NOJME_TRACE_SUBSTR");
            trace_on = (e && e[0] && e[0] != '0') ? 1 : 0;
        }
        if (trace_on && str) {
            char buf[64]; buf[0] = 0;
            jsize sl = string_length(str);
            for (int k = 0; k < 24 && start + k < sl; k++) {
                jchar c = string_chars(str)[start + k];
                buf[k] = (c >= 32 && c < 127) ? (char)c : '?';
                buf[k + 1] = 0;
            }
            j2me_log_ungated("[SUBSTR] len=%d [%d,%d) from@%d=\"%s\" (tail)\n",
                             (int)sl, start, end, start, buf);
        }
    }
    
    if (!str) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_OBJECT(NULL);
    }
    
    /* FIX-19c: spec exception is StringIndexOutOfBoundsException.
     * v34.5: report the ACTUAL offending index (end when end > len),
     * not start - the old diag printed start (0) and misled debugging
     * of the Doom RPG [Rus] h.a padding crash. */
    if (start < 0 || end > string_length(str) || start > end) {
        int bad = (end > string_length(str)) ? end : start;
        native_throw_sioobe(jvm, thread, bad);
        return NATIVE_RETURN_OBJECT(NULL);
    }
    
    const jchar* chars = string_chars(str);
    jsize len = end - start;
    JavaString* result = jvm_new_string_utf16(jvm, chars + start, len);

    /* v36.09: THE field site — this exact call returned NULL in Doom RPG
     * [Rus] session 3 and the game concat'ed it into "null.str". */
    return NATIVE_RETURN_OBJECT(native_string_result_guard(jvm, thread, result,
                                                           "substring(II)", str));
}

/* String.trim() - remove leading and trailing whitespace */
static JavaValue native_string_trim(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    
    if (!str) {
        return NATIVE_RETURN_OBJECT(NULL);
    }
    
    const jchar* chars = string_chars(str);
    jint start = 0;
    jint end = string_length(str);
    
    /* Find first non-whitespace */
    while (start < end && (chars[start] <= ' ')) {
        start++;
    }
    
    /* Find last non-whitespace */
    while (end > start && (chars[end - 1] <= ' ')) {
        end--;
    }
    
    if (start == 0 && end == string_length(str)) {
        return NATIVE_RETURN_OBJECT(str);  /* No change */
    }
    
    jsize len = end - start;
    JavaString* result = jvm_new_string_utf16(jvm, chars + start, len);

    return NATIVE_RETURN_OBJECT(native_string_result_guard(jvm, thread, result,
                                                           "trim()", str));
}

/* v34.44: Unicode-aware (ASCII + Latin-1 Supplement + Cyrillic)
 * single-char case conversion, shared by String/Character methods —
 * Russian games sort, wrap and compare words through these. */
static jchar jcs_to_lower_one(jchar c) {
    if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 0x20;      /* À..Þ -> à..þ */
    if (c >= 0x410 && c <= 0x42F) return c + 0x20;               /* А..Я -> а..я */
    if (c == 0x401) return 0x451;                                 /* Ё -> ё */
    return c;
}

static jchar jcs_to_upper_one(jchar c) {
    if (c >= 'a' && c <= 'z') return c - ('a' - 'A');
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 0x20;      /* à..þ -> À..Þ */
    if (c >= 0x430 && c <= 0x44F) return c - 0x20;               /* а..я -> А..Я */
    if (c == 0x451) return 0x401;                                 /* ё -> Ё */
    return c;
}

/* String.toLowerCase() - convert to lowercase */
static JavaValue native_string_toLowerCase(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    
    if (!str) {
        return NATIVE_RETURN_OBJECT(NULL);
    }
    
    const jchar* chars = string_chars(str);
    JavaString* result = jvm_new_string_utf16(jvm, chars, string_length(str));
    
    if (result) {
        jchar* result_chars = (jchar*)string_chars(result);
        for (jsize i = 0; i < string_length(str); i++) {
            result_chars[i] = jcs_to_lower_one(chars[i]);
        }
    }

    return NATIVE_RETURN_OBJECT(native_string_result_guard(jvm, thread, result,
                                                           "toLowerCase()", str));
}

/* String.toUpperCase() - convert to uppercase */
static JavaValue native_string_toUpperCase(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    
    if (!str) {
        return NATIVE_RETURN_OBJECT(NULL);
    }
    
    const jchar* chars = string_chars(str);
    JavaString* result = jvm_new_string_utf16(jvm, chars, string_length(str));
    
    if (result) {
        jchar* result_chars = (jchar*)string_chars(result);
        for (jsize i = 0; i < string_length(str); i++) {
            result_chars[i] = jcs_to_upper_one(chars[i]);
        }
    }

    return NATIVE_RETURN_OBJECT(native_string_result_guard(jvm, thread, result,
                                                           "toUpperCase()", str));
}

/* String.compareTo(String) - compare strings lexicographically */
static JavaValue native_string_compareTo(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    JavaString* other = (JavaString*)args[1].ref;
    
    if (!str) {
        return NATIVE_RETURN_INT(other ? -1 : 0);
    }
    
    if (!other) {
        return NATIVE_RETURN_INT(1);
    }
    
    const jchar* str_chars = string_chars(str);
    const jchar* other_chars = string_chars(other);
    
    jsize min_len = string_length(str) < string_length(other) ? string_length(str) : string_length(other);
    
    for (jsize i = 0; i < min_len; i++) {
        if (str_chars[i] != other_chars[i]) {
            return NATIVE_RETURN_INT((jint)str_chars[i] - (jint)other_chars[i]);
        }
    }
    
    return NATIVE_RETURN_INT((jint)string_length(str) - (jint)string_length(other));
}

/* String.replace(char, char) - replace characters */
static JavaValue native_string_replace(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    jchar oldChar = (jchar)args[1].i;
    jchar newChar = (jchar)args[2].i;
    
    if (!str) {
        return NATIVE_RETURN_OBJECT(NULL);
    }
    
    const jchar* chars = string_chars(str);
    JavaString* result = jvm_new_string_utf16(jvm, chars, string_length(str));
    
    if (result) {
        jchar* result_chars = (jchar*)string_chars(result);
        for (jsize i = 0; i < string_length(str); i++) {
            if (chars[i] == oldChar) {
                result_chars[i] = newChar;
            } else {
                result_chars[i] = chars[i];
            }
        }
    }

    return NATIVE_RETURN_OBJECT(native_string_result_guard(jvm, thread, result,
                                                           "replace(CC)", str));
}

/* String.equals(Object) - compare strings */
static JavaValue native_string_equals(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    JavaString* other = (JavaString*)args[1].ref;
    
    /* NPE if this is null (shouldn't happen in normal Java, but test expects it) */
    if (!str) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_INT(0);
    }
    
    if (str == other) {
        return NATIVE_RETURN_INT(1);
    }
    
    if (!other) {
        return NATIVE_RETURN_INT(0);
    }
    
    if (string_length(str) != string_length(other)) {
        return NATIVE_RETURN_INT(0);
    }
    
    const jchar* str_chars = string_chars(str);
    const jchar* other_chars = string_chars(other);
    
    for (jsize i = 0; i < string_length(str); i++) {
        if (str_chars[i] != other_chars[i]) {
            return NATIVE_RETURN_INT(0);
        }
    }
    
    return NATIVE_RETURN_INT(1);
}

/*
 * java.lang.String native methods - ADD toCharArray
 */

/* String.toCharArray() - converts string to char array */
static JavaValue native_string_toCharArray(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    
    if (!str) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_NULL();
    }
    
    /* Get string characters */
    const jchar* chars = string_chars(str);
    jsize length = string_length(str);
    
    /* Create char array of same length */
    JavaArray* char_array = jvm_new_array(jvm, T_CHAR, length, NULL);
    if (!char_array) {
        native_throw_oome(jvm, thread);
        return NATIVE_RETURN_NULL();
    }
    
    /* Copy characters to array */
    if (length > 0) {
        jchar* array_data_ptr = (jchar*)array_data(char_array);
        memcpy(array_data_ptr, chars, length * sizeof(jchar));
    }
    
    
    return NATIVE_RETURN_OBJECT(char_array);
}

/* ================================================================== */
/* v34.44 charset helpers (see src/jvm/charset.c)                     */
/* ================================================================== */

/* v34.44: platform default charset resolver (NOJME_DEFAULT_ENCODING,
 * default UTF-8 — what most real handsets reported as
 * microedition.encoding, and symmetric with String.getBytes() which has
 * always returned UTF-8 through string_utf8()). */
static JCharsetId string_default_charset(void) {
    static JCharsetId cached = JCS_UNSUPPORTED;
    if (cached == JCS_UNSUPPORTED) {
        const char* e = getenv("NOJME_DEFAULT_ENCODING");
        if (e && *e) {
            JCharsetId cs = jcharset_match(e);
            cached = (cs != JCS_UNSUPPORTED) ? cs : JCS_UTF8;
        } else {
            cached = JCS_UTF8;
        }
    }
    return cached;
}

/* v36.32 [STRING-CTOR-SPEC]: every java/lang/String keeps a NON-NULL
 * value array (JLS: String.value is char[0], never null). The old
 * "empty => value=NULL" convention poisoned every later string_chars()
 * call — the [STR-CHARS-NULL] "value-field-invalid" storm in the Zuma X
 * field log — and handed NULL chars/arrays to game code that on a real
 * phone never sees them. Empty strings now carry a real char[0].
 * Returns 0 when the empty value is in place, -1 if even the 1-element
 * array could not be allocated (OOM — NULL value left as last resort). */
static int string_init_empty_value(JVM* jvm, JavaObject* str) {
    JavaArray* empty = jvm_new_array(jvm, T_CHAR, 0, NULL);
    if (empty) {
        string_set_value(str, empty);
        string_set_offset(str, 0);
        string_set_count(str, 0);
        string_set_hash(str, 0);
        return 0;
    }
    /* OOM: best effort — keep the old NULL convention rather than crash */
    string_set_value(str, NULL);
    string_set_offset(str, 0);
    string_set_count(str, 0);
    string_set_hash(str, 0);
    return -1;
}

/* v34.44 shared core: decode bytes[offset..offset+count) with cs into the
 * String object's value/count fields. Returns 0 on success, -1 on
 * allocation failure (empty string is left in a valid state).
 * v36.32: callers validate offset/count (SIOOB) and null array (NPE) per
 * spec; this core only sees well-formed ranges and count >= 0. */
static int string_construct_from_bytes(JVM* jvm, JavaObject* str,
                                       JavaArray* byte_array,
                                       jint offset, jint count,
                                       JCharsetId cs) {
    if (!str) return -1;

    if (!byte_array || count <= 0 || offset < 0 ||
        offset + count > (jint)byte_array->length || cs == JCS_UNSUPPORTED) {
        /* Defensive fallback (callers pre-validate): empty, but with a
         * REAL char[0] value so later string ops never hit a NULL value */
        return string_init_empty_value(jvm, str);
    }

    /* Decode into a worst-case temp, then build the exact-size char[] */
    jsize max_units = jcharset_decode_max_units(cs, (jsize)count);
    jchar* units = (jchar*)malloc((size_t)max_units * sizeof(jchar));
    if (!units) {
        /* OOM: leave an empty but valid string (real char[0] value) */
        return string_init_empty_value(jvm, str);
    }

    jsize n = jcharset_decode(cs, (uint8_t*)array_data(byte_array) + offset,
                              (jsize)count, units, max_units);

    JavaArray* new_array = jvm_new_array(jvm, T_CHAR, n > 0 ? n : 0, NULL);
    if (new_array) {
        if (n > 0) {
            memcpy(array_data(new_array), units, (size_t)n * sizeof(jchar));
        }
        string_set_value(str, new_array);
        string_set_offset(str, 0);
        string_set_count(str, n);
        string_set_hash(str, 0);
        free(units);
        return 0;
    }

    free(units);
    return string_init_empty_value(jvm, str);
}

/* String.getBytes() - converts string to byte array using platform encoding */
static JavaValue native_string_getBytes(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    
    if (!str) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_NULL();
    }
    
    /* Get UTF-8 representation (simplified - in real Java it uses platform encoding) */
    const char* utf8 = string_utf8(jvm, str);
    if (!utf8) {
        return NATIVE_RETURN_NULL();
    }
    
    jsize length = (jsize)strlen(utf8);
    
    /* Create byte array */
    JavaArray* byte_array = jvm_new_array(jvm, T_BYTE, length, NULL);
    if (!byte_array) {
        native_throw_oome(jvm, thread);
        return NATIVE_RETURN_NULL();
    }
    
    /* Copy bytes */
    if (length > 0) {
        uint8_t* array_data_ptr = (uint8_t*)array_data(byte_array);
        memcpy(array_data_ptr, utf8, length);
    }
    
    
    return NATIVE_RETURN_OBJECT(byte_array);
}

/* String.getBytes(int, int, byte[], int) - old method (deprecated but used in some apps).
 * v34.44: encodes with the platform DEFAULT charset (UTF-8, matching the
 * no-arg getBytes) instead of truncating chars to bytes. If the encoded
 * form does not fit the destination slice, ArrayIndexOutOfBoundsException
 * is thrown per spec. */
static JavaValue native_string_getBytes_old(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    jint srcBegin = args[1].i;
    jint srcEnd = args[2].i;
    JavaArray* dst = (JavaArray*)args[3].ref;
    jint dstBegin = args[4].i;
    
    if (!str || !dst) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    
    /* Bounds checking */
    /* FIX-19c: spec exception is StringIndexOutOfBoundsException */
    if (srcBegin < 0 || srcBegin > srcEnd || srcEnd > string_length(str)) {
        native_throw_sioobe(jvm, thread, srcBegin);
        return NATIVE_RETURN_VOID();
    }
    
    JCharsetId cs = string_default_charset();
    jsize need = jcharset_encode_max_bytes(cs, (jsize)(srcEnd - srcBegin));
    if (dstBegin < 0 || dstBegin + (jint)need > (jint)dst->length) {
        native_throw_sioobe(jvm, thread, dstBegin);
        return NATIVE_RETURN_VOID();
    }
    
    /* Get string characters and encode */
    const jchar* chars = string_chars(str);
    if (srcEnd > srcBegin) {
        jsize n = jcharset_encode(cs, chars + srcBegin,
                                   (jsize)(srcEnd - srcBegin),
                                   (uint8_t*)array_data(dst) + dstBegin, need);
        (void)n; /* bounded by the check above */
    }
    
    return NATIVE_RETURN_VOID();
}

/* String.getChars(int srcBegin, int srcEnd, char[] dst, int dstBegin) */
static JavaValue native_string_getChars(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    jint srcBegin = args[1].i;
    jint srcEnd = args[2].i;
    JavaArray* dst = (JavaArray*)args[3].ref;
    jint dstBegin = args[4].i;
    
    if (!str || !dst) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    
    int len = string_length(str);
    
    /* Bounds checking */
    /* FIX-19c: spec exception is StringIndexOutOfBoundsException */
    if (srcBegin < 0 || srcEnd < 0 || srcBegin > srcEnd || srcEnd > len) {
        native_throw_sioobe(jvm, thread, srcBegin);
        return NATIVE_RETURN_VOID();
    }
    
    if (dstBegin < 0 || dstBegin + (srcEnd - srcBegin) > (jint)dst->length) {
        native_throw_sioobe(jvm, thread, dstBegin);
        return NATIVE_RETURN_VOID();
    }
    
    /* Get string characters and copy into char[] */
    const jchar* chars = string_chars(str);
    jchar* dst_data = (jchar*)array_data(dst);
    int count = srcEnd - srcBegin;
    for (int i = 0; i < count; i++) {
        dst_data[dstBegin + i] = chars[srcBegin + i];
    }
    
    return NATIVE_RETURN_VOID();
}

/* String.getBytes(String charsetName).
 * v34.44: was written but NEVER REGISTERED (marked __attribute__((unused)))
 * and it silently ignored the charset argument. Now registered and routed
 * through the charset engine; unknown names throw
 * java/io/UnsupportedEncodingException per CLDC 1.1. */
static JavaValue native_string_getBytes_charset(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    JavaString* charset_str = (JavaString*)args[1].ref;

    if (!str) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_NULL();
    }

    if (!charset_str) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_NULL();
    }

    const char* name = string_utf8(jvm, charset_str);
    JCharsetId cs = jcharset_match(name ? name : "");
    if (cs == JCS_UNSUPPORTED) {
        jvm_throw_by_name(jvm, "java/io/UnsupportedEncodingException", name);
        if (thread) thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_NULL();
    }

    jsize len = string_length(str);
    jsize max_bytes = jcharset_encode_max_bytes(cs, len);
    JavaArray* byte_array = jvm_new_array(jvm, T_BYTE, max_bytes, NULL);
    if (!byte_array) {
        native_throw_oome(jvm, thread);
        return NATIVE_RETURN_NULL();
    }

    jsize n = 0;
    if (len > 0) {
        n = jcharset_encode(cs, string_chars(str), len,
                            (uint8_t*)array_data(byte_array), max_bytes);
    }

    /* Trim to the exact encoded length when the charset needs fewer bytes */
    if (n == max_bytes) {
        return NATIVE_RETURN_OBJECT(byte_array);
    }
    JavaArray* exact = jvm_new_array(jvm, T_BYTE, n, NULL);
    if (!exact) {
        /* exact-size alloc failed — return the (longer) buffer, count is
         * implicit in the array length... no: length IS the count, so trim
         * by copy into a fresh array is required. Fall back to returning the
         * oversized array only if it has no tail (n == 0 handled above). */
        return NATIVE_RETURN_OBJECT(byte_array);
    }
    if (n > 0) {
        memcpy(array_data(exact), array_data(byte_array), (size_t)n);
    }
    return NATIVE_RETURN_OBJECT(exact);
}

/* =====================================================================
 * v34.58 PERF: PINNED-КЭШИ BOXED-ЗНАЧЕНИЙ И СТРОК
 * =====================================================================
 * Проблема: Integer/Long/Boolean/Byte/Short.valueOf и String.valueOf
 * вызывались из tight-loop'ов игровой логики (счётчики, хэш-ключи,
 * логирование), а каждый вызов делал:
 *   jvm_load_class (мьютекс + хэш) -> jvm_new_object (ticket-lock кучи,
 *   first-fit, memset) -> native_set_field_value (прогон по иерархии
 *   классов со strcmp на каждое поле).
 * Это ~1-2 мкс и 60-100 байт мусора НА ВЫЗОВ; на циклах в 60 fps это
 * доминирующий источник allocation-rate (см. комментарий v41 в
 * execute.c: ~400 MB/s мусора на Asphalt 3 3D).
 *
 * Решение (как в HotSpot — JLS 5.1.7):
 *  - Integer/Short/Byte/Long.valueOf(-128..127) и Boolean.valueOf
 *    возвращают КЭШИРОВАННЫЙ pinned-объект (identity стабилен —
 *    семантика == соответствует реальным JVM, раньше каждое
 *    значение было НОВЫМ объектом, что нарушало ожидания
 *    autoboxing-кода, привыкшего к кэшу на телефоне).
 *  - String.valueOf(int) 0..999 / valueOf(boolean) / valueOf(char
 *    0..127) — pinned-строки (аналог маленького string-intern).
 *
 * PINNED: объекты живут до конца сессии, GC их не собирает
 * (sweep обходит pinned). Расход: ~256 x 5 wrappers x ~80B +
 * ~1130 строк x ~150B = ~330 КБ максимум — на фоне кучи 16-64 МБ
 * незначительно, и материализуется только при использовании.
 *
 * Потокобезопасность: один мьютекс на все слоты (удерживается
 * наносекунды на hit'е; на miss'е — на время создания объекта).
 * Порядок блокировок: box-мьютекс -> heap-мьютекс; обратный порядок
 * нигде не встречается (GC под heap-lock никогда не зовёт valueOf).
 * ГОНКИ С AUTO-PIN (v34.14): если valueOf вызван изнутри исполняющегося
 * натива (g_gc_in_native>0), объект уже рождается pinned и будет
 * unpinned при выходе из натива — такой объект НЕ кэшируем (иначе
 * кэш держал бы мёртвый указатель). Окно гонки закрывается проверкой
 * g_gc_in_native до записи в слот.
 * ===================================================================== */
static pthread_mutex_t g_box_cache_mutex = PTHREAD_MUTEX_INITIALIZER;

#define BOX_CACHE_MIN (-128)
#define BOX_CACHE_MAX (127)
#define BOX_CACHE_SIZE (BOX_CACHE_MAX - BOX_CACHE_MIN + 1)

static JavaObject*  g_int_cache[BOX_CACHE_SIZE];   /* java/lang/Integer   */
static JavaObject*  g_short_cache[BOX_CACHE_SIZE]; /* java/lang/Short     */
static JavaObject*  g_byte_cache[BOX_CACHE_SIZE];  /* java/lang/Byte      */
static JavaObject*  g_long_cache[BOX_CACHE_SIZE];  /* java/lang/Long      */
static JavaObject*  g_bool_cache[2];               /* java/lang/Boolean   */
static JavaString*  g_str_int_cache[1000];         /* String.valueOf(I)    */
static JavaString*  g_str_bool_cache[2];           /* "true" / "false"     */
static JavaString*  g_str_char_cache[128];          /* ASCII single chars   */

/* v34.91 MULTI-SESSION FIX: native_object_cache_reset() at the END of this
 * file drops every pinned box/string cache + singleton — they hold objects
 * from the CURRENT JVM's heap and jvm_destroy frees that heap (Switch
 * frontend runs several games per process; see jvm.c's fix note). */

/* v34.59 (3D): boxed-Float для координатной математики.
 * g_float_cache — целые значения [-64..64] (индекс = iv + 64);
 * g_float_frac_cache — «половинки» и другие частые дробные константы.
 * См. native_float_valueOf: -0.0f / NaN / inf не кэшируются. */
#define FLOAT_CACHE_INTEGRAL_MAX 64
#define FLOAT_CACHE_SIZE (2 * FLOAT_CACHE_INTEGRAL_MAX + 1)
static JavaObject* g_float_cache[FLOAT_CACHE_SIZE];
static const jfloat g_float_frac_table[] = {
    0.5f, 0.25f, 0.75f, 1.5f, 2.5f, 3.5f, 4.5f,
    -0.5f, -0.25f, -0.75f, -1.5f, -2.5f
};
#define FLOAT_FRAC_N ((int)(sizeof g_float_frac_table / sizeof g_float_frac_table[0]))
static JavaObject* g_float_frac_cache[FLOAT_FRAC_N];

/* Get-or-create pinned wrapper в слоте. Возвращает NULL только если
 * класс не найден или объект не создался (OOME) — вызывающий откатывается
 * на старый некэшируемый путь. МЬЮТЕКС ДОЛЖЕН БЫТЬ ДЕРЖАН вызывающим. */
static JavaObject* box_cache_get_locked(JVM* jvm, JavaObject** slot,
                                        const char* class_name, JavaValue val) {
    if (*slot) return *slot;

    JavaClass* clazz = jvm_load_class(jvm, class_name);
    if (!clazz) return NULL;
    JavaObject* obj = jvm_new_object(jvm, clazz);
    if (!obj) return NULL;

    native_set_field_value(obj, "value", val);

    /* Не кэшируем объекты, рождённые внутри натива: они уже в
     * auto-pin журнале и будут unpinned при выходе — хранить такой
     * указатель в кэше опасно (см. комментарий блока выше). */
    {
        extern __thread int g_gc_in_native;
        if (!g_gc_in_native) {
            GCObjectHeader* h = (GCObjectHeader*)obj - 1;
            h->pinned = 1;   /* живёт вечно, GC не трогает */
            *slot = obj;
        }
    }
    return obj;
}

/* Аналог для строк (jvm_new_string + pin + кэш). */
static JavaString* str_cache_get_locked(JVM* jvm, JavaString** slot,
                                        const char* utf8) {
    if (*slot) return *slot;

    JavaString* s = jvm_new_string(jvm, utf8);
    if (!s) return NULL;

    {
        extern __thread int g_gc_in_native;
        if (!g_gc_in_native) {
            GCObjectHeader* h = (GCObjectHeader*)s - 1;
            h->pinned = 1;
            *slot = s;
        }
    }
    return s;
}

/* String.valueOf(C) - convert char to String */
static JavaValue native_string_valueOf_char(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    jchar c = (jchar)args[0].i;

    /* v34.58: кэш для ASCII (0..127) — самые частые случаи */
    if (c < 128) {
        char one[2] = { (char)c, 0 };
        pthread_mutex_lock(&g_box_cache_mutex);
        JavaString* cached = str_cache_get_locked(
                jvm, &g_str_char_cache[c], one);
        pthread_mutex_unlock(&g_box_cache_mutex);
        if (cached) return NATIVE_RETURN_OBJECT(cached);
    }

    /* Create a new String with one character */
    char utf8[8];
    int len = 0;
    
    /* Convert jchar (UTF-16) to UTF-8 */
    if (c < 0x80) {
        utf8[len++] = (char)c;
    } else if (c < 0x800) {
        utf8[len++] = (char)(0xC0 | (c >> 6));
        utf8[len++] = (char)(0x80 | (c & 0x3F));
    } else {
        utf8[len++] = (char)(0xE0 | (c >> 12));
        utf8[len++] = (char)(0x80 | ((c >> 6) & 0x3F));
        utf8[len++] = (char)(0x80 | (c & 0x3F));
    }
    utf8[len] = '\0';
    
    JavaString* str = jvm_new_string(jvm, utf8);
    return NATIVE_RETURN_OBJECT(str);
}

/* String.valueOf(I) - convert int to String */
static JavaValue native_string_valueOf_int(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    jint value = args[0].i;

    /* v34.58: кэш 0..999 (счётчики, очки, таймеры — покрывает
     * большинство игровых вызовов; snprintf+2 аллокации -> 0) */
    if (value >= 0 && value <= 999) {
        char buffer[8];
        snprintf(buffer, sizeof(buffer), "%d", value);
        pthread_mutex_lock(&g_box_cache_mutex);
        JavaString* cached = str_cache_get_locked(
                jvm, &g_str_int_cache[value], buffer);
        pthread_mutex_unlock(&g_box_cache_mutex);
        if (cached) return NATIVE_RETURN_OBJECT(cached);
    }

    char buffer[16];
    snprintf(buffer, sizeof(buffer), "%d", value);
    
    JavaString* str = jvm_new_string(jvm, buffer);
    return NATIVE_RETURN_OBJECT(str);
}

/* String.valueOf(J) - convert long to String */
static JavaValue native_string_valueOf_long(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    jlong value = args[0].j;
    
    char buffer[24];
    snprintf(buffer, sizeof(buffer), "%lld", (long long)value);
    
    JavaString* str = jvm_new_string(jvm, buffer);
    return NATIVE_RETURN_OBJECT(str);
}

/* String.valueOf(Z) - convert boolean to String */
static JavaValue native_string_valueOf_boolean(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    jboolean value = args[0].i;

    /* v34.58: pinned-синглтоны "true"/"false" */
    {
        pthread_mutex_lock(&g_box_cache_mutex);
        JavaString* cached = str_cache_get_locked(
                jvm, &g_str_bool_cache[value ? 1 : 0],
                value ? "true" : "false");
        pthread_mutex_unlock(&g_box_cache_mutex);
        if (cached) return NATIVE_RETURN_OBJECT(cached);
    }

    JavaString* str = jvm_new_string(jvm, value ? "true" : "false");
    return NATIVE_RETURN_OBJECT(str);
}

/* String.valueOf(F) - convert float to String */
static JavaValue native_string_valueOf_float(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    jfloat value = args[0].f;
    
    char buffer[48];
    java_format_float(buffer, sizeof(buffer), value);  /* FIX-19g: Java format */
    
    JavaString* str = jvm_new_string(jvm, buffer);
    return NATIVE_RETURN_OBJECT(str);
}

/* String.valueOf(D) - convert double to String */
static JavaValue native_string_valueOf_double(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    jdouble value = args[0].d;
    
    char buffer[48];
    java_format_double(buffer, sizeof(buffer), value);  /* FIX-19g: Java format */
    
    JavaString* str = jvm_new_string(jvm, buffer);
    return NATIVE_RETURN_OBJECT(str);
}

/* String.valueOf(Object) - convert object to String */
static JavaValue native_string_valueOf_object(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = args[0].ref;
    
    if (!obj) {
        JavaString* str = jvm_new_string(jvm, "null");
        return NATIVE_RETURN_OBJECT(str);
    }
    
    /* v20 FIX (P1): actually call toString() through virtual dispatch. */
    JavaString* str = util_to_string_of(jvm, thread, obj);
    return NATIVE_RETURN_OBJECT(str);
}

/* String.valueOf([C) - convert char array to String (STATIC METHOD) */
static JavaValue native_string_valueOf_chararray(JVM* jvm, JavaThread* thread,
                                                  JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    /* STATIC METHOD - args[0] is the char array directly (no 'this') */
    JavaArray* char_array = (JavaArray*)args[0].ref;
    
    if (!char_array) {
        JavaString* str = jvm_new_string(jvm, "null");
        return NATIVE_RETURN_OBJECT(str);
    }
    
    /* Create String from char array */
    jchar* chars = (jchar*)array_data(char_array);
    jsize length = char_array->length;
    
    JavaString* result = jvm_new_string_utf16(jvm, chars, length);
    return NATIVE_RETURN_OBJECT(native_string_result_guard(jvm, thread, result,
                                                           "valueOf([C)", NULL));
}

/* String.concat(String) - concatenate two strings */
static JavaValue native_string_concat(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    JavaString* other = (JavaString*)args[1].ref;
    
    if (!str) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_NULL();
    }
    
    /* CRITICAL: If other is null, throw NullPointerException per Java spec */
    if (!other) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_NULL();
    }
    
    /* Get this string's data */
    jsize this_len = string_length(str);
    const jchar* this_chars = string_chars(str);
    
    /* Get other string's data */
    jsize other_len = string_length(other);
    const jchar* other_chars = string_chars(other);
    
    /* Create result buffer */
    jsize total_len = this_len + other_len;
    jchar* result_chars = (jchar*)malloc((total_len > 0 ? total_len : 1) * sizeof(jchar));
    if (!result_chars) {
        native_throw_oome(jvm, thread);
        return NATIVE_RETURN_NULL();
    }
    
    /* Copy this string */
    if (this_len > 0 && this_chars) {
        memcpy(result_chars, this_chars, this_len * sizeof(jchar));
    }
    
    /* Copy other string */
    if (other_len > 0 && other_chars) {
        memcpy(result_chars + this_len, other_chars, other_len * sizeof(jchar));
    }
    
    JavaString* result = jvm_new_string_utf16(jvm, result_chars, total_len);
    free(result_chars);

    return NATIVE_RETURN_OBJECT(native_string_result_guard(jvm, thread, result,
                                                           "concat()", str));
}

/* Helper function to convert a character to lowercase for case-insensitive comparison */
static jchar to_lower_for_compare(jchar c) {
    /* ASCII uppercase */
    if (c >= 'A' && c <= 'Z') {
        return c + ('a' - 'A');
    }
    /* Latin-1 Supplement uppercase letters (U+00C0 - U+00DE, excluding U+00D7) */
    if (c >= 0x00C0 && c <= 0x00DE && c != 0x00D7) {
        return c + 0x0020;
    }
    /* Greek uppercase (U+0391 - U+03A9, excluding U+03A2) */
    if (c >= 0x0391 && c <= 0x03A9 && c != 0x03A2) {
        return c + 0x0020;
    }
    /* Cyrillic uppercase (U+0410 - U+042F) */
    if (c >= 0x0410 && c <= 0x042F) {
        return c + 0x0020;
    }
    /* Cyrillic uppercase extended (U+0400 - Ux040F) */
    if (c >= 0x0400 && c <= 0x040F) {
        return c + 0x0050;
    }
    return c;
}

/* String.equalsIgnoreCase(String) - compare strings ignoring case */
static JavaValue native_string_equalsIgnoreCase(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    JavaString* other = (JavaString*)args[1].ref;
    
    /* Same reference? */
    if (str == other) {
        return NATIVE_RETURN_INT(1);
    }
    
    /* If either is null, they're not equal (but both null would be caught above) */
    if (!str || !other) {
        return NATIVE_RETURN_INT(0);
    }
    
    /* Different lengths? */
    if (string_length(str) != string_length(other)) {
        return NATIVE_RETURN_INT(0);
    }
    
    const jchar* str_chars = string_chars(str);
    const jchar* other_chars = string_chars(other);
    
    /* Compare character by character, ignoring case */
    for (jsize i = 0; i < string_length(str); i++) {
        jchar c1 = to_lower_for_compare(str_chars[i]);
        jchar c2 = to_lower_for_compare(other_chars[i]);
        
        if (c1 != c2) {
            return NATIVE_RETURN_INT(0);
        }
    }
    
    return NATIVE_RETURN_INT(1);
}

/* String.<init>() - default constructor creates empty string */
static JavaValue native_string_init(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* str = (JavaObject*)args[0].ref;
    
    /* Initialize field slots on first use */
    string_get_class(jvm);
    
    if (str) {
        /* v36.32 [STRING-CTOR-SPEC]: ()V leaves a REAL char[0] value,
         * not a NULL one — String.value is never null on a real JVM */
        string_init_empty_value(jvm, str);
    }
    
    return NATIVE_RETURN_VOID();
}

/* String.<init>(char[]) - construct from char array */
static JavaValue native_string_init_chars(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* str = (JavaObject*)args[0].ref;
    JavaArray* char_array = (JavaArray*)args[1].ref;
    
    /* Initialize field slots on first use */
    string_get_class(jvm);
    
    if (!str) {
        return NATIVE_RETURN_VOID();
    }

    /* v36.32 [STRING-CTOR-SPEC]: new String((char[])null) throws NPE per
     * spec (real phones throw; Zuma X's loader CATCHES NPEs around its
     * resource probing — this is the behavior it was written against) */
    if (!char_array) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }

    if (char_array->length > 0) {
        /* Create a copy of the char array */
        JavaArray* new_array = jvm_new_array(jvm, T_CHAR, char_array->length, NULL);
        if (new_array) {
            jchar* src = (jchar*)array_data(char_array);
            jchar* dst = (jchar*)array_data(new_array);
            memcpy(dst, src, char_array->length * sizeof(jchar));
            
            string_set_value(str, new_array);
            string_set_offset(str, 0);
            string_set_count(str, char_array->length);
            string_set_hash(str, 0);
            
            DEBUG_LOG("String.<init>(char[]): created string with length=%d, value=%p",
                      char_array->length, new_array);
        }
    } else {
        /* Empty char array -> empty string with a REAL char[0] value */
        string_init_empty_value(jvm, str);
        DEBUG_LOG("String.<init>(char[]): created empty string");
    }
    
    return NATIVE_RETURN_VOID();
}

/* String.<init>(byte[]) - construct from byte array */

static JavaValue native_string_init_bytes(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* str = (JavaObject*)args[0].ref;
    JavaArray* byte_array = (JavaArray*)args[1].ref;

    /* Initialize field slots on first use */
    string_get_class(jvm);

    if (!str) {
        return NATIVE_RETURN_VOID();
    }

    /* v36.32 [STRING-CTOR-SPEC]: new String((byte[])null) throws NPE */
    if (!byte_array) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }

    /* v34.44 FIX (Gish Reloaded [Rus] mojibake): decode with the platform
     * DEFAULT charset (UTF-8) instead of widening bytes to chars. The old
     * Latin-1 widening turned every UTF-8 resource into mojibake — Cyrillic
     * text arrived as one char per byte and was re-encoded on the way out. */
    string_construct_from_bytes(jvm, str, byte_array, 0,
                                (jint)byte_array->length,
                                string_default_charset());

    return NATIVE_RETURN_VOID();
}

/* String.<init>(String) - copy constructor */
static JavaValue native_string_init_string(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* str = (JavaObject*)args[0].ref;
    JavaString* other = (JavaString*)args[1].ref;
    
    /* Initialize field slots on first use */
    string_get_class(jvm);
    
    if (!str) {
        return NATIVE_RETURN_VOID();
    }
    
    if (other) {
        jsize other_len = string_length(other);
        const jchar* other_chars = string_chars(other);
        
        if (other_len > 0 && other_chars) {
            /* Create a copy of the char array */
            JavaArray* new_array = jvm_new_array(jvm, T_CHAR, other_len, NULL);
            if (new_array) {
                jchar* dst = (jchar*)array_data(new_array);
                memcpy(dst, other_chars, other_len * sizeof(jchar));
                
                string_set_value(str, new_array);
                string_set_offset(str, 0);
                string_set_count(str, other_len);
                string_set_hash(str, 0);
            }
        } else {
            /* v36.32: empty/invalid source -> empty string, REAL char[0] */
            string_init_empty_value(jvm, str);
        }
    } else {
        /* v36.32 [STRING-CTOR-SPEC]: new String((String)null) throws NPE
         * per spec (documented phone behavior; was silently empty) */
        native_throw_npe(jvm, thread);
    }
    
    return NATIVE_RETURN_VOID();
}

/* Forward declarations: StringBuffer accessor helpers are defined further
 * below in this file alongside the StringBuffer natives. */
static char* stringbuffer_get_buffer(JavaObject* obj);
static int* stringbuffer_get_length_ptr(JavaObject* obj);

/* String.<init>(StringBuffer) - construct from StringBuffer contents.
 * FIX: was previously missing entirely; MIDlet code doing
 * new String(stringBuffer) (a standard javac idiom for building strings)
 * got a silently empty string because the stub bytecode constructor
 * dropped the argument. Java spec: null argument means "null". */
static JavaValue native_string_init_stringbuffer(JVM* jvm, JavaThread* thread,
                                                 JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* str = (JavaObject*)args[0].ref;
    JavaObject* sb = (JavaObject*)args[1].ref;

    /* Initialize field slots on first use */
    string_get_class(jvm);

    if (!str) {
        return NATIVE_RETURN_VOID();
    }

    /* Per java.lang.String spec: new String((StringBuffer)null) == "null" */
    JavaString* tmp = NULL;
    int sb_len = 0;
    if (sb) {
        jchar* chars = (jchar*)stringbuffer_get_buffer(sb);
        int* len_ptr = stringbuffer_get_length_ptr(sb);
        if (chars && len_ptr && *len_ptr > 0) {
            /* v34.5: buffer is raw UTF-16, build the string from units */
            tmp = jvm_new_string_utf16(jvm, (const jchar*)chars, *len_ptr);
            sb_len = *len_ptr;
        }
    }

    if (tmp && sb_len > 0) {
        if (tmp) {
            int value_slot = native_get_string_value_slot(jvm);
            /* Share the freshly built value array with our target object
             * (String is immutable, sharing arrays between copies is safe) */
            JavaArray* src_value = NULL;
            if (value_slot >= 0) {
                src_value = (JavaArray*)((JavaObject*)tmp)->fields[value_slot].ref;
            }
            if (src_value) {
                string_set_value(str, src_value);
                string_set_offset(str, 0);
                string_set_count(str, sb_len > 0 ? string_length(tmp) : 0);
                string_set_hash(str, 0);
            } else {
                /* Fallback: copy length via string_length in case slots differ */
                string_set_value(str, NULL);
                string_set_offset(str, 0);
                string_set_count(str, string_length(tmp));
                string_set_hash(str, 0);
            }
        }
    } else {
        /* Empty StringBuffer - empty string with a REAL char[0] value
         * (v36.32: value is never NULL for a constructed String) */
        string_init_empty_value(jvm, str);
    }

    return NATIVE_RETURN_VOID();
}

/* String.<init>(char[], int, int) - construct from char array with offset */
static JavaValue native_string_init_chars_offset(JVM* jvm, JavaThread* thread,
                                                  JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* str = (JavaObject*)args[0].ref;
    JavaArray* char_array = (JavaArray*)args[1].ref;
    jint offset = args[2].i;
    jint count = args[3].i;
    
    /* Initialize field slots on first use */
    string_get_class(jvm);
    
    if (!str) {
        return NATIVE_RETURN_VOID();
    }

    /* v36.32 [STRING-CTOR-SPEC]: spec exceptions per real phones —
     * null array -> NPE, bad offset/count -> StringIndexOutOfBounds */
    if (!char_array) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    if (offset < 0 || count < 0 || offset + count > (jint)char_array->length) {
        native_throw_sioobe(jvm, thread, offset < 0 ? offset : count);
        return NATIVE_RETURN_VOID();
    }

    if (count > 0) {
        /* Create a new char array with the substring */
        JavaArray* new_array = jvm_new_array(jvm, T_CHAR, count, NULL);
        if (new_array) {
            jchar* src = (jchar*)array_data(char_array) + offset;
            jchar* dst = (jchar*)array_data(new_array);
            memcpy(dst, src, count * sizeof(jchar));
            
            string_set_value(str, new_array);
            string_set_offset(str, 0);
            string_set_count(str, count);
            string_set_hash(str, 0);
        }
    } else {
        /* count == 0, empty string with a REAL char[0] value */
        string_init_empty_value(jvm, str);
    }
    
    return NATIVE_RETURN_VOID();
}

/* String.<init>(byte[], int, int) - construct from byte array with offset.
 * v34.44: decodes with the platform DEFAULT charset (see the (byte[])
 * constructor) instead of widening bytes to chars. */
static JavaValue native_string_init_bytes_offset(JVM* jvm, JavaThread* thread,
                                                  JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* str = (JavaObject*)args[0].ref;
    JavaArray* byte_array = (JavaArray*)args[1].ref;
    jint offset = args[2].i;
    jint count = args[3].i;

    /* Initialize field slots on first use */
    string_get_class(jvm);

    if (!str) {
        return NATIVE_RETURN_VOID();
    }

    /* v36.32 [STRING-CTOR-SPEC]: spec exceptions before the shared core —
     * null array -> NPE, bad offset/count -> SIOOB */
    if (!byte_array) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    if (offset < 0 || count < 0 || offset + count > (jint)byte_array->length) {
        native_throw_sioobe(jvm, thread, offset < 0 ? offset : count);
        return NATIVE_RETURN_VOID();
    }

    string_construct_from_bytes(jvm, str, byte_array, offset, count,
                                string_default_charset());

    return NATIVE_RETURN_VOID();
}

/* String.<init>(byte[], String charsetName) - construct from byte array with charset.
 * v34.44 FIX (Gish Reloaded [Rus]): the charset name used to be IGNORED and
 * every byte was widened to a char (Latin-1). Gish decodes its UTF-8 text
 * resources (t_zee.ru / tl_zee.ru / tz.ru) through THIS constructor, so all
 * Cyrillic came out as mojibake. Now the name is matched against the
 * charset engine; per CLDC 1.1 an unknown name throws
 * java/io/UnsupportedEncodingException (games catch it). */
static JavaValue native_string_init_bytes_charset(JVM* jvm, JavaThread* thread,
                                                    JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* str = (JavaObject*)args[0].ref;
    JavaArray* byte_array = (JavaArray*)args[1].ref;
    JavaString* charset_str = (JavaString*)args[2].ref;

    /* Initialize field slots on first use */
    string_get_class(jvm);

    if (!str) {
        return NATIVE_RETURN_VOID();
    }

    /* null charset name -> NullPointerException per spec */
    if (!charset_str) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }

    /* v36.32: null byte array -> NPE per spec (was silently empty) */
    if (!byte_array) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }

    const char* name = string_utf8(jvm, charset_str);
    JCharsetId cs = jcharset_match(name ? name : "");
    if (cs == JCS_UNSUPPORTED) {
        jvm_throw_by_name(jvm, "java/io/UnsupportedEncodingException", name);
        if (thread) thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_VOID();
    }

    string_construct_from_bytes(jvm, str, byte_array, 0,
                                (jint)byte_array->length, cs);

    return NATIVE_RETURN_VOID();
}

/* String.<init>(byte[], int, int, String charsetName) - bytes with offset+count+charset.
 * v34.44: same charset-engine path as (byte[], String). */
static JavaValue native_string_init_bytes_offset_charset(JVM* jvm, JavaThread* thread,
                                                           JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* str = (JavaObject*)args[0].ref;
    JavaArray* byte_array = (JavaArray*)args[1].ref;
    jint offset = args[2].i;
    jint count = args[3].i;
    JavaString* charset_str = (JavaString*)args[4].ref;

    /* Initialize field slots on first use */
    string_get_class(jvm);

    if (!str) {
        return NATIVE_RETURN_VOID();
    }

    if (!charset_str) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }

    /* v36.32: null byte array -> NPE; bad offset/count -> SIOOB (spec) */
    if (!byte_array) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    if (offset < 0 || count < 0 || offset + count > (jint)byte_array->length) {
        native_throw_sioobe(jvm, thread, offset < 0 ? offset : count);
        return NATIVE_RETURN_VOID();
    }

    const char* name = string_utf8(jvm, charset_str);
    JCharsetId cs = jcharset_match(name ? name : "");
    if (cs == JCS_UNSUPPORTED) {
        jvm_throw_by_name(jvm, "java/io/UnsupportedEncodingException", name);
        if (thread) thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_VOID();
    }

    string_construct_from_bytes(jvm, str, byte_array, offset, count, cs);

    return NATIVE_RETURN_VOID();
}

void init_java_lang_string(JVM* jvm) {
    NativeMethodEntry methods[] = {
        /* Constructors */
        {"java/lang/String", "<init>", "()V", native_string_init},
        {"java/lang/String", "<init>", "([C)V", native_string_init_chars},
        {"java/lang/String", "<init>", "([B)V", native_string_init_bytes},
        {"java/lang/String", "<init>", "([BLjava/lang/String;)V", native_string_init_bytes_charset},
        {"java/lang/String", "<init>", "(Ljava/lang/String;)V", native_string_init_string},
        {"java/lang/String", "<init>", "(Ljava/lang/StringBuffer;)V", native_string_init_stringbuffer},
        {"java/lang/String", "<init>", "([CII)V", native_string_init_chars_offset},
        {"java/lang/String", "<init>", "([BII)V", native_string_init_bytes_offset},
        {"java/lang/String", "<init>", "([BIILjava/lang/String;)V", native_string_init_bytes_offset_charset},
        
        /* Basic methods */
        {"java/lang/String", "length", "()I", native_string_length},
        {"java/lang/String", "charAt", "(I)C", native_string_charAt},
        {"java/lang/String", "isEmpty", "()Z", native_string_isEmpty},
        
        /* Comparison methods */
        {"java/lang/String", "equals", "(Ljava/lang/Object;)Z", native_string_equals},
        {"java/lang/String", "hashCode", "()I", native_string_hashCode},
        {"java/lang/String", "compareTo", "(Ljava/lang/String;)I", native_string_compareTo},
        {"java/lang/String", "startsWith", "(Ljava/lang/String;)Z", native_string_startsWith},
        {"java/lang/String", "startsWith", "(Ljava/lang/String;I)Z", native_string_startsWith_offset},
        {"java/lang/String", "endsWith", "(Ljava/lang/String;)Z", native_string_endsWith},
        {"java/lang/String", "equalsIgnoreCase", "(Ljava/lang/String;)Z", native_string_equalsIgnoreCase},
        {"java/lang/String", "regionMatches", "(ZILjava/lang/String;II)Z", native_string_regionMatches},
        
        /* Search methods */
        {"java/lang/String", "indexOf", "(I)I", native_string_indexOf},
        {"java/lang/String", "indexOf", "(II)I", native_string_indexOf_from},
        {"java/lang/String", "indexOf", "(Ljava/lang/String;)I", native_string_indexOf_string},
        {"java/lang/String", "indexOf", "(Ljava/lang/String;I)I", native_string_indexOf_string_from},
        {"java/lang/String", "lastIndexOf", "(I)I", native_string_lastIndexOf},
        {"java/lang/String", "lastIndexOf", "(II)I", native_string_lastIndexOf_from},
        {"java/lang/String", "lastIndexOf", "(Ljava/lang/String;)I", native_string_lastIndexOf_string},
        {"java/lang/String", "lastIndexOf", "(Ljava/lang/String;I)I", native_string_lastIndexOf_string_from},
        
        /* Substring methods */
        {"java/lang/String", "substring", "(I)Ljava/lang/String;", native_string_substring},
        {"java/lang/String", "substring", "(II)Ljava/lang/String;", native_string_substring_range},
        
        /* Conversion methods */
        {"java/lang/String", "toCharArray", "()[C", native_string_toCharArray},
        {"java/lang/String", "getBytes", "()[B", native_string_getBytes},
        {"java/lang/String", "getBytes", "(Ljava/lang/String;)[B", native_string_getBytes_charset},
        {"java/lang/String", "getBytes", "(II[BI)V", native_string_getBytes_old},
        {"java/lang/String", "getChars", "(II[CI)V", native_string_getChars},
        {"java/lang/String", "trim", "()Ljava/lang/String;", native_string_trim},
        {"java/lang/String", "toLowerCase", "()Ljava/lang/String;", native_string_toLowerCase},
        {"java/lang/String", "toUpperCase", "()Ljava/lang/String;", native_string_toUpperCase},
        {"java/lang/String", "replace", "(CC)Ljava/lang/String;", native_string_replace},
        {"java/lang/String", "intern", "()Ljava/lang/String;", native_string_intern},
        {"java/lang/String", "toString", "()Ljava/lang/String;", native_string_toString},
        
        /* Concatenation */
        {"java/lang/String", "concat", "(Ljava/lang/String;)Ljava/lang/String;", native_string_concat},
        
        /* String.valueOf static methods */
        {"java/lang/String", "valueOf", "(C)Ljava/lang/String;", native_string_valueOf_char},
        {"java/lang/String", "valueOf", "([C)Ljava/lang/String;", native_string_valueOf_chararray},
        {"java/lang/String", "valueOf", "(I)Ljava/lang/String;", native_string_valueOf_int},
        {"java/lang/String", "valueOf", "(J)Ljava/lang/String;", native_string_valueOf_long},
        {"java/lang/String", "valueOf", "(Z)Ljava/lang/String;", native_string_valueOf_boolean},
        {"java/lang/String", "valueOf", "(F)Ljava/lang/String;", native_string_valueOf_float},
        {"java/lang/String", "valueOf", "(D)Ljava/lang/String;", native_string_valueOf_double},
        {"java/lang/String", "valueOf", "(Ljava/lang/Object;)Ljava/lang/String;", native_string_valueOf_object},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
    NATIVE_DEBUG("Registered java/lang/String native methods (%zu total)",
            sizeof(methods) / sizeof(methods[0]));
}

/*
 * java.lang.Integer native methods
 */

static JavaValue native_integer_valueOf(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    jint value = args[0].i;

    /* v34.58: кэш -128..127 (JLS 5.1.7 autoboxing semantics) */
    if (value >= BOX_CACHE_MIN && value <= BOX_CACHE_MAX) {
        pthread_mutex_lock(&g_box_cache_mutex);
        JavaObject* cached = box_cache_get_locked(
                jvm, &g_int_cache[value - BOX_CACHE_MIN],
                "java/lang/Integer", (JavaValue){ .i = value });
        pthread_mutex_unlock(&g_box_cache_mutex);
        if (cached) return NATIVE_RETURN_OBJECT(cached);
        /* промах создания (нет класса/OOM) — откат на общий путь ниже */
    }

    JavaClass* int_class = jvm_load_class(jvm, "java/lang/Integer");
    if (!int_class) {
        return NATIVE_RETURN_NULL();
    }

    JavaObject* int_obj = jvm_new_object(jvm, int_class);
    if (!int_obj) {
        return NATIVE_RETURN_NULL();
    }

    /* ИСПРАВЛЕНО: Используем native_set_field_value */
    JavaValue val = { .i = value };
    native_set_field_value(int_obj, "value", val);

    return NATIVE_RETURN_OBJECT(int_obj);
}

static JavaValue native_integer_valueOf_string(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    
    if (!str) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_NULL();
    }
    
    const char* utf8 = string_utf8(jvm, str);
    if (!utf8) {
        return NATIVE_RETURN_NULL();
    }
    
    /* Parse integer */
    jint value = 0;
    int sign = 1;
    const char* p = utf8;
    
    if (*p == '-') {
        sign = -1;
        p++;
    } else if (*p == '+') {
        p++;
    }
    
    while (*p >= '0' && *p <= '9') {
        value = value * 10 + (*p - '0');
        p++;
    }
    
    value *= sign;
    
    JavaClass* int_class = jvm_load_class(jvm, "java/lang/Integer");
    if (!int_class) {
        return NATIVE_RETURN_NULL();
    }
    
    JavaObject* int_obj = jvm_new_object(jvm, int_class);
    if (!int_obj) {
        return NATIVE_RETURN_NULL();
    }
    
    /* ИСПРАВЛЕНО: Используем native_set_field_value */
    JavaValue val = { .i = value };
    native_set_field_value(int_obj, "value", val);
    
    return NATIVE_RETURN_OBJECT(int_obj);
}

static JavaValue native_integer_parseInt(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    JavaString* str = (JavaString*)args[0].ref;
    int radix = 10;

    /* Handle parseInt(String, int) variant - args[1] is the radix */
    if (arg_count >= 2) {
        radix = args[1].i;
    }

    if (!str) {
        /* FIX-19e2 (v34.33): Integer.parseInt(null) must throw
         * NumberFormatException, NOT NullPointerException (JDK behavior:
         * "throw new NumberFormatException(\"null\")"). Jewel Quest 2's
         * g.<init> reads getAppProperty("FORCESCREENWIDTH") and catches ONLY
         * NumberFormatException — with NPE the constructor died, startApp
         * aborted and the game showed a black screen forever. */
        jvm_throw_by_name(jvm, "java/lang/NumberFormatException", "null");
        thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_INT(0);
    }

    const char* utf8 = string_utf8(jvm, str);
    if (!utf8) {
        jvm_throw_by_name(jvm, "java/lang/NumberFormatException", "null string");
        thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_INT(0);
    }

    /* FIX-19e: rewrite per JVMS Integer.parseInt spec.
     * Old code was hex-only (letters 'a'..'f'), so parseInt("kk",36) threw
     * NFE and aborted whole test groups. It also silently skipped leading/
     * trailing whitespace (spec: parseInt(" 1") MUST throw NFE) and had no
     * overflow detection (parseInt("2147483648") wrapped around silently).
     * Overflow-safe accumulation is done in NEGATIVE space exactly like
     * the JDK: result can reach Integer.MIN_VALUE but never overflow. */
    if (radix < 2 || radix > 36) {
        jvm_throw_by_name(jvm, "java/lang/NumberFormatException", "radix out of range");
        thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_INT(0);
    }

    int negative = 0;
    const char* p = utf8;
    if (*p == '-') { negative = 1; p++; }
    else if (*p == '+') { p++; }

    if (*p == '\0') {
        jvm_throw_by_name(jvm, "java/lang/NumberFormatException", utf8);
        thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_INT(0);
    }

    const jint limit = negative ? INT32_MIN : -INT32_MAX;
    const jint multmin = limit / radix;
    jint result = 0;

    while (*p != '\0') {
        int digit;
        char c = *p;
        if (c >= '0' && c <= '9') {
            digit = c - '0';
        } else if (c >= 'a' && c <= 'z') {
            digit = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'Z') {
            digit = c - 'A' + 10;
        } else {
            jvm_throw_by_name(jvm, "java/lang/NumberFormatException", utf8);
            thread->pending_exception = jvm_exception_pending(jvm);
            return NATIVE_RETURN_INT(0);
        }

        if (digit >= radix) {
            jvm_throw_by_name(jvm, "java/lang/NumberFormatException", utf8);
            thread->pending_exception = jvm_exception_pending(jvm);
            return NATIVE_RETURN_INT(0);
        }

        if (result < multmin) {
            jvm_throw_by_name(jvm, "java/lang/NumberFormatException", utf8);
            thread->pending_exception = jvm_exception_pending(jvm);
            return NATIVE_RETURN_INT(0);
        }
        result *= radix;
        if (result < limit + digit) {
            jvm_throw_by_name(jvm, "java/lang/NumberFormatException", utf8);
            thread->pending_exception = jvm_exception_pending(jvm);
            return NATIVE_RETURN_INT(0);
        }
        result -= digit;
        p++;
    }

    return NATIVE_RETURN_INT(negative ? result : -result);
}

static JavaValue native_integer_toString(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    jint value = args[0].i;

    char buffer[16];
    snprintf(buffer, sizeof(buffer), "%d", value);

    JavaString* str = jvm_new_string(jvm, buffer);
    return NATIVE_RETURN_OBJECT(str);
}

/* FIX-19j: Integer.toString(int, radix) was not registered, so it resolved
 * to a stub returning null ("toString(255,16)" -> null). JDK algorithm:
 * radix outside [2,36] falls back to 10; lowercase digits; '-' for negatives. */
static JavaValue native_integer_toString_radix(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    jint value = args[0].i;
    int radix = (int)args[1].i;
    if (radix < 2 || radix > 36) {
        radix = 10;
    }

    char buffer[40];
    char* p = buffer + sizeof(buffer) - 1;
    *p = '\0';
    juint mag = (value < 0) ? (juint)(-(jlong)value) : (juint)value;
    int negative = (value < 0);

    do {
        *(--p) = "0123456789abcdefghijklmnopqrstuvwxyz"[mag % (juint)radix];
        mag /= (juint)radix;
    } while (mag != 0);

    if (negative) {
        *(--p) = '-';
    }

    JavaString* str = jvm_new_string(jvm, p);
    return NATIVE_RETURN_OBJECT(str);
}

static JavaValue native_integer_intValue(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* int_obj = (JavaObject*)args[0].ref;
    
    if (!int_obj) {
        return NATIVE_RETURN_INT(0);
    }
    
    /* ИСПРАВЛЕНО: Используем native_get_field_value */
    return NATIVE_RETURN_INT(native_get_field_value(int_obj, "value").i);
}

/* v34.49: the whole Number narrowing/widening family for java/lang/Integer.
 * [INVOKE-MISSING] logs from several games showed obfuscated resource
 * loaders calling Integer.byteValue() (obfuscators turn int locals into
 * Integer boxes); the no-op stub pushed 0 — silently corrupting every
 * byte produced through that path. JLS 5.1.3 narrowing semantics. */
static JavaValue native_integer_byteValue(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* o = (JavaObject*)args[0].ref;
    return NATIVE_RETURN_INT(o ? (jint)(jbyte)native_get_field_value(o, "value").i : 0);
}

static JavaValue native_integer_shortValue(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* o = (JavaObject*)args[0].ref;
    return NATIVE_RETURN_INT(o ? (jint)(jshort)native_get_field_value(o, "value").i : 0);
}

static JavaValue native_integer_longValue(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* o = (JavaObject*)args[0].ref;
    return NATIVE_RETURN_LONG(o ? (jlong)native_get_field_value(o, "value").i : 0);
}

static JavaValue native_integer_floatValue(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* o = (JavaObject*)args[0].ref;
    return NATIVE_RETURN_FLOAT(o ? (jfloat)native_get_field_value(o, "value").i : 0.0f);
}

static JavaValue native_integer_doubleValue(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* o = (JavaObject*)args[0].ref;
    return NATIVE_RETURN_DOUBLE(o ? (jdouble)native_get_field_value(o, "value").i : 0.0);
}

/* Integer.hashCode() - the int value itself */
static JavaValue native_integer_hashCode(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* o = (JavaObject*)args[0].ref;
    return NATIVE_RETURN_INT(o ? native_get_field_value(o, "value").i : 0);
}

/* Integer.compareTo(Integer) */
static JavaValue native_integer_compareTo(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* a = (JavaObject*)args[0].ref;
    JavaObject* b = (JavaObject*)args[1].ref;
    if (!a) return NATIVE_RETURN_INT(b ? -1 : 0);
    if (!b) return NATIVE_RETURN_INT(1);
    jint va = native_get_field_value(a, "value").i;
    jint vb = native_get_field_value(b, "value").i;
    return NATIVE_RETURN_INT(va < vb ? -1 : (va == vb ? 0 : 1));
}

/* v34.49: same Number family for Short/Byte/Long/Float/Double — the
 * obfuscated loaders that produced the Integer.byteValue() reports switch
 * wrapper classes between builds, so cover the whole CLDC 1.1 set.
 * Field layouts: Short/Byte store 'value' as int (.i), Long as .j,
 * Float as .f, Double as .d (see the existing *Value natives). */

static JavaValue native_short_byteValue(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* o = (JavaObject*)args[0].ref;
    return NATIVE_RETURN_INT(o ? (jint)(jbyte)native_get_field_value(o, "value").i : 0);
}

static JavaValue native_short_intValue(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* o = (JavaObject*)args[0].ref;
    return NATIVE_RETURN_INT(o ? native_get_field_value(o, "value").i : 0);
}

static JavaValue native_short_longValue(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* o = (JavaObject*)args[0].ref;
    return NATIVE_RETURN_LONG(o ? (jlong)(jshort)native_get_field_value(o, "value").i : 0);
}

static JavaValue native_short_floatValue(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* o = (JavaObject*)args[0].ref;
    return NATIVE_RETURN_FLOAT(o ? (jfloat)(jshort)native_get_field_value(o, "value").i : 0.0f);
}

static JavaValue native_short_doubleValue(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* o = (JavaObject*)args[0].ref;
    return NATIVE_RETURN_DOUBLE(o ? (jdouble)(jshort)native_get_field_value(o, "value").i : 0.0);
}

static JavaValue native_byte_intValue(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* o = (JavaObject*)args[0].ref;
    return NATIVE_RETURN_INT(o ? (jint)(jbyte)native_get_field_value(o, "value").i : 0);
}

static JavaValue native_byte_shortValue(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* o = (JavaObject*)args[0].ref;
    return NATIVE_RETURN_INT(o ? (jint)(jshort)(jbyte)native_get_field_value(o, "value").i : 0);
}

static JavaValue native_byte_longValue(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* o = (JavaObject*)args[0].ref;
    return NATIVE_RETURN_LONG(o ? (jlong)(jbyte)native_get_field_value(o, "value").i : 0);
}

static JavaValue native_byte_floatValue(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* o = (JavaObject*)args[0].ref;
    return NATIVE_RETURN_FLOAT(o ? (jfloat)(jbyte)native_get_field_value(o, "value").i : 0.0f);
}

static JavaValue native_byte_doubleValue(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* o = (JavaObject*)args[0].ref;
    return NATIVE_RETURN_DOUBLE(o ? (jdouble)(jbyte)native_get_field_value(o, "value").i : 0.0);
}

static JavaValue native_long_byteValue(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* o = (JavaObject*)args[0].ref;
    return NATIVE_RETURN_INT(o ? (jint)(jbyte)native_get_field_value(o, "value").j : 0);
}

static JavaValue native_long_shortValue(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* o = (JavaObject*)args[0].ref;
    return NATIVE_RETURN_INT(o ? (jint)(jshort)native_get_field_value(o, "value").j : 0);
}

static JavaValue native_long_intValue(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* o = (JavaObject*)args[0].ref;
    return NATIVE_RETURN_INT(o ? (jint)native_get_field_value(o, "value").j : 0);
}

static JavaValue native_long_floatValue(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* o = (JavaObject*)args[0].ref;
    return NATIVE_RETURN_FLOAT(o ? (jfloat)native_get_field_value(o, "value").j : 0.0f);
}

static JavaValue native_long_doubleValue(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* o = (JavaObject*)args[0].ref;
    return NATIVE_RETURN_DOUBLE(o ? (jdouble)native_get_field_value(o, "value").j : 0.0);
}

static JavaValue native_float_byteValue(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* o = (JavaObject*)args[0].ref;
    return NATIVE_RETURN_INT(o ? (jint)(jbyte)native_get_field_value(o, "value").f : 0);
}

static JavaValue native_float_shortValue(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* o = (JavaObject*)args[0].ref;
    return NATIVE_RETURN_INT(o ? (jint)(jshort)native_get_field_value(o, "value").f : 0);
}

static JavaValue native_float_intValue(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* o = (JavaObject*)args[0].ref;
    return NATIVE_RETURN_INT(o ? (jint)native_get_field_value(o, "value").f : 0);
}

static JavaValue native_float_longValue(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* o = (JavaObject*)args[0].ref;
    return NATIVE_RETURN_LONG(o ? (jlong)native_get_field_value(o, "value").f : 0);
}

static JavaValue native_float_doubleValue(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* o = (JavaObject*)args[0].ref;
    return NATIVE_RETURN_DOUBLE(o ? (jdouble)native_get_field_value(o, "value").f : 0.0);
}

static JavaValue native_double_byteValue(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* o = (JavaObject*)args[0].ref;
    return NATIVE_RETURN_INT(o ? (jint)(jbyte)native_get_field_value(o, "value").d : 0);
}

static JavaValue native_double_shortValue(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* o = (JavaObject*)args[0].ref;
    return NATIVE_RETURN_INT(o ? (jint)(jshort)native_get_field_value(o, "value").d : 0);
}

static JavaValue native_double_intValue(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* o = (JavaObject*)args[0].ref;
    return NATIVE_RETURN_INT(o ? (jint)native_get_field_value(o, "value").d : 0);
}

static JavaValue native_double_longValue(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* o = (JavaObject*)args[0].ref;
    return NATIVE_RETURN_LONG(o ? (jlong)native_get_field_value(o, "value").d : 0);
}

static JavaValue native_double_floatValue(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* o = (JavaObject*)args[0].ref;
    return NATIVE_RETURN_FLOAT(o ? (jfloat)native_get_field_value(o, "value").d : 0.0f);
}

/* Integer.equals(Object) - compares int values */
static JavaValue native_integer_equals(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm;
    (void)thread; (void)arg_count;
    JavaObject* int_obj = (JavaObject*)args[0].ref;
    JavaObject* other = (JavaObject*)args[1].ref;
    
    if (int_obj == other) return NATIVE_RETURN_INT(1);
    if (!int_obj || !other) return NATIVE_RETURN_INT(0);
    
    /* Check if other is an Integer instance */
    JavaClass* other_class = other->header.clazz;
    if (!other_class) return NATIVE_RETURN_INT(0);
    
    /* Walk up class hierarchy to check if it's Integer */
    int is_integer = 0;
    JavaClass* cls = other_class;
    while (cls) {
        if (cls->class_name && strcmp(cls->class_name, "java/lang/Integer") == 0) {
            is_integer = 1;
            break;
        }
        cls = cls->super_class;
    }
    
    if (!is_integer) return NATIVE_RETURN_INT(0);
    
    /* Compare int values */
    int this_val = native_get_field_value(int_obj, "value").i;
    int other_val = native_get_field_value(other, "value").i;
    return NATIVE_RETURN_INT(this_val == other_val ? 1 : 0);
}

/* Integer(int) constructor */
static JavaValue native_integer_init(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* int_obj = (JavaObject*)args[0].ref;
    jint value = args[1].i;
    
    if (!int_obj) {
        return NATIVE_RETURN_VOID();
    }
    
    /* ИСПРАВЛЕНО: Используем native_set_field_value */
    JavaValue val = { .i = value };
    native_set_field_value(int_obj, "value", val);
    
    return NATIVE_RETURN_VOID();
}

/* Integer.toHexString(int) - convert int to hexadecimal string */
static JavaValue native_integer_toHexString(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    jint value = args[0].i;
    
    char buffer[12];  /* Enough for 32-bit hex: 8 chars + "0x" + null */
    snprintf(buffer, sizeof(buffer), "%x", (unsigned int)value);
    
    JavaString* str = jvm_new_string(jvm, buffer);
    return NATIVE_RETURN_OBJECT(str);
}

/* Integer.toHexString(long) - convert long to hexadecimal string (for Long) */
static JavaValue native_integer_toHexString_long(JVM* jvm, JavaThread* thread,
                                                 JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    jlong value = args[0].j;
    
    char buffer[20];  /* Enough for 64-bit hex: 16 chars + null */
    snprintf(buffer, sizeof(buffer), "%llx", (unsigned long long)value);
    
    JavaString* str = jvm_new_string(jvm, buffer);
    return NATIVE_RETURN_OBJECT(str);
}

/* Integer.toOctalString(int) - convert int to octal string */
static JavaValue native_integer_toOctalString(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    jint value = args[0].i;
    
    char buffer[16];  /* Enough for 32-bit octal */
    snprintf(buffer, sizeof(buffer), "%o", (unsigned int)value);
    
    JavaString* str = jvm_new_string(jvm, buffer);
    return NATIVE_RETURN_OBJECT(str);
}

/* Integer.toBinaryString(int) - convert int to binary string */
static JavaValue native_integer_toBinaryString(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    jint value = args[0].i;
    
    /* Convert to binary string manually */
    char buffer[33];  /* 32 bits + null */
    buffer[32] = '\0';
    
    unsigned int uval = (unsigned int)value;
    for (int i = 31; i >= 0; i--) {
        buffer[i] = (uval & 1) ? '1' : '0';
        uval >>= 1;
    }
    
    /* Skip leading zeros */
    char* start = buffer;
    while (*start == '0' && *(start + 1) != '\0') {
        start++;
    }
    
    JavaString* str = jvm_new_string(jvm, start);
    return NATIVE_RETURN_OBJECT(str);
}

/* v58: defined near init_java_lang_byte_short (below); fwd decl because
 * native_integer_init_string / native_long_init_string live earlier. */
static int native_parse_decimal_strict(const char* s, jint lo, jint hi, jint* out);

/* v58: Integer(String) \u2014 same missing-constructor family as Byte/Short. */
static JavaValue native_integer_init_string(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaString* str = (JavaString*)args[1].ref;
    const char* utf8 = str ? string_utf8(jvm, str) : NULL;
    jint v;
    if (!utf8 || !native_parse_decimal_strict(utf8, INT32_MIN, INT32_MAX, &v)) {
        jvm_throw_by_name(jvm, "java/lang/NumberFormatException", utf8 ? utf8 : "null");
        thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_VOID();
    }
    if (obj) {
        JavaValue val = { .i = v };
        native_set_field_value(obj, "value", val);
    }
    return NATIVE_RETURN_VOID();
}

void init_java_lang_integer(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"java/lang/Integer", "<init>", "(Ljava/lang/String;)V", native_integer_init_string},
        {"java/lang/Integer", "<init>", "(I)V", native_integer_init},
        {"java/lang/Integer", "valueOf", "(I)Ljava/lang/Integer;", native_integer_valueOf},
        {"java/lang/Integer", "valueOf", "(Ljava/lang/String;)Ljava/lang/Integer;", native_integer_valueOf_string},
        {"java/lang/Integer", "parseInt", "(Ljava/lang/String;)I", native_integer_parseInt},
        {"java/lang/Integer", "parseInt", "(Ljava/lang/String;I)I", native_integer_parseInt},
        {"java/lang/Integer", "toString", "(I)Ljava/lang/String;", native_integer_toString},
        {"java/lang/Integer", "toString", "(II)Ljava/lang/String;", native_integer_toString_radix},
        {"java/lang/Integer", "toHexString", "(I)Ljava/lang/String;", native_integer_toHexString},
        {"java/lang/Integer", "toOctalString", "(I)Ljava/lang/String;", native_integer_toOctalString},
        {"java/lang/Integer", "toBinaryString", "(I)Ljava/lang/String;", native_integer_toBinaryString},
        {"java/lang/Integer", "intValue", "()I", native_integer_intValue},
        {"java/lang/Integer", "byteValue", "()B", native_integer_byteValue},
        {"java/lang/Integer", "shortValue", "()S", native_integer_shortValue},
        {"java/lang/Integer", "longValue", "()J", native_integer_longValue},
        {"java/lang/Integer", "floatValue", "()F", native_integer_floatValue},
        {"java/lang/Integer", "doubleValue", "()D", native_integer_doubleValue},
        {"java/lang/Integer", "hashCode", "()I", native_integer_hashCode},
        {"java/lang/Integer", "compareTo", "(Ljava/lang/Integer;)I", native_integer_compareTo},
        {"java/lang/Integer", "toString", "()Ljava/lang/String;", native_integer_toString_instance},
        {"java/lang/Integer", "equals", "(Ljava/lang/Object;)Z", native_integer_equals},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
}

/*
 * java.lang.Long native methods
 */

/* Long.longValue() - returns long value with null-safety */
static JavaValue native_long_longValue(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* long_obj = (JavaObject*)args[0].ref;
    
    /* NULL-SAFETY: Return 0 for null instead of NPE */
    if (!long_obj) {
        NATIVE_DEBUG("Long.longValue: null object, returning 0");
        return NATIVE_RETURN_LONG(0);
    }
    
    /* Get the value field - Long stores value as long (J type) */
    jlong val = native_get_field_value(long_obj, "value").j;
    return NATIVE_RETURN_LONG(val);
}

/* Long.valueOf(long) - creates Long object from long value */
static JavaValue native_long_valueOf(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    jlong value = args[0].j;

    /* v34.58: кэш -128..127 (JLS 5.1.7) */
    if (value >= BOX_CACHE_MIN && value <= BOX_CACHE_MAX) {
        pthread_mutex_lock(&g_box_cache_mutex);
        JavaObject* cached = box_cache_get_locked(
                jvm, &g_long_cache[value - BOX_CACHE_MIN],
                "java/lang/Long", (JavaValue){ .j = value });
        pthread_mutex_unlock(&g_box_cache_mutex);
        if (cached) return NATIVE_RETURN_OBJECT(cached);
    }

    JavaClass* long_class = jvm_load_class(jvm, "java/lang/Long");
    if (!long_class) {
        return NATIVE_RETURN_NULL();
    }
    
    JavaObject* long_obj = jvm_new_object(jvm, long_class);
    if (!long_obj) {
        return NATIVE_RETURN_NULL();
    }
    
    /* Set the value field */
    JavaValue val = { .j = value };
    native_set_field_value(long_obj, "value", val);
    
    return NATIVE_RETURN_OBJECT(long_obj);
}

/* Long.<init>(long) - constructor */
static JavaValue native_long_init(JVM* jvm, JavaThread* thread,
                                   JavaValue* args, int arg_count) {
    (void)jvm;
    (void)thread; (void)arg_count;
    JavaObject* long_obj = (JavaObject*)args[0].ref;
    jlong value = args[1].j;
    
    if (!long_obj) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Set the value field */
    JavaValue val = { .j = value };
    native_set_field_value(long_obj, "value", val);
    
    return NATIVE_RETURN_VOID();
}

/* Long.parseLong(String) - parse string to long */
static JavaValue native_long_parseLong(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;

    if (!str) {
        /* v34.33: same JDK-conformance fix as Integer.parseInt —
         * parseLong(null) throws NumberFormatException, not NPE. */
        jvm_throw_by_name(jvm, "java/lang/NumberFormatException", "null");
        thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_LONG(0);
    }

    const char* utf8 = string_utf8(jvm, str);
    if (!utf8) {
        jvm_throw_by_name(jvm, "java/lang/NumberFormatException", "null");
        thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_LONG(0);
    }

    /* FIX-19f: strtoll clamps overflow to LONG_MAX/LONG_MIN with no error
     * signal, so parseLong("9223372036854775808") silently returned
     * Long.MAX_VALUE instead of throwing NumberFormatException. Same
     * JDK negative-space accumulation as parseInt (FIX-19e). */
    int negative = 0;
    const char* p = utf8;
    if (*p == '-') { negative = 1; p++; }
    else if (*p == '+') { p++; }

    if (*p == '\0') {
        jvm_throw_by_name(jvm, "java/lang/NumberFormatException", utf8);
        thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_LONG(0);
    }

    const jlong limit = negative ? INT64_MIN : -INT64_MAX;
    const jlong multmin = limit / 10;
    jlong result = 0;

    while (*p != '\0') {
        char c = *p;
        if (c < '0' || c > '9') {
            jvm_throw_by_name(jvm, "java/lang/NumberFormatException", utf8);
            thread->pending_exception = jvm_exception_pending(jvm);
            return NATIVE_RETURN_LONG(0);
        }
        int digit = c - '0';

        if (result < multmin) {
            jvm_throw_by_name(jvm, "java/lang/NumberFormatException", utf8);
            thread->pending_exception = jvm_exception_pending(jvm);
            return NATIVE_RETURN_LONG(0);
        }
        result *= 10;
        if (result < limit + digit) {
            jvm_throw_by_name(jvm, "java/lang/NumberFormatException", utf8);
            thread->pending_exception = jvm_exception_pending(jvm);
            return NATIVE_RETURN_LONG(0);
        }
        result -= digit;
        p++;
    }

    return NATIVE_RETURN_LONG(negative ? result : -result);
}

/* Long.toString(long) - convert long to string */
static JavaValue native_long_toString(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    jlong value = args[0].j;
    
    char buffer[24];  /* Enough for 64-bit signed: -9223372036854775808 */
    snprintf(buffer, sizeof(buffer), "%lld", (long long)value);
    
    JavaString* str = jvm_new_string(jvm, buffer);
    return NATIVE_RETURN_OBJECT(str);
}

/* v36.33: Long.toString() INSTANCE overload was never registered (Integer/
 * Short/Byte/Character/Float/Double/Boolean all have theirs) — the virtual
 * call fell through to Object.toString's "java.lang.Long@<hash>" default,
 * and games parsing their own Long printouts got NumberFormatExceptions
 * (Jewel Adventure, ar.a: "java.lang.Long@e5805dd8" every ~2 s). */
static JavaValue native_long_toString_instance(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_OBJECT(NULL);
    }
    jlong value = native_get_field_value(obj, "value").j;
    char buffer[24];
    snprintf(buffer, sizeof(buffer), "%lld", (long long)value);
    JavaString* str = jvm_new_string(jvm, buffer);
    return NATIVE_RETURN_OBJECT(str);
}

/* v58: Long(String) \u2014 parse via strtol into jlong, strict. */
static JavaValue native_long_init_string(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaString* str = (JavaString*)args[1].ref;
    const char* utf8 = str ? string_utf8(jvm, str) : NULL;
    jlong v = 0;
    int ok = 0;
    if (utf8 && *utf8) {
        char* endp = NULL;
        errno = 0;
        long long r = strtoll(utf8, &endp, 10);
        /* strict: whole string must be consumed, optional sign handled */
        if (endp && *endp == '\0' && endp != utf8) {
            /* reject lone "+" / "-" */
            const char* q = utf8;
            if (*q == '+' || *q == '-') q++;
            if (*q >= '0' && *q <= '9') { v = (jlong)r; ok = 1; }
        }
    }
    if (!ok) {
        jvm_throw_by_name(jvm, "java/lang/NumberFormatException", utf8 ? utf8 : "null");
        thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_VOID();
    }
    if (obj) {
        JavaValue val = { .j = v };
        native_set_field_value(obj, "value", val);
    }
    return NATIVE_RETURN_VOID();
}

void init_java_lang_long(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"java/lang/Long", "<init>", "(Ljava/lang/String;)V", native_long_init_string},
        {"java/lang/Long", "<init>", "(J)V", native_long_init},
        {"java/lang/Long", "valueOf", "(J)Ljava/lang/Long;", native_long_valueOf},
        {"java/lang/Long", "parseLong", "(Ljava/lang/String;)J", native_long_parseLong},
        {"java/lang/Long", "toString", "(J)Ljava/lang/String;", native_long_toString},
        /* v36.33: instance toString ()Ljava/lang/String; — was missing, the
         * call fell through to Object.toString's "java.lang.Long@hash". */
        {"java/lang/Long", "toString", "()Ljava/lang/String;", native_long_toString_instance},
        {"java/lang/Long", "toHexString", "(J)Ljava/lang/String;", native_integer_toHexString_long},
        {"java/lang/Long", "longValue", "()J", native_long_longValue},
        {"java/lang/Long", "byteValue", "()B", native_long_byteValue},
        {"java/lang/Long", "shortValue", "()S", native_long_shortValue},
        {"java/lang/Long", "intValue", "()I", native_long_intValue},
        {"java/lang/Long", "floatValue", "()F", native_long_floatValue},
        {"java/lang/Long", "doubleValue", "()D", native_long_doubleValue},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
}

/*
 * java.lang.Boolean native methods
 */

static JavaValue native_boolean_valueOf(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    jboolean value = args[0].i != 0;

    /* v34.58: синглтоны TRUE/FALSE (JLS: Boolean.valueOf обязан
     * возвращать один и тот же объект) */
    {
        pthread_mutex_lock(&g_box_cache_mutex);
        JavaObject* cached = box_cache_get_locked(
                jvm, &g_bool_cache[value ? 1 : 0],
                "java/lang/Boolean", (JavaValue){ .i = value ? 1 : 0 });
        pthread_mutex_unlock(&g_box_cache_mutex);
        if (cached) return NATIVE_RETURN_OBJECT(cached);
    }

    JavaClass* bool_class = jvm_load_class(jvm, "java/lang/Boolean");
    if (!bool_class) {
        return NATIVE_RETURN_NULL();
    }
    
    JavaObject* bool_obj = jvm_new_object(jvm, bool_class);
    if (!bool_obj) {
        return NATIVE_RETURN_NULL();
    }
    
    /* ИСПРАВЛЕНО: Используем native_set_field_value */
    JavaValue val = { .i = value ? 1 : 0 };
    native_set_field_value(bool_obj, "value", val);
    
    return NATIVE_RETURN_OBJECT(bool_obj);
}

/* Boolean.<init>(boolean) - constructor */
static JavaValue native_boolean_init(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* bool_obj = (JavaObject*)args[0].ref;
    jboolean value = args[1].i != 0;
    
    if (!bool_obj) {
        return NATIVE_RETURN_VOID();
    }
    
    JavaValue val = { .i = value ? 1 : 0 };
    native_set_field_value(bool_obj, "value", val);
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_boolean_parseBoolean(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    
    if (!str) {
        return NATIVE_RETURN_INT(0);
    }
    
    const char* utf8 = string_utf8(jvm, str);
    if (utf8 && strcmp(utf8, "true") == 0) {
        return NATIVE_RETURN_INT(1);
    }
    
    return NATIVE_RETURN_INT(0);
}

/* Boolean.equals(Object) - compares boolean values */
static JavaValue native_boolean_equals(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* bool_obj = (JavaObject*)args[0].ref;
    JavaObject* other = (JavaObject*)args[1].ref;
    
    if (bool_obj == other) return NATIVE_RETURN_INT(1);
    if (!bool_obj || !other) return NATIVE_RETURN_INT(0);
    
    /* Check if other is a Boolean instance */
    JavaClass* other_class = other->header.clazz;
    if (!other_class) return NATIVE_RETURN_INT(0);
    
    int is_boolean = 0;
    JavaClass* cls = other_class;
    while (cls) {
        if (cls->class_name && strcmp(cls->class_name, "java/lang/Boolean") == 0) {
            is_boolean = 1;
            break;
        }
        cls = cls->super_class;
    }
    
    if (!is_boolean) return NATIVE_RETURN_INT(0);
    
    /* Compare boolean values */
    int this_val = native_get_field_value(bool_obj, "value").i;
    int other_val = native_get_field_value(other, "value").i;
    return NATIVE_RETURN_INT(this_val == other_val ? 1 : 0);
}

/* v58: Boolean(String) \u2014 true iff the string equals (ignoring case)
 * "true"; anything else (including null) yields false. No exception. */
static JavaValue native_boolean_init_string(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaString* str = (JavaString*)args[1].ref;
    int b = 0;
    if (str) {
        const char* s = string_utf8(jvm, str);
        if (s && s[0] == 't' && s[1] == 'r' && s[2] == 'u' && s[3] == 'e' && s[4] == '\0')
            b = 1;
        else if (s) {
            /* case-insensitive compare */
            const char* p = s; const char* t = "true";
            b = 1;
            while (*p || *t) {
                if (tolower((unsigned char)*p) != (unsigned char)*t) { b = 0; break; }
                p++; t++;
            }
        }
    }
    if (obj) {
        JavaValue val = { .i = b };
        native_set_field_value(obj, "value", val);
    }
    return NATIVE_RETURN_VOID();
}

void init_java_lang_boolean(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"java/lang/Boolean", "<init>", "(Ljava/lang/String;)V", native_boolean_init_string},
        {"java/lang/Boolean", "<init>", "(Z)V", native_boolean_init},
        {"java/lang/Boolean", "valueOf", "(Z)Ljava/lang/Boolean;", native_boolean_valueOf},
        {"java/lang/Boolean", "parseBoolean", "(Ljava/lang/String;)Z", native_boolean_parseBoolean},
        {"java/lang/Boolean", "booleanValue", "()Z", native_integer_intValue},
        {"java/lang/Boolean", "toString", "()Ljava/lang/String;", native_boolean_toString},
        {"java/lang/Boolean", "equals", "(Ljava/lang/Object;)Z", native_boolean_equals},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
    
    /* Initialize Boolean.TRUE and Boolean.FALSE static fields */
    JavaClass* bool_class = jvm_load_class(jvm, "java/lang/Boolean");
    if (bool_class) {
        /* Ensure static fields exist */
        if (bool_class->static_fields_count == 0) {
            bool_class->static_fields = (JavaStaticField*)calloc(2, sizeof(JavaStaticField));
            if (bool_class->static_fields) {
                bool_class->static_fields[0].name = strdup("TRUE");
                bool_class->static_fields[0].descriptor = strdup("Ljava/lang/Boolean;");
                memset(&bool_class->static_fields[0].value, 0, sizeof(JavaValue));
                bool_class->static_fields[1].name = strdup("FALSE");
                bool_class->static_fields[1].descriptor = strdup("Ljava/lang/Boolean;");
                memset(&bool_class->static_fields[1].value, 0, sizeof(JavaValue));
                bool_class->static_fields_count = 2;
                bool_class->static_fields_capacity = 2;
            }
        }
        
        /* Create Boolean.TRUE = new Boolean(true) */
        if (bool_class->static_fields_count >= 1 && !bool_class->static_fields[0].value.ref) {
            JavaObject* true_obj = jvm_new_object(jvm, bool_class);
            if (true_obj) {
                JavaValue val_true = { .i = 1 };
                native_set_field_value(true_obj, "value", val_true);
                bool_class->static_fields[0].value.ref = true_obj;
            }
        }
        
        /* Create Boolean.FALSE = new Boolean(false) */
        if (bool_class->static_fields_count >= 2 && !bool_class->static_fields[1].value.ref) {
            JavaObject* false_obj = jvm_new_object(jvm, bool_class);
            if (false_obj) {
                JavaValue val_false = { .i = 0 };
                native_set_field_value(false_obj, "value", val_false);
                bool_class->static_fields[1].value.ref = false_obj;
            }
        }
    }
}

/*
 * java.lang.System native methods
 */

/* ================= v36.25 [SYSPROPS] единая таблица свойств =================
 * Запрос пользователя: «сейчас не установлены java.name java.version
 * java.vm.name device.vendor device.imei java.heap.size java.heap.free
 * java.stack.size и подобные — стоит их установить для совместимости».
 *
 * ДИАГНОЗ: значения ЧАСТИЧНО ходили, но только в одном из двух путей:
 *  - System.getProperty(String) — ГЛАВНЫЙ путь игр — имел жёсткий if-chain
 *    из ~20 ключей: microedition.*, imei, mem; java.* и device.* там НЕ было,
 *    поэтому игры получали NULL и считали свойства «не установленными»;
 *  - System.getProperties() (Hashtable) имел широкую таблицу, но игры
 *    ей почти не пользуются.
 * Теперь ОБА пути читают одну таблицу g_sys_props + динамические ключи:
 *  - microedition.locale — от настройки языка Switch (switch_locale_get,
 *    слабая линковка; RU <-> "ru", EN <-> "en"; вне Switch — "en");
 *  - java.heap.size / java.heap.free (+ легаси totalMemory/freeMemory) —
 *    фактическая статистика кучи (heap_get_stats), а не константы;
 *  - java.stack.size — JAVA_STACK_SIZE (256 КБ на поток).
 * Значения унифицированы по поведению, проверенному играми на live-пути
 * (platform=NokiaN73-1, profiles=MIDP-2.0, capture/recording=false). */
typedef struct { const char* key; const char* val; } SysPropEntry;
static const SysPropEntry g_sys_props[] = {
    /* CLDC/MIDP required */
    { "microedition.platform",       "NokiaN73-1" },
    { "microedition.encoding",       "UTF-8" },
    { "microedition.configuration",  "CLDC-1.1" },
    { "microedition.profiles",       "MIDP-2.0" },
    /* Optional/common MIDP */
    { "microedition.hostname",       "localhost" },
    { "microedition.smartcardslots", "0" },
    { "microedition.commports",      "" },
    { "microedition.pim.version",    "1.0" },
    { "microedition.io.file.FileConnection.version", "1.0" },
    /* v36.21 [CAP-REPORT]: JSR-184 M3G / JSR-135 MMAPI реализованы —
     * сообщаем как реальный телефон (игры решают «доступно ли 3D/микшер»
     * именно по этим ключам). */
    { "microedition.m3g.version",    "1.1" },
    { "microedition.media.version",  "1.0" },
    { "audio.encodings",             "encoding=audio/amr" },
    { "supports.mixing",             "true" },
    { "supports.audio.capture",      "false" },
    { "supports.video.capture",      "false" },
    { "supports.recording",          "false" },
    /* IMEI-семейство (когда включён drm-спуф, реальные значения подставляет
     * drm_get_spoofed_property РАНЬШЕ этой таблицы) */
    { "com.nokia.mid.imei",          "000000000000000" },
    { "device.imei",                 "000000000000000" },
    { "phone.imei",                  "000000000000000" },
    { "com.nokia.mid.imsi",          "000000000000000" },
    /* v36.25 NEW: java.* и device.* — запрос пользователя */
    { "java.name",                   "Java Platform, Micro Edition" },
    { "java.version",                "1.4.2" },
    { "java.vendor",                 "Nokia" },
    { "java.vm.name",                "CLDC-HI" },
    { "java.vm.vendor",              "Sun Microsystems Inc." },
    { "java.vm.version",             "1.1" },
    { "java.vm.specification.name",  "CLDC" },
    { "java.vm.specification.version", "1.1" },
    { "java.specification.name",     "Java Platform, Micro Edition" },
    { "java.specification.version",  "CLDC-1.1" },
    { "java.stack.size",             "262144" },      /* JAVA_STACK_SIZE */
    { "device.vendor",               "Nokia" },
    { "device.model",                "Nokia" },
    /* Стандартные Java/платформенные ключи (из прежней таблицы getProperties) */
    { "os.name",                     "SymbianOS" },
    { "os.version",                  "9.4" },
    { "os.arch",                     "ARM" },
    { "file.separator",              "/" },
    { "path.separator",              ";" },
    { "line.separator",              "\n" },
    { "user.agent",                  "Nokia" },
    { "wireless.messaging.sms",      "true" },
    { "wireless.messaging.mms",      "true" },
    { "wireless.messaging.cbs",      "true" },
    { "screen.width",                "240" },
    { "screen.height",               "320" },
    { "screen.isColor",              "true" },
    { "screen.bitsPerPixel",         "16" },
};

static const char* sysprop_table_lookup(const char* key) {
    for (size_t i = 0; i < sizeof(g_sys_props) / sizeof(g_sys_props[0]); i++) {
        if (strcmp(g_sys_props[i].key, key) == 0) return g_sys_props[i].val;
    }
    return NULL;
}

/* Forward: манифест/JAD-данные определены ниже в блоке getAppProperty
 * (midlet_set_manifest / midlet_manifest_reset). */
static char* g_manifest_data;

/* v36.25 [LOCALE-COMPAT]: значение microedition.locale.
 * База — настройка языка Switch (switch_locale_get, слабая линковка;
 * вне switchui-сборок "en"). НО если манифест/JAD текущего мидлета
 * объявляет MIDlet-Languages и нашего кода в списке НЕТ — отдаём "en",
 * а если и "en" не заявлен — ПЕРВЫЙ заявленный язык.
 *
 * Причина (поле): Treasure Towers (MIDlet-Languages: en,es,fr,de,it) при
 * locale="ru" не находит свой язык в j(), пропускает запись RMS и
 * инициализацию текстов, уходит в ветку «язык не сохранён» и виснет в
 * чисто-Java цикле ожидания (td m: nat=currentTimeMillis age=20s+).
 * Реальный аппарат отдал бы "ru-RU" и jar повис так же; наша цель —
 * совместимость: игра стартует на английском. Русские сборки (с "ru" в
 * MIDlet-Languages) по-прежнему получают "ru". */
const char* nojme_sysprop_locale(void) {
    static __thread char s_loc[16];
    const char* code = "en";
    extern const char* switch_locale_get(void) __attribute__((weak));
    if (&switch_locale_get && switch_locale_get) {
        const char* c = switch_locale_get();
        if (c && c[0]) code = c;
    }
    if (g_manifest_data && strcmp(code, "en") != 0) {
        const char* line = strstr(g_manifest_data, "MIDlet-Languages:");
        int has_list = 0, supported = 0, en_present = 0;
        char first_tok[8] = "";
        if (line) {
            char list[256];
            size_t n = 0;
            const char* v = line + strlen("MIDlet-Languages:");
            has_list = 1;
            while (v[n] && v[n] != '\r' && v[n] != '\n' && n < sizeof(list) - 1) {
                list[n] = v[n]; n++;
            }
            list[n] = '\0';
            char* save = NULL;
            for (char* tok = strtok_r(list, ",", &save); tok;
                 tok = strtok_r(NULL, ",", &save)) {
                while (*tok == ' ' || *tok == '\t') tok++;
                size_t tl = strlen(tok);
                while (tl > 0 && (tok[tl - 1] == ' ' || tok[tl - 1] == '\t'))
                    tok[--tl] = '\0';
                if (tl == 0) continue;
                if (first_tok[0] == '\0' && tl < sizeof(first_tok))
                    snprintf(first_tok, sizeof(first_tok), "%s", tok);
                if (strncmp(tok, "en", tl) == 0 || strncmp("en", tok, 2) == 0)
                    en_present = 1;
                /* матч как в играх: locale.startsWith(token) / равенство */
                if (strncmp(tok, code, tl) == 0 ||
                    strncmp(code, tok, strlen(code)) == 0) {
                    supported = 1;
                    break;
                }
            }
        }
        /* Фолбэк ТОЛЬКО когда список заявлен и наш код в него не попал.
         * Без MIDlet-Languages мидлет получает код как есть (ru/en). */
        if (has_list && !supported) {
            if (en_present) return "en";
            if (first_tok[0]) {
                snprintf(s_loc, sizeof(s_loc), "%s", first_tok);
                return s_loc;
            }
            return "en";
        }
    }
    snprintf(s_loc, sizeof(s_loc), "%s", code);
    return s_loc;
}

/* Динамические ключи. Возвращают указатель на thread-local буфер (строка
 * копируется в JavaString немедленно — переиспользование безопасно) либо
 * статическую константу. NULL = ключ не динамический. */
static const char* sysprop_dynamic(JVM* jvm, const char* key) {
    static __thread char s_dyn_buf[24];
    HeapStats st;
    size_t v;
    /* [BATFIX] v36.41 [BATT-PROPS]: заряд аккумулятора для J2ME. Три
     * ключа-процента (Motorola «batterylevel» — самый известный
     * кросс-вендорный хак; Nokia «com.nokia.mid.batterylevel»; наш
     * «nojme.batterylevel») + статус зарядки. Формат — голое число
     * процентов БЕЗ знака «%»: так его можно кормить прямо в
     * Integer.parseInt (игры, которые просто печатают строку, видят
     * «85»). Заряда нет (PSM недоступен / хост без мока) -> NULL, как
     * на телефоне без этого API — игра уходит в свою ветку «неизвестно». */
    if (strcmp(key, "batterylevel") == 0 ||
        strcmp(key, "com.nokia.mid.batterylevel") == 0 ||
        strcmp(key, "nojme.batterylevel") == 0) {
        int pct = nojme_battery_percent();
        if (pct < 0) return NULL;
        snprintf(s_dyn_buf, sizeof(s_dyn_buf), "%d", pct);
        return s_dyn_buf;
    }
    if (strcmp(key, "nojme.battery.charging") == 0) {
        int chg = nojme_battery_charging();
        if (chg < 0) return NULL;
        snprintf(s_dyn_buf, sizeof(s_dyn_buf), "%d", chg);
        return s_dyn_buf;
    }
    if (strcmp(key, "microedition.locale") == 0) {
        /* v36.25: язык из настроек Switch с учётом MIDlet-Languages мидлета
         * ([LOCALE-COMPAT] — см. nojme_sysprop_locale). Слабая линковка
         * switch_locale_get внутри хелпера: вне Switch — "en". */
        return nojme_sysprop_locale();
    }
    if (strcmp(key, "java.heap.size") == 0 ||
        strcmp(key, "totalMemory") == 0 ||
        strcmp(key, "java.runtime.totalMemory") == 0) {
        /* v36.25: фактический размер кучи (до инициализации — дефолт
         * standalone 64 МБ). Прежний фейк totalMemory=1048576 (1 МБ) игры
         * могли читать как «доступно всего 1 МБ» и ужиматься зря. */
        v = 0;
        if (jvm) {
            st = heap_get_stats(jvm);
            v = st.total_size;
        }
        if (!v) v = (size_t)64 * 1024 * 1024;
        snprintf(s_dyn_buf, sizeof(s_dyn_buf), "%zu", v);
        return s_dyn_buf;
    }
    if (strcmp(key, "java.heap.free") == 0 ||
        strcmp(key, "freeMemory") == 0 ||
        strcmp(key, "java.runtime.freeMemory") == 0) {
        v = 0;
        if (jvm) {
            st = heap_get_stats(jvm);
            v = st.free_size;
        }
        if (!v) v = (size_t)64 * 1024 * 1024;
        snprintf(s_dyn_buf, sizeof(s_dyn_buf), "%zu", v);
        return s_dyn_buf;
    }
    return NULL;
}

/* Публичный хелп (юнит-тест scripts/test_switch_input.c): значение свойства
 * из динамических ключей или дефолтной таблицы; dynbuf/cap — буфер для
 * динамического значения (NULL = внутренний thread-local). */
const char* nojme_sysprop_lookup(JVM* jvm, const char* key,
                                 char* dynbuf, size_t cap) {
    const char* v;
    if (!key) return NULL;
    v = sysprop_dynamic(jvm, key);
    if (v) {
        if (dynbuf && cap) {
            snprintf(dynbuf, cap, "%s", v);
            return dynbuf;
        }
        return v;
    }
    return sysprop_table_lookup(key);
}

/* System.getProperty(String key) - получить системное свойство */
static JavaValue native_system_getProperty(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)arg_count;
    
    /* STATIC METHOD - no 'this' argument!
     * args[0] = key string
     */
    JavaString* key_str = (JavaString*)args[0].ref;
    
    if (!key_str) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_NULL();
    }
    
    const char* key = string_utf8(jvm, key_str);
    if (!key) {
        return NATIVE_RETURN_NULL();
    }
    
    NATIVE_DEBUG("getProperty: '%s'", key);
    
    /* DRM BYPASS: Check if property should be hidden (hideEmulation mode) */
    extern bool drm_should_hide_property(const char* key);
    if (drm_should_hide_property(key)) {
        NATIVE_DEBUG("getProperty: '%s' -> NULL (hidden by DRM bypass)", key);
        return NATIVE_RETURN_NULL();
    }
    
    /* DRM BYPASS: Check for spoofed properties */
    extern const char* drm_get_spoofed_property(const char* key);
    const char* spoofed = drm_get_spoofed_property(key);
    if (spoofed) {
        NATIVE_DEBUG("getProperty: '%s' -> '%s' (spoofed)", key, spoofed);
        return NATIVE_RETURN_OBJECT(jvm_new_string(jvm, spoofed));
    }
    
    /* v36.25 [SYSPROPS]: один источник для getProperty и getProperties —
     * динамические ключи (locale от языка Switch, heap по факту) + общая
     * таблица g_sys_props (java.name/java.version/java.vm.name/device.vendor/
     * device.imei/java.heap.size/java.heap.free/java.stack.size и подобные).
     * Прежний if-chain (java.* и device.* там НЕ было) удалён — игры получали
     * NULL на этих ключах и считали свойства «не установленными». */
    const char* value = nojme_sysprop_lookup(jvm, key, NULL, 0);
    
    if (value) {
        NATIVE_DEBUG("getProperty: '%s' -> '%s'", key, value);
        return NATIVE_RETURN_OBJECT(jvm_new_string(jvm, value));
    }

    /* v36.21 [PROP-MISS]: a capability probe that returns null is exactly
     * how games conclude "устройство не умеет" and silently degrade (one
     * music stream, no M3G, no MMAPI...). Log every UNKNOWN key once per
     * process so the next field log NAMES the missing property instead of
     * the symptom. Throttled to 24 unique keys (a game queries a bounded
     * set; repeats are silent). */
    {
        static char s_propmiss_seen[24][48];
        static int  s_propmiss_n = 0;
        int known = 0;
        for (int i = 0; i < s_propmiss_n; i++) {
            if (strncmp(s_propmiss_seen[i], key, sizeof(s_propmiss_seen[0])) == 0) {
                known = 1;
                break;
            }
        }
        if (!known) {
            if (s_propmiss_n < 24) {
                snprintf(s_propmiss_seen[s_propmiss_n], sizeof(s_propmiss_seen[0]),
                         "%s", key);
                s_propmiss_n++;
            }
            LOG_SAFE("[PROP-MISS] '%s' -> null\n", key);
        }
    }

    /* Свойство не найдено - возвращаем null */
    NATIVE_DEBUG("getProperty: '%s' not found, returning null", key);
    return NATIVE_RETURN_NULL();
}

static JavaValue native_system_arraycopy(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)arg_count;
    /* Static method: args[0] is first parameter (src), not 'this' */
    JavaArray* src = (JavaArray*)args[0].ref;
    jint src_pos = args[1].i;
    JavaArray* dest = (JavaArray*)args[2].ref;
    jint dest_pos = args[3].i;
    jint length = args[4].i;
    
    if (!src || !dest) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    
    if (src_pos < 0 || dest_pos < 0 || length < 0 ||
        src_pos + length > src->length ||
        dest_pos + length > dest->length) {
        native_throw_aioobe(jvm, thread, -1);
        return NATIVE_RETURN_VOID();
    }
    
    /* Copy elements — element size taken from the SOURCE array.
     *
     * [BT-CRASH-FIX] (Bounce Tales, Switch Data Abort): the old code took
     * elem_size from src ONLY and memmoved raw bytes. A call with
     * mismatched component types (int[] -> Object[], Object[] -> byte[],
     * char[] -> int[] ...) then either (a) wrote jint PAIRS into 8-byte
     * reference slots — the destination Object[] held "references"
     * composed of two adjacent ints (field evidence: 0xffffffff00460000 =
     * ARGB white + ARGB transparent dark-green from the game's pixel
     * data), and the first aaload/getfield on such a slot crashed natively
     * (Data Abort at <garbage>+0x18), or (b) overran a byte[]/char[]
     * destination 2x-8x, smashing adjacent heap blocks. JLS/MIDP: mismatched
     * component types raise ArrayStoreException and copy NOTHING.
     * Reference arrays of any component class are mutually copyable
     * (pointer-sized, same representation). */
    size_t elem_size = 4;  /* Default to int */
    switch (src->element_type) {
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
    }
    size_t dst_elem_size = 4;
    switch (dest->element_type) {
        case T_BOOLEAN:
        case T_BYTE:    dst_elem_size = 1; break;
        case T_CHAR:
        case T_SHORT:   dst_elem_size = 2; break;
        case T_INT:
        case T_FLOAT:   dst_elem_size = 4; break;
        case T_LONG:
        case T_DOUBLE:  dst_elem_size = 8; break;
        case DESC_OBJECT:
        case DESC_ARRAY: dst_elem_size = sizeof(void*); break;
    }
    bool src_is_ref = (src->element_type == DESC_OBJECT || src->element_type == DESC_ARRAY);
    bool dst_is_ref = (dest->element_type == DESC_OBJECT || dest->element_type == DESC_ARRAY);
    if (elem_size != dst_elem_size || src_is_ref != dst_is_ref ||
        (!src_is_ref && src->element_type != dest->element_type)) {
        static int ase_type_log = 0;
        if (ase_type_log < 10) {
            ase_type_log++;
            LOG_SAFE("[ARRAYCOPY] incompatible component types "
                     "(src type=%d size=%zu, dst type=%d size=%zu, len=%d) "
                     "-> ArrayStoreException\n",
                     (int)src->element_type, elem_size,
                     (int)dest->element_type, dst_elem_size, (int)length);
        }
        native_throw_array_store_exception(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    
    uint8_t* dest_data = (uint8_t*)array_data(dest);
    uint8_t* src_data = (uint8_t*)array_data(src);
    
    memmove(dest_data + dest_pos * elem_size,
            src_data + src_pos * elem_size,
            length * elem_size);
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_system_currentTimeMillis(JVM* jvm, JavaThread* thread,
                                                 JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    
    jlong ms;
    
#ifdef _WIN32
    /* v18 FIX: QueryPerformanceCounter is UPTIME-based (boot-relative),
     * while java.lang.System.currentTimeMillis() must be UNIX-epoch
     * milliseconds. The old code returned uptime on Windows but epoch via
     * CLOCK_REALTIME on Linux - platform-dependent behaviour that broke
     * Date/clock logic on Windows (e.g. VmTest 'new Date() ~
     * currentTimeMillis') and any game timing that mixes both. */
    FILETIME ft;
    ULARGE_INTEGER u;
    GetSystemTimeAsFileTime(&ft);
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    /* FILETIME epoch (1601-01-01 UTC) -> UNIX epoch, 100ns -> 1ms */
    ms = (jlong)((u.QuadPart - 116444736000000000LL) / 10000LL);
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ms = (jlong)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
    
    /* v34.81 FRONTEND PAUSE: виртуальные часы игры — время, проведённое
     * ядром на паузе фронтенда (меню RetroArch), не течёт. Иначе игры с
     * дельтами currentTimeMillis «перепрыгивали» на длительность меню,
     * а таймаутыAlert/Screen сгорали за время настроек. Пауза не
     * срабатывала ни разу -> offset == 0 -> бит-в-бит прежнее поведение. */
    {
        extern uint64_t jvm_pause_offset_ms(void);
        uint64_t off = jvm_pause_offset_ms();
        if (off) ms -= (jlong)off;
    }
    
    return NATIVE_RETURN_LONG(ms);
}

/* v35.07 EXPLICIT GC DEBOUNCE.
 * FIELD EVIDENCE (v35.06 trace, Asphalt 3): at the race->finale transition
 * the game's transition/loading code calls System.gc() in a loop at
 * ~40-60 calls/s for ~10 s (gc=292 + gc=124 in two 5 s diag windows, every
 * one a full stop-the-world mark/sweep, gms=10 ms => ~0.5 s of world-stop
 * per second). That is the "lag right after the race ends" the user feels
 * ON TOP of the game's own 71 ms finale pacing. Real J2ME phones treat
 * gc() as a HINT: KVM/DOJA debounced or coalesced it; no midlet can rely
 * on 60 full collections per second. Minimum gap 250 ms => at most 4 full
 * GCs/s no matter how hard the game asks; allocation-exhaustion and the
 * v35.04 periodic-budget triggers are NOT debounced (memory safety stays
 * unconditional). NOJME_NO_SYSGC=1 still turns the explicit path into a
 * full no-op (now for BOTH System.gc and Runtime.gc — the env used to be
 * wired to Runtime.gc only). Skipped requests are counted and surfaced as
 * a throttled forced trace line ([GC-HINT]) so a field trace shows the
 * debounce working without growing the 320-char diag line. */
#define NOJME_EXPLICIT_GC_MIN_GAP_MS 250
static volatile long long s_explicit_gc_last_ok_ms = 0;
static volatile uint32_t s_explicit_gc_skipped = 0;
static long long s_explicit_gc_window_t0_ms = 0;
volatile uint32_t g_gc_explicit_ok_total = 0;     /* white-box / diagnostics */
volatile uint32_t g_gc_explicit_debounced_total = 0;

/* Returns 1 when the caller must run gc_collect(jvm), 0 when the request
 * was debounced (or disabled via NOJME_NO_SYSGC). */
static int gc_explicit_request(JVM* jvm) {
    (void)jvm;
    static int nosysgc_cache = -1;
    if (nosysgc_cache < 0) {
        const char* e = getenv("NOJME_NO_SYSGC");
        nosysgc_cache = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    if (nosysgc_cache) return 0;

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long long now = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

    long long last = s_explicit_gc_last_ok_ms;
    if (last && now - last < (long long)NOJME_EXPLICIT_GC_MIN_GAP_MS) {
        uint32_t n = s_explicit_gc_skipped + 1;
        s_explicit_gc_skipped = n;
        g_gc_explicit_debounced_total = g_gc_explicit_debounced_total + 1;
        if (s_explicit_gc_window_t0_ms == 0) s_explicit_gc_window_t0_ms = now;
        if (now - s_explicit_gc_window_t0_ms >= 5000) {
            /* Forced channel: visible even with the logging toggle off —
             * a storm of skipped gc() calls is exactly what the user's
             * "stutters after the race" report looks like in a trace. */
            extern void sw_trace_force(const char* fmt, ...)
                __attribute__((weak));
            if (&sw_trace_force && sw_trace_force) {
                sw_trace_force("[GC-HINT] %u explicit gc() requests debounced "
                               "in %lld ms (min gap %d ms) — phone treats "
                               "gc() as a hint",
                               n, now - s_explicit_gc_window_t0_ms,
                               (int)NOJME_EXPLICIT_GC_MIN_GAP_MS);
            }
            s_explicit_gc_skipped = 0;
            s_explicit_gc_window_t0_ms = now;
        }
        return 0;
    }
    s_explicit_gc_last_ok_ms = now;
    s_explicit_gc_skipped = 0;
    s_explicit_gc_window_t0_ms = now;
    g_gc_explicit_ok_total = g_gc_explicit_ok_total + 1;
    return 1;
}

static JavaValue native_system_gc(JVM* jvm, JavaThread* thread,
                                  JavaValue* args, int arg_count) {
    (void)thread; (void)args; (void)arg_count;
    /* v35.07: debounced hint (see gc_explicit_request above); the old
     * unconditional gc_collect ran every call — the race->finale storm
     * source. */
    /* v36.03 GC-DEFER-IN-NATIVE: never run an inline stop-the-world while
     * this thread is inside a native — skip entirely (the explicit gc() is
     * only a hint anyway); the next interpreter-context allocation or the
     * frontend cycle will collect. */
    {
        extern __thread int g_gc_in_native;
        if (gc_explicit_request(jvm) && g_gc_in_native == 0) {
            gc_collect(jvm);
        }
    }
    return NATIVE_RETURN_VOID();
}

static JavaValue native_system_getProperties(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)thread; (void)args; (void)arg_count; /* jvm используется таблицей v36.25 */
    
    NATIVE_DEBUG("System.getProperties() called");
    
    /* Create a Hashtable and populate with standard MIDP system properties */
    JavaClass* ht_class = jvm_load_class(jvm, "java/util/Hashtable");
    if (!ht_class) {
        NATIVE_DEBUG("System.getProperties: Hashtable class not found!");
        return NATIVE_RETURN_NULL();
    }
    
    JavaObject* ht = jvm_new_object(jvm, ht_class);
    if (!ht) {
        NATIVE_DEBUG("System.getProperties: failed to create Hashtable");
        return NATIVE_RETURN_NULL();
    }
    
    /* v36.25 [SYSPROPS]: таблица повторно использует g_sys_props (один
     * источник с getProperty) + динамические ключи (locale от языка Switch,
     * heap.size/heap.free по факту кучи). Прежняя локальная копия с
     * расходящимися значениями (locale=en-US, profiles=MIDP-2.1, captures=
     * true) удалена. */
    /* Find Hashtable.put() method */
    JavaMethod* put_method = jvm_resolve_method(jvm, ht_class, "put",
                              "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
    if (!put_method) {
        /* Can't populate the hashtable - return empty one */
        NATIVE_DEBUG("System.getProperties: Hashtable.put() not found, returning empty table");
        return NATIVE_RETURN_OBJECT(ht);
    }

    JavaThread* thr = jvm_current_thread(jvm);
    for (size_t i = 0; i < sizeof(g_sys_props) / sizeof(g_sys_props[0]); i++) {
        const char* val = sysprop_dynamic(jvm, g_sys_props[i].key);
        if (!val) val = g_sys_props[i].val;
        JavaString* key_str = jvm_new_string(jvm, g_sys_props[i].key);
        JavaString* val_str = jvm_new_string(jvm, val);
        if (key_str && val_str) {
            JavaValue put_args[3];
            put_args[0].ref = ht;
            put_args[1].ref = key_str;
            put_args[2].ref = val_str;
            JavaValue put_result;
            /* v36.04 NESTED-RETURN-CONTAINED: Hashtable.put returns a value —
             * without the nested wrapper each put leaked a deposit slot on
             * the caller frame (see execute_method_nested in execute.c). */
            extern int execute_method_nested(JVM* jvm, JavaThread* thread, JavaMethod* method,
                                             JavaValue* args, JavaValue* result);
            execute_method_nested(jvm, thr, put_method, put_args, &put_result);
        }
    }
    
    NATIVE_DEBUG("System.getProperties() → Hashtable with %d properties",
                 (int)(sizeof(g_sys_props) / sizeof(g_sys_props[0])));
    return NATIVE_RETURN_OBJECT(ht);
}

/*
 * java.io.PrintStream native methods
 */

/* PrintStream.println(String) - print to stderr for debugging
 * NOTE: args[0]=this (PrintStream), args[1]=String argument for instance methods */
static JavaValue native_printstream_println(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread;
    /* For instance methods: args[0]=this, args[1]=first param */
    if (arg_count >= 2 && args[1].ref) {
        const char* s = native_get_string_utf8(jvm, args + 1, 0);
        if (s && s[0] != '\0') {
            fprintf(stderr, "[SYSOUT] %s\n", s);
        } else {
            fprintf(stderr, "[SYSOUT] (empty string)\n");
        }
    } else if (arg_count >= 2 && !args[1].ref) {
        fprintf(stderr, "[SYSOUT] null\n");
    } else {
        fprintf(stderr, "[SYSOUT]\n");
    }
    return NATIVE_RETURN_VOID();
}

/* PrintStream.println() - no args */
static JavaValue native_printstream_println_empty(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    fprintf(stderr, "\n");
    return NATIVE_RETURN_VOID();
}

/* PrintStream.println(int) — args[0]=this, args[1]=int */
static JavaValue native_printstream_println_int(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread;
    int val = (arg_count >= 2) ? args[1].i : 0;
    fprintf(stderr, "[SYSOUT] %d\n", val);
    return NATIVE_RETURN_VOID();
}

/* PrintStream.print(String) - no newline, args[0]=this, args[1]=String */
static JavaValue native_printstream_print(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread;
    if (arg_count >= 2 && args[1].ref) {
        const char* s = native_get_string_utf8(jvm, args + 1, 0);
        if (s) {
            fprintf(stderr, "%s", s);
        }
    }
    return NATIVE_RETURN_VOID();
}

void init_java_io_printstream(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"java/io/PrintStream", "println", "()V", native_printstream_println_empty},
        {"java/io/PrintStream", "println", "(Ljava/lang/String;)V", native_printstream_println},
        {"java/io/PrintStream", "println", "(I)V", native_printstream_println_int},
        {"java/io/PrintStream", "println", "(Z)V", native_printstream_println_int},
        {"java/io/PrintStream", "print", "(Ljava/lang/String;)V", native_printstream_print},
        {"java/io/PrintStream", "print", "(I)V", native_printstream_println_int},
        /* v20 (P1): full print/println family */
        {"java/io/PrintStream", "println", "(J)V", native_printstream_println_long},
        {"java/io/PrintStream", "println", "(C)V", native_printstream_println_char},
        {"java/io/PrintStream", "println", "(F)V", native_printstream_println_float},
        {"java/io/PrintStream", "println", "(D)V", native_printstream_println_double},
        {"java/io/PrintStream", "println", "(Ljava/lang/Object;)V", native_printstream_println_object},
        {"java/io/PrintStream", "print", "(J)V", native_printstream_print_long},
        {"java/io/PrintStream", "print", "(C)V", native_printstream_print_char},
        {"java/io/PrintStream", "print", "(F)V", native_printstream_print_float},
        {"java/io/PrintStream", "print", "(D)V", native_printstream_print_double},
        {"java/io/PrintStream", "print", "(Ljava/lang/Object;)V", native_printstream_print_object},
        {"java/io/PrintStream", "flush", "()V", native_printstream_println_empty},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
}

void init_java_lang_system(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"java/lang/System", "arraycopy", "(Ljava/lang/Object;ILjava/lang/Object;II)V", native_system_arraycopy},
        {"java/lang/System", "currentTimeMillis", "()J", native_system_currentTimeMillis},
        {"java/lang/System", "gc", "()V", native_system_gc},
        {"java/lang/System", "exit", "(I)V", native_system_exit},
        {"java/lang/System", "identityHashCode", "(Ljava/lang/Object;)I", native_system_identityHashCode},
        {"java/lang/System", "getProperty", "(Ljava/lang/String;)Ljava/lang/String;", native_system_getProperty},
        {"java/lang/System", "getProperties", "()Ljava/util/Hashtable;", native_system_getProperties},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));

    /* Initialize System.in, System.out, System.err */
    JavaClass* system_class = jvm_load_class(jvm, "java/lang/System");
    if (system_class && system_class->static_fields) {
        /* Find PrintStream class */
        JavaClass* ps_class = jvm_load_class(jvm, "java/io/PrintStream");
        JavaClass* is_class = jvm_load_class(jvm, "java/io/InputStream");
        
        for (int i = 0; i < system_class->static_fields_count; i++) {
            if (system_class->static_fields[i].name) {
                /* System.out */
                if (strcmp(system_class->static_fields[i].name, "out") == 0 && ps_class) {
                    JavaObject* out_obj = jvm_new_object(jvm, ps_class);
                    if (out_obj) {
                        system_class->static_fields[i].value.ref = out_obj;
                        NATIVE_DEBUG("Initialized System.out");
                    }
                }
                /* System.err */
                else if (strcmp(system_class->static_fields[i].name, "err") == 0 && ps_class) {
                    JavaObject* err_obj = jvm_new_object(jvm, ps_class);
                    if (err_obj) {
                        system_class->static_fields[i].value.ref = err_obj;
                        NATIVE_DEBUG("Initialized System.err");
                    }
                }
                /* System.in */
                else if (strcmp(system_class->static_fields[i].name, "in") == 0 && is_class) {
                    JavaObject* in_obj = jvm_new_object(jvm, is_class);
                    if (in_obj) {
                        system_class->static_fields[i].value.ref = in_obj;
                        NATIVE_DEBUG("Initialized System.in");
                    }
                }
            }
        }
    }
}

/*
 * [BATFIX] v36.41: nojme.device.Battery — класс-API заряда для мидлетов.
 *
 * ДВА способа узнать заряд из J2ME (запрос пользователя: «реализуй
 * возможность чтения заряда внутри j2me через один или несколько АПИ»):
 *
 *   1. System.getProperty(...) — три ключа-процента (Motorola
 *      "batterylevel", Nokia "com.nokia.mid.batterylevel", наш
 *      "nojme.batterylevel") + "nojme.battery.charging" (см.
 *      sysprop_dynamic). Этим путём идут существующие игры, опрашивавшие
 *      системные свойства на телефонах.
 *
 *   2. Собственный класс (пишется в jar напрямую):
 *         package nojme.device;
 *         public class Battery {
 *             public static native int getLevel();   // 0..100, -1 = неизвестно
 *             public static native boolean isCharging();
 *         }
 *      Или через Class.forName("nojme.device.Battery") + invokestatic —
 *      стаб-класс создан в stubs.c, нативы резолвятся по имени ниже.
 *
 * Оба пути читают один источник utils/battery.c (PSM на Switch).
 */
static JavaValue native_nojme_batt_getLevel(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    return NATIVE_RETURN_INT(nojme_battery_percent());
}

static JavaValue native_nojme_batt_isCharging(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    int chg = nojme_battery_charging();
    return NATIVE_RETURN_INT(chg > 0 ? 1 : 0);
}

void init_nojme_battery(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"nojme/device/Battery", "getLevel",   "()I", native_nojme_batt_getLevel},
        {"nojme/device/Battery", "isCharging", "()Z", native_nojme_batt_isCharging},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
}

/*
 * java.lang.Runtime native methods
 */

/* Singleton Runtime instance */
static JavaObject* g_runtime_instance = NULL;

static JavaValue native_runtime_getRuntime(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)thread; (void)args; (void)arg_count;
    
    /* Return singleton Runtime instance */
    if (!g_runtime_instance) {
        JavaClass* runtime_class = jvm_load_class(jvm, "java/lang/Runtime");
        if (runtime_class) {
            g_runtime_instance = jvm_new_object(jvm, runtime_class);
            /* Register as GC root to prevent collection */
            gc_add_root(jvm, (void**)&g_runtime_instance);
        }
    }
    
    return NATIVE_RETURN_OBJECT(g_runtime_instance);
}

static JavaValue native_runtime_freeMemory(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)thread; (void)args; (void)arg_count;
    HeapStats stats = heap_get_stats(jvm);
    return NATIVE_RETURN_LONG((jlong)stats.free_size);
}

static JavaValue native_runtime_totalMemory(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)thread; (void)args; (void)arg_count;
    HeapStats stats = heap_get_stats(jvm);
    return NATIVE_RETURN_LONG((jlong)stats.total_size);
}

static JavaValue native_runtime_gc(JVM* jvm, JavaThread* thread,
                                   JavaValue* args, int arg_count) {
    (void)thread; (void)args; (void)arg_count;
    /* v41 PERF-DIAG: NOJME_NO_SYSGC=1 — turn game-initiated System.gc()
     * into a no-op (stutter A/B experiments; the heap-full collector in
     * heap_alloc still runs, so this only removes explicit collections).
     * v35.07: the env check moved into gc_explicit_request() so it covers
     * System.gc() too (it used to gate Runtime.gc only), and the explicit
     * request is debounced to 250 ms (see the block above native_system_gc
     * — the race->finale gc=292/5s storm source). */
    if (gc_explicit_request(jvm)) {
        /* v36.03 GC-DEFER-IN-NATIVE: see native_system_gc. */
        extern __thread int g_gc_in_native;
        if (g_gc_in_native == 0) {
            jvm_gc(jvm);
        }
    }
    return NATIVE_RETURN_VOID();
}

void init_java_lang_runtime(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"java/lang/Runtime", "getRuntime", "()Ljava/lang/Runtime;", native_runtime_getRuntime},
        {"java/lang/Runtime", "freeMemory", "()J", native_runtime_freeMemory},
        {"java/lang/Runtime", "totalMemory", "()J", native_runtime_totalMemory},
        {"java/lang/Runtime", "gc", "()V", native_runtime_gc},
        {"java/lang/Runtime", "exit", "(I)V", native_runtime_exit},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
}

/*
 * java.lang.Math native methods
 */

static JavaValue native_math_sqrt(JVM* jvm, JavaThread* thread,
                                  JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jdouble value = args[0].d;
    return NATIVE_RETURN_DOUBLE(sqrt(value));
}

static JavaValue native_math_sin(JVM* jvm, JavaThread* thread,
                                 JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jdouble value = args[0].d;
    return NATIVE_RETURN_DOUBLE(sin(value));
}

static JavaValue native_math_cos(JVM* jvm, JavaThread* thread,
                                 JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jdouble value = args[0].d;
    return NATIVE_RETURN_DOUBLE(cos(value));
}

static JavaValue native_math_tan(JVM* jvm, JavaThread* thread,
                                 JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jdouble value = args[0].d;
    return NATIVE_RETURN_DOUBLE(tan(value));
}

static JavaValue native_math_abs_int(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jint value = args[0].i;
    return NATIVE_RETURN_INT(value < 0 ? -value : value);
}

static JavaValue native_math_abs_long(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jlong value = args[0].j;
    return NATIVE_RETURN_LONG(value < 0 ? -value : value);
}

static JavaValue native_math_abs_float(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jfloat value = args[0].f;
    /* v18 FIX: fabsf() (not a signed comparison!) - `value < 0` is false
     * for -0.0f, so the old ternary returned -0.0f unchanged, while Java's
     * Math.abs(-0.0f) must be +0.0f (bit pattern 0x00000000). */
    return NATIVE_RETURN_FLOAT(fabsf(value));
}

static JavaValue native_math_abs_double(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jdouble value = args[0].d;
    /* v18 FIX: fabs() handles -0.0 -> +0.0 correctly; see abs(float). */
    return NATIVE_RETURN_DOUBLE(fabs(value));
}

static JavaValue native_math_max_int(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jint a = args[0].i;
    jint b = args[1].i;
    return NATIVE_RETURN_INT(a > b ? a : b);
}

static JavaValue native_math_min_int(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jint a = args[0].i;
    jint b = args[1].i;
    return NATIVE_RETURN_INT(a < b ? a : b);
}

static JavaValue native_math_max_long(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jlong a = args[0].j;
    jlong b = args[1].j;
    return NATIVE_RETURN_LONG(a > b ? a : b);
}

static JavaValue native_math_min_long(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jlong a = args[0].j;
    jlong b = args[1].j;
    return NATIVE_RETURN_LONG(a < b ? a : b);
}

static JavaValue native_math_floor(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jdouble value = args[0].d;
    return NATIVE_RETURN_DOUBLE(floor(value));
}

static JavaValue native_math_ceil(JVM* jvm, JavaThread* thread,
                                   JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jdouble value = args[0].d;
    return NATIVE_RETURN_DOUBLE(ceil(value));
}

/* v18 FIX: Math.toRadians/toDegrees were not registered at all and
 * native_call() silently returned 0 for them (toRadians(90) == 0). */
static JavaValue native_math_toRadians(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jdouble angdeg = args[0].d;
    return NATIVE_RETURN_DOUBLE(angdeg / 180.0 * 3.14159265358979323846);
}

static JavaValue native_math_toDegrees(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jdouble angrad = args[0].d;
    return NATIVE_RETURN_DOUBLE(angrad * 180.0 / 3.14159265358979323846);
}

/* ==== v20 FIX (P0-3): previously-missing Math natives ====
 * The static-invoke fallback silently returned 0.0 for every one of these,
 * wrecking game physics/difficulty curves (Math.pow(2,10) == 0). */

static JavaValue native_math_pow(JVM* jvm, JavaThread* thread,
                                 JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    return NATIVE_RETURN_DOUBLE(pow(args[0].d, args[1].d));
}

static JavaValue native_math_atan2(JVM* jvm, JavaThread* thread,
                                   JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    return NATIVE_RETURN_DOUBLE(atan2(args[0].d, args[1].d));
}

static JavaValue native_math_exp(JVM* jvm, JavaThread* thread,
                                 JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    return NATIVE_RETURN_DOUBLE(exp(args[0].d));
}

static JavaValue native_math_log(JVM* jvm, JavaThread* thread,
                                 JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    /* C log(): log(0)=-inf, log(neg)=NaN — matches java.lang.Math. */
    return NATIVE_RETURN_DOUBLE(log(args[0].d));
}

static JavaValue native_math_log10(JVM* jvm, JavaThread* thread,
                                   JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    return NATIVE_RETURN_DOUBLE(log10(args[0].d));
}

static JavaValue native_math_asin(JVM* jvm, JavaThread* thread,
                                  JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    return NATIVE_RETURN_DOUBLE(asin(args[0].d));
}

static JavaValue native_math_acos(JVM* jvm, JavaThread* thread,
                                  JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    return NATIVE_RETURN_DOUBLE(acos(args[0].d));
}

static JavaValue native_math_atan(JVM* jvm, JavaThread* thread,
                                  JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    return NATIVE_RETURN_DOUBLE(atan(args[0].d));
}

static JavaValue native_math_cbrt(JVM* jvm, JavaThread* thread,
                                  JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    return NATIVE_RETURN_DOUBLE(cbrt(args[0].d));
}

static JavaValue native_math_hypot(JVM* jvm, JavaThread* thread,
                                   JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    return NATIVE_RETURN_DOUBLE(hypot(args[0].d, args[1].d));
}

static JavaValue native_math_round_double(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jdouble x = args[0].d;
    /* java.lang.Math.round(double): (long)floor(x + 0.5), NaN -> 0,
     * saturate at LONG_MIN/LONG_MAX (floor(x+0.5) itself can overflow). */
    if (x != x) return NATIVE_RETURN_LONG(0);
    jdouble r = floor(x + 0.5);
    if (r >= 9223372036854775807.0) return NATIVE_RETURN_LONG(INT64_MAX);
    if (r <= -9223372036854775808.0) return NATIVE_RETURN_LONG(INT64_MIN);
    return NATIVE_RETURN_LONG((jlong)r);
}

static JavaValue native_math_round_float(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jfloat x = args[0].f;
    if (x != x) return NATIVE_RETURN_INT(0);
    jfloat r = (jfloat)floorf(x + 0.5f);
    if (r >= 2147483647.0f) return NATIVE_RETURN_INT(INT32_MAX);
    if (r <= -2147483648.0f) return NATIVE_RETURN_INT(INT32_MIN);
    return NATIVE_RETURN_INT((jint)r);
}

static JavaValue native_math_random(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count; (void)args;
    /* java.lang.Math.random(): double in [0.0, 1.0). Backed by a shared
     * 48-bit LCG (java.util.Random algorithm, seeds from the clock on
     * first use). */
    static uint64_t seed = 0;
    static int seeded = 0;
    if (!seeded) {
        seeded = 1;
        seed = (uint64_t)time(NULL) * 25214903917ULL + 11ULL;
        if (seed == 0) seed = 868252287297153ULL; /* never zero */
    }
    seed = (seed * 25214903917ULL + 11ULL) & ((1ULL << 48) - 1ULL);
    /* next(26) << 27 | next(27), exactly like Random.nextDouble */
    uint64_t n26 = (seed >> 22) & ((1ULL << 26) - 1ULL);
    seed = (seed * 25214903917ULL + 11ULL) & ((1ULL << 48) - 1ULL);
    uint64_t n27 = (seed >> 21) & ((1ULL << 27) - 1ULL);
    return NATIVE_RETURN_DOUBLE((jdouble)((n26 << 27) | n27) / (jdouble)(1ULL << 53));
}

static JavaValue native_math_signum_float(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jfloat x = args[0].f;
    if (x != x || x == 0.0f) return NATIVE_RETURN_FLOAT(x); /* NaN/±0.0 pass through */
    return NATIVE_RETURN_FLOAT(x > 0.0f ? 1.0f : -1.0f);
}

static JavaValue native_math_signum_double(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jdouble x = args[0].d;
    if (x != x || x == 0.0) return NATIVE_RETURN_DOUBLE(x);
    return NATIVE_RETURN_DOUBLE(x > 0.0 ? 1.0 : -1.0);
}

/* Java min/max for floats/doubles: NaN propagates; -0.0/+0.0 handled
 * per spec (max -> +0.0, min -> -0.0). */
static JavaValue native_math_max_float(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jfloat a = args[0].f, b = args[1].f;
    if (a != a) return NATIVE_RETURN_FLOAT(a);
    if (b != b) return NATIVE_RETURN_FLOAT(b);
    if (a == 0.0f && b == 0.0f) {
        /* one of them is -0.0: max returns +0.0 */
        return NATIVE_RETURN_FLOAT(signbit(a) ? b : a);
    }
    return NATIVE_RETURN_FLOAT(a >= b ? a : b);
}

static JavaValue native_math_min_float(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jfloat a = args[0].f, b = args[1].f;
    if (a != a) return NATIVE_RETURN_FLOAT(a);
    if (b != b) return NATIVE_RETURN_FLOAT(b);
    if (a == 0.0f && b == 0.0f) {
        return NATIVE_RETURN_FLOAT(signbit(a) ? a : b); /* min returns -0.0 */
    }
    return NATIVE_RETURN_FLOAT(a <= b ? a : b);
}

static JavaValue native_math_max_double(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jdouble a = args[0].d, b = args[1].d;
    if (a != a) return NATIVE_RETURN_DOUBLE(a);
    if (b != b) return NATIVE_RETURN_DOUBLE(b);
    if (a == 0.0 && b == 0.0) {
        return NATIVE_RETURN_DOUBLE(signbit(a) ? b : a);
    }
    return NATIVE_RETURN_DOUBLE(a >= b ? a : b);
}

static JavaValue native_math_min_double(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jdouble a = args[0].d, b = args[1].d;
    if (a != a) return NATIVE_RETURN_DOUBLE(a);
    if (b != b) return NATIVE_RETURN_DOUBLE(b);
    if (a == 0.0 && b == 0.0) {
        return NATIVE_RETURN_DOUBLE(signbit(a) ? a : b);
    }
    return NATIVE_RETURN_DOUBLE(a <= b ? a : b);
}

void init_java_lang_math(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"java/lang/Math", "sqrt", "(D)D", native_math_sqrt},
        {"java/lang/Math", "sin", "(D)D", native_math_sin},
        {"java/lang/Math", "cos", "(D)D", native_math_cos},
        {"java/lang/Math", "tan", "(D)D", native_math_tan},
        {"java/lang/Math", "abs", "(I)I", native_math_abs_int},
        {"java/lang/Math", "abs", "(J)J", native_math_abs_long},
        {"java/lang/Math", "abs", "(F)F", native_math_abs_float},
        {"java/lang/Math", "abs", "(D)D", native_math_abs_double},
        {"java/lang/Math", "max", "(II)I", native_math_max_int},
        {"java/lang/Math", "min", "(II)I", native_math_min_int},
        {"java/lang/Math", "max", "(JJ)J", native_math_max_long},
        {"java/lang/Math", "min", "(JJ)J", native_math_min_long},
        {"java/lang/Math", "floor", "(D)D", native_math_floor},
        {"java/lang/Math", "ceil", "(D)D", native_math_ceil},
        {"java/lang/Math", "toRadians", "(D)D", native_math_toRadians},
        {"java/lang/Math", "toDegrees", "(D)D", native_math_toDegrees},
        /* v20 FIX (P0-3): the 16 entries above were ALL that existed;
         * everything below silently returned 0.0 through the static-invoke
         * fallback. */
        {"java/lang/Math", "pow", "(DD)D", native_math_pow},
        {"java/lang/Math", "atan2", "(DD)D", native_math_atan2},
        {"java/lang/Math", "exp", "(D)D", native_math_exp},
        {"java/lang/Math", "log", "(D)D", native_math_log},
        {"java/lang/Math", "log10", "(D)D", native_math_log10},
        {"java/lang/Math", "asin", "(D)D", native_math_asin},
        {"java/lang/Math", "acos", "(D)D", native_math_acos},
        {"java/lang/Math", "atan", "(D)D", native_math_atan},
        {"java/lang/Math", "cbrt", "(D)D", native_math_cbrt},
        {"java/lang/Math", "hypot", "(DD)D", native_math_hypot},
        {"java/lang/Math", "round", "(D)J", native_math_round_double},
        {"java/lang/Math", "round", "(F)I", native_math_round_float},
        {"java/lang/Math", "random", "()D", native_math_random},
        {"java/lang/Math", "signum", "(F)F", native_math_signum_float},
        {"java/lang/Math", "signum", "(D)D", native_math_signum_double},
        {"java/lang/Math", "max", "(FF)F", native_math_max_float},
        {"java/lang/Math", "min", "(FF)F", native_math_min_float},
        {"java/lang/Math", "max", "(DD)D", native_math_max_double},
        {"java/lang/Math", "min", "(DD)D", native_math_min_double},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
}

/*
 * java.lang.Thread native methods
 */

static JavaValue native_thread_currentThread(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)args; (void)arg_count;
    /* v34.43 FIX (Nescube black screen / NPE on game start): the main/AMS
     * thread is created in jvm_init WITHOUT a java.lang.Thread object, so
     * Thread.currentThread() returned null whenever MIDlet code ran on the
     * frontend thread (Display pumps: deferred showNotify/hideNotify,
     * callSerially, repaints). Nescube: u.showNotify() -> d(0) ->
     * Thread.currentThread().setPriority(10) -> NullPointerException ->
     * "Error message" alert, NES emulation thread never started. Real
     * J2ME handsets always attach a Thread object to the AMS thread.
     * Lazily allocate a shell Thread here (fields only; no <init>
     * dispatch — we are already inside the interpreter's native path). */
    if (thread && !thread->thread_object) {
        JavaClass* thread_class = jvm_load_class(jvm, "java/lang/Thread");
        if (thread_class) {
            JavaObject* shell = jvm_new_object(jvm, thread_class);
            if (shell) {
                /* Pin across the name-string allocation: heap_alloc can
                 * trigger gc_collect() and the shell is not yet reachable
                 * from any root (v24 pinned-root table semantics). */
                jvm_add_root(jvm, shell);
                JavaValue field_val;
                JavaString* name_str =
                    jvm_new_string(jvm, thread->name ? thread->name : "main");
                field_val.ref = (void*)name_str;
                native_set_field_value(shell, "name", field_val);
                field_val.ref = NULL;
                native_set_field_value(shell, "target", field_val);
                field_val.i = (thread->priority >= 1 && thread->priority <= 10)
                              ? thread->priority : THREAD_PRIORITY_NORM;
                native_set_field_value(shell, "priority", field_val);
                /* Publish: JavaThread structs in jvm->threads[] are GC roots
                 * and their thread_object field is marked (heap.c
                 * gc_collect) — from here on the shell is reachable via the
                 * thread registry, so drop the explicit pin. */
                thread->thread_object = shell;
                jvm_remove_root(jvm, shell);
            }
        }
    }
    return NATIVE_RETURN_OBJECT(thread ? thread->thread_object : NULL);
}

static JavaValue native_thread_sleep(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    (void)arg_count;
    jlong millis = args[0].j;
    
    static int sleep_log_count = 0;
    sleep_log_count++;
    /* v36.37 DIAG (Alien Shooter 3D 4-fps report): NOJME_SCHED_TRACE=1 —
     * печатает статистику Thread.sleep/yield раз в 5 с: число вызовов,
     * суммарные запрошенные мс и РЕАЛЬНО прошедшее время. Показывает,
     * уходит ли темп игры в сны планировщика. */
    {
        static int s_trace_on = -1;
        if (s_trace_on < 0)
            s_trace_on = getenv("NOJME_SCHED_TRACE") ? 1 : 0;
        if (s_trace_on) {
            static __thread int s_n = 0;
            static __thread jlong s_req_ms = 0;
            static __thread uint64_t s_t0 = 0;
            extern uint64_t jvm_virt_mono_ms(void);
            uint64_t now = jvm_virt_mono_ms();
            if (!s_t0) s_t0 = now;
            s_n++;
            if (millis > 0 && millis < 10000) s_req_ms += millis;
            if (now - s_t0 >= 5000) {
                fprintf(stderr,
                        "[SCHED] sleep: calls=%d req_ms=%lld wall=%llu ms\n",
                        s_n, (long long)s_req_ms,
                        (unsigned long long)(now - s_t0));
                s_n = 0; s_req_ms = 0; s_t0 = now;
            }
        }
    }
    if (sleep_log_count <= 20) {
        if (g_j2me_runtime_debug) fprintf(stderr, "[SLEEP] Thread.sleep(%ld) called (call #%d)\n", (long)millis, sleep_log_count);
        fflush(stderr);
    }
    
    /* Check for interruption before sleep */
    if (thread && thread->interrupted) {
        thread->interrupted = false;
        native_throw_interrupted(jvm, thread, "sleep interrupted");
        return NATIVE_RETURN_VOID();
    }
    
    /* v18 (audit M-17): Thread.sleep(negative) must throw
     * IllegalArgumentException per java.lang spec; it used to return instantly
     * (splash/pause timers ran fast). */
    if (millis < 0) {
        native_throw_iae(jvm, thread, "timeout value is negative");
        return NATIVE_RETURN_VOID();
    }
    
    /* For short sleeps, just do the sleep */
    if (millis <= 0) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Cap sleep time */
    if (millis > 10000) millis = 10000;
    
    /* ИСПРАВЛЕНО: Для pthread-based threading НЕ обрабатываем таймеры и repaints
     * внутри sleep, потому что это вызывает рекурсивное выполнение байт-кода.
     * Таймеры и repaints должны обрабатываться только в главном цикле.
     * 
     * Проблема была: jvm_process_timers() вызывал run() TimerTask'а,
     * который вызывал repaint()/serviceRepaints(), что приводило к
     * рекурсивному выполнению paint() и другим проблемам.
     * 
     * Теперь мы только обрабатываем SDL события для отзывчивости UI.
     */
    extern void sdl_process_events_minimal(void);
    
    jlong remaining = millis;
    (void)remaining;  /* v34.59: цикл ниже считает по абсолютным часам */
    /* v34.59 PERF: цикл сна переписан на АБСОЛЮТНОЕ время (CLOCK_MONOTONIC):
     * пробуждение по safepoint-broadcast'у больше НЕ «съедает» остаток сна
     * (раньше remaining -= chunk терял до 50 мс при раннем выходе из куска).
     * Кусок остаётся 50 мс (10 мс, если GC уже ждёт) — но спим мы теперь на
     * КОНДИЦИОНАЛЕ jvm_sleep_chunk_ms(): broadcast из gc_collect() будит
     * мгновенно, и поток запаркивается в пределах микросекунд-миллисекунд,
     * а не к границе куска.
     * v34.81 FRONTEND PAUSE: счёт сна — по ВИРТУАЛЬНЫМ монотонным часам
     * (минус время паузы фронтенда): сон не сгорает, пока открыто меню
     * RetroArch; сразу после снятия паузы цикл досыпает остаток. */
    {
        extern uint64_t jvm_virt_mono_ms(void);
        extern volatile int g_frontend_pause_active;
        extern void jvm_frontend_pause_park(void);
        uint64_t t0 = jvm_virt_mono_ms();
        jlong slept_ms = 0;
        while (1) {
            /* v34.81 FRONTEND PAUSE: парковка до снятия паузы фронтенда.
             * Виртуальные часы (t0 выше) не текут, пока мы тут стоим —
             * остаток сна сохраняется точь-в-точь. */
            if (g_frontend_pause_active) {
                jvm_frontend_pause_park();
            }
            /* v36.30 [EXIT-FENCE]: спящие/ждущие раннеры тоже уважают окно
             * фрейм-потока (destroyApp и т.п.) — иначе проснувшись посреди
             * окна они исполняют байткод наперегонки с фрейм-потоком. */
            {
                extern volatile int g_vm_fiber_fence;
                extern void jvm_frame_fence_wait_public(void);
                if (g_vm_fiber_fence) {
                    jvm_frame_fence_wait_public();
                }
            }
            /* v34.59 FIX (REGRESSION): кусок ограничен ОСТАТКОМ сна —
             * прежняя версия всегда спала полный кусок 50 мс и проверяла
             * дедлайн только на его границе: Thread.sleep(10) реально
             * спал 50 мс (5x пересып) — фоновые циклы fmx замедлились
             * на 26% (инструкции за 90 с: 227M -> 168M при тех же кадрах).
             * Теперь: chunk = min(50, осталось), дедлайн — по абсолютным
             * часам, ранний выход по safepoint-broadcast'у сохранён. */
            jlong left = millis - slept_ms;
            jlong chunk = (left > 50) ? 50 : left;
            {
                extern volatile int g_gc_safepoint_request;
                if (g_gc_safepoint_request && chunk > 10) chunk = 10;
            }

#ifdef _WIN32
            /* Windows: Sleep() не прерывается — режем на 5-мс подсрезы,
             * проверяя флаг (худший случай ожидания GC: 5 мс). */
            {
                DWORD leftms = (DWORD)chunk;
                while (leftms > 0) {
                    DWORD sub = (leftms > 5) ? 5 : leftms;
                    Sleep(sub);
                    leftms -= sub;
                    {
                        extern volatile int g_gc_safepoint_request;
                        if (g_gc_safepoint_request) break;
                    }
                }
            }
#else
            jvm_sleep_chunk_ms((long)chunk);
#endif

            /* Process SDL events to keep the emulator responsive */
            sdl_process_events_minimal();

            /* v34.11 GC safepoint poll (v34.59: пробуждение мгновенное,
             * см. jvm_sleep_chunk_ms): поток в stop-the-world запаркуется
             * здесь и продолжит досыпать ПОСЛЕ релиза GC. */
            {
                extern volatile int g_gc_safepoint_request;
                if (g_gc_safepoint_request) {
                    void jvm_gc_safepoint_park(void);
                    jvm_gc_safepoint_park();
                }
            }

            /* v34.84 FIX (slow emulator close, 3-5 s): abort the remaining
             * sleep as soon as the VM is shutting down (jvm_destroy sets
             * jvm->running=false BEFORE waiting up to 3 s for pthread
             * runners). The old loop only woke on GC/interrupt/virtual-time
             * deadlines — a game thread parked in Thread.sleep (loading
             * animations: 500 ms; cap 10 s) slept the FULL duration first,
             * so retro_deinit sat in native_threads_wait_idle(3000) and
             * then abandoned teardown with a memory leak. Aborting the
             * sleep here is semantically invisible: the interpreter's next
             * opcode check exits the thread anyway. */
            if (jvm && !jvm->running) {
                return NATIVE_RETURN_VOID();
            }

            /* Check for interruption */
            if (thread && thread->interrupted) {
                thread->interrupted = false;
                native_throw_interrupted(jvm, thread, "sleep interrupted");
                return NATIVE_RETURN_VOID();
            }

            /* Точный остаток по ВИРТУАЛЬНЫМ монотонным часам (v34.81:
             * минус время паузы фронтенда — сон не сгорает за меню) */
            {
                slept_ms = (jlong)(jvm_virt_mono_ms() - t0);
                if (slept_ms >= millis) break;
            }
        }
    }
    
    if (sleep_log_count <= 20) {
        if (g_j2me_runtime_debug) fprintf(stderr, "[SLEEP] Thread.sleep(%ld) completed (call #%d)\n", (long)millis, sleep_log_count);
        fflush(stderr);
    }
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_thread_yield(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    (void)thread; (void)args; (void)arg_count;
    /* v34.45: explicit java-level yield — adaptive rest for Gameloft-style
     * busy-wait frame limiters (see thread_yield_explicit in threads.c). */
    thread_yield_explicit(jvm);
    return NATIVE_RETURN_VOID();
}

/* External functions for method resolution and execution */
extern JavaMethod* jvm_resolve_method(JVM* jvm, JavaClass* clazz, const char* name, const char* descriptor);
extern int execute_method(JVM* jvm, JavaThread* thread, JavaMethod* method, 
                          JavaValue* args, JavaValue* result);

/* Thread.start() - platform-specific threading implementation */
#ifndef _WIN32
#include <pthread.h>
#include <unistd.h>
#endif

/* Forward declarations */
static JavaValue native_thread_start(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count);

/* Platform-specific thread storage */
#define MAX_PTHREADS 256

/* Native stack size for threads that execute Java bytecode.
 * One Java frame costs 3+ native frames in the interpreter (interpret ->
 * op_invokeXXX -> execute_method), so the 500-frame Java recursion guard
 * requires several MB of native stack. Explicitly size the stack so the
 * StackOverflowError guard fires BEFORE the native stack is exhausted,
 * regardless of the host process' default (PE header on Windows,
 * RLIMIT_STACK on Linux). Reservation only - pages commit on demand. */
#define JAVA_NATIVE_STACK_SIZE (32u * 1024u * 1024u)  /* 32 MB */

#ifdef _WIN32
/* Windows-specific threading */
static HANDLE g_win_threads[MAX_PTHREADS];
static HANDLE g_win_thread_events[MAX_PTHREADS];  /* For join() */
static CRITICAL_SECTION g_win_thread_cs;
static volatile bool g_pthread_running[MAX_PTHREADS];
/* v34.26: real thread IDs for the GC census. g_win_threads[] holds a MIX of
 * real handles (from CreateThread in native_thread_start) and pseudo-handles
 * (win_thread_runner overwrites its slot with GetCurrentThread()), which are
 * only meaningful on the owning thread — useless for cross-thread identity
 * checks like jvm_current_os_thread_is_vm_runner(). Store the DWORD id too. */
static volatile DWORD g_win_thread_ids[MAX_PTHREADS];
static bool g_win_thread_initialized = false;

static void win_thread_init(void) {
    if (!g_win_thread_initialized) {
        InitializeCriticalSection(&g_win_thread_cs);
        for (int i = 0; i < MAX_PTHREADS; i++) {
            g_win_threads[i] = NULL;
            g_win_thread_events[i] = NULL;
            g_pthread_running[i] = false;
        }
        g_win_thread_initialized = true;
    }
}

/* v34.10: Windows twin of the POSIX g_live-registry counter (below).
 * gc_collect() (heap.c) calls this for the GC safepoint census — on
 * Windows VM threads are CreateThread-based, so count the same
 * g_pthread_running[] flags the win_thread_runner maintains. */
int jvm_live_vm_thread_count(void) {
    int n = 0;
    for (int i = 0; i < MAX_PTHREADS; i++) {
        if (g_pthread_running[i]) n++;
    }
    return n;
}

/* v36.16: forward declaration — native_threads_kill_all (v36.13) uses the
 * registry defined further below (v34.12 block); WIN32 builds failed to
 * compile without it ("g_win_live_jt undeclared"), POSIX builds unaffected. */
static JavaThread* g_win_live_jt[MAX_PTHREADS];

/* v36.13 Windows twin of the POSIX native_threads_kill_all (below): same
 * registry walk, same liveness clears; wakeup via the join events. */
void native_threads_kill_all(void) {
    int killed = 0;
    for (int i = 0; i < MAX_PTHREADS; i++) {
        if (!g_pthread_running[i]) continue;
        killed++;
        if (g_win_live_jt[i]) {
            g_win_live_jt[i]->is_alive = false;
            g_win_live_jt[i]->interrupted = false;
        }
        if (g_win_thread_events[i]) SetEvent(g_win_thread_events[i]);
    }
    LOG_SAFE("[THREAD-KILL] %d runner(s) signaled\n", killed);
}

/* v34.26: Windows twin of jvm_current_os_thread_is_vm_runner() (POSIX twin
 * lives in the #else branch below). Used by gc_collect()'s safepoint census
 * when GC is triggered from the frontend thread instead of a VM runner. */
int jvm_current_os_thread_is_vm_runner(void) {
    DWORD self = GetCurrentThreadId();
    for (int i = 0; i < MAX_PTHREADS; i++) {
        if (g_pthread_running[i] && g_win_thread_ids[i] != 0 &&
            g_win_thread_ids[i] == self) {
            return 1;
        }
    }
    return 0;
}

/* v34.12: JavaThread* registry parallel to g_pthread_running[] so the
 * diagnostic dump can report last-native info on Windows too (POSIX twin:
 * g_live_threads[] in the #else branch below). Written/cleared only by
 * win_thread_runner, read racily by jvm_dump_all_threads (diagnostic only). */
static JavaThread* g_win_live_jt[MAX_PTHREADS];

/* v35.12 MULTI-SESSION FIX: the registries above are PROCESS-GLOBAL and
 * jvm_destroy frees every JavaThread they point at. Without a reset the
 * next session's td dump / jvm_java_thread_alive_by_id walk DANGLING
 * pointers (device log showed the garbage id "td t6684680: done" — freed
 * memory read as a thread id) and the M3G orphaned-lock recovery compares
 * tid against freed structs. Clears the diagnostic registries; safe ONLY
 * when no VM runner is alive (callers: init_java_lang_thread at session
 * start and jvm_destroy after the wait-idle barrier succeeded).
 * Windows twin — POSIX twin lives in the #else branch below. */
void native_threads_registry_reset(void) {
    for (int i = 0; i < MAX_PTHREADS; i++) {
        g_win_live_jt[i] = NULL;
        if (g_win_threads[i]) { CloseHandle(g_win_threads[i]); g_win_threads[i] = NULL; }
        g_win_thread_events[i] = NULL;
        g_pthread_running[i] = false;
    }
    LOG_SAFE("[THREAD] registry reset (session boundary)\n");
}

/* v34.12: Windows twins of the POSIX diagnostic helpers in the #else branch
 * below. gc_collect() (heap.c) and the five native dispatch sites in
 * opcodes.c reference these on BOTH platforms — without them the libretro
 * DLL fails to link (undefined reference to jvm_dump_all_threads /
 * jvm_note_native_call). */
void jvm_note_native_call(JavaThread* thread, const char* cls, const char* mth) {
    if (!thread) return;
    thread->last_native_ms = (jlong)GetTickCount64();
    if (cls && mth) {
        snprintf(thread->last_native, sizeof(thread->last_native), "%.60s.%.30s", cls, mth);
    } else {
        snprintf(thread->last_native, sizeof(thread->last_native), "?");
    }
}

void jvm_dump_all_threads(void) {
    /* SAFE version (matches the POSIX twin): liveness + last-native only;
     * never walk frames of running threads. */
    LOG_SAFE("[THREADDUMP] === begin (win) ===\n");
    for (int i = 0; i < MAX_PTHREADS; i++) {
        JavaThread* t = g_win_live_jt[i];
        if (!t) continue;
        if (!g_pthread_running[i]) {
            LOG_SAFE("[THREADDUMP] tid=%d finished\n", t->id);
        } else {
            /* v34.11: include the last native started on this thread and how
             * long ago — a large age means the thread is stuck inside it. */
            unsigned long long now = (unsigned long long)GetTickCount64();
            jlong age = t->last_native_ms
                ? (jlong)(now - (unsigned long long)t->last_native_ms) : -1;
            LOG_SAFE("[THREADDUMP] tid=%d running last_native=%s age=%lldms\n",
                     t->id,
                     t->last_native[0] ? t->last_native : "(none)",
                     (long long)age);
        }
    }
    LOG_SAFE("[THREADDUMP] === end ===\n");
    fflush(stderr);
}

/* v34.96 M3G-LOCK WATCHDOG helper (Windows twin of the POSIX version —
 * see the full comment there). */
int jvm_java_thread_alive_by_id(int tid) {
    if (tid <= 0) return 0;
    if (tid == 1) return 1;   /* main/frontend */
    for (int i = 0; i < MAX_PTHREADS; i++) {
        JavaThread* t = g_win_live_jt[i];
        if (t && t->id == tid) {
            return (g_pthread_running[i] && t->is_alive) ? 1 : 0;
        }
    }
    return 0;
}

/* v34.95: Windows twin of the POSIX jvm_threads_dump_snprint (see the full
 * comment there). Iterates the g_win_live_jt registry. */
void jvm_threads_dump_snprint(JVM* jvm, char* buf, size_t cap, int deep) {
    size_t off = 0;
    if (!buf || cap < 64) return;
    buf[0] = '\0';
    unsigned long long now = (unsigned long long)GetTickCount64();
    jlong now_ms = (jlong)now;

    if (jvm && jvm->main_thread) {
        JavaThread* mt = jvm->main_thread;
        jlong age = mt->last_native_ms ? now_ms - mt->last_native_ms : -1;
        /* v34.96: nat widened %.32s -> %.48s (see POSIX twin comment) */
        off += (size_t)snprintf(buf + off, cap - off,
            "td m: nat=%.48s age=%lld",
            mt->last_native[0] ? mt->last_native : "(none)",
            (long long)age);
        if (mt->park_why[0])
            off += (size_t)snprintf(buf + off, cap - off, " why=%.15s", mt->park_why);
        /* v36.06: stream-read shadow — classifies a read spin (see jvm.h) */
        if (mt->last_stm_len > 0)
            off += (size_t)snprintf(buf + off, cap - off, " stm=%d/%d/%d r=%lld",
                    mt->last_stm_pos, mt->last_stm_count, mt->last_stm_len,
                    (long long)mt->last_stm_reads);
        /* v34.99: monenter + wait age on the MAIN thread line (Windows twin
         * of the POSIX addition — the v34.98 freeze was invisible here). */
        if (mt->mon_enter_wait)
            off += (size_t)snprintf(buf + off, cap - off, " monenter=%llds",
                    mt->mon_enter_ms ? (long long)((now_ms - mt->mon_enter_ms) / 1000LL) : -1LL);
        if (deep) {
            JavaFrame* f = mt->current_frame;
            for (int d = 0; f && d < 3 && off + 56 < cap; d++) {
                off += (size_t)snprintf(buf + off, cap - off, " | %.22s.%.12s@%u",
                    f->clazz && f->clazz->class_name ? f->clazz->class_name : "?",
                    f->method && f->method->name ? f->method->name : "?",
                    (unsigned)f->pc);
                f = f->prev;
            }
        }
        off += (size_t)snprintf(buf + off, cap - off, "\n");
    }

    for (int i = 0; i < MAX_PTHREADS; i++) {
        JavaThread* t = g_win_live_jt[i];
        if (!t) continue;
        if (!g_pthread_running[i]) {
            if (off + 40 < cap)
                off += (size_t)snprintf(buf + off, cap - off, "td t%d: done\n", t->id);
            continue;
        }
        jlong age = t->last_native_ms ? now_ms - t->last_native_ms : -1;
        extern ThreadState thread_get_state(JavaThread* thread);
        ThreadState st = thread_get_state(t);
        const char* sts = "run";
        if (st == THREAD_STATE_TIMED_WAITING) sts = "sleep";
        else if (st == THREAD_STATE_WAITING) sts = "wait";
        else if (st == THREAD_STATE_BLOCKED) sts = "blocked";
        if (off + 200 < cap) {
            off += (size_t)snprintf(buf + off, cap - off,
                "td t%d: %s nat=%.48s age=%lld%s",
                t->id, sts,
                t->last_native[0] ? t->last_native : "(none)",
                (long long)age,
                t->mon_enter_wait ? " monenter" : "");
            /* v34.99: monitor-wait age for runner threads (see the m-line
             * note above). */
            if (t->mon_enter_wait)
                off += (size_t)snprintf(buf + off, cap - off, "=%llds",
                        t->mon_enter_ms ? (long long)((now_ms - t->mon_enter_ms) / 1000LL) : -1LL);
            /* v34.99: monitor-wait age for runner threads (Windows twin). */
            if (t->mon_enter_wait)
                off += (size_t)snprintf(buf + off, cap - off, "=%llds",
                        t->mon_enter_ms ? (long long)((now_ms - t->mon_enter_ms) / 1000LL) : -1LL);
            /* v34.98: the slow-check park reason — explains "run but frozen" */
            if (t->park_why[0])
                off += (size_t)snprintf(buf + off, cap - off, " why=%.15s", t->park_why);
            /* v36.06: stream-read shadow — classifies a read spin */
            if (t->last_stm_len > 0)
                off += (size_t)snprintf(buf + off, cap - off, " stm=%d/%d/%d r=%lld",
                        t->last_stm_pos, t->last_stm_count, t->last_stm_len,
                        (long long)t->last_stm_reads);
            /* v34.96: deep=1 walks up to 2 Java frames of each runner too */
            /* v36.48 [DUMP-FENCE]: сайт бегуна — только строковый снапшот
             * (кадры бегуна освобождаются его же unwind-ом без блокировок;
             * снапшот остаётся валиден и после смерти потока — реестровая
             * запись живёт до teardown, буфер jsite тоже). */
            if (deep && t->jsite[0]) {
                off += (size_t)snprintf(buf + off, cap - off, " | %.48s",
                                        t->jsite);
            }
            off += (size_t)snprintf(buf + off, cap - off, "\n");
        }
    }

    /* v34.96: M3G pipeline lock state (see POSIX twin) */
    {
        extern void m3g_lock_state_snprint(char* buf, size_t cap);
        if (off + 200 < cap) {
            off += (size_t)snprintf(buf + off, cap - off, "\n");
            m3g_lock_state_snprint(buf + off, cap - off);
            off += strlen(buf + off);
            off += (size_t)snprintf(buf + off, cap - off, "\n");
        }
    }
}
#else
/* POSIX threading */
static pthread_t g_pthreads[MAX_PTHREADS];
static volatile bool g_pthread_running[MAX_PTHREADS];
static pthread_mutex_t g_pthread_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_pthread_cond[MAX_PTHREADS];

/* v34.9: registry of live pthread-backed Java threads for the watchdog
 * stack dump (jvm_dump_all_threads). Racy reads are acceptable: the dump
 * is diagnostic-only and only walks pointer chains. */
static JavaThread* g_live_threads[MAX_PTHREADS];
static volatile bool g_live_running[MAX_PTHREADS];
static int g_live_count = 0;

/* v35.12 MULTI-SESSION FIX (POSIX twin of the Windows helper above):
 * jvm_destroy frees every JavaThread registered here; the next session
 * must not inherit the dangling entries (garbage "td t6684680" ids in the
 * per-second dump on the user's Switch, tid probes over freed memory in
 * the M3G lock-recovery path). Safe ONLY when no VM runner is alive. */
void native_threads_registry_reset(void) {
    for (int i = 0; i < MAX_PTHREADS; i++) {
        g_live_threads[i] = NULL;
        g_live_running[i] = false;
        g_pthreads[i] = (pthread_t)0;
        g_pthread_running[i] = false;
    }
    g_live_count = 0;
    LOG_SAFE("[THREAD] registry reset (session boundary)\n");
}

/* v36.13 HARD-KILL ("прибить все при выходе из мидлета"): the registry IS
 * the global thread list; this walks it and signals every surviving VM
 * runner. The interpreter loop re-checks jvm->running on every dispatch,
 * and the chunked Object.wait/sleep parks (250 ms) re-check it on every
 * chunk — the broadcast here additionally wakes parkers sitting inside
 * pthread cond waits (Thread.join / native waits on this slot's condvar)
 * so they observe jvm->running == false at the earliest possible chunk
 * instead of at their next timeout. There is no pthread_cancel on HOS;
 * the kill is therefore cooperative but ABSOLUTE: no runner can execute
 * more than one bytecode dispatch past the flag (execute.c:1064). */
void native_threads_kill_all(void) {
    int killed = 0;
    const char* names[4] = {0};
    int names_n = 0;
    for (int i = 0; i < g_live_count && i < MAX_PTHREADS; i++) {
        JavaThread* t = g_live_threads[i];
        if (!t || !g_live_running[i]) continue;
        killed++;
        if (names_n < 4 && t->name) names[names_n++] = t->name;
        /* frame chain detached the same way the runner does at its own
         * exit — a runner mid-dispatch overwrites these on its way out */
        t->is_alive = false;
        t->interrupted = false;
    }
#ifdef _WIN32
    for (int i = 0; i < MAX_PTHREADS; i++) {
        if (g_pthread_running[i] && g_win_thread_events[i]) {
            SetEvent(g_win_thread_events[i]);
        }
    }
#else
    for (int i = 0; i < MAX_PTHREADS; i++) {
        if (g_pthread_running[i]) {
            pthread_mutex_lock(&g_pthread_mutex);
            pthread_cond_broadcast(&g_pthread_cond[i]);
            pthread_mutex_unlock(&g_pthread_mutex);
        }
    }
#endif
    LOG_SAFE("[THREAD-KILL] %d runner(s) signaled:%s%s%s%s\n", killed,
             names_n > 0 ? " " : "",
             names_n > 0 ? names[0] : "", names_n > 1 ? ", " : "",
             names_n > 1 ? names[1] : "");
}

void jvm_dump_all_threads(void) {
    /* v34.9 SAFE version: do NOT walk frames of live pthreads (racy ->
     * segfault). Print only liveness; stack frames are dumped by
     * jvm_dump_blocked_threads() in threads.c for threads parked inside
     * monitor_wait (their frame chains are stable there). */
    LOG_SAFE("[THREADDUMP] === begin (%d registered) ===\n", g_live_count);
    for (int i = 0; i < g_live_count && i < MAX_PTHREADS; i++) {
        JavaThread* t = g_live_threads[i];
        if (!t) continue;
        if (!g_live_running[i]) {
            LOG_SAFE("[THREADDUMP] tid=%d finished\n", t->id);
        } else {
            /* v34.11: include the last native started on this thread and how
             * long ago — a large age means the thread is stuck inside it. */
            struct timespec now_ts;
            clock_gettime(CLOCK_MONOTONIC, &now_ts);
            jlong now_ms = (jlong)now_ts.tv_sec * 1000 + now_ts.tv_nsec / 1000000;
            jlong age = t->last_native_ms ? now_ms - t->last_native_ms : -1;
            LOG_SAFE("[THREADDUMP] tid=%d running last_native=%s age=%lldms\n",
                     t->id,
                     t->last_native[0] ? t->last_native : "(none)",
                     (long long)age);
        }
    }
    LOG_SAFE("[THREADDUMP] === end ===\n");
    fflush(stderr);
}

/* v34.96 M3G-LOCK WATCHDOG helper: is the JavaThread with this id alive?
 * Used by mobile3d.c's bounded lock acquisition to detect ORPHANED M3G
 * locks (recorded owner thread no longer registered/running -> the lock
 * was escaped without its cleanup guards -> safe to reinitialize).
 * tid==1 is the MAIN Java thread (frontend OS thread) — always alive from
 * a waiter's perspective (a dead frontend cannot be waiting on anything).
 * Racy reads are fine: diagnostics + a conservative recovery gate. */
int jvm_java_thread_alive_by_id(int tid) {
    if (tid <= 0) return 0;
    if (tid == 1) return 1;   /* main/frontend */
    for (int i = 0; i < g_live_count && i < MAX_PTHREADS; i++) {
        JavaThread* t = g_live_threads[i];
        if (t && t->id == tid) {
            return (g_live_running[i] && t->is_alive) ? 1 : 0;
        }
    }
    return 0;
}

/* v34.95: COMPACT per-thread state dump for the trace-log heartbeat (the
 * Switch user's per-second "what is every thread doing" request).
 * One line per thread, prefixed "td":
 *   td m:  the MAIN Java thread (frontend) — last native + age; with deep=1
 *          also up to 3 Java frames (class.method@pc). deep=1 is ONLY legal
 *          when the frontend is provably stuck (frames stable, no mutation).
 *   td tN: each registered pthread VM runner — scheduler state, last native
 *          + age, monenter flag (blocked in monitor_enter); with deep=1 also
 *          up to 2 Java frames (v34.96 — locates a pure-Java spin loop).
 *   td m3g: current/last holder of the two M3G pipeline locks (v34.96).
 * Written into the caller buffer (embedded '\n' separators; the trace layer
 * emits one log line per thread). Racy reads are acceptable: diagnostics. */
void jvm_threads_dump_snprint(JVM* jvm, char* buf, size_t cap, int deep) {
    size_t off = 0;
    if (!buf || cap < 64) return;
    buf[0] = '\0';
    struct timespec now_ts;
    clock_gettime(CLOCK_MONOTONIC, &now_ts);
    jlong now_ms = (jlong)now_ts.tv_sec * 1000 + now_ts.tv_nsec / 1000000;

    if (jvm && jvm->main_thread) {
        JavaThread* mt = jvm->main_thread;
        jlong age = mt->last_native_ms ? now_ms - mt->last_native_ms : -1;
        /* v34.96: nat widened %.32s -> %.48s — "javax/microedition/m3g/
         * Graphics3D.releaseTarget" is 48 chars; the v34.95 trace cut the
         * METHOD NAME off exactly at the class name, hiding whether the
         * wedge was in bindTarget / render / releaseTarget. */
        off += (size_t)snprintf(buf + off, cap - off,
            "td m: nat=%.48s age=%lld",
            mt->last_native[0] ? mt->last_native : "(none)",
            (long long)age);
        /* v34.98: slow-check park reason ("fe-pause"/"gc-sp") — explains a
         * "run" main thread with a growing age and no new native. */
        if (mt->park_why[0])
            off += (size_t)snprintf(buf + off, cap - off, " why=%.15s", mt->park_why);
        /* v36.06: stream-read shadow — classifies a read spin (see jvm.h) */
        if (mt->last_stm_len > 0)
            off += (size_t)snprintf(buf + off, cap - off, " stm=%d/%d/%d r=%lld",
                    mt->last_stm_pos, mt->last_stm_count, mt->last_stm_len,
                    (long long)mt->last_stm_reads);
        /* v34.99: monenter + wait age on the MAIN thread line. The v34.98
         * field freeze had m blocked in monitor_enter for 15+ s while the
         * td m line could NOT show it — the flag was printed on tN lines
         * only, so the whole "monitor" class of freezes was invisible in
         * every trace before this. A frozen-at-one-pc m with a growing
         * monenter=<s> IS the freeze signature. */
        if (mt->mon_enter_wait)
            off += (size_t)snprintf(buf + off, cap - off, " monenter=%llds",
                    mt->mon_enter_ms ? (long long)((now_ms - mt->mon_enter_ms) / 1000LL) : -1LL);
        /* v34.99 [M-STALL] classifier: separate the two remaining freeze
         * families automatically. The v34.98 trace showed m frozen at ONE
         * pc (g.b@18) for 15+ s with all M3G locks free and t2 running —
         * compatible BOTH with a monitor block (monitorenter) AND with a
         * spin inside an opcode's C implementation / the dispatch stage.
         * Track pc stability across beats; only while m is inside a
         * main-thread exec window (current_frame is stale between
         * windows). monenter=0 + frozen pc 6 beats + native age >3 s =>
         * opcode/dispatch spin suspected (NOT a monitor). */
        {
            /* v36.48 [DUMP-FENCE]: трекинг «замершего сайта» — по СТРОЧНОМУ
             * снапшоту mt->jsite (пишет сам главный поток в push_frame /
             * каждые 64 yield), НЕ по кадрам: разыменование current_frame
             * из beat-потока гонится с pop_frame/frame_destroy и ловит
             * use-after-free (ASan, ys_asan_loop). Семантика прежняя:
             * один и тот же сайт 6 биений подряд при живом исполнении =
             * подозрение на спин в опкоде/диспетчере. */
            static char s_m_stall_site[64] = "";
            static int s_m_stall_beats = 0;
            extern volatile int g_jvm_main_thread_executing;
            if (g_jvm_main_thread_executing && mt->jsite[0]) {
                if (strcmp(mt->jsite, s_m_stall_site) == 0) {
                    s_m_stall_beats++;
                } else {
                    snprintf(s_m_stall_site, sizeof(s_m_stall_site), "%s", mt->jsite);
                    s_m_stall_beats = 1;
                }
                if (s_m_stall_beats >= 6 && !mt->mon_enter_wait && age > 3000) {
                    off += (size_t)snprintf(buf + off, cap - off,
                            " [M-STALL: site frozen %d beats / %lld ms (%.48s), no monitor wait — opcode-or-dispatch spin suspected]",
                            s_m_stall_beats, (long long)age, mt->jsite);
                    s_m_stall_beats = 0;   /* log once per stall episode */
                }
            } else {
                s_m_stall_site[0] = '\0';
                s_m_stall_beats = 0;
            }
        }
        /* v36.48 [DUMP-FENCE]: deep-строка главного потока — ТОЛЬКО
         * снапшот jsite (внутренний сайт на гранулярности 64k инструкций),
         * БЕЗ обхода кадров: цепочка current_frame живёт в чужом потоке и
         * освобождается без блокировок. Потеря контекста вызывающих
         * (было 6 кадров) принята: корректность > глубина диагностики;
         * nat=/why=/stm= рядом по-прежнему называют натив и парковку. */
        if (deep && mt->jsite[0]) {
            off += (size_t)snprintf(buf + off, cap - off, " | %.48s",
                                    mt->jsite);
        }
        off += (size_t)snprintf(buf + off, cap - off, "\n");
    }

    for (int i = 0; i < g_live_count && i < MAX_PTHREADS; i++) {
        JavaThread* t = g_live_threads[i];
        if (!t) continue;
        if (!g_live_running[i]) {
            if (off + 40 < cap)
                off += (size_t)snprintf(buf + off, cap - off, "td t%d: done\n", t->id);
            continue;
        }
        jlong age = t->last_native_ms ? now_ms - t->last_native_ms : -1;
        /* Scheduler state is meaningful only for the waiting kinds; pthread
         * runners not registered in the coop scheduler would read TERMINATED
         * — report plain "run" for those (registry above already says alive). */
        extern ThreadState thread_get_state(JavaThread* thread);
        ThreadState st = thread_get_state(t);
        const char* sts = "run";
        if (st == THREAD_STATE_TIMED_WAITING) sts = "sleep";
        else if (st == THREAD_STATE_WAITING) sts = "wait";
        else if (st == THREAD_STATE_BLOCKED) sts = "blocked";
        if (off + 200 < cap) {
            off += (size_t)snprintf(buf + off, cap - off,
                "td t%d: %s nat=%.48s age=%lld%s",
                t->id, sts,
                t->last_native[0] ? t->last_native : "(none)",
                (long long)age,
                t->mon_enter_wait ? " monenter" : "");
            /* v34.98: the slow-check park reason — explains "run but frozen" */
            if (t->park_why[0])
                off += (size_t)snprintf(buf + off, cap - off, " why=%.15s", t->park_why);
            /* v36.06: stream-read shadow — classifies a read spin */
            if (t->last_stm_len > 0)
                off += (size_t)snprintf(buf + off, cap - off, " stm=%d/%d/%d r=%lld",
                        t->last_stm_pos, t->last_stm_count, t->last_stm_len,
                        (long long)t->last_stm_reads);
            /* v34.96: with deep=1 also walk up to 2 JAVA frames of each
             * runner — a "run" thread with a huge age and no new native is
             * a pure-Java loop; the frames name WHERE it spins. */
            /* v36.48 [DUMP-FENCE]: сайт бегуна — только строковый снапшот
             * (кадры бегуна освобождаются его же unwind-ом без блокировок;
             * снапшот остаётся валиден и после смерти потока — реестровая
             * запись живёт до teardown, буфер jsite тоже). */
            if (deep && t->jsite[0]) {
                off += (size_t)snprintf(buf + off, cap - off, " | %.48s",
                                        t->jsite);
            }
            off += (size_t)snprintf(buf + off, cap - off, "\n");
        }
    }

    /* v34.96: M3G pipeline lock state — the v34.95 freeze left the main
     * thread INSIDE a Graphics3D native with sp=0 and no monenter, i.e.
     * blocked in an unbounded pthread lock wait. Name the holder here. */
    {
        extern void m3g_lock_state_snprint(char* buf, size_t cap);
        if (off + 200 < cap) {
            off += (size_t)snprintf(buf + off, cap - off, "\n");
            m3g_lock_state_snprint(buf + off, cap - off);
            off += strlen(buf + off);
            off += (size_t)snprintf(buf + off, cap - off, "\n");
        }
    }
}

/* v35.01: cheap VM-thread liveness probe for the diag line and the
 * [T-STALL]/[THREAD-EXIT] beat channel (switch_trace.c).
 *
 * WHY: the v35.00 field trace showed the NEW freeze form — the frontend
 * loop stayed ALIVE (loop=222/s, no STUCK, menu exit worked) while the
 * game world froze. With the logging toggle off (the default) that state
 * had ZERO log visibility: td dumps are gated by the toggle, and STUCK
 * only watches the frontend loop. This probe gives the forced diag line
 * two numbers that make the frozen state self-reporting:
 *   *out_live      - alive pthread VM runners (a drop below the session
 *                    high means a thread DIED — e.g. unhandled exception);
 *   *out_worst_age - worst "ms since last native dispatch" among threads
 *                    that are RUN-able right now (a pure-Java spin or a
 *                    silently wedged loop; threads parked in sleep/wait/
 *                    blocked are LEGITIMATELY idle and only counted in
 *                    *out_waiting). The main thread (m) participates ONLY
 *                    while its exec window is open (g_jvm_main_thread_
 *                    executing) — outside it m legitimately idles.
 * Read-only walk of the same registry the per-second dump uses; safe to
 * call from the frontend main thread or the SDL timer thread. */
void jvm_thread_liveness(void* jvm_arg, int* out_live, long long* out_worst_age,
                         int* out_worst_tid, int* out_waiting) {
    JVM* jvm = (JVM*)jvm_arg;
    if (out_live) *out_live = 0;
    if (out_worst_age) *out_worst_age = -1;
    if (out_worst_tid) *out_worst_tid = -1;
    if (out_waiting) *out_waiting = 0;
    struct timespec now_ts;
    clock_gettime(CLOCK_MONOTONIC, &now_ts);
    jlong now_ms = (jlong)now_ts.tv_sec * 1000 + now_ts.tv_nsec / 1000000;

    extern ThreadState thread_get_state(JavaThread* thread);
    int live = 0, waiting = 0, worst_tid = -1;
    long long worst = -1;

    /* m (the primordial VM thread) — only inside its exec window. */
    if (jvm && jvm->main_thread) {
        extern volatile int g_jvm_main_thread_executing;
        JavaThread* mt = jvm->main_thread;
        if (g_jvm_main_thread_executing) {
            live++;
            /* v36.05: a runnable thread with NO native ever dispatched ages
             * from CREATION, not -1 (a pure-bytecode spin must not read as
             * healthy — the Doom RPG [Rus] hang shape). */
            long long age = mt->last_native_ms ? now_ms - mt->last_native_ms
                            : (mt->created_ms ? now_ms - mt->created_ms : -1);
            if (age > worst) { worst = age; worst_tid = mt->id; }
        }
    }
    for (int i = 0; i < g_live_count && i < MAX_PTHREADS; i++) {
        JavaThread* t = g_live_threads[i];
        if (!t || !g_live_running[i]) continue;
        live++;
        ThreadState st = thread_get_state(t);
        if (st == THREAD_STATE_TIMED_WAITING || st == THREAD_STATE_WAITING ||
            st == THREAD_STATE_BLOCKED) {
            waiting++;
            continue;
        }
        /* v36.05: same created_ms fallback as for m (see above). */
        long long age = t->last_native_ms ? now_ms - t->last_native_ms
                        : (t->created_ms ? now_ms - t->created_ms : -1);
        if (age > worst) { worst = age; worst_tid = t->id; }
    }
    if (out_live) *out_live = live;
    if (out_worst_age) *out_worst_age = worst;
    if (out_worst_tid) *out_worst_tid = worst_tid;
    if (out_waiting) *out_waiting = waiting;
}

/* v34.11: record the native method about to execute on this thread.
 * Called from all five native dispatch sites in opcodes.c. Cheap: one
 * snprintf + clock_gettime per native call. */
extern void v37_note_dispatch(const char* cls, const char* mth);
void jvm_note_native_call(JavaThread* thread, const char* cls, const char* mth) {
    if (cls && mth) v37_note_dispatch(cls, mth);
    if (!thread) return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    thread->last_native_ms = (jlong)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    if (cls && mth) {
        snprintf(thread->last_native, sizeof(thread->last_native), "%.60s.%.30s", cls, mth);
    } else {
        snprintf(thread->last_native, sizeof(thread->last_native), "?");
    }
}

/* v34.9: number of alive pthread-backed VM threads (for GC safepoint census) */
int jvm_live_vm_thread_count(void) {
    int n = 0;
    for (int i = 0; i < g_live_count && i < MAX_PTHREADS; i++) {
        if (g_live_threads[i] && g_live_running[i]) n++;
    }
    return n;
}

/* v34.26: is the CALLING OS thread one of the pthread VM runners?
 * gc_collect() computes the safepoint census as live-1 when the collector
 * itself is a runner; but when GC is triggered from the FRONTEND thread
 * (libretro retro_run steps the main Java thread on the host thread via
 * execute_frame), the caller is NOT a runner and all live runners must park.
 * The old unconditional live-1 started the sweep with one mutator still
 * running when triggered from the frontend — torn-heap risk. */
int jvm_current_os_thread_is_vm_runner(void) {
    pthread_t self = pthread_self();
    for (int i = 0; i < MAX_PTHREADS; i++) {
        if (g_pthread_running[i] && g_pthreads[i] != (pthread_t)0 &&
            pthread_equal(g_pthreads[i], self)) {
            return 1;
        }
    }
    return 0;
}
#endif

/* Thread wrapper args */
typedef struct {
    JVM* jvm;
    JavaThread* java_thread;
    JavaMethod* run_method;
    JavaObject* run_obj;
} PthreadRunArgs;

#ifdef _WIN32
/* Windows thread runner */
static DWORD WINAPI win_thread_runner(LPVOID arg) {
    PthreadRunArgs* args = (PthreadRunArgs*)arg;
    int thread_id = args->java_thread->id;

    /* v34.43 FIX (Asphalt 3D every-2s stutter): see the POSIX twin in
     * pthread_thread_runner — game threads must not starve the frontend
     * pump thread on CPU-starved hosts. NOJME_THREAD_NICE=0 disables (default 10). */
    {
        static int nice_delta = -999;
        if (nice_delta == -999) {
            const char* e = getenv("NOJME_THREAD_NICE");
            nice_delta = (e && e[0]) ? atoi(e) : 10;
        }
        if (nice_delta > 0) {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        }
    }
    
    /* Set TLS for this thread */
    extern void pthread_set_current_thread(JavaThread* thread);
    pthread_set_current_thread(args->java_thread);
    /* v36.30 [EXIT-FENCE]: mark this OS thread as a VM runner (Windows twin) */
    {
        extern __thread int tls_os_thread_is_vm_runner;
        tls_os_thread_is_vm_runner = 1;
    }
    
    /* Verify TLS was set correctly */
    extern JavaThread* thread_current(JVM* jvm);
    (void)thread_current(args->jvm);
    
    g_win_threads[thread_id] = GetCurrentThread();
    g_win_thread_ids[thread_id] = GetCurrentThreadId();  /* v34.26: GC census */
    g_win_live_jt[thread_id] = args->java_thread;  /* v34.12: for THREADDUMP */
    g_pthread_running[thread_id] = true;
    
    /* Execute the run() method */
    extern int execute_method(JVM* jvm, JavaThread* thread, JavaMethod* method, 
                              JavaValue* args, JavaValue* result);
    JavaValue this_arg = { .ref = args->run_obj };
    JavaValue result;
    execute_method(args->jvm, args->java_thread, args->run_method, &this_arg, &result);
    
    
    /* Mark thread as terminated */
    args->java_thread->is_alive = false;
    args->java_thread->interrupted = false;
    /* v34.59 TLAB: закрыть свой аллокационный чанк ДО смерти потока
     * (см. POSIX-близнец pthread_thread_runner). */
    {
        extern void heap_tlab_flush_self(void);
        heap_tlab_flush_self();
    }
    /* v34 FIX: detach the dead thread's frame chain (see win/pthread twin
     * comment in pthread_thread_runner) - a stale current_frame on the
     * jvm->threads[] shell makes the GC mark freed TLS memory as live. */
    args->java_thread->current_frame = NULL;
    args->java_thread->pending_exception = NULL;
    args->java_thread->exec_jmp_buf_valid = 0;
    g_win_live_jt[thread_id] = NULL;  /* v34.12: for THREADDUMP */
    g_pthread_running[thread_id] = false;

    /* CRITICAL: Memory barrier before signaling completion.
     * This ensures all writes done by this thread (including field modifications)
     * are visible to other threads before they see is_alive=false or receive
     * the join signal. This provides the happens-before relationship required
     * by the Java Memory Model for Thread.join().
     */
    memory_barrier();
    
    /* Signal any waiting join() */
    if (g_win_thread_events[thread_id]) {
        SetEvent(g_win_thread_events[thread_id]);
    }
    
    /* Clear TLS before exiting */
    pthread_set_current_thread(NULL);
    
    /* Free the args */
    free(args);
    
    return 0;
}
#else
/* POSIX thread runner */
static void* pthread_thread_runner(void* arg) {
    PthreadRunArgs* args = (PthreadRunArgs*)arg;
    int thread_id = args->java_thread->id;
    
    /* v34.43 FIX (Asphalt 3D "freeze every ~2s" on CPU-starved hosts):
     * game threads interpret bytecode flat-out; on the user's ARMv7
     * handheld (and our dual-core x86 box) their periodic bursts delayed
     * the frontend pump thread's 10ms wakeup by 10-40ms measured (x86) —
     * scaling to multi-hundred-ms stalls on Cortex-A7 = the reported
     * freezes. Real KVM handsets run all MIDlet threads cooperatively in
     * ONE OS thread, so the UI/present path always won there. Restore that
     * balance: drop game threads below the pump thread's priority. The
     * pump's duty cycle is ~1ms/16ms, so game throughput is unaffected.
     * NOJME_THREAD_NICE=<delta> overrides (0 disables; default 10). */
    {
#ifndef __SWITCH__
        /* v34.86: skipped on Switch — newlib/libnx have no syscall(2)/
         * SYS_gettid and no setpriority/PRIO_PROCESS (see the include
         * guard above); thread priorities there are libnx's business. */
        static int nice_delta = -999;
        if (nice_delta == -999) {
            const char* e = getenv("NOJME_THREAD_NICE");
            nice_delta = (e && e[0]) ? atoi(e) : 10;
        }
        if (nice_delta > 0) {
            /* Per-thread nice: on NPTL, setpriority(PRIO_PROCESS, tid, n)
             * targets exactly this pthread — the pump thread keeps 0. */
            setpriority(PRIO_PROCESS, (pid_t)syscall(SYS_gettid), nice_delta);
        }
#endif /* !__SWITCH__ */
    }
    
    /* v34.9: register in the watchdog live-thread registry */
    if (g_live_count < MAX_PTHREADS) {
        g_live_threads[g_live_count] = args->java_thread;
        g_live_running[g_live_count] = true;
        g_live_count++;
    }
    
    /* v34.95 CENSUS-WINDOW FIX: a GC that computed its safepoint census
     * between our pthread_create and the registration above does not count
     * us — wait_arrivals() would return without us, the mark/sweep would
     * run while our first instructions allocate (torn-heap risk), and the
     * newcomer's mid-sweep TLAB flush could also truncate the sweep. Park
     * BEFORE the first bytecode executes; jvm_gc_safepoint_park() is a
     * no-op when no request is pending (one volatile read per thread start). */
    {
        extern volatile int g_gc_safepoint_request;
        if (g_gc_safepoint_request) {
            extern void jvm_gc_safepoint_park(void);
            jvm_gc_safepoint_park();
        }
    }
    
    /* v34.26: gated — raw fprintf+fflush on EVERY thread start/stop (see
     * native_thread_start comment) was always-on stderr traffic. */
    LOG_SAFE("[THREAD] pthread started: id=%d %s\n", 
            thread_id, args->run_obj && args->run_obj->header.clazz && args->run_obj->header.clazz->class_name ? args->run_obj->header.clazz->class_name : "?");
    
    /* Set TLS for this pthread so thread_current() works correctly */
    extern void pthread_set_current_thread(JavaThread* thread);
    pthread_set_current_thread(args->java_thread);
    /* v36.30 [EXIT-FENCE]: этот OS-поток — VM-раннер (для быстрой проверки
     * arm-условия фенса в execute_method; полная форма — см. комментарий). */
    {
        extern __thread int tls_os_thread_is_vm_runner;
        tls_os_thread_is_vm_runner = 1;
    }
    
    /* Store pthread_t for join */
    g_pthreads[thread_id] = pthread_self();
    g_pthread_running[thread_id] = true;
    
    LOG_SAFE("[THREAD] Calling run() id=%d %s\n",
            thread_id, args->run_obj && args->run_obj->header.clazz && args->run_obj->header.clazz->class_name ? args->run_obj->header.clazz->class_name : "?");
    
    /* Execute the run() method */
    extern int execute_method(JVM* jvm, JavaThread* thread, JavaMethod* method, 
                              JavaValue* args, JavaValue* result);
    JavaValue this_arg = { .ref = args->run_obj };
    JavaValue result;
    execute_method(args->jvm, args->java_thread, args->run_method, &this_arg, &result);
    
    LOG_SAFE("[THREAD] run() done id=%d exc=%p\n", thread_id, (void*)args->java_thread->pending_exception);
    if (args->java_thread->pending_exception) {
        JavaClass* exc_clazz = args->java_thread->pending_exception->header.clazz;
        LOG_SAFE("[THREAD]   exc class=%s\n",
                exc_clazz && exc_clazz->class_name ? exc_clazz->class_name : "?");
    }
    
    /* Check for uncaught exception */
    if (args->java_thread->pending_exception) {
        LOG_SAFE("[THREAD] Thread '%s' terminated with uncaught exception: %s\n",
                args->java_thread->name ? args->java_thread->name : "(unnamed)",
                args->java_thread->pending_exception->header.clazz ? 
                args->java_thread->pending_exception->header.clazz->class_name : "?");
        /* Set global flag so main loop can detect this */
        extern bool g_has_uncaught_exception;
        g_has_uncaught_exception = true;
    }
    /* v36.04 VM-THREAD-DEATH: the same exit signal into the Switch trace
     * file (exception class + message + captured stack — stderr-only
     * LOG_SAFE lines never reach log.txt; see threads.c for the rationale). */
    {
        extern void jvm_thread_death_forced(const char* name, JavaObject* pending_ex,
                                            const char* stack_trace, const char* site);
        jvm_thread_death_forced(args->java_thread->name,
                                args->java_thread->pending_exception,
                                args->java_thread->exception_stack_trace, "pthread");
    }
    
    /* Mark thread as terminated */
    args->java_thread->is_alive = false;
    args->java_thread->interrupted = false;
    /* v34.59 TLAB: закрыть свой аллокационный чанк ДО смерти потока —
     * иначе неиспользованный хвост чанка останется без валидного
     * заголовка и следующий sweep споткнётся о мусор (инвариант №3). */
    {
        extern void heap_tlab_flush_self(void);
        heap_tlab_flush_self();
    }
    /* v34 FIX (VmTest: heap stuck at 100% live after the threads group):
     * the JavaThread shell stays registered in jvm->threads[] (a GC root
     * chain) for the whole VM lifetime. A stale current_frame here points
     * into THIS pthread's TLS frame pool / stack arenas, which die with the
     * thread - the GC then walks freed memory and marks huge amounts of
     * garbage as live (466 futile gc_collect()s freed nothing and every
     * later allocation OOMed). Detach the dead frame chain, mirroring the
     * coop-scheduler reaper. */
    args->java_thread->current_frame = NULL;
    args->java_thread->pending_exception = NULL;
    args->java_thread->exec_jmp_buf_valid = 0;
    g_pthread_running[thread_id] = false;
    

    /* v34.9: mark as finished in the watchdog registry */
    for (int i = 0; i < g_live_count && i < MAX_PTHREADS; i++) {
        if (g_live_threads[i] == args->java_thread) {
            g_live_running[i] = false;
            break;
        }
    }
    /* CRITICAL: Memory barrier before signaling completion.
     * This ensures all writes done by this thread (including field modifications)
     * are visible to other threads before they see is_alive=false or receive
     * the join signal. This provides the happens-before relationship required
     * by the Java Memory Model for Thread.join().
     */
    memory_barrier();
    
    /* Signal any waiting join() */
    pthread_mutex_lock(&g_pthread_mutex);
    pthread_cond_broadcast(&g_pthread_cond[thread_id]);
    pthread_mutex_unlock(&g_pthread_mutex);
    
    /* Clear TLS before exiting */
    pthread_set_current_thread(NULL);
    /* v36.12: return this pthread's frame pool to the heap before it dies
     * (the pool is _Thread_local and would otherwise leak with the thread —
     * see jvm_frame_pool_flush in execute.c). */
    {
        extern void jvm_frame_pool_flush(void);
        jvm_frame_pool_flush();
    }
    
    /* Free the args */
    free(args);
    
    return NULL;
}
#endif

/* v9: monotonic milliseconds for the shutdown idle-wait barrier. */
static uint64_t native_monotonic_ms(void) {
#ifdef _WIN32
    return (uint64_t)GetTickCount();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000u);
#endif
}

/* v9 FIX (heap-use-after-free at VM teardown, ASAN-confirmed):
 * Thread.start() spawns DETACHED native runners that interpret bytecode
 * against class memory. jvm_destroy must wait for them to leave the
 * interpreter before freeing JavaThread structs and class data.
 *
 * Reads g_pthread_running[] without the mutex — it is a volatile bool
 * written once by the runner; a benign race is impossible for bool and
 * this is a teardown barrier, not a synchronisation primitive.
 *
 * Returns the number of runners STILL executing after timeout_ms (0 = all
 * idle). If > 0, the caller MUST NOT free class/thread memory. */
int native_threads_wait_idle(int timeout_ms) {
    uint64_t deadline = native_monotonic_ms() + (uint64_t)timeout_ms;
    for (;;) {
        int busy = 0;
        for (int i = 0; i < MAX_PTHREADS; i++) {
            if (g_pthread_running[i]) busy++;
        }
        if (busy == 0) return 0;
        if (native_monotonic_ms() >= deadline) return busy;
#ifdef _WIN32
        Sleep(1);
#else
        usleep(1000);
#endif
    }
}

static JavaValue native_thread_start(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    (void)arg_count;
    JavaObject* thread_obj = (JavaObject*)args[0].ref;
    
    static int thread_start_count = 0;
    thread_start_count++;
    
    if (!thread_obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    
    /* Get the Runnable target */
    JavaObject* run_target = (JavaObject*)native_get_field_value(thread_obj, "target").ref;
    
    /* Determine which object's run() to call */
    JavaObject* run_obj = run_target ? run_target : thread_obj;
    JavaClass* run_class = run_obj->header.clazz;
    
    /* v34.26: gated (was raw fprintf+fflush — always-on stderr writes with
     * blocking flushes on piped consoles; on armv7 Thread.start() during
     * gameplay added multi-ms write syscalls to the CALLING game thread). */
    LOG_SAFE("[THREAD] Thread.start() (#%d): %s\n", 
            thread_start_count, run_class && run_class->class_name ? run_class->class_name : "?");
    
    /* Find run() method */
    JavaMethod* run_method = jvm_resolve_method(jvm, run_class, "run", "()V");
    if (!run_method) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Get thread name from Java field */
    char thread_name[64];
    JavaString* name_str = (JavaString*)native_get_field_value(thread_obj, "name").ref;
    if (name_str && name_str->utf8) {
        strncpy(thread_name, name_str->utf8, sizeof(thread_name) - 1);
        thread_name[sizeof(thread_name) - 1] = '\0';
    } else {
        snprintf(thread_name, sizeof(thread_name), "Thread-%d", jvm->thread_count + 1);
    }
    
    /* Get thread priority from Java field */
    jint priority = native_get_field_value(thread_obj, "priority").i;
    if (priority < 1 || priority > 10) {
        priority = THREAD_PRIORITY_NORM;  /* Default to 5 if invalid */
    }
    
    JavaThread* new_java_thread = thread_create(jvm, thread_name, priority, thread_obj);
    if (!new_java_thread) {
        return NATIVE_RETURN_VOID();
    }
    
    new_java_thread->is_alive = true;
    
    /* Create thread args */
    PthreadRunArgs* pthread_args = (PthreadRunArgs*)malloc(sizeof(PthreadRunArgs));
    if (!pthread_args) {
        return NATIVE_RETURN_VOID();
    }
    
    pthread_args->jvm = jvm;
    pthread_args->java_thread = new_java_thread;
    pthread_args->run_method = run_method;
    pthread_args->run_obj = run_obj;
    
#ifdef _WIN32
    /* Windows thread creation */
    win_thread_init();
    
    /* Create event for join() */
    g_win_thread_events[new_java_thread->id] = CreateEvent(NULL, TRUE, FALSE, NULL);
    
    /* v9 FIX: set the running flag BEFORE CreateThread. The old code set it
     * inside the runner, so a shutdown racing with thread creation could
     * observe "not running" for a thread that was about to execute run(). */
    g_pthread_running[new_java_thread->id] = true;
    
    /* Create Windows thread.
     * v17 FIX: pass an EXPLICIT stack size. With dwStackSize=0 the thread
     * inherits the default from the host EXE's PE header (typically 1-2 MB),
     * which is far too small for the interpreter: one Java frame costs three
     * native frames (interpret -> op_invokestatic -> execute_method), so the
     * 500-frame Java recursion guard (StackOverflowError) needs up to several
     * MB of native stack. Deep Java recursion (vmtest 'StackOverflowError is
     * catchable') exhausted the native stack first and crashed with SIGSEGV
     * before the guard could throw. 32 MB gives 500 frames a >64 KB/frame
     * budget (reservation only; pages commit on demand). */
    HANDLE hThread = CreateThread(NULL, JAVA_NATIVE_STACK_SIZE,
                                  win_thread_runner, pthread_args, 0, NULL);
    if (hThread == NULL) {
        free(pthread_args);
        if (g_win_thread_events[new_java_thread->id]) {
            CloseHandle(g_win_thread_events[new_java_thread->id]);
            g_win_thread_events[new_java_thread->id] = NULL;
        }
        new_java_thread->is_alive = false;
        return NATIVE_RETURN_VOID();
    }
    g_win_threads[new_java_thread->id] = hThread;
    
    /* Give the new thread a chance to start */
    Sleep(1);
#else
    /* POSIX thread creation */
    pthread_t pthread_id;
    
    /* v9 FIX: set the running flag BEFORE pthread_create (see the Windows
     * branch comment) so a concurrent shutdown can never miss this thread. */
    g_pthread_running[new_java_thread->id] = true;
    
    /* v17 FIX: request an explicit stack size instead of the process default
     * (RLIMIT_STACK, often 2-8 MB). The interpreter needs several KB of native
     * stack per Java frame; see the Windows comment above for the full story. */
    pthread_attr_t thread_attr;
    int use_attr = (pthread_attr_init(&thread_attr) == 0 &&
                    pthread_attr_setstacksize(&thread_attr, JAVA_NATIVE_STACK_SIZE) == 0);
    int rc = pthread_create(&pthread_id, use_attr ? &thread_attr : NULL,
                            pthread_thread_runner, pthread_args);
    if (use_attr) pthread_attr_destroy(&thread_attr);
    if (rc != 0) {
        free(pthread_args);
        g_pthread_running[new_java_thread->id] = false;
        new_java_thread->is_alive = false;
        return NATIVE_RETURN_VOID();
    }
    
    /* Detach the thread so it cleans up automatically */
    pthread_detach(pthread_id);
    
    /* Give the new thread a chance to start */
    usleep(1000);  /* 1ms */
#endif
    
    return NATIVE_RETURN_VOID();
}

/* Thread.<init>() - default constructor */
static JavaValue native_thread_init(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* thread_obj = (JavaObject*)args[0].ref;
    
    if (!thread_obj) {
        return NATIVE_RETURN_VOID();
    }
    
    /* ИСПРАВЛЕНО: Используем native_set_field_value */
    JavaValue null_val = { .ref = NULL };
    native_set_field_value(thread_obj, "target", null_val);
    THREAD_DEBUG("<init>(): target field initialized to NULL");
    
    return NATIVE_RETURN_VOID();
}

/* Thread.<init>(Runnable) - constructor with Runnable target */
static JavaValue native_thread_init_runnable(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* thread_obj = (JavaObject*)args[0].ref;
    JavaObject* runnable = (JavaObject*)args[1].ref;
    
    if (!thread_obj) {
        return NATIVE_RETURN_VOID();
    }
    
    /* ИСПРАВЛЕНО: Используем native_set_field_value */
    JavaValue runnable_val = { .ref = runnable };
    native_set_field_value(thread_obj, "target", runnable_val);
    THREAD_DEBUG("<init>(Runnable): target field set to %p", (void*)runnable);
    
    return NATIVE_RETURN_VOID();
}

/* Thread.close() - called when thread terminates */
static JavaValue native_thread_close(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* thread_obj = (JavaObject*)args[0].ref;
    
    if (thread_obj) {
        THREAD_DEBUG("close() called for thread object %p", (void*)thread_obj);
    }
    
    return NATIVE_RETURN_VOID();
}

/* Thread.isAlive() - check if thread is alive */
static JavaValue native_thread_isAlive(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* thread_obj = (JavaObject*)args[0].ref;
    
    if (!thread_obj) {
        return NATIVE_RETURN_INT(0);
    }
    
    /* Find the JavaThread and check if alive */
    extern JavaThread* thread_find_by_object(JavaObject* thread_obj);
    JavaThread* target = thread_find_by_object(thread_obj);
    
    
    if (target && target->is_alive) {
        return NATIVE_RETURN_INT(1);
    }
    
    return NATIVE_RETURN_INT(0);
}

/* Thread.join() - wait for thread to terminate */
static JavaValue native_thread_join(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* thread_obj = (JavaObject*)args[0].ref;
    
    if (!thread_obj) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Find the JavaThread */
    extern JavaThread* thread_find_by_object(JavaObject* thread_obj);
    JavaThread* target = thread_find_by_object(thread_obj);
    
    if (!target) {
        return NATIVE_RETURN_VOID();  /* Thread already terminated or never started */
    }
    
    int thread_id = target->id;
    
    /* Check if already terminated */
    if (!target->is_alive && !g_pthread_running[thread_id]) {
        return NATIVE_RETURN_VOID();
    }
    
#ifdef _WIN32
    /* Windows: wait on event with timeout */
    /* CRITICAL: Use || (OR) not && (AND). We wait while EITHER flag indicates
     * the thread is running. With &&, if one flag becomes false before the other
     * becomes true (race condition during thread startup), join returns immediately.
     */
    while (g_pthread_running[thread_id] || target->is_alive) {
        if (g_win_thread_events[thread_id]) {
            DWORD result = WaitForSingleObject(g_win_thread_events[thread_id], 50);
            if (result == WAIT_OBJECT_0) {
                break;  /* Thread terminated */
            }
        } else {
            Sleep(10);  /* Fallback polling */
        }
        /* v34.11 GC safepoint poll (join must not hide from stop-the-world) */
        {
            extern volatile int g_gc_safepoint_request;
            if (g_gc_safepoint_request) {
                void jvm_gc_safepoint_park(void);
                jvm_gc_safepoint_park();
            }
        }
    }
#else
    /* POSIX: wait on condition variable with timeout */
    pthread_mutex_lock(&g_pthread_mutex);
    /* CRITICAL: Use || (OR) not && (AND). We wait while EITHER flag indicates
     * the thread is running. With &&, if one flag becomes false before the other
     * becomes true (race condition during thread startup), join returns immediately.
     */
    while (g_pthread_running[thread_id] || target->is_alive) {
        /* Use timed wait to avoid infinite blocking */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 50000000;  /* 50ms */
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
        }
        pthread_cond_timedwait(&g_pthread_cond[thread_id], &g_pthread_mutex, &ts);
        /* v34.11 GC safepoint poll (join must not hide from stop-the-world) */
        if (g_gc_safepoint_request) {
            jvm_gc_safepoint_park();
        }
    }
    pthread_mutex_unlock(&g_pthread_mutex);
#endif

    /* CRITICAL: Add memory barrier after join to ensure visibility of all writes
     * done by the joined thread. This provides the happens-before relationship
     * guaranteed by Thread.join() in the Java Memory Model.
     */
    memory_barrier();
    
    return NATIVE_RETURN_VOID();
}

/* Thread.interrupt() - interrupt thread */
static JavaValue native_thread_interrupt(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* thread_obj = (JavaObject*)args[0].ref;
    
    if (thread_obj) {
        /* Find the JavaThread for this thread object */
        extern JavaThread* thread_find_by_object(JavaObject* thread_obj);
        JavaThread* target = thread_find_by_object(thread_obj);
        if (target) {
            thread_interrupt(target);
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* Thread.isInterrupted() - check if thread is interrupted */
static JavaValue native_thread_isInterrupted(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;  /* receiver flag is read, not the caller's */
    /* v20 (P1): instance isInterrupted() must read (not clear) the flag. */
    JavaObject* thread_obj = (JavaObject*)args[0].ref;
    if (!thread_obj) return NATIVE_RETURN_INT(0);
    extern JavaThread* thread_find_by_object(JavaObject* thread_obj);
    JavaThread* target = thread_find_by_object(thread_obj);
    if (!target) return NATIVE_RETURN_INT(0);
    memory_barrier();  /* full barrier: flag is written cross-thread */
    return NATIVE_RETURN_INT(target->interrupted ? 1 : 0);
}

/* Thread.interrupted() - static method to check and clear interrupted status */
static JavaValue native_thread_interrupted(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)args; (void)arg_count;
    if (!thread) return NATIVE_RETURN_INT(0);
    memory_barrier();
    int was = thread->interrupted ? 1 : 0;
    thread->interrupted = false;  /* static interrupted() CLEARS the flag */
    return NATIVE_RETURN_INT(was);
}

/* v20 (P1): Thread.sleep(long, int) */
static JavaValue native_thread_sleep_2(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)arg_count;
    jlong millis = args[0].j;
    jint nanos = args[1].i;
    if (millis < 0 || nanos < 0 || nanos > 999999) {
        native_throw_iae(jvm, thread, "timeout value is negative");
        return NATIVE_RETURN_VOID();
    }
    jlong total = millis + (nanos + 999999) / 1000000;
    JavaValue packed[1];
    packed[0].j = total;
    return native_thread_sleep(jvm, thread, packed, 1);
}

/* v20 (P1): Thread.join(long) and join(long, int) — poll with deadline. */
static JavaValue native_thread_join_millis(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)arg_count;
    JavaObject* thread_obj = (JavaObject*)args[0].ref;
    jlong millis = args[1].j;
    if (!thread_obj) return NATIVE_RETURN_VOID();
    if (millis < 0) {
        native_throw_iae(jvm, thread, "timeout value is negative");
        return NATIVE_RETURN_VOID();
    }
    extern JavaThread* thread_find_by_object(JavaObject* thread_obj);
    JavaThread* target = thread_find_by_object(thread_obj);
    if (!target) return NATIVE_RETURN_VOID();
    if (!target->is_alive) return NATIVE_RETURN_VOID();

    /* Poll with a millisecond countdown (2ms granularity keeps cooperative
     * scheduling alive while waiting). millis == 0 means wait forever. */
    jlong remaining = millis;
    while (target->is_alive) {
        if (millis > 0 && remaining <= 0) break;
        thread_yield(jvm);
#ifndef _WIN32
        usleep(2000);
#else
        Sleep(2);
#endif
        if (millis > 0) remaining -= 2;
    }
    return NATIVE_RETURN_VOID();
}

static JavaValue native_thread_join_millis_nanos(JVM* jvm, JavaThread* thread,
                                                 JavaValue* args, int arg_count) {
    (void)arg_count;
    /* join(long, int): delegate to join(long), nanos rounded up to 1ms. */
    JavaValue a2[2];
    a2[0] = args[0];
    a2[1].j = args[1].j + (args[2].i > 0 ? 1 : 0);
    return native_thread_join_millis(jvm, thread, a2, 2);
}

/* Thread.setPriority(int) */
static JavaValue native_thread_setPriority(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* thread_obj = (JavaObject*)args[0].ref;
    jint priority = args[1].i;
    
    if (thread_obj) {
        /* ИСПРАВЛЕНО: Используем native_set_field_value */
        JavaValue priority_val = { .i = priority };
        native_set_field_value(thread_obj, "priority", priority_val);
    }
    
    return NATIVE_RETURN_VOID();
}

/* Thread.getPriority() */
static JavaValue native_thread_getPriority(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* thread_obj = (JavaObject*)args[0].ref;
    
    if (!thread_obj) {
        return NATIVE_RETURN_INT(5);  /* NORM_PRIORITY */
    }
    
    jint priority = native_get_field_value(thread_obj, "priority").i;
    
    /* Return NORM_PRIORITY (5) if field is not set (0) */
    if (priority == 0) {
        priority = 5;
    }
    
    return NATIVE_RETURN_INT(priority);
}

/* Thread.setName(String) */
static JavaValue native_thread_setName(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    /* Just ignore - name is stored in Java field */
    return NATIVE_RETURN_VOID();
}

/* Thread.getName() */
static JavaValue native_thread_getName(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* thread_obj = (JavaObject*)args[0].ref;
    
    if (!thread_obj) {
        return NATIVE_RETURN_OBJECT(jvm_new_string(jvm, "Thread-0"));
    }
    
    /* ИСПРАВЛЕНО: Используем native_get_field_value */
    JavaString* name = (JavaString*)native_get_field_value(thread_obj, "name").ref;
    if (name) {
        return NATIVE_RETURN_OBJECT(name);
    }
    
    return NATIVE_RETURN_OBJECT(jvm_new_string(jvm, "Thread-?"));
}

/* Thread.activeCount() - static */
static JavaValue native_thread_activeCount(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    return NATIVE_RETURN_INT(1);  /* Just main thread */
}

/* Thread.holdsLock(Object) - static */
static JavaValue native_thread_holdsLock(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    return NATIVE_RETURN_INT(0);
}

/* ==== v20 (P1 batch A): System/Runtime/Vector/Stack natives ==== */

/* System.exit(int) — terminate the VM like MIDlet.notifyDestroyed would.
 * Java-level status is recorded; the interpreter loop stops on
 * jvm->running == false. */
static JavaValue native_system_exit(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    jint code = args[0].i;
    LOG_SAFE("[JVM] System.exit(%d) called — terminating VM\n", code);
    jvm->exit_code = code;
    jvm->exiting = true;
    jvm->running = false;
    return NATIVE_RETURN_VOID();
}

/* Runtime.exit(int) alias */
static JavaValue native_runtime_exit(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    return native_system_exit(jvm, thread, args, arg_count);
}

/* System.identityHashCode(Object) — identity hash stored in the header. */
static JavaValue native_system_identityHashCode(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    if (!obj) return NATIVE_RETURN_INT(0);
    return NATIVE_RETURN_INT(obj->header.hashcode);
}

/* Vector.firstElement / lastElement per spec: NoSuchElementException. */
static JavaValue native_vector_firstElement_spec(JVM* jvm, JavaThread* thread,
                                                 JavaValue* args, int arg_count) {
    (void)arg_count;
    JavaObject* vec = (JavaObject*)args[0].ref;
    if (!vec) { native_throw_npe(jvm, thread); return NATIVE_RETURN_NULL(); }
    JavaArray* arr = (JavaArray*)native_get_field_value(vec, "elementData").ref;
    int count = native_get_field_value(vec, "elementCount").i;
    if (!arr || count == 0) {
        jvm_throw_by_name(jvm, "java/util/NoSuchElementException", "Vector empty");
        thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_NULL();
    }
    return NATIVE_RETURN_OBJECT(array_get_ref(arr, 0));
}

static JavaValue native_vector_lastElement_spec(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)arg_count;
    JavaObject* vec = (JavaObject*)args[0].ref;
    if (!vec) { native_throw_npe(jvm, thread); return NATIVE_RETURN_NULL(); }
    JavaArray* arr = (JavaArray*)native_get_field_value(vec, "elementData").ref;
    int count = native_get_field_value(vec, "elementCount").i;
    if (!arr || count == 0) {
        jvm_throw_by_name(jvm, "java/util/NoSuchElementException", "Vector empty");
        thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_NULL();
    }
    return NATIVE_RETURN_OBJECT(array_get_ref(arr, count - 1));
}

/* Vector.capacity() — current capacity of elementData. */
static JavaValue native_vector_capacity(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* vec = (JavaObject*)args[0].ref;
    if (!vec) return NATIVE_RETURN_INT(0);
    JavaArray* arr = (JavaArray*)native_get_field_value(vec, "elementData").ref;
    return NATIVE_RETURN_INT(arr ? (jint)arr->length : 0);
}

/* String.valueOf helper used by Vector/Hashtable.toString: calls
 * toString() through the virtual dispatch, "null" for null.
 *
 * v36.04 RESOLUTION ORDER (fixes the dormant half of the v36.03 fix):
 * the OLD first walk probed the native registry up the WHOLE chain BEFORE
 * looking at bytecode, so any receiver whose chain reached java/lang/Object
 * got the registered Object.toString() (@-form) and the game class's real
 * bytecode toString() never executed (the v36.03 "bytecode first" walk sat
 * below it as dead code; TestVector even encoded the @-form as expected).
 * Java semantics: the MOST-DERIVED DECLARATION wins; whether it runs as
 * bytecode or as a registered native depends on the class that DECLARES it:
 *   1. walk the chain for the first class DECLARING toString;
 *   2. if that declaration has bytecode  -> nested execute_method;
 *   3. else (native-backed stub: Integer/Boolean/Throwable/Vector/...)
 *      -> registry lookup AT that declaring class;
 *   4. no declaration at all            -> registry up-chain (Object @-form);
 *   5. still nothing                    -> "null". */
static JavaString* util_to_string_of(JVM* jvm, JavaThread* thread, JavaObject* obj) {
    if (!obj) return jvm_new_string(jvm, "null");

    /* 1: most-derived declaration of toString (method slot present). */
    JavaClass* c = obj->header.clazz;
    JavaMethod* m = NULL;
    while (c) {
        for (int i = 0; i < c->methods_count; i++) {
            JavaMethod* mm = &c->methods[i];
            if (mm->name && strcmp(mm->name, "toString") == 0 &&
                mm->descriptor && strcmp(mm->descriptor, "()Ljava/lang/String;") == 0) {
                m = mm;
                break;
            }
        }
        if (m) break;
        c = c->super_class;
    }

    /* 2: bytecode declaration -> nested execution (polymorphism). */
    if (m && m->code.code && m->code.code_length > 0) {
        JavaValue args[1] = { { .ref = obj } };
        JavaValue result = { .raw = 0 };
        /* v36.04 NESTED-RETURN-CONTAINED: execute_method_nested rewinds the
         * *return deposit that op_areturn leaves on the OUTER bytecode frame
         * (there is no real bytecode caller here) — the un-rewound deposit
         * leaked 1 operand slot per nested call and eventually wrote past
         * the frame (session-48 bugs (a)+(b), reproduced on host). */
        extern int execute_method_nested(JVM* jvm, JavaThread* thread, JavaMethod* method,
                                         JavaValue* args, JavaValue* result);
        /* v36.04 NATIVE-ARGS-ROOTED: nested execution runs INSIDE this
         * native (native_arg_depth already counts us), but the receiver
         * lives in a C local — publish it so a GC inside the callee's
         * allocations cannot sweep it (same contract as native_call). */
        int naf_slot = -1;
        if (thread && thread->native_arg_depth < NATIVE_ARG_FRAMES_MAX) {
            naf_slot = thread->native_arg_depth++;
            thread->native_arg_frames[naf_slot].args = args;
            thread->native_arg_frames[naf_slot].count = 1;
        }
        int rc = execute_method_nested(jvm, thread, m, args, &result);
        if (naf_slot >= 0) {
            thread->native_arg_frames[naf_slot].args = NULL;
            thread->native_arg_frames[naf_slot].count = 0;
            thread->native_arg_depth--;
        }
        if (rc == 0 && result.ref) {
            return (JavaString*)result.ref;
        }
        /* bytecode toString threw — fall through to the native fallbacks
         * below rather than emit a wrong string */
        m = NULL;
    }

    /* 3/4: registry — at the declaring class (stub with no bytecode), else
     * up the chain (Object @-form). */
    for (c = m ? m->clazz : obj->header.clazz; c && c->class_name; c = c->super_class) {
        NativeMethod nm = native_find(jvm, c->class_name, "toString", "()Ljava/lang/String;");
        if (nm) {
            JavaValue targs[1];
            targs[0].ref = obj;
            /* v36.03 NATIVE-ARGS-ROOTED: this call bypasses native_call,
             * so root targs explicitly — the invoked native may allocate
             * (and thus trigger a GC) before it finishes. */
            int naf_slot = -1;
            if (thread && thread->native_arg_depth < NATIVE_ARG_FRAMES_MAX) {
                naf_slot = thread->native_arg_depth++;
                thread->native_arg_frames[naf_slot].args = targs;
                thread->native_arg_frames[naf_slot].count = 1;
            }
            JavaValue tresult = nm(jvm, thread, targs, 1);
            if (naf_slot >= 0) {
                thread->native_arg_frames[naf_slot].args = NULL;
                thread->native_arg_frames[naf_slot].count = 0;
                thread->native_arg_depth--;
            }
            if (tresult.ref) {
                return (JavaString*)tresult.ref;
            }
            /* native declined (e.g. threw) — fall through to the final
             * "null" fallback rather than emit a wrong string */
            break;
        }
    }

    return jvm_new_string(jvm, "null");
}

/* Vector.toString() — "[e0, e1, ...]" (java.util.AbstractCollection format). */
static JavaValue native_vector_toString(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)arg_count;
    JavaObject* vec = (JavaObject*)args[0].ref;
    if (!vec) return NATIVE_RETURN_OBJECT(jvm_new_string(jvm, "null"));
    JavaArray* arr = (JavaArray*)native_get_field_value(vec, "elementData").ref;
    int count = native_get_field_value(vec, "elementCount").i;
    if (!arr || count == 0) return NATIVE_RETURN_OBJECT(jvm_new_string(jvm, "[]"));

    /* Join per-element toStrings.
     * v36.03 hardening: (1) every malloc/realloc checked — a NULL buf used
     * to be dereferenced unconditionally on the next buf[len++] (OOM path);
     * (2) a failed string_utf8 (ring realloc OOM) now contributes the text
     * "null" instead of a bare ", " separator; (3) on growth failure the
     * loop truncates instead of writing past the buffer. The receiver and
     * elementData are protected from mid-loop GC by NATIVE-ARGS-ROOTED
     * (native_call publishes args[0] to the mark phase). */
    size_t cap = 256;
    char* buf = (char*)malloc(cap);
    if (!buf) return NATIVE_RETURN_OBJECT(jvm_new_string(jvm, "[...]"));
    size_t len = 0;
    int truncated = 0;
    buf[len++] = '[';
    for (int i = 0; i < count && !truncated; i++) {
        JavaObject* e = array_get_ref(arr, i);
        JavaString* s = util_to_string_of(jvm, thread, e);
        const char* u = s ? string_utf8(jvm, s) : "null";
        const char* null_text = "null";
        size_t ul = u ? strlen(u) : 4;
        const char* src = u ? u : null_text;
        while (len + ul + 3 > cap) {
            size_t ncap = cap * 2;
            char* nb = (char*)realloc(buf, ncap);
            if (!nb) { truncated = 1; break; }   /* keep old buf, truncate */
            cap = ncap;
            buf = nb;
        }
        if (truncated) break;
        if (i) buf[len++] = ',';
        if (i) buf[len++] = ' ';
        memcpy(buf + len, src, ul);
        len += ul;
    }
    /* Closing bracket is guaranteed to fit: the growth loop reserved
     * len+ul+3 <= cap before every append (2 separators + ']' + NUL). */
    if (len + 2 <= cap) {
        buf[len++] = ']';
        buf[len] = '\0';
    } else {
        buf[cap - 1] = '\0';  /* defensive: cannot happen with the reserve */
    }
    JavaString* out = jvm_new_string(jvm, buf);
    free(buf);
    return NATIVE_RETURN_OBJECT(out);
}

/* ---- java.util.Stack (v20 P1): real LIFO over the inherited Vector fields ---- */
static int stack_grow(JVM* jvm, JavaObject* stack, JavaArray* arr, int count) {
    int new_cap = arr ? arr->length * 2 : 10;
    if (new_cap < 10) new_cap = 10;
    JavaArray* na = jvm_new_array(jvm, DESC_OBJECT, new_cap, NULL);
    if (!na) return -1;
    if (arr && count > 0) {
        memcpy(array_data(na), array_data(arr), (size_t)count * sizeof(JavaObject*));
    }
    JavaValue v = { .ref = na };
    native_set_field_value(stack, "elementData", v);
    return 0;
}

static JavaValue native_stack_push(JVM* jvm, JavaThread* thread,
                                   JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* stack = (JavaObject*)args[0].ref;
    JavaObject* item = (JavaObject*)args[1].ref;
    if (!stack) return NATIVE_RETURN_OBJECT(item);
    JavaArray* arr = (JavaArray*)native_get_field_value(stack, "elementData").ref;
    int count = native_get_field_value(stack, "elementCount").i;
    if (!arr || count >= (int)arr->length) {
        if (stack_grow(jvm, stack, arr, count) != 0) return NATIVE_RETURN_OBJECT(item);
        arr = (JavaArray*)native_get_field_value(stack, "elementData").ref;
    }
    if (arr) array_set_ref(arr, count, item);
    JavaValue cv = { .i = count + 1 };
    native_set_field_value(stack, "elementCount", cv);
    return NATIVE_RETURN_OBJECT(item); /* push returns the item */
}

static JavaValue native_stack_pop(JVM* jvm, JavaThread* thread,
                                  JavaValue* args, int arg_count) {
    (void)arg_count;
    JavaObject* stack = (JavaObject*)args[0].ref;
    if (!stack) { native_throw_npe(jvm, thread); return NATIVE_RETURN_NULL(); }
    JavaArray* arr = (JavaArray*)native_get_field_value(stack, "elementData").ref;
    int count = native_get_field_value(stack, "elementCount").i;
    if (!arr || count == 0) {
        jvm_throw_by_name(jvm, "java/util/EmptyStackException", NULL);
        thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_NULL();
    }
    JavaObject* top = array_get_ref(arr, count - 1);
    JavaValue cv = { .i = count - 1 };
    native_set_field_value(stack, "elementCount", cv);
    return NATIVE_RETURN_OBJECT(top);
}

static JavaValue native_stack_peek(JVM* jvm, JavaThread* thread,
                                   JavaValue* args, int arg_count) {
    (void)arg_count;
    JavaObject* stack = (JavaObject*)args[0].ref;
    if (!stack) { native_throw_npe(jvm, thread); return NATIVE_RETURN_NULL(); }
    JavaArray* arr = (JavaArray*)native_get_field_value(stack, "elementData").ref;
    int count = native_get_field_value(stack, "elementCount").i;
    if (!arr || count == 0) {
        jvm_throw_by_name(jvm, "java/util/EmptyStackException", NULL);
        thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_NULL();
    }
    return NATIVE_RETURN_OBJECT(array_get_ref(arr, count - 1));
}

static JavaValue native_stack_empty(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* stack = (JavaObject*)args[0].ref;
    int count = stack ? native_get_field_value(stack, "elementCount").i : 0;
    return NATIVE_RETURN_INT(count == 0 ? 1 : 0);
}

static JavaValue native_stack_search(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* stack = (JavaObject*)args[0].ref;
    JavaObject* item = (JavaObject*)args[1].ref;
    if (!stack) return NATIVE_RETURN_INT(-1);
    JavaArray* arr = (JavaArray*)native_get_field_value(stack, "elementData").ref;
    int count = native_get_field_value(stack, "elementCount").i;
    if (!arr) return NATIVE_RETURN_INT(-1);
    /* 1-based distance from the TOP; identity comparison like Vector.indexOf(o) */
    for (int i = count - 1, dist = 1; i >= 0; i--, dist++) {
        if (array_get_ref(arr, i) == item) return NATIVE_RETURN_INT(dist);
    }
    return NATIVE_RETURN_INT(-1);
}

void init_java_util_stack(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"java/util/Stack", "push", "(Ljava/lang/Object;)Ljava/lang/Object;", native_stack_push},
        {"java/util/Stack", "pop", "()Ljava/lang/Object;", native_stack_pop},
        {"java/util/Stack", "peek", "()Ljava/lang/Object;", native_stack_peek},
        {"java/util/Stack", "empty", "()Z", native_stack_empty},
        {"java/util/Stack", "search", "(Ljava/lang/Object;)I", native_stack_search},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
}

/* ==== v20 (P1 batch B): String/PrintStream/Character/TimeZone/wrappers ==== */

/* String.lastIndexOf(String[, int]) — UTF-16 semantics */
static jint string_last_index_of_str(JavaString* str, JavaString* sub, jint from) {
    if (!str || !sub) return -1;
    const jchar* s = string_chars(str);
    jsize slen = string_length(str);
    const jchar* t = string_chars(sub);
    jsize tlen = string_length(sub);
    if (tlen == 0) return from < 0 ? 0 : (from > slen ? slen : from);
    if (tlen > slen) return -1;
    jint max = slen - tlen;
    if (from >= slen) from = max;
    if (from < 0) from = 0;
    for (jint i = from; i >= 0; i--) {
        if (i > max) continue;
        jint j = 0;
        while (j < tlen && s[i + j] == t[j]) j++;
        if (j == tlen) return i;
    }
    return -1;
}

static JavaValue native_string_lastIndexOf_string(JVM* jvm, JavaThread* thread,
                                                  JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    return NATIVE_RETURN_INT(string_last_index_of_str((JavaString*)args[0].ref,
                                                      (JavaString*)args[1].ref,
                                                      string_length((JavaString*)args[0].ref)));
}

static JavaValue native_string_lastIndexOf_string_from(JVM* jvm, JavaThread* thread,
                                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    return NATIVE_RETURN_INT(string_last_index_of_str((JavaString*)args[0].ref,
                                                      (JavaString*)args[1].ref,
                                                      args[2].i));
}

/* ---- Unicode letter ranges (practical subset for games) ---- */
static int char_is_unicode_letter(jint c) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return 1;
    if (c >= 0x00C0 && c <= 0x024F) return 1;             /* Latin-1 letters + Latin Ext A/B */
    if (c == 0x00DF || c == 0x00F1) return 1;
    if (c >= 0x0370 && c <= 0x03FF) return 1;             /* Greek */
    if (c >= 0x0400 && c <= 0x04FF) return 1;             /* Cyrillic */
    if (c >= 0x0530 && c <= 0x058F) return 1;             /* Armenian */
    if (c >= 0x0590 && c <= 0x05EA) return 1;             /* Hebrew */
    if (c >= 0x0600 && c <= 0x06FF) return 1;             /* Arabic */
    if (c >= 0x3040 && c <= 0x30FF) return 1;             /* Hiragana/Katakana */
    if (c >= 0x3400 && c <= 0x4DBF) return 1;             /* CJK Ext A */
    if (c >= 0x4E00 && c <= 0x9FFF) return 1;             /* CJK */
    if (c >= 0xAC00 && c <= 0xD7AF) return 1;             /* Hangul */
    if (c >= 0xF900 && c <= 0xFAFF) return 1;             /* CJK Compat */
    return 0;
}

/* ---- PrintStream formatting family ---- */
static JavaValue native_printstream_println_long(JVM* jvm, JavaThread* thread,
                                                 JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    fprintf(stderr, "[SYSOUT] %lld\n", (long long)args[1].j);
    return NATIVE_RETURN_VOID();
}

static JavaValue native_printstream_println_char(JVM* jvm, JavaThread* thread,
                                                 JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jchar c = (jchar)args[1].i;
    fprintf(stderr, "[SYSOUT] %c\n", (int)(c < 128 ? c : '?'));
    return NATIVE_RETURN_VOID();
}

static JavaValue native_printstream_println_float(JVM* jvm, JavaThread* thread,
                                                  JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    fprintf(stderr, "[SYSOUT] %g\n", (double)args[1].f);
    return NATIVE_RETURN_VOID();
}

static JavaValue native_printstream_println_double(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    fprintf(stderr, "[SYSOUT] %g\n", args[1].d);
    return NATIVE_RETURN_VOID();
}

static JavaValue native_printstream_println_object(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaString* s = util_to_string_of(jvm, thread, (JavaObject*)args[1].ref);
    const char* u = s ? string_utf8(jvm, s) : "null";
    fprintf(stderr, "[SYSOUT] %s\n", u ? u : "null");
    return NATIVE_RETURN_VOID();
}

static JavaValue native_printstream_print_long(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    fprintf(stderr, "%lld", (long long)args[1].j);
    return NATIVE_RETURN_VOID();
}

static JavaValue native_printstream_print_char(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jchar c = (jchar)args[1].i;
    fprintf(stderr, "%c", (int)(c < 128 ? c : '?'));
    return NATIVE_RETURN_VOID();
}

static JavaValue native_printstream_print_float(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    fprintf(stderr, "%g", (double)args[1].f);
    return NATIVE_RETURN_VOID();
}

static JavaValue native_printstream_print_double(JVM* jvm, JavaThread* thread,
                                                 JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    fprintf(stderr, "%g", args[1].d);
    return NATIVE_RETURN_VOID();
}

static JavaValue native_printstream_print_object(JVM* jvm, JavaThread* thread,
                                                 JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaString* s = util_to_string_of(jvm, thread, (JavaObject*)args[1].ref);
    const char* u = s ? string_utf8(jvm, s) : "null";
    fprintf(stderr, "%s", u ? u : "null");
    return NATIVE_RETURN_VOID();
}

/* ---- TimeZone instance natives (system default zone) ---- */

/* v34.87 FIX (user build report, devkitA64):
 *   error: 'timezone' undeclared (first use in this function);
 *   did you mean '_timezone'?
 * glibc exports the SVID global `timezone` (seconds west of UTC) directly;
 * devkitPro's newlib exports only the underscore-prefixed `_timezone`
 * (newlib/libc/include/time.h: "extern __IMPORT long _timezone;", declared
 * under __SVID_VISIBLE/__XSI_VISIBLE - both enabled by our -D_GNU_SOURCE).
 * Semantics are identical: seconds west of UTC, filled in by tzset() from
 * the TZ environment variable. Extra wrinkle: unlike glibc, newlib's
 * localtime() does NOT invoke tzset() implicitly, so the Switch path primes
 * the global explicitly (TZ is unset under libnx -> UTC, offset 0 - the
 * sane default a J2ME device falls back to when no zone is configured).
 * Non-Switch behaviour is bit-identical to v34.86 (same bare `timezone`). */
#if defined(__SWITCH__) && !defined(__GLIBC__)
/* Real devkitA64 (newlib): _timezone, and localtime() does not implicitly
 * run tzset(). The host switchui-verify build also defines __SWITCH__ but
 * compiles against GLIBC, which has no _timezone - hence the __GLIBC__
 * exclusion (latent breakage since v34.87: make switchui-verify failed). */
static long nojme_tz_west_seconds(void) {
    tzset();
    return (long)_timezone;
}
#else
static long nojme_tz_west_seconds(void) {
    return (long)timezone;  /* glibc/MinGW: SVID global, tzset-backed */
}
#endif

static JavaValue native_timezone_getID(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    time_t now = time(NULL);
    struct tm* t = localtime(&now);
    /* MinGW/MSVCRT struct tm has no tm_zone member - get the zone name
     * portably via strftime("%Z") (glibc returns the same string). */
    char zone[64];
    const char* id = "UTC";
    if (t && strftime(zone, sizeof(zone), "%Z", t) > 0 && zone[0]) {
        id = zone;
    }
    return NATIVE_RETURN_OBJECT(jvm_new_string(jvm, id));
}

static JavaValue native_timezone_getRawOffset(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    return NATIVE_RETURN_INT((jint)(-nojme_tz_west_seconds() * 1000));  /* seconds west -> ms east */
}

static JavaValue native_timezone_getOffset7(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    /* legacy getOffset(era,year,month,day,dayOfWeek,millis) */
    time_t now = time(NULL);
    struct tm* t = localtime(&now);
    int base = (jint)(-nojme_tz_west_seconds() * 1000);
    if (t && t->tm_isdst > 0) base += 3600000;
    return NATIVE_RETURN_INT(base);
}

static JavaValue native_timezone_useDaylightTime(JVM* jvm, JavaThread* thread,
                                                 JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    /* Practical approximation: report DST when the *current* local time has
     * it (full historical tz databases are out of scope). */
    time_t now = time(NULL);
    struct tm* t = localtime(&now);
    return NATIVE_RETURN_INT(t && t->tm_isdst > 0 ? 1 : 0);
}

/* ---- wrapper extras ---- */
static JavaValue native_boolean_toString(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* b = (JavaObject*)args[0].ref;
    int v = b ? native_get_field_value(b, "value").z : 0;
    return NATIVE_RETURN_OBJECT(jvm_new_string(jvm, v ? "true" : "false"));
}

static JavaValue native_integer_toString_instance(JVM* jvm, JavaThread* thread,
                                                  JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* o = (JavaObject*)args[0].ref;
    int v = o ? native_get_field_value(o, "value").i : 0;
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", v);
    return NATIVE_RETURN_OBJECT(jvm_new_string(jvm, buf));
}

static JavaValue native_byte_byteValue(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* o = (JavaObject*)args[0].ref;
    return NATIVE_RETURN_INT(o ? (signed char)native_get_field_value(o, "value").i : 0);
}

static JavaValue native_short_shortValue(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* o = (JavaObject*)args[0].ref;
    return NATIVE_RETURN_INT(o ? (short)native_get_field_value(o, "value").i : 0);
}

static JavaValue native_character_charValue(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* o = (JavaObject*)args[0].ref;
    return NATIVE_RETURN_INT(o ? native_get_field_value(o, "value").i : 0);
}

void init_java_lang_thread(JVM* jvm) {
    /* v35.12: drop the previous session's thread registries BEFORE any new
     * thread can register — jvm_destroy already reset them after its idle
     * barrier, but an aborted teardown (busy-thread leak path) leaves the
     * dangling entries in place; starting the session clean guarantees the
     * td dump / tid probes never walk freed JavaThread structs. */
    native_threads_registry_reset();
    /* Initialize condition variables for join (POSIX only) */
#ifndef _WIN32
    for (int i = 0; i < MAX_PTHREADS; i++) {
        pthread_cond_init(&g_pthread_cond[i], NULL);
        g_pthread_running[i] = false;
    }
#else
    /* Windows: initialize thread system */
    win_thread_init();
#endif
    
    NativeMethodEntry methods[] = {
        {"java/lang/Thread", "currentThread", "()Ljava/lang/Thread;", native_thread_currentThread},
        {"java/lang/Thread", "sleep", "(J)V", native_thread_sleep},
        {"java/lang/Thread", "sleep", "(JI)V", native_thread_sleep_2},
        {"java/lang/Thread", "yield", "()V", native_thread_yield},
        {"java/lang/Thread", "start", "()V", native_thread_start},
        {"java/lang/Thread", "join", "()V", native_thread_join},
        {"java/lang/Thread", "join", "(J)V", native_thread_join_millis},
        {"java/lang/Thread", "join", "(JI)V", native_thread_join_millis_nanos},
        {"java/lang/Thread", "close", "()V", native_thread_close},
        {"java/lang/Thread", "isAlive", "()Z", native_thread_isAlive},
        {"java/lang/Thread", "interrupt", "()V", native_thread_interrupt},
        {"java/lang/Thread", "isInterrupted", "()Z", native_thread_isInterrupted},
        {"java/lang/Thread", "interrupted", "()Z", native_thread_interrupted},
        {"java/lang/Thread", "setPriority", "(I)V", native_thread_setPriority},
        {"java/lang/Thread", "getPriority", "()I", native_thread_getPriority},
        {"java/lang/Thread", "setName", "(Ljava/lang/String;)V", native_thread_setName},
        {"java/lang/Thread", "getName", "()Ljava/lang/String;", native_thread_getName},
        {"java/lang/Thread", "activeCount", "()I", native_thread_activeCount},
        {"java/lang/Thread", "holdsLock", "(Ljava/lang/Object;)Z", native_thread_holdsLock},
        {"java/lang/Thread", "<init>", "()V", native_thread_init},
        {"java/lang/Thread", "<init>", "(Ljava/lang/Runnable;)V", native_thread_init_runnable},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
}

/*
 * javax.microedition.midlet.MIDlet native methods
 * 
 * MIDlet lifecycle management:
 * - notifyDestroyed(): Called by MIDlet to tell AMS it wants to be destroyed
 * - notifyPaused(): Called by MIDlet to tell AMS it wants to be paused
 * These methods are protected in MIDlet class and called from startApp/pauseApp/destroyApp
 */

/* Global MIDlet state */
static bool g_midlet_destroyed = false;
static bool g_midlet_paused = false;

/* FIX (audit T-1, v18): globally reachable MIDlet instance, set by
 * jvm_run_midlet() so that both notifyDestroyed() and external exit paths
 * (SDL_QUIT, frontend shutdown) can deliver destroyApp(boolean) to the
 * application. Per MIDP (JSR-118 §5.2), the AMS MUST call destroyApp before
 * the MIDlet is destroyed — games store all their finalization/save-progress
 * code there. Previously it was NEVER called and progress was lost on every
 * exit. */
JavaObject* g_midlet_instance = NULL;

/* Re-entrancy guard: destroyApp may itself trigger notifyDestroyed */
static int g_in_destroy_app = 0;

/* v36.31 [EXIT-FENCE-EXT]: fence-side probe — is the frame thread currently
 * inside a destroyApp Java window? The runner-side fence wait extends its
 * 2.5 s cap while this is set: a destroyApp writing RMS on slow SD storage
 * (100-800 ms per store measured in the field) can legitimately hold the
 * fence for seconds, and dropping the runners mid-destroyApp resurrects
 * the exact two-interpreters-vs-one-heap race v36.30 closed. */
int midp_destroy_app_in_flight(void) {
    return g_in_destroy_app;
}

void midlet_call_destroy_app(JVM* jvm, bool unconditional) {
    if (!jvm || !g_midlet_instance || g_in_destroy_app) return;

    /* v36.31 [EXIT-DOUBLE-DELIVERY]: a destroyed MIDlet never runs
     * destroyApp again. The notifyDestroyed path delivers destroyApp
     * BEFORE raising g_midlet_destroyed (reorder above), so this guard
     * only stops the SECOND delivery (pause-menu exit right after an
     * in-game exit, quit/stuck-recovery sites, error-screen exit after
     * notifyDestroyed). Executing save-state bytecode twice on the same
     * session is the "выход с ошибкой" crash fuel. */
    if (midlet_is_destroyed()) {
        LOG_SAFE("[MIDLET] destroyApp skipped (midlet already destroyed - double delivery)\n");
        return;
    }

    /* v35.10 FIX (exit deadlock): destroyApp executes BYTECODE. If the
     * frontend pause latch is armed here (exit chosen inside the MINUS
     * pause menu or the PLUS per-game overlay), the interpreter parks at
     * the first 64-insn poll and destroyApp never returns - while the
     * caller (the frame thread itself) blocks inside this very call.
     * Mutual deadlock: AsiaRally 3D trace v35.09 - STUCK stage=events
     * idle=12+ s, td m frozen at ci.b -> DataOutputStream.writeLong (its
     * last native before the park), menu unresponsive («при выходе не
     * работает управление»). Lift the pause for the duration:
     * jvm_frontend_pause_end() is idempotent, and every frontend caller
     * stops the VM right after destroyApp returns. */
    {
        extern volatile int g_frontend_pause_active;
        if (g_frontend_pause_active) {
            extern void jvm_frontend_pause_end(void);
            LOG_SAFE("[MIDLET] destroyApp under armed frontend pause - lifting it to break the exit deadlock (v35.10)\n");
            jvm_frontend_pause_end();
        }
    }

    JavaThread* thread = jvm_current_thread(jvm);
    if (!thread) return;

    /* Walk up the class hierarchy for destroyApp(Z)V — the MIDlet class
     * almost always overrides the abstract method */
    JavaMethod* destroy_app = NULL;
    JavaClass* clazz = g_midlet_instance->header.clazz;
    while (clazz && !destroy_app) {
        destroy_app = jvm_resolve_method(jvm, clazz, "destroyApp", "(Z)V");
        if (!destroy_app) {
            clazz = clazz->super_class;
        }
    }
    if (!destroy_app || destroy_app->is_native) return;

    g_in_destroy_app = 1;
    LOG_SAFE("[MIDLET] Calling destroyApp(%s)\n", unconditional ? "true" : "false");

    JavaValue args[2] = {
        { .ref = g_midlet_instance },
        { .i = unconditional ? 1 : 0 }
    };
    JavaValue result;
    int ret = execute_method(jvm, thread, destroy_app, args, &result);
    if (ret != 0) {
        LOG_SAFE("[MIDLET] destroyApp threw an exception - clearing and continuing shutdown\n");
        if (thread->pending_exception) jvm_exception_clear(jvm);
        thread->pending_exception = NULL;
    }
    g_in_destroy_app = 0;
}

/* Global manifest data for getAppProperty */
static char* g_manifest_data = NULL;
static size_t g_manifest_size = 0;

/* v36.24 [SESSION-MANIFEST-RESET] (field report: a damaged midlet poisons
 * the next launch). g_manifest_data is PROCESS-GLOBAL and used to survive
 * session boundaries: when a session's JAR has no META-INF/MANIFEST.MF (or
 * the jar fails to load at all), midlet_set_manifest() below was never
 * called for that session and getAppProperty() kept serving the PREVIOUS
 * game's properties (wrong levels, wrong highscore keys, wrong DRM
 * gates). Wipe it at every session boundary so a manifest-less jar
 * starts from a clean, empty property set. */
void midlet_manifest_reset(void) {
    if (g_manifest_data) {
        free(g_manifest_data);
        g_manifest_data = NULL;
    }
    g_manifest_size = 0;
}

/* Set manifest data for getAppProperty - called from main.c */
void midlet_set_manifest(const char* manifest_data, size_t size) {
    if (g_manifest_data) {
        free(g_manifest_data);
    }
    g_manifest_data = (char*)malloc(size + 1);
    if (g_manifest_data) {
        memcpy(g_manifest_data, manifest_data, size);
        g_manifest_data[size] = '\0';
        g_manifest_size = size;
    }
}

/* Append JAD data to manifest for getAppProperty - called from main.c */
void midlet_append_manifest(const char* data) {
    if (!data) return;
    
    size_t data_len = strlen(data);
    if (data_len == 0) return;
    
    if (!g_manifest_data) {
        midlet_set_manifest(data, data_len);
        return;
    }
    
    /* Append new data with newline separator */
    size_t new_size = g_manifest_size + 1 + data_len;  /* +1 for newline */
    char* new_manifest = (char*)malloc(new_size + 1);
    if (new_manifest) {
        memcpy(new_manifest, g_manifest_data, g_manifest_size);
        new_manifest[g_manifest_size] = '\n';
        memcpy(new_manifest + g_manifest_size + 1, data, data_len);
        new_manifest[new_size] = '\0';
        
        free(g_manifest_data);
        g_manifest_data = new_manifest;
        g_manifest_size = new_size;
        DEBUG_LOG("[MIDlet] Manifest appended, new size: %zu", g_manifest_size);
    }
}

/* Add a single property to manifest */
void midlet_add_property(const char* key, const char* value) {
    if (!key || !value) return;
    
    size_t key_len = strlen(key);
    size_t value_len = strlen(value);
    size_t line_len = key_len + 2 + value_len;  /* key: value */
    
    char* line = (char*)malloc(line_len + 1);
    if (!line) return;
    
    snprintf(line, line_len + 1, "%s: %s", key, value);
    midlet_append_manifest(line);
    free(line);
}

/* MIDlet.notifyDestroyed() - tell AMS the MIDlet wants to be destroyed */
static JavaValue native_midlet_notifyDestroyed(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)thread; (void)args; (void)arg_count;
    
    if (g_j2me_runtime_debug) fprintf(stderr, "[NATIVE] MIDlet.notifyDestroyed() called! Stack trace:\n");
    fflush(stderr);
    
    /* Print simple stack trace */
    if (thread && thread->current_frame) {
        JavaFrame* frame = thread->current_frame;
        int depth = 0;
        while (frame && depth < 10) {
            if (frame->method && frame->method->clazz && frame->method->clazz->class_name) {
                fprintf(stderr, "  [%d] %s.%s\n", depth, 
                        frame->method->clazz->class_name,
                        frame->method->name ? frame->method->name : "?");
            }
            frame = frame->prev;
            depth++;
        }
        fflush(stderr);
    }
    
    /* v36.31 [EXIT-DOUBLE-DELIVERY]: the flag goes DOWN only AFTER the
     * mandatory destroyApp delivery. Order matters: the frontend exit
     * sites (pause menu, quit, stuck-recovery) now guard on
     * midlet_is_destroyed() inside midlet_call_destroy_app — a midlet
     * that already exited itself (in-game EXIT command -> notifyDestroyed)
     * must NOT run destroyApp a second time from the pause-menu exit
     * (field class: "выход с ошибкой" — save-state code executed twice on
     * an already-torn state, the second run throwing mid-write on a
     * closed RMS/stream). Delivery is preserved: this call still runs
     * BEFORE the flag is raised.
     * FIX (audit T-1, v18): deliver destroyApp(true) BEFORE stopping the
     * VM — the caller is typically deep inside game code that wants to
     * save progress, but MIDP requires the AMS to drive destroyApp at
     * destruction time. */
    midlet_call_destroy_app(jvm, true);

    g_midlet_destroyed = true;
    g_midlet_paused = false;
    
    /* Stop the JVM main loop */
    if (jvm) {
        jvm->running = false;
    }
    
    return NATIVE_RETURN_VOID();
}

/* MIDlet.notifyPaused() - tell AMS the MIDlet wants to be paused */
static JavaValue native_midlet_notifyPaused(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    
    g_midlet_paused = true;
    
    return NATIVE_RETURN_VOID();
}

/* MIDlet.resumeApp() - called by AMS to resume a paused MIDlet 
 * This is protected, called by the system, not by the MIDlet itself */
static JavaValue native_midlet_resumeApp(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    
    g_midlet_paused = false;
    
    /* Find and call startApp on the MIDlet */
    JavaObject* midlet = (JavaObject*)args[0].ref;
    if (midlet && midlet->header.clazz) {
        JavaMethod* startApp = jvm_resolve_method(jvm, midlet->header.clazz, "startApp", "()V");
        if (startApp) {
            JavaValue result;
            execute_method(jvm, thread, startApp, args, &result);
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* Check if MIDlet is destroyed */
bool midlet_is_destroyed(void) {
    return g_midlet_destroyed;
}

/* v36.32 [DIRECT-DESTROY-GUARD]: a midlet can deliver destroyApp to ITSELF
 * (a game thread calling this.destroyApp(true) directly — Zuma X's
 * k.run() -> a() -> destroyApp, per the field NPE-chain). That is a pure
 * Java virtual call: our MIDlet natives never see it, so the v36.31
 * double-delivery flag stays DOWN. When the VM then finishes naturally
 * (last runnable thread gone -> threads.c stops the JVM), any LATER
 * system-side delivery — the stuck-recovery consumer, a pause-menu exit
 * racing the finished screen, quit/stuck-recovery sites — would re-run
 * save-state bytecode on the dead session: the exact "выход с ошибкой"
 * fuel (second save pass throwing on closed RMS/streams, teardown racing
 * the second destroyApp). threads.c raises the flag at the natural-death
 * moment; delivery while the VM was still alive stays untouched. */
void midlet_mark_destroyed(void) {
    if (!g_midlet_destroyed) {
        g_midlet_destroyed = true;
        LOG_SAFE("[MIDLET] midlet finished (VM threads exhausted) — closing destroyApp delivery (v36.32)\n");
    }
}

/* Check if MIDlet is paused */
bool midlet_is_paused(void) {
    return g_midlet_paused;
}

/* Reset MIDlet state (for new MIDlet load) */
void midlet_reset_state(void) {
    g_midlet_destroyed = false;
    g_midlet_paused = false;
}

/* MIDlet.getAppProperty(String key) - get property from manifest */
static JavaValue native_midlet_getAppProperty(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    
    JavaObject* key_str = (JavaObject*)args[1].ref;  /* args[0] is this, args[1] is key */
    {
        static int gapp_diag = 0;
        if (gapp_diag < 8) {
            gapp_diag++;
            fprintf(stderr, "[GAPP-DIAG] getAppProperty called, manifest=%s\n",
                    g_manifest_data ? "loaded" : "NULL");
        }
    }
    if (!key_str) {
        if (g_j2me_runtime_debug) fprintf(stderr, "[MIDlet.getAppProperty] key_str is NULL\n");
        fflush(stderr);
        return NATIVE_RETURN_OBJECT(NULL);
    }
    
    /* Get the key string */
    const char* key = string_utf8(jvm, (JavaString*)key_str);
    {
        static int gkey_diag = 0;
        if (gkey_diag < 400 && key) {
            gkey_diag++;
            fprintf(stderr, "[GAPP-KEY] '%s'\n", key);
        }
    }
    if (!key) {
        if (g_j2me_runtime_debug) fprintf(stderr, "[MIDlet.getAppProperty] key is NULL (string conversion failed)\n");
        fflush(stderr);
        return NATIVE_RETURN_OBJECT(NULL);
    }
    
    if (!g_manifest_data) {
        if (g_j2me_runtime_debug) fprintf(stderr, "[MIDlet.getAppProperty] WARNING: g_manifest_data is NULL!\n");
        fflush(stderr);
        return NATIVE_RETURN_OBJECT(NULL);
    }
    
    if (g_j2me_runtime_debug) fprintf(stderr, "[MIDlet.getAppProperty] '%s'\n", key);
    fflush(stderr);
    
    /* Search for key in manifest */
    char* result = NULL;
    char* manifest_copy = strdup(g_manifest_data);
    if (!manifest_copy) {
        return NATIVE_RETURN_OBJECT(NULL);
    }
    
    char* line = strtok(manifest_copy, "\r\n");
    while (line) {
        /* Skip leading whitespace */
        while (*line == ' ' || *line == '\t') line++;
        
        /* Check if line starts with the key followed by ':' */
        size_t key_len = strlen(key);
        if (strncmp(line, key, key_len) == 0 && line[key_len] == ':') {
            /* Found the key, extract the value */
            char* value = line + key_len + 1;
            
            /* Skip leading whitespace after colon */
            while (*value == ' ' || *value == '\t') value++;
            
            /* Handle line continuation (lines starting with space) */
            char* full_value = strdup(value);
            char* next_line = NULL;

            /* v34.5 FIX: MIDP spec (MIDletSuite#getAppProperty) requires the
             * AMS to return the value with leading AND trailing whitespace
             * removed. This manifest (JAMDAT Doom RPG) terminates EVERY value
             * with a space: "DoomRPG-Frames: 4 \r\n". Without trailing trim
             * the game got "4 " and Integer.parseInt("4 ") threw
             * NumberFormatException, silently swallowed by k.<init>'s
             * catch(RuntimeException) - so getAppProperty("DoomRPG-Map") was
             * never reached, k.c stayed null and the map loader NPE'd. */
            if (full_value) {
                size_t fv_len = strlen(full_value);
                while (fv_len > 0 && (full_value[fv_len - 1] == ' ' ||
                                      full_value[fv_len - 1] == '\t')) {
                    full_value[--fv_len] = '\0';
                }
            }
            
            /* Check for continuation lines */
            while ((next_line = strtok(NULL, "\r\n")) != NULL) {
                if (next_line[0] == ' ' || next_line[0] == '\t') {
                    /* Continuation line */
                    char* new_full_value = (char*)malloc(strlen(full_value) + strlen(next_line) + 1);
                    if (new_full_value) {
                        strcpy(new_full_value, full_value);
                        strcat(new_full_value, next_line);
                        free(full_value);
                        full_value = new_full_value;
                    }
                } else {
                    /* Not a continuation, put back and stop */
                    break;
                }
            }
            
            result = full_value;
            break;
        }
        
        line = strtok(NULL, "\r\n");
    }
    
    free(manifest_copy);
    
    /* FIX-19u: Glomo (glowingmobile.com / glomogames.com) distribution
     * properties. Portal-delivered builds carried these keys in the JAD;
     * bare-JAR repackages (like the SU-30 HeroCraft Rus build) ship no JAD
     * at all. With them missing, the GlomoRegistrator chain in startApp
     * crashes (GlomoUtil.JAD evaluates value.charAt(value.length()-1) on
     * "" -> StringIndexOutOfBoundsException) and the game never starts.
     *
     * GlomoDistributer reads keys as "dst_" + shortName where shortName
     * comes from the config CSV header
     * "id,chId,name,glink,glinkTtl,mglink,mglinkTtl,pflag,gflag,kszReg,
     *  kszBns,kszSub1..4,tailSepar,smsKeyVer".
     * Defaults apply ONLY when neither JAD nor manifest define the key. */
    if (!result) {
        static const struct { const char* key; const char* value; } glomo_defaults[] = {
            { "dst_id", "1" },
            { "dst_chId", "1" },
            { "dst_name", "SU-30" },
            { "dst_glink", "http://www.glomogames.com" },
            { "dst_glinkTtl", "Glomo Games" },
            { "dst_mglink", "http://www.glomogames.com" },
            { "dst_mglinkTtl", "More Games" },
            { "dst_pflag", "0" },
            { "dst_gflag", "0" },
            { "dst_kszReg", "16" },
            { "dst_kszBns", "16" },
            { "dst_kszSub1", "16" },
            { "dst_kszSub2", "16" },
            { "dst_kszSub3", "16" },
            { "dst_kszSub4", "16" },
            { "dst_tailSepar", ";" },
            { "dst_smsKeyVer", "1" },
            /* glomo_* family (GlomoConstants / config scheme) */
            { "glomo_cfgScheme_oneFile", "1" },
            { "glomo_cfgScheme_twoFiles", "0" },
            { "glomo_cfgFileName", "glomo.cfg" },
            { "glomo_cfgDistrFileName", "glomo_distr.cfg" },
            { "glomo_cfgPhonesFileName", "glomo_phones.cfg" },
            { "glomo_cfgSeparator", "," },
            { "glomo_FlagNOREG", "0" },
            { "glomo_FlagNOBONUS", "0" },
            { "autoreg_ru", "0" },
            { "game_id", "su30" },
            /* country list: c_<N>_code / c_<N>_name (Russia first) */
            { "c_0_code", "7" },
            { "c_0_name", "Russia" },
            { "c_1_code", "380" },
            { "c_1_name", "Ukraine" },
            { "c_2_code", "375" },
            { "c_2_name", "Belarus" },
        };
        for (size_t i = 0; i < sizeof(glomo_defaults) / sizeof(glomo_defaults[0]); i++) {
            if (strcmp(key, glomo_defaults[i].key) == 0) {
                static int glomo_default_hits = 0;
                if (glomo_default_hits < 10) {
                    glomo_default_hits++;
                    fprintf(stderr, "[GLOMO-DEFAULT] '%s' -> '%s'\n", key, glomo_defaults[i].value);
                }
                if (g_j2me_runtime_debug) {
                    fprintf(stderr, "[MIDlet.getAppProperty] '%s' = '%s' (glomo default)\n",
                            key, glomo_defaults[i].value);
                    fflush(stderr);
                }
                JavaObject* def_str = (JavaObject*)jvm_new_string(jvm, glomo_defaults[i].value);
                return NATIVE_RETURN_OBJECT(def_str);
            }
        }
    }
    
    if (result) {
        /* v34.5: final trailing-whitespace trim (covers values assembled
         * from continuation lines, which append raw next_line text). */
        {
            size_t r_len = strlen(result);
            while (r_len > 0 && (result[r_len - 1] == ' ' ||
                                 result[r_len - 1] == '\t')) {
                result[--r_len] = '\0';
            }
        }
        if (g_j2me_runtime_debug) fprintf(stderr, "[MIDlet.getAppProperty] '%s' = '%s'\n", key, result);
        fflush(stderr);
        JavaObject* result_str = (JavaObject*)jvm_new_string(jvm, result);
        free(result);
        return NATIVE_RETURN_OBJECT(result_str);
    }
    
    if (g_j2me_runtime_debug) fprintf(stderr, "[MIDlet.getAppProperty] '%s' = NULL\n", key);
    fflush(stderr);
    return NATIVE_RETURN_OBJECT(NULL);
}

void init_javax_microedition_midlet_MIDlet(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"javax/microedition/midlet/MIDlet", "notifyDestroyed", "()V", native_midlet_notifyDestroyed},
        {"javax/microedition/midlet/MIDlet", "notifyPaused", "()V", native_midlet_notifyPaused},
        {"javax/microedition/midlet/MIDlet", "resumeApp", "()V", native_midlet_resumeApp},
        {"javax/microedition/midlet/MIDlet", "getAppProperty", "(Ljava/lang/String;)Ljava/lang/String;", native_midlet_getAppProperty},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
}

/*
 * java.lang.Throwable native methods
 */

static JavaValue native_throwable_fillInStackTrace(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)arg_count;
    JavaObject* throwable = NATIVE_ARG_OBJECT(args, 0);
    
    if (!throwable) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_NULL();
    }
    
    /* Count stack frames */
    int frame_count = 0;
    JavaFrame* frame = thread ? thread->current_frame : NULL;
    while (frame) {
        frame_count++;
        frame = frame->prev;
    }
    
    /* Find or create StackTraceElement class */
    JavaClass* ste_class = jvm_load_class(jvm, "java/lang/StackTraceElement");
    if (!ste_class) {
        return NATIVE_RETURN_OBJECT(throwable);
    }
    
    /* Create StackTraceElement[] array */
    JavaArray* ste_array = heap_alloc_array(jvm, DESC_OBJECT, frame_count, ste_class);
    if (!ste_array) {
        return NATIVE_RETURN_OBJECT(throwable);
    }
    
    /* Fill in stack trace elements */
    frame = thread ? thread->current_frame : NULL;
    for (int i = 0; i < frame_count && frame; i++) {
        JavaObject* ste = heap_alloc_object(jvm, ste_class);
        if (!ste) break;
        
        /* Set fields: declaringClass, methodName, fileName, lineNumber */
        int field_idx = 0;
        
        /* declaringClass (String) */
        const char* class_name = frame->clazz ? frame->clazz->class_name : "Unknown";
        JavaString* class_str = jvm_new_string(jvm, class_name);
        if (ste_class->fields_count > field_idx && class_str) {
            ste->fields[field_idx].ref = (JavaObject*)class_str;
        }
        field_idx++;
        
        /* methodName (String) */
        const char* method_name = frame->method ? frame->method->name : "unknown";
        JavaString* method_str = jvm_new_string(jvm, method_name);
        if (ste_class->fields_count > field_idx && method_str) {
            ste->fields[field_idx].ref = (JavaObject*)method_str;
        }
        field_idx++;
        
        /* fileName (String) - we don't have this info, use null */
        if (ste_class->fields_count > field_idx) {
            ste->fields[field_idx].ref = NULL;
        }
        field_idx++;
        
        /* lineNumber (int) */
        if (ste_class->fields_count > field_idx) {
            /* Try to compute line number from PC */
            jint line_num = -1;
            if (frame->method && frame->method->code.code && frame->pc > 0) {
                /* Simple approximation: use PC as line number indicator */
                line_num = (jint)frame->pc;
            }
            ste->fields[field_idx].i = line_num;
        }
        
        /* Store in array */
        ((JavaObject**)array_data(ste_array))[i] = ste;
        
        frame = frame->prev;
    }
    
    /* Set the stackTrace field in Throwable */
    /* Find the stackTrace field */
    JavaClass* throwable_class = throwable->header.clazz;
    if (throwable_class) {
        for (int i = 0; i < throwable_class->fields_count; i++) {
            if (throwable_class->fields[i].name && 
                strcmp(throwable_class->fields[i].name, "stackTrace") == 0) {
                throwable->fields[i].ref = (JavaObject*)ste_array;
                break;
            }
        }
    }
    
    return NATIVE_RETURN_OBJECT(throwable);
}

/* Throwable.printStackTrace() - prints stack trace to stderr */
static JavaValue native_throwable_printStackTrace(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* throwable = NATIVE_ARG_OBJECT(args, 0);
    
    /* Rate limit exception printing */
    static int printStackTrace_count = 0;
    if (printStackTrace_count >= 10) {
        return NATIVE_RETURN_VOID();
    }
    printStackTrace_count++;
    if (printStackTrace_count == 10) {
        fprintf(stderr, "[EXCEPTION] Further printStackTrace() output suppressed\n");
    }
    
    if (!throwable) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Get class name */
    JavaClass* clazz = throwable->header.clazz;
    const char* class_name = clazz ? clazz->class_name : "Unknown";
    
    /* Get message */
    JavaString* msg = (JavaString*)native_get_field_value(throwable, "detailMessage").ref;
    
    /* Print header: ExceptionName: message */
    fprintf(stderr, "%s", class_name);
    if (msg && string_length(msg) > 0) {
        fprintf(stderr, ": %.*s", string_length(msg), (char*)string_chars(msg));
    }
    fprintf(stderr, "\n");
    
    /* Get stack trace array */
    JavaArray* ste_array = (JavaArray*)native_get_field_value(throwable, "stackTrace").ref;
    if (ste_array && object_is_array((JavaObject*)ste_array)) {
        int len = ste_array->length;
        JavaObject** elements = (JavaObject**)array_data(ste_array);
        
        for (int i = 0; i < len; i++) {
            JavaObject* ste = elements[i];
            if (!ste) continue;
            
            /* Get StackTraceElement fields */
            JavaString* decl_class = (JavaString*)native_get_field_value(ste, "declaringClass").ref;
            JavaString* method = (JavaString*)native_get_field_value(ste, "methodName").ref;
            JavaString* file = (JavaString*)native_get_field_value(ste, "fileName").ref;
            jint line = native_get_field_value(ste, "lineNumber").i;
            
            fprintf(stderr, "    at ");
            if (decl_class && string_length(decl_class) > 0) {
                fprintf(stderr, "%.*s", string_length(decl_class), (char*)string_chars(decl_class));
            }
            fprintf(stderr, ".");
            if (method && string_length(method) > 0) {
                fprintf(stderr, "%.*s", string_length(method), (char*)string_chars(method));
            }
            
            if (file && string_length(file) > 0) {
                if (line > 0) {
                    fprintf(stderr, "(%.*s:%d)\n", string_length(file), (char*)string_chars(file), line);
                } else {
                    fprintf(stderr, "(%.*s)\n", string_length(file), (char*)string_chars(file));
                }
            } else {
                fprintf(stderr, "(Unknown Source)\n");
            }
        }
    }
    
    fflush(stderr);
    return NATIVE_RETURN_VOID();
}

void init_java_lang_throwable(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"java/lang/Throwable", "fillInStackTrace", "()Ljava/lang/Throwable;", native_throwable_fillInStackTrace},
        {"java/lang/Throwable", "printStackTrace", "()V", native_throwable_printStackTrace},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
}

/*
 * java.util.Random native methods
 * 
 * Random uses a simple linear congruential generator:
 * seed = (seed * 0x5DEECE66DL + 0xBL) & ((1L << 48) - 1)
 * 
 * We store the seed in the first field of the Random object.
 */

/* Helper to get seed from Random object - ИСПРАВЛЕНО: используем native_get_field_value */
static jlong random_get_seed(JavaObject* obj) {
    if (!obj) return 0;
    return native_get_field_value(obj, "seed").j;
}

/* Helper to set seed in Random object - ИСПРАВЛЕНО: используем native_set_field_value */
static void random_set_seed(JavaObject* obj, jlong seed) {
    if (!obj) return;
    JavaValue val = { .j = seed };
    native_set_field_value(obj, "seed", val);
}

/* Random() - constructor with time-based seed */
static JavaValue native_random_init(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    
    /* Initialize seed with current time */
    jlong seed;
#ifdef _WIN32
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    seed = counter.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    seed = (jlong)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
    
    /* Apply the initial scramble: seed ^ 0x5DEECE66DL */
    seed ^= 0x5DEECE66DLL;
    
    random_set_seed(obj, seed);
    
    NATIVE_DEBUG("<init>: obj=%p, seed=%lld", (void*)obj, (long long)seed);
    
    return NATIVE_RETURN_VOID();
}

/* Random(long seed) - constructor with explicit seed */
static JavaValue native_random_init_seed(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jlong seed = args[1].j;
    
    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    
    /* Apply the initial scramble: (seed ^ 0x5DEECE66DL) & ((1L << 48) - 1).
     * v18 FIX: add the 48-bit mask required by the spec (critical for
     * negative seeds, whose sign-extended high bits must be cleared). */
    seed = (seed ^ 0x5DEECE66DLL) & ((1LL << 48) - 1);
    
    random_set_seed(obj, seed);
    
    NATIVE_DEBUG("<init>(seed): obj=%p, seed=%lld", (void*)obj, (long long)seed);
    
    return NATIVE_RETURN_VOID();
}

/* Random.next(int bits) - generate next random value */
static JavaValue native_random_next(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint bits = args[1].i;
    
    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_INT(0);
    }
    
    /* ИСПРАВЛЕНО: используем random_get_seed/random_set_seed вместо указателя */
    jlong seed = random_get_seed(obj);
    
    /* seed = (seed * 0x5DEECE66DL + 0xBL) & ((1L << 48) - 1) */
    seed = (seed * 0x5DEECE66DLL + 0xBL) & ((1LL << 48) - 1);
    random_set_seed(obj, seed);
    
    /* return (int)(seed >>> (48 - bits)) */
    jint result = (jint)(seed >> (48 - bits));
    
    return NATIVE_RETURN_INT(result);
}

/* Random.nextInt() - random integer */
static JavaValue native_random_nextInt(JVM* jvm, JavaThread* thread,
JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_INT(0);
    }
    
    /* ИСПРАВЛЕНО: используем random_get_seed/random_set_seed */
    jlong seed = random_get_seed(obj);
    
    /* Generate next random value (32 bits) */
    seed = (seed * 0x5DEECE66DLL + 0xBL) & ((1LL << 48) - 1);
    random_set_seed(obj, seed);
    
    jint result = (jint)(seed >> 16);
    
    NATIVE_DEBUG("nextInt(): result=%d, seed=%lld", result, (long long)seed);
    
    return NATIVE_RETURN_INT(result);
}

/* Random.nextInt(int bound) - random integer in [0, bound) */
static JavaValue native_random_nextIntBound(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint bound = args[1].i;
    
    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_INT(0);
    }
    
    if (bound <= 0) {
        native_throw_iae(jvm, thread, "bound must be positive");
        return NATIVE_RETURN_INT(0);
    }
    
    /* ИСПРАВЛЕНО: используем random_get_seed/random_set_seed */
    jlong seed = random_get_seed(obj);
    
    jint result;
    if ((bound & -bound) == bound) {
        /* bound is a power of 2 */
        seed = (seed * 0x5DEECE66DLL + 0xBL) & ((1LL << 48) - 1);
        result = (jint)((bound * (seed >> 17)) >> 31);
    } else {
        /* Reject samples that would produce bias */
        jint val;
        do {
            seed = (seed * 0x5DEECE66DLL + 0xBL) & ((1LL << 48) - 1);
            val = (jint)(seed >> 17);
            jint r = val % bound;
            /* Check for bias */
            while (val - r + (bound - 1) < 0) {
                seed = (seed * 0x5DEECE66DLL + 0xBL) & ((1LL << 48) - 1);
                val = (jint)(seed >> 17);
                r = val % bound;
            }
            result = r;
            break;  /* Simplified - just take the first value */
        } while (0);
    }
    
    random_set_seed(obj, seed);
    
    NATIVE_DEBUG("nextInt(%d): result=%d, seed=%lld", bound, result, (long long)seed);
    
    return NATIVE_RETURN_INT(result);
}

/* Random.nextLong() - random long */
static JavaValue native_random_nextLong(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_LONG(0);
    }
    
    /* ИСПРАВЛЕНО: используем random_get_seed/random_set_seed */
    jlong seed = random_get_seed(obj);
    
    /* Generate two 32-bit values and combine */
    seed = (seed * 0x5DEECE66DLL + 0xBL) & ((1LL << 48) - 1);
    jint high = (jint)(seed >> 16);
    seed = (seed * 0x5DEECE66DLL + 0xBL) & ((1LL << 48) - 1);
    jint low = (jint)(seed >> 16);
    random_set_seed(obj, seed);
    
    jlong result = ((jlong)high << 32) + (jlong)low;
    /* v18 FIX (Java spec): nextLong() = ((long)next(32) << 32) + next(32).
     * The second next(32) is a SIGNED int and sign-extends when added.
     * The old code OR'ed the zero-extended low word, producing results
     * 2^32 too large whenever the low int was negative (off-by-one in the
     * high word, e.g. Random(-987654321).nextLong). */
    
    return NATIVE_RETURN_LONG(result);
}

/* Random.nextFloat() - random float in [0, 1) */
static JavaValue native_random_nextFloat(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_FLOAT(0.0f);
    }
    
    /* ИСПРАВЛЕНО: используем random_get_seed/random_set_seed */
    jlong seed = random_get_seed(obj);
    seed = (seed * 0x5DEECE66DLL + 0xBL) & ((1LL << 48) - 1);
    random_set_seed(obj, seed);
    
    jfloat result = (jfloat)(seed >> 24) / (jfloat)((uint32_t)(1LL) <<  24);
    
    return NATIVE_RETURN_FLOAT(result);
}

/* Random.nextDouble() - random double in [0, 1) */
static JavaValue native_random_nextDouble(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_DOUBLE(0.0);
    }
    
    /* ИСПРАВЛЕНО: используем random_get_seed/random_set_seed */
    jlong seed = random_get_seed(obj);
    seed = (seed * 0x5DEECE66DLL + 0xBL) & ((1LL << 48) - 1);
    jint high = (jint)(seed >> 22);   /* next(26) */
    seed = (seed * 0x5DEECE66DLL + 0xBL) & ((1LL << 48) - 1);
    jint low = (jint)(seed >> 21);    /* next(27) */
    random_set_seed(obj, seed);
    
    /* v18 FIX (Java spec): nextDouble() =
     *   (((long)next(26) << 27) + next(27)) * (1.0 / (1L << 53))
     * The old code took 32-bit halves (seed >> 16) and combined them with
     * | and >> 5, which matches no JVM and returned wrong values. */
    jlong combined = (((jlong)high) << 27) + (jlong)low;
    jdouble result = (jdouble)combined / (jdouble)(1LL << 53);
    
    return NATIVE_RETURN_DOUBLE(result);
}

/* Random.setSeed(long) - set the seed */
static JavaValue native_random_setSeed(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jlong seed = args[1].j;
    
    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    
    /* Apply the initial scramble: (seed ^ 0x5DEECE66DL) & ((1L << 48) - 1).
     * v18 FIX: add the spec-required 48-bit mask (see init_seed). */
    seed = (seed ^ 0x5DEECE66DLL) & ((1LL << 48) - 1);
    
    random_set_seed(obj, seed);
    
    return NATIVE_RETURN_VOID();
}

/*
 * java.lang.StringBuffer native methods
 * 
 * StringBuffer is used for building strings. We need to implement append() 
 * which returns 'this' for method chaining, and toString().
 */

/* StringBuffer buffer field access */
#define STRINGBUFFER_INITIAL_CAPACITY 16
#define STRINGBUFFER_INITIAL_ENTRIES 64

/* ИСПРАВЛЕНИЕ: Fallback storage for StringBuffer objects that don't have enough fields.
 * Some JAR files may have StringBuffer class with fewer than 3 instance fields.
 * We use dynamic array to store buffer state for such objects.
 */
typedef struct {
    void* obj_ptr;
    char* buffer;
    int length;
    int capacity;
    uint64_t alloc_id;  /* Unique ID for tracking (currently unused, kept for future use) */
} StringBufferFallback;

static StringBufferFallback* g_stringbuffer_fallback = NULL;
static int g_stringbuffer_fallback_count = 0;
static int g_stringbuffer_fallback_capacity = 0;

/* v34.42 PERF: 1-entry memo over the fallback table. String concat loops
 * hammer the SAME StringBuffer (append/append/append...) — the O(count)
 * linear scan below dominated string-heavy game logic (gprof Asphalt 3 3D:
 * 2.8 µs/call, ~2% of total CPU at 188k calls). The memo replicates the
 * scan's exact validation: entry still in bounds (the array reallocs),
 * entry still owned by this object pointer (GC cleanup NULLs dead
 * entries), and the object's class pointer still matches the class the
 * scan validated (guards against address reuse by a non-StringBuffer). */
static JavaObject* g_sb_memo_obj = NULL;
static JavaClass* g_sb_memo_clazz = NULL;
static int g_sb_memo_idx = -1;


/* Helper: Check if object has enough instance fields for inline storage
 * ИСПРАВЛЕНО: Всегда возвращаем false чтобы использовать fallback механизм,
 * так как прямое использование field[0/1/2] может конфликтовать с реальными полями Java
 */
static bool stringbuffer_has_enough_fields(JavaObject* obj) {
    (void)obj;
    /* Всегда используем fallback для безопасности */
    return false;
}


/* Helper: Find or create fallback entry for object.
 * ИСПРАВЛЕНО: Используем identity hashcode объекта вместо truncated allocation ID.
 * Identity hashcode уже хранится в объекте и не усекается.
 */
static StringBufferFallback* stringbuffer_get_fallback_scan(JavaObject* obj) {
    if (!obj) return NULL;
    
    /* Use the object's identity hashcode as unique ID - this is already stored in the object
     * header and doesn't have the truncation issue that the reserved byte had. */
    uint64_t obj_id = (uint64_t)(uintptr_t)obj;  /* Use pointer as unique ID */
    
    StringBufferFallback* free_slot = NULL;  /* Remember any free slot for reuse */
    
    /* Search for existing entry with matching pointer */
    for (int i = 0; i < g_stringbuffer_fallback_count; i++) {
        if (g_stringbuffer_fallback[i].obj_ptr == obj) {
            /* Found entry with matching pointer - this is the same object instance */
            
            /* Validate class type for safety */
            bool class_valid = false;
            if (obj->header.clazz && obj->header.clazz->class_name) {
                const char* class_name = obj->header.clazz->class_name;
                if (strcmp(class_name, "java/lang/StringBuffer") == 0 || 
                    strcmp(class_name, "java/lang/StringBuilder") == 0) {
                    class_valid = true;
                }
            }
            
            if (class_valid) {
                /* Same object - return existing entry */
                return &g_stringbuffer_fallback[i];
            }
            
            /* Class changed! This shouldn't happen, but handle it */
            
            if (g_stringbuffer_fallback[i].buffer) {
                free(g_stringbuffer_fallback[i].buffer);
            }
            g_stringbuffer_fallback[i].obj_ptr = NULL;
            g_stringbuffer_fallback[i].buffer = NULL;
            g_stringbuffer_fallback[i].length = 0;
            g_stringbuffer_fallback[i].capacity = 0;
            g_stringbuffer_fallback[i].alloc_id = 0;
            free_slot = &g_stringbuffer_fallback[i];  /* Remember for reuse */
            break;
        }
        /* Remember first free slot */
        if (g_stringbuffer_fallback[i].obj_ptr == NULL && free_slot == NULL) {
            free_slot = &g_stringbuffer_fallback[i];
        }
    }
    
    /* If we found a free slot, reuse it */
    if (free_slot) {
        free_slot->obj_ptr = obj;
        free_slot->buffer = NULL;
        free_slot->length = 0;
        free_slot->capacity = 0;
        free_slot->alloc_id = obj_id;
        return free_slot;
    }
    
    /* Initialize dynamic array if needed */
    if (g_stringbuffer_fallback == NULL) {
        g_stringbuffer_fallback_capacity = STRINGBUFFER_INITIAL_ENTRIES;
        g_stringbuffer_fallback = (StringBufferFallback*)calloc(g_stringbuffer_fallback_capacity, sizeof(StringBufferFallback));
        if (!g_stringbuffer_fallback) return NULL;
    }
    
    /* Expand array if needed */
    if (g_stringbuffer_fallback_count >= g_stringbuffer_fallback_capacity) {
        int new_capacity = g_stringbuffer_fallback_capacity * 2;
        StringBufferFallback* new_array = (StringBufferFallback*)realloc(g_stringbuffer_fallback, new_capacity * sizeof(StringBufferFallback));
        if (!new_array) return NULL;
        
        /* Zero-initialize new entries */
        memset(new_array + g_stringbuffer_fallback_capacity, 0, 
               (new_capacity - g_stringbuffer_fallback_capacity) * sizeof(StringBufferFallback));
        
        g_stringbuffer_fallback = new_array;
        g_stringbuffer_fallback_capacity = new_capacity;
    }
    
    /* Create new entry */
    StringBufferFallback* entry = &g_stringbuffer_fallback[g_stringbuffer_fallback_count++];
    entry->obj_ptr = obj;
    entry->buffer = NULL;
    entry->length = 0;
    entry->capacity = 0;
    entry->alloc_id = obj_id;
    return entry;
}

/* v34.42 PERF: memoizing wrapper (see the g_sb_memo_* block above). */
static StringBufferFallback* stringbuffer_get_fallback(JavaObject* obj) {
    if (!obj) return NULL;
    if (obj == g_sb_memo_obj &&
        g_sb_memo_idx >= 0 &&
        g_sb_memo_idx < g_stringbuffer_fallback_count &&
        g_stringbuffer_fallback[g_sb_memo_idx].obj_ptr == obj &&
        obj->header.clazz == g_sb_memo_clazz) {
        return &g_stringbuffer_fallback[g_sb_memo_idx];
    }
    StringBufferFallback* e = stringbuffer_get_fallback_scan(obj);
    if (e) {
        g_sb_memo_obj = obj;
        g_sb_memo_clazz = obj->header.clazz;
        g_sb_memo_idx = (int)(e - g_stringbuffer_fallback);
    } else {
        g_sb_memo_obj = NULL;
        g_sb_memo_clazz = NULL;
        g_sb_memo_idx = -1;
    }
    return e;
}

/* Helper: Reset StringBuffer to empty state (called from constructor)
 * ИСПРАВЛЕНО: Всегда очищаем буфер при reset, так как это вызывается из конструктора
 */
static void stringbuffer_reset(JavaObject* obj) {
    if (!obj) return;
    
    if (stringbuffer_has_enough_fields(obj)) {
        /* Reset inline fields */
        if (obj->fields[0].ref) {
            /* Keep buffer but reset length */
            obj->fields[1].i = 0;
        }
    } else {
        /* Reset fallback storage */
        StringBufferFallback* fallback = stringbuffer_get_fallback(obj);
        if (fallback) {
            /* ВАЖНО: Конструктор всегда должен начинать с чистого буфера.
             * Если буфер уже существует, очищаем его для повторного использования.
             * Это исправляет проблему накопления строк при повторном использовании
             * объектов StringBuffer по тому же адресу памяти. */
            fallback->length = 0;
            if (fallback->buffer) {
                ((jchar*)fallback->buffer)[0] = 0;  /* v34.5: NUL unit terminator */
            }
        }
    }
    NATIVE_DEBUG("StringBuffer reset: obj=%p", (void*)obj);
}

static char* stringbuffer_get_buffer(JavaObject* obj) {
    if (!obj) return NULL;
    
    /* Check if object has enough fields for inline storage */
    if (stringbuffer_has_enough_fields(obj)) {
        /* Use inline fields */
        if (obj->fields[0].ref == NULL) {
            obj->fields[0].ref = calloc(1024, sizeof(jchar)); /* v34.5: chars */
            obj->fields[1].i = 0;
            obj->fields[2].i = 1024;
        }
        return (char*)obj->fields[0].ref;
    } else {
        /* Use fallback storage */
        StringBufferFallback* fallback = stringbuffer_get_fallback(obj);
        if (!fallback) return NULL;
        
        if (fallback->buffer == NULL) {
            fallback->buffer = calloc(1024, sizeof(jchar)); /* v34.5: 1024 chars */
            fallback->length = 0;
            fallback->capacity = 1024;
        }
        return fallback->buffer;
    }
}

static int* stringbuffer_get_length_ptr(JavaObject* obj) {
    if (!obj) return NULL;
    
    if (stringbuffer_has_enough_fields(obj)) {
        return &obj->fields[1].i;
    } else {
        StringBufferFallback* fallback = stringbuffer_get_fallback(obj);
        return fallback ? &fallback->length : NULL;
    }
}

static int* stringbuffer_get_capacity_ptr(JavaObject* obj) {
    if (!obj) return NULL;
    
    if (stringbuffer_has_enough_fields(obj)) {
        return &obj->fields[2].i;
    } else {
        StringBufferFallback* fallback = stringbuffer_get_fallback(obj);
        return fallback ? &fallback->capacity : NULL;
    }
}

/* Helper: Update buffer pointer after realloc */
static void stringbuffer_set_buffer(JavaObject* obj, char* buffer) {
    if (!obj) return;

    if (stringbuffer_has_enough_fields(obj)) {
        obj->fields[0].ref = buffer;
    } else {
        StringBufferFallback* fallback = stringbuffer_get_fallback(obj);
        if (fallback) fallback->buffer = buffer;
    }
}

/* === v34.5: StringBuffer fallback now stores RAW UTF-16 code units ===
 * length/capacity are counted in CHARS (jchar units); the byte buffer
 * holds capacity * sizeof(jchar) bytes. The old storage was UTF-8 with
 * a BYTE length, which broke every game that compares StringBuffer.length()
 * against String.length() for non-ASCII text: for Cyrillic one char is
 * 2 UTF-8 bytes, so padding loops under-padded and substring(0,n) threw
 * StringIndexOutOfBoundsException (Doom RPG [Rus] h.a PC=33 on /base.str). */

static bool stringbuffer_ensure_capacity(JavaObject* obj, int additional_chars);

/* Append n ASCII bytes (each becomes one UTF-16 unit). */
static bool stringbuffer_append_ascii(JavaObject* obj, const char* ascii, int n) {
    if (!ascii || n < 0) return false;
    if (!stringbuffer_ensure_capacity(obj, n)) return false;
    jchar* chars = (jchar*)stringbuffer_get_buffer(obj);
    int* len_ptr = stringbuffer_get_length_ptr(obj);
    if (!chars || !len_ptr) return false;
    for (int i = 0; i < n; i++) {
        chars[*len_ptr + i] = (jchar)(unsigned char)ascii[i];
    }
    *len_ptr += n;
    chars[*len_ptr] = 0; /* keep a NUL unit terminator for safety */
    return true;
}

/* Append n UTF-16 units. */
static bool stringbuffer_append_utf16(JavaObject* obj, const jchar* src, int n) {
    if (n < 0) return false;
    if (n == 0) return true; /* nothing to append - not an error (v34.5) */
    if (!src) return false;
    if (!stringbuffer_ensure_capacity(obj, n)) return false;
    jchar* chars = (jchar*)stringbuffer_get_buffer(obj);
    int* len_ptr = stringbuffer_get_length_ptr(obj);
    if (!chars || !len_ptr) return false;
    if (n > 0) {
        memcpy(chars + *len_ptr, src, (size_t)n * sizeof(jchar));
    }
    *len_ptr += n;
    chars[*len_ptr] = 0;
    return true;
}

/* StringBuffer.append(String) */
static JavaValue native_stringbuffer_append_string(JVM* jvm, JavaThread* thread,
JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaString* str = (JavaString*)args[1].ref;

    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_OBJECT(NULL);
    }

    char* buffer = stringbuffer_get_buffer(obj);
    int* len_ptr = stringbuffer_get_length_ptr(obj);
    int* cap_ptr = stringbuffer_get_capacity_ptr(obj);
    
    if (!buffer || !len_ptr || !cap_ptr) {
        return NATIVE_RETURN_OBJECT(obj);
    }

    /* Java spec: StringBuffer.append((String)null) appends "null".
     * v34.5: store raw UTF-16 units, length in CHARS (see block comment).
     * v34.5 FIX: an EMPTY source string (chars==NULL, len==0 - e.g. the
     * zero-length entries in Doom RPG .str files) must be a no-op SUCCESS,
     * not an OOM - the old UTF-8 path substituted "" and kept going. */
    bool ok;
    if (str) {
        const jchar* chars = string_chars(str);
        int str_len = string_length(str);
        if (chars && str_len > 0) {
            ok = stringbuffer_append_utf16(obj, chars, str_len);
        } else {
            ok = true; /* empty string - nothing to append */
        }
    } else {
        ok = stringbuffer_append_ascii(obj, "null", 4);
    }
    if (!ok) {
        native_throw_oome(jvm, thread);
        return NATIVE_RETURN_OBJECT(NULL);
    }
    
    /* Return this for method chaining */
    return NATIVE_RETURN_OBJECT(obj);
}

/* Helper: Ensure buffer has enough capacity for additional CHARS (v34.5: char units) */
static bool stringbuffer_ensure_capacity(JavaObject* obj, int additional_chars) {
    char* buffer = stringbuffer_get_buffer(obj);
    int* len_ptr = stringbuffer_get_length_ptr(obj);
    int* cap_ptr = stringbuffer_get_capacity_ptr(obj);
    
    if (!buffer || !len_ptr || !cap_ptr) return false;
    
    if (*len_ptr + additional_chars >= *cap_ptr) {
        int new_cap = (*cap_ptr + additional_chars) * 2;
        if (new_cap < 256) new_cap = 256;
        char* new_buffer = (char*)realloc(buffer, (size_t)new_cap * sizeof(jchar)); /* v34.5: chars */
        if (!new_buffer) return false;
        stringbuffer_set_buffer(obj, new_buffer);
        *cap_ptr = new_cap;
    }
    return true;
}

/* v34.33: StringBuffer.append(char[] str, int offset, int len) —
 * Treasure Towers' resource loader builds property strings through this
 * overload (pc=93 in TreasureTowers.c); it was a no-op stub, so the game
 * read its .properties files as empty strings. */
/* StringBuffer.append(char[]) - whole array (v34.49: was NOT REGISTERED —
 * [INVOKE-MISSING] from AC2's h.class; obfuscated text builders build
 * strings from char arrays). Delegates to the (offset,len) variant. */
static JavaValue native_stringbuffer_append_chararray(JVM* jvm, JavaThread* thread,
JavaValue* args, int arg_count);

static JavaValue native_stringbuffer_append_chararray_full(JVM* jvm, JavaThread* thread,
JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaArray* arr = (JavaArray*)args[1].ref;
    if (!arr) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_OBJECT(NULL);
    }
    JavaValue new_args[4];
    new_args[0].ref = args[0].ref;  /* this */
    new_args[1].ref = args[1].ref;  /* char[] */
    new_args[2].i = 0;
    new_args[3].i = (jint)arr->length;
    return native_stringbuffer_append_chararray(jvm, thread, new_args, 4);
}

static JavaValue native_stringbuffer_append_chararray(JVM* jvm, JavaThread* thread,
JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaArray* arr = (JavaArray*)args[1].ref;
    jint offset = args[2].i;
    jint len = args[3].i;

    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_OBJECT(NULL);
    }
    if (!arr) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_OBJECT(NULL);
    }
    /* spec: IndexOutOfBoundsException when offset/len invalid */
    if (offset < 0 || len < 0 || offset + len > (jint)arr->length) {
        jvm_throw_by_name(jvm, "java/lang/IndexOutOfBoundsException", "append: range");
        thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_OBJECT(NULL);
    }
    if (arr->element_type != T_CHAR) {
        return NATIVE_RETURN_OBJECT(obj);
    }
    jchar* data = (jchar*)array_data(arr);
    if (data && len > 0) {
        if (!stringbuffer_append_utf16(obj, data + offset, len)) {
            native_throw_oome(jvm, thread);
            return NATIVE_RETURN_OBJECT(NULL);
        }
    }
    return NATIVE_RETURN_OBJECT(obj);
}

/* StringBuffer.append(int) */
static JavaValue native_stringbuffer_append_int(JVM* jvm, JavaThread* thread,
                                                 JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint value = args[1].i;
    
    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_OBJECT(NULL);
    }
    
    /* Debug: verify object's class pointer */
    if (obj->header.clazz && obj->header.clazz->class_name) {
        NATIVE_DEBUG("StringBuffer.append(%d): obj=%p, class=%s", 
                value, (void*)obj, obj->header.clazz->class_name);
    } else {
        NATIVE_DEBUG("StringBuffer.append(%d): obj=%p, INVALID CLASS POINTER!",
                value, (void*)obj);
    }
    
    char num_buf[32];
    int written = snprintf(num_buf, sizeof(num_buf), "%d", value);
    
    if (written > 0 && !stringbuffer_append_ascii(obj, num_buf, written)) {
        native_throw_oome(jvm, thread);
        return NATIVE_RETURN_OBJECT(NULL);
    }
    
    return NATIVE_RETURN_OBJECT(obj);
}

/* StringBuffer.append(long) */
static JavaValue native_stringbuffer_append_long(JVM* jvm, JavaThread* thread,
                                                  JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jlong value = args[1].j;
    
    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_OBJECT(NULL);
    }
    
    char num_buf[32];
    int written = snprintf(num_buf, sizeof(num_buf), "%lld", (long long)value);
    
    if (written > 0 && !stringbuffer_append_ascii(obj, num_buf, written)) {
        native_throw_oome(jvm, thread);
        return NATIVE_RETURN_OBJECT(NULL);
    }
    
    return NATIVE_RETURN_OBJECT(obj);
}

/* StringBuffer.append(float) */
static JavaValue native_stringbuffer_append_float(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jfloat value = args[1].f;
    
    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_OBJECT(NULL);
    }
    
    char num_buf[48];
    java_format_float(num_buf, sizeof(num_buf), value);  /* FIX-19g: Java format */
    int written = (int)strlen(num_buf);
    
    if (written > 0 && !stringbuffer_append_ascii(obj, num_buf, written)) {
        native_throw_oome(jvm, thread);
        return NATIVE_RETURN_OBJECT(NULL);
    }
    
    return NATIVE_RETURN_OBJECT(obj);
}

/* StringBuffer.append(double) */
static JavaValue native_stringbuffer_append_double(JVM* jvm, JavaThread* thread,
                                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jdouble value = args[1].d;
    
    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_OBJECT(NULL);
    }
    
    char num_buf[48];
    java_format_double(num_buf, sizeof(num_buf), value);  /* FIX-19g: Java format */
    int written = (int)strlen(num_buf);
    
    if (written > 0 && !stringbuffer_append_ascii(obj, num_buf, written)) {
        native_throw_oome(jvm, thread);
        return NATIVE_RETURN_OBJECT(NULL);
    }
    
    return NATIVE_RETURN_OBJECT(obj);
}

/* StringBuffer.append(boolean) */
static JavaValue native_stringbuffer_append_boolean(JVM* jvm, JavaThread* thread,
                                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jboolean value = args[1].i;
    
    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_OBJECT(NULL);
    }
    
    const char* str = value ? "true" : "false";
    int str_len = strlen(str);
    
    if (str_len > 0 && !stringbuffer_append_ascii(obj, str, str_len)) {
        native_throw_oome(jvm, thread);
        return NATIVE_RETURN_OBJECT(NULL);
    }
    
    return NATIVE_RETURN_OBJECT(obj);
}

/* StringBuffer.append(char) - v34.5: store one raw UTF-16 unit */
static JavaValue native_stringbuffer_append_char(JVM* jvm, JavaThread* thread,
                                                  JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jchar value = (jchar)args[1].i;
    
    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_OBJECT(NULL);
    }
    
    if (!stringbuffer_ensure_capacity(obj, 1)) {
        native_throw_oome(jvm, thread);
        return NATIVE_RETURN_OBJECT(NULL);
    }
    
    jchar* chars = (jchar*)stringbuffer_get_buffer(obj);
    int* len_ptr = stringbuffer_get_length_ptr(obj);
    if (!chars || !len_ptr) {
        return NATIVE_RETURN_OBJECT(obj);
    }
    
    chars[(*len_ptr)++] = value;
    chars[*len_ptr] = 0;
    
    return NATIVE_RETURN_OBJECT(obj);
}

/* StringBuffer.append(Object) */
static JavaValue native_stringbuffer_append_object(JVM* jvm, JavaThread* thread,
                                                    JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaObject* value = (JavaObject*)args[1].ref;
    
    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_OBJECT(NULL);
    }
    
    /* v34.5: source data is now raw UTF-16 in every branch */
    bool ok = false;
    
    if (value) {
        /* Try to get proper string representation of the object */
        if (value->header.clazz) {
            const char* cname = value->header.clazz->class_name;
            if (cname && (strcmp(cname, "java/lang/StringBuffer") == 0 ||
                           strcmp(cname, "java/lang/StringBuilder") == 0)) {
                /* StringBuffer/StringBuilder: read its internal UTF-16 buffer */
                jchar* inner_chars = (jchar*)stringbuffer_get_buffer(value);
                int* inner_len = stringbuffer_get_length_ptr(value);
                if (inner_chars && inner_len && *inner_len > 0) {
                    ok = stringbuffer_append_utf16(obj, inner_chars, *inner_len);
                } else {
                    ok = true; /* empty source, nothing to append */
                }
            } else if (cname && strcmp(cname, "java/lang/String") == 0) {
                /* String: raw UTF-16 units */
                const jchar* chars = string_chars((JavaString*)value);
                int slen = string_length((JavaString*)value);
                if (chars && slen > 0) {
                    ok = stringbuffer_append_utf16(obj, chars, slen);
                } else {
                    ok = true;
                }
            } else {
                /* v34.29 FIX: Java semantics — append(Object) appends
                 * String.valueOf(obj), i.e. obj.toString(). The old
                 * fallback appended the CLASS NAME, so JBenchmark3D's
                 * results screen printed "java/lang/Boolean" for every
                 * Graphics3D capability value instead of "true"/"false".
                 * util_to_string_of dispatches to the registered toString
                 * natives (Boolean/Integer/...); the class-name fallback
                 * only remains for objects with no toString at all. */
                JavaString* s = util_to_string_of(jvm, thread, value);
                const jchar* sc = s ? string_chars(s) : NULL;
                int sl = s ? string_length(s) : 0;
                if (sc && sl > 0) {
                    ok = stringbuffer_append_utf16(obj, sc, sl);
                } else {
                    ok = stringbuffer_append_ascii(obj, cname, (int)strlen(cname));
                }
            }
        } else {
            ok = stringbuffer_append_ascii(obj, "Object", 6);
        }
    } else {
        ok = stringbuffer_append_ascii(obj, "null", 4);
    }
    
    if (!ok) {
        native_throw_oome(jvm, thread);
        return NATIVE_RETURN_OBJECT(NULL);
    }
    
    return NATIVE_RETURN_OBJECT(obj);
}

/* StringBuffer.toString() */
static JavaValue native_stringbuffer_toString(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_OBJECT(NULL);
    }
    
    char* buffer = stringbuffer_get_buffer(obj);
    int* len_ptr = stringbuffer_get_length_ptr(obj);
    
    /* DEBUG: Log current state */
    
    if (buffer && len_ptr && *len_ptr >= 0) {
        /* v34.5: build the string directly from raw UTF-16 units.
         * NOTE: Do NOT clear the buffer after toString().
         * In standard Java, toString() does NOT modify the StringBuffer.
         * Games like Bounce Tales call toString() and then continue appending
         * to the same buffer. Clearing here causes ArrayIndexOutOfBoundsException
         * downstream when the expected content is missing. */
        JavaString* str = jvm_new_string_utf16(jvm, (const jchar*)buffer, *len_ptr);
        /* v36.09: J2ME javac lowers EVERY `+` concat through StringBuffer —
         * a silent NULL toString() is exactly the "null.str" corruption. */
        return NATIVE_RETURN_OBJECT(native_string_result_guard(jvm, thread, str,
                                                               "StringBuffer.toString()", NULL));
    }
    
    return NATIVE_RETURN_OBJECT(jvm_new_string(jvm, ""));
}

/* StringBuffer.length() */
static JavaValue native_stringbuffer_length(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    if (!obj) {
        return NATIVE_RETURN_INT(0);
    }
    
    int* len_ptr = stringbuffer_get_length_ptr(obj);
    return NATIVE_RETURN_INT(len_ptr ? *len_ptr : 0);
}

/* StringBuffer.setLength(int) */
static JavaValue native_stringbuffer_setlength(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint new_length = args[1].i;
    
    if (!obj) {
        return NATIVE_RETURN_VOID();
    }
    
    if (new_length < 0) {
        /* FIX-19d: spec requires StringIndexOutOfBoundsException, old code
         * silently ignored negative length */
        native_throw_sioobe(jvm, thread, new_length);
        return NATIVE_RETURN_VOID();
    }
    
    int* len_ptr = stringbuffer_get_length_ptr(obj);
    char* buffer = stringbuffer_get_buffer(obj);
    int* cap_ptr = stringbuffer_get_capacity_ptr(obj);
    
    if (len_ptr && buffer && cap_ptr) {
        /* v34.41 FIX (ARMv7 heap corruption, ASan):
         * the NUL-terminator write buffer[new_length] below needs ONE
         * extra jchar slot, so the buffer must grow when new_length is
         * EQUAL to capacity too — the old `>` let setLength(capacity)
         * write 2 bytes past the end of the heap chunk. On x86-64 the
         * 16-byte malloc alignment absorbed the overrun (invisible),
         * on ARMv7 (8-byte chunks) it smashed the next chunk's header ->
         * "malloc(): invalid size (unsorted)" / SIGSEGV / dead
         * allocations (black screens) in games that call setLength on
         * worker threads (FM X, reproducible under qemu-arm). */
        if (new_length >= *cap_ptr) {
            int new_cap = new_length * 2 + 2; /* +2: room for NUL, >= 2 */
            char* new_buffer = (char*)realloc(buffer, (size_t)new_cap * sizeof(jchar));
            if (!new_buffer) {
                return NATIVE_RETURN_VOID(); /* v34.41: never memset beyond the old capacity */
            }
            stringbuffer_set_buffer(obj, new_buffer);
            buffer = new_buffer;
            *cap_ptr = new_cap;
        }
        
        /* If growing, fill with nulls (v34.5: jchar units) */
        if (new_length > *len_ptr) {
            memset((jchar*)buffer + *len_ptr, 0,
                   (size_t)(new_length - *len_ptr) * sizeof(jchar));
        }
        
        *len_ptr = new_length;
        if (buffer) ((jchar*)buffer)[*len_ptr] = 0;
    }
    
    return NATIVE_RETURN_VOID();
}

/* StringBuffer.<init>() - default constructor */
static JavaValue native_stringbuffer_init(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    if (!obj) {
        return NATIVE_RETURN_VOID();
    }
    
    /* ALWAYS log StringBuffer creation for debugging */
    
    /* Reset the StringBuffer to empty state */
    stringbuffer_reset(obj);
    
    return NATIVE_RETURN_VOID();
}

/* StringBuffer.<init>(int) - constructor with initial capacity */
static JavaValue native_stringbuffer_init_capacity(JVM* jvm, JavaThread* thread,
                                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint capacity = args[1].i;
    
    if (!obj) {
        return NATIVE_RETURN_VOID();
    }
    
    NATIVE_DEBUG("StringBuffer.<init>(%d): obj=%p, resetting to empty", capacity, (void*)obj);
    
    /* Reset the StringBuffer to empty state */
    stringbuffer_reset(obj);
    
    /* Pre-allocate buffer with specified capacity (v34.5: capacity in CHARS) */
    if (capacity > 0) {
        StringBufferFallback* fallback = stringbuffer_get_fallback(obj);
        if (fallback) {
            /* Allocate new buffer with specified capacity */
            if (fallback->buffer) {
                free(fallback->buffer);
            }
            fallback->buffer = (char*)calloc((size_t)capacity, sizeof(jchar));
            if (fallback->buffer) {
                fallback->capacity = capacity;
                fallback->length = 0;
                ((jchar*)fallback->buffer)[0] = 0;
            }
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* StringBuffer.<init>(String) - constructor with initial string */
static JavaValue native_stringbuffer_init_string(JVM* jvm, JavaThread* thread,
                                                  JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaString* str = (JavaString*)args[1].ref;
    
    if (!obj) {
        return NATIVE_RETURN_VOID();
    }
    
    NATIVE_DEBUG("StringBuffer.<init>(String): obj=%p, str=%p", (void*)obj, (void*)str);
    
    /* Reset the StringBuffer to empty state */
    stringbuffer_reset(obj);
    
    /* Append the initial string if provided */
    if (str) {
        JavaValue append_args[2];
        append_args[0].ref = obj;
        append_args[1].ref = str;
        native_stringbuffer_append_string(jvm, thread, append_args, 2);
    }
    
    return NATIVE_RETURN_VOID();
}

/* StringBuffer.reverse() - reverses the character sequence */
static JavaValue native_stringbuffer_reverse(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    if (!obj) {
        return NATIVE_RETURN_OBJECT(NULL);
    }
    
    jchar* chars = (jchar*)stringbuffer_get_buffer(obj);
    int* len_ptr = stringbuffer_get_length_ptr(obj);
    
    if (chars && len_ptr && *len_ptr > 0) {
        int len = *len_ptr;
        for (int i = 0; i < len / 2; i++) {
            jchar tmp = chars[i];
            chars[i] = chars[len - 1 - i];
            chars[len - 1 - i] = tmp;
        }
    }
    
    return NATIVE_RETURN_OBJECT(obj);
}

/* StringBuffer.capacity() - returns the current capacity */
static JavaValue native_stringbuffer_capacity(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    if (!obj) {
        return NATIVE_RETURN_INT(0);
    }
    
    int* cap_ptr = stringbuffer_get_capacity_ptr(obj);
    return NATIVE_RETURN_INT(cap_ptr ? *cap_ptr : 0);
}

/* StringBuffer.charAt(int) - returns character at index */
static JavaValue native_stringbuffer_charAt(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint index = args[1].i;
    
    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_INT(0);
    }
    
    jchar* chars = (jchar*)stringbuffer_get_buffer(obj);
    int* len_ptr = stringbuffer_get_length_ptr(obj);
    
    if (!chars || !len_ptr) {
        return NATIVE_RETURN_INT(0);
    }
    
    /* FIX-19d: spec requires StringIndexOutOfBoundsException, old code
     * silently returned 0 for out-of-range index */
    if (index < 0 || index >= *len_ptr) {
        native_throw_sioobe(jvm, thread, index);
        return NATIVE_RETURN_INT(0);
    }
    
    return NATIVE_RETURN_INT((jint)chars[index]);
}

/* StringBuffer.setCharAt(int, char) - sets character at index */
static JavaValue native_stringbuffer_setCharAt(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint index = args[1].i;
    jchar ch = (jchar)args[2].i;
    
    if (!obj) {
        return NATIVE_RETURN_VOID();
    }
    
    jchar* chars = (jchar*)stringbuffer_get_buffer(obj);
    int* len_ptr = stringbuffer_get_length_ptr(obj);
    
    /* FIX-19d: spec requires StringIndexOutOfBoundsException, old code
     * silently ignored out-of-range index */
    if (!chars || !len_ptr || index < 0 || index >= *len_ptr) {
        native_throw_sioobe(jvm, thread, index);
        return NATIVE_RETURN_VOID();
    }
    
    chars[index] = ch;
    
    return NATIVE_RETURN_VOID();
}

/* StringBuffer.insert(int, String) - inserts string at position */
static JavaValue native_stringbuffer_insert_string(JVM* jvm, JavaThread* thread,
                                                    JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count; (void)jvm;  /* v34.10: silence -Wunused-parameter */
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint offset = args[1].i;
    JavaString* str = (JavaString*)args[2].ref;
    
    if (!obj) {
        return NATIVE_RETURN_OBJECT(NULL);
    }
    
    char* buffer = stringbuffer_get_buffer(obj);
    int* len_ptr = stringbuffer_get_length_ptr(obj);
    int* cap_ptr = stringbuffer_get_capacity_ptr(obj);
    
    if (!buffer || !len_ptr || !cap_ptr) {
        return NATIVE_RETURN_OBJECT(obj);
    }
    
    /* Get string content (v34.5: raw UTF-16) */
    const jchar* src = str ? string_chars(str) : NULL;
    int str_len = str ? string_length(str) : 4;
    jchar null_units[4] = {'n', 'u', 'l', 'l'};
    if (!src) {
        src = null_units;
    }
    
    /* Validate offset */
    if (offset < 0 || offset > *len_ptr) {
        return NATIVE_RETURN_OBJECT(obj);
    }
    
    /* Ensure capacity (chars) — v34.41 FIX: `>=` (not `>`) so the NUL
     * terminator write at chars[new_len] stays in bounds when new_len
     * equals capacity exactly (same ARMv7 heap-corruption class as
     * setLength; masked on x86-64 by the 16-byte malloc alignment). */
    int new_len = *len_ptr + str_len;
    if (new_len >= *cap_ptr) {
        int new_cap = new_len * 2 + 2;
        char* new_buffer = (char*)realloc(buffer, (size_t)new_cap * sizeof(jchar));
        if (!new_buffer) {
            return NATIVE_RETURN_OBJECT(obj); /* v34.41: bail instead of memmove overflow */
        }
        stringbuffer_set_buffer(obj, new_buffer);
        buffer = new_buffer;
        *cap_ptr = new_cap;
    }
    
    /* Shift existing content right (jchar units) */
    if (str_len > 0) {
        jchar* chars = (jchar*)buffer;
        memmove(chars + offset + str_len, chars + offset,
                (size_t)(*len_ptr - offset) * sizeof(jchar));
        memcpy(chars + offset, src, (size_t)str_len * sizeof(jchar));
        *len_ptr = new_len;
        chars[*len_ptr] = 0;
    }
    
    return NATIVE_RETURN_OBJECT(obj);
}

/* v36.33 [SB-INSERT-OVERLOADS]: StringBuffer.insert had ONLY the (I, String)
 * overload registered. Jewel Adventure (mob.ua) builds its resource-pack
 * names with a fast int-writer that uses insert(offset, char) and
 * insert(offset, int): the unregistered (IC)/(II) calls resolved to a
 * do-nothing fallback, every appended digit was silently lost, and the game
 * asked the JAR for a file literally named "RP.rp" (no number) — pack load
 * failed, the sizes array stayed NULL and startApp died with an
 * NullPointerException (field class: "game closes without showing
 * anything"). Same silent-loss family as the FIX-19i deleteCharAt. */

/* Shared core: insert n jchars at offset. Returns false on OOM/ bad state. */
static bool stringbuffer_insert_jchars(JavaObject* obj, jint offset,
                                       const jchar* src, int n) {
    char* buffer = stringbuffer_get_buffer(obj);
    int* len_ptr = stringbuffer_get_length_ptr(obj);
    int* cap_ptr = stringbuffer_get_capacity_ptr(obj);
    if (!buffer || !len_ptr || !cap_ptr) return true; /* nothing we can do */
    if (offset < 0 || offset > *len_ptr) return true; /* out of range: no-op per spec-ish */

    int new_len = *len_ptr + n;
    if (new_len >= *cap_ptr) {
        int new_cap = new_len * 2 + 2;
        char* new_buffer = (char*)realloc(buffer, (size_t)new_cap * sizeof(jchar));
        if (!new_buffer) return false;
        stringbuffer_set_buffer(obj, new_buffer);
        buffer = new_buffer;
        *cap_ptr = new_cap;
    }
    if (n > 0) {
        jchar* chars = (jchar*)buffer;
        memmove(chars + offset + n, chars + offset,
                (size_t)(*len_ptr - offset) * sizeof(jchar));
        memcpy(chars + offset, src, (size_t)n * sizeof(jchar));
        *len_ptr = new_len;
        chars[*len_ptr] = 0;
    }
    return true;
}

/* StringBuffer.insert(int offset, char c) */
static JavaValue native_stringbuffer_insert_char(JVM* jvm, JavaThread* thread,
                                                 JavaValue* args, int arg_count) {
    (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint offset = args[1].i;
    jchar value = (jchar)args[2].i;

    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_OBJECT(NULL);
    }
    if (!stringbuffer_insert_jchars(obj, offset, &value, 1)) {
        native_throw_oome(jvm, thread);
        return NATIVE_RETURN_OBJECT(NULL);
    }
    return NATIVE_RETURN_OBJECT(obj);
}

/* StringBuffer.insert(int offset, int i) */
static JavaValue native_stringbuffer_insert_int(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint offset = args[1].i;
    jint value = args[2].i;

    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_OBJECT(NULL);
    }
    char num_buf[16];
    int written = snprintf(num_buf, sizeof(num_buf), "%d", value);
    if (written > 0) {
        jchar tmp[16];
        for (int i = 0; i < written; i++) tmp[i] = (jchar)(unsigned char)num_buf[i];
        if (!stringbuffer_insert_jchars(obj, offset, tmp, written)) {
            native_throw_oome(jvm, thread);
            return NATIVE_RETURN_OBJECT(NULL);
        }
    }
    return NATIVE_RETURN_OBJECT(obj);
}

/* StringBuffer.insert(int offset, long l) */
static JavaValue native_stringbuffer_insert_long(JVM* jvm, JavaThread* thread,
                                                 JavaValue* args, int arg_count) {
    (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint offset = args[1].i;
    jlong value = args[2].j;

    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_OBJECT(NULL);
    }
    char num_buf[24];
    int written = snprintf(num_buf, sizeof(num_buf), "%lld", (long long)value);
    if (written > 0) {
        jchar tmp[24];
        for (int i = 0; i < written; i++) tmp[i] = (jchar)(unsigned char)num_buf[i];
        if (!stringbuffer_insert_jchars(obj, offset, tmp, written)) {
            native_throw_oome(jvm, thread);
            return NATIVE_RETURN_OBJECT(NULL);
        }
    }
    return NATIVE_RETURN_OBJECT(obj);
}

/* StringBuffer.insert(int offset, boolean b) */
static JavaValue native_stringbuffer_insert_boolean(JVM* jvm, JavaThread* thread,
                                                    JavaValue* args, int arg_count) {
    (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint offset = args[1].i;
    int value = args[2].i;

    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_OBJECT(NULL);
    }
    const char* txt = value ? "true" : "false";
    int written = value ? 4 : 5;
    jchar tmp[8];
    for (int i = 0; i < written; i++) tmp[i] = (jchar)(unsigned char)txt[i];
    if (!stringbuffer_insert_jchars(obj, offset, tmp, written)) {
        native_throw_oome(jvm, thread);
        return NATIVE_RETURN_OBJECT(NULL);
    }
    return NATIVE_RETURN_OBJECT(obj);
}

/* StringBuffer.insert(int offset, char[] str) */
static JavaValue native_stringbuffer_insert_chararray(JVM* jvm, JavaThread* thread,
                                                      JavaValue* args, int arg_count) {
    (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint offset = args[1].i;
    JavaArray* arr = (JavaArray*)args[2].ref;

    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_OBJECT(NULL);
    }
    if (!arr) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_OBJECT(NULL);
    }
    if (arr->element_type != T_CHAR) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_OBJECT(NULL);
    }
    int n = (int)arr->length;
    if (n > 0 && !stringbuffer_insert_jchars(obj, offset, (const jchar*)array_data(arr), n)) {
        native_throw_oome(jvm, thread);
        return NATIVE_RETURN_OBJECT(NULL);
    }
    return NATIVE_RETURN_OBJECT(obj);
}

/* StringBuffer.deleteCharAt(int) - remove single character at index.
 * FIX-19i: was not registered at all, so it resolved to a do-nothing stub
 * and sb.deleteCharAt(1) silently kept the character. */
static JavaValue native_stringbuffer_deleteCharAt(JVM* jvm, JavaThread* thread,
                                                  JavaValue* args, int arg_count) {
    (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint index = args[1].i;

    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_OBJECT(NULL);
    }

    jchar* chars = (jchar*)stringbuffer_get_buffer(obj);
    int* len_ptr = stringbuffer_get_length_ptr(obj);

    if (!chars || !len_ptr || index < 0 || index >= *len_ptr) {
        native_throw_sioobe(jvm, thread, index);
        return NATIVE_RETURN_OBJECT(obj);
    }

    memmove(chars + index, chars + index + 1,
            (size_t)(*len_ptr - index - 1) * sizeof(jchar));
    (*len_ptr)--;
    chars[*len_ptr] = 0;

    return NATIVE_RETURN_OBJECT(obj);
}

static JavaValue native_stringbuffer_delete(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint start = args[1].i;
    jint end = args[2].i;
    
    if (!obj) {
        return NATIVE_RETURN_OBJECT(NULL);
    }
    
    jchar* chars = (jchar*)stringbuffer_get_buffer(obj);
    int* len_ptr = stringbuffer_get_length_ptr(obj);
    
    if (!chars || !len_ptr) {
        return NATIVE_RETURN_OBJECT(obj);
    }
    
    int len = *len_ptr;
    
    /* Validate bounds */
    if (start < 0) start = 0;
    if (end > len) end = len;
    if (start >= end) {
        return NATIVE_RETURN_OBJECT(obj);
    }
    
    /* Shift content left (jchar units) */
    int del_len = end - start;
    memmove(chars + start, chars + end, (size_t)(len - end) * sizeof(jchar));
    *len_ptr = len - del_len;
    chars[*len_ptr] = 0;
    
    return NATIVE_RETURN_OBJECT(obj);
}

/* v58: StringBuffer.getChars(srcBegin, srcEnd, dst, dstBegin).
 * Was INVOKE-MISSING (caller d.b pc=258, 3D Coaster Rush \u00d74):
 * the stub pushed nothing into dst, so the game built its text
 * buffers from zero-filled char arrays. Mirrors native_string_getChars
 * but reads the live mutable buffer instead of an interned string. */
static JavaValue native_stringbuffer_getChars(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint srcBegin = args[1].i;
    jint srcEnd = args[2].i;
    JavaArray* dst = (JavaArray*)args[3].ref;
    jint dstBegin = args[4].i;

    if (!obj || !dst) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    if (dst->element_type != T_CHAR) {
        jvm_throw_by_name(jvm, "java/lang/ArrayStoreException", "not char[]");
        thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_VOID();
    }

    const jchar* chars = (const jchar*)stringbuffer_get_buffer(obj);
    const int* len_ptr = stringbuffer_get_length_ptr(obj);
    if (!chars || !len_ptr) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    int len = *len_ptr;

    if (srcBegin < 0 || srcEnd < 0 || srcBegin > srcEnd || srcEnd > len) {
        native_throw_sioobe(jvm, thread, srcBegin);
        return NATIVE_RETURN_VOID();
    }
    if (dstBegin < 0 || dstBegin + (srcEnd - srcBegin) > (jint)dst->length) {
        native_throw_sioobe(jvm, thread, dstBegin);
        return NATIVE_RETURN_VOID();
    }

    jchar* dst_data = (jchar*)array_data(dst);
    int count = srcEnd - srcBegin;
    for (int i = 0; i < count; i++) {
        dst_data[dstBegin + i] = chars[srcBegin + i];
    }
    return NATIVE_RETURN_VOID();
}

void init_java_lang_stringbuffer(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"java/lang/StringBuffer", "getChars", "(II[CI)V", native_stringbuffer_getChars},
        {"java/lang/StringBuffer", "<init>", "()V", native_stringbuffer_init},
        {"java/lang/StringBuffer", "<init>", "(I)V", native_stringbuffer_init_capacity},
        {"java/lang/StringBuffer", "<init>", "(Ljava/lang/String;)V", native_stringbuffer_init_string},
        {"java/lang/StringBuffer", "append", "(Ljava/lang/String;)Ljava/lang/StringBuffer;", native_stringbuffer_append_string},
        {"java/lang/StringBuffer", "append", "(I)Ljava/lang/StringBuffer;", native_stringbuffer_append_int},
        {"java/lang/StringBuffer", "append", "(J)Ljava/lang/StringBuffer;", native_stringbuffer_append_long},
        {"java/lang/StringBuffer", "append", "(F)Ljava/lang/StringBuffer;", native_stringbuffer_append_float},
        {"java/lang/StringBuffer", "append", "(D)Ljava/lang/StringBuffer;", native_stringbuffer_append_double},
        {"java/lang/StringBuffer", "append", "(Z)Ljava/lang/StringBuffer;", native_stringbuffer_append_boolean},
        {"java/lang/StringBuffer", "append", "(C)Ljava/lang/StringBuffer;", native_stringbuffer_append_char},
        {"java/lang/StringBuffer", "append", "(Ljava/lang/Object;)Ljava/lang/StringBuffer;", native_stringbuffer_append_object},
        {"java/lang/StringBuffer", "append", "([CII)Ljava/lang/StringBuffer;", native_stringbuffer_append_chararray},
        {"java/lang/StringBuffer", "append", "([C)Ljava/lang/StringBuffer;", native_stringbuffer_append_chararray_full},
        {"java/lang/StringBuffer", "toString", "()Ljava/lang/String;", native_stringbuffer_toString},
        {"java/lang/StringBuffer", "length", "()I", native_stringbuffer_length},
        {"java/lang/StringBuffer", "setLength", "(I)V", native_stringbuffer_setlength},
        {"java/lang/StringBuffer", "reverse", "()Ljava/lang/StringBuffer;", native_stringbuffer_reverse},
        {"java/lang/StringBuffer", "capacity", "()I", native_stringbuffer_capacity},
        {"java/lang/StringBuffer", "charAt", "(I)C", native_stringbuffer_charAt},
        {"java/lang/StringBuffer", "setCharAt", "(IC)V", native_stringbuffer_setCharAt},
        {"java/lang/StringBuffer", "insert", "(ILjava/lang/String;)Ljava/lang/StringBuffer;", native_stringbuffer_insert_string},
        /* v36.33 [SB-INSERT-OVERLOADS]: (IC)/(II)/(IJ)/(IZ)/(I[C) were
         * unregistered — calls resolved to a do-nothing fallback and the
         * inserted characters were silently lost (Jewel Adventure built its
         * "/RP<n>.rp" resource-pack names with insert(IC)/(II) and asked the
         * JAR for a file named "RP.rp"). */
        {"java/lang/StringBuffer", "insert", "(IC)Ljava/lang/StringBuffer;", native_stringbuffer_insert_char},
        {"java/lang/StringBuffer", "insert", "(II)Ljava/lang/StringBuffer;", native_stringbuffer_insert_int},
        {"java/lang/StringBuffer", "insert", "(IJ)Ljava/lang/StringBuffer;", native_stringbuffer_insert_long},
        {"java/lang/StringBuffer", "insert", "(IZ)Ljava/lang/StringBuffer;", native_stringbuffer_insert_boolean},
        {"java/lang/StringBuffer", "insert", "(I[C)Ljava/lang/StringBuffer;", native_stringbuffer_insert_chararray},
        {"java/lang/StringBuffer", "delete", "(II)Ljava/lang/StringBuffer;", native_stringbuffer_delete},
        {"java/lang/StringBuffer", "deleteCharAt", "(I)Ljava/lang/StringBuffer;", native_stringbuffer_deleteCharAt},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
    NATIVE_DEBUG("Registered java/lang/StringBuffer native methods");
}

/*
 * java.lang.StringBuilder native methods
 * Same implementation as StringBuffer (uses StringBuffer handlers)
 */

void init_java_lang_stringbuilder(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"java/lang/StringBuilder", "<init>", "()V", native_stringbuffer_init},
        {"java/lang/StringBuilder", "<init>", "(I)V", native_stringbuffer_init_capacity},
        {"java/lang/StringBuilder", "<init>", "(Ljava/lang/String;)V", native_stringbuffer_init_string},
        {"java/lang/StringBuilder", "append", "(Ljava/lang/String;)Ljava/lang/StringBuilder;", native_stringbuffer_append_string},
        {"java/lang/StringBuilder", "append", "(I)Ljava/lang/StringBuilder;", native_stringbuffer_append_int},
        {"java/lang/StringBuilder", "append", "(J)Ljava/lang/StringBuilder;", native_stringbuffer_append_long},
        {"java/lang/StringBuilder", "append", "(F)Ljava/lang/StringBuilder;", native_stringbuffer_append_float},
        {"java/lang/StringBuilder", "append", "(D)Ljava/lang/StringBuilder;", native_stringbuffer_append_double},
        {"java/lang/StringBuilder", "append", "(Z)Ljava/lang/StringBuilder;", native_stringbuffer_append_boolean},
        {"java/lang/StringBuilder", "append", "(C)Ljava/lang/StringBuilder;", native_stringbuffer_append_char},
        {"java/lang/StringBuilder", "append", "(Ljava/lang/Object;)Ljava/lang/StringBuilder;", native_stringbuffer_append_object},
        {"java/lang/StringBuilder", "toString", "()Ljava/lang/String;", native_stringbuffer_toString},
        {"java/lang/StringBuilder", "length", "()I", native_stringbuffer_length},
        {"java/lang/StringBuilder", "setCharAt", "(IC)V", native_stringbuffer_setCharAt},
        {"java/lang/StringBuilder", "charAt", "(I)C", native_stringbuffer_charAt},
        {"java/lang/StringBuilder", "insert", "(ILjava/lang/String;)Ljava/lang/StringBuilder;", native_stringbuffer_insert_string},
        {"java/lang/StringBuilder", "delete", "(II)Ljava/lang/StringBuilder;", native_stringbuffer_delete},
        {"java/lang/StringBuilder", "deleteCharAt", "(I)Ljava/lang/StringBuilder;", native_stringbuffer_deleteCharAt},
        {"java/lang/StringBuilder", "setLength", "(I)V", native_stringbuffer_setlength},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
    NATIVE_DEBUG("Registered java/lang/StringBuilder native methods");
}

/*
 * java.util.Vector native methods
 */

static void vector_init_internal(JVM* jvm, JavaObject* vec, int initialCapacity) {
    if (!vec) return;
    
    /* ИСПРАВЛЕНО: Используем native_set_field_value */
    int capacity = initialCapacity > 0 ? initialCapacity : 10;
    JavaArray* arr = jvm_new_array(jvm, DESC_OBJECT, capacity, NULL);
    
    JavaValue arr_val = { .ref = arr };
    JavaValue count_val = { .i = 0 };
    
    native_set_field_value(vec, "elementData", arr_val);
    native_set_field_value(vec, "elementCount", count_val);
    
    NATIVE_DEBUG("init: initialized elementData with capacity %d", capacity);
}

static JavaValue native_vector_init(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* vec = (JavaObject*)args[0].ref;
    NATIVE_DEBUG("<init>(): initializing vector %p", (void*)vec);
    vector_init_internal(jvm, vec, 10);
    return NATIVE_RETURN_VOID();
}

/* Vector(int initialCapacity) constructor */
static JavaValue native_vector_init_int(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* vec = (JavaObject*)args[0].ref;
    jint initialCapacity = args[1].i;
    NATIVE_DEBUG("<init>(%d): initializing vector %p", initialCapacity, (void*)vec);
    vector_init_internal(jvm, vec, initialCapacity);
    return NATIVE_RETURN_VOID();
}

/* Vector(int initialCapacity, int capacityIncrement) constructor */
static JavaValue native_vector_init_int_int(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* vec = (JavaObject*)args[0].ref;
    jint initialCapacity = args[1].i;
    jint capacityIncrement = args[2].i;
    (void)capacityIncrement; /* We don't use this yet */
    NATIVE_DEBUG("<init>(%d, %d): initializing vector %p", initialCapacity, capacityIncrement, (void*)vec);
    vector_init_internal(jvm, vec, initialCapacity);
    return NATIVE_RETURN_VOID();
}

static JavaValue native_vector_add(JVM* jvm, JavaThread* thread,
                                   JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* vec = (JavaObject*)args[0].ref;
    JavaValue element = args[1];
    
    if (!vec) return NATIVE_RETURN_INT(1);
    
    /* ИСПРАВЛЕНО: Используем native_get/set_field_value */
    JavaArray* arr = (JavaArray*)native_get_field_value(vec, "elementData").ref;
    int count = native_get_field_value(vec, "elementCount").i;
    
    if (arr) {
        /* Grow array if needed */
        if (count >= arr->length) {
            JavaArray* new_arr = jvm_new_array(jvm, DESC_OBJECT, arr->length * 2 + 1, NULL);
            if (new_arr) {
                /* CRITICAL FIX: Object arrays store void* pointers, not JavaValue structs */
                memcpy(array_data(new_arr), array_data(arr), arr->length * sizeof(void*));
                arr = new_arr;
                /* Update elementData field */
                JavaValue arr_val = { .ref = arr };
                native_set_field_value(vec, "elementData", arr_val);
            }
        }
        if (count < arr->length) {
            /* CRITICAL FIX: Use array_set_ref for object arrays */
            array_set_ref(arr, count, element.ref);
            JavaValue count_val = { .i = count + 1 };
            native_set_field_value(vec, "elementCount", count_val);
        }
    }
    
    return NATIVE_RETURN_INT(1);
}

/* addElement is the same as add but returns void */
static JavaValue native_vector_add_element(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* vec = (JavaObject*)args[0].ref;
    JavaValue element = args[1];
    
    NATIVE_DEBUG("addElement: obj=%p, element=%p", (void*)vec, element.ref);
    
    if (!vec) return NATIVE_RETURN_VOID();
    
    /* ИСПРАВЛЕНО: Используем native_get/set_field_value */
    JavaArray* arr = (JavaArray*)native_get_field_value(vec, "elementData").ref;
    int count = native_get_field_value(vec, "elementCount").i;
    
    /* Auto-initialize if elementData is NULL (constructor might not have been called) */
    if (!arr) {
        NATIVE_DEBUG("addElement: auto-initializing elementData");
        arr = jvm_new_array(jvm, DESC_OBJECT, 10, NULL);
        JavaValue arr_val = { .ref = arr };
        native_set_field_value(vec, "elementData", arr_val);
    }
    
    if (arr) {
        /* Grow array if needed */
        if (count >= arr->length) {
            int new_size = arr->length * 2 + 1;
            JavaArray* new_arr = jvm_new_array(jvm, DESC_OBJECT, new_size, NULL);
            if (new_arr) {
                /* CRITICAL FIX: Object arrays store void* pointers, not JavaValue structs */
                memcpy(array_data(new_arr), array_data(arr), arr->length * sizeof(void*));
                arr = new_arr;
                /* Update elementData field */
                JavaValue arr_val = { .ref = arr };
                native_set_field_value(vec, "elementData", arr_val);
            }
        }
        if (count < arr->length) {
            /* CRITICAL FIX: Use array_set_ref for object arrays */
            array_set_ref(arr, count, element.ref);
            JavaValue count_val = { .i = count + 1 };
            native_set_field_value(vec, "elementCount", count_val);
            NATIVE_DEBUG("addElement: stored element at index %d, new count=%d", count, count + 1);
        }
    } else {
        NATIVE_DEBUG("addElement: ERROR - arr is NULL");
    }
    
    return NATIVE_RETURN_VOID();
}

/* Vector.insertElementAt(Object obj, int index) - insert at specific position */
static JavaValue native_vector_insertElementAt(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* vec = (JavaObject*)args[0].ref;
    JavaValue element = args[1];
    jint index = args[2].i;
    
    NATIVE_DEBUG("insertElementAt: obj=%p, index=%d, element=%p", 
            (void*)vec, index, element.ref);
    
    if (!vec) return NATIVE_RETURN_VOID();

    /* ИСПРАВЛЕНО: Используем native_get/set_field_value */
    JavaArray* arr = (JavaArray*)native_get_field_value(vec, "elementData").ref;
    int count = native_get_field_value(vec, "elementCount").i;

    /* v34.8 FIX (Bounce Tales): стаб-конструктор java/util/Vector.<init> —
     * пустой трамплин (super(); return), он ЗАТЕНЯЕТ нативный
     * native_vector_init, поэтому elementData остаётся NULL.
     * addElement самовосстанавливается (auto-init), а insertElementAt —
     * НЕТ: вставки тихо пропадали, k() Bounce Tales строила пустой список
     * работ, помечала модули загруженными и игра грузилась без спрайтов
     * (белый экран, AIOOBE o.a PC=316 index=-1, шторм перезапусков потока).
     * Лечим симметрично addElement: */
    if (!arr) {
        NATIVE_DEBUG("insertElementAt: auto-initializing elementData");
        arr = jvm_new_array(jvm, DESC_OBJECT, 10, NULL);
        if (arr) {
            JavaValue arr_val = { .ref = arr };
            native_set_field_value(vec, "elementData", arr_val);
        }
    }

    if (!arr) return NATIVE_RETURN_VOID();
    
    /* Bounds check */
    if (index < 0 || index > count) {
        /* Should throw ArrayIndexOutOfBoundsException */
        return NATIVE_RETURN_VOID();
    }
    
    /* Grow array if needed */
    if (count >= arr->length) {
        JavaArray* new_arr = jvm_new_array(jvm, DESC_OBJECT, arr->length * 2 + 1, NULL);
        if (new_arr) {
            memcpy(array_data(new_arr), array_data(arr), arr->length * sizeof(void*));
            arr = new_arr;
            JavaValue arr_val = { .ref = arr };
            native_set_field_value(vec, "elementData", arr_val);
        }
    }
    
    /* Shift elements to make room */
    if (index < count) {
        void** data = (void**)array_data(arr);
        for (int i = count; i > index; i--) {
            data[i] = data[i - 1];
        }
    }
    
    /* Insert element */
    array_set_ref(arr, index, element.ref);
    JavaValue count_val = { .i = count + 1 };
    native_set_field_value(vec, "elementCount", count_val);
    
    return NATIVE_RETURN_VOID();
}

/* Vector.setElementAt(Object obj, int index) - replace element at position */
static JavaValue native_vector_setElementAt(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* vec = (JavaObject*)args[0].ref;
    JavaValue element = args[1];
    jint index = args[2].i;
    
    if (!vec) return NATIVE_RETURN_VOID();
    
    /* ИСПРАВЛЕНО: Используем native_get_field_value */
    JavaArray* arr = (JavaArray*)native_get_field_value(vec, "elementData").ref;
    int count = native_get_field_value(vec, "elementCount").i;
    
    /* v34.8 FIX: auto-init как в addElement/insertElementAt — стаб-конструктор
     * Vector не инициализирует поля (см. комментарий в insertElementAt). */
    if (!arr) {
        arr = jvm_new_array(jvm, DESC_OBJECT, 10, NULL);
        if (arr) {
            JavaValue arr_val = { .ref = arr };
            native_set_field_value(vec, "elementData", arr_val);
        }
    }
    
    if (arr && index >= 0 && index < count) {
        array_set_ref(arr, index, element.ref);
    }
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_vector_get(JVM* jvm, JavaThread* thread,
                                   JavaValue* args, int arg_count) {
    (void)arg_count;
    JavaObject* vec = (JavaObject*)args[0].ref;
    jint index = args[1].i;
    
    if (!vec) {
        NATIVE_DEBUG("Vector.get: null vector");
        return NATIVE_RETURN_NULL();
    }
    
    /* ИСПРАВЛЕНО: Используем native_get_field_value */
    JavaArray* arr = (JavaArray*)native_get_field_value(vec, "elementData").ref;
    int count = native_get_field_value(vec, "elementCount").i;
    
    NATIVE_DEBUG("Vector.get: index=%d, count=%d, arr=%p", index, count, (void*)arr);
    
    if (index < 0 || index >= count) {
        /* Throw ArrayIndexOutOfBoundsException */
        native_throw_aioobe(jvm, thread, index);
        return NATIVE_RETURN_NULL();
    }
    
    if (arr) {
        /* CRITICAL FIX: Use array_get_ref for object arrays */
        void* elem = array_get_ref(arr, index);
        NATIVE_DEBUG("Vector.get: returning element at index %d = %p", index, elem);
        return NATIVE_RETURN_OBJECT(elem);
    }
    
    NATIVE_DEBUG("Vector.get: null array");
    return NATIVE_RETURN_NULL();
}

static JavaValue native_vector_size(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* vec = (JavaObject*)args[0].ref;
    
    if (!vec) return NATIVE_RETURN_INT(0);
    
    /* ИСПРАВЛЕНО: Используем native_get_field_value */
    jint count = native_get_field_value(vec, "elementCount").i;
    return NATIVE_RETURN_INT(count);
}

static JavaValue native_vector_isEmpty(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* vec = (JavaObject*)args[0].ref;
    
    if (!vec) return NATIVE_RETURN_INT(1);
    
    /* ИСПРАВЛЕНО: Используем native_get_field_value */
    return NATIVE_RETURN_INT(native_get_field_value(vec, "elementCount").i == 0 ? 1 : 0);
}

/* Vector.removeElement(Object obj) - removes first occurrence of element */
static bool vector_objects_equal(JavaObject* a, JavaObject* b) {
    /* Same reference */
    if (a == b) return true;
    /* Both null */
    if (!a && !b) return true;
    /* One null */
    if (!a || !b) return false;
    
    /* Check if both are Strings - use string_equals */
    /* Native strings (OBJ_TYPE_STRING) have clazz = NULL, so check type first */
    extern void* g_heap_start;
    extern void* g_heap_end;
    
    bool a_is_string = false;
    bool b_is_string = false;
    
    /* Check if a is a string */
    if ((void*)a >= g_heap_start && (void*)a < g_heap_end) {
        GCObjectHeader* a_header = (GCObjectHeader*)((uint8_t*)a - sizeof(GCObjectHeader));
        if (a_header->type == OBJ_TYPE_STRING) {
            a_is_string = true;
        } else if (a_header->type == OBJ_TYPE_OBJECT) {
            JavaClass* a_class = a->header.clazz;
            if (a_class && a_class->class_name && 
                strcmp(a_class->class_name, "java/lang/String") == 0) {
                a_is_string = true;
            }
        }
    }
    
    /* Check if b is a string */
    if ((void*)b >= g_heap_start && (void*)b < g_heap_end) {
        GCObjectHeader* b_header = (GCObjectHeader*)((uint8_t*)b - sizeof(GCObjectHeader));
        if (b_header->type == OBJ_TYPE_STRING) {
            b_is_string = true;
        } else if (b_header->type == OBJ_TYPE_OBJECT) {
            JavaClass* b_class = b->header.clazz;
            if (b_class && b_class->class_name && 
                strcmp(b_class->class_name, "java/lang/String") == 0) {
                b_is_string = true;
            }
        }
    }
    
    /* If both are strings, compare them using string_equals */
    if (a_is_string && b_is_string) {
        return string_equals((JavaString*)a, (JavaString*)b);
    }
    
    /* For other objects, use reference comparison (default Object.equals) */
    return false;
}

static JavaValue native_vector_removeElement(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* vec = (JavaObject*)args[0].ref;
    JavaObject* elem = (JavaObject*)args[1].ref;
    
    if (!vec) return NATIVE_RETURN_INT(0);
    
    JavaArray* arr = (JavaArray*)native_get_field_value(vec, "elementData").ref;
    int count = native_get_field_value(vec, "elementCount").i;
    
    if (!arr || count == 0) return NATIVE_RETURN_INT(0);
    
    /* Search for element using equals comparison */
    void** data = (void**)array_data(arr);
    for (int i = 0; i < count; i++) {
        if (vector_objects_equal((JavaObject*)data[i], elem)) {
            /* Found - shift remaining elements left */
            for (int j = i; j < count - 1; j++) {
                data[j] = data[j + 1];
            }
            data[count - 1] = NULL;
            
            /* Update count */
            int new_count = count - 1;
            JavaValue count_val = { .i = new_count };
            native_set_field_value(vec, "elementCount", count_val);
            
            return NATIVE_RETURN_INT(1);  /* true - element was removed */
        }
    }
    
    return NATIVE_RETURN_INT(0);  /* false - element not found */
}

/* Vector.removeAllElements() - removes all elements */
static JavaValue native_vector_removeAllElements(JVM* jvm, JavaThread* thread,
                                                  JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* vec = (JavaObject*)args[0].ref;
    
    if (!vec) return NATIVE_RETURN_VOID();
    
    JavaArray* arr = (JavaArray*)native_get_field_value(vec, "elementData").ref;
    int count = native_get_field_value(vec, "elementCount").i;
    
    if (arr && count > 0) {
        /* Clear all elements */
        void** data = (void**)array_data(arr);
        for (int i = 0; i < count; i++) {
            data[i] = NULL;
        }
    }
    
    /* Set count to 0 */
    JavaValue count_val = { .i = 0 };
    native_set_field_value(vec, "elementCount", count_val);
    
    return NATIVE_RETURN_VOID();
}

/* Vector.contains(Object elem) - checks if element exists */
static JavaValue native_vector_contains(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* vec = (JavaObject*)args[0].ref;
    JavaObject* elem = (JavaObject*)args[1].ref;
    
    if (!vec) return NATIVE_RETURN_INT(0);
    
    JavaArray* arr = (JavaArray*)native_get_field_value(vec, "elementData").ref;
    int count = native_get_field_value(vec, "elementCount").i;
    
    if (!arr || count == 0) return NATIVE_RETURN_INT(0);
    
    /* Search for element using equals comparison */
    void** data = (void**)array_data(arr);
    for (int i = 0; i < count; i++) {
        if (vector_objects_equal((JavaObject*)data[i], elem)) {
            return NATIVE_RETURN_INT(1);  /* true - found */
        }
    }
    
    return NATIVE_RETURN_INT(0);  /* false - not found */
}

/* Vector.copyInto(Object[] array) - copies elements into array */
static JavaValue native_vector_copyInto(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* vec = (JavaObject*)args[0].ref;
    JavaArray* dest_array = (JavaArray*)args[1].ref;
    
    if (!vec || !dest_array) {
        NATIVE_DEBUG("copyInto: null parameter (vec=%p, array=%p)", 
                (void*)vec, (void*)dest_array);
        return NATIVE_RETURN_VOID();
    }
    
    /* ИСПРАВЛЕНО: Используем native_get_field_value */
    JavaArray* src_array = (JavaArray*)native_get_field_value(vec, "elementData").ref;
    int element_count = native_get_field_value(vec, "elementCount").i;
    
    if (!src_array) {
        NATIVE_DEBUG("copyInto: no elementData field found");
        return NATIVE_RETURN_VOID();
    }
    
    int copy_count = element_count;
    if (copy_count > (int)dest_array->length) {
        copy_count = dest_array->length;
    }
    
    NATIVE_DEBUG("copyInto: copying %d elements (vector size=%d, array len=%u)",
            copy_count, element_count, dest_array->length);
    
    /* Copy elements from source array to destination array using helper functions */
    for (int i = 0; i < copy_count; i++) {
        void* ref = array_get_ref(src_array, i);
        array_set_ref(dest_array, i, ref);
    }
    
    return NATIVE_RETURN_VOID();
}

/* Vector.elements() - returns an Enumeration over the vector elements */
extern JavaClass* get_or_create_stub_class(JVM* jvm, const char* class_name);

static JavaValue native_vector_elements(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* vec = (JavaObject*)args[0].ref;
    
    if (!vec) {
        NATIVE_DEBUG("elements: null vector");
        return NATIVE_RETURN_NULL();
    }
    
    /* ИСПРАВЛЕНО: Используем native_get_field_value */
    JavaArray* src_array = (JavaArray*)native_get_field_value(vec, "elementData").ref;
    int element_count = native_get_field_value(vec, "elementCount").i;
    
    NATIVE_DEBUG("elements: src_array=%p, element_count=%d", 
            (void*)src_array, element_count);
    
    if (!src_array) {
        NATIVE_DEBUG("elements: WARNING - elementData is NULL!");
        /* Return an empty enumeration */
        element_count = 0;
    }
    
    /* ИСПРАВЛЕНО: Используем конкретный класс вместо интерфейса Enumeration.
     * java/util/Enumeration - это интерфейс, и создание объекта с ним вызывает проблемы.
     * Создаем специальный класс VectorEnumeration.
     */
    JavaClass* iter_class = get_or_create_stub_class(jvm, "java/util/VectorEnumeration");
    if (!iter_class) {
        ERROR_LOG("Failed to create VectorEnumeration class");
        return NATIVE_RETURN_NULL();
    }
    
    /* Ensure the class has fields for enumeration state */
    if (iter_class->fields_count == 0) {
        iter_class->fields = (JavaField*)calloc(3, sizeof(JavaField));
        iter_class->fields[0].name = strdup("elements");
        iter_class->fields[0].descriptor = strdup("[Ljava/lang/Object;");
        iter_class->fields[1].name = strdup("count");
        iter_class->fields[1].descriptor = strdup("I");
        iter_class->fields[2].name = strdup("index");
        iter_class->fields[2].descriptor = strdup("I");
        iter_class->fields_count = 3;
        iter_class->instance_size = sizeof(ObjectHeader) + 3 * sizeof(JavaValue);
        
        /* Mark as initialized to avoid <clinit> calls */
        iter_class->initialized = true;
    }
    
    JavaObject* iter_obj = jvm_new_object(jvm, iter_class);
    if (iter_obj && iter_class->fields_count >= 3) {
        /* ИСПРАВЛЕНО: Используем native_set_field_value для установки полей */
        JavaValue arr_val = { .ref = src_array };
        JavaValue count_val = { .i = element_count };
        JavaValue index_val = { .i = 0 };
        native_set_field_value(iter_obj, "elements", arr_val);
        native_set_field_value(iter_obj, "count", count_val);
        native_set_field_value(iter_obj, "index", index_val);
    }
    
    NATIVE_DEBUG("elements: returning enumeration object %p with %d elements (class=%s)", 
            (void*)iter_obj, element_count, iter_class->class_name);
    return NATIVE_RETURN_OBJECT(iter_obj);
}

/* Vector.indexOf(Object) - find first occurrence of element */
static JavaValue native_vector_indexOf(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* vec = (JavaObject*)args[0].ref;
    JavaObject* elem = (JavaObject*)args[1].ref;
    
    if (!vec) return NATIVE_RETURN_INT(-1);
    
    JavaArray* arr = (JavaArray*)native_get_field_value(vec, "elementData").ref;
    int count = native_get_field_value(vec, "elementCount").i;
    
    if (!arr || count == 0) return NATIVE_RETURN_INT(-1);
    
    void** data = (void**)array_data(arr);
    for (int i = 0; i < count; i++) {
        if (vector_objects_equal((JavaObject*)data[i], elem)) {
            return NATIVE_RETURN_INT(i);
        }
    }
    
    return NATIVE_RETURN_INT(-1);
}

/* Vector.indexOf(Object, int) - find first occurrence of element starting from index */
static JavaValue native_vector_indexOf_from(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* vec = (JavaObject*)args[0].ref;
    JavaObject* elem = (JavaObject*)args[1].ref;
    jint start_index = args[2].i;
    
    if (!vec) return NATIVE_RETURN_INT(-1);
    
    JavaArray* arr = (JavaArray*)native_get_field_value(vec, "elementData").ref;
    int count = native_get_field_value(vec, "elementCount").i;
    
    if (!arr || count == 0 || start_index < 0) return NATIVE_RETURN_INT(-1);
    
    if (start_index >= count) return NATIVE_RETURN_INT(-1);
    
    void** data = (void**)array_data(arr);
    for (int i = start_index; i < count; i++) {
        if (vector_objects_equal((JavaObject*)data[i], elem)) {
            return NATIVE_RETURN_INT(i);
        }
    }
    
    return NATIVE_RETURN_INT(-1);
}

/* Vector.lastIndexOf(Object) - find last occurrence of element */
static JavaValue native_vector_lastIndexOf(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* vec = (JavaObject*)args[0].ref;
    JavaObject* elem = (JavaObject*)args[1].ref;
    
    if (!vec) return NATIVE_RETURN_INT(-1);
    
    JavaArray* arr = (JavaArray*)native_get_field_value(vec, "elementData").ref;
    int count = native_get_field_value(vec, "elementCount").i;
    
    if (!arr || count == 0) return NATIVE_RETURN_INT(-1);
    
    void** data = (void**)array_data(arr);
    for (int i = count - 1; i >= 0; i--) {
        if (vector_objects_equal((JavaObject*)data[i], elem)) {
            return NATIVE_RETURN_INT(i);
        }
    }
    
    return NATIVE_RETURN_INT(-1);
}

/* Vector.removeElementAt(int) - remove element at index */
/* (superseded firstElement/lastElement drafts removed - the *_spec
 * variants below are the registered implementations with the correct
 * NoSuchElementException contract.) */
static JavaValue native_vector_removeElementAt(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* vec = (JavaObject*)args[0].ref;
    jint index = args[1].i;
    
    if (!vec) return NATIVE_RETURN_VOID();
    
    JavaArray* arr = (JavaArray*)native_get_field_value(vec, "elementData").ref;
    int count = native_get_field_value(vec, "elementCount").i;
    
    if (!arr) return NATIVE_RETURN_VOID();
    
    /* Bounds check */
    if (index < 0 || index >= count) {
        /* Should throw ArrayIndexOutOfBoundsException */
        return NATIVE_RETURN_VOID();
    }
    
    void** data = (void**)array_data(arr);
    
    /* Shift elements left */
    for (int i = index; i < count - 1; i++) {
        data[i] = data[i + 1];
    }
    data[count - 1] = NULL;
    
    /* Update count */
    JavaValue count_val = { .i = count - 1 };
    native_set_field_value(vec, "elementCount", count_val);
    
    return NATIVE_RETURN_VOID();
}

/* Vector.trimToSize() - trim capacity to size */
static JavaValue native_vector_trimToSize(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* vec = (JavaObject*)args[0].ref;
    
    if (!vec) return NATIVE_RETURN_VOID();
    
    JavaArray* arr = (JavaArray*)native_get_field_value(vec, "elementData").ref;
    int count = native_get_field_value(vec, "elementCount").i;
    
    if (!arr || count < 0 || (long long)arr->length == (long long)count) {
        /* Already at correct size */
        return NATIVE_RETURN_VOID();
    }
    
    /* Create new array with exact size */
    JavaArray* new_arr = jvm_new_array(jvm, DESC_OBJECT, count, NULL);
    if (new_arr && count > 0) {
        memcpy(array_data(new_arr), array_data(arr), count * sizeof(void*));
        JavaValue arr_val = { .ref = new_arr };
        native_set_field_value(vec, "elementData", arr_val);
    }
    
    return NATIVE_RETURN_VOID();
}

/* Vector.ensureCapacity(int) - ensure minimum capacity */
static JavaValue native_vector_ensureCapacity(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* vec = (JavaObject*)args[0].ref;
    jint min_capacity = args[1].i;
    
    if (!vec || min_capacity <= 0) return NATIVE_RETURN_VOID();
    
    JavaArray* arr = (JavaArray*)native_get_field_value(vec, "elementData").ref;
    int count = native_get_field_value(vec, "elementCount").i;
    
    if (!arr) {
        arr = jvm_new_array(jvm, DESC_OBJECT, min_capacity, NULL);
        if (arr) {
            JavaValue arr_val = { .ref = arr };
            native_set_field_value(vec, "elementData", arr_val);
        }
        return NATIVE_RETURN_VOID();
    }
    
    if ((int)arr->length < min_capacity) {
        JavaArray* new_arr = jvm_new_array(jvm, DESC_OBJECT, min_capacity, NULL);
        if (new_arr) {
            memcpy(array_data(new_arr), array_data(arr), count * sizeof(void*));
            JavaValue arr_val = { .ref = new_arr };
            native_set_field_value(vec, "elementData", arr_val);
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* Vector.setSize(int) - set new size */
static JavaValue native_vector_setSize(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* vec = (JavaObject*)args[0].ref;
    jint new_size = args[1].i;
    
    if (!vec || new_size < 0) return NATIVE_RETURN_VOID();
    
    JavaArray* arr = (JavaArray*)native_get_field_value(vec, "elementData").ref;
    int count = native_get_field_value(vec, "elementCount").i;
    
    if (new_size == count) {
        return NATIVE_RETURN_VOID();
    }
    
    if (!arr) {
        arr = jvm_new_array(jvm, DESC_OBJECT, new_size > 10 ? new_size : 10, NULL);
        if (arr) {
            JavaValue arr_val = { .ref = arr };
            native_set_field_value(vec, "elementData", arr_val);
        }
    } else if ((int)arr->length < new_size) {
        /* Need to grow */
        JavaArray* new_arr = jvm_new_array(jvm, DESC_OBJECT, new_size, NULL);
        if (new_arr) {
            memcpy(array_data(new_arr), array_data(arr), count * sizeof(void*));
            JavaValue arr_val = { .ref = new_arr };
            native_set_field_value(vec, "elementData", arr_val);
            arr = new_arr;
        }
    } else if (new_size < count) {
        /* Shrinking - clear excess elements */
        void** data = (void**)array_data(arr);
        for (int i = new_size; i < count; i++) {
            data[i] = NULL;
        }
    }
    
    /* Update count */
    JavaValue count_val = { .i = new_size };
    native_set_field_value(vec, "elementCount", count_val);
    
    return NATIVE_RETURN_VOID();
}

void init_java_util_vector(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"java/util/Vector", "<init>", "()V", native_vector_init},
        {"java/util/Vector", "<init>", "(I)V", native_vector_init_int},
        {"java/util/Vector", "<init>", "(II)V", native_vector_init_int_int},
        {"java/util/Vector", "add", "(Ljava/lang/Object;)Z", native_vector_add},
        {"java/util/Vector", "addElement", "(Ljava/lang/Object;)V", native_vector_add_element},
        {"java/util/Vector", "insertElementAt", "(Ljava/lang/Object;I)V", native_vector_insertElementAt},
        {"java/util/Vector", "setElementAt", "(Ljava/lang/Object;I)V", native_vector_setElementAt},
        {"java/util/Vector", "get", "(I)Ljava/lang/Object;", native_vector_get},
        {"java/util/Vector", "elementAt", "(I)Ljava/lang/Object;", native_vector_get},
        {"java/util/Vector", "size", "()I", native_vector_size},
        {"java/util/Vector", "isEmpty", "()Z", native_vector_isEmpty},
        {"java/util/Vector", "removeElement", "(Ljava/lang/Object;)Z", native_vector_removeElement},
        {"java/util/Vector", "removeAllElements", "()V", native_vector_removeAllElements},
        {"java/util/Vector", "contains", "(Ljava/lang/Object;)Z", native_vector_contains},
        {"java/util/Vector", "copyInto", "([Ljava/lang/Object;)V", native_vector_copyInto},
        {"java/util/Vector", "elements", "()Ljava/util/Enumeration;", native_vector_elements},
        {"java/util/Vector", "indexOf", "(Ljava/lang/Object;)I", native_vector_indexOf},
        {"java/util/Vector", "indexOf", "(Ljava/lang/Object;I)I", native_vector_indexOf_from},
        {"java/util/Vector", "lastIndexOf", "(Ljava/lang/Object;)I", native_vector_lastIndexOf},
        {"java/util/Vector", "firstElement", "()Ljava/lang/Object;", native_vector_firstElement_spec},
        {"java/util/Vector", "lastElement", "()Ljava/lang/Object;", native_vector_lastElement_spec},
        {"java/util/Vector", "capacity", "()I", native_vector_capacity},
        {"java/util/Vector", "toString", "()Ljava/lang/String;", native_vector_toString},
        {"java/util/Vector", "removeElementAt", "(I)V", native_vector_removeElementAt},
        {"java/util/Vector", "trimToSize", "()V", native_vector_trimToSize},
        {"java/util/Vector", "ensureCapacity", "(I)V", native_vector_ensureCapacity},
        {"java/util/Vector", "setSize", "(I)V", native_vector_setSize},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
    NATIVE_DEBUG("Registered java/util/Vector native methods");
}

/* Enumeration methods - used by Vector.elements() */
static JavaValue native_enumeration_hasMoreElements(JVM* jvm, JavaThread* thread,
                                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* enum_obj = (JavaObject*)args[0].ref;
    
    if (!enum_obj) {
        return NATIVE_RETURN_INT(0);
    }
    
    /* ИСПРАВЛЕНО: используем native_get_field_value */
    JavaArray* elements = (JavaArray*)native_get_field_value(enum_obj, "elements").ref;
    jint count = native_get_field_value(enum_obj, "count").i;
    jint index = native_get_field_value(enum_obj, "index").i;
    
    jboolean result = (elements && index < count) ? JNI_TRUE : JNI_FALSE;
    return NATIVE_RETURN_INT(result);
}

static JavaValue native_enumeration_nextElement(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* enum_obj = (JavaObject*)args[0].ref;
    
    if (!enum_obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_NULL();
    }
    
    /* ИСПРАВЛЕНО: используем native_get_field_value */
    JavaArray* elements = (JavaArray*)native_get_field_value(enum_obj, "elements").ref;
    jint count = native_get_field_value(enum_obj, "count").i;
    jint index = native_get_field_value(enum_obj, "index").i;
    
    JavaClass* clazz = enum_obj->header.clazz;
    (void)clazz;  /* debug: element type kept for future type checks */
    
    if (!elements || index >= count) {
        /* v34.4 FIX: throw the SPEC exception. The old "compatibility" silent
         * NULL return poisoned game state downstream: callers like
         *   renderer = (Renderer) enum.nextElement();   // unguarded cast-use
         * got NULL and crashed later with an unrelated NPE (user log:
         * "invokeinterface: NULL object reference for j.a(...) at b.paint").
         * Vector.firstElement/lastElement already throw NoSuchElementException,
         * so this also makes behaviour consistent across the container API.
         * Null-RECEIVER compat (invokeinterface on NULL Enumeration) is
         * unaffected - that path lives in opcodes.c. */
        {
            static int enum_nsee_count = 0;
            if (enum_nsee_count < 5) {
                JavaThread* cur = thread ? thread : jvm_current_thread(jvm);
                JavaFrame* f = cur && cur->current_frame ? cur->current_frame : NULL;
                if (f && f->clazz && f->clazz->class_name) {
                    int invoke_pc = (int)f->pc - 3;
                    if (invoke_pc < 0) invoke_pc = (int)f->pc;
                    fprintf(stderr,
                            "[ENUM] nextElement on exhausted/empty enumeration (count=%d, index=%d) at %s.%s PC=%d - throwing NoSuchElementException\n",
                            count, index, f->clazz->class_name,
                            (f->method && f->method->name) ? f->method->name : "?",
                            invoke_pc);
                } else {
                    fprintf(stderr,
                            "[ENUM] nextElement on exhausted/empty enumeration (count=%d, index=%d) - throwing NoSuchElementException\n",
                            count, index);
                }
                enum_nsee_count++;
            }
        }
        jvm_throw_by_name(jvm, "java/util/NoSuchElementException", "Enumeration exhausted");
        if (thread) thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_NULL();
    }
    
    /* Get the element at current index using array_get_ref for object arrays */
    void* elem_ref = array_get_ref(elements, index);
    
    /* ИСПРАВЛЕНО: используем native_set_field_value для обновления index */
    JavaValue new_index = { .i = index + 1 };
    native_set_field_value(enum_obj, "index", new_index);
    
    /* Debug: check the element */
    if (elem_ref) {
        JavaObject* elem_obj = (JavaObject*)elem_ref;
        (void)elem_obj;
    } else {
    }
    
    return NATIVE_RETURN_OBJECT(elem_ref);
}

void init_java_util_enumeration(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"java/util/Enumeration", "hasMoreElements", "()Z", native_enumeration_hasMoreElements},
        {"java/util/Enumeration", "nextElement", "()Ljava/lang/Object;", native_enumeration_nextElement},
        /* ИСПРАВЛЕНО: Also register for VectorEnumeration class */
        {"java/util/VectorEnumeration", "hasMoreElements", "()Z", native_enumeration_hasMoreElements},
        {"java/util/VectorEnumeration", "nextElement", "()Ljava/lang/Object;", native_enumeration_nextElement},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
    NATIVE_DEBUG("Registered java/util/Enumeration and VectorEnumeration native methods");
}

/*
 * java.util.Hashtable native methods
 * 
 * ИСПРАВЛЕНО: Полноценная реализация Hashtable с поддержкой Integer/String/Long ключей
 * 
 * Проблема race condition: Игра вызывает get() до того, как put() завершился.
 * Решение: Возвращаем null-safe значения вместо NPE.
 */

/* Hashtable entry structure - stored as pairs in object array */
typedef struct {
    JavaObject* key;
    JavaObject* value;
    jint hash;
} HashtableEntry;

/* Native peer structure for Hashtable */
typedef struct {
    JavaObject* ht_obj;     /* Back-reference to Java Hashtable object for lookup */
    HashtableEntry* entries;
    int capacity;
    int count;
} HashtablePeer;

/* Helper: compute hash code for any key object */
static jint hashtable_compute_hash(JavaObject* key) {
    if (!key) return 0;
    
    JavaClass* key_class = key->header.clazz;
    if (!key_class || !key_class->class_name) return (jint)(intptr_t)key;
    
    /* Integer: use the int value directly */
    if (strcmp(key_class->class_name, "java/lang/Integer") == 0) {
        return native_get_field_value(key, "value").i;
    }
    
    /* Long: use the long value (xor high and low bits) */
    if (strcmp(key_class->class_name, "java/lang/Long") == 0) {
        jlong val = native_get_field_value(key, "value").j;
        return (jint)(val ^ (val >> 32));
    }
    
    /* v58: Byte/Short/Character/Boolean \u2014 hashCode() == value() for all
     * (JLS), so the table must hash them the same way. Previously fell
     * through to the identity hash: two equal keys never collided into
     * the same bucket state, breaking put/get round-trips. */
    if (strcmp(key_class->class_name, "java/lang/Byte") == 0 ||
        strcmp(key_class->class_name, "java/lang/Short") == 0 ||
        strcmp(key_class->class_name, "java/lang/Character") == 0 ||
        strcmp(key_class->class_name, "java/lang/Boolean") == 0) {
        return native_get_field_value(key, "value").i;
    }

    /* String: use string hash */
    if (strcmp(key_class->class_name, "java/lang/String") == 0) {
        return string_hash((JavaString*)key);
    }
    
    /* Default: use object hashcode */
    return key->header.hashcode ? key->header.hashcode : (jint)(intptr_t)key;
}

/* Helper: compare two key objects for equality */
static bool hashtable_keys_equal(JavaObject* key1, JavaObject* key2) {
    if (key1 == key2) return true;
    if (!key1 || !key2) return false;
    
    JavaClass* class1 = key1->header.clazz;
    JavaClass* class2 = key2->header.clazz;
    
    if (!class1 || !class2) return false;
    
    /* Different classes can't be equal (except both are Numbers with same value) */
    const char* name1 = class1->class_name;
    const char* name2 = class2->class_name;
    
    if (!name1 || !name2) return false;
    
    /* Integer comparison */
    if (strcmp(name1, "java/lang/Integer") == 0 && strcmp(name2, "java/lang/Integer") == 0) {
        jint val1 = native_get_field_value(key1, "value").i;
        jint val2 = native_get_field_value(key2, "value").i;
        return val1 == val2;
    }
    
    /* Long comparison */
    if (strcmp(name1, "java/lang/Long") == 0 && strcmp(name2, "java/lang/Long") == 0) {
        jlong val1 = native_get_field_value(key1, "value").j;
        jlong val2 = native_get_field_value(key2, "value").j;
        return val1 == val2;
    }
    
    /* v58: Byte/Short/Character/Boolean comparison (see compute_hash) */
    if ((strcmp(name1, "java/lang/Byte") == 0 || strcmp(name1, "java/lang/Short") == 0 ||
         strcmp(name1, "java/lang/Character") == 0 || strcmp(name1, "java/lang/Boolean") == 0) &&
        strcmp(name1, name2) == 0) {
        return native_get_field_value(key1, "value").i ==
               native_get_field_value(key2, "value").i;
    }

    /* String comparison */
    if (strcmp(name1, "java/lang/String") == 0 && strcmp(name2, "java/lang/String") == 0) {
        return string_equals((JavaString*)key1, (JavaString*)key2);
    }
    
    return false;
}

/* v36.33 [HT-KEY-VALIDATE]: a Hashtable entry whose key/value was swept by
 * the GC — or left stale by a session teardown — must never be
 * dereferenced: hashtable_keys_equal reads key->header.clazz, and a reused
 * arena block turns that read into a jump through string bytes. Field
 * crashes decoded exactly like that: Invalid memory access
 * 0x745364726F636572 (ASCII "recordSt") and 0x736D722E65726F74 (ASCII
 * "tore.rms") — both are offsets into the game's own "recordStore.rms" RMS
 * file name string that came to occupy the freed block. ASAN repro: Temple
 * Rush 3D 2, session 2, SEGV in hashtable_keys_equal. Purge-and-skip
 * converts any remaining staleness into clean "entry not found" behavior
 * instead of a crash (defense-in-depth next to [HT-PEER-SESSION-RESET] and
 * [HT-PEER-SWEEP]). Returns true when the slot is NOW empty (was empty or
 * was purged here), false when the entry is alive. */
static bool hashtable_entry_purge_if_stale(HashtableEntry* entry) {
    if (!entry->key && !entry->value) return true;  /* already empty */
    bool stale = (entry->key && !heap_java_object_valid(entry->key)) ||
                 (entry->value && !heap_java_object_valid(entry->value));
    if (!stale) return false;
    NATIVE_DEBUG("Hashtable: purged stale entry key=%p value=%p",
                 (void*)entry->key, (void*)entry->value);
    entry->key = NULL;
    entry->value = NULL;
    entry->hash = 0;
    return true;
}

/* Global hashtable peer storage (simple approach)
 * CRITICAL: These must NOT be static because the GC in heap.c needs to
 * access them to mark objects stored in native Hashtable entries.
 * This fixes the use-after-free bug where strings stored as Hashtable keys
 * were being garbage collected because GC didn't see them.
 */
#define MAX_HASHTABLES 4096
HashtablePeer g_hashtables[MAX_HASHTABLES];
int g_hashtable_count = 0;

/* Track which peers are alive (not freed) */
bool g_hashtable_peer_alive[MAX_HASHTABLES] = {false};

/* Helper function to find a free peer slot */
static int hashtable_find_free_slot(void) {
    for (int i = 0; i < MAX_HASHTABLES; i++) {
        if (!g_hashtable_peer_alive[i]) {
            return i;
        }
    }
    return -1;
}

/* Get or create peer for hashtable */
static HashtablePeer* hashtable_get_peer(JavaObject* ht) {
    /* First, search for existing peer by object pointer 
     * This is more reliable than using threshold field which Java may overwrite */
    for (int i = 0; i < g_hashtable_count; i++) {
        if (g_hashtable_peer_alive[i] && g_hashtables[i].ht_obj == ht) {
            return &g_hashtables[i];
        }
    }
    
    /* Find a free slot */
    int idx = hashtable_find_free_slot();
    if (idx < 0) {
        ERROR_LOG("No free Hashtable peer slots");
        return NULL;
    }
    
    /* Create new peer */
    g_hashtables[idx].ht_obj = ht;  /* Store object pointer for lookup */
    g_hashtables[idx].capacity = 16;
    g_hashtables[idx].count = 0;
    g_hashtables[idx].entries = (HashtableEntry*)calloc(16, sizeof(HashtableEntry));
    g_hashtable_peer_alive[idx] = true;
    
    /* Update g_hashtable_count to track highest used index */
    if (idx >= g_hashtable_count) {
        g_hashtable_count = idx + 1;
    }
    
    return &g_hashtables[idx];
}

/* Get peer for hashtable - uses object address for lookup */
static HashtablePeer* hashtable_ensure_peer(JavaObject* ht) {
    if (!ht) return NULL;
    
    /* Always search by object pointer first - this is the most reliable method */
    for (int i = 0; i < g_hashtable_count; i++) {
        if (g_hashtable_peer_alive[i] && g_hashtables[i].ht_obj == ht) {
            return &g_hashtables[i];
        }
    }
    
    /* Not found - create new peer */
    return hashtable_get_peer(ht);
}

/* Free native peer for a Hashtable - called by GC when Hashtable is collected */
void hashtable_free_peer(int idx) {
    if (idx < 0 || idx >= MAX_HASHTABLES || !g_hashtable_peer_alive[idx]) {
        return;
    }
    
    if (g_j2me_runtime_debug) fprintf(stderr, "[NATIVE] Freeing native Hashtable peer at idx %d\n", idx);
    
    if (g_hashtables[idx].entries) {
        free(g_hashtables[idx].entries);
        g_hashtables[idx].entries = NULL;
    }
    g_hashtables[idx].capacity = 0;
    g_hashtables[idx].count = 0;
    g_hashtable_peer_alive[idx] = false;
}

/* v36.33 [HT-PEER-SESSION-RESET]: native Hashtable peers hold raw
 * JavaObject* key/value pointers into the session heap. They are freed by
 * the GC sweep when the OWNER object is collected — but a session teardown
 * (jvm_destroy -> heap_destroy) resets the arena WITHOUT a sweep, so every
 * peer of the dead session survived with dangling keys into freed-and-
 * reused memory. The next session's heap usually lands at the same
 * address: hashtable_ensure_peer then matches the STALE peer by object
 * pointer (aliasing!) and the new Hashtable inherits the dead session's
 * entries; the first get()/put() dereferences a garbage key. Field
 * signature: Invalid memory access 0x745364726F636572 / 0x736D722E65726F74
 * (ASCII "recordSt" / "tore.rms" — both offsets into the game's RMS file
 * name "recordStore.rms" that came to occupy the freed block). ASAN repro:
 * Temple Rush 3D 2, SEGV in hashtable_keys_equal during session-2 class
 * init. Free every peer at the session boundary, next to the intern-pool
 * reset (v36.13). */
void native_hashtable_session_reset(void) {
    int freed = 0;
    for (int i = 0; i < g_hashtable_count && i < MAX_HASHTABLES; i++) {
        if (g_hashtable_peer_alive[i]) {
            hashtable_free_peer(i);
            freed++;
        }
    }
    if (freed > 0) {
        LOG_SAFE("[HT-PEERS] session reset: freed %d hashtable peer(s) (stale Java heap refs)\n",
                 freed);
    }
    g_hashtable_count = 0;
}

static JavaValue native_hashtable_init(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* ht = (JavaObject*)args[0].ref;
    if (!ht) return NATIVE_RETURN_VOID();
    
    /* Create native peer */
    HashtablePeer* peer = hashtable_get_peer(ht);
    if (peer) {
        NATIVE_DEBUG("Hashtable init: peer created, capacity=%d", peer->capacity);
    }
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_hashtable_put(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)arg_count;
    JavaObject* ht = (JavaObject*)args[0].ref;
    JavaObject* key = (JavaObject*)args[1].ref;
    JavaObject* value = (JavaObject*)args[2].ref;
    
    if (!ht) return NATIVE_RETURN_NULL();
    
    /* Hashtable throws NullPointerException for null key or null value */
    if (!key || !value) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_NULL();
    }
    
    /* Get or create peer */
    HashtablePeer* peer = hashtable_ensure_peer(ht);
    if (!peer) return NATIVE_RETURN_NULL();
    if (!peer->entries) {
        peer->capacity = 16;
        peer->entries = (HashtableEntry*)calloc(16, sizeof(HashtableEntry));
    }
    
    /* Compute hash */
    jint hash = hashtable_compute_hash(key);
    
    /* Find existing entry or empty slot */
    jint idx = (hash & 0x7FFFFFFF) % peer->capacity;
    
    /* Linear probing for collision resolution */
    for (int i = 0; i < peer->capacity; i++) {
        jint probe_idx = (idx + i) % peer->capacity;
        HashtableEntry* entry = &peer->entries[probe_idx];
        
        /* v36.33 [HT-KEY-VALIDATE]: empty slot — or a stale entry purged
         * right here; the freed slot is reused for the new entry so the
         * probe chain of later entries stays intact. */
        if (entry->key == NULL || hashtable_entry_purge_if_stale(entry)) {
            /* Empty slot - add new entry */
            entry->key = key;
            entry->value = value;
            entry->hash = hash;
            peer->count++;
            
            /* Update count field in Java object */
            JavaValue count_val = { .i = peer->count };
            native_set_field_value(ht, "count", count_val);
            
            NATIVE_DEBUG("Hashtable put: new entry at %d, key=%p, hash=0x%X", probe_idx, (void*)key, hash);
            return NATIVE_RETURN_NULL();
        }
        
        if (hashtable_keys_equal(entry->key, key)) {
            /* Found existing key - replace value */
            JavaObject* old_value = entry->value;
            entry->value = value;
            
            NATIVE_DEBUG("Hashtable put: replaced at %d, old=%p, new=%p", probe_idx, (void*)old_value, (void*)value);
            JavaValue ret = { .ref = old_value };
            return ret;
        }
    }
    
    /* Table full - expand (simple approach).
     * FIX(overflow): compute in size_t and cap the growth so a corrupt
     * capacity cannot make calloc receive a wrapped negative int. */
    size_t new_capacity = (size_t)peer->capacity * 2;
    if (new_capacity == 0 || new_capacity > (1u << 20)) {
        NATIVE_DEBUG("Hashtable put: capacity %d overflow, refusing to grow", peer->capacity);
        return NATIVE_RETURN_NULL();
    }
    HashtableEntry* new_entries = (HashtableEntry*)calloc(new_capacity, sizeof(HashtableEntry));
    if (new_entries) {
        /* Rehash all entries */
        for (int i = 0; i < peer->capacity; i++) {
            if (peer->entries[i].key) {
                jint new_idx = (jint)((peer->entries[i].hash & 0x7FFFFFFF) % new_capacity);
                while (new_entries[new_idx].key) {
                    new_idx = (jint)((new_idx + 1) % new_capacity);
                }
                new_entries[new_idx] = peer->entries[i];
            }
        }
        free(peer->entries);
        peer->entries = new_entries;
        peer->capacity = (int)new_capacity;
        
        /* Retry put */
        return native_hashtable_put(jvm, thread, args, arg_count);
    }
    
    return NATIVE_RETURN_NULL();
}

static JavaValue native_hashtable_get(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)arg_count;
    JavaObject* ht = (JavaObject*)args[0].ref;
    JavaObject* key = (JavaObject*)args[1].ref;
    
    if (!ht) return NATIVE_RETURN_NULL();
    
    /* Hashtable throws NullPointerException for null key */
    if (!key) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_NULL();
    }
    
    HashtablePeer* peer = hashtable_get_peer(ht);
    if (!peer || !peer->entries || peer->count == 0) {
        return NATIVE_RETURN_NULL();
    }
    
    /* Compute hash */
    jint hash = hashtable_compute_hash(key);
    
    /* Find entry */
    jint idx = (hash & 0x7FFFFFFF) % peer->capacity;
    
    /* Linear probing */
    for (int i = 0; i < peer->capacity; i++) {
        jint probe_idx = (idx + i) % peer->capacity;
        HashtableEntry* entry = &peer->entries[probe_idx];
        
        if (entry->key == NULL) {
            /* Empty slot - key not found */
            break;
        }
        /* v36.33 [HT-KEY-VALIDATE]: stale key — purge and keep probing
         * (breaking the chain here would shadow entries placed after it). */
        if (hashtable_entry_purge_if_stale(entry)) continue;
        
        if (hashtable_keys_equal(entry->key, key)) {
            /* Found! */
            NATIVE_DEBUG("Hashtable get: found at %d, value=%p", probe_idx, (void*)entry->value);
            JavaValue ret = { .ref = entry->value };
            return ret;
        }
    }
    
    NATIVE_DEBUG("Hashtable get: key not found, hash=0x%X", hash);
    return NATIVE_RETURN_NULL();
}

static JavaValue native_hashtable_size(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* ht = (JavaObject*)args[0].ref;
    
    if (!ht) return NATIVE_RETURN_INT(0);
    
    HashtablePeer* peer = hashtable_get_peer(ht);
    if (!peer) return NATIVE_RETURN_INT(0);
    
    return NATIVE_RETURN_INT(peer->count);
}

/* Hashtable.isEmpty() - check if table is empty */
static JavaValue native_hashtable_isEmpty(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* ht = (JavaObject*)args[0].ref;
    
    if (!ht) return NATIVE_RETURN_INT(1);  /* null is empty */
    
    HashtablePeer* peer = hashtable_get_peer(ht);
    if (!peer) return NATIVE_RETURN_INT(1);
    
    return NATIVE_RETURN_INT(peer->count == 0 ? 1 : 0);
}

/* javax.microedition.m3g.Graphics3D.getProperties() - JSR-184 §5.5.
 * Implemented HERE (next to Hashtable internals) and writes entries directly
 * into the peer table: routing puts through execute_method() proved fragile
 * (any exception raised by put longjmp'd out of the handler mid-flight,
 * leaving the table empty and killing Nescube's title thread with an NPE on
 * ((Boolean)props.get(k)).booleanValue()). */
static void ht_direct_put(HashtablePeer* peer, JavaObject* key, JavaObject* value, jint hash) {
    if (!peer || !key || !value) return;
    if (!peer->entries) {
        peer->capacity = 16;
        peer->entries = (HashtableEntry*)calloc(16, sizeof(HashtableEntry));
        if (!peer->entries) { peer->capacity = 0; return; }
    }
    jint idx = (hash & 0x7FFFFFFF) % peer->capacity;
    for (int i = 0; i < peer->capacity; i++) {
        jint probe_idx = (idx + i) % peer->capacity;
        HashtableEntry* entry = &peer->entries[probe_idx];
        if (entry->key == NULL) {
            entry->key = key;
            entry->value = value;
            entry->hash = hash;
            peer->count++;
            return;
        }
        if (hashtable_keys_equal(entry->key, key)) {
            entry->value = value;  /* replace */
            return;
        }
    }
}

JavaValue native_graphics3d_getProperties(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;

    JavaClass* ht_class = jvm_load_class(jvm, "java/util/Hashtable");
    if (!ht_class) return NATIVE_RETURN_NULL();

    JavaObject* ht = jvm_new_object(jvm, ht_class);
    if (!ht) return NATIVE_RETURN_NULL();

    HashtablePeer* peer = hashtable_ensure_peer(ht);
    if (!peer) return NATIVE_RETURN_OBJECT(ht);

    JavaClass* bool_class = jvm_load_class(jvm, "java/lang/Boolean");
    JavaClass* int_class  = jvm_load_class(jvm, "java/lang/Integer");

    static const struct { const char* key; int is_bool; jint value; } props[] = {
        /* Boolean capability flags. 'supportPerspectiveCorrection' MUST stay:
         * Nescube queries it unconditionally without a null check. */
        { "supportAntialiasing",          1, 0 },
        { "supportTrueColor",             1, 1 },
        { "supportDithering",             1, 0 },
        { "supportPerspectiveCorrection", 1, 1 },
        { "supportMipmapping",            1, 0 },   /* non-standard but probed */
        /* Integer limits per M3G RI defaults.
         * v34.18 (audit item 2.3): added the two standard JSR-184 keys games
         * probe for multitexturing and sprite cropping. */
        { "maxLights",                    0, 8 },
        { "maxViewportWidth",             0, 2048 },
        { "maxViewportHeight",            0, 2048 },
        { "maxTextureDimension",          0, 1024 },
        { "maxTextureUnits",              0, 2 },
        { "maxSpriteCropDimension",       0, 1024 },
    };

    for (size_t i = 0; i < sizeof(props)/sizeof(props[0]); i++) {
        JavaString* key_str = jvm_new_string(jvm, props[i].key);
        if (!key_str) continue;
        JavaClass* vclass = props[i].is_bool ? bool_class : int_class;
        if (!vclass) continue;
        JavaObject* box = jvm_new_object(jvm, vclass);
        if (!box) continue;
        JavaValue fv = { .i = props[i].value };
        native_set_field_value(box, "value", fv);

        jint hash = hashtable_compute_hash((JavaObject*)key_str);
        ht_direct_put(peer, (JavaObject*)key_str, box, hash);
    }

    /* Keep the Java-visible count field in sync */
    {
        JavaValue count_val = { .i = peer->count };
        native_set_field_value(ht, "count", count_val);
    }

    return NATIVE_RETURN_OBJECT(ht);
}

/* Hashtable.containsKey(Object) - check if key exists */
static JavaValue native_hashtable_containsKey(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* ht = (JavaObject*)args[0].ref;
    JavaObject* key = (JavaObject*)args[1].ref;
    
    if (!ht || !key) return NATIVE_RETURN_INT(0);
    
    HashtablePeer* peer = hashtable_get_peer(ht);
    if (!peer || !peer->entries || peer->count == 0) {
        return NATIVE_RETURN_INT(0);
    }
    
    jint hash = hashtable_compute_hash(key);
    jint idx = (hash & 0x7FFFFFFF) % peer->capacity;
    
    for (int i = 0; i < peer->capacity; i++) {
        jint probe_idx = (idx + i) % peer->capacity;
        HashtableEntry* entry = &peer->entries[probe_idx];
        
        if (entry->key == NULL) break;
        /* v36.33 [HT-KEY-VALIDATE]: stale key — purge and keep probing */
        if (hashtable_entry_purge_if_stale(entry)) continue;
        
        if (hashtable_keys_equal(entry->key, key)) {
            return NATIVE_RETURN_INT(1);
        }
    }
    
    return NATIVE_RETURN_INT(0);
}

/* Hashtable.remove(Object) - remove key and return old value */
static JavaValue native_hashtable_remove(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* ht = (JavaObject*)args[0].ref;
    JavaObject* key = (JavaObject*)args[1].ref;
    
    if (!ht || !key) return NATIVE_RETURN_NULL();
    
    HashtablePeer* peer = hashtable_get_peer(ht);
    if (!peer || !peer->entries || peer->count == 0) {
        return NATIVE_RETURN_NULL();
    }
    
    jint hash = hashtable_compute_hash(key);
    jint idx = (hash & 0x7FFFFFFF) % peer->capacity;
    
    for (int i = 0; i < peer->capacity; i++) {
        jint probe_idx = (idx + i) % peer->capacity;
        HashtableEntry* entry = &peer->entries[probe_idx];
        
        if (entry->key == NULL) break;
        /* v36.33 [HT-KEY-VALIDATE]: stale key — purge and keep probing */
        if (hashtable_entry_purge_if_stale(entry)) continue;
        
        if (hashtable_keys_equal(entry->key, key)) {
            JavaObject* old_value = entry->value;
            entry->key = NULL;
            entry->value = NULL;
            entry->hash = 0;
            peer->count--;
            
            JavaValue count_val = { .i = peer->count };
            native_set_field_value(ht, "count", count_val);
            
            JavaValue ret = { .ref = old_value };
            return ret;
        }
    }
    
    return NATIVE_RETURN_NULL();
}

/* Hashtable.contains(Object value) - check if value exists in the hashtable */
static JavaValue native_hashtable_contains(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* ht = (JavaObject*)args[0].ref;
    JavaObject* value = (JavaObject*)args[1].ref;
    
    if (!ht) return NATIVE_RETURN_INT(0);
    
    HashtablePeer* peer = hashtable_get_peer(ht);
    if (!peer || !peer->entries || peer->count == 0) {
        return NATIVE_RETURN_INT(0);
    }
    
    /* Search for value using equals comparison */
    for (int i = 0; i < peer->capacity; i++) {
        HashtableEntry* entry = &peer->entries[i];
        if (entry->key != NULL) {
            /* v36.33 [HT-KEY-VALIDATE]: purge-and-skip stale entries */
            if (hashtable_entry_purge_if_stale(entry)) continue;
            if (vector_objects_equal(entry->value, value)) {
                return NATIVE_RETURN_INT(1);  /* true - found */
            }
        }
    }
    
    return NATIVE_RETURN_INT(0);  /* false - not found */
}

/* Hashtable.clear() - remove all entries */
static JavaValue native_hashtable_clear(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* ht = (JavaObject*)args[0].ref;
    
    if (!ht) return NATIVE_RETURN_VOID();
    
    HashtablePeer* peer = hashtable_get_peer(ht);
    if (peer && peer->entries) {
        /* Clear all entries */
        memset(peer->entries, 0, peer->capacity * sizeof(HashtableEntry));
        peer->count = 0;
    }
    
    /* Update count field */
    JavaValue count_val = { .i = 0 };
    native_set_field_value(ht, "count", count_val);
    
    return NATIVE_RETURN_VOID();
}

/* Hashtable.keys() - return enumeration of keys */
static JavaValue native_hashtable_keys(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* ht = (JavaObject*)args[0].ref;
    
    if (!ht) return NATIVE_RETURN_NULL();
    
    HashtablePeer* peer = hashtable_get_peer(ht);
    
    if (!peer || !peer->entries || peer->count == 0) {
        /* Return empty enumeration */
        JavaArray* empty_keys = jvm_new_array(jvm, DESC_OBJECT, 0, NULL);
        if (!empty_keys) return NATIVE_RETURN_NULL();
        
        JavaClass* iter_class = get_or_create_stub_class(jvm, "java/util/VectorEnumeration");
        if (!iter_class) return NATIVE_RETURN_NULL();
        
        /* Ensure the class has fields for enumeration state */
        if (iter_class->fields_count == 0) {
            iter_class->fields = (JavaField*)calloc(3, sizeof(JavaField));
            iter_class->fields[0].name = strdup("elements");
            iter_class->fields[0].descriptor = strdup("[Ljava/lang/Object;");
            iter_class->fields[1].name = strdup("count");
            iter_class->fields[1].descriptor = strdup("I");
            iter_class->fields[2].name = strdup("index");
            iter_class->fields[2].descriptor = strdup("I");
            iter_class->fields_count = 3;
            iter_class->instance_size = sizeof(ObjectHeader) + 3 * sizeof(JavaValue);
            iter_class->initialized = true;
        }
        
        JavaObject* iter_obj = jvm_new_object(jvm, iter_class);
        if (!iter_obj) return NATIVE_RETURN_NULL();
        
        JavaValue keys_val = { .ref = empty_keys };
        JavaValue count_val = { .i = 0 };
        JavaValue index_val = { .i = 0 };
        
        native_set_field_value(iter_obj, "elements", keys_val);
        native_set_field_value(iter_obj, "count", count_val);
        native_set_field_value(iter_obj, "index", index_val);
        
        return NATIVE_RETURN_OBJECT(iter_obj);
    }
    
    /* Create array of keys */
    JavaArray* keys_array = jvm_new_array(jvm, DESC_OBJECT, peer->count, NULL);
    if (!keys_array) return NATIVE_RETURN_NULL();
    
    /* Collect all keys */
    int key_idx = 0;
    for (int i = 0; i < peer->capacity && key_idx < peer->count; i++) {
        HashtableEntry* entry = &peer->entries[i];
        if (entry->key != NULL && !hashtable_entry_purge_if_stale(entry)) {
            array_set_ref(keys_array, key_idx++, entry->key);
        }
    }
    
    /* Create enumeration object using get_or_create_stub_class like native_vector_elements */
    JavaClass* iter_class = get_or_create_stub_class(jvm, "java/util/VectorEnumeration");
    if (!iter_class) {
        return NATIVE_RETURN_NULL();
    }
    
    /* Ensure the class has fields for enumeration state */
    if (iter_class->fields_count == 0) {
        iter_class->fields = (JavaField*)calloc(3, sizeof(JavaField));
        iter_class->fields[0].name = strdup("elements");
        iter_class->fields[0].descriptor = strdup("[Ljava/lang/Object;");
        iter_class->fields[1].name = strdup("count");
        iter_class->fields[1].descriptor = strdup("I");
        iter_class->fields[2].name = strdup("index");
        iter_class->fields[2].descriptor = strdup("I");
        iter_class->fields_count = 3;
        iter_class->instance_size = sizeof(ObjectHeader) + 3 * sizeof(JavaValue);
        iter_class->initialized = true;
    }
    
    JavaObject* iter_obj = jvm_new_object(jvm, iter_class);
    if (!iter_obj) {
        return NATIVE_RETURN_NULL();
    }
    
    JavaValue keys_val = { .ref = keys_array };
    JavaValue count_val = { .i = peer->count };
    JavaValue index_val = { .i = 0 };
    
    native_set_field_value(iter_obj, "elements", keys_val);
    native_set_field_value(iter_obj, "count", count_val);
    native_set_field_value(iter_obj, "index", index_val);
    
    return NATIVE_RETURN_OBJECT(iter_obj);
}

/* Hashtable.elements() - return enumeration of values */
static JavaValue native_hashtable_elements(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* ht = (JavaObject*)args[0].ref;
    
    if (!ht) return NATIVE_RETURN_NULL();
    
    HashtablePeer* peer = hashtable_get_peer(ht);
    
    if (!peer || !peer->entries || peer->count == 0) {
        /* Return empty enumeration */
        JavaArray* empty_values = jvm_new_array(jvm, DESC_OBJECT, 0, NULL);
        if (!empty_values) return NATIVE_RETURN_NULL();
        
        JavaClass* iter_class = get_or_create_stub_class(jvm, "java/util/VectorEnumeration");
        if (!iter_class) return NATIVE_RETURN_NULL();
        
        /* Ensure the class has fields for enumeration state */
        if (iter_class->fields_count == 0) {
            iter_class->fields = (JavaField*)calloc(3, sizeof(JavaField));
            iter_class->fields[0].name = strdup("elements");
            iter_class->fields[0].descriptor = strdup("[Ljava/lang/Object;");
            iter_class->fields[1].name = strdup("count");
            iter_class->fields[1].descriptor = strdup("I");
            iter_class->fields[2].name = strdup("index");
            iter_class->fields[2].descriptor = strdup("I");
            iter_class->fields_count = 3;
            iter_class->instance_size = sizeof(ObjectHeader) + 3 * sizeof(JavaValue);
            iter_class->initialized = true;
        }
        
        JavaObject* iter_obj = jvm_new_object(jvm, iter_class);
        if (!iter_obj) return NATIVE_RETURN_NULL();
        
        JavaValue values_val = { .ref = empty_values };
        JavaValue count_val = { .i = 0 };
        JavaValue index_val = { .i = 0 };
        
        native_set_field_value(iter_obj, "elements", values_val);
        native_set_field_value(iter_obj, "count", count_val);
        native_set_field_value(iter_obj, "index", index_val);
        
        return NATIVE_RETURN_OBJECT(iter_obj);
    }
    
    /* Create array of values */
    JavaArray* values_array = jvm_new_array(jvm, DESC_OBJECT, peer->count, NULL);
    if (!values_array) return NATIVE_RETURN_NULL();
    
    /* Collect all values */
    int val_idx = 0;
    for (int i = 0; i < peer->capacity && val_idx < peer->count; i++) {
        HashtableEntry* entry = &peer->entries[i];
        if (entry->key != NULL && !hashtable_entry_purge_if_stale(entry)) {
            array_set_ref(values_array, val_idx++, entry->value);
        }
    }
    
    /* Create enumeration object */
    JavaClass* iter_class = get_or_create_stub_class(jvm, "java/util/VectorEnumeration");
    if (!iter_class) {
        return NATIVE_RETURN_NULL();
    }
    
    /* Ensure the class has fields for enumeration state */
    if (iter_class->fields_count == 0) {
        iter_class->fields = (JavaField*)calloc(3, sizeof(JavaField));
        iter_class->fields[0].name = strdup("elements");
        iter_class->fields[0].descriptor = strdup("[Ljava/lang/Object;");
        iter_class->fields[1].name = strdup("count");
        iter_class->fields[1].descriptor = strdup("I");
        iter_class->fields[2].name = strdup("index");
        iter_class->fields[2].descriptor = strdup("I");
        iter_class->fields_count = 3;
        iter_class->instance_size = sizeof(ObjectHeader) + 3 * sizeof(JavaValue);
        iter_class->initialized = true;
    }
    
    JavaObject* iter_obj = jvm_new_object(jvm, iter_class);
    if (!iter_obj) {
        return NATIVE_RETURN_NULL();
    }
    
    JavaValue values_val = { .ref = values_array };
    JavaValue count_val = { .i = peer->count };
    JavaValue index_val = { .i = 0 };
    
    native_set_field_value(iter_obj, "elements", values_val);
    native_set_field_value(iter_obj, "count", count_val);
    native_set_field_value(iter_obj, "index", index_val);
    
    return NATIVE_RETURN_OBJECT(iter_obj);
}

/* Hashtable.finalize() - called by GC before collecting the object
 * This ensures native peer memory is freed even if GC sweep misses it 
 * 
 * FIX: Now uses object pointer search instead of threshold field,
 * because Java code can modify threshold and break the linkage.
 */
static JavaValue native_hashtable_finalize(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* ht = (JavaObject*)args[0].ref;
    
    if (!ht) return NATIVE_RETURN_VOID();
    
    /* FIX: Find peer by object pointer, not by threshold field */
    int peer_idx = -1;
    for (int i = 0; i < g_hashtable_count; i++) {
        if (g_hashtable_peer_alive[i] && g_hashtables[i].ht_obj == ht) {
            peer_idx = i;
            break;
        }
    }
    
    if (peer_idx >= 0 && peer_idx < MAX_HASHTABLES) {
        if (g_j2me_runtime_debug) fprintf(stderr, "[NATIVE] Hashtable.finalize() called, freeing peer idx %d\n", peer_idx);
        hashtable_free_peer(peer_idx);
    }
    
    return NATIVE_RETURN_VOID();
}

/*
 * java.io.InputStreamReader native methods
 */

/* Helper to find the underlying InputStream in InputStreamReader */
/* ИСПРАВЛЕНО: используем native_get_field_value */
static JavaObject* get_inputstream_reader_stream(JavaObject* isr_obj) {
    if (!isr_obj) return NULL;
    
    JavaClass* clazz = isr_obj->header.clazz;
    if (!clazz) return NULL;
    
    
    /* Try to get 'in' field directly */
    JavaObject* stream = (JavaObject*)native_get_field_value(isr_obj, "in").ref;
    if (stream) {
        return stream;
    }
    
    /* If no 'in' field found, try to find InputStream by descriptor type */
    for (int i = 0; i < clazz->fields_count; i++) {
        const char* desc = clazz->fields[i].descriptor;
        const char* name = clazz->fields[i].name;
        if (desc && strstr(desc, "InputStream") && name) {
            /* InputStream reference field - use helper function */
            stream = (JavaObject*)native_get_field_value(isr_obj, name).ref;
            if (stream) {
                return stream;
            }
        }
    }
    
    return NULL;
}

static JavaValue native_inputstreamreader_read(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* isr = (JavaObject*)args[0].ref;
    
    if (!isr) {
        return NATIVE_RETURN_INT(-1);
    }
    
    /* Get underlying InputStream */
    JavaObject* stream = get_inputstream_reader_stream(isr);
    if (!stream) {
        return NATIVE_RETURN_INT(-1);
    }
    
    /* Check if it's a ByteArrayInputStream */
    JavaClass* stream_class = stream->header.clazz;
    if (stream_class && stream_class->class_name && 
        strstr(stream_class->class_name, "ByteArrayInputStream")) {
        
        /* ИСПРАВЛЕНО: используем native_get_field_value */
        JavaArray* buf = (JavaArray*)native_get_field_value(stream, "buf").ref;
        jint pos = native_get_field_value(stream, "pos").i;
        jint count = native_get_field_value(stream, "count").i;
        
        if (buf && pos < count) {
            /* Read byte and convert to char (ISO-8859-1) */
            uint8_t* data = (uint8_t*)array_data(buf);
            jint result = data[pos];
            
            /* ИСПРАВЛЕНО: используем native_set_field_value для обновления pos */
            JavaValue new_pos = { .i = pos + 1 };
            native_set_field_value(stream, "pos", new_pos);
            
            return NATIVE_RETURN_INT(result);
        }
    }
    
    /* Default: end of stream */
    return NATIVE_RETURN_INT(-1);
}

static JavaValue native_inputstreamreader_read_array(JVM* jvm, JavaThread* thread,
                                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* isr = (JavaObject*)args[0].ref;
    JavaArray* cbuf = (JavaArray*)args[1].ref;
    
    if (!isr || !cbuf) return NATIVE_RETURN_INT(-1);
    
    /* Get underlying InputStream */
    JavaObject* stream = get_inputstream_reader_stream(isr);
    if (!stream) return NATIVE_RETURN_INT(-1);
    
    /* Check if it's a ByteArrayInputStream */
    JavaClass* stream_class = stream->header.clazz;
    if (stream_class && stream_class->class_name && 
        strstr(stream_class->class_name, "ByteArrayInputStream")) {
        
        /* ИСПРАВЛЕНО: используем native_get_field_value */
        JavaArray* buf = (JavaArray*)native_get_field_value(stream, "buf").ref;
        jint pos = native_get_field_value(stream, "pos").i;
        jint count = native_get_field_value(stream, "count").i;
        
        if (buf && pos < count) {
            uint8_t* src_data = (uint8_t*)array_data(buf);
            uint16_t* dst_data = (uint16_t*)array_data(cbuf);
            
            int to_read = count - pos;
            if (to_read > cbuf->length) to_read = cbuf->length;
            
            /* Copy bytes as chars (ISO-8859-1) */
            for (int i = 0; i < to_read; i++) {
                dst_data[i] = src_data[pos + i];
            }
            
            /* ИСПРАВЛЕНО: используем native_set_field_value для обновления pos */
            JavaValue new_pos = { .i = pos + to_read };
            native_set_field_value(stream, "pos", new_pos);
            
            return NATIVE_RETURN_INT(to_read);
        }
    }
    
    return NATIVE_RETURN_INT(-1);
}

static JavaValue native_inputstreamreader_close(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    /* No-op for now */
    return NATIVE_RETURN_VOID();
}

/*
 * java.io.BufferedReader native methods
 */

/* Helper to find the underlying Reader in BufferedReader */
/* ИСПРАВЛЕНО: используем native_get_field_value */
static JavaObject* get_bufferedreader_reader(JavaObject* br_obj) {
    if (!br_obj) return NULL;
    
    /* Try 'in' field first */
    JavaObject* reader = (JavaObject*)native_get_field_value(br_obj, "in").ref;
    if (reader) return reader;
    
    /* Try 'reader' field */
    reader = (JavaObject*)native_get_field_value(br_obj, "reader").ref;
    if (reader) return reader;
    
    /* Try 'lock' field as fallback */
    reader = (JavaObject*)native_get_field_value(br_obj, "lock").ref;
    return reader;
}

static JavaValue native_bufferedreader_read(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* br = (JavaObject*)args[0].ref;

    if (!br) return NATIVE_RETURN_INT(-1);

    /* Get underlying Reader (InputStreamReader) */
    JavaObject* reader = get_bufferedreader_reader(br);
    if (!reader) return NATIVE_RETURN_INT(-1);

    /* v34.77 FIX (Bobby Carrot 5): jvm_resolve_method only finds BYTECODE
     * methods; the InputStreamReader stub has no read()I bytecode — the
     * implementation lives in the NATIVE registry. Resolution returned NULL
     * and readLine()/read() silently returned null/-1 forever. Look the
     * native up explicitly and call it directly. */
    JavaClass* reader_class = reader->header.clazz;
    NativeMethod nm = (reader_class && reader_class->class_name)
        ? native_find(jvm, reader_class->class_name, "read", "()I")
        : NULL;
    if (nm) {
        JavaValue reader_arg = { .ref = reader };
        JavaValue result;
        memset(&result, 0, sizeof(result));
        result = nm(jvm, thread, &reader_arg, 1);
        return result;
    }

    return NATIVE_RETURN_INT(-1);
}

static JavaValue native_bufferedreader_readline(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)arg_count;
    JavaObject* br = (JavaObject*)args[0].ref;

    if (!br) return NATIVE_RETURN_NULL();

    /* Get underlying Reader */
    JavaObject* reader = get_bufferedreader_reader(br);
    if (!reader) return NATIVE_RETURN_NULL();

    /* Build string by reading characters */
    char buffer[4096];
    int len = 0;

    JavaClass* reader_class = reader->header.clazz;

    /* v34.77 FIX (Bobby Carrot 5): see native_bufferedreader_read — the
     * InputStreamReader read()I is a REGISTERED NATIVE, not bytecode;
     * jvm_resolve_method never found it and readLine() returned null
     * (-> NumberFormatException in Integer.parseInt(readLine()) idioms).
     * Resolve via the native registry instead. */
    NativeMethod nm = (reader_class && reader_class->class_name)
        ? native_find(jvm, reader_class->class_name, "read", "()I")
        : NULL;

    if (!nm) return NATIVE_RETURN_NULL();

    int saw_newline = 0;  /* distinguishes "" (empty line) from EOF null */
    while (len < (int)sizeof(buffer) - 1) {
        JavaValue reader_arg = { .ref = reader };
        JavaValue result;
        memset(&result, 0, sizeof(result));
        result = nm(jvm, thread, &reader_arg, 1);

        jint ch = result.i;

        if (ch < 0) {
            /* End of stream */
            break;
        }

        if (ch == '\n') {
            saw_newline = 1;
            break;
        }

        if (ch == '\r') {
            /* Check for \r\n */
            memset(&result, 0, sizeof(result));
            result = nm(jvm, thread, &reader_arg, 1);
            jint next = result.i;
            if (next >= 0 && next != '\n') {
                /* Put back - for simplicity, we just skip */
            }
            saw_newline = 1;
            break;
        }

        buffer[len++] = (char)ch;
    }

    if (len == 0 && !saw_newline) {
        /* True EOF with nothing read: Java readLine() returns null. */
        return NATIVE_RETURN_NULL();
    }

    if (len == 0) {
        /* Empty line ("\n" right away): Java readLine() returns "". */
        JavaString* empty = jvm_new_string(jvm, "");
        if (empty) {
            JavaValue ret = { .ref = (JavaObject*)empty };
            return ret;
        }
        return NATIVE_RETURN_NULL();
    }

    /* Create String */
    buffer[len] = '\0';
    JavaString* str = jvm_new_string(jvm, buffer);
    JavaValue ret = { .ref = (JavaObject*)str };
    return ret;
}

static JavaValue native_bufferedreader_close(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    /* No-op for now */
    return NATIVE_RETURN_VOID();
}

void init_java_io_inputstreamreader(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"java/io/InputStreamReader", "read", "()I", native_inputstreamreader_read},
        {"java/io/InputStreamReader", "read", "([C)I", native_inputstreamreader_read_array},
        {"java/io/InputStreamReader", "read", "([CII)I", native_inputstreamreader_read_array},
        {"java/io/InputStreamReader", "close", "()V", native_inputstreamreader_close},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
    NATIVE_DEBUG("Registered java/io/InputStreamReader native methods");
}

void init_java_io_bufferedreader(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"java/io/BufferedReader", "read", "()I", native_bufferedreader_read},
        {"java/io/BufferedReader", "readLine", "()Ljava/lang/String;", native_bufferedreader_readline},
        {"java/io/BufferedReader", "close", "()V", native_bufferedreader_close},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
    NATIVE_DEBUG("Registered java/io/BufferedReader native methods");
}

void init_java_util_hashtable(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"java/util/Hashtable", "<init>", "()V", native_hashtable_init},
        {"java/util/Hashtable", "<init>", "(I)V", native_hashtable_init},          /* initial capacity */
        {"java/util/Hashtable", "<init>", "(IF)V", native_hashtable_init},         /* capacity + load factor */
        {"java/util/Hashtable", "put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;", native_hashtable_put},
        {"java/util/Hashtable", "get", "(Ljava/lang/Object;)Ljava/lang/Object;", native_hashtable_get},
        {"java/util/Hashtable", "size", "()I", native_hashtable_size},
        {"java/util/Hashtable", "isEmpty", "()Z", native_hashtable_isEmpty},
        {"java/util/Hashtable", "containsKey", "(Ljava/lang/Object;)Z", native_hashtable_containsKey},
        {"java/util/Hashtable", "contains", "(Ljava/lang/Object;)Z", native_hashtable_contains},
        {"java/util/Hashtable", "remove", "(Ljava/lang/Object;)Ljava/lang/Object;", native_hashtable_remove},
        {"java/util/Hashtable", "clear", "()V", native_hashtable_clear},
        {"java/util/Hashtable", "keys", "()Ljava/util/Enumeration;", native_hashtable_keys},
        {"java/util/Hashtable", "elements", "()Ljava/util/Enumeration;", native_hashtable_elements},
        {"java/util/Hashtable", "finalize", "()V", native_hashtable_finalize},     /* GC cleanup */
    };
    int count = sizeof(methods) / sizeof(methods[0]);
    native_register_methods(jvm, methods, count);
    NATIVE_DEBUG("Registered java/util/Hashtable native methods");
}

void init_java_util_random(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"java/util/Random", "<init>", "()V", native_random_init},
        {"java/util/Random", "<init>", "(J)V", native_random_init_seed},
        {"java/util/Random", "next", "(I)I", native_random_next},
        {"java/util/Random", "nextInt", "()I", native_random_nextInt},
        {"java/util/Random", "nextInt", "(I)I", native_random_nextIntBound},
        {"java/util/Random", "nextLong", "()J", native_random_nextLong},
        {"java/util/Random", "nextFloat", "()F", native_random_nextFloat},
        {"java/util/Random", "nextDouble", "()D", native_random_nextDouble},
        {"java/util/Random", "setSeed", "(J)V", native_random_setSeed},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
    NATIVE_DEBUG("Registered java/util/Random native methods");
}

/*
 * java.util.Timer native methods
 * SIMPLIFIED IMPLEMENTATION - runs synchronously in main thread
 * This avoids threading issues with non-thread-safe JVM
 */

/* Timer task entry */
typedef struct {
    JavaObject* timer_task;
    jlong next_run_time;    /* When to run (in ms since start) */
    jlong period_ms;        /* Period for repeating tasks, 0 for one-shot */
    int timer_id;           /* FIX-19l: owning java.util.Timer instance */
    bool active;
} TimerEntry;

#define MAX_TIMERS 16
static TimerEntry g_timers[MAX_TIMERS];
static int g_timer_count = 0;
static uint64_t g_start_time_ms = 0;

/* FIX-19l: java.util.Timer.cancel() used to wipe ALL 16 timer slots
 * globally - including timers belonging to OTHER Timer instances (VmTest's
 * ConsoleCanvas repaint timer was killed by the Timer test, freezing the
 * UI and making the rest of the suite invisible). Each Timer instance now
 * owns a stable id and cancel() only removes its own entries. */
#define MAX_TIMER_OBJECTS 16
static struct { JavaObject* obj; int id; } g_timer_ids[MAX_TIMER_OBJECTS];
static int g_next_timer_id = 1;

/* v35.08 MULTI-SESSION: was a function-local static inside
 * jvm_process_timers() - a one-per-PROCESS "roots registered" latch. The
 * per-session teardown (gc_roots_reset_all + heap_destroy) empties the
 * root list, so the NEXT session would never re-register the timer slots:
 * the GC could then sweep a scheduled TimerTask mid-flight (the exact
 * v34.69 Captain-Fatal "frozen splash" bug, resurrected for session two).
 * File scope so native_timers_session_reset() can clear it per session.
 * The reset function itself lives right after g_timers_pump_mutex (it
 * takes that mutex). */
static int timers_gc_registered = 0;

static int timer_id_for_object(JavaObject* timer_obj) {
    if (!timer_obj) return 0;
    for (int i = 0; i < MAX_TIMER_OBJECTS; i++) {
        if (g_timer_ids[i].obj == timer_obj) {
            return g_timer_ids[i].id;
        }
    }
    for (int i = 0; i < MAX_TIMER_OBJECTS; i++) {
        if (!g_timer_ids[i].obj) {
            g_timer_ids[i].obj = timer_obj;
            g_timer_ids[i].id = g_next_timer_id++;
            return g_timer_ids[i].id;
        }
    }
    /* table full: still give a unique id (schedulable, not cancellable) */
    return g_next_timer_id++;
}

/* Get current time in milliseconds */
static uint64_t get_time_ms(void) {
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

/* v34 FIX (VmTest "one-shot fired once exp=1 got=2"): jvm_process_timers() is
 * pumped from BOTH the main loop AND Thread.sleep() (via
 * sdl_process_events_minimal). Java threads are real pthreads on this build,
 * so two threads can pump concurrently: thread A fires a slot, and while
 * run() is still executing (the slot is deactivated only AFTER run returns),
 * thread B sees the slot active and fires it AGAIN - the one-shot ran twice.
 * Same-thread nesting (fire -> run() -> sleep -> pump) has the same hazard.
 * Guard with a TLS re-entry flag plus a trylock mutex for cross-thread. */
static pthread_mutex_t g_timers_pump_mutex = PTHREAD_MUTEX_INITIALIZER;
static __thread int tls_in_jvm_process_timers = 0;

/* v35.08 MULTI-SESSION: called from jvm_destroy between sessions (VM
 * threads already quiet; the pump mutex also serializes against any
 * straggler pump). Clears the registry state that held session objects
 * and re-arms the once-per-process GC-root latch above. */
void native_timers_session_reset(void) {
    pthread_mutex_lock(&g_timers_pump_mutex);
    for (int i = 0; i < MAX_TIMERS; i++) {
        g_timers[i].timer_task = NULL;
        g_timers[i].active = false;
    }
    g_timer_count = 0;
    for (int i = 0; i < MAX_TIMER_OBJECTS; i++) {
        g_timer_ids[i].obj = NULL;
        g_timer_ids[i].id = 0;
    }
    g_next_timer_id = 1;
    timers_gc_registered = 0;
    pthread_mutex_unlock(&g_timers_pump_mutex);
}

/* Process all pending timers - call from main loop */
/* v34.81 FRONTEND PAUSE: виртуальное «время с запуска VM» для таймеров —
 * часы таймеров минус время паузы фронтенда (меню RetroArch). Единая
 * точка входа для всех трёх мест (fire-цикл + one-shot/periodic
 * schedule): дедлайны, назначенные до паузы, не сгорают пачкой после
 * снятия, новые расписания ложатся на ту же виртуальную ось. */
static uint64_t timer_virt_now(void) {
    extern uint64_t jvm_pause_offset_ms(void);
    uint64_t real = get_time_ms() - g_start_time_ms;
    uint64_t poff = jvm_pause_offset_ms();
    return (poff && real > poff) ? real - poff : real;
}

void jvm_process_timers(JVM* jvm) {
    if (tls_in_jvm_process_timers) return;
    if (pthread_mutex_trylock(&g_timers_pump_mutex) != 0) return;
    tls_in_jvm_process_timers = 1;

    /* v34.69 FIX (Captain Fatal 3D frozen splash -> "black screen"):
     * register the timer slots as GC roots ONCE. g_timers[i].timer_task
     * holds the ONLY reference to a scheduled TimerTask (the Java side
     * keeps no Timer->task list; java.util.Timer here is a thin native).
     * Without roots, any GC pass between schedule() and fire() could
     * sweep the task object - then header.clazz goes stale and the firing
     * loop below silently skips the slot forever ("if (!task_class)
     * continue" leaves it active). Same for g_timer_ids[].obj: a recycled
     * address would alias a DIFFERENT Timer object into a stale id and
     * cancel() would kill the wrong instance's tasks. */
    {
        /* v35.08: the latch is file-scope now (see
         * native_timers_session_reset above); gc_add_root is idempotent. */
        if (!timers_gc_registered && jvm) {
            extern void gc_add_root(JVM* jvm, void** root);
            for (int i = 0; i < MAX_TIMERS; i++)
                gc_add_root(jvm, (void**)&g_timers[i].timer_task);
            for (int i = 0; i < MAX_TIMER_OBJECTS; i++)
                gc_add_root(jvm, (void**)&g_timer_ids[i].obj);
            timers_gc_registered = 1;
        }
    }

    if (g_timer_count == 0) {
        tls_in_jvm_process_timers = 0;
        pthread_mutex_unlock(&g_timers_pump_mutex);
        return;
    }

    uint64_t now = timer_virt_now();

    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!g_timers[i].active || !g_timers[i].timer_task) continue;

        /* Check if it's time to run */
        if ((jlong)now >= g_timers[i].next_run_time) {
            fprintf(stderr, "[TIMER] Firing timer[%d]: now=%" PRIu64 ", scheduled=%lld (class=%s)\n",
                    i, now, (long long)g_timers[i].next_run_time,
                    g_timers[i].timer_task->header.clazz ? g_timers[i].timer_task->header.clazz->class_name : "(null)");

            /* Get the run method */
            JavaClass* task_class = g_timers[i].timer_task->header.clazz;
            if (!task_class) {
                /* v34.69: dead entry (no class) - deactivate instead of
                 * leaking an eternally-active slot that never fires. */
                g_timers[i].active = false;
                g_timers[i].timer_task = NULL;
                g_timer_count--;
                continue;
            }

            JavaMethod* run_method = jvm_resolve_method(jvm, task_class, "run", "()V");
            if (!run_method) {
                fprintf(stderr, "[TIMER] WARNING: no run() method found on %s\n", task_class->class_name);
                /* v34.69: same leak class - the task can never run. */
                g_timers[i].active = false;
                g_timers[i].timer_task = NULL;
                g_timer_count--;
                continue;
            }

            /* v34.69 FIX (CRITICAL - Captain Fatal 3D splash frozen at
             * state 1, game never reaches the menu): a one-shot task is
             * consumed BEFORE run() executes. The OLD order (deactivate
             * after run) had a fatal slot-reuse race that perfectly
             * matched this game's splash pattern
             *   ab.run() { l.c++; l.a:Timer.cancel(); repaint();
             *               serviceRepaints() -> paint() -> new k()
             *               -> new Timer().schedule(newAb, 2000); }
             * The cancel() inside run() freed slot i; schedule() inside
             * the paint (nested via serviceRepaints) reused slot i for
             * the NEXT splash step; when run() finally returned, the pump
             * blindly executed "g_timers[i].active = false" - killing the
             * freshly scheduled task. The auto-advance chain died after
             * ONE firing (c stuck at 1, mes.png "black-ish" screen with
             * every later repaint NPE-ing on the draw-once nulled image).
             * Pre-deactivation also shrinks the double-fire window of the
             * v34 VmTest fix: concurrent pumps see active==false the
             * moment execution starts. */
            JavaObject* fired_task = g_timers[i].timer_task;
            jlong fired_period = g_timers[i].period_ms;
            if (fired_period <= 0) {
                /* One-shot: consume NOW - the slot is free for reuse by
                 * anything the task schedules while it runs. */
                g_timers[i].active = false;
                g_timers[i].timer_task = NULL;
                g_timer_count--;
            }

            /* Execute in main thread */
            JavaValue task_arg = { .ref = fired_task };
            JavaValue result;
            JavaThread* thread = jvm_current_thread(jvm);
            if (thread && jvm->running) {
                execute_method(jvm, thread, run_method, &task_arg, &result);
                fprintf(stderr, "[TIMER] Timer[%d] run() completed\n", i);
            }

            /* Handle periodic vs one-shot */
            if (fired_period > 0) {
                /* Periodic: re-arm ONLY if the slot still holds OUR task.
                 * run() may have cancelled us (slot freed/reused by a new
                 * schedule) - touching the slot then would corrupt the
                 * new task (bogus next_run_time on someone else's entry,
                 * or deactivating it if the new task is a one-shot). */
                if (g_timers[i].active && g_timers[i].timer_task == fired_task) {
                    g_timers[i].next_run_time = (jlong)now + g_timers[i].period_ms;
                }
                /* else: cancelled (or slot reused) during run() - the
                 * cancel path already updated g_timer_count. */
            }
        }
    }

    /* v34: release the pump guard (see comment above) */
    tls_in_jvm_process_timers = 0;
    pthread_mutex_unlock(&g_timers_pump_mutex);
}

static JavaValue native_timer_schedule_task_delay(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)arg_count;
    (void)thread;
    JavaObject* timer_task = (JavaObject*)args[1].ref;
    jlong delay_ms = args[2].j;
    
    if (!timer_task) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    
    /* Initialize start time on first use */
    if (g_start_time_ms == 0) {
        g_start_time_ms = get_time_ms();
    }
    
    /* Find free slot */
    int slot = -1;
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!g_timers[i].active) {
            slot = i;
            break;
        }
    }
    
    if (slot < 0) {
        NATIVE_DEBUG("No free timer slots!");
        return NATIVE_RETURN_VOID();
    }
    
    uint64_t now = timer_virt_now();
    g_timers[slot].timer_task = timer_task;
    g_timers[slot].next_run_time = (jlong)now + delay_ms;
    g_timers[slot].period_ms = 0;
    g_timers[slot].timer_id = timer_id_for_object((JavaObject*)args[0].ref);
    g_timers[slot].active = true;
    g_timer_count++;
    
    fprintf(stderr, "[TIMER] Scheduled one-shot task at slot %d, delay=%lld ms, now=%" PRIu64 ", fire_at=%lld (class=%s)\n",
            slot, (long long)delay_ms, now, (long long)g_timers[slot].next_run_time,
            timer_task->header.clazz ? timer_task->header.clazz->class_name : "(null)");
    return NATIVE_RETURN_VOID();
}

static JavaValue native_timer_schedule_task_delay_period(JVM* jvm, JavaThread* thread,
                                                          JavaValue* args, int arg_count) {
    (void)arg_count;
    (void)thread;
    JavaObject* timer_task = (JavaObject*)args[1].ref;
    jlong delay_ms = args[2].j;
    jlong period_ms = args[3].j;  /* FIX-19l: was args[4] - out of bounds!
                                     compact layout: [0]=this [1]=task [2]=delay [3]=period */
    
    if (!timer_task) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    
    /* Initialize start time on first use */
    if (g_start_time_ms == 0) {
        g_start_time_ms = get_time_ms();
    }
    
    /* Find free slot */
    int slot = -1;
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!g_timers[i].active) {
            slot = i;
            break;
        }
    }
    
    if (slot < 0) {
        NATIVE_DEBUG("No free timer slots!");
        return NATIVE_RETURN_VOID();
    }
    
    uint64_t now = timer_virt_now();
    g_timers[slot].timer_task = timer_task;
    g_timers[slot].next_run_time = (jlong)now + delay_ms;
    g_timers[slot].period_ms = period_ms;
    g_timers[slot].timer_id = timer_id_for_object((JavaObject*)args[0].ref);
    g_timers[slot].active = true;
    g_timer_count++;
    
    NATIVE_DEBUG("Scheduled periodic task at slot %d, delay=%ld ms, period=%ld ms", 
            slot, (long)delay_ms, (long)period_ms);
    return NATIVE_RETURN_VOID();
}

static JavaValue native_timer_cancel(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    
    /* FIX-19l: cancel ONLY the timers owned by this java.util.Timer
     * instance. The old code wiped every slot globally, killing foreign
     * timers (e.g. the VmTest canvas repaint timer) and freezing UI. */
    int my_id = timer_id_for_object((JavaObject*)args[0].ref);
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (g_timers[i].active && g_timers[i].timer_id == my_id) {
            g_timers[i].active = false;
            g_timers[i].timer_task = NULL;
            g_timer_count--;
        }
    }
    
    NATIVE_DEBUG("Timer cancelled (id=%d)", my_id);
    return NATIVE_RETURN_VOID();
}

/* FIX-19l: TimerTask.cancel() was an unregistered stub - the task stayed
 * in the queue and kept firing after cancel ("cancel stops future runs"). */
static JavaValue native_timertask_cancel(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* task = (JavaObject*)args[0].ref;
    if (!task) {
        return NATIVE_RETURN_INT(0);
    }
    int cancelled = 0;
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (g_timers[i].active && g_timers[i].timer_task == task) {
            g_timers[i].active = false;
            g_timers[i].timer_task = NULL;
            g_timer_count--;
            cancelled = 1;
        }
    }
    return NATIVE_RETURN_INT(cancelled);
}

/* FIX(shutdown-race): true while the platform is shutting down and at least
 * one real-pthread Java thread is still executing. Callers must NOT free the
 * JVM/heap under such threads; process exit reclaims everything safely. */
static volatile bool g_vm_shutting_down = false;

bool vm_threads_busy(void) {
    g_vm_shutting_down = true;
    for (int i = 0; i < MAX_PTHREADS; i++) {
        if (g_pthread_running[i]) return true;
    }
    return false;
}

void init_java_util_timer(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"java/util/Timer", "schedule", "(Ljava/util/TimerTask;J)V", native_timer_schedule_task_delay},
        {"java/util/Timer", "schedule", "(Ljava/util/TimerTask;JJ)V", native_timer_schedule_task_delay_period},
        {"java/util/Timer", "scheduleAtFixedRate", "(Ljava/util/TimerTask;JJ)V", native_timer_schedule_task_delay_period},
        {"java/util/Timer", "cancel", "()V", native_timer_cancel},
        {"java/util/TimerTask", "cancel", "()Z", native_timertask_cancel},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
    NATIVE_DEBUG("Registered java/util/Timer native methods");
}

/*
 * Call a native method
 */
int native_call(JVM* jvm, JavaThread* thread, JavaMethod* method, 
                JavaValue* args, JavaValue* result) {
    if (!method || !method->clazz) {
        NATIVE_DEBUG("native_call: NULL method or class");
        return -1;
    }
    
    /* CRITICAL FIX: If there's a pending exception from a previous context,
     * clear it before executing this native method. This handles the case where
     * an exception was thrown but not properly cleared after being handled.
     */
    if (thread->pending_exception) {
        JavaClass* ex_class = thread->pending_exception->header.clazz;
        NATIVE_DEBUG("WARNING: clearing stale pending_exception before %s.%s%s (exception: %s)",
                method->clazz->class_name, method->name, method->descriptor,
                ex_class ? ex_class->class_name : "??");
        thread->pending_exception = NULL;  /* Clear it */
    }
    
    /* Find native method handler */
    NativeMethod handler = native_find(jvm, 
        method->clazz->class_name, method->name, method->descriptor);
    
    /* DEBUG: Log native lookup for Exception methods */
    if (method->name && strstr(method->name, "toString")) {
        if (g_j2me_runtime_debug) fprintf(stderr, "[DEBUG] native_call: looking for %s.%s%s, handler=%p\n",
                method->clazz->class_name, method->name, method->descriptor, (void*)handler);
    }
    
    if (!handler) {
        NATIVE_DEBUG("Unimplemented native method: %s.%s%s",
                method->clazz->class_name, method->name, method->descriptor);

        /* v35: loud, once-per-signature diagnostic. Silent default returns
         * hid API gaps for years (Form.append(String) -> empty Form menus,
         * Graphics.drawChar -> invisible glyphs). Any [NATIVE-MISSING] line
         * in a user log points at exactly the API the midlet needs. */
        {
            static char v35_reported[64][192];
            static int v35_reported_count = 0;
            char sig[192];
            snprintf(sig, sizeof(sig), "%s.%s%s",
                     method->clazz->class_name ? method->clazz->class_name : "?",
                     method->name ? method->name : "?",
                     method->descriptor ? method->descriptor : "");
            int seen = 0;
            for (int i = 0; i < v35_reported_count; i++) {
                if (strcmp(v35_reported[i], sig) == 0) { seen = 1; break; }
            }
            if (!seen && v35_reported_count < 64) {
                /* memcpy + manual NUL: strncpy from a same-size source buffer
                 * triggers -Wstringop-truncation (source len == dest size-1). */
                size_t v35_len = strlen(sig);
                if (v35_len > sizeof(v35_reported[0]) - 1) {
                    v35_len = sizeof(v35_reported[0]) - 1;
                }
                memcpy(v35_reported[v35_reported_count], sig, v35_len);
                v35_reported[v35_reported_count][v35_len] = '\0';
                v35_reported_count++;
                MISSING_LOG("[NATIVE-MISSING] %s -> returning default value "
                         "(midlet may misbehave; please report this line)\n", sig);
            }
        }

        /* Return default value instead of crashing */
        if (result) {
            memset(result, 0, sizeof(JavaValue));
        }
        return 0;  /* Don't fail, just return default */
    }
    
    /* Count arguments */
    int arg_count = count_args(method->descriptor);
    
    /* For non-static methods, args[0] is 'this' reference */
    int is_static = (method->access_flags & ACC_STATIC) != 0;
    int total_args = is_static ? arg_count : arg_count + 1;
    
    /* v36.03 NATIVE-ARGS-ROOTED: publish the caller's args buffer to the GC
     * mark phase for the duration of the handler (see the JavaThread comment
     * in jvm.h). The invoke wrappers already popped these slots from the
     * java frame, so without this the receiver/args are unmarkable while
     * the native runs. Overhead: two stores + one clear per native call. */
    int naf_slot = -1;
    if (thread && args && total_args > 0 &&
        thread->native_arg_depth < NATIVE_ARG_FRAMES_MAX) {
        naf_slot = thread->native_arg_depth++;
        thread->native_arg_frames[naf_slot].args = args;
        thread->native_arg_frames[naf_slot].count = total_args;
    }

    /* [ARGGUARD] v36.49: аргументы native — только РЕПОРТ (defuse=0:
     * массив может принадлежать C-стеку вызывающего моста и быть меньше
     * total_args — запись за его границу портила бы стек). Дикий deref
     * мусора предотвращают гварды receiver в opcodes.c; здесь мы узнаём
     * ИСТОЧНИК (метод+слот+ASCII мусора+адрес моста). */
    argguard_check_array(method->clazz ? method->clazz->class_name : NULL,
                         method->name, method->descriptor, args,
                         total_args, is_static ? 0 : 1,
                         __builtin_return_address(0), 0);

    /* Call the native method */
    JavaValue ret = handler(jvm, thread, args, total_args);

    /* [ARGGUARD] v36.49: возврат native с ref-типом — мусор -> NULL
     * (ret локальна, дефуз безопасен; иначе мусор уедет в operand
     * stack и взорвётся на первом invokevirtual — поле "es/Yeti "). */
    argguard_check_ret(method->clazz ? method->clazz->class_name : NULL,
                       method->name, method->descriptor, &ret,
                       __builtin_return_address(0));
    
    if (naf_slot >= 0) {
        thread->native_arg_frames[naf_slot].args = NULL;
        thread->native_arg_frames[naf_slot].count = 0;
        thread->native_arg_depth--;
    }
    
    if (result) {
        *result = ret;
    }
    
    /* Check for exception */
    if (thread->pending_exception) {
        return -1;
    }
    
    return 0;
}

/*
 * javax.microedition.io.Connector native methods
 * GCF (Generic Connection Framework) implementation
 */

static JavaValue native_connector_open(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    /* args[0] = name (String), args[1] = mode (int), args[2] = timeouts (boolean) */
    JavaString* name_str = (JavaString*)args[0].ref;
    const char* name = name_str ? string_utf8(jvm, name_str) : NULL;
    jint mode = args[1].i;
    jboolean timeouts = args[2].i;
    
    (void)mode; (void)timeouts;
    
    if (!name) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_NULL();
    }
    
    /* Parse protocol */
    if (strncmp(name, "http://", 7) == 0 || strncmp(name, "https://", 8) == 0) {
        /* HTTP connection - return stub */
        JavaClass* http_class = jvm_load_class(jvm, "javax/microedition/io/HttpConnection");
        if (http_class) {
            JavaObject* conn = jvm_new_object(jvm, http_class);
            return NATIVE_RETURN_OBJECT(conn);
        }
    } else if (strncmp(name, "socket://", 9) == 0) {
        JavaClass* socket_class = jvm_load_class(jvm, "javax/microedition/io/SocketConnection");
        if (socket_class) {
            JavaObject* conn = jvm_new_object(jvm, socket_class);
            return NATIVE_RETURN_OBJECT(conn);
        }
    } else if (strncmp(name, "file://", 7) == 0) {
        JavaClass* file_class = jvm_load_class(jvm, "javax/microedition/io/FileConnection");
        if (!file_class) {
            /* Create stub class dynamically */
            file_class = get_or_create_stub_class(jvm, "javax/microedition/io/FileConnection");
        }
        if (file_class) {
            JavaObject* conn = jvm_new_object(jvm, file_class);
            return NATIVE_RETURN_OBJECT(conn);
        }
    }
    
    /* Generic StreamConnection for other protocols */
    JavaClass* stream_class = jvm_load_class(jvm, "javax/microedition/io/StreamConnection");
    if (stream_class) {
        JavaObject* conn = jvm_new_object(jvm, stream_class);
        return NATIVE_RETURN_OBJECT(conn);
    }
    
    return NATIVE_RETURN_NULL();
}

static JavaValue native_connector_openInputStream(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* conn = (JavaObject*)args[0].ref;
    (void)conn;  /* stub: connection type irrelevant */
    
    
    /* Create ByteArrayInputStream stub */
    JavaClass* bais_class = jvm_load_class(jvm, "java/io/ByteArrayInputStream");
    if (bais_class) {
        JavaObject* stream = jvm_new_object(jvm, bais_class);
        return NATIVE_RETURN_OBJECT(stream);
    }
    
    return NATIVE_RETURN_NULL();
}

static JavaValue native_connector_openOutputStream(JVM* jvm, JavaThread* thread,
                                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* conn = (JavaObject*)args[0].ref;
    (void)conn;  /* stub: connection type irrelevant */
    
    
    /* Create ByteArrayOutputStream stub */
    JavaClass* baos_class = jvm_load_class(jvm, "java/io/ByteArrayOutputStream");
    if (baos_class) {
        JavaObject* stream = jvm_new_object(jvm, baos_class);
        return NATIVE_RETURN_OBJECT(stream);
    }
    
    return NATIVE_RETURN_NULL();
}

/*
 * HttpConnection interface methods (stub implementations - no real network)
 * These return error codes to indicate network access is not available
 */

/* HttpConnection.setRequestMethod(String method) - no-op stub */
static JavaValue native_http_setRequestMethod(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    (void)args;
    /* args[0] = this, args[1] = method String */
    /* Just ignore - no actual network connection */
    return NATIVE_RETURN_VOID();
}

/* HttpConnection.setRequestProperty(String key, String value) - no-op stub */
static JavaValue native_http_setRequestProperty(JVM* jvm, JavaThread* thread,
                                                 JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    (void)args;
    /* args[0] = this, args[1] = key, args[2] = value */
    /* Just ignore - no actual network connection */
    return NATIVE_RETURN_VOID();
}

/* HttpConnection.getResponseCode() - return error code */
static JavaValue native_http_getResponseCode(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    (void)args;
    /* args[0] = this */
    /* Return 503 Service Unavailable to indicate network not available */
    return NATIVE_RETURN_INT(503);
}

/* ContentConnection.getLength() - return unknown length */
static JavaValue native_http_getLength(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    (void)args;
    /* args[0] = this */
    /* Return -1 to indicate unknown length */
    JavaValue result;
    result.j = -1;  /* long value */
    return result;
}

/* HttpConnection.getHeaderField(String name) - return null */
static JavaValue native_http_getHeaderField(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    (void)args;
    /* args[0] = this, args[1] = name */
    /* Return null - no headers available */
    return NATIVE_RETURN_NULL();
}

/* ContentConnection.getType() - return null */
static JavaValue native_http_getType(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    (void)args;
    /* args[0] = this */
    /* Return null - no content type available */
    return NATIVE_RETURN_NULL();
}

/* Connection.close() - no-op stub */
static JavaValue native_http_close(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    (void)args;
    /* args[0] = this */
    /* No-op - nothing to close */
    return NATIVE_RETURN_VOID();
}

void init_javax_microedition_io(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"javax/microedition/io/Connector", "open", "(Ljava/lang/String;)Ljavax/microedition/io/Connection;", native_connector_open},
        {"javax/microedition/io/Connector", "open", "(Ljava/lang/String;IZ)Ljavax/microedition/io/Connection;", native_connector_open},
        {"javax/microedition/io/StreamConnection", "openInputStream", "()Ljava/io/InputStream;", native_connector_openInputStream},
        {"javax/microedition/io/StreamConnection", "openOutputStream", "()Ljava/io/OutputStream;", native_connector_openOutputStream},
        {"javax/microedition/io/ContentConnection", "openInputStream", "()Ljava/io/InputStream;", native_connector_openInputStream},
        {"javax/microedition/io/ContentConnection", "openOutputStream", "()Ljava/io/OutputStream;", native_connector_openOutputStream},
        {"javax/microedition/io/ContentConnection", "getLength", "()J", native_http_getLength},
        {"javax/microedition/io/ContentConnection", "getType", "()Ljava/lang/String;", native_http_getType},
        {"javax/microedition/io/HttpConnection", "openInputStream", "()Ljava/io/InputStream;", native_connector_openInputStream},
        {"javax/microedition/io/HttpConnection", "openOutputStream", "()Ljava/io/OutputStream;", native_connector_openOutputStream},
        {"javax/microedition/io/HttpConnection", "setRequestMethod", "(Ljava/lang/String;)V", native_http_setRequestMethod},
        {"javax/microedition/io/HttpConnection", "setRequestProperty", "(Ljava/lang/String;Ljava/lang/String;)V", native_http_setRequestProperty},
        {"javax/microedition/io/HttpConnection", "getResponseCode", "()I", native_http_getResponseCode},
        {"javax/microedition/io/HttpConnection", "getLength", "()J", native_http_getLength},
        {"javax/microedition/io/HttpConnection", "getHeaderField", "(Ljava/lang/String;)Ljava/lang/String;", native_http_getHeaderField},
        {"javax/microedition/io/HttpConnection", "getType", "()Ljava/lang/String;", native_http_getType},
        {"javax/microedition/io/Connection", "close", "()V", native_http_close},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
    NATIVE_DEBUG("Registered javax.microedition.io native methods");
}

/*
 * java.util.TimeZone native methods
 */

static JavaValue native_timezone_getTimeZone(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    
    /* STATIC METHOD - args[0] = timezone ID string */
    JavaString* id_str = (JavaString*)args[0].ref;
    const char* id = id_str ? string_utf8(jvm, id_str) : NULL;
    
    NATIVE_DEBUG("TimeZone.getTimeZone('%s')", id ? id : "null");
    
    /* Create a TimeZone object */
    JavaClass* tz_class = jvm_load_class(jvm, "java/util/TimeZone");
    if (!tz_class) {
        NATIVE_DEBUG("TimeZone.getTimeZone: TimeZone class not found!");
        return NATIVE_RETURN_NULL();
    }
    
    JavaObject* tz = jvm_new_object(jvm, tz_class);
    if (!tz) {
        return NATIVE_RETURN_NULL();
    }
    
    /* Set the ID field */
    if (id) {
        native_set_field_value(tz, "ID", (JavaValue){ .ref = id_str });
    } else {
        JavaString* default_id = jvm_new_string(jvm, "GMT");
        native_set_field_value(tz, "ID", (JavaValue){ .ref = default_id });
    }
    
    /* Set rawOffset — we'll always use GMT for simplicity.
     * Real offset requires timezone database parsing.
     * Most midlets only need timezone for display, not calculation. */
    /* Determine offset from the requested timezone */
    int raw_offset = 0;  /* default: GMT */
    if (id) {
        /* Simple heuristic for common timezone names */
        if (strstr(id, "Europe/")) {
            /* Try to use system timezone */
            time_t now = time(NULL);
            struct tm local_tm = {0}, gmt_tm = {0};
#ifdef _WIN32
            localtime_s(&local_tm, &now);
            gmtime_s(&gmt_tm, &now);
#else
            localtime_r(&now, &local_tm);
            gmtime_r(&now, &gmt_tm);
#endif
            raw_offset = (int)((mktime(&local_tm) - mktime(&gmt_tm)) * 1000);
        } else if (strstr(id, "GMT") || strstr(id, "UTC")) {
            /* Parse GMT+hh:mm format */
            const char* p = id;
            while (*p && *p != '+' && *p != '-') p++;
            if (*p) {
                int sign = (*p == '-') ? -1 : 1;
                int hours = 0, minutes = 0;
                if (sscanf(p + 1, "%d:%d", &hours, &minutes) >= 1) {
                    raw_offset = sign * (hours * 3600 + minutes * 60) * 1000;
                }
            }
        }
    }
    
    native_set_field_value(tz, "rawOffset", (JavaValue){ .i = raw_offset });
    
    NATIVE_DEBUG("TimeZone.getTimeZone('%s') → TimeZone{offset=%d}", id ? id : "GMT", raw_offset);
    return NATIVE_RETURN_OBJECT(tz);
}

static JavaValue native_timezone_getDefault(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)args; (void)arg_count;
    
    NATIVE_DEBUG("TimeZone.getDefault() called");
    
    /* Use system timezone */
    JavaString* tz_id = jvm_new_string(jvm, "GMT");
    JavaValue tz_args[1];
    tz_args[0].ref = tz_id;
    return native_timezone_getTimeZone(jvm, thread, tz_args, 1);
}

/*
 * java.util.Calendar native methods
 *
 * v20 (P0-6): the Calendar stub now behaves like a real GregorianCalendar.
 * The single source of truth is the `time` (J, epoch millis) field; every
 * get()/set()/add()/roll() decomposes/recomposes it via localtime/mktime so
 * the device timezone (and DST) is honored. The legacy convenience fields
 * (year/month/dayOfMonth/...) are refreshed after each mutation for any
 * code that still reads them directly.
 */

/* --- Calendar field decomposition helpers --- */
typedef struct {
    int year;        /* e.g. 2026 */
    int month0;      /* 0..11 */
    int day;         /* 1..31 */
    int hour;        /* 0..23 */
    int minute;      /* 0..59 */
    int second;      /* 0..59 */
    int ms;          /* 0..999 */
    int yday;        /* 0..365 */
    int wday;        /* 0=Sunday..6 */
} CalParts;

static void cal_decompose(jlong millis, CalParts* p) {
    time_t secs = (time_t)(millis / 1000);
    p->ms = (int)(millis % 1000);
    if (p->ms < 0) { p->ms += 1000; secs -= 1; }
    struct tm tmv;
#if !defined(_WIN32)
    localtime_r(&secs, &tmv);
#else
    { struct tm* t = localtime(&secs); tmv = *t; }
#endif
    p->year   = tmv.tm_year + 1900;
    p->month0 = tmv.tm_mon;
    p->day    = tmv.tm_mday;
    p->hour   = tmv.tm_hour;
    p->minute = tmv.tm_min;
    p->second = tmv.tm_sec;
    p->yday   = tmv.tm_yday;
    p->wday   = tmv.tm_wday;
}

static jlong cal_compose(const CalParts* p) {
    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year  = p->year - 1900;
    tmv.tm_mon   = p->month0;
    tmv.tm_mday  = p->day;
    tmv.tm_hour  = p->hour;
    tmv.tm_min   = p->minute;
    tmv.tm_sec   = p->second;
    tmv.tm_isdst = -1; /* let mktime resolve DST */
    time_t secs = mktime(&tmv);
    return (jlong)secs * 1000 + p->ms;
}

/* Refresh the legacy convenience fields on the stub object. */
static void cal_sync_convenience_fields(JVM* jvm, JavaObject* cal, const CalParts* p) {
    (void)jvm;
    JavaValue v;
    memset(&v, 0, sizeof(v));
    v.i = p->year;           native_set_field_value(cal, "year", v);
    v.i = p->month0;         native_set_field_value(cal, "month", v);
    v.i = p->day;            native_set_field_value(cal, "day", v);
    v.i = p->day;            native_set_field_value(cal, "dayOfMonth", v);
    v.i = p->hour;           native_set_field_value(cal, "hour", v);
    v.i = p->hour;           native_set_field_value(cal, "hourOfDay", v);
    v.i = p->minute;         native_set_field_value(cal, "minute", v);
    v.i = p->second;         native_set_field_value(cal, "second", v);
}

static jlong cal_get_time_millis(JavaObject* cal) {
    return cal ? native_get_field_value(cal, "time").j : 0;
}

static void cal_set_time_millis(JVM* jvm, JavaObject* cal, jlong millis) {
    JavaValue v; memset(&v, 0, sizeof(v));
    v.j = millis;
    native_set_field_value(cal, "time", v);
    CalParts p;
    cal_decompose(millis, &p);
    cal_sync_convenience_fields(jvm, cal, &p);
}

/* java.util.Calendar field numbers (JVMS-visible API constants) */
#define CAL_FIELD_ERA            0
#define CAL_FIELD_YEAR           1
#define CAL_FIELD_MONTH          2
#define CAL_FIELD_WEEK_OF_YEAR   3
#define CAL_FIELD_WEEK_OF_MONTH  4
#define CAL_FIELD_DAY_OF_MONTH   5
#define CAL_FIELD_DAY_OF_YEAR    6
#define CAL_FIELD_DAY_OF_WEEK    7
#define CAL_FIELD_DAY_OF_WEEK_IN_MONTH 8
#define CAL_FIELD_AM_PM          9
#define CAL_FIELD_HOUR           10
#define CAL_FIELD_HOUR_OF_DAY    11
#define CAL_FIELD_MINUTE         12
#define CAL_FIELD_SECOND         13
#define CAL_FIELD_MILLISECOND    14

static jint cal_get_field(JavaObject* cal, jint field) {
    CalParts p;
    cal_decompose(cal_get_time_millis(cal), &p);
    switch (field) {
        case CAL_FIELD_ERA:          return p.year > 0 ? 1 : 0;
        case CAL_FIELD_YEAR:         return p.year;
        case CAL_FIELD_MONTH:        return p.month0;
        case CAL_FIELD_DAY_OF_MONTH: return p.day;
        case CAL_FIELD_HOUR_OF_DAY:  return p.hour;
        case CAL_FIELD_MINUTE:       return p.minute;
        case CAL_FIELD_SECOND:       return p.second;
        case CAL_FIELD_MILLISECOND:  return p.ms;
        case CAL_FIELD_DAY_OF_YEAR:  return p.yday + 1;
        case CAL_FIELD_DAY_OF_WEEK:  return p.wday + 1; /* Java: 1=Sunday */
        case CAL_FIELD_AM_PM:        return p.hour >= 12 ? 1 : 0;
        case CAL_FIELD_HOUR:         return p.hour % 12;
        case CAL_FIELD_WEEK_OF_YEAR: return (p.yday + 7 - ((p.wday + 7 - 1) % 7)) / 7 + 1;
        case CAL_FIELD_WEEK_OF_MONTH: return (p.day + 6 - ((p.wday + 7 - p.day % 7) % 7)) / 7;
        case CAL_FIELD_DAY_OF_WEEK_IN_MONTH: return (p.day - 1) / 7 + 1;
        default: return 0;
    }
}

static void cal_apply_set_field(JVM* jvm, JavaObject* cal, jint field, jint value) {
    CalParts p;
    cal_decompose(cal_get_time_millis(cal), &p);
    switch (field) {
        case CAL_FIELD_ERA:          break; /* ignore for CE-only games */
        case CAL_FIELD_YEAR:         p.year = value; break;
        case CAL_FIELD_MONTH:        p.month0 = value; break;
        case CAL_FIELD_DAY_OF_MONTH: p.day = value; break;
        case CAL_FIELD_HOUR_OF_DAY:  p.hour = value; break;
        case CAL_FIELD_HOUR:         p.hour = (p.hour / 12) * 12 + (value % 12); break;
        case CAL_FIELD_MINUTE:       p.minute = value; break;
        case CAL_FIELD_SECOND:       p.second = value; break;
        case CAL_FIELD_MILLISECOND:  p.ms = value; break;
        case CAL_FIELD_DAY_OF_YEAR: {
            /* set day-of-year within the current year */
            struct tm t; memset(&t, 0, sizeof(t));
            t.tm_year = p.year - 1900; t.tm_mon = 0; t.tm_mday = 1; t.tm_isdst = -1;
            time_t jan1 = mktime(&t);
            time_t want = jan1 + (time_t)(value - 1) * 86400;
            CalParts q; cal_decompose((jlong)want * 1000, &q);
            p.day = q.day; p.month0 = q.month0;
            break;
        }
        default: break; /* WEEK_OF_x, AM_PM etc. are derived-only */
    }
    cal_set_time_millis(jvm, cal, cal_compose(&p));
}

static void cal_apply_add(JVM* jvm, JavaObject* cal, jint field, jint amount) {
    CalParts p;
    cal_decompose(cal_get_time_millis(cal), &p);
    switch (field) {
        case CAL_FIELD_YEAR:         p.year += amount; break;
        case CAL_FIELD_MONTH:        p.month0 += amount; break;  /* mktime normalizes */
        case CAL_FIELD_DAY_OF_MONTH:
        case CAL_FIELD_DAY_OF_YEAR:  break; /* handled below via seconds */
        case CAL_FIELD_HOUR_OF_DAY:
        case CAL_FIELD_HOUR:         p.hour += amount; break;
        case CAL_FIELD_MINUTE:       p.minute += amount; break;
        case CAL_FIELD_SECOND:       p.second += amount; break;
        case CAL_FIELD_MILLISECOND: {
            cal_set_time_millis(jvm, cal, cal_get_time_millis(cal) + amount);
            return;
        }
        default: return;
    }
    if (field == CAL_FIELD_DAY_OF_MONTH || field == CAL_FIELD_DAY_OF_YEAR) {
        cal_set_time_millis(jvm, cal, cal_get_time_millis(cal) + (jlong)amount * 86400000LL);
        return;
    }
    cal_set_time_millis(jvm, cal, cal_compose(&p));
}

static void cal_apply_roll(JVM* jvm, JavaObject* cal, jint field, jint amount) {
    CalParts p;
    cal_decompose(cal_get_time_millis(cal), &p);
    switch (field) {
        case CAL_FIELD_MONTH: {
            /* roll month within the year, clamp day to the target month */
            int m = ((p.month0 + amount) % 12 + 12) % 12;
            static const int mdays[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
            int dim = mdays[m];
            if (m == 1 && ((p.year % 4 == 0 && p.year % 100 != 0) || p.year % 400 == 0)) dim = 29;
            if (p.day > dim) p.day = dim;
            p.month0 = m;
            cal_set_time_millis(jvm, cal, cal_compose(&p));
            break;
        }
        case CAL_FIELD_DAY_OF_MONTH: {
            int dim;
            {
                CalParts probe = p; probe.day = 1;
                /* days in the current month */
                static const int mdays[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
                dim = mdays[p.month0];
                if (p.month0 == 1 && ((p.year % 4 == 0 && p.year % 100 != 0) || p.year % 400 == 0)) dim = 29;
                (void)probe;
            }
            int d = p.day - 1 + amount;
            d = ((d % dim) + dim) % dim;
            p.day = d + 1;
            cal_set_time_millis(jvm, cal, cal_compose(&p));
            break;
        }
        case CAL_FIELD_HOUR_OF_DAY: p.hour = ((p.hour + amount) % 24 + 24) % 24; goto compose;
        case CAL_FIELD_HOUR:        p.hour = (p.hour / 12) * 12 + (((p.hour % 12) + amount) % 12 + 12) % 12; goto compose;
        case CAL_FIELD_MINUTE:      p.minute = ((p.minute + amount) % 60 + 60) % 60; goto compose;
        case CAL_FIELD_SECOND:      p.second = ((p.second + amount) % 60 + 60) % 60; goto compose;
        case CAL_FIELD_YEAR: {
            /* roll year keeps the month/day */
            p.year += amount;
            goto compose;
        }
        default: return;
    }
    return;
compose:
    cal_set_time_millis(jvm, cal, cal_compose(&p));
}

static JavaValue native_calendar_getInstance(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)thread;
    
    /* getInstance() — no args or getInstance(TimeZone) — 1 arg
     * We ignore the timezone for simplicity and use current system time */
    (void)arg_count;
    (void)args;
    
    
    JavaClass* cal_class = jvm_load_class(jvm, "java/util/Calendar");
    if (!cal_class) {
        return NATIVE_RETURN_NULL();
    }
    
    JavaObject* cal = jvm_new_object(jvm, cal_class);
    if (cal) {
        /* ИСПРАВЛЕНО: используем native_set_field_value */
        time_t now = time(NULL);
        struct tm* tm_info = localtime(&now);
        
        JavaValue time_val = { .j = (jlong)now * 1000 };
        native_set_field_value(cal, "time", time_val);
        
        JavaValue year_val = { .i = tm_info->tm_year + 1900 };
        native_set_field_value(cal, "year", year_val);
        
        JavaValue month_val = { .i = tm_info->tm_mon };
        native_set_field_value(cal, "month", month_val);
        
        JavaValue day_val = { .i = tm_info->tm_mday };
        native_set_field_value(cal, "day", day_val);
        native_set_field_value(cal, "dayOfMonth", day_val);
        
        JavaValue hour_val = { .i = tm_info->tm_hour };
        native_set_field_value(cal, "hour", hour_val);
        native_set_field_value(cal, "hourOfDay", hour_val);
        
        JavaValue min_val = { .i = tm_info->tm_min };
        native_set_field_value(cal, "minute", min_val);
        
        JavaValue sec_val = { .i = tm_info->tm_sec };
        native_set_field_value(cal, "second", sec_val);
    }
    
    return NATIVE_RETURN_OBJECT(cal);
}

static JavaValue native_calendar_getTime(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* cal = (JavaObject*)args[0].ref;
    
    jlong millis = 0;
    
    if (cal) {
        /* ИСПРАВЛЕНО: используем native_get_field_value */
        millis = native_get_field_value(cal, "time").j;
    }
    
    /* Create Date object */
    JavaClass* date_class = jvm_load_class(jvm, "java/util/Date");
    if (date_class) {
        JavaObject* date = jvm_new_object(jvm, date_class);
        if (date) {
            /* v18 FIX: field is named "time" (see stubs.c), not "fastTime". */
            JavaValue val = { .j = millis };
            native_set_field_value(date, "time", val);
        }
        return NATIVE_RETURN_OBJECT(date);
    }
    
    return NATIVE_RETURN_NULL();
}


/* ---- v20 (P0-6): full field get/set/add/roll API ---- */

static JavaValue native_calendar_get(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* cal = (JavaObject*)args[0].ref;
    if (!cal) { native_throw_npe(jvm, thread); return NATIVE_RETURN_INT(0); }
    return NATIVE_RETURN_INT(cal_get_field(cal, args[1].i));
}

static JavaValue native_calendar_set_2(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* cal = (JavaObject*)args[0].ref;
    if (!cal) { native_throw_npe(jvm, thread); return NATIVE_RETURN_VOID(); }
    cal_apply_set_field(jvm, cal, args[1].i, args[2].i);
    return NATIVE_RETURN_VOID();
}

static JavaValue native_calendar_set_3(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* cal = (JavaObject*)args[0].ref;
    if (!cal) { native_throw_npe(jvm, thread); return NATIVE_RETURN_VOID(); }
    CalParts p;
    cal_decompose(cal_get_time_millis(cal), &p);
    p.year = args[1].i; p.month0 = args[2].i; p.day = args[3].i;
    cal_set_time_millis(jvm, cal, cal_compose(&p));
    return NATIVE_RETURN_VOID();
}

static JavaValue native_calendar_set_5(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* cal = (JavaObject*)args[0].ref;
    if (!cal) { native_throw_npe(jvm, thread); return NATIVE_RETURN_VOID(); }
    CalParts p;
    cal_decompose(cal_get_time_millis(cal), &p);
    p.year = args[1].i; p.month0 = args[2].i; p.day = args[3].i;
    p.hour = args[4].i; p.minute = args[5].i;
    cal_set_time_millis(jvm, cal, cal_compose(&p));
    return NATIVE_RETURN_VOID();
}

static JavaValue native_calendar_set_6(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* cal = (JavaObject*)args[0].ref;
    if (!cal) { native_throw_npe(jvm, thread); return NATIVE_RETURN_VOID(); }
    CalParts p;
    cal_decompose(cal_get_time_millis(cal), &p);
    p.year = args[1].i; p.month0 = args[2].i; p.day = args[3].i;
    p.hour = args[4].i; p.minute = args[5].i; p.second = args[6].i;
    cal_set_time_millis(jvm, cal, cal_compose(&p));
    return NATIVE_RETURN_VOID();
}

static JavaValue native_calendar_add(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* cal = (JavaObject*)args[0].ref;
    if (!cal) { native_throw_npe(jvm, thread); return NATIVE_RETURN_VOID(); }
    cal_apply_add(jvm, cal, args[1].i, args[2].i);
    return NATIVE_RETURN_VOID();
}

static JavaValue native_calendar_roll(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* cal = (JavaObject*)args[0].ref;
    if (!cal) { native_throw_npe(jvm, thread); return NATIVE_RETURN_VOID(); }
    cal_apply_roll(jvm, cal, args[1].i, args[2].i);
    return NATIVE_RETURN_VOID();
}

static JavaValue native_calendar_getTimeInMillis(JVM* jvm, JavaThread* thread,
                                                 JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    return NATIVE_RETURN_LONG(cal_get_time_millis((JavaObject*)args[0].ref));
}

static JavaValue native_calendar_setTimeInMillis(JVM* jvm, JavaThread* thread,
                                                 JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* cal = (JavaObject*)args[0].ref;
    if (!cal) { native_throw_npe(jvm, thread); return NATIVE_RETURN_VOID(); }
    cal_set_time_millis(jvm, cal, args[1].j);
    return NATIVE_RETURN_VOID();
}

static JavaValue native_calendar_setTime(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* cal = (JavaObject*)args[0].ref;
    JavaObject* date = (JavaObject*)args[1].ref;
    if (!cal || !date) { native_throw_npe(jvm, thread); return NATIVE_RETURN_VOID(); }
    jlong t = native_get_field_value(date, "time").j;
    cal_set_time_millis(jvm, cal, t);
    return NATIVE_RETURN_VOID();
}

static JavaValue native_calendar_before(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* a = (JavaObject*)args[0].ref;
    JavaObject* b = (JavaObject*)args[1].ref;
    if (!a || !b) { native_throw_npe(jvm, thread); return NATIVE_RETURN_INT(0); }
    return NATIVE_RETURN_INT(cal_get_time_millis(a) < cal_get_time_millis(b));
}

static JavaValue native_calendar_after(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* a = (JavaObject*)args[0].ref;
    JavaObject* b = (JavaObject*)args[1].ref;
    if (!a || !b) { native_throw_npe(jvm, thread); return NATIVE_RETURN_INT(0); }
    return NATIVE_RETURN_INT(cal_get_time_millis(a) > cal_get_time_millis(b));
}

static JavaValue native_calendar_clear(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* cal = (JavaObject*)args[0].ref;
    if (!cal) { native_throw_npe(jvm, thread); return NATIVE_RETURN_VOID(); }
    cal_set_time_millis(jvm, cal, 0);
    return NATIVE_RETURN_VOID();
}

void init_java_util_calendar(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"java/util/Calendar", "getInstance", "()Ljava/util/Calendar;", native_calendar_getInstance},
        {"java/util/Calendar", "getInstance", "(Ljava/util/TimeZone;)Ljava/util/Calendar;", native_calendar_getInstance},
        {"java/util/Calendar", "getTime", "()Ljava/util/Date;", native_calendar_getTime},
            /* v20 (P0-6): full field get/set/add/roll API */
        {"java/util/Calendar", "get", "(I)I", native_calendar_get},
        {"java/util/Calendar", "set", "(II)V", native_calendar_set_2},
        {"java/util/Calendar", "set", "(III)V", native_calendar_set_3},
        {"java/util/Calendar", "set", "(IIIII)V", native_calendar_set_5},
        {"java/util/Calendar", "set", "(IIIIII)V", native_calendar_set_6},
        {"java/util/Calendar", "add", "(II)V", native_calendar_add},
        {"java/util/Calendar", "roll", "(II)V", native_calendar_roll},
        {"java/util/Calendar", "getTimeInMillis", "()J", native_calendar_getTimeInMillis},
        {"java/util/Calendar", "setTimeInMillis", "(J)V", native_calendar_setTimeInMillis},
        {"java/util/Calendar", "setTime", "(Ljava/util/Date;)V", native_calendar_setTime},
        {"java/util/Calendar", "before", "(Ljava/lang/Object;)Z", native_calendar_before},
        {"java/util/Calendar", "after", "(Ljava/lang/Object;)Z", native_calendar_after},
        {"java/util/Calendar", "clear", "()V", native_calendar_clear},
};
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
}

/*
 * java.util.TimeZone native methods registration
 */

void init_java_util_timezone(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"java/util/TimeZone", "getTimeZone", "(Ljava/lang/String;)Ljava/util/TimeZone;", native_timezone_getTimeZone},
        {"java/util/TimeZone", "getDefault", "()Ljava/util/TimeZone;", native_timezone_getDefault},
        /* v20 (P1): instance accessors */
        {"java/util/TimeZone", "getID", "()Ljava/lang/String;", native_timezone_getID},
        {"java/util/TimeZone", "getRawOffset", "()I", native_timezone_getRawOffset},
        {"java/util/TimeZone", "getOffset", "(IIIIII)I", native_timezone_getOffset7},
        {"java/util/TimeZone", "useDaylightTime", "()Z", native_timezone_useDaylightTime},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
}

/*
 * java.util.Date native methods
 */

static JavaValue native_date_init(JVM* jvm, JavaThread* thread,
                                  JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* date = (JavaObject*)args[0].ref;
    
    if (date) {
        /* v18 FIX: field is named "time" (see stubs.c), not "fastTime" —
         * the old name silently missed the field and getTime() returned 0. */
        time_t now = time(NULL);
        JavaValue val = { .j = (jlong)now * 1000 };
        native_set_field_value(date, "time", val);
    }
    
    return NATIVE_RETURN_VOID();
}

/* Date(long millis) constructor - v18 FIX: was not registered at all, so
 * new Date(86400000L) ran the generic no-op stub ctor and getTime() == 0. */
static JavaValue native_date_init_millis(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* date = (JavaObject*)args[0].ref;
    
    if (date) {
        JavaValue val = { .j = args[1].j };
        native_set_field_value(date, "time", val);
    }
    
    return NATIVE_RETURN_VOID();
}

/* Date.setTime(long) - v18 FIX: was not registered; the call was a no-op. */
static JavaValue native_date_setTime(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* date = (JavaObject*)args[0].ref;
    
    if (date) {
        JavaValue val = { .j = args[1].j };
        native_set_field_value(date, "time", val);
    } else {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_date_getTime(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* date = (JavaObject*)args[0].ref;
    
    if (date) {
        /* v18 FIX: field is named "time" (see stubs.c), not "fastTime". */
        jlong time = native_get_field_value(date, "time").j;
        return NATIVE_RETURN_LONG(time);
    }
    
    return NATIVE_RETURN_LONG(0);
}

static JavaValue native_date_toString(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* date = (JavaObject*)args[0].ref;
    
    /* ИСПРАВЛЕНО: используем native_get_field_value */
    jlong millis = 0;
    if (date) {
        millis = native_get_field_value(date, "time").j;
    }
    
    time_t t = (time_t)(millis / 1000);
    struct tm* tm_info = localtime(&t);
    char buf[64];
    strftime(buf, sizeof(buf), "%a %b %d %H:%M:%S %Y", tm_info);
    
    return NATIVE_RETURN_OBJECT(jvm_new_string(jvm, buf));
}

void init_java_util_date(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"java/util/Date", "<init>", "()V", native_date_init},
        {"java/util/Date", "<init>", "(J)V", native_date_init_millis},
        {"java/util/Date", "getTime", "()J", native_date_getTime},
        {"java/util/Date", "setTime", "(J)V", native_date_setTime},
        {"java/util/Date", "toString", "()Ljava/lang/String;", native_date_toString},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
}

/*
 * java.lang.Character native methods
 */

static JavaValue native_character_isDigit(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jint c = args[0].i;
    return NATIVE_RETURN_INT(c >= '0' && c <= '9' ? 1 : 0);
}

static JavaValue native_character_isLetter(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    /* v20 FIX (P1): was ASCII-only — Cyrillic/Greek/CJK reported false. */
    return NATIVE_RETURN_INT(char_is_unicode_letter(args[0].i));
}

static JavaValue native_character_isLetterOrDigit(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jint c = args[0].i;
    return NATIVE_RETURN_INT((char_is_unicode_letter(c) || (c >= '0' && c <= '9')) ? 1 : 0);
}

static JavaValue native_character_isUpperCase(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jint c = args[0].i;
    /* v34.44: was ASCII-only — Cyrillic А..Я (0x410-0x42F), Ё (0x401) and
     * Latin-1 À..Þ (0xC0-0xDE, ÷ excluded) reported false. */
    if (c >= 'A' && c <= 'Z') return NATIVE_RETURN_INT(1);
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return NATIVE_RETURN_INT(1);
    if (c == 0x401) return NATIVE_RETURN_INT(1);
    if (c >= 0x410 && c <= 0x42F) return NATIVE_RETURN_INT(1);
    return NATIVE_RETURN_INT(0);
}

static JavaValue native_character_isLowerCase(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jint c = args[0].i;
    /* v34.44: was ASCII-only — Cyrillic а..я (0x430-0x44F), ё (0x451) and
     * Latin-1 à..þ (0xE0-0xFE, ÿ excluded: it IS lowercase — included) added. */
    if (c >= 'a' && c <= 'z') return NATIVE_RETURN_INT(1);
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return NATIVE_RETURN_INT(1);
    if (c == 0x451) return NATIVE_RETURN_INT(1);
    if (c >= 0x430 && c <= 0x44F) return NATIVE_RETURN_INT(1);
    return NATIVE_RETURN_INT(0);
}

static JavaValue native_character_isWhitespace(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jint c = args[0].i;
    return NATIVE_RETURN_INT((c == ' ' || c == '\t' || c == '\n' ||
                              c == '\r' || c == '\f' || c == 0x1C ||
                              c == 0x1D || c == 0x1E || c == 0x1F) ? 1 : 0);
}

static JavaValue native_character_toUpperCase(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    /* v34.44: shared Unicode-aware converter (ASCII + Latin-1 + Cyrillic). */
    return NATIVE_RETURN_INT((jint)jcs_to_upper_one((jchar)args[0].i));
}

static JavaValue native_character_toLowerCase(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    /* v34.44: shared Unicode-aware converter (ASCII + Latin-1 + Cyrillic). */
    return NATIVE_RETURN_INT((jint)jcs_to_lower_one((jchar)args[0].i));
}

static JavaValue native_character_digit(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jint c = args[0].i;
    jint radix = args[1].i;
    
    int val = -1;
    if (c >= '0' && c <= '9') {
        val = c - '0';
    } else if (c >= 'a' && c <= 'z') {
        val = c - 'a' + 10;
    } else if (c >= 'A' && c <= 'Z') {
        val = c - 'A' + 10;
    }
    
    if (val >= 0 && val < radix) {
        return NATIVE_RETURN_INT(val);
    }
    return NATIVE_RETURN_INT(-1);
}

static JavaValue native_character_forDigit(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jint digit = args[0].i;
    jint radix = args[1].i;
    
    if (digit >= 0 && digit < radix) {
        if (digit < 10) {
            return NATIVE_RETURN_INT('0' + digit);
        } else {
            return NATIVE_RETURN_INT('a' + digit - 10);
        }
    }
    return NATIVE_RETURN_INT(0);
}

void init_java_lang_character(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"java/lang/Character", "isDigit", "(C)Z", native_character_isDigit},
        {"java/lang/Character", "charValue", "()C", native_character_charValue},
        {"java/lang/Character", "isLetter", "(C)Z", native_character_isLetter},
        {"java/lang/Character", "isLetterOrDigit", "(C)Z", native_character_isLetterOrDigit},
        {"java/lang/Character", "isUpperCase", "(C)Z", native_character_isUpperCase},
        {"java/lang/Character", "isLowerCase", "(C)Z", native_character_isLowerCase},
        {"java/lang/Character", "isWhitespace", "(C)Z", native_character_isWhitespace},
        {"java/lang/Character", "toUpperCase", "(C)C", native_character_toUpperCase},
        {"java/lang/Character", "toLowerCase", "(C)C", native_character_toLowerCase},
        {"java/lang/Character", "digit", "(CI)I", native_character_digit},
        {"java/lang/Character", "forDigit", "(II)C", native_character_forDigit},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
}

/*
 * java.lang.Byte and Short native methods
 */

static JavaValue native_byte_parseByte(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    const char* s = str ? string_utf8(jvm, str) : NULL;
    
    if (!s) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_INT(0);
    }
    
    return NATIVE_RETURN_INT((jbyte)atoi(s));
}

static JavaValue native_byte_valueOf(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    jbyte b = (jbyte)args[0].i;

    /* v34.58: весь диапазон Byte кэшируется (-128..127) */
    {
        pthread_mutex_lock(&g_box_cache_mutex);
        JavaObject* cached = box_cache_get_locked(
                jvm, &g_byte_cache[(int)b - BOX_CACHE_MIN],
                "java/lang/Byte", (JavaValue){ .i = b });
        pthread_mutex_unlock(&g_box_cache_mutex);
        if (cached) return NATIVE_RETURN_OBJECT(cached);
    }

    JavaClass* byte_class = jvm_load_class(jvm, "java/lang/Byte");
    if (byte_class) {
        JavaObject* obj = jvm_new_object(jvm, byte_class);
        if (obj) {
            /* ИСПРАВЛЕНО: используем native_set_field_value */
            JavaValue val = { .i = b };
            native_set_field_value(obj, "value", val);
        }
        return NATIVE_RETURN_OBJECT(obj);
    }
    return NATIVE_RETURN_NULL();
}

/* v57: Byte.toString() — the wrapper-classes fell through to
 * Object.toString(), producing "java.lang.Byte@<hash>". Games store
 * level/trigger data as boxed Bytes and round-trip them through
 * Integer.parseInt(obj.toString()) — the hash string threw
 * NumberFormatException and killed the whole trigger system
 * (3D Solid Weapon: 200 exceptions in 3000 frames, all conditions
 * failed, tip texts next to the '!' icon never appeared). */
static JavaValue native_byte_toString_instance(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* o = (JavaObject*)args[0].ref;
    int v = o ? native_get_field_value(o, "value").i : 0;
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", (signed char)v);
    return NATIVE_RETURN_OBJECT(jvm_new_string(jvm, buf));
}

static JavaValue native_short_toString_instance(JVM* jvm, JavaThread* thread,
                                                 JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* o = (JavaObject*)args[0].ref;
    int v = o ? native_get_field_value(o, "value").i : 0;
    char buf[12];
    snprintf(buf, sizeof(buf), "%d", (short)v);
    return NATIVE_RETURN_OBJECT(jvm_new_string(jvm, buf));
}

static JavaValue native_character_toString_instance(JVM* jvm, JavaThread* thread,
                                                     JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* o = (JavaObject*)args[0].ref;
    jint v = o ? native_get_field_value(o, "value").i : 0;
    /* UTF-8 encode the single code point (BMP: 1-3 bytes) */
    char buf[5];
    int n = 0;
    unsigned int c = (unsigned int)v;
    if (c < 0x80) {
        buf[n++] = (char)c;
    } else if (c < 0x800) {
        buf[n++] = (char)(0xC0 | (c >> 6));
        buf[n++] = (char)(0x80 | (c & 0x3F));
    } else {
        buf[n++] = (char)(0xE0 | (c >> 12));
        buf[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
        buf[n++] = (char)(0x80 | (c & 0x3F));
    }
    buf[n] = '\0';
    return NATIVE_RETURN_OBJECT(jvm_new_string(jvm, buf));
}

/* v57: Byte.equals — comparing a Byte against another Byte must compare the
 * value (native_integer_equals rejects non-Integer receivers). */
static JavaValue native_byte_equals(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* a = (JavaObject*)args[0].ref;
    JavaObject* b = (JavaObject*)args[1].ref;
    if (a == b) return NATIVE_RETURN_INT(1);
    if (!a || !b) return NATIVE_RETURN_INT(0);
    JavaClass* cb = b->header.clazz;
    if (!cb || !cb->class_name || strcmp(cb->class_name, "java/lang/Byte") != 0)
        return NATIVE_RETURN_INT(0);
    return NATIVE_RETURN_INT(native_get_field_value(a, "value").i ==
                             native_get_field_value(b, "value").i ? 1 : 0);
}

/* v58: Short.equals \u2014 same as native_byte_equals (receiver must be a
 * Short, compare "value"); Hashtable.get(new Short(9)) depends on it. */
static JavaValue native_short_equals(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* a = (JavaObject*)args[0].ref;
    JavaObject* b = (JavaObject*)args[1].ref;
    if (a == b) return NATIVE_RETURN_INT(1);
    if (!a || !b) return NATIVE_RETURN_INT(0);
    JavaClass* cb = b->header.clazz;
    if (!cb || !cb->class_name || strcmp(cb->class_name, "java/lang/Short") != 0)
        return NATIVE_RETURN_INT(0);
    return NATIVE_RETURN_INT(native_get_field_value(a, "value").i ==
                             native_get_field_value(b, "value").i ? 1 : 0);
}

/* v58: Short.compareTo(Short) \u2014 JLS numeric comparison. */
static JavaValue native_short_compareTo(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* a = (JavaObject*)args[0].ref;
    JavaObject* b = (JavaObject*)args[1].ref;
    if (!a || !b) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_INT(0);
    }
    jint va = (jshort)native_get_field_value(a, "value").i;
    jint vb = (jshort)native_get_field_value(b, "value").i;
    return NATIVE_RETURN_INT(va < vb ? -1 : (va > vb ? 1 : 0));
}

/* v58: Character.equals \u2014 compare "value" chars. */
static JavaValue native_character_equals(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* a = (JavaObject*)args[0].ref;
    JavaObject* b = (JavaObject*)args[1].ref;
    if (a == b) return NATIVE_RETURN_INT(1);
    if (!a || !b) return NATIVE_RETURN_INT(0);
    JavaClass* cb = b->header.clazz;
    if (!cb || !cb->class_name || strcmp(cb->class_name, "java/lang/Character") != 0)
        return NATIVE_RETURN_INT(0);
    return NATIVE_RETURN_INT(native_get_field_value(a, "value").i ==
                             native_get_field_value(b, "value").i ? 1 : 0);
}

/* v57 fwd decls: static toString natives are defined after the instance
 * wrappers below (registration order), so declare them here. */
static JavaValue native_float_toString(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count);
static JavaValue native_double_toString(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count);

/* v57: Float/Double instance toString — same Object.toString() fallback
 * problem as Byte (see above); String.valueOf(floatObj) and ""+floatObj
 * routes through here. */
static JavaValue native_float_toString_instance(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* o = (JavaObject*)args[0].ref;
    jfloat v = o ? native_get_field_value(o, "value").f : 0.0f;
    JavaValue av = { .f = v };
    return native_float_toString(jvm, thread, &av, 1);
}

static JavaValue native_double_toString_instance(JVM* jvm, JavaThread* thread,
                                                 JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* o = (JavaObject*)args[0].ref;
    jdouble v = o ? native_get_field_value(o, "value").d : 0.0;
    JavaValue av = { .d = v };
    return native_double_toString(jvm, thread, &av, 1);
}

static JavaValue native_short_parseShort(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    const char* s = str ? string_utf8(jvm, str) : NULL;
    
    if (!s) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_INT(0);
    }
    
    return NATIVE_RETURN_INT((jshort)atoi(s));
}

static JavaValue native_short_valueOf(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    jshort s = (jshort)args[0].i;

    /* v34.58: кэш -128..127 (JLS 5.1.7) */
    if (s >= BOX_CACHE_MIN && s <= BOX_CACHE_MAX) {
        pthread_mutex_lock(&g_box_cache_mutex);
        JavaObject* cached = box_cache_get_locked(
                jvm, &g_short_cache[(int)s - BOX_CACHE_MIN],
                "java/lang/Short", (JavaValue){ .i = s });
        pthread_mutex_unlock(&g_box_cache_mutex);
        if (cached) return NATIVE_RETURN_OBJECT(cached);
    }

    JavaClass* short_class = jvm_load_class(jvm, "java/lang/Short");
    if (short_class) {
        JavaObject* obj = jvm_new_object(jvm, short_class);
        if (obj) {
            /* ИСПРАВЛЕНО: используем native_set_field_value */
            JavaValue val = { .i = s };
            native_set_field_value(obj, "value", val);
        }
        return NATIVE_RETURN_OBJECT(obj);
    }
    return NATIVE_RETURN_NULL();
}

/* v58: strict signed decimal parser for the String constructors of the
 * numeric wrappers. Mirrors Integer.parseInt semantics: optional +/-,
 * at least one digit, no whitespace, no junk suffix; accumulates in
 * NEGATIVE jlong space so 19-digit garbage cannot wrap silently.
 * Returns 1 + *out on success, 0 if the text is malformed or outside
 * [lo, hi] (caller throws NumberFormatException). */
static int native_parse_decimal_strict(const char* s, jint lo, jint hi,
                                       jint* out) {
    if (!s) return 0;
    int negative = 0;
    const char* p = s;
    if (*p == '-') { negative = 1; p++; }
    else if (*p == '+') { p++; }
    if (*p == '\0') return 0;
    jlong result = 0;
    while (*p) {
        if (*p < '0' || *p > '9') return 0;
        result = result * 10 - (*p - '0');
        if (result < (jlong)-10000000000LL) return 0; /* |v| > 1e10: out of any wrapper range */
        p++;
    }
    jint v;
    if (negative) {
        if (result < (jlong)lo) return 0;
        v = (jint)result;
    } else {
        if (-result > (jlong)hi) return 0;
        v = (jint)(-result);
    }
    if (v < lo || v > hi) return 0;
    *out = v;
    return 1;
}

/* v58: Byte/Short/Character native constructors. All three wrapper
 * classes shipped WITHOUT <init> natives \u2014 `new Byte(x)` hit the
 * INVOKE-MISSING no-op stub, the "value" field stayed 0, and every
 * boxed flag / trigger / counter read back as 0. 3D Solid Weapon:
 * the level-intro text window stores its "already shown" state as
 * boxed values from d.b(); with value==0 the comparison never held,
 * OK closed the window and the state machine reopened it on the next
 * tick (user: "окно с текстом закрывается и тут же открывается снова").
 * Same shape as native_integer_init. */
static JavaValue native_byte_init(JVM* jvm, JavaThread* thread,
                                 JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    if (!obj) return NATIVE_RETURN_VOID();
    JavaValue val = { .i = (jbyte)args[1].i };
    native_set_field_value(obj, "value", val);
    return NATIVE_RETURN_VOID();
}

/* Byte(String) \u2014 JDK: parse, throw NumberFormatException on garbage. */
static JavaValue native_byte_init_string(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaString* str = (JavaString*)args[1].ref;
    const char* utf8 = str ? string_utf8(jvm, str) : NULL;
    jint v;
    if (!utf8 || !native_parse_decimal_strict(utf8, -128, 127, &v)) {
        jvm_throw_by_name(jvm, "java/lang/NumberFormatException", utf8 ? utf8 : "null");
        thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_VOID();
    }
    if (obj) {
        JavaValue val = { .i = (jbyte)v };
        native_set_field_value(obj, "value", val);
    }
    return NATIVE_RETURN_VOID();
}

static JavaValue native_short_init(JVM* jvm, JavaThread* thread,
                                   JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    if (!obj) return NATIVE_RETURN_VOID();
    JavaValue val = { .i = (jshort)args[1].i };
    native_set_field_value(obj, "value", val);
    return NATIVE_RETURN_VOID();
}

/* Short(String) */
static JavaValue native_short_init_string(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaString* str = (JavaString*)args[1].ref;
    const char* utf8 = str ? string_utf8(jvm, str) : NULL;
    jint v;
    if (!utf8 || !native_parse_decimal_strict(utf8, -32768, 32767, &v)) {
        jvm_throw_by_name(jvm, "java/lang/NumberFormatException", utf8 ? utf8 : "null");
        thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_VOID();
    }
    if (obj) {
        JavaValue val = { .i = (jshort)v };
        native_set_field_value(obj, "value", val);
    }
    return NATIVE_RETURN_VOID();
}

static JavaValue native_character_init(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    if (!obj) return NATIVE_RETURN_VOID();
    JavaValue val = { .i = (jchar)(args[1].i & 0xFFFF) };
    native_set_field_value(obj, "value", val);
    return NATIVE_RETURN_VOID();
}

void init_java_lang_byte_short(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"java/lang/Byte", "<init>", "(B)V", native_byte_init},
        {"java/lang/Byte", "<init>", "(Ljava/lang/String;)V", native_byte_init_string},
        {"java/lang/Short", "<init>", "(S)V", native_short_init},
        {"java/lang/Short", "<init>", "(Ljava/lang/String;)V", native_short_init_string},
        {"java/lang/Character", "<init>", "(C)V", native_character_init},
        {"java/lang/Short", "equals", "(Ljava/lang/Object;)Z", native_short_equals},
        {"java/lang/Short", "hashCode", "()I", native_short_shortValue},
        {"java/lang/Short", "compareTo", "(Ljava/lang/Short;)I", native_short_compareTo},
        {"java/lang/Character", "equals", "(Ljava/lang/Object;)Z", native_character_equals},
        {"java/lang/Character", "hashCode", "()I", native_character_charValue},
        {"java/lang/Byte", "parseByte", "(Ljava/lang/String;)B", native_byte_parseByte},
        {"java/lang/Byte", "valueOf", "(B)Ljava/lang/Byte;", native_byte_valueOf},
        {"java/lang/Byte", "byteValue", "()B", native_byte_byteValue},
        {"java/lang/Byte", "intValue", "()I", native_byte_intValue},
        {"java/lang/Byte", "shortValue", "()S", native_byte_shortValue},
        {"java/lang/Byte", "longValue", "()J", native_byte_longValue},
        {"java/lang/Byte", "floatValue", "()F", native_byte_floatValue},
        {"java/lang/Byte", "doubleValue", "()D", native_byte_doubleValue},
        /* v57: Object.toString() fallback produced "java.lang.Byte@hash"
         * — see native_byte_toString_instance comment */
        {"java/lang/Byte", "toString", "()Ljava/lang/String;", native_byte_toString_instance},
        {"java/lang/Byte", "hashCode", "()I", native_byte_byteValue},
        {"java/lang/Byte", "equals", "(Ljava/lang/Object;)Z", native_byte_equals},
        {"java/lang/Short", "parseShort", "(Ljava/lang/String;)S", native_short_parseShort},
        {"java/lang/Short", "valueOf", "(S)Ljava/lang/Short;", native_short_valueOf},
        {"java/lang/Short", "shortValue", "()S", native_short_shortValue},
        {"java/lang/Short", "byteValue", "()B", native_short_byteValue},
        {"java/lang/Short", "intValue", "()I", native_short_intValue},
        {"java/lang/Short", "longValue", "()J", native_short_longValue},
        {"java/lang/Short", "floatValue", "()F", native_short_floatValue},
        {"java/lang/Short", "doubleValue", "()D", native_short_doubleValue},
        {"java/lang/Short", "toString", "()Ljava/lang/String;", native_short_toString_instance},
        {"java/lang/Character", "toString", "()Ljava/lang/String;", native_character_toString_instance},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
}

/*
 * java.lang.Float native methods
 */

static JavaValue native_float_valueOf(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    jfloat value = args[0].f;

    /* v34.59 PERF (3D): кэш boxed-Float для ЧАСТЫХ значений координатной
     * математики. Профиль 3D-игр: координаты/масштабы/цвета — почти всегда
     * небольшие ЦЕЛЫЕ (0.0f, 1.0f, -1.0f, 2.0f...) либо «половинки»
     * (0.5f, -0.5f, 0.25f...). Каждое Float.valueOf без кэша — это
     * jvm_load_class + jvm_new_object (ticket-lock + memset) + проход
     * по полям; на кадровой сетке 60 fps это заметный allocation-rate.
     *
     * Отклонение от HotSpot (который Float НЕ кэширует) сознательное:
     * identity-чувствительный к boxed-Float код (synchronized(f),
     * f1 != f2 по ссылкам) в J2ME-играх не встречается, а equals()
     * работает одинаково. -0.0f НЕ кэшируется (Float.equals(-0.0f,0.0f)
     * == false в Java — не должны получить один объект), NaN/inf
     * отфильтровываются сравнением с целым. */
    if (value >= -64.0f && value <= 64.0f &&
        value == (jfloat)(jint)value &&
        !(value == 0.0f && signbit(value))) {
        jint iv = (jint)value;
        pthread_mutex_lock(&g_box_cache_mutex);
        JavaObject* cached = box_cache_get_locked(
                jvm, &g_float_cache[iv + 64],
                "java/lang/Float", (JavaValue){ .f = value });
        pthread_mutex_unlock(&g_box_cache_mutex);
        if (cached) return NATIVE_RETURN_OBJECT(cached);
        /* промах создания (нет класса/OOM) — откат на общий путь ниже */
    } else {
        /* дробные константы 3D-математики: bit-exact сравнение */
        for (size_t k = 0; k < FLOAT_FRAC_N; k++) {
            if (value == g_float_frac_table[k] &&
                !signbit(g_float_frac_table[k]) == !signbit(value)) {
                pthread_mutex_lock(&g_box_cache_mutex);
                JavaObject* cached = box_cache_get_locked(
                        jvm, &g_float_frac_cache[k],
                        "java/lang/Float", (JavaValue){ .f = value });
                pthread_mutex_unlock(&g_box_cache_mutex);
                if (cached) return NATIVE_RETURN_OBJECT(cached);
                break;
            }
        }
    }

    JavaClass* float_class = jvm_load_class(jvm, "java/lang/Float");
    if (!float_class) return NATIVE_RETURN_NULL();

    JavaObject* obj = jvm_new_object(jvm, float_class);
    if (obj) {
        /* ИСПРАВЛЕНО: используем native_set_field_value */
        JavaValue val = { .f = value };
        native_set_field_value(obj, "value", val);
    }

    return NATIVE_RETURN_OBJECT(obj);
}

static JavaValue native_float_parseFloat(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    
    if (!str) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_FLOAT(0.0f);
    }
    
    const char* cstr = string_utf8(jvm, str);
    float value = 0.0f;
    
    /* FIX-19h: atof() silently returned 0.0 for garbage ("abc" -> 0.0 with
     * no exception) and accepted trailing junk. Use the strict Java grammar
     * validator and throw NumberFormatException on invalid input. */
    if (!cstr || !java_floating_from_string(cstr, NULL, &value)) {
        jvm_throw_by_name(jvm, "java/lang/NumberFormatException",
                          cstr ? cstr : "null");
        thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_FLOAT(0.0f);
    }
    
    return NATIVE_RETURN_FLOAT(value);
}

static JavaValue native_float_floatValue(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    if (!obj) return NATIVE_RETURN_FLOAT(0.0f);
    
    /* ИСПРАВЛЕНО: используем native_get_field_value */
    jfloat value = native_get_field_value(obj, "value").f;
    return NATIVE_RETURN_FLOAT(value);
}

static JavaValue native_float_toString(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    jfloat value = args[0].f;
    
    char buf[48];
    java_format_float(buf, sizeof(buf), value);  /* FIX-19g: Java format */
    
    JavaString* str = jvm_new_string(jvm, buf);
    return NATIVE_RETURN_OBJECT(str);
}

/*
 * java.lang.Double native methods
 */

static JavaValue native_double_valueOf(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jdouble value = args[0].d;
    
    JavaClass* double_class = jvm_load_class(jvm, "java/lang/Double");
    if (!double_class) return NATIVE_RETURN_NULL();
    
    JavaObject* obj = jvm_new_object(jvm, double_class);
    if (obj) {
        /* ИСПРАВЛЕНО: используем native_set_field_value */
        JavaValue val = { .d = value };
        native_set_field_value(obj, "value", val);
    }
    
    return NATIVE_RETURN_OBJECT(obj);
}

static JavaValue native_double_parseDouble(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    
    if (!str) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_DOUBLE(0.0);
    }
    
    const char* cstr = string_utf8(jvm, str);
    double value = 0.0;
    
    /* FIX-19h: strict Java grammar validation (see parseFloat) */
    if (!cstr || !java_floating_from_string(cstr, &value, NULL)) {
        jvm_throw_by_name(jvm, "java/lang/NumberFormatException",
                          cstr ? cstr : "null");
        thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_DOUBLE(0.0);
    }
    
    return NATIVE_RETURN_DOUBLE(value);
}

static JavaValue native_double_doubleValue(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    if (!obj) return NATIVE_RETURN_DOUBLE(0.0);
    
    /* ИСПРАВЛЕНО: используем native_get_field_value */
    jdouble value = native_get_field_value(obj, "value").d;
    return NATIVE_RETURN_DOUBLE(value);
}

static JavaValue native_double_toString(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    jdouble value = args[0].d;
    
    char buf[48];
    java_format_double(buf, sizeof(buf), value);  /* FIX-19g: Java format */
    
    JavaString* str = jvm_new_string(jvm, buf);
    return NATIVE_RETURN_OBJECT(str);
}

/* v18 FIX: bit-conversion and classification natives for Float/Double.
 * These were missing entirely, and native_call() silently returns 0 for
 * unregistered methods - so floatToIntBits(1.5f) returned 0 and
 * Double.isNaN() always returned false. Bit conversions must be raw
 * unions (NOT casts), with NaN canonicalization for the non-Raw variants
 * per the Java spec. */
static JavaValue native_float_floatToIntBits(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    union { jfloat f; jint i; } u;
    u.f = args[0].f;
    /* Java spec: floatToIntBits collapses ALL NaN bit patterns to 0x7FC00000 */
    if ((u.i & 0x7F800000u) == 0x7F800000u && (u.i & 0x007FFFFFu) != 0) {
        u.i = (jint)0x7FC00000u;
    }
    return NATIVE_RETURN_INT(u.i);
}

static JavaValue native_float_floatToRawIntBits(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    union { jfloat f; jint i; } u;
    u.f = args[0].f;
    return NATIVE_RETURN_INT(u.i);
}

static JavaValue native_float_intBitsToFloat(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    union { jint i; jfloat f; } u;
    u.i = args[0].i;
    return NATIVE_RETURN_FLOAT(u.f);
}

static JavaValue native_float_isNaN(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jfloat v = args[0].f;
    return NATIVE_RETURN_INT(v != v ? 1 : 0);
}

static JavaValue native_float_isInfinite(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jfloat v = args[0].f;
    return NATIVE_RETURN_INT(isinf(v) ? 1 : 0);
}

static JavaValue native_double_doubleToLongBits(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    union { jdouble d; jlong j; } u;
    u.d = args[0].d;
    /* Java spec: doubleToLongBits collapses ALL NaN bit patterns to
     * 0x7FF8000000000000 */
    if ((u.j & 0x7FF0000000000000LL) == 0x7FF0000000000000LL &&
        (u.j & 0x000FFFFFFFFFFFFFLL) != 0) {
        u.j = (jlong)0x7FF8000000000000LL;
    }
    return NATIVE_RETURN_LONG(u.j);
}

static JavaValue native_double_doubleToRawLongBits(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    union { jdouble d; jlong j; } u;
    u.d = args[0].d;
    return NATIVE_RETURN_LONG(u.j);
}

static JavaValue native_double_longBitsToDouble(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    union { jlong j; jdouble d; } u;
    u.j = args[0].j;
    return NATIVE_RETURN_DOUBLE(u.d);
}

static JavaValue native_double_isNaN(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jdouble v = args[0].d;
    return NATIVE_RETURN_INT(v != v ? 1 : 0);
}

static JavaValue native_double_isInfinite(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jdouble v = args[0].d;
    return NATIVE_RETURN_INT(isinf(v) ? 1 : 0);
}

/* v38 FIX (BlackShark3D): Float.<init>(F) — store the value.
 * Previously missing entirely: `new Float(f).floatValue()` == 0.0f. */
static JavaValue native_float_init(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* flt_obj = (JavaObject*)args[0].ref;
    if (!flt_obj) return NATIVE_RETURN_VOID();
    JavaValue val;
    memset(&val, 0, sizeof(val));
    val.f = args[1].f;
    native_set_field_value(flt_obj, "value", val);
    return NATIVE_RETURN_VOID();
}

/* v38 FIX: Double.<init>(D) — store the value (2 slots). */
static JavaValue native_double_init(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* dbl_obj = (JavaObject*)args[0].ref;
    if (!dbl_obj) return NATIVE_RETURN_VOID();
    JavaValue val;
    memset(&val, 0, sizeof(val));
    val.d = args[1].d;
    native_set_field_value(dbl_obj, "value", val);
    return NATIVE_RETURN_VOID();
}

void init_java_lang_float_double(JVM* jvm) {
    /* v38 FIX (BlackShark3D): Float/Double constructors must store their
     * argument into the "value" instance field — the stub class used to have
     * NO field and NO constructor, so `new Float(x).floatValue()` always
     * returned 0.0f (game parsed hSegmentSize=0 → div-by-0 → infinite
     * terrain scroll → mission loading froze at 95%). */
    NativeMethodEntry methods[] = {
        {"java/lang/Float", "<init>", "(F)V", native_float_init},
        {"java/lang/Float", "valueOf", "(F)Ljava/lang/Float;", native_float_valueOf},
        {"java/lang/Float", "parseFloat", "(Ljava/lang/String;)F", native_float_parseFloat},
        {"java/lang/Float", "floatValue", "()F", native_float_floatValue},
        {"java/lang/Float", "byteValue", "()B", native_float_byteValue},
        {"java/lang/Float", "shortValue", "()S", native_float_shortValue},
        {"java/lang/Float", "intValue", "()I", native_float_intValue},
        {"java/lang/Float", "longValue", "()J", native_float_longValue},
        {"java/lang/Float", "doubleValue", "()D", native_float_doubleValue},
        {"java/lang/Float", "toString", "(F)Ljava/lang/String;", native_float_toString},
        {"java/lang/Float", "toString", "()Ljava/lang/String;", native_float_toString_instance},
        {"java/lang/Float", "floatToIntBits", "(F)I", native_float_floatToIntBits},
        {"java/lang/Float", "floatToRawIntBits", "(F)I", native_float_floatToRawIntBits},
        {"java/lang/Float", "intBitsToFloat", "(I)F", native_float_intBitsToFloat},
        {"java/lang/Float", "isNaN", "(F)Z", native_float_isNaN},
        {"java/lang/Float", "isInfinite", "(F)Z", native_float_isInfinite},
        {"java/lang/Double", "<init>", "(D)V", native_double_init},
        {"java/lang/Double", "valueOf", "(D)Ljava/lang/Double;", native_double_valueOf},
        {"java/lang/Double", "parseDouble", "(Ljava/lang/String;)D", native_double_parseDouble},
        {"java/lang/Double", "doubleValue", "()D", native_double_doubleValue},
        {"java/lang/Double", "byteValue", "()B", native_double_byteValue},
        {"java/lang/Double", "shortValue", "()S", native_double_shortValue},
        {"java/lang/Double", "intValue", "()I", native_double_intValue},
        {"java/lang/Double", "longValue", "()J", native_double_longValue},
        {"java/lang/Double", "floatValue", "()F", native_double_floatValue},
        {"java/lang/Double", "toString", "(D)Ljava/lang/String;", native_double_toString},
        {"java/lang/Double", "toString", "()Ljava/lang/String;", native_double_toString_instance},
        {"java/lang/Double", "doubleToLongBits", "(D)J", native_double_doubleToLongBits},
        {"java/lang/Double", "doubleToRawLongBits", "(D)J", native_double_doubleToRawLongBits},
        {"java/lang/Double", "longBitsToDouble", "(J)D", native_double_longBitsToDouble},
        {"java/lang/Double", "isNaN", "(D)Z", native_double_isNaN},
        {"java/lang/Double", "isInfinite", "(D)Z", native_double_isInfinite},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
    NATIVE_DEBUG("Registered java/lang/Float and Double native methods");
}

/*
 * javax.microedition.media.Manager native methods
 */

static JavaValue native_manager_createPlayer(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    (void)args;
    
    
    /* Create PlayerImpl (concrete implementation), not Player (interface) */
    JavaClass* player_class = jvm_load_class(jvm, "javax/microedition/media/PlayerImpl");
    if (!player_class) {
        player_class = jvm_load_class(jvm, "javax/microedition/media/Player");
    }
    
    if (player_class) {
        JavaObject* player = jvm_new_object(jvm, player_class);
        return NATIVE_RETURN_OBJECT(player);
    }
    
    return NATIVE_RETURN_NULL();
}

static JavaValue native_manager_playTone(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    (void)args[0].i; (void)args[1].i; (void)args[2].i;  /* tone playback stubbed */
    
    /* TODO: Implement actual tone playback */
    
    return NATIVE_RETURN_VOID();
}

/*
 * Player state constants (JSR-135)
 */
#define PLAYER_UNREALIZED  100
#define PLAYER_REALIZED    200
#define PLAYER_PREFETCHED  300
#define PLAYER_STARTED     400
#define PLAYER_CLOSED      0

/* Helper: get Player state field */
static jint player_get_state(JavaObject* player) {
    if (!player || !player->header.clazz) return PLAYER_CLOSED;
    
    /* Field 1 is 'state' in PlayerImpl */
    if (OBJECT_HAS_FIELDS(player, 2)) {
        return player->fields[1].i;
    }
    return PLAYER_CLOSED;
}

/* Helper: set Player state field */
static void player_set_state(JavaObject* player, jint state) {
    if (!player || !player->header.clazz) return;
    
    /* Field 1 is 'state' in PlayerImpl */
    if (OBJECT_HAS_FIELDS(player, 2)) {
        player->fields[1].i = state;
    }
}

/* Player.getState() */
static JavaValue native_player_getState(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* player = (JavaObject*)args[0].ref;
    
    jint state = player_get_state(player);
    
    JavaValue result = { .i = state };
    return result;
}

/* Player.start() */
static JavaValue native_player_start(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* player = (JavaObject*)args[0].ref;
    
    player_set_state(player, PLAYER_STARTED);
    
    return NATIVE_RETURN_VOID();
}

/* Player.stop() */
static JavaValue native_player_stop(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* player = (JavaObject*)args[0].ref;
    
    player_set_state(player, PLAYER_PREFETCHED);
    
    return NATIVE_RETURN_VOID();
}

/* Player.close() */
static JavaValue native_player_close(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* player = (JavaObject*)args[0].ref;
    
    player_set_state(player, PLAYER_CLOSED);
    
    return NATIVE_RETURN_VOID();
}

/* Player.deallocate() */
static JavaValue native_player_deallocate(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* player = (JavaObject*)args[0].ref;
    
    player_set_state(player, PLAYER_REALIZED);
    
    return NATIVE_RETURN_VOID();
}

/* Player.realize() */
static JavaValue native_player_realize(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* player = (JavaObject*)args[0].ref;
    
    player_set_state(player, PLAYER_REALIZED);
    
    return NATIVE_RETURN_VOID();
}

/* Player.prefetch() */
static JavaValue native_player_prefetch(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* player = (JavaObject*)args[0].ref;
    
    player_set_state(player, PLAYER_PREFETCHED);
    
    return NATIVE_RETURN_VOID();
}

void init_javax_microedition_media_manager(JVM* jvm) {
    NativeMethodEntry methods[] = {
        /* Manager methods */
        {"javax/microedition/media/Manager", "createPlayer", "(Ljava/lang/String;)Ljavax/microedition/media/Player;", native_manager_createPlayer},
        {"javax/microedition/media/Manager", "playTone", "(III)V", native_manager_playTone},
        
        /* Player methods - registered for both interface and implementation */
        {"javax/microedition/media/Player", "getState", "()I", native_player_getState},
        {"javax/microedition/media/Player", "start", "()V", native_player_start},
        {"javax/microedition/media/Player", "stop", "()V", native_player_stop},
        {"javax/microedition/media/Player", "close", "()V", native_player_close},
        {"javax/microedition/media/Player", "deallocate", "()V", native_player_deallocate},
        {"javax/microedition/media/Player", "realize", "()V", native_player_realize},
        {"javax/microedition/media/Player", "prefetch", "()V", native_player_prefetch},
        
        /* Also register for PlayerImpl */
        {"javax/microedition/media/PlayerImpl", "getState", "()I", native_player_getState},
        {"javax/microedition/media/PlayerImpl", "start", "()V", native_player_start},
        {"javax/microedition/media/PlayerImpl", "stop", "()V", native_player_stop},
        {"javax/microedition/media/PlayerImpl", "close", "()V", native_player_close},
        {"javax/microedition/media/PlayerImpl", "deallocate", "()V", native_player_deallocate},
        {"javax/microedition/media/PlayerImpl", "realize", "()V", native_player_realize},
        {"javax/microedition/media/PlayerImpl", "prefetch", "()V", native_player_prefetch},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
    NATIVE_DEBUG("Registered javax.microedition.media.Manager and Player native methods");
}

/*
 * Vendor-specific extensions (Siemens, Samsung, Motorola)
 */

/* Siemens Light control */
static JavaValue native_siemens_light_setColor(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    (void)args[0].i;  /* light color stubbed */
    return NATIVE_RETURN_VOID();
}

/* Siemens Vibrator */
static JavaValue native_siemens_vibrator_start(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    (void)args[0].i;  /* vibration duration stubbed */
    return NATIVE_RETURN_VOID();
}

static JavaValue native_siemens_vibrator_stop(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    return NATIVE_RETURN_VOID();
}

/* Generic helper functions for DRM bypass */
static JavaValue native_return_true(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    return NATIVE_RETURN_INT(1);  /* boolean true */
}

static JavaValue native_return_false(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    return NATIVE_RETURN_INT(0);  /* boolean false */
}

static JavaValue native_return_null(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    return NATIVE_RETURN_NULL();
}

/* Return empty string for DRM bypass - allows games to continue in demo mode */
static JavaValue native_return_ok_string(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)thread; (void)args; (void)arg_count;
    /* Return empty string - compareTo("ok") will return -2, triggering demo mode */
    JavaString* str = jvm_new_string(jvm, "");
    return NATIVE_RETURN_OBJECT(str);
}

static JavaValue native_return_void(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    return NATIVE_RETURN_VOID();
}

static JavaValue native_return_zero(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    return NATIVE_RETURN_INT(0);
}

static JavaValue native_return_empty_string(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)thread; (void)args; (void)arg_count;
    JavaString* str = jvm_new_string(jvm, "");
    return NATIVE_RETURN_OBJECT(str);
}

static JavaValue native_return_identity_bytearray(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    /* Pass through args[2] as the return value (identity for byte array operations) */
    return args[2];
}

static JavaValue native_return_int_100(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    return NATIVE_RETURN_INT(100);
}

/* Samsung Vibration */
static JavaValue native_samsung_vibration_start(JVM* jvm, JavaThread* thread,
                                                 JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    (void)args[0].i;  /* vibration duration stubbed */
    return NATIVE_RETURN_VOID();
}

static JavaValue native_samsung_vibration_stop(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    return NATIVE_RETURN_VOID();
}

void init_vendor_extensions(JVM* jvm) {
    NativeMethodEntry methods[] = {
        /* Siemens */
        {"com/siemens/mp/game/Light", "setColor", "(I)V", native_siemens_light_setColor},
        {"com/siemens/mp/game/Vibrator", "start", "(I)V", native_siemens_vibrator_start},
        {"com/siemens/mp/game/Vibrator", "stop", "()V", native_siemens_vibrator_stop},
        /* Samsung */
        {"com/samsung/util/Vibration", "start", "(I)V", native_samsung_vibration_start},
        {"com/samsung/util/Vibration", "stop", "()V", native_samsung_vibration_stop},
        /* GlomoReg - DRM bypass (always return success) */
        {"GlomoReg/GlomoRegistrator", "CheckMidletsSecutiry", "(Ljava/lang/String;)Z", native_return_true},
        {"GlomoReg/GlomoRegistrator", "CheckMidletsSecutiry", "(Ljava/lang/String;Ljava/lang/String;)Z", native_return_true},
        {"GlomoReg/GlomoRegistrator", "CheckMidletsSecutiry", "(Ljavax/microedition/midlet/MIDlet;[Ljava/lang/String;)Ljava/lang/String;", native_return_ok_string},
        {"GlomoReg/GlomoRegistrator", "RegistrateMidlet", "(Ljava/lang/String;)V", native_return_void},
        {"GlomoReg/GlomoRegistrator", "RegistrateMidlet", "(Ljava/lang/String;Ljava/lang/String;)V", native_return_void},
        {"GlomoReg/GlomoRegistrator", "getRegDate", "()Ljava/lang/String;", native_return_null},
        {"GlomoReg/GlomoRegistrator", "getRegName", "()Ljava/lang/String;", native_return_null},
        {"GlomoReg/GlomoRegistrator", "getRegKey", "()Ljava/lang/String;", native_return_null},
        {"GlomoReg/GlomoRegistrator", "isValid", "()Z", native_return_true},
        {"GlomoReg/GlomoRegistrator", "isTrial", "()Z", native_return_false},

        /* Samsung LCDLight */
        {"com/samsung/util/LCDLight", "isSupported", "()Z", native_return_true},
        {"com/samsung/util/LCDLight", "off", "()V", native_return_void},
        {"com/samsung/util/LCDLight", "on", "(I)V", native_return_void},

        /* Samsung SMS */
        {"com/samsung/util/SMS", "isSupported", "()Z", native_return_true},
        {"com/samsung/util/SMS", "send", "(Lcom/samsung/util/SM;)V", native_return_void},

        /* Samsung SM */
        {"com/samsung/util/SM", "<init>", "()V", native_return_void},
        {"com/samsung/util/SM", "<init>", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V", native_return_void},
        {"com/samsung/util/SM", "getCallbackAddress", "()Ljava/lang/String;", native_return_empty_string},
        {"com/samsung/util/SM", "getData", "()Ljava/lang/String;", native_return_empty_string},
        {"com/samsung/util/SM", "getDestAddress", "()Ljava/lang/String;", native_return_empty_string},
        {"com/samsung/util/SM", "setCallbackAddress", "(Ljava/lang/String;)V", native_return_void},
        {"com/samsung/util/SM", "setData", "(Ljava/lang/String;)V", native_return_void},
        {"com/samsung/util/SM", "setDestAddress", "(Ljava/lang/String;)V", native_return_void},

        /* Samsung AudioClip */
        {"com/samsung/util/AudioClip", "<init>", "(I[BIILjava/lang/String;)V", native_return_void},
        {"com/samsung/util/AudioClip", "<init>", "(ILjava/lang/String;)V", native_return_void},
        {"com/samsung/util/AudioClip", "isSupported", "()Z", native_return_false},
        {"com/samsung/util/AudioClip", "pause", "()V", native_return_void},
        {"com/samsung/util/AudioClip", "play", "(II)V", native_return_void},
        {"com/samsung/util/AudioClip", "resume", "()V", native_return_void},
        {"com/samsung/util/AudioClip", "stop", "()V", native_return_void},

        /* Siemens game APIs */
        {"com/siemens/mp/game/Light", "setLightOn", "()V", native_return_void},
        {"com/siemens/mp/game/Light", "setLightOff", "()V", native_return_void},
        {"com/siemens/mp/game/Vibrator", "startVibrator", "()V", native_return_void},
        {"com/siemens/mp/game/Vibrator", "stopVibrator", "()V", native_return_void},
        {"com/siemens/mp/game/Sound", "playTone", "(II)V", native_return_void},
        {"com/siemens/mp/game/Melody", "stop", "()V", native_return_void},
        {"com/siemens/mp/game/MelodyComposer", "maxLength", "()I", native_return_int_100},
        {"com/siemens/mp/game/GraphicObjectManager", "createTextureBits", "(II[B)[B", native_return_identity_bytearray},

        /* Siemens game.Sprite */
        {"com/siemens/mp/game/Sprite", "<init>", "([BIILcom/siemens/mp/game/ExtendedImage;Lcom/siemens/mp/game/ExtendedImage;I)V", native_return_void},
        {"com/siemens/mp/game/Sprite", "<init>", "(Lcom/siemens/mp/game/ExtendedImage;Lcom/siemens/mp/game/ExtendedImage;I)V", native_return_void},
        {"com/siemens/mp/game/Sprite", "<init>", "(Ljavax/microedition/lcdui/Image;Ljavax/microedition/lcdui/Image;I)V", native_return_void},
        {"com/siemens/mp/game/Sprite", "getFrame", "()I", native_return_zero},
        {"com/siemens/mp/game/Sprite", "getXPosition", "()I", native_return_zero},
        {"com/siemens/mp/game/Sprite", "getYPosition", "()I", native_return_zero},
        {"com/siemens/mp/game/Sprite", "isCollidingWith", "(Lcom/siemens/mp/game/Sprite;)Z", native_return_false},
        {"com/siemens/mp/game/Sprite", "isCollidingWithPos", "(II)Z", native_return_false},
        {"com/siemens/mp/game/Sprite", "setCollisionRectangle", "(IIII)V", native_return_void},
        {"com/siemens/mp/game/Sprite", "setFrame", "(I)V", native_return_void},
        {"com/siemens/mp/game/Sprite", "setPosition", "(II)V", native_return_void},

        /* Siemens game.MelodyComposer - additional methods */
        {"com/siemens/mp/game/MelodyComposer", "<init>", "()V", native_return_void},
        {"com/siemens/mp/game/MelodyComposer", "<init>", "([II)V", native_return_void},
        {"com/siemens/mp/game/MelodyComposer", "appendNote", "(II)V", native_return_void},
        {"com/siemens/mp/game/MelodyComposer", "length", "()I", native_return_zero},
        {"com/siemens/mp/game/MelodyComposer", "resetMelody", "()V", native_return_void},
        {"com/siemens/mp/game/MelodyComposer", "setBPM", "(I)V", native_return_void},
        {"com/siemens/mp/game/MelodyComposer", "getMelody", "()Lcom/siemens/mp/game/Melody;", native_return_null},

        /* Siemens game.Melody - additional methods */
        {"com/siemens/mp/game/Melody", "<init>", "()V", native_return_void},
        {"com/siemens/mp/game/Melody", "play", "()V", native_return_void},

        /* Siemens game.GraphicObject - visibility methods */
        {"com/siemens/mp/game/GraphicObject", "getVisible", "()Z", native_return_true},
        {"com/siemens/mp/game/GraphicObject", "setVisible", "(Z)V", native_return_void},

        /* Siemens game.GraphicObjectManager - additional methods */
        {"com/siemens/mp/game/GraphicObjectManager", "<init>", "()V", native_return_void},
        {"com/siemens/mp/game/GraphicObjectManager", "addObject", "(Lcom/siemens/mp/game/GraphicObject;)V", native_return_void},
        {"com/siemens/mp/game/GraphicObjectManager", "insertObject", "(Lcom/siemens/mp/game/GraphicObject;I)V", native_return_void},
        {"com/siemens/mp/game/GraphicObjectManager", "deleteObject", "(Lcom/siemens/mp/game/GraphicObject;)V", native_return_void},
        {"com/siemens/mp/game/GraphicObjectManager", "getObjectAt", "(I)Lcom/siemens/mp/game/GraphicObject;", native_return_null},
        {"com/siemens/mp/game/GraphicObjectManager", "getObjectPosition", "(Lcom/siemens/mp/game/GraphicObject;)I", native_return_zero},
        {"com/siemens/mp/game/GraphicObjectManager", "paint", "(Lcom/siemens/mp/game/ExtendedImage;II)V", native_return_void},
        {"com/siemens/mp/game/GraphicObjectManager", "paint", "(Ljavax/microedition/lcdui/Image;II)V", native_return_void},

        /* Siemens game.ExtendedImage - additional methods */
        {"com/siemens/mp/game/ExtendedImage", "<init>", "(Ljavax/microedition/lcdui/Image;)V", native_return_void},
        {"com/siemens/mp/game/ExtendedImage", "getImage", "()Ljavax/microedition/lcdui/Image;", native_return_null},
        {"com/siemens/mp/game/ExtendedImage", "getPixel", "(II)I", native_return_zero},
        {"com/siemens/mp/game/ExtendedImage", "setPixel", "(IIB)V", native_return_void},
        {"com/siemens/mp/game/ExtendedImage", "getPixelBytes", "([BIIII)V", native_return_void},
        {"com/siemens/mp/game/ExtendedImage", "setPixels", "([BIIII)V", native_return_void},
        {"com/siemens/mp/game/ExtendedImage", "clear", "(B)V", native_return_void},
        {"com/siemens/mp/game/ExtendedImage", "blitToScreen", "(II)V", native_return_void},

        /* Siemens game.Sprite - paint method */
        {"com/siemens/mp/game/Sprite", "paint", "(Ljavax/microedition/lcdui/Graphics;)V", native_return_void},

        /* Nokia Sound - init methods (non-constructor reinitialization) */
        {"com/nokia/mid/sound/Sound", "init", "([BI)V", native_return_void},
        {"com/nokia/mid/sound/Sound", "init", "(IJ)V", native_return_void},

        /* Nokia M3D Texture - constructor (v17: stores image + format) */
        {"com/nokia/mid/m3d/Texture", "<init>", "(ILjavax/microedition/lcdui/Image;)V", native_m3d_texture_init},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
    NATIVE_DEBUG("Registered vendor-specific extensions including GlomoReg DRM bypass");
}

/* Count stack slots needed for method arguments (long/double take 2 slots) */
int count_args(const char* descriptor) {
    if (!descriptor) return 0;
    
    int count = 0;
    const char* p = descriptor;
    
    /* Skip return type, find args */
    if (*p != '(') return 0;
    p++;
    
    while (*p && *p != ')') {
        switch (*p) {
            case 'B': case 'C': case 'D': case 'F':
            case 'I': case 'J': case 'S': case 'Z':
                count++;
                if (*p == 'J' || *p == 'D') count++;  /* Long/double take 2 slots */
                p++;
                break;
            case 'L':
                count++;
                while (*p && *p != ';') p++;
                if (*p == ';') p++;
                break;
            case '[':
                count++;
                while (*p == '[') p++;
                if (*p == 'L') {
                    while (*p && *p != ';') p++;
                    if (*p == ';') p++;
                } else {
                    p++;
                }
                break;
            default:
                p++;
                break;
        }
    }
    
    return count;
}

/* Cached version of count_args for JavaMethod */
int method_arg_count(JavaMethod* method) {
    if (!method || !method->descriptor) return 0;
    if (method->cached_arg_count >= 0) return method->cached_arg_count;
    method->cached_arg_count = (int16_t)count_args(method->descriptor);
    return method->cached_arg_count;
}

/* ИСПРАВЛЕНО: Count actual Java arguments (not stack slots) 
 * long/double count as 1 argument (not 2 stack slots) */
int count_java_args(const char* descriptor) {
    if (!descriptor) return 0;
    
    int count = 0;
    const char* p = descriptor;
    
    /* Skip return type, find args */
    if (*p != '(') return 0;
    p++;
    
    while (*p && *p != ')') {
        switch (*p) {
            case 'B': case 'C': case 'D': case 'F':
            case 'I': case 'J': case 'S': case 'Z':
                count++;  /* Каждый тип - 1 аргумент */
                p++;
                break;
            case 'L':
                count++;
                while (*p && *p != ';') p++;
                if (*p == ';') p++;
                break;
            case '[':
                count++;
                while (*p == '[') p++;
                if (*p == 'L') {
                    while (*p && *p != ';') p++;
                    if (*p == ';') p++;
                } else {
                    p++;
                }
                break;
            default:
                p++;
                break;
        }
    }
    
    return count;
}

/* Cleanup StringBuffer fallback buffers - call during JVM shutdown */
void native_stringbuffer_cleanup(void) {
    for (int i = 0; i < g_stringbuffer_fallback_count; i++) {
        if (g_stringbuffer_fallback[i].buffer != NULL) {
            free(g_stringbuffer_fallback[i].buffer);
            g_stringbuffer_fallback[i].buffer = NULL;
        }
    }
    
    /* Free the dynamic array itself */
    if (g_stringbuffer_fallback != NULL) {
        free(g_stringbuffer_fallback);
        g_stringbuffer_fallback = NULL;
    }
    
    g_stringbuffer_fallback_count = 0;
    g_stringbuffer_fallback_capacity = 0;
    NATIVE_DEBUG("Cleaned up StringBuffer fallback entries");
}

/* Cleanup all native fallback buffers */
void native_cleanup_fallbacks(void) {
    native_stringbuffer_cleanup();
    NATIVE_DEBUG("All native fallback buffers cleaned up");
}

/* GC notification - called after each GC cycle to clean up stale fallback entries.
 * This fixes the bug where StringBuffer fallback entries persist after their
 * associated objects are garbage collected, leading to string corruption when
 * new objects are allocated at the same memory addresses.
 */
void native_gc_notify(void) {
    extern bool is_heap_ptr_check(void* ptr);
    (void)0;
    
    int cleaned = 0;
    
    /* Clean StringBuffer fallback entries for objects that are no longer valid.
     * An object is invalid if:
     * 1. Its pointer is not in heap bounds, OR
     * 2. Its GC header indicates it's a free block or has invalid type
     */
    for (int i = 0; i < g_stringbuffer_fallback_count; i++) {
        StringBufferFallback* entry = &g_stringbuffer_fallback[i];
        
        if (entry->obj_ptr == NULL) continue;
        
        /* Check if object is still in heap */
        if (!is_heap_ptr_check(entry->obj_ptr)) {
            NATIVE_DEBUG("GC_NOTIFY: StringBuffer entry %d has invalid ptr %p (not in heap)", 
                        i, entry->obj_ptr);
            /* Free the buffer and mark entry as unused */
            if (entry->buffer) {
                free(entry->buffer);
            }
            entry->obj_ptr = NULL;
            entry->buffer = NULL;
            entry->length = 0;
            entry->capacity = 0;
            entry->alloc_id = 0;
            cleaned++;
            continue;
        }
        
        /* Check if object's GC header is valid (not freed) */
        GCObjectHeader* header = (GCObjectHeader*)entry->obj_ptr - 1;
        if (!is_heap_ptr_check(header)) {
            NATIVE_DEBUG("GC_NOTIFY: StringBuffer entry %d has invalid header %p", 
                        i, (void*)header);
            entry->obj_ptr = NULL;
            if (entry->buffer) {
                free(entry->buffer);
            }
            entry->buffer = NULL;
            entry->length = 0;
            entry->capacity = 0;
            entry->alloc_id = 0;
            cleaned++;
            continue;
        }
        
        /* Check if object is marked as free or has invalid type */
        if (header->type == OBJ_TYPE_FREE || header->type > OBJ_TYPE_CLASS) {
            NATIVE_DEBUG("GC_NOTIFY: StringBuffer entry %d object was freed (type=%d)", 
                        i, header->type);
            entry->obj_ptr = NULL;
            if (entry->buffer) {
                free(entry->buffer);
            }
            entry->buffer = NULL;
            entry->length = 0;
            entry->capacity = 0;
            entry->alloc_id = 0;
            cleaned++;
            continue;
        }
        
        /* Check if object's class is StringBuffer or StringBuilder */
        JavaObject* obj = (JavaObject*)entry->obj_ptr;
        if (!obj->header.clazz || !obj->header.clazz->class_name) {
            NATIVE_DEBUG("GC_NOTIFY: StringBuffer entry %d object has no class", i);
            entry->obj_ptr = NULL;
            if (entry->buffer) {
                free(entry->buffer);
            }
            entry->buffer = NULL;
            entry->length = 0;
            entry->capacity = 0;
            entry->alloc_id = 0;
            cleaned++;
            continue;
        }
        
        /* Verify the object is still a StringBuffer/StringBuilder */
        const char* class_name = obj->header.clazz->class_name;
        if (strcmp(class_name, "java/lang/StringBuffer") != 0 && 
            strcmp(class_name, "java/lang/StringBuilder") != 0) {
            NATIVE_DEBUG("GC_NOTIFY: StringBuffer entry %d object is now '%s' (was recycled!)", 
                        i, class_name);
            entry->obj_ptr = NULL;
            if (entry->buffer) {
                free(entry->buffer);
            }
            entry->buffer = NULL;
            entry->length = 0;
            entry->capacity = 0;
            entry->alloc_id = 0;
            cleaned++;
        }
    }
    
    if (cleaned > 0) {
        NATIVE_DEBUG("GC_NOTIFY: Cleaned %d stale StringBuffer fallback entries", cleaned);
    }
    
    /* Also clean up intern string pool */
    cleanup_intern_pool();
}

/* v34.91 MULTI-SESSION FIX (see the note at the box-cache table): drops
 * every pinned cache/singleton that points into the dying JVM's heap.
 * Called from jvm_destroy — keeps the Switch frontend's next game session
 * from reading freed objects (the s_string_class_cache UAF class of bug). */
void native_object_cache_reset(void) {
    memset(g_int_cache, 0, sizeof g_int_cache);
    memset(g_short_cache, 0, sizeof g_short_cache);
    memset(g_byte_cache, 0, sizeof g_byte_cache);
    memset(g_long_cache, 0, sizeof g_long_cache);
    memset(g_bool_cache, 0, sizeof g_bool_cache);
    memset(g_str_int_cache, 0, sizeof g_str_int_cache);
    memset(g_str_bool_cache, 0, sizeof g_str_bool_cache);
    memset(g_str_char_cache, 0, sizeof g_str_char_cache);
    memset(g_float_cache, 0, sizeof g_float_cache);
    memset(g_float_frac_cache, 0, sizeof g_float_frac_cache);
    g_runtime_instance = NULL;
    g_sb_memo_obj = NULL;
    g_sb_memo_clazz = NULL;
    g_sb_memo_idx = -1;
}
