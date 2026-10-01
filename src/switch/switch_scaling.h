/*
 * switch_scaling.h — pure (SDL-free) display scaling math + background
 * blur downsample for the Nintendo Switch frontend.
 *
 * Kept free of any SDL/libnx dependency so it is unit-testable on any
 * host (scripts/test_switch_scaling.c) and reusable by any presenter.
 */
#ifndef SWITCH_SCALING_H
#define SWITCH_SCALING_H

#include <stdint.h>

/* Destination rectangle of the game canvas on the Switch screen for the
 * given scaling mode. mode NOJME_SCALE_STRETCH fills the screen (aspect
 * ignored); the two FIT modes letterbox centered. */
void switch_scaling_game_rect(int scale_mode,
                              int fb_w, int fb_h,
                              int screen_w, int screen_h,
                              int* x, int* y, int* w, int* h);

/* Downsample src (fb_w x fb_h ARGB8888) into dst (dst_w x dst_h) with
 * box averaging, then apply `passes` 3x3 box-blur rounds on the small
 * buffer. dst is uploaded as a texture and linearly stretched to the
 * whole screen by the presenter — the classic "blurred background fill"
 * behind a letterboxed game. Cost: O(dst pixels) per frame, tiny. */
void switch_scaling_blur_bg(const uint32_t* src, int src_w, int src_h,
                            uint32_t* dst, int dst_w, int dst_h,
                            int passes);

/* Recommended background buffer dimensions for a game canvas: ~1/8 of the
 * canvas, min 8x8. */
void switch_scaling_bg_size(int fb_w, int fb_h, int* bg_w, int* bg_h);

/* ================= v36.15 per-game screen rotation ================= */

/* Rotate src (w x h ARGB8888, row-major) by 90 degrees into dst (h x w).
 * dir=1: clockwise ("90 right"), dir=2: counter-clockwise ("90 left").
 * Same mapping as the proven libretro rotate_frame_90 (v34.48), bpp fixed
 * to 4. Presentation-only: the canvas is NOT touched; the caller feeds the
 * rotated buffer to the texture/blur/scale2x pipeline with swapped dims
 * and maps touch coordinates back. Pure function, unit-testable. */
void switch_scaling_rotate90(const uint32_t* src, int w, int h,
                             uint32_t* dst, int dir);

/* ================= v36.14 Scale2x ================= */

/* Reference scalar Scale2x (Andrea Mazzoleni's algorithm, the official
 * short form): every source pixel E (with clamped neighbors B=top, D=left,
 * F=right, H=bottom) produces a 2x2 block:
 *   guard = (B != H) && (D != F)
 *   E0 = guard && D == B ? D : E    (top-left)
 *   E1 = guard && B == F ? F : E    (top-right)
 *   E2 = guard && D == H ? D : E    (bottom-left)
 *   E3 = guard && H == F ? H : E    (bottom-right)
 * provably identical to the long-form rules published on scale2x.it.
 * ARGB8888 in/out, dst is (2*w) x (2*h). Always compiled: the unit test
 * diffs it bit-for-bit against the NEON kernel on aarch64. */
void switch_scaling_scale2x_scalar(const uint32_t* src, int w, int h,
                                   uint32_t* dst);

/* Public entry: NEON kernel on ARM64 (the Switch), the scalar reference
 * elsewhere. Same contract as above. Frame-thread-only (a lazily grown
 * scratch, no locking) — the caller is the frontend present path. */
void switch_scaling_scale2x(const uint32_t* src, int w, int h,
                            uint32_t* dst);

/* Backend actually compiled in: "neon" or "scalar" — for the honest
 * game_begin log line (field logs must prove which kernel ran). */
const char* switch_scaling_scale2x_backend(void);

#endif /* SWITCH_SCALING_H */
