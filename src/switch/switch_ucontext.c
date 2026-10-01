/* switch_ucontext.c — v34.87
 *
 * Minimal aarch64 ucontext implementation for the Switch port.
 * See switch_ucontext.h for the rationale. Layout pinned by the asserts
 * below; the assembly offsets MUST match them:
 *
 *   +0    uc_link                 (struct ucontext *)
 *   +8    uc_stack.ss_sp          (void *)
 *   +16   uc_stack.ss_size        (size_t)
 *   +24   uc_stack.ss_flags       (int) + pad
 *   +32   x19, x20 ... x29, x30   (12 x 8 bytes, x19 at +32, x30 at +120)
 *   +128  sp
 *   +136  pc
 *   +144  d8, d9 ... d14, d15     (8 x 8 bytes)
 *
 * The implementation follows the classic musl/libucontext pattern:
 *  - getcontext stores its own return address as the resume PC, so a later
 *    setcontext/swapcontext lands right after the getcontext call with x0=0;
 *  - swapcontext does save-into-oucp then restore-from-ucp in one hop;
 *  - makecontext zeroes the register banks, points PC at func, the x30 slot
 *    at the exit trampoline and x19 at the ucontext itself (x19 is
 *    callee-saved, so the thread function's epilogue restores it before
 *    returning to the trampoline);
 *  - the trampoline tail-calls setcontext(uc->uc_link); a missing uc_link
 *    executes brk #1 (unreachable: threads.c always sets uc_link).
 *
 * The context-switch functions are emitted as top-level assembly (label
 * definitions, no C bodies) so that -Wall -Wextra stay silent - a naked C
 * function whose asm always branches away triggers -Wreturn-type.
 *
 * Functional test: scripts/switch_ucontext_test.c (+ .sh) builds this file
 * freestanding and runs it under qemu-aarch64, verifying register/FP
 * preservation, SP alignment and the uc_link trampoline.
 */
#include "switch_ucontext.h"

#if defined(__SWITCH__) && defined(__aarch64__)

#include <stddef.h>
#include <string.h>
#include <stdlib.h>   /* abort() for the argc != 0 contract */

_Static_assert(sizeof(ucontext_t) == 208, "switch_ucontext: struct grew - update asm offsets");
_Static_assert(offsetof(ucontext_t, uc_link)    ==   0, "switch_ucontext: uc_link offset");
_Static_assert(offsetof(ucontext_t, uc_stack)   ==   8, "switch_ucontext: uc_stack offset");
_Static_assert(offsetof(ucontext_t, x19_x30)    ==  32, "switch_ucontext: x19 offset");
_Static_assert(offsetof(ucontext_t, sp)         == 128, "switch_ucontext: sp offset");
_Static_assert(offsetof(ucontext_t, pc)         == 136, "switch_ucontext: pc offset");
_Static_assert(offsetof(ucontext_t, d8_d15)     == 144, "switch_ucontext: d8 offset");

/* The exit trampoline, referenced by makecontext below. */
extern void switch_uc_exit_trampoline(void);

__asm__(
".text\n"
".p2align 4\n"
".globl  getcontext\n"
".hidden getcontext\n"
".type   getcontext, %function\n"
"getcontext:\n"
"   stp x19, x20, [x0, #32]\n"
"   stp x21, x22, [x0, #48]\n"
"   stp x23, x24, [x0, #64]\n"
"   stp x25, x26, [x0, #80]\n"
"   stp x27, x28, [x0, #96]\n"
"   stp x29, x30, [x0, #112]\n"
"   mov x1, sp\n"
"   str x1, [x0, #128]\n"
"   str x30, [x0, #136]\n"        /* resume at getcontext's caller */
"   stp d8,  d9,  [x0, #144]\n"
"   stp d10, d11, [x0, #160]\n"
"   stp d12, d13, [x0, #176]\n"
"   stp d14, d15, [x0, #192]\n"
"   mov x0, #0\n"
"   ret\n"
".size getcontext, .-getcontext\n"
);

__asm__(
".text\n"
".p2align 4\n"
".globl  setcontext\n"
".hidden setcontext\n"
".type   setcontext, %function\n"
"setcontext:\n"
"   ldp x19, x20, [x0, #32]\n"
"   ldp x21, x22, [x0, #48]\n"
"   ldp x23, x24, [x0, #64]\n"
"   ldp x25, x26, [x0, #80]\n"
"   ldp x27, x28, [x0, #96]\n"
"   ldp x29, x30, [x0, #112]\n"
"   ldp d8,  d9,  [x0, #144]\n"
"   ldp d10, d11, [x0, #160]\n"
"   ldp d12, d13, [x0, #176]\n"
"   ldp d14, d15, [x0, #192]\n"
"   ldr x1, [x0, #128]\n"         /* sp */
"   ldr x2, [x0, #136]\n"         /* pc */
"   mov sp, x1\n"
"   mov x0, #0\n"                 /* getcontext/swapcontext return 0 */
"   br  x2\n"
".size setcontext, .-setcontext\n"
);

__asm__(
".text\n"
".p2align 4\n"
".globl  swapcontext\n"
".hidden swapcontext\n"
".type   swapcontext, %function\n"
"swapcontext:\n"
"   stp x19, x20, [x0, #32]\n"    /* --- save current into oucp (x0) --- */
"   stp x21, x22, [x0, #48]\n"
"   stp x23, x24, [x0, #64]\n"
"   stp x25, x26, [x0, #80]\n"
"   stp x27, x28, [x0, #96]\n"
"   stp x29, x30, [x0, #112]\n"
"   mov x2, sp\n"
"   str x2, [x0, #128]\n"
"   str x30, [x0, #136]\n"        /* resume at swapcontext's caller */
"   stp d8,  d9,  [x0, #144]\n"
"   stp d10, d11, [x0, #160]\n"
"   stp d12, d13, [x0, #176]\n"
"   stp d14, d15, [x0, #192]\n"
"   ldp x19, x20, [x1, #32]\n"    /* --- resume ucp (x1) --- */
"   ldp x21, x22, [x1, #48]\n"
"   ldp x23, x24, [x1, #64]\n"
"   ldp x25, x26, [x1, #80]\n"
"   ldp x27, x28, [x1, #96]\n"
"   ldp x29, x30, [x1, #112]\n"
"   ldp d8,  d9,  [x1, #144]\n"
"   ldp d10, d11, [x1, #160]\n"
"   ldp d12, d13, [x1, #176]\n"
"   ldp d14, d15, [x1, #192]\n"
"   ldr x2, [x1, #128]\n"         /* sp */
"   ldr x3, [x1, #136]\n"         /* pc */
"   mov sp, x2\n"
"   mov x0, #0\n"
"   br  x3\n"
".size swapcontext, .-swapcontext\n"
);

/* Exit trampoline: entered when a makecontext-created thread function
 * returns. x19 (callee-saved, restored by the function's epilogue) holds
 * the ucontext_t of the finished thread. */
__asm__(
".text\n"
".p2align 4\n"
".globl  switch_uc_exit_trampoline\n"
".hidden switch_uc_exit_trampoline\n"
".type   switch_uc_exit_trampoline, %function\n"
"switch_uc_exit_trampoline:\n"
"   ldr x0, [x19]\n"              /* x0 = uc_link */
"   cbz x0, 1f\n"
"   b   setcontext\n"             /* tail-call; never returns */
"1:\n"
"   brk #1\n"                     /* no uc_link configured - developer error */
".size switch_uc_exit_trampoline, .-switch_uc_exit_trampoline\n"
);

void makecontext(ucontext_t *ucp, void (*func)(void), int argc, ...)
{
    if (argc != 0) {
        /* The cooperative scheduler only ever creates zero-argument
         * contexts (makecontext(&ctx, thread_entry_wrapper, 0)); refuse
         * anything else loudly rather than guessing an argument ABI. */
        abort();
    }
    /* Zero the register banks (x19 gets the self pointer below). */
    memset(ucp->x19_x30, 0, sizeof(ucp->x19_x30));
    memset(ucp->d8_d15, 0, sizeof(ucp->d8_d15));
    ucp->sp = 0;
    ucp->pc = 0;

    /* 16-byte aligned top of the provided stack. aarch64 requires SP
     * alignment of 16 at (function entry / call boundaries); the frame is
     * the callee's own business. */
    uint64_t top = (uint64_t)ucp->uc_stack.ss_sp + (uint64_t)ucp->uc_stack.ss_size;
    top &= ~(uint64_t)0xF;

    ucp->x19_x30[0]  = (uint64_t)ucp;                        /* x19 = self */
    ucp->x19_x30[11] = (uint64_t)&switch_uc_exit_trampoline; /* x30 slot  */
    ucp->sp          = top;
    ucp->pc          = (uint64_t)func;
}

#endif /* __SWITCH__ && __aarch64__ */
