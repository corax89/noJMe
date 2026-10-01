/*
 * NOJME VM statistics (v34.26) — env-gated counters for performance triage.
 *
 * Motivation: the user reports stutter + dropped frames on ARMv7 (m17).
 * Before optimizing we need to know WHERE the time goes and HOW the
 * cooperative scheduler actually behaves per frame:
 *   - instructions/s, yields/s, context switches/s (swapcontext on ARM
 *     costs 2 rt_sigprocmask syscalls each — expensive at kHz rates)
 *   - sleep wake latency (how LATE a Thread.sleep wake happens — jitter
 *     source when nothing pumps the scheduler between frames)
 *   - how many execute_frame budget steps actually run (the retro_run
 *     drive path) vs paint-pump driven execution
 *   - wall time of retro_run and of the VM section inside it
 *
 * Enable with NOJME_STATS=1. Report goes through the always-on log channel
 * (MISSING_LOG / j2me_log_ungated) so it appears even with logging off,
 * every N frames from retro_run. Zero overhead when disabled: the only
 * always-on touch points are a non-volatile counter bump in the interpreter
 * fetch (same cost class as the pre-existing jvm->instr_count++) and a few
 * rare-event increments (yield/switch/wake — max a few kHz).
 */
#ifndef VM_STATS_H
#define VM_STATS_H

#include <stdint.h>
#include <stdbool.h>

typedef struct VmStats {
    /* instruction totals (main loop + frame-stepping path combined) */
    uint64_t instr;              /* interpreted opcodes total */
    /* scheduler behaviour */
    uint64_t yields;             /* thread_yield() invocations */
    uint64_t ctx_swaps;          /* actual swapcontext/setcontext switches */
    uint64_t sleep_calls;        /* Thread.sleep() cooperative parks */
    uint64_t wake_count;         /* threads woken from sleep list */
    uint64_t wake_late_ms_sum;   /* sum(now - wake_time) over wakes, ms */
    uint64_t wake_late_ms_max;   /* worst single wake lateness, ms */
    /* retro_run drive path */
    uint64_t execute_frame_calls;/* budgeted steps taken in retro_run */
    uint64_t paint_calls;        /* paint() invocations from repaint pump */
    /* timing */
    uint64_t frames;             /* retro_run() invocations */
    uint64_t run_us;             /* total wall time inside retro_run */
    uint64_t vm_us;              /* time in the VM section (JVM+timers+pumps) */
    uint64_t run_us_max;         /* worst single retro_run */
    uint64_t vm_us_max;          /* worst single VM section */
    /* reported snapshot bookkeeping */
    uint64_t last_report_instr;
} VmStats;

extern VmStats g_vmstats;
extern int g_vmstats_enabled;    /* 0 = off (default), 1 = NOJME_STATS=1 */

/* One-line report through the always-on channel. Call periodically
 * (e.g. every 60 frames from retro_run). Never called when disabled. */
void vm_stats_report(void);

#endif /* VM_STATS_H */
