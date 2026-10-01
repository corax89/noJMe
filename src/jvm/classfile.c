/*
 * J2ME Emulator - Class File Parser
 * Parses Java .class files according to JVM Specification
 */

#include <stdio.h>
#include "debug.h"
#include "debug_macros.h"
#include <stdlib.h>
#include <string.h>

#include "classfile.h"
#include "jvm.h"
#include "opcodes.h"  /* For ACC_NATIVE */
#include "native.h"  /* For get_or_create_stub_class */

/* Create a class file reader */
ClassFileReader* classfile_reader_create(const uint8_t* data, size_t length) {
    ClassFileReader* reader = (ClassFileReader*)malloc(sizeof(ClassFileReader));
    if (!reader) return NULL;
    
    reader->data = data;
    reader->length = length;
    reader->position = 0;
    reader->error = NULL;
    reader->error_position = 0;
    
    return reader;
}

/* Destroy a class file reader */
void classfile_reader_destroy(ClassFileReader* reader) {
    free(reader);
}

/* Read a single byte */
int classfile_read_u1(ClassFileReader* reader, uint8_t* value) {
    if (reader->position >= reader->length) {
        reader->error = "Unexpected end of data";
        reader->error_position = reader->position;
        return -1;
    }
    *value = reader->data[reader->position++];
    return 0;
}

/* Read a 16-bit value */
int classfile_read_u2(ClassFileReader* reader, uint16_t* value) {
    if (reader->position + 2 > reader->length) {
        reader->error = "Unexpected end of data";
        reader->error_position = reader->position;
        return -1;
    }
    *value = jvm_read_u16(reader->data + reader->position);
    reader->position += 2;
    return 0;
}

/* Read a 32-bit value */
int classfile_read_u4(ClassFileReader* reader, uint32_t* value) {
    if (reader->position + 4 > reader->length) {
        reader->error = "Unexpected end of data";
        reader->error_position = reader->position;
        return -1;
    }
    *value = jvm_read_u32(reader->data + reader->position);
    reader->position += 4;
    return 0;
}

/* Read bytes into buffer */
int classfile_read_bytes(ClassFileReader* reader, void* buffer, size_t count) {
    if (reader->position + count > reader->length) {
        reader->error = "Unexpected end of data";
        reader->error_position = reader->position;
        return -1;
    }
    memcpy(buffer, reader->data + reader->position, count);
    reader->position += count;
    return 0;
}

/* Skip bytes */
int classfile_skip(ClassFileReader* reader, size_t count) {
    if (reader->position + count > reader->length) {
        reader->error = "Unexpected end of data";
        reader->error_position = reader->position;
        return -1;
    }
    reader->position += count;
    return 0;
}

/* Get current position */
size_t classfile_position(ClassFileReader* reader) {
    return reader->position;
}

/* Check remaining bytes */
size_t classfile_remaining(ClassFileReader* reader) {
    return reader->length - reader->position;
}

/* Parse constant pool */
int classfile_parse_constant_pool(ClassFileReader* reader, JavaClass* clazz) {
    if (classfile_read_u2(reader, &clazz->constant_pool_count) != 0) {
        return -1;
    }
    
    DEBUG_LOG("[CP DEBUG] constant_pool_count = %d, sizeof(ConstantPoolEntry) = %zu",
            clazz->constant_pool_count, sizeof(ConstantPoolEntry));
    
    clazz->constant_pool_capacity = clazz->constant_pool_count;
    clazz->constant_pool = (ConstantPoolEntry*)calloc(
        clazz->constant_pool_count, sizeof(ConstantPoolEntry));
    if (!clazz->constant_pool) return -1;
    
    /* Index 0 is unused */
    for (uint16_t i = 1; i < clazz->constant_pool_count; i++) {
        ConstantPoolEntry* entry = &clazz->constant_pool[i];
        
        if (classfile_read_u1(reader, &entry->tag) != 0) {
            return -1;
        }
        
        /* DEBUG: Log first 20 entries */
        if (i <= 20 && g_j2me_runtime_debug) {
            LOG_SAFE("[CP DEBUG] entry[%d].tag = %d\n", i, entry->tag);
        }
        
        switch (entry->tag) {
            case CONSTANT_Utf8: {
                uint16_t length;
                if (classfile_read_u2(reader, &length) != 0) return -1;
                entry->info.utf8.length = length;
                entry->info.utf8.bytes = (char*)malloc(length + 1);
                if (!entry->info.utf8.bytes) return -1;
                if (classfile_read_bytes(reader, entry->info.utf8.bytes, length) != 0) {
                    return -1;
                }
                entry->info.utf8.bytes[length] = '\0';
                break;
            }
            
            case CONSTANT_Integer: {
                uint32_t value;
                if (classfile_read_u4(reader, &value) != 0) return -1;
                entry->info.integer.value = (jint)value;
                break;
            }
            
            case CONSTANT_Float: {
                uint32_t value;
                if (classfile_read_u4(reader, &value) != 0) return -1;
                memcpy(&entry->info.float_val.value, &value, sizeof(jfloat));
                break;
            }
            
            case CONSTANT_Long: {
                uint32_t high, low;
                if (classfile_read_u4(reader, &high) != 0) return -1;
                if (classfile_read_u4(reader, &low) != 0) return -1;
                uint64_t val = ((uint64_t)high << 32) | low;
                memcpy(&entry->info.long_val.value, &val, sizeof(jlong));
                i++;  /* Long takes two entries */
                break;
            }
            
            case CONSTANT_Double: {
                uint32_t high, low;
                if (classfile_read_u4(reader, &high) != 0) return -1;
                if (classfile_read_u4(reader, &low) != 0) return -1;
                uint64_t val = ((uint64_t)high << 32) | low;
                memcpy(&entry->info.double_val.value, &val, sizeof(jdouble));
                i++;  /* Double takes two entries */
                break;
            }
            
            case CONSTANT_Class:
                if (classfile_read_u2(reader, &entry->info.class_info.name_index) != 0) return -1;
                break;
            
            case CONSTANT_String:
                if (classfile_read_u2(reader, &entry->info.string_info.string_index) != 0) return -1;
                break;
            
            case CONSTANT_Fieldref:
            case CONSTANT_Methodref:
            case CONSTANT_InterfaceMethodref:
                if (classfile_read_u2(reader, &entry->info.ref_info.class_index) != 0) return -1;
                if (classfile_read_u2(reader, &entry->info.ref_info.name_and_type_index) != 0) return -1;
                break;
            
            case CONSTANT_NameAndType:
                if (classfile_read_u2(reader, &entry->info.name_and_type.name_index) != 0) return -1;
                if (classfile_read_u2(reader, &entry->info.name_and_type.descriptor_index) != 0) return -1;
                break;
            
            default:
                LOG_SAFE("Unknown constant pool tag: %d at index %d\n", 
                        entry->tag, i);
                return -1;
        }
    }
    
    return 0;
}

/* Get UTF8 string from constant pool */
const char* classfile_get_utf8(JavaClass* clazz, uint16_t index) {
    if (index == 0 || index >= clazz->constant_pool_count) return NULL;
    
    ConstantPoolEntry* entry = &clazz->constant_pool[index];
    if (entry->tag != CONSTANT_Utf8) return NULL;
    
    return entry->info.utf8.bytes;
}

/* Get class name from constant pool */
const char* classfile_get_class_name(JavaClass* clazz, uint16_t index) {
    if (index == 0 || index >= clazz->constant_pool_count) return NULL;
    
    ConstantPoolEntry* entry = &clazz->constant_pool[index];
    if (entry->tag != CONSTANT_Class) return NULL;
    
    return classfile_get_utf8(clazz, entry->info.class_info.name_index);
}

/* Get name and type */
int classfile_get_name_and_type(JavaClass* clazz, uint16_t index, 
                                const char** name, const char** descriptor) {
    if (index == 0 || index >= clazz->constant_pool_count) return -1;
    
    ConstantPoolEntry* entry = &clazz->constant_pool[index];
    if (entry->tag != CONSTANT_NameAndType) return -1;
    
    *name = classfile_get_utf8(clazz, entry->info.name_and_type.name_index);
    *descriptor = classfile_get_utf8(clazz, entry->info.name_and_type.descriptor_index);
    
    return 0;
}

/* Parse fields */
int classfile_parse_fields(ClassFileReader* reader, JavaClass* clazz) {
    if (classfile_read_u2(reader, &clazz->fields_count) != 0) return -1;
    
    if (clazz->fields_count == 0) {
        clazz->fields = NULL;
        return 0;
    }
    
    clazz->fields = (JavaField*)calloc(clazz->fields_count, sizeof(JavaField));
    if (!clazz->fields) return -1;
    
    for (uint16_t i = 0; i < clazz->fields_count; i++) {
        JavaField* field = &clazz->fields[i];
        
        if (classfile_read_u2(reader, &field->access_flags) != 0) return -1;
        if (classfile_read_u2(reader, &field->name_index) != 0) return -1;
        if (classfile_read_u2(reader, &field->descriptor_index) != 0) return -1;
        
        field->name = (char*)classfile_get_utf8(clazz, field->name_index);
        field->descriptor = (char*)classfile_get_utf8(clazz, field->descriptor_index);
        field->clazz = clazz;
        
        /* Read attributes count first, then parse attributes */
        if (classfile_read_u2(reader, &field->attributes_count) != 0) return -1;
        
        if (classfile_parse_attributes(reader, clazz, &field->attributes, 
                                        field->attributes_count) != 0) {
            return -1;
        }
    }
    
    return 0;
}

/* Parse methods */
int classfile_parse_methods(ClassFileReader* reader, JavaClass* clazz) {
    if (classfile_read_u2(reader, &clazz->methods_count) != 0) return -1;
    
    if (clazz->methods_count == 0) {
        clazz->methods = NULL;
        clazz->methods_capacity = 0;
        return 0;
    }
    
    clazz->methods = (JavaMethod*)calloc(clazz->methods_count, sizeof(JavaMethod));
    if (!clazz->methods) return -1;
    clazz->methods_capacity = clazz->methods_count;
    
    for (uint16_t i = 0; i < clazz->methods_count; i++) {
        JavaMethod* method = &clazz->methods[i];
        
        if (classfile_read_u2(reader, &method->access_flags) != 0) return -1;
        if (classfile_read_u2(reader, &method->name_index) != 0) return -1;
        if (classfile_read_u2(reader, &method->descriptor_index) != 0) return -1;
        
        method->name = (char*)classfile_get_utf8(clazz, method->name_index);
        method->descriptor = (char*)classfile_get_utf8(clazz, method->descriptor_index);
        method->clazz = clazz;
        method->is_native = (method->access_flags & ACC_NATIVE) != 0;
        /* FIX: calloc zeroes the cache; but 0 is a VALID arg count ("()V" etc).
         * Sentinel convention is "-1 = not yet computed", so set it explicitly,
         * otherwise jvm_invoke_virtual/special built full_args with argc=0 and
         * every Java-level callback invoked from C lost its arguments. */
        method->cached_arg_count = -1;
        
        uint16_t attr_count;
        if (classfile_read_u2(reader, &attr_count) != 0) return -1;
        
        /* Parse attributes */
        for (uint16_t j = 0; j < attr_count; j++) {
            uint16_t name_index;
            uint32_t length;
            
            if (classfile_read_u2(reader, &name_index) != 0) return -1;
            if (classfile_read_u4(reader, &length) != 0) return -1;
            
            const char* attr_name = classfile_get_utf8(clazz, name_index);
            
            if (attr_name && strcmp(attr_name, "Code") == 0) {
                /* Parse Code attribute */
                if (classfile_read_u2(reader, &method->code.max_stack) != 0) return -1;
                if (classfile_read_u2(reader, &method->code.max_locals) != 0) return -1;
                if (classfile_read_u4(reader, &method->code.code_length) != 0) return -1;

                /* DEBUG: Log max_locals=0 methods */
                if (method->code.max_locals == 0 && method->code.code_length > 0) {
                    VERBOSE_LOG("[CLASSFILE-DEBUG] %s.%s%s has max_locals=0, code_len=%u\n",
                            clazz->class_name ? clazz->class_name : "?",
                            method->name ? method->name : "?",
                            method->descriptor ? method->descriptor : "?",
                            method->code.code_length);
                }
                
                method->code.code = (uint8_t*)malloc(method->code.code_length);
                if (!method->code.code) return -1;
                
                if (classfile_read_bytes(reader, method->code.code, 
                                         method->code.code_length) != 0) {
                    return -1;
                }
                
                /* Exception table */
                if (classfile_read_u2(reader, &method->code.exception_table_length) != 0) {
                    return -1;
                }
                
                if (method->code.exception_table_length > 0) {
                    method->code.exception_table = (ExceptionTableEntry*)calloc(
                        method->code.exception_table_length, sizeof(ExceptionTableEntry));
                    
                    for (uint16_t k = 0; k < method->code.exception_table_length; k++) {
                        classfile_read_u2(reader, &method->code.exception_table[k].start_pc);
                        classfile_read_u2(reader, &method->code.exception_table[k].end_pc);
                        classfile_read_u2(reader, &method->code.exception_table[k].handler_pc);
                        classfile_read_u2(reader, &method->code.exception_table[k].catch_type);

                        /* DEBUG: Log each exception_table entry */
                        const char* catch_cls = (method->code.exception_table[k].catch_type != 0)
                            ? classfile_get_class_name(clazz, method->code.exception_table[k].catch_type)
                            : "<any/finally>";
                        VERBOSE_LOG("[EX-TABLE] %s.%s%s entry[%d]: start_pc=%d end_pc=%d handler_pc=%d catch_type=%d (%s)\n",
                            clazz->class_name ? clazz->class_name : "?",
                            method->name ? method->name : "?",
                            method->descriptor ? method->descriptor : "?",
                            k,
                            method->code.exception_table[k].start_pc,
                            method->code.exception_table[k].end_pc,
                            method->code.exception_table[k].handler_pc,
                            method->code.exception_table[k].catch_type,
                            catch_cls ? catch_cls : "?");
                    }
                }
                
                /* Skip code attributes */
                uint16_t code_attr_count;
                if (classfile_read_u2(reader, &code_attr_count) != 0) return -1;
                
                for (uint16_t k = 0; k < code_attr_count; k++) {
                    uint16_t skip_name;
                    uint32_t skip_len = 0;
                    classfile_read_u2(reader, &skip_name);
                    classfile_read_u4(reader, &skip_len);
                    classfile_skip(reader, skip_len);
                }

                /* === FIX: Auto-correct max_locals for obfuscated bytecode ===
                 * Some obfuscated J2ME games have max_locals=0 but still use
                 * local variable operations (fstore_0, aload_0, etc.).
                 * KEmulator and other emulators handle this by silently
                 * expanding max_locals. We scan bytecode for the highest
                 * local index used and ensure max_locals covers it.
                 * Also account for method parameters (this + args). */
                {
                    int needed_locals = 0;
                    /* Count method parameters */
                    int param_slots = 0;
                    if (method->descriptor) {
                        const char* d = method->descriptor;
                        if (d[0] != '(') { /* not a descriptor */ }
                        else {
                            d++; /* skip '(' */
                            while (*d && *d != ')') {
                                param_slots++;
                                if (*d == 'J' || *d == 'D') param_slots++; /* 2-slot types */
                                if (*d == 'L') { while (*d && *d != ';') d++; }
                                if (*d == '[') { while (*d == '[') d++; if (*d == 'L') { while (*d && *d != ';') d++; } }
                                d++;
                            }
                        }
                    }
                    if (!(method->access_flags & 0x0008)) param_slots++; /* non-static: this */
                    needed_locals = param_slots;

                    /* Scan bytecode for local variable references */
                    uint8_t* code = method->code.code;
                    uint32_t code_len = method->code.code_length;
                    uint32_t pc = 0;
                    while (pc < code_len) {
                        uint8_t op = code[pc];
                        int local_idx = -1;
                        /* Wide prefix */
                        if (op == 0xc4 && pc + 1 < code_len) {
                            if (pc + 3 < code_len) {
                                local_idx = (code[pc + 2] << 8) | code[pc + 3];
                            }
                            pc += 4; /* wide opcode takes 4 bytes */
                        } else {
                            /* xstore_0..3, xload_0..3 shortcuts */
                            if (op >= 0x1a && op <= 0x1d) local_idx = op - 0x1a;  /* iload_0..3 */
                            else if (op >= 0x1e && op <= 0x21) local_idx = op - 0x1e;  /* lload_0..3 */
                            else if (op >= 0x22 && op <= 0x25) local_idx = op - 0x22;  /* fload_0..3 */
                            else if (op >= 0x26 && op <= 0x29) local_idx = op - 0x26;  /* dload_0..3 */
                            else if (op >= 0x2a && op <= 0x2d) local_idx = op - 0x2a;  /* aload_0..3 */
                            else if (op >= 0x3b && op <= 0x3e) local_idx = op - 0x3b;  /* istore_0..3 */
                            else if (op >= 0x3f && op <= 0x42) local_idx = op - 0x3f;  /* lstore_0..3 */
                            else if (op >= 0x43 && op <= 0x46) local_idx = op - 0x43;  /* fstore_0..3 */
                            else if (op >= 0x47 && op <= 0x4a) local_idx = op - 0x47;  /* dstore_0..3 */
                            else if (op >= 0x4b && op <= 0x4e) local_idx = op - 0x4b;  /* astore_0..3 */

                            /* iinc, load, store with explicit index byte */
                            if (local_idx < 0) {
                                if (op == 0x84) local_idx = code[pc + 1];       /* iinc */
                                else if (op == 0x15) local_idx = code[pc + 1];  /* iload */
                                else if (op == 0x16) local_idx = code[pc + 1];  /* lload */
                                else if (op == 0x17) local_idx = code[pc + 1];  /* fload */
                                else if (op == 0x18) local_idx = code[pc + 1];  /* dload */
                                else if (op == 0x19) local_idx = code[pc + 1];  /* aload */
                                else if (op == 0x36) local_idx = code[pc + 1];  /* istore */
                                else if (op == 0x37) local_idx = code[pc + 1];  /* lstore */
                                else if (op == 0x38) local_idx = code[pc + 1];  /* fstore */
                                else if (op == 0x39) local_idx = code[pc + 1];  /* dstore */
                                else if (op == 0x3a) local_idx = code[pc + 1];  /* astore */
                                else if (op == 0xa9) local_idx = code[pc + 1];  /* ret */
                            }

                            /* Advance PC based on opcode */
                            switch (op) {
                                case 0x10: pc += 2; break; /* bipush */
                                case 0x11: pc += 3; break; /* sipush */
                                case 0x12: pc += 2; break; /* ldc */
                                case 0x13: pc += 3; break; /* ldc_w */
                                case 0x14: pc += 3; break; /* ldc2_w */
                                case 0x15: case 0x16: case 0x17: case 0x18:
                                case 0x19: /* xload */
                                case 0x36: case 0x37: case 0x38: case 0x39:
                                case 0x3a: /* xstore */
                                case 0x84: /* iinc */
                                case 0xa9: /* ret */
                                    pc += 2; break;
                                case 0x99: case 0x9a: case 0x9b: case 0x9c:
                                case 0x9d: case 0x9e: case 0x9f: case 0xa0:
                                case 0xa1: case 0xa2: case 0xa3: case 0xa4:
                                case 0xa5: case 0xa6: /* if<cond> */
                                case 0xa7: /* goto */
                                case 0xa8: /* jsr */
                                    pc += 3; break;
                                /* tableswitch: variable length (aligned to 4 bytes) */
                                case 0xaa: {
                                    int pad = (4 - ((pc + 1) % 4)) % 4;
                                    int base = pc + 1 + pad;
                                    if (base + 12 <= (int)code_len) {
                                        int32_t low = (int32_t)(((uint32_t)(code[base+4]) <<  24) | (code[base+5] << 16) | (code[base+6] << 8) | code[base+7]);
                                        int32_t high = (int32_t)(((uint32_t)(code[base+8]) <<  24) | (code[base+9] << 16) | (code[base+10] << 8) | code[base+11]);
                                        pc = base + 12 + (high - low + 1) * 4;
                                    } else {
                                        pc = code_len;
                                    }
                                    break;
                                }
                                /* lookupswitch: variable length (aligned to 4 bytes) */
                                case 0xab: {
                                    int pad = (4 - ((pc + 1) % 4)) % 4;
                                    int base = pc + 1 + pad;
                                    if (base + 8 <= (int)code_len) {
                                        int32_t npairs = (int32_t)(((uint32_t)(code[base+4]) <<  24) | (code[base+5] << 16) | (code[base+6] << 8) | code[base+7]);
                                        pc = base + 8 + npairs * 8;
                                    } else {
                                        pc = code_len;
                                    }
                                    break;
                                }
                                case 0xb2: case 0xb3: case 0xb4: case 0xb5:
                                case 0xb6: case 0xb7: case 0xb8:
                                case 0xbb:
                                case 0xc0: case 0xc1: /* checkcast, instanceof */
                                case 0xc6: /* ifnull */
                                case 0xc7: /* ifnonnull */
                                    pc += 3; break;
                                case 0xb9: pc += 5; break; /* invokeinterface */
                                case 0xba: pc += 5; break; /* invokedynamic */
                                case 0xbc: pc += 2; break; /* newarray */
                                case 0xbd: pc += 3; break; /* anewarray */
                                case 0xc5: pc += 4; break; /* multianewarray */
                                case 0xc8: pc += 5; break; /* goto_w */
                                case 0xc9: pc += 5; break; /* jsr_w */
                                default: pc += 1; break;
                            }
                        }
                        if (local_idx >= 0 && local_idx + 1 > needed_locals) {
                            needed_locals = local_idx + 1;
                        }
                    }

                    VERBOSE_LOG("[CLASSFILE-SCAN] %s.%s%s max_locals=%d needed=%d\n",
                            clazz->class_name ? clazz->class_name : "?",
                            method->name ? method->name : "?",
                            method->descriptor ? method->descriptor : "?",
                            method->code.max_locals, needed_locals);

                    if (needed_locals > method->code.max_locals) {
                        LOG_SAFE("[CLASSFILE] Fix: %s.%s%s max_locals=%d but bytecode needs %d, correcting\n",
                                clazz->class_name ? clazz->class_name : "?",
                                method->name ? method->name : "?",
                                method->descriptor ? method->descriptor : "?",
                                method->code.max_locals, needed_locals);
                        method->code.max_locals = (uint16_t)needed_locals;
                    }
                }
            } else {
                /* Skip other attributes */
                classfile_skip(reader, length);
            }
        }
    }
    
    return 0;
}

/* Parse attributes */
int classfile_parse_attributes(ClassFileReader* reader, JavaClass* clazz,
                               AttributeInfo** attributes, uint16_t count) {
    (void)clazz;
    if (count == 0) {
        *attributes = NULL;
        return 0;
    }
    
    *attributes = (AttributeInfo*)calloc(count, sizeof(AttributeInfo));
    if (!*attributes) return -1;
    
    for (uint16_t i = 0; i < count; i++) {
        AttributeInfo* attr = &(*attributes)[i];
        
        if (classfile_read_u2(reader, &attr->name_index) != 0) return -1;
        if (classfile_read_u4(reader, &attr->length) != 0) return -1;
        
        if (attr->length > 0) {
            attr->info = (uint8_t*)malloc(attr->length);
            if (!attr->info) return -1;
            if (classfile_read_bytes(reader, attr->info, attr->length) != 0) return -1;
        }
    }
    
    return 0;
}

/* Parse a class file */
JavaClass* classfile_parse(JVM* jvm, const uint8_t* data, size_t length) {
    ClassFileReader* reader = classfile_reader_create(data, length);
    if (!reader) return NULL;
    
    JavaClass* clazz = (JavaClass*)calloc(1, sizeof(JavaClass));
    if (!clazz) {
        classfile_reader_destroy(reader);
        return NULL;
    }
    
    /* Read magic number */
    if (classfile_read_u4(reader, &clazz->magic) != 0) {
        LOG_SAFE("Failed to read magic number\n");
        goto error;
    }
    
    if (clazz->magic != CLASS_FILE_MAGIC) {
        LOG_SAFE("Invalid class file magic: 0x%08X\n", clazz->magic);
        goto error;
    }
    
    /* Read version */
    if (classfile_read_u2(reader, &clazz->minor_version) != 0) goto error;
    if (classfile_read_u2(reader, &clazz->major_version) != 0) goto error;
    
    /* Parse constant pool */
    if (classfile_parse_constant_pool(reader, clazz) != 0) {
        LOG_SAFE("Failed to parse constant pool\n");
        goto error;
    }
    
    /* Read access flags */
    if (classfile_read_u2(reader, &clazz->access_flags) != 0) goto error;
    
    /* Read this class */
    if (classfile_read_u2(reader, &clazz->this_class) != 0) goto error;
    
    /* Read super class */
    if (classfile_read_u2(reader, &clazz->super_class_index) != 0) goto error;
    
    /* Read interfaces */
    if (classfile_read_u2(reader, &clazz->interfaces_count) != 0) goto error;
    
    if (clazz->interfaces_count > 0) {
        clazz->interfaces = (uint16_t*)calloc(clazz->interfaces_count, sizeof(uint16_t));
        for (uint16_t i = 0; i < clazz->interfaces_count; i++) {
            if (classfile_read_u2(reader, &clazz->interfaces[i]) != 0) goto error;
        }
    }
    
    /* Parse fields */
    if (classfile_parse_fields(reader, clazz) != 0) {
        LOG_SAFE("Failed to parse fields\n");
        goto error;
    }
    
    /* Parse methods */
    if (classfile_parse_methods(reader, clazz) != 0) {
        LOG_SAFE("Failed to parse methods\n");
        goto error;
    }
    
    /* Read class attributes */
    if (classfile_read_u2(reader, &clazz->attributes_count) != 0) goto error;
    
    if (clazz->attributes_count > 0) {
        if (classfile_parse_attributes(reader, clazz, &clazz->attributes, 
                                        clazz->attributes_count) != 0) {
            goto error;
        }
    }
    
    /* Resolve class name */
    clazz->class_name = (char*)classfile_get_class_name(clazz, clazz->this_class);
    
    /* Resolve super class name */
    if (clazz->super_class_index > 0) {
        clazz->super_class_name = (char*)classfile_get_class_name(clazz, clazz->super_class_index);
    }

    /* === НАЧАЛО ИСПРАВЛЕНИЯ: Расчет instance_size === */
    
    /* 1. Наследуем размер от суперкласса или начинаем с ObjectHeader */
    if (jvm && clazz->super_class_name) {
        /* Ищем суперкласс в списке уже загруженных классов */
        if (jvm->class_loader.classes != NULL) {
            for (size_t i = 0; i < jvm->class_loader.count; i++) {
                JavaClass* super = jvm->class_loader.classes[i];
                if (super->class_name && strcmp(super->class_name, clazz->super_class_name) == 0) {
                    /* v34.49 FIX (3D Ferrari): THIS direct array hit used to
                     * bind the BUILTIN stub as the superclass without the
                     * JAR-override chance (only get_or_create_stub_class had
                     * it). A MIDlet shipping its own platform shim
                     * (com/nokia/mid/ui/FullCanvas extends FullCn) got its
                     * subclass bound to the builtin FullCanvas (extends
                     * plain Canvas) — every later checkcast against the
                     * game's own hierarchy threw ClassCastException. */
                    if (super->is_stub) {
                        JavaClass* real = jvm_stub_jar_override_linear(jvm, clazz->super_class_name, super);
                        if (real) super = real;
                    }
                    clazz->super_class = super;
                    /* Наследуем размер экземпляра (уже включает ObjectHeader) */
                    clazz->instance_size = super->instance_size;
                    break;
                }
            }
        }
        /* CRITICAL FIX: If superclass not found in loaded classes, try to create
         * a stub. This handles cases like Life3D where MIDlet extends
         * javax/microedition/midlet/MIDlet which hasn't been loaded yet.
         * Without this, super_class stays NULL and jvm_resolve_method can't
         * find inherited methods like startApp(). */
        if (!clazz->super_class && strcmp(clazz->super_class_name, "java/lang/Object") != 0) {
            clazz->super_class = get_or_create_stub_class(jvm, clazz->super_class_name);
            if (clazz->super_class) {
                clazz->instance_size = clazz->super_class->instance_size;
            }
        }
    }
    
    /* Если суперкласс не найден, начинаем с размера ObjectHeader */
    if (clazz->instance_size == 0) {
        clazz->instance_size = sizeof(ObjectHeader);
    }

    /* 2. Добавляем размер собственных полей экземпляра */
    if (clazz->fields) {
        for (uint16_t i = 0; i < clazz->fields_count; i++) {
            JavaField* field = &clazz->fields[i];
            
            /* Пропускаем статические поля - они не хранятся в экземпляре */
            if (field->access_flags & ACC_STATIC) {
                continue;
            }
            
            /* Увеличиваем размер на один слот (JavaValue) */
            clazz->instance_size += sizeof(JavaValue);
            
            /* Long и Double занимают 2 слота */
            if (field->descriptor && 
                (field->descriptor[0] == 'J' || field->descriptor[0] == 'D')) {
                clazz->instance_size += sizeof(JavaValue);
            }
        }
    }
    
    /* === КОНЕЦ ИСПРАВЛЕНИЯ === */
    
    classfile_reader_destroy(reader);
    return clazz;
    
error:
    classfile_reader_destroy(reader);
    classfile_free(clazz);
    return NULL;
}

/* Parse class from file */
JavaClass* classfile_parse_file(JVM* jvm, const char* filename) {
    FILE* f = fopen(filename, "rb");
    if (!f) return NULL;
    
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (size <= 0 || size > 10 * 1024 * 1024) {  /* Max 10 MB */
        fclose(f);
        return NULL;
    }
    
    uint8_t* data = (uint8_t*)malloc(size);
    if (!data) {
        fclose(f);
        return NULL;
    }
    
    if (fread(data, 1, size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return NULL;
    }
    
    fclose(f);
    
    JavaClass* clazz = classfile_parse(jvm, data, size);
    free(data);
    
    return clazz;
}

/* Free a class */
void classfile_free(JavaClass* clazz) {
    if (!clazz) return;
    
    /* Free constant pool */
    if (clazz->constant_pool) {
        for (uint16_t i = 1; i < clazz->constant_pool_count; i++) {
            if (clazz->constant_pool[i].tag == CONSTANT_Utf8) {
                free(clazz->constant_pool[i].info.utf8.bytes);
            }
        }
        free(clazz->constant_pool);
    }
    
    /* Free interfaces */
    free(clazz->interfaces);
    
    /* Free fields */
    if (clazz->fields) {
        for (uint16_t i = 0; i < clazz->fields_count; i++) {
            if (clazz->fields[i].attributes) {
                for (uint16_t j = 0; j < clazz->fields[i].attributes_count; j++) {
                    free(clazz->fields[i].attributes[j].info);
                }
                free(clazz->fields[i].attributes);
            }
        }
        free(clazz->fields);
    }
    
    /* Free methods */
    if (clazz->methods) {
        for (uint16_t i = 0; i < clazz->methods_count; i++) {
            free(clazz->methods[i].code.code);
            free(clazz->methods[i].code.exception_table);
        }
        free(clazz->methods);
    }
    
    /* Free attributes */
    if (clazz->attributes) {
        for (uint16_t i = 0; i < clazz->attributes_count; i++) {
            free(clazz->attributes[i].info);
        }
        free(clazz->attributes);
    }
    
    /* Free static fields */
    /* v36.12: static field name/descriptor are ALWAYS strdups when they
     * exist (execute.c putstatic injection and stub_add_static_const both
     * strdup; the classfile parser never populates this array) — the bare
     * free(static_fields) leaked every strdup for the process lifetime. */
    if (clazz->static_fields) {
        for (int sf_i = 0; sf_i < clazz->static_fields_count; sf_i++) {
            free(clazz->static_fields[sf_i].name);
            free(clazz->static_fields[sf_i].descriptor);
        }
    }
    free(clazz->static_fields);
    
        /* v34.35 PERF: CP inline caches */
    free(clazz->field_cache);
    free(clazz->invoke_cache);
    free(clazz->static_field_cache);
    /* v34.59 PERF: GC mark-layout cache */
    free(clazz->gc_ref_slots);
    free(clazz);
}
