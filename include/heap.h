/*
 * J2ME Emulator - Heap and Garbage Collector
 * Memory management for Java objects
 */

#ifndef HEAP_H
#define HEAP_H

#include "jvm.h"

/*
 * Object types for GC
 */
typedef enum {
    OBJ_TYPE_OBJECT,
    OBJ_TYPE_ARRAY,
    OBJ_TYPE_STRING,
    OBJ_TYPE_CLASS,
    OBJ_TYPE_FREE    /* Free block in heap (not a live object) */
} ObjectType;

/*
 * Object header for GC tracking
 * CRITICAL: Layout must be compatible with FreeBlock for recycling!
 * 
 * FreeBlock = { next*, uint32_t size, uint32_t _reserved }
 * On 32-bit: next=4bytes at offset 0, size=4bytes at offset 4
 * On 64-bit: next=8bytes at offset 0, size=4bytes at offset 8, _reserved=4bytes at offset 12
 * 
 * GCObjectHeader MUST have size at the same offset as FreeBlock!
 * The key insight: size field must be at the SAME offset in both structures.
 * 
 * CRITICAL FIX: sizeof(GCObjectHeader) must be multiple of 8!
 * This ensures that user data (ptr = header + 1) is aligned to 8 bytes.
 * 
 * On 32-bit: 32 bytes (next=4, size=4, type=4, marked=1, pinned=1, reserved=2, _align_pad=8, clazz=4, magic=4)
 * On 64-bit: 40 bytes (next=8, size=4+pad=8, type=4, marked=1, pinned=1, reserved=2, _align_pad=8, clazz=8, magic=4+pad=8)
 */
typedef struct GCObjectHeader {
    struct GCObjectHeader* next;    /* Next object in heap - offset 0 */
    uint32_t size;                  /* Object size - offset 4 (32-bit) / 8 (64-bit) */
    ObjectType type;                /* Object type - offset 8 (32-bit) / 12 (64-bit) */
    uint8_t marked;                 /* GC mark flag - offset 12 (32-bit) / 16 (64-bit) */
    uint8_t pinned;                 /* Cannot be moved - offset 13 (32-bit) / 17 (64-bit) */
    uint8_t reserved[2];            /* Padding - offset 14-15 (32-bit) / 18-19 (64-bit) */
    uint8_t _align_pad[8];          /* PADDING for 8-byte alignment - offset 16-23 (32-bit) / 20-27 (64-bit) */
    JavaClass* clazz;               /* Object's class - offset 24 (32-bit) / 28 (64-bit) */
    
    /* MAGIC number for detecting memory corruption */
    uint32_t magic;                 /* Magic: 0xDEADBEEF - if corrupted, memory was overwritten */
    uint32_t _magic_pad;            /* Padding to make total size multiple of 8 */
} GCObjectHeader;                   /* Total: 32 bytes (32-bit) / 40 bytes (64-bit) */

#define GC_HEADER_MAGIC 0xDEADBEEF

/*
 * FreeBlock structure for free list management
 * CRITICAL: Must have size field at SAME offset as GCObjectHeader!
 * 
 * On 32-bit: next(4) at offset 0, size(4) at offset 4 - matches GCObjectHeader
 * On 64-bit: next(8) at offset 0, size(4) at offset 8 - matches GCObjectHeader!
 *           _reserved(4) at offset 12 for alignment
 * 
 * This ensures binary compatibility when casting between FreeBlock* and GCObjectHeader*
 */
typedef struct FreeBlock {
    struct FreeBlock* next;         /* Next free block - offset 0 */
    uint32_t size;                  /* Block size - offset 4 (32-bit) / 8 (64-bit) - MUST match GCObjectHeader! */
    uint32_t _reserved;             /* Reserved for alignment on 64-bit - offset 8 (32-bit) / 12 (64-bit) */
} FreeBlock;

/*
 * Heap statistics
 */
typedef struct {
    size_t total_size;
    size_t used_size;
    size_t free_size;
    size_t object_count;
    size_t gc_cycles;
    size_t gc_time_ms;
} HeapStats;

/*
 * Heap initialization
 */
int heap_init(JVM* jvm, size_t initial_size, size_t max_size);
void heap_destroy(JVM* jvm);

/*
 * Object allocation
 */
void* heap_alloc(JVM* jvm, size_t size, JavaClass* clazz, ObjectType type);

/* v19: emergency allocation — may use the reserve area beyond the normal
 * heap end. Reserved for exception object creation (see jvm_throw_by_name)
 * so OutOfMemoryError and friends stay catchable on a full heap. */
void* heap_alloc_emergency(JVM* jvm, size_t size, JavaClass* clazz, ObjectType type);

/* v34: sizes of the LAST failed heap allocation (see heap.c v34 FIX note).
 * Used by native_throw_oome to build an OutOfMemoryError detailMessage so
 * genuinely-fatal (uncaught) OOMs still show "Requested/Available" on the
 * error screen via the main.c / libretro.c uncaught-exception handlers. */
void heap_get_last_oom_info(size_t* requested, size_t* available);

/* Allocate object */
JavaObject* heap_alloc_object(JVM* jvm, JavaClass* clazz);

/* v19: emergency variant of heap_alloc_object (exception construction). */
JavaObject* heap_alloc_object_emergency(JVM* jvm, JavaClass* clazz);

/* v34.59 TLAB: закрыть чанк ТЕКУЩЕГО потока (без lock — только TLS
 * владельца). Вызывается jvm_gc_safepoint_park() ДО arrival-broadcast,
 * при входе в gc_collect() и перед выходом потокового раннера.
 * Идемпотентно. */
void heap_tlab_flush_self(void);

/* v34.59 TLAB: диагностические счётчики (racy — для логов/бенчей). */
void heap_tlab_stats(uint64_t* carves, uint64_t* fast_allocs);

/* v34.59: heap.current — верхняя граница занятой области (диагностика). */
void* heap_top_ptr(void);

/* Allocate array - element_class is the class of array elements for object arrays, NULL for primitives */
JavaArray* heap_alloc_array(JVM* jvm, uint8_t element_type, jsize length, JavaClass* element_class);

/* Allocate string */
JavaString* heap_alloc_string(JVM* jvm, jsize length);

/*
 * Garbage Collection
 */

/* Run garbage collection */
void gc_collect(JVM* jvm);

/* Add GC root. v35.08: IDEMPOTENT - registering the same slot twice within
 * one heap lifetime adds it once (the per-session reset below empties the
 * list, so call sites no longer need "already registered" flags that
 * silently skipped re-registration in the NEXT session). */
void gc_add_root(JVM* jvm, void** root);

/* Remove GC root */
void gc_remove_root(JVM* jvm, void** root);

/* v35.08 MULTI-SESSION (Switch frontend: menu -> game -> menu -> game):
 * NULL every registered root slot, then empty the list.
 *
 * The root slots are PROCESS-GLOBAL statics spread over the whole native
 * layer (display.c g_display_instance / current_displayable_obj /
 * g_graphics_object / callSerially queue, form.c g_current_form ...,
 * native.c Runtime/Timer slots, rms.c enum/listener slots, mobile3d.c
 * imm-mode bridge ...). They pointed into THIS session's Java heap; after
 * heap_destroy they dangle, and the next session's heap is frequently
 * malloc'd at the SAME address - the stale pointers then alias foreign
 * objects (field-repro: second game died in Display.setCurrent with
 * "Object has NULL or invalid class pointer"). Every registered root slot
 * lives in static storage (or the static RMS store table), so NULLing the
 * slots here is safe and hands every layer a clean slate mechanically.
 * Call with VM threads quiet (jvm_destroy does). */
void gc_roots_reset_all(void);

/* Pin object (prevent GC from moving it) */
void gc_pin(JVM* jvm, void* object);

/* Unpin object */
void gc_unpin(JVM* jvm, void* object);

/* Get heap statistics */
HeapStats heap_get_stats(JVM* jvm);

/*
 * Object operations
 */

/* Get object class */
JavaClass* object_get_class(void* object);

/* Get object size */
size_t object_get_size(void* object);

/* Get object hash code */
jint object_hash_code(void* object);

/* Check if object is an array */
bool object_is_array(void* object);

/* Check if object is a string */
bool object_is_string(void* object);

/* Check if object is instance of class */
bool object_instance_of(void* object, JavaClass* clazz);

/*
 * Array operations
 */

/* Get pointer to array data (inline after JavaArray structure) */
static inline void* array_data(JavaArray* array) {
    return array ? (void*)((uint8_t*)array + sizeof(JavaArray)) : NULL;
}

/* Get array length */
jsize array_length(JavaArray* array);

/* Get array element type */
uint8_t array_element_type(JavaArray* array);

/* Get array element */
JavaValue array_get(JavaArray* array, jsize index);

/* Set array element */
void array_set(JavaArray* array, jsize index, JavaValue value);

/* Get reference from object array */
void* array_get_ref(JavaArray* array, jsize index);

/* Set reference in object array */
void array_set_ref(JavaArray* array, jsize index, void* ref);

/*
 * String operations
 */

/* Get string length */
jsize string_length(JavaString* str);

/* Get string characters (UTF-16) */
const jchar* string_chars(JavaString* str);

/* Get string as UTF-8
 * IMPORTANT: Returns a pointer that caller MUST NOT free!
 * The returned pointer is either an internal cached buffer (for native strings)
 * or a thread-local buffer that is reused on the next call.
 * Use string_utf8_copy() if you need to store the result for later use.
 */
const char* string_utf8(JVM* jvm, JavaString* str);

/* Get string as UTF-8 copy - caller MUST free the returned pointer!
 * Always returns a malloc'd copy that the caller owns.
 * Use this when you need to store the string for later use.
 */
char* string_utf8_copy(JVM* jvm, JavaString* str);

/* Cleanup thread-local string buffer (call at JVM shutdown) */
void string_utf8_cleanup(void);

/* Compare strings */
bool string_equals(JavaString* a, JavaString* b);

/* String hash code */
jint string_hash(JavaString* str);

/*
 * Memory debugging
 */
void heap_dump(JVM* jvm);
void heap_validate(JVM* jvm);

/* Check if pointer is in heap - for validation 
 * Note: This uses external heap bounds from heap.c */
extern void* g_heap_start;
extern void* g_heap_end;

static inline bool is_heap_ptr_check(void* ptr) {
    extern void* g_heap_start;
    extern void* g_heap_end;
    return ptr >= g_heap_start && ptr < g_heap_end;
}

/* [ARGGUARD] v36.49: правдоподобность ref-значения ПЕРЕД сырым
 * разыменованием header.clazz (смещение 0 — первое, что VM читает у
 * объекта). Полевой факт (Ryujinx): Invalid memory access at
 * 0x20697465592F7365 = ASCII "es/Yeti " — байты ПУТИ ИГРЫ, попавшие в
 * ref-слот (C-стек соседствовал со строкой пути). Такой мусор всегда
 * не-каноничен (>= 2^48) и/или не выровнен на 8; настоящие ссылки
 * (объекты кучи, malloc-классы, интерьеры строк) — каноничны и
 * выровнены минимум на 8 на всех целях (x86-64, aarch64, armv7).
 * NULL мусором не считается. Возвращает 1 = «разыменовывать нельзя». */
static inline int argguard_bad_ptr(uintptr_t raw) {
    return raw != 0 && (raw > 0x0000FFFFFFFFFFF8ull || (raw & 7) != 0);
}

/* [ARGGUARD] реализация в execute.c: validate-walk дескриптора + дамп
 * (метод, слот, hex + ASCII-8 байтов мусора, адрес моста-вызователя).
 * check_array: defuse=1 -> мусорные ref-слоты заменяются NULL (безопасно
 * только когда массив гарантированно >= числа слотов дескриптора —
 * например, malloc-массивы op_invoke*; для чужих C-стек-мостов вызывать
 * с defuse=0 — только репорт). check_ret: валидация возвращаемого
 * значения native (L/[), дефуз безопасен всегда (локальная JavaValue). */
void argguard_check_array(const char* cname, const char* mname,
                          const char* desc, JavaValue* args,
                          int argc_total, int has_this, void* callsite,
                          int defuse);
void argguard_check_ret(const char* cname, const char* mname,
                        const char* desc, JavaValue* rv, void* callsite);

/*
 * v15: Validate that a JavaObject* held by NATIVE code (M3G object registry,
 * pending-render queue, cached World/Camera pointers, ...) is still a live
 * GC object. Native-side structures keep raw pointers that the garbage
 * collector cannot see; after a collection the pointed-to object may be
 * freed and its memory reused, so dereferencing header.clazz is a wild
 * read (crash after the last M3GTest scene on Windows). */
bool heap_java_object_valid(void* ptr);

#endif /* HEAP_H */
