/*
 * Headless SDL Backend for Testing
 * No display, no audio - just executes bytecode
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 199309L
#endif

#define LIBRETRO  /* Use stubs */
#include "sdl_backend.h"
#include "midp.h"

/* v34.72: finished-MIDlet probe (native.c) — guards the running-revive
 * hack below against reviving a deliberately finished VM. */
extern bool midlet_is_destroyed(void);

/* Global context */
static SdlContext g_headless_ctx = {0};

SdlContext* sdl_get_global_context(void) { return &g_headless_ctx; }

void sdl_set_global_context(SdlContext* ctx) {
    if (ctx) g_headless_ctx = *ctx;
    else memset(&g_headless_ctx, 0, sizeof(SdlContext));
}

int sdl_init(JVM* jvm, int width, int height, int scale, bool headless) {
    (void)scale; (void)headless;
    
    if (width <= 0) width = 240;
    if (height <= 0) height = 320;
    
    g_headless_ctx.jvm = jvm;
    g_headless_ctx.width = width;
    g_headless_ctx.height = height;
    g_headless_ctx.scale = 1;
    g_headless_ctx.target_fps = 30;
    g_headless_ctx.running = true;
    g_headless_ctx.headless = true;
    
    /* Allocate framebuffer */
    g_headless_ctx.framebuffer = (uint32_t*)calloc(width * height, sizeof(uint32_t));
    if (!g_headless_ctx.framebuffer) {
        fprintf(stderr, "[Headless] Failed to allocate framebuffer\n");
        return -1;
    }
    
    fprintf(stderr, "[Headless] Initialized %dx%d\n", width, height);
    return 0;
}

void sdl_destroy(SdlContext* ctx) {
    if (ctx && ctx->framebuffer) {
        free(ctx->framebuffer);
        ctx->framebuffer = NULL;
    }
}

void sdl_run(SdlContext* ctx) {
    if (!ctx) return;
    
    /* In headless mode, process timers and repaints (no window rendering) */
    fprintf(stderr, "[Headless] Running headless mode (waiting for MIDlet)...\n");
    
    /* Force running flags - after startApp() returns, the JVM should still be 
     * considered "running" to process timers, repaints, etc.
     * v34.72: ...UNLESS the MIDlet deliberately finished inside
     * startApp/constructor (notifyDestroyed/System.exit) — reviving it
     * then left a dead VM pumping a frozen screen forever. A finished
     * MIDlet falls through to the explicit black "MIDlet finished"
     * screen instead. */
    ctx->running = true;
    if (ctx->jvm && !ctx->jvm->running &&
        !ctx->jvm->exiting && !midlet_is_destroyed()) {
        ctx->jvm->running = true;
    }
    
    int frames = 0;
    int max_frames = 60000; /* Safety limit */
    
    extern int midp_process_repaints(JVM* jvm);
    extern void jvm_process_timers(JVM* jvm);
    extern void midp_process_call_serially_queue(JVM* jvm);
    extern bool midp_check_alert_timeout(JVM* jvm);
    extern void midp_clear_pending_repaint(void);
    extern void midp_repaint_current(JVM* jvm);
    extern bool midp_handle_soft_button(JVM* jvm, int button_index);
    
    struct timespec start_ts, current_ts;
    clock_gettime(CLOCK_MONOTONIC, &start_ts);
    uint64_t last_auto_redraw = (uint64_t)start_ts.tv_sec * 1000 + start_ts.tv_nsec / 1000000;
    uint64_t start_time_ms = last_auto_redraw;
    
    /* HEADLESS KEY INJECTION: Simulate key presses to advance past loading/menu screens.
     * Many J2ME games wait for user input (FIRE/SELECT key) before starting gameplay.
     * In headless mode, we inject key presses after a delay to allow the game to load.
     * MIDP key codes: FIRE=5, SELECT=-5, LEFT=-2, RIGHT=-4, UP=-1, DOWN=-3
     * Game action codes are negative: FIRE=-5, LEFT=-2, RIGHT=-4, UP=-1, DOWN=-6 */
    extern void midp_call_keyPressed(JVM* jvm, int keycode);
    extern void midp_call_keyReleased(JVM* jvm, int keycode);
    extern int headless_check_text_activity(void);
    
    /* Key injection schedule: inject keys aggressively in the first phase,
     * then switch to soft button presses and idle detection. */
    int keys_to_try[] = {5, -5, -6, 4, 8, 35, 42, 48, 49, 50, 51, 52, 53, 55, 56, 57};
    int num_keys = sizeof(keys_to_try) / sizeof(keys_to_try[0]);
    int key_fire_frame = 100;  /* Start injecting after ~1 second */
    int key_interval = 50;     /* Inject every 0.5 seconds */
    int last_key_frame = key_fire_frame + (num_keys - 1) * key_interval; /* Last key frame */

    /* v22: NOJME_HEADLESS_KEYSEQ="5,-5,-1,-2" + NOJME_HEADLESS_KEYSEQ_EVERY=<frames>
     * Cyclically injects this key sequence (press+release) forever - lets the
     * harness navigate multi-step menus (down,down,FIRE) to reach gameplay. */
    int seq_keys[32];
    int seq_len = 0;
    int seq_every = 60;
    {
        const char* seq_env = getenv("NOJME_HEADLESS_KEYSEQ");
        if (seq_env && seq_env[0]) {
            const char* every_env = getenv("NOJME_HEADLESS_KEYSEQ_EVERY");
            if (every_env) {
                int v = atoi(every_env);
                if (v > 0) seq_every = v;
            }
            const char* p = seq_env;
            while (*p && seq_len < 32) {
                char* end = NULL;
                long v = strtol(p, &end, 10);
                if (end == p) break;
                seq_keys[seq_len++] = (int)v;
                p = end;
                while (*p == ',' || *p == ' ' || *p == ';') p++;
            }
            fprintf(stderr, "[Headless] KEYSEQ active: %d keys, every %d frames\n",
                    seq_len, seq_every);
        }
    }

    /* v34.34: NOJME_HEADLESS_POINTERSCRIPT="f:x,y:p;f:x,y:r;f:x,y:d;..."
     * — one-shot pointer event injection at the given frame numbers.
     * Actions: p=pressed, r=released, d=dragged. For stylus-only midlets
     * (Block 3D 2) that cannot be driven with keys at all. */
#define PTRSCRIPT_MAX 64
    int ptr_frame[PTRSCRIPT_MAX];
    int ptr_x[PTRSCRIPT_MAX];
    int ptr_y[PTRSCRIPT_MAX];
    char ptr_act[PTRSCRIPT_MAX];
    int ptr_len = 0;
    {
        const char* penv = getenv("NOJME_HEADLESS_POINTERSCRIPT");
        if (penv && penv[0]) {
            const char* p = penv;
            while (*p && ptr_len < PTRSCRIPT_MAX) {
                char* end = NULL;
                long f = strtol(p, &end, 10);
                if (end == p) break;
                p = end;
                if (*p != ':') break;
                p++;
                long x = strtol(p, &end, 10);
                if (end == p) break;
                p = end;
                if (*p != ',') break;
                p++;
                long y = strtol(p, &end, 10);
                if (end == p) break;
                p = end;
                if (*p != ':') break;
                p++;
                char act = *p++;
                if (*p == ';' || *p == ',') p++;
                ptr_frame[ptr_len] = (int)f;
                ptr_x[ptr_len] = (int)x;
                ptr_y[ptr_len] = (int)y;
                ptr_act[ptr_len] = act;
                ptr_len++;
            }
            fprintf(stderr, "[Headless] POINTERSCRIPT active: %d events\n", ptr_len);
        }
    }
    extern void midp_call_pointerPressed(JVM* jvm, int x, int y);
    extern void midp_call_pointerReleased(JVM* jvm, int x, int y);
    extern void midp_call_pointerDragged(JVM* jvm, int x, int y);

    /* v34.33: key script (one-shot scheduled injections) */
#define KEYSCRIPT_MAX 64
    int script_frame[KEYSCRIPT_MAX];
    int script_key[KEYSCRIPT_MAX];
    int script_len = 0;
    {
        const char* scr_env = getenv("NOJME_HEADLESS_KEYSCRIPT");
        if (scr_env && scr_env[0]) {
            const char* p = scr_env;
            while (*p && script_len < KEYSCRIPT_MAX) {
                char* end = NULL;
                long f = strtol(p, &end, 10);
                if (end == p) break;
                p = end;
                if (*p != ':') break;
                p++;
                long k = strtol(p, &end, 10);
                if (end == p) break;
                p = end;
                script_frame[script_len] = (int)f;
                script_key[script_len] = (int)k;
                script_len++;
                while (*p == ';' || *p == ',' || *p == ' ') p++;
            }
            fprintf(stderr, "[Headless] KEYSCRIPT active: %d events\n", script_len);
        }
    }
    
    /* Soft button injection: after regular keys, try soft buttons (left=0, right=1)
     * This handles Form/List displayables with commands (like Exit/OK). */
    int soft_button_frames[] = {
        last_key_frame + key_interval,     /* Left soft button */
        last_key_frame + key_interval * 2, /* Right soft button */
        last_key_frame + key_interval * 3, /* Left soft button again */
        last_key_frame + key_interval * 4, /* Right soft button again */
    };
    int num_soft_keys = sizeof(soft_button_frames) / sizeof(soft_button_frames[0]);
    int soft_button_ids[] = {0, 1, 0, 1}; /* left, right, left, right */
    int last_activity_frame = 0; /* Track last frame with meaningful activity */
    
    /* Idle detection: if no activity for this many frames after key injection phase,
     * the MIDlet is considered idle and we exit cleanly. */
    int idle_timeout_frames = 300; /* 3 seconds at 10ms/frame */
    /* FIX-19k: allow overriding the harness limits via environment so long
     * self-test suites (VmTest threads/memory/perf groups take >60s) can
     * complete headlessly. */
    {
        const char* idle_env = getenv("NOJME_HEADLESS_IDLE");
        if (idle_env) {
            int v = atoi(idle_env);
            if (v > 0) idle_timeout_frames = v;
        }
    }
    
    while (ctx->running && ctx->jvm && ctx->jvm->running) {
        frames++;
        int had_activity = 0;
        
        /* v34.43 PERF-DIAG (Asphalt 3D "freeze every ~2s"): time the WORK
         * part of each iteration (pumps + repaint, excluding the pacing
         * sleep). NOJME_SLOWFRAME_MS=<threshold> — iterations whose work
         * exceeds the threshold are reported with instruction and GC
         * deltas so periodic stalls (GC pauses, texture reload bursts,
         * scheduler hiccups) become visible in the log. */
        uint64_t slowframe_iter_start_us = 0;
        uint64_t slowframe_instr0 = 0;
        {
            static int sf_ms = -1;
            if (sf_ms < 0) {
                const char* sf = getenv("NOJME_SLOWFRAME_MS");
                sf_ms = (sf && atoi(sf) > 0) ? atoi(sf) : 0;
            }
            if (sf_ms > 0) {
                struct timespec sf_ts;
                clock_gettime(CLOCK_MONOTONIC, &sf_ts);
                slowframe_iter_start_us = (uint64_t)sf_ts.tv_sec * 1000000ULL
                                        + (uint64_t)sf_ts.tv_nsec / 1000ULL;
                slowframe_instr0 = ctx->jvm->instr_count;
            }
        }
        
        /* v31: NOJME_HEADLESS_NOKEYS=1 — disable ALL key/softkey/KEYSEQ
         * injection (M3GTest binds 4/6 to scene switching; auto-injection
         * perturbs deterministic scene capture). */
        int no_keys = 0;
        {
            const char* nk = getenv("NOJME_HEADLESS_NOKEYS");
            if (nk && (nk[0] == '1' || nk[0] == 'y' || nk[0] == 'Y')) no_keys = 1;
        }
        
        /* v31: NOJME_HEADLESS_SNAP_FRAMES="f1,f2,..." — snapshot exactly at
         * these frame numbers (10ms/frame). Complements SNAP_EVERY. */
        int snap_exact = 0;
        {
            static int snap_frames[256];
            static int snap_count = -1;
            if (snap_count < 0) {
                snap_count = 0;
                const char* sf = getenv("NOJME_HEADLESS_SNAP_FRAMES");
                if (sf && sf[0]) {
                    const char* p = sf;
                    while (*p && snap_count < 256) {
                        char* end = NULL;
                        long v = strtol(p, &end, 10);
                        if (end == p) break;
                        if (v > 0) snap_frames[snap_count++] = (int)v;
                        p = end;
                        while (*p == ',' || *p == ' ') p++;
                    }
                    fprintf(stderr, "[Headless] SNAP_FRAMES: %d exact frames\n", snap_count);
                }
            }
            if (snap_count > 0) {
                for (int si = 0; si < snap_count; si++) {
                    if (snap_frames[si] == frames) { snap_exact = 1; break; }
                }
            }
        }
        
        /* FIX-19w: optional periodic frame snapshots for visual debugging
         * (NOJME_HEADLESS_SNAP_EVERY=<frames>, /tmp/j2me_frame_<n>.ppm) */
        {
            static int snap_every = -1;
            if (snap_every < 0) {
                snap_every = 0;
                const char* snap_env = getenv("NOJME_HEADLESS_SNAP_EVERY");
                if (snap_env) snap_every = atoi(snap_env);
            }
            if ((snap_exact || (snap_every > 0 && (frames % snap_every) == 0)) && ctx->framebuffer) {
                /* v29 FLICKER FIX: snapshot only settled canvas states, same
                 * rule as the libretro presenter — mid-paint snapshots caught
                 * the "white/gradient underlay" intermediate and looked like
                 * frame flicker. Skipped frames keep their number (gap), so
                 * analysis tools see true timestamps. */
                extern int midp_canvas_mid_frame(void);
                if (!midp_canvas_mid_frame()) {
                    char snap_path[128];
                    snprintf(snap_path, sizeof(snap_path), "/tmp/j2me_frame_%05d.ppm", frames);
                    sdl_save_framebuffer_to_file(ctx, snap_path);
                } else {
                    /* v34.11: measure how often snapshots are skipped due to
                     * the mid-frame gate — high skip rates make snapshot
                     * files stale and fake a "frozen screen". */
                    static int snap_skip_count = 0;
                    static int snap_skip_last_report = 0;
                    snap_skip_count++;
                    if (snap_skip_count - snap_skip_last_report >= 50) {
                        fprintf(stderr, "[SNAPSKIP] skipped %d snapshots total (frame %d, mid_frame=1)\n",
                                snap_skip_count, frames);
                        snap_skip_last_report = snap_skip_count;
                    }
                }
            }
        }
        
        /* Phase 1: Inject regular key presses */
        for (int ki = 0; no_keys == 0 && ki < num_keys; ki++) {
            int target_frame = key_fire_frame + ki * key_interval;
            if (frames == target_frame) {
                fprintf(stderr, "[Headless] Injecting key code %d (frame %d)\n", keys_to_try[ki], frames);
                midp_call_keyPressed(ctx->jvm, keys_to_try[ki]);
                midp_call_keyReleased(ctx->jvm, keys_to_try[ki]);
                had_activity = 1;
            }
        }
        
        /* Phase 2: Inject soft button presses (for Form/List Exit/OK commands) */
        for (int si = 0; no_keys == 0 && si < num_soft_keys; si++) {
            if (frames == soft_button_frames[si]) {
                fprintf(stderr, "[Headless] Injecting soft button %d (frame %d)\n",
                        soft_button_ids[si], frames);
                midp_handle_soft_button(ctx->jvm, soft_button_ids[si]);
                had_activity = 1;
            }
        }

        /* v29: cyclic soft-button injection (NOJME_HEADLESS_SOFTKEY_EVERY=<frames>).
         * The fixed phase-2 schedule fires only in the first ~1s; slow loads
         * (RMS-dependent "Enable sounds?" dialogs at frame 6000+) never see a
         * soft key and the harness stalls on dialogs forever. */
        {
            static int sk_every = -1;
            if (sk_every < 0) {
                sk_every = 0;
                const char* sk_env = getenv("NOJME_HEADLESS_SOFTKEY_EVERY");
                if (sk_env) sk_every = atoi(sk_env);
            }
            if (sk_every > 0 && !no_keys && frames > soft_button_frames[num_soft_keys - 1] &&
                ((frames - soft_button_frames[num_soft_keys - 1]) % sk_every) == 0) {
                int sk_id = ((frames / sk_every) & 1);
                fprintf(stderr, "[Headless] Cyclic soft button %d (frame %d)\n", sk_id, frames);
                midp_handle_soft_button(ctx->jvm, sk_id);
                had_activity = 1;
            }
        }

        /* Phase 2.5 (v22): cyclic KEYSEQ injection for multi-step menus
         * (v31: NOT disabled by NOKEYS — explicit user key sequences are
         * intentional harness input, unlike the blind auto-injection)
         * [YS-TEST] NOJME_HEADLESS_SWKEY=1: for -6/-7 model the EXACT
         * Switch/SDL frontend dispatch (sdl_handle_key_event soft-button
         * block): press goes through midp_handle_soft_button() FIRST and
         * only reaches the game as keyPressed() when the interceptor
         * returns false; release always falls through to keyReleased().
         * Default 0 = legacy direct injection. */
        if (seq_len > 0 && frames > 100 &&
            ((frames - 100) % seq_every) == 0) {
            int ki = ((frames - 100) / seq_every) % seq_len;
            fprintf(stderr, "[Headless] KEYSEQ inject %d (frame %d)\n",
                    seq_keys[ki], frames);
            {
                static int swkey_mode = -1;
                int kc = seq_keys[ki];
                if (swkey_mode < 0)
                    swkey_mode = (getenv("NOJME_HEADLESS_SWKEY") &&
                                  getenv("NOJME_HEADLESS_SWKEY")[0] != '0') ? 1 : 0;
                if (swkey_mode && (kc == -6 || kc == -7)) {
                    int btn = (kc == -6) ? 0 : 1;
                    if (!midp_handle_soft_button(ctx->jvm, btn)) {
                        midp_call_keyPressed(ctx->jvm, kc);
                        fprintf(stderr, "[Headless] SWKEY: interceptor=false -> keyPressed(%d)\n", kc);
                    } else {
                        fprintf(stderr, "[Headless] SWKEY: interceptor=TRUE, keyPressed(%d) SWALLOWED\n", kc);
                    }
                    midp_call_keyReleased(ctx->jvm, kc);
                } else {
                    midp_call_keyPressed(ctx->jvm, kc);
                    midp_call_keyReleased(ctx->jvm, kc);
                }
            }
            had_activity = 1;
        }

        /* Phase 2.6 (v34.9): one-shot KEYSCRIPT injection, works even with
         * NOKEYS=1 (deterministic multi-step navigation for regression).
         * v34.19 (Worms Forts 3D): the release is now DELAYED by 12 frames
         * (~120ms) instead of firing back-to-back with the press. Games
         * that poll HELD-key flags once per 80ms tick (Worms movement:
         * s.a(4)/s.a(6) pressed-flags) never saw a 0ms pulse — the flag was
         * set and cleared within the same headless iteration, before the
         * game thread's next tick. Ring-buffer games are unaffected: the
         * buffer records on PRESS only, so a longer hold still yields
         * exactly one key event. */
        {
            static int script_pos = 0;
            /* Pending releases: (key, due-frame) pairs still held down. */
            static int held_key[KEYSCRIPT_MAX];
            static int held_due[KEYSCRIPT_MAX];
            static int held_n = 0;
            /* v36.01 testkeys: hold-window override. The fixed 12-frame hold
             * (~1-2 polls of a 10ms getKeyStates loop) proves the tap latch;
             * the HELD-bit semantics need a LONG hold (field fix claims FIRE
             * visible in hundreds of consecutive polls).
             * NOJME_HEADLESS_KEYHOLD=<frames> extends the hold for the
             * self-test; default stays 12. */
            static int hold_frames = -1;
            if (hold_frames < 0) {
                const char* he = getenv("NOJME_HEADLESS_KEYHOLD");
                hold_frames = (he && he[0]) ? atoi(he) : 12;
                if (hold_frames <= 0) hold_frames = 12;
            }
            /* Release any key whose hold window expired. */
            for (int i = 0; i < held_n; ) {
                if (frames >= held_due[i]) {
                    midp_call_keyReleased(ctx->jvm, held_key[i]);
                    held_key[i] = held_key[held_n - 1];
                    held_due[i] = held_due[held_n - 1];
                    held_n--;
                } else {
                    i++;
                }
            }
            while (script_pos < script_len && frames >= script_frame[script_pos]) {
                fprintf(stderr, "[Headless] KEYSCRIPT inject %d (frame %d)\n",
                        script_key[script_pos], frames);
                midp_call_keyPressed(ctx->jvm, script_key[script_pos]);
                if (held_n < KEYSCRIPT_MAX) {
                    held_key[held_n] = script_key[script_pos];
                    held_due[held_n] = frames + hold_frames;  /* ~120ms hold, KEYHOLD overrides */
                    held_n++;
                } else {
                    /* Overflow (shouldn't happen): fall back to instant release. */
                    midp_call_keyReleased(ctx->jvm, script_key[script_pos]);
                }
                script_pos++;
                had_activity = 1;
            }
        }

        /* v34.34: one-shot POINTERSCRIPT injection (stylus-only games). */
        {
            static int ptr_pos = 0;
            while (ptr_pos < ptr_len && frames >= ptr_frame[ptr_pos]) {
                int x = ptr_x[ptr_pos], y = ptr_y[ptr_pos];
                char a = ptr_act[ptr_pos];
                fprintf(stderr, "[Headless] POINTERSCRIPT inject %d,%d:%c (frame %d)\n",
                        x, y, a, frames);
                if (a == 'p') midp_call_pointerPressed(ctx->jvm, x, y);
                else if (a == 'r') midp_call_pointerReleased(ctx->jvm, x, y);
                else if (a == 'd') midp_call_pointerDragged(ctx->jvm, x, y);
                ptr_pos++;
                had_activity = 1;
            }
        }

        /* Process timers
         * v34.43 SLOWFRAME: timed — java.util.Timer tasks execute HERE on
         * the main thread; periodic game tasks (Asphalt's 2s loader ticks)
         * showed up as unattributed main-loop stalls. */
        uint64_t slowframe_sec_timers_us = 0;
        uint64_t slowframe_sec_pre_us = 0;   /* loop-top .. here (keys/snap) */
        if (slowframe_iter_start_us) {
            struct timespec sec_ts;
            clock_gettime(CLOCK_MONOTONIC, &sec_ts);
            uint64_t t0 = (uint64_t)sec_ts.tv_sec * 1000000ULL + (uint64_t)sec_ts.tv_nsec / 1000ULL;
            slowframe_sec_pre_us = t0 - slowframe_iter_start_us;
            jvm_process_timers(ctx->jvm);
            clock_gettime(CLOCK_MONOTONIC, &sec_ts);
            slowframe_sec_timers_us = (uint64_t)sec_ts.tv_sec * 1000000ULL + (uint64_t)sec_ts.tv_nsec / 1000ULL - t0;
        } else {
            jvm_process_timers(ctx->jvm);
        }

        /* v38 DIAG: NOJME_BS_STATEDUMP=1 — dump BlackShark3D game-state
         * statics each 300 frames (menu state al.a, engine state ap.c,
         * scene-loaded flag q.a, FIRE flag m.d, load progress f.e).
         * Reads Java statics the same way op_getstatic does. */
        {
            static int bs_dump_env = -1;
            if (bs_dump_env < 0) {
                const char* e = getenv("NOJME_BS_STATEDUMP");
                bs_dump_env = (e && e[0] && e[0] != '0') ? 1 : 0;
            }
            if (bs_dump_env && (frames % 300) == 0) {
                struct { const char* cls; const char* name; const char* desc; } vars[] = {
                    {"al", "a", "I"},   /* menu state */
                    {"ap", "c", "I"},   /* engine state */
                    {"q",  "a", "Z"},   /* scene loaded */
                    {"m",  "d", "Z"},   /* FIRE held */
                    {"m",  "f", "Z"},   /* LSK held */
                    {"f",  "e", "I"},   /* load progress % */
                    {"Main","a", "I"},  /* frame counter */
                };
                fprintf(stderr, "[BSSTATE] f=%d:", frames);
                for (size_t vi = 0; vi < sizeof(vars)/sizeof(vars[0]); vi++) {
                    JavaClass* jc = jvm_load_class(ctx->jvm, vars[vi].cls);
                    int val = -1;
                    if (jc) {
                        for (JavaClass* sc = jc; sc && val == -1; sc = sc->super_class) {
                            if (!sc->static_fields) continue;
                            for (int i = 0; i < sc->static_fields_count; i++) {
                                if (sc->static_fields[i].name &&
                                    strcmp(sc->static_fields[i].name, vars[vi].name) == 0 &&
                                    sc->static_fields[i].descriptor &&
                                    strcmp(sc->static_fields[i].descriptor, vars[vi].desc) == 0) {
                                    val = sc->static_fields[i].value.i;
                                    break;
                                }
                            }
                        }
                    }
                    fprintf(stderr, " %s.%s%s=%d", vars[vi].cls, vars[vi].name, vars[vi].desc, val);
                }
                fprintf(stderr, "\n");
                /* v38 DIAG part 2: instance fields of as.a:Laf (terrain map
                 * object) — the scroll-loop inputs af.a:F / af.e:I / w.e:F.
                 * Reads via the same hierarchy walk as getfield (name+desc,
                 * most-derived class first). */
                {
                    JavaClass* as_cls = jvm_load_class(ctx->jvm, "as");
                    JavaObject* map = NULL;
                    if (as_cls && as_cls->static_fields) {
                        for (int i = 0; i < as_cls->static_fields_count; i++) {
                            if (as_cls->static_fields[i].name &&
                                strcmp(as_cls->static_fields[i].name, "a") == 0 &&
                                as_cls->static_fields[i].descriptor &&
                                strcmp(as_cls->static_fields[i].descriptor, "Laf;") == 0) {
                                map = (JavaObject*)as_cls->static_fields[i].value.ref;
                                break;
                            }
                        }
                    }
                    if (map && map->header.clazz) {
                        /* v38 DIAG: raw dump of ALL slots (int bits + ref guess) */
                        {
                            int maxs = (int)((map->header.clazz->instance_size - 24) / 8);
                            if (maxs > 40) maxs = 40;
                            fprintf(stderr, "[BSSLOT] af=%p slots:", (void*)map);
                            for (int s = 0; s < maxs; s++) {
                                JavaValue v = map->fields[s];
                                if (v.i >= 0x10000 && v.i < 0x7fffffff)
                                    fprintf(stderr, " [%d]=REF?%08x", s, (unsigned)v.i);
                                else
                                    fprintf(stderr, " [%d]=%08x", s, (unsigned)v.i);
                            }
                            fprintf(stderr, "\n");
                        }
                        fprintf(stderr, "[BSMAP] af-instance %p (af size=%zu, w size=%zu, af fields=%d, w fields=%d):",
                                (void*)map,
                                (size_t)map->header.clazz->instance_size,
                                map->header.clazz->super_class ? (size_t)map->header.clazz->super_class->instance_size : 0,
                                map->header.clazz->fields_count,
                                map->header.clazz->super_class ? map->header.clazz->super_class->fields_count : -1);
                        struct { const char* name; const char* desc; } ivars[] = {
                            {"a", "F"}, {"e", "I"}, {"g", "I"}, {"g", "F"},
                            {"e", "F"}, {"f", "F"}, {"b", "F"}, {"a", "I"}, {"b", "I"},
                        };
                        fprintf(stderr, "[BSMAP] af-instance %p:", (void*)map);
                        for (size_t k = 0; k < sizeof(ivars)/sizeof(ivars[0]); k++) {
                            JavaClass* hc[64]; int hd = 0;
                            int found_slot = -1; const char* defc = "?";
                            for (JavaClass* sc = map->header.clazz; sc && hd < 64; sc = sc->super_class)
                                hc[hd++] = sc;
                            for (int h = hd - 1; h >= 0 && found_slot < 0; h--) {
                                if (!hc[h]->fields) continue;
                                for (int i = 0; i < hc[h]->fields_count; i++) {
                                    JavaField* f = &hc[h]->fields[i];
                                    if ((f->access_flags & 0x0008) /* ACC_STATIC */) continue;
                                    if (f->name && strcmp(f->name, ivars[k].name) == 0 &&
                                        f->descriptor && strcmp(f->descriptor, ivars[k].desc) == 0) {
                                        int slot = 0;
                                        for (int j = 0; j < h; j++)
                                            for (int q = 0; q < hc[j]->fields_count; q++)
                                                if (!(hc[j]->fields[q].access_flags & 0x0008)) slot++;
                                        for (int q = 0; q < i; q++)
                                            if (!(hc[h]->fields[q].access_flags & 0x0008)) slot++;
                                        found_slot = slot;
                                        defc = hc[h]->class_name ? hc[h]->class_name : "?";
                                        break;
                                    }
                                }
                            }
                            if (found_slot >= 0) {
                                int maxs = (map->header.clazz->instance_size - 24) / 8;
                                if (found_slot < maxs) {
                                    JavaValue v = map->fields[found_slot];
                                    if (ivars[k].desc[0] == 'F')
                                        fprintf(stderr, " %s.%s%s(def:%s)=%f", "as.a", ivars[k].name, ivars[k].desc, defc, v.f);
                                    else
                                        fprintf(stderr, " %s.%s%s(def:%s)=%d", "as.a", ivars[k].name, ivars[k].desc, defc, v.i);
                                } else {
                                    fprintf(stderr, " %s.%s%s(slot %d OOB max %d)", "as.a", ivars[k].name, ivars[k].desc, found_slot, maxs);
                                }
                            } else {
                                fprintf(stderr, " %s.%s%s(not-found)", "as.a", ivars[k].name, ivars[k].desc);
                            }
                        }
                        fprintf(stderr, "\n");
                    }
                }
            }
        }

        /* v34.9: NOJME_HEADLESS_WATCHDOG=1 — periodic thread-stack dump to
         * diagnose hangs (which game method is each thread stuck in). */
        {
            static int watchdog_env = -1;
            if (watchdog_env < 0) {
                const char* w = getenv("NOJME_HEADLESS_WATCHDOG");
                watchdog_env = (w && w[0] && w[0] != '0') ? 1 : 0;
            }
            if (watchdog_env && (frames % 300) == 0) {
                extern void jvm_dump_all_threads(void);
                extern void jvm_dump_blocked_threads(void);
                extern volatile int mondiag_dbg_state[16];
                extern volatile int mondiag_dbg_iter[16];
                /* v34.11: also show presenter-gating state — a stuck
                 * mid-frame flag freezes the display while the game lives. */
                {
                    extern volatile int g_m3g_target_bound_flag;
                    extern int midp_canvas_mid_frame(void);
                    int mid = midp_canvas_mid_frame();
                    fprintf(stderr, "[Headless] PRESENTGATE mid=%d m3g_bound=%d\n",
                            mid, g_m3g_target_bound_flag);
                }
                fprintf(stderr, "[Headless] THREADDUMP (frame %d) wait per-tid:", frames);
                for (int ti = 0; ti < 8; ti++) {
                    fprintf(stderr, " t%d=[%d,%d]", ti, mondiag_dbg_state[ti], mondiag_dbg_iter[ti]);
                }
                fprintf(stderr, "\n");
                jvm_dump_all_threads();
                jvm_dump_blocked_threads();
                had_activity = 1;
            }
        }
        
        /* v35: age key queue + deliver keys nobody else will drain
         * (event-driven Form/List menus never re-enter the interpreter).
         * v34.43 SLOWFRAME: per-section timing (env-gated, same knob as
         * the SLOWFRAME detector) — attributes main-loop stalls to the
         * responsible pump. */
        uint64_t slowframe_sec_pumps_us = 0, slowframe_sec_serially_us = 0,
                 slowframe_sec_repaint_us = 0;
        uint64_t slowframe_sec_shownotify_us = 0, slowframe_sec_frametick_us = 0,
                 slowframe_sec_defkeys_us = 0;
        if (slowframe_iter_start_us) {
            struct timespec sec_ts;
            uint64_t t0, t1;
            {
                extern void midp_frame_tick(void);
                extern void midp_pump_deferred_keys(JVM* jvm);
                extern void midp_pump_pending_shownotify(JVM* jvm);
                clock_gettime(CLOCK_MONOTONIC, &sec_ts);
                t0 = (uint64_t)sec_ts.tv_sec * 1000000ULL + (uint64_t)sec_ts.tv_nsec / 1000ULL;
                midp_pump_pending_shownotify(ctx->jvm);  /* v34.33: deferred setCurrent, before callSerially/repaints */
                clock_gettime(CLOCK_MONOTONIC, &sec_ts);
                t1 = (uint64_t)sec_ts.tv_sec * 1000000ULL + (uint64_t)sec_ts.tv_nsec / 1000ULL;
                slowframe_sec_shownotify_us = t1 - t0;
                t0 = t1;
                midp_frame_tick();
                clock_gettime(CLOCK_MONOTONIC, &sec_ts);
                t1 = (uint64_t)sec_ts.tv_sec * 1000000ULL + (uint64_t)sec_ts.tv_nsec / 1000ULL;
                slowframe_sec_frametick_us = t1 - t0;
                t0 = t1;
                midp_pump_deferred_keys(ctx->jvm);
                clock_gettime(CLOCK_MONOTONIC, &sec_ts);
                t1 = (uint64_t)sec_ts.tv_sec * 1000000ULL + (uint64_t)sec_ts.tv_nsec / 1000ULL;
                slowframe_sec_defkeys_us = t1 - t0;
            }
            slowframe_sec_pumps_us = slowframe_sec_shownotify_us + slowframe_sec_frametick_us + slowframe_sec_defkeys_us;
            (void)slowframe_sec_pumps_us; /* aggregate retained for future reports */
            t0 = (uint64_t)sec_ts.tv_sec * 1000000ULL + (uint64_t)sec_ts.tv_nsec / 1000ULL;
            /* Process callSerially queue */
            midp_process_call_serially_queue(ctx->jvm);
            clock_gettime(CLOCK_MONOTONIC, &sec_ts);
            slowframe_sec_serially_us = (uint64_t)sec_ts.tv_sec * 1000000ULL + (uint64_t)sec_ts.tv_nsec / 1000ULL - t0;
        } else {
            {
                extern void midp_frame_tick(void);
                extern void midp_pump_deferred_keys(JVM* jvm);
                extern void midp_pump_pending_shownotify(JVM* jvm);
                midp_pump_pending_shownotify(ctx->jvm);  /* v34.33: deferred setCurrent, before callSerially/repaints */
                midp_frame_tick();
                midp_pump_deferred_keys(ctx->jvm);
            }
            /* Process callSerially queue */
            midp_process_call_serially_queue(ctx->jvm);
        }
        
        /* Check Alert timeout */
        midp_check_alert_timeout(ctx->jvm);
        
        /* Auto-redraw at ~60 FPS */
        clock_gettime(CLOCK_MONOTONIC, &current_ts);
        uint64_t current_time = (uint64_t)current_ts.tv_sec * 1000 + current_ts.tv_nsec / 1000000;
        if (current_time - last_auto_redraw >= 16) {
            last_auto_redraw = current_time;
            if (slowframe_iter_start_us) {
                struct timespec sec_ts;
                clock_gettime(CLOCK_MONOTONIC, &sec_ts);
                uint64_t t0 = (uint64_t)sec_ts.tv_sec * 1000000ULL + (uint64_t)sec_ts.tv_nsec / 1000ULL;
                midp_process_repaints(ctx->jvm);
                midp_clear_pending_repaint();
                clock_gettime(CLOCK_MONOTONIC, &sec_ts);
                slowframe_sec_repaint_us = (uint64_t)sec_ts.tv_sec * 1000000ULL + (uint64_t)sec_ts.tv_nsec / 1000ULL - t0;
            } else {
                midp_process_repaints(ctx->jvm);
                midp_clear_pending_repaint();
            }
        }
        
        /* Check for error display */
        if (sdl_has_error()) {
            fprintf(stderr, "[Headless] Error detected, stopping.\n");
            break;
        }
        
        /* Track activity - also count text drawing as activity */
        if (had_activity || headless_check_text_activity()) {
            last_activity_frame = frames;
        }
        
        /* Idle detection: after all key injections are complete,
         * if no activity for idle_timeout_frames, exit cleanly.
         * This handles MIDlets that never call notifyDestroyed(). */
        if (frames > soft_button_frames[num_soft_keys - 1] + 50) {
            if (frames - last_activity_frame >= idle_timeout_frames) {
                fprintf(stderr, "[Headless] Idle timeout: no activity for %d frames, exiting cleanly.\n",
                        idle_timeout_frames);
                break;
            }
        }
        
        /* Safety: max time limit (60 seconds by default, NOJME_HEADLESS_MAX_MS) */
        uint64_t elapsed_ms = current_time - start_time_ms;
        {
            static uint64_t max_ms = 0;
            if (max_ms == 0) {
                max_ms = 60000;
                const char* max_env = getenv("NOJME_HEADLESS_MAX_MS");
                if (max_env) {
                    long v = atol(max_env);
                    if (v > 0) max_ms = (uint64_t)v;
                }
            }
            if (elapsed_ms > max_ms) {
                fprintf(stderr, "[Headless] Max time limit reached (%llu ms), exiting.\n",
                        (unsigned long long)max_ms);
                break;
            }
        }
        
        /* v34.42 PERF-DIAG: NOJME_HEADLESS_NO_PACE=1 skips the 10ms pacing
         * sleep — CPU-bound profiling (gprof) of the render/VM pipeline.
         * Default behavior (paced, 100 ticks/s) is unchanged. */
        uint64_t slowframe_sec_presleep_us = 0;
        if (slowframe_iter_start_us) {
            struct timespec sec_ts;
            clock_gettime(CLOCK_MONOTONIC, &sec_ts);
            slowframe_sec_presleep_us = (uint64_t)sec_ts.tv_sec * 1000000ULL + (uint64_t)sec_ts.tv_nsec / 1000ULL - slowframe_iter_start_us;
        }
        {
            static int no_pace = -1;
            if (no_pace < 0) no_pace = getenv("NOJME_HEADLESS_NO_PACE") ? 1 : 0;
            if (!no_pace) usleep(10000); /* 10ms per iteration */
        }

        /* v50 (Asphalt image freeze): end-of-iteration vsync tick — the
         * headless twin of libretro_end_frame's tick. Game-thread
         * serviceRepaints() paces itself on this (KVM vblank semantics);
         * without a ticking driver the paced wait would always hit its
         * 250ms bail-out and slow runner-driven games down. */
        {
            extern void midp_vsync_tick(void);
            midp_vsync_tick();
        }

        /* v34.43 PERF-DIAG: report slow work-iterations (see loop top). */
        if (slowframe_iter_start_us) {
            static int sf_ms = -1;
            extern uint64_t g_gc_collections;
            if (sf_ms < 0) {
                const char* sf = getenv("NOJME_SLOWFRAME_MS");
                sf_ms = (sf && atoi(sf) > 0) ? atoi(sf) : 0;
            }
            struct timespec sf_ts;
            clock_gettime(CLOCK_MONOTONIC, &sf_ts);
            uint64_t now_us = (uint64_t)sf_ts.tv_sec * 1000000ULL
                            + (uint64_t)sf_ts.tv_nsec / 1000ULL;
            uint64_t work_us = now_us - slowframe_iter_start_us;
            if (work_us > (uint64_t)sf_ms * 1000ULL) {
                fprintf(stderr,
                        "[SLOWFRAME] frame=%d work=%.2fms presleep=%.2fms sleep=%.2fms pre=%.2fms timers=%.2fms shownotify=%.2fms frametick=%.2fms defkeys=%.2fms serially=%.2fms repaint=%.2fms instr=+%llu gc_total=%llu\n",
                        frames,
                        (double)work_us / 1000.0,
                        (double)slowframe_sec_presleep_us / 1000.0,
                        (double)((work_us > slowframe_sec_presleep_us) ? work_us - slowframe_sec_presleep_us : 0) / 1000.0,
                        (double)slowframe_sec_pre_us / 1000.0,
                        (double)slowframe_sec_timers_us / 1000.0,
                        (double)slowframe_sec_shownotify_us / 1000.0,
                        (double)slowframe_sec_frametick_us / 1000.0,
                        (double)slowframe_sec_defkeys_us / 1000.0,
                        (double)slowframe_sec_serially_us / 1000.0,
                        (double)slowframe_sec_repaint_us / 1000.0,
                        (unsigned long long)(ctx->jvm->instr_count - slowframe_instr0),
                        (unsigned long long)g_gc_collections);
                /* v34.43: NOJME_SLOWFRAME_DUMP=1 — attribute the spike:
                 * dump every thread's current Java frame (who is executing
                 * WHAT during the stall). ~0.5 Hz worst case, cheap. */
                {
                    static int sf_dump = -1;
                    if (sf_dump < 0) {
                        const char* d = getenv("NOJME_SLOWFRAME_DUMP");
                        sf_dump = (d && d[0] && d[0] != '0') ? 1 : 0;
                    }
                    if (sf_dump) {
                        extern void jvm_dump_all_threads(void);
                        jvm_dump_all_threads();
                    }
                }
            }
        }
    }
    
    if (frames >= max_frames) {
        fprintf(stderr, "[Headless] Reached max iterations\n");
    } else {
        fprintf(stderr, "[Headless] Stopped after %d iterations\n", frames);
    }

    /* v34.72: if the loop ended because the VM stopped (MIDlet finished),
     * draw the explicit finished screen so frame dumps/tests can see it. */
    if (ctx && ctx->jvm && !ctx->jvm->running && !sdl_has_error()) {
        sdl_draw_midlet_finished_screen(ctx);
    }
    
    /* Analyze framebuffer content */
    if (ctx && ctx->framebuffer) {
        int non_zero = 0;
        int total = ctx->width * ctx->height;
        for (int i = 0; i < total; i++) {
            if ((ctx->framebuffer[i] & 0x00FFFFFF) != 0) non_zero++;
        }
        fprintf(stderr, "[Headless] Framebuffer: %d/%d non-zero pixels (%.1f%%)\n",
                non_zero, total, 100.0 * non_zero / total);
        /* Save framebuffer to PPM for visual inspection */
        sdl_save_framebuffer_to_file(ctx, "/tmp/bounce_framebuffer.ppm");
        fprintf(stderr, "[Headless] Framebuffer saved to /tmp/bounce_framebuffer.ppm\n");
    }
    
    fprintf(stderr, "[Headless] Finished\n");
}

void sdl_set_fullscreen(SdlContext* ctx, bool fullscreen) { (void)ctx; (void)fullscreen; }
uint64_t sdl_get_ticks(SdlContext* ctx) { (void)ctx; return 0; }
void sdl_delay(uint32_t ms) { usleep(ms * 1000); }

void sdl_update_screen(SdlContext* ctx) { (void)ctx; }
void sdl_handle_events(SdlContext* ctx) { (void)ctx; }

bool sdl_key_pressed(SdlContext* ctx, int key) { (void)ctx; (void)key; return false; }
MidpGraphics* sdl_get_graphics(SdlContext* ctx) { (void)ctx; return NULL; }

int sdl_audio_init_simple(uint32_t sample_rate) { (void)sample_rate; return 0; }
void sdl_audio_shutdown(void) {}
void sdl_audio_queue_samples(const int16_t* samples, size_t count) { (void)samples; (void)count; }
/* v34.98: headless twin — media.c's teardown path calls this; the real
 * implementation lives in sdl_graphics.c (not linked into the headless app). */
void sdl_audio_close(SdlContext* ctx) { (void)ctx; }
size_t sdl_audio_get_queued_size(void) { return 0; }
int sdl_audio_device_alive(void) { return 0; } /* v34.94: no device here */

void sdl_present(SdlContext* ctx) { (void)ctx; }

/* v34.11 diagnostics: address of the headless video framebuffer, for
 * [PAINTBIND] MATCH checks in display.c. */
void* sdl_headless_framebuffer_addr(void) {
    return (void*)g_headless_ctx.framebuffer;
}

/* Headless mode needs to process repaints */
static bool g_headless_needs_redraw = false;

void sdl_request_redraw(void) {
    g_headless_needs_redraw = true;
}

bool sdl_needs_redraw(SdlContext* ctx) { 
    (void)ctx; 
    return g_headless_needs_redraw; 
}

void sdl_clear_redraw(SdlContext* ctx) { 
    (void)ctx; 
    g_headless_needs_redraw = false; 
}

void sdl_process_events_minimal(void) {
    /* Process timers (e.g. java.util.Timer, TimerTask) */
    extern void jvm_process_timers(JVM* jvm);
    if (g_headless_ctx.jvm && g_headless_ctx.jvm->running) {
        jvm_process_timers(g_headless_ctx.jvm);
    }
    
    /* Process any pending repaints */
    extern int midp_process_repaints(JVM* jvm);
    if (g_headless_needs_redraw && g_headless_ctx.jvm) {
        midp_process_repaints(g_headless_ctx.jvm);
        g_headless_needs_redraw = false;
    }
}

void sdl_update_texture(SdlContext* ctx) { (void)ctx; }

void sdl_clear(SdlContext* ctx, uint32_t color) {
    if (!ctx || !ctx->framebuffer) return;
    for (int i = 0; i < ctx->width * ctx->height; i++)
        ctx->framebuffer[i] = color;
}

int sdl_resize(SdlContext* ctx, int width, int height) { (void)ctx; (void)width; (void)height; return 0; }
int sdl_screenshot(SdlContext* ctx, const char* filename) { (void)ctx; (void)filename; return -1; }
void sdl_frame_end(SdlContext* ctx) { (void)ctx; }
void sdl_sleep(uint32_t ms) { usleep(ms * 1000); }
void sdl_set_title(SdlContext* ctx, const char* title) { (void)ctx; (void)title; }
void sdl_toggle_fullscreen(SdlContext* ctx) { (void)ctx; }
void sdl_get_window_size(SdlContext* ctx, int* width, int* height) {
    if (width && ctx) *width = ctx->width;
    if (height && ctx) *height = ctx->height;
}
void sdl_stop(SdlContext* ctx) { if (ctx) ctx->running = false; }
MidpPlatformCallbacks* sdl_get_platform_callbacks(SdlContext* ctx) { (void)ctx; return NULL; }
bool sdl_is_screen_graphics(MidpGraphics* gfx) { (void)gfx; return true; }

int sdl_audio_init(SdlContext* ctx, int frequency, int channels, int samples) {
    (void)ctx; (void)frequency; (void)channels; (void)samples;
    return 0;
}

int sdl_audio_queue(SdlContext* ctx, const void* data, size_t length) {
    (void)ctx; (void)data; (void)length;
    return 0;
}

uint32_t sdl_audio_queued_size(SdlContext* ctx) { (void)ctx; return 0; }
void sdl_audio_clear(SdlContext* ctx) { (void)ctx; }
void sdl_process_events(SdlContext* ctx) { (void)ctx; }
int sdl_key_to_midp(int sdl_key) { (void)sdl_key; return 0; }
int sdl_key_to_game_action(int sdl_key) { (void)sdl_key; return 0; }
void sdl_get_pointer(SdlContext* ctx, int* x, int* y) { if (x) *x = 0; if (y) *y = 0; (void)ctx; }
bool sdl_pointer_pressed(SdlContext* ctx) { (void)ctx; return false; }
void sdl_dump_info(SdlContext* ctx) { (void)ctx; }
void sdl_save_framebuffer_to_file(SdlContext* ctx, const char* filename) {
    if (!ctx || !ctx->framebuffer || !filename) return;
    FILE* f = fopen(filename, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", ctx->width, ctx->height);
    for (int i = 0; i < ctx->width * ctx->height; i++) {
        uint32_t pixel = ctx->framebuffer[i];
        uint8_t r = (pixel >> 16) & 0xFF;
        uint8_t g = (pixel >> 8) & 0xFF;
        uint8_t b = pixel & 0xFF;
        fputc(r, f); fputc(g, f); fputc(b, f);
    }
    fclose(f);
}

/* JAR resource loading - provided by main.c */
extern uint8_t* load_jar_resource(const char* path, size_t* size);

/* Error display stubs for headless mode */
static char g_error_title[512] = {0};
static char g_error_message[2048] = {0};
static char g_error_stack[8192] = {0};
static char g_error_extra[2048] = {0};
static bool g_has_error = false;

void sdl_set_error_info(const char* title, const char* message, const char* stack_trace) {
    g_has_error = true;
    if (title) {
        strncpy(g_error_title, title, sizeof(g_error_title) - 1);
        g_error_title[sizeof(g_error_title) - 1] = '\0';
    }
    if (message) {
        strncpy(g_error_message, message, sizeof(g_error_message) - 1);
        g_error_message[sizeof(g_error_message) - 1] = '\0';
    }
    if (stack_trace) {
        strncpy(g_error_stack, stack_trace, sizeof(g_error_stack) - 1);
        g_error_stack[sizeof(g_error_stack) - 1] = '\0';
    }
    
    fprintf(stderr, "\n========================================\n");
    fprintf(stderr, "  [J2ME UNCAUGHT EXCEPTION]\n");
    fprintf(stderr, "========================================\n");
    fprintf(stderr, "  Exception: %s\n", g_error_title);
    if (g_error_message[0]) fprintf(stderr, "  Message:   %s\n", g_error_message);
    if (g_error_extra[0]) fprintf(stderr, "  Details:   %s\n", g_error_extra);
    if (g_error_stack[0]) fprintf(stderr, "  Stack:\n%s", g_error_stack);
    fprintf(stderr, "========================================\n\n");
    fflush(stderr);
}

void sdl_set_error_extra(const char* extra) {
    if (extra) {
        strncpy(g_error_extra, extra, sizeof(g_error_extra) - 1);
        g_error_extra[sizeof(g_error_extra) - 1] = '\0';
    } else {
        g_error_extra[0] = '\0';
    }
}

bool sdl_has_error(void) {
    return g_has_error;
}

void sdl_clear_error(void) {
    g_has_error = false;
    g_error_title[0] = '\0';
    g_error_message[0] = '\0';
    g_error_stack[0] = '\0';
    g_error_extra[0] = '\0';
}

void sdl_draw_error_screen(SdlContext* ctx) {
    /* In headless mode, just print to stderr */
    (void)ctx;
    fprintf(stderr, "\n========== ERROR SCREEN ==========\n");
    fprintf(stderr, "Error: %s\n", g_error_title);
    if (g_error_message[0]) {
        fprintf(stderr, "%s\n", g_error_message);
    }
    if (g_error_stack[0]) {
        fprintf(stderr, "\nStack:\n%s\n", g_error_stack);
    }
    fprintf(stderr, "==================================\n");
    fflush(stderr);
}

/* v34.72: "MIDlet finished" screen — headless twin. The headless driver
 * exits its loop when the VM stops, so the screen only needs to be drawn
 * once into the framebuffer (for frame-dump based tests) and reported on
 * stderr. Uses the shared 5x7 bitmap font (ASCII + Cyrillic). */
#include "midp/bitmap_font.h"

void sdl_draw_midlet_finished_screen(SdlContext* ctx) {
    static bool reported = false;
    if (!ctx || !ctx->framebuffer) {
        if (!reported) {
            fprintf(stderr, "[Headless] MIDlet finished\n");
            reported = true;
        }
        return;
    }
    int width = ctx->width > 0 ? ctx->width : 240;
    int height = ctx->height > 0 ? ctx->height : 320;
    uint32_t* fb = ctx->framebuffer;

    for (int i = 0; i < width * height; i++) {
        fb[i] = 0xFF000000;
    }

    /* Minimal centered text using the shared bitmap font. */
    const char* lines[2] = {
        "MIDlet finished",
        "\xD0\x9C\xD0\xB8\xD0\xB4\xD0\xBB\xD0\xB5\xD1\x82 \xD0\xB7\xD0\xB0\xD0\xB2\xD0\xB5\xD1\x80\xD1\x88\xD1\x91\xD0\xBD"  /* "Мидлет завершён" */
    };
    const uint32_t colors[2] = { 0xFFFFFFFF, 0xFFA8A8A8 };
    for (int l = 0; l < 2; l++) {
        int n = 0, i = 0, len = 0;
        while (lines[l][len]) len++;
        while (i < len) {
            int cp = utf8_decode(lines[l], &i, len);
            if (cp < 0) break;
            n++;
        }
        int x = (width - (n * (FONT_WIDTH + 1) - 1)) / 2;
        if (x < 1) x = 1;
        int y = height / 2 - 12 + l * (FONT_HEIGHT + 6);
        i = 0;
        while (i < len) {
            int cp = utf8_decode(lines[l], &i, len);
            if (cp < 0) break;
            const uint8_t* cd = get_char_data_unicode(cp);
            for (int row = 0; row < FONT_HEIGHT; row++) {
                uint8_t rd = cd[row];
                for (int col = 0; col < FONT_WIDTH; col++) {
                    if (rd & (1 << (4 - col))) {
                        int px = x + col, py = y + row;
                        if (px >= 0 && px < width && py >= 0 && py < height) {
                            fb[py * width + px] = colors[l];
                        }
                    }
                }
            }
            x += FONT_WIDTH + 1;
        }
    }

    if (!reported) {
        fprintf(stderr, "[Headless] MIDlet finished — black finished screen drawn\n");
        reported = true;
    }

    /* Settled-frame handoff (v34.61 scanout semantics). */
    {
        extern void midp_present_settled_snapshot(void);
        midp_present_settled_snapshot();
    }
}

