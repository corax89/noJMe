/*
 * J2ME Emulator - SDL2 Backend
 * SDL2 implementation for graphics, input, and audio
 */

#ifndef SDL_BACKEND_H
#define SDL_BACKEND_H

#include <stdatomic.h>
#include "jvm.h"
#include "midp.h"

/* SDL types - defined here for stub compatibility */
#ifndef SDL_DEFINES
#define SDL_DEFINES
typedef struct SDL_Window SDL_Window;
typedef struct SDL_Renderer SDL_Renderer;
typedef struct SDL_Texture SDL_Texture;
typedef uint32_t SDL_AudioDeviceID;
#endif

/*
 * SDL2 emulator context
 */
typedef struct {
    SDL_Window* window;
    SDL_Renderer* renderer;
    SDL_Texture* texture;
    
    /* Framebuffer */
    uint32_t* framebuffer;
    int width;
    int height;
    int scale;
    
    /* Input state */
    struct {
        bool key_states[256];
        int pointer_x;
        int pointer_y;
        bool pointer_pressed;
    } input;
    
    /* Audio */
    struct {
        SDL_AudioDeviceID device;
        int frequency;
        int channels;
        int samples;
        bool enabled;
    } audio;
    
    /* Timing */
    uint64_t start_time;
    uint64_t frame_time;
    int target_fps;
    bool vsync;
    
    /* State */
    bool running;
    bool minimized;
    bool fullscreen;
    bool headless;       /* Headless mode flag */
    _Atomic bool needs_redraw;  /* ИСПРАВЛЕНО: atomic для настоящей межпоточной синхронизации */
    
    /* JVM reference */
    JVM* jvm;
    
} SdlContext;

/*
 * Initialization
 */
void sdl_set_global_context(SdlContext* ctx);
SdlContext* sdl_get_global_context(void);
int sdl_init(JVM* jvm, int width, int height, int scale, bool headless);
void sdl_destroy(SdlContext* ctx);

/*
 * Main loop
 */
void sdl_run(SdlContext* ctx);
void sdl_stop(SdlContext* ctx);

/*
 * Graphics operations
 */
void sdl_clear(SdlContext* ctx, uint32_t color);
void sdl_present(SdlContext* ctx);
void sdl_update_texture(SdlContext* ctx);

/* ИСПРАВЛЕНО: Запросить перерисовку экрана (thread-safe) */
void sdl_request_redraw(void);

/* Check if redraw is needed */
bool sdl_needs_redraw(SdlContext* ctx);

/* Clear redraw flag */
void sdl_clear_redraw(SdlContext* ctx);

/* Get framebuffer for direct access */
uint32_t* sdl_get_framebuffer(SdlContext* ctx);

/* Resize framebuffer */
int sdl_resize(SdlContext* ctx, int width, int height);

/* Take screenshot */
int sdl_screenshot(SdlContext* ctx, const char* filename);

/* Save framebuffer to PPM file (for headless mode debugging) */
void sdl_save_framebuffer_to_file(SdlContext* ctx, const char* filename);

/*
 * Input handling
 */

/* Process pending events */
void sdl_process_events(SdlContext* ctx);

/* Minimal event processing for cooperative threading */
void sdl_process_events_minimal(void);

/* Get key state */
bool sdl_key_pressed(SdlContext* ctx, int keycode);

/* Map SDL key to MIDP key */
int sdl_key_to_midp(int sdl_key);

/* Map SDL key to game action */
int sdl_key_to_game_action(int sdl_key);

/* Get pointer position */
void sdl_get_pointer(SdlContext* ctx, int* x, int* y);

/* Check pointer pressed */
bool sdl_pointer_pressed(SdlContext* ctx);

/*
 * Audio operations
 */

/* Initialize audio subsystem */
int sdl_audio_init(SdlContext* ctx, int frequency, int channels, int samples);

/* Initialize audio without context (simple mode) */
int sdl_audio_init_simple(uint32_t sample_rate);

/* Queue audio data */
int sdl_audio_queue(SdlContext* ctx, const void* data, size_t length);

/* Get queued audio size */
uint32_t sdl_audio_queued_size(SdlContext* ctx);

/* Clear audio queue */
void sdl_audio_clear(SdlContext* ctx);

/* Shutdown audio */
void sdl_audio_shutdown(void);

/*
 * Timing operations
 */

/* Get elapsed time in milliseconds */
uint64_t sdl_get_ticks(SdlContext* ctx);

/* Frame timing - call at end of frame */
void sdl_frame_end(SdlContext* ctx);

/* Sleep for milliseconds */
void sdl_sleep(uint32_t ms);

/*
 * Window operations
 */

/* Set window title */
void sdl_set_title(SdlContext* ctx, const char* title);

/* Set fullscreen mode */
void sdl_set_fullscreen(SdlContext* ctx, bool fullscreen);

/* Toggle fullscreen */
void sdl_toggle_fullscreen(SdlContext* ctx);

/* Get window size */
void sdl_get_window_size(SdlContext* ctx, int* width, int* height);

/*
 * Platform integration
 */

/* Get MIDP platform callbacks */
MidpPlatformCallbacks* sdl_get_platform_callbacks(SdlContext* ctx);

/* Create graphics from framebuffer */
MidpGraphics* sdl_get_graphics(SdlContext* ctx);

/* Check if the given graphics is the screen graphics */
bool sdl_is_screen_graphics(MidpGraphics* gfx);

/*
 * Key code mappings
 */

/* MIDP key codes */
#define MIDP_KEY_0          48
#define MIDP_KEY_1          49
#define MIDP_KEY_2          50
#define MIDP_KEY_3          51
#define MIDP_KEY_4          52
#define MIDP_KEY_5          53
#define MIDP_KEY_6          54
#define MIDP_KEY_7          55
#define MIDP_KEY_8          56
#define MIDP_KEY_9          57
#define MIDP_KEY_STAR       42
#define MIDP_KEY_POUND      35
#define MIDP_KEY_UP         -1
#define MIDP_KEY_DOWN       -2
#define MIDP_KEY_LEFT       -3
#define MIDP_KEY_RIGHT      -4
#define MIDP_KEY_SELECT     -5
#define MIDP_KEY_SOFT_LEFT  -6
#define MIDP_KEY_SOFT_RIGHT -7
#define MIDP_KEY_CLEAR      -8
#define MIDP_KEY_SEND       -9
#define MIDP_KEY_END        -10

/*
 * Debug utilities
 */
void sdl_dump_info(SdlContext* ctx);

/*
 * Error display - Shows unhandled exception info on screen
 */

/* Set error information for display */
void sdl_set_error_info(const char* title, const char* message, const char* stack_trace);

/* Set extra detail info (thread name, PC, etc.) */
void sdl_set_error_extra(const char* extra);

/* Check if there's an error to display */
bool sdl_has_error(void);

/* Clear error state */
void sdl_clear_error(void);

/* Draw the error screen on framebuffer */
void sdl_draw_error_screen(SdlContext* ctx);

/* v34.72: draw the "MIDlet finished" screen (black background, explicit
 * message) after the VM stopped — a finished MIDlet must not look like a
 * frozen frame. Implemented by every backend (SDL window, libretro stubs,
 * headless). */
void sdl_draw_midlet_finished_screen(SdlContext* ctx);

#endif /* SDL_BACKEND_H */
