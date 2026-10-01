/* switch_ucontext.h — v34.87
 *
 * Minimal ucontext replacement for the Nintendo Switch port (aarch64 /
 * devkitPro newlib + libnx).
 *
 * WHY: the JVM's cooperative scheduler (src/jvm/threads.c) is built on the
 * POSIX <ucontext.h> API (getcontext / setcontext / swapcontext /
 * makecontext). glibc (Linux), musl and macOS all ship it; newlib does NOT
 * - devkitA64 compiles threads.c happily up to `#include <ucontext.h>`
 * and then dies with "fatal error: ucontext.h: No such file or directory".
 * libnx has no equivalent either (its threads are real kernel threads).
 *
 * WHAT: a hand-written aarch64 implementation of exactly the subset the
 * scheduler uses:
 *   - getcontext(ucp)          - save callee-saved GPRs x19..x30, SP, PC
 *                                (the return address) and d8..d15 (the
 *                                callee-saved FP registers);
 *   - setcontext(ucp)          - restore them and jump to the saved PC;
 *   - swapcontext(oucp, ucp)   - save into oucp, resume ucp;
 *   - makecontext(ucp, f, 0)   - prepare a fresh context on ucp->uc_stack
 *                                whose entry point is f; when f RETURNS,
 *                                control passes to ucp->uc_link via
 *                                setcontext (the classic trampoline).
 *   - argc != 0 is NOT supported (the scheduler only calls it with 0);
 *     it abort()s loudly instead of corrupting the context.
 *
 * Notes:
 *   - signal masks are not saved/restored: libnx has no POSIX signals;
 *   - the thread pointer is not switched: all cooperative contexts run on
 *     one native thread, so __thread variables are shared across them -
 *     exactly the semantics the glibc build has (glibc swapcontext restores
 *     the same TP value), so heap.c's TLAB / g_gc_in_native behave the same;
 *   - ucontext_t layout is INTERNAL to this implementation (newlib defines
 *     none, so there is no ABI to match); switch_ucontext.c pins it in
 *     place with _Static_asserts against the assembly offsets.
 *
 * Testing: scripts/switch_ucontext_test.c builds a freestanding static
 * binary (aarch64 cross-gcc) and runs it under qemu-aarch64 with raw
 * Linux syscalls for I/O - it verifies register/FP preservation across
 * switches, stack alignment at the entry point and the uc_link return
 * trampoline. See scripts/switch_ucontext_test.sh.
 */
#ifndef SWITCH_UCONTEXT_H
#define SWITCH_UCONTEXT_H

#include <stddef.h>
#include <stdint.h>

#if defined(__SWITCH__) && defined(__aarch64__)

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void    *ss_sp;
    size_t   ss_size;
    int      ss_flags;
    int      _pad;
} switch_stack_t;

typedef struct ucontext {
    struct ucontext *uc_link;     /* +0   resumed when the thread func returns */
    switch_stack_t   uc_stack;    /* +8   ss_sp@+8, ss_size@+16, ss_flags@+24 */
    uint64_t         x19_x30[12]; /* +32  callee-saved GPRs, x19 at index 0 */
    uint64_t         sp;          /* +128 stack pointer for resume */
    uint64_t         pc;          /* +136 resume address */
    uint64_t         d8_d15[8];   /* +144 callee-saved FP registers */
} ucontext_t;

/* Hidden visibility: these live inside the NRO / the libretro core archive;
 * keeping them local avoids any interaction with the host environment. */
__attribute__((visibility("hidden")))
int  getcontext(ucontext_t *ucp);

__attribute__((visibility("hidden")))
int  setcontext(const ucontext_t *ucp);

__attribute__((visibility("hidden")))
int  swapcontext(ucontext_t *oucp, const ucontext_t *ucp);

__attribute__((visibility("hidden")))
void makecontext(ucontext_t *ucp, void (*func)(void), int argc, ...);

#ifdef __cplusplus
}
#endif

#endif /* __SWITCH__ && __aarch64__ */
#endif /* SWITCH_UCONTEXT_H */
