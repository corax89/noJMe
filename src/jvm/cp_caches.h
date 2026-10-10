/*
 * cp_caches.h — v36.37: shared definitions of the constant-pool inline
 * caches (field/static-field resolution). Previously private to
 * opcodes.c; the v36.37 interpreter fast-path (execute.c) inlines the
 * hot getfield/putfield/getstatic/putstatic accessors and needs the
 * same cache types + accessors.
 *
 * v36.38: MOVED from include/ to src/jvm/ — its only consumers
 * (execute.c, opcodes.c) live here, and a quoted #include resolves from
 * the INCLUDING FILE's directory first. The header is now found even if
 * the -I flags are lost or the include/ tree is stale/partially synced
 * (field report: mingw32 build died with "cp_caches.h: No such file or
 * directory" while every -I-included system header resolved fine).
 */
#ifndef NOJME_CP_CACHES_H
#define NOJME_CP_CACHES_H

#include "jvm.h"

/* v34.42 PERF: getstatic/putstatic inline cache. Caches the OWNING class
 * + slot index of the resolved JavaStaticField — the value itself is read
 * fresh on every access (fields mutate constantly). */
typedef struct StaticFieldRefCache {
    JavaClass* owner;      /* class that DECLARES the field */
    int32_t    slot;       /* index into owner->static_fields */
    uint16_t   gen;        /* g_vm_cache_gen at fill time */
    uint8_t    wide;       /* J/D: 2 operand-stack slots */
    uint8_t    valid;
} StaticFieldRefCache;

/* v34.35 PERF: getfield/putfield inline cache (declared-class anchored). */
typedef struct FieldRefCache {
    JavaClass* declared;   /* class named in the CP Fieldref */
    int32_t    slot;       /* absolute instance slot (Object-first layout) */
    uint16_t   gen;        /* g_vm_cache_gen at fill time (invalidation) */
    uint8_t    wide;       /* J/D: 2 operand-stack slots */
    uint8_t    valid;
} FieldRefCache;

/* v36.103 [FAST-INVOKES]: invoke dispatch inline cache — moved here from
 * opcodes.c (private since v34.35) so the interpreter fast core (execute.c)
 * can take the IC-hit call path WITHOUT the cold_dispatch round trip.
 * Fillers stay in opcodes.c (they run on resolution); only the accessor
 * is shared. Layout mirrors the historical opcodes.c struct exactly —
 * the arrays are calloc'd per class and their element size must not
 * change between builds sharing a heap dump. */
typedef struct InvokeRefCache {
    JavaClass* target_class; /* virtual/interface: receiver class of the
                              * MONOMORPHIC slot (v34.35); static/special:
                              * the resolved method's class */
    JavaMethod* method;      /* resolved bytecode method (NULL => native/unresolved) */
    void*       native_fn;   /* NativeMethod if resolved native */
    const char* cls_name;    /* v34.42: CP-declared class name (native dispatch note) */
    const char* mth_name;   /* v34.42: method name (native dispatch note) */
    int16_t     java_argc;   /* compact arg count (J/D = 1), this NOT included */
    uint16_t    gen;         /* g_vm_cache_gen at fill time (invalidation) */
    uint8_t     arg_slots[12]; /* operand-stack slots per Java arg (1 or 2), 0-terminated */
    uint8_t     kind;        /* 0=bytecode method, 1=native, 2=unresolved (never cached) */
    uint8_t     ret_wide;    /* J/D return pushes 2 slots */
    uint8_t     has_ret;     /* descriptor return != V */
    uint8_t     valid;
    /* v36.103 [PIC]: 3 additional (receiver-class, method) pairs for
     * POLYMORPHIC virtual/interface sites. The v34.35 monomorphic design
     * thrashed on every receiver-class change: a 2-class site paid the
     * FULL slow path (3 descriptor parses + malloc + name resolution)
     * per call — CallBench measured 44% of all invokes landing there.
     * arg_slots/java_argc are descriptor-derived (site-constant) and
     * shared by all pairs. One gen covers the whole entry: ANY fill after
     * a generation bump resets every slot first (stale JavaMethod pointers
     * are UAF after methods[] realloc). Sites with >4 live classes evict
     * the mono slot (megamorphic degrade). */
    JavaClass* pic_class[3];
    JavaMethod* pic_method[3];
} InvokeRefCache;

/* v34.35 FIX (stubs.c methods[] realloc / late field additions): bump this
 * whenever any class's methods[] array is reallocated or instance field
 * slots shift; inline caches record the generation they were built in and
 * self-invalidate on mismatch. Defined in opcodes.c. */
extern uint16_t g_vm_cache_gen;

/* Accessors/fillers (defined in opcodes.c). */
StaticFieldRefCache* static_field_cache_entry(JavaFrame* frame, uint16_t index);
void static_field_cache_fill(JavaFrame* frame, uint16_t index,
                             JavaClass* owner, int slot, uint8_t wide);
FieldRefCache* field_cache_entry(JavaFrame* frame, uint16_t index);
void field_cache_fill(JavaFrame* frame, uint16_t index,
                      JavaClass* declared, int slot, uint8_t wide);
InvokeRefCache* invoke_cache_entry(JavaFrame* frame, uint16_t index);

/* Fast subclass test: walk oc's super chain looking for `of`. */
bool jvm_class_is_subclass_of(JavaClass* oc, JavaClass* of);

#endif /* NOJME_CP_CACHES_H */
