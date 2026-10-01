/*
 * J2ME Emulator - Optimized Method and Class Lookup
 * 
 * Performance optimizations:
 * 1. FNV-1a hash function for method/class names
 * 2. Global method cache with hash table
 * 3. Class hash table for fast class lookup
 * 4. VTable for virtual method dispatch
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "jvm.h"
#include "classfile.h"
#include "debug.h"
#include "debug_macros.h"

/* ============================================
 * FNV-1a Hash Function
 * Fast, high-quality hash for method/class names
 * ============================================ */

#define FNV_OFFSET_BASIS 0x811C9DC5u
#define FNV_PRIME 0x01000193u

/* Compute FNV-1a hash for a string */
static inline uint32_t fnv1a_hash(const char* str) {
    if (!str) return 0;
    
    uint32_t hash = FNV_OFFSET_BASIS;
    while (*str) {
        hash ^= (uint8_t)*str++;
        hash *= FNV_PRIME;
    }
    return hash;
}

/* Compute combined hash for name + descriptor */
static inline uint32_t method_hash(const char* name, const char* descriptor) {
    uint32_t hash = fnv1a_hash(name);
    hash ^= 0xFF;  /* Separator */
    hash *= FNV_PRIME;
    
    /* Mix in descriptor */
    if (descriptor) {
        while (*descriptor) {
            hash ^= (uint8_t)*descriptor++;
            hash *= FNV_PRIME;
        }
    }
    return hash;
}

/* ============================================
 * Global Method Cache
 * Caches resolved methods for fast lookup
 * ============================================ */

#define METHOD_CACHE_SIZE 4096  /* Must be power of 2 */
#define METHOD_CACHE_MASK (METHOD_CACHE_SIZE - 1)

typedef struct {
    JavaClass* clazz;
    const char* name;
    const char* descriptor;
    JavaMethod* method;
    uint32_t hash;
} MethodCacheEntry;

static MethodCacheEntry g_method_cache[METHOD_CACHE_SIZE];
static int g_method_cache_hits = 0;
static int g_method_cache_misses = 0;

/* Fast method lookup in global cache */
JavaMethod* method_cache_lookup(JavaClass* clazz, const char* name, const char* descriptor) {
    uint32_t hash = method_hash(name, descriptor);
    uint32_t idx = hash & METHOD_CACHE_MASK;
    
    /* Check primary slot */
    MethodCacheEntry* entry = &g_method_cache[idx];
    if (entry->method && entry->clazz == clazz && 
        strcmp(entry->name, name) == 0 && strcmp(entry->descriptor, descriptor) == 0) {
        g_method_cache_hits++;
        return entry->method;
    }
    
    /* Check secondary slot (for collision handling) */
    uint32_t idx2 = (idx ^ (hash >> 16)) & METHOD_CACHE_MASK;
    entry = &g_method_cache[idx2];
    if (entry->method && entry->clazz == clazz &&
        strcmp(entry->name, name) == 0 && strcmp(entry->descriptor, descriptor) == 0) {
        g_method_cache_hits++;
        return entry->method;
    }
    
    g_method_cache_misses++;
    return NULL;
}

/* Store method in global cache */
void method_cache_store(JavaClass* clazz, const char* name, const char* descriptor, JavaMethod* method) {
    uint32_t hash = method_hash(name, descriptor);
    uint32_t idx = hash & METHOD_CACHE_MASK;
    
    MethodCacheEntry* entry = &g_method_cache[idx];
    
    /* Use primary slot if empty or same key */
    if (!entry->method || (entry->clazz == clazz && strcmp(entry->name, name) == 0 && strcmp(entry->descriptor, descriptor) == 0)) {
        entry->clazz = clazz;
        entry->name = name;
        entry->descriptor = descriptor;
        entry->method = method;
        entry->hash = hash;
        return;
    }
    
    /* Use secondary slot for collision */
    uint32_t idx2 = (idx ^ (hash >> 16)) & METHOD_CACHE_MASK;
    entry = &g_method_cache[idx2];
    entry->clazz = clazz;
    entry->name = name;
    entry->descriptor = descriptor;
    entry->method = method;
    entry->hash = hash;
}

/* Get cache statistics */
void method_cache_stats(int* hits, int* misses) {
    if (hits) *hits = g_method_cache_hits;
    if (misses) *misses = g_method_cache_misses;
}

/* ============================================
 * Class Hash Table
 * Fast lookup for loaded classes by name
 * ============================================ */

#define CLASS_HASH_SIZE 1024  /* Must be power of 2 */
#define CLASS_HASH_MASK (CLASS_HASH_SIZE - 1)

typedef struct ClassHashEntry {
    JavaClass* clazz;
    const char* name;
    struct ClassHashEntry* next;
    /* v20 (P0-2): set once a JAR lookup proved there is no real class with
     * this name — prevents re-scanning the JAR on every hot stub lookup. */
    bool jar_checked;
} ClassHashEntry;

static ClassHashEntry* g_class_hash[CLASS_HASH_SIZE];
static int g_class_count = 0;

/* Add class to hash table */
void class_hash_add(JavaClass* clazz) {
    if (!clazz || !clazz->class_name) return;
    
    uint32_t hash = fnv1a_hash(clazz->class_name);
    uint32_t idx = hash & CLASS_HASH_MASK;
    
    /* Check if already exists */
    ClassHashEntry* entry = g_class_hash[idx];
    while (entry) {
        if (strcmp(entry->name, clazz->class_name) == 0) {
            entry->clazz = clazz;  /* Update */
            return;
        }
        entry = entry->next;
    }
    
    /* Create new entry */
    entry = (ClassHashEntry*)malloc(sizeof(ClassHashEntry));
    if (!entry) return;
    
    entry->clazz = clazz;
    entry->name = clazz->class_name;
    entry->next = g_class_hash[idx];
    entry->jar_checked = false;
    g_class_hash[idx] = entry;
    g_class_count++;
}

/* Find class in hash table */
JavaClass* class_hash_lookup(const char* name) {
    if (!name) return NULL;
    
    uint32_t hash = fnv1a_hash(name);
    uint32_t idx = hash & CLASS_HASH_MASK;
    
    ClassHashEntry* entry = g_class_hash[idx];
    while (entry) {
        if (strcmp(entry->name, name) == 0) {
            return entry->clazz;
        }
        entry = entry->next;
    }
    return NULL;
}

/* v20 (P0-2): remember that the JAR was searched and does NOT contain a
 * real class under this name, so stub hits skip the (binary-search) JAR
 * probe on subsequent lookups. */
void class_hash_mark_jar_checked(const char* name) {
    if (!name) return;
    uint32_t hash = fnv1a_hash(name);
    uint32_t idx = hash & CLASS_HASH_MASK;
    ClassHashEntry* entry = g_class_hash[idx];
    while (entry) {
        if (strcmp(entry->name, name) == 0) {
            entry->jar_checked = true;
            return;
        }
        entry = entry->next;
    }
}

bool class_hash_was_jar_checked(const char* name) {
    if (!name) return true;
    uint32_t hash = fnv1a_hash(name);
    uint32_t idx = hash & CLASS_HASH_MASK;
    ClassHashEntry* entry = g_class_hash[idx];
    while (entry) {
        if (strcmp(entry->name, name) == 0) {
            return entry->jar_checked;
        }
        entry = entry->next;
    }
    return true;
}

/* Remove class from hash table */
void class_hash_remove(const char* name) {
    if (!name) return;
    
    uint32_t hash = fnv1a_hash(name);
    uint32_t idx = hash & CLASS_HASH_MASK;
    
    ClassHashEntry** pp = &g_class_hash[idx];
    while (*pp) {
        if (strcmp((*pp)->name, name) == 0) {
            ClassHashEntry* entry = *pp;
            *pp = entry->next;
            free(entry);
            g_class_count--;
            return;
        }
        pp = &(*pp)->next;
    }
}

/* Clear all entries */
void class_hash_clear(void) {
    for (int i = 0; i < CLASS_HASH_SIZE; i++) {
        ClassHashEntry* entry = g_class_hash[i];
        while (entry) {
            ClassHashEntry* next = entry->next;
            free(entry);
            entry = next;
        }
        g_class_hash[i] = NULL;
    }
    g_class_count = 0;
}

/* ============================================
 * Optimized Method Resolution
 * Uses hash table + global cache for fast lookup
 * ============================================ */

/* Internal method lookup in class (linear search) */
static JavaMethod* find_method_in_class(JavaClass* clazz, const char* name, const char* descriptor) {
    if (!clazz || !name) return NULL;
    
    for (int i = 0; i < clazz->methods_count; i++) {
        JavaMethod* m = &clazz->methods[i];
        if (m->name && strcmp(m->name, name) == 0) {
            /* If descriptor provided, match it */
            if (descriptor && m->descriptor) {
                if (strcmp(m->descriptor, descriptor) == 0) {
                    return m;
                }
            } else if (!descriptor) {
                /* No descriptor - return first match (for overloads) */
                return m;
            }
        }
    }
    return NULL;
}

/* Optimized jvm_resolve_method implementation */
JavaMethod* jvm_resolve_method_fast(JVM* jvm, JavaClass* clazz, const char* name, const char* descriptor) {
    (void)jvm;
    if (!clazz || !name || !descriptor) return NULL;
    
    /* Step 1: Check global method cache */
    JavaMethod* method = method_cache_lookup(clazz, name, descriptor);
    if (method) return method;
    
    /* Step 2: Search class hierarchy */
    JavaClass* search_class = clazz;
    while (search_class) {
        method = find_method_in_class(search_class, name, descriptor);
        if (method) {
            /* Cache the result */
            method_cache_store(clazz, name, descriptor, method);
            return method;
        }
        search_class = search_class->super_class;
    }
    
    return NULL;
}

/* ============================================
 * Optimized Class Loading
 * Uses hash table for fast lookup
 * ============================================ */

JavaClass* jvm_find_class_fast(JVM* jvm, const char* name) {
    if (!jvm || !name) return NULL;
    
    /* Step 1: Check hash table */
    return class_hash_lookup(name);
}

/* ============================================
 * VTable Support for Virtual Method Dispatch
 * Pre-computed virtual method tables
 * ============================================ */

#define VTABLE_SIZE 256

typedef struct {
    JavaMethod* methods[VTABLE_SIZE];
    int count;
} VTable;

/* Get or create vtable for a class */
VTable* vtable_get_or_create(JavaClass* clazz) {
    (void)clazz;
    /* VTable is stored in clazz->header.reserved for now */
    /* This is a placeholder for future implementation */
    return NULL;
}

/* ============================================
 * Initialization
 * ============================================ */

void method_cache_init(void) {
    memset(g_method_cache, 0, sizeof(g_method_cache));
    memset(g_class_hash, 0, sizeof(g_class_hash));
    g_method_cache_hits = 0;
    g_method_cache_misses = 0;
    g_class_count = 0;
}

void method_cache_cleanup(void) {
    class_hash_clear();
}

/* FIX (audit S-10, v18): drop every cached JavaMethod* — must be called when
 * a class's methods[] array is reallocated (stubs.c ensure_methods_capacity
 * growth), otherwise cached pointers keep pointing into the freed block and
 * the VM executes freed memory. */
void method_cache_flush(void) {
    memset(g_method_cache, 0, sizeof(g_method_cache));
}
