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

/* Fast subclass test: walk oc's super chain looking for `of`. */
bool jvm_class_is_subclass_of(JavaClass* oc, JavaClass* of);

#endif /* NOJME_CP_CACHES_H */
