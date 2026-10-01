/*
 * switch_glue.h — the seam between the SDL backend (sdl_graphics.c, owns
 * the window/renderer/textures) and the pure menu logic (switch_ui.c).
 *
 * switch_ui.c contains NO SDL calls: it draws into a raw ARGB8888 canvas
 * and consumes abstract key events, which makes the whole menu system
 * compilable and testable on any host (Makefile target `switchui`).
 * sdl_graphics.c implements these functions next to its other
 * __SWITCH__-guarded sections.
 */
#ifndef SWITCH_GLUE_H
#define SWITCH_GLUE_H

#include <stdint.h>

/* ---- Menu key model (bitmask, edge-triggered presses) ---- */
#define SWK_UP    (1u << 0)
#define SWK_DOWN  (1u << 1)
#define SWK_LEFT  (1u << 2)
#define SWK_RIGHT (1u << 3)
#define SWK_A     (1u << 4)  /* выбрать */
#define SWK_B     (1u << 5)  /* назад */
#define SWK_EXIT  (1u << 6)  /* закрыть приложение (SDL_QUIT / «Выход») */
#define SWK_MINUS (1u << 7)  /* v34.90: edge MINUS - switch menu language */

/* Menu canvas the UI draws into (SWITCH_UI_WIDTH x SWITCH_UI_HEIGHT,
 * ARGB8888). Valid until sdl_switch_platform_shutdown(). */
uint32_t* sdl_switch_ui_canvas(void);

/* Pump SDL events and return the menu keys PRESSED this call (edges only).
 * Translates raw joystick buttons (SDL_JOY*, v34.89), keyboard
 * arrows/Enter/Escape and SDL_QUIT. v34.90: also SWK_MINUS on a MINUS
 * press edge, and HELD keys (sdl_switch_ui_held_keys) now reflect the
 * LIVE pad state across calls (not just the pump that saw the event). */
unsigned sdl_switch_ui_pump_keys(void);

/* Keys currently HELD (for auto-repeat in long lists). */
unsigned sdl_switch_ui_held_keys(void);

/* ============ v36.26 [TOUCH-UI]: touch for every UI surface ============ */
/* Sandbox script hook (TAP tokens): inject a press edge at (x, y). */
void sdl_switch_touch_inject(int x, int y);
/* Consume the pending tap edge: 1 exactly once per FINGERDOWN. */
int  sdl_switch_touch_tap(int* x, int* y);
/* Live finger state (drag scrolling): returns touch_down. */
int  sdl_switch_touch_state(int* x, int* y, int* down, int* moved);

/* Upload the menu canvas and flip. */
void sdl_switch_ui_present(void);

/* v36.17: live background of the per-game settings screen — copy the
 * LAST game frame (same single-source as the normal present, rotation
 * and current effective scale applied) into the menu canvas with a
 * CPU nearest-scaler, so the game stays visible behind the translucent
 * settings panel. Returns 1 when a frame was drawn, 0 otherwise
 * (no active game / no framebuffer — caller falls back to the opaque
 * background). Must be called from the frame-thread context (the
 * per-game screen runs from the game pump with the VM parked). */
int sdl_switch_ui_game_frame_bg(uint32_t* dst, int dst_w, int dst_h);

/* Millisecond monotonic clock (SDL_GetPerformanceCounter based). */
uint64_t sdl_switch_ui_ticks_ms(void);

/* Sleep (pumping nothing — menus are event-driven). */
void sdl_switch_ui_wait(uint32_t ms);

/* ---- One-time platform bootstrap (before any game / menu): ----
 * SDL_Init(VIDEO|AUDIO|JOYSTICK|EVENTS), 1280x720 window, renderer,
 * UI texture, up to 4 raw joysticks opened. v34.89: raw SDL_JOY* input
 * replaces the GameController API — SDL_CONTROLLER* events never fire
 * on switch-SDL2 setups without a controller mapping, which left the
 * frontend dead. v34.90: SINGLE-PUMP rule - see switch_input_pump() in
 * sdl_graphics.c; the game thread (Thread.sleep / thread_yield) never
 * drains the SDL event queue on __SWITCH__.
 * Returns 0 on success. Idempotent (second call is a no-op returning 0). */
int sdl_switch_platform_init(void);

/* Final shutdown at app exit (destroy window/renderer, SDL_Quit). */
void sdl_switch_platform_shutdown(void);

/* ---- Game-session lifecycle (called around init_emulator/run_midlet) ----
 * Prepare/teardown per-game textures on the SAME window. sdl_init() on
 * __SWITCH__ delegates here. */
int sdl_switch_game_begin(int fb_w, int fb_h);
void sdl_switch_game_end(void);

#endif /* SWITCH_GLUE_H */
