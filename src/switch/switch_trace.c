/*
 * switch_trace.c — v34.91 freeze-triage implementation. See switch_trace.h
 * for the design. Linked into the Switch NRO and the host verification
 * builds only (never into the linux desktop app / libretro core).
 *
 * Log line format: "<ticks-ms> <text>\n" — one open(O_APPEND)+write+close
 * per line (stdio buffers would lose the tail exactly when it matters: a
 * hard hang or a crash). A first failure permanently disables file logging.
 */
#include "switch/switch_trace.h"

#if defined(__SWITCH__) || defined(NOJME_SWITCH_TRACE)

#include "switch/switch_glue.h"
#include "switch/switch_common.h" /* SWITCH_UI_WIDTH/HEIGHT */
#include "switch/switch_font.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h> /* v35.03: CLOCK_MONOTONIC for the [GC-SLOW] recency check */

/* v34.94: heap counters for the diag line (JVM linked into every build
 * that links this file). include/ is on the include path (-I$(INCDIR)). */
#include "jvm.h"
#include "heap.h"

/* SDL headers (same discovery order as sdl_graphics.c): switch portlibs
 * expose both layouts, host pkg-config adds -I.../SDL2, the sandbox test
 * snapshot has SDL.h at its root. */
#if defined(__has_include)
#  if __has_include(<SDL2/SDL.h>)
#    include <SDL2/SDL.h>
#  elif __has_include(<SDL.h>)
#    include <SDL.h>
#  endif
#else
#  include <SDL.h>
#endif

#define TRACE_LINE_CAP 320
#define TRACE_LOG_CAP  (1024 * 1024)   /* v34.95: 1 MB — the per-second thread dump eats budget */

/* ========================================================================
 * v34.94 DIAG counters + logging gate.
 * ======================================================================== */

/* --- logging gate (v34.92 feature recreated): default OFF, the settings
 * row "Лог" flips it; the enable/disable event itself always reaches the
 * log so the user can see WHEN it was toggled. Implemented in switch_ui.c. */
extern int switch_settings_logging_flag(void);

static int g_logging = -1;   /* -1 = not probed yet (reads settings) */

static int logging_on(void) {
    if (g_logging < 0) {
        /* First use: NOJME_LOGGING=1|0 env wins (host verification builds
         * run the __SWITCH__ code path where settings.ini lives under
         * sdmc: and is unreadable); otherwise the settings toggle. */
        const char* e = getenv("NOJME_LOGGING");
        if (e && e[0]) {
            g_logging = (strcmp(e, "0") == 0) ? 0 : 1;
        } else {
            g_logging = switch_settings_logging_flag() ? 1 : 0;
        }
    }
    return g_logging;
}

int sw_logging_enabled(void) { return logging_on(); }

/* Live toggle from the settings screen (persisted there via settings.ini). */
void sw_logging_set(int on) {
    g_logging = on ? 1 : 0;
}

/* --- diag counters (see switch_trace.h for semantics) --- */
volatile uint32_t g_sd_loop = 0, g_sd_pr = 0, g_sd_skip = 0, g_sd_pump = 0;
volatile uint32_t g_sd_stl_ms = 0, g_sd_sr_ms = 0, g_sd_fl = 0;
volatile uint32_t g_sd_vs = 0;
volatile uint32_t g_sd_pv_ms = 0, g_sd_pv_ms_max = 0, g_sd_pw_ms_max = 0;
volatile uint32_t g_sd_iter_ms_max = 0;
/* v35.02 logic-heartbeat: pp = repaint pumps COMPLETED (took the lock),
 * psk = pumps skipped (UI lock busy), cs = callSerially game-logic Runnables
 * executed on the frontend thread. The v35.01 field analysis pinned the
 * "race crawls ~1 Hz then freezes" on the callSerially drain starvation —
 * cs is ITS pulse: healthy = roughly one third of loop (30/s at 30 fps
 * game), crawl = cs collapsing to single digits, freeze = cs == 0 while
 * bn keeps counting (renders alive, logic dead). */
volatile uint32_t g_sd_pp = 0, g_sd_pump_skip = 0, g_sd_csq = 0;
/* v35.04 stage-cost gauges: worst single execution (ms) of the three
 * frontend-loop stages that host VM work, per diag period. Splits a slow
 * iteration across the actual cost center — the v35.03 race trace showed
 * 19 ms iterations with present down to 1-3 ms, but loop/stl/mx cannot
 * name WHICH stage ate the budget:
 *   tj = "timers"      — jvm_process_timers + key-queue aging (scheduler)
 *   rj = "repaints"    — the single-pump repaint (paint() runs HERE on
 *                        the frontend in the v35.02 protocol)
 *   sj = "callserially" — the game-logic Runnable drain
 * Healthy race phase: tj<=2, rj<=6 (bind), sj<=2. A climbing field while
 * mx grows names the stage to blame without a second trace. */
volatile uint32_t g_sd_tj_ms_max = 0, g_sd_rj_ms_max = 0, g_sd_sj_ms_max = 0;
volatile uint32_t g_sd_lvc = 0, g_sd_dset = 0;   /* v35.06 direct-render delivery */
volatile uint32_t g_sd_utf = 0, g_sd_rcf = 0, g_sd_ch = 0, g_sd_chf = 0; /* v36.43 [PRESENT-GLASS] */
/* v36.44 [SPLIT-PROBE]: chg = пробы, где изменился ХОТЯ БЫ пиксель внутри
 * прямоугольника игры (g_game_rect); chb = пробы, где изменилось что-то
 * ВНЕ его (блюр-поля/оверлеи). Полевой фриз Yeti («вид игры замер на
 * «5 second(s)», блюр-поля живут») требует РАЗДЕЛЬНЫХ ответов: единый ch
 * не отличает «весь композит жив» от «жив только угол/фон». */
volatile uint32_t g_sd_chg = 0, g_sd_chb = 0;
volatile const char* g_sd_loop_stage = NULL;
volatile uint64_t g_sd_loop_ms = 0;

/* v35.01: session-high watermark of alive VM runners (written by the beat
 * callback; a drop below it = a thread DIED) + the stall-log throttle.
 * File scope so sw_diag_session_reset can clear the watermark per session. */
static int s_live_hwm = 0;
static uint64_t s_last_stall_log = 0;
static long long s_last_gcslow_log = 0; /* v35.03: [GC-SLOW] throttle (mono ms) */
static int s_slow_beats = 0; /* v35.03: sustained ~1 Hz crawl counter (1 s per beat) */
/* v35.04 [NO-FRAME]: settled-frame sequence tracking for the beat channel.
 * Fires when a VM thread is ACTIVELY RUNNING (live>0, worst_age small) but
 * no new frame has settled for 10 consecutive beats — the "game thread
 * spins, screen never updates" class that T-STALL cannot see (the thread
 * keeps progressing) and STUCK cannot see (the frontend loop is alive).
 * Field case: Bobby Carrot 4 froze on load with tn=1, ta=12-20/t2 (a live
 * runner dispatching natives every <=20 ms), fl=0 — a busy wait loop the
 * next [NO-FRAME] dump will name by last_native + displayable class. */
static uint32_t s_nf_seq = 0;
static int s_nf_beats = 0;
static long long s_last_noframe_log = 0;
#define NOJME_NF_BEATS 10
static void s_live_hwm_reset(void) { s_live_hwm = 0; }

/* v36.06 [NO-FRAME-ESC]: escalation of the NO-FRAME window into the SAME
 * graceful recovery MIDLET-STUCK uses. Field case (Doom RPG [Rus], v36.05
 * trace, 87.2M ms uptime = a day of open/close cycles): the loader thread
 * t2 spins inside a stream read native FOREVER (age=0 at every 1 s beat —
 * it keeps RE-entering the native, so the "why=" ladder and MIDLET-STUCK
 * both see a healthy live runner), heap frozen at 176 KB, no [EX-THROW],
 * no [RMS] — and the session hangs on the loading screen until the user
 * pauses manually. [NO-FRAME] fired every 15 s but only dumped.
 *
 * New rule: the settled-frame sequence stayed frozen for 30 beats (~30 s)
 * WHILE at least one VM thread is live AND the heap did not grow >64 KB
 * over the whole window => the session can never paint again => request
 * the same graceful exit. The 64 KB guard keeps a legitimately long,
 * allocation-heavy load (big games read megabytes while loading) from
 * false-firing: real work moves the heap, a corpse does not. */
#define NOJME_NF_ESC_BEATS 30
static uint32_t s_nfe_heap0 = 0;   /* heap used at window start (KB) */
static int s_nfe_state = 0;        /* 0 no baseline, 1 frozen, 2 heap moved */

/* v36.05 [MIDLET-STUCK]: dead-air self-recovery. The v36.04 field trace
 * (Doom RPG [Rus], 2nd launch) caught the terminal hang class: the game's
 * loader thread died with an uncaught NullPointerException (~1.2 s into
 * the load) while the midlet's main thread spun in bytecode forever —
 * live=0 waiting=0, no new settled frames, session immortal (pause/menu
 * still worked, so the frontend had no reason to end it). The user saw a
 * frozen loading screen and had to exit MANUALLY.
 *
 * Detector (1 s beat): a game session is on, the frontend loop is alive
 * (!stuck), no pause overlay, the settled-frame sequence has not advanced
 * AND the VM is in DEAD AIR (live==0 && waiting==0: nothing runnable,
 * nothing sleeping — nothing can ever wake up; or every live runner is
 * spinning >5 s without a native dispatch) for 15 consecutive beats.
 * A heap-growth guard aborts the window when a pure-Java worker is
 * genuinely computing (allocates => heap moves).
 *
 * Action: raise g_midp_stuck_recover_req; the SDL frame loop (the thread
 * that owns the session) consumes it and performs the SAME graceful exit
 * as the pause-menu "exit to menu" (destroyApp + stop), so the user lands
 * in the game menu instead of a dead loading screen. NOT a crash, NOT a
 * kill: the teardown path is byte-identical to a manual exit. */
#define NOJME_STUCK_BEATS 15
static uint32_t s_stk_seq = 0;
static int s_stk_beats = 0;
static uint32_t s_stk_heap0 = 0;   /* heap used at window start (KB) */
static int s_stk_valid = 0;        /* window has a baseline */
volatile int g_midp_stuck_recover_req = 0;

#define SW_DIAG_PERIOD_MS 5000
static uint64_t g_sd_period_t0 = 0;
static uint32_t g_sd_gc_base = 0;
static uint32_t g_sd_top_sample = 0;
static JVM* g_sd_jvm = NULL;   /* set by the frontend once per session */

void sw_diag_set_jvm(void* jvm) { g_sd_jvm = (JVM*)jvm; }

/* Forward decls (defined below) */
static void trace_emit_impl(const char* text, int force);
void sw_trace_force(const char* fmt, ...);

/* Weak lookups: the white-box input test links this file WITHOUT the JVM —
 * every JVM symbol below must be null-checked. */
extern uint64_t g_gc_collections __attribute__((weak));
extern HeapStats heap_get_stats(JVM* jvm) __attribute__((weak));

/* Session-monotonic allocation watermark ("top="), sampled every frame —
 * matches the v34.93 diag semantics (top only ever grew within a run). */
static uint32_t g_sd_top_hwm = 0;

static void diag_emit(void) {
    uint32_t loop = g_sd_loop, pr = g_sd_pr, skip = g_sd_skip, pump = g_sd_pump;
    uint32_t stl = g_sd_stl_ms, sr = g_sd_sr_ms, fl = g_sd_fl, vs = g_sd_vs;
    g_sd_loop = 0; g_sd_pr = 0; g_sd_skip = 0; g_sd_pump = 0;
    g_sd_stl_ms = 0; g_sd_sr_ms = 0; g_sd_fl = 0; g_sd_vs = 0;

    uint32_t gc = 0;
    if (g_sd_jvm && &g_gc_collections) {
        uint64_t c = g_gc_collections;
        gc = (uint32_t)(c > g_sd_gc_base ? (c - g_sd_gc_base) : 0);
        g_sd_gc_base = (uint32_t)c;
    }

    uint32_t heap = 0, top = 0, sb = 0;
    if (g_sd_jvm && heap_get_stats) {
        HeapStats hs = heap_get_stats(g_sd_jvm);
        heap = (uint32_t)hs.used_size;
        top = g_sd_top_hwm;
    }
    {
        extern size_t sdl_audio_queued_bytes(void);
        sb = (uint32_t)sdl_audio_queued_bytes();
    }

    /* v34.98: M3G bind pace — bn = binds this period, bd = slowest single
     * bind's buffer work (ms). A bd that climbs toward hundreds/thousands
     * of ms is the slow-bind degradation that precedes the freeze.
     * Weak + address-checked: the white-box input test links this file
     * without the JVM/M3G objects. */
    {
        extern volatile uint32_t g_sd_m3g_binds __attribute__((weak));
        extern volatile uint32_t g_sd_m3g_bind_ms_max __attribute__((weak));
        uint32_t bn = 0, bd = 0;
        if (&g_sd_m3g_binds && &g_sd_m3g_bind_ms_max) {
            bn = g_sd_m3g_binds; bd = g_sd_m3g_bind_ms_max;
            g_sd_m3g_binds = 0; g_sd_m3g_bind_ms_max = 0;
        }
        /* v35.01: game-thread liveness (tn/ta) + bind age (ma) + present
         * costs (pv/pk/pw) + worst iteration (mx). The v35.00 field trace
         * caught the NEW freeze form with the frontend loop ALIVE — the
         * frozen state itself was invisible (no STUCK, td gated by the
         * logging toggle). These forced fields make it self-reporting:
         *   tn drop  = a VM thread DIED (e.g. unhandled exception)
         *   ta large = a RUN-able thread stopped dispatching natives
         *   ma large = the paint path stopped completing binds
         *   pk/pw/mx large = the stall lives in the frontend present/loop */
        int tn = -1, tatid = -1, tw = -1;
        long long ta = -1;
        {
            extern void jvm_thread_liveness(void*, int*, long long*, int*, int*)
                __attribute__((weak));
            if (jvm_thread_liveness)
                jvm_thread_liveness((void*)g_sd_jvm, &tn, &ta, &tatid, &tw);
        }
        uint32_t ma = 0;
        {
            extern uint32_t m3g_bind_age_ms(void) __attribute__((weak));
            if (m3g_bind_age_ms) ma = m3g_bind_age_ms();
        }
        uint32_t pv = g_sd_pv_ms, pk = g_sd_pv_ms_max, pw = g_sd_pw_ms_max,
                 mx = g_sd_iter_ms_max;
        g_sd_pv_ms = 0; g_sd_pv_ms_max = 0; g_sd_pw_ms_max = 0;
        g_sd_iter_ms_max = 0;
        uint32_t pp = g_sd_pp, psk = g_sd_pump_skip, cs = g_sd_csq;
        g_sd_pp = 0; g_sd_pump_skip = 0; g_sd_csq = 0;
        /* v35.04 stage-cost gauges — worst single ms of timers/repaints/
         * callserially this period (see the definitions above). */
        uint32_t tj = g_sd_tj_ms_max, rj = g_sd_rj_ms_max, sj = g_sd_sj_ms_max;
        g_sd_tj_ms_max = 0; g_sd_rj_ms_max = 0; g_sd_sj_ms_max = 0;
        uint32_t lvc = g_sd_lvc, dset = g_sd_dset;   /* v35.06 */
        g_sd_lvc = 0; g_sd_dset = 0;
        uint32_t utf = g_sd_utf, rcf = g_sd_rcf, ch = g_sd_ch, chf = g_sd_chf;
        g_sd_utf = 0; g_sd_rcf = 0; g_sd_ch = 0; g_sd_chf = 0; /* v36.43 */
        uint32_t chg = g_sd_chg, chb = g_sd_chb;
        g_sd_chg = 0; g_sd_chb = 0; /* v36.44 [SPLIT-PROBE] */
        /* v35.03: GC-паузы — gms = последняя сборка, мс; gxm = худшая за
         * период. gc=0 всю сессию + первая сборка на границе круга =
         * кандидат в «внезапный 1 fps»: пара строк в логе это докажут
         * или исключат. */
        uint32_t gms = 0, gxm = 0;
        {
            extern volatile uint32_t g_gc_last_ms __attribute__((weak));
            extern volatile uint32_t g_gc_worst_ms __attribute__((weak));
            if (&g_gc_last_ms && &g_gc_worst_ms) {
                gms = g_gc_last_ms; gxm = g_gc_worst_ms;
                g_gc_worst_ms = 0;
            }
        }
        sw_trace_force("diag: loop=%u pr=%u stl=%u vs=%u gc=%u heap=%u top=%u skip=%u pump=%u sr=%u fl=%u sb=%u bn=%u bd=%u tn=%d ta=%lld/t%d tw=%d ma=%u pv=%u pk=%u pw=%u mx=%u pp=%u psk=%u cs=%u lvc=%u dset=%u gms=%u gxm=%u tj=%u rj=%u sj=%u utf=%u rcf=%u ch=%u chf=%u chg=%u chb=%u",
                       loop, pr, stl, vs, gc, heap, top, skip, pump, sr, fl, sb, bn, bd,
                       tn, ta, tatid, tw, ma, pv, pk, pw, mx, pp, psk, cs, lvc, dset, gms, gxm,
                       tj, rj, sj, utf, rcf, ch, chf, chg, chb);
    }
}

void sw_diag_tick(void) {
    uint64_t now = sdl_switch_ui_ticks_ms();
    g_sd_loop_ms = now;
    /* v34.94: sample the allocation watermark every frame (cheap: one
     * stats walk is NOT cheap — only every 64th call). */
    if (g_sd_jvm && heap_get_stats && ((++g_sd_top_sample & 63) == 0)) {
        HeapStats hs = heap_get_stats(g_sd_jvm);
        uint32_t used = (uint32_t)hs.used_size;
        if (used > g_sd_top_hwm) g_sd_top_hwm = used;
    }
    if (!g_sd_period_t0) g_sd_period_t0 = now;
    if (now - g_sd_period_t0 >= SW_DIAG_PERIOD_MS) {
        g_sd_period_t0 = now;
        diag_emit();
    }
}

void sw_diag_session_reset(void) {
    g_sd_gc_base = 0;
    g_sd_period_t0 = 0;
    g_sd_top_hwm = 0;
    g_sd_top_sample = 0;
    g_sd_loop = 0; g_sd_pr = 0; g_sd_skip = 0; g_sd_pump = 0;
    g_sd_stl_ms = 0; g_sd_sr_ms = 0; g_sd_fl = 0; g_sd_vs = 0;
    g_sd_pv_ms = 0; g_sd_pv_ms_max = 0; g_sd_pw_ms_max = 0;
    g_sd_iter_ms_max = 0;
    g_sd_tj_ms_max = 0; g_sd_rj_ms_max = 0; g_sd_sj_ms_max = 0; /* v35.04 */
    g_sd_lvc = 0; g_sd_dset = 0; /* v35.06 */
    g_sd_utf = 0; g_sd_rcf = 0; g_sd_ch = 0; g_sd_chf = 0; /* v36.43 */
    g_sd_chg = 0; g_sd_chb = 0; /* v36.44 [SPLIT-PROBE] */
    g_sd_loop_stage = NULL; /* no game session: the STUCK watchdog idles */
    g_sd_loop_ms = 0;
    /* v35.01: the alive-runner watermark is session-scoped — a NEW game
     * starts with a fresh thread census (the old session's high must not
     * fire [THREAD-EXIT] against the next session's teardown). */
    s_live_hwm_reset();
    /* v35.04 [NO-FRAME]: the settled-seq baseline is session-scoped too —
     * the new game's first frames must not be compared against the
     * previous session's sequence. */
    s_nf_seq = 0;
    s_nf_beats = 0;
    s_last_noframe_log = 0;
    /* v36.05 [MIDLET-STUCK]: the dead-air window is session-scoped too; a
     * stale request (raised in the session's final beats but never
     * consumed) must not kill the NEXT session. */
    s_stk_seq = 0;
    s_stk_beats = 0;
    s_stk_heap0 = 0;
    s_stk_valid = 0;
    g_midp_stuck_recover_req = 0;
}

static char g_line[TRACE_LINE_CAP];      /* on-screen status line */
static int  g_log_fd = -2;               /* -2 not probed, -1 disabled, >=0 fd */
static uint64_t g_bytes = 0;             /* bytes written (cap guard) */
static int  g_beat_on = 0;
/* v36.60 [ALIVE-IDLE]: время последней РЕАЛЬНО записанной строки лога
 * (обновляется в trace_emit_impl). Пока строки идут (diag/td/KEYIN/
 * FILEOP...) — alive не печатается ВООБЩЕ; если лог замолчал на 10 с —
 * печатается одна строка alive и счётчик тишины перезапускается.
 * Прежнее поведение «alive каждые 2 с безусловно» давало до 1800 строк
 * нулевой информативности за час живой сессии (запрос пользователя:
 * логи компактнее). Ценность alive сохранена: при замолкании лога
 * (фриз/тишина в меню) строка доказывает, что процесс и SDL-таймер
 * живы, и показывает последнюю строку перед тишиной. */
static volatile uint64_t g_last_emit_ms = 0;
static int  g_banner_done = 0;
static SDL_TimerID g_beat_id = 0;        /* v34.95: removed at shutdown */
static volatile int g_beat_stop = 0;     /* v34.95: shutdown flag for the cb */

/* Log path: sdmc:/switch/j2me/log.txt on the Switch (the settings dir —
 * proven writable: every MINUS toggle saves settings.ini there); the
 * NOJME_SWITCH_UI_DIR verification dir on host builds; stderr only when
 * neither applies. */
static const char* trace_log_path(char* buf, size_t cap) {
    /* v34.94: verification dir wins on EVERY platform (host verify builds
     * compile with -D__SWITCH__ but have no sdmc:). */
    const char* env = getenv("NOJME_SWITCH_UI_DIR");
    if (env && env[0]) {
        snprintf(buf, cap, "%s/log.txt", env);
        return buf;
    }
#ifdef __SWITCH__
    (void)buf; (void)cap;
    return "sdmc:/switch/j2me/log.txt";
#else
    (void)buf; (void)cap;
    return NULL;
#endif
}

static void trace_log_write(const char* s, size_t n) {
    if (g_log_fd == -1 || g_bytes >= TRACE_LOG_CAP) return;

    if (g_log_fd == -2) {
        g_log_fd = -1; /* pessimistic default until the open succeeds */
        char pbuf[512];
        const char* p = trace_log_path(pbuf, sizeof(pbuf));
        if (p) {
#ifdef __SWITCH__
            mkdir("sdmc:/switch", 0777);
            mkdir("sdmc:/switch/j2me", 0777);
#endif
            /* First open per process TRUNCATES (one run = one log);
             * O_APPEND afterwards makes every write atomic-at-end, so
             * heartbeat lines from the SDL timer thread cannot clobber
             * lines written by the main thread. */
            int fd = open(p, O_WRONLY | O_APPEND | O_CREAT | O_TRUNC, 0666);
            if (fd >= 0) g_log_fd = fd;
        }
    }
    if (g_log_fd >= 0) {
        ssize_t w = write(g_log_fd, s, n);
        if (w > 0) {
            g_bytes += (uint64_t)w;
        } else if (w < 0) {
            close(g_log_fd);        /* broken card / removed dir: stop trying */
            g_log_fd = -1;
        }
    }
}

static void trace_emit_impl(const char* text, int force) {
    /* v35.08 SILENT-BY-DEFAULT (user request): with the logging toggle OFF
     * NOTHING is written anywhere - not log.txt, not stderr. Previously the
     * forced channel (diag lines every 5 s, [GC-HINT], STUCK/T-STALL dumps)
     * bypassed the gate by design and kept growing log.txt on a device where
     * the user had switched logging off. The on-screen status line (g_line)
     * is NOT affected - it is updated by the callers before this point, so
     * the frontend footer diag line keeps working with logging disabled.
     * (void)force: since v35.08 there is no write path that bypasses the
     * gate; the parameter survives for call-site compatibility. */
    if (!logging_on()) {
        (void)force;
        return;
    }
    char out[TRACE_LINE_CAP + 64];
    uint64_t t = sdl_switch_ui_ticks_ms();
    /* v36.07 SESSION-RELATIVE TICKS ("проблема с логом"): the tick source is
     * SDL_GetPerformanceCounter — on HOS that is the console/process tick,
     * and the hbloader PROCESS persists across "exit to hbmenu -> relaunch"
     * (an NRO return does not kill the process). The 2nd launch therefore
     * opened log.txt with ticks already at the first run's end (plus hbmenu
     * time): a session that really lived 60 s reported thousands of seconds
     * of "runtime". Every nojme launch TRUNCATES log.txt (g_log_fd == -2 is
     * fresh .bss per NRO load), so anchoring the base at the FIRST emit of
     * this launch makes every printed tick = wall time of THIS nojme run.
     * Deltas inside diag/STUCK use the raw counter — unaffected. */
    static uint64_t s_trace_t0 = 0;
    if (s_trace_t0 == 0) s_trace_t0 = (t > 0) ? t : 1;
    uint64_t rel = (t > s_trace_t0) ? (t - s_trace_t0) : 0;
    int n = snprintf(out, sizeof(out), "%llu %s\n",
                     (unsigned long long)rel, text);
    if (n > 0) {
        if ((size_t)n >= sizeof(out)) n = (int)sizeof(out) - 1;
        trace_log_write(out, (size_t)n);
        fprintf(stderr, "[TRACE] %s\n", text);
        fflush(stderr);
        /* v36.60 [ALIVE-IDLE]: каждая записанная строка продлевает
         * «тишину» для сторожа alive (см. beat-cb). */
        g_last_emit_ms = t;
    }
    (void)force;
}

/* Forced line: intended for diag / settings events / STUCK. v35.08: the
 * emit gate applies to EVERY line, so with logging off this still refreshes
 * the on-screen status line (g_line) but writes nothing anywhere. */
void sw_trace_force(const char* fmt, ...) {
    char text[TRACE_LINE_CAP];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    snprintf(g_line, sizeof(g_line), "%s", text);
    trace_emit_impl(text, 1);
}

void sw_trace(const char* fmt, ...) {
    char text[TRACE_LINE_CAP];

    /* One-time banner on first use (NOT a constructor: this must run after
     * SDL/env setup, and it proves WHICH build is actually running — the
     * whole point of CORE_BUILD_ID). v35.08: written ONLY when logging is
     * enabled (the emit gate decides); with the toggle off nothing is
     * written anywhere. */
    if (!g_banner_done) {
        g_banner_done = 1;
        if (logging_on()) {
            extern const char* j2me_core_build_id(void);
            char b[128];
            snprintf(b, sizeof(b), "nojme %s trace start", j2me_core_build_id());
            trace_emit_impl(b, 1);
        }
    }

    /* v34.92 logging gate (recreated): with the toggle OFF only the banner
     * and forced lines (settings events, STUCK watchdog) reach the log.
     * v35.08: the gate moved INTO trace_emit_impl - with logging off
     * NOTHING reaches the log or stderr (the on-screen status line still
     * updates, it is UI, not a log). */
    if (!logging_on()) return;

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);

    /* on-screen line: single writer (main thread); benign torn reads from
     * the heartbeat timer at worst garble one log line. */
    snprintf(g_line, sizeof(g_line), "%s", text);
    trace_emit_impl(text, 0);
}

const char* sw_trace_line(void) { return g_line; }

void sw_trace_flush(void) {
    uint32_t* cv = sdl_switch_ui_canvas();
    if (!cv || !g_line[0]) return;

    /* dark screen + centered stage line, presented immediately: if the code
     * right after this call hangs, the user SEES which stage did it. */
    const uint32_t bg = 0xFF0A0C10u;
    const uint32_t fg = 0xFFE8ECF4u;
    const int n = SWITCH_UI_WIDTH * SWITCH_UI_HEIGHT;
    for (int i = 0; i < n; i++) cv[i] = bg;
    switch_font_draw_text(cv, SWITCH_UI_WIDTH, SWITCH_UI_HEIGHT,
                          32, SWITCH_UI_HEIGHT / 2 - 16,
                          g_line, fg, 0, 2);
    sdl_switch_ui_present();
}

static Uint32 trace_beat_cb(Uint32 interval, void* param) {
    (void)param;
    /* v34.95: app is shutting down — return 0 (do NOT reschedule) and
     * never touch stderr/file: SDL_Quit joins this callback's thread, and
     * a callback racing the teardown (fprintf/stdio locks, open/write)
     * is exactly the "hang after 'menu exit — shutting down'" signature
     * seen on the host verify build with logging enabled. */
    if (g_beat_stop) return 0;
    char text[TRACE_LINE_CAP];

    /* v34.94 STUCK watchdog: when a game session is on (a stage marker
     * exists) and the main loop has not completed an iteration for >5 s,
     * say WHERE it is stuck. This line bypasses the logging gate — a
     * frozen user should not also lose the diagnosis because the toggle
     * was off.
     * v34.95: STUCK also carries sp= (GC safepoint request pending — a
     * stuck collector freezes EVERY VM thread at its next poll) and sb=
     * (queued audio bytes — whether the audio thread still produces). */
    const char* stage = (const char*)(uintptr_t)g_sd_loop_stage;
    uint64_t now = sdl_switch_ui_ticks_ms();
    uint64_t last = g_sd_loop_ms;
    int stuck = (stage && last && now > last + 5000);
    if (stuck) {
        int sp = 0;
        {
            extern volatile int g_gc_safepoint_request __attribute__((weak));
            if (&g_gc_safepoint_request) sp = g_gc_safepoint_request;
        }
        extern size_t sdl_audio_queued_bytes(void);
        uint32_t sb = (uint32_t)sdl_audio_queued_bytes();
        /* v34.98 forensics: pause state, settled-frame age, M3G target size
         * and the period's bind counters — one line answers "frontend parked
         * (pause?), game not completing frames (seqage), or wedged inside a
         * slow bind (bd/m3g)". All weak-linked: the input test build has no
         * JVM/display layer. */
        int pause = -1;
        {
            extern int jvm_frontend_pause_snapshot(void) __attribute__((weak));
            if (jvm_frontend_pause_snapshot) pause = jvm_frontend_pause_snapshot();
        }
        /* [STUCK-PAUSE] v36.45 (yetifix2): при открытом меню паузы главный
         * цикл ЛЕГИТИМНО блокирован (sdl_switch_pause_menu — модальный
         * цикл с собственным насосом событий), и СТОРОЖ ПОДРЯД пишет
         * пугающие STUCK-строки в каждый полевой лог, когда пользователь
         * просто стоит в паузе >5 с (лог v36.44: STUCK idle=5335 при
         * живом меню). Детекция зависания меню СОХРАНЕНА (строка
         * продолжает печататься), но этап помечается [pause-menu], чтобы
         * полевой лог читался однозначно. */
        char stagebuf[48];
        if (pause == 3) /* bits: 1 = pause requested, 2 = latch held */
            snprintf(stagebuf, sizeof(stagebuf), "%.30s[pause-menu]", stage);
        else
            snprintf(stagebuf, sizeof(stagebuf), "%s", stage);
        uint32_t seq = 0; uint64_t seqage = 0;
        {
            extern uint32_t midp_present_stable_seq(void) __attribute__((weak));
            extern uint64_t midp_present_stable_age_ms(void) __attribute__((weak));
            if (midp_present_stable_seq) seq = midp_present_stable_seq();
            if (midp_present_stable_age_ms) seqage = midp_present_stable_age_ms();
        }
        int mw = 0, mh = 0;
        {
            extern void m3g_target_size(int*, int*) __attribute__((weak));
            if (m3g_target_size) m3g_target_size(&mw, &mh);
        }
        uint32_t binds = 0, bdmax = 0;
        {
            extern volatile uint32_t g_sd_m3g_binds, g_sd_m3g_bind_ms_max;
            binds = g_sd_m3g_binds; bdmax = g_sd_m3g_bind_ms_max;
        }
        snprintf(text, sizeof(text),
                 "STUCK: stage=%.40s idle=%llu ms sp=%d sb=%u pause=%d seq=%u seqage=%llu ms m3g=%dx%d bn=%u bd=%u last=%.24s",
                 stagebuf, (unsigned long long)(now - last), sp, sb,
                 pause, seq, (unsigned long long)seqage, mw, mh,
                 binds, bdmax, g_line);
        trace_emit_impl(text, 1);
    }

    /* v35.01 [T-STALL]/[THREAD-EXIT]: the v35.00 field trace caught the NEW
     * freeze form — the frontend loop stayed ALIVE (loop=222, no STUCK,
     * menu exit worked) while the game world froze, and with the logging
     * toggle off (the default) that state had ZERO log visibility (td
     * dumps are gated by the toggle; STUCK only watches the frontend
     * loop). This channel works regardless of the toggle: every beat
     * checks VM-thread liveness and, when a RUN-able thread stops
     * dispatching for >5 s OR the alive-runner count drops below its
     * session high, emits a forced dump of all threads (deep=0 — legal
     * from the timer thread; the frontend is NOT provably blocked here). */
    int stall_alarm = 0;
    if (stage && g_sd_jvm && !stuck) {
        extern void jvm_thread_liveness(void*, int*, long long*, int*, int*)
            __attribute__((weak));
        if (jvm_thread_liveness) {
            int live = 0, waiting = 0, worst_tid = -1;
            long long worst = -1;
            jvm_thread_liveness((void*)g_sd_jvm, &live, &worst, &worst_tid, &waiting);
            if (live > s_live_hwm) s_live_hwm = live;
            /* v35.07: while the frontend pause latch is held (MINUS menu /
             * PLUS per-game screen) VM threads are PARKED by design —
             * their last-native age grows into the T-STALL/SLOWLOGIC band
             * and the settled sequence freezes into the [NO-FRAME] band.
             * The v35.06 field trace burned a [SLOWLOGIC] line + a ~3.7 s
             * thread dump (mx=3717, stl=3722, td t2 why=fe-pause) on the
             * user simply opening the pause menu at the end of a session.
             * Suppress the two liveness channels while the latch is held:
             * the menu overlay is visible on screen, a menu HANG is
             * STUCK's job (it keeps firing and carries pause= in its
             * line) and the 20 s force-lift backstop still applies. */
            int pause_latch = 0;
            {
                extern int jvm_frontend_pause_snapshot(void)
                    __attribute__((weak));
                if (jvm_frontend_pause_snapshot)
                    pause_latch = jvm_frontend_pause_snapshot();
            }
            const char* why = NULL;
            if (s_live_hwm > 0 && live < s_live_hwm)
                why = "THREAD-EXIT";
            else if (pause_latch & 2) {
                s_slow_beats = 0;   /* parked: not a logic-stall sample */
                s_nf_beats = 0;
            }
            else if (worst > 5000)
                why = "T-STALL";
            /* v35.03 SLOWLOGIC: логика ползёт на ~1 Гц — worst_age стоит в
             * полосе 0.4..5 с — это НЕВИДИМО для T-STALL (порог 5 с) и не
             * даёт STUCK (фронтенд жив). Полевой отчёт v35.02: круг на
             * полной скорости, затем игра падает до ~1 к/с; между ударами
             * сторожа поток "прогрессирует", поэтому тишина. Устойчивое
             * (3 удара подряд) нахождение в полосе = принудительный дамп. */
            else if (worst > 400) {
                s_slow_beats++;
                if (s_slow_beats >= 3) why = "SLOWLOGIC";
            } else {
                s_slow_beats = 0; /* healthy beat — reset the sustained band */
            }
            if (why) {
                uint64_t now2 = sdl_switch_ui_ticks_ms();
                if (now2 - s_last_stall_log >= 5000) {
                    s_last_stall_log = now2;
                    /* v35.03: контекст в самой строке — GC-пауза (gms) и
                     * возраст бинда (ma): отличают GC-шторм (gms велик) от
                     * ступора потока игры (ma велик, gms мал) с одного
                     * взгляда на лог. */
                    uint32_t gms = 0, ma2 = 0;
                    {
                        extern volatile uint32_t g_gc_last_ms __attribute__((weak));
                        if (&g_gc_last_ms) gms = g_gc_last_ms;
                    }
                    {
                        extern uint32_t m3g_bind_age_ms(void) __attribute__((weak));
                        if (m3g_bind_age_ms) ma2 = m3g_bind_age_ms();
                    }
                    char text2[TRACE_LINE_CAP];
                    snprintf(text2, sizeof(text2),
                             "[%s] live=%d (hwm %d) waiting=%d worst_age=%lld ms (t%d) gms=%u ma=%u — frontend loop alive, dumping VM threads",
                             why, live, s_live_hwm, waiting,
                             (long long)worst, worst_tid, gms, ma2);
                    trace_emit_impl(text2, 1);
                    stall_alarm = 1;
                }
            } else {
                s_last_stall_log = 0; /* re-arm the throttle after recovery */
            }

            /* v35.04 [NO-FRAME]: VM thread(s) alive and PROGRESSING (the
             * why= classes above did not fire) but the settled-frame
             * sequence has not advanced for 10 beats. The game thread is
             * spinning in its loop without ever completing a paint — the
             * Bobby Carrot 4 "hangs on load" signature (tn=1, ta=12-20,
             * fl=0, heap static). The dump below prints last_native per
             * thread; the line carries the queue/paint/displayable state
             * needed to classify the wait (key input? paint handshake?
             * which screen?). A normally-idling game parks its loop in
             * sleep => it lands in `waiting`, live drops to 0 and this
             * never fires; a static screen with a SLEEPING game thread is
             * NOT reported (by design — only active spins are). */
            if (!why && live > 0 && !(pause_latch & 2)) {   /* v35.07 latch guard */
                extern uint32_t midp_present_stable_seq(void) __attribute__((weak));
                if (midp_present_stable_seq) {
                    uint32_t seq = midp_present_stable_seq();
                    if (seq != s_nf_seq) {
                        s_nf_seq = seq;
                        s_nf_beats = 0;
                        s_nfe_state = 0;   /* v36.06: new window, new baseline */
                    } else {
                        s_nf_beats++;
                        /* v36.06 [NO-FRAME-ESC]: heap-motion tracking + the
                         * escalation itself (see the block comment above). */
                        {
                            extern HeapStats heap_get_stats(JVM*) __attribute__((weak));
                            if (heap_get_stats && g_sd_jvm) {
                                uint32_t kb = (uint32_t)(heap_get_stats((JVM*)g_sd_jvm).used_size >> 10);
                                if (s_nfe_state == 0) {
                                    s_nfe_state = 1;
                                    s_nfe_heap0 = kb;
                                } else if (s_nfe_state == 1 &&
                                           kb > s_nfe_heap0 && kb - s_nfe_heap0 > 64) {
                                    s_nfe_state = 2; /* real work — never escalate */
                                }
                            }
                        }
                        if (s_nf_beats >= NOJME_NF_ESC_BEATS &&
                            s_nfe_state != 2 && !g_midp_stuck_recover_req) {
                            g_midp_stuck_recover_req = 1;
                            s_nf_beats = 0;
                            s_nfe_state = 0;
                            uint32_t esc_kb = s_nfe_heap0;
                            char esctext[TRACE_LINE_CAP];
                            snprintf(esctext, sizeof(esctext),
                                     "[NO-FRAME-ESC] no settled frame for %d s with a live VM and a frozen heap (~%uK) — live-spin hang (worst_age=%lld): requesting graceful exit to menu",
                                     NOJME_NF_ESC_BEATS, esc_kb, (long long)worst);
                            trace_emit_impl(esctext, 1);
                            stall_alarm = 1;
                        }
                        if (s_nf_beats >= NOJME_NF_BEATS) {
                            uint64_t now3 = sdl_switch_ui_ticks_ms();
                            if (now3 - s_last_noframe_log >= 15000) {
                                s_last_noframe_log = now3;
                                int keyq = -1, ppend = -1, pactive = -1;
                                char cls[48]; cls[0] = '\0';
                                {
                                    extern void midp_noframe_state(int*, int*, int*, char*, int)
                                        __attribute__((weak));
                                    if (midp_noframe_state)
                                        midp_noframe_state(&keyq, &ppend, &pactive,
                                                           cls, (int)sizeof(cls));
                                }
                                uint32_t heapkb = 0;
                                {
                                    extern HeapStats heap_get_stats(JVM*) __attribute__((weak));
                                    if (heap_get_stats && g_sd_jvm)
                                        heapkb = (uint32_t)(heap_get_stats((JVM*)g_sd_jvm).used_size >> 10);
                                }
                                char nftext[TRACE_LINE_CAP];
                                snprintf(nftext, sizeof(nftext),
                                         "[NO-FRAME] no settled frame for %d s; live=%d worst_age=%lld (t%d) heap=%uK paint_pending=%d keyq=%d pump_active=%d disp=%.40s — dumping VM threads",
                                         s_nf_beats, live, (long long)worst, worst_tid,
                                         heapkb, ppend, keyq, pactive, cls);
                                trace_emit_impl(nftext, 1);
                                stall_alarm = 1;
                            }
                        }
                    }
                }
            }

            /* v36.05 [MIDLET-STUCK]: dead-air self-recovery (see the block
             * comment at NOJME_STUCK_BEATS). Runs INDEPENDENTLY of the
             * logging toggle and of the why= ladder above (the hang has
             * live=0, so THREAD-EXIT owns the 5 s throttle and T-STALL
             * never fires). Conditions, every beat:
             *   - no pause overlay (guard above) and !stuck (guard outside);
             *   - settled-frame seq frozen for the whole window;
             *   - VM dead air: (live==0 && waiting==0) — nothing runnable,
             *     nothing sleeping, nothing can ever wake — OR live>0 with
             *     every runner spinning >5 s without a native dispatch;
             *   - heap used grew <64 KB since the window opened (a real
             *     pure-Java worker allocates; a corpse does not).
             * 15 beats (~15 s) => request the graceful midlet exit. The
             * SDL frame loop consumes the request on the thread that owns
             * the session (never from this timer thread). */
            if (!(pause_latch & 2)) {   /* v35.07 latch guard: overlays park the VM by design */
            {
                uint32_t seq_now = 0;
                {
                    extern uint32_t midp_present_stable_seq(void) __attribute__((weak));
                    if (midp_present_stable_seq) seq_now = midp_present_stable_seq();
                }
                int dead_air = (live == 0 && waiting == 0) ||
                               (live > 0 && worst > 5000);
                int heap_moved = 0;
                if (dead_air) {
                    extern HeapStats heap_get_stats(JVM*) __attribute__((weak));
                    if (heap_get_stats && g_sd_jvm) {
                        uint32_t kb = (uint32_t)(heap_get_stats((JVM*)g_sd_jvm).used_size >> 10);
                        if (!s_stk_valid) {
                            s_stk_valid = 1;
                            s_stk_heap0 = kb;
                        } else if (kb > s_stk_heap0 && kb - s_stk_heap0 > 64) {
                            heap_moved = 1; /* real work — abort the window */
                        }
                    }
                }
                if (!dead_air || heap_moved || seq_now != s_stk_seq) {
                    s_stk_beats = 0;
                    s_stk_valid = 0;
                } else {
                    if (s_stk_beats == 0) s_stk_seq = seq_now;
                    s_stk_beats++;
                }
                if (s_stk_beats >= NOJME_STUCK_BEATS && !g_midp_stuck_recover_req) {
                    g_midp_stuck_recover_req = 1;
                    s_stk_beats = 0;
                    s_stk_valid = 0;
                    int fired_beats = NOJME_STUCK_BEATS;
                    char ktext[TRACE_LINE_CAP];
                    snprintf(ktext, sizeof(ktext),
                             "[MIDLET-STUCK] dead air %d s (live=%d waiting=%d worst=%lld seq=%u) — requesting graceful exit to menu (uncaught thread death? see [VM-THREAD-DEATH]/[EX-THROW] above)",
                             fired_beats, live, waiting,
                             (long long)worst, seq_now);
                    trace_emit_impl(ktext, 1);
                    stall_alarm = 1;
                }
            }
            }
        }
    }

    /* v35.03 [GC-SLOW]: длинная GC-пауза — главный подозреваемый отчёта
     * «внезапный 1 fps после круга» (Gameloft-игры зовут System.gc() на
     * границах кругов/уровней; первая в сессии сборка при gc=0 обязана
     * пройтись по ВСЕМ объектам, накопленным с момента старта — на Switch
     * это сотни мс, а при повторных вызовах игры — кадр в секунду).
     * Принудительный канал: виден при выключенном тумблере лога.
     * Recency по CLOCK_MONOTONIC — тот же источник, что g_gc_last_end_ms. */
    if (g_sd_jvm && !stuck) {
        extern volatile long long g_gc_last_end_ms __attribute__((weak));
        extern volatile uint32_t g_gc_last_ms __attribute__((weak));
        if (&g_gc_last_end_ms && &g_gc_last_ms) {
            struct timespec gcts;
            clock_gettime(CLOCK_MONOTONIC, &gcts);
            long long now_ms = (long long)gcts.tv_sec * 1000LL + gcts.tv_nsec / 1000000LL;
            long long age = now_ms - g_gc_last_end_ms;
            if (g_gc_last_ms > 300 && age >= 0 && age < 2000 &&
                now_ms - s_last_gcslow_log >= 5000) {
                s_last_gcslow_log = now_ms;
                char gtext[192];
                snprintf(gtext, sizeof(gtext),
                         "[GC-SLOW] last GC pause %u ms (ended %lld ms ago) — watch gms=/gxm= in diag; repeat pauses = GC storm",
                         g_gc_last_ms, age);
                trace_emit_impl(gtext, 1);
                stall_alarm = 1; /* + дамп потоков: кто паркируется в safepoint */
            }
        }
    }

    /* v34.95: per-second THREAD DUMP while a game session is on (the
     * user's "раз в секунду показывать информацию о работающих и спящих
     * потоках"). Gated by the logging toggle; v35.08: strictly gated -
     * with logging OFF nothing is written anywhere, so building the dump
     * (which walks every VM thread) is pure waste; STUCK detection above
     * still runs (cheap) and keeps s_slow_beats bookkeeping alive.
     * Weak-linked: the white-box input test builds this file without the
     * JVM, so the symbol may be absent. */
    if (stage && g_sd_jvm && logging_on()) {
        extern void jvm_threads_dump_snprint(void* jvm, char* buf, size_t cap, int deep)
            __attribute__((weak));
        if (jvm_threads_dump_snprint) {
            /* v36.06: while a NO-FRAME window is active the dump runs DEEP
             * (Java call sites per thread, e.g. "g.b@18") every 10th beat,
             * not only on STUCK. The v36.05 hang trace had t2 spinning in
             * "java/io/InputStream.read" with age=0 and the shallow dump
             * could not name the game method inside which the stream was
             * being read — the deep dump can. */
            int nf_deep = stuck || (s_nf_beats > 0 && (s_nf_beats % NOJME_NF_BEATS) == 0);
            char dbuf[1024];   /* v34.96: 640 -> 1024 — wider nat names + td m3g line */
            dbuf[0] = '\0';
            jvm_threads_dump_snprint((void*)g_sd_jvm, dbuf, sizeof(dbuf), nf_deep);
            char* cur = dbuf;
            while (cur && *cur) {
                char* nl = strchr(cur, '\n');
                if (nl) *nl = '\0';
                if (cur[0]) trace_emit_impl(cur, stuck || stall_alarm);
                cur = nl ? nl + 1 : NULL;
            }
        }
    }

    if (stuck) return interval;

    if (!logging_on()) return interval; /* v34.92 gate */
    /* v36.60 [ALIVE-IDLE]: только после 10 с полной тишины лога — и не
     * чаще раза в 10 с (запись alive обновляет g_last_emit_ms в
     * trace_emit_impl). В живой сессии строк нет вовсе; в замолкшей —
     * раз в 10 с, как сторож «процесс жив, лог просто пуст». */
    {
        uint64_t nowb = sdl_switch_ui_ticks_ms();
        if (g_last_emit_ms && nowb > g_last_emit_ms &&
            nowb - g_last_emit_ms >= 10000) {
            snprintf(text, sizeof(text), "alive: %.100s", g_line);
            trace_emit_impl(text, 0);
        }
    }
    return interval; /* keep firing */
}

void sw_trace_heartbeat(void) {
    if (g_beat_on) return;
    g_beat_on = 1;
    /* v34.95: 1 s beat — the user asked for a per-second thread dump, and
     * the STUCK watchdog now detects within 1 s instead of 2 s. */
    g_beat_id = SDL_AddTimer(1000, trace_beat_cb, NULL);
}

/* v34.95: deterministic shutdown — stop the heartbeat BEFORE the app tears
 * down SDL. Removes the timer (cancels pending fires), then waits for an
 * in-flight callback to observe g_beat_stop and leave. */
void sw_trace_shutdown(void) {
    g_beat_stop = 1;
    if (g_beat_id) {
        SDL_RemoveTimer(g_beat_id);
        g_beat_id = 0;
    }
    /* SDL_RemoveTimer synchronizes the timer thread internally
     * (cancels + joins), so after it returns no callback is running or
     * pending. The flag guards the tiny window before removal. */
}

/* [WILDGUARD] v36.50 (yetifix7): отчёт детектора мусорных указателей —
 * см. src/include/wildguard.h (там же порог и rationale). Три канала:
 * sw_trace_force (log.txt при включённом «Лог»), stderr (хост) и STDOUT —
 * Ryu/Ryujinx печатают stdout homebrew в СВОЁМ логе рядом со строками
 * InvalidAccess, так что [WILDGUARD]-строка атрибутирует источник даже
 * при выключенном «Лог». Кап 24 отчёта: назвать семейство сайтов, не
 * заливая лог (вспышки и так короткие — 3-6 чтений). */
#include "wildguard.h"

void nojme_wg_report(const char* site, const void* val) {
    static int s_wg_reports = 0;
    if (s_wg_reports >= 24) return;
    s_wg_reports++;
    uint64_t v = (uint64_t)(uintptr_t)val;
    char ascii[9];
    for (int i = 0; i < 8; i++) ascii[i] = (char)((v >> (8 * i)) & 0xFF);
    ascii[8] = '\0';
    sw_trace_force("[WILDGUARD] %s ptr=%016llx ascii='%.8s'",
                   site ? site : "?", (unsigned long long)v, ascii);
    fprintf(stderr, "[WILDGUARD] %s ptr=%016llx ascii='%.8s'\n",
            site ? site : "?", (unsigned long long)v, ascii);
    fflush(stderr);
    /* v36.48 [BUILD-BANNER-STDOUT]: stdout — единственный канал, который
     * Ryu показывает в своём логе; строка встанет рядом с InvalidAccess. */
    printf("[WILDGUARD] %s ptr=%016llx ascii='%.8s'\n",
           site ? site : "?", (unsigned long long)v, ascii);
    fflush(stdout);
}

#endif /* __SWITCH__ || NOJME_SWITCH_TRACE */
