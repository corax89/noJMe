/*
 * switch_trace.h — v34.91 «freeze triage»: lightweight stage diagnostics for
 * the Switch frontend.
 *
 * WHY: the user reports that on a Switch EMULATOR the frontend menu works
 * (navigation, MINUS language toggle — which already writes settings.ini to
 * sdmc:, proving the card is writable) but pressing A on «Открыть игру» /
 * «Настройки» freezes the app. The same paths pass the full host E2E
 * (test_switch_input: MINUS,A,A -> jar selected), so the freeze is
 * emulator-specific and cannot be reproduced in the sandbox. stderr is not
 * visible on an emulator, so this module turns the next user run into a
 * precise diagnosis:
 *
 *   1. sw_trace(stage)      — every stage goes to stderr AND a persistent
 *                              log file AND a one-line on-screen status
 *                              (drawn by the menu above the footer).
 *   2. sw_trace_flush()     — PRESENTS the stage line BEFORE a potentially
 *                              blocking action (directory scan, JAR read,
 *                              VM init). If the action hangs, the last
 *                              visible line names the culprit.
 *   3. sw_trace_heartbeat() — SDL timer thread appends "alive: <line>" to
 *                              the log every 2 s: alive-lines flowing while
 *                              the screen is frozen == the main thread is
 *                              stuck inside the named stage; lines stopped
 *                              == the whole process died inside it.
 *
 * Log file: sdmc:/switch/j2me/log.txt on __SWITCH__ (same directory as
 * settings.ini — proven writable by the MINUS toggle), $NOJME_SWITCH_UI_DIR/
 * log.txt on host verification builds (NOJME_SWITCH_TRACE), stderr-only
 * otherwise. Truncated once per process, capped at 256 KB, auto-disabled on
 * first write error — diagnostics must never make things worse.
 *
 * Desktop builds (no __SWITCH__, no NOJME_SWITCH_TRACE) get macro no-ops so
 * shared sources (main.c, sdl_graphics.c) compile unchanged everywhere.
 */
#ifndef SWITCH_TRACE_H
#define SWITCH_TRACE_H

#include <stdint.h>

#if defined(__SWITCH__) || defined(NOJME_SWITCH_TRACE)

/* Stage marker: fmt + args (printf subset, max ~150 chars). Writes the line
 * to stderr + the log file (timestamped) and stores it as the on-screen
 * status line (sw_trace_line). Thread-safe best effort; never blocks the
 * caller for longer than one small append write. */
void sw_trace(const char* fmt, ...);

/* Current status line ("" before the first sw_trace). The menu renders it
 * every frame; sw_trace_flush() puts it on screen alone. */
const char* sw_trace_line(void);

/* Dark screen + the current stage line, PRESENTED immediately. Call right
 * BEFORE a potentially blocking action so a hang leaves the stage visible.
 * No-op when the UI canvas does not exist yet. */
void sw_trace_flush(void);

/* 2-second heartbeat logger (SDL timer thread). Idempotent. */
void sw_trace_heartbeat(void);

/* v34.95: stop the heartbeat deterministically BEFORE SDL teardown
 * (removes the timer; the callback also self-disarms). Idempotent. */
void sw_trace_shutdown(void);

/* v34.92 logging gate: forced line bypass (diag/settings/STUCK) and the
 * live toggle used by the settings screen (updates settings.ini there). */
void sw_trace_force(const char* fmt, ...);
void sw_logging_set(int on);
int  sw_logging_enabled(void);

/* ========================================================================
 * v34.94 DIAG — per-period counters + main-loop stage watchdog.
 *
 * The v34.93 diag line proved the freeze degrades progressively AFTER
 * "audio: open" (sb/stl/sr climbing, skip climbing, then a FULL stop of
 * the frontend loop). This recreation keeps the same counter names so the
 * user's next log reads identically, and adds a STAGE watchdog: the
 * heartbeat prints "STUCK <stage> <ms>" when the main loop stops making
 * iterations, pinpointing the frozen step even when nothing else logs.
 *
 * Counter semantics (per ~5 s diag period unless noted):
 *   loop  - main-loop iterations completed
 *   pr    - game frames PRESENTED (RenderPresent on the game rect)
 *   stl   - ms the main loop ran over its 16.6 ms frame budget (stall)
 *   vs    - vsync ticks delivered to the VM
 *   gc    - JVM collections completed (delta)
 *   heap  - JVM live bytes (heap_get_stats.used_size)
 *   top   - JVM arena bump offset in bytes (monotonic per session)
 *   skip  - presents whose settled frame version did NOT advance (stale
 *           content: the game produced no new frame)
 *   pump  - input pumps (switch_input_pump calls)
 *   sr    - ms spent inside SDL audio queue calls (sound-related stall;
 *           SDL_QueueAudio takes the device lock — a slow/blocked device
 *           callback shows up here)
 *   fl    - settled-frame version advances (game actually finished frames)
 *   sb    - SDL queued audio bytes at emit time (sound backlog; growth
 *           means production outpaces consumption)
 * ======================================================================== */

/* Counters (incremented from multiple threads; relaxed atomics via GCC
 * builtins — values are diagnostics, torn/racy reads are acceptable). */
extern volatile uint32_t g_sd_loop, g_sd_pr, g_sd_skip, g_sd_pump;
extern volatile uint32_t g_sd_stl_ms, g_sd_sr_ms, g_sd_fl;
extern volatile uint32_t g_sd_vs;

/* v35.01 present/iteration cost telemetry (written by sdl_graphics.c,
 * read+reset by the diag line):
 *   pv_ms     - ms spent inside SDL_RenderPresent this period
 *   pv_ms_max - worst SINGLE RenderPresent in the period (ms)
 *   pw_ms_max - worst SINGLE whole present (upload+blur+copy+present, ms)
 *   iter_ms_max - worst SINGLE main-loop iteration in the period (ms) —
 *           splits "uniformly slow" from "a few giant stalls" (stl alone
 *           cannot: 1299 ms of over-budget can be 78 x 17 ms OR 1 x 1300) */
extern volatile uint32_t g_sd_pv_ms, g_sd_pv_ms_max, g_sd_pw_ms_max;
extern volatile uint32_t g_sd_iter_ms_max;

/* v36.43 [PRESENT-GLASS] (written by sdl_graphics.c, read+reset by the diag
 * line) — закрывает СЛЕПОЙ СЛОЙ между «кадр собран» и «кадр показан»:
 *   utf - SDL_UpdateTexture refusals this period (texture upload fails =>
 *         the view stays on a STALE frame while fl/lvc stay healthy —
 *         the suspected layer of the v36.42 Yeti field freeze);
 *   rcf - SDL_RenderCopy refusals this period;
 *   ch  - composite heartbeats (1 per 5 s) whose on-screen hash CHANGED —
 *         the picture really reached the render target;
 *   chf - RenderReadPixels refusals (the probe itself is blind there).
 * v36.44 [SPLIT-PROBE] (Yeti field freeze: "the game view froze on
 * "5 second(s)" while the blur fields kept updating" — a single ch
 * cannot separate the two regions):
 *   chg - probes where at least one pixel INSIDE g_game_rect changed;
 *   chb - probes where something OUTSIDE it changed (blur fill, FPS
 *         strip, battery badge, menu overlays).
 * Field freeze decoding: fl>0 && chg=0 && chb>0 => the GAME TEXTURE
 * content is stale in the composite while the border lives — the
 * divergence sits between present_src and the game texture (upload /
 * Scale2x path / stale snapshot — next: [PRESENT-MODE] trace).
 * chg=0 && chb=0 (ch=0) => the whole composite is static — the game
 * itself stopped painting new content (or the settled snapshot froze). */
extern volatile uint32_t g_sd_utf, g_sd_rcf, g_sd_ch, g_sd_chf;
extern volatile uint32_t g_sd_chg, g_sd_chb;

/* v35.02 logic-heartbeat (written by display.c, read+reset by the diag line):
 *   pp  - repaint pumps COMPLETED (took the UI lock, ran the impl)
 *   psk - pumps SKIPPED (UI lock busy — paint in flight elsewhere)
 *   cs  - callSerially game-logic Runnables executed on the frontend thread.
 * The v35.01 field trace + user report ("race ~1 fps from start, main frozen
 * after lap 1, blur alive") decoded to: renders healthy (bn=30/s, settles
 * 30/s) while the WORLD logic — the callSerially Runnable drained on the
 * frontend thread — starved on the paint-flag race between the two threads.
 * cs= is the pulse of that logic: healthy ~30-60/s, crawl = collapsing,
 * freeze = 0 while bn keeps counting. */
extern volatile uint32_t g_sd_pp, g_sd_pump_skip, g_sd_csq;

/* v35.04 stage-cost gauges (written by sdl_graphics.c, read+reset by the
 * diag line): worst single execution in ms of the three frontend-loop
 * stages that host VM work —
 *   tj = timers       (jvm_process_timers + key aging, the scheduler tick)
 *   rj = repaints     (the v35.02 single-pump; paint() runs on the frontend)
 *   sj = callserially (the game-logic Runnable drain)
 * A slow iteration (mx large, stl climbing) with ONE of these climbing
 * names the cost center directly: tj -> scheduler/TimerTasks, rj -> the
 * game's paint() executed on the frontend thread, sj -> game logic. */
extern volatile uint32_t g_sd_tj_ms_max, g_sd_rj_ms_max, g_sd_sj_ms_max;

/* v35.06 direct-render delivery (written by sdl_graphics.c, read+reset by
 * the diag line) — answers "are frames dropped at the final render stage?":
 *   lvc  - presents where the LIVE framebuffer content changed (sampled
 *          64-pixel hash) = what the game actually drew per period.
 *   dset - divergence settles: presents where the live framebuffer was
 *          FRESHER than the settled snapshot outside any paint/M3G bind
 *          and was re-settled on the spot (phone-faithful direct-render
 *          delivery). 0 on pump-paced games; >0 with lvc>fl means the
 *          game draws outside paint() and v35.06 delivers it live.
 *   lvc == fl         -> nothing dropped, game-paced (the whole story).
 *   lvc >> fl, dset=0 -> pre-v35.06 build or gate held (mid-frame).
 *   fl counts BOTH pump settles and divergence settles, so after v35.06
 *   lvc > fl persistently means the game draws faster than 60 fps
 *   presents can deliver — the true ceiling, not a pipeline loss. */
extern volatile uint32_t g_sd_lvc, g_sd_dset;

/* Main-loop stage marker + last completed iteration timestamp (ms).
 * Set from the main loop only; read by the heartbeat timer thread. */
extern volatile const char* g_sd_loop_stage;
extern volatile uint64_t g_sd_loop_ms;

/* Stage marker macro: place at each main-loop step. */
#define SW_DIAG_STAGE(name) do { \
    g_sd_loop_stage = (name); \
} while (0)

/* Called once per main-loop iteration from sdl_run: updates the iteration
 * timestamp and emits the diag line every SW_DIAG_PERIOD_MS. */
void sw_diag_tick(void);

/* Called once per session end: resets the arena-top baseline so "top" reads
 * meaningful for the NEXT session (top deltas stay session-scoped). */
void sw_diag_session_reset(void);

/* v36.05 [MIDLET-STUCK] self-recovery request. Raised by the heartbeat
 * timer after 15 s of VM dead air with no settled-frame progress (the
 * "loader thread died uncaught, midlet thread spins forever" hang —
 * Doom RPG [Rus] 2nd launch, trace v36.04). Consumed by the SDL frame
 * loop ON THE FRONTEND THREAD: it performs the same graceful exit as the
 * pause-menu "exit to menu" (destroyApp + stop) so the user lands in the
 * game menu instead of a dead loading screen. Cleared by
 * sw_diag_session_reset() and by the consumer after acting on it. */
extern volatile int g_midp_stuck_recover_req;

#else /* desktop: compile away */

#define sw_trace(...)        do { } while (0)
#define sw_trace_line()      ("")
#define sw_trace_flush()     do { } while (0)
#define sw_trace_heartbeat() do { } while (0)
#define sw_trace_shutdown()  do { } while (0)
#define sw_trace_force(...)  do { } while (0)
#define sw_logging_set(on)   do { } while (0)
#define sw_logging_enabled() (0)
#define SW_DIAG_STAGE(name)  do { } while (0)
#define sw_diag_tick()       do { } while (0)
#define sw_diag_session_reset() do { } while (0)

#endif

#endif /* SWITCH_TRACE_H */
