/*
 * J2ME Emulator - Bytecode Opcode Handlers
 * Implementation of all Java bytecode instructions
 */

/* For strdup on POSIX */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <time.h>       /* v34.6: clock_gettime in invoke tracer */
#include "core_version.h"
#include <limits.h>

/* For Windows strdup compatibility */
#ifdef _WIN32
#ifndef strdup
#define strdup _strdup
#endif
#endif

#include "opcodes.h"
#include "cp_caches.h"
#include "jvm.h"

/* M3G debug logging - disabled by default */
#ifndef M3G_DEBUG_LOG
#define M3G_DEBUG_LOG 0
#endif
#include "classfile.h"
#include "heap.h"
#include "native.h"
#include "drm_bypass.h"
#include "threads.h"  /* For monitor_enter/exit */
#include "debug.h"    /* For g_j2me_runtime_debug and logging macros */
#include "debug_macros.h"

#ifndef J2ME_DEBUG
#define J2ME_DEBUG 1
#endif

/* Bounds checking can be explicitly disabled for release builds (e.g. m17 ARM) */
#ifdef OPT_NO_BOUNDS_CHECK
#undef J2ME_DEBUG
#define J2ME_DEBUG 0
#endif

/* 
 * Logging control - uses runtime debug flag (toggle with F12)
 * Debug messages are only shown when g_j2me_runtime_debug is set.
 * Errors and warnings are always shown via ERROR_LOG/WARN_LOG macros.
 */
#define LOG_OPCODE(...) do { \
    if (g_j2me_runtime_debug) { \
        LOG_SAFE(__VA_ARGS__); \
    } \
} while(0)
#define LOG_DEBUG(...) do { \
    if (g_j2me_runtime_debug) { \
        LOG_SAFE(__VA_ARGS__); \
    } \
} while(0)

/* External function from native.c */
extern int count_args(const char* descriptor);

/* External function from execute.c */
extern int execute_method(JVM* jvm, JavaThread* thread, JavaMethod* method, 
                          JavaValue* args, JavaValue* result);

/* ==================================================================
 * v18 FIX (argument-marshalling desync): count_args() returns STACK
 * SLOTS (J/D = 2 slots), while the execute_method binder and the
 * native handlers consume a COMPACT args[] - exactly one JavaValue
 * per Java argument, J/D included. op_invokespecial already builds
 * the compact form; op_invokestatic/-virtual/-interface used to pop
 * slot-wise, which SHIFTED every argument after the first long/double
 * (e.g. Log.eqD(String,double,double,double) received `expected` as
 * `actual` and `actual` as `eps`, making every comparison with a
 * negative double fail). The helpers live BELOW the POP/PUSH macro
 * definitions and centralize the compact form.
 * ================================================================== */

/* External functions from jvm.c */
extern JavaFrame* create_frame(JVM* jvm, JavaThread* thread, JavaMethod* method, JavaValue* args);
extern void free_frame(JavaFrame* frame);

/* External function from heap.c - for getting UTF8 from Java string */
extern const char* string_utf8(JVM* jvm, JavaString* str);

/* External function from native.c - for intern pool lookup by UTF-8 content */
extern JavaString* native_intern_find_by_utf8(const char* utf8, jsize utf8_len);

/* Forward declarations for long/double specific opcodes */
int op_lload_0(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_lload_1(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_lload_2(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_lload_3(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_dload_0(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_dload_1(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_dload_2(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_dload_3(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_lstore_0(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_lstore_1(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_lstore_2(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_lstore_3(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_dstore_0(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_dstore_1(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_dstore_2(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_dstore_3(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_lstore(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_dstore(JVM* jvm, JavaThread* thread, JavaFrame* frame);

/* Opcode information table */
const OpcodeInfo opcode_table[256] = {
    [OPC_NOP]           = {"nop", 0, 0, op_nop},
    [OPC_ACONST_NULL]   = {"aconst_null", 0, 1, op_aconst_null},
    [OPC_ICONST_M1]     = {"iconst_m1", 0, 1, op_iconst},
    [OPC_ICONST_0]      = {"iconst_0", 0, 1, op_iconst},
    [OPC_ICONST_1]      = {"iconst_1", 0, 1, op_iconst},
    [OPC_ICONST_2]      = {"iconst_2", 0, 1, op_iconst},
    [OPC_ICONST_3]      = {"iconst_3", 0, 1, op_iconst},
    [OPC_ICONST_4]      = {"iconst_4", 0, 1, op_iconst},
    [OPC_ICONST_5]      = {"iconst_5", 0, 1, op_iconst},
    [OPC_LCONST_0]      = {"lconst_0", 0, 2, op_lconst},
    [OPC_LCONST_1]      = {"lconst_1", 0, 2, op_lconst},
    [OPC_FCONST_0]      = {"fconst_0", 0, 1, op_fconst},
    [OPC_FCONST_1]      = {"fconst_1", 0, 1, op_fconst},
    [OPC_FCONST_2]      = {"fconst_2", 0, 1, op_fconst},
    [OPC_DCONST_0]      = {"dconst_0", 0, 2, op_dconst},
    [OPC_DCONST_1]      = {"dconst_1", 0, 2, op_dconst},
    [OPC_BIPUSH]        = {"bipush", 1, 1, op_bipush},
    [OPC_SIPUSH]        = {"sipush", 2, 1, op_sipush},
    [OPC_LDC]           = {"ldc", 1, 1, op_ldc},
    [OPC_LDC_W]         = {"ldc_w", 2, 1, op_ldc_w},
    [OPC_LDC2_W]        = {"ldc2_w", 2, 2, op_ldc2_w},
    
    /* Load instructions */
    [OPC_ILOAD]         = {"iload", 1, 1, op_iload},
    [OPC_LLOAD]         = {"lload", 1, 2, op_lload},
    [OPC_FLOAD]         = {"fload", 1, 1, op_fload},
    [OPC_DLOAD]         = {"dload", 1, 2, op_dload},
    [OPC_ALOAD]         = {"aload", 1, 1, op_aload},
    [OPC_ILOAD_0]       = {"iload_0", 0, 1, op_load_0},
    [OPC_ILOAD_1]       = {"iload_1", 0, 1, op_load_1},
    [OPC_ILOAD_2]       = {"iload_2", 0, 1, op_load_2},
    [OPC_ILOAD_3]       = {"iload_3", 0, 1, op_load_3},
    [OPC_LLOAD_0]       = {"lload_0", 0, 2, op_lload_0},
    [OPC_LLOAD_1]       = {"lload_1", 0, 2, op_lload_1},
    [OPC_LLOAD_2]       = {"lload_2", 0, 2, op_lload_2},
    [OPC_LLOAD_3]       = {"lload_3", 0, 2, op_lload_3},
    [OPC_FLOAD_0]       = {"fload_0", 0, 1, op_load_0},
    [OPC_FLOAD_1]       = {"fload_1", 0, 1, op_load_1},
    [OPC_FLOAD_2]       = {"fload_2", 0, 1, op_load_2},
    [OPC_FLOAD_3]       = {"fload_3", 0, 1, op_load_3},
    [OPC_DLOAD_0]       = {"dload_0", 0, 2, op_dload_0},
    [OPC_DLOAD_1]       = {"dload_1", 0, 2, op_dload_1},
    [OPC_DLOAD_2]       = {"dload_2", 0, 2, op_dload_2},
    [OPC_DLOAD_3]       = {"dload_3", 0, 2, op_dload_3},
    [OPC_ALOAD_0]       = {"aload_0", 0, 1, op_load_0},
    [OPC_ALOAD_1]       = {"aload_1", 0, 1, op_load_1},
    [OPC_ALOAD_2]       = {"aload_2", 0, 1, op_load_2},
    [OPC_ALOAD_3]       = {"aload_3", 0, 1, op_load_3},
    
    /* Array load */
    [OPC_IALOAD]        = {"iaload", 0, -1, op_array_load},
    [OPC_LALOAD]        = {"laload", 0, 0, op_array_load},
    [OPC_FALOAD]        = {"faload", 0, -1, op_array_load},
    [OPC_DALOAD]        = {"daload", 0, 0, op_array_load},
    [OPC_AALOAD]        = {"aaload", 0, -1, op_array_load},
    [OPC_BALOAD]        = {"baload", 0, -1, op_array_load},
    [OPC_CALOAD]        = {"caload", 0, -1, op_array_load},
    [OPC_SALOAD]        = {"saload", 0, -1, op_array_load},
    
    /* Store instructions */
    [OPC_ISTORE]        = {"istore", 1, -1, op_store},
    [OPC_LSTORE]        = {"lstore", 1, -2, op_lstore},
    [OPC_FSTORE]        = {"fstore", 1, -1, op_store},
    [OPC_DSTORE]        = {"dstore", 1, -2, op_dstore},
    [OPC_ASTORE]        = {"astore", 1, -1, op_store},
    [OPC_ISTORE_0]      = {"istore_0", 0, -1, op_store_0},
    [OPC_ISTORE_1]      = {"istore_1", 0, -1, op_store_1},
    [OPC_ISTORE_2]      = {"istore_2", 0, -1, op_store_2},
    [OPC_ISTORE_3]      = {"istore_3", 0, -1, op_store_3},
    [OPC_LSTORE_0]      = {"lstore_0", 0, -2, op_lstore_0},
    [OPC_LSTORE_1]      = {"lstore_1", 0, -2, op_lstore_1},
    [OPC_LSTORE_2]      = {"lstore_2", 0, -2, op_lstore_2},
    [OPC_LSTORE_3]      = {"lstore_3", 0, -2, op_lstore_3},
    [OPC_FSTORE_0]      = {"fstore_0", 0, -1, op_store_0},
    [OPC_FSTORE_1]      = {"fstore_1", 0, -1, op_store_1},
    [OPC_FSTORE_2]      = {"fstore_2", 0, -1, op_store_2},
    [OPC_FSTORE_3]      = {"fstore_3", 0, -1, op_store_3},
    [OPC_DSTORE_0]      = {"dstore_0", 0, -2, op_dstore_0},
    [OPC_DSTORE_1]      = {"dstore_1", 0, -2, op_dstore_1},
    [OPC_DSTORE_2]      = {"dstore_2", 0, -2, op_dstore_2},
    [OPC_DSTORE_3]      = {"dstore_3", 0, -2, op_dstore_3},
    [OPC_ASTORE_0]      = {"astore_0", 0, -1, op_store_0},
    [OPC_ASTORE_1]      = {"astore_1", 0, -1, op_store_1},
    [OPC_ASTORE_2]      = {"astore_2", 0, -1, op_store_2},
    [OPC_ASTORE_3]      = {"astore_3", 0, -1, op_store_3},
    
    /* Array store */
    [OPC_IASTORE]       = {"iastore", 0, -3, op_array_store},
    [OPC_LASTORE]       = {"lastore", 0, -4, op_array_store},
    [OPC_FASTORE]       = {"fastore", 0, -3, op_array_store},
    [OPC_DASTORE]       = {"dastore", 0, -4, op_array_store},
    [OPC_AASTORE]       = {"aastore", 0, -3, op_array_store},
    [OPC_BASTORE]       = {"bastore", 0, -3, op_array_store},
    [OPC_CASTORE]       = {"castore", 0, -3, op_array_store},
    [OPC_SASTORE]       = {"sastore", 0, -3, op_array_store},
    
    /* Stack operations */
    [OPC_POP]           = {"pop", 0, -1, op_pop},
    [OPC_POP2]          = {"pop2", 0, -2, op_pop2},
    [OPC_DUP]           = {"dup", 0, 1, op_dup},
    [OPC_DUP_X1]        = {"dup_x1", 0, 1, op_dup_x1},
    [OPC_DUP_X2]        = {"dup_x2", 0, 1, op_dup_x2},
    [OPC_DUP2]          = {"dup2", 0, 2, op_dup2},
    [OPC_DUP2_X1]       = {"dup2_x1", 0, 2, op_dup2_x1},
    [OPC_DUP2_X2]       = {"dup2_x2", 0, 2, op_dup2_x2},
    [OPC_SWAP]          = {"swap", 0, 0, op_swap},
    
    /* Arithmetic */
    [OPC_IADD]          = {"iadd", 0, -1, op_add},
    [OPC_LADD]          = {"ladd", 0, -2, op_add},
    [OPC_FADD]          = {"fadd", 0, -1, op_add},
    [OPC_DADD]          = {"dadd", 0, -2, op_add},
    [OPC_ISUB]          = {"isub", 0, -1, op_sub},
    [OPC_LSUB]          = {"lsub", 0, -2, op_sub},
    [OPC_FSUB]          = {"fsub", 0, -1, op_sub},
    [OPC_DSUB]          = {"dsub", 0, -2, op_sub},
    [OPC_IMUL]          = {"imul", 0, -1, op_mul},
    [OPC_LMUL]          = {"lmul", 0, -2, op_mul},
    [OPC_FMUL]          = {"fmul", 0, -1, op_mul},
    [OPC_DMUL]          = {"dmul", 0, -2, op_mul},
    [OPC_IDIV]          = {"idiv", 0, -1, op_div},
    [OPC_LDIV]          = {"ldiv", 0, -2, op_div},
    [OPC_FDIV]          = {"fdiv", 0, -1, op_div},
    [OPC_DDIV]          = {"ddiv", 0, -2, op_div},
    [OPC_IREM]          = {"irem", 0, -1, op_rem},
    [OPC_LREM]          = {"lrem", 0, -2, op_rem},
    [OPC_FREM]          = {"frem", 0, -1, op_rem},
    [OPC_DREM]          = {"drem", 0, -2, op_rem},
    [OPC_INEG]          = {"ineg", 0, 0, op_neg},
    [OPC_LNEG]          = {"lneg", 0, 0, op_neg},
    [OPC_FNEG]          = {"fneg", 0, 0, op_neg},
    [OPC_DNEG]          = {"dneg", 0, 0, op_neg},
    
    /* Bit operations */
    [OPC_ISHL]          = {"ishl", 0, -1, op_shl},
    [OPC_LSHL]          = {"lshl", 0, -1, op_shl},
    [OPC_ISHR]          = {"ishr", 0, -1, op_shr},
    [OPC_LSHR]          = {"lshr", 0, -1, op_shr},
    [OPC_IUSHR]         = {"iushr", 0, -1, op_ushr},
    [OPC_LUSHR]         = {"lushr", 0, -1, op_ushr},
    [OPC_IAND]          = {"iand", 0, -1, op_and},
    [OPC_LAND]          = {"land", 0, -2, op_and},
    [OPC_IOR]           = {"ior", 0, -1, op_or},
    [OPC_LOR]           = {"lor", 0, -2, op_or},
    [OPC_IXOR]          = {"ixor", 0, -1, op_xor},
    [OPC_LXOR]          = {"lxor", 0, -2, op_xor},
    [OPC_IINC]          = {"iinc", 2, 0, op_iinc},
    
    /* Conversions */
    [OPC_I2L]           = {"i2l", 0, 1, op_convert},
    [OPC_I2F]           = {"i2f", 0, 0, op_convert},
    [OPC_I2D]           = {"i2d", 0, 1, op_convert},
    [OPC_L2I]           = {"l2i", 0, -1, op_convert},
    [OPC_L2F]           = {"l2f", 0, -1, op_convert},
    [OPC_L2D]           = {"l2d", 0, 0, op_convert},
    [OPC_F2I]           = {"f2i", 0, 0, op_convert},
    [OPC_F2L]           = {"f2l", 0, 1, op_convert},
    [OPC_F2D]           = {"f2d", 0, 1, op_convert},
    [OPC_D2I]           = {"d2i", 0, -1, op_convert},
    [OPC_D2L]           = {"d2l", 0, 0, op_convert},
    [OPC_D2F]           = {"d2f", 0, -1, op_convert},
    [OPC_I2B]           = {"i2b", 0, 0, op_convert},
    [OPC_I2C]           = {"i2c", 0, 0, op_convert},
    [OPC_I2S]           = {"i2s", 0, 0, op_convert},
    
    /* Comparisons */
    [OPC_LCMP]          = {"lcmp", 0, -3, op_lcmp},
    [OPC_FCMPL]         = {"fcmpl", 0, -1, op_fcmpl},
    [OPC_FCMPG]         = {"fcmpg", 0, -1, op_fcmpg},
    [OPC_DCMPL]         = {"dcmpl", 0, -3, op_dcmpl},
    [OPC_DCMPG]         = {"dcmpg", 0, -3, op_dcmpg},
    
    /* Branches */
    [OPC_IFEQ]          = {"ifeq", 2, -1, op_if},
    [OPC_IFNE]          = {"ifne", 2, -1, op_if},
    [OPC_IFLT]          = {"iflt", 2, -1, op_if},
    [OPC_IFGE]          = {"ifge", 2, -1, op_if},
    [OPC_IFGT]          = {"ifgt", 2, -1, op_if},
    [OPC_IFLE]          = {"ifle", 2, -1, op_if},
    [OPC_IF_ICMPEQ]     = {"if_icmpeq", 2, -2, op_if_icmp},
    [OPC_IF_ICMPNE]     = {"if_icmpne", 2, -2, op_if_icmp},
    [OPC_IF_ICMPLT]     = {"if_icmplt", 2, -2, op_if_icmp},
    [OPC_IF_ICMPGE]     = {"if_icmpge", 2, -2, op_if_icmp},
    [OPC_IF_ICMPGT]     = {"if_icmpgt", 2, -2, op_if_icmp},
    [OPC_IF_ICMPLE]     = {"if_icmple", 2, -2, op_if_icmp},
    [OPC_IF_ACMPEQ]     = {"if_acmpeq", 2, -2, op_if_acmp},
    [OPC_IF_ACMPNE]     = {"if_acmpne", 2, -2, op_if_acmp},
    [OPC_GOTO]          = {"goto", 2, 0, op_goto},
    [OPC_JSR]           = {"jsr", 2, 1, op_jsr},
    [OPC_RET]           = {"ret", 1, 0, op_ret},
    [OPC_TABLESWITCH]   = {"tableswitch", 0, -1, op_tableswitch},
    [OPC_LOOKUPSWITCH]  = {"lookupswitch", 0, -1, op_lookupswitch},
    
    /* Returns */
    [OPC_IRETURN]       = {"ireturn", 0, -1, op_ireturn},
    [OPC_LRETURN]       = {"lreturn", 0, -2, op_lreturn},
    [OPC_FRETURN]       = {"freturn", 0, -1, op_freturn},
    [OPC_DRETURN]       = {"dreturn", 0, -2, op_dreturn},
    [OPC_ARETURN]       = {"areturn", 0, -1, op_areturn},
    [OPC_RETURN]        = {"return", 0, 0, op_return},
    
    /* Field access */
    [OPC_GETSTATIC]     = {"getstatic", 2, 1, op_getstatic},
    [OPC_PUTSTATIC]     = {"putstatic", 2, -1, op_putstatic},
    [OPC_GETFIELD]      = {"getfield", 2, 0, op_getfield},
    [OPC_PUTFIELD]      = {"putfield", 2, -2, op_putfield},
    
    /* Method invocation */
    [OPC_INVOKEVIRTUAL]   = {"invokevirtual", 2, 0, op_invokevirtual},
    [OPC_INVOKESPECIAL]   = {"invokespecial", 2, 0, op_invokespecial},
    [OPC_INVOKESTATIC]    = {"invokestatic", 2, 0, op_invokestatic},
    [OPC_INVOKEINTERFACE] = {"invokeinterface", 4, 0, op_invokeinterface},
    
    /* Object creation */
    [OPC_NEW]             = {"new", 2, 1, op_new},
    [OPC_NEWARRAY]        = {"newarray", 1, 0, op_newarray},
    [OPC_ANEWARRAY]       = {"anewarray", 2, 0, op_anewarray},
    [OPC_ARRAYLENGTH]     = {"arraylength", 0, 1, op_arraylength},
    [OPC_ATHROW]          = {"athrow", 0, 0, op_athrow},
    [OPC_CHECKCAST]       = {"checkcast", 2, 0, op_checkcast},
    [OPC_INSTANCEOF]      = {"instanceof", 2, 0, op_instanceof},
    [OPC_MONITORENTER]    = {"monitorenter", 0, -1, op_monitorenter},
    [OPC_MONITOREXIT]     = {"monitorexit", 0, -1, op_monitorexit},
    
    /* Extended */
    [OPC_WIDE]            = {"wide", 0, 0, op_wide},
    [OPC_MULTIANEWARRAY]  = {"multianewarray", 3, 1, op_multianewarray},
    [OPC_IFNULL]          = {"ifnull", 2, -1, op_ifnull},
    [OPC_IFNONNULL]       = {"ifnonnull", 2, -1, op_ifnonnull},
    [OPC_GOTO_W]          = {"goto_w", 4, 0, op_goto_w},
    [OPC_JSR_W]           = {"jsr_w", 4, 1, op_jsr_w},
};

/* Initialize opcodes */
void opcodes_init(void) {
    /* Opcode table is statically initialized */
}

/* Macro helpers */
#define FETCH_U1(frame) ((frame)->code[(frame)->pc++])
#define FETCH_U2(frame) (frame->pc += 2, jvm_read_u16(frame->code + frame->pc - 2))
#define FETCH_S1(frame) ((int8_t)FETCH_U1(frame))
#define FETCH_S2(frame) ((int16_t)FETCH_U2(frame))

/* Stack operations - bounds checking ON by default for correctness.
 * Disabled only when OPT_NO_BOUNDS_CHECK is defined (m17 release build).
 */
#if J2ME_DEBUG
/* Debug: full bounds checking */
#define PUSH(frame, val) do { \
    if ((frame)->stack_top + 1 >= (int)(frame)->max_stack) { \
        ERROR_LOG("[EXEC] Stack overflow in %s: top=%d, max=%d", \
                (frame)->method->name, (frame)->stack_top + 1, (frame)->max_stack); \
        return -1; \
    } \
    (frame)->stack[++(frame)->stack_top] = (val); \
} while(0)

#define PUSH2(frame, val) do { \
    if ((frame)->stack_top + 2 > (int)(frame)->max_stack) { \
        ERROR_LOG("[EXEC] Stack overflow (2 slots) in %s: top=%d, max=%d", \
                (frame)->method->name, (frame)->stack_top + 2, (frame)->max_stack); \
        return -1; \
    } \
    (frame)->stack[++(frame)->stack_top] = (val); \
    (frame)->stack[++(frame)->stack_top] = (val); \
} while(0)

static inline JavaValue safe_pop(JavaFrame* frame) {
    if (frame->stack_top < 0) {
        const char* cls = frame->clazz && frame->clazz->class_name ? frame->clazz->class_name : "?";
        const char* mth = frame->method && frame->method->name ? frame->method->name : "?";
        const char* desc = frame->method && frame->method->descriptor ? frame->method->descriptor : "";
        LOG_SAFE("[EXEC-UNDERFLOW] class=%s method=%s%s PC=%d stack_top=%d max_stack=%d max_locals=%d\n",
                cls, mth, desc, frame->pc, frame->stack_top, frame->max_stack, frame->max_locals);
        JavaValue v = { .i = 0 };
        return v;
    }
    return frame->stack[frame->stack_top--];
}

#define POP(frame) safe_pop(frame)
#define POP_VOID(frame) safe_pop(frame)   /* value still checked/validated */
#define PEEK(frame) ((frame)->stack[(frame)->stack_top])

static inline JavaValue* safe_local_ptr(JavaFrame* frame, uint16_t idx) {
    if (idx >= frame->max_locals) {
        ERROR_LOG("[EXEC] Local variable out of bounds in %s: index=%d, max_locals=%d",
                frame->method->name, idx, frame->max_locals);
        return NULL;
    }
    return &frame->locals[idx];
}

static inline int load_local(JavaFrame* frame, uint16_t idx, JavaValue* result) {
    if (idx >= frame->max_locals) {
        ERROR_LOG("[EXEC] Local variable out of bounds in %s: index=%d, max_locals=%d",
                frame->method->name, idx, frame->max_locals);
        return -1;
    }
    *result = frame->locals[idx];
    return 0;
}

static inline int store_local(JavaFrame* frame, uint16_t idx, JavaValue value) {
    if (idx >= frame->max_locals) {
        ERROR_LOG("[EXEC] Local variable out of bounds in %s: index=%d, max_locals=%d",
                frame->method->name, idx, frame->max_locals);
        return -1;
    }
    frame->locals[idx] = value;
    return 0;
}
#else
/* Release: no bounds checking for maximum performance */
#define PUSH(frame, val) do { (frame)->stack[++(frame)->stack_top] = (val); } while(0)
#define PUSH2(frame, val) do { \
    (frame)->stack[++(frame)->stack_top] = (val); \
    (frame)->stack[++(frame)->stack_top] = (val); \
} while(0)
#define POP(frame) ((frame)->stack[(frame)->stack_top--])
/* Statement-only pop (value discarded) — avoids -Wunused-value on release build */
#define POP_VOID(frame) do { (void)(frame)->stack[(frame)->stack_top]; (frame)->stack_top--; } while(0)
#define PEEK(frame) ((frame)->stack[(frame)->stack_top])
#define safe_pop(frame) POP(frame)
#define safe_local_ptr(frame, idx) (&((frame)->locals[idx]))
#define load_local(frame, idx, result) (*(result) = (frame)->locals[idx], 0)
#define store_local(frame, idx, value) ((frame)->locals[idx] = (value), 0)
#endif

/* v18 FIX: Number of Java-level arguments (J/D count as ONE). */
static int count_java_args(const char* descriptor) {
    if (!descriptor) return 0;
    int count = 0;
    const char* p = descriptor;
    if (*p == '(') p++;
    while (*p && *p != ')') {
        switch (*p) {
            case 'J': case 'D':
                count++; p++;
                break;
            case 'L':
                count++;
                while (*p && *p != ';') p++;
                if (*p == ';') p++;
                break;
            case '[':
                count++;
                while (*p == '[') p++;
                if (*p == 'L') { while (*p && *p != ';') p++; if (*p == ';') p++; }
                else if (*p) p++;
                break;
            default:
                count++; p++;
                break;
        }
    }
    return count;
}

/* v18 FIX: Pop the argument slots for `descriptor` off the frame stack (the
 * objectref first when include_this - it sits BELOW the arguments) and
 * fill args[] COMPACTLY. args must have room for
 * count_java_args(descriptor) + (include_this ? 1 : 0) entries.
 * For J/D both stack slots hold the same full JavaValue (the push-twice
 * 2-slot convention); the low slot becomes the single args[] entry.
 * Returns the number of entries filled, or -1 on OOM. */
static int pop_args_compact(JavaFrame* frame, const char* descriptor,
                            int include_this, JavaValue* args) {
    int stack_slots = count_args(descriptor);
    JavaValue* sv = (JavaValue*)malloc((stack_slots > 0 ? stack_slots : 1)
                                       * sizeof(JavaValue));
    if (!sv) return -1;

    for (int i = stack_slots - 1; i >= 0; i--) {
        sv[i] = POP(frame);
    }

    int ai = 0;
    if (include_this) {
        args[ai++] = POP(frame);
    }

    const char* p = descriptor;
    if (*p == '(') p++;
    int si = 0;
    while (*p && *p != ')') {
        switch (*p) {
            case 'J': case 'D':
                args[ai++] = sv[si];
                si += 2;
                p++;
                break;
            case 'L':
                args[ai++] = sv[si++];
                while (*p && *p != ';') p++;
                if (*p == ';') p++;
                break;
            case '[':
                args[ai++] = sv[si++];
                while (*p == '[') p++;
                if (*p == 'L') { while (*p && *p != ';') p++; if (*p == ';') p++; }
                else if (*p) p++;
                break;
            default:
                args[ai++] = sv[si++];
                p++;
                break;
        }
    }

    free(sv);
    return ai;
}

/* Helper to calculate instance field slot index */
__attribute__((unused))
static int get_instance_field_slot(JavaClass* clazz, int field_index) {
    int slot = 0;
    
    /* Add superclass instance size offset */
    if (clazz->super_class) {
        /* BUG FIX: instance_size includes ObjectHeader, must subtract it
         * to get the actual number of field slots from the superclass. */
        slot = (clazz->super_class->instance_size - sizeof(ObjectHeader)) / sizeof(JavaValue);
    }
    
    /* Count instance fields preceding the target field */
    for (int i = 0; i < field_index; i++) {
        if (!(clazz->fields[i].access_flags & ACC_STATIC)) {
            slot++;
            /* Long and double occupy 2 slots */
            if (clazz->fields[i].descriptor && 
                (clazz->fields[i].descriptor[0] == 'J' || clazz->fields[i].descriptor[0] == 'D')) {
                slot++;
            }
        }
    }
    return slot;
}

/* === CRITICAL FIX: Find field in class hierarchy ===
 * Searches for a field by name in the class and all superclasses.
 * Returns the class where the field was found and sets *out_field_index.
 * Returns NULL if field not found.
 */
typedef struct {
    JavaClass* defining_class;  /* Class where field is defined */
    int field_index;            /* Index in defining_class->fields[] */
    int slot;                   /* Calculated slot in object instance */
} FieldLookupResult;

/* Helper: count instance fields in a class (excluding static) */
static int count_instance_fields(JavaClass* clazz) {
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

/* Helper: count instance fields up to (but not including) field_index */
static int count_instance_fields_before(JavaClass* clazz, int field_index) {
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
static int build_hierarchy(JavaClass* obj_class, JavaClass** hierarchy, int max_depth) {
    int depth = 0;
    JavaClass* c = obj_class;
    
    /* Walk up to Object */
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

static FieldLookupResult find_field_in_hierarchy(JavaClass* obj_class, 
                                                   const char* field_name, 
                                                   const char* descriptor) {
    FieldLookupResult result = { NULL, -1, -1 };
    
    /* Build hierarchy from Object to obj_class */
    JavaClass* hierarchy[64];
    int depth = build_hierarchy(obj_class, hierarchy, 64);
    
    /* Search the hierarchy (obj_class FIRST, then its superclasses).
     * v18 FIX (JVMS 5.4.3.2): field resolution starts at the class being
     * searched and walks UP the superclass chain. The old Object-first
     * order made a shadowing field in a superclass win over the class's
     * own field: putfield ClsDerived.fld (slot 1) overwrote the inherited
     * ClsBase.fld (slot 0), corrupting both. */
    for (int h = depth - 1; h >= 0; h--) {
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
                
                /* Calculate slot: sum all instance fields in classes before this one,
                 * plus instance fields before this field in current class */
                result.slot = 0;
                
                /* Add fields from all classes above current_class in hierarchy */
                for (int j = 0; j < h; j++) {
                    result.slot += count_instance_fields(hierarchy[j]);
                }
                
                /* Add fields before this one in current_class */
                result.slot += count_instance_fields_before(current_class, i);
                
                return result;
            }
        }
    }
    
    return result;
}

/* Macros for backward compatibility with bounds checking */
#define LOCAL(frame, idx) (*safe_local_ptr(frame, idx))

/* Basic opcodes */
int op_nop(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread; (void)frame;
    return 0;
}

int op_aconst_null(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    JavaValue v = { .ref = NULL };
    PUSH(frame, v);
    return 0;
}

int op_iconst(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    uint8_t opcode = frame->code[frame->pc - 1];
    jint value = opcode - OPC_ICONST_0;  /* -1 to 5 */
    JavaValue v = { .i = value };
    PUSH(frame, v);
    return 0;
}

int op_lconst(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    uint8_t opcode = frame->code[frame->pc - 1];
    jlong value = opcode - OPC_LCONST_0;  /* 0 or 1 */
    
    /* ИСПРАВЛЕНО: Long занимает 2 слота стека по JVM Spec.
     * Оба слота содержат одно и то же значение (дублирование для учета стека).
     * При чтении используется только первый слот.
     * Ранее high = { .raw = 0 } было неправильно - теперь оба слота содержат value.
     */
    JavaValue v = { .j = value };
    
    PUSH(frame, v);
    PUSH(frame, v);  /* Second slot for stack accounting */
    return 0;
}

int op_fconst(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    uint8_t opcode = frame->code[frame->pc - 1];
    jfloat value = (jfloat)(opcode - OPC_FCONST_0);  /* 0.0, 1.0, or 2.0 */
    JavaValue v = { .f = value };
    PUSH(frame, v);
    return 0;
}

int op_dconst(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    uint8_t opcode = frame->code[frame->pc - 1];
    jdouble value = (jdouble)(opcode - OPC_DCONST_0);  /* 0.0 or 1.0 */
    
    /* ИСПРАВЛЕНО: Double занимает 2 слота стека по JVM Spec.
     * Оба слота содержат одно и то же значение (дублирование для учета стека).
     * При чтении используется только первый слот.
     * Ранее high = { .raw = 0 } было неправильно - теперь оба слота содержат value.
     */
    JavaValue v = { .d = value };
    
    PUSH(frame, v);
    PUSH(frame, v);  /* Second slot for stack accounting */
    return 0;
}

int op_bipush(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    int8_t value = FETCH_S1(frame);
    JavaValue v = { .i = (jint)value };
    PUSH(frame, v);
    return 0;
}

int op_sipush(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    int16_t value = FETCH_S2(frame);
    JavaValue v = { .i = (jint)value };
    PUSH(frame, v);
    return 0;
}

int op_ldc(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)thread;
    uint8_t index = FETCH_U1(frame);
    JavaClass* clazz = frame->clazz;
    
    /* ИСПРАВЛЕНО: NULL check для constant_pool */
    if (!clazz || !clazz->constant_pool) {
        LOG_SAFE("[JVM] ERROR: ldc - NULL class or constant pool\n");
        return -1;
    }
    
    /* ИСПРАВЛЕНО: Bounds checking для constant pool */
    if (index == 0 || index >= clazz->constant_pool_count) {
        LOG_SAFE("[JVM] ERROR: ldc - invalid constant pool index %u (count=%u)\n",
                index, clazz->constant_pool_count);
        return -1;
    }
    
    ConstantPoolEntry* entry = &clazz->constant_pool[index];
    
    JavaValue v = { .raw = 0 };
    
    switch (entry->tag) {
        case CONSTANT_Integer:
            v.i = entry->info.integer.value;
            break;
        case CONSTANT_Float:
            v.f = entry->info.float_val.value;
            break;
        case CONSTANT_String: {
            const char* str = classfile_get_utf8(clazz, entry->info.string_info.string_index);
            /* ИСПРАВЛЕНО: NULL check для строки */
            if (!str) {
                LOG_SAFE("[JVM] ERROR: ldc - failed to get UTF8 string at index %u\n",
                        entry->info.string_info.string_index);
                return -1;
            }
            /* Optimization: check intern pool first to avoid allocating duplicate strings */
            jsize str_len = (jsize)strlen(str);
            JavaString* interned = native_intern_find_by_utf8(str, str_len);
            if (interned) {
                v.ref = interned;
            } else {
                JavaString* new_str = jvm_new_string(jvm, str);
                if (!new_str) {
                    /* v34.2 FIX (VmTest: "OPCODE FAIL ... WITHOUT exception" +
                     * endless repaint-timer OOM loop): when the heap is down to
                     * its last bytes, jvm_new_string() fails and this path used
                     * to plain return -1 - NO pending exception. execute_method
                     * then treats it as a silent method abort: MemTests.run (and
                     * every caller up the chain) died without a Throwable, the
                     * test engine never advanced, and the ConsoleCanvas repaint
                     * timer kept re-entering paint() -> ldc -> OOM forever
                     * (plus a GC-DIAG2 dump per failure). JVMS: ldc of a
                     * CONSTANT_String under memory exhaustion must throw
                     * OutOfMemoryError - a CATCHABLE condition. Route the
                     * failure through the standard OOM thrower: it allocates a
                     * fresh OutOfMemoryError from the v19 emergency reserve and
                     * falls back to the pre-allocated singleton when even the
                     * reserve is gone. MemTests catches it and the suite
                     * continues. */
                    LOG_SAFE("[JVM] ERROR: ldc - failed to create String object (throwing OutOfMemoryError)\n");
                    native_throw_oome(jvm, thread);
                    return -1;
                }
                v.ref = native_intern_string(jvm, new_str);
            }
            break;
        }
        case CONSTANT_Class: {
            const char* name = classfile_get_utf8(clazz, entry->info.class_info.name_index);
            /* ИСПРАВЛЕНО: NULL check для имени класса */
            if (!name) {
                LOG_SAFE("[JVM] ERROR: ldc - failed to get class name at index %u\n",
                        entry->info.class_info.name_index);
                return -1;
            }
            v.ref = jvm_load_class(jvm, name);
            break;
        }
        default:
            LOG_SAFE("[JVM] ERROR: ldc - unsupported constant pool tag %d at index %u\n",
                    entry->tag, index);
            return -1;
    }
    
    PUSH(frame, v);
    return 0;
}

int op_ldc_w(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)thread;
    uint16_t index = FETCH_U2(frame);
    JavaClass* clazz = frame->clazz;
    
    /* ИСПРАВЛЕНО: NULL check для constant_pool */
    if (!clazz || !clazz->constant_pool) {
        LOG_SAFE("[JVM] ERROR: ldc_w - NULL class or constant pool\n");
        return -1;
    }
    
    /* ИСПРАВЛЕНО: Bounds checking для constant pool */
    if (index == 0 || index >= clazz->constant_pool_count) {
        LOG_SAFE("[JVM] ERROR: ldc_w - invalid constant pool index %u (count=%u)\n",
                index, clazz->constant_pool_count);
        return -1;
    }
    
    ConstantPoolEntry* entry = &clazz->constant_pool[index];
    
    JavaValue v = { .raw = 0 };
    
    switch (entry->tag) {
        case CONSTANT_Integer:
            v.i = entry->info.integer.value;
            break;
        case CONSTANT_Float:
            v.f = entry->info.float_val.value;
            break;
        case CONSTANT_String: {
            const char* str = classfile_get_utf8(clazz, entry->info.string_info.string_index);
            if (!str) {
                LOG_SAFE("[JVM] ERROR: ldc_w - failed to get UTF8 string at index %u\n",
                        entry->info.string_info.string_index);
                return -1;
            }
            /* Optimization: check intern pool first to avoid allocating duplicate strings */
            jsize str_len = (jsize)strlen(str);
            JavaString* interned = native_intern_find_by_utf8(str, str_len);
            if (interned) {
                v.ref = interned;
            } else {
                JavaString* new_str = jvm_new_string(jvm, str);
                if (!new_str) {
                    /* v34.2 FIX: same as ldc - throw catchable OutOfMemoryError
                     * instead of a silent -1 (see the ldc comment above). */
                    LOG_SAFE("[JVM] ERROR: ldc_w - failed to create String object (throwing OutOfMemoryError)\n");
                    native_throw_oome(jvm, thread);
                    return -1;
                }
                v.ref = native_intern_string(jvm, new_str);
            }
            break;
        }
        case CONSTANT_Class: {
            /* ИСПРАВЛЕНО: Добавлена обработка CONSTANT_Class (для ldc Class.class) */
            const char* name = classfile_get_utf8(clazz, entry->info.class_info.name_index);
            if (!name) {
                LOG_SAFE("[JVM] ERROR: ldc_w - failed to get class name at index %u\n",
                        entry->info.class_info.name_index);
                return -1;
            }
            v.ref = jvm_load_class(jvm, name);
            break;
        }
        default:
            LOG_SAFE("[JVM] ERROR: ldc_w - unsupported constant pool tag %d at index %u\n",
                    entry->tag, index);
            return -1;
    }
    
    PUSH(frame, v);
    return 0;
}

int op_ldc2_w(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm;
    (void)thread;
    uint16_t index = FETCH_U2(frame);
    JavaClass* clazz = frame->clazz;
    
    /* ИСПРАВЛЕНО: NULL check для constant_pool */
    if (!clazz || !clazz->constant_pool) {
        LOG_SAFE("[JVM] ERROR: ldc2_w - NULL class or constant pool\n");
        return -1;
    }
    
    /* ИСПРАВЛЕНО: Bounds checking для constant pool */
    if (index == 0 || index >= clazz->constant_pool_count) {
        LOG_SAFE("[JVM] ERROR: ldc2_w - invalid constant pool index %u (count=%u)\n",
                index, clazz->constant_pool_count);
        return -1;
    }
    
    ConstantPoolEntry* entry = &clazz->constant_pool[index];
    
    JavaValue v = { .raw = 0 };
    
    switch (entry->tag) {
        case CONSTANT_Long:
            v.j = entry->info.long_val.value;
            break;
        case CONSTANT_Double:
            v.d = entry->info.double_val.value;
            break;
        default:
            LOG_SAFE("[JVM] ERROR: ldc2_w - unsupported constant pool tag %d at index %u (expected Long/Double)\n",
                    entry->tag, index);
            return -1;
    }
    
    /* Long/double occupy 2 stack slots */
    PUSH(frame, v);
    /* Push a second slot (the value is already in v.j/v.d, this is just for stack accounting) */
    PUSH(frame, v);
    return 0;
}

/* Load opcodes */
int op_iload(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    uint8_t index = FETCH_U1(frame);
    JavaValue v;
    if (load_local(frame, index, &v) != 0) {
        return -1;
    }
    PUSH(frame, v);
    return 0;
}

int op_lload(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    uint8_t index = FETCH_U1(frame);
    JavaValue v1, v2;
    if (load_local(frame, index, &v1) != 0) return -1;
    if (load_local(frame, index + 1, &v2) != 0) return -1;
    PUSH(frame, v1);
    PUSH(frame, v2);
    return 0;
}

int op_fload(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    uint8_t index = FETCH_U1(frame);
    JavaValue v;
    if (load_local(frame, index, &v) != 0) return -1;
    PUSH(frame, v);
    return 0;
}

int op_dload(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    uint8_t index = FETCH_U1(frame);
    JavaValue v1, v2;
    if (load_local(frame, index, &v1) != 0) return -1;
    if (load_local(frame, index + 1, &v2) != 0) return -1;
    PUSH(frame, v1);
    PUSH(frame, v2);
    return 0;
}

int op_aload(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    uint8_t index = FETCH_U1(frame);
    JavaValue v;
    if (load_local(frame, index, &v) != 0) {
        return -1;
    }
    PUSH(frame, v);
    return 0;
}

int op_load_0(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    JavaValue v;
    if (load_local(frame, 0, &v) != 0) return -1;
    PUSH(frame, v);
    return 0;
}

int op_load_1(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    JavaValue v;
    if (load_local(frame, 1, &v) != 0) {
        LOG_SAFE("[LOAD_1] ERROR: load_local failed! max_locals=%d\n", frame->max_locals);
        return -1;
    }
    PUSH(frame, v);
    return 0;
}

int op_load_2(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    JavaValue v;
    if (load_local(frame, 2, &v) != 0) return -1;
    PUSH(frame, v);
    return 0;
}

int op_load_3(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    JavaValue v;
    if (load_local(frame, 3, &v) != 0) return -1;
    PUSH(frame, v);
    return 0;
}

/* Long/Double load opcodes - these need 2 stack slots */
int op_lload_0(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    JavaValue v0, v1;
    if (load_local(frame, 0, &v0) != 0) return -1;
    if (load_local(frame, 1, &v1) != 0) return -1;
    PUSH(frame, v0);
    PUSH(frame, v1);
    return 0;
}

int op_lload_1(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    JavaValue v1, v2;
    if (load_local(frame, 1, &v1) != 0) return -1;
    if (load_local(frame, 2, &v2) != 0) return -1;
    PUSH(frame, v1);
    PUSH(frame, v2);
    return 0;
}

int op_lload_2(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    JavaValue v2, v3;
    if (load_local(frame, 2, &v2) != 0) return -1;
    if (load_local(frame, 3, &v3) != 0) return -1;
    PUSH(frame, v2);
    PUSH(frame, v3);
    return 0;
}

int op_lload_3(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    JavaValue v3, v4;
    if (load_local(frame, 3, &v3) != 0) return -1;
    if (load_local(frame, 4, &v4) != 0) return -1;
    PUSH(frame, v3);
    PUSH(frame, v4);
    return 0;
}

/* dload_X same as lload_X */
int op_dload_0(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    JavaValue v0, v1;
    if (load_local(frame, 0, &v0) != 0) return -1;
    if (load_local(frame, 1, &v1) != 0) return -1;
    PUSH(frame, v0);
    PUSH(frame, v1);
    return 0;
}

int op_dload_1(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    JavaValue v1, v2;
    if (load_local(frame, 1, &v1) != 0) return -1;
    if (load_local(frame, 2, &v2) != 0) return -1;
    PUSH(frame, v1);
    PUSH(frame, v2);
    return 0;
}

int op_dload_2(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    JavaValue v2, v3;
    if (load_local(frame, 2, &v2) != 0) return -1;
    if (load_local(frame, 3, &v3) != 0) return -1;
    PUSH(frame, v2);
    PUSH(frame, v3);
    return 0;
}

int op_dload_3(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    JavaValue v3, v4;
    if (load_local(frame, 3, &v3) != 0) return -1;
    if (load_local(frame, 4, &v4) != 0) return -1;
    PUSH(frame, v3);
    PUSH(frame, v4);
    return 0;
}

/* Array load */
int op_array_load(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm;
    uint8_t opcode = frame->code[frame->pc - 1];
    jint index = POP(frame).i;
    JavaArray* array = (JavaArray*)POP(frame).ref;
    
    if (!array) {
        /* DRM bypass: return safe defaults for null array access in DRM classes */
        const char* caller_class = frame ? frame->clazz->class_name : NULL;
        if (caller_class && drm_is_drm_class(caller_class)) {
            JavaValue v = { .i = 0 };
            PUSH(frame, v);
            if (opcode == OPC_LALOAD || opcode == OPC_DALOAD) {
                PUSH(frame, v);
            }
            return 0;
        }
        native_throw_npe(jvm, thread);
        return -1;
    }
    
    if (index < 0 || index >= array->length) {
        /* v34.8 DIAG: index+len+caller for game debugging (throttled) */
        {
            static int aioobe_diag_count = 0;
            if (aioobe_diag_count < 40) {
                aioobe_diag_count++;
                const char* cc = (frame && frame->clazz && frame->clazz->class_name)
                    ? frame->clazz->class_name : "?";
                const char* cm = (frame && frame->method && frame->method->name)
                    ? frame->method->name : "?";
                fprintf(stderr, "[AIOOBE-DIAG] LOAD op=%s index=%d len=%d at %s.%s PC=%d\n",
                        (opcode == OPC_AALOAD) ? "aaload" : (opcode == OPC_BALOAD) ? "baload"
                        : (opcode == OPC_IALOAD) ? "iaload" : (opcode == OPC_SALOAD) ? "saload"
                        : (opcode == OPC_CALOAD) ? "caload" : (opcode == OPC_LALOAD) ? "laload"
                        : (opcode == OPC_FALOAD) ? "faload" : "daload",
                        index, array->length, cc, cm,
                        frame ? (int)frame->throwing_pc : -1);
            }
        }
        native_throw_aioobe(jvm, thread, index);
        return -1;
    }
    
    JavaValue v = array_get(array, index);
    
    /* Log AALOAD operations for debugging */
    if (opcode == OPC_AALOAD) {
        LOG_OPCODE("[AALOAD] array=%p, index=%d, value=%p, len=%d\n",
                (void*)array, index, v.ref, array->length);
    }
    
    PUSH(frame, v);
    
    /* Long and double need 2 stack slots */
    if (opcode == OPC_LALOAD || opcode == OPC_DALOAD) {
        PUSH(frame, v);
    }
    
    return 0;
}

/* Store opcodes */
int op_store(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    uint8_t index = FETCH_U1(frame);
    JavaValue v = POP(frame);
    if (store_local(frame, index, v) != 0) {
        LOG_SAFE("[EXEC] op_store: failed to store at index %d (max_locals=%d)\n", 
                index, frame->max_locals);
        return -1;
    }
    return 0;
}

/* Long/Double store - needs to pop 2 slots and store in 2 locals */
int op_lstore(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    uint8_t index = FETCH_U1(frame);
    JavaValue high = POP(frame);  /* High word */
    JavaValue low = POP(frame);   /* Low word */
    if (store_local(frame, index, low) != 0) return -1;
    if (store_local(frame, index + 1, high) != 0) return -1;
    return 0;
}

int op_dstore(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    return op_lstore(jvm, thread, frame);  /* Same implementation */
}

int op_store_0(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    JavaValue v = POP(frame);
    if (store_local(frame, 0, v) != 0) return -1;
    return 0;
}

int op_store_1(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    JavaValue v = POP(frame);
    if (store_local(frame, 1, v) != 0) return -1;
    return 0;
}

int op_store_2(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    JavaValue v = POP(frame);
    if (store_local(frame, 2, v) != 0) return -1;
    return 0;
}

int op_store_3(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    JavaValue v = POP(frame);
    if (store_local(frame, 3, v) != 0) return -1;
    return 0;
}

/* Long/Double store opcodes - these need 2 stack slots */
int op_lstore_0(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    JavaValue v1 = POP(frame);  /* Pop high word first */
    JavaValue v0 = POP(frame);  /* Pop low word second */
    if (store_local(frame, 1, v1) != 0) return -1;
    if (store_local(frame, 0, v0) != 0) return -1;
    return 0;
}

int op_lstore_1(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    JavaValue v2 = POP(frame);
    JavaValue v1 = POP(frame);
    if (store_local(frame, 2, v2) != 0) return -1;
    if (store_local(frame, 1, v1) != 0) return -1;
    return 0;
}

int op_lstore_2(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    JavaValue v3 = POP(frame);
    JavaValue v2 = POP(frame);
    if (store_local(frame, 3, v3) != 0) return -1;
    if (store_local(frame, 2, v2) != 0) return -1;
    return 0;
}

int op_lstore_3(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    JavaValue v4 = POP(frame);
    JavaValue v3 = POP(frame);
    if (store_local(frame, 4, v4) != 0) return -1;
    if (store_local(frame, 3, v3) != 0) return -1;
    return 0;
}

/* dstore_X same as lstore_X */
int op_dstore_0(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    JavaValue v1 = POP(frame);
    JavaValue v0 = POP(frame);
    if (store_local(frame, 1, v1) != 0) return -1;
    if (store_local(frame, 0, v0) != 0) return -1;
    return 0;
}

int op_dstore_1(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    JavaValue v2 = POP(frame);
    JavaValue v1 = POP(frame);
    if (store_local(frame, 2, v2) != 0) return -1;
    if (store_local(frame, 1, v1) != 0) return -1;
    return 0;
}

int op_dstore_2(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    JavaValue v3 = POP(frame);
    JavaValue v2 = POP(frame);
    if (store_local(frame, 3, v3) != 0) return -1;
    if (store_local(frame, 2, v2) != 0) return -1;
    return 0;
}

int op_dstore_3(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    JavaValue v4 = POP(frame);
    JavaValue v3 = POP(frame);
    if (store_local(frame, 4, v4) != 0) return -1;
    if (store_local(frame, 3, v3) != 0) return -1;
    return 0;
}

/* Array store */
int op_array_store(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    uint8_t opcode = frame->code[frame->pc - 1];
    
    
    /* Long and double need to pop 2 stack slots for the value */
    JavaValue value;
    if (opcode == OPC_LASTORE || opcode == OPC_DASTORE) {
        POP_VOID(frame); /* discard high slot */
        value = POP(frame);
    } else {
        value = POP(frame);
    }
    
    jint index = POP(frame).i;
    JavaArray* array = (JavaArray*)POP(frame).ref;

    /* Debug: log aastore for object arrays */
    if (opcode == OPC_AASTORE) {
        LOG_OPCODE("[AASTORE] array=%p, index=%d, value=%p, array_len=%d\n",
                (void*)array, index, value.ref, array ? array->length : -1);
        
        /* CRITICAL: Validate the value is a valid heap object before storing */
        if (value.ref != NULL && !is_heap_ptr_check(value.ref)) {
            if (g_j2me_runtime_debug) LOG_SAFE("[AASTORE] CRITICAL: Storing INVALID pointer %p (not in heap %p-%p)\n",
                    value.ref, g_heap_start, g_heap_end);
            /* Still store it but log the issue */
        }
    }
    
    
    if (!array || !is_heap_ptr_check((void*)array)) {
        /* v34.32 DIAG: garbage array ref on the operand stack (misaligned
         * pops upstream) — report and throw NPE instead of crashing. */
        if (!array) {
            /* DRM bypass: silently ignore array store on null in DRM classes */
            const char* caller_class = frame ? frame->clazz->class_name : NULL;
            if (caller_class && drm_is_drm_class(caller_class)) {
                return 0;
            }
        } else {
            fprintf(stderr, "[AARRAY-DIAG] garbage array=%p (heap %p-%p) op=%02x at %s.%s PC=%d\n",
                    (void*)array, g_heap_start, g_heap_end, opcode,
                    frame&&frame->clazz&&frame->clazz->class_name?frame->clazz->class_name:"?",
                    frame&&frame->method&&frame->method->name?frame->method->name:"?",
                    (int)frame->pc);
        }
        native_throw_npe(jvm, thread);
        return -1;
    }
    
    if (index < 0 || index >= array->length) {
        /* v34.8 DIAG: index+len+caller for game debugging (throttled) */
        {
            static int aioobe_store_diag_count = 0;
            if (aioobe_store_diag_count < 40) {
                aioobe_store_diag_count++;
                const char* cc = (frame && frame->clazz && frame->clazz->class_name)
                    ? frame->clazz->class_name : "?";
                const char* cm = (frame && frame->method && frame->method->name)
                    ? frame->method->name : "?";
                fprintf(stderr, "[AIOOBE-DIAG] STORE op=%s index=%d len=%d at %s.%s PC=%d\n",
                        (opcode == OPC_AASTORE) ? "aastore" : (opcode == OPC_BASTORE) ? "bastore"
                        : (opcode == OPC_IASTORE) ? "iastore" : (opcode == OPC_SASTORE) ? "sastore"
                        : (opcode == OPC_CASTORE) ? "castore" : "xxstore",
                        index, array->length, cc, cm,
                        frame ? (int)frame->throwing_pc : -1);
            }
        }
        native_throw_aioobe(jvm, thread, index);
        return -1;
    }
    
    /* Check ArrayStoreException for AASTORE
     * NOTE: Type checking is skipped if element_class is NULL (class not loaded).
     * This is intentional — obfuscated MIDlets may have class references that
     * don't resolve correctly, but the runtime types are still compatible.
     * We also skip the check if element_class has no class_name, which can
     * happen for dynamically generated or stub classes. */
    if (opcode == OPC_AASTORE && value.ref != NULL) {
        JavaClass* element_class = array->element_class;
        
        if (element_class && element_class->class_name) {
            /* v24 FIX (Brick Breaker): element type is itself an array
             * (e.g. int[][], Object[][]) — the descriptor-based instanceof
             * check cannot fully verify nested-array covariance (element_class
             * is an array stub class, the stored value may come from a
             * different-but-compatible construction path). Permissively skip
             * the check for '['-typed element classes instead of throwing a
             * spurious ArrayStoreException that cascaded into a load hang. */
            if (element_class->class_name[0] == '[') {
                /* skip check — nested array element, store permissively */
            } else {
            JavaObject* obj = (JavaObject*)value.ref;
            /* [ARGGUARD] v36.49: мусорное значение массива не deref-им:
             * value_class=NULL -> перmissive store (как при потере класса) */
            JavaClass* value_class =
                (obj && !argguard_bad_ptr((uintptr_t)obj)) ? obj->header.clazz : NULL;
            
            /* Only check if both classes have valid names */
            if (value_class && value_class->class_name && object_instance_of(obj, element_class)) {
                /* OK — type matches */
            } else if (value_class && value_class->class_name &&
                       strcmp(element_class->class_name, "java/lang/Object") == 0) {
                /* OK — Object[] accepts anything */
            } else if (!value_class || !value_class->class_name || !element_class->class_name) {
                /* Skip check — can't verify type without class info */
            } else {
                /* Type mismatch — throw ArrayStoreException */
                native_throw_array_store_exception(jvm, thread);
                return -1;
            }
            }
        }
    }
    
    if (getenv("NOJME_TRACE_ARR")) {
        fprintf(stderr, "[ARR] store op=%02x array=%p len=%d type=%d idx=%d at %s.%s PC~%d\n",
                opcode, (void*)array, array->length, array->element_type, index,
                frame&&frame->clazz?frame->clazz->class_name:"?",
                frame&&frame->method?frame->method->name:"?",
                (int)frame->pc);
    }
    array_set(array, index, value);
    return 0;
}

/* Stack operations */
int op_pop(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    POP_VOID(frame);
    return 0;
}

int op_pop2(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    POP_VOID(frame);
    POP_VOID(frame);
    return 0;
}

int op_dup(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    JavaValue v = PEEK(frame);
    PUSH(frame, v);
    return 0;
}

int op_dup_x1(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    JavaValue v1 = POP(frame);
    JavaValue v2 = POP(frame);
    PUSH(frame, v1);
    PUSH(frame, v2);
    PUSH(frame, v1);
    return 0;
}

int op_dup_x2(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    /* JVM spec (JVMS 6.5 dup_x2): ..., v3, v2, v1 -> ..., v1, v3, v2, v1
     * FIX: previously pushed v1,v2,v3,v1 which swapped v2/v3 and corrupted
     * obfuscated code relying on correct operand-stack ordering. */
    JavaValue v1 = POP(frame);
    JavaValue v2 = POP(frame);
    JavaValue v3 = POP(frame);
    PUSH(frame, v1);
    PUSH(frame, v3);
    PUSH(frame, v2);
    PUSH(frame, v1);
    return 0;
}

int op_dup2(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    JavaValue v1 = POP(frame);
    JavaValue v2 = POP(frame);
    PUSH(frame, v2);
    PUSH(frame, v1);
    PUSH(frame, v2);
    PUSH(frame, v1);
    return 0;
}

int op_dup2_x1(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    /* JVM spec (JVMS 6.5 dup2_x1): ..., v3, v2, v1 -> ..., v2, v1, v3, v2, v1
     * FIX: was a complete misrotation (v1,v2,v3,v1,v2). */
    JavaValue v1 = POP(frame);
    JavaValue v2 = POP(frame);
    JavaValue v3 = POP(frame);
    PUSH(frame, v2);
    PUSH(frame, v1);
    PUSH(frame, v3);
    PUSH(frame, v2);
    PUSH(frame, v1);
    return 0;
}

int op_dup2_x2(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    /* JVM spec (JVMS 6.5 dup2_x2):
     * ..., v4, v3, v2, v1 -> ..., v2, v1, v4, v3, v2, v1
     * FIX: previous push order produced a mirrored, invalid layout. */
    JavaValue v1 = POP(frame);
    JavaValue v2 = POP(frame);
    JavaValue v3 = POP(frame);
    JavaValue v4 = POP(frame);
    PUSH(frame, v2);
    PUSH(frame, v1);
    PUSH(frame, v4);
    PUSH(frame, v3);
    PUSH(frame, v2);
    PUSH(frame, v1);
    return 0;
}

int op_swap(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    JavaValue v1 = POP(frame);
    JavaValue v2 = POP(frame);
    PUSH(frame, v1);
    PUSH(frame, v2);
    return 0;
}

/* Arithmetic operations */
int op_add(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    uint8_t opcode = frame->code[frame->pc - 1];
    JavaValue v2, v1, result;
    
    switch (opcode) {
        case OPC_IADD: 
            v2 = POP(frame);
            v1 = POP(frame);
            result.i = v1.i + v2.i; 
            PUSH(frame, result);
            break;
            
        case OPC_LADD: 
            /* ИСПРАВЛЕНО: Long хранится в одном JavaValue (union), но занимает 2 слота стека.
             * Сначала pop high slot (фиктивный), потом low slot с реальным значением */
            POP_VOID(frame);  /* v2 high slot (фиктивный) */
            JavaValue v2_l = POP(frame);  /* v2 low slot с реальным jlong */
            POP_VOID(frame);  /* v1 high slot (фиктивный) */
            JavaValue v1_l = POP(frame);  /* v1 low slot с реальным jlong */
            
            result.j = v1_l.j + v2_l.j;
            
            /* Push результат как 2 слота (low + high для совместимости) */
            PUSH(frame, result);
            PUSH(frame, result);  /* high slot = копия low slot */
            break;
            
        case OPC_FADD: 
            v2 = POP(frame);
            v1 = POP(frame);
            result.f = v1.f + v2.f; 
            PUSH(frame, result);
            break;
            
        case OPC_DADD: 
            /* ИСПРАВЛЕНО: Double хранится в одном JavaValue (union), но занимает 2 слота стека */
            POP_VOID(frame);  /* d2 high slot (фиктивный) */
            JavaValue d2_d = POP(frame);  /* d2 low slot с реальным jdouble */
            POP_VOID(frame);  /* d1 high slot (фиктивный) */
            JavaValue d1_d = POP(frame);  /* d1 low slot с реальным jdouble */
            
            result.d = d1_d.d + d2_d.d;
            
            PUSH(frame, result);
            PUSH(frame, result);  /* high slot = копия low slot */
            break;
    }
    
    return 0;
}

int op_sub(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    uint8_t opcode = frame->code[frame->pc - 1];
    JavaValue v2, v1, result;
    
    switch (opcode) {
        case OPC_ISUB: 
            v2 = POP(frame);
            v1 = POP(frame);
            result.i = v1.i - v2.i; 
            PUSH(frame, result);
            break;
        case OPC_LSUB: 
            /* Pop 2 slots for each long */
            POP_VOID(frame); /* v2 high */
            v2 = POP(frame);
            POP_VOID(frame); /* v1 high */
            v1 = POP(frame);
            result.j = v1.j - v2.j;
            PUSH(frame, result);
            PUSH(frame, result);
            break;
        case OPC_FSUB: 
            v2 = POP(frame);
            v1 = POP(frame);
            result.f = v1.f - v2.f; 
            PUSH(frame, result);
            break;
        case OPC_DSUB: 
            /* Pop 2 slots for each double */
            POP_VOID(frame); /* v2 high */
            v2 = POP(frame);
            POP_VOID(frame); /* v1 high */
            v1 = POP(frame);
            result.d = v1.d - v2.d;
            PUSH(frame, result);
            PUSH(frame, result);
            break;
    }
    
    return 0;
}

int op_mul(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    uint8_t opcode = frame->code[frame->pc - 1];
    JavaValue v2, v1, result;
    
    switch (opcode) {
        case OPC_IMUL: 
            v2 = POP(frame);
            v1 = POP(frame);
            result.i = v1.i * v2.i; 
            PUSH(frame, result);
            break;
        case OPC_LMUL: 
            /* Pop 2 slots for each long */
            POP_VOID(frame); /* v2 high */
            v2 = POP(frame);
            POP_VOID(frame); /* v1 high */
            v1 = POP(frame);
            result.j = v1.j * v2.j;
            PUSH(frame, result);
            PUSH(frame, result);
            break;
        case OPC_FMUL: 
            v2 = POP(frame);
            v1 = POP(frame);
            result.f = v1.f * v2.f; 
            PUSH(frame, result);
            break;
        case OPC_DMUL: 
            /* Pop 2 slots for each double */
            POP_VOID(frame); /* v2 high */
            v2 = POP(frame);
            POP_VOID(frame); /* v1 high */
            v1 = POP(frame);
            result.d = v1.d * v2.d;
            PUSH(frame, result);
            PUSH(frame, result);
            break;
    }
    
    return 0;
}

int op_div(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    uint8_t opcode = frame->code[frame->pc - 1];
    JavaValue v2, v1, result;
    
    switch (opcode) {
        case OPC_IDIV: 
            v2 = POP(frame);
            v1 = POP(frame);
            if (v2.i == 0) {
                jvm_throw_by_name(jvm, "java/lang/ArithmeticException", "/ by zero");
                return -1;
            }
            /* v20 FIX (JVMS §6.5 idiv): Integer.MIN_VALUE / -1 ==
             * Integer.MIN_VALUE — the result overflows to itself and NO
             * exception is thrown. The old code raised a spurious
             * ArithmeticException on valid obfuscated bytecode. Mirrors the
             * already-fixed IREM below. */
            if (v1.i == INT32_MIN && v2.i == -1) {
                result.i = INT32_MIN;
            } else {
                result.i = v1.i / v2.i; 
            }
            PUSH(frame, result);
            break;
        case OPC_LDIV: 
            /* Pop 2 slots for each long */
            POP_VOID(frame); /* v2 high */
            v2 = POP(frame);
            POP_VOID(frame); /* v1 high */
            v1 = POP(frame);
            if (v2.j == 0LL) {
                jvm_throw_by_name(jvm, "java/lang/ArithmeticException", "/ by zero");
                return -1;
            }
            /* v20 FIX (JVMS §6.5 ldiv): Long.MIN_VALUE / -1 == Long.MIN_VALUE
             * (overflow saturates to itself, no exception). */
            if (v1.j == INT64_MIN && v2.j == -1LL) {
                result.j = INT64_MIN;
            } else {
                result.j = v1.j / v2.j;
            }
            PUSH(frame, result);
            PUSH(frame, result);
            break;
        case OPC_FDIV: 
            v2 = POP(frame);
            v1 = POP(frame);
            /* Float division by zero returns Infinity or NaN per IEEE 754 - no exception */
            result.f = v1.f / v2.f; 
            PUSH(frame, result);
            break;
        case OPC_DDIV: 
            /* Pop 2 slots for each double */
            POP_VOID(frame); /* v2 high */
            v2 = POP(frame);
            POP_VOID(frame); /* v1 high */
            v1 = POP(frame);
            /* Double division by zero returns Infinity or NaN per IEEE 754 - no exception */
            result.d = v1.d / v2.d;
            PUSH(frame, result);
            PUSH(frame, result);
            break;
    }
    
    return 0;
}

int op_rem(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    uint8_t opcode = frame->code[frame->pc - 1];
    JavaValue v2, v1, result;
    
    switch (opcode) {
        case OPC_IREM: 
            v2 = POP(frame);
            v1 = POP(frame);
            if (v2.i == 0) {
                jvm_throw_by_name(jvm, "java/lang/ArithmeticException", "/ by zero");
                return -1;
            }
            /* Java spec: MIN_VALUE % -1 = 0 (C behavior is undefined) */
            if (v1.i == INT32_MIN && v2.i == -1) {
                result.i = 0;
            } else {
                result.i = v1.i % v2.i; 
            }
            PUSH(frame, result);
            break;
        case OPC_LREM: 
            /* Pop 2 slots for each long */
            POP_VOID(frame); /* v2 high */
            v2 = POP(frame);
            POP_VOID(frame); /* v1 high */
            v1 = POP(frame);
            if (v2.j == 0LL) {
                jvm_throw_by_name(jvm, "java/lang/ArithmeticException", "/ by zero");
                return -1;
            }
            /* Java spec: MIN_VALUE % -1 = 0 (C behavior is undefined) */
            if (v1.j == INT64_MIN && v2.j == -1LL) {
                result.j = 0LL;
            } else {
                result.j = v1.j % v2.j;
            }
            PUSH(frame, result);
            PUSH(frame, result);
            break;
        case OPC_FREM: 
            v2 = POP(frame);
            v1 = POP(frame);
            /* Float modulo by zero returns NaN per IEEE 754 - no exception */
            result.f = fmodf(v1.f, v2.f); 
            PUSH(frame, result);
            break;
        case OPC_DREM: 
            /* Pop 2 slots for each double */
            POP_VOID(frame); /* v2 high */
            v2 = POP(frame);
            POP_VOID(frame); /* v1 high */
            v1 = POP(frame);
            /* Double modulo by zero returns NaN per IEEE 754 - no exception */
            result.d = fmod(v1.d, v2.d);
            PUSH(frame, result);
            PUSH(frame, result);
            break;
    }
    
    return 0;
}

int op_neg(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    uint8_t opcode = frame->code[frame->pc - 1];
    JavaValue v, result;
    
    switch (opcode) {
        case OPC_INEG: 
            v = POP(frame);
            result.i = (jint)(0u - (uint32_t)v.i); /* well-defined two's complement negation (v18) */
            PUSH(frame, result);
            break;
        case OPC_LNEG: 
            /* Pop 2 slots for long */
            POP_VOID(frame); /* high */
            v = POP(frame);
            result.j = (jlong)(0ull - (uint64_t)v.j); /* well-defined two's complement negation (v18) */
            PUSH(frame, result);
            PUSH(frame, result);
            break;
        case OPC_FNEG: 
            v = POP(frame);
            result.f = -v.f; 
            PUSH(frame, result);
            break;
        case OPC_DNEG: 
            /* Pop 2 slots for double */
            POP_VOID(frame); /* high */
            v = POP(frame);
            result.d = -v.d;
            PUSH(frame, result);
            PUSH(frame, result);
            break;
    }
    
    return 0;
}

int op_shl(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    uint8_t opcode = frame->code[frame->pc - 1];
    jint raw_shift = POP(frame).i;
    JavaValue v, result;
    
    switch (opcode) {
        case OPC_ISHL: 
            v = POP(frame);
            result.i = v.i << (raw_shift & 0x1F); 
            PUSH(frame, result);
            break;
        case OPC_LSHL: 
            POP_VOID(frame); /* high */
            v = POP(frame);
            result.j = v.j << (raw_shift & 0x3F);
            PUSH(frame, result);
            PUSH(frame, result);
            break;
    }
    
    return 0;
}

int op_shr(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    uint8_t opcode = frame->code[frame->pc - 1];
    jint raw_shift = POP(frame).i;
    JavaValue v, result;
    
    switch (opcode) {
        case OPC_ISHR: 
            v = POP(frame);
            result.i = v.i >> (raw_shift & 0x1F); 
            PUSH(frame, result);
            break;
        case OPC_LSHR: 
            POP_VOID(frame); /* high */
            v = POP(frame);
            result.j = v.j >> (raw_shift & 0x3F);
            PUSH(frame, result);
            PUSH(frame, result);
            break;
    }
    
    return 0;
}

int op_ushr(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    uint8_t opcode = frame->code[frame->pc - 1];
    jint raw_shift = POP(frame).i;
    JavaValue v, result;
    
    switch (opcode) {
        case OPC_IUSHR: 
            v = POP(frame);
            result.i = ((juint)v.i) >> (raw_shift & 0x1F); 
            PUSH(frame, result);
            break;
        case OPC_LUSHR: 
            POP_VOID(frame); /* high */
            v = POP(frame);
            result.j = ((julong)v.j) >> (raw_shift & 0x3F);
            PUSH(frame, result);
            PUSH(frame, result);
            break;
    }
    
    return 0;
}

int op_and(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    uint8_t opcode = frame->code[frame->pc - 1];
    JavaValue v2, v1, result;
    
    switch (opcode) {
        case OPC_IAND: 
            v2 = POP(frame);
            v1 = POP(frame);
            result.i = v1.i & v2.i; 
            PUSH(frame, result);
            break;
        case OPC_LAND: 
            /* Pop 2 slots for each long */
            POP_VOID(frame); /* v2 high */
            v2 = POP(frame);
            POP_VOID(frame); /* v1 high */
            v1 = POP(frame);
            result.j = v1.j & v2.j;
            PUSH(frame, result);
            PUSH(frame, result);
            break;
    }
    
    return 0;
}

int op_or(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    uint8_t opcode = frame->code[frame->pc - 1];
    JavaValue v2, v1, result;
    
    switch (opcode) {
        case OPC_IOR: 
            v2 = POP(frame);
            v1 = POP(frame);
            result.i = v1.i | v2.i; 
            PUSH(frame, result);
            break;
        case OPC_LOR: 
            /* Pop 2 slots for each long */
            POP_VOID(frame); /* v2 high */
            v2 = POP(frame);
            POP_VOID(frame); /* v1 high */
            v1 = POP(frame);
            result.j = v1.j | v2.j;
            PUSH(frame, result);
            PUSH(frame, result);
            break;
    }
    
    return 0;
}

int op_xor(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    uint8_t opcode = frame->code[frame->pc - 1];
    JavaValue v2, v1, result;
    
    switch (opcode) {
        case OPC_IXOR: 
            v2 = POP(frame);
            v1 = POP(frame);
            result.i = v1.i ^ v2.i; 
            PUSH(frame, result);
            break;
        case OPC_LXOR: 
            /* Pop 2 slots for each long */
            POP_VOID(frame); /* v2 high */
            v2 = POP(frame);
            POP_VOID(frame); /* v1 high */
            v1 = POP(frame);
            result.j = v1.j ^ v2.j;
            PUSH(frame, result);
            PUSH(frame, result);
            break;
    }
    
    return 0;
}

int op_iinc(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    uint8_t index = FETCH_U1(frame);
    int8_t constant = FETCH_S1(frame);
    JavaValue v;
    if (load_local(frame, index, &v) != 0) {
        LOG_SAFE("[EXEC] iinc: failed to load from index %d\n", index);
        return -1;
    }
    v.i += constant;
    if (store_local(frame, index, v) != 0) {
        LOG_SAFE("[EXEC] iinc: failed to store to index %d\n", index);
        return -1;
    }
    return 0;
}

/* FIX (audit J-2, v18): JVMS-compliant saturating float/double -> int/long
 * conversions. NaN -> 0; values beyond the target range clamp to MIN/MAX of
 * the target type; in-range values truncate toward zero. Avoids the UB of
 * raw C casts (platform-dependent garbage physics on NaN/overflow). */
static jint jvm_f2i(jfloat f) {
    if (isnan(f)) return 0;
    if (f >= 2147483647.0f) return INT32_MAX;   /* rounds up past max */
    if (f <= -2147483648.0f) return INT32_MIN;
    return (jint)f;
}
static jlong jvm_f2l(jfloat f) {
    if (isnan(f)) return 0;
    if (f >= 9223372036854775807.0f) return INT64_MAX;
    if (f <= -9223372036854775808.0f) return INT64_MIN;
    return (jlong)f;
}
static jint jvm_d2i(jdouble d) {
    if (isnan(d)) return 0;
    if (d >= 2147483647.0) return INT32_MAX;
    if (d <= -2147483648.0) return INT32_MIN;
    return (jint)d;
}
static jlong jvm_d2l(jdouble d) {
    if (isnan(d)) return 0;
    if (d >= 9223372036854775807.0) return INT64_MAX;
    if (d <= -9223372036854775808.0) return INT64_MIN;
    return (jlong)d;
}

/* Conversion operations */
int op_convert(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    uint8_t opcode = frame->code[frame->pc - 1];
    JavaValue v, result;
    
    switch (opcode) {
        /* int/float -> long/double: pop 1, push 2 */
        case OPC_I2L: 
            v = POP(frame);
            result.j = (jlong)v.i; 
            PUSH(frame, result);
            PUSH(frame, result);
            break;
        case OPC_I2D: 
            v = POP(frame);
            result.d = (jdouble)v.i; 
            PUSH(frame, result);
            PUSH(frame, result);
            break;
        case OPC_F2L: 
            v = POP(frame);
            result.j = jvm_f2l(v.f); /* saturating per JVMS (J-2, v18) */ 
            PUSH(frame, result);
            PUSH(frame, result);
            break;
        case OPC_F2D: 
            v = POP(frame);
            result.d = (jdouble)v.f; 
            PUSH(frame, result);
            PUSH(frame, result);
            break;
            
        /* long/double -> int/float: pop 2, push 1 */
        case OPC_L2I: 
            v = POP(frame); /* discard high slot */
            v = POP(frame);
            result.i = (jint)v.j;
            PUSH(frame, result);
            break;
        case OPC_L2F: 
            POP_VOID(frame); /* discard high slot */
            v = POP(frame);
            result.f = (jfloat)v.j; 
            PUSH(frame, result);
            break;
        case OPC_D2I: 
            POP_VOID(frame); /* discard high slot */
            v = POP(frame);
            result.i = jvm_d2i(v.d); /* saturating per JVMS (J-2, v18) */ 
            PUSH(frame, result);
            break;
        case OPC_D2F: 
            POP_VOID(frame); /* discard high slot */
            v = POP(frame);
            result.f = (jfloat)v.d; 
            PUSH(frame, result);
            break;
            
        /* long <-> double: pop 2, push 2 */
        case OPC_L2D: 
            v = POP(frame); /* high slot */
            result = POP(frame); /* low slot with the actual value */
            result.d = (jdouble)result.j; 
            PUSH(frame, result);
            PUSH(frame, result);
            break;
        case OPC_D2L: 
            v = POP(frame); /* high slot */
            result = POP(frame); /* low slot with the actual value */
            result.j = jvm_d2l(result.d); /* saturating per JVMS (J-2, v18) */ 
            PUSH(frame, result);
            PUSH(frame, result);
            break;
            
        /* int -> int: pop 1, push 1 */
        case OPC_I2F: 
            v = POP(frame);
            result.f = (jfloat)v.i; 
            PUSH(frame, result);
            break;
        case OPC_F2I: 
            v = POP(frame);
            result.i = jvm_f2i(v.f); /* saturating per JVMS (J-2, v18) */ 
            PUSH(frame, result);
            break;
        case OPC_I2B: 
            v = POP(frame);
            result.i = (jint)(int8_t)v.i; 
            PUSH(frame, result);
            break;
        case OPC_I2C: 
            v = POP(frame);
            /* CRITICAL FIX: Java char is 16-bit unsigned (0-65535), not 8-bit! */
            result.i = (jint)(uint16_t)v.i; 
            PUSH(frame, result);
            break;
        case OPC_I2S: 
            v = POP(frame);
            result.i = (jint)(int16_t)v.i; 
            PUSH(frame, result);
            break;
        default: return -1;
    }
    
    return 0;
}

/* Comparison operations */
int op_lcmp(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    /* Pop 2 slots for each long value */
    POP_VOID(frame); /* v2 high slot */
    jlong v2 = POP(frame).j;
    POP_VOID(frame); /* v1 high slot */
    jlong v1 = POP(frame).j;
    JavaValue result;
    result.i = (v1 > v2) ? 1 : (v1 < v2) ? -1 : 0;
    PUSH(frame, result);
    return 0;
}

int op_fcmpl(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    jfloat v2 = POP(frame).f;
    jfloat v1 = POP(frame).f;
    JavaValue result;
    if (isnan(v1) || isnan(v2)) result.i = -1;
    else result.i = (v1 > v2) ? 1 : (v1 < v2) ? -1 : 0;
    PUSH(frame, result);
    return 0;
}

int op_fcmpg(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    jfloat v2 = POP(frame).f;
    jfloat v1 = POP(frame).f;
    JavaValue result;
    if (isnan(v1) || isnan(v2)) result.i = 1;
    else result.i = (v1 > v2) ? 1 : (v1 < v2) ? -1 : 0;
    PUSH(frame, result);
    return 0;
}

int op_dcmpl(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    /* Pop 2 slots for each double value */
    POP_VOID(frame); /* v2 high slot */
    jdouble v2 = POP(frame).d;
    POP_VOID(frame); /* v1 high slot */
    jdouble v1 = POP(frame).d;
    JavaValue result;
    if (isnan(v1) || isnan(v2)) result.i = -1;
    else result.i = (v1 > v2) ? 1 : (v1 < v2) ? -1 : 0;
    PUSH(frame, result);
    return 0;
}

int op_dcmpg(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    /* Pop 2 slots for each double value */
    POP_VOID(frame); /* v2 high slot */
    jdouble v2 = POP(frame).d;
    POP_VOID(frame); /* v1 high slot */
    jdouble v1 = POP(frame).d;
    JavaValue result;
    if (isnan(v1) || isnan(v2)) result.i = 1;
    else result.i = (v1 > v2) ? 1 : (v1 < v2) ? -1 : 0;
    PUSH(frame, result);
    return 0;
}

/* Branch operations */
int op_if(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    uint8_t opcode = frame->code[frame->pc - 1];
    int16_t offset = FETCH_S2(frame);
    JavaValue val = POP(frame);
    bool branch = false;
    
    /* Use jint for comparison - this is correct for both int and reference types
       For references: null = 0, non-null = non-zero address (lower 32 bits on 64-bit)
       For integers: the actual integer value */
    jint value = val.i;
    
    switch (opcode) {
        case OPC_IFEQ: branch = (value == 0); break;
        case OPC_IFNE: branch = (value != 0); break;
        case OPC_IFLT: branch = (value < 0); break;
        case OPC_IFGE: branch = (value >= 0); break;
        case OPC_IFGT: branch = (value > 0); break;
        case OPC_IFLE: branch = (value <= 0); break;
    }
    
    if (branch) {
        frame->pc += offset - 3;  /* -3 for opcode + operand */
    }
    
    return 0;
}

int op_if_icmp(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    uint8_t opcode = frame->code[frame->pc - 1];
    int16_t offset = FETCH_S2(frame);
    jint v2 = POP(frame).i;
    jint v1 = POP(frame).i;
    bool branch = false;
    
    switch (opcode) {
        case OPC_IF_ICMPEQ: branch = (v1 == v2); break;
        case OPC_IF_ICMPNE: branch = (v1 != v2); break;
        case OPC_IF_ICMPLT: branch = (v1 < v2); break;
        case OPC_IF_ICMPGE: branch = (v1 >= v2); break;
        case OPC_IF_ICMPGT: branch = (v1 > v2); break;
        case OPC_IF_ICMPLE: branch = (v1 <= v2); break;
    }
    
    if (branch) {
        frame->pc += offset - 3;
    }
    
    return 0;
}

int op_if_acmp(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    uint8_t opcode = frame->code[frame->pc - 1];
    int16_t offset = FETCH_S2(frame);
    void* v2 = POP(frame).ref;
    void* v1 = POP(frame).ref;
    bool branch = false;
    
    switch (opcode) {
        case OPC_IF_ACMPEQ: branch = (v1 == v2); break;
        case OPC_IF_ACMPNE: branch = (v1 != v2); break;
    }
    
    if (branch) {
        frame->pc += offset - 3;
    }
    
    return 0;
}

int op_goto(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    int16_t offset = FETCH_S2(frame);
    uint32_t old_pc = frame->pc - 3;  // start of goto instruction
    uint32_t new_pc = old_pc + offset;
    
    /* DEBUG: Log goto for method a.a when near case 11 */
    if (g_j2me_runtime_debug && frame->method && frame->method->name && strcmp(frame->method->name, "a") == 0 &&
        frame->method->clazz && frame->method->clazz->class_name && strcmp(frame->method->clazz->class_name, "a") == 0) {
        static int log_count = 0;
        if (log_count < 10 && old_pc >= 630 && old_pc <= 650) {
            if (g_j2me_runtime_debug) LOG_SAFE("[GOTO] a.a: old_pc=%d, offset=%d, new_pc=%d\n", old_pc, offset, new_pc);
            log_count++;
        }
    }
    
    frame->pc = new_pc;
    return 0;
}

int op_jsr(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    int16_t offset = FETCH_S2(frame);
    JavaValue ret_addr = { .i = frame->pc };
    PUSH(frame, ret_addr);
    frame->pc += offset - 3;
    return 0;
}

int op_ret(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    uint8_t index = FETCH_U1(frame);
    frame->pc = LOCAL(frame, index).i;
    return 0;
}

int op_tableswitch(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    
    /* Save the start of tableswitch instruction (opcode is at pc-1) */
    /* Offsets in tableswitch are relative to the opcode's address */
    uint32_t start_pc = frame->pc - 1;
    
    /* Align to 4-byte boundary */
    while (frame->pc % 4 != 0) frame->pc++;
    
    int32_t default_offset = (int32_t)jvm_read_u32(frame->code + frame->pc);
    frame->pc += 4;
    int32_t low = (int32_t)jvm_read_u32(frame->code + frame->pc);
    frame->pc += 4;
    int32_t high = (int32_t)jvm_read_u32(frame->code + frame->pc);
    frame->pc += 4;
    
    jint index = POP(frame).i;
    int32_t offset;

    if (index < low || index > high) {
        offset = default_offset;
    } else {
        offset = (int32_t)jvm_read_u32(frame->code + frame->pc + (index - low) * 4);
    }
    
    /* Jump to target - offset is relative to start of tableswitch instruction */
    frame->pc = start_pc + offset;
    
    return 0;
}

int op_lookupswitch(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    
    /* Save the start of lookupswitch instruction (opcode is at pc-1) */
    /* Offsets in lookupswitch are relative to the opcode's address */
    uint32_t start_pc = frame->pc - 1;
    
    /* Align to 4-byte boundary */
    while (frame->pc % 4 != 0) frame->pc++;
    
    int32_t default_offset = (int32_t)jvm_read_u32(frame->code + frame->pc);
    frame->pc += 4;
    int32_t npairs = (int32_t)jvm_read_u32(frame->code + frame->pc);
    frame->pc += 4;
    
    jint key = POP(frame).i;
    int32_t offset = default_offset;
    
    for (int32_t i = 0; i < npairs; i++) {
        int32_t match = (int32_t)jvm_read_u32(frame->code + frame->pc);
        frame->pc += 4;
        int32_t jump = (int32_t)jvm_read_u32(frame->code + frame->pc);
        frame->pc += 4;
        
        if (match == key) {
            offset = jump;
            break;
        }
    }
    
    /* Jump to target - offset is relative to start of lookupswitch instruction */
    frame->pc = start_pc + offset;
    return 0;
}

/* Return operations */
int op_return(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread; (void)frame;
    return 1;  /* Signal return */
}

/* v36.04 NESTED-RETURN-CONTAINED companion: the result deposit onto the
 * caller frame must ALWAYS fit (the caller was args-popped before the
 * call). A trip here means operand-stack drift — the leak class fixed by
 * execute_method_nested (execute.c). Fail loudly instead of silently
 * corrupting the frame / neighbouring malloc heap. */
static int return_deposit_ok(JavaFrame* prev, int slots) {
    if (prev->stack_top + slots >= prev->max_stack) {
        static int dep_violations = 0;
        if (dep_violations < 8) {
            dep_violations++;
            LOG_SAFE("[EXEC] RETURN-DEPOSIT overflow: caller stack_top=%d max=%d slots=%d in %s.%s\n",
                     (int)prev->stack_top, (int)prev->max_stack, slots,
                     prev->clazz && prev->clazz->class_name ? prev->clazz->class_name : "?",
                     prev->method && prev->method->name ? prev->method->name : "?");
        }
        return 0;
    }
    return 1;
}

int op_ireturn(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    
    /* КРИТИЧЕСКАЯ ПРОВЕРКА: есть ли кадр вызова */
    if (!frame->prev) {
        LOG_SAFE("[EXEC] ERROR: op_ireturn with no caller frame!\n");
        return -1;
    }
    if (!return_deposit_ok(frame->prev, 1)) return -1;
    
    JavaValue result = POP(frame);
    frame->prev->stack_top++;
    frame->prev->stack[frame->prev->stack_top] = result;
    return 1;
}

int op_lreturn(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    
    /* КРИТИЧЕСКАЯ ПРОВЕРКА: есть ли кадр вызова */
    if (!frame->prev) {
        LOG_SAFE("[EXEC] ERROR: op_lreturn with no caller frame!\n");
        return -1;
    }
    
    /* Pop 2 slots для long */
    if (!return_deposit_ok(frame->prev, 2)) return -1;
    (void)POP(frame);            /* high slot (копия/может быть устаревшей) */
    JavaValue result = POP(frame); /* low slot — каноническое значение */
    
    /* Push 2 slots в стек вызвавшего (обе копии результата: high-слот,
     * снятый с стека calleе, мог быть устаревшим после быстрой long-
     * арифметики — v36.38-BT2) */
    frame->prev->stack_top++;
    frame->prev->stack[frame->prev->stack_top] = result;
    frame->prev->stack_top++;
    frame->prev->stack[frame->prev->stack_top] = result;
    return 1;
}

int op_freturn(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    
    /* КРИТИЧЕСКАЯ ПРОВЕРКА: есть ли кадр вызова */
    if (!frame->prev) {
        LOG_SAFE("[EXEC] ERROR: op_freturn with no caller frame!\n");
        return -1;
    }
    if (!return_deposit_ok(frame->prev, 1)) return -1;
    
    JavaValue result = POP(frame);
    frame->prev->stack_top++;
    frame->prev->stack[frame->prev->stack_top] = result;
    return 1;
}

int op_dreturn(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    
    /* КРИТИЧЕСКАЯ ПРОВЕРКА: есть ли кадр вызова */
    if (!frame->prev) {
        LOG_SAFE("[EXEC] ERROR: op_dreturn with no caller frame!\n");
        return -1;
    }
    
    /* Pop 2 slots для double */
    if (!return_deposit_ok(frame->prev, 2)) return -1;
    (void)POP(frame);            /* high slot (копия/может быть устаревшей) */
    JavaValue result = POP(frame); /* low slot — каноническое значение */
    
    /* Push 2 слота (обе копии результата — см. v36.38-BT2 в op_lreturn) */
    frame->prev->stack_top++;
    frame->prev->stack[frame->prev->stack_top] = result;
    frame->prev->stack_top++;
    frame->prev->stack[frame->prev->stack_top] = result;
    return 1;
}

int op_areturn(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;

    /* КРИТИЧЕСКАЯ ПРОВЕРКА: есть ли кадр вызова */
    if (!frame->prev) {
        LOG_SAFE("[EXEC] ERROR: op_areturn with no caller frame!\n");
        return -1;
    }
    if (!return_deposit_ok(frame->prev, 1)) return -1;

    JavaValue result = POP(frame);

    frame->prev->stack_top++;
    frame->prev->stack[frame->prev->stack_top] = result;

    return 1;
}

/* v34.42 PERF: getstatic/putstatic inline cache. Caches the OWNING class
 * + slot index of the resolved JavaStaticField — the value itself is read
 * fresh on every access (fields mutate constantly). Slots are append-only
 * (never removed/reordered: created by the classfile parser up front or by
 * putstatic/stub injection at the tail), and the owner's static_fields
 * array pointer is re-dereferenced per access, so reallocs are safe.
 * NOT filled while the field is unresolved (default-0 fallback) — the
 * slot may appear later via stub injection. */
StaticFieldRefCache* static_field_cache_entry(JavaFrame* frame, uint16_t index) {
    /* v36.35 DIAG: NOJME_NO_STATIC_CACHE=1 disables the static-field inline
     * cache (both get/put) for sandbox debugging of stale-slot reads. */
    static int8_t disabled = -1;
    if (disabled < 0) {
        const char* e = getenv("NOJME_NO_STATIC_CACHE");
        disabled = (e && e[0] != '\0' && strcmp(e, "0") != 0) ? 1 : 0;
    }
    if (disabled) return NULL;
    JavaClass* clazz = frame->clazz;
    if (!clazz || !clazz->constant_pool_count || index >= clazz->constant_pool_count) return NULL;
    if (!clazz->static_field_cache) {
        clazz->static_field_cache = (StaticFieldRefCache*)calloc(clazz->constant_pool_count, sizeof(StaticFieldRefCache));
    }
    return clazz->static_field_cache ? &clazz->static_field_cache[index] : NULL;
}

/* Memoize a successful static-field resolution (slot >= 0). */
void static_field_cache_fill(JavaFrame* frame, uint16_t index,
                                    JavaClass* owner, int slot, uint8_t wide) {
    StaticFieldRefCache* sc = static_field_cache_entry(frame, index);
    if (sc) {
        sc->owner = owner;
        sc->slot = slot;
        sc->wide = wide;
        sc->gen = g_vm_cache_gen;
        sc->valid = (slot >= 0 && owner) ? 1 : 0;
    }
}

/* Field access - simplified stubs */
int op_getstatic(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)thread;
    /* v34.42 PERF: static-field inline cache — the getstatic twin of the
     * v34.35 getfield cache. gprof (Asphalt 3 3D in-race): op_getstatic ran
     * 21.4M times per 60 s, each access re-resolving class by NAME
     * (jvm_load_class hash) + CP name/type + a strcmp walk over the
     * superclass chain's static_fields. Contract: frame->pc enters at
     * opcode+1; on miss fall through to the original slow path. */
    {
        StaticFieldRefCache* sc = static_field_cache_entry(frame,
                (uint16_t)(((uint16_t)frame->code[frame->pc] << 8) | (uint16_t)frame->code[frame->pc + 1])  /* JVMS: big-endian u16 */);
        if (sc && sc->valid && sc->gen == g_vm_cache_gen &&
            sc->owner->static_fields && sc->slot < sc->owner->static_fields_count) {
            JavaValue v = sc->owner->static_fields[sc->slot].value;
            frame->pc += 2;   /* pc entered at opcode+1; getstatic is 3 bytes */
            PUSH(frame, v);
            if (sc->wide) {
                PUSH(frame, v);
            }
            return 0;
        }
    }
    uint16_t index = FETCH_U2(frame);
    
    const char* class_name = NULL;
    const char* field_name = NULL;
    const char* descriptor = NULL;
    
    if (index > 0 && index < frame->clazz->constant_pool_count) {
        ConstantPoolEntry* entry = &frame->clazz->constant_pool[index];
        if (entry->tag == CONSTANT_Fieldref) {
            uint16_t class_idx = entry->info.ref_info.class_index;
            uint16_t nat_idx = entry->info.ref_info.name_and_type_index;
            
            class_name = classfile_get_class_name(frame->clazz, class_idx);
            classfile_get_name_and_type(frame->clazz, nat_idx, &field_name, &descriptor);
        }
    }

    if (!class_name || !field_name || !descriptor) {
        LOG_SAFE("[EXEC] getstatic: Invalid constant pool index %d\n", index);
        JavaValue v = { .i = 0 };
        PUSH(frame, v);
        if (descriptor && (descriptor[0] == 'J' || descriptor[0] == 'D')) PUSH(frame, v);
        return 0;
    }
    
    /* DEBUG: print getstatic */
    LOG_OPCODE("[GETSTATIC] %s.%s %s\n", class_name, field_name, descriptor);
    
    /* Load class */
    JavaClass* target_class = jvm_load_class(jvm, class_name);
    if (!target_class) {
        LOG_SAFE("[EXEC] getstatic: Class %s not found\n", class_name);
        JavaValue v = { .i = 0 };
        PUSH(frame, v);
        if (descriptor && (descriptor[0] == 'J' || descriptor[0] == 'D')) PUSH(frame, v);
        return 0;
    }
    
    /* CRITICAL: Initialize class before access! */
    if (!target_class->initialized) {
        if (jvm_init_class(jvm, target_class) != 0) {
            return -1;
        }
    }
    
    /* Ensure static storage exists (for stubs) */
    if (!target_class->static_fields) {
        target_class->static_fields = (JavaStaticField*)calloc(16, sizeof(JavaStaticField));
        target_class->static_fields_count = 0;
        target_class->static_fields_capacity = 16;
    }
    
    /* v31 FIX: resolve static fields through the SUPERCLASS chain.
     * Java inherits static fields: `MainCanvas.KEY_NUM6` must resolve to
     * `Canvas.KEY_NUM6`. The old lookup searched only the named class, so
     * any inherited constant read back 0 — e.g. M3GTest.keyPressed compared
     * keyCode(54) with KEY_NUM6(0): LEFT/RIGHT/4/6 navigation never fired.
     * Walk up (bounded, cycle-safe); first name+descriptor match wins. */
    JavaValue v = { .raw = 0 };
    bool found = false;

    {
        int depth = 0;
        for (JavaClass* sc = target_class; sc && !found && depth < 64; sc = sc->super_class, depth++) {
            if (!sc->static_fields) continue;
            for (int i = 0; i < sc->static_fields_count; i++) {
                if (sc->static_fields[i].name &&
                    strcmp(sc->static_fields[i].name, field_name) == 0) {
                    /* Must match descriptor exactly - fields can have same name but different types */
                    if (descriptor && sc->static_fields[i].descriptor &&
                        strcmp(sc->static_fields[i].descriptor, descriptor) == 0) {
                        v = sc->static_fields[i].value;
                        found = true;
                        /* v34.42 PERF: memoize the resolution (owner class +
                         * slot); the VALUE is re-read on every fast-path
                         * access so writes stay visible. */
                        static_field_cache_fill(frame, index, sc, i,
                                (uint8_t)(descriptor[0] == 'J' || descriptor[0] == 'D'));
                        break;
                    }
                    /* No fallback - if descriptor doesn't match, this is a different field */
                }
            }
        }
    }
    
    if (!found) {
        LOG_OPCODE("[EXEC] getstatic: Field %s.%s %s not found (count=%d), returning default.\n", 
                class_name, field_name, descriptor, target_class->static_fields_count);
    } else {
        LOG_OPCODE("[EXEC] getstatic: Found %s.%s %s = %p\n", 
                class_name, field_name, descriptor, (void*)v.ref);
    }
    
    PUSH(frame, v);
    
    /* Long/double occupy 2 slots */
    if (descriptor[0] == 'J' || descriptor[0] == 'D') {
        PUSH(frame, v);
    }
    
    return 0;
}

int op_putstatic(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)thread;
    /* v34.42 PERF: static-field inline cache (see op_getstatic). The value
     * is popped FIRST in both paths — keep that order: a <clinit> triggered
     * by the slow path's jvm_init_class may read the field we are about to
     * write, and the pop must already have happened on our frame. */
    {
        StaticFieldRefCache* sc = static_field_cache_entry(frame,
                (uint16_t)(((uint16_t)frame->code[frame->pc] << 8) | (uint16_t)frame->code[frame->pc + 1])  /* JVMS: big-endian u16 */);
        if (sc && sc->valid && sc->gen == g_vm_cache_gen &&
            sc->owner->static_fields && sc->slot < sc->owner->static_fields_count) {
            JavaValue v;
            if (sc->wide) {
                POP_VOID(frame); /* high slot */
                v = POP(frame);
            } else {
                v = POP(frame);
            }
            frame->pc += 2;   /* pc entered at opcode+1; putstatic is 3 bytes */
            sc->owner->static_fields[sc->slot].value = v;
            return 0;
        }
    }
    uint16_t index = FETCH_U2(frame);
    
    const char* class_name = NULL;
    const char* field_name = NULL;
    const char* descriptor = NULL;
    
    if (index > 0 && index < frame->clazz->constant_pool_count) {
        ConstantPoolEntry* entry = &frame->clazz->constant_pool[index];
        if (entry->tag == CONSTANT_Fieldref) {
            uint16_t class_idx = entry->info.ref_info.class_index;
            uint16_t nat_idx = entry->info.ref_info.name_and_type_index;
            
            class_name = classfile_get_class_name(frame->clazz, class_idx);
            classfile_get_name_and_type(frame->clazz, nat_idx, &field_name, &descriptor);
        }
    }
    
    /* Pop value */
    JavaValue v;
    if (descriptor && (descriptor[0] == 'J' || descriptor[0] == 'D')) {
        POP_VOID(frame); /* high slot */
        v = POP(frame);
    } else {
        v = POP(frame);
    }
    
    if (!class_name || !field_name) {
        LOG_SAFE("[EXEC] putstatic: Invalid field ref\n");
        return 0;
    }

    LOG_OPCODE("[EXEC] putstatic: %s.%s = %p\n", class_name, field_name, (void*)v.ref);
    
    /* Load class */
    JavaClass* target_class = jvm_load_class(jvm, class_name);
    if (!target_class) {
        LOG_SAFE("[EXEC] putstatic: Class %s not found\n", class_name);
        return 0;
    }
    
    /* CRITICAL: Initialize class before access! */
    if (!target_class->initialized) {
        if (jvm_init_class(jvm, target_class) != 0) {
            return -1;
        }
    }
    
    /* Ensure static storage */
    if (!target_class->static_fields) {
        target_class->static_fields = (JavaStaticField*)calloc(16, sizeof(JavaStaticField));
        target_class->static_fields_count = 0;
        target_class->static_fields_capacity = 16;
    }
    
    /* Find or create slot - MUST match by name AND descriptor for obfuscated code */
    /* CRITICAL: Fields with same name but different descriptors are DIFFERENT fields! */
    /* v31 FIX: resolve the DECLARING class through the superclass chain first
     * (JVM spec: putstatic writes the inherited field, not a subclass shadow).
     * Without this, `putstatic SubClass.FIELD` where FIELD is declared in a
     * superclass created a SHADOW slot: reads via the superclass saw the old
     * value, reads via the subclass the new one — divergent state. */
    JavaClass* owner_class = target_class;
    int slot = -1;
    {
        int depth = 0;
        for (JavaClass* sc = target_class; sc && slot == -1 && depth < 64; sc = sc->super_class, depth++) {
            if (!sc->static_fields) continue;
            for (int i = 0; i < sc->static_fields_count; i++) {
                if (sc->static_fields[i].name &&
                    strcmp(sc->static_fields[i].name, field_name) == 0) {
                    /* Must match descriptor exactly - fields can have same name but different types */
                    if (descriptor && sc->static_fields[i].descriptor &&
                        strcmp(sc->static_fields[i].descriptor, descriptor) == 0) {
                        slot = i;
                        owner_class = sc;
                        break;
                    }
                    /* If no descriptor in request OR no descriptor stored - can't match safely */
                    /* Don't use fallback - it causes corruption in obfuscated code */
                }
            }
        }
    }
    
    if (slot == -1) {
        /* Create new slot on the named class - always create for unseen name+descriptor combination */
        owner_class = target_class;
        if (!target_class->static_fields) {
            target_class->static_fields = (JavaStaticField*)calloc(16, sizeof(JavaStaticField));
            target_class->static_fields_count = 0;
            target_class->static_fields_capacity = 16;
        }
        if (target_class->static_fields_count >= target_class->static_fields_capacity) {
            int new_cap = target_class->static_fields_capacity + 16;
            JavaStaticField* new_fields = (JavaStaticField*)realloc(
                target_class->static_fields, new_cap * sizeof(JavaStaticField));
            if (!new_fields) return -1;
            
            memset(new_fields + target_class->static_fields_capacity, 0, 
                   16 * sizeof(JavaStaticField));
            target_class->static_fields = new_fields;
            target_class->static_fields_capacity = new_cap;
        }
        
        slot = target_class->static_fields_count++;
        target_class->static_fields[slot].name = strdup(field_name);
        target_class->static_fields[slot].descriptor = descriptor ? strdup(descriptor) : NULL;
        LOG_OPCODE("[EXEC] putstatic: Created new slot %d for %s.%s %s\n", slot, class_name, field_name, descriptor ? descriptor : "");
    }
    
    /* v31: write through the resolved owner (declares the field) */
    owner_class->static_fields[slot].value = v;
    /* v34.42 PERF: memoize the resolution (owner class + slot). */
    static_field_cache_fill(frame, index, owner_class, slot,
            (uint8_t)(descriptor && (descriptor[0] == 'J' || descriptor[0] == 'D')));
    LOG_OPCODE("[EXEC] putstatic: Stored %s.%s %s = %p (slot %d, owner=%s, count=%d)\n", 
            class_name, field_name, descriptor ? descriptor : "", (void*)v.ref, slot,
            owner_class->class_name ? owner_class->class_name : "?",
            owner_class->static_fields_count);
    return 0;
}

/* ============================================================
 * v34.35 PERF: constant-pool inline caches (see JavaClass fields).
 * getfield/putfield resolution used to walk the CP strings, call
 * jvm_load_class (per access!) and strcmp-walk the field hierarchy on
 * EVERY access. These caches memoize the resolution per CP index.
 * ============================================================ */
/* v34.35 FIX (stubs.c methods[] realloc / late field additions): bump this
 * whenever any class's methods[] array is reallocated or instance field
 * slots shift; inline caches record the generation they were built in and
 * self-invalidate on mismatch. Mirrors the v18 method_cache_flush fix,
 * which only covered the global hash cache. */
uint16_t g_vm_cache_gen = 0;

typedef struct InvokeRefCache {
    JavaClass* target_class; /* virtual: receiver class this entry resolved
                              * for (monomorphic site guard);
                              * static/special: the resolved method's class */
    JavaMethod* method;      /* resolved bytecode method (NULL => native/unresolved) */
    void*       native_fn;   /* NativeMethod if resolved native */
    const char* cls_name;    /* v34.42: CP-declared class name (native dispatch note) */
    const char* mth_name;   /* v34.42: method name (native dispatch note) */
    int16_t     java_argc;   /* compact arg count (J/D = 1), this INCLUDED for virtual */
    uint16_t    gen;         /* g_vm_cache_gen at fill time (invalidation) */
    uint8_t     arg_slots[12]; /* operand-stack slots per Java arg (1 or 2), 0-terminated */
    uint8_t     kind;        /* 0=bytecode method, 1=native, 2=unresolved (never cached) */
    uint8_t     ret_wide;    /* J/D return pushes 2 slots */
    uint8_t     has_ret;     /* descriptor return != V */
    uint8_t     valid;
} InvokeRefCache;

/* Fast subclass test: walk oc's super chain looking for `of`. */
bool jvm_class_is_subclass_of(JavaClass* oc, JavaClass* of) {
    while (oc) {
        if (oc == of) return true;
        oc = oc->super_class;
    }
    return false;
}

/* v34.35: lazily allocated arrays parallel to constant_pool. CP growth at
 * runtime (stubs.c ensure_cp_capacity) FREES + NULLs these arrays (see the
 * bump sites), so the element count always matches constant_pool_count. */
FieldRefCache* field_cache_entry(JavaFrame* frame, uint16_t index) {
    JavaClass* clazz = frame->clazz;
    if (!clazz || !clazz->constant_pool_count || index >= clazz->constant_pool_count) return NULL;
    if (!clazz->field_cache) {
        clazz->field_cache = (FieldRefCache*)calloc(clazz->constant_pool_count, sizeof(FieldRefCache));
    }
    return clazz->field_cache ? &clazz->field_cache[index] : NULL;
}

static InvokeRefCache* invoke_cache_entry(JavaFrame* frame, uint16_t index) {
    JavaClass* clazz = frame->clazz;
    if (!clazz || !clazz->constant_pool_count || index >= clazz->constant_pool_count) return NULL;
    if (!clazz->invoke_cache) {
        clazz->invoke_cache = (InvokeRefCache*)calloc(clazz->constant_pool_count, sizeof(InvokeRefCache));
    }
    return clazz->invoke_cache ? &clazz->invoke_cache[index] : NULL;
}

/* v34.42 PERF: fill helper for the op_invokestatic inline cache.
 * kind 0 = resolved bytecode method, kind 1 = registered native.
 * Methods with >11 Java args (or unparsable descriptors) stay uncached. */
static void invokestatic_cache_fill(JavaFrame* frame, uint16_t index,
                                    JavaClass* target_class, JavaMethod* method,
                                    void* native_fn,
                                    const char* class_name, const char* method_name,
                                    const char* descriptor) {
    int jac = count_java_args(descriptor);
    if (jac < 0 || jac > 11) return;
    if (jac == 0 && native_fn == NULL) {
        /* zero-arg bytecode call is fine; nothing to parse below */
    }
    InvokeRefCache* ic = invoke_cache_entry(frame, index);
    if (!ic) return;
    /* arg slot layout from the descriptor (J/D = 2 slots) */
    const char* dp = descriptor;
    if (*dp == '(') dp++;
    int ai = 0, slots_ok = 1;
    while (*dp && *dp != ')' && ai < jac) {
        if (*dp == 'J' || *dp == 'D') { ic->arg_slots[ai++] = 2; dp++; }
        else if (*dp == 'L') { ic->arg_slots[ai++] = 1; while (*dp && *dp != ';') dp++; if (*dp) dp++; }
        else if (*dp == '[') { ic->arg_slots[ai] = 1; dp++; if (*dp == 'L') { while (*dp && *dp != ';') dp++; if (*dp) dp++; } else if (*dp) dp++; ai++; }
        else { ic->arg_slots[ai++] = 1; dp++; }
    }
    if (ai != jac || (*dp != ')')) slots_ok = 0;
    if (!slots_ok) return;
    ic->target_class = target_class;
    ic->method = method;
    ic->native_fn = native_fn;
    ic->cls_name = class_name;
    ic->mth_name = method_name;
    ic->java_argc = (int16_t)jac;
    ic->gen = g_vm_cache_gen;
    ic->kind = (native_fn != NULL) ? 1 : 0;
    const char* rp = strchr(descriptor, ')');
    ic->has_ret = (rp && rp[1] != 'V' && rp[1] != '\0') ? 1 : 0;
    ic->ret_wide = (rp && (rp[1] == 'J' || rp[1] == 'D')) ? 1 : 0;
    ic->valid = 1;
}

/* Memoize a successful field resolution. slot<0 clears the entry. */
void field_cache_fill(JavaFrame* frame, uint16_t index,
                             JavaClass* declared, int slot, uint8_t wide) {
    FieldRefCache* fc = field_cache_entry(frame, index);
    if (fc) {
        fc->declared = declared;
        fc->slot = slot;
        fc->wide = wide;
        fc->gen = g_vm_cache_gen;
        fc->valid = (slot >= 0) ? 1 : 0;
    }
}

int op_getfield(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    /* v34.35 PERF: inline-cache fast path. Entry contract: frame->pc points
     * AFTER the opcode byte (set by the interpreter loop). On any miss we
     * fall through to the ORIGINAL slow path below (which re-fetches the
     * operand itself — do NOT consume anything here). */
    {
        FieldRefCache* fc = field_cache_entry(frame, (uint16_t)(((uint16_t)frame->code[frame->pc] << 8) | (uint16_t)frame->code[frame->pc + 1])  /* JVMS: big-endian u16 */);
        if (fc && fc->valid && fc->gen == g_vm_cache_gen) {
            JavaObject* obj = (JavaObject*)PEEK(frame).ref;
            /* [ARGGUARD] v36.49: мусорный receiver -> медленный путь */
            if (obj && !argguard_bad_ptr((uintptr_t)obj)) {
                JavaClass* oc = obj->header.clazz;
                if (oc && (oc == fc->declared || jvm_class_is_subclass_of(oc, fc->declared))) {
                    /* slot is absolute (Object-first layout); shadowed-field
                     * semantics are anchored at the CP-declared class —
                     * exactly what the slow path resolves. */
                    JavaValue v = obj->fields[fc->slot];
                    frame->stack[frame->stack_top] = v;
                    if (fc->wide) {
                        PUSH(frame, v);
                    }
                    frame->pc += 2;   /* pc entered at opcode+1; getfield is 3 bytes */
                    return 0;
                }
            }
        }
    }
    uint16_t index = FETCH_U2(frame);
    
    /* Get field reference from constant pool */
    const char* class_name = NULL;
    const char* field_name = NULL;
    const char* descriptor = NULL;
    
    if (index > 0 && index < frame->clazz->constant_pool_count) {
        ConstantPoolEntry* entry = &frame->clazz->constant_pool[index];
        if (entry->tag == CONSTANT_Fieldref) {
            uint16_t class_idx = entry->info.ref_info.class_index;
            uint16_t nat_idx = entry->info.ref_info.name_and_type_index;
            
            class_name = classfile_get_class_name(frame->clazz, class_idx);
            classfile_get_name_and_type(frame->clazz, nat_idx, &field_name, &descriptor);
        }
    }
    
    /* Pop object reference */
    JavaObject* obj = (JavaObject*)POP(frame).ref;
    
    /* === Safety Checks === */
    if (!obj) {
        /* DRM bypass: return safe defaults for null DRM objects */
        if (class_name && drm_is_drm_class(class_name)) {
            JavaValue v = { .i = 0 };
            PUSH(frame, v);
            /* long and double need 2 stack slots */
            if (descriptor && (descriptor[0] == 'J' || descriptor[0] == 'D')) {
                PUSH(frame, v);
            }
            return 0;
        }
        LOG_SAFE("[EXEC] getfield: NULL object for field %s.%s\n", 
                class_name ? class_name : "?", field_name ? field_name : "?");
        native_throw_npe(jvm, thread);
        return -1;
    }
    
    /* [ARGGUARD] v36.49: мусорный receiver (не-каноничный/невыровненный)
     * — НЕ deref [obj+0]; тот же исход, что при NULL классе. */
    if (argguard_bad_ptr((uintptr_t)obj)) {
        LOG_SAFE("[EXEC] getfield: FATAL: garbage receiver %p for field %s.%s (ARGGUARD)\n",
                (void*)obj, class_name ? class_name : "?", field_name ? field_name : "?");
        jvm_throw_by_name(jvm, "java/lang/InternalError", "garbage receiver (getfield)");
        return -1;
    }
    
    if (!obj->header.clazz) {
        LOG_SAFE("[EXEC] getfield: FATAL: Object %p has NULL class pointer! Field: %s.%s\n", 
                (void*)obj, class_name ? class_name : "?", field_name ? field_name : "?");
        jvm_throw_by_name(jvm, "java/lang/InternalError", "Object has no class (getfield)");
        return -1;
    }
    
    if ((uintptr_t)obj->header.clazz < 0x10000) {
        LOG_SAFE("[EXEC] getfield: FATAL: Object %p has invalid class pointer %p!\n", 
                (void*)obj, (void*)obj->header.clazz);
        jvm_throw_by_name(jvm, "java/lang/InternalError", "Object has invalid class pointer");
        return -1;
    }

    JavaClass* obj_class = obj->header.clazz;
    JavaValue v = { .i = 0 };
    
    /* === v18 FIX (JVMS 6.5 getfield): field resolution is anchored at the
     * CLASS named in the constant-pool Fieldref, NOT at the object's runtime
     * class. With shadowed fields (ClsBase.fld=1 hidden by ClsDerived.fld=2),
     * reading through a base-typed reference must observe the BASE field.
     * Slot offsets are absolute (Object-first), so the returned slot indexes
     * obj->fields correctly regardless of which hierarchy class defines it.
     * Fall back to the runtime class when the declared class cannot be
     * loaded or does not declare the field (obfuscated-bytecode safety). === */
    JavaClass* ref_class = obj_class;
    JavaClass* cache_declared = NULL;   /* v34.35: loaded CP-declared class */
    if (class_name) {
        JavaClass* declared = jvm_load_class(jvm, class_name);
        if (declared) { ref_class = declared; cache_declared = declared; }
    }
    FieldLookupResult field_result = find_field_in_hierarchy(ref_class, field_name, descriptor);
    if (!field_result.defining_class && ref_class != obj_class) {
        /* v34.35 PERF: obj_class fallback — the resolved slot depends on the
         * receiver's runtime class (subclass shadowing); NOT memoizable. */
        field_result = find_field_in_hierarchy(obj_class, field_name, descriptor);
        cache_declared = NULL;
    }

    if (field_result.defining_class) {
        int slot = field_result.slot;
        
        /* Bounds check - max_slots is number of field slots (excluding ObjectHeader) */
        int max_slots = (obj_class->instance_size - sizeof(ObjectHeader)) / sizeof(JavaValue);
        if (slot < 0 || slot >= max_slots) {
            LOG_SAFE("[EXEC] getfield: FATAL: Calculated slot %d out of bounds (max %d) for field %s.%s (defined in %s)\n",
                    slot, max_slots, class_name ? class_name : "?", field_name ? field_name : "?",
                    field_result.defining_class->class_name ? field_result.defining_class->class_name : "?");
            jvm_throw_by_name(jvm, "java/lang/InternalError", "Field slot out of bounds");
            return -1;
        }

        v = obj->fields[slot];

        /* v34.35 PERF: memoize declared-anchored hits (cache_declared set
         * above) — the obj_class fallback is receiver-class-dependent and
         * must stay on the slow path. */
        if (cache_declared) {
            uint8_t wide = (descriptor && (descriptor[0] == 'J' || descriptor[0] == 'D')) ? 1 : 0;
            field_cache_fill(frame, index, cache_declared, slot, wide);
        }
    } else {
        LOG_SAFE("[EXEC] getfield: field %s.%s%s not found in hierarchy, returning 0\n", 
                class_name ? class_name : "?", field_name ? field_name : "?",
                descriptor ? descriptor : "?");
    }
    
    PUSH(frame, v);
    
    /* Long and double fields need 2 stack slots */
    if (descriptor && (descriptor[0] == 'J' || descriptor[0] == 'D')) {
        PUSH(frame, v);
    }
    
    return 0;
}

int op_putfield(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    /* v34.35 PERF: inline-cache fast path (see op_getfield). On miss, the
     * original slow path below re-fetches and handles wide/DRM/NPE cases.
     * Frame->pc enters at opcode+1. Stack layout: [.., obj, value] or
     * [.., obj, v, v] for J/D. */
    {
        FieldRefCache* fc = field_cache_entry(frame, (uint16_t)(((uint16_t)frame->code[frame->pc] << 8) | (uint16_t)frame->code[frame->pc + 1])  /* JVMS: big-endian u16 */);
        if (fc && fc->valid && fc->gen == g_vm_cache_gen) {
            int voff = fc->wide ? 2 : 1;
            JavaObject* obj = (JavaObject*)frame->stack[frame->stack_top - voff].ref;
            /* [ARGGUARD] v36.49: мусорный receiver -> медленный путь */
            if (obj && !argguard_bad_ptr((uintptr_t)obj)) {
                JavaClass* oc = obj->header.clazz;
                if (oc && (oc == fc->declared || jvm_class_is_subclass_of(oc, fc->declared))) {
                    JavaValue v = POP(frame);
                    if (fc->wide) {
                        POP_VOID(frame);   /* discard the J/D high slot (copy) */
                    }
                    POP_VOID(frame);       /* discard the receiver ref */
                    obj->fields[fc->slot] = v;
                    frame->pc += 2;   /* pc entered at opcode+1; putfield is 3 bytes */
                    return 0;
                }
            }
        }
    }
    uint16_t index = FETCH_U2(frame);
    
    /* Get field reference from constant pool */
    const char* class_name = NULL;
    const char* field_name = NULL;
    const char* descriptor = NULL;
    
    if (index > 0 && index < frame->clazz->constant_pool_count) {
        ConstantPoolEntry* entry = &frame->clazz->constant_pool[index];
        if (entry->tag == CONSTANT_Fieldref) {
            uint16_t class_idx = entry->info.ref_info.class_index;
            uint16_t nat_idx = entry->info.ref_info.name_and_type_index;
            
            class_name = classfile_get_class_name(frame->clazz, class_idx);
            classfile_get_name_and_type(frame->clazz, nat_idx, &field_name, &descriptor);
        }
    }
    
    /* Pop value - long and double need 2 stack slots */
    JavaValue value;
    if (descriptor && (descriptor[0] == 'J' || descriptor[0] == 'D')) {
        POP_VOID(frame); /* discard high slot */
        value = POP(frame);
    } else {
        value = POP(frame);
    }
    
    /* Pop object reference */
    JavaObject* obj = (JavaObject*)POP(frame).ref;

    /* === Safety Checks === */
    if (!obj) {
        /* DRM bypass: silently ignore putfield on null DRM objects */
        if (class_name && drm_is_drm_class(class_name)) {
            return 0;
        }
        LOG_SAFE("[EXEC] putfield: NULL object for field %s.%s\n", 
                class_name ? class_name : "?", field_name ? field_name : "?");
        native_throw_npe(jvm, thread);
        return -1;
    }
    
    /* [ARGGUARD] v36.49: мусорный receiver — НЕ deref [obj+0] */
    if (argguard_bad_ptr((uintptr_t)obj)) {
        LOG_SAFE("[EXEC] putfield: FATAL: garbage receiver %p for field %s.%s (ARGGUARD)\n",
                (void*)obj, class_name ? class_name : "?", field_name ? field_name : "?");
        jvm_throw_by_name(jvm, "java/lang/InternalError", "garbage receiver (putfield)");
        return -1;
    }
    
    if (!obj->header.clazz) {
        LOG_SAFE("[EXEC] putfield: FATAL: Object %p has NULL class pointer! Field: %s.%s\n", 
                (void*)obj, class_name ? class_name : "?", field_name ? field_name : "?");
        jvm_throw_by_name(jvm, "java/lang/InternalError", "Object has no class (putfield)");
        return -1;
    }
    
    if ((uintptr_t)obj->header.clazz < 0x10000) {
        LOG_SAFE("[EXEC] putfield: FATAL: Object %p has invalid class pointer %p!\n", 
                (void*)obj, (void*)obj->header.clazz);
        jvm_throw_by_name(jvm, "java/lang/InternalError", "Object has invalid class pointer");
        return -1;
    }
    
    JavaClass* obj_class = obj->header.clazz;
    
    /* === v18 FIX (JVMS 6.5 putfield): resolve from the declared class of
     * the Fieldref (see the getfield comment). Writing through a base-typed
     * reference must update the BASE field, not a subclass shadow. === */
    JavaClass* ref_class = obj_class;
    JavaClass* cache_declared = NULL;   /* v34.35: loaded CP-declared class */
    if (class_name) {
        JavaClass* declared = jvm_load_class(jvm, class_name);
        if (declared) { ref_class = declared; cache_declared = declared; }
    }
    FieldLookupResult field_result = find_field_in_hierarchy(ref_class, field_name, descriptor);
    if (!field_result.defining_class && ref_class != obj_class) {
        /* v34.35 PERF: obj_class fallback — NOT memoizable (see getfield). */
        field_result = find_field_in_hierarchy(obj_class, field_name, descriptor);
        cache_declared = NULL;
    }

    if (field_result.defining_class) {
        int slot = field_result.slot;
        
        /* Bounds check - max_slots is number of field slots (excluding ObjectHeader) */
        int max_slots = (obj_class->instance_size - sizeof(ObjectHeader)) / sizeof(JavaValue);
        if (slot < 0 || slot >= max_slots) {
            LOG_SAFE("[EXEC] putfield: FATAL: Calculated slot %d out of bounds (max %d) for field %s.%s (defined in %s)\n",
                    slot, max_slots, class_name ? class_name : "?", field_name ? field_name : "?",
                    field_result.defining_class->class_name ? field_result.defining_class->class_name : "?");
            return -1;
        }

        obj->fields[slot] = value;

        /* v34.35 PERF: memoize declared-anchored hits for the fast path. */
        if (cache_declared) {
            uint8_t wide = (descriptor && (descriptor[0] == 'J' || descriptor[0] == 'D')) ? 1 : 0;
            field_cache_fill(frame, index, cache_declared, slot, wide);
        }

        LOG_OPCODE("[EXEC] putfield: %s.%s (slot %d)\n", 
                   class_name ? class_name : "?", field_name ? field_name : "?", slot);
    } else {
        LOG_SAFE("[EXEC] putfield: field %s.%s not found in hierarchy!\n", 
                class_name ? class_name : "?", field_name ? field_name : "?");
    }
    
    return 0;
}

/* Method invocation - UPDATED with exception checking after native calls */
/* v34.4 DIAG: compact context dump for invoke stack underflows. Printed at
 * most 5 times per process; shows the caller frame, a short bytecode window
 * right before the invoke (which instruction was supposed to produce the
 * missing slot) and the surviving operand stack. Diagnoses cases like the
 * user log "Stack underflow in invokevirtual: Alert.setString ... (stack
 * has 1)" where an earlier push silently never happened. */
static void log_invoke_underflow_diag(const char* kind, JavaFrame* frame,
                                      const char* cls, const char* meth,
                                      const char* desc, int arg_count,
                                      int available, int invoke_len) {
    static int diag_count = 0;
    if (diag_count >= 5) return;
    diag_count++;
    int pc = (int)frame->pc - invoke_len;
    if (pc < 0) pc = (int)frame->pc;
    LOG_SAFE("[EXEC] Stack underflow in %s: %s.%s%s at PC=%d (need %d args + this, stack has %d) - padding with zeros\n",
             kind, cls ? cls : "?", meth ? meth : "?", desc ? desc : "?",
             pc, arg_count, available);
    LOG_SAFE("[EXEC]   caller=%s.%s max_stack=%d\n",
             (frame->clazz && frame->clazz->class_name) ? frame->clazz->class_name : "?",
             (frame->method && frame->method->name) ? frame->method->name : "?",
             (int)frame->max_stack);
    /* Bytecode window before the invoke */
    {
        int start = pc - 12;
        if (start < 0) start = 0;
        char hex[3 * 12 + 1];
        size_t pos = 0;
        for (int i = start; i < pc && pos + 3 < sizeof(hex); i++) {
            pos += (size_t)snprintf(hex + pos, sizeof(hex) - pos, "%02X ", frame->code[i]);
        }
        hex[pos] = '\0';
        LOG_SAFE("[EXEC]   bytes before invoke [PC %d..%d]: %s\n", start, pc, hex);
    }
    /* Surviving operand stack (top 8 entries, top first) */
    {
        int depth = frame->stack_top + 1;
        int n = depth > 8 ? 8 : depth;
        for (int k = 0; k < n; k++) {
            int idx = frame->stack_top - k;
            JavaValue* v = &frame->stack[idx];
            LOG_SAFE("[EXEC]   stack[%d] ref=%p raw=0x%llx\n", idx,
                     (void*)v->ref, (unsigned long long)v->raw);
        }
        if (depth > 8) {
            LOG_SAFE("[EXEC]   ... %d more slots\n", depth - 8);
        }
    }
}

/* === v34.6: generic invoke tracer ======================================
 * NOJME_TRACE_INVOKE="Cls.m1;Cls.m2;..." — print every call of the listed
 * class.method pairs (invokestatic/virtual/special/interface), throttled:
 * first 40 hits, then every 500th. Used to trace game-side render chains
 * (Doom RPG: k.J / i.a / r.c / r.a never reaching Graphics.drawRGB). */
typedef struct { char* cls; char* mth; char* desc; int hits; } InvokeWatchEntry;
static InvokeWatchEntry g_invoke_watch[16];
static int g_invoke_watch_n = -1;   /* -1 = not initialized */
/* v34.11: NOJME_TRACE_INVOKE_EVERY=<n> — after the first 40 hits of an entry,
 * print every <n>-th (default 500). Set 1 to log every call. */
static int g_invoke_watch_every = 500;

static void invoke_watch_init(void) {
    if (g_invoke_watch_n >= 0) return;
    g_invoke_watch_n = 0;
    const char* every_env = getenv("NOJME_TRACE_INVOKE_EVERY");
    if (every_env && *every_env) {
        int v = atoi(every_env);
        if (v > 0) g_invoke_watch_every = v;
    }
    const char* env = getenv("NOJME_TRACE_INVOKE");
    if (!env || !*env) return;
    /* Manual tokenizer: strtok_r is not portable to all MinGW headers */
    const char* p = env;
    while (*p && g_invoke_watch_n < 16) {
        const char* semi = strchr(p, ';');
        size_t len = semi ? (size_t)(semi - p) : strlen(p);
        if (len > 0 && len < 64) {
            char tok[64];
            memcpy(tok, p, len);
            tok[len] = '\0';
            char* dot = strchr(tok, '.');
            if (dot && dot != tok && dot[1]) {
                *dot = '\0';
                g_invoke_watch[g_invoke_watch_n].cls = strdup(tok);
                /* Optional descriptor prefix: "Cls.mth(desc" matches methods
                 * whose descriptor starts with "(desc" (overload disambiguation). */
                char* paren = strchr(dot + 1, '(');
                if (paren) {
                    g_invoke_watch[g_invoke_watch_n].desc = strdup(paren);
                    *paren = '\0';
                } else {
                    g_invoke_watch[g_invoke_watch_n].desc = NULL;
                }
                g_invoke_watch[g_invoke_watch_n].mth = strdup(dot + 1);
                g_invoke_watch[g_invoke_watch_n].hits = 0;
                g_invoke_watch_n++;
            }
        }
        p = semi ? semi + 1 : p + len;
    }
}

static void invoke_watch_hit(const char* cls, const char* mth, const char* desc,
                             const char* kind, JavaFrame* frame, jint top_int) {
    if (g_invoke_watch_n <= 0) return;
    for (int i = 0; i < g_invoke_watch_n; i++) {
        if (strcmp(g_invoke_watch[i].cls, cls) == 0 &&
            strcmp(g_invoke_watch[i].mth, mth) == 0 &&
            (!g_invoke_watch[i].desc ||
             (desc && strncmp(desc, g_invoke_watch[i].desc,
                              strlen(g_invoke_watch[i].desc)) == 0))) {
            int h = ++g_invoke_watch[i].hits;
            if (h <= 40 || (h % g_invoke_watch_every) == 0) {
                struct timespec ts;
                clock_gettime(CLOCK_MONOTONIC, &ts);
                LOG_SAFE("[INVOKE-WATCH] [%ld.%03ld] %s %s.%s%s hit#%d topInt=%d caller=%s.%s pc=%u\n",
                         (long)(ts.tv_sec % 100000), (long)(ts.tv_nsec / 1000000),
                         kind, cls, mth, desc ? desc : "", h, top_int,
                         (frame->clazz && frame->clazz->class_name) ? frame->clazz->class_name : "?",
                         (frame->method && frame->method->name) ? frame->method->name : "?",
                         (unsigned)frame->pc);
            }
            return;
        }
    }
}

/* v36.03 NATIVE-ARGS-ROOTED: the slow paths of the invoke wrappers call
 * natives DIRECTLY (bypassing native_call), so the receiver/args are
 * invisible to the GC frame scan for the whole native. The v34.14 pins
 * only protect the arg OBJECTS themselves — not the object graph behind
 * them (a pinned Vector whose elementData elements still get swept was
 * exactly the field Data Abort shape). Publishing the args buffer to the
 * mark phase makes the GC mark the full graph reachable from the args
 * while the native runs. Same helper pattern as native_call's rooting. */
static int nargs_root_push(JavaThread* thread, JavaValue* args, int count) {
    if (!thread || !args || count <= 0) return -1;
    if (thread->native_arg_depth >= NATIVE_ARG_FRAMES_MAX) return -1;
    int slot = thread->native_arg_depth++;
    thread->native_arg_frames[slot].args = args;
    thread->native_arg_frames[slot].count = count;
    return slot;
}

static void nargs_root_pop(JavaThread* thread, int slot) {
    if (slot < 0 || !thread) return;
    thread->native_arg_frames[slot].args = NULL;
    thread->native_arg_frames[slot].count = 0;
    thread->native_arg_depth--;
}

int op_invokevirtual(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    /* v34.35 PERF: monomorphic inline cache. Contract: frame->pc enters at
     * opcode+1. On miss (no cache entry, receiver class changed, stack
     * underflow) we fall through to the original slow path which re-fetches
     * everything and re-executes the compat/DRM/null-receiver hooks. */
    {
        InvokeRefCache* ic = invoke_cache_entry(frame,
                (uint16_t)(((uint16_t)frame->code[frame->pc] << 8) | (uint16_t)frame->code[frame->pc + 1]));
        if (ic && ic->valid && ic->kind == 0 && ic->java_argc >= 0 && ic->gen == g_vm_cache_gen) {
            int total_slots = 1;   /* this */
            for (int i = 0; i < ic->java_argc; i++) total_slots += ic->arg_slots[i];
            if (frame->stack_top + 1 >= total_slots) {
                int base = frame->stack_top + 1 - total_slots;
                JavaObject* recv = (JavaObject*)frame->stack[base].ref;
                /* [ARGGUARD] v36.49: receiver из operand stack может быть
                 * мусором (поле: 0x20697465592F7365 = ASCII "es/Yeti ") —
                 * сначала правдоподобие, ПОТОМ deref [recv+0]; мусор уходит
                 * в медленный путь с валидацией (Ryujinx suppression
                 * давала тот же исход: сравнение провалено -> slow path). */
                if (recv && !argguard_bad_ptr((uintptr_t)recv) &&
                    recv->header.clazz == ic->target_class) {
                    /* Hot path: compact args pop + direct execute_method. */
                    JavaValue args_buf[12];
                    JavaValue* args = (total_slots <= 12) ? args_buf
                        : (JavaValue*)malloc((size_t)total_slots * sizeof(JavaValue));
                    if (args) {
                        args[0] = frame->stack[base];
                        int cur = base + 1;
                        for (int i = 0; i < ic->java_argc; i++) {
                            args[i + 1] = frame->stack[cur];   /* low slot of Java arg i */
                            cur += ic->arg_slots[i];
                        }
                        frame->stack_top = (int16_t)(base - 1);

                        frame->pc += 2;   /* pc entered at opcode+1; invokevirtual is 3 bytes total */
                        JavaValue result;
                        int ret = execute_method(jvm, thread, ic->method, args, &result);
                        if (args != args_buf) free(args);
                        if (ret != 0) return -1;   /* pending_exception set by callee */
                        if (ic->method->is_native && ic->has_ret) {
                            PUSH(frame, result);
                            if (ic->ret_wide) PUSH(frame, result);
                        }
                        return 0;
                    }
                }
            }
        }
    }
    uint16_t index = FETCH_U2(frame);

    const char* class_name = NULL;
    const char* method_name = NULL;
    const char* descriptor = NULL;

    if (index > 0 && index < frame->clazz->constant_pool_count) {
        ConstantPoolEntry* entry = &frame->clazz->constant_pool[index];
        if (entry->tag == CONSTANT_Methodref) {
            uint16_t class_idx = entry->info.ref_info.class_index;
            uint16_t nat_idx = entry->info.ref_info.name_and_type_index;

            class_name = classfile_get_class_name(frame->clazz, class_idx);
            classfile_get_name_and_type(frame->clazz, nat_idx, &method_name, &descriptor);
        }
    }

    if (!descriptor || !method_name) {
        /* v9: also reject NULL method_name — downstream native_find() would
         * strcmp() it and MinGW printf would print it, both crash. */
        LOG_SAFE("[EXEC] FATAL: invokevirtual at index %d has unresolved name/type!\n", index);
        jvm_throw_by_name(jvm, "java/lang/NoSuchMethodError", "Unresolved virtual method ref");
        return -1;
    }

    invoke_watch_init();
    /* v34.9: also extract topInt for virtual calls (top of stack = last int
     * arg for (I...)V style signatures; for (Ls;Z)V the boolean is the top
     * int slot). Static calls did this from the start. */
    invoke_watch_hit(class_name, method_name, descriptor, "virtual", frame,
                     frame->stack_top >= 0 ? frame->stack[frame->stack_top].i : 0);

    int arg_count = count_args(descriptor);

    /* === FIX: Handle stack underflow gracefully ===
     * Some J2ME games have slightly invalid bytecode or rely on
     * implementation-specific stack behavior. Instead of aborting,
     * pad missing arguments with zeros and continue execution.
     * This lets the game continue past minor bytecode issues.
     * v9: bound the padding by max_stack — if arg_count+1 (args + this)
     * can never fit, the classfile is broken and padding would PUSH past
     * frame->stack (release PUSH is unchecked) -> VerifyError instead. */
    int available = frame->stack_top + 1;
    if (available < arg_count + 1) {
        if (arg_count + 1 > (int)frame->max_stack) {
            char msg[160];
            snprintf(msg, sizeof(msg),
                     "invokevirtual needs %d stack slots, max_stack=%d",
                     arg_count + 1, (int)frame->max_stack);
            LOG_SAFE("[EXEC] invokevirtual: %s\n", msg);
            jvm_throw_by_name(jvm, "java/lang/VerifyError", msg);
            return -1;
        }
        static int underflow_count = 0;
        if (underflow_count < 20) {
            underflow_count++;
            /* v34.4: richer context (caller frame, bytecode window, surviving
             * stack) - the plain line could not explain WHERE the missing
             * push was lost (user log game 2: Alert.setString "stack has 1"). */
            log_invoke_underflow_diag("invokevirtual", frame, class_name,
                                      method_name, descriptor, arg_count,
                                      available, 3);
        }
        /* Pad the stack with zero values so we can pop what we need */
        while (frame->stack_top + 1 < arg_count + 1) {
            JavaValue zero;
            memset(&zero, 0, sizeof(zero));
            PUSH(frame, zero);
        }
    }

    /* v18 FIX: compact args[] (args[0]=this + one entry per Java argument,
     * J/D included) — see count_java_args/pop_args_compact. The old
     * slot-wise pop shifted every argument after the first long/double. */
    int java_arg_count = count_java_args(descriptor);
    JavaValue* args = (JavaValue*)malloc((java_arg_count + 1) * sizeof(JavaValue));
    if (!args) return -1;
    
    /* Pop 'this' + arguments compactly */
    if (pop_args_compact(frame, descriptor, 1, args) < 0) {
        free(args);
        jvm_throw_by_name(jvm, "java/lang/OutOfMemoryError", "Args pop failed");
        return -1;
    }
    
    JavaObject* obj = (JavaObject*)args[0].ref;


    /* M3G TRACE: Always log bindTarget/render/releaseTarget (critical for debugging) */
    if (class_name && strstr(class_name, "m3g") && method_name) {
        if (strcmp(method_name, "bindTarget") == 0 || 
            strcmp(method_name, "render") == 0 ||
            strcmp(method_name, "releaseTarget") == 0 ||
            strcmp(method_name, "clear") == 0) {
#if M3G_DEBUG_LOG
            LOG_SAFE("[M3G-CRITICAL] invokevirtual: %s.%s%s (obj=%p)\n",
                    class_name, method_name, descriptor, (void*)obj);
#endif
        }
    }
    
    /* M3G TRACE: Log other m3g calls with limit */
    if (class_name && strstr(class_name, "m3g")) {
        static int m3g_invoke_count = 0;
        if (m3g_invoke_count < 5000) {
#if M3G_DEBUG_LOG
            LOG_SAFE("[M3G-TRACE] invokevirtual: %s.%s%s (obj=%p)\n",
                    class_name, method_name, descriptor, (void*)obj);
#endif
            m3g_invoke_count++;
        }
    }

    LOG_OPCODE("[EXEC] invokevirtual: %s.%s%s (obj=%p, args=%d)\n",
            class_name ? class_name : "?",
            method_name ? method_name : "?",
            descriptor ? descriptor : "?",
            (void*)obj, arg_count);

    /* Safety Checks */
    if (!obj) {
        
        /* COMPATIBILITY FIX: For String.equals() on null, throw NPE as per Java spec.
         * This is needed for String tests.
         */
        if (class_name && strcmp(class_name, "java/lang/String") == 0 &&
            method_name && strcmp(method_name, "equals") == 0 && 
            descriptor && strcmp(descriptor, "(Ljava/lang/Object;)Z") == 0) {
            LOG_SAFE("[NPE] String.equals() on null at PC=%d in %s.%s\n",
                    frame->throwing_pc,
                    frame->clazz ? (frame->clazz->class_name ? frame->clazz->class_name : "?") : "?",
                    frame->method ? (frame->method->name ? frame->method->name : "?") : "?");
            free(args);
            jvm_throw_by_name(jvm, "java/lang/NullPointerException", NULL);
            return -1;
        }
        
        /* COMPATIBILITY FIX: Some games call Object.equals() on null objects.
         * KEmulator and other emulators return false instead of throwing NPE.
         * For equals() method on non-String objects, we return false for compatibility.
         */
        if (method_name && strcmp(method_name, "equals") == 0 && 
            descriptor && strcmp(descriptor, "(Ljava/lang/Object;)Z") == 0) {
            /* Debug logging disabled */
            JavaValue false_val = { .i = 0 };
            PUSH(frame, false_val);
            free(args);
            return 0;
        }
        
        /* COMPATIBILITY FIX: Many J2ME games use pattern:
         *   Integer val = (Integer)hashtable.get(key);
         *   int v = val.intValue();  // expects 0 if val is null
         * KEmulator returns 0 for null.intValue() instead of NPE.
         * This is NOT correct Java but needed for game compatibility.
         */
        if (class_name && strcmp(class_name, "java/lang/Integer") == 0 &&
            method_name && strcmp(method_name, "intValue") == 0 &&
            descriptor && strcmp(descriptor, "()I") == 0) {
            /* Debug logging disabled */
            JavaValue zero_val = { .i = 0 };
            PUSH(frame, zero_val);
            free(args);
            return 0;
        }
        
        /* Same for Long.longValue() */
        if (class_name && strcmp(class_name, "java/lang/Long") == 0 &&
            method_name && strcmp(method_name, "longValue") == 0 &&
            descriptor && strcmp(descriptor, "()J") == 0) {
            /* Debug logging disabled */
            JavaValue zero_val = { .j = 0 };
            PUSH(frame, zero_val);
            free(args);
            return 0;
        }
        
        /* COMPATIBILITY FIX: Many J2ME games use Font without null checks.
         * Pattern: Font font = Font.getFont(...); font.stringWidth("text");
         * If Font.getFont() returns null (e.g., unsupported font), the game crashes.
         * KEmulator and other emulators return default values instead of NPE.
         * This is NOT correct Java but needed for game compatibility.
         */
        if (class_name && strcmp(class_name, "javax/microedition/lcdui/Font") == 0) {
            /* Default font dimensions (bitmap_font.h: FONT_WIDTH=5, FONT_HEIGHT=7).
             * v36.29 [FONT-HEIGHT]: getHeight() = glyph box + 2px LEADING = 9,
             * matching midp_font_height (graphics.c) — lines spaced by
             * getHeight() must not touch. Baseline (ascent) stays 5. */
            #define DEFAULT_FONT_WIDTH 5
            #define DEFAULT_FONT_HEIGHT 9
            #define DEFAULT_FONT_BASELINE 5
            
            /* Font.stringWidth(String) - return estimated width */
            if (method_name && strcmp(method_name, "stringWidth") == 0 &&
                descriptor && strcmp(descriptor, "(Ljava/lang/String;)I") == 0) {
                /* Debug logging disabled */
                /* Try to get string length for better estimate */
                jint width = 0;
                if (arg_count >= 1 && args[1].ref) {
                    JavaString* str = (JavaString*)args[1].ref;
                    const char* utf8 = string_utf8(jvm, str);
                    if (utf8) {
                        int len = 0;
                        for (int i = 0; utf8[i]; ) {
                            unsigned char c = (unsigned char)utf8[i];
                            if (c < 0x80) i++;
                            else if ((c & 0xE0) == 0xC0) i += 2;
                            else if ((c & 0xF0) == 0xE0) i += 3;
                            else i++;
                            len++;
                        }
                        width = len * (DEFAULT_FONT_WIDTH + 1);
                    }
                }
                JavaValue result = { .i = width };
                PUSH(frame, result);
                free(args);
                return 0;
            }
            
            /* Font.charWidth(char) - return default char width */
            if (method_name && strcmp(method_name, "charWidth") == 0 &&
                descriptor && strcmp(descriptor, "(C)I") == 0) {
                /* Debug logging disabled */
                JavaValue result = { .i = DEFAULT_FONT_WIDTH };
                PUSH(frame, result);
                free(args);
                return 0;
            }
            
            /* Font.charsWidth(char[], int, int) - return estimated width */
            if (method_name && strcmp(method_name, "charsWidth") == 0 &&
                descriptor && strcmp(descriptor, "([CII)I") == 0) {
                /* Debug logging disabled */
                jint width = 0;
                if (arg_count >= 3 && args[1].ref) {
                    JavaArray* arr = (JavaArray*)args[1].ref;
                    jint offset = args[2].i;
                    jint length = args[3].i;
                    if (arr && offset >= 0 && length >= 0 && offset + length <= (jint)arr->length) {
                        width = length * (DEFAULT_FONT_WIDTH + 1);
                    }
                }
                JavaValue result = { .i = width };
                PUSH(frame, result);
                free(args);
                return 0;
            }
            
            /* Font.substringWidth(String, int, int) - return estimated width */
            if (method_name && strcmp(method_name, "substringWidth") == 0 &&
                descriptor && strcmp(descriptor, "(Ljava/lang/String;II)I") == 0) {
                /* Debug logging disabled */
                jint width = 0;
                if (arg_count >= 4 && args[3].i > 0) {
                    width = args[3].i * (DEFAULT_FONT_WIDTH + 1);  /* length * char_width */
                }
                JavaValue result = { .i = width };
                PUSH(frame, result);
                free(args);
                return 0;
            }
            
            /* Font.getHeight() - return default font height */
            if (method_name && strcmp(method_name, "getHeight") == 0 &&
                descriptor && strcmp(descriptor, "()I") == 0) {
                /* Debug logging disabled */
                JavaValue result = { .i = DEFAULT_FONT_HEIGHT };
                PUSH(frame, result);
                free(args);
                return 0;
            }
            
            /* Font.getBaselinePosition() - return default baseline */
            if (method_name && strcmp(method_name, "getBaselinePosition") == 0 &&
                descriptor && strcmp(descriptor, "()I") == 0) {
                /* Debug logging disabled */
                JavaValue result = { .i = DEFAULT_FONT_BASELINE };
                PUSH(frame, result);
                free(args);
                return 0;
            }
            
            /* Font.getFace(), getStyle(), getSize() - return default values */
            if (method_name && strcmp(method_name, "getFace") == 0 &&
                descriptor && strcmp(descriptor, "()I") == 0) {
                JavaValue result = { .i = 0 };  /* FACE_SYSTEM */
                PUSH(frame, result);
                free(args);
                return 0;
            }
            
            if (method_name && strcmp(method_name, "getStyle") == 0 &&
                descriptor && strcmp(descriptor, "()I") == 0) {
                JavaValue result = { .i = 0 };  /* STYLE_PLAIN */
                PUSH(frame, result);
                free(args);
                return 0;
            }
            
            if (method_name && strcmp(method_name, "getSize") == 0 &&
                descriptor && strcmp(descriptor, "()I") == 0) {
                JavaValue result = { .i = 8 };  /* SIZE_MEDIUM (typically 8) */
                PUSH(frame, result);
                free(args);
                return 0;
            }
            
            /* Font.isPlain(), isBold(), isItalic(), isUnderlined() - return false for null */
            if (method_name && (strcmp(method_name, "isPlain") == 0 ||
                               strcmp(method_name, "isBold") == 0 ||
                               strcmp(method_name, "isItalic") == 0 ||
                               strcmp(method_name, "isUnderlined") == 0) &&
                descriptor && strcmp(descriptor, "()Z") == 0) {
                JavaValue result = { .i = 0 };  /* false */
                PUSH(frame, result);
                free(args);
                return 0;
            }
            
            /* For other Font methods on null, still throw NPE */
            static int npe_font_count = 0;
            if (npe_font_count < 5) {
                LOG_SAFE("[NPE] Font method on null: %s.%s%s\n",
                        class_name ? class_name : "?", method_name ? method_name : "?", descriptor ? descriptor : "?");
                npe_font_count++;
            }
            free(args);
            jvm_throw_by_name(jvm, "java/lang/NullPointerException", NULL);
            return -1;
        }
        
        /* CORRECT Java BEHAVIOR: String methods on null throw NullPointerException.
         * The J2ME spec requires this. Tests verify it. */
        if (class_name && strcmp(class_name, "java/lang/String") == 0) {
            free(args);
            jvm_throw_by_name(jvm, "java/lang/NullPointerException", NULL);
            return -1;
        }
        
        /* COMPATIBILITY FIX: Collection/Map methods on null
         * Many midlets call .get(), .size(), .put(), .isEmpty() on references
         * that may be null when APIs like System.getProperties() aren't implemented.
         * Return safe defaults instead of throwing NPE (KEmulator compat).
         */
        if (class_name && (strcmp(class_name, "java/util/Hashtable") == 0 ||
                           strcmp(class_name, "java/util/HashMap") == 0 ||
                           strcmp(class_name, "java/util/Vector") == 0 ||
                           strcmp(class_name, "java/util/Stack") == 0 ||
                           strcmp(class_name, "java/util/ArrayList") == 0)) {
            /* .get(key) / .elementAt(index) / .remove(key) → return null */
            if (method_name && (strcmp(method_name, "get") == 0 ||
                               strcmp(method_name, "elementAt") == 0 ||
                               strcmp(method_name, "remove") == 0)) {
                DISP_DEBUG("[NPE-SAFE] %s.%s on null → null", class_name, method_name);
                JavaValue result = { .ref = NULL };
                PUSH(frame, result);
                free(args);
                return 0;
            }
            /* .size() / .getSize() → return 0 */
            if (method_name && (strcmp(method_name, "size") == 0 ||
                               strcmp(method_name, "getSize") == 0)) {
                DISP_DEBUG("[NPE-SAFE] %s.%s on null → 0", class_name, method_name);
                JavaValue result = { .i = 0 };
                PUSH(frame, result);
                free(args);
                return 0;
            }
            /* .isEmpty() → return true */
            if (method_name && strcmp(method_name, "isEmpty") == 0) {
                DISP_DEBUG("[NPE-SAFE] %s.isEmpty on null → true", class_name);
                JavaValue result = { .i = 1 };  /* true */
                PUSH(frame, result);
                free(args);
                return 0;
            }
            /* .put() / .addElement() / .add() → no-op, return null/old value */
            if (method_name && (strcmp(method_name, "put") == 0 ||
                               strcmp(method_name, "addElement") == 0 ||
                               strcmp(method_name, "add") == 0 ||
                               strcmp(method_name, "contains") == 0 ||
                               strcmp(method_name, "containsKey") == 0)) {
                DISP_DEBUG("[NPE-SAFE] %s.%s on null → safe default", class_name, method_name);
                JavaValue result = { .ref = NULL };
                /* put returns old value (null), add/contains return boolean/int */
                const char* ret = descriptor ? strchr(descriptor, ')') : NULL;
                if (ret && ret[1] == 'Z') {
                    result.i = 0;  /* false */
                } else if (ret && (ret[1] == 'I' || ret[1] == 'B' || ret[1] == 'S')) {
                    result.i = 0;
                }
                PUSH(frame, result);
                free(args);
                return 0;
            }
            /* .toString() → return "null" */
            if (method_name && strcmp(method_name, "toString") == 0 &&
                descriptor && strcmp(descriptor, "()Ljava/lang/String;") == 0) {
                JavaValue result = { .ref = jvm_new_string(jvm, "null") };
                PUSH(frame, result);
                free(args);
                return 0;
            }
            /* .keys() / .elements() / .iterator() → return null */
            if (method_name && (strcmp(method_name, "keys") == 0 ||
                               strcmp(method_name, "elements") == 0 ||
                               strcmp(method_name, "iterator") == 0)) {
                JavaValue result = { .ref = NULL };
                PUSH(frame, result);
                free(args);
                return 0;
            }
            /* Other methods on null collections - return safe default based on return type */
            if (descriptor) {
                char last_char = descriptor[strlen(descriptor) - 1];
                if (last_char == ';' || last_char == 'L') {
                    JavaValue result = { .ref = NULL };
                    PUSH(frame, result);
                    free(args);
                    return 0;
                } else {
                    JavaValue result = { .i = 0 };
                    PUSH(frame, result);
                    free(args);
                    return 0;
                }
            }
        }
        
        /* COMPATIBILITY FIX: Form/List methods on null
         * Form.size(), Form.append(), List.size() etc. called on null Form
         * when constructors fail or System.getProperties() returns null cascade.
         */
        if (class_name && (strcmp(class_name, "javax/microedition/lcdui/Form") == 0 ||
                           strcmp(class_name, "javax/microedition/lcdui/List") == 0 ||
                           strcmp(class_name, "javax/microedition/lcdui/Alert") == 0)) {
            /* .size() → return 0 */
            if (method_name && strcmp(method_name, "size") == 0 &&
                descriptor && strcmp(descriptor, "()I") == 0) {
                DISP_DEBUG("[NPE-SAFE] %s.size on null → 0", class_name);
                JavaValue result = { .i = 0 };
                PUSH(frame, result);
                free(args);
                return 0;
            }
            /* .append() / .delete() / .deleteAll() → no-op */
            if (method_name && (strcmp(method_name, "append") == 0 ||
                               strcmp(method_name, "delete") == 0 ||
                               strcmp(method_name, "deleteAll") == 0 ||
                               strcmp(method_name, "set") == 0 ||
                               strcmp(method_name, "insert") == 0)) {
                DISP_DEBUG("[NPE-SAFE] %s.%s on null → no-op", class_name, method_name);
                /* Return type varies: append returns int for some overloads */
                const char* ret = descriptor ? strchr(descriptor, ')') : NULL;
                if (ret && (ret[1] == 'I' || ret[1] == 'Z')) {
                    JavaValue result = { .i = 0 };
                    PUSH(frame, result);
                }
                free(args);
                return 0;
            }
            /* .getTitle() / .getString() → return null */
            if (method_name && (strcmp(method_name, "getTitle") == 0 ||
                               strcmp(method_name, "getString") == 0 ||
                               strcmp(method_name, "getSelectedIndex") == 0)) {
                JavaValue result = { .ref = NULL };
                PUSH(frame, result);
                free(args);
                return 0;
            }
        }
        
        /* COMPATIBILITY FIX: Universal DRM bypass
         * Any class registered in the DRM bypass registry will have its
         * null-object method calls return safe defaults, preventing NPE crashes.
         * This matches KEmulator behavior for various protection systems.
         * To add a new DRM system, edit drm_bypass.c: g_drm_bypass_prefixes[]
         */
        if (class_name && drm_is_drm_class(class_name)) {
            /* info() methods - return null */
            if (method_name && strcmp(method_name, "info") == 0 &&
                descriptor && strcmp(descriptor, "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;") == 0) {
                JavaValue result = { .ref = NULL };
                PUSH(frame, result);
                free(args);
                return 0;
            }
            
            /* prefix_bonus() - return null */
            if (method_name && strcmp(method_name, "prefix_bonus") == 0 &&
                descriptor && strcmp(descriptor, "(ILjava/lang/String;)Ljava/lang/String;") == 0) {
                JavaValue result = { .ref = NULL };
                PUSH(frame, result);
                free(args);
                return 0;
            }
            
            /* Other GlomoReg methods on null - return safe default */
            if (descriptor) {
                char last_char = descriptor[strlen(descriptor) - 1];
                if (last_char == ';') {
                    JavaValue result = { .ref = NULL };
                    PUSH(frame, result);
                    free(args);
                    return 0;
                } else if (last_char == 'Z') {
                    JavaValue result = { .i = 0 };
                    PUSH(frame, result);
                    free(args);
                    return 0;
                } else if (last_char == 'I' || last_char == 'S' || last_char == 'B' || last_char == 'C') {
                    JavaValue result = { .i = 0 };
                    PUSH(frame, result);
                    free(args);
                    return 0;
                }
            }
        }
        
        /* Throw NPE for other null method calls - correct Java behavior.
         * v34.4: carry a MESSAGE - "(no message)" NPEs in game logs told us
         * nothing about WHICH reference was null (user log game 1:
         * b.paint PC=13 -> "NullPointerException (no message)"). */
        static int npe_invokevirtual_count = 0;
        if (npe_invokevirtual_count < 10) {
            LOG_SAFE("[NPE] invokevirtual on null object: %s.%s%s\n",
                    class_name ? class_name : "?", 
                    method_name ? method_name : "?", 
                    descriptor ? descriptor : "?");
            npe_invokevirtual_count++;
            if (npe_invokevirtual_count == 10) {
                LOG_SAFE("[NPE] Further invokevirtual NPE logging suppressed\n");
            }
        }
        free(args);
        {
            char npe_msg[192];
            snprintf(npe_msg, sizeof(npe_msg),
                     "Null receiver calling %s.%s%s",
                     class_name ? class_name : "?",
                     method_name ? method_name : "?",
                     descriptor ? descriptor : "?");
            jvm_throw_by_name(jvm, "java/lang/NullPointerException", npe_msg);
        }
        return -1;
    }
    
    /* Проверка что объект находится в куче ИЛИ это JavaClass* (Class object) */
    bool obj_is_class = false;  /* Flag to track if this is a Class object */
    if (!is_heap_ptr_check(obj)) {
        /* Check if this is a JavaClass* acting as a Class object */
        /* JavaClass structs have header.clazz pointing to java/lang/Class */
        /* [ARGGUARD] v36.49: не-кучный obj может быть МУСОРОМ (не-каноничен/
         * не выровнен) — проба Class-объекта deref-ит [obj+0]; сначала
         * правдоподобие, проба только для правдоподобных указателей. */
        JavaClass* potential_class = (JavaClass*)obj;
        if (!argguard_bad_ptr((uintptr_t)obj) && potential_class->header.clazz && 
            potential_class->header.clazz->class_name &&
            strcmp(potential_class->header.clazz->class_name, "java/lang/Class") == 0) {
            /* This is a valid JavaClass* acting as a Class object - allow it */
            obj_is_class = true;
        } else {
            LOG_SAFE("[EXEC] invokevirtual: FATAL: Object %p is NOT in heap! (heap: %p - %p)\n", 
                    (void*)obj, g_heap_start, g_heap_end);
            LOG_SAFE("[EXEC]   Method: %s.%s%s\n", 
                    class_name ? class_name : "?", method_name ? method_name : "?", descriptor);
            free(args);
            return -1;
        }
    }
    
    /* Проверка на повреждённый или освобождённый объект */
    /* First check if clazz pointer itself is valid before dereferencing */
    if (!obj->header.clazz || (uintptr_t)obj->header.clazz < 0x10000) {
        LOG_SAFE("[EXEC] invokevirtual: FATAL: Object %p has NULL or invalid class pointer %p\n", 
                (void*)obj, (void*)obj->header.clazz);
        LOG_SAFE("[EXEC]   Method: %s.%s%s\n", 
                class_name ? class_name : "?", method_name ? method_name : "?", descriptor);
        free(args);
        return -1;
    }
    
    /* Check if clazz is a valid JavaClass pointer - it should either be in heap OR be a malloc'd JavaClass */
    /* JavaClass* pointers are malloc'd, not in JVM heap */
    JavaClass* clazz_ptr = obj->header.clazz;
    
    /* Verify the GC header of the object to check if it's valid (only for heap objects, not Class objects) */
    if (!obj_is_class) {
        GCObjectHeader* obj_gc_hdr = (GCObjectHeader*)obj - 1;
        if (!is_heap_ptr_check(obj_gc_hdr)) {
            LOG_SAFE("[EXEC] invokevirtual: FATAL: Object %p has invalid GC header %p (not in heap)\n", 
                    (void*)obj, (void*)obj_gc_hdr);
            LOG_SAFE("[EXEC]   Method: %s.%s%s\n", 
                    class_name ? class_name : "?", method_name ? method_name : "?", descriptor);
            free(args);
            return -1;
        }
        
        /* Check if the object was freed (type == OBJ_TYPE_FREE) */
        if (obj_gc_hdr->type == OBJ_TYPE_FREE) {
            LOG_SAFE("[EXEC] invokevirtual: FATAL: Object %p was already freed (GC header type=FREE)\n", 
                    (void*)obj);
            LOG_SAFE("[EXEC]   Method: %s.%s%s\n", 
                    class_name ? class_name : "?", method_name ? method_name : "?", descriptor);
            free(args);
            return -1;
        }
        
        /* Check magic number to detect memory corruption */
        if (obj_gc_hdr->magic != GC_HEADER_MAGIC) {
            LOG_SAFE("[EXEC] invokevirtual: FATAL: Object %p has corrupted magic: expected 0x%08X, got 0x%08X\n",
                    (void*)obj, GC_HEADER_MAGIC, obj_gc_hdr->magic);
            LOG_SAFE("[EXEC]   Method: %s.%s%s\n", 
                    class_name ? class_name : "?", method_name ? method_name : "?", descriptor);
            free(args);
            return -1;
        }
        
        /* Check if the object's clazz in GC header matches the object's header.clazz */
        if (obj_gc_hdr->clazz != clazz_ptr) {
            LOG_SAFE("[EXEC] invokevirtual: FATAL: Object %p class pointer mismatch: obj->header.clazz=%p, gc_hdr->clazz=%p\n", 
                    (void*)obj, (void*)clazz_ptr, (void*)obj_gc_hdr->clazz);
            LOG_SAFE("[EXEC]   GC header type=%d, clazz=%p\n", obj_gc_hdr->type, (void*)obj_gc_hdr->clazz);
            LOG_SAFE("[EXEC]   Method: %s.%s%s\n", 
                    class_name ? class_name : "?", method_name ? method_name : "?", descriptor);
            
            /* Check if gc_hdr->clazz looks like a valid pointer (not a corrupted value) */
            /* Valid pointers should be > 0x10000 and < 0x7fff00000000 for user space on 64-bit */
            /* Also check that it's not obviously UTF-16 data (common corruption pattern) */
            /* UTF-16 strings have pattern 0x00XX00YY00ZZ... where every other byte is 0x00 */
            uintptr_t clazz_addr = (uintptr_t)obj_gc_hdr->clazz;
            
            /* Check if this looks like UTF-16 data: high byte of each 16-bit word is 0 */
            /* Pattern: for addr like 0x003200650064002f, bytes are 00 32 00 65 00 64 00 2f */
            /* We check if every other byte (positions 1, 3, 5, 7) is 0x00 */
            uint64_t a64 = (uint64_t)clazz_addr;   /* wide type: shifts up to 56 are legal on 32-bit targets too */
            bool looks_like_utf16 = (
                ((a64 >> 8) & 0xFF) == 0x00 ||  /* byte 1 */
                ((a64 >> 24) & 0xFF) == 0x00 || /* byte 3 */
                ((a64 >> 40) & 0xFF) == 0x00 || /* byte 5 */
                ((a64 >> 56) & 0xFF) == 0x00    /* byte 7 */
            );
            
            /* Also check if value looks too small to be a pointer (but could be a valid small int) */
            bool is_small_value = clazz_addr != 0 && clazz_addr < 0x100000;
            
            /* Compare in uint64_t (a64): on 32-bit targets (MinGW32/ARM) clazz_addr
             * is 32-bit, so the raw < 0x7fff00000000ULL check is always-true and
             * trips -Wtype-limits under -Wextra. a64 is the same value, widened. */
            if (obj_gc_hdr->clazz && a64 > 0x10000 && a64 < 0x7fff00000000ULL && !looks_like_utf16 && !is_small_value) {
                /* Check if it might be a valid JavaClass by checking if it's in heap */
                if (is_heap_ptr_check(obj_gc_hdr->clazz)) {
                    clazz_ptr = obj_gc_hdr->clazz;
                } else {
                    /* Not in heap - assume it's a valid JavaClass* */
                    /* This is risky but necessary for Class objects allocated via malloc */
                    clazz_ptr = obj_gc_hdr->clazz;
                }
            } else {
                LOG_SAFE("[EXEC] invokevirtual: GC header clazz appears corrupted (addr=0x%llx, utf16=%d, small=%d), cannot recover\n", 
                        (unsigned long long)clazz_addr, looks_like_utf16, is_small_value);
                free(args);
                return -1;
            }
        }
    }
    
    /* Now check if class_name is accessible - clazz_ptr should be a valid JavaClass* */
    if (!clazz_ptr->class_name) {
        LOG_SAFE("[EXEC] invokevirtual: FATAL: Object %p has class with NULL class_name (clazz=%p)\n", 
                (void*)obj, (void*)clazz_ptr);
        LOG_SAFE("[EXEC]   Method: %s.%s%s\n", 
                class_name ? class_name : "?", method_name ? method_name : "?", descriptor);
        free(args);
        return -1;
    }
    
    /* Resolve and Execute */
    JavaClass* target_class = obj->header.clazz;
    JavaMethod* method = jvm_resolve_method(jvm, target_class, method_name, descriptor);

    /* DEBUG: Track ALL invokevirtual calls around PC 4520-4535 */
    if (g_j2me_runtime_debug && frame->clazz && frame->clazz->class_name && strcmp(frame->clazz->class_name, "TestCanvas") == 0 &&
        frame->method && frame->method->name && strcmp(frame->method->name, "runAllTests") == 0 &&
        frame->throwing_pc >= 4460 && frame->throwing_pc <= 4540) {
        if (g_j2me_runtime_debug) LOG_SAFE("[INVOKEVIRTUAL_TRACE] >>>ENTER instr_pc=%d (throwing_pc), method=%s, descriptor=%s\n",
                frame->throwing_pc, method_name ? method_name : "?", descriptor ? descriptor : "?");
        if (g_j2me_runtime_debug) LOG_SAFE("[INVOKEVIRTUAL_TRACE] stack_top=%d\n", frame->stack_top);
        for (int i = 0; i <= frame->stack_top && i < 6; i++) {
            if (g_j2me_runtime_debug) LOG_SAFE("[INVOKEVIRTUAL_TRACE]   stack[%d] = %p\n", i, (void*)frame->stack[i].ref);
        }
    }

    /* DEBUG: Track method resolution for InterfaceFactory */
    if (g_j2me_runtime_debug && method_name && strcmp(method_name, "create") == 0) {
        if (g_j2me_runtime_debug) LOG_SAFE("[DEBUG_RESOLVE] Method %s.%s%s: method=%p, target_class=%s\n",
                class_name ? class_name : "?", method_name, descriptor,
                (void*)method, target_class ? target_class->class_name : "NULL");
        if (target_class) {
            if (g_j2me_runtime_debug) LOG_SAFE("[DEBUG_RESOLVE] target_class has %d methods:\n", target_class->methods_count);
            for (int i = 0; i < target_class->methods_count; i++) {
                LOG_SAFE("  [%d] %s%s\n", i, 
                        target_class->methods[i].name ? target_class->methods[i].name : "?",
                        target_class->methods[i].descriptor ? target_class->methods[i].descriptor : "?");
            }
        }
    }

    if (!method) {
        /* M3G-DEBUG: Log when method resolution failed */
        if (class_name && strstr(class_name, "m3g") && method_name) {
#if M3G_DEBUG_LOG
            LOG_SAFE("[M3G-DISPATCH] method=NULL, trying native_find for %s.%s%s (target=%s)\n",
                    class_name, method_name, descriptor, target_class ? target_class->class_name : "null");
#endif
        }
        /* Try native */
        NativeMethod native = NULL;
        JavaClass* search_class = target_class;
        while (search_class && !native) {
            native = native_find(jvm, search_class->class_name, method_name, descriptor);
            if (!native) search_class = search_class->super_class;
        }
        /* v34.76: descriptor-relaxed second pass (exact chain missed).
         * Legacy games compiled against non-standard API stubs (Roboros:
         * void LayerManager.append) land here; args section must match
         * byte-for-byte, only the return type may differ. Result push
         * below is driven by the CALLER's descriptor, so a ")V" caller
         * of a ")I" native simply discards the value. */
        if (!native) {
            JavaClass* relax_class = target_class;
            while (relax_class && !native) {
                native = native_find_relaxed(jvm, relax_class->class_name, method_name, descriptor);
                if (!native) relax_class = relax_class->super_class;
            }
        }
        /* M3G-DEBUG: Log native_find result */
        if (class_name && strstr(class_name, "m3g") && method_name) {
#if M3G_DEBUG_LOG
            LOG_SAFE("[M3G-DISPATCH] native_find result=%p for %s.%s%s\n",
                    (void*)native, class_name, method_name, descriptor);
#endif
        }

        /* DEBUG: Track when method not found */
        if (g_j2me_runtime_debug && method_name && strcmp(method_name, "create") == 0) {
            if (g_j2me_runtime_debug) LOG_SAFE("[DEBUG_RESOLVE] Method not found! native=%p\n", (void*)native);
        }

        if (native) {
            /* M3G-DEBUG: Log M3G native dispatch */
            if (class_name && strstr(class_name, "m3g")) {
#if M3G_DEBUG_LOG
                LOG_SAFE("[M3G-DISPATCH] Calling native handler for %s.%s%s\n", class_name, method_name, descriptor);
#endif
            }
            /* NATIVE_CALL logging removed for performance */
            
            /* DIAG: Log field count mismatch for M3G native methods.
             * Compare descriptor's expected arg count with actual arg_count.
             * Descriptor format: (args)return — parse types between ( and ). */
            if (class_name && strstr(class_name, "m3g") && method_name && descriptor) {
                const char* d = descriptor;
                if (*d == '(') {
                    d++; /* skip ( */
                    int expected_args = 0;
                    while (*d && *d != ')') {
                        if (*d == 'L') { d++; while (*d && *d != ';') d++; if (*d) d++; expected_args++; } /* skip L..; */
                        else if (*d == '[') { d++; /* skip [, count the base type on next iter */ }
                        else { expected_args++; d++; } /* primitive: B,C,D,F,I,J,S,Z */
                    }
                    /* +1 for 'this' (instance ref) */
                    int total_expected = expected_args + 1;
                    int actual_total = arg_count + 1;
                    if (actual_total < total_expected) {
#if M3G_DEBUG_LOG
                        LOG_SAFE("[M3G-ARG-COUNT-WARN] %s.%s%s: expected %d args (incl this), got %d (missing %d)\n",
                                class_name, method_name, descriptor, total_expected, actual_total, total_expected - actual_total);
#endif
                    }
                }
            }
            
            /* Clear any stale exception before calling native */
            if (thread->pending_exception) {
                LOG_DEBUG("[INVOKEVIRTUAL] Clearing stale exception before %s.%s%s\n",
                        class_name, method_name, descriptor);
                thread->pending_exception = NULL;
            }
            
            /* PINNING: Pin all object arguments before calling native method
             * This prevents GC from moving/collecting them during native execution */
            for (int pi = 0; pi <= java_arg_count; pi++) {
                if (args[pi].ref && is_heap_ptr_check(args[pi].ref)) {
                    gc_pin(jvm, args[pi].ref);
                }
            }
            
            /* v34.14 NATIVE-SCOPE AUTO-PIN: objects allocated INSIDE the
             * native (held only in C locals) are born pinned; the pins are
             * released together when the native returns. */
            extern __thread int g_gc_in_native;
            extern size_t gc_autopin_base(void);
            extern void gc_autopin_release(size_t base);
            size_t ap_base = gc_autopin_base();
            g_gc_in_native++;
            
            extern void jvm_note_native_call(JavaThread*, const char*, const char*);
            jvm_note_native_call(thread, class_name, method_name);
            /* [ARGGUARD] v36.49: валидация ref-слотов (malloc-массив,
             * defuse безопасен) + возврата native */
            argguard_check_array(class_name, method_name, descriptor, args,
                                 java_arg_count + 1, 1,
                                 __builtin_return_address(0), 1);
            int naf_slot = nargs_root_push(thread, args, java_arg_count + 1);
            JavaValue result = native(jvm, thread, args, java_arg_count + 1);
            argguard_check_ret(class_name, method_name, descriptor, &result,
                               __builtin_return_address(0));
            nargs_root_pop(thread, naf_slot);
            g_gc_in_native--;
            
            /* UNPINNING: Unpin all object arguments after native method returns */
            for (int pi = 0; pi <= java_arg_count; pi++) {
                if (args[pi].ref && is_heap_ptr_check(args[pi].ref)) {
                    gc_unpin(jvm, args[pi].ref);
                }
            }
            gc_autopin_release(ap_base);
            
            free(args);
            
            if (thread->pending_exception) return -1;
            
            if (strlen(descriptor) > 0) {
                const char* ret = strchr(descriptor, ')');
                if (ret && ret[1] != 'V') {
                    PUSH(frame, result);
                    if (ret[1] == 'J' || ret[1] == 'D') PUSH(frame, result);
                }
            }
            return 0;
        }
        
        /* Stub fallback */
        LOG_OPCODE("[EXEC] invokevirtual: method %s.%s%s not found (stub?), using default\n",
                target_class->class_name, method_name, descriptor);

        /* v34.6: unhooked API calls are SILENT feature killers for EVERY API
         * family, not just M3G (Doom RPG: Graphics.drawRGB registered with a
         * 7-param descriptor while the game called the 8-param one - the 3D
         * viewport silently never rendered). Surface ALL of them loudly,
         * throttled. */
        {
            static int inv_missing_count = 0;
            if (inv_missing_count < 60) {
                inv_missing_count++;
                MISSING_LOG("[INVOKE-MISSING] virtual: %s.%s%s (caller=%s.%s pc=%u) - stub no-op, default pushed\n",
                         target_class->class_name ? target_class->class_name : "?",
                         method_name ? method_name : "?", descriptor ? descriptor : "?",
                         (frame->clazz && frame->clazz->class_name) ? frame->clazz->class_name : "?",
                         (frame->method && frame->method->name) ? frame->method->name : "?",
                         (unsigned)frame->pc);
            }
        }
        
        /* v11 DIAG: unhooked M3G API calls are SILENT feature killers (they
         * return defaults and the game skips/loses 3D). Surface them loudly,
         * throttled, so a game log pinpoints exactly which signature to add. */
        if (target_class->class_name && method_name && descriptor &&
            (strstr(target_class->class_name, "m3g/") != NULL ||
             strstr(target_class->class_name, "micro3d") != NULL)) {
            static int m3g_missing_count = 0;
            if (m3g_missing_count < 40) {
                m3g_missing_count++;
                MISSING_LOG("[M3G-MISSING] unresolved instance call: %s.%s%s\n",
                        target_class->class_name, method_name, descriptor);
            }
        }
        
        if (strlen(descriptor) > 0) {
            const char* ret = strchr(descriptor, ')');
            if (ret && ret[1] != 'V') {
                JavaValue default_val = { .raw = 0 };
                PUSH(frame, default_val);
                if (ret[1] == 'J' || ret[1] == 'D') PUSH(frame, default_val);
            }
        }
        free(args);
        return 0;
    }
    
    /* v34.35 PERF: fill the inline cache (monomorphic on the receiver's
     * runtime class — exactly what jvm_resolve_method dispatched on).
     * Methods with >11 Java args or unresolved stubs stay uncached. */
    {
        int jac = count_java_args(descriptor);
        if (jac >= 0 && jac <= 11 && method->code.code && method->code.code_length > 0) {
            InvokeRefCache* ic = invoke_cache_entry(frame, index);
            if (ic) {
                /* arg slot layout from the descriptor (J/D = 2 slots) */
                const char* dp = descriptor;
                if (*dp == '(') dp++;
                int ai = 0, slots_ok = 1;
                while (*dp && *dp != ')' && ai < jac) {
                    if (*dp == 'J' || *dp == 'D') { ic->arg_slots[ai++] = 2; dp++; }
                    else if (*dp == 'L') { ic->arg_slots[ai++] = 1; while (*dp && *dp != ';') dp++; if (*dp) dp++; }
                    else if (*dp == '[') { ic->arg_slots[ai] = 1; dp++; if (*dp == 'L') { while (*dp && *dp != ';') dp++; if (*dp) dp++; } else if (*dp) dp++; ai++; }
                    else { ic->arg_slots[ai++] = 1; dp++; }
                }
                if (ai != jac || (*dp != ')')) slots_ok = 0;
                if (slots_ok) {
                    ic->target_class = obj->header.clazz;
                    ic->method = method;
                    ic->java_argc = (int16_t)jac;
                    ic->gen = g_vm_cache_gen;
                    ic->kind = 0;
                    const char* rp = strchr(descriptor, ')');
                    ic->has_ret = (rp && rp[1] != 'V' && rp[1] != '\0') ? 1 : 0;
                    ic->ret_wide = (rp && (rp[1] == 'J' || rp[1] == 'D')) ? 1 : 0;
                    ic->valid = 1;
                }
            }
        }
    }

    JavaValue result;
    int ret = execute_method(jvm, thread, method, args, &result);

    /* DEBUG: After execute_method for create method */
    if (g_j2me_runtime_debug && method_name && strcmp(method_name, "create") == 0) {
        if (g_j2me_runtime_debug) LOG_SAFE("[DEBUG_POST_EXECUTE] PC=%d After execute_method for %s: ret=%d, is_native=%d\n",
                frame->pc, method_name, ret, method->is_native);
        if (g_j2me_runtime_debug) LOG_SAFE("[DEBUG_POST_EXECUTE] frame=%p, frame->stack_top=%d\n",
                (void*)frame, frame->stack_top);
        if (frame->stack_top >= 0) {
            if (g_j2me_runtime_debug) LOG_SAFE("[DEBUG_POST_EXECUTE] frame->stack[top]=%p\n",
                    (void*)frame->stack[frame->stack_top].ref);
        }
    }

    /* DEBUG: Track ALL invokevirtual returns around PC 4460-4540 */
    if (g_j2me_runtime_debug && frame->clazz && frame->clazz->class_name && strcmp(frame->clazz->class_name, "TestCanvas") == 0 &&
        frame->method && frame->method->name && strcmp(frame->method->name, "runAllTests") == 0 &&
        frame->pc >= 4460 && frame->pc <= 4540) {
        if (g_j2me_runtime_debug) LOG_SAFE("[INVOKEVIRTUAL_EXIT] PC=%d method=%s ret=%d stack_top=%d\n",
                frame->pc, method_name ? method_name : "?", ret, frame->stack_top);
        if (g_j2me_runtime_debug) LOG_SAFE("[INVOKEVIRTUAL_EXIT] stack[0]=%p [1]=%p [2]=%p [3]=%p\n",
                (void*)frame->stack[0].ref, (void*)frame->stack[1].ref,
                (void*)frame->stack[2].ref, (void*)frame->stack[3].ref);
    }

    /* NOTE: For Java methods, result is already pushed to caller's stack by op_areturn/etc.
     * For native methods, result is filled in the result parameter and needs to be pushed.
     * Native methods have method->is_native set to true.
     */
    if (method->is_native && ret == 0) {
        /* Push result for native methods */
        if (strlen(descriptor) > 0) {
            const char* ret_type = strchr(descriptor, ')');
            if (ret_type && ret_type[1] != 'V') {
                PUSH(frame, result);
                if (ret_type[1] == 'J' || ret_type[1] == 'D') {
                    PUSH(frame, result);  /* Long/double need 2 slots */
                }
            }
        }
    }

    /* DEBUG: Final check before return from invokevirtual for create */
    if (g_j2me_runtime_debug && method_name && strcmp(method_name, "create") == 0) {
        if (g_j2me_runtime_debug) LOG_SAFE("[DEBUG_PRE_RETURN] PC=%d About to return from invokevirtual for %s\n", 
                frame->pc - 3, method_name);  /* PC-3 because we already fetched the opcode and cp_index */
        if (g_j2me_runtime_debug) LOG_SAFE("[DEBUG_PRE_RETURN] frame=%p, frame->stack=%p, stack_top=%d\n",
                (void*)frame, (void*)frame->stack, frame->stack_top);
        for (int i = 0; i <= frame->stack_top; i++) {
            if (g_j2me_runtime_debug) LOG_SAFE("[DEBUG_PRE_RETURN]   stack[%d] = %p\n", i, (void*)frame->stack[i].ref);
        }
        if (g_j2me_runtime_debug) LOG_SAFE("[DEBUG_PRE_RETURN] args=%p, about to free(args)\n", (void*)args);
    }

    free(args);

    /* DEBUG: Check if free(args) corrupted stack */
    if (g_j2me_runtime_debug && method_name && strcmp(method_name, "create") == 0) {
        if (g_j2me_runtime_debug) LOG_SAFE("[DEBUG_AFTER_FREE] PC=%d After free(args): frame=%p, stack_top=%d\n",
                frame->pc - 3, (void*)frame, frame->stack_top);
        for (int i = 0; i <= frame->stack_top; i++) {
            if (g_j2me_runtime_debug) LOG_SAFE("[DEBUG_AFTER_FREE]   stack[%d] = %p\n", i, (void*)frame->stack[i].ref);
        }
    }

    return ret;
}

int op_invokespecial(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)thread; (void)frame;
    uint16_t index = FETCH_U2(frame);
    
    /* Get method reference */
    const char* class_name = NULL;
    const char* method_name = NULL;
    const char* descriptor = NULL;
    {
        /* v34.17: cover invokespecial in the INVOKE-WATCH tracer (private
         * render methods of NET Lizard games are invokespecial targets) */
        if (index > 0 && index < frame->clazz->constant_pool_count) {
            ConstantPoolEntry* entry = &frame->clazz->constant_pool[index];
            if (entry->tag == CONSTANT_Methodref || entry->tag == CONSTANT_InterfaceMethodref) {
                const char* cn = classfile_get_class_name(frame->clazz, entry->info.ref_info.class_index);
                const char* mn = NULL, * dn = NULL;
                classfile_get_name_and_type(frame->clazz, entry->info.ref_info.name_and_type_index, &mn, &dn);
                if (cn && mn) invoke_watch_hit(cn, mn, dn, "special", frame, 0);
            }
        }
    }
    
    /* DEBUG: Log constant pool state */
    if (g_j2me_runtime_debug) {
        if (g_j2me_runtime_debug) LOG_SAFE("[INVOKESPECIAL DEBUG] class=%s, index=%d, constant_pool_count=%d\n", 
                frame->clazz->class_name ? frame->clazz->class_name : "NULL",
                index, frame->clazz->constant_pool_count);
    }
    
    if (index > 0 && index < frame->clazz->constant_pool_count) {
        ConstantPoolEntry* entry = &frame->clazz->constant_pool[index];
        
        /* DEBUG: Log entry tag */
        if (g_j2me_runtime_debug) {
            if (g_j2me_runtime_debug) LOG_SAFE("[INVOKESPECIAL DEBUG] entry[%d].tag=%d (expected Methodref=%d or InterfaceMethodref=%d)\n",
                    index, entry->tag, CONSTANT_Methodref, CONSTANT_InterfaceMethodref);
        }
        
        if (entry->tag == CONSTANT_Methodref || entry->tag == CONSTANT_InterfaceMethodref) {
            uint16_t class_idx = entry->info.ref_info.class_index;
            uint16_t nat_idx = entry->info.ref_info.name_and_type_index;
            
            if (g_j2me_runtime_debug) {
                if (g_j2me_runtime_debug) LOG_SAFE("[INVOKESPECIAL DEBUG] class_idx=%d, nat_idx=%d\n", class_idx, nat_idx);
            }
            
            class_name = classfile_get_class_name(frame->clazz, class_idx);
            classfile_get_name_and_type(frame->clazz, nat_idx, &method_name, &descriptor);
            
            if (g_j2me_runtime_debug) {
                if (g_j2me_runtime_debug) LOG_SAFE("[INVOKESPECIAL DEBUG] class_name=%s, method_name=%s, descriptor=%s\n",
                        class_name ? class_name : "NULL",
                        method_name ? method_name : "NULL",
                        descriptor ? descriptor : "NULL");
            }
        } else {
            /* DEBUG: Dump raw entry data */
            if (g_j2me_runtime_debug) {
                if (g_j2me_runtime_debug) LOG_SAFE("[INVOKESPECIAL DEBUG] Unexpected tag! Raw entry data: ");
                uint8_t* raw = (uint8_t*)entry;
                for (int i = 0; i < (int)sizeof(ConstantPoolEntry) && i < 32; i++) {
                    LOG_SAFE("%02x ", raw[i]);
                }
                LOG_SAFE("\n");
            }
        }
    } else {
        if (g_j2me_runtime_debug) {
            if (g_j2me_runtime_debug) LOG_SAFE("[INVOKESPECIAL DEBUG] Index out of bounds! index=%d, count=%d\n",
                    index, frame->clazz->constant_pool_count);
        }
    }
    
    /* Pop arguments based on descriptor */
    int arg_count = 0;
    JavaValue* args = NULL;
    
    if (descriptor) {
        /* Count actual Java arguments (not stack slots) */
        const char* p = descriptor;
        if (*p == '(') p++;
        int java_args = 0;
        while (*p && *p != ')') {
            switch (*p) {
                case 'J': case 'D':
                    java_args++;  /* Long/double - 1 argument, 2 slots */
                    p++;
                    break;
                case 'B': case 'C': case 'F': case 'I': case 'S': case 'Z':
                    java_args++;
                    p++;
                    break;
                case 'L':
                    java_args++;
                    while (*p && *p != ';') p++;
                    if (*p == ';') p++;
                    break;
                case '[':
                    java_args++;
                    while (*p == '[') p++;
                    if (*p == 'L') while (*p && *p != ';') p++;
                    if (*p) p++;
                    break;
                default:
                    p++;
                    break;
            }
        }
        
        /* Get stack slots needed (including 2 slots for long/double) */
        int stack_slots = count_args(descriptor);

        /* Underflow guard (release POP is unchecked!): popping stack_slots
         * argument slots + 1 'this' slot must not run below the stack base.
         * A corrupted classfile / CP can desync the stack — throw VerifyError
         * instead of segfaulting below frame->stack (seen on MinGW release). */
        if (frame->stack_top < stack_slots) {
            char msg[192];
            snprintf(msg, sizeof(msg),
                     "%s.%s%s needs %d stack slots, stack has %d",
                     class_name ? class_name : "?",
                     method_name ? method_name : "?",
                     descriptor ? descriptor : "",
                     stack_slots, frame->stack_top + 1);
            LOG_SAFE("[EXEC] invokespecial stack underflow: %s\n", msg);
            free(args);
            jvm_throw_by_name(jvm, "java/lang/VerifyError", msg);
            return -1;
        }
        
        /* Allocate args array: this + java_args */
        args = (JavaValue*)malloc((java_args + 1) * sizeof(JavaValue));
        if (!args) return -1;
        
        /* Pop arguments from stack, handling long/double correctly */
        p = descriptor;
        if (*p == '(') p++;
        
        int arg_idx = java_args;
        
        /* First, collect all stack values into a temporary buffer */
        JavaValue* stack_vals = (JavaValue*)malloc((stack_slots + 1) * sizeof(JavaValue));
        if (!stack_vals) { free(args); return -1; }
        
        for (int i = stack_slots - 1; i >= 0; i--) {
            stack_vals[i] = POP(frame);
        }
        
        /* Pop object reference (this) */
        if (frame->stack_top < 0) {
            /* v9: throw instead of bare return — the interpreter loop expects
             * a pending exception (or a fatal abort) when a handler returns
             * -1; a catchable VerifyError is the spec-conformant choice. */
            LOG_SAFE("[EXEC] invokespecial: stack underflow, no 'this' reference\n");
            free(stack_vals);
            free(args);
            jvm_throw_by_name(jvm, "java/lang/VerifyError",
                              "invokespecial: missing objectref");
            return -1;
        }
        args[0] = POP(frame);
        
        /* Now map stack values to args, combining long/double */
        int sv = 0;
        arg_idx = 1;
        p = descriptor;
        if (*p == '(') p++;
        
        while (*p && *p != ')') {
            if (*p == 'J' || *p == 'D') {
                /* Long or double - combine two slots */
                args[arg_idx].j = stack_vals[sv].j;  /* Already properly stored */
                sv += 2;
                arg_idx++;
                p++;
            } else if (*p == 'L') {
                args[arg_idx] = stack_vals[sv];
                sv++;
                arg_idx++;
                while (*p && *p != ';') p++;
                if (*p == ';') p++;
            } else if (*p == '[') {
                args[arg_idx] = stack_vals[sv];
                sv++;
                arg_idx++;
                while (*p == '[') p++;
                if (*p == 'L') while (*p && *p != ';') p++;
                if (*p) p++;
            } else {
                args[arg_idx] = stack_vals[sv];
                sv++;
                arg_idx++;
                p++;
            }
        }
        
        free(stack_vals);
        arg_count = java_args;
    }
    
    /* FIX: guard against unresolved constant pool references.
     * Previously a NULL descriptor (bad/absent CP entry, e.g. from stub
     * classes) left args == NULL and the unconditional args[0] read below
     * crashed the whole emulator with SIGSEGV. Throw a Java-level error
     * instead, as required by the JVM spec. */
    if (!class_name || !method_name || !descriptor || !args) {
        LOG_SAFE("[EXEC] invokespecial: unresolved method ref #%d in %s.%s -> throwing NoSuchMethodError\n",
                index,
                frame->clazz && frame->clazz->class_name ? frame->clazz->class_name : "?",
                class_name ? class_name : "?");
        free(args); /* free(NULL) is safe */
        {
            char msg[256];
            snprintf(msg, sizeof(msg), "%s.%s%s", 
                    class_name ? class_name : "?",
                    method_name ? method_name : "<unknown>",
                    descriptor ? descriptor : "");
            jvm_throw_by_name(jvm, "java/lang/NoSuchMethodError", msg);
        }
        return -1;
    }
    
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    LOG_OPCODE("[EXEC] invokespecial: %s.%s%s (obj=%p, args=%d)\n", 
            class_name ? class_name : "??", 
            method_name ? method_name : "??", 
            descriptor ? descriptor : "??",
            (void*)obj, arg_count);
    
    if (!obj) {
        LOG_SAFE("[EXEC] invokespecial: NULL object!\n");
        free(args);
        native_throw_npe(jvm, thread);
        return -1;
    }
    
    /* Get the class for the method */
    JavaClass* target_class = jvm_load_class(jvm, class_name);
    if (!target_class) {
        LOG_SAFE("[EXEC] invokespecial: class %s not found\n", class_name);
        free(args);
        return -1;
    }
    
    /* Find method */
    JavaMethod* method = jvm_resolve_method(jvm, target_class, method_name, descriptor);
    
    /* v18 FIX: constructors are NOT inherited. jvm_resolve_method() walks
     * up the superclass chain, so for a stub class without its own
     * <init>()V (e.g. java/util/Date after the generic-ctor exclusion) it
     * found Object.<init>()V - a no-op - and the class's registered native
     * constructor was never invoked, leaving internal fields at 0
     * (Date.getTime() == 0). If the resolved <init> is not declared by the
     * target class itself, prefer the class's own native <init>.
     *
     * v34.12 FIX: also prefer the native ctor when the declared one is a
     * STUB-GENERATED TRIVIAL constructor. create_stub_class() emits
     * `2A B7 .. .. B1` (aload_0; invokespecial super.<init>; return)
     * constructors whose ONLY purpose is to make `new X()` resolvable —
     * they carry no real initialization. Such stub ctors win method
     * resolution and silently skip the registered native ctors of the M3G
     * classes (native_transform_init never allocated Transform's matrix
     * array => every camera transform was identity => M3GTest rendered
     * blank scenes). Classes whose stub ctor does real work (Thread/Vector/
     * Hashtable/MIDlet putfield their fields) are NOT trivial and keep
     * executing bytecode — zero behavior change for them. */
    if (method_name && strcmp(method_name, "<init>") == 0) {
        bool ctor_declared_here = false;
        for (int mi = 0; mi < target_class->methods_count; mi++) {
            JavaMethod* m = &target_class->methods[mi];
            if (m->name && m->descriptor &&
                strcmp(m->name, "<init>") == 0 &&
                strcmp(m->descriptor, descriptor) == 0) {
                ctor_declared_here = true;
                break;
            }
        }

        /* v34.12: is the declared ctor a trivial stub (no real init)? */
        bool ctor_is_trivial_stub = true;
        if (ctor_declared_here && method) {
            ctor_is_trivial_stub = false;
            if (!method->code.code || method->code.code_length == 0) {
                ctor_is_trivial_stub = true;            /* no code at all */
            } else if (method->code.code_length == 1 &&
                       method->code.code[0] == 0xB1) {  /* bare return */
                ctor_is_trivial_stub = true;
            } else if (method->code.code_length == 5 &&
                       method->code.code[0] == 0x2A &&  /* aload_0 */
                       method->code.code[1] == 0xB7 &&  /* invokespecial */
                       method->code.code[4] == 0xB1) {  /* return */
                ctor_is_trivial_stub = true;            /* super-call only */
            }
        }

        if (!ctor_declared_here || ctor_is_trivial_stub) {
            NativeMethod native_ctor = native_find(jvm, target_class->class_name, "<init>", descriptor);
            if (native_ctor) {
                if (thread->pending_exception) thread->pending_exception = NULL;
                argguard_check_array(target_class->class_name, "<init>", descriptor,
                                     args, arg_count + 1, 1,
                                     __builtin_return_address(0), 1);
                JavaValue result = native_ctor(jvm, thread, args, arg_count + 1);
                (void)result;  /* constructor returns void */
                free(args);
                if (thread->pending_exception) return -1;
                return 0;
            }
        }
    }
    
    if (!method) {
        /* Try native method in class hierarchy - BUT NOT for constructors!
         * Constructors (<init>) must be executed as bytecode, not native handlers.
         * Each class has its own constructor that must run its specific initialization code.
         */
        NativeMethod native = NULL;
        
        if (method_name && strcmp(method_name, "<init>") != 0 && strcmp(method_name, "<clinit>") != 0) {
            JavaClass* search_class = target_class;
            
            while (search_class && !native) {
                native = native_find(jvm, search_class->class_name, method_name, descriptor);
                if (!native) {
                    search_class = search_class->super_class;
                }
            }
            /* v34.76: descriptor-relaxed second pass (see invokevirtual). */
            if (!native) {
                JavaClass* relax_class = target_class;
                while (relax_class && !native) {
                    native = native_find_relaxed(jvm, relax_class->class_name, method_name, descriptor);
                    if (!native) relax_class = relax_class->super_class;
                }
            }
        }
        
        if (native) {
            /* NATIVE_CALL logging removed for performance */
            
            /* Clear any stale exception before calling native */
            if (thread->pending_exception) {
                LOG_DEBUG("[INVOKESPECIAL] Clearing stale exception before %s.%s%s\n",
                        class_name, method_name, descriptor);
                thread->pending_exception = NULL;
            }
            
            /* PINNING: Pin all object arguments before calling native method */
            for (int pi = 0; pi <= arg_count; pi++) {
                if (args[pi].ref && is_heap_ptr_check(args[pi].ref)) {
                    gc_pin(jvm, args[pi].ref);
                }
            }
            
            /* v34.14 NATIVE-SCOPE AUTO-PIN (see invokevirtual site) */
            extern __thread int g_gc_in_native;
            extern size_t gc_autopin_base(void);
            extern void gc_autopin_release(size_t base);
            size_t ap_base = gc_autopin_base();
            g_gc_in_native++;
            
            extern void jvm_note_native_call(JavaThread*, const char*, const char*);
            jvm_note_native_call(thread, class_name, method_name);
            /* [ARGGUARD] v36.49: см. invokevirtual-сайт выше */
            argguard_check_array(class_name, method_name, descriptor, args,
                                 arg_count + 1, 1,
                                 __builtin_return_address(0), 1);
            int naf_slot = nargs_root_push(thread, args, arg_count + 1);
            JavaValue result = native(jvm, thread, args, arg_count + 1);
            argguard_check_ret(class_name, method_name, descriptor, &result,
                               __builtin_return_address(0));
            nargs_root_pop(thread, naf_slot);
            g_gc_in_native--;
            
            /* UNPINNING: Unpin all object arguments after native method returns */
            for (int pi = 0; pi <= arg_count; pi++) {
                if (args[pi].ref && is_heap_ptr_check(args[pi].ref)) {
                    gc_unpin(jvm, args[pi].ref);
                }
            }
            gc_autopin_release(ap_base);
            
            /* DEBUG: Log result for array return types */
            if (g_j2me_runtime_debug && descriptor && strlen(descriptor) > 0) {
                const char* ret = strchr(descriptor, ')');
                if (ret && ret[1] == '[') {
                    JavaArray* arr = (JavaArray*)result.ref;
                    /* Safety check: verify pointer is valid before dereferencing */
                    bool is_valid_ptr = (arr != NULL && ((uintptr_t)arr) > 0x10000);
                    if (is_valid_ptr) {
                        if (g_j2me_runtime_debug) LOG_SAFE("[DEBUG] Native %s.%s%s returned array: %p, length=%d\n",
                                class_name, method_name, descriptor, (void*)arr, arr->length);
                    } else {
                        if (g_j2me_runtime_debug) LOG_SAFE("[DEBUG] Native %s.%s%s returned INVALID array ref: %p (raw=0x%lx)\n",
                                class_name, method_name, descriptor, (void*)arr, (unsigned long)result.raw);
                    }
                }
            }
            
            free(args);
            
            /* Check for exceptions */
            if (thread->pending_exception) {
                return -1;
            }
            
            /* Push result if not void */
            if (descriptor && strlen(descriptor) > 0) {
                const char* ret = strchr(descriptor, ')');
                if (ret && ret[1] != 'V') {
                    PUSH(frame, result);
                    if (ret[1] == 'J' || ret[1] == 'D') {
                        PUSH(frame, result);
                    }
                }
            }
            return 0;
        }
        
        /* 
         * CRITICAL FIX: If method is a constructor (<init>) and not found,
         * handle special cases for obfuscated/malformed bytecode.
         */
        if (method_name && strcmp(method_name, "<init>") == 0) {
            /* 
             * SPECIAL CASE: Object class only has <init>()V constructor.
             * Obfuscated bytecode may try to call Object.<init>(String) etc.
             * In this case, just call the default Object constructor and ignore extra args.
             */
            if (strcmp(target_class->class_name, "java/lang/Object") == 0) {
                LOG_SAFE("[EXEC] invokespecial: Object.<init>%s not found, using default <init>()V\n",
                        descriptor ? descriptor : "??");
                /* Object's constructor does nothing special, just return success */
                free(args);
                return 0;
            }
            
            /*
             * CRITICAL FIX: Check if there's a native constructor registered for this signature.
             * Some classes (like DataOutputStream, DataInputStream) have native constructors
             * that are not in the class file bytecode.
             */
            NativeMethod native_ctor = native_find(jvm, target_class->class_name, "<init>", descriptor);
            if (native_ctor) {
                argguard_check_array(target_class->class_name, "<init>", descriptor,
                                     args, arg_count + 1, 1,
                                     __builtin_return_address(0), 1);
                JavaValue result = native_ctor(jvm, thread, args, arg_count + 1);
                (void)result;  /* Constructor returns void */
                free(args);

                /* Check for exceptions */
                if (thread->pending_exception) {
                    return -1;
                }
                return 0;
            }
            
            /* 
             * Try to find default constructor <init>()V as fallback.
             * Some obfuscated code calls wrong constructor signatures.
             */
            JavaMethod* default_init = jvm_resolve_method(jvm, target_class, "<init>", "()V");
            if (default_init) {
                static int constructor_fallback_log_count = 0;
                if (constructor_fallback_log_count < 5) {
                    LOG_SAFE("[EXEC] invokespecial: Constructor %s.<init>%s not found, using default <init>()V\n",
                            target_class->class_name ? target_class->class_name : "?",
                            descriptor ? descriptor : "??");
                    constructor_fallback_log_count++;
                    if (constructor_fallback_log_count == 5) {
                        LOG_SAFE("[EXEC] Further constructor fallback logging suppressed\n");
                    }
                }
                /* Call default constructor with just 'this' argument */
                JavaValue default_args[1];
                default_args[0] = args[0];  /* this reference */
                JavaValue result;
                int ret = execute_method(jvm, thread, default_init, default_args, &result);
                
                /* CRITICAL FIX: For LCDUI classes (Form, List, Alert, TextBox, etc.),
                 * initialize the 'title' field from the first String argument.
                 * These classes have constructors like <init>(String) or <init>(String, int, etc.)
                 * where the first argument is always the title.
                 */
                if (ret == 0 && target_class->class_name && args[0].ref && arg_count >= 1) {
                    JavaObject* obj = (JavaObject*)args[0].ref;
                    const char* class_name = target_class->class_name;
                    
                    /* Check if this is an LCDUI Screen subclass with title field */
                    bool has_title = (strstr(class_name, "javax/microedition/lcdui/Form") != NULL ||
                                     strstr(class_name, "javax/microedition/lcdui/List") != NULL ||
                                     strstr(class_name, "javax/microedition/lcdui/Alert") != NULL ||
                                     strstr(class_name, "javax/microedition/lcdui/TextBox") != NULL);
                    
                    if (has_title && descriptor) {
                        /* Check if descriptor starts with (Ljava/lang/String; */
                        if (strncmp(descriptor, "(Ljava/lang/String;", 19) == 0) {
                            /* args[1] is the title String argument */
                            JavaString* title = (JavaString*)args[1].ref;
                            
                            /* Set title field (field 0 in our stub layout) */
                            if (OBJECT_HAS_FIELDS(obj, 1)) {
                                obj->fields[0].ref = title;
                                LOG_SAFE("[EXEC] Set title='%s' for %s\n",
                                        title && title->utf8 ? title->utf8 : "null",
                                        class_name);
                            }
                        }
                    }
                    
                    /* CRITICAL FIX: For TextField, initialize label, text, maxSize, constraints.
                     * Constructor: TextField.<init>(String label, String text, int maxSize, int constraints)
                     * Fields: [0]=label, [1]=text, [2]=maxSize, [3]=constraints
                     */
                    if (strstr(class_name, "javax/microedition/lcdui/TextField") != NULL && 
                        descriptor && arg_count >= 4) {
                        /* TextField.<init>(Ljava/lang/String;Ljava/lang/String;II)V */
                        /* Check descriptor starts with (String, String, int, int) */
                        if (strncmp(descriptor, "(Ljava/lang/String;Ljava/lang/String;II)V", 41) == 0 ||
                            strncmp(descriptor, "(Ljava/lang/String;Ljava/lang/String;II)", 39) == 0) {
                            JavaString* label = (JavaString*)args[1].ref;
                            JavaString* text_str = (JavaString*)args[2].ref;
                            jint max_size = args[3].i;
                            jint constraints = args[4].i;
                            
                            if (OBJECT_HAS_FIELDS(obj, 4)) {
                                obj->fields[0].ref = label;       /* label */
                                obj->fields[1].ref = text_str;    /* text */
                                obj->fields[2].i = max_size;      /* maxSize */
                                obj->fields[3].i = constraints;   /* constraints */
                                LOG_SAFE("[EXEC] Set TextField: label='%s', text='%s', maxSize=%d, constraints=%d\n",
                                        label && label->utf8 ? label->utf8 : "null",
                                        text_str && text_str->utf8 ? text_str->utf8 : "null",
                                        max_size, constraints);
                            }
                        }
                    }
                    
                    /* CRITICAL FIX: For StringItem, initialize label and text.
                     * Constructor: StringItem.<init>(String label, String text)
                     * Fields: [0]=label, [1]=text
                     */
                    if (strstr(class_name, "javax/microedition/lcdui/StringItem") != NULL && 
                        descriptor && arg_count >= 2) {
                        /* StringItem.<init>(Ljava/lang/String;Ljava/lang/String;)V */
                        if (strncmp(descriptor, "(Ljava/lang/String;Ljava/lang/String;)V", 40) == 0 ||
                            strncmp(descriptor, "(Ljava/lang/String;Ljava/lang/String;)", 38) == 0) {
                            JavaString* label = (JavaString*)args[1].ref;
                            JavaString* text_str = (JavaString*)args[2].ref;
                            
                            if (OBJECT_HAS_FIELDS(obj, 2)) {
                                obj->fields[0].ref = label;    /* label */
                                obj->fields[1].ref = text_str; /* text */
                                LOG_SAFE("[EXEC] Set StringItem: label='%s', text='%s'\n",
                                        label && label->utf8 ? label->utf8 : "null",
                                        text_str && text_str->utf8 ? text_str->utf8 : "null");
                            }
                        }
                    }
                    
                    /* CRITICAL FIX: For Alert, initialize title, text, image, alertType.
                     * Constructor: Alert.<init>(String title, String text, Image image, AlertType type)
                     * Fields: [0]=title, [1]=text, [2]=image, [3]=alertType, [4]=timeout
                     */
                    if (strstr(class_name, "javax/microedition/lcdui/Alert") != NULL && 
                        descriptor && arg_count >= 4) {
                        /* Alert.<init>(Ljava/lang/String;Ljava/lang/String;Ljavax/microedition/lcdui/Image;Ljavax/microedition/lcdui/AlertType;)V */
                        if (strncmp(descriptor, "(Ljava/lang/String;Ljava/lang/String;Ljavax/microedition/lcdui/Image;Ljavax/microedition/lcdui/AlertType;", 104) == 0) {
                            JavaString* title = (JavaString*)args[1].ref;
                            JavaString* text_str = (JavaString*)args[2].ref;
                            void* image = args[3].ref;
                            void* alert_type = args[4].ref;
                            
                            if (OBJECT_HAS_FIELDS(obj, 5)) {
                                obj->fields[0].ref = title;      /* title */
                                obj->fields[1].ref = text_str;   /* text */
                                obj->fields[2].ref = image;      /* image */
                                obj->fields[3].ref = alert_type; /* alertType */
                                /* fields[4] = timeout (int) - not set here, default is FOREVER */
                                LOG_SAFE("[EXEC] Set Alert: title='%s', text='%s'\n",
                                        title && title->utf8 ? title->utf8 : "null",
                                        text_str && text_str->utf8 ? text_str->utf8 : "null");
                            }
                        }
                    }
                    
                    /* CRITICAL FIX: For TextBox, initialize title, text, maxSize, constraints.
                     * Constructor: TextBox.<init>(String title, String text, int maxSize, int constraints)
                     * Fields: [0]=title, [1]=text, [2]=maxSize, [3]=constraints
                     */
                    if (strstr(class_name, "javax/microedition/lcdui/TextBox") != NULL && 
                        descriptor && arg_count >= 4) {
                        /* TextBox.<init>(Ljava/lang/String;Ljava/lang/String;II)V */
                        if (strncmp(descriptor, "(Ljava/lang/String;Ljava/lang/String;II)V", 42) == 0 ||
                            strncmp(descriptor, "(Ljava/lang/String;Ljava/lang/String;II)", 40) == 0) {
                            JavaString* title = (JavaString*)args[1].ref;
                            JavaString* text_str = (JavaString*)args[2].ref;
                            jint max_size = args[3].i;
                            jint constraints = args[4].i;
                            
                            if (OBJECT_HAS_FIELDS(obj, 4)) {
                                obj->fields[0].ref = title;      /* title */
                                obj->fields[1].ref = text_str;   /* text */
                                obj->fields[2].i = max_size;     /* maxSize */
                                obj->fields[3].i = constraints;  /* constraints */
                                LOG_SAFE("[EXEC] Set TextBox: title='%s', text='%s', maxSize=%d, constraints=%d\n",
                                        title && title->utf8 ? title->utf8 : "null",
                                        text_str && text_str->utf8 ? text_str->utf8 : "null",
                                        max_size, constraints);
                            }
                        }
                    }
                    
                    /* CRITICAL FIX: For Ticker, initialize text field.
                     * Constructor: Ticker.<init>(String str)
                     * Fields: [0]=text
                     */
                    if (strstr(class_name, "javax/microedition/lcdui/Ticker") != NULL && 
                        descriptor && arg_count >= 1) {
                        /* Ticker.<init>(Ljava/lang/String;)V */
                        if (strncmp(descriptor, "(Ljava/lang/String;)V", 21) == 0 ||
                            strncmp(descriptor, "(Ljava/lang/String;)", 20) == 0) {
                            JavaString* text_str = (JavaString*)args[1].ref;
                            
                            if (OBJECT_HAS_FIELDS(obj, 1)) {
                                obj->fields[0].ref = text_str;    /* text */
                                LOG_SAFE("[EXEC] Set Ticker: text='%s'\n",
                                        text_str && text_str->utf8 ? text_str->utf8 : "null");
                            }
                        }
                    }
                    
                    /* CRITICAL FIX: For Gauge, initialize label, interactive, maxValue, value.
                     * Constructor: Gauge.<init>(String label, boolean interactive, int maxValue, int initialValue)
                     * Fields: [0]=label, [1]=interactive, [2]=maxValue, [3]=value
                     */
                    if (strstr(class_name, "javax/microedition/lcdui/Gauge") != NULL && 
                        descriptor && arg_count >= 4) {
                        /* Gauge.<init>(Ljava/lang/String;ZII)V */
                        if (strncmp(descriptor, "(Ljava/lang/String;ZII)V", 24) == 0 ||
                            strncmp(descriptor, "(Ljava/lang/String;ZII)", 22) == 0) {
                            JavaString* label = (JavaString*)args[1].ref;
                            jboolean interactive = args[2].i;
                            jint max_value = args[3].i;
                            jint initial_value = args[4].i;
                            
                            if (OBJECT_HAS_FIELDS(obj, 4)) {
                                obj->fields[0].ref = label;       /* label */
                                obj->fields[1].i = interactive;   /* interactive */
                                obj->fields[2].i = max_value;     /* maxValue */
                                obj->fields[3].i = initial_value; /* value */
                                LOG_SAFE("[EXEC] Set Gauge: label='%s', interactive=%d, max=%d, value=%d\n",
                                        label && label->utf8 ? label->utf8 : "null",
                                        interactive, max_value, initial_value);
                            }
                        }
                    }

                    /* CRITICAL FIX: For Boolean, initialize value field.
                     * Constructor: Boolean.<init>(boolean value)
                     * Fields: [0]=value (Z)
                     */
                    if (strstr(class_name, "java/lang/Boolean") != NULL &&
                        descriptor && arg_count >= 1) {
                        if (strncmp(descriptor, "(Z)V", 4) == 0) {
                            jint bool_val = args[1].i;
                            if (OBJECT_HAS_FIELDS(obj, 1)) {
                                obj->fields[0].i = bool_val ? 1 : 0;  /* value */
                            }
                        }
                    }

                    /* CRITICAL FIX: For Integer, initialize value field.
                     * Constructor: Integer.<init>(int value)
                     * Fields: [0]=value (I)
                     */
                    if (strstr(class_name, "java/lang/Integer") != NULL &&
                        descriptor && arg_count >= 1) {
                        if (strncmp(descriptor, "(I)V", 4) == 0) {
                            jint int_val = args[1].i;
                            if (OBJECT_HAS_FIELDS(obj, 1)) {
                                obj->fields[0].i = int_val;  /* value */
                            }
                        }
                    }

                    /* CRITICAL FIX: For Long, initialize value field.
                     * Constructor: Long.<init>(long value)
                     * Fields: [0]=value (J, 2 slots)
                     */
                    if (strstr(class_name, "java/lang/Long") != NULL &&
                        descriptor && arg_count >= 1) {
                        if (strncmp(descriptor, "(J)V", 4) == 0) {
                            jlong long_val = args[1].j;
                            if (OBJECT_HAS_FIELDS(obj, 2)) {
                                obj->fields[0].j = long_val;  /* value (2 slots) */
                            }
                        }
                    }

                    /* v38 FIX (BlackShark3D): Float.<init>(float) must store
                     * its argument into the value field — new Float(x).floatValue()
                     * returned 0.0f (mission loading froze at 95%). */
                    if (strstr(class_name, "java/lang/Float") != NULL &&
                        descriptor && arg_count >= 1) {
                        if (strncmp(descriptor, "(F)V", 4) == 0) {
                            jfloat float_val = args[1].f;
                            if (OBJECT_HAS_FIELDS(obj, 1)) {
                                obj->fields[0].f = float_val;  /* value */
                            }
                        }
                    }

                    /* v38 FIX: Double.<init>(double) — same pattern. */
                    if (strstr(class_name, "java/lang/Double") != NULL &&
                        descriptor && arg_count >= 1) {
                        if (strncmp(descriptor, "(D)V", 4) == 0) {
                            jdouble dbl_val = args[1].d;
                            if (OBJECT_HAS_FIELDS(obj, 2)) {
                                obj->fields[0].d = dbl_val;  /* value (2 slots) */
                            }
                        }
                    }
                }
                
                free(args);
                return ret;
            }
            
            LOG_SAFE("[EXEC] invokespecial: Constructor %s.<init>%s not found. Throwing error.\n", 
                    target_class->class_name ? target_class->class_name : "?",
                    descriptor ? descriptor : "??");
            free(args);
            jvm_throw_by_name(jvm, "java/lang/NoSuchMethodError", "Constructor not found");
            thread->pending_exception = jvm_exception_pending(jvm);
            return -1;
        }
        
        /* For other methods on stub classes, provide default return */
        if (target_class->is_stub) {
            LOG_SAFE("[EXEC] invokespecial: method %s.%s%s not found (stub class), using default\n", 
                    target_class->class_name, method_name, descriptor);
            
            /* Push default return value based on type */
            if (descriptor && strlen(descriptor) > 0) {
                const char* ret = strchr(descriptor, ')');
                if (ret && ret[1] != 'V') {
                    JavaValue default_val = { .raw = 0 };
                    PUSH(frame, default_val);
                    if (ret[1] == 'J' || ret[1] == 'D') {
                        PUSH(frame, default_val);
                    }
                }
            }
            free(args);
            return 0;
        }
        
        LOG_SAFE("[EXEC] invokespecial: method %s.%s%s not found\n", 
                target_class->class_name, method_name, descriptor);
        free(args);
        return -1;
    }
    
    /* Execute Java method */
    JavaValue result;
    memset(&result, 0, sizeof(result));
    int ret = execute_method(jvm, thread, method, args, &result);
    
    /* CRITICAL FIX: For native methods called via invokespecial, the result is returned
     * in the `result` parameter but NOT pushed to the caller's stack (unlike bytecode
     * methods which push via op_ireturn). We must push the result here. */
    if (ret == 0 && method->is_native && descriptor && strlen(descriptor) > 0) {
        const char* ret_type = strchr(descriptor, ')');
        if (ret_type && ret_type[1] != 'V') {
            PUSH(frame, result);
            if (ret_type[1] == 'J' || ret_type[1] == 'D') {
                JavaValue zero = { .raw = 0 };
                PUSH(frame, zero);  /* Push high word for long/double */
            }
        }
    }
    
    /* NOTE: Native handlers for constructors are ONLY called when method is NOT found
     * (see native_ctor handling above in the !method branch).
     * If we get here, the bytecode method was found and executed, so we should NOT
     * call the native handler again - bytecode already initialized the fields.
     */
    
    /* DEBUG: For array return types, check what op_areturn pushed to our stack */
    if (ret == 0 && descriptor && strlen(descriptor) > 0) {
        const char* ret_type = strchr(descriptor, ')');
        if (ret_type && ret_type[1] == '[' && frame->stack_top >= 0) {
            /* Result should be on top of our stack now (pushed by op_areturn) */
            JavaArray* arr = (JavaArray*)frame->stack[frame->stack_top].ref;
            /* Safety check: verify pointer is valid before dereferencing */
            bool is_valid_ptr = (arr != NULL && ((uintptr_t)arr) > 0x10000);
            if (is_valid_ptr) {
                if (g_j2me_runtime_debug) LOG_SAFE("[DEBUG] %s.%s%s returned array: %p, length=%d\n",
                        class_name, method_name, descriptor, (void*)arr, arr->length);
            } else {
                if (g_j2me_runtime_debug) LOG_SAFE("[DEBUG] %s.%s%s returned INVALID array: %p (raw=0x%lx)\n",
                        class_name, method_name, descriptor, (void*)arr, 
                        (unsigned long)frame->stack[frame->stack_top].raw);
            }
        }
    }
    
    /* NOTE: Result is already pushed to caller's stack by op_areturn/op_ireturn/etc.
     * We do NOT need to PUSH here - that would cause duplication!
     */
    
    free(args);
    return ret;
}

int op_invokestatic(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    /* v34.42 PERF: monomorphic inline cache — the static twin of the
     * v34.35 invokevirtual cache. Contract: frame->pc enters at opcode+1;
     * on any miss (no entry, generation mismatch, stack underflow, arg
     * buffer alloc failure) fall through to the original slow path which
     * re-fetches everything and re-runs the compat/watch hooks.
     * gprof (Asphalt 3 3D in-race): op_invokestatic ran 26.5M times per
     * 60 s, every call re-parsing the descriptor 3x (count_args /
     * count_java_args / pop_args_compact), malloc-ing the arg block and
     * re-resolving class+method+native by name. */
    {
        InvokeRefCache* ic = invoke_cache_entry(frame,
                (uint16_t)(((uint16_t)frame->code[frame->pc] << 8) | (uint16_t)frame->code[frame->pc + 1])  /* JVMS: big-endian u16 */);
        if (ic && ic->valid && ic->gen == g_vm_cache_gen &&
            (ic->kind == 0 || ic->kind == 1)) {
            int total_slots = 0;   /* static: no 'this' */
            for (int i = 0; i < ic->java_argc; i++) total_slots += ic->arg_slots[i];
            if (frame->stack_top + 1 >= total_slots) {
                int base = frame->stack_top + 1 - total_slots;
                JavaValue args_buf[12];
                JavaValue* args = (total_slots <= 12) ? args_buf
                    : (JavaValue*)malloc((size_t)total_slots * sizeof(JavaValue));
                if (args) {
                    int cur = base;
                    for (int i = 0; i < ic->java_argc; i++) {
                        args[i] = frame->stack[cur];   /* low slot of Java arg i */
                        cur += ic->arg_slots[i];
                    }
                    frame->stack_top = (int16_t)(base - 1);

                    frame->pc += 2;   /* pc entered at opcode+1; invokestatic is 3 bytes total */

                    if (ic->kind == 0) {
                        /* Hot path: bytecode method. The callee's return
                         * opcodes deposit the result onto OUR operand stack
                         * (same contract as the slow path's execute_method
                         * call with a NULL result pointer — do NOT push). */
                        JavaValue result;
                        int ret = execute_method(jvm, thread, ic->method, args, &result);
                        if (args != args_buf) free(args);
                        if (ret != 0) return -1;   /* pending_exception set by callee */
                        return 0;
                    }

                    /* Hot path: registered native. Mirrors the slow path
                     * exactly: stale-exception clear, pin args, autopin
                     * window, dispatch note, call, unpin, release. */
                    if (thread->pending_exception) {
                        thread->pending_exception = NULL;
                    }
                    for (int pi = 0; pi < ic->java_argc; pi++) {
                        if (args[pi].ref && is_heap_ptr_check(args[pi].ref)) {
                            gc_pin(jvm, args[pi].ref);
                        }
                    }
                    {
                        extern __thread int g_gc_in_native;
                        extern size_t gc_autopin_base(void);
                        extern void gc_autopin_release(size_t base);
                        extern void jvm_note_native_call(JavaThread*, const char*, const char*);
                        size_t ap_base = gc_autopin_base();
                        g_gc_in_native++;

                        jvm_note_native_call(thread, ic->cls_name, ic->mth_name);
                        JavaValue result = ((NativeMethod)ic->native_fn)(jvm, thread, args, ic->java_argc);
                        g_gc_in_native--;

                        for (int pi = 0; pi < ic->java_argc; pi++) {
                            if (args[pi].ref && is_heap_ptr_check(args[pi].ref)) {
                                gc_unpin(jvm, args[pi].ref);
                            }
                        }
                        gc_autopin_release(ap_base);

                        if (args != args_buf) free(args);

                        if (thread->pending_exception) {
                            return -1;
                        }
                        if (ic->has_ret) {
                            PUSH(frame, result);
                            if (ic->ret_wide) PUSH(frame, result);
                        }
                        return 0;
                    }
                }
            }
        }
    }
    uint16_t index = FETCH_U2(frame);
    
    const char* class_name = NULL;
    const char* method_name = NULL;
    const char* descriptor = NULL;
    
    if (index > 0 && index < frame->clazz->constant_pool_count) {
        ConstantPoolEntry* entry = &frame->clazz->constant_pool[index];
        if (entry->tag == CONSTANT_Methodref) {
            uint16_t class_idx = entry->info.ref_info.class_index;
            uint16_t nat_idx = entry->info.ref_info.name_and_type_index;
            
            class_name = classfile_get_class_name(frame->clazz, class_idx);
            classfile_get_name_and_type(frame->clazz, nat_idx, &method_name, &descriptor);
        }
    }

    /* === CRITICAL FIX: Validate descriptor === */
    if (!descriptor) {
        LOG_SAFE("[EXEC] FATAL: op_invokestatic index %d has NULL descriptor!\n", index);
        jvm_throw_by_name(jvm, "java/lang/InternalError", "Null descriptor");
        return -1;
    }

    /* FIX (v9, mirrors the v8 invokespecial guard): a stub/corrupt CP entry
     * can leave class_name or method_name NULL. jvm_load_class tolerates
     * NULL, but native_find() strcmp()s it and LOG_SAFE("%s") prints it —
     * both crash on MinGW (strcmp/printf with NULL). Throw a Java-level
     * error instead, as the JVM spec demands for unresolved refs. */
    if (!class_name || !method_name) {
        LOG_SAFE("[EXEC] invokestatic: unresolved method ref #%d in %s -> NoSuchMethodError\n",
                index,
                frame->clazz && frame->clazz->class_name ? frame->clazz->class_name : "?");
        jvm_throw_by_name(jvm, "java/lang/NoSuchMethodError", "Unresolved static method ref");
        return -1;
    }

    invoke_watch_init();
    invoke_watch_hit(class_name, method_name, descriptor, "static", frame,
                     frame->stack_top >= 0 ? frame->stack[frame->stack_top].i : 0);

    int arg_count = count_args(descriptor);

    /* === FIX: Handle stack underflow gracefully ===
     * Pad missing arguments with zeros instead of aborting — but ONLY when
     * the call could ever fit on this frame. If arg_count exceeds max_stack
     * the classfile is broken: padding would PUSH past frame->stack (release
     * PUSH is unchecked!) -> heap overflow. Throw VerifyError instead. */
    int available = frame->stack_top + 1;
    if (available < arg_count) {
        if (arg_count > (int)frame->max_stack) {
            char msg[160];
            snprintf(msg, sizeof(msg),
                     "invokestatic needs %d stack slots, max_stack=%d",
                     arg_count, (int)frame->max_stack);
            LOG_SAFE("[EXEC] invokestatic: %s\n", msg);
            jvm_throw_by_name(jvm, "java/lang/VerifyError", msg);
            return -1;
        }
        static int static_underflow_count = 0;
        if (static_underflow_count < 20) {
            LOG_SAFE("[EXEC] Stack underflow in invokestatic (need %d args, stack has %d) — padding with zeros\n", 
                    arg_count, available);
            static_underflow_count++;
        }
        while (frame->stack_top + 1 < arg_count) {
            JavaValue zero;
            memset(&zero, 0, sizeof(zero));
            PUSH(frame, zero);
        }
    }

    /* Allocate args array.
     * v18 FIX: build the COMPACT args[] (one JavaValue per Java argument,
     * J/D included). The old slot-wise pop shifted every argument after
     * the first long/double; see count_java_args/pop_args_compact. */
    int java_arg_count = count_java_args(descriptor);
    JavaValue* args = NULL;
    if (java_arg_count > 0) {
        args = (JavaValue*)malloc(java_arg_count * sizeof(JavaValue));
        if (!args) {
            jvm_throw_by_name(jvm, "java/lang/OutOfMemoryError", "Args alloc failed");
            return -1;
        }
        
        /* Pop arguments compactly */
        if (pop_args_compact(frame, descriptor, 0, args) < 0) {
            free(args);
            jvm_throw_by_name(jvm, "java/lang/OutOfMemoryError", "Args pop failed");
            return -1;
        }
    }
    

    /* M3G TRACE: Log all calls to javax/microedition/m3g classes */
    if (class_name && strstr(class_name, "m3g")) {
        static int m3g_static_count = 0;
        if (m3g_static_count < 5000) {
#if M3G_DEBUG_LOG
            LOG_SAFE("[M3G-TRACE] invokestatic: %s.%s%s\n",
                    class_name, method_name, descriptor);
#endif
            m3g_static_count++;
        }
    }

    LOG_OPCODE("[EXEC] invokestatic: %s.%s%s (args=%d)\n", 
            class_name ? class_name : "??", 
            method_name ? method_name : "??", 
            descriptor, 
            arg_count);
    
    /* ALWAYS LOG key methods for game debugging */
    if (method_name && (strcmp(method_name, "f") == 0 ||
                        strcmp(method_name, "a") == 0 ||
                        strcmp(method_name, "g") == 0 ||
                        strcmp(method_name, "forName") == 0)) {
        if (g_j2me_runtime_debug) LOG_SAFE("[METHOD] invokestatic: %s.%s%s\n",
                class_name ? class_name : "??", method_name, descriptor);
    }
    
    /* Load class */
    JavaClass* target_class = jvm_load_class(jvm, class_name);
    JavaMethod* method = NULL;
    
    if (target_class) {
        /* Initialize class before static call */
        if (!target_class->initialized) {
            if (jvm_init_class(jvm, target_class) != 0) {
                free(args);  /* ИСПРАВЛЕНО: утечка памяти при ошибке */
                return -1;
            }
        }
        method = jvm_resolve_method(jvm, target_class, method_name, descriptor);
        /* v34.42 PERF: fill the static inline cache (kind 0) for resolved
         * BYTECODE methods. Guard mirrors the dispatch preference below: a
         * class method AND a registered native for the same signature
         * dispatches to the NATIVE — do not cache kind 0 in that case
         * (the native branch below fills kind 1 instead). */
        if (method && !method->is_native &&
            method->code.code && method->code.code_length > 0 &&
            !native_find(jvm, class_name, method_name, descriptor)) {
            invokestatic_cache_fill(frame, index, target_class, method, NULL,
                                    class_name, method_name, descriptor);
        }
    }
    
    if (method) {
        LOG_DEBUG("[EXEC] invokestatic: calling Java method\n");
        LOG_DEBUG("[INVOKESTATIC] Found Java method %s.%s%s, executing...\n", 
                class_name, method_name, descriptor);
        
        /* Check if this is a native method - handle differently */
        if (method->is_native || native_find(jvm, class_name, method_name, descriptor)) {
            /* Native method: call directly and push result */
            NativeMethod native = native_find(jvm, class_name, method_name, descriptor);
            if (native) {
                /* v34.42 PERF: fill the static inline cache (kind 1) for the
                 * registered native. */
                invokestatic_cache_fill(frame, index, target_class, NULL, (void*)native,
                                        class_name, method_name, descriptor);
                LOG_DEBUG("[INVOKESTATIC] Native via execute_method: %s.%s%s\n",
                        class_name, method_name, descriptor);
                
                /* Clear stale exception */
                if (thread->pending_exception) {
                    thread->pending_exception = NULL;
                }
                
                /* Pin arguments */
                for (int pi = 0; pi < java_arg_count; pi++) {
                    if (args[pi].ref && is_heap_ptr_check(args[pi].ref)) {
                        gc_pin(jvm, args[pi].ref);
                    }
                }
                
                /* v34.14 NATIVE-SCOPE AUTO-PIN (see invokevirtual site) */
                extern __thread int g_gc_in_native;
                extern size_t gc_autopin_base(void);
                extern void gc_autopin_release(size_t base);
                size_t ap_base = gc_autopin_base();
                g_gc_in_native++;
                
                extern void jvm_note_native_call(JavaThread*, const char*, const char*);
                jvm_note_native_call(thread, class_name, method_name);
                /* [ARGGUARD] v36.49: static — без this */
                argguard_check_array(class_name, method_name, descriptor, args,
                                     java_arg_count, 0,
                                     __builtin_return_address(0), 1);
                int naf_slot = nargs_root_push(thread, args, java_arg_count);
                JavaValue result = native(jvm, thread, args, java_arg_count);
                argguard_check_ret(class_name, method_name, descriptor, &result,
                                   __builtin_return_address(0));
                nargs_root_pop(thread, naf_slot);
                g_gc_in_native--;
                
                /* Unpin arguments */
                for (int pi = 0; pi < java_arg_count; pi++) {
                    if (args[pi].ref && is_heap_ptr_check(args[pi].ref)) {
                        gc_unpin(jvm, args[pi].ref);
                    }
                }
                gc_autopin_release(ap_base);
                
                free(args);
                
                if (thread->pending_exception) {
                    return -1;
                }
                
                /* Push result if not void */
                if (strlen(descriptor) > 0) {
                    const char* ret_type = strchr(descriptor, ')');
                    if (ret_type && ret_type[1] != 'V') {
                        PUSH(frame, result);
                        if (ret_type[1] == 'J' || ret_type[1] == 'D') {
                            PUSH(frame, result);
                        }
                    }
                }
                return 0;
            }
        }
        
        /* Java method: execute normally */
        int ret = execute_method(jvm, thread, method, args, NULL);
        free(args);
        
        /* IMPORTANT: op_areturn/op_ireturn/etc already pushed the result to our stack!
         * Do NOT push again - that would cause duplicate stack entries. */
        return ret;
    }
    
    /* Try native method */
    NativeMethod native = native_find(jvm, class_name, method_name, descriptor);
    if (!native) {
        /* v34.76: descriptor-relaxed fallback (see invokevirtual). The
         * inline cache below is filled with the matched handler under the
         * CALLER's descriptor, so hot-path re-dispatch stays consistent. */
        native = native_find_relaxed(jvm, class_name, method_name, descriptor);
    }
    if (native) {
        /* v34.42 PERF: fill the static inline cache (kind 1) for the
         * fallback-resolved native. */
        invokestatic_cache_fill(frame, index, target_class, NULL, (void*)native,
                                class_name, method_name, descriptor);
        LOG_DEBUG("[EXEC] invokestatic: calling native\n");
        LOG_DEBUG("[INVOKESTATIC] Found native %s.%s%s, calling...\n", 
                class_name, method_name, descriptor);
        
        /* Clear any stale exception before calling native */
        if (thread->pending_exception) {
            LOG_DEBUG("[INVOKESTATIC] Clearing stale exception before %s.%s%s\n",
                    class_name, method_name, descriptor);
            thread->pending_exception = NULL;
        }
        
        /* PINNING: Pin all object arguments before calling native method */
        for (int pi = 0; pi < java_arg_count; pi++) {
            if (args[pi].ref && is_heap_ptr_check(args[pi].ref)) {
                gc_pin(jvm, args[pi].ref);
            }
        }
        
        /* v34.14 NATIVE-SCOPE AUTO-PIN (see invokevirtual site) */
        extern __thread int g_gc_in_native;
        extern size_t gc_autopin_base(void);
        extern void gc_autopin_release(size_t base);
        size_t ap_base = gc_autopin_base();
        g_gc_in_native++;
        
        extern void jvm_note_native_call(JavaThread*, const char*, const char*);
        jvm_note_native_call(thread, class_name, method_name);
        /* [ARGGUARD] v36.49: static — без this */
        argguard_check_array(class_name, method_name, descriptor, args,
                             java_arg_count, 0,
                             __builtin_return_address(0), 1);
        int naf_slot = nargs_root_push(thread, args, java_arg_count);
        JavaValue result = native(jvm, thread, args, java_arg_count);
        argguard_check_ret(class_name, method_name, descriptor, &result,
                           __builtin_return_address(0));
        nargs_root_pop(thread, naf_slot);
        g_gc_in_native--;
        
        /* UNPINNING: Unpin all object arguments after native method returns */
        for (int pi = 0; pi < java_arg_count; pi++) {
            if (args[pi].ref && is_heap_ptr_check(args[pi].ref)) {
                gc_unpin(jvm, args[pi].ref);
            }
        }
        gc_autopin_release(ap_base);
        
        free(args);
        
        if (thread->pending_exception) {
            static int invoke_exception_log_count = 0;
            if (invoke_exception_log_count < 10) {
                LOG_SAFE("[INVOKESTATIC] Native %s.%s%s threw exception!\n",
                        class_name ? class_name : "??", 
                        method_name ? method_name : "??", 
                        descriptor);
                invoke_exception_log_count++;
                if (invoke_exception_log_count == 10) {
                    LOG_SAFE("[INVOKESTATIC] Further native exception logging suppressed\n");
                }
            }
            return -1;
        }
        
        /* Push result if not void */
        if (strlen(descriptor) > 0) {
            const char* ret = strchr(descriptor, ')');
            if (ret && ret[1] != 'V') {
                PUSH(frame, result);
                if (ret[1] == 'J' || ret[1] == 'D') {
                    PUSH(frame, result);
                }
            }
        }
        return 0;
    }
    
    /* Method not found in class — check native registry as fallback.
     * This handles CLDC 1.1 missing methods like Math.floor(D)D, Math.ceil(D)D,
     * which have native handlers registered but aren't in the CLDC stub class. */
    {
        NativeMethod native_fallback = native_find(jvm, class_name, method_name, descriptor);
        if (native_fallback) {
            VERBOSE_LOG("[EXEC] invokestatic: %s.%s%s not in class, but found native handler (CLDC fallback)\n",
                    class_name ? class_name : "??", method_name, descriptor);

            /* Clear any stale exception before calling native */
            if (thread->pending_exception) {
                thread->pending_exception = NULL;
            }

            /* Pin arguments */
            for (int pi = 0; pi < arg_count; pi++) {
                if (args[pi].ref && is_heap_ptr_check(args[pi].ref)) {
                    gc_pin(jvm, args[pi].ref);
                }
            }

            argguard_check_array(class_name, method_name, descriptor, args,
                                 arg_count, 0,
                                 __builtin_return_address(0), 1);
            JavaValue native_result = native_fallback(jvm, thread, args, arg_count);
            argguard_check_ret(class_name, method_name, descriptor, &native_result,
                               __builtin_return_address(0));

            /* Unpin arguments */
            for (int pi = 0; pi < arg_count; pi++) {
                if (args[pi].ref && is_heap_ptr_check(args[pi].ref)) {
                    gc_unpin(jvm, args[pi].ref);
                }
            }

            free(args);

            if (thread->pending_exception) {
                static int fallback_exception_log_count = 0;
                if (fallback_exception_log_count < 10) {
                    LOG_SAFE("[INVOKESTATIC] Native fallback %s.%s%s threw exception!\n",
                            class_name ? class_name : "??",
                            method_name ? method_name : "??",
                            descriptor);
                    fallback_exception_log_count++;
                }
                return -1;
            }

            /* Push native result based on return type */
            const char* ret_type = strchr(descriptor, ')');
            if (ret_type && ret_type[1] != 'V') {
                PUSH(frame, native_result);
                if (ret_type[1] == 'J' || ret_type[1] == 'D') {
                    PUSH(frame, native_result);
                }
            }

            return 0;
        }
    }

    LOG_SAFE("[EXEC] invokestatic: %s.%s%s not found (stub?), returning default\n",
            class_name, method_name, descriptor);
    
    /* v11 DIAG: see the invokevirtual comment — surface unhooked M3G statics */
    if (class_name && method_name && descriptor &&
        (strstr(class_name, "m3g/") != NULL || strstr(class_name, "micro3d") != NULL)) {
        static int m3g_missing_static_count = 0;
        if (m3g_missing_static_count < 40) {
            m3g_missing_static_count++;
            MISSING_LOG("[M3G-MISSING] unresolved static call: %s.%s%s\n",
                    class_name, method_name, descriptor);
        }
    }
    
    free(args);
    
    /* Push default return value if not void */
    if (strlen(descriptor) > 0) {
        const char* ret = strchr(descriptor, ')');
        if (ret && ret[1] != 'V') {
            JavaValue result = { .i = 0 };
            PUSH(frame, result);
            if (ret[1] == 'J' || ret[1] == 'D') {
                PUSH(frame, result);
            }
        }
    }
    
    return 0;
}

int op_invokeinterface(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    uint16_t index = FETCH_U2(frame);
    uint8_t count = FETCH_U1(frame);
    uint8_t zero = FETCH_U1(frame);
    (void)zero;
    
    /* Get method reference */
    const char* class_name = NULL;
    const char* method_name = NULL;
    const char* descriptor = NULL;
    
    if (index > 0 && index < frame->clazz->constant_pool_count) {
        ConstantPoolEntry* entry = &frame->clazz->constant_pool[index];
        if (entry->tag == CONSTANT_InterfaceMethodref) {
            uint16_t class_idx = entry->info.ref_info.class_index;
            uint16_t nat_idx = entry->info.ref_info.name_and_type_index;
            
            class_name = classfile_get_class_name(frame->clazz, class_idx);
            classfile_get_name_and_type(frame->clazz, nat_idx, &method_name, &descriptor);
        }
    }

    if (!descriptor || !method_name) {
        /* v9: also reject NULL method_name — see the invokevirtual comment. */
        LOG_SAFE("[EXEC] FATAL: invokeinterface at index %d has unresolved name/type!\n", index);
        jvm_throw_by_name(jvm, "java/lang/NoSuchMethodError", "Unresolved interface method ref");
        return -1;
    }

    /* v34.8: cover invokeinterface in the generic tracer too (Bounce Tales
     * resource pipeline runs entirely through interface p.a(...) calls). */
    invoke_watch_hit(class_name ? class_name : "?", method_name, descriptor,
                     "interface", frame, 0);
    
    int arg_count = count_args(descriptor);
    
    /* === FIX: Handle stack underflow gracefully ===
     * Pad missing arguments with zeros instead of aborting.
     * v9: bound by max_stack — see the invokevirtual comment. */
    {
        int available = frame->stack_top + 1;
        if (available < arg_count + 1) {
            if (arg_count + 1 > (int)frame->max_stack) {
                char msg[160];
                snprintf(msg, sizeof(msg),
                         "invokeinterface needs %d stack slots, max_stack=%d",
                         arg_count + 1, (int)frame->max_stack);
                LOG_SAFE("[EXEC] invokeinterface: %s\n", msg);
                jvm_throw_by_name(jvm, "java/lang/VerifyError", msg);
                return -1;
            }
            static int iface_underflow_count = 0;
            if (iface_underflow_count < 20) {
                iface_underflow_count++;
                /* v34.4: same enriched context as invokevirtual */
                log_invoke_underflow_diag("invokeinterface", frame, class_name,
                                          method_name, descriptor, arg_count,
                                          available, 5);
            }
            while (frame->stack_top + 1 < arg_count + 1) {
                JavaValue zero;
                memset(&zero, 0, sizeof(zero));
                PUSH(frame, zero);
            }
        }
    }
    
    LOG_OPCODE("[EXEC] invokeinterface: %s.%s%s (count=%d, args=%d)\n", 
            class_name ? class_name : "?", 
            method_name ? method_name : "?", 
            descriptor, count, arg_count);
    
    /* v18 FIX: compact args[] (args[0]=this + one entry per Java argument,
     * J/D included) — see count_java_args/pop_args_compact. */
    int java_arg_count = count_java_args(descriptor);
    JavaValue* args = (JavaValue*)malloc((java_arg_count + 1) * sizeof(JavaValue));
    if (!args) return -1;
    
    /* Pop 'this' + arguments compactly */
    if (pop_args_compact(frame, descriptor, 1, args) < 0) {
        free(args);
        jvm_throw_by_name(jvm, "java/lang/OutOfMemoryError", "Args pop failed");
        return -1;
    }
    
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    if (!obj) {
        LOG_SAFE("[EXEC] invokeinterface: NULL object reference for %s.%s%s\n",
                class_name ? class_name : "?", method_name ? method_name : "?", descriptor);
        
        /* COMPATIBILITY FIX: For well-known interface methods on null, return default
         * values instead of throwing NullPointerException. Many MIDlets create Player
         * objects that can be null (e.g. Manager.createPlayer returns null on failure)
         * and then poll getMediaTime() in a loop — throwing NPE each time spams logs
         * and can crash the game loop. */
        
        /* Player.getMediaTime() -> TIME_UNKNOWN (-1) */
        if (class_name && strstr(class_name, "Player") != NULL) {
            if (method_name && strcmp(method_name, "getMediaTime") == 0) {
                JavaValue val = { .j = -1LL };  /* TIME_UNKNOWN */
                PUSH(frame, val);
                free(args);
                return 0;
            }
            if (method_name && strcmp(method_name, "getState") == 0) {
                JavaValue val = { .i = 0 };  /* UNREALIZED / CLOSED */
                PUSH(frame, val);
                free(args);
                return 0;
            }
            if (method_name && strcmp(method_name, "getDuration") == 0) {
                JavaValue val = { .j = -1LL };  /* TIME_UNKNOWN */
                PUSH(frame, val);
                free(args);
                return 0;
            }
            if ((method_name && strcmp(method_name, "realize") == 0) ||
                (method_name && strcmp(method_name, "prefetch") == 0) ||
                (method_name && strcmp(method_name, "start") == 0) ||
                (method_name && strcmp(method_name, "stop") == 0) ||
                (method_name && strcmp(method_name, "deallocate") == 0) ||
                (method_name && strcmp(method_name, "close") == 0)) {
                /* void methods — just return without doing anything */
                free(args);
                return 0;
            }
            if ((method_name && strcmp(method_name, "getContentType") == 0) ||
                (method_name && strcmp(method_name, "getControl") == 0)) {
                JavaValue null_val = { .ref = NULL };
                PUSH(frame, null_val);
                free(args);
                return 0;
            }
        }
        
        /* Enumeration methods on null */
        if (class_name && strstr(class_name, "Enumeration") != NULL) {
            if (method_name && strcmp(method_name, "hasMoreElements") == 0) {
                LOG_SAFE("[EXEC] invokeinterface: NULL Enumeration.hasMoreElements() -> false (compat)\n");
                JavaValue false_val = { .i = 0 };
                PUSH(frame, false_val);
                free(args);
                return 0;
            }
            if (method_name && strcmp(method_name, "nextElement") == 0) {
                LOG_SAFE("[EXEC] invokeinterface: NULL Enumeration.nextElement() -> null (compat)\n");
                JavaValue null_val = { .ref = NULL };
                PUSH(frame, null_val);
                free(args);
                return 0;
            }
        }
        
        /* v34.4: message on the NPE so game logs identify the null receiver */
        free(args);
        {
            char npe_msg[192];
            snprintf(npe_msg, sizeof(npe_msg),
                     "Null receiver calling interface method %s.%s%s",
                     class_name ? class_name : "?",
                     method_name ? method_name : "?",
                     descriptor ? descriptor : "?");
            jvm_throw_by_name(jvm, "java/lang/NullPointerException", npe_msg);
        }
        return -1;
    }
    
    /* [ARGGUARD] v36.49: мусорный receiver интерфейсного вызова — НЕ deref
     * [obj+0]; тот же исход, что у NULL receiver (NPE, catchable). */
    if (argguard_bad_ptr((uintptr_t)obj)) {
        char npe_msg[192];
        snprintf(npe_msg, sizeof(npe_msg),
                 "Garbage receiver calling interface method %s.%s%s",
                 class_name ? class_name : "?",
                 method_name ? method_name : "?",
                 descriptor ? descriptor : "?");
        free(args);
        jvm_throw_by_name(jvm, "java/lang/NullPointerException", npe_msg);
        return -1;
    }

    /* For interface methods, we need to find the actual implementation in the object's class */
    JavaClass* target_class = obj->header.clazz;
    JavaMethod* method = NULL;
    NativeMethod native = NULL;
    
    /* Search in the object's class hierarchy for the method implementation */
    JavaClass* search_class = target_class;
    while (search_class && !method && !native) {
        method = jvm_resolve_method(jvm, search_class, method_name, descriptor);
        if (!method) {
            native = native_find(jvm, search_class->class_name, method_name, descriptor);
        }
        if (!method && !native) {
            search_class = search_class->super_class;
        }
    }
    
    /* If still not found, try the interface class name */
    if (!method && !native && class_name) {
        native = native_find(jvm, class_name, method_name, descriptor);
    }
    
    /* v34.76: descriptor-relaxed second pass (see invokevirtual) - the
     * receiver's class chain first, then the interface name itself. */
    if (!method && !native) {
        JavaClass* relax_class = target_class;
        while (relax_class && !native) {
            native = native_find_relaxed(jvm, relax_class->class_name, method_name, descriptor);
            if (!native) relax_class = relax_class->super_class;
        }
        if (!native && class_name) {
            native = native_find_relaxed(jvm, class_name, method_name, descriptor);
        }
    }
    
    if (method) {
        /* Execute the method */
        JavaValue result;
        memset(&result, 0, sizeof(result));
        int ret = execute_method(jvm, thread, method, args, &result);
        free(args);
        
        if (ret < 0) return -1;
        if (thread->pending_exception) return -1;
        
        /* CRITICAL FIX: For native methods, result is in `result` param, not on stack */
        if (method->is_native && descriptor && strlen(descriptor) > 0) {
            const char* ret_type = strchr(descriptor, ')');
            if (ret_type && ret_type[1] != 'V') {
                PUSH(frame, result);
                if (ret_type[1] == 'J' || ret_type[1] == 'D') {
                    /* v18 FIX: push the SAME value twice (2-slot convention:
                     * both slots hold the full JavaValue, like every other
                     * push site — the old zero high slot broke nothing today
                     * but violated the invariant readers rely on). */
                    PUSH(frame, result);
                }
            }
        }
    } else if (native) {
        /* Clear any stale exception before calling native */
        if (thread->pending_exception) {
            thread->pending_exception = NULL;
        }
        
        /* PINNING: Pin all object arguments before calling native method */
        for (int pi = 0; pi <= java_arg_count; pi++) {
            if (args[pi].ref && is_heap_ptr_check(args[pi].ref)) {
                gc_pin(jvm, args[pi].ref);
            }
        }
        
        /* Call native method */
        /* v34.14 NATIVE-SCOPE AUTO-PIN (see invokevirtual site) */
        extern __thread int g_gc_in_native;
        extern size_t gc_autopin_base(void);
        extern void gc_autopin_release(size_t base);
        size_t ap_base = gc_autopin_base();
        g_gc_in_native++;
        
        extern void jvm_note_native_call(JavaThread*, const char*, const char*);
        jvm_note_native_call(thread, class_name, method_name);
        /* [ARGGUARD] v36.49: invokeinterface — args[0]=receiver */
        argguard_check_array(class_name, method_name, descriptor, args,
                             java_arg_count + 1, 1,
                             __builtin_return_address(0), 1);
        int naf_slot = nargs_root_push(thread, args, java_arg_count + 1);
        JavaValue result = native(jvm, thread, args, java_arg_count + 1);
        argguard_check_ret(class_name, method_name, descriptor, &result,
                           __builtin_return_address(0));
        nargs_root_pop(thread, naf_slot);
        g_gc_in_native--;
        
        /* UNPINNING: Unpin all object arguments after native method returns */
        for (int pi = 0; pi <= java_arg_count; pi++) {
            if (args[pi].ref && is_heap_ptr_check(args[pi].ref)) {
                gc_unpin(jvm, args[pi].ref);
            }
        }
        gc_autopin_release(ap_base);
        
        free(args);
        
        if (thread->pending_exception) return -1;
        
        /* Push return value if not void */
        if (strlen(descriptor) > 0) {
            const char* ret_type = strchr(descriptor, ')');
            if (ret_type && ret_type[1] != 'V') {
                PUSH(frame, result);
                if (ret_type[1] == 'J' || ret_type[1] == 'D') {
                    PUSH(frame, result);
                }
            }
        }
    } else {
        /* Suppress repeated "not found" logs for known Nokia/vendor API extensions */
        static int invokeinterface_notfound_count = 0;
        bool should_log = true;
        
        /* Suppress Nokia UI extension methods (known unsupported APIs) */
        if (class_name && strstr(class_name, "com/nokia/mid/ui") != NULL) {
            should_log = false;
        }
        /* Suppress other known vendor-specific APIs */
        if (class_name && (strstr(class_name, "com/siemens/") != NULL ||
                           strstr(class_name, "com/motorola/") != NULL ||
                           strstr(class_name, "com/samsung/") != NULL ||
                           strstr(class_name, "com/sonyericsson/") != NULL)) {
            should_log = false;
        }
        
        if (should_log && invokeinterface_notfound_count < 5) {
            LOG_SAFE("[EXEC] invokeinterface: method %s.%s%s not found! Returning default value.\n",
                    class_name ? class_name : "?", method_name ? method_name : "?", descriptor);
            invokeinterface_notfound_count++;
            if (invokeinterface_notfound_count == 5) {
                LOG_SAFE("[EXEC] Further invokeinterface 'not found' logging suppressed\n");
            }
        }
        free(args);
        
        /* Push default return value based on return type */
        const char* ret_type = strchr(descriptor, ')');
        if (ret_type && ret_type[1] != 'V') {
            JavaValue default_val = {0};
            switch (ret_type[1]) {
                case 'B': case 'C': case 'I': case 'S': case 'Z':
                    default_val.i = 0;
                    PUSH(frame, default_val);
                    break;
                case 'J': case 'D':
                    default_val.j = 0;
                    PUSH(frame, default_val);
                    PUSH(frame, default_val);  /* 2 slots */
                    break;
                case 'L': case '[':
                    default_val.ref = NULL;
                    PUSH(frame, default_val);
                    break;
            }
        }
    }
    
    return 0;
}

/* Object creation */
int op_new(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    uint16_t index = FETCH_U2(frame);
    const char* class_name = classfile_get_class_name(frame->clazz, index);
    
    JavaClass* clazz = jvm_load_class(jvm, class_name);
    if (!clazz) {
        /* v9: class_name may be NULL (bad CP) — MinGW printf crashes on %s NULL */
        LOG_SAFE("[EXEC] op_new: class %s not found!\n", class_name ? class_name : "<null>");
        native_throw_cnfe(jvm, thread, class_name);
        return -1;
    }
    
    /* Ensure super_class is resolved (may have been loaded from JAR) */
    if (clazz->super_class == NULL && clazz->super_class_name != NULL) {
        LOG_DEBUG("[EXEC] op_new: resolving super_class %s for %s\n", 
                clazz->super_class_name, clazz->class_name);
        clazz->super_class = jvm_load_class(jvm, clazz->super_class_name);
    }
    
    /* ИСПРАВЛЕНО: Используем jvm_init_class() вместо ручной инициализации.
     * Код ниже устанавливал initialized = true ДО выполнения <clinit>,
     * что нарушает JVM spec. Если <clinit> выбросит исключение,
     * класс останется помеченным как initialized, что неправильно.
     * 
     * jvm_init_class() правильно:
     * 1. Инициализирует суперкласс сначала
     * 2. Устанавливает initializing=true (не initialized)
     * 3. Выполняет <clinit>
     * 4. Устанавливает initialized=true только при успехе
     */
    if (!clazz->initialized) {
        LOG_DEBUG("[EXEC] op_new: initializing class %s\n", clazz->class_name);
        
        if (jvm_init_class(jvm, clazz) != 0) {
            LOG_SAFE("[EXEC] op_new: class %s initialization failed!\n", clazz->class_name);
            /* Класс не был инициализирован - это ошибка */
            return -1;
        }
    }
    
    LOG_DEBUG("[EXEC] op_new: creating %s (super_class=%p, super_class_name=%s)\n", 
            clazz->class_name, 
            (void*)clazz->super_class,
            clazz->super_class ? clazz->super_class->class_name : 
            (clazz->super_class_name ? clazz->super_class_name : "none"));
    
    JavaObject* obj = jvm_new_object(jvm, clazz);
    if (!obj) {
        native_throw_oome(jvm, thread);
        return -1;
    }
    
    JavaValue v = { .ref = obj };
    PUSH(frame, v);
    return 0;
}

/* В op_newarray */
int op_newarray(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    uint8_t atype = FETCH_U1(frame);
    jint count = POP(frame).i;
    
    if (count < 0) {
        native_throw_negative_array_size(jvm, thread);
        return -1;
    }
    
    /* FIX-19m: do NOT throw IllegalArgumentException for large sizes.
     * Per JVMS, NEWARRAY signals insufficient memory with a CATCHABLE
     * OutOfMemoryError - MemTests legitimately allocates new byte[1048576]
     * and expects OOME, not IAE. Try the allocation and let heap_alloc
     * decide (it rejects >1MB with NULL -> OOME below). */
    if (count > 1000000) {
        LOG_SAFE("[NEWARRAY] Large array allocation attempt: %d bytes\n", count);
    }
    
    JavaArray* array = jvm_new_array(jvm, atype, count, NULL);
    if (!array) {
        native_throw_oome(jvm, thread);
        return -1;
    }
    
    JavaValue v = { .ref = array };
    PUSH(frame, v);
    return 0;
}

int op_anewarray(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    uint16_t index = FETCH_U2(frame);
    jint count = POP(frame).i;
    
    if (count < 0) {
        native_throw_negative_array_size(jvm, thread);
        return -1;
    }
    
    /* Get the element class from the constant pool */
    const char* element_class_name = classfile_get_class_name(frame->clazz, index);
    JavaClass* element_class = NULL;
    
    if (element_class_name) {
        /* Load the element class */
        element_class = jvm_load_class(jvm, element_class_name);
    }
    
    JavaArray* array = jvm_new_array(jvm, DESC_OBJECT, count, element_class);
    if (!array) {
        native_throw_oome(jvm, thread);
        return -1;
    }
    
    /* DEBUG: Log array creation in constructor */
    if (g_j2me_runtime_debug && frame->method && frame->method->name && strcmp(frame->method->name, "<init>") == 0) {
        LOG_SAFE("[ANEWARRAY DEBUG] Constructor PC=%d: Created array %p, length=%d, element_class=%s\n",
                frame->pc - 3, (void*)array, count, element_class_name ? element_class_name : "NULL");
        LOG_SAFE("  array->length=%d, array->element_type=%d, array at offset %zu from heap start\n",
                array->length, array->element_type, (uint8_t*)array - (uint8_t*)g_heap_start);
    }
    
    JavaValue v = { .ref = array };
    PUSH(frame, v);
    return 0;
}

int op_arraylength(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    JavaArray* array = (JavaArray*)PEEK(frame).ref;
    
    if (!array) {
        /* DRM bypass: return 0 for null array in DRM classes */
        const char* caller_class = frame ? frame->clazz->class_name : NULL;
        if (caller_class && drm_is_drm_class(caller_class)) {
            POP_VOID(frame);
            JavaValue v = { .i = 0 };
            PUSH(frame, v);
            return 0;
        }
        /* Correct Java behavior: throw NullPointerException for null array */
        jvm_throw_by_name(jvm, "java/lang/NullPointerException", "null array");
        thread->pending_exception = jvm_exception_pending(jvm);
        return -1;
    }
    
    frame->stack[frame->stack_top].i = array->length;
    LOG_DEBUG("[ARRAYLEN] result=%d\n", array->length);
    return 0;
}

int op_athrow(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    JavaObject* exception = (JavaObject*)POP(frame).ref;
    
    /* DEBUG: Log exception throws (only first 60 to avoid log spam).
     * v34.99: budget was 10 — early-boot class-loading exceptions consumed
     * the whole budget, and the race-start exception (the monitor-leak
     * trigger at the countdown end) never reached the log. 60 covers boot
     * noise and still names the trigger. */
    static int athrow_log_count = 0;
    if (athrow_log_count < 60) {
        JavaClass* exc_class = exception ? object_get_class(exception) : NULL;
        const char* exc_name = exc_class ? (exc_class->class_name ? exc_class->class_name : "?") : "NULL";
        
        /* Get context: method and class where exception was thrown */
        const char* method_name = frame && frame->method ? frame->method->name : "?";
        const char* class_name = frame && frame->clazz ? (frame->clazz->class_name ? frame->clazz->class_name : "?") : "?";
        
        LOG_SAFE("[ATHROW] %s at %s.%s PC=%d\n", exc_name, class_name, method_name,
                frame ? (int)frame->pc - 1 : -1);
        
        /* Also print exception message if available.
         *
         * [BT-CRASH-FIX] (Bounce Tales, Switch hbloader Data Abort,
         * ffffffff00460018): the popped "exception" reference can be a
         * CORRUPTED pseudo-reference — a 64-bit slot whose halves are two
         * adjacent jint values from the game's pixel data (field evidence:
         * 0xffffffff00460000 = ARGB white + ARGB transparent dark-green).
         * object_get_class() above survives garbage (is_heap_ptr + header
         * validation), but this direct exception->fields[0] read did NOT:
         * on 64-bit sizeof(ObjectHeader)==24, so the FIRST dereference of a
         * garbage "object" lands at exactly ptr+0x18 — the reported fault
         * address 0xffffffff00460018. Guard the receiver with the
         * heap_java_object_valid() net (session-76 [HT-KEY-VALIDATE]
         * discipline); read the message length via the validated
         * string_length() instead of a raw msg->length (same +0x18 trap). */
        if (exception && heap_java_object_valid(exception)) {
            JavaString* msg = (JavaString*)exception->fields[0].ref;  /* detailMessage is usually first field */
            if (msg) {
                jsize msg_len = string_length(msg);
                const jchar* msg_chars = string_chars(msg);
                if (msg_len > 0 && msg_chars) {
                    LOG_SAFE("[ATHROW]   Message: %.*s\n", (int)msg_len, (char*)msg_chars);
                }
            }
        }
        
        athrow_log_count++;
        if (athrow_log_count == 60) {
            LOG_SAFE("[ATHROW] Further exception logging suppressed\n");
        }
    }
    
    if (!exception) {
        native_throw_npe(jvm, thread);
        return -1;
    }
    
    /* [BT-CRASH-FIX] A garbage pseudo-reference (e.g. two adjacent jints
     * from pixel data sitting in a ref slot) must NEVER enter
     * pending_exception: every later dereference on the propagation path
     * (handler lookup, printStackTrace, getMessage, thread-death logging)
     * would be a native crash. Detect it here, at the single choke point
     * every athrow passes through, and substitute a clean catchable
     * InternalError — the game keeps running (J2ME phone semantics: an
     * internal VM error must not kill the process) and the trace names the
     * site. Rate-limited log; the thrown InternalError carries the site. */
    if (!heap_java_object_valid(exception)) {
        static int athrow_garbage_log = 0;
        if (athrow_garbage_log < 20) {
            athrow_garbage_log++;
            LOG_SAFE("[ATHROW] CORRUPTED exception reference %p at %s.%s PC=%d "
                     "-> InternalError (ref slot held non-object data)\n",
                     (void*)exception,
                     frame && frame->clazz && frame->clazz->class_name ? frame->clazz->class_name : "?",
                     frame && frame->method && frame->method->name ? frame->method->name : "?",
                     frame ? (int)frame->pc - 1 : -1);
        }
        jvm_throw_by_name(jvm, "java/lang/InternalError", "corrupted exception reference (athrow)");
        thread->pending_exception = jvm_exception_pending(jvm);
        return -1;
    }
    
    jvm_throw(jvm, exception);
    return -1;  /* Signal exception */
}

int op_checkcast(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)thread;
    uint16_t index = FETCH_U2(frame);

    /* DEBUG: Log stack state before PEEK for ITestInterface1 checkcasts */
    const char* debug_class_name = classfile_get_class_name(frame->clazz, index);
    static int checkcast_debug_count = 0;
    if (g_j2me_runtime_debug && debug_class_name && strstr(debug_class_name, "ITestInterface")) {
        checkcast_debug_count++;
        LOG_SAFE("[CHECKCAST_DEBUG #%d] frame=%p, &frame->stack=%p, stack_top=%d\n",
                checkcast_debug_count, (void*)frame, (void*)frame->stack, frame->stack_top);
        for (int i = frame->stack_top; i >= 0 && i > frame->stack_top - 4; i--) {
            LOG_SAFE("[CHECKCAST_DEBUG]   &stack[%d]=%p, stack[%d]=%p\n",
                    i, (void*)&frame->stack[i], i, (void*)frame->stack[i].ref);
        }
    }

    JavaValue peek_val = PEEK(frame);
    JavaObject* obj = (JavaObject*)peek_val.ref;

    const char* class_name = classfile_get_class_name(frame->clazz, index);

    /* DEBUG: Log checkcast for Exception-related classes */
    static int checkcast_log_count = 0;
    if (g_j2me_runtime_debug) {
        checkcast_log_count++;
        if (checkcast_log_count <= 50 && class_name) {
            JavaClass* obj_class = obj ? object_get_class(obj) : NULL;
            LOG_SAFE("[CHECKCAST #%d] obj=%p obj_class=%s target=%s\n",
                    checkcast_log_count,
                    (void*)obj,
                    obj_class ? (obj_class->class_name ? obj_class->class_name : "?") : "NULL",
                    class_name);
        }
    }
    
    if (obj) {
        /* Validate object is in heap before proceeding */
        if (!is_heap_ptr_check(obj)) {
            /* Check if this might be a JavaClass* (Class object) */
            JavaClass* potential_class = (JavaClass*)obj;
            
            /* [ARGGUARD] v36.49: мусорный obj не deref-им: проба только
             * для правдоподобных указателей; мусор -> ClassCastException
             * (тот же исход, что при проваленной Ryujinx-подавленной пробе). */
            if (!argguard_bad_ptr((uintptr_t)obj) && potential_class->header.clazz && 
                potential_class->header.clazz->class_name &&
                strcmp(potential_class->header.clazz->class_name, "java/lang/Class") == 0) {
                /* This is a Class object - check if target is java/lang/Class or Object */
                if (class_name && 
                    (strcmp(class_name, "java/lang/Class") == 0 ||
                     strcmp(class_name, "java/lang/Object") == 0)) {
                    return 0;  /* Cast succeeds */
                }
            }
            
            jvm_throw_by_name(jvm, "java/lang/ClassCastException", "Object not in heap");
            return -1;
        }
        
        JavaClass* target = jvm_load_class(jvm, class_name);
        JavaClass* obj_class = object_get_class(obj);
        
        if (!obj_class) {
            jvm_throw_by_name(jvm, "java/lang/ClassCastException", "Invalid object");
            return -1;
        }
        
        if (!object_instance_of(obj, target)) {
            jvm_throw_by_name(jvm, "java/lang/ClassCastException", NULL);
            return -1;
        }
    }
    
    return 0;
}

int op_instanceof(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)thread;
    uint16_t index = FETCH_U2(frame);
    JavaObject* obj = (JavaObject*)POP(frame).ref;
    
    JavaValue result = { .i = 0 };
    
    if (obj) {
        const char* class_name = classfile_get_class_name(frame->clazz, index);
        JavaClass* target = jvm_load_class(jvm, class_name);
        result.i = object_instance_of(obj, target) ? 1 : 0;
    }
    
    PUSH(frame, result);
    return 0;
}

int op_monitorenter(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    JavaObject* obj = (JavaObject*)POP(frame).ref;
    
    if (!obj) {
        native_throw_npe(jvm, thread);
        return -1;
    }
    
    int mon_rc;
    /* v36.57 [MON-SITE]: записываем сайт входа ДО monitor_enter — долгое
     * ожидание будет названо в [MON-WAIT] с точностью до метода/pc. */
    {
        extern void jvm_mon_site_note(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
        jvm_mon_site_note("block %s.%s@%u",
                          (frame && frame->clazz && frame->clazz->class_name) ? frame->clazz->class_name : "?",
                          (frame && frame->method && frame->method->name) ? frame->method->name : "?",
                          frame ? (unsigned)frame->pc : 0u);
    }
    mon_rc = monitor_enter(jvm, obj);
    /* v36.57 [PAINT-BUDGET]: pump-поток исчерпал бюджет ожидания монитора
     * (paint заблокирован чужим synchronized — см. threads.c). Бросаем
     * обычный RuntimeException: unwind освобождает мониторы этого кадра
     * (v34.99 MONITOR-UNWIND), paint() прерывается, насос возвращается к
     * главному циклу — эмулятор остаётся живым (ввод/пауза/выход). */
    if (mon_rc != JNI_OK) {
        extern int jvm_paint_lock_aborted(void);
        if (jvm_paint_lock_aborted()) {
            jvm_throw_by_name(jvm, "java/lang/RuntimeException",
                              "nojme paint-lock budget exceeded");
        }
        return mon_rc;
    }
    /* v34.99 MONITOR-UNWIND: record block-level ownership so an exception
     * unwinding out of this frame RELEASES the monitor (JVM spec: monitors
     * acquired by monitorenter are released when the frame is abandoned).
     * Without this, a leaked monitor was owned forever and the next
     * monitor_enter from another OS thread (frontend paint thread) blocked
     * forever — the standalone countdown-freeze root cause. */
    if (mon_rc == JNI_OK && frame) {
        if (frame->mon_owned_n < MON_OWNED_MAX) {
            frame->mon_owned[frame->mon_owned_n++] = obj;
        } else {
            static int mon_track_overflow_logs = 0;
            if (mon_track_overflow_logs < 5) {
                mon_track_overflow_logs++;
                LOG_SAFE("[MON-TRACK] ownership list full (%d) in %s.%s — unwind release not tracked for this acquire\n",
                         MON_OWNED_MAX,
                         frame->clazz && frame->clazz->class_name ? frame->clazz->class_name : "?",
                         frame->method && frame->method->name ? frame->method->name : "?");
            }
        }
    }
    return mon_rc;
}

int op_monitorexit(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    JavaObject* obj = (JavaObject*)POP(frame).ref;
    
    if (!obj) {
        native_throw_npe(jvm, thread);
        return -1;
    }
    
    if (monitor_exit(jvm, obj) != JNI_OK) {
        /* v18 (audit): JVMS monitorexit — releasing a monitor not owned by the
         * current thread must throw IllegalMonitorStateException as a normal
         * catchable exception, never abort the interpreter silently. */
        native_throw_illegal_monitor_state(jvm, thread);
        return -1;
    }
    /* v34.99 MONITOR-UNWIND: drop the ownership record (last matching) so
     * the frame-exit drain does not double-release. */
    if (frame) {
        for (int mo_i = frame->mon_owned_n - 1; mo_i >= 0; mo_i--) {
            if (frame->mon_owned[mo_i] == obj) {
                for (int mo_k = mo_i; mo_k < frame->mon_owned_n - 1; mo_k++)
                    frame->mon_owned[mo_k] = frame->mon_owned[mo_k + 1];
                frame->mon_owned[frame->mon_owned_n - 1] = NULL;
                frame->mon_owned_n--;
                break;
            }
        }
    }
    return 0;
}

/* Wide instructions */
int op_wide(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm;
    (void)thread;
    uint8_t opcode = FETCH_U1(frame);
    uint16_t index = FETCH_U2(frame);
    JavaValue v1, v2;
    
    LOG_OPCODE("[EXEC] wide instruction: opcode=0x%02X, index=%d\n", opcode, index);
    
    switch (opcode) {
        case OPC_ILOAD:
        case OPC_FLOAD:
        case OPC_ALOAD:
            if (load_local(frame, index, &v1) != 0) return -1;
            PUSH(frame, v1);
            break;
            
        case OPC_LLOAD:
        case OPC_DLOAD:
            if (load_local(frame, index, &v1) != 0) return -1;
            if (load_local(frame, index + 1, &v2) != 0) return -1;
            PUSH(frame, v1);
            PUSH(frame, v2);
            break;
            
        case OPC_ISTORE:
        case OPC_FSTORE:
        case OPC_ASTORE:
            v1 = POP(frame);
            if (store_local(frame, index, v1) != 0) return -1;
            break;
            
        case OPC_LSTORE:
        case OPC_DSTORE:
            v2 = POP(frame);
            v1 = POP(frame);
            if (store_local(frame, index + 1, v2) != 0) return -1;
            if (store_local(frame, index, v1) != 0) return -1;
            break;
            
        case OPC_IINC: {
            int16_t constant = FETCH_S2(frame);
            if (load_local(frame, index, &v1) != 0) return -1;
            v1.i += constant;
            if (store_local(frame, index, v1) != 0) return -1;
            break;
        }
            
        case OPC_RET:
            if (load_local(frame, index, &v1) != 0) return -1;
            frame->pc = v1.i;
            break;
            
        default:
            LOG_SAFE("[EXEC] Unknown wide opcode: 0x%02X\n", opcode);
            return -1;
    }
    
    return 0;
}

/* v24 FIX (Brick Breaker load hang): build one level of a multi-dimensional
 * array from its EXACT descriptor. The old code skipped ALL '[' characters to
 * find the base element type and never stored an element_class on the inner
 * array levels, so int[][][]/String[][][] walls were built with element_class
 * = NULL at every reference level. Any later checkcast/instanceof/aastore on
 * those rows then mis-derived descriptors ("[L[I;") and threw cascading
 * exceptions that killed the game's loader ~1/3 through.
 *
 * This helper strips exactly ONE '[' per level:
 *   desc="[[[I":                level0 desc="[[[I" -> DESC_ARRAY elem_class "[[I"
 *                               level1 desc="[[I"  -> DESC_ARRAY elem_class "[I"
 *                               level2 desc="[I"   -> T_INT (primitive row)
 *   desc="[[Ljava/lang/String;": level0 -> DESC_ARRAY elem_class "[Ljava/lang/String;"
 *                                level1 -> DESC_OBJECT elem_class String
 * Array classes are resolved through jvm_load_class (generic stub named by the
 * descriptor, e.g. "[I"), which object_instance_of now matches exactly. */
static JavaArray* create_multidim_array_desc(JVM* jvm, const char* desc,
                                             jint* sizes, int dimensions, int level) {
    if (level >= dimensions || !desc || desc[0] != '[') return NULL;
    
    const char* elem_desc = desc + 1;  /* strip exactly one '[' */
    int elem_type = T_INT;
    JavaClass* elem_class = NULL;
    
    if (*elem_desc == '[') {
        /* Element is itself an array: resolve its array class by descriptor */
        elem_type = DESC_ARRAY;
        elem_class = jvm_load_class(jvm, elem_desc);
    } else if (*elem_desc == 'L') {
        elem_type = DESC_OBJECT;
        char name[512];
        const char* semi = strchr(elem_desc, ';');
        size_t len = semi ? (size_t)(semi - elem_desc - 1) : strlen(elem_desc + 1);
        if (len >= sizeof(name)) len = sizeof(name) - 1;
        memcpy(name, elem_desc + 1, len);
        name[len] = '\0';
        elem_class = jvm_load_class(jvm, name);
    } else {
        switch (*elem_desc) {
            case 'Z': elem_type = T_BOOLEAN; break;
            case 'B': elem_type = T_BYTE; break;
            case 'C': elem_type = T_CHAR; break;
            case 'S': elem_type = T_SHORT; break;
            case 'I': elem_type = T_INT; break;
            case 'J': elem_type = T_LONG; break;
            case 'F': elem_type = T_FLOAT; break;
            case 'D': elem_type = T_DOUBLE; break;
            default: elem_type = T_INT; break;
        }
    }
    
    JavaArray* array = (JavaArray*)jvm_new_array(jvm, elem_type, sizes[level], elem_class);
    if (!array) return NULL;
    
    if (level + 1 < dimensions) {
        for (jint i = 0; i < sizes[level]; i++) {
            JavaArray* inner = create_multidim_array_desc(jvm, elem_desc, sizes, dimensions, level + 1);
            if (inner) {
                array_set_ref(array, i, inner);
            }
        }
    }
    
    return array;
}

int op_multianewarray(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)thread;
    uint16_t class_index = FETCH_U2(frame);
    uint8_t dimensions = FETCH_U1(frame);
    
    /* Get array class name */
    const char* class_name = NULL;
    if (class_index > 0 && class_index < frame->clazz->constant_pool_count) {
        class_name = classfile_get_class_name(frame->clazz, class_index);
    }
    
    /* Pop dimension sizes from stack (in reverse order) */
    jint* sizes = (jint*)malloc(dimensions * sizeof(jint));
    if (!sizes) return -1;
    
    for (int i = dimensions - 1; i >= 0; i--) {
        sizes[i] = POP(frame).i;
        if (sizes[i] < 0) {
            free(sizes);
            /* v18 (audit): JVMS multianewarray — a negative dimension must
             * throw a catchable NegativeArraySizeException, not silently
             * abort the interpreter. */
            native_throw_negative_array_size(jvm, thread);
            return -1;
        }
    }
    
    /* v24 FIX: build the whole tree from the exact CP descriptor, storing the
     * proper element_class on EVERY reference level (see
     * create_multidim_array_desc above). This also fixes dimensions=1 with a
     * multi-'[' descriptor (e.g. multianewarray "[[I" 1 must yield int[][],
     * not int[]) and the dimensions==2 case for object elements. */
    if (!class_name || class_name[0] != '[') {
        free(sizes);
        LOG_SAFE("[EXEC] multianewarray: CP entry '%s' is not an array descriptor\n",
                 class_name ? class_name : "(null)");
        jvm_throw_by_name(jvm, "java/lang/ClassNotFoundException", class_name);
        return -1;
    }
    
    JavaArray* result_array = create_multidim_array_desc(jvm, class_name, sizes, dimensions, 0);
    if (!result_array) {
        free(sizes);
        /* v18 (audit): allocation failed — OOM is already pending from the
         * allocator; do NOT push a fake success value. */
        return -1;
    }
    
    LOG_DEBUG("[EXEC] multianewarray: created %dD array (%s)\n", dimensions, class_name);
    JavaValue v = { .ref = result_array };
    PUSH(frame, v);
    
    free(sizes);
    return 0;
}

int op_ifnull(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    int16_t offset = FETCH_S2(frame);
    void* value = POP(frame).ref;
    
    if (value == NULL) {
        frame->pc += offset - 3;
    }
    
    return 0;
}

int op_ifnonnull(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    int16_t offset = FETCH_S2(frame);
    void* value = POP(frame).ref;
    
    if (value != NULL) {
        frame->pc += offset - 3;
    }
    
    return 0;
}

int op_goto_w(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    int32_t offset = (int32_t)jvm_read_u32(frame->code + frame->pc);
    frame->pc += 4 + offset - 5;
    return 0;
}

int op_jsr_w(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    (void)jvm; (void)thread;
    int32_t offset = (int32_t)jvm_read_u32(frame->code + frame->pc);
    JavaValue ret_addr = { .i = frame->pc + 4 };
    PUSH(frame, ret_addr);
    /* v18 (audit): jsr_w opcode is 5 bytes; pc points at the operand start
     * after the fetch, so the branch target relative to the START of the
     * opcode is (pc - 1) + offset = pc + offset - 1. */
    frame->pc += offset - 1;
    return 0;
}