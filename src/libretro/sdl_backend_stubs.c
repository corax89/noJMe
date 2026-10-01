/*
 * SDL Backend Stubs for Libretro Build
 * With proper double buffering for multi-threaded mode
 * 
 * MODIFIED: Audio processing moved to separate thread
 * - Audio thread generates samples asynchronously
 * - Main thread retrieves processed samples
 * - Thread-safe ring buffer for audio data exchange
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#if defined(_WIN32)
/* No <pthread.h> on plain MinGW targets — use the Win32 shim */
#include "win_thread_shim.h"
#else
#include <pthread.h>
#endif
#include <stdatomic.h>
#include <sys/time.h>
#include <unistd.h>

#include "debug.h"
#include "debug_macros.h"
#include "libretro.h"
#include "sdl_backend.h"
#include "midp.h"
#include "jar_reader.h" /* v19: canonical JAR reader */
#include "miniz.h"

/* ============================================
 * External variables from libretro.c
 * ============================================ */

extern JVM* g_jvm;  /* defined in libretro.c (non-static) */
extern retro_video_refresh_t video_cb;
extern retro_input_poll_t input_poll_cb;
extern retro_input_state_t input_state_cb;
extern retro_environment_t environ_cb;

extern uint32_t* libretro_get_framebuffer(void);
extern int libretro_get_screen_width(void);
extern int libretro_get_screen_height(void);

/* ============================================
 * Audio Pipeline (v34.27 REWRITE)
 * ============================================ */

/* Audio buffer constants */
#define AUDIO_BUFFER_SIZE 8192
#define AUDIO_SOURCE_RATE 44100

/* v34.27: the libretro build no longer runs any background generator
 * thread. libretro_process_audio() (frontend thread, once per retro_run)
 * mixes exactly the samples the elapsed wall time calls for via
 * media_generate_audio_samples() and pushes them through audio_batch_cb.
 *
 * What this removes and why (the actual "crackle + performance drop"):
 *  1. media.c's audio_thread_func used to run even in libretro builds
 *     (its g_libretro_mode guard checked J2ME_LIBRETRO, a macro NOTHING
 *     defines - the Makefile passes -DLIBRETRO). Every 5 ms it mixed a
 *     FULL 4096-sample buffer (93 ms of audio, 18x realtime) - pure wasted
 *     CPU competing with the VM interpreter and 200Hz wakeups on
 *     single-core armv7.
 *  2. That overproduction overflowed every handoff buffer: under the old
 *     stubs the ring path DELIVERED RAW 44100 Hz samples to a frontend
 *     configured for 22050/11025 (the resampler only ran on the legacy
 *     path!) - wrong pitch/speed audio.
 *  3. Every full ring write blocked up to 10 ms in pthread_cond_timedwait
 *     and then DROPPED the whole chunk - periodic gaps = the crackle.
 *  4. malloc/free of mix and resample buffers on every generation call.
 *  5. Per-sample atomic RMWs in the ring read/write loops. */

static bool g_audio_initialized = false;

/* Audio parameters */
static uint32_t g_frontend_sample_rate = 22050;
static double g_audio_resample_ratio = 1.0;
static double g_audio_resample_pos = 0.0;

/* Time-based audio control */
static uint64_t g_last_frame_time = 0;
static double g_audio_time_accumulator = 0.0;

/* Forward declaration */
extern void media_generate_audio_samples(int samples);

/* ============================================
 * Legacy single-producer queue (the only path now)
 * ============================================ */

/* ============================================
 * Audio Buffer (single-producer queue: media.c -> libretro frontend)
 * ============================================ */

/* Small buffer - just enough for 2-3 frames to prevent overflow */
static int16_t g_audio_buffer[AUDIO_BUFFER_SIZE * 2];
static size_t g_audio_write_pos = 0;
static size_t g_audio_read_pos = 0;
static size_t g_audio_count = 0;

static uint64_t audio_get_time_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

static void audio_buffer_init(void) {
    if (!g_audio_initialized) {
        memset(g_audio_buffer, 0, sizeof(g_audio_buffer));
        g_audio_write_pos = 0;
        g_audio_read_pos = 0;
        g_audio_count = 0;
        g_audio_resample_pos = 0.0;
        g_audio_time_accumulator = 0.0;
        g_last_frame_time = audio_get_time_us();
        g_audio_initialized = true;
    }
}

void libretro_set_sample_rate(uint32_t rate) {
    g_frontend_sample_rate = rate;
    g_audio_resample_ratio = (double)AUDIO_SOURCE_RATE / (double)rate;
    g_audio_time_accumulator = 0.0;
    g_last_frame_time = audio_get_time_us();
}

void libretro_set_fps(int fps) {
    /* v34.74 (F-1): intentionally a NO-OP in the libretro build — pacing is
     * FRONTEND-driven: retro_get_system_av_info reports the fps (and the
     * live-change path pushes SET_SYSTEM_AV_INFO), so RetroArch calls
     * retro_run at the target rate. Nothing inside the core may sleep or
     * throttle per-frame. Kept for API compatibility with the SDL builds,
     * where ctx->target_fps drives the main-loop delay. */
    (void)fps;
}

static void resample_audio(const int16_t* input, size_t input_samples,
                           int16_t* output, size_t* output_samples,
                           double ratio, double* pos) {
    if (ratio <= 0) ratio = 1.0;
    size_t out_idx = 0;
    double src_pos = *pos;
    
    while (src_pos < (double)(input_samples - 1) && out_idx < *output_samples) {
        int idx0 = (int)src_pos;
        int idx1 = idx0 + 1;
        if (idx1 >= (int)input_samples) idx1 = idx0;
        double frac = src_pos - (double)idx0;
        output[out_idx * 2] = (int16_t)(input[idx0 * 2] * (1.0 - frac) + input[idx1 * 2] * frac);
        output[out_idx * 2 + 1] = (int16_t)(input[idx0 * 2 + 1] * (1.0 - frac) + input[idx1 * 2 + 1] * frac);
        src_pos += ratio;
        out_idx++;
    }
    *pos = src_pos - (double)input_samples;
    *output_samples = out_idx;
}

/* Called from media.c (frontend thread in the libretro build, the SDL audio
 * thread in the desktop build) to queue resampled samples. */
void sdl_audio_queue_samples(const int16_t* samples, size_t count) {
    if (!samples || count == 0) return;
    audio_buffer_init();

    size_t frames = count / 2;

    if (g_audio_resample_ratio != 1.0 && g_audio_resample_ratio > 0.0) {
        /* v34.27: static scratch instead of malloc/free per call. */
        static int16_t resampled[AUDIO_BUFFER_SIZE * 2];
        size_t max_output = (size_t)((double)frames / g_audio_resample_ratio) + 16;
        if (max_output > AUDIO_BUFFER_SIZE) max_output = AUDIO_BUFFER_SIZE;
        size_t output_count = max_output;
        resample_audio(samples, frames, resampled, &output_count,
                       g_audio_resample_ratio, &g_audio_resample_pos);
        samples = resampled;
        frames = output_count;
    }

    for (size_t i = 0; i < frames; i++) {
        if (g_audio_count >= AUDIO_BUFFER_SIZE) {
            /* Full: drop the oldest samples (bounded latency beats blocking) */
            g_audio_read_pos = (g_audio_read_pos + 1) % AUDIO_BUFFER_SIZE;
            g_audio_count--;
        }
        g_audio_buffer[g_audio_write_pos * 2] = samples[i * 2];
        g_audio_buffer[g_audio_write_pos * 2 + 1] = samples[i * 2 + 1];
        g_audio_write_pos = (g_audio_write_pos + 1) % AUDIO_BUFFER_SIZE;
        g_audio_count++;
    }
}

size_t sdl_audio_get_queued_size(void) {
    return g_audio_count;
}

/* v34.94: libretro has no SDL queue device - the sync path feeds the
 * frontend directly, so report "alive" to keep the media thread idle-safe
 * (it is never created in libretro mode anyway). */
int sdl_audio_device_alive(void) {
    return 1;
}

/* Frontend thread, once per retro_run: mix and deliver exactly the audio the
 * elapsed wall time calls for. All mixing happens synchronously here - no
 * background thread, no queue handoff, no drift. */
void libretro_process_audio(void) {
    extern retro_audio_sample_batch_t audio_batch_cb;
    if (!audio_batch_cb) return;

    audio_buffer_init();

    /* v41 PERF-DIAG: NOJME_AUDIO_STALL_MS=<n> — artificially stall the
     * audio generation (emulates a slow device where media mixing eats
     * most of the frame; GC-safepoint-timeout experiments). */
    {
        static int s_stall_ms = -1;
        if (s_stall_ms < 0) {
            const char* e = getenv("NOJME_AUDIO_STALL_MS");
            s_stall_ms = (e && atoi(e) > 0) ? atoi(e) : 0;
        }
        if (s_stall_ms > 0) {
            struct timespec rq = { .tv_sec = s_stall_ms / 1000,
                                   .tv_nsec = (long)(s_stall_ms % 1000) * 1000000L };
            nanosleep(&rq, NULL);
        }
    }

    /* TIME-BASED SYNCHRONIZATION - how many frontend-rate samples does the
     * elapsed time since the previous frame call for? */
    uint64_t current_time = audio_get_time_us();
    uint64_t elapsed_us = current_time - g_last_frame_time;
    g_last_frame_time = current_time;

    /* Clamp elapsed time to reasonable range (1ms - 100ms) */
    if (elapsed_us < 1000) elapsed_us = 1000;
    if (elapsed_us > 100000) elapsed_us = 100000;

    double samples_needed = (double)g_frontend_sample_rate * (double)elapsed_us / 1000000.0;
    g_audio_time_accumulator += samples_needed;

    /* v34.73 DIAG: NOJME_AUDDBG=1 — one-shot trace of the synchronous audio
     * pump's state (callback registered? elapsed time? sample demand?). */
    {
        static int s_adbg = -1;
        if (s_adbg < 0) {
            const char* e = getenv("NOJME_AUDDBG");
            s_adbg = (e && e[0] == '1') ? 1 : 0;
            if (s_adbg)
                fprintf(stderr, "[AUDDBG] proc_audio: cb=%p rate=%u elapsed=%lluus need=%.1f\n",
                        (void*)(size_t)(audio_batch_cb ? 1 : 0), g_frontend_sample_rate,
                        (unsigned long long)elapsed_us, samples_needed);
        }
    }

    int samples_to_send = (int)g_audio_time_accumulator;
    if (samples_to_send < 1) return;
    g_audio_time_accumulator -= (double)samples_to_send;

    /* Clamp to reasonable range */
    if (samples_to_send > 2048) samples_to_send = 2048;

    /* v34.27: generate synchronously - mix at the 44100 source rate exactly
     * the amount that resamples into samples_to_send (+2 for the fractional
     * resampler position). sdl_audio_queue_samples() resamples into the
     * queue as it enqueues. */
    double ratio = (g_audio_resample_ratio > 0.0) ? g_audio_resample_ratio : 1.0;
    int src_samples = (int)((double)samples_to_send * ratio) + 2;
    if (src_samples > AUDIO_BUFFER_SIZE) src_samples = AUDIO_BUFFER_SIZE;
    media_generate_audio_samples(src_samples);

    /* Pull from the (already resampled) queue. */
    int avail = (int)g_audio_count;
    if (avail <= 0) return;
    if (avail > samples_to_send) avail = samples_to_send;

    int samples_sent = 0;
    while (samples_sent < avail) {
        int remaining = avail - samples_sent;
        int chunk = remaining > 512 ? 512 : remaining;

        size_t contiguous = AUDIO_BUFFER_SIZE - g_audio_read_pos;
        if (contiguous > (size_t)chunk) contiguous = (size_t)chunk;
        if (contiguous > g_audio_count) contiguous = g_audio_count;
        if (contiguous == 0) break;

        audio_batch_cb(&g_audio_buffer[g_audio_read_pos * 2], contiguous);
        g_audio_read_pos = (g_audio_read_pos + contiguous) % AUDIO_BUFFER_SIZE;
        g_audio_count -= contiguous;
        samples_sent += contiguous;
    }
}

/* ============================================
 * Double Buffering - J2ME renders to ONE buffer
 * ============================================ */

static SdlContext g_libretro_context = {0};

/* Double buffer: J2ME always renders to buffer[0], we copy to buffer[1] for display */
#define BUFFER_COUNT 2
static uint32_t* g_buffers[BUFFER_COUNT] = {NULL, NULL};
static atomic_bool g_frame_ready = false;
static atomic_bool g_copying = false;

static int g_buffer_width = 0;
static int g_buffer_height = 0;
static size_t g_buffer_size = 0;

static bool init_buffers(int width, int height) {
    if (width <= 0 || height <= 0) {
        LOG_SAFE("[J2ME] Invalid buffer size: %dx%d\n", width, height);
        return false;
    }

    g_buffer_width = width;
    g_buffer_height = height;
    g_buffer_size = (size_t)width * (size_t)height * sizeof(uint32_t);

    for (int i = 0; i < BUFFER_COUNT; i++) {
        g_buffers[i] = (uint32_t*)malloc(g_buffer_size);
        if (!g_buffers[i]) {
            for (int j = 0; j < i; j++) {
                free(g_buffers[j]);
                g_buffers[j] = NULL;
            }
            LOG_SAFE("[J2ME] Failed to allocate buffer %d\n", i);
            return false;
        }
        memset(g_buffers[i], 0, g_buffer_size);
    }

    atomic_store(&g_frame_ready, false);
    atomic_store(&g_copying, false);

    /* J2ME always renders to buffer[0] */
    g_libretro_context.framebuffer = g_buffers[0];

    LOG_SAFE("[J2ME] Double buffer initialized: %dx%d\n", width, height);
    return true;
}

/* Resize buffers - called when resolution changes at runtime */
bool libretro_resize_buffers(int width, int height) {
    if (width <= 0 || height <= 0) {
        LOG_SAFE("[J2ME] Invalid resize dimensions: %dx%d\n", width, height);
        return false;
    }

    /* Check if size actually changed */
    if (width == g_buffer_width && height == g_buffer_height) {
        return true;  /* No change needed */
    }

    LOG_SAFE("[J2ME] Resizing buffers from %dx%d to %dx%d\n",
            g_buffer_width, g_buffer_height, width, height);

    /* Free old buffers */
    for (int i = 0; i < BUFFER_COUNT; i++) {
        if (g_buffers[i]) {
            free(g_buffers[i]);
            g_buffers[i] = NULL;
        }
    }

    /* Allocate new buffers */
    return init_buffers(width, height);
}

/* Called at START of frame - nothing to do */
void libretro_begin_frame(void) {
    /* J2ME renders incrementally, don't clear */
}

/* Called at END of frame - copy the latest SETTLED frame to the display
 * buffer.
 * v29 FLICKER FIX: only present SETTLED canvas states (a MIDlet frame is
 * mid-flight while paint() is executing or a Graphics3D bind->release cycle
 * is open) - implemented then via a UI trylock + mid-frame gates.
 * v34.61 (Asphalt 3 3D "rarely delivers 3D frames"): the gate design itself
 * was the starvation. A 3D paint longer than one frontend frame (15-40ms on
 * ARMv7 vs a 16.6ms retro_run period) held the UI pump lock nearly
 * full-time; the trylock failed and the display kept stale content while
 * the game ran on ("the game works fast but rarely delivers 3D frames";
 * partially filled frames leaked through the remaining gate windows, e.g.
 * GameCanvas flushGraphics which takes no lock and sets no flags).
 * FIX - KVM-authentic scanout semantics, like a real display controller:
 * present the latest SETTLED-frame snapshot (display.c keeps it; snapshots
 * exist only after a COMPLETE frame - torn/partial output is impossible by
 * construction), and NEVER consult the UI lock or the mid-frame gates.
 * The vblank (vsync) tick moved to AFTER the copy so a woken game thread
 * can neither tear nor veto the frame it just produced. */
void libretro_end_frame(void) {
    if (!g_buffers[0] || !g_buffers[1]) return;

    /* Wait if previous copy is still being read */
    while (atomic_load(&g_copying)) {
        /* Busy wait - should be very short */
    }

    {
        extern int midp_canvas_mid_frame(void);
        extern int midp_ui_trylock_external(void);
        extern void midp_ui_unlock_external(void);
        extern uint32_t midp_present_stable_copy(uint32_t* dst, int dst_px);

        /* 1. Present the latest settled frame (scanout). */
        atomic_store(&g_copying, true);
        atomic_thread_fence(memory_order_release);
        uint32_t stable_seq = midp_present_stable_copy(
                g_buffers[1], g_buffer_width * g_buffer_height);
        if (stable_seq != 0) {
            atomic_store(&g_frame_ready, true);
        } else {
            /* Bootstrap: no frame has settled yet (nothing completed a
             * pump before this retro_run). Legacy gated direct copy -
             * identical to the pre-v34.61 behavior. */
            int ui_held = (midp_ui_trylock_external() == 0);
            if (ui_held) {
                if (!midp_canvas_mid_frame()) {
                    memcpy(g_buffers[1], g_buffers[0], g_buffer_size);
                    atomic_store(&g_frame_ready, true);
                }
                midp_ui_unlock_external();
            }
        }
        atomic_thread_fence(memory_order_release);
        atomic_store(&g_copying, false);

        /* 2. v34.61 STALL-DIAG: NOJME_STALL_DIAG=1 counts consecutive
         * frontend frames during which NO new settled frame appeared
         * (unchanged stable seq) - the true "frozen picture while the game
         * runs" detector. (The old counter measured trylock skips, a
         * condition that no longer exists.) */
        {
            static int sd_on = -1;
            if (sd_on < 0) {
                const char* e = getenv("NOJME_STALL_DIAG");
                sd_on = (e && e[0] && e[0] != '0') ? 1 : 0;
            }
            if (sd_on) {
                static volatile uint32_t s_last_seq = 0;
                static volatile int s_stall_streak = 0;
                static volatile int s_stall_total = 0;
                if (stable_seq == s_last_seq) {
                    s_stall_streak++;
                    s_stall_total++;
                    if (s_stall_streak == 10) {
                        char line[128];
                        int ln = snprintf(line, sizeof(line),
                                "[STALL-DIAG] no new settled frame for %d frames in a row (total %d)\n",
                                s_stall_streak, s_stall_total);
                        if (ln > 0) fwrite(line, 1, (size_t)ln, stderr);
                    } else if (s_stall_streak > 10 && (s_stall_streak % 30) == 0) {
                        char line[128];
                        int ln = snprintf(line, sizeof(line),
                                "[STALL-DIAG] no new settled frame for %d frames in a row (total %d)\n",
                                s_stall_streak, s_stall_total);
                        if (ln > 0) fwrite(line, 1, (size_t)ln, stderr);
                    }
                    /* v34.61 DIAG: dump all VM thread stacks at streak
                     * milestones (60 / 300 frames) - shows WHERE the game
                     * thread is while no frames settle. */
                    if (s_stall_streak == 60 || s_stall_streak == 300) {
                        extern void jvm_dump_all_threads(void);
                        jvm_dump_all_threads();
                    }
                } else {
                    if (s_stall_streak >= 10) {
                        char line[128];
                        int ln = snprintf(line, sizeof(line),
                                "[STALL-DIAG] settled frames resumed after %d stalled frames (total %d)\n",
                                s_stall_streak, s_stall_total);
                        if (ln > 0) fwrite(line, 1, (size_t)ln, stderr);
                    }
                    s_stall_streak = 0;
                    s_last_seq = stable_seq;
                }
            }
        }

        /* 3. v50 (Asphalt image freeze): end-of-frame vsync tick - game-thread
         * serviceRepaints() waits on this to pace itself to the frontend
         * frame rate (KVM "block until vblank" semantics). Bumped ALWAYS
         * (presented or skipped) so a paced game thread never hangs; v34.61:
         * bumped AFTER the presentation copy above, so a woken game thread
         * can neither tear nor veto the frame it just produced. */
        {
            extern void midp_vsync_tick(void);
            midp_vsync_tick();
        }
    }
}

/* Get buffer for display - always return buffer[1] */
bool libretro_get_display_buffer(uint32_t** buffer, int* width, int* height) {
    if (!g_buffers[1]) {
        *buffer = libretro_get_framebuffer();
        *width = g_libretro_context.width > 0 ? g_libretro_context.width : 240;
        *height = g_libretro_context.height > 0 ? g_libretro_context.height : 320;
        return true;
    }
    
    /* Wait for copy to complete if in progress */
    while (atomic_load(&g_copying)) {
        /* Busy wait */
    }
    
    atomic_thread_fence(memory_order_acquire);
    
    /* Always return display buffer[1] */
    *buffer = g_buffers[1];
    *width = g_buffer_width > 0 ? g_buffer_width : 240;
    *height = g_buffer_height > 0 ? g_buffer_height : 320;
    
    return true;
}

SdlContext* sdl_get_global_context(void) { return &g_libretro_context; }

void sdl_set_global_context(SdlContext* ctx) {
    if (ctx) {
        g_libretro_context = *ctx;
    } else {
        memset(&g_libretro_context, 0, sizeof(SdlContext));
    }
}

int sdl_init(JVM* jvm, int width, int height, int scale, bool headless) {
    (void)headless; (void)scale;
    
    if (width <= 0 || height <= 0) {
        LOG_SAFE("[J2ME] Invalid dimensions %dx%d, using defaults\n", width, height);
        width = 240;
        height = 320;
    }
    
    g_libretro_context.jvm = jvm;
    g_libretro_context.width = width;
    g_libretro_context.height = height;
    g_libretro_context.scale = 1;
    g_libretro_context.target_fps = 30;
    g_libretro_context.running = true;
    
    if (!init_buffers(width, height)) {
        uint32_t* fb = (uint32_t*)libretro_get_framebuffer();
        if (fb) {
            g_libretro_context.framebuffer = fb;
            LOG_SAFE("[J2ME] Using single buffer fallback\n");
        } else {
            LOG_SAFE("[J2ME] ERROR: No framebuffer available!\n");
        }
    }
    /* framebuffer already set to g_buffers[0] in init_buffers */
    
    /* v34.27: no audio thread anymore - libretro_process_audio() mixes
     * synchronously once per frame. */
    audio_buffer_init();

    return 0;
}

void sdl_destroy(SdlContext* ctx) {
    (void)ctx;

    for (int i = 0; i < BUFFER_COUNT; i++) {
        if (g_buffers[i]) free(g_buffers[i]);
        g_buffers[i] = NULL;
    }
    g_buffer_size = 0;

    g_audio_initialized = false;
    
    LOG_SAFE("[J2ME] Destroyed\n");
}

void sdl_run(SdlContext* ctx) { (void)ctx; }
void sdl_set_fullscreen(SdlContext* ctx, bool fullscreen) { (void)ctx; (void)fullscreen; }
uint64_t sdl_get_ticks(SdlContext* ctx) { (void)ctx; return 0; }
void sdl_delay(uint32_t ms) { (void)ms; }

/* Update screen - called multiple times during frame, DON'T swap here! */
void sdl_update_screen(SdlContext* ctx) {
    /* Do nothing here - we swap only in libretro_end_frame */
    (void)ctx;
}

void sdl_handle_events(SdlContext* ctx) { (void)ctx; }

bool sdl_key_pressed(SdlContext* ctx, int key) {
    (void)ctx;
    extern int libretro_get_key_states(void);
    int keys = libretro_get_key_states();
    
    switch (key) {
        case 1:  return (keys & (1 << 1)) != 0;
        case 6:  return (keys & (1 << 6)) != 0;
        case 2:  return (keys & (1 << 2)) != 0;
        case 5:  return (keys & (1 << 5)) != 0;
        case 8:  return (keys & (1 << 8)) != 0;
        case 9:  return (keys & (1 << 9)) != 0;
        case 10: return (keys & (1 << 10)) != 0;
        case 11: return (keys & (1 << 11)) != 0;
        case 12: return (keys & (1 << 12)) != 0;
        default: return false;
    }
}

MidpGraphics* sdl_get_graphics(SdlContext* ctx) { (void)ctx; return NULL; }
int sdl_audio_init_simple(uint32_t sample_rate) { (void)sample_rate; audio_buffer_init(); return 0; }

/* v36.16 FIX: media_shutdown_full() (v36.13 bounded exit path) calls
 * sdl_audio_close(NULL); media.c is shared with the libretro core, but the
 * core had NO twin — the link failed with "undefined reference to
 * sdl_audio_close" (--no-undefined in LDFLAGS; broke `make` for the
 * Windows and linux shared-core builds). Libretro twin is a no-op: the
 * audio device belongs to the frontend (audio_batch_cb), nothing to close. */
void sdl_audio_close(SdlContext* ctx) { (void)ctx; }

void sdl_audio_shutdown(void) {
    g_audio_initialized = false;
    g_audio_write_pos = 0;
    g_audio_read_pos = 0;
    g_audio_count = 0;
}

/* JAR resource loading
 * v19: rewritten on top of the single canonical miniz-based reader
 * (src/utils/jar_reader.c). The old hand-rolled central-directory scan was
 * one of FOUR divergent copies in the tree; on at least one Windows build it
 * returned NULL for every resource (classes still loaded via the jvm.c copy)
 * which left NEscube with a textureless black cube and NPE storms in
 * getResourceAsStream. miniz handles data descriptors, ZIP64, SFX prefixes
 * and repacked entry layouts that the old code did not.
 */
uint8_t* load_jar_resource(const char* path, size_t* size) {
    if (size) *size = 0;
    if (!path) return NULL;

    /* Prefer the libretro.c global JVM; fall back to the context copy set
     * by sdl_init() (same instance, different storage). */
    JVM* jvm = g_jvm;
    if (!jvm) {
        jvm = g_libretro_context.jvm;
    }
    if (!jvm || !jvm->class_loader.jar_data || jvm->class_loader.jar_size == 0) {
        /* Class loading uses the same storage — if we get here with a NULL
         * jar the whole VM would be non-functional anyway. Log it once so a
         * future log pinpoints the exact failure instead of a silent NULL. */
        static int logged = 0;
        if (!logged) {
            logged = 1;
            fprintf(stderr,
                    "[JAR-READ] FAILED (no-jar): '%s' (g_jvm=%p ctx_jvm=%p jar=%p size=%zu)\n",
                    path, (void*)g_jvm, (void*)g_libretro_context.jvm,
                    (jvm && jvm->class_loader.jar_data) ? jvm->class_loader.jar_data : NULL,
                    (jvm && jvm->class_loader.jar_size) ? (size_t)jvm->class_loader.jar_size : (size_t)0);
        }
        return NULL;
    }

    return jar_read_file(jvm->class_loader.jar_data, jvm->class_loader.jar_size,
                         path, size);
}

void sdl_present(SdlContext* ctx) { (void)ctx; }

/* Stubs */
void sdl_request_redraw(void) { g_libretro_context.needs_redraw = true; }
bool sdl_needs_redraw(SdlContext* ctx) { return ctx ? ctx->needs_redraw : false; }
void sdl_clear_redraw(SdlContext* ctx) { if (ctx) ctx->needs_redraw = false; }
void sdl_process_events_minimal(void) {}
void sdl_update_texture(SdlContext* ctx) { (void)ctx; }

void sdl_clear(SdlContext* ctx, uint32_t color) {
    if (!ctx || !ctx->framebuffer) return;
    int size = ctx->width * ctx->height;
    for (int i = 0; i < size; i++) ctx->framebuffer[i] = color;
}

int sdl_resize(SdlContext* ctx, int width, int height) { (void)ctx; (void)width; (void)height; return 0; }
int sdl_screenshot(SdlContext* ctx, const char* filename) { (void)ctx; (void)filename; return -1; }
void sdl_frame_end(SdlContext* ctx) { (void)ctx; }
void sdl_sleep(uint32_t ms) { (void)ms; }
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
    (void)ctx; (void)frequency; (void)channels; (void)samples; audio_buffer_init(); return 0;
}
int sdl_audio_queue(SdlContext* ctx, const void* data, size_t length) { (void)ctx; (void)data; (void)length; return 0; }
uint32_t sdl_audio_queued_size(SdlContext* ctx) { (void)ctx; return (uint32_t)g_audio_count; }
void sdl_audio_clear(SdlContext* ctx) { (void)ctx; g_audio_read_pos = 0; g_audio_write_pos = 0; g_audio_count = 0; }
void sdl_process_events(SdlContext* ctx) { (void)ctx; }
int sdl_key_to_midp(int sdl_key) { (void)sdl_key; return 0; }
int sdl_key_to_game_action(int sdl_key) { (void)sdl_key; return 0; }
void sdl_get_pointer(SdlContext* ctx, int* x, int* y) { if (x) *x = 0; if (y) *y = 0; (void)ctx; }
bool sdl_pointer_pressed(SdlContext* ctx) { (void)ctx; return false; }
void sdl_dump_info(SdlContext* ctx) { (void)ctx; }

/* g_j2me_runtime_debug defined in jvm/debug_var.c */
void sdl_save_framebuffer_to_file(SdlContext* ctx, const char* filename) { (void)ctx; (void)filename; }

/* ============================================
 * Error Display Implementation for Libretro
 * ============================================ */

#include "../midp/bitmap_font.h"

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
    } else {
        g_error_title[0] = '\0';
    }
    if (message) {
        strncpy(g_error_message, message, sizeof(g_error_message) - 1);
        g_error_message[sizeof(g_error_message) - 1] = '\0';
    } else {
        g_error_message[0] = '\0';
    }
    if (stack_trace) {
        strncpy(g_error_stack, stack_trace, sizeof(g_error_stack) - 1);
        g_error_stack[sizeof(g_error_stack) - 1] = '\0';
    } else {
        g_error_stack[0] = '\0';
    }
    
    /* Detailed log output */
    LOG_SAFE("\n========================================\n");
    LOG_SAFE("  [J2ME UNCAUGHT EXCEPTION]\n");
    LOG_SAFE("========================================\n");
    LOG_SAFE("  Exception: %s\n", g_error_title);
    if (g_error_message[0]) {
        LOG_SAFE("  Message:   %s\n", g_error_message);
    }
    if (g_error_extra[0]) {
        LOG_SAFE("  Details:   %s\n", g_error_extra);
    }
    if (g_error_stack[0]) {
        LOG_SAFE("  Stack:\n%s", g_error_stack);
    }
    LOG_SAFE("========================================\n\n");
}

/* Set extra detail info (thread name, PC, etc.) */
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

/* Draw a single character on framebuffer */
__attribute__((unused))
static void draw_char(uint32_t* fb, int fb_width, int fb_height, int x, int y, char c, uint32_t color) {
    if (x < 0 || y < 0 || x + FONT_WIDTH >= fb_width || y + FONT_HEIGHT >= fb_height) return;
    
    const uint8_t* char_data = get_char_data(c);
    for (int row = 0; row < FONT_HEIGHT; row++) {
        uint8_t row_data = char_data[row];
        for (int col = 0; col < FONT_WIDTH; col++) {
            if (row_data & (1 << (4 - col))) {
                fb[(y + row) * fb_width + (x + col)] = color;
            }
        }
    }
}

/* Draw a UTF-8 string on framebuffer with word wrap */
static void draw_string(uint32_t* fb, int fb_width, int fb_height, int* x, int* y, const char* str, uint32_t color, int max_width) {
    if (!str || !fb) return;
    
    int len = 0;
    while (str[len]) len++;
    
    int i = 0;
    int start_x = *x;
    int char_width = FONT_WIDTH + 1;
    
    while (i < len) {
        int cp = utf8_decode(str, &i, len);
        if (cp < 0) continue;
        
        /* Handle newlines */
        if (cp == '\n') {
            *x = start_x;
            *y += FONT_HEIGHT + 2;
            continue;
        }
        
        /* Word wrap check */
        if (*x + char_width >= start_x + max_width) {
            *x = start_x;
            *y += FONT_HEIGHT + 2;
        }
        
        /* Check if we're still within bounds */
        if (*y + FONT_HEIGHT >= fb_height - 10) break;
        
        /* Draw the character */
        const uint8_t* char_data = get_char_data_unicode(cp);
        for (int row = 0; row < FONT_HEIGHT; row++) {
            uint8_t row_data = char_data[row];
            for (int col = 0; col < FONT_WIDTH; col++) {
                if (row_data & (1 << (4 - col))) {
                    int px = *x + col;
                    int py = *y + row;
                    if (px >= 0 && px < fb_width && py >= 0 && py < fb_height) {
                        fb[py * fb_width + px] = color;
                    }
                }
            }
        }
        
        *x += char_width;
    }
    
    *y += FONT_HEIGHT + 2;
}

/* Draw horizontal line */
static void draw_hline(uint32_t* fb, int fb_width, int y, int x1, int x2, uint32_t color) {
    if (y < 0) return;
    for (int x = x1; x < x2 && x < fb_width; x++) {
        if (x >= 0) fb[y * fb_width + x] = color;
    }
}

void sdl_draw_error_screen(SdlContext* ctx) {
    if (!ctx || !ctx->framebuffer) return;
    
    int width = ctx->width > 0 ? ctx->width : 240;
    int height = ctx->height > 0 ? ctx->height : 320;
    uint32_t* fb = ctx->framebuffer;
    
    /* Colors: ARGB format */
    uint32_t bg_color = 0xFF1A1A2E;    /* Dark blue background */
    uint32_t header_bg = 0xFF16213E;   /* Slightly lighter header */
    uint32_t border_color = 0xFFE94560; /* Red-pink accent */
    uint32_t title_color = 0xFFFFFFFF;  (void)title_color; /* White title */
    uint32_t exc_color = 0xFFFF6B6B;    /* Red for exception name */
    uint32_t msg_color = 0xFFFFFF00;    /* Yellow text for message */
    uint32_t detail_color = 0xFF87CEEB; /* Light blue for details */
    uint32_t stack_color = 0xFFCCCCCC;  /* Light gray stack trace */
    uint32_t throw_color = 0xFFFF6B6B;  /* Red for throwing frame */
    uint32_t help_color = 0xFF888888;   /* Dim help text */
    
    /* Fill background */
    for (int i = 0; i < width * height; i++) {
        fb[i] = bg_color;
    }
    
    /* Draw header background (top 12px) */
    for (int hy = 0; hy < 14; hy++) {
        draw_hline(fb, width, hy, 0, width, header_bg);
    }
    
    /* Draw top/bottom border lines */
    draw_hline(fb, width, 0, 0, width, border_color);
    draw_hline(fb, width, 13, 0, width, border_color);
    draw_hline(fb, width, height - 1, 0, width, border_color);
    
    int y_pos = 3;
    int x_pos = 5;
    
    /* Draw header: "!! EXCEPTION !!" */
    draw_string(fb, width, height, &x_pos, &y_pos, "!! EXCEPTION !!", exc_color, width - 10);
    
    y_pos += 4;
    
    /* Draw exception class name (prominently) */
    if (g_error_title[0]) {
        x_pos = 5;
        draw_string(fb, width, height, &x_pos, &y_pos, g_error_title, exc_color, width - 10);
        y_pos += 2;
    }
    
    /* Separator */
    draw_hline(fb, width, y_pos, 2, width - 2, 0xFF333355);
    y_pos += 4;
    
    /* Draw message */
    if (g_error_message[0]) {
        x_pos = 5;
        /* Draw "Message:" label */
        draw_string(fb, width, height, &x_pos, &y_pos, "Msg:", detail_color, width - 10);
        x_pos = 5;
        draw_string(fb, width, height, &x_pos, &y_pos, g_error_message, msg_color, width - 10);
        y_pos += 2;
    }
    
    /* Draw extra details (thread name, etc.) */
    if (g_error_extra[0]) {
        x_pos = 5;
        draw_string(fb, width, height, &x_pos, &y_pos, g_error_extra, detail_color, width - 10);
        y_pos += 2;
    }
    
    /* Separator before stack trace */
    draw_hline(fb, width, y_pos, 2, width - 2, 0xFF333355);
    y_pos += 4;
    
    /* Draw stack trace with throwing frame highlighted */
    if (g_error_stack[0]) {
        x_pos = 5;
        /* Count lines in stack trace to decide layout */
        int stack_lines = 0;
        const char* p = g_error_stack;
        while (*p) {
            if (*p == '\n') stack_lines++;
            p++;
        }
        if (p > g_error_stack) stack_lines++;
        
        /* Draw each line of stack trace */
        /* First line (throwing frame) in red, rest in gray */
        char stack_copy[8192];
        strncpy(stack_copy, g_error_stack, sizeof(stack_copy) - 1);
        stack_copy[sizeof(stack_copy) - 1] = '\0';
        
        char* line = strtok(stack_copy, "\n");
        int line_num = 0;
        while (line && y_pos + FONT_HEIGHT < height - 18) {
            x_pos = 5;
            uint32_t line_color = (line_num == 0) ? throw_color : stack_color;
            draw_string(fb, width, height, &x_pos, &y_pos, line, line_color, width - 10);
            line = strtok(NULL, "\n");
            line_num++;
        }
        
        /* If stack was truncated, show count */
        if (line != NULL && y_pos + FONT_HEIGHT < height - 18) {
            x_pos = 5;
            char more_buf[64];
            snprintf(more_buf, sizeof(more_buf), "  ... +%d more frames", stack_lines - line_num);
            draw_string(fb, width, height, &x_pos, &y_pos, more_buf, help_color, width - 10);
        }
    }
    
    /* Draw help text at bottom */
    y_pos = height - 15;
    x_pos = 5;
    draw_string(fb, width, height, &x_pos, &y_pos, "SELECT=exit", help_color, width - 10);

    /* v34.61: the error screen is drawn straight into the framebuffer by
     * the frontend thread; make it the settled frame so the scanout
     * presenter (which no longer reads the live framebuffer) shows it. */
    {
        extern void midp_present_settled_snapshot(void);
        midp_present_settled_snapshot();
    }
}

/* ============================================
 * v34.72: "MIDlet finished" screen for the libretro core.
 *
 * After MIDlet.notifyDestroyed() (or System.exit) the VM stops, but the
 * frontend keeps calling retro_run() forever — the last game frame used
 * to stay on screen indefinitely and was indistinguishable from a hang
 * (user report: "it is not clear the MIDlet has ended, it just looks
 * frozen"). Draw an unmistakable message on a BLACK background instead.
 * ============================================ */

static int finished_text_width_px(const char* s) {
    /* UTF-8 codepoint count * (FONT_WIDTH + 1) - 1 */
    int n = 0, i = 0, len = 0;
    if (!s) return 0;
    while (s[len]) len++;
    while (i < len) {
        int cp = utf8_decode(s, &i, len);
        if (cp < 0) break;
        n++;
    }
    return n > 0 ? n * (FONT_WIDTH + 1) - 1 : 0;
}

void sdl_draw_midlet_finished_screen(SdlContext* ctx) {
    if (!ctx || !ctx->framebuffer) return;

    int width  = ctx->width  > 0 ? ctx->width  : 240;
    int height = ctx->height > 0 ? ctx->height : 320;
    uint32_t* fb = ctx->framebuffer;

    /* Black background — visually distinct from every game screen and
     * from the dark-red/dark-blue error screens. */
    for (int i = 0; i < width * height; i++) {
        fb[i] = 0xFF000000;
    }

    const uint32_t title_color  = 0xFFFFFFFF;  /* white */
    const uint32_t sub_color    = 0xFFA8A8A8;  /* light gray */
    const uint32_t dim_color    = 0xFF707070;  /* dim gray */
    const uint32_t frame_color  = 0xFF303030;  /* thin frame */

    /* Thin frame around the screen so a black frame is clearly intentional
     * even on OLED frontends where "black" could read as "off". */
    draw_hline(fb, width, 0, 0, width, frame_color);
    draw_hline(fb, width, height - 1, 0, width, frame_color);
    for (int y = 1; y < height - 1; y++) {
        fb[y * width] = frame_color;
        fb[y * width + width - 1] = frame_color;
    }

    /* Centered message block */
    int center_y = (height / 2) - 24;

    const char* title = "MIDlet finished";
    {
        int x_pos = (width - finished_text_width_px(title)) / 2;
        if (x_pos < 1) x_pos = 1;
        int y_pos = center_y;
        draw_string(fb, width, height, &x_pos, &y_pos, title, title_color, width - 4);
    }
    {
        const char* sub = "\xD0\x9C\xD0\xB8\xD0\xB4\xD0\xBB\xD0\xB5\xD1\x82 \xD0\xB7\xD0\xB0\xD0\xB2\xD0\xB5\xD1\x80\xD1\x88\xD1\x91\xD0\xBD";  /* "Мидлет завершён" */
        int x_pos = (width - finished_text_width_px(sub)) / 2;
        if (x_pos < 1) x_pos = 1;
        int y_pos = center_y + FONT_HEIGHT + 6;
        draw_string(fb, width, height, &x_pos, &y_pos, sub, sub_color, width - 4);
    }
    /* Small separator */
    {
        int y = center_y + 2 * (FONT_HEIGHT + 6) + 2;
        draw_hline(fb, width, y, width / 4, width - width / 4, frame_color);
    }
    {
        const char* hint = "The application has ended";
        int x_pos = (width - finished_text_width_px(hint)) / 2;
        if (x_pos < 1) x_pos = 1;
        int y_pos = center_y + 3 * (FONT_HEIGHT + 6);
        draw_string(fb, width, height, &x_pos, &y_pos, hint, dim_color, width - 4);
    }

    /* Bottom hint: how to close */
    {
        const char* hint = "SELECT=close";
        int x_pos = (width - finished_text_width_px(hint)) / 2;
        if (x_pos < 1) x_pos = 1;
        int y_pos = height - 15;
        draw_string(fb, width, height, &x_pos, &y_pos, hint, dim_color, width - 4);
    }

    /* Same settled-frame handoff as the error screen (v34.61 scanout). */
    {
        extern void midp_present_settled_snapshot(void);
        midp_present_settled_snapshot();
    }
}

