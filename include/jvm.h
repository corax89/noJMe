/*
 * J2ME Emulator - JVM Core Header
 * Cross-platform Java Virtual Machine for MIDP2
 */

#ifndef JVM_H
#define JVM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Platform-specific path separator */
#ifdef _WIN32
    #define PATH_SEPARATOR '\\'
    #define PATH_SEPARATOR_STR "\\"
#else
    #define PATH_SEPARATOR '/'
    #define PATH_SEPARATOR_STR "/"
#endif

/* Version info */
#define J2ME_EMULATOR_VERSION "1.0.0"
#define JAVA_VERSION "1.3"  /* J2ME targets Java 1.3 */

/* Limits */
#define MAX_METHOD_CODE_SIZE 65535
#define MAX_CONSTANT_POOL_SIZE 65535
#define MAX_FIELDS_PER_CLASS 65535
#define MAX_METHODS_PER_CLASS 65535
#define MAX_INTERFACES_PER_CLASS 65535
#define MAX_ATTRIBUTES_PER_CLASS 65535
#define MAX_LOCAL_VARIABLES 65535
#define MAX_OPERAND_STACK 65535
#define MAX_EXCEPTION_TABLE 65535

/* Heap Configuration */
/* v34.9: raised from 16 MB — multi-threaded games (Bounce Tales loader)
 * allocate ~12 MB during chapter loading; with the old 16 MB heap every
 * "New game" triggered a GC storm right in the middle of the loader
 * choreography (GC vs loader thread races -> white-screen hang). */
#define HEAP_INITIAL_SIZE (32 * 1024 * 1024)  /* 32 MB */
/* v36.33: 64 -> 96 MB. Field repro (Jewel Adventure [480x800]): the game
 * re-reads a 1.9 MB resource pack every ~150 ms (its own pack cache resets
 * between tile loads); with the arena capped at 64 MB the first allocation
 * that hit the top landed INSIDE a native (getResourceAsStream), where
 * GC-DEFER-IN-NATIVE (v36.03) rightly refuses an inline stop-the-world —
 * the game got a catchable OOM, its loader thread died and the screen went
 * black. 96 MB gives high-res games ~32 MB of churn headroom; HOS homebrew
 * has hundreds of MB to spare. Desktop/libretro memory cost only grows
 * when a game actually pressures the cap. */
#define HEAP_MAX_SIZE (96 * 1024 * 1024)      /* 96 MB */
#define OBJECT_ALIGNMENT 8

/* Stack Configuration */
#define JAVA_STACK_SIZE (256 * 1024)          /* 256 KB per thread */
#define MAX_JAVA_THREADS 16

/* Java Types */
typedef int8_t   jbyte;
typedef uint8_t  jubyte;
typedef int16_t  jshort;
typedef uint16_t jushort;
typedef int32_t  jint;
typedef uint32_t juint;
typedef int64_t  jlong;
typedef uint64_t julong;
typedef float    jfloat;
typedef double   jdouble;
typedef jint     jsize;
/* v34.13 FIX (Bounce 128x128 heap corruption): jboolean MUST be 1 byte
 * (JNI spec: typedef uint8_t jboolean). It used to alias jint (4 bytes),
 * so every native that walked a JVM boolean[] through a jboolean* cast
 * (List.append/setSelectedIndex/isSelected/...) read and wrote FOUR
 * bytes per element - overflowing the 1-byte-per-element heap arrays
 * (heap_alloc_array allocates elem_size=1 for T_BOOLEAN) and smashing
 * adjacent heap blocks / the GC free list. That corrupted the LCDUI
 * List state (menu drawn wrong, keys ignored) and crashed the
 * allocator on Linux while "only" corrupting the menu on Windows. */
typedef uint8_t  jboolean;
typedef jushort  jchar;  /* Java char is 16-bit unsigned */

#define JNI_TRUE  1
#define JNI_FALSE 0
#define JNI_OK    0
#define JNI_ERR   (-1)

/* Forward declarations */
typedef struct JavaClass JavaClass;
typedef struct JavaObject JavaObject;
typedef struct JavaArray JavaArray;
typedef struct JavaString JavaString;
typedef struct JavaThread JavaThread;
typedef struct JavaFrame JavaFrame;
typedef struct JavaMethod JavaMethod;
typedef struct JavaField JavaField;
typedef struct JVM JVM;

/* Object Header - must be defined before JavaClass for Class object support
 * CRITICAL: Size must be multiple of 8 for proper alignment of double/long fields!
 * 
 * Without padding on 32-bit: clazz(4) + hashcode(4) + gc_mark+reserved(4) = 12 bytes
 * 12 is NOT divisible by 8, so fields would start at offset 12 and be misaligned!
 * 
 * With padding: 16 bytes on both 32-bit and 64-bit, ensuring fields are 8-byte aligned.
 */
typedef struct {
    JavaClass* clazz;
    jint hashcode;
    juint gc_mark: 1;
    juint reserved: 31;
    uint32_t _align_pad;    /* Padding to make ObjectHeader 16 bytes on 32-bit */
} ObjectHeader;

/* Helper macro: Get number of instance fields from instance_size 
 * instance_size = sizeof(ObjectHeader) + num_fields * sizeof(JavaValue)
 */
#define OBJECT_NUM_FIELDS(obj) \
    (((obj)->header.clazz->instance_size - sizeof(ObjectHeader)) / sizeof(JavaValue))

/* Helper macro: Check if object has at least N instance fields */
#define OBJECT_HAS_FIELDS(obj, n) \
    (((obj)->header.clazz->instance_size - sizeof(ObjectHeader)) >= (n) * sizeof(JavaValue))

/* Java Value (union for operand stack) */
typedef union {
    jbyte    b;
    jshort   s;
    jint     i;
    jlong    j;
    jfloat   f;
    jdouble  d;
    jboolean z;
    void*    ref;      /* Object reference */
    uint64_t raw;      /* Raw 64-bit value */
} JavaValue;

/* Constant Pool Tags (JVMS §4.4) */
typedef enum {
    CONSTANT_Utf8               = 1,
    CONSTANT_Integer            = 3,
    CONSTANT_Float              = 4,
    CONSTANT_Long               = 5,
    CONSTANT_Double             = 6,
    CONSTANT_Class              = 7,
    CONSTANT_String             = 8,
    CONSTANT_Fieldref           = 9,
    CONSTANT_Methodref          = 10,
    CONSTANT_InterfaceMethodref = 11,
    CONSTANT_NameAndType        = 12,
    CONSTANT_MethodHandle       = 15,
    CONSTANT_MethodType         = 16,
    CONSTANT_Dynamic            = 17,
    CONSTANT_InvokeDynamic      = 18,
    CONSTANT_Module             = 19,
    CONSTANT_Package            = 20
} ConstantPoolTag;

/* Constant Pool Entry
 * CRITICAL: Must have consistent layout across 32-bit and 64-bit platforms!
 * 
 * On 32-bit: jlong/jdouble require 8-byte alignment, so union must start at offset 8.
 * On 64-bit: pointers are 8 bytes, so union naturally aligns to 8.
 * 
 * Solution: Explicit padding after tag to ensure 8-byte alignment of union.
 */
typedef struct {
    uint8_t tag;
    uint8_t _padding[7];  /* Explicit padding for 8-byte alignment */
    union {
        struct { uint16_t length; char* bytes; } utf8;
        struct { jint value; } integer;
        struct { jfloat value; } float_val;
        struct { jlong value; } long_val;
        struct { jdouble value; } double_val;
        struct { uint16_t name_index; } class_info;
        struct { uint16_t string_index; } string_info;
        struct { uint16_t class_index; uint16_t name_and_type_index; } ref_info;
        struct { uint16_t name_index; uint16_t descriptor_index; } name_and_type;
        struct { uint8_t reference_kind; uint16_t reference_index; } method_handle;
        struct { uint16_t descriptor_index; } method_type;
        struct { uint16_t bootstrap_method_attr_index; uint16_t name_and_type_index; } invoke_dynamic;
    } info;
} ConstantPoolEntry;

/* Verify ConstantPoolEntry size at compile time */
#include <assert.h>
/* Expected size: 8 (tag + padding) + max(union members)
 * On 32-bit: 8 + 8 (long_val/double_val) = 16
 * On 64-bit: 8 + 16 (utf8 with 8-byte pointer + padding) = 24
 */
_Static_assert(sizeof(ConstantPoolEntry) == 16 || sizeof(ConstantPoolEntry) == 24,
    "ConstantPoolEntry size mismatch - check alignment");

/* Attribute Info */
typedef struct {
    uint16_t name_index;
    uint32_t length;
    uint8_t* info;
} AttributeInfo;

/* Exception Table Entry - defined before JavaMethod */
typedef struct {
    uint16_t start_pc;
    uint16_t end_pc;
    uint16_t handler_pc;
    uint16_t catch_type;
} ExceptionTableEntry;

/* Field Info */
struct JavaField {
    uint16_t access_flags;
    uint16_t name_index;
    uint16_t descriptor_index;
    uint16_t attributes_count;
    AttributeInfo* attributes;
    
    /* Resolved data */
    char* name;
    char* descriptor;
    jint offset;        /* Offset in object instance */
    JavaClass* clazz;
};

/* Static Field Info */
typedef struct {
    char* name;
    char* descriptor;
    JavaValue value;
} JavaStaticField;

/* Method Info */
struct JavaMethod {
    uint16_t access_flags;
    uint16_t name_index;
    uint16_t descriptor_index;
    uint16_t attributes_count;
    AttributeInfo* attributes;
    
    /* Code attribute (if present) */
    struct {
        uint16_t max_stack;
        uint16_t max_locals;
        uint32_t code_length;
        uint8_t* code;
        uint16_t exception_table_length;
        ExceptionTableEntry* exception_table;
        uint16_t attributes_count;
        AttributeInfo* attributes;
    } code;
    
    /* Resolved data */
    char* name;
    char* descriptor;
    JavaClass* clazz;
    bool is_native;
    void* native_func;  /* Native method pointer */
    /* FIX (audit M-4, v18): was int8_t — descriptors with >127 argument slots
     * (spec max 255) overflowed and corrupted native arg marshalling. */
    int16_t cached_arg_count;  /* -1 = not yet computed */
};

/* Java Class Structure */
struct JavaClass {
    /* Object header - allows JavaClass* to be used as a Class object */
    ObjectHeader header;

    uint32_t magic;
    uint16_t minor_version;
    uint16_t major_version;
    
    uint16_t constant_pool_count;
    size_t constant_pool_capacity;  /* Capacity of constant_pool array */
    ConstantPoolEntry* constant_pool;
    
    uint16_t access_flags;
    uint16_t this_class;
    uint16_t super_class_index;  /* Index in constant pool */
    
    uint16_t interfaces_count;
    uint16_t* interfaces;
    
    uint16_t fields_count;
    JavaField* fields;
    
    uint16_t methods_count;
    int methods_capacity;  /* Capacity of methods array */
    JavaMethod* methods;
    
    uint16_t attributes_count;
    AttributeInfo* attributes;
    
    /* Resolved/runtime data */
    char* class_name;
    char* super_class_name;
    JavaClass* super_class;
    JavaClass** interface_classes;
    
    size_t instance_size;   /* Size of instance data */
    jint field_count;       /* Total fields including super */
    
    /* Class state */
    bool initialized;
    JavaThread* initializing_thread;
    
    /* Static fields */
    JavaStaticField* static_fields;
    int static_fields_count;
        int static_fields_capacity;  /* Для динамического расширения */
        bool initializing;           /* Флаг инициализации */
    
    /* Method cache — UNUSED, method_cache.c handles caching globally */
    /* JavaMethod* method_cache[64]; */

    /* Stub class flag */
    bool is_stub;

    /* v34.35 PERF: constant-pool inline caches, lazily allocated arrays
     * parallel to constant_pool (indexed by CP index). See opcodes.c
     * op_getfield/op_putfield (field resolution) and the invoke wrappers
     * (method dispatch). Zero-initialized by the calloc in classfile.c /
     * stubs.c, freed by classfile_free_class. */
    struct FieldRefCache*  field_cache;    /* getfield/putfield resolution */
    struct InvokeRefCache* invoke_cache;   /* invokevirtual/special/static dispatch */
    struct StaticFieldRefCache* static_field_cache; /* v34.42: getstatic/putstatic */

    /* v34.59 PERF (GC mark/sweep): кэш разметки класса. Строится ЛЕНИВО
     * при первой встрече объекта класса в mark/sweep (мир остановлен и
     * heap_lock удерживается — гонок нет; классы в рантайме не выгружаются).
     *
     *   gc_ref_slots[]     — индексы слотов fields[] instance-полей,
     *                        хранящих ССЫЛКИ (дескриптор 'L...'/'[...'),
     *                        в порядке иерархии (Object -> ... -> класс);
     *                        поле nativePeer исключено (нативный указатель,
     *                        не Java-ссылка). Заменяет построение массива
     *                        hierarchy[64] + проход по всем fields для КАЖДОГО
     *                        объекта в mark-фазе (это была доминирующая
     *                        константа маркировки больших живых графов
     *                        3D-игр: Transform/Node/Vector-сети).
     *   gc_ref_slot_count  — число таких слотов.
     *   gc_kind           — биты GC_KIND_* (см. heap.c): String /
     *                        Hashtable-family / Vector-family / Entry-family /
     *                        LAYOUT_READY. Строковые strcmp по имени класса
     *                        выполняются ОДИН раз на класс вместо раза на
     *                        объект (mark: String value-check; sweep:
     *                        Hashtable peer cleanup).
     *
     * Если malloc массива слотов не удался, LAYOUT_READY не ставится и
     * mark-фаза честно уходит в медленный резервный проход. */
    uint16_t* gc_ref_slots;
    uint16_t  gc_ref_slot_count;
    uint8_t   gc_kind;
};

/* Java Object */
struct JavaObject {
    ObjectHeader header;
    JavaValue fields[];  /* Flexible array member */
};

/* Java Array - data follows the structure
 * CRITICAL: sizeof(JavaArray) must be multiple of 8 for proper alignment of array data!
 * 
 * On 32-bit: ObjectHeader(12) + length(4) + element_type(1) + _reserved(3) + element_class(4) = 24 bytes
 * On 64-bit: ObjectHeader(16) + length(4) + element_type(1) + _reserved(3) + element_class(8) = 32 bytes
 */
struct JavaArray {
    ObjectHeader header;        /* 16 bytes on both 32-bit and 64-bit (with _align_pad) */
    jsize length;               /* 4 bytes */
    uint8_t element_type;       /* 1 byte - primitive type or DESC_OBJECT/DESC_ARRAY */
    uint8_t _reserved[3];       /* 3 bytes reserved for alignment */
    JavaClass* element_class;   /* 4 bytes on 32-bit, 8 bytes on 64-bit - for object arrays */
    uint32_t _align_pad;        /* 4 bytes padding on 32-bit to make total 32 bytes (8-byte aligned) */
    /* Data follows here - access via helper functions */
};

/* Static assertion to verify JavaArray size is 8-byte aligned */
/* On 64-bit: ObjectHeader(16) + length(4) + element_type(1) + reserved(3) + element_class(8) = 32 */
/* On 32-bit: ObjectHeader(16) + length(4) + element_type(1) + reserved(3) + element_class(4) + pad(4) = 32 */

/* Java String - chars follow the structure */
struct JavaString {
    ObjectHeader header;
    jsize length;
    juint hash;
    char* utf8;         /* Cached UTF-8 version (malloc'd) */
    jchar* chars;       /* Cached UTF-16 chars (malloc'd) - for fast access */
    /* jchar[] may also follow here for native strings - access via string_chars() */
};

/* Stack Frame */
struct JavaFrame {
    JavaMethod* method;
    JavaClass* clazz;
    
    /* Program counter */
    uint32_t pc;
    uint8_t* code;
    uint32_t code_length;
    
    /* Local variables */
    JavaValue* locals;
    uint16_t max_locals;
    
    /* Operand stack */
    JavaValue* stack;
    int16_t stack_top;
    uint16_t max_stack;
    
    /* Exception handling */
    ExceptionTableEntry* exception_table;
    uint16_t exception_table_length;
    
        int throwing_pc; // PC инструкции, вызвавшей исключение (для поиска handler)
        
    /* v34.99 MONITOR-UNWIND: monitors acquired by monitorenter bytecodes in
     * THIS frame and not yet released by monitorexit. The interpreter never
     * tracked these — an exception unwinding out of a synchronized BLOCK
     * left the monitor owned forever (entry_count stuck >0), and the next
     * monitor_enter on that object from ANOTHER OS thread waited on the
     * entry condvar forever. That was the standalone-only "freeze right
     * after the countdown" class: paint on the frontend thread blocks on a
     * monitor a game thread leaked, while the game thread keeps spinning
     * (CPU ~100%, all M3G locks free, t2 'run', m frozen at one pc).
     * libretro never saw it because its single-threaded retro_run makes
     * every monitor re-entry reentrant (same pthread).
     * Fixed-capacity list: javac nests synchronized blocks 1-3 deep; if a
     * pathological method exceeds MON_OWNED_MAX, ownership of the deepest
     * entries is not tracked (behavior identical to pre-v34.99) and a
     * throttled warning is logged. Unwind releases in reverse order. */
#define MON_OWNED_MAX 6
    JavaObject* mon_owned[MON_OWNED_MAX];
    int16_t mon_owned_n;
        
    /* Previous frame */
    JavaFrame* prev;
};

/* Java Thread */
struct JavaThread {
    jint id;
    char* name;
    jint priority;
    bool is_daemon;
    bool is_alive;
    bool interrupted;
    
    /* Stack */
    JavaFrame* current_frame;
    uint8_t* stack_base;
    size_t stack_size;
    size_t stack_used;
    
    /* Thread-specific data */
    JavaObject* thread_object;
    JavaObject* thread_group;
    
    /* Exception */
    JavaObject* pending_exception;
    
    /* Saved stack trace for uncaught exception display */
    char* exception_stack_trace;  /* Allocated string with stack trace, saved when exception first becomes unhandled */
    char* exception_throw_info;   /* Class.method where exception was thrown */
    
    /* Exception handling context - each thread has its own jmp_buf */
    void* exec_jmp_buf;        /* Pointer to current jmp_buf in interpret() */
    int exec_jmp_buf_valid;    /* Whether jmp_buf is valid */
    
    /* Per-thread recursion depth for execute_method (prevents stack overflow) */
    int recursion_depth;

    /* v36.03 NATIVE-ARGS-ROOTED: the invoke wrappers pop this+args from the
     * java frame BEFORE calling the native, so during the whole native call
     * the receiver and arguments are invisible to the GC frame scan. Any GC
     * triggered inside the native (util_to_string_of -> bytecode -> alloc,
     * jvm_new_string, jvm_new_array, ...) could then sweep the receiver's
     * whole object graph while the native still dereferences it through C
     * locals (field crash: Vector.toString over a temporary Vector whose
     * elementData was recycled mid-loop; Data Abort inside the float
     * formatting path, 0xd7cf9080+8 = recycled 32-bit data read as a ref).
     * native_call pushes the caller's args buffer here for the duration of
     * the handler; the GC mark phase marks every slot (gc_push_mark
     * validates each value, so primitive slots are skipped safely). Nested
     * native->bytecode->native chains stack up to NATIVE_ARG_FRAMES_MAX
     * deep; deeper chains simply lose the protection (degraded, no crash). */
#define NATIVE_ARG_FRAMES_MAX 8
    struct {
        JavaValue* args;   /* caller's args buffer (C stack or heap) */
        int count;         /* slots: receiver (unless static) + java args */
    } native_arg_frames[NATIVE_ARG_FRAMES_MAX];
    int native_arg_depth;

    /* v34.11 diagnostics: last native method started on this thread +
     * monotonic ms timestamp. Printed by THREADDUMP so a thread stuck
     * inside a native (not parked in monitor_wait) can be located. */
    char last_native[96];
    jlong last_native_ms;

    /* v36.48 [DUMP-FENCE] diagnostics: последний Java-сайт «class.method@pc»
     * этого потока. Пишется ТОЛЬКО потоком-владельцем (push_frame на входе в
     * exec-окно + каждый 64-й yield интерпретатора — jvm_thread_note_site),
     * читается beat-таймером дампа. Прежде beat разыменовывал current_frame
     * ДРУГИХ потоков без блокировки — между pop_frame и frame_destroy это
     * heap-use-after-free (ASan, ys_asan_loop). Разрыв записи/чтения строки
     * теоретически рвёт строку пополам — для диагностики это допустимо. */
    char jsite[64];

    /* v36.05 diagnostics: monotonic ms of thread creation (0 = unknown).
     * jvm_thread_liveness() uses it as the age fallback for a RUNNABLE
     * thread that has NEVER dispatched a native (last_native_ms == 0):
     * such a thread previously reported age = -1 and was invisible to the
     * T-STALL/[MIDLET-STUCK] detectors — a pure-bytecode spin (the Doom
     * RPG hang shape) read as "healthy". */
    jlong created_ms;

    /* v34.95 diagnostics: 1 = this thread is blocked inside monitor_enter's
     * entry_cond wait (contention path only — cold, never in the hot loop).
     * Read by the per-second trace-log thread dump: "monenter" marks a
     * thread waiting for a monitor whose owner may itself be stuck. */
    volatile uint8_t mon_enter_wait;

    /* v34.99 diagnostics: monotonic ms timestamp of when mon_enter_wait was
     * set (0 = not waiting). Printed as "monenter=<s>" by the td dump — the
     * v34.98 freeze showed a frontend thread blocked in monitor_enter for
     * 15+ s while the td m line had NO way to show it (the flag was only
     * printed for tN lines). */
    jlong mon_enter_ms;

    /* v34.98 diagnostics: short reason this thread is currently parked at an
     * interpreter slow-check ("" when running). Values: "fe-pause" (frontend
     * pause park), "gc-sp" (GC safepoint park), "budget" (runner instruction
     * budget yield). Read by the per-second td dump: answers "state=run but
     * age grows" — the thread is parked here, not spinning. */
    char park_why[16];

    /* v36.06 diagnostics: stream-read shadow. Updated by the BAIS/DIS read
     * natives on every read: position/limit/underlying-buffer length and a
     * cumulative read counter. Printed as "stm=pos/count/len r=N" by the td
     * dump. Field case (Doom RPG v36.05 trace): t2 spun inside
     * "java/io/InputStream.read" for 25+ s with age=0 at every beat and a
     * heap frozen at 176 KB — the td line could not say whether the stream
     * position was advancing, what the stream size was, or how many reads
     * had been issued. With the shadow a spin is instantly classifiable:
     *   pos frozen, r climbing  -> reads never advance (corrupt/stub stream)
     *   pos climbing, r huge    -> parser grinding through a huge/garbage blob
     *   pos/count both 0        -> reads returned EOF immediately (loop bug) */
    volatile jint last_stm_pos;
    volatile jint last_stm_count;
    volatile jint last_stm_len;
    volatile jlong last_stm_reads;
    /* v36.08 STM-SPIN: consecutive reads that returned EOF (-1) without any
     * successful byte in between. A healthy parser hits EOF once (the loop
     * sees -1 and stops); the field hang (Doom RPG e.a() readFully helper
     * "for (i=n; i>0; i -= read(...))" — read()=-1 INCREMENTS i at EOF) spins
     * on an exhausted stream FOREVER with zero exceptions and zero
     * allocations, so neither GC, nor MIDLET-STUCK (age=0: the thread keeps
     * re-entering the read native), nor [EX-THROW] ever fire. Counting the
     * consecutive EOF reads lets the VM recognize the loop after
     * NOJME_STM_SPIN_EOF_LIMIT calls and inject a catchable IOException,
     * which turns the eternal loading screen into the game's own error path
     * (thread exit -> THREAD-EXIT/MIDLET-STUCK graceful recovery). */
    volatile jlong last_stm_eof_streak;
    
    /* Allocator */
    struct {
        void* heap;
        size_t heap_size;
        size_t heap_used;
    } allocator;
};

/* Garbage Collector */
typedef struct {
    uint8_t* heap_start;
    uint8_t* heap_end;
    uint8_t* heap_ptr;
    size_t heap_size;
    
    /* Statistics */
    size_t total_allocated;
    size_t total_freed;
    size_t gc_cycles;
    
    /* Roots */
    JavaObject** roots;
    size_t roots_count;
    size_t roots_capacity;
} GarbageCollector;

/* String Pool */
typedef struct {
    JavaString** strings;
    size_t count;
    size_t capacity;
} StringPool;

/* Class Loader */
typedef struct {
    JavaClass** classes;
    size_t count;
    size_t capacity;
    
    char** class_path;
    size_t class_path_count;
    
    /* JAR support */
    uint8_t* jar_data;
    size_t jar_size;
    char* jar_path;
} ClassLoader;

/* JVM Instance */
struct JVM {
    /* Configuration */
    struct {
        size_t heap_size;
        size_t stack_size;
        jint max_threads;
        bool verbose_gc;
        bool verbose_class;
        bool verbose_jni;
    } config;
    
    /* Runtime state */
    bool running;
    bool exiting;
    jint exit_code;
    
    /* Main thread */
    JavaThread* main_thread;
    
    /* All threads
     * FIX (audit T-2, v18): dynamic array — the fixed 16-slot array silently
     * dropped threads beyond the limit from GC scanning (games with >16 worker
     * threads corrupted collectible state). Capacity grows on demand. */
    JavaThread** threads;
    jint thread_count;
    jint threads_capacity;

    /* Instruction budget (debug/headless mode)
     * FIX (audit M-5, v18): counters live per-JVM so a ROM restart no longer
     * inherits the exhausted instruction budget of a previous run. */
    uint64_t instr_count;
    uint64_t instr_limit;   /* 0 = unlimited (default) */
    bool instr_env_checked;
    
    /* Class loader */
    ClassLoader class_loader;
    
    /* Garbage collector */
    GarbageCollector gc;
    
    /* String pool */
    StringPool string_pool;
    
    /* MIDP2 specific */
    struct {
        void* display;
        void* graphics;
        void* rms;
        void* game;
    } midp;
    
    /* Platform callbacks */
    struct {
        void (*on_paint)(void* data);
        void (*on_key)(int keycode, int pressed);
        void (*on_pointer)(int x, int y, int pressed);
        void* user_data;
    } callbacks;
};

/* JVM API Functions */

/* Initialization */
JVM* jvm_create(void);
void jvm_destroy(JVM* jvm);
int jvm_init(JVM* jvm);
int jvm_run(JVM* jvm, const char* main_class, int argc, char** argv);

/* Method Cache Functions (optimized lookup) */
void method_cache_init(void);
void method_cache_cleanup(void);
/* FIX (audit S-10, v18): drop every cached JavaMethod* — must be called when
 * a class's methods[] array is reallocated, otherwise cached pointers dangle. */
void method_cache_flush(void);
JavaMethod* method_cache_lookup(JavaClass* clazz, const char* name, const char* descriptor);
void method_cache_store(JavaClass* clazz, const char* name, const char* descriptor, JavaMethod* method);
void method_cache_stats(int* hits, int* misses);

/* Class Hash Functions (optimized class lookup) */
void class_hash_add(JavaClass* clazz);
JavaClass* class_hash_lookup(const char* name);
/* v20 (P0-2): per-entry negative-cache for JAR-vs-stub resolution. */
void class_hash_mark_jar_checked(const char* name);
bool class_hash_was_jar_checked(const char* name);
void class_hash_remove(const char* name);
void class_hash_clear(void);

/* Fast method resolution */
JavaMethod* jvm_resolve_method_fast(JVM* jvm, JavaClass* clazz, const char* name, const char* descriptor);
JavaClass* jvm_find_class_fast(JVM* jvm, const char* name);

/* Class Loading */
void jvm_set_jar_data(JVM* jvm, uint8_t* data, size_t size, const char* path);
JavaClass* jvm_load_class(JVM* jvm, const char* class_name);
JavaClass* jvm_define_class(JVM* jvm, const uint8_t* data, size_t length);
JavaClass* jvm_load_class_from_jar(JVM* jvm, const char* class_name);
/* v34.49: JAR-over-stub override for the linear lookup paths (Step 2 of
 * jvm_load_class_impl + get_or_create_stub_class). Returns the MIDlet's
 * own class from the JAR when it must replace a builtin stub, else NULL. */
JavaClass* jvm_stub_jar_override_linear(JVM* jvm, const char* class_name, JavaClass* stub);
JavaMethod* jvm_resolve_method(JVM* jvm, JavaClass* clazz, const char* name, const char* descriptor);
JavaField* jvm_resolve_field(JVM* jvm, JavaClass* clazz, const char* name, const char* descriptor);
void jvm_recalculate_instance_size(JVM* jvm, JavaClass* clazz);

/* Object Creation */
JavaObject* jvm_new_object(JVM* jvm, JavaClass* clazz);
/* v19: emergency variant — allocates from the reserve area so exception
 * objects can be created even on a full heap (see jvm_throw_by_name). */
JavaObject* jvm_new_object_emergency(JVM* jvm, JavaClass* clazz);
JavaArray* jvm_new_array(JVM* jvm, uint8_t type, jsize length, JavaClass* element_class);
JavaString* jvm_new_string(JVM* jvm, const char* utf8);
JavaString* jvm_new_string_utf16(JVM* jvm, const jchar* chars, jsize length);

/* Method Invocation */
int jvm_call_method(JVM* jvm, JavaObject* obj, JavaMethod* method, JavaValue* args, JavaValue* result);
int jvm_call_static(JVM* jvm, JavaClass* clazz, JavaMethod* method, JavaValue* args, JavaValue* result);
int jvm_call_virtual(JVM* jvm, JavaObject* obj, const char* name, const char* descriptor, JavaValue* args, JavaValue* result);

/* Exception Handling */
void jvm_throw(JVM* jvm, JavaObject* exception);
void jvm_throw_by_name(JVM* jvm, const char* class_name, const char* message);
JavaObject* jvm_exception_pending(JVM* jvm);
JavaObject* jvm_exception_clear(JVM* jvm);

/* Thread Management */
JavaThread* jvm_thread_create(JVM* jvm, const char* name, jint priority);
void jvm_thread_destroy(JVM* jvm, JavaThread* thread);
JavaThread* jvm_current_thread(JVM* jvm);

/* Memory Management */
void* jvm_alloc(JVM* jvm, size_t size);
void jvm_gc(JVM* jvm);
void jvm_add_root(JVM* jvm, JavaObject* obj);
void jvm_remove_root(JVM* jvm, JavaObject* obj);

/* String Operations */
char* jvm_string_to_utf8(JVM* jvm, JavaString* str);
jint jvm_string_hash(JavaString* str);
bool jvm_string_equals(JavaString* a, JavaString* b);

/* Utility Functions */
uint16_t jvm_read_u16(const uint8_t* data);
uint32_t jvm_read_u32(const uint8_t* data);
uint64_t jvm_read_u64(const uint8_t* data);
int16_t jvm_read_s16(const uint8_t* data);
int32_t jvm_read_s32(const uint8_t* data);
int64_t jvm_read_s64(const uint8_t* data);

/* Debug Functions */
void jvm_dump_class(JavaClass* clazz);
void jvm_dump_method(JavaMethod* method);
void jvm_dump_stack(JavaThread* thread);
void jvm_dump_heap(JVM* jvm);

/* MIDlet Execution */
int jvm_run_midlet(JVM* jvm, JavaClass* main_class);
int jvm_init_class(JVM* jvm, JavaClass* clazz);
int jvm_invoke_virtual(JVM* jvm, JavaObject* obj, const char* name, 
                       const char* descriptor, JavaValue* args, JavaValue* result);
int jvm_invoke_static(JVM* jvm, JavaClass* clazz, const char* name,
                      const char* descriptor, JavaValue* args, JavaValue* result);
int jvm_invoke_special(JVM* jvm, JavaObject* obj, JavaClass* clazz,
                       const char* name, const char* descriptor,
                       JavaValue* args, JavaValue* result);
JavaObject* jvm_new_object_with_constructor(JVM* jvm, JavaClass* clazz,
                                            const char* descriptor,
                                            JavaValue* args);

#endif /* JVM_H */
