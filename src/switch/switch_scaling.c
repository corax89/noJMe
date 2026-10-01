/*
 * switch_scaling.c — pure scaling math + blur background for the Switch
 * frontend. No SDL, no libnx: unit-testable everywhere (see
 * scripts/test_switch_scaling.c).
 */
#include "switch/switch_scaling.h"
#include "switch/switch_common.h"

#include <stdlib.h>
#include <string.h>

void switch_scaling_game_rect(int scale_mode,
                              int fb_w, int fb_h,
                              int screen_w, int screen_h,
                              int* x, int* y, int* w, int* h) {
    if (x) *x = 0;
    if (y) *y = 0;
    if (w) *w = screen_w;
    if (h) *h = screen_h;
    if (fb_w <= 0 || fb_h <= 0 || screen_w <= 0 || screen_h <= 0) return;

    if (scale_mode == NOJME_SCALE_STRETCH) return; /* full screen */

    /* FIT (both variants): largest centered rect preserving fb aspect. */
    double s = (double)screen_w / (double)fb_w;
    if ((double)screen_h / (double)fb_h < s) s = (double)screen_h / (double)fb_h;

    int rw = (int)((double)fb_w * s + 0.5);
    int rh = (int)((double)fb_h * s + 0.5);
    if (rw < 1) rw = 1;
    if (rh < 1) rh = 1;
    if (rw > screen_w) rw = screen_w;
    if (rh > screen_h) rh = screen_h;

    if (x) *x = (screen_w - rw) / 2;
    if (y) *y = (screen_h - rh) / 2;
    if (w) *w = rw;
    if (h) *h = rh;
}

void switch_scaling_bg_size(int fb_w, int fb_h, int* bg_w, int* bg_h) {
    int w = fb_w / 8;
    int h = fb_h / 8;
    if (w < 8) w = 8;
    if (h < 8) h = 8;
    if (w > fb_w) w = fb_w;
    if (h > fb_h) h = fb_h;
    if (bg_w) *bg_w = w;
    if (bg_h) *bg_h = h;
}

/* ================= v36.15: 90-degree frame rotation =================
 * Bit-exact port of the libretro rotate_frame_90 (v34.48, bpp=4 path):
 *   dir=1 ("90 right", CW):  dst[x*h + (h-1-y)] = src[y*w + x]
 *   dir=2 ("90 left",  CCW): dst[(w-1-x)*h + y] = src[y*w + x]
 * dst is an (h x w) image (display width = h). Scalar loop: the frame is
 * at most a few hundred thousand pixels for candybar canvases; if that
 * ever shows up in a present-cost trace it can be NEON-ized like Scale2x. */
void switch_scaling_rotate90(const uint32_t* src, int w, int h,
                             uint32_t* dst, int dir) {
    if (!src || !dst || w <= 0 || h <= 0) return;
    if (dir != 1 && dir != 2) return;
    if (dir == 1) {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                dst[(size_t)x * h + (h - 1 - y)] = src[(size_t)y * w + x];
    } else {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                dst[(size_t)(w - 1 - x) * h + y] = src[(size_t)y * w + x];
    }
}

void switch_scaling_blur_bg(const uint32_t* src, int src_w, int src_h,
                            uint32_t* dst, int dst_w, int dst_h,
                            int passes) {
    if (!src || !dst || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0)
        return;

    /* ---- Pass 1: box-average downsample (area averaging) ----
     * For each destination pixel, average the source rectangle it covers.
     * Integer accumulation; alpha forced opaque. */
    for (int dy = 0; dy < dst_h; dy++) {
        int y0 = (int)((int64_t)dy * src_h / dst_h);
        int y1 = (int)((int64_t)(dy + 1) * src_h / dst_h);
        if (y1 <= y0) y1 = y0 + 1;
        if (y1 > src_h) y1 = src_h;
        for (int dx = 0; dx < dst_w; dx++) {
            int x0 = (int)((int64_t)dx * src_w / dst_w);
            int x1 = (int)((int64_t)(dx + 1) * src_w / dst_w);
            if (x1 <= x0) x1 = x0 + 1;
            if (x1 > src_w) x1 = src_w;

            uint32_t rs = 0, gs = 0, bs = 0, n = 0;
            const uint32_t* row = src + (size_t)y0 * src_w;
            for (int sy = y0; sy < y1; sy++, row += src_w) {
                for (int sx = x0; sx < x1; sx++) {
                    uint32_t p = row[sx];
                    rs += (p >> 16) & 0xFF;
                    gs += (p >> 8) & 0xFF;
                    bs += p & 0xFF;
                    n++;
                }
            }
            if (n == 0) n = 1;
            dst[(size_t)dy * dst_w + dx] = 0xFF000000u |
                    ((rs / n) << 16) | ((gs / n) << 8) | (bs / n);
        }
    }

    /* ---- Pass 2: 3x3 box blur rounds (in place, two saved source rows) ----
     * Operating on the tiny dst buffer. Row y-1 must be read in its
     * ORIGINAL (pre-blur) form while dst row y is being overwritten, so
     * the two previous source rows are kept in side buffers; row y+1 is
     * still untouched in dst at the time row y is computed. */
    if (passes <= 0) return;

    uint32_t* prev_orig = (uint32_t*)malloc((size_t)dst_w * sizeof(uint32_t));
    uint32_t* cur_orig = (uint32_t*)malloc((size_t)dst_w * sizeof(uint32_t));
    uint32_t* out = (uint32_t*)malloc((size_t)dst_w * sizeof(uint32_t));
    if (!prev_orig || !cur_orig || !out) {
        free(prev_orig); free(cur_orig); free(out);
        return; /* blur is cosmetic — flat downsample is acceptable */
    }

    for (int p = 0; p < passes; p++) {
        for (int y = 0; y < dst_h; y++) {
            uint32_t* dst_row = dst + (size_t)y * dst_w;
            const uint32_t* row_above = (y > 0) ? prev_orig : NULL;
            const uint32_t* row_below = (y + 1 < dst_h) ? dst + (size_t)(y + 1) * dst_w : NULL;
            memcpy(cur_orig, dst_row, (size_t)dst_w * sizeof(uint32_t));

            for (int x = 0; x < dst_w; x++) {
                uint32_t rs = 0, gs = 0, bs = 0, n = 0;
                for (int kx = -1; kx <= 1; kx++) {
                    int xx = x + kx;
                    if (xx < 0 || xx >= dst_w) continue;
                    for (int ky = -1; ky <= 1; ky++) {
                        uint32_t px;
                        if (ky < 0) {
                            if (!row_above) continue;
                            px = row_above[xx];
                        } else if (ky > 0) {
                            if (!row_below) continue;
                            px = row_below[xx];
                        } else {
                            px = cur_orig[xx];
                        }
                        rs += (px >> 16) & 0xFF;
                        gs += (px >> 8) & 0xFF;
                        bs += px & 0xFF;
                        n++;
                    }
                }
                out[x] = 0xFF000000u |
                        ((rs / n) << 16) | ((gs / n) << 8) | (bs / n);
            }
            memcpy(dst_row, out, (size_t)dst_w * sizeof(uint32_t));
            /* cur_orig becomes the previous original row for y+1 */
            uint32_t* t = prev_orig; prev_orig = cur_orig; cur_orig = t;
        }
    }
    free(prev_orig); free(cur_orig); free(out);
}

/* ================= v36.14 Scale2x (NEON + scalar reference) =================
 *
 * The official Scale2x short form (provably identical to the long-form
 * rules on scale2x.it — each condition pair reduces to the same guard):
 *   guard = (B != H) && (D != F)
 *   E0 = guard && D == B ? D : E ; E1 = guard && B == F ? F : E
 *   E2 = guard && D == H ? D : E ; E3 = guard && H == F ? H : E
 * Border pixels use clamped (replicated) neighbors, like the reference
 * scale2x implementation. 32-bit pixel equality (alpha included; the game
 * framebuffer is opaque ARGB8888). */

void switch_scaling_scale2x_scalar(const uint32_t* src, int w, int h,
                                   uint32_t* dst) {
    if (!src || !dst || w <= 0 || h <= 0) return;
    const size_t dw = (size_t)w * 2;
    for (int y = 0; y < h; y++) {
        const int ym = (y > 0) ? y - 1 : 0;
        const int yp = (y < h - 1) ? y + 1 : h - 1;
        const uint32_t* rb = src + (size_t)ym * w; /* B row (clamped) */
        const uint32_t* re = src + (size_t)y * w;  /* E row */
        const uint32_t* rh = src + (size_t)yp * w; /* H row (clamped) */
        uint32_t* d0 = dst + (size_t)(2 * y) * dw;
        uint32_t* d1 = d0 + dw;
        for (int x = 0; x < w; x++) {
            const int xm = (x > 0) ? x - 1 : 0;
            const int xp = (x < w - 1) ? x + 1 : w - 1;
            const uint32_t B = rb[x], D = re[xm], E = re[x];
            const uint32_t F = re[xp], H = rh[x];
            uint32_t e0 = E, e1 = E, e2 = E, e3 = E;
            if (B != H && D != F) {
                if (D == B) e0 = D;
                if (B == F) e1 = F;
                if (D == H) e2 = D;
                if (H == F) e3 = H;
            }
            d0[2 * x]     = e0;
            d0[2 * x + 1] = e1;
            d1[2 * x]     = e2;
            d1[2 * x + 1] = e3;
        }
    }
}

#if defined(__aarch64__) || defined(__ARM_NEON)

#include <arm_neon.h>

/* Scratch rows with one replicated pad pixel on each side so every vector
 * load (including the F load that reads one pixel past the chunk) stays in
 * bounds. Frame-thread-only: grown lazily, no locking (the only caller is
 * the frontend present path; the unit test drives it single-threaded). */
static uint32_t* s2x_pad_prev = NULL;
static uint32_t* s2x_pad_cur  = NULL;
static uint32_t* s2x_pad_next = NULL;
static int       s2x_pad_cap  = 0; /* capacity in uint32_t (w + 2) */

static void s2x_fill_pad(uint32_t* pad, const uint32_t* row, int w) {
    if (row) memcpy(pad + 1, row, (size_t)w * sizeof(uint32_t));
    pad[0] = pad[1];
    pad[w + 1] = pad[w];
}

static void s2x_scale2x_neon(const uint32_t* src, int w, int h, uint32_t* dst) {
    const size_t dw = (size_t)w * 2;
    for (int y = 0; y < h; y++) {
        const uint32_t* rb = src + (size_t)((y > 0) ? y - 1 : 0) * w;
        const uint32_t* re = src + (size_t)y * w;
        const uint32_t* rh = src + (size_t)((y < h - 1) ? y + 1 : h - 1) * w;
        s2x_fill_pad(s2x_pad_prev, rb, w);
        s2x_fill_pad(s2x_pad_cur, re, w);
        s2x_fill_pad(s2x_pad_next, rh, w);
        uint32_t* d0 = dst + (size_t)(2 * y) * dw;
        uint32_t* d1 = d0 + dw;

        int x = 0;
        for (; x + 4 <= w; x += 4) {
            const uint32x4_t Ev = vld1q_u32(s2x_pad_cur + 1 + x);
            const uint32x4_t Dv = vld1q_u32(s2x_pad_cur + x);
            const uint32x4_t Fv = vld1q_u32(s2x_pad_cur + 2 + x);
            const uint32x4_t Bv = vld1q_u32(s2x_pad_prev + 1 + x);
            const uint32x4_t Hv = vld1q_u32(s2x_pad_next + 1 + x);

            const uint32x4_t guard =
                vandq_u32(vmvnq_u32(vceqq_u32(Bv, Hv)),
                          vmvnq_u32(vceqq_u32(Dv, Fv)));

            const uint32x4_t E0 =
                vbslq_u32(vandq_u32(guard, vceqq_u32(Dv, Bv)), Dv, Ev);
            const uint32x4_t E1 =
                vbslq_u32(vandq_u32(guard, vceqq_u32(Bv, Fv)), Fv, Ev);
            const uint32x4_t E2 =
                vbslq_u32(vandq_u32(guard, vceqq_u32(Dv, Hv)), Dv, Ev);
            const uint32x4_t E3 =
                vbslq_u32(vandq_u32(guard, vceqq_u32(Hv, Fv)), Hv, Ev);

            { /* interleave E0,E1 / E2,E3 into the two dst rows */
                uint32x4x2_t top = {E0, E1};
                uint32x4x2_t bot = {E2, E3};
                vst2q_u32(d0 + 2 * x, top);
                vst2q_u32(d1 + 2 * x, bot);
            }
        }
        /* scalar tail (w % 4) — same rules, clamped pads already valid */
        for (; x < w; x++) {
            const uint32_t B = s2x_pad_prev[x + 1];
            const uint32_t D = s2x_pad_cur[x];
            const uint32_t E = s2x_pad_cur[x + 1];
            const uint32_t F = s2x_pad_cur[x + 2];
            const uint32_t H = s2x_pad_next[x + 1];
            uint32_t e0 = E, e1 = E, e2 = E, e3 = E;
            if (B != H && D != F) {
                if (D == B) e0 = D;
                if (B == F) e1 = F;
                if (D == H) e2 = D;
                if (H == F) e3 = H;
            }
            d0[2 * x]     = e0;
            d0[2 * x + 1] = e1;
            d1[2 * x]     = e2;
            d1[2 * x + 1] = e3;
        }
    }
}

void switch_scaling_scale2x(const uint32_t* src, int w, int h, uint32_t* dst) {
    if (!src || !dst || w <= 0 || h <= 0 || w > 8192 || h > 8192) return;
    if (s2x_pad_cap < w + 2) {
        int cap = s2x_pad_cap ? s2x_pad_cap : 256;
        while (cap < w + 2) cap *= 2;
        uint32_t* p0 = (uint32_t*)realloc(s2x_pad_prev, (size_t)cap * 4);
        if (!p0) return; /* keep old pointers valid; fall back to scalar */
        uint32_t* p1 = (uint32_t*)realloc(s2x_pad_cur, (size_t)cap * 4);
        if (!p1) { s2x_pad_prev = p0; return; }
        uint32_t* p2 = (uint32_t*)realloc(s2x_pad_next, (size_t)cap * 4);
        if (!p2) { s2x_pad_prev = p0; s2x_pad_cur = p1; return; }
        s2x_pad_prev = p0; s2x_pad_cur = p1; s2x_pad_next = p2;
        s2x_pad_cap = cap;
    }
    s2x_scale2x_neon(src, w, h, dst);
}

#else /* non-ARM host: the public entry IS the scalar reference */

void switch_scaling_scale2x(const uint32_t* src, int w, int h, uint32_t* dst) {
    switch_scaling_scale2x_scalar(src, w, h, dst);
}

#endif /* __aarch64__ / __ARM_NEON */

const char* switch_scaling_scale2x_backend(void) {
#if defined(__aarch64__) || defined(__ARM_NEON)
    return "neon";
#else
    return "scalar";
#endif
}
