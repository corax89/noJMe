/* ============================================================================
 * noJME render module — implementation (v34.20)
 *
 * Contains EVERYTHING that pushes pixels, 2D and 3D:
 *
 *   SECTION 1 — MIDP 2D primitives (moved from midp/graphics.c):
 *     set_pixel, drawLine (Bresenham), fillTriangle (half-plane scan),
 *     fill/drawRect, draw/fillArc, round rects, 5x7 glyph rasterizer,
 *     drawString (UTF-8 -> glyphs, MIDP anchors), drawImage/drawRegion
 *     (clip + alpha + 90/180/270 sprite transforms), copyArea, getRGB,
 *     drawRGB blit stage, surface blits.
 *
 *   SECTION 2 — M3G (JSR-184) 3D software rasterizer (moved from
 *     midp/mobile3d.c): clear, clip-to-screen, Sutherland-Hodgman frustum
 *     clipper, perspective-correct interpolators, texture sampler with
 *     wrap modes, Gouraud vertex lighting, pixel blend/fog/texfunc stages,
 *     and the scanline triangle rasterizer with depth test + write masks.
 *
 * Optimizing? This is the file. Everything hot is here; nothing outside
 * this file writes pixels.
 * ============================================================================ */

#define _USE_MATH_DEFINES
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/time.h> /* v34.28: nojme_2d_bench timing (POSIX + MinGW) */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "debug.h"
#include "debug_macros.h"
#include "midp.h"
#include "jvm.h"
#include "render/render.h"
#include "midp/bitmap_font.h"

/* ============================================================================
 * SECTION 0: ARM NEON SIMD — conditional compilation (v34.24)
 * ============================================================================
 * Enabled automatically when the compiler targets NEON (gcc/clang define
 * __ARM_NEON__ for -mfpu=neon[-vfpv4], i.e. the Makefile's m17 and
 * linux-armv7 targets). Everything compiles away to nothing on x86/other
 * hosts, and NOJME_NEON_SCALAR=1 (or g_m3g_neon_force_scalar) forces the
 * scalar path at runtime for A/B verification.
 *
 * Bit-exactness contract: every vectorized helper mirrors the scalar
 * reference evaluation ORDER operation-by-operation (IEEE-754 ops are
 * per-lane identical; integer ops use the exact /255 identity
 * (x + (x>>8) + 1) >> 8 for x in [0, 65280]). Verified by
 * render_neon_selftest() (env NOJME_NEON_SELFTEST=1) which A/B-renders
 * through the public entry points with the NEON path forced off/on and
 * compares the output buffers byte-for-byte.
 *
 * Vectorized:
 *   - m3g_clear_ex color/depth clears (8/4 pixels per store)
 *   - fill_rect opaque fill + constant-color alpha blend
 *   - drawImage / drawRegion(TRANS_NONE) per-pixel-alpha src-over spans
 *   - drawRGB opaque and all-opaque-quad fast paths
 *   - m3g_rasterize_triangle scanline spans: 4 pixels per iteration for
 *     single nearest-filtered textures, MODULATE/REPLACE texture function
 *     and REPLACE/MODULATE/ALPHA/ALPHA_ADD compositing (the dominant
 *     configuration in real games). Fog, multitexturing, UV transforms,
 *     bilinear filtering and alpha thresholds keep the scalar path.
 * ============================================================================ */

#if defined(__ARM_NEON__) || defined(__ARM_NEON)
#define RENDER_HAVE_NEON 1
#include <arm_neon.h>
#else
#define RENDER_HAVE_NEON 0
#endif

/* v34.51 PERF (x86-64): SSE2 fast paths mirroring the NEON ones. Every
 * x86-64 CPU (and every MSYS2/MinGW build the user runs) guarantees SSE2,
 * so no runtime detection is needed. Like the NEON paths, every helper is
 * INTEGER-EXACT vs its scalar reference (same formula, same rounding) —
 * verified by the SSE2 sections of render_neon_selftest(). */
#if !RENDER_HAVE_NEON && (defined(__x86_64__) || defined(_M_X64) || defined(__SSE2__))
#define RENDER_HAVE_SSE2 1
#include <emmintrin.h>
#else
#define RENDER_HAVE_SSE2 0
#endif

#if RENDER_HAVE_NEON || RENDER_HAVE_SSE2
#define RENDER_HAVE_SIMD 1
#else
#define RENDER_HAVE_SIMD 0
#endif

/* A/B testing + user escape hatch: force the scalar rasterizer path.
 * -1 = unresolved (lazy env NOJME_NEON_SCALAR check on first triangle). */
int g_m3g_neon_force_scalar = -1;

/* v34.24: gate for the "first N" inner-loop diagnostics ([BLITRGB],
 * [M3G-SAMP], [M3G-WPIX], [M3G-RAST]). Silent in release builds; visible
 * in debug builds (J2ME_DEBUG=1 / --verbose) or with NOJME_TRACE_RAST=1. */
static int render_diag_on(void) {
    static int env_cache = -1;
    if (env_cache < 0) {
        const char* e = getenv("NOJME_TRACE_RAST");
        env_cache = (e && e[0] != '\0' && strcmp(e, "0") != 0) ? 1 : 0;
    }
    return env_cache || g_j2me_runtime_debug;
}

/* ============================================================================
 * v36.54/v36.55 [M3G-VIEWPORT-FRESH] + [M3G-COV-OPAQUE] coverage-маска
 * (восстановлено в v36.62 из потерянных сессий v36.53-v36.56).
 * См. большой контракт в render.h. Всё живёт на игровом потоке
 * (paint -> 2D-примитивы; bindTarget/releaseTarget/render — та же
 * нить), блокировки не нужны. Потребитель маски — посев в
 * mobile3d.c (m3g_bind_seed_graphics), маркеры — здесь.
 * ============================================================================ */

static uint8_t*      s_cov_mask = NULL;   /* canvas WxH, 1 = детерминирован 2D */
static size_t        s_cov_cap = 0;       /* выделено байт */
static MidpGraphics* s_cov_gfx = NULL;    /* биндящаяся канва (окно bind->bind) */
static uint32_t*     s_cov_pixels = NULL; /* идентичность пиксельного буфера */
static int           s_cov_w = 0, s_cov_h = 0;

static int cov_env_off(const char* name) {
    /* «=0» выключает; не задано/любое другое значение — дефолт (ВКЛ) */
    const char* e = getenv(name);
    return (e && e[0] != '\0' && strcmp(e, "0") == 0);
}

int nojme_m3g_bind_fresh_mode(void) {
    /* дефолт ВКЛ: свежий bind-вьюпорт — семантика window-surfaces железа */
    static int mode = -1;
    if (mode < 0) mode = cov_env_off("NOJME_M3G_BIND_FRESH") ? 0 : 1;
    return mode;
}

int nojme_m3g_cov_opaque_mode(void) {
    /* дефолт ВКЛ: opaque-семантика v36.55; «=0» — полная прямоугольная
     * отметка v36.54 (A/B-рубильник полевого разбора) */
    static int mode = -1;
    if (mode < 0) mode = cov_env_off("NOJME_M3G_COV_OPAQUE") ? 0 : 1;
    return mode;
}

void nojme_m3g_cov_window_begin(MidpGraphics* gfx) {
    if (!gfx || !gfx->pixels || gfx->width <= 0 || gfx->height <= 0 ||
        !nojme_m3g_bind_fresh_mode()) {
        /* не-Graphics-цель/без буфера/BIND_FRESH выкл — слежение гаснет:
         * маркеры дешёвые no-op, посев уходит в полный v25 */
        s_cov_gfx = NULL;
        s_cov_pixels = NULL;
        s_cov_w = s_cov_h = 0;
        return;
    }
    size_t need = (size_t)gfx->width * (size_t)gfx->height;
    if (need > s_cov_cap) {
        /* растим с запасом (маска живёт между сессиями, сброс — cov_reset) */
        size_t cap = s_cov_cap ? s_cov_cap : 65536;
        while (cap < need) cap *= 2;
        uint8_t* nm = (uint8_t*)realloc(s_cov_mask, cap);
        if (!nm) {           /* OOM: окно не открываем (полный посев) */
            s_cov_gfx = NULL;
            s_cov_pixels = NULL;
            return;
        }
        s_cov_mask = nm;
        s_cov_cap = cap;
    }
    memset(s_cov_mask, 0, need);
    s_cov_gfx = gfx;
    s_cov_pixels = gfx->pixels;
    s_cov_w = gfx->width;
    s_cov_h = gfx->height;
}

const uint8_t* nojme_m3g_cov_mask_for(MidpGraphics* gfx) {
    if (!s_cov_mask || !gfx || gfx != s_cov_gfx ||
        gfx->pixels != s_cov_pixels ||
        gfx->width != s_cov_w || gfx->height != s_cov_h)
        return NULL;
    return s_cov_mask;
}

void nojme_m3g_cov_mark_rect(MidpGraphics* gfx, int x, int y, int w, int h) {
    if (gfx != s_cov_gfx || !s_cov_mask) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s_cov_w) w = s_cov_w - x;
    if (y + h > s_cov_h) h = s_cov_h - y;
    if (w <= 0 || h <= 0) return;
    for (int yy = 0; yy < h; yy++) {
        uint8_t* row = s_cov_mask + (size_t)(y + yy) * s_cov_w + x;
        memset(row, 1, (size_t)w);
    }
}

void nojme_m3g_cov_mark_span(MidpGraphics* gfx, int x, int y,
                             const uint32_t* src, int n) {
    if (gfx != s_cov_gfx || !s_cov_mask || !src) return;
    if (y < 0 || y >= s_cov_h) return;
    int x0 = x > 0 ? x : 0;
    int x1 = x + n < s_cov_w ? x + n : s_cov_w;
    for (int xx = x0; xx < x1; xx++) {
        if ((src[xx - x] >> 24) == 0xFF)
            s_cov_mask[(size_t)y * s_cov_w + xx] = 1;
    }
}

void nojme_m3g_cov_reset(void) {
    free(s_cov_mask);
    s_cov_mask = NULL;
    s_cov_cap = 0;
    s_cov_gfx = NULL;
    s_cov_pixels = NULL;
    s_cov_w = s_cov_h = 0;
}

/* [M3G-2D-SYNC] v36.54: хук на входе каждого 2D-примитива — общий
 * счётчик + синхронизация с открытым bind-окном (реализация хука в
 * mobile3d.c: flush 3D-буфера при «2D после 3D», dirty-бит canvas-ahead
 * иначе). Одна проверка указателя, когда M3G не биндит эту канву. */
static void midp_gfx_op_enter(MidpGraphics* gfx) {
    ++g_midp_gfx_draw_ops; /* префиксная форма — вне replace-маски ниже */
    nojme_m3g_2d_enter(gfx);
}

#if RENDER_HAVE_NEON

/* ---------------- small lane helpers ----------------------------------- */

/* ARGB8888 channel extraction/packing on uint32 lanes */
static inline uint32x4_t vchan_r(uint32x4_t p) { return vandq_u32(vshrq_n_u32(p, 16), vdupq_n_u32(0xFF)); }
static inline uint32x4_t vchan_g(uint32x4_t p) { return vandq_u32(vshrq_n_u32(p,  8), vdupq_n_u32(0xFF)); }
static inline uint32x4_t vchan_b(uint32x4_t p) { return vandq_u32(p, vdupq_n_u32(0xFF)); }
static inline uint32x4_t vchan_a(uint32x4_t p) { return vshrq_n_u32(p, 24); }

static inline uint32x4_t vpack_argb(uint32x4_t a, uint32x4_t r, uint32x4_t g, uint32x4_t b) {
    return vorrq_u32(vorrq_u32(vshlq_n_u32(vandq_u32(a, vdupq_n_u32(0xFF)), 24),
                                vshlq_n_u32(vandq_u32(r, vdupq_n_u32(0xFF)), 16)),
                     vorrq_u32(vshlq_n_u32(vandq_u32(g, vdupq_n_u32(0xFF)), 8),
                                vandq_u32(b, vdupq_n_u32(0xFF))));
}

/* Exact integer division by 255 for x in [0, 65280]:
 * (x + (x >> 8) + 1) >> 8 == x / 255 — same value as the scalar (v*t)/255. */
static inline uint32x4_t vdiv255_u32(uint32x4_t x) {
    return vshrq_n_u32(vaddq_u32(vaddq_u32(x, vshrq_n_u32(x, 8)), vdupq_n_u32(1)), 8);
}

/* v34.24: exact float32x4 division for ARMv7 — vdivq_f32 exists only on
 * AArch64. Per-lane scalar division is IEEE-754 correctly rounded, so each
 * lane is bit-identical to the scalar path's division. */
/* v34.35 PERF (① fast division): NEON reciprocal estimate + 2 Newton-
 * Raphson refinements. ACCURACY: ~0.5-1 ULP worse than IEEE division —
 * NOT bit-identical. v34.41 PERF: ON BY DEFAULT — the NEON span paid
 * 12-16 scalar VDIV.F32 per 4-pixel quad (denominator, U, V, 4 color
 * channels, blend alphas; Cortex-A7 VDIV.F32 ≈ 20+ cycles, non-
 * pipelined) ≈ 250-320 cycles/quad JUST dividing — the dominant cost
 * at 240x320 fill (76.8k px/frame). vrecpe+2×vrecps ≈ 12-14 cycles for
 * all 4 lanes. Depth stays bit-exact (z uses mul/add only); only
 * UV/color/alpha ULPs shift (invisible in motion). NOJME_NEON_FASTDIV=0
 * restores strict IEEE per-lane division (the selftest forces it for
 * its bit-exact A/B contract). */
static int g_m3g_neon_fastdiv = -1;  /* -1 = unresolved env, 0 = off, 1 = on */
static inline float32x4_t vfast_div_f32(float32x4_t n, float32x4_t d) {
    float32x4_t recip = vrecpeq_f32(d);
    recip = vmulq_f32(vrecpsq_f32(d, recip), recip);
    recip = vmulq_f32(vrecpsq_f32(d, recip), recip);
    return vmulq_f32(n, recip);
}
static inline float32x4_t vdivq_f32v7(float32x4_t n, float32x4_t d) {
    if (g_m3g_neon_fastdiv < 0) {
        const char* fd = getenv("NOJME_NEON_FASTDIV");
        g_m3g_neon_fastdiv = (fd && fd[0] == '0' && fd[1] == '\0') ? 0 : 1;
    }
    if (g_m3g_neon_fastdiv) return vfast_div_f32(n, d);
    float32x4_t r = n;
    r = vsetq_lane_f32(vgetq_lane_f32(n, 0) / vgetq_lane_f32(d, 0), r, 0);
    r = vsetq_lane_f32(vgetq_lane_f32(n, 1) / vgetq_lane_f32(d, 1), r, 1);
    r = vsetq_lane_f32(vgetq_lane_f32(n, 2) / vgetq_lane_f32(d, 2), r, 2);
    r = vsetq_lane_f32(vgetq_lane_f32(n, 3) / vgetq_lane_f32(d, 3), r, 3);
    return r;
}

/* floorf per lane, exactly: trunc then correct negatives (t > v -> t-1) */
static inline float32x4_t vfloorq_f32(float32x4_t v) {
    int32x4_t ti = vcvtq_s32_f32(v);
    float32x4_t t = vcvtq_f32_s32(ti);
    uint32x4_t gt = vcgtq_f32(t, v);
    return vsubq_f32(t, vbslq_f32(gt, vdupq_n_f32(1.0f), vdupq_n_f32(0.0f)));
}

/* Build [base, base+s, (base+s)+s, ((base+s)+s)+s] — sequential adds,
 * bitwise identical to four scalar iterations of the x-loop. */
static inline float32x4_t lane_steps4(float base, float step) {
    float32x4_t b = vdupq_n_f32(base);
    float32x4_t s = vdupq_n_f32(step);
    float32x4_t l1 = vaddq_f32(b, s);
    float32x4_t l2 = vaddq_f32(l1, s);
    float32x4_t l3 = vaddq_f32(l2, s);
    float32x4_t r = b;
    r = vsetq_lane_f32(vgetq_lane_f32(l1, 0), r, 1);
    r = vsetq_lane_f32(vgetq_lane_f32(l2, 0), r, 2);
    r = vsetq_lane_f32(vgetq_lane_f32(l3, 0), r, 3);
    return r;
}

static inline int vmask_count(uint32x4_t m) {
    return (vgetq_lane_u32(m, 0) ? 1 : 0) + (vgetq_lane_u32(m, 1) ? 1 : 0) +
           (vgetq_lane_u32(m, 2) ? 1 : 0) + (vgetq_lane_u32(m, 3) ? 1 : 0);
}

/* ---------------- 2D span helpers --------------------------------------- */

/* Opaque fill: 8 pixels per iteration. */
static void span_fill_u32(uint32_t* dst, int n, uint32_t v) {
    int i = 0;
    uint32x4_t v4 = vdupq_n_u32(v);
    for (; i + 8 <= n; i += 8) {
        vst1q_u32(dst + i, v4);
        vst1q_u32(dst + i + 4, v4);
    }
    for (; i + 4 <= n; i += 4) vst1q_u32(dst + i, v4);
    for (; i < n; i++) dst[i] = v;
}

/* Depth clear: 4 floats per iteration. */
static void span_fill_f32(float* dst, int n, float v) {
    int i = 0;
    float32x4_t v4 = vdupq_n_f32(v);
    for (; i + 8 <= n; i += 8) {
        vst1q_f32(dst + i, v4);
        vst1q_f32(dst + i + 4, v4);
    }
    for (; i + 4 <= n; i += 4) vst1q_f32(dst + i, v4);
    for (; i < n; i++) dst[i] = v;
}

/* fill_rect alpha path: constant ARGB color blended over the row.
 * Scalar: r = (sr*a + dst_r*ia + 128) >> 8, out alpha = a. Integer-exact.
 * v34.35 PERF (② vld4 deinterleave): when the row pointer is 16-byte
 * aligned (framebuffer rows of all real games: width*4 % 16 == 0), load
 * the dst pixels with VLD4.8 — the hardware deinterleaves B/G/R/A into
 * four u8 vectors, replacing 3× (vshr+vand) channel extractions per quad
 * with zero instructions. The blend widens to u16 lanes with
 * vmull_u8/vmlal_u8; (x+128)>>8 becomes vaddq_n_u16+vshrn_n_u16 — the
 * SAME integer arithmetic as the scalar formula, so output is
 * bit-identical. Unaligned rows (odd widths, e.g. the 61px selftest) keep
 * the vchan path below. */
static void span_blend_row_const(uint32_t* dst, int n, uint32_t color_argb) {
    const uint32_t a  = (color_argb >> 24) & 0xFF;
    const uint32_t ia = 255u - a;
    const uint32_t sr = (color_argb >> 16) & 0xFF;
    const uint32_t sg = (color_argb >> 8) & 0xFF;
    const uint32_t sb = color_argb & 0xFF;
    const uint32x4_t a4 = vdupq_n_u32(a), ia4 = vdupq_n_u32(ia);
    const uint32x4_t sr4 = vdupq_n_u32(sr), sg4 = vdupq_n_u32(sg), sb4 = vdupq_n_u32(sb);
    const uint32x4_t c128 = vdupq_n_u32(128);
    int i = 0;
#if RENDER_HAVE_NEON
    /* vld4 path: 16 pixels per iteration, requires 16-byte alignment. */
    if (((uintptr_t)dst & 15u) == 0) {
        const uint8x8_t sr8 = vdup_n_u8((uint8_t)sr);
        const uint8x8_t sg8 = vdup_n_u8((uint8_t)sg);
        const uint8x8_t sb8 = vdup_n_u8((uint8_t)sb);
        const uint8x8_t a8  = vdup_n_u8((uint8_t)a);
        const uint8x8_t ia8 = vdup_n_u8((uint8_t)ia);
        for (; i + 16 <= n; i += 16) {
            uint8_t* row = (uint8_t*)(dst + i);
            uint8x16x4_t d = vld4q_u8(row);
            /* val[0]=B val[1]=G val[2]=R val[3]=A (little-endian ARGB) */
            uint16x8_t rlo = vaddq_u16(vmlal_u8(vmull_u8(sr8, a8), vget_low_u8(d.val[2]), ia8), vdupq_n_u16(128));
            uint16x8_t rhi = vaddq_u16(vmlal_u8(vmull_u8(sr8, a8), vget_high_u8(d.val[2]), ia8), vdupq_n_u16(128));
            uint16x8_t glo = vaddq_u16(vmlal_u8(vmull_u8(sg8, a8), vget_low_u8(d.val[1]), ia8), vdupq_n_u16(128));
            uint16x8_t ghi = vaddq_u16(vmlal_u8(vmull_u8(sg8, a8), vget_high_u8(d.val[1]), ia8), vdupq_n_u16(128));
            uint16x8_t blo = vaddq_u16(vmlal_u8(vmull_u8(sb8, a8), vget_low_u8(d.val[0]), ia8), vdupq_n_u16(128));
            uint16x8_t bhi = vaddq_u16(vmlal_u8(vmull_u8(sb8, a8), vget_high_u8(d.val[0]), ia8), vdupq_n_u16(128));
            uint8x16x4_t o;
            o.val[0] = vcombine_u8(vshrn_n_u16(blo, 8), vshrn_n_u16(bhi, 8));
            o.val[1] = vcombine_u8(vshrn_n_u16(glo, 8), vshrn_n_u16(ghi, 8));
            o.val[2] = vcombine_u8(vshrn_n_u16(rlo, 8), vshrn_n_u16(rhi, 8));
            o.val[3] = vdupq_n_u8((uint8_t)a); /* out alpha = a (scalar semantics) */
            vst4q_u8(row, o);
        }
    }
#endif
    for (; i + 4 <= n; i += 4) {
        uint32x4_t d = vld1q_u32(dst + i);
        uint32x4_t r = vshrq_n_u32(vaddq_u32(vaddq_u32(vmulq_u32(sr4, a4), vmulq_u32(vchan_r(d), ia4)), c128), 8);
        uint32x4_t g = vshrq_n_u32(vaddq_u32(vaddq_u32(vmulq_u32(sg4, a4), vmulq_u32(vchan_g(d), ia4)), c128), 8);
        uint32x4_t b = vshrq_n_u32(vaddq_u32(vaddq_u32(vmulq_u32(sb4, a4), vmulq_u32(vchan_b(d), ia4)), c128), 8);
        vst1q_u32(dst + i, vpack_argb(a4, r, g, b));
    }
    for (; i < n; i++) {
        uint32_t dcol = dst[i];
        uint32_t r = (sr * a + ((dcol >> 16) & 0xFF) * ia + 128) >> 8;
        uint32_t g = (sg * a + ((dcol >> 8) & 0xFF) * ia + 128) >> 8;
        uint32_t b = (sb * a + (dcol & 0xFF) * ia + 128) >> 8;
        dst[i] = (a << 24) | (r << 16) | (g << 8) | b;
    }
}

/* drawImage/drawRegion alpha path: per-pixel source alpha over dst,
 * output alpha forced opaque. Scalar semantics: a==255 -> copy src,
 * a==0 -> leave dst, else (sc*a + dc*ia + 128)>>8. Integer-exact. */
static void span_src_over_span(uint32_t* dst, const uint32_t* src, int n) {
    const uint32x4_t c255 = vdupq_n_u32(255);
    const uint32x4_t c128 = vdupq_n_u32(128);
    const uint32x4_t out_a = vdupq_n_u32(0xFF000000u);
    int i = 0;
    for (; i + 4 <= n; i += 4) {
        uint32x4_t s = vld1q_u32(src + i);
        uint32x4_t d = vld1q_u32(dst + i);
        uint32x4_t a = vchan_a(s);
        uint32x4_t full = vceqq_u32(a, c255);
        uint32x4_t zero = vceqq_u32(a, vdupq_n_u32(0));
        uint32x4_t ia = vsubq_u32(c255, a);
        uint32x4_t r = vshrq_n_u32(vaddq_u32(vaddq_u32(vmulq_u32(vchan_r(s), a), vmulq_u32(vchan_r(d), ia)), c128), 8);
        uint32x4_t g = vshrq_n_u32(vaddq_u32(vaddq_u32(vmulq_u32(vchan_g(s), a), vmulq_u32(vchan_g(d), ia)), c128), 8);
        uint32x4_t b = vshrq_n_u32(vaddq_u32(vaddq_u32(vmulq_u32(vchan_b(s), a), vmulq_u32(vchan_b(d), ia)), c128), 8);
        uint32x4_t blended = vorrq_u32(out_a, vpack_argb(vdupq_n_u32(0xFF), r, g, b));
        /* full -> src (src alpha is already 0xFF), zero -> dst, else blended */
        uint32x4_t out = vbslq_u32(full, s, vbslq_u32(zero, d, blended));
        vst1q_u32(dst + i, out);
    }
    for (; i < n; i++) {
        uint32_t sc = src[i];
        uint32_t sa = (sc >> 24) & 0xFF;
        if (sa == 255) { dst[i] = sc; }
        else if (sa > 0) {
            uint32_t dc = dst[i];
            uint32_t ia = 255 - sa;
            uint32_t r = (((sc >> 16) & 0xFF) * sa + ((dc >> 16) & 0xFF) * ia + 128) >> 8;
            uint32_t g = (((sc >> 8) & 0xFF) * sa + ((dc >> 8) & 0xFF) * ia + 128) >> 8;
            uint32_t b = ((sc & 0xFF) * sa + (dc & 0xFF) * ia + 128) >> 8;
            dst[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
}

/* drawRGB opaque path: dst = 0xFF000000 | (src & 0x00FFFFFF). Exact. */
static void span_copy_or32(uint32_t* dst, const uint32_t* src, int n) {
    const uint32x4_t lo = vdupq_n_u32(0x00FFFFFFu);
    const uint32x4_t hi = vdupq_n_u32(0xFF000000u);
    int i = 0;
    for (; i + 8 <= n; i += 8) {
        vst1q_u32(dst + i,     vorrq_u32(hi, vandq_u32(vld1q_u32(src + i),     lo)));
        vst1q_u32(dst + i + 4, vorrq_u32(hi, vandq_u32(vld1q_u32(src + i + 4), lo)));
    }
    for (; i + 4 <= n; i += 4) vst1q_u32(dst + i, vorrq_u32(hi, vandq_u32(vld1q_u32(src + i), lo)));
    for (; i < n; i++) dst[i] = 0xFF000000u | (src[i] & 0x00FFFFFFu);
}

/* v34.28: reversed copy for drawRegion MIRROR/ROT180 opaque spans:
 * dst[i] = src[n-1-i], 4 pixels per iteration. vrev64q_u32 reverses the
 * lanes within each 64-bit half; swapping the halves completes the full
 * 4-lane reversal. Pure data movement — bit-exact. */
static void span_copy_rev32(uint32_t* dst, const uint32_t* src, int n) {
    int i = 0;
    for (; i + 4 <= n; i += 4) {
        uint32x4_t p = vld1q_u32(src + n - 4 - i);
        uint32x4_t r = vrev64q_u32(p); /* [a,b,c,d] -> [b,a,d,c] */
        uint32x4_t f = vcombine_u32(vget_high_u32(r), vget_low_u32(r)); /* -> [d,c,b,a] */
        vst1q_u32(dst + i, f);
    }
    for (; i < n; i++) dst[i] = src[n - 1 - i];
}

#endif /* RENDER_HAVE_NEON */

#if RENDER_HAVE_SSE2
/* ============================================================================
 * v34.51: SSE2 span helpers (x86-64) — mirror the NEON ones above.
 * Every helper is integer-exact vs the scalar reference loops:
 *  - fills / copies are pure data movement;
 *  - blends use the madd-pair trick so (x*a + y*ia + 128) is evaluated
 *    exactly like the scalar integer formula (all intermediates <= 130178,
 *    computed in 32-bit madd lanes);
 *  - /255 divisions use (x + (x>>8) + 1) >> 8, identical to the scalar.
 * All call sites dispatch through the RENDER_HAVE_SIMD sections, so the
 * scalar loops remain the NOJME_2D_SCALAR/NEON_SCALAR reference paths.
 * ==========================================================================*/

/* ARGB8888 channel extraction/packing on 32-bit lanes */
static inline __m128i svchan_r(__m128i p) { return _mm_and_si128(_mm_srli_epi32(p, 16), _mm_set1_epi32(0xFF)); }
static inline __m128i svchan_g(__m128i p) { return _mm_and_si128(_mm_srli_epi32(p,  8), _mm_set1_epi32(0xFF)); }
static inline __m128i svchan_b(__m128i p) { return _mm_and_si128(p, _mm_set1_epi32(0xFF)); }
static inline __m128i svchan_a(__m128i p) { return _mm_srli_epi32(p, 24); }

/* (x + (x>>8) + 1) >> 8 == x/255 for x in [0, 65280] — exact. */
static inline __m128i sdiv255(__m128i x) {
    return _mm_srli_epi32(_mm_add_epi32(_mm_add_epi32(x, _mm_srli_epi32(x, 8)),
                                       _mm_set1_epi32(1)), 8);
}

/* Widen the low 4 epi16 words of two epi32-lane vectors into an
 * interleaved epi16 vector [a0,b0,a1,b1,a2,b2,a3,b3]: the value lanes are
 * <= 255 so _mm_packs_epi32 cannot saturate. */
static inline __m128i sinter_lo(__m128i a32, __m128i b32) {
    return _mm_unpacklo_epi16(_mm_packs_epi32(a32, a32), _mm_packs_epi32(b32, b32));
}

/* Opaque fill: 16 pixels per iteration. */
static void sse2_fill_u32(uint32_t* dst, int n, uint32_t v) {
    int i = 0;
    __m128i v4 = _mm_set1_epi32((int)v);
    for (; i + 16 <= n; i += 16) {
        _mm_storeu_si128((__m128i*)(dst + i),      v4);
        _mm_storeu_si128((__m128i*)(dst + i + 4),  v4);
        _mm_storeu_si128((__m128i*)(dst + i + 8),  v4);
        _mm_storeu_si128((__m128i*)(dst + i + 12), v4);
    }
    for (; i + 4 <= n; i += 4) _mm_storeu_si128((__m128i*)(dst + i), v4);
    for (; i < n; i++) dst[i] = v;
}

/* Depth clear: 8 floats per iteration. */
static void sse2_fill_f32(float* dst, int n, float v) {
    int i = 0;
    __m128 v4 = _mm_set1_ps(v);
    for (; i + 8 <= n; i += 8) {
        _mm_storeu_ps(dst + i,     v4);
        _mm_storeu_ps(dst + i + 4, v4);
    }
    for (; i + 4 <= n; i += 4) _mm_storeu_ps(dst + i, v4);
    for (; i < n; i++) dst[i] = v;
}

/* fill_rect alpha path: constant ARGB color blended over the row.
 * r = (sr*a + dr*ia + 128) >> 8 per channel — the madd pair trick computes
 * sr*a + dr*ia in ONE 32-bit lane each, so the arithmetic is identical to
 * the scalar formula. 4 pixels per iteration. */
static void sse2_blend_row_const(uint32_t* dst, int n, uint32_t color_argb) {
    const int a  = (int)((color_argb >> 24) & 0xFF);
    const int ia = 255 - a;
    const int sr = (int)((color_argb >> 16) & 0xFF);
    const int sg = (int)((color_argb >> 8) & 0xFF);
    const int sb = (int)(color_argb & 0xFF);
    /* Y = [a, ia, a, ia, ...] as epi16 words (one set1_epi32). */
    const __m128i yai = _mm_set1_epi32((int)((unsigned)a | ((unsigned)ia << 16)));
    const __m128i c128 = _mm_set1_epi32(128);
    /* NOTE: sinter_lo takes EPI32 lanes (it packs them itself) — the src
     * channels must be _mm_set1_epi32, NOT _mm_set1_epi16 (an epi16
     * constant reinterpreted as epi32 lanes saturates inside packs). */
    const __m128i sr32 = _mm_set1_epi32(sr);
    const __m128i sg32 = _mm_set1_epi32(sg);
    const __m128i sb32 = _mm_set1_epi32(sb);
    const __m128i a32  = _mm_set1_epi32(a);
    int i = 0;
    for (; i + 4 <= n; i += 4) {
        __m128i d = _mm_loadu_si128((const __m128i*)(dst + i));
        __m128i r = _mm_srli_epi32(_mm_add_epi32(
            _mm_madd_epi16(sinter_lo(sr32, svchan_r(d)), yai), c128), 8);
        __m128i g = _mm_srli_epi32(_mm_add_epi32(
            _mm_madd_epi16(sinter_lo(sg32, svchan_g(d)), yai), c128), 8);
        __m128i b = _mm_srli_epi32(_mm_add_epi32(
            _mm_madd_epi16(sinter_lo(sb32, svchan_b(d)), yai), c128), 8);
        /* pack: (a<<24) | (r<<16) | (g<<8) | b — channels are <= 255. */
        __m128i out = _mm_or_si128(_mm_or_si128(
            _mm_slli_epi32(_mm_and_si128(r, _mm_set1_epi32(0xFF)), 16),
            _mm_slli_epi32(_mm_and_si128(g, _mm_set1_epi32(0xFF)), 8)),
            _mm_and_si128(b, _mm_set1_epi32(0xFF)));
        _mm_storeu_si128((__m128i*)(dst + i), _mm_or_si128(out, _mm_slli_epi32(a32, 24)));
    }
    for (; i < n; i++) {
        uint32_t dcol = dst[i];
        uint32_t r = ((uint32_t)sr * (uint32_t)a + ((dcol >> 16) & 0xFF) * (uint32_t)ia + 128) >> 8;
        uint32_t g = ((uint32_t)sg * (uint32_t)a + ((dcol >> 8) & 0xFF) * (uint32_t)ia + 128) >> 8;
        uint32_t b = ((uint32_t)sb * (uint32_t)a + (dcol & 0xFF) * (uint32_t)ia + 128) >> 8;
        dst[i] = ((uint32_t)a << 24) | (r << 16) | (g << 8) | b;
    }
}

/* drawImage/drawRegion alpha path: per-pixel source alpha over dst,
 * output alpha forced opaque. a==255 -> copy src, a==0 -> leave dst,
 * else (sc*a + dc*ia + 128)>>8 — madd pairs make it exact. */
static void sse2_src_over_span(uint32_t* dst, const uint32_t* src, int n) {
    const __m128i c255 = _mm_set1_epi32(255);
    const __m128i c128 = _mm_set1_epi32(128);
    const __m128i out_a = _mm_set1_epi32((int)0xFF000000u);
    int i = 0;
    for (; i + 4 <= n; i += 4) {
        __m128i s = _mm_loadu_si128((const __m128i*)(src + i));
        __m128i d = _mm_loadu_si128((const __m128i*)(dst + i));
        __m128i a  = svchan_a(s);
        __m128i ia = _mm_sub_epi32(c255, a);
        /* Y = [a0,ia0, a1,ia1, ...] interleaved by sinter_lo. */
        __m128i y = sinter_lo(a, ia);
        __m128i r = _mm_srli_epi32(_mm_add_epi32(
            _mm_madd_epi16(sinter_lo(svchan_r(s), svchan_r(d)), y), c128), 8);
        __m128i g = _mm_srli_epi32(_mm_add_epi32(
            _mm_madd_epi16(sinter_lo(svchan_g(s), svchan_g(d)), y), c128), 8);
        __m128i b = _mm_srli_epi32(_mm_add_epi32(
            _mm_madd_epi16(sinter_lo(svchan_b(s), svchan_b(d)), y), c128), 8);
        __m128i blended = _mm_or_si128(out_a, _mm_or_si128(_mm_or_si128(
            _mm_slli_epi32(_mm_and_si128(r, _mm_set1_epi32(0xFF)), 16),
            _mm_slli_epi32(_mm_and_si128(g, _mm_set1_epi32(0xFF)), 8)),
            _mm_and_si128(b, _mm_set1_epi32(0xFF))));
        /* per-lane select: full -> src, zero -> dst, else blended.
         * (NEON does this with two vbsl; SSE2 uses and/andnot/or.) */
        __m128i full = _mm_cmpeq_epi32(a, c255);
        __m128i zero = _mm_cmpeq_epi32(a, _mm_setzero_si128());
        __m128i sel_src = _mm_and_si128(full, s);
        __m128i sel_dst = _mm_andnot_si128(full, _mm_and_si128(zero, d));
        __m128i sel_bl  = _mm_andnot_si128(zero, _mm_andnot_si128(full, blended));
        __m128i out = _mm_or_si128(_mm_or_si128(sel_src, sel_dst), sel_bl);
        _mm_storeu_si128((__m128i*)(dst + i), out);
    }
    for (; i < n; i++) {
        uint32_t sc = src[i];
        uint32_t sa = (sc >> 24) & 0xFF;
        if (sa == 255) { dst[i] = sc; }
        else if (sa > 0) {
            uint32_t dc = dst[i];
            uint32_t ia2 = 255 - sa;
            uint32_t r = (((sc >> 16) & 0xFF) * sa + ((dc >> 16) & 0xFF) * ia2 + 128) >> 8;
            uint32_t g = (((sc >> 8) & 0xFF) * sa + ((dc >> 8) & 0xFF) * ia2 + 128) >> 8;
            uint32_t b = ((sc & 0xFF) * sa + (dc & 0xFF) * ia2 + 128) >> 8;
            dst[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
}

/* drawRGB opaque path: dst = 0xFF000000 | (src & 0x00FFFFFF). Exact. */
static void sse2_copy_or32(uint32_t* dst, const uint32_t* src, int n) {
    const __m128i lo = _mm_set1_epi32((int)0x00FFFFFFu);
    const __m128i hi = _mm_set1_epi32((int)0xFF000000u);
    int i = 0;
    for (; i + 16 <= n; i += 16) {
        _mm_storeu_si128((__m128i*)(dst + i),
            _mm_or_si128(hi, _mm_and_si128(_mm_loadu_si128((const __m128i*)(src + i)), lo)));
        _mm_storeu_si128((__m128i*)(dst + i + 4),
            _mm_or_si128(hi, _mm_and_si128(_mm_loadu_si128((const __m128i*)(src + i + 4)), lo)));
        _mm_storeu_si128((__m128i*)(dst + i + 8),
            _mm_or_si128(hi, _mm_and_si128(_mm_loadu_si128((const __m128i*)(src + i + 8)), lo)));
        _mm_storeu_si128((__m128i*)(dst + i + 12),
            _mm_or_si128(hi, _mm_and_si128(_mm_loadu_si128((const __m128i*)(src + i + 12)), lo)));
    }
    for (; i + 4 <= n; i += 4)
        _mm_storeu_si128((__m128i*)(dst + i),
            _mm_or_si128(hi, _mm_and_si128(_mm_loadu_si128((const __m128i*)(src + i)), lo)));
    for (; i < n; i++) dst[i] = 0xFF000000u | (src[i] & 0x00FFFFFFu);
}

/* Reversed copy for drawRegion MIRROR/ROT180 opaque spans: dst[i] =
 * src[n-1-i], 4 pixels per iteration — one full lane shuffle. Bit-exact. */
static void sse2_copy_rev32(uint32_t* dst, const uint32_t* src, int n) {
    int i = 0;
    for (; i + 4 <= n; i += 4) {
        __m128i p = _mm_loadu_si128((const __m128i*)(src + n - 4 - i));
        _mm_storeu_si128((__m128i*)(dst + i), _mm_shuffle_epi32(p, _MM_SHUFFLE(0, 1, 2, 3)));
    }
    for (; i < n; i++) dst[i] = src[n - 1 - i];
}

#endif /* RENDER_HAVE_SSE2 */

/* Platform-neutral dispatch: on ARM the fast helpers above are compiled
 * under their span_* names directly; on x86-64 the SSE2 implementations
 * are named sse2_* and are re-exposed under the same span_* names. The
 * scalar reference loops behind the #else branches stay as-is. */
#if RENDER_HAVE_SSE2
#define span_fill_u32        sse2_fill_u32
#define span_fill_f32        sse2_fill_f32
#define span_blend_row_const sse2_blend_row_const
#define span_src_over_span   sse2_src_over_span
#define span_copy_or32       sse2_copy_or32
#define span_copy_rev32      sse2_copy_rev32
#endif

/* v34.28: runtime A/B escape hatch for the optimized 2D paths (fill_triangle
 * span intervals, draw_region row spans for transforms 1/2/3, batched glyph
 * rows, hoisted getRGB bounds). NOJME_2D_SCALAR=1 forces the original
 * per-pixel algorithms — used by the selftest and the pixel-identity
 * regression scripts. -1 = unresolved (lazy env check). */
int g_2d_force_scalar = -1;

static int two_d_scalar_on(void) {
    if (g_2d_force_scalar < 0) {
        g_2d_force_scalar = getenv("NOJME_2D_SCALAR") ? 1 : 0;
    }
    return g_2d_force_scalar;
}

/* Public accessor for other TUs (graphics.c getRGB hoisting). */
int nojme_2d_scalar_forced(void) { return two_d_scalar_on(); }

/* ============================================================================
 * v34.28: XRGB8888 -> RGB565 full-frame conversion (NEON)
 * ============================================================================
 * libretro presents RGB565 by default (the ARM/RetroArch case — m17): this
 * converts the ENTIRE screen buffer every frame. The scalar loop did ~10
 * ops per pixel on 240x320=76800 px; the NEON path converts 8 pixels per
 * iteration. dst16 = ((r&0xF8)<<8)|((g&0xFC)<<3)|(b>>3)
 *                 = (p>>8 & 0xF800) | (p>>5 & 0x7E0) | (p>>3 & 0x1F)
 * Pure shifts/masks/narrowing — bit-identical to the scalar formula. */
void nojme_convert_xrgb8888_to_rgb565(const uint32_t* restrict src, uint16_t* restrict dst,
                                      int width, int height) {
    if (!src || !dst || width <= 0 || height <= 0) return;
    const int total = width * height;
    int i = 0;
#if RENDER_HAVE_SSE2
    /* v34.51: 8 pixels per iteration. Same shifts/masks as the scalar
     * formula — d = (p>>8 & 0xF800)|(p>>5 & 0x7E0)|(p>>3 & 0x1F). SSE2 has
     * no UNSIGNED pack of epi32->epi16 (packus_epi32 is SSE4.1) and the
     * signed packs_epi32 saturates values >= 0x8000; the SUB/ADD variant
     * keeps the pack saturation-free: d-0x8000 lands in [-0x8000,0x7FFF]
     * (exact int16 range) and the wrapping 16-bit add of 0x8000 after the
     * pack restores d bit-exactly. */
    {
        const __m128i m_r = _mm_set1_epi32((int)0xF800u);
        const __m128i m_g = _mm_set1_epi32((int)0x07E0u);
        const __m128i m_b = _mm_set1_epi32((int)0x001Fu);
        const __m128i h32 = _mm_set1_epi32((int)0x8000u);
        const __m128i h16 = _mm_set1_epi16((short)(int)0x8000u);
        for (; i + 8 <= total; i += 8) {
            __m128i p0 = _mm_loadu_si128((const __m128i*)(src + i));
            __m128i p1 = _mm_loadu_si128((const __m128i*)(src + i + 4));
            __m128i d0 = _mm_or_si128(_mm_or_si128(
                _mm_and_si128(_mm_srli_epi32(p0, 8), m_r),
                _mm_and_si128(_mm_srli_epi32(p0, 5), m_g)),
                _mm_and_si128(_mm_srli_epi32(p0, 3), m_b));
            __m128i d1 = _mm_or_si128(_mm_or_si128(
                _mm_and_si128(_mm_srli_epi32(p1, 8), m_r),
                _mm_and_si128(_mm_srli_epi32(p1, 5), m_g)),
                _mm_and_si128(_mm_srli_epi32(p1, 3), m_b));
            d0 = _mm_sub_epi32(d0, h32);
            d1 = _mm_sub_epi32(d1, h32);
            __m128i pk = _mm_packs_epi32(d0, d1);
            _mm_storeu_si128((__m128i*)(dst + i), _mm_add_epi16(pk, h16));
        }
    }
#endif
#if RENDER_HAVE_NEON
    {
        const uint32x4_t m_r = vdupq_n_u32(0xF800u);
        const uint32x4_t m_g = vdupq_n_u32(0x07E0u);
        const uint32x4_t m_b = vdupq_n_u32(0x001Fu);
        for (; i + 8 <= total; i += 8) {
            uint32x4_t p0 = vld1q_u32(src + i);
            uint32x4_t p1 = vld1q_u32(src + i + 4);
            uint32x4_t d0 = vorrq_u32(vorrq_u32(vandq_u32(vshrq_n_u32(p0, 8), m_r),
                                                vandq_u32(vshrq_n_u32(p0, 5), m_g)),
                                      vandq_u32(vshrq_n_u32(p0, 3), m_b));
            uint32x4_t d1 = vorrq_u32(vorrq_u32(vandq_u32(vshrq_n_u32(p1, 8), m_r),
                                                vandq_u32(vshrq_n_u32(p1, 5), m_g)),
                                      vandq_u32(vshrq_n_u32(p1, 3), m_b));
            vst1q_u16(dst + i, vcombine_u16(vmovn_u32(d0), vmovn_u32(d1)));
        }
        for (; i + 4 <= total; i += 4) {
            uint32x4_t p0 = vld1q_u32(src + i);
            uint32x4_t d0 = vorrq_u32(vorrq_u32(vandq_u32(vshrq_n_u32(p0, 8), m_r),
                                                vandq_u32(vshrq_n_u32(p0, 5), m_g)),
                                      vandq_u32(vshrq_n_u32(p0, 3), m_b));
            vst1_u16(dst + i, vmovn_u32(d0));
        }
    }
#endif
    for (; i < total; i++) {
        uint32_t pixel = src[i];
        uint8_t r = (pixel >> 16) & 0xFF;
        uint8_t g = (pixel >> 8) & 0xFF;
        uint8_t b = pixel & 0xFF;
        dst[i] = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    }
}


/* ============================================================================
 * SECTION 1: MIDP 2D primitives (from graphics.c)
 * ============================================================================ */

/* Forward declaration (draw_char below is defined before set_pixel) */
static void set_pixel(MidpGraphics* gfx, int x, int y);

/* v34.51 PINK-FIX: graphics ops that can store a pixel with alpha != 0xFF
 * into a MUTABLE image must invalidate the image's cached exact-opacity
 * scan (midp_image_all_opaque) — otherwise a later drawImage could memcpy
 * the image over the framebuffer and show raw (pink) color-key pixels.
 * Screen/canvas graphics have owner_image == NULL (nothing to guard).
 * Ops that always write alpha==0xFF pixels (drawImage/drawRegion/copyArea,
 * drawRGB with processAlpha==false) do NOT need this call. */
static inline void midp_gfx_writes_alpha(MidpGraphics* gfx) {
    if (gfx->owner_image && gfx->alpha != 255)
        midp_image_invalidate_alpha_scan(gfx->owner_image);
}

/* v34.28: glyph rasterizer core shared by the two public wrappers.
 *
 * OLD cost: one set_pixel() call per set font bit (35 calls per glyph),
 * each with 2 additions + 8 comparisons. Text-heavy screens (Form/List
 * redraw every frame — see midp_process_repaints_impl) spent thousands of
 * those calls per frame.
 *
 * NEW path: clip evaluated ONCE per glyph into a [row_lo,row_hi) x
 * [col_lo,col_hi) window; each row writes directly into the framebuffer.
 * Same pixels, same packed color, same row-major order — bit-identical to
 * the set_pixel loop (which stays available via NOJME_2D_SCALAR=1 for A/B
 * verification and serves as the reference in the selftest).
 *
 * v36.29 [FONT-2X]: scale > 1 renders each glyph pixel as an
 * scale*scale block (same top-left origin, advance handled by the caller).
 * Scaled glyphs go through the set_pixel block loop — text is a small
 * fraction of the frame, correctness first. */
static void draw_char_core(MidpGraphics* gfx, const uint8_t* char_data, int x, int y,
                           int scale) {
    if (scale <= 0) scale = 1;
    if (scale == 1 && !two_d_scalar_on()) {
    /* scale-1 fast path (pixel output unchanged from v34.28) */

    /* Device-space glyph origin */
    const int gx = x + gfx->translate_x;
    const int gy = y + gfx->translate_y;

    /* Combined clip window: set_pixel accepts pixels inside BOTH the clip
     * rect and the screen bounds — intersect the two up front. */
    int wx1 = gfx->clip_x > 0 ? gfx->clip_x : 0;
    int wy1 = gfx->clip_y > 0 ? gfx->clip_y : 0;
    int wx2 = (gfx->clip_x + gfx->clip_width) < gfx->width ? (gfx->clip_x + gfx->clip_width) : gfx->width;
    int wy2 = (gfx->clip_y + gfx->clip_height) < gfx->height ? (gfx->clip_y + gfx->clip_height) : gfx->height;
    if (wx2 <= wx1 || wy2 <= wy1) return;

    /* Row/col window of the glyph that lands inside [wx1,wx2) x [wy1,wy2) */
    int row_lo = gy < wy1 ? wy1 - gy : 0;
    int row_hi = (gy + FONT_HEIGHT > wy2) ? wy2 - gy : FONT_HEIGHT;
    int col_lo = gx < wx1 ? wx1 - gx : 0;
    int col_hi = (gx + FONT_WIDTH > wx2) ? wx2 - gx : FONT_WIDTH;
    if (row_lo >= row_hi || col_lo >= col_hi) return;
    /* Glyph pixels are also rejected by set_pixel when x/y are negative —
     * col/row windows above already exclude those (wx1/wy1 >= 0). */

    const uint32_t color = ((uint32_t)(gfx->alpha) << 24) | gfx->rgb_color;
    midp_gfx_writes_alpha(gfx); /* v34.51 */
    const int stride = gfx->width;

    for (int row = row_lo; row < row_hi; row++) {
        uint8_t row_data = char_data[row];
        if (!row_data) continue; /* nothing set in this glyph row */
        uint32_t* base = gfx->pixels + (size_t)(gy + row) * stride + (gx + col_lo);
        for (int col = col_lo; col < col_hi; col++) {
            if (row_data & (1 << (FONT_WIDTH - 1 - col))) {
                base[col - col_lo] = color;
            }
        }
    }
    return;
    }

    /* Scalar path: scale 1 = legacy set_pixel loop (incl. NOJME_2D_SCALAR=1
     * A/B mode), scale > 1 = scale*scale blocks per glyph pixel. */
    for (int row = 0; row < FONT_HEIGHT; row++) {
        uint8_t row_data = char_data[row];
        for (int col = 0; col < FONT_WIDTH; col++) {
            /* MSB is leftmost pixel, so use (FONT_WIDTH - 1 - col) to get correct bit */
            if (row_data & (1 << (FONT_WIDTH - 1 - col))) {
                if (scale == 1) {
                    set_pixel(gfx, x + col, y + row);
                } else {
                    for (int sy = 0; sy < scale; sy++)
                        for (int sx = 0; sx < scale; sx++)
                            set_pixel(gfx, x + col * scale + sx,
                                      y + row * scale + sy);
                }
            }
        }
    }
}

/* Draw a single character using the bitmap font (legacy single-byte) */
__attribute__((unused))
static void midp_graphics_draw_char(MidpGraphics* gfx, char c, int x, int y) {
    const uint8_t* char_data = get_char_data(c);
    draw_char_core(gfx, char_data, x, y, 1);
}

/* Draw a single character using Unicode codepoint */
static void midp_graphics_draw_char_unicode(MidpGraphics* gfx, int codepoint, int x, int y,
                                            int scale) {
    const uint8_t* char_data = get_char_data_unicode(codepoint);
    draw_char_core(gfx, char_data, x, y, scale);
}

static void set_pixel(MidpGraphics* gfx, int x, int y) {
    x += gfx->translate_x;
    y += gfx->translate_y;
    
    if (x < gfx->clip_x || x >= gfx->clip_x + gfx->clip_width ||
        y < gfx->clip_y || y >= gfx->clip_y + gfx->clip_height) {
        return;
    }
    
    if (x < 0 || x >= gfx->width || y < 0 || y >= gfx->height) {
        return;
    }
    
    midp_gfx_writes_alpha(gfx); /* v34.51 */
    uint32_t color = ((uint32_t)(gfx->alpha) <<  24) | gfx->rgb_color;
    gfx->pixels[y * gfx->width + x] = color;
}

void midp_graphics_draw_line(MidpGraphics* gfx, int x1, int y1, int x2, int y2) {
    midp_gfx_op_enter(gfx);
    midp_gfx_writes_alpha(gfx); /* v34.51 */
    GFX_DEBUG("drawLine: (%d,%d) -> (%d,%d), color=0x%06X", 
            x1, y1, x2, y2, gfx->rgb_color);
    x1 += gfx->translate_x;
    y1 += gfx->translate_y;
    x2 += gfx->translate_x;
    y2 += gfx->translate_y;
    
    /* Pre-compute clip bounds once for inline pixel writes */
    int clip_x1 = gfx->clip_x;
    int clip_y1 = gfx->clip_y;
    int clip_x2 = gfx->clip_x + gfx->clip_width;
    int clip_y2 = gfx->clip_y + gfx->clip_height;
    int scr_w = gfx->width;
    int scr_h = gfx->height;
    uint32_t color = ((uint32_t)(gfx->alpha) <<  24) | gfx->rgb_color;
    
    /* Bresenham's line algorithm */
    int dx = abs(x2 - x1);
    int dy = abs(y2 - y1);
    int sx = x1 < x2 ? 1 : -1;
    int sy = y1 < y2 ? 1 : -1;
    int err = dx - dy;
    
    /* FIX-19s: the old loop SKIPPED the endpoint citing a non-existent
     * "MIDP spec" rule. Real devices (and J2SE) draw drawLine() inclusive
     * of both endpoints; VmTest drawLine(0,0,63,47) checks pixel (63,47). */
    while (true) {
        /* Inline clip check and direct framebuffer write */
        if (x1 >= clip_x1 && x1 < clip_x2 && y1 >= clip_y1 && y1 < clip_y2 &&
            x1 >= 0 && x1 < scr_w && y1 >= 0 && y1 < scr_h) {
            gfx->pixels[y1 * scr_w + x1] = color;
        }

        if (x1 == x2 && y1 == y2) break;  /* endpoint drawn - now stop */

        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x1 += sx; }
        if (e2 < dx) { err += dx; y1 += sy; }
    }
}

/* v34.8 FIX (Bounce Tales): Graphics.fillTriangle отсутствовал (тихий стаб).
 * Полуэкранная растеризация с учётом translate/clip, спека JSR-118.
 * v34.28: per-scanline EXACT span intervals instead of the O(w*h) half-plane
 * tests. Each edge function is linear in xx: w0 = d23x*(yy-y2) - d23y*(xx-x2)
 * = C0 - K0*xx (C/K constant per row). "w0*sgn >= 0" is therefore a
 * half-line in xx, and the intersection of the three half-lines is an
 * integer interval [lo,hi] computed with exact floor/ceil division — the
 * set of accepted pixels is IDENTICAL to the per-pixel test (the original
 * loop remains as the NOJME_2D_SCALAR reference path and selftest oracle). */
static int ftri_floor_div_pos(int a, int b) { /* b > 0 */
    return a >= 0 ? a / b : (a - b + 1) / b;
}
static int ftri_ceil_div_pos(int a, int b) {  /* b > 0 */
    return a >= 0 ? (a + b - 1) / b : -((-a) / b);
}

void midp_graphics_fill_triangle(MidpGraphics* gfx, int x1, int y1, int x2, int y2,
                                 int x3, int y3) {
    midp_gfx_op_enter(gfx);
    midp_gfx_writes_alpha(gfx); /* v34.51 */
    x1 += gfx->translate_x; y1 += gfx->translate_y;
    x2 += gfx->translate_x; y2 += gfx->translate_y;
    x3 += gfx->translate_x; y3 += gfx->translate_y;

    int clip_x1 = gfx->clip_x, clip_y1 = gfx->clip_y;
    int clip_x2 = gfx->clip_x + gfx->clip_width;
    int clip_y2 = gfx->clip_y + gfx->clip_height;
    int scr_w = gfx->width, scr_h = gfx->height;
    uint32_t color = ((uint32_t)(gfx->alpha) << 24) | gfx->rgb_color;

    int miny = y1 < y2 ? (y1 < y3 ? y1 : y3) : (y2 < y3 ? y2 : y3);
    int maxy = y1 > y2 ? (y1 > y3 ? y1 : y3) : (y2 > y3 ? y2 : y3);
    if (miny < clip_y1) miny = clip_y1;
    if (miny < 0) miny = 0;
    if (maxy > clip_y2) maxy = clip_y2;
    if (maxy > scr_h) maxy = scr_h;

    int d12x = x2 - x1, d12y = y2 - y1;
    int d23x = x3 - x2, d23y = y3 - y2;
    int d31x = x1 - x3, d31y = y1 - y3;
    /* Double-area for orientation-independent fill (include edges like MIDP) */
    int area2 = d12x * d23y - d12y * d23x;
    if (area2 == 0) return;

    int minx = clip_x1 > 0 ? clip_x1 : 0;
    int maxx = clip_x2 < scr_w ? clip_x2 : scr_w;

    /* Degenerate huge-coordinate guard: outside this range the edge-products
     * could overflow int32 — fall back to the original per-pixel loop so the
     * A/B contract stays exact even for pathological inputs. */
    const long long big = (1LL << 24);
    if (two_d_scalar_on() ||
        llabs(d12x) > big || llabs(d12y) > big || llabs(d23x) > big || llabs(d23y) > big ||
        llabs(d31x) > big || llabs(d31y) > big ||
        llabs(x1) > big || llabs(y1) > big || llabs(x2) > big || llabs(y2) > big ||
        llabs(x3) > big || llabs(y3) > big) {
        for (int yy = miny; yy < maxy; yy++) {
            for (int xx = minx; xx < maxx; xx++) {
                /* Half-plane tests; px in triangle if all cross-products of the
                 * consistent sign (edge-inclusive: use >=0 / <=0 by orientation) */
                int w0 = d23x * (yy - y2) - d23y * (xx - x2);
                int w1 = d31x * (yy - y3) - d31y * (xx - x3);
                int w2 = d12x * (yy - y1) - d12y * (xx - x1);
                int sgn = area2 > 0 ? 1 : -1;
                if (w0 * sgn >= 0 && w1 * sgn >= 0 && w2 * sgn >= 0) {
                    gfx->pixels[yy * scr_w + xx] = color;
                }
            }
        }
        return;
    }

    /* Fold the orientation sign into the edge functions once:
     * f_j(xx) = C_j - K_j * xx >= 0  (K_j = sgn * dY_j). Per row yy we
     * compute C_j, then the accepted xx interval from the three
     * half-planes, and fill it exactly like the per-pixel loop would. */
    const int sgn = area2 > 0 ? 1 : -1;
    const int K0 = sgn * d23y, K1 = sgn * d31y, K2 = sgn * d12y;

    for (int yy = miny; yy < maxy; yy++) {
        const int C0 = sgn * (d23x * (yy - y2) + d23y * x2);
        const int C1 = sgn * (d31x * (yy - y3) + d31y * x3);
        const int C2 = sgn * (d12x * (yy - y1) + d12y * x1);

        int lo = minx, hi = maxx - 1; /* inclusive span bounds from clip */
        int reject = 0;

        /* edge j: f = C - K*xx >= 0 */
        if (K0 > 0) { int ub = ftri_floor_div_pos(C0, K0); if (hi > ub) hi = ub; }
        else if (K0 < 0) { int lb = ftri_ceil_div_pos(-C0, -K0); if (lo < lb) lo = lb; }
        else if (C0 < 0) reject = 1;
        if (!reject) {
            if (K1 > 0) { int ub = ftri_floor_div_pos(C1, K1); if (hi > ub) hi = ub; }
            else if (K1 < 0) { int lb = ftri_ceil_div_pos(-C1, -K1); if (lo < lb) lo = lb; }
            else if (C1 < 0) reject = 1;
        }
        if (!reject) {
            if (K2 > 0) { int ub = ftri_floor_div_pos(C2, K2); if (hi > ub) hi = ub; }
            else if (K2 < 0) { int lb = ftri_ceil_div_pos(-C2, -K2); if (lo < lb) lo = lb; }
            else if (C2 < 0) reject = 1;
        }
        if (reject || lo > hi) continue;

        /* Fill the accepted span — same raw color write as the scalar loop
         * (it never alpha-blends fillTriangle), v34.28: NEON stores. */
        uint32_t* row = gfx->pixels + (size_t)yy * scr_w + lo;
        const int n = hi - lo + 1;
#if RENDER_HAVE_SIMD
        span_fill_u32(row, n, color);
#else
        for (int i = 0; i < n; i++) row[i] = color;
#endif
    }
}

void midp_graphics_fill_rect(MidpGraphics* gfx, int x, int y, int w, int h) {
    midp_gfx_op_enter(gfx);
    midp_gfx_writes_alpha(gfx); /* v34.51 */
    GFX_DEBUG("fillRect: (%d,%d) %dx%d, color=0x%06X, clip=(%d,%d %dx%d)",
            x, y, w, h, gfx->rgb_color, gfx->clip_x, gfx->clip_y, gfx->clip_width, gfx->clip_height);
    x += gfx->translate_x;
    y += gfx->translate_y;
    
    /* Compute clipped rect once */
    int x1 = x > gfx->clip_x ? x : gfx->clip_x;
    int y1 = y > gfx->clip_y ? y : gfx->clip_y;
    int x2 = (x + w) < (gfx->clip_x + gfx->clip_width) ? (x + w) : (gfx->clip_x + gfx->clip_width);
    int y2 = (y + h) < (gfx->clip_y + gfx->clip_height) ? (y + h) : (gfx->clip_y + gfx->clip_height);
    
    /* Clamp to screen bounds once */
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > gfx->width) x2 = gfx->width;
    if (y2 > gfx->height) y2 = gfx->height;
    
    int row_width = x2 - x1;
    if (row_width <= 0) return;
    
    /* v29 TEMP: full-canvas fill trace */
    {
        static int fw_trace = -1;
        if (fw_trace < 0) fw_trace = getenv("NOJME_GFXW") ? 1 : 0;
        if (fw_trace && row_width * (y2 - y1) >= (gfx->width * gfx->height * 3) / 4) {
            fprintf(stderr, "[GFX-W] fillRect (%d,%d) %dx%d color=%06X alpha=%d gfx=%p\n",
                    x1, y1, row_width, y2 - y1, gfx->rgb_color, gfx->alpha, (void*)gfx);
        }
    }
    
    uint32_t color = ((uint32_t)(gfx->alpha) <<  24) | gfx->rgb_color;

    /* [M3G-VIEWPORT-FRESH]/[M3G-COV-OPAQUE] v36.54/v36.55: fillRect
     * детерминирует накрытые пиксели ТОЛЬКО при полной непрозрачности
     * (MIDP setColor не задаёт alpha заливки — alpha<255 бывает лишь у
     * внутренних полупрозрачных занулений эмулятора; полупрозрачный
     * оверлей поверх неопределённой базы = мусор в поле). Отметка —
     * РОВНО записанная область (x1..x2/y1..y2 после translate+clip).
     * NOJME_M3G_COV_OPAQUE=0 вернёт безусловную прямоугольную v36.54. */
    if (gfx->alpha == 255 || !nojme_m3g_cov_opaque_mode()) {
        nojme_m3g_cov_mark_rect(gfx, x1, y1, row_width, y2 - y1);
    }
    
    /* Fast path: opaque fill — tight loop, no per-pixel bounds checks.
     * v34.24: 8/4 pixels per NEON store on ARM. */
    if (gfx->alpha == 255) {
        for (int py = y1; py < y2; py++) {
            uint32_t *row = gfx->pixels + py * gfx->width + x1;
#if RENDER_HAVE_SIMD
            span_fill_u32(row, row_width, color);
#else
            for (int i = 0; i < row_width; i++) {
                row[i] = color;
            }
#endif
        }
    } else {
        /* Alpha blend path with pre-computed values (v34.24: NEON 4-wide,
         * integer-exact replica of the scalar formula) */
#if RENDER_HAVE_SIMD
        for (int py = y1; py < y2; py++) {
            uint32_t *row = gfx->pixels + py * gfx->width + x1;
            span_blend_row_const(row, row_width, color);
        }
#else
        uint8_t alpha = gfx->alpha;
        uint8_t inv_alpha = 255 - alpha;
        uint8_t sr = (gfx->rgb_color >> 16) & 0xFF;
        uint8_t sg = (gfx->rgb_color >> 8) & 0xFF;
        uint8_t sb = gfx->rgb_color & 0xFF;
        for (int py = y1; py < y2; py++) {
            uint32_t *row = gfx->pixels + py * gfx->width + x1;
            for (int i = 0; i < row_width; i++) {
                uint32_t dst = row[i];
                uint8_t r = (sr * alpha + ((dst >> 16) & 0xFF) * inv_alpha + 128) >> 8;
                uint8_t g = (sg * alpha + ((dst >> 8) & 0xFF) * inv_alpha + 128) >> 8;
                uint8_t b = (sb * alpha + (dst & 0xFF) * inv_alpha + 128) >> 8;
                row[i] = ((uint32_t)(alpha) <<  24) | (r << 16) | (g << 8) | b;
            }
        }
#endif
    }
}

void midp_graphics_draw_rect(MidpGraphics* gfx, int x, int y, int w, int h) {
    midp_gfx_op_enter(gfx);
    midp_graphics_draw_line(gfx, x, y, x + w - 1, y);
    midp_graphics_draw_line(gfx, x, y + h - 1, x + w - 1, y + h - 1);
    midp_graphics_draw_line(gfx, x, y, x, y + h - 1);
    midp_graphics_draw_line(gfx, x + w - 1, y, x + w - 1, y + h - 1);
}

void midp_graphics_draw_arc(MidpGraphics* gfx, int x, int y, int w, int h,
                            int start_angle, int arc_angle) {
    midp_gfx_op_enter(gfx);
    midp_gfx_writes_alpha(gfx); /* v34.51 */
    x += gfx->translate_x;
    y += gfx->translate_y;
    
    if (w < 0 || h < 0) return;
    
    double cx = x + w / 2.0;
    double cy = y + h / 2.0;
    double rx = w / 2.0;
    double ry = h / 2.0;
    
    /* MIDP uses clockwise angles with 0=right, 90=up, 180=left, 270=down
     * Mathematical convention: 0=right, 90=down (screen coords), 180=left, 270=up
     * Need to negate the angle to convert from MIDP to screen coordinates */
    double start_rad = -start_angle * M_PI / 180.0;
    double end_rad = -(start_angle + arc_angle) * M_PI / 180.0;
    
    int steps = abs(arc_angle) * 2;
    if (steps < 4) steps = 4;
    if (steps > 720) steps = 720;
    
    double step = (end_rad - start_rad) / steps;
    
    /* Pre-compute clip bounds for inline pixel writes */
    int clip_x1 = gfx->clip_x;
    int clip_y1 = gfx->clip_y;
    int clip_x2 = gfx->clip_x + gfx->clip_width;
    int clip_y2 = gfx->clip_y + gfx->clip_height;
    int scr_w = gfx->width;
    int scr_h = gfx->height;
    uint32_t color = ((uint32_t)(gfx->alpha) <<  24) | gfx->rgb_color;
    
    int prev_px = -1, prev_py = -1;
    for (int i = 0; i <= steps; i++) {
        double angle = start_rad + i * step;
        int px = (int)(cx + rx * cos(angle));
        int py = (int)(cy + ry * sin(angle));
        /* Only draw if position changed (avoid overdraw) */
        if (px != prev_px || py != prev_py) {
            /* Inline clip check and direct framebuffer write */
            if (px >= clip_x1 && px < clip_x2 && py >= clip_y1 && py < clip_y2 &&
                px >= 0 && px < scr_w && py >= 0 && py < scr_h) {
                gfx->pixels[py * scr_w + px] = color;
            }
            prev_px = px;
            prev_py = py;
        }
    }
}

void midp_graphics_fill_arc(MidpGraphics* gfx, int x, int y, int w, int h,
                            int start_angle, int arc_angle) {
    midp_gfx_op_enter(gfx);
    midp_gfx_writes_alpha(gfx); /* v34.51 */
    x += gfx->translate_x;
    y += gfx->translate_y;
    
    if (w <= 0 || h <= 0) return;
    
    double cx = x + w / 2.0;
    double cy = y + h / 2.0;
    double rx = w / 2.0;
    double ry = h / 2.0;
    
    double start_rad = -start_angle * M_PI / 180.0;
    double end_rad = -(start_angle + arc_angle) * M_PI / 180.0;
    
    /* Normalize angles to [0, 2*PI) */
    while (start_rad < 0) start_rad += 2 * M_PI;
    while (start_rad >= 2 * M_PI) start_rad -= 2 * M_PI;
    while (end_rad < 0) end_rad += 2 * M_PI;
    while (end_rad >= 2 * M_PI) end_rad -= 2 * M_PI;
    
    double arc_start, arc_end;
    
    if (arc_angle > 0) {
        arc_start = end_rad;
        arc_end = start_rad;
    } else {
        arc_start = start_rad;
        arc_end = end_rad;
    }
    
    bool full_circle = (abs(arc_angle) >= 360);
    
    /* Pre-compute clip bounds for inline pixel writes */
    int clip_x1 = gfx->clip_x;
    int clip_y1 = gfx->clip_y;
    int clip_x2 = gfx->clip_x + gfx->clip_width;
    int clip_y2 = gfx->clip_y + gfx->clip_height;
    int scr_w = gfx->width;
    int scr_h = gfx->height;
    uint32_t color = ((uint32_t)(gfx->alpha) <<  24) | gfx->rgb_color;
    
    /* Fast path for full circle: use row-based ellipse scan without angle check */
    if (full_circle) {
        int bx1 = x > clip_x1 ? x : clip_x1;
        int by1 = y > clip_y1 ? y : clip_y1;
        int bx2 = (x + w) < clip_x2 ? (x + w) : clip_x2;
        int by2 = (y + h) < clip_y2 ? (y + h) : clip_y2;
        if (bx1 < 0) bx1 = 0;
        if (by1 < 0) by1 = 0;
        if (bx2 > scr_w) bx2 = scr_w;
        if (by2 > scr_h) by2 = scr_h;
        
        for (int py = by1; py < by2; py++) {
            double ndy = (py - cy) / ry;
            double ndy2 = ndy * ndy;
            if (ndy2 > 1.0) continue;
            double ndx_max = sqrt(1.0 - ndy2);
            int px_start = (int)(cx - ndx_max * rx);
            int px_end = (int)(cx + ndx_max * rx);
            if (px_start < bx1) px_start = bx1;
            if (px_end >= bx2) px_end = bx2 - 1;
            uint32_t *row = gfx->pixels + py * scr_w;
            /* v34.28: NEON stores for the full-circle row span (raw color
             * write, exactly like the scalar loop — bit-exact). */
            const int n = px_end - px_start + 1;
            if (n > 0) {
#if RENDER_HAVE_SIMD
                span_fill_u32(row + px_start, n, color);
#else
                for (int px = px_start; px <= px_end; px++) {
                    row[px] = color;
                }
#endif
            }
        }
        return;
    }
    
    /* Pre-compute direction vectors for cross-product arc sector test.
     * This replaces per-pixel atan2() with just multiply+compare ops.
     * For a point (dx,dy), cross(dir_A, P) and cross(P, dir_B) determine
     * whether the point's angle falls within the arc sector. */
    double sin_start = sin(arc_start);
    double cos_start = cos(arc_start);
    double sin_end = sin(arc_end);
    double cos_end = cos(arc_end);
    /* cross_AB = sin(arc_end - arc_start), determines sector winding */
    double cross_AB = cos_start * sin_end - sin_start * cos_end;
    
    for (int py = y; py < y + h && py < scr_h; py++) {
        if (py < 0 || py < clip_y1 || py >= clip_y2) continue;
        
        for (int px = x; px < x + w && px < scr_w; px++) {
            if (px < 0 || px < clip_x1 || px >= clip_x2) continue;
            
            if (rx <= 0 || ry <= 0) continue;
            
            double dx = (px - cx) / rx;
            double dy = (py - cy) / ry;
            
            if (dx * dx + dy * dy > 1.0) continue;
            
            /* Cross-product sector test: much faster than atan2 on ARM */
            double cross_ap = cos_start * dy - sin_start * dx;
            double cross_pb = dx * sin_end - dy * cos_end;
            
            bool in_arc;
            if (cross_AB >= 0) {
                /* Non-wrapping sector: point must be CCW from A and B CCW from P */
                in_arc = (cross_ap >= 0) && (cross_pb >= 0);
            } else {
                /* Wrapping sector: point must be on either side */
                in_arc = (cross_ap >= 0) || (cross_pb >= 0);
            }
            
            if (in_arc) {
                gfx->pixels[py * scr_w + px] = color;
            }
        }
    }
}

void midp_graphics_draw_round_rect(MidpGraphics* gfx, int x, int y, int w, int h,
                                   int arc_w, int arc_h) {
    midp_gfx_op_enter(gfx);
    /* Draw lines (drawLine excludes endpoint, so pass arc boundary as endpoint
     * to draw pixels up to one pixel before the arc starts) */
    midp_graphics_draw_line(gfx, x + arc_w/2, y, x + w - arc_w/2, y);
    midp_graphics_draw_line(gfx, x + arc_w/2, y + h - 1, x + w - arc_w/2, y + h - 1);
    midp_graphics_draw_line(gfx, x, y + arc_h/2, x, y + h - arc_h/2);
    midp_graphics_draw_line(gfx, x + w - 1, y + arc_h/2, x + w - 1, y + h - arc_h/2);
    
    /* Draw arcs */
    midp_graphics_draw_arc(gfx, x, y, arc_w, arc_h, 90, 90);
    midp_graphics_draw_arc(gfx, x + w - arc_w, y, arc_w, arc_h, 0, 90);
    midp_graphics_draw_arc(gfx, x + w - arc_w, y + h - arc_h, arc_w, arc_h, 270, 90);
    midp_graphics_draw_arc(gfx, x, y + h - arc_h, arc_w, arc_h, 180, 90);
}

void midp_graphics_fill_round_rect(MidpGraphics* gfx, int x, int y, int w, int h,
                                   int arc_w, int arc_h) {
    midp_gfx_op_enter(gfx);
    /* Fill center */
    midp_graphics_fill_rect(gfx, x + arc_w/2, y, w - arc_w, h);
    midp_graphics_fill_rect(gfx, x, y + arc_h/2, arc_w/2, h - arc_h);
    midp_graphics_fill_rect(gfx, x + w - arc_w/2, y + arc_h/2, arc_w/2, h - arc_h);
    
    /* Fill corners */
    midp_graphics_fill_arc(gfx, x, y, arc_w, arc_h, 90, 90);
    midp_graphics_fill_arc(gfx, x + w - arc_w, y, arc_w, arc_h, 0, 90);
    midp_graphics_fill_arc(gfx, x + w - arc_w, y + h - arc_h, arc_w, arc_h, 270, 90);
    midp_graphics_fill_arc(gfx, x, y + h - arc_h, arc_w, arc_h, 180, 90);
}

/* v36.29 [FONT-2X]: scaled core. midp_graphics_draw_string = current-font
 * scale (SIZE_LARGE -> 2x); midp_graphics_draw_string_scaled/_2x force it.
 * scale 1 is bit-identical to the pre-v36.29 renderer. */
static void draw_string_scaled_core(MidpGraphics* gfx, const char* str,
                                    int x, int y, int anchor, int scale) {
    midp_gfx_op_enter(gfx);
    GFX_DEBUG("drawString: str='%s', x=%d, y=%d, anchor=%d, color=0x%06X",
            str ? str : "(null)", x, y, anchor, gfx ? gfx->rgb_color : 0);
    if (!str || !gfx) return;
    if (scale < 1) scale = 1;

    /* v36.58 [TEXTLOG]: env-gated drawString trace (NOJME_TEXTLOG=1) — the
     * "blind menu" instrument: game canvases drawn from a game thread (Rally
     * 3D menu/race) never reach the repaint pump, so host/field sessions had
     * no way to see WHICH screen the game is showing. One line per drawn
     * string, stderr (unbuffered, lands next to [TRACE] lines). Default off. */
    {
        static int s_textlog_on = -1;
        if (s_textlog_on < 0) {
            const char* e = getenv("NOJME_TEXTLOG");
            s_textlog_on = (e && e[0] && e[0] != '0') ? 1 : 0;
        }
        if (s_textlog_on) {
            fprintf(stderr, "[TEXT] (%d,%d) c=0x%06X %s\n", x, y,
                    gfx ? gfx->rgb_color : 0, str);
            fflush(stderr);
            /* v36.58: device bridge — fprintf(stderr) is gated by the
             * quiet log policy on HOS; the weak sw_trace_force mirror
             * lands the same line in log.txt (same pattern as mobile3d.c
             * diagnostics). */
            {
                extern void sw_trace_force(const char* fmt, ...)
                    __attribute__((weak));
                if (&sw_trace_force && sw_trace_force) {
                    sw_trace_force("[TEXT] (%d,%d) c=0x%06X %s", x, y,
                                   gfx ? gfx->rgb_color : 0, str);
                }
            }
        }
    }
    
#ifdef J2ME_HEADLESS
    /* Capture text for headless console output */
    midp_headless_log_text(str, x, y, gfx->rgb_color);
#endif
    
    int byte_len = strlen(str);
    if (byte_len == 0) return;
    
    /* Count characters (not bytes) using UTF-8 decoding */
    int char_count = utf8_strlen(str);
    if (char_count == 0) return;
    
    /* ИСПРАВЛЕНО: Properly truncate both char_count and calculate max byte_len */
    int max_chars = 4096;
    if (char_count > max_chars) {
        char_count = max_chars;
        /* Find the byte offset for the truncated character count */
        int chars_seen = 0;
        int safe_byte_len = 0;
        while (chars_seen < max_chars && safe_byte_len < byte_len) {
            int codepoint = utf8_decode(str, &safe_byte_len, byte_len);
            if (codepoint < 0) break;
            chars_seen++;
        }
        byte_len = safe_byte_len;
    }
    
    /* Calculate text dimensions based on character count */
    int adv = (FONT_WIDTH + 1) * scale;       /* +1 for spacing between chars */
    int text_width = char_count * adv - scale;
    int text_height = FONT_HEIGHT * scale;    /* glyph box (NOT getHeight()) */
    
    /* Apply translation */
    int draw_x = x + gfx->translate_x;
    int draw_y = y + gfx->translate_y;
    
    /* MIDP2 anchor constants:
     * HCENTER = 1, VCENTER = 2, LEFT = 4, RIGHT = 8, TOP = 16, BOTTOM = 32, BASELINE = 64
     */
    
    /* Apply horizontal anchor */
    if (anchor & 0x01) {        /* HCENTER = 1 */
        draw_x -= text_width / 2;
    } else if (anchor & 0x08) { /* RIGHT = 8 */
        draw_x -= text_width;
    }
    /* LEFT = 4 is default (no adjustment needed) */
    
    /* Apply vertical anchor */
    if (anchor & 0x02) {        /* VCENTER = 2 */
        draw_y -= text_height / 2;
    } else if (anchor & 0x40) { /* BASELINE = 64 */
        draw_y -= (FONT_HEIGHT - 2) * scale;  /* ascent is scale-invariant */
    } else if (anchor & 0x20) { /* BOTTOM = 32 */
        draw_y -= text_height;
    }
    /* TOP = 16 is default (no adjustment needed) */
    
    /* Debug output for anchor calculation */
    GFX_DEBUG("drawString: char_count=%d, text_width=%d, draw_x=%d, draw_y=%d (anchor: HCENTER=%d RIGHT=%d LEFT=%d)",
            char_count, text_width, draw_x, draw_y, 
            (anchor & 0x01) ? 1 : 0, 
            (anchor & 0x08) ? 1 : 0,
            (anchor & 0x04) ? 1 : 0);
    
    /* Draw each character, decoding UTF-8 */
    /* Use a separate counter to limit drawn characters (prevents buffer issues) */
    int cur_x = draw_x;
    int i = 0;
    int chars_drawn = 0;
    while (i < byte_len && chars_drawn < char_count) {
        int codepoint = utf8_decode(str, &i, byte_len);
        if (codepoint >= 0) {
            midp_graphics_draw_char_unicode(gfx, codepoint, cur_x, draw_y, scale);
            cur_x += adv;
            chars_drawn++;
        }
    }
}

/* v36.29 [FONT-2X]: the default entry — scale comes from the CURRENT font
 * of the graphics context (Font.getFont(..., SIZE_LARGE) + setFont -> 2x).
 * A missing/stale font pointer means the default (medium, scale 1). */
void midp_graphics_draw_string(MidpGraphics* gfx, const char* str,
                               int x, int y, int anchor) {
    MidpFont* f = gfx ? (MidpFont*)gfx->font : NULL;
    int scale = (f && f->size == FONT_SIZE_LARGE) ? 2 : 1;
    draw_string_scaled_core(gfx, str, x, y, anchor, scale);
}

/* Forced-scale variants (see midp.h): C-side callers that want the shared
 * bitmap font bigger WITHOUT touching the graphics' current font. */
void midp_graphics_draw_string_scaled(MidpGraphics* gfx, const char* str,
                                      int x, int y, int anchor, int scale) {
    draw_string_scaled_core(gfx, str, x, y, anchor, scale < 1 ? 1 : scale);
}

void midp_graphics_draw_string_2x(MidpGraphics* gfx, const char* str,
                                  int x, int y, int anchor) {
    draw_string_scaled_core(gfx, str, x, y, anchor, 2);
}

void midp_graphics_draw_image(MidpGraphics* gfx, MidpImage* img,
                              int x, int y, int anchor) {
    midp_gfx_op_enter(gfx);
    x += gfx->translate_x;
    y += gfx->translate_y;
    
    if (!img || !img->pixels) {
        return;
    }
    /* v29 TEMP: big-image blit trace */
    {
        static int di_trace = -1;
        if (di_trace < 0) di_trace = getenv("NOJME_GFXW") ? 1 : 0;
        if (di_trace && img->width * img->height >= (gfx->width * gfx->height * 3) / 4) {
            fprintf(stderr, "[GFX-W] drawImage %dx%d @(%d,%d) gfx=%p\n",
                    img->width, img->height, x, y, (void*)gfx);
        }
    }
    
    /* MIDP2 anchor constants:
     * HCENTER = 1, VCENTER = 2, LEFT = 4, RIGHT = 8, TOP = 16, BOTTOM = 32, BASELINE = 64
     */
    
    /* Apply horizontal anchor */
    if (anchor & 0x01) {        /* HCENTER */
        x -= img->width / 2;
    } else if (anchor & 0x08) { /* RIGHT */
        x -= img->width;
    }
    /* LEFT = 4 is default */
    
    /* Apply vertical anchor */
    if (anchor & 0x02) {        /* VCENTER */
        y -= img->height / 2;
    } else if (anchor & 0x20) { /* BOTTOM */
        y -= img->height;
    }
    /* TOP = 16 is default */
    
    /* Compute clipped destination region once */
    int dst_x1 = x > gfx->clip_x ? x : gfx->clip_x;
    int dst_y1 = y > gfx->clip_y ? y : gfx->clip_y;
    int dst_x2 = (x + img->width) < (gfx->clip_x + gfx->clip_width) ? 
                  (x + img->width) : (gfx->clip_x + gfx->clip_width);
    int dst_y2 = (y + img->height) < (gfx->clip_y + gfx->clip_height) ?
                 (y + img->height) : (gfx->clip_y + gfx->clip_height);
    
    /* Clamp to screen bounds */
    if (dst_x1 < 0) dst_x1 = 0;
    if (dst_y1 < 0) dst_y1 = 0;
    if (dst_x2 > gfx->width) dst_x2 = gfx->width;
    if (dst_y2 > gfx->height) dst_y2 = gfx->height;
    
    int copy_w = dst_x2 - dst_x1;
    int copy_h = dst_y2 - dst_y1;
    if (copy_w <= 0 || copy_h <= 0) return;
    
    /* Compute source region offset */
    int src_x_off = dst_x1 - x;
    int src_y_off = dst_y1 - y;
    
    /* v34.51 PINK-FIX: EXACT opacity state (see midp_image_all_opaque).
 * The old corner-sampling heuristic memcpy'd sprites whose transparent
 * color-key pixels (pink + alpha==0) were inside the region — the
 * presentation stage then showed the raw pink. */
    bool likely_opaque = midp_image_all_opaque(img);

    /* [M3G-VIEWPORT-FRESH]/[M3G-COV-OPAQUE] v36.54/v36.55: непрозрачный
     * memcpy-путь полностью детерминирует прямоугольник — полная отметка;
     * бленд-путь — пер-пиксельно, только source-пиксели с alpha==0xFF
     * (src-over с srcA==255 заменяет dst; полупрозрачные пиксели спрайта
     * базу не детерминируют — ореолы HUD v36.54). Отметка «липкая» в
     * окне bind->bind: непрозрачная база под полупрозрачным оверлеем
     * по-прежнему сеется. NOJME_M3G_COV_OPAQUE=0 — полные прямоугольники
     * v36.54. */
    if (likely_opaque || !nojme_m3g_cov_opaque_mode()) {
        nojme_m3g_cov_mark_rect(gfx, dst_x1, dst_y1, copy_w, copy_h);
    } else {
        for (int py = 0; py < copy_h; py++) {
            const uint32_t* src_row =
                img->pixels + (size_t)(src_y_off + py) * img->width + src_x_off;
            nojme_m3g_cov_mark_span(gfx, dst_x1, dst_y1 + py, src_row, copy_w);
        }
    }

    if (likely_opaque) {
        /* Fast path: memcpy per row for opaque images */
        for (int py = 0; py < copy_h; py++) {
            uint32_t *src_row = img->pixels + (src_y_off + py) * img->width + src_x_off;
            uint32_t *dst_row = gfx->pixels + (dst_y1 + py) * gfx->width + dst_x1;
            memcpy(dst_row, src_row, copy_w * sizeof(uint32_t));
        }
    } else {
        /* Alpha blending path with shift instead of division (faster on ARM).
         * v34.24: NEON src-over spans (integer-exact). */
#if RENDER_HAVE_SIMD
        for (int py = 0; py < copy_h; py++) {
            uint32_t *src_row = img->pixels + (src_y_off + py) * img->width + src_x_off;
            uint32_t *dst_row = gfx->pixels + (dst_y1 + py) * gfx->width + dst_x1;
            span_src_over_span(dst_row, src_row, copy_w);
        }
#else
        for (int py = 0; py < copy_h; py++) {
            uint32_t *src_row = img->pixels + (src_y_off + py) * img->width + src_x_off;
            uint32_t *dst_row = gfx->pixels + (dst_y1 + py) * gfx->width + dst_x1;
            for (int px = 0; px < copy_w; px++) {
                uint32_t src_color = src_row[px];
                uint8_t alpha = (src_color >> 24) & 0xFF;
                if (alpha == 255) {
                    dst_row[px] = src_color;
                } else if (alpha > 0) {
                    uint32_t dst_color = dst_row[px];
                    uint8_t inv_alpha = 255 - alpha;
                    uint8_t r = (((src_color >> 16) & 0xFF) * alpha + 
                                 ((dst_color >> 16) & 0xFF) * inv_alpha + 128) >> 8;
                    uint8_t g = (((src_color >> 8) & 0xFF) * alpha + 
                                 ((dst_color >> 8) & 0xFF) * inv_alpha + 128) >> 8;
                    uint8_t b = ((src_color & 0xFF) * alpha + 
                                 (dst_color & 0xFF) * inv_alpha + 128) >> 8;
                    dst_row[px] = ((uint32_t)(0xFF) <<  24) | (r << 16) | (g << 8) | b;
                }
            }
        }
#endif
    }
}

/* Draw a region of an image with optional transform
 * MIDP2 Transform constants:
 * TRANS_NONE = 0, TRANS_MIRROR_ROT180 = 1, TRANS_MIRROR = 2, TRANS_ROT180 = 3
 * TRANS_MIRROR_ROT270 = 4, TRANS_ROT90 = 5, TRANS_ROT270 = 6, TRANS_MIRROR_ROT90 = 7
 */
void midp_graphics_draw_region(MidpGraphics* gfx, MidpImage* img,
                               int x_src, int y_src, int w, int h,
                               int transform, int x_dest, int y_dest, int anchor) {
    midp_gfx_op_enter(gfx);
    x_dest += gfx->translate_x;
    y_dest += gfx->translate_y;
    
    if (!img || !img->pixels) return;
    if (w <= 0 || h <= 0) return;
    if (x_src < 0 || y_src < 0 || x_src + w > img->width || y_src + h > img->height) return;
    
    /* Calculate output dimensions based on transform */
    int out_w = w;
    int out_h = h;
    
    /* Rotations 90 and 270 swap width and height */
    if (transform == 5 || transform == 6 || transform == 4 || transform == 7) {
        out_w = h;
        out_h = w;
    }
    
    /* Apply anchor */
    if (anchor & 0x01) {        /* HCENTER */
        x_dest -= out_w / 2;
    } else if (anchor & 0x08) { /* RIGHT */
        x_dest -= out_w;
    }
    
    if (anchor & 0x02) {        /* VCENTER */
        y_dest -= out_h / 2;
    } else if (anchor & 0x20) { /* BOTTOM */
        y_dest -= out_h;
    }
    
    /* Fast path for TRANS_NONE: pre-compute clipped region, use memcpy for opaque */
    if (transform == 0) {
        int dx1 = x_dest > gfx->clip_x ? x_dest : gfx->clip_x;
        int dy1 = y_dest > gfx->clip_y ? y_dest : gfx->clip_y;
        int dx2 = (x_dest + w) < (gfx->clip_x + gfx->clip_width) ?
                  (x_dest + w) : (gfx->clip_x + gfx->clip_width);
        int dy2 = (y_dest + h) < (gfx->clip_y + gfx->clip_height) ?
                  (y_dest + h) : (gfx->clip_y + gfx->clip_height);
        if (dx1 < 0) dx1 = 0;
        if (dy1 < 0) dy1 = 0;
        if (dx2 > gfx->width) dx2 = gfx->width;
        if (dy2 > gfx->height) dy2 = gfx->height;
        
        int cw = dx2 - dx1;
        int ch = dy2 - dy1;
        if (cw <= 0 || ch <= 0) return;
        
        int sx_off = dx1 - x_dest;
        int sy_off = dy1 - y_dest;
        
        /* v34.51 PINK-FIX: exact per-image opacity (was: 2-corner sample
 * that memcpy'd pink color-key pixels when the region corners happened
 * to be opaque — pink boxes on screen after the alpha-dropping
 * presentation). */
        bool opaque = midp_image_all_opaque(img);
        
        if (opaque) {
            for (int py = 0; py < ch; py++) {
                memcpy(gfx->pixels + (dy1 + py) * gfx->width + dx1,
                       img->pixels + (y_src + sy_off + py) * img->width + (x_src + sx_off),
                       cw * sizeof(uint32_t));
            }
        } else {
#if RENDER_HAVE_SIMD
            /* v34.24: NEON src-over spans — integer-exact vs the scalar
             * formula below (a==255 copy / a==0 skip / else blend+128>>8). */
            for (int py = 0; py < ch; py++) {
                uint32_t *src_row = img->pixels + (y_src + sy_off + py) * img->width + (x_src + sx_off);
                uint32_t *dst_row = gfx->pixels + (dy1 + py) * gfx->width + dx1;
                span_src_over_span(dst_row, src_row, cw);
            }
#else
            for (int py = 0; py < ch; py++) {
                uint32_t *src_row = img->pixels + (y_src + sy_off + py) * img->width + (x_src + sx_off);
                uint32_t *dst_row = gfx->pixels + (dy1 + py) * gfx->width + dx1;
                for (int px = 0; px < cw; px++) {
                    uint32_t sc = src_row[px];
                    uint8_t a = (sc >> 24) & 0xFF;
                    if (a == 255) {
                        dst_row[px] = sc;
                    } else if (a > 0) {
                        uint32_t dc = dst_row[px];
                        uint8_t ia = 255 - a;
                        uint8_t r = (((sc >> 16) & 0xFF) * a + ((dc >> 16) & 0xFF) * ia + 128) >> 8;
                        uint8_t g = (((sc >> 8) & 0xFF) * a + ((dc >> 8) & 0xFF) * ia + 128) >> 8;
                        uint8_t b = ((sc & 0xFF) * a + (dc & 0xFF) * ia + 128) >> 8;
                        dst_row[px] = ((uint32_t)(0xFF) <<  24) | (r << 16) | (g << 8) | b;
                    }
                }
            }
#endif
        }
        return;
    }

    /* v34.28: row-span fast paths for the three row-preserving transforms
     * (MIRROR_ROT180=1 vertical flip, MIRROR=2 horizontal flip, ROT180=3).
     * These are the transforms games actually use per frame (flipped
     * sprites). The per-pixel switch + clip test + blend of the general
     * path below collapses to: clip once, then per-row span blits.
     *
     * Mapping (from the case table below):
     *   t=1: dst(dx,dy) = src(x_src + dx-x_dest, y_src + (y_dest+h-1-dy))
     *        — dst rows map 1:1 to src rows (reversed), columns FORWARD
     *   t=2: dst(dx,dy) = src(x_src + (x_dest+w-1-dx), y_src + dy-y_dest)
     *        — rows forward, columns REVERSED
     *   t=3: both reversed
     * Blend semantics are byte-identical to the general path (a==0 skip,
     * a==255 copy, else (sc*a + dc*ia + 128)>>8 with 0xFF output alpha) —
     * which is exactly what neon_src_over_span implements; the reversed
     * blend loop replicates the same formula per pixel. */
    if (!two_d_scalar_on() && (transform == 1 || transform == 2 || transform == 3)) {
        int dx1 = x_dest > gfx->clip_x ? x_dest : gfx->clip_x;
        int dy1 = y_dest > gfx->clip_y ? y_dest : gfx->clip_y;
        int dx2 = (x_dest + w) < (gfx->clip_x + gfx->clip_width) ?
                  (x_dest + w) : (gfx->clip_x + gfx->clip_width);
        int dy2 = (y_dest + h) < (gfx->clip_y + gfx->clip_height) ?
                  (y_dest + h) : (gfx->clip_y + gfx->clip_height);
        if (dx1 < 0) dx1 = 0;
        if (dy1 < 0) dy1 = 0;
        if (dx2 > gfx->width) dx2 = gfx->width;
        if (dy2 > gfx->height) dy2 = gfx->height;

        int cw = dx2 - dx1;
        int ch = dy2 - dy1;
        if (cw <= 0 || ch <= 0) return;

        const int col_rev = (transform == 2 || transform == 3);
        const int row_rev = (transform == 1 || transform == 3);
        /* Source mapping derived from the case table below:
         *   forward cols: src_col(dx) = x_src + (dx - x_dest)          — base at dx1
         *   mirrored cols: src_col(dx) = x_src + (x_dest + w-1 - dx)   — base at dx1, step -1
         *   forward rows: src_row(dy) = y_src + (dy - y_dest)          — base at dy1
         *   flipped rows: src_row(dy) = y_src + (y_dest + h-1 - dy)    — base at dy1, step -1
         * (The src region was already validated fully in-bounds above.) */
        const int src_c0 = col_rev ? (x_src + x_dest + w - 1 - dx1)
                                   : (x_src + (dx1 - x_dest));
        const int src_r0 = row_rev ? (y_src + y_dest + h - 1 - dy1)
                                   : (y_src + (dy1 - y_dest));
        const bool opaque = midp_image_all_opaque(img); /* v34.51: exact, was !img->alpha */
        for (int py = 0; py < ch; py++) {
            const int src_row_idx = row_rev ? (src_r0 - py) : (src_r0 + py);
            const uint32_t* src_row = img->pixels + (size_t)src_row_idx * img->width + src_c0;
            uint32_t* dst_row = gfx->pixels + (size_t)(dy1 + py) * gfx->width + dx1;
            if (!col_rev) {
                /* forward columns: opaque -> memcpy, else src-over span
                 * (NEON path is the same formula as the general loop) */
                if (opaque) {
                    memcpy(dst_row, src_row, (size_t)cw * sizeof(uint32_t));
                } else {
#if RENDER_HAVE_SIMD
                    span_src_over_span(dst_row, src_row, cw);
#else
                    for (int px = 0; px < cw; px++) {
                        uint32_t sc = src_row[px];
                        uint8_t a = (sc >> 24) & 0xFF;
                        if (a == 255) {
                            dst_row[px] = sc;
                        } else if (a > 0) {
                            uint32_t dc = dst_row[px];
                            uint8_t ia = 255 - a;
                            uint8_t r = (((sc >> 16) & 0xFF) * a + ((dc >> 16) & 0xFF) * ia + 128) >> 8;
                            uint8_t g = (((sc >> 8) & 0xFF) * a + ((dc >> 8) & 0xFF) * ia + 128) >> 8;
                            uint8_t b = ((sc & 0xFF) * a + (dc & 0xFF) * ia + 128) >> 8;
                            dst_row[px] = ((uint32_t)(0xFF) << 24) | (r << 16) | (g << 8) | b;
                        }
                    }
#endif
                }
            } else {
                /* reversed columns: dst[px] = src_row[-px] (src_cstep=-1) */
                if (opaque) {
#if RENDER_HAVE_SIMD
                    span_copy_rev32(dst_row, src_row, cw);
#else
                    for (int px = 0; px < cw; px++) dst_row[px] = src_row[-px];
#endif
                } else {
                    for (int px = 0; px < cw; px++) {
                        uint32_t sc = src_row[-px];
                        uint8_t a = (sc >> 24) & 0xFF;
                        if (a == 255) {
                            dst_row[px] = sc;
                        } else if (a > 0) {
                            uint32_t dc = dst_row[px];
                            uint8_t ia = 255 - a;
                            uint8_t r = (((sc >> 16) & 0xFF) * a + ((dc >> 16) & 0xFF) * ia + 128) >> 8;
                            uint8_t g = (((sc >> 8) & 0xFF) * a + ((dc >> 8) & 0xFF) * ia + 128) >> 8;
                            uint8_t b = ((sc & 0xFF) * a + (dc & 0xFF) * ia + 128) >> 8;
                            dst_row[px] = ((uint32_t)(0xFF) << 24) | (r << 16) | (g << 8) | b;
                        }
                    }
                }
            }
        }
        return;
    }

    /* General transform path with pre-computed clip bounds and optimized alpha blend */
    int clip_x1 = gfx->clip_x;
    int clip_y1 = gfx->clip_y;
    int clip_x2 = gfx->clip_x + gfx->clip_width;
    int clip_y2 = gfx->clip_y + gfx->clip_height;
    int scr_w = gfx->width;
    int scr_h = gfx->height;
    int drawn = 0;  /* v22 DIAG: pixels actually written */
    
    for (int py = 0; py < h; py++) {
        for (int px = 0; px < w; px++) {
            int src_x = x_src + px;
            int src_y = y_src + py;
            
            uint32_t src_color = img->pixels[src_y * img->width + src_x];
            uint8_t alpha = (src_color >> 24) & 0xFF;
            if (alpha == 0) continue;
            
            /* Calculate destination coordinates based on transform */
            int dst_px, dst_py;
            
            switch (transform) {
                /* MIDP2 Sprite Transform constants:
                 * TRANS_NONE = 0: no transform
                 * TRANS_MIRROR_ROT180 = 1: reflect horizontally (X-axis mirror), same as 180° rotation then mirror
                 * TRANS_MIRROR = 2: reflect vertically (Y-axis mirror)  
                 * TRANS_ROT180 = 3: rotate 180° clockwise
                 * TRANS_MIRROR_ROT270 = 4: reflect horizontally then rotate 90° counter-clockwise
                 * TRANS_ROT90 = 5: rotate 90° clockwise
                 * TRANS_ROT270 = 6: rotate 270° clockwise (90° counter-clockwise)
                 * TRANS_MIRROR_ROT90 = 7: reflect horizontally then rotate 90° clockwise
                 */
                case 1: /* TRANS_MIRROR_ROT180 - vertical flip (Y-axis mirror) */
                    dst_px = px; dst_py = h - 1 - py; break;
                case 2: /* TRANS_MIRROR - horizontal flip (X-axis mirror) */
                    dst_px = w - 1 - px; dst_py = py; break;
                case 3: /* TRANS_ROT180 */
                    dst_px = w - 1 - px; dst_py = h - 1 - py; break;
                case 4: /* TRANS_MIRROR_ROT270 - reflect X, then rotate 90° CCW = transpose */
                    dst_px = py; dst_py = px; break;
                case 5: /* TRANS_ROT90 - rotate 90° clockwise */
                    dst_px = h - 1 - py; dst_py = px; break;
                case 6: /* TRANS_ROT270 - rotate 270° clockwise (90° CCW) */
                    dst_px = py; dst_py = w - 1 - px; break;
                case 7: /* TRANS_MIRROR_ROT90 - reflect X, then rotate 90° CW */
                    dst_px = h - 1 - py; dst_py = w - 1 - px; break;
                default: 
                    dst_px = px; dst_py = py; break;
            }
            
            int dst_x = x_dest + dst_px;
            int dst_y = y_dest + dst_py;
            
            /* Clip check */
            if (dst_x < clip_x1 || dst_x >= clip_x2 || dst_y < clip_y1 || dst_y >= clip_y2) continue;
            if (dst_x < 0 || dst_x >= scr_w || dst_y < 0 || dst_y >= scr_h) continue;
            drawn++;
            
            /* Alpha blending with shift instead of division */
            if (alpha == 255) {
                gfx->pixels[dst_y * scr_w + dst_x] = src_color;
            } else {
                uint32_t dst_color = gfx->pixels[dst_y * scr_w + dst_x];
                uint8_t inv_alpha = 255 - alpha;
                uint8_t r = (((src_color >> 16) & 0xFF) * alpha + 
                             ((dst_color >> 16) & 0xFF) * inv_alpha + 128) >> 8;
                uint8_t g = (((src_color >> 8) & 0xFF) * alpha +
                             ((dst_color >> 8) & 0xFF) * inv_alpha + 128) >> 8;
                uint8_t b = (((src_color & 0xFF) * alpha +
                             (dst_color & 0xFF) * inv_alpha + 128)) >> 8;
                gfx->pixels[dst_y * scr_w + dst_x] = ((uint32_t)(0xFF) <<  24) | (r << 16) | (g << 8) | b;
            }
        }
    }

    /* v22 DIAG: env-gated zero-draw trace for transformed blits. A big source
     * (whole-atlas Sprite) with drawn==0 means the sprite was fully culled -
     * the #1 cause of "invisible plane" class bugs. */
    {
        static int dr_trace = -1;
        if (dr_trace < 0) dr_trace = getenv("NOJME_DR_TRACE") ? 1 : 0;
        if (dr_trace && drawn == 0 && (w * h) >= 4096) {
            static int dr_zero_logs = 0;
            if (dr_zero_logs++ < 60) {
                fprintf(stderr,
                        "[DR-ZERO] src=%dx%d@(%d,%d) xf=%d dest=(%d,%d) "
                        "out=%dx%d clip=(%d,%d,%d,%d) scr=%dx%d\n",
                        w, h, x_src, y_src, transform, x_dest, y_dest,
                        out_w, out_h,
                        gfx->clip_x, gfx->clip_y, gfx->clip_width, gfx->clip_height,
                        scr_w, scr_h);
            }
        }
    }
}

/* Copy a region within the graphics context */
void midp_graphics_copy_area(MidpGraphics* gfx, int x_src, int y_src,
                             int w, int h, int x_dest, int y_dest, int anchor) {
    midp_gfx_op_enter(gfx);
    x_src += gfx->translate_x;
    y_src += gfx->translate_y;
    x_dest += gfx->translate_x;
    y_dest += gfx->translate_y;
    
    if (w <= 0 || h <= 0) return;
    
    /* Apply anchor for destination */
    if (anchor & 0x01) {        /* HCENTER */
        x_dest -= w / 2;
    } else if (anchor & 0x08) { /* RIGHT */
        x_dest -= w;
    }
    
    if (anchor & 0x02) {        /* VCENTER */
        y_dest -= h / 2;
    } else if (anchor & 0x20) { /* BOTTOM */
        y_dest -= h;
    }
    
    /* Compute valid source region clamped to screen bounds */
    int sx1 = x_src > 0 ? x_src : 0;
    int sy1 = y_src > 0 ? y_src : 0;
    int sx2 = (x_src + w) < gfx->width ? (x_src + w) : gfx->width;
    int sy2 = (y_src + h) < gfx->height ? (y_src + h) : gfx->height;
    int cw = sx2 - sx1;
    int ch = sy2 - sy1;
    if (cw <= 0 || ch <= 0) return;
    
    /* Corresponding destination coords */
    int dx1 = x_dest + (sx1 - x_src);
    int dy1 = y_dest + (sy1 - y_src);
    
    /* Clip destination to clip region and screen */
    int clip_x2 = gfx->clip_x + gfx->clip_width;
    int clip_y2 = gfx->clip_y + gfx->clip_height;
    int adj;
    adj = gfx->clip_x - dx1; if (adj > 0) { dx1 += adj; sx1 += adj; cw -= adj; }
    adj = gfx->clip_y - dy1; if (adj > 0) { dy1 += adj; sy1 += adj; ch -= adj; }
    if (dx1 + cw > clip_x2) cw = clip_x2 - dx1;
    if (dy1 + ch > clip_y2) ch = clip_y2 - dy1;
    if (dx1 + cw > gfx->width) cw = gfx->width - dx1;
    if (dy1 + ch > gfx->height) ch = gfx->height - dy1;
    if (cw <= 0 || ch <= 0) return;
    
    /* Use a scratch buffer for safe copy (handles overlapping regions), memcpy per row.
     * v34.28: static grow-on-demand scratch instead of malloc/free per call —
     * scrolling games that copyArea() every frame paid a full heap
     * round-trip each time. All 2D primitives run under the MIDP UI lock
     * (see midp_process_repaints), so the shared scratch is race-free. */
    size_t row_bytes = (size_t)cw * sizeof(uint32_t);
    static uint32_t* copy_scratch = NULL;
    static size_t copy_scratch_cap = 0;
    size_t needed = (size_t)cw * (size_t)ch;
    if (needed > copy_scratch_cap) {
        uint32_t* grown = (uint32_t*)realloc(copy_scratch, needed * sizeof(uint32_t));
        if (!grown) return; /* allocation failure: bail like the old malloc path */
        copy_scratch = grown;
        copy_scratch_cap = needed;
    }
    uint32_t* temp = copy_scratch;
    
    for (int py = 0; py < ch; py++) {
        memcpy(temp + (size_t)py * cw,
               gfx->pixels + (sy1 + py) * gfx->width + sx1,
               row_bytes);
    }
    for (int py = 0; py < ch; py++) {
        memcpy(gfx->pixels + (dy1 + py) * gfx->width + dx1,
               temp + (size_t)py * cw,
               row_bytes);
    }
}

void midp_graphics_get_rgb(MidpGraphics* gfx, jint* rgb_data,
                           int offset, int scanlength, int x, int y, int w, int h) {
    x += gfx->translate_x;
    y += gfx->translate_y;

    /* v34.28: hoist the per-pixel bounds checks. The original loop skipped
     * out-of-bounds (src_x, src_y) individually; the in-bounds source
     * window is [sxlo,sxhi) x [sylo,syhi), each in-range row copies ONE
     * contiguous run — same elements, same values (jint = same 32 bits),
     * bit-identical to the per-pixel loop (kept for NOJME_2D_SCALAR=1). */
    if (two_d_scalar_on()) {
        for (int py = 0; py < h; py++) {
            int src_y = y + py;
            if (src_y < 0 || src_y >= gfx->height) continue;
            
            for (int px = 0; px < w; px++) {
                int src_x = x + px;
                if (src_x < 0 || src_x >= gfx->width) continue;
                
                rgb_data[offset + py * scanlength + px] = 
                    (jint)gfx->pixels[src_y * gfx->width + src_x];
            }
        }
        return;
    }

    const int sxlo = x > 0 ? x : 0;
    const int sylo = y > 0 ? y : 0;
    const int sxhi = ((long long)x + w < (long long)gfx->width) ? (x + w) : gfx->width;
    const int syhi = ((long long)y + h < (long long)gfx->height) ? (y + h) : gfx->height;
    const int pxlo = sxlo - x, pxhi = sxhi - x;
    const int pylo = sylo - y, pyhi = syhi - y;
    const int run = pxhi - pxlo; /* full row only when pxlo==0 && pxhi==w */

    for (int py = pylo; py < pyhi; py++) {
        const uint32_t* src_row = gfx->pixels + (size_t)(y + py) * gfx->width + sxlo;
        jint* dst_row = rgb_data + (size_t)offset + (size_t)py * scanlength + pxlo;
        if (run == w && run > 8) {
            memcpy(dst_row, src_row, (size_t)run * sizeof(uint32_t));
        } else {
            for (int px = 0; px < run; px++) {
                dst_row[px] = (jint)src_row[px];
            }
        }
    }
}

/* ---------------------------------------------------------------------------
 * Graphics.drawRGB pixel stage (from display.c v34.20): clip once, then
 * opaque fast path / alpha blend path. Diagnostics kept behind an internal
 * call counter so the hot loops stay untouched by the JNI layer.
 * --------------------------------------------------------------------------- */
/* v34.24: scalar per-pixel body of the drawRGB alpha path, factored out so
 * the NEON all-opaque-quad fast path and the scalar loop share one formula
 * (returns dst unchanged for the skip branches — matching `continue`). */
static inline uint32_t blit_rgb_blend_px(uint32_t dst, uint32_t src) {
    int srcA = (src >> 24) & 0xFF;
    if (srcA == 0) return dst;          /* fully transparent, skip */
    if (srcA == 0xFF) return src;       /* fully opaque, fast copy */
    int invSrcA = 255 - srcA;
    int dstA = (dst >> 24) & 0xFF;
    int outA = srcA + ((dstA * invSrcA) >> 8);
    if (outA == 0) return dst;
    int srcR = (src >> 16) & 0xFF;
    int srcG = (src >> 8) & 0xFF;
    int srcB = src & 0xFF;
    int dstR = (dst >> 16) & 0xFF;
    int dstG = (dst >> 8) & 0xFF;
    int dstB = dst & 0xFF;
    int r = ((srcR * srcA) + ((dstR * dstA * invSrcA) >> 8)) / outA;
    int g = ((srcG * srcA) + ((dstG * dstA * invSrcA) >> 8)) / outA;
    int b = ((srcB * srcA) + ((dstB * dstA * invSrcA) >> 8)) / outA;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return (uint32_t)((outA << 24) | (r << 16) | (g << 8) | b);
}

void midp_blit_rgb(MidpGraphics* gfx, const jint* rgb_data, jint src_len,
                   jint offset, jint scanlength, jint x, jint y, jint width,
                   jint height, jint processAlpha) {
    midp_gfx_op_enter(gfx);
    static long blitno = 0;
    long callno = ++blitno;

    /* Clip once OUTSIDE the loops (matching Java FreeJ2ME reference) */
    int startX = 0, startY = 0, endX = width, endY = height;
    int tx = x + gfx->translate_x;
    int ty = y + gfx->translate_y;

    if (tx < gfx->clip_x) { startX = gfx->clip_x - tx; tx = gfx->clip_x; }
    if (ty < gfx->clip_y) { startY = gfx->clip_y - ty; ty = gfx->clip_y; }
    if (tx + width > gfx->clip_x + gfx->clip_width) endX = gfx->clip_x + gfx->clip_width - tx;
    if (ty + height > gfx->clip_y + gfx->clip_height) endY = gfx->clip_y + gfx->clip_height - ty;
    if (tx < 0) { startX = -tx; tx = 0; }
    if (ty < 0) { startY = -ty; ty = 0; }
    if (tx + width > gfx->width) endX = gfx->width - tx;
    if (ty + height > gfx->height) endY = gfx->height - ty;

    if (endX <= startX || endY <= startY) {
        if (render_diag_on() && (callno <= 8 || (callno % 3600) == 0))
            fprintf(stderr, "[BLITRGB] #%ld CLIPPED-AWAY: tx=%d ty=%d endX=%d startX=%d endY=%d startY=%d\n",
                    callno, tx, ty, endX, startX, endY, startY);
        return;
    }

    /* [BT-CRASH-FIX] source-row pre-clip against src_len: compute each
     * visible row's [srcFirst, srcLast) source-index window and skip the
     * out-of-bounds head/tail instead of reading past rgb_data (adjacent
     * heap blocks / beyond the arena -> Data Abort on Switch). Per-row
     * jlong math: offset + row*scanlength can legitimately exceed int32
     * when a game passes an oversized window relying on the clip. */
    const jlong src_n = (jlong)src_len;

    int visibleWidth = endX - startX;
    int visibleHeight = endY - startY;
    int dstStride = gfx->width;
    int dstStart = ty * dstStride + tx;

    if (!processAlpha) {
        /* Fast path: no alpha, force opaque, direct buffer write
         * (v34.24: NEON 4/8-wide OR-store on ARM) */
        for (int py = 0; py < visibleHeight; py++) {
            jlong srcRow = (jlong)offset + (jlong)(startY + py) * (jlong)scanlength + (jlong)startX;
            int dstRow = dstStart + py * dstStride;
            /* visible source span for this row: [srcRow, srcRow+visibleWidth) */
            jlong sFirst = srcRow > 0 ? srcRow : 0;
            jlong sLast = srcRow + (jlong)visibleWidth;
            if (sLast > src_n) sLast = src_n;
            if (sFirst >= sLast) {
                /* whole row out of source range: leave dst untouched */
                continue;
            }
            int lead = (int)(sFirst - srcRow);          /* skipped head px */
            int npx = (int)(sLast - sFirst);            /* in-range px */
            /* [M3G-VIEWPORT-FRESH] v36.54: processAlpha=0 пишет 0xFF000000|
             * src — пиксели полностью детерминированы; отметка ТОЧНО по
             * фактически записанному диапазону строки ([BT-CRASH-FIX]
             * пре-клип может отрезать голову/хвост). */
            nojme_m3g_cov_mark_rect(gfx, tx + lead, ty + py, npx, 1);
#if RENDER_HAVE_SIMD
            span_copy_or32(&gfx->pixels[dstRow + lead],
                           (const uint32_t*)&rgb_data[sFirst], npx);
#else
            for (int px = 0; px < npx; px++) {
                gfx->pixels[dstRow + lead + px] = (uint32_t)(0xFF000000 | (rgb_data[sFirst + px] & 0x00FFFFFF));
            }
#endif
        }
    } else {
        /* v34.51: this branch stores the SOURCE alpha (outA can be < 0xFF
         * and even == 0) — a mutable destination image must drop its cached
         * exact-opacity scan, or a later drawImage could memcpy the image
         * (raw pink color-key pixels included) onto the screen. */
        if (gfx->owner_image)
            midp_image_invalidate_alpha_scan(gfx->owner_image);
        /* Alpha blending path: v34.24 NEON quads store fully-opaque runs
         * (and skip fully-transparent quads); mixed quads and the row tail
         * use the shared scalar pixel body — bit-identical output. */
        for (int py = 0; py < visibleHeight; py++) {
            jlong srcRow = (jlong)offset + (jlong)(startY + py) * (jlong)scanlength + (jlong)startX;
            int dstRow = dstStart + py * dstStride;
            jlong sFirst = srcRow > 0 ? srcRow : 0;
            jlong sLast = srcRow + (jlong)visibleWidth;
            if (sLast > src_n) sLast = src_n;
            if (sFirst >= sLast) continue;
            int lead = (int)(sFirst - srcRow);
            int npx = (int)(sLast - sFirst);
            /* [M3G-VIEWPORT-FRESH]/[M3G-COV-OPAQUE] v36.55: processAlpha=1 —
             * пер-пиксельно только (src>>24)==0xFF (ровно случай, когда
             * blit_rgb_blend_px заменяет dst); NOJME_M3G_COV_OPAQUE=0 —
             * полная прямоугольная отметка строки v36.54. */
            if (nojme_m3g_cov_opaque_mode()) {
                nojme_m3g_cov_mark_span(gfx, tx + lead, ty + py,
                                        (const uint32_t*)&rgb_data[sFirst], npx);
            } else {
                nojme_m3g_cov_mark_rect(gfx, tx + lead, ty + py, npx, 1);
            }
            int px = 0;
#if RENDER_HAVE_NEON
            while (px + 4 <= npx) {
                uint32x4_t s4 = vld1q_u32((const uint32_t*)&rgb_data[sFirst + px]);
                uint32x4_t a4 = vshrq_n_u32(s4, 24);
                uint32x4_t eq = vceqq_u32(a4, vdupq_n_u32(0xFF));
                if (vgetq_lane_u32(eq, 0) && vgetq_lane_u32(eq, 1) &&
                    vgetq_lane_u32(eq, 2) && vgetq_lane_u32(eq, 3)) {
                    vst1q_u32(&gfx->pixels[dstRow + lead + px], s4); /* all opaque */
                    px += 4;
                    continue;
                }
                if (!(vgetq_lane_u32(a4, 0) | vgetq_lane_u32(a4, 1) |
                      vgetq_lane_u32(a4, 2) | vgetq_lane_u32(a4, 3))) {
                    px += 4; /* all transparent */
                    continue;
                }
                for (int k = 0; k < 4; k++, px++) {
                    gfx->pixels[dstRow + lead + px] = blit_rgb_blend_px(
                            gfx->pixels[dstRow + lead + px], (uint32_t)rgb_data[sFirst + px]);
                }
            }
#endif
            for (; px < npx; px++) {
                gfx->pixels[dstRow + lead + px] = blit_rgb_blend_px(
                        gfx->pixels[dstRow + lead + px], (uint32_t)rgb_data[sFirst + px]);
            }
        }
    }
}

/* Full-surface blit (GameCanvas flushGraphics). */
void midp_blit_surface(uint32_t* dst, int dst_width, int dst_height,
                       const uint32_t* src, int src_width, int src_height) {
    if (!dst || !src) return;
    int copy_width  = src_width  < dst_width  ? src_width  : dst_width;
    int copy_height = src_height < dst_height ? src_height : dst_height;
    for (int y = 0; y < copy_height; y++) {
        memcpy(dst + (size_t)y * dst_width, src + (size_t)y * src_width,
               (size_t)copy_width * sizeof(uint32_t));
    }
}

/* Region blit (GameCanvas flushGraphics(x,y,w,h)): same-coordinate copy
 * clamped to both surfaces. */
void midp_blit_surface_region(uint32_t* dst, int dst_width, int dst_height,
                              const uint32_t* src, int src_width, int src_height,
                              int x, int y, int w, int h) {
    if (!dst || !src || w <= 0 || h <= 0) return;
    /* clamp region to source */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > src_width)  w = src_width  - x;
    if (y + h > src_height) h = src_height - y;
    /* clamp region to destination */
    if (x < 0) { w += x; x = 0; }   /* no-op after source clamp, kept for symmetry */
    if (y < 0) { h += y; y = 0; }
    if (x + w > dst_width)  w = dst_width  - x;
    if (y + h > dst_height) h = dst_height - y;
    if (w <= 0 || h <= 0) return;
    for (int py = y; py < y + h; py++) {
        memcpy(dst + (size_t)py * dst_width + x, src + (size_t)py * src_width + x,
               (size_t)w * sizeof(uint32_t));
    }
}

/* ============================================================================
 * SECTION 2: M3G (JSR-184) software rasterizer (from mobile3d.c)
 * ============================================================================ */

/* Global M3G context (extern in render.h; scene side mobile3d.c owns the
 * setup/reset logic that writes it). */
M3GContext g_m3g;
M3GLight g_m3g_active_lights[M3G_MAX_ACTIVE_LIGHTS];
int g_m3g_active_light_count = 0;

/* v34.23: 2D draw-operation counter for the force-render heuristic.
 * Incremented by every public 2D entry point below; reset at the start of
 * each paint() cycle (m3g_reset_paint_tracking). m3g_needs_force_render()
 * suppresses the "render the scene although bindTarget was never called"
 * fallback when the game itself drew 2D content in this paint — e.g. SU-30's
 * level-loading screen paints a progress bar and must NOT show the partially
 * loaded 3D models behind it (a real phone shows only what paint() drew). */
volatile int g_midp_gfx_draw_ops = 0;

/* Raster diagnostics */
/* Diagnostic counter for debugging */
int g_m3g_clip_triangles_in = 0;
int g_m3g_clip_triangles_out = 0;
int g_m3g_raster_pixels_written = 0;
/* v34.16 DIAG (Galaxy on Fire): count rasterizer early-exit reasons —
 * tells exactly which guard silently eats all triangles. */
long g_m3g_rj_clip2screen, g_m3g_rj_depth, g_m3g_rj_cull, g_m3g_rj_offscreen, g_m3g_rj_area, g_m3g_rj_pass;
/* v34.88 culling self-heal per-MESH counters (reset by m3g_raster_begin_mesh,
 * read by m3g_render_single_mesh after the triangle loop; safe under the M3G
 * pipeline lock, which serializes all rasterization). */
long g_m3g_mesh_facing_submitted, g_m3g_mesh_facing_culled, g_m3g_mesh_drawn;
/* v32 DIAG (NOJME_TRACE_BBOX): rasterized-pixel accumulator, reset per mesh */
int g_m3g_rast_trace_on = 0;
int g_m3g_rast_acc_x0 = 1 << 29, g_m3g_rast_acc_x1 = -(1 << 29);
int g_m3g_rast_acc_y0 = 1 << 29, g_m3g_rast_acc_y1 = -(1 << 29);

/* Clear buffers - optimized to avoid per-iteration multiplication */
M3G_HOT void m3g_clear_ex(float r, float g, float b, float a, float depth,
                                 int clear_color, int clear_depth) {
    /* v20 FIX (JSR-184 Background / FIX-21 semantics): Graphics3D.clear must
     * honor Background.colorClearEnable / depthClearEnable instead of
     * unconditionally overwriting both buffers. */
    if (!clear_color && !clear_depth) return;
    uint32_t color = ((uint32_t)(a * 255) << 24) |
                     ((uint32_t)(r * 255) << 16) |
                     ((uint32_t)(g * 255) << 8) |
                     ((uint32_t)(b * 255));
    
    /* v34.54 FIX: JSR-184 clear() clears the VIEWPORT only (the bindTarget
     * clip), not the whole canvas buffer. Clearing the full buffer with a
     * color clear would erase the game's 2D background outside the 3D
     * viewport (HUD bands, painted sky). Depth-only clears remain
     * effectively whole-buffer-safe but are restricted the same way for
     * spec conformance; the rest of the buffer keeps the seeded 2D pixels. */
    int vx0 = g_m3g.viewport_x, vy0 = g_m3g.viewport_y;
    int vw = g_m3g.viewport_width, vh = g_m3g.viewport_height;
    if (vx0 < 0) { vw += vx0; vx0 = 0; }
    if (vy0 < 0) { vh += vy0; vy0 = 0; }
    if (vx0 + vw > g_m3g.buffer_width)  vw = g_m3g.buffer_width - vx0;
    if (vy0 + vh > g_m3g.buffer_height) vh = g_m3g.buffer_height - vy0;
    if (vw <= 0 || vh <= 0) return;
    uint32_t* cb = g_m3g.color_buffer + (size_t)vy0 * g_m3g.buffer_width + vx0;
    float* db = g_m3g.depth_buffer + (size_t)vy0 * g_m3g.buffer_width + vx0;
    for (int y = 0; y < vh; y++) {
        if (clear_color && clear_depth) {
#if RENDER_HAVE_SIMD
            span_fill_u32(cb, vw, color);
            span_fill_f32(db, vw, depth);
#else
            for (int i = 0; i < vw; i++) {
                cb[i] = color;
                db[i] = depth;
            }
#endif
        } else if (clear_color) {
#if RENDER_HAVE_SIMD
            span_fill_u32(cb, vw, color);
#else
            for (int i = 0; i < vw; i++) cb[i] = color;
#endif
        } else {
#if RENDER_HAVE_SIMD
            span_fill_f32(db, vw, depth);
#else
            for (int i = 0; i < vw; i++) db[i] = depth;
#endif
        }
        cb += g_m3g.buffer_width;
        db += g_m3g.buffer_width;
    }
}

/* Convert clip coordinates to screen coordinates.
 * Returns 0 on success, -1 if vertex is behind near plane (w <= 0). */
int m3g_clip_to_screen(float* screen, const float* clip) {
    /* Perspective divide - reject vertices behind camera.
     * v24 FIX (Brick Breaker black screen): also reject on the REAL near-plane
     * criterion z + w < 0 (OpenGL convention: -w <= z <= w). Vertices between
     * the eye and the near plane have w > 0 but z + w < 0; with the old
     * w-only test they passed here and produced NDC z ~ -300000 garbage
     * depths that made the rasterizer discard whole corridor walls when the
     * camera sat inside the geometry. */
    float w = clip[3];
    if (w < 0.001f) return -1;  /* Behind the eye */
    if (clip[2] + w < 0.0f) return -1;  /* v24: in front of eye but before near plane */
    
    float ndc_x = clip[0] / w;
    float ndc_y = clip[1] / w;
    float ndc_z = clip[2] / w;
    
    /* NDC to screen */
    screen[0] = (ndc_x + 1.0f) * 0.5f * g_m3g.viewport_width + g_m3g.viewport_x;
    screen[1] = (1.0f - ndc_y) * 0.5f * g_m3g.viewport_height + g_m3g.viewport_y;  /* Y-flip */
    screen[2] = (ndc_z + 1.0f) * 0.5f;  /* Depth to [0,1] */
    screen[3] = 1.0f / w;  /* 1/w for perspective correction */
    return 0;
}

/* Near-plane clip threshold: the near plane is the OpenGL set { z + w >= 0 }.
 * v24 FIX: the old threshold tested w alone (w >= 0.001), which let vertices
 * between the eye and the near plane (w > 0, z + w < 0) through unclipped.
 * We keep a small positive w floor as a degenerate-sliver guard on top of the
 * exact plane test. */
#define M3G_NEAR_CLIP_W  0.001f
#define M3G_NEAR_CLIP_ZW 0.0f

/* Interpolate between two clip-space vertices (linearly in clip space).
 * Used by Sutherland-Hodgman clipping. Also interpolates colors and texcoords. */
static void m3g_lerp_clip(float* out_clip, uint8_t* out_color, float* out_tex,
                           const float* in_clip0, const uint8_t* in_color0, const float* in_tex0,
                           const float* in_clip1, const uint8_t* in_color1, const float* in_tex1,
                           float t) {
    float s = 1.0f - t;
    out_clip[0] = s * in_clip0[0] + t * in_clip1[0];
    out_clip[1] = s * in_clip0[1] + t * in_clip1[1];
    out_clip[2] = s * in_clip0[2] + t * in_clip1[2];
    out_clip[3] = s * in_clip0[3] + t * in_clip1[3];
    if (out_color && in_color0 && in_color1) {
        out_color[0] = (uint8_t)(s * in_color0[0] + t * in_color1[0] + 0.5f);
        out_color[1] = (uint8_t)(s * in_color0[1] + t * in_color1[1] + 0.5f);
        out_color[2] = (uint8_t)(s * in_color0[2] + t * in_color1[2] + 0.5f);
        out_color[3] = (uint8_t)(s * in_color0[3] + t * in_color1[3] + 0.5f);
    }
    if (out_tex && in_tex0 && in_tex1) {
        out_tex[0] = s * in_tex0[0] + t * in_tex1[0];
        out_tex[1] = s * in_tex0[1] + t * in_tex1[1];
    }
}

/* v34.18 FIX (audit item 1.4): FULL FRUSTUM CLIPPING.
 * The v25 code clipped only the eye plane (w >= eps) and the near plane
 * (z + w >= 0). Triangles spanning the FAR plane or the side planes relied
 * on the screen-space bounding box to reject them, but the rasterizer still
 * interpolated 1/w and NDC z across huge screen-space triangles whose
 * vertices were far outside the frustum: precision loss in the barycentric
 * 1/w interpolation, z-fighting against geometry clipped correctly, and
 * wasted fill on off-screen spans. Sutherland-Hodgman now runs against
 * ALL clip-space half-spaces (OpenGL conventions):
 *   plane 0: w - eps   >= 0   (eye plane; kills w<eps degenerates)
 *   plane 1: z + w     >= 0   (near)
 *   plane 2: w - z     >= 0   (far)
 *   plane 3: w + x     >= 0   (left)
 *   plane 4: w - x     >= 0   (right)
 *   plane 5: w + y     >= 0   (bottom)
 *   plane 6: w - y     >= 0   (top)
 * A polygon starting as a triangle gains at most one vertex per plane, so
 * 3 + 7 = 10 vertices / 8 output triangles is the hard upper bound.
 * Each pass is skipped entirely when all vertices are inside (the common
 * case), so the fast path costs 7 cheap all-inside tests.
 * Winding order is preserved by both the S-H clip and the fan triangulation,
 * so back-face culling stays correct. */
/* M3G_CLIP_MAX_* limits are declared in render/render.h (shared with the caller) */

int m3g_clip_triangle_near(const float* in_clip[3], const uint8_t* in_color[3], const float* in_tex[3],
                                    float out_clip[M3G_CLIP_MAX_TRIS * 3][4], uint8_t out_color[M3G_CLIP_MAX_TRIS * 3][4],
                                    float out_tex[M3G_CLIP_MAX_TRIS * 3][2],
                                    const float* in_tex1[3], float out_tex1[M3G_CLIP_MAX_TRIS * 3][2],
                                    const float* in_fog[3], float out_fog[M3G_CLIP_MAX_TRIS * 3]) {
    float poly_c[M3G_CLIP_MAX_POLY][4];
    uint8_t poly_col[M3G_CLIP_MAX_POLY][4];
    float poly_tex[M3G_CLIP_MAX_POLY][2];
    float poly_tex1[M3G_CLIP_MAX_POLY][2];
    float poly_fog[M3G_CLIP_MAX_POLY];
    float tmp_c[M3G_CLIP_MAX_POLY][4];
    uint8_t tmp_col[M3G_CLIP_MAX_POLY][4];
    float tmp_tex[M3G_CLIP_MAX_POLY][2];
    float tmp_tex1[M3G_CLIP_MAX_POLY][2];
    float tmp_fog[M3G_CLIP_MAX_POLY];
    /* v34.35 PERF (⑦ pointer swap): the old loop ended every S-H pass with
     * 5 memcpys poly<-tmp (up to 5 × ~120B per pass × 7 passes per
     * triangle). Double-buffer with pointers instead: each pass writes to
     * nxt_* and we swap cur/nxt. Same data, same order — bit-identical. */
    float (*cur_c)[4]  = poly_c,  (*nxt_c)[4]  = tmp_c;
    uint8_t (*cur_col)[4] = poly_col, (*nxt_col)[4] = tmp_col;
    float (*cur_tex)[2] = poly_tex, (*nxt_tex)[2] = tmp_tex;
    float (*cur_tex1)[2] = poly_tex1, (*nxt_tex1)[2] = tmp_tex1;
    float *cur_fog = poly_fog, *nxt_fog = tmp_fog;
    int has_color = (in_color[0] != NULL);
    int has_tex = (in_tex[0] != NULL);
    /* v30: second texture-coordinate set (multitexturing). Clipped with the
     * same interpolation parameters as unit 0 — the S-H t values depend only
     * on the clip-space geometry, which is identical for both UV sets. */
    int has_tex1 = (in_tex1 != NULL && in_tex1[0] != NULL);
    /* v34.18 (audit item 1.3): per-vertex VIEW-SPACE DEPTH for fog —
     * interpolated linearly in clip space (it is affine along the segment,
     * so the S-H t is exact). Perspective cameras previously recovered the
     * distance from 1/w; for PARALLEL cameras w is constant 1.0 and that
     * path produced a constant fog distance (fog wall / no fog at all). */
    int has_fog = (in_fog != NULL && in_fog[0] != NULL && in_fog[1] != NULL && in_fog[2] != NULL);

    /* Seed the working polygon with the input triangle (cyclic order kept) */
    int n = 3;
    for (int i = 0; i < 3; i++) {
        memcpy(cur_c[i], in_clip[i], 4 * sizeof(float));
        if (has_color) memcpy(cur_col[i], in_color[i], 4);
        if (has_tex) memcpy(cur_tex[i], in_tex[i], 2 * sizeof(float));
        if (has_tex1) memcpy(cur_tex1[i], in_tex1[i], 2 * sizeof(float));
        if (has_fog) cur_fog[i] = in_fog[i][0];
    }

    /* v34.18: 7 clip-space half-space passes — eye, near, far, left, right,
     * bottom, top (see the table in the comment above). Pass 0 keeps the
     * v25 degenerate-sliver w floor; all later passes use exact planes. */
    for (int pass = 0; pass < 7; pass++) {
        float eps = (pass == 0) ? M3G_NEAR_CLIP_W : 0.0f;
        /* fast path: skip the pass when the whole polygon is inside */
        int all_in = 1;
        for (int i = 0; i < n; i++) {
            const float* v = cur_c[i];
            float d;
            switch (pass) {
                case 0: d = v[3] - eps;  break;   /* eye    */
                case 1: d = v[2] + v[3]; break;   /* near   */
                case 2: d = v[3] - v[2]; break;   /* far    */
                case 3: d = v[3] + v[0]; break;   /* left   */
                case 4: d = v[3] - v[0]; break;   /* right  */
                case 5: d = v[3] + v[1]; break;   /* bottom */
                default: d = v[3] - v[1]; break;  /* top    */
            }
            if (d < 0.0f) { all_in = 0; break; }
        }
        if (all_in) continue;

        int m = 0;
        for (int i = 0; i < n; i++) {
            int j = (i + 1) % n;
            const float* a = cur_c[i];
            const float* b = cur_c[j];
            float da, db;
            switch (pass) {
                case 0: da = a[3] - eps;  db = b[3] - eps;  break;
                case 1: da = a[2] + a[3]; db = b[2] + b[3]; break;
                case 2: da = a[3] - a[2]; db = b[3] - b[2]; break;
                case 3: da = a[3] + a[0]; db = b[3] + b[0]; break;
                case 4: da = a[3] - a[0]; db = b[3] - b[0]; break;
                case 5: da = a[3] + a[1]; db = b[3] + b[1]; break;
                default: da = a[3] - a[1]; db = b[3] - b[1]; break;
            }
            int a_in = (da >= 0.0f);
            int b_in = (db >= 0.0f);
            if (a_in) {
                memcpy(nxt_c[m], a, 4 * sizeof(float));
                if (has_color) memcpy(nxt_col[m], cur_col[i], 4);
                if (has_tex) memcpy(nxt_tex[m], cur_tex[i], 2 * sizeof(float));
                if (has_tex1) memcpy(nxt_tex1[m], cur_tex1[i], 2 * sizeof(float));
                if (has_fog) nxt_fog[m] = cur_fog[i];
                m++;
            }
            if (a_in != b_in) {
                float t = da / (da - db);
                m3g_lerp_clip(nxt_c[m],
                              has_color ? nxt_col[m] : NULL,
                              has_tex ? nxt_tex[m] : NULL,
                              a, has_color ? cur_col[i] : NULL, has_tex ? cur_tex[i] : NULL,
                              b, has_color ? cur_col[j] : NULL, has_tex ? cur_tex[j] : NULL,
                              t);
                if (has_tex1) {
                    /* interpolate unit-1 uvs with the SAME t */
                    float s1v = 1.0f - t;
                    nxt_tex1[m][0] = s1v * cur_tex1[i][0] + t * cur_tex1[j][0];
                    nxt_tex1[m][1] = s1v * cur_tex1[i][1] + t * cur_tex1[j][1];
                }
                if (has_fog) {
                    /* view-space depth is affine along the segment: the S-H
                     * t is exact (v34.18 audit item 1.3). */
                    float s1v = 1.0f - t;
                    nxt_fog[m] = s1v * cur_fog[i] + t * cur_fog[j];
                }
                m++;
            }
            if (m > M3G_CLIP_MAX_POLY) return 0;  /* safety; unreachable */
        }
        if (m == 0) return 0;
        /* v34.35: swap cur/nxt instead of copying tmp back into poly. */
        {
            float (*tc)[4] = cur_c;   cur_c = nxt_c;   nxt_c = tc;
            uint8_t (*tl)[4] = cur_col; cur_col = nxt_col; nxt_col = tl;
            float (*tt)[2] = cur_tex; cur_tex = nxt_tex; nxt_tex = tt;
            float (*tt1)[2] = cur_tex1; cur_tex1 = nxt_tex1; nxt_tex1 = tt1;
            float* tf = cur_fog; cur_fog = nxt_fog; nxt_fog = tf;
        }
        n = m;
    }

    /* Fan-triangulate the clipped polygon: (p0, pi, pi+1) */
    int num_tris = n - 2;
    if (num_tris > M3G_CLIP_MAX_TRIS) num_tris = M3G_CLIP_MAX_TRIS;
    for (int ct = 0; ct < num_tris; ct++) {
        memcpy(out_clip[ct * 3 + 0], cur_c[0], 4 * sizeof(float));
        memcpy(out_clip[ct * 3 + 1], cur_c[ct + 1], 4 * sizeof(float));
        memcpy(out_clip[ct * 3 + 2], cur_c[ct + 2], 4 * sizeof(float));
        if (has_color) {
            memcpy(out_color[ct * 3 + 0], cur_col[0], 4);
            memcpy(out_color[ct * 3 + 1], cur_col[ct + 1], 4);
            memcpy(out_color[ct * 3 + 2], cur_col[ct + 2], 4);
        }
        if (has_tex) {
            memcpy(out_tex[ct * 3 + 0], cur_tex[0], 2 * sizeof(float));
            memcpy(out_tex[ct * 3 + 1], cur_tex[ct + 1], 2 * sizeof(float));
            memcpy(out_tex[ct * 3 + 2], cur_tex[ct + 2], 2 * sizeof(float));
        }
        if (has_tex1 && out_tex1) {
            memcpy(out_tex1[ct * 3 + 0], cur_tex1[0], 2 * sizeof(float));
            memcpy(out_tex1[ct * 3 + 1], cur_tex1[ct + 1], 2 * sizeof(float));
            memcpy(out_tex1[ct * 3 + 2], cur_tex1[ct + 2], 2 * sizeof(float));
        }
        if (has_fog && out_fog) {
            out_fog[ct * 3 + 0] = cur_fog[0];
            out_fog[ct * 3 + 1] = cur_fog[ct + 1];
            out_fog[ct * 3 + 2] = cur_fog[ct + 2];
        }
    }
    return num_tris;
}

/* Edge function for triangle rasterization */
static inline float m3g_edge_function(const float* a, const float* b, const float* c) {
    return (c[0] - a[0]) * (b[1] - a[1]) - (c[1] - a[1]) * (b[0] - a[0]);
}

/* Compute barycentric coordinates */
__attribute__((unused))
static void m3g_barycentric(float* bary, const float* v0, const float* v1, 
                            const float* v2, float x, float y) {
    float area = m3g_edge_function(v0, v1, v2);
    if (fabsf(area) < 0.0001f) {
        bary[0] = bary[1] = bary[2] = 0;
        return;
    }
    
    float p[2] = {x, y};
    bary[0] = m3g_edge_function(v1, v2, p) / area;
    bary[1] = m3g_edge_function(v2, v0, p) / area;
    bary[2] = m3g_edge_function(v0, v1, p) / area;
}

/* Simple color interpolation.
 * v14 FIX: perspective-correct — colors are interpolated as c/w divided by
 * interpolated 1/w. Screen-linear color interpolation caused visible color
 * distortion across large faces spanning depth (M3GTest scenes). */
static void m3g_interpolate_color(uint8_t* result,
                                   const uint8_t* c0, const uint8_t* c1, const uint8_t* c2,
                                   float w0, float w1, float w2,
                                   float iw0, float iw1, float iw2) {
    float denom = iw0 * w0 + iw1 * w1 + iw2 * w2;
    if (denom <= 1e-9f) {
        /* Degenerate (shouldn't happen: all iw > 0, barycentrics >= 0) */
        for (int i = 0; i < 4; i++) {
            result[i] = (uint8_t)(c0[i] * w0 + c1[i] * w1 + c2[i] * w2);
        }
        return;
    }
    for (int i = 0; i < 4; i++) {
        float v = (c0[i] * iw0 * w0 + c1[i] * iw1 * w1 + c2[i] * iw2 * w2) / denom;
        if (v < 0.0f) v = 0.0f;
        if (v > 255.0f) v = 255.0f;
        result[i] = (uint8_t)(v + 0.5f);
    }
}

/* Interpolate texture coordinates.
 * v14 CRITICAL FIX: this was the root cause of "black cube" (Nescube) and
 * wrong/stretched textures (SU30, M3GTest scene 4). The old code
 * interpolated RAW u,v linearly in SCREEN space and then multiplied by the
 * interpolated 1/w:  u = (Σ bary_i*u_i) * (Σ bary_i*1/W_i)  — which both
 * scales the whole UV set by ~1/W (at view distance 30 the entire texture
 * collapsed into the first 3% of texels) and distorts it nonlinearly
 * across the face ("back faces look bigger than front faces").
 *
 * Correct perspective-correct interpolation:
 *   u = (Σ bary_i * u_i/W_i) / (Σ bary_i * 1/W_i)
 * where iw0/iw1/iw2 are the per-vertex 1/w_clip values (screen[3] from
 * m3g_clip_to_screen). */
static void m3g_interpolate_texcoord(float* result,
                                      const float* t0, const float* t1, const float* t2,
                                      float w0, float w1, float w2,
                                      float iw0, float iw1, float iw2) {
    float denom = iw0 * w0 + iw1 * w1 + iw2 * w2;
    if (denom <= 1e-9f) {
        result[0] = t0[0];
        result[1] = t0[1];
        return;
    }
    result[0] = (t0[0] * iw0 * w0 + t1[0] * iw1 * w1 + t2[0] * iw2 * w2) / denom;
    result[1] = (t0[1] * iw0 * w0 + t1[1] * iw1 * w1 + t2[1] * iw2 * w2) / denom;
}

/* ============================================================================
 * v34.79 texture smoothing (core option j2me_texture_smoothing)
 * ============================================================================
 * The global filter override lives here; see render.h for the value map.
 * libretro's update_variables writes it directly; headless builds resolve
 * it lazily from NOJME_TEXTURE_FILTER (nearest|smooth, default auto). */
int g_m3g_texture_filter_override = -1;

static int m3g_texture_filter_override_resolved(void) {
    if (g_m3g_texture_filter_override < 0) {
        const char* e = getenv("NOJME_TEXTURE_FILTER");
        int v = 0;
        if (e) {
            if (strcmp(e, "nearest") == 0 || strcmp(e, "off") == 0) v = 1;
            else if (strcmp(e, "smooth") == 0 || strcmp(e, "bilinear") == 0 ||
                     strcmp(e, "linear") == 0 || strcmp(e, "on") == 0) v = 2;
        }
        g_m3g_texture_filter_override = v;
    }
    return g_m3g_texture_filter_override;
}

/* v34.79: per-channel two-stage bilinear lerp with 7-bit weights.
 * Pure integer, every intermediate <= 255*128 — u16-safe by construction,
 * which is what lets the NEON/SSE2 spans replicate the sequence in 16-bit
 * lanes BIT-EXACTLY (integer ops have no rounding drift). The weight pair
 * sums to 128, so (c00*(128-wx) + c10*wx) is a weighted average that can
 * never exceed 255*128 before the >>7. */
static inline uint32_t m3g_bilerp_chan(uint32_t t00, uint32_t t10,
                                       uint32_t t01, uint32_t t11,
                                       int sh, int wx, int wy) {
    const uint32_t c00 = (t00 >> sh) & 0xFF, c10 = (t10 >> sh) & 0xFF;
    const uint32_t c01 = (t01 >> sh) & 0xFF, c11 = (t11 >> sh) & 0xFF;
    const uint32_t a0 = (c00 * (uint32_t)(128 - wx) + c10 * (uint32_t)wx) >> 7;
    const uint32_t a1 = (c01 * (uint32_t)(128 - wx) + c11 * (uint32_t)wx) >> 7;
    return (a0 * (uint32_t)(128 - wy) + a1 * (uint32_t)wy) >> 7;
}

/* v34.79: SMOOTH-mode reference sampler — integer bilinear (fixed point).
 * u and v must already be wrapped/clamped to [0, 0.999999] by the caller
 * (m3g_sample_texture does the wrap before dispatching here). The float
 * prologue is exactly two mul/sub + clamps + one truncating conversion per
 * axis — then everything is integer: ~3x fewer per-texel operations than
 * the legacy float bilinear (which stays untouched for AUTO mode so the
 * default remains bit-identical to v34.78). The NEON/SSE2 span bilinear
 * (m3g_span4_neon / m3g_span4_sse2, tex_smooth block) replays this exact
 * operation sequence per lane; NOJME_NEON_SELFTEST A/B-verifies that. */
static uint32_t m3g_sample_texture_smooth(const M3GTexture2D* tex, float u, float v) {
    const int tw = tex->width, th = tex->height;
    float fx = u * (float)tw - 0.5f;
    float fy = v * (float)th - 0.5f;
    if (fx < 0.0f) fx = 0.0f;
    if (fy < 0.0f) fy = 0.0f;
    const int x0 = (int)fx;
    const int y0 = (int)fy;
    float frx = fx - (float)x0;   /* exact: fx <= 1023.5 has <= 24 sig bits */
    float fry = fy - (float)y0;
    if (frx > 1.0f) frx = 1.0f;  /* dead for wrapped u,v; belt+suspenders */
    if (fry > 1.0f) fry = 1.0f;
    int wx = (int)(frx * 128.0f);
    int wy = (int)(fry * 128.0f);
    int x1 = x0 + 1;
    int y1 = y0 + 1;
    /* u <= 0.999999 and the fx>=0 clamp keep x0/y0 in [0, dim-1]; only the
     * +1 neighbors need clamping (identical to the legacy float path). */
    if (x1 > tw - 1) x1 = tw - 1;
    if (y1 > th - 1) y1 = th - 1;

    const uint32_t* px = tex->pixels;
    const uint32_t t00 = px[(size_t)y0 * tw + x0];
    const uint32_t t10 = px[(size_t)y0 * tw + x1];
    const uint32_t t01 = px[(size_t)y1 * tw + x0];
    const uint32_t t11 = px[(size_t)y1 * tw + x1];

    return m3g_bilerp_chan(t00, t10, t01, t11,  0, wx, wy)
         | (m3g_bilerp_chan(t00, t10, t01, t11,  8, wx, wy) <<  8)
         | (m3g_bilerp_chan(t00, t10, t01, t11, 16, wx, wy) << 16)
         | (m3g_bilerp_chan(t00, t10, t01, t11, 24, wx, wy) << 24);
}

/* Sample texture (bilinear filtering) */
static uint32_t m3g_sample_texture(M3GTexture2D* tex, float u, float v) {
    if (!tex || !tex->pixels || tex->width <= 0 || tex->height <= 0) return 0xFFFFFFFF;
    
    /* Guard against NaN / Inf from extreme perspective division.
     * When w is near zero or negative, UV interpolation produces NaN,
     * and (int)NaN is undefined behavior (typically INT_MIN on x86),
     * leading to out-of-bounds access and SIGSEGV. */
    if (u != u || v != v ||           /* isnan */
        u == 1.0f/0.0f || u == -1.0f/0.0f ||  /* isinf */
        v == 1.0f/0.0f || v == -1.0f/0.0f) {
        return 0xFFFFFFFF;  /* Return white for invalid coords */
    }
    
    /* v23: wrap modes — internal blend_s/blend_t: 1 = WRAP_CLAMP(240),
     * 0 = WRAP_REPEAT(241, legacy default). */
    if (tex->blend_s) {
        if (u < 0.0f) u = 0.0f;
        if (u > 1.0f) u = 0.999999f;
    } else {
        u = u - floorf(u);
        if (u < 0) u += 1.0f;
        if (u >= 1.0f) u = 0.999999f;
    }
    if (tex->blend_t) {
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 0.999999f;
    } else {
        v = v - floorf(v);
        if (v < 0) v += 1.0f;
        if (v >= 1.0f) v = 0.999999f;
    }
    
    /* v23: FILTER_LINEAR(209)/FILTER_TRILINEAR(211) → bilinear on the base
     * level; FILTER_BASE_LEVEL(208)/legacy → nearest (unchanged path).
     * v34.79 (j2me_texture_smoothing): the override decides FIRST —
     * smooth(2) routes to the fast integer bilinear, nearest(1) skips
     * filtering entirely; auto(0) keeps the legacy per-texture dispatch
     * below (bit-identical to v34.78). */
    const int fov = m3g_texture_filter_override_resolved();
    if (fov == 2) {
        return m3g_sample_texture_smooth(tex, u, v);
    }
    if (fov != 1 && (tex->filter_level == 209 || tex->filter_level == 211)) {
        float fx = u * (float)tex->width - 0.5f;
        float fy = v * (float)tex->height - 0.5f;
        if (fx < 0.0f) fx = 0.0f;
        if (fy < 0.0f) fy = 0.0f;
        int x0 = (int)fx;
        int y0 = (int)fy;
        int x1 = x0 + 1;
        int y1 = y0 + 1;
        if (x0 >= tex->width) x0 = tex->width - 1;
        if (y0 >= tex->height) y0 = tex->height - 1;
        if (x1 >= tex->width) x1 = tex->width - 1;
        if (y1 >= tex->height) y1 = tex->height - 1;
        float frac_x = fx - (float)x0;
        float frac_y = fy - (float)y0;
        if (frac_x < 0.0f) frac_x = 0.0f;
        if (frac_x > 1.0f) frac_x = 1.0f;
        if (frac_y < 0.0f) frac_y = 0.0f;
        if (frac_y > 1.0f) frac_y = 1.0f;

        const uint32_t* px = tex->pixels;
        uint32_t t00 = px[y0 * tex->width + x0];
        uint32_t t10 = px[y0 * tex->width + x1];
        uint32_t t01 = px[y1 * tex->width + x0];
        uint32_t t11 = px[y1 * tex->width + x1];

        float r0 = (float)(t00 & 0xFF) + ((float)(t10 & 0xFF) - (float)(t00 & 0xFF)) * frac_x;
        float g0 = (float)((t00 >> 8) & 0xFF) + ((float)((t10 >> 8) & 0xFF) - (float)((t00 >> 8) & 0xFF)) * frac_x;
        float b0 = (float)((t00 >> 16) & 0xFF) + ((float)((t10 >> 16) & 0xFF) - (float)((t00 >> 16) & 0xFF)) * frac_x;
        float a0 = (float)((t00 >> 24) & 0xFF) + ((float)((t10 >> 24) & 0xFF) - (float)((t00 >> 24) & 0xFF)) * frac_x;
        float r1 = (float)(t01 & 0xFF) + ((float)(t11 & 0xFF) - (float)(t01 & 0xFF)) * frac_x;
        float g1 = (float)((t01 >> 8) & 0xFF) + ((float)((t11 >> 8) & 0xFF) - (float)((t01 >> 8) & 0xFF)) * frac_x;
        float b1 = (float)((t01 >> 16) & 0xFF) + ((float)((t11 >> 16) & 0xFF) - (float)((t01 >> 16) & 0xFF)) * frac_x;
        float a1 = (float)((t01 >> 24) & 0xFF) + ((float)((t11 >> 24) & 0xFF) - (float)((t01 >> 24) & 0xFF)) * frac_x;

        int r = (int)(r0 + (r1 - r0) * frac_y + 0.5f);
        int g = (int)(g0 + (g1 - g0) * frac_y + 0.5f);
        int b = (int)(b0 + (b1 - b0) * frac_y + 0.5f);
        int a = (int)(a0 + (a1 - a0) * frac_y + 0.5f);
        if (r < 0) r = 0;
        if (r > 255) r = 255;
        if (g < 0) g = 0;
        if (g > 255) g = 255;
        if (b < 0) b = 0;
        if (b > 255) b = 255;
        if (a < 0) a = 0;
        if (a > 255) a = 255;
        /* Texture pixel layout is ARGB (A<<24 | R<<16 | G<<8 | B) — same as
         * the Image2D builders above. */
        return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
    
    /* Nearest neighbor (simple version) */
    int x = (int)(u * tex->width);
    int y = (int)(v * tex->height);
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= tex->width) x = tex->width - 1;
    if (y >= tex->height) y = tex->height - 1;
    
    return tex->pixels[y * tex->width + x];
}

/* Task 16-a: Gouraud (per-vertex) lighting, JSR-184 §Light/Material semantics.
 * position/normal are in VIEW (eye) space; V is (0,0,1).
 *   result = emissive
 *          + sum(AMBIENT lights) * material.ambient
 *          + sum(other lights) * [ diffuse * max(0,N.L) + specular * max(0,N.H)^shininess ]
 *            * lightColor * intensity * distanceAttenuation [ * spotFactor ]
 * Only called when g_m3g_active_light_count > 0 (see the unlit-path guard in
 * m3g_render_single_mesh) — with no lights the legacy vertex color path is
 * used untouched, keeping Nescube/M3GTest output byte-identical. */
void m3g_light_vertex(float* result, const float* position,
                              const float* normal, const M3GMaterial* mat,
                              int two_sided) {
    /* Start with emissive; AMBIENT-type lights add material.ambient below. */
    result[0] = mat->emissive[0];
    result[1] = mat->emissive[1];
    result[2] = mat->emissive[2];

    float N[3] = {normal[0], normal[1], normal[2]};
    m3g_vec3_normalize(N);

    for (int i = 0; i < g_m3g_active_light_count && i < M3G_MAX_ACTIVE_LIGHTS; i++) {
        const M3GLight* light = &g_m3g_active_lights[i];

        float lr = light->color[0] * light->color[3]; /* color * intensity */
        float lg = light->color[1] * light->color[3];
        float lb = light->color[2] * light->color[3];

        if (light->type == M3G_LIGHT_AMBIENT) {
            result[0] += lr * mat->ambient[0];
            result[1] += lg * mat->ambient[1];
            result[2] += lb * mat->ambient[2];
            continue;
        }

        float L[3];        /* Unit vector towards the light (view space) */
        float attenuation = 1.0f;
        float spot_factor = 1.0f;

        if (light->type == M3G_LIGHT_DIRECTIONAL) {
            L[0] = -light->view_dir[0];
            L[1] = -light->view_dir[1];
            L[2] = -light->view_dir[2];
            m3g_vec3_normalize(L);
        } else if (light->type == M3G_LIGHT_OMNI || light->type == M3G_LIGHT_SPOT) {
            L[0] = light->view_pos[0] - position[0];
            L[1] = light->view_pos[1] - position[1];
            L[2] = light->view_pos[2] - position[2];
            float dist = sqrtf(L[0]*L[0] + L[1]*L[1] + L[2]*L[2]);
            if (dist > 0.0001f) {
                L[0] /= dist; L[1] /= dist; L[2] /= dist;
            } else {
                continue;
            }
            /* JSR-184 distance attenuation 1/(kc + kl*d + kq*d^2) */
            float kc = light->attenuation[0];
            float kl = light->attenuation[1];
            float kq = light->attenuation[2];
            float denom = kc + kl * dist + kq * dist * dist;
            if (denom <= 0.0001f) denom = 1.0f; /* all-zero params: no falloff */
            attenuation = 1.0f / denom;

            if (light->type == M3G_LIGHT_SPOT) {
                /* Spot cone: light shines along view_dir; only fragments
                 * within spotAngle of the axis are lit (degrees). */
                float spot_dir[3] = { light->view_dir[0], light->view_dir[1], light->view_dir[2] };
                m3g_vec3_normalize(spot_dir);
                float cos_axis = -(L[0]*spot_dir[0] + L[1]*spot_dir[1] + L[2]*spot_dir[2]);
                float half_angle = light->spot_angle * 0.5f * 3.14159265f / 180.0f;
                float cos_cutoff = cosf(half_angle);
                if (cos_axis < cos_cutoff) {
                    continue; /* Outside the cone */
                }
                if (light->spot_exponent > 0.0f && cos_axis > 0.0f) {
                    spot_factor = m3g_fast_specular(cos_axis, light->spot_exponent);
                }
            }
        } else {
            continue; /* Unknown type */
        }

        /* Diffuse */
        float NdotL = m3g_vec3_dot(N, L);
        if (two_sided && NdotL < 0.0f) NdotL = -NdotL;
        {   /* v31 DIAG (M3G_TRACE_LIGHT=1) */
            static int ld = -1;
            if (ld < 0) ld = getenv("M3G_TRACE_LIGHT") ? 24 : 0;
            if (ld > 0) { ld--;
                fprintf(stderr, "[M3G-LTDIAG] type=%d N=(%.2f,%.2f,%.2f) L=(%.2f,%.2f,%.2f) NdotL=%.3f lr=%.2f dif=(%.2f,%.2f,%.2f) vpos=(%.1f,%.1f,%.1f) lpos=(%.1f,%.1f,%.1f)\n",
                        light->type, N[0],N[1],N[2], L[0],L[1],L[2], NdotL, lr,
                        mat->diffuse[0],mat->diffuse[1],mat->diffuse[2],
                        position[0],position[1],position[2],
                        light->view_pos[0],light->view_pos[1],light->view_pos[2]);
            }
        }
        if (NdotL > 0.0f) {
            result[0] += lr * mat->diffuse[0] * NdotL * attenuation * spot_factor;
            result[1] += lg * mat->diffuse[1] * NdotL * attenuation * spot_factor;
            result[2] += lb * mat->diffuse[2] * NdotL * attenuation * spot_factor;
        }

        /* Specular: view vector per JSR-184 §PolygonMode.
         * v34.18 (audit 2.4): localCameraLighting computes V from the actual
         * vertex position (view space: camera at origin -> V = -position);
         * the default keeps the infinite-viewer approximation V=(0,0,1). */
        float V[3] = {0, 0, 1};
        if (mat->local_camera) {
            V[0] = -position[0]; V[1] = -position[1]; V[2] = -position[2];
            m3g_vec3_normalize(V);
        }
        float H[3];
        H[0] = L[0] + V[0];
        H[1] = L[1] + V[1];
        H[2] = L[2] + V[2];
        m3g_vec3_normalize(H);

        float NdotH = m3g_vec3_dot(N, H);
        if (two_sided && NdotH < 0.0f) NdotH = -NdotH;
        if (NdotH > 0.0f) {
            float spec = m3g_fast_specular(NdotH, mat->shininess);
            result[0] += lr * mat->specular[0] * spec * attenuation * spot_factor;
            result[1] += lg * mat->specular[1] * spec * attenuation * spot_factor;
            result[2] += lb * mat->specular[2] * spec * attenuation * spot_factor;
        }
    }

    /* Clamp */
    result[0] = result[0] < 0.0f ? 0.0f : (result[0] > 1.0f ? 1.0f : result[0]);
    result[1] = result[1] < 0.0f ? 0.0f : (result[1] > 1.0f ? 1.0f : result[1]);
    result[2] = result[2] < 0.0f ? 0.0f : (result[2] > 1.0f ? 1.0f : result[2]);
}

/* Pixel blending modes matching FreeJ2ME Graphics3D.blendPixels */
static uint32_t m3g_blend_pixels(uint32_t bg, uint32_t fg, int alpha, int blend_mode) {
    int bgA = (bg >> 24) & 0xFF;
    int bgR = (bg >> 16) & 0xFF;
    int bgG = (bg >> 8) & 0xFF;
    int bgB = bg & 0xFF;

    int fgA = (fg >> 24) & 0xFF;
    int fgR = (fg >> 16) & 0xFF;
    int fgG = (fg >> 8) & 0xFF;
    int fgB = fg & 0xFF;

    int outR, outG, outB, outA;
    float alphaNorm = alpha / 255.0f;

    #define CLAMP8(v) ((v) < 0 ? 0 : ((v) > 255 ? 255 : (v)))

    switch (blend_mode) {
        /* CompositingMode.REPLACE / Texture2D.FUNC_REPLACE */
        case 67:
            outA = CLAMP8((int)(fgA + bgA * (1 - fgA / 255.0f)));
            outR = CLAMP8((int)(fgR * (fgA / 255.0f) + bgR * (1 - fgA / 255.0f)));
            outG = CLAMP8((int)(fgG * (fgA / 255.0f) + bgG * (1 - fgA / 255.0f)));
            outB = CLAMP8((int)(fgB * (fgA / 255.0f) + bgB * (1 - fgA / 255.0f)));
            return ((uint32_t)(outA) <<  24) | (outR << 16) | (outG << 8) | outB;

        /* CompositingMode.ALPHA_ADD */
        case 65:
            outR = (int)fminf(255.0f, fgR * alphaNorm + bgR);
            outG = (int)fminf(255.0f, fgG * alphaNorm + bgG);
            outB = (int)fminf(255.0f, fgB * alphaNorm + bgB);
            outA = (int)fminf(255.0f, bgA + (int)(alpha * (1 - bgA / 255.0f)));
            return ((uint32_t)(outA) <<  24) | (outR << 16) | (outG << 8) | outB;

        /* CompositingMode.ALPHA / Texture2D.FUNC_BLEND */
        case 64:
            outR = CLAMP8((int)((fgR * alphaNorm) + (bgR * (1 - alphaNorm))));
            outG = CLAMP8((int)((fgG * alphaNorm) + (bgG * (1 - alphaNorm))));
            outB = CLAMP8((int)((fgB * alphaNorm) + (bgB * (1 - alphaNorm))));
            outA = CLAMP8((int)(bgA * (1 - alphaNorm) + fgA * alphaNorm));
            return ((uint32_t)(outA) <<  24) | (outR << 16) | (outG << 8) | outB;

        /* CompositingMode.MODULATE / Texture2D.FUNC_MODULATE
         * v31 FIX: alpha was max(bgA, fgA) — an opaque vertex color (255)
         * forced EVERY textured fragment opaque, killing alpha textures
         * (glow panes, particle sprites, masked decals rendered solid).
         * JSR-184 MODULATE multiplies: A' = A_bg * A_fg / 255. */
        case 68:
            outR = (fgR * bgR) / 255;
            outG = (fgG * bgG) / 255;
            outB = (fgB * bgB) / 255;
            outA = (bgA * fgA) / 255;
            return ((uint32_t)(CLAMP8(outA)) <<  24) | (CLAMP8(outR) << 16) | (CLAMP8(outG) << 8) | CLAMP8(outB);

        /* CompositingMode.MODULATE_X2 */
        case 69:
            outR = (2 * fgR * bgR) / 255;
            outG = (2 * fgG * bgG) / 255;
            outB = (2 * fgB * bgB) / 255;
            outA = (bgA * fgA) / 255;
            return ((uint32_t)(CLAMP8(outA)) <<  24) | (CLAMP8(outR) << 16) | (CLAMP8(outG) << 8) | CLAMP8(outB);

        /* Texture2D.FUNC_DECAL */
        case 70:
            outR = (fgR * fgA / 255) + (bgR * (255 - fgA) / 255);
            outG = (fgG * fgA / 255) + (bgG * (255 - fgA) / 255);
            outB = (fgB * fgA / 255) + (bgB * (255 - fgA) / 255);
            outA = fgA;
            return ((uint32_t)(CLAMP8(outA)) <<  24) | (CLAMP8(outR) << 16) | (CLAMP8(outG) << 8) | CLAMP8(outB);

        /* Texture2D.FUNC_ADD */
        case 71:
            outR = bgR + fgR < 255 ? bgR + fgR : 255;
            outG = bgG + fgG < 255 ? bgG + fgG : 255;
            outB = bgB + fgB < 255 ? bgB + fgB : 255;
            outA = bgA > fgA ? bgA : fgA;
            return ((uint32_t)(CLAMP8(outA)) <<  24) | (outR << 16) | (outG << 8) | outB;

        default:
            return bg;
    }
    #undef CLAMP8
}

/* v31: JSR-184 §Texture2D texture blending functions (vertex color Cv/Av
 * blended with texel Ct/At). The generic m3g_blend_pixels implements
 * CompositingMode semantics; applying it to the TEXTURE stage produced
 * wrong DECAL/BLEND/ADD/MODULATE results (e.g. MODULATE's old
 * A'=max(Av,At) forced alpha-textured fragments opaque).
 * Internal codes: 67=REPLACE 68=MODULATE 70=DECAL 64=BLEND 71=ADD.
 *   REPLACE : C'=Ct,                       A'=At
 *   MODULATE: C'=Cv*Ct,                     A'=Av*At
 *   DECAL   : C'=Cv*(1-At)+Ct*At,           A'=Av
 *   BLEND   : C'=Cv*(1-At)+Cblend*At,       A'=Av*At
 *   ADD     : C'=Cv+Ct,                     A'=Av*At            */
static uint32_t m3g_texfunc_blend(uint32_t vert, uint32_t tex, int mode, uint32_t blend_color) {
    #define CLAMP8T(v) ((v) < 0 ? 0 : ((v) > 255 ? 255 : (v)))
    int vA = (vert >> 24) & 0xFF, vR = (vert >> 16) & 0xFF, vG = (vert >> 8) & 0xFF, vB = vert & 0xFF;
    int tA = (tex >> 24) & 0xFF,  tR = (tex >> 16) & 0xFF,  tG = (tex >> 8) & 0xFF,  tB = tex & 0xFF;
    int outR, outG, outB, outA;

    switch (mode) {
        case 67: /* REPLACE */
            return tex;
        case 68: /* MODULATE */
            outR = (vR * tR) / 255; outG = (vG * tG) / 255; outB = (vB * tB) / 255;
            outA = (vA * tA) / 255;
            break;
        case 70: { /* DECAL */
            int ia = 255 - tA;
            outR = (vR * ia + tR * tA) / 255;
            outG = (vG * ia + tG * tA) / 255;
            outB = (vB * ia + tB * tA) / 255;
            outA = vA;
            break;
        }
        case 64: { /* BLEND (Cblend) */
            int bR = (blend_color >> 16) & 0xFF, bG = (blend_color >> 8) & 0xFF, bB = blend_color & 0xFF;
            int ia = 255 - tA;
            outR = (vR * ia + bR * tA) / 255;
            outG = (vG * ia + bG * tA) / 255;
            outB = (vB * ia + bB * tA) / 255;
            outA = (vA * tA) / 255;
            break;
        }
        case 71: /* ADD */
            outR = vR + tR; if (outR > 255) outR = 255;
            outG = vG + tG; if (outG > 255) outG = 255;
            outB = vB + tB; if (outB > 255) outB = 255;
            outA = (vA * tA) / 255;
            break;
        default: /* unknown — keep texel */
            return tex;
    }
    return ((uint32_t)(CLAMP8T(outA)) << 24) | ((uint32_t)CLAMP8T(outR) << 16) |
           ((uint32_t)CLAMP8T(outG) << 8) | (uint32_t)CLAMP8T(outB);
    #undef CLAMP8T
}

/* Fog blending matching FreeJ2ME Graphics3D.blendFog */
static uint32_t m3g_blend_fog(uint32_t pixel_color, int fog_color, float fog_factor) {
    int r = (int)(((pixel_color >> 16) & 0xFF) * fog_factor + ((fog_color >> 16) & 0xFF) * (1 - fog_factor));
    int g = (int)(((pixel_color >> 8) & 0xFF) * fog_factor + ((fog_color >> 8) & 0xFF) * (1 - fog_factor));
    int b = (int)((pixel_color & 0xFF) * fog_factor + (fog_color & 0xFF) * (1 - fog_factor));
    /* Fog is always fully opaque */
    return ((uint32_t)(255) <<  24) | (r << 16) | (g << 8) | b;
}

#if RENDER_HAVE_NEON
/* ============================================================================
 * v34.24 NEON rasterizer span: 4 pixels per iteration.
 * ============================================================================
 * Eligibility is checked per-triangle in m3g_rasterize_triangle:
 *   single nearest-filtered texture, no fog, no second texture unit,
 *   no UV transform, no alpha threshold, alphaFactor==1,
 *   texture function MODULATE(68)/REPLACE(67),
 *   compositing REPLACE(67)/MODULATE(68)/ALPHA(64)/ALPHA_ADD(65).
 *
 * Everything below mirrors the scalar loop evaluation order exactly:
 *   - barycentric lanes are built with SEQUENTIAL adds (lane_steps4),
 *     so each lane holds the same bits the scalar w0/w1/w2 would hold;
 *   - z, UV and vertex-color interpolations use the same
 *     mul/mul/add/add/div sequence as the scalar functions;
 *   - the /255 integer divisions use the exact (x+(x>>8)+1)>>8 identity;
 *   - degenerate-denominator fallbacks replicate the scalar branches;
 *   - out-of-triangle lanes produce garbage that is selected away by the
 *     coverage mask and never stored.
 * Returns the number of pixels written (coverage && depth && alpha!=0).
 * ==========================================================================*/
static int m3g_span4_neon(uint32_t* color_buf, float* depth_buf,
                          int row_idx, int x,
                          const float* s0, const float* s1, const float* s2,
                          float w0, float w1, float w2,
                          float step0, float step1, float step2,
                          const float* t0, const float* t1, const float* t2,
                          const uint8_t* c0, const uint8_t* c1, const uint8_t* c2,
                          M3GTexture2D* tex,
                          int tex_replace, int tex_smooth, int blend_mode,
                          int do_depth_test, int do_depth_write, float z_offset)
{
    const float32x4_t zero4 = vdupq_n_f32(0.0f);

    /* Barycentric lanes (sequential adds — bit-identical to scalar) */
    float32x4_t w0q = lane_steps4(w0, step0);
    float32x4_t w1q = lane_steps4(w1, step1);
    float32x4_t w2q = lane_steps4(w2, step2);

    /* Coverage: w0 >= 0 && w1 >= 0 && w2 >= 0.
     * (v34.25) vcgeq_f32/vcleq_f32 already yield uint32x4_t: the extra
     * vreinterpretq_u32_f32() wrappers here were accepted by clang
     * (lax vector conversions) but are hard type errors on real GCC
     * (m17 toolchain 6.4.0) — removed. */
    uint32x4_t mask = vandq_u32(vandq_u32(
        vcgeq_f32(w0q, zero4),
        vcgeq_f32(w1q, zero4)),
        vcgeq_f32(w2q, zero4));

    /* z = s0[2]*w0 + s1[2]*w1 + s2[2]*w2 + z_offset */
    float32x4_t zq = vaddq_f32(vaddq_f32(vaddq_f32(
        vmulq_n_f32(w0q, s0[2]), vmulq_n_f32(w1q, s1[2])),
        vmulq_n_f32(w2q, s2[2])), vdupq_n_f32(z_offset));

    const int idx = row_idx + x;

    /* Depth test: pass when depth >= z (scalar rejects depth < z) */
    float32x4_t dep = vdupq_n_f32(0.0f);
    if (do_depth_test) {
        dep = vld1q_f32(&depth_buf[idx]);
        mask = vandq_u32(mask, vcgeq_f32(dep, zq));
    }
    if (!(vgetq_lane_u32(mask, 0) | vgetq_lane_u32(mask, 1) |
          vgetq_lane_u32(mask, 2) | vgetq_lane_u32(mask, 3))) {
        return 0; /* no covered+passing pixel in this quad */
    }

    /* Perspective-correct UV (m3g_interpolate_texcoord order) */
    const float iw0 = s0[3], iw1 = s1[3], iw2 = s2[3];
    float32x4_t denq = vaddq_f32(vaddq_f32(
        vmulq_n_f32(w0q, iw0), vmulq_n_f32(w1q, iw1)),
        vmulq_n_f32(w2q, iw2));
    const uint32x4_t deg = vcleq_f32(denq, vdupq_n_f32(1e-9f));

    const float a0u = t0[0] * iw0, a1u = t1[0] * iw1, a2u = t2[0] * iw2;
    const float a0v = t0[1] * iw0, a1v = t1[1] * iw1, a2v = t2[1] * iw2;
    float32x4_t uq = vdivq_f32v7(vaddq_f32(vaddq_f32(
        vmulq_n_f32(w0q, a0u), vmulq_n_f32(w1q, a1u)),
        vmulq_n_f32(w2q, a2u)), denq);
    float32x4_t vq = vdivq_f32v7(vaddq_f32(vaddq_f32(
        vmulq_n_f32(w0q, a0v), vmulq_n_f32(w1q, a1v)),
        vmulq_n_f32(w2q, a2v)), denq);
    /* Degenerate denominator: fall back to vertex-0 texcoords */
    uq = vbslq_f32(deg, vdupq_n_f32(t0[0]), uq);
    vq = vbslq_f32(deg, vdupq_n_f32(t0[1]), vq);

    /* Wrap modes (m3g_sample_texture order) */
    if (tex->blend_s) {
        uq = vminq_f32(vmaxq_f32(uq, zero4), vdupq_n_f32(0.999999f));
    } else {
        uq = vsubq_f32(uq, vfloorq_f32(uq));
        uq = vaddq_f32(uq, vbslq_f32(vcltq_f32(uq, zero4), vdupq_n_f32(1.0f), zero4));
        uq = vbslq_f32(vcgeq_f32(uq, vdupq_n_f32(1.0f)), vdupq_n_f32(0.999999f), uq);
    }
    if (tex->blend_t) {
        vq = vminq_f32(vmaxq_f32(vq, zero4), vdupq_n_f32(0.999999f));
    } else {
        vq = vsubq_f32(vq, vfloorq_f32(vq));
        vq = vaddq_f32(vq, vbslq_f32(vcltq_f32(vq, zero4), vdupq_n_f32(1.0f), zero4));
        vq = vbslq_f32(vcgeq_f32(vq, vdupq_n_f32(1.0f)), vdupq_n_f32(0.999999f), vq);
    }

    /* v34.79 SMOOTH (j2me_texture_smoothing): integer bilinear span sampler.
     * Replays m3g_sample_texture_smooth BIT-EXACTLY per lane: the same
     * float mul/sub/max/sub/min/cvt sequence for coordinates and weights
     * (vcvtq_s32_f32 truncates like the C cast on armv7 NEON — RZ; a
     * aarch64 build would round instead, which only degrades an already
     * masked/garbage lane to the nearest texel, never OOB; vcvtq_f32_s32
     * is exact), then the same 7-bit two-stage lerp in 16-bit lanes —
     * u16-safe, since
     * every weighted sum is <= 255*128 and the >>7 results are <= 255.
     * Index/weight clamps are no-ops for valid lanes (uq/vq are wrapped
     * above exactly like the scalar sampler's inputs); for garbage lanes
     * (masked out of every store) they only keep the 16 gathers in bounds.
     * ARMv7 cost per 4-pixel quad: ~16 setup ops + 16 gathers + ~24 lerp
     * ops — vs ~4x that for four scalar sampler calls (each with its own
     * float->int conversions, clamps and 4 loads). */
    uint32x4_t texel;
    if (tex_smooth) {
        const int tw = tex->width, th = tex->height;
        float32x4_t fxq = vsubq_f32(vmulq_n_f32(uq, (float)tw), vdupq_n_f32(0.5f));
        float32x4_t fyq = vsubq_f32(vmulq_n_f32(vq, (float)th), vdupq_n_f32(0.5f));
        fxq = vmaxq_f32(fxq, zero4);   /* VMAX: NaN lane -> 0 (numeric wins) */
        fyq = vmaxq_f32(fyq, zero4);
        int32x4_t x0q = vcvtq_s32_f32(fxq);   /* trunc, like the C cast */
        int32x4_t y0q = vcvtq_s32_f32(fyq);
        float32x4_t frxq = vsubq_f32(fxq, vcvtq_f32_s32(x0q));
        float32x4_t fryq = vsubq_f32(fyq, vcvtq_f32_s32(y0q));
        frxq = vminq_f32(frxq, vdupq_n_f32(1.0f));
        fryq = vminq_f32(fryq, vdupq_n_f32(1.0f));
        int32x4_t wxq = vcvtq_s32_f32(vmulq_n_f32(frxq, 128.0f));
        int32x4_t wyq = vcvtq_s32_f32(vmulq_n_f32(fryq, 128.0f));
        /* v34.80: vaddq_n_s32 is an AArch64-only intrinsic (implicit
         * declaration + type error on every 32-bit armv7 toolchain —
         * caught on the user's build). Portable dup+add instead: the
         * exact pattern span_blend_row_const has used since v34.35
         * (vaddq_u16(x, vdupq_n_u16(128))), valid on armv7 AND aarch64,
         * one VDUP hoisted by GCC out of the quad loop. */
        const int32x4_t one_i = vdupq_n_s32(1);
        int32x4_t x1q = vaddq_s32(x0q, one_i);
        int32x4_t y1q = vaddq_s32(y0q, one_i);
        const int32x4_t zi = vdupq_n_s32(0);
        x0q = vminq_s32(vmaxq_s32(x0q, zi), vdupq_n_s32(tw - 1));
        x1q = vminq_s32(vmaxq_s32(x1q, zi), vdupq_n_s32(tw - 1));
        y0q = vminq_s32(vmaxq_s32(y0q, zi), vdupq_n_s32(th - 1));
        y1q = vminq_s32(vmaxq_s32(y1q, zi), vdupq_n_s32(th - 1));
        wxq = vminq_s32(vmaxq_s32(wxq, zi), vdupq_n_s32(128));
        wyq = vminq_s32(vmaxq_s32(wyq, zi), vdupq_n_s32(128));

        /* row bases + column offsets -> 4 index vectors */
        int32x4_t r0 = vmulq_n_s32(y0q, tw);
        int32x4_t r1 = vmulq_n_s32(y1q, tw);
        int32x4_t i00 = vaddq_s32(r0, x0q);
        int32x4_t i10 = vaddq_s32(r0, x1q);
        int32x4_t i01 = vaddq_s32(r1, x0q);
        int32x4_t i11 = vaddq_s32(r1, x1q);

        /* 16 texel gathers (indices clamped above — no OOB even for
         * garbage lanes; valid lanes equal the scalar sampler's picks) */
        uint32x4_t t00 = vdupq_n_u32(0);
        uint32x4_t t10 = vdupq_n_u32(0);
        uint32x4_t t01 = vdupq_n_u32(0);
        uint32x4_t t11 = vdupq_n_u32(0);
        t00 = vld1q_lane_u32(&tex->pixels[vgetq_lane_s32(i00, 0)], t00, 0);
        t00 = vld1q_lane_u32(&tex->pixels[vgetq_lane_s32(i00, 1)], t00, 1);
        t00 = vld1q_lane_u32(&tex->pixels[vgetq_lane_s32(i00, 2)], t00, 2);
        t00 = vld1q_lane_u32(&tex->pixels[vgetq_lane_s32(i00, 3)], t00, 3);
        t10 = vld1q_lane_u32(&tex->pixels[vgetq_lane_s32(i10, 0)], t10, 0);
        t10 = vld1q_lane_u32(&tex->pixels[vgetq_lane_s32(i10, 1)], t10, 1);
        t10 = vld1q_lane_u32(&tex->pixels[vgetq_lane_s32(i10, 2)], t10, 2);
        t10 = vld1q_lane_u32(&tex->pixels[vgetq_lane_s32(i10, 3)], t10, 3);
        t01 = vld1q_lane_u32(&tex->pixels[vgetq_lane_s32(i01, 0)], t01, 0);
        t01 = vld1q_lane_u32(&tex->pixels[vgetq_lane_s32(i01, 1)], t01, 1);
        t01 = vld1q_lane_u32(&tex->pixels[vgetq_lane_s32(i01, 2)], t01, 2);
        t01 = vld1q_lane_u32(&tex->pixels[vgetq_lane_s32(i01, 3)], t01, 3);
        t11 = vld1q_lane_u32(&tex->pixels[vgetq_lane_s32(i11, 0)], t11, 0);
        t11 = vld1q_lane_u32(&tex->pixels[vgetq_lane_s32(i11, 1)], t11, 1);
        t11 = vld1q_lane_u32(&tex->pixels[vgetq_lane_s32(i11, 2)], t11, 2);
        t11 = vld1q_lane_u32(&tex->pixels[vgetq_lane_s32(i11, 3)], t11, 3);

        /* Split ARGB into 16-bit channel lanes: lo = (B,R), hi = (G,A).
         * Each u16 lane then holds exactly one 0..255 channel — the same
         * granularity as the scalar m3g_bilerp_chan, so the lerp below is
         * the same integer sequence vectorized. */
        const uint32x4_t mff = vdupq_n_u32(0x00FF00FFu);
        uint16x8_t l00 = vreinterpretq_u16_u32(vandq_u32(t00, mff));
        uint16x8_t h00 = vreinterpretq_u16_u32(vandq_u32(vshrq_n_u32(t00, 8), mff));
        uint16x8_t l10 = vreinterpretq_u16_u32(vandq_u32(t10, mff));
        uint16x8_t h10 = vreinterpretq_u16_u32(vandq_u32(vshrq_n_u32(t10, 8), mff));
        uint16x8_t l01 = vreinterpretq_u16_u32(vandq_u32(t01, mff));
        uint16x8_t h01 = vreinterpretq_u16_u32(vandq_u32(vshrq_n_u32(t01, 8), mff));
        uint16x8_t l11 = vreinterpretq_u16_u32(vandq_u32(t11, mff));
        uint16x8_t h11 = vreinterpretq_u16_u32(vandq_u32(vshrq_n_u32(t11, 8), mff));

        /* Duplicate each 32-bit lane's weight into both of its u16 lanes:
         * wx | (wx << 16) — then every u16 lane of a pixel carries the same
         * wx the scalar sampler uses for both of its channel halves. */
        const uint32x4_t wxo = vorrq_u32(vreinterpretq_u32_s32(wxq),
                                         vshlq_n_u32(vreinterpretq_u32_s32(wxq), 16));
        const uint32x4_t wyo = vorrq_u32(vreinterpretq_u32_s32(wyq),
                                         vshlq_n_u32(vreinterpretq_u32_s32(wyq), 16));
        const uint16x8_t wxu = vreinterpretq_u16_u32(wxo);
        const uint16x8_t wyu = vreinterpretq_u16_u32(wyo);
        const uint16x8_t wxc = vsubq_u16(vdupq_n_u16(128), wxu);
        const uint16x8_t wyc = vsubq_u16(vdupq_n_u16(128), wyu);

        /* Two-stage lerp — the scalar formula, stage X then stage Y, >>7
         * each stage (u16-safe: weighted sums <= 255*128). */
        uint16x8_t la0 = vshrq_n_u16(vaddq_u16(vmulq_u16(l00, wxc), vmulq_u16(l10, wxu)), 7);
        uint16x8_t ha0 = vshrq_n_u16(vaddq_u16(vmulq_u16(h00, wxc), vmulq_u16(h10, wxu)), 7);
        uint16x8_t la1 = vshrq_n_u16(vaddq_u16(vmulq_u16(l01, wxc), vmulq_u16(l11, wxu)), 7);
        uint16x8_t ha1 = vshrq_n_u16(vaddq_u16(vmulq_u16(h01, wxc), vmulq_u16(h11, wxu)), 7);
        uint16x8_t lo = vshrq_n_u16(vaddq_u16(vmulq_u16(la0, wyc), vmulq_u16(la1, wyu)), 7);
        uint16x8_t hi = vshrq_n_u16(vaddq_u16(vmulq_u16(ha0, wyc), vmulq_u16(ha1, wyu)), 7);

        /* Repack: ARGB = lo | (hi << 8) — restores A<<24|R<<16|G<<8|B. */
        texel = vreinterpretq_u32_u16(vorrq_u16(lo, vshlq_n_u16(hi, 8)));
    } else {
    /* Nearest texel indices, clamped (safe gathers even for garbage lanes) */
    int32x4_t tx = vcvtq_s32_f32(vmulq_n_f32(uq, (float)tex->width));
    int32x4_t ty = vcvtq_s32_f32(vmulq_n_f32(vq, (float)tex->height));
    tx = vminq_s32(vmaxq_s32(tx, vdupq_n_s32(0)), vdupq_n_s32(tex->width - 1));
    ty = vminq_s32(vmaxq_s32(ty, vdupq_n_s32(0)), vdupq_n_s32(tex->height - 1));

    /* Gather 4 texels */
    texel = vdupq_n_u32(0);
    texel = vld1q_lane_u32(&tex->pixels[vgetq_lane_s32(ty, 0) * tex->width + vgetq_lane_s32(tx, 0)], texel, 0);
    texel = vld1q_lane_u32(&tex->pixels[vgetq_lane_s32(ty, 1) * tex->width + vgetq_lane_s32(tx, 1)], texel, 1);
    texel = vld1q_lane_u32(&tex->pixels[vgetq_lane_s32(ty, 2) * tex->width + vgetq_lane_s32(tx, 2)], texel, 2);
    texel = vld1q_lane_u32(&tex->pixels[vgetq_lane_s32(ty, 3) * tex->width + vgetq_lane_s32(tx, 3)], texel, 3);
    }

    /* Paint pixel: REPLACE path keeps the texel; MODULATE path multiplies
     * the perspective-correct interpolated vertex color (both replicate
     * the scalar m3g_texfunc_blend MODULATE / pass-through). */
    uint32x4_t paint;
    if (tex_replace || !c0) {
        paint = texel;
    } else {
        /* Interpolate vertex colors (m3g_interpolate_color, 4 channels) */
        uint32x4_t col[4];
        for (int j = 0; j < 4; j++) {
            const float A0 = (float)c0[j] * iw0;
            const float A1 = (float)c1[j] * iw1;
            const float A2 = (float)c2[j] * iw2;
            float32x4_t num = vaddq_f32(vaddq_f32(
                vmulq_n_f32(w0q, A0), vmulq_n_f32(w1q, A1)),
                vmulq_n_f32(w2q, A2));
            float32x4_t val = vdivq_f32v7(num, denq);
            /* non-degenerate: clamp [0,255] + 0.5 + trunc */
            float32x4_t cl = vaddq_f32(vminq_f32(vmaxq_f32(val, zero4),
                vdupq_n_f32(255.0f)), vdupq_n_f32(0.5f));
            /* degenerate: (uint8_t)(c0[j]*w0 + c1[j]*w1 + c2[j]*w2) */
            float32x4_t raw = vaddq_f32(vaddq_f32(
                vmulq_n_f32(w0q, (float)c0[j]), vmulq_n_f32(w1q, (float)c1[j])),
                vmulq_n_f32(w2q, (float)c2[j]));
            val = vbslq_f32(deg, raw, cl);
            col[j] = vreinterpretq_u32_s32(vcvtq_s32_f32(val));
        }
        /* texfunc MODULATE: (vC * tC) / 255 per channel */
        uint32x4_t pr = vdiv255_u32(vmulq_u32(col[0], vchan_r(texel)));
        uint32x4_t pg = vdiv255_u32(vmulq_u32(col[1], vchan_g(texel)));
        uint32x4_t pb = vdiv255_u32(vmulq_u32(col[2], vchan_b(texel)));
        uint32x4_t pa = vdiv255_u32(vmulq_u32(col[3], vchan_a(texel)));
        paint = vpack_argb(pa, pr, pg, pb);
    }

    /* Skip fully-transparent paint (scalar: paint_alpha == 0 -> no write) */
    mask = vandq_u32(mask, vmvnq_u32(vceqq_u32(vandq_u32(vchan_a(paint), vdupq_n_u32(0xFF)), vdupq_n_u32(0))));
    if (!(vgetq_lane_u32(mask, 0) | vgetq_lane_u32(mask, 1) |
          vgetq_lane_u32(mask, 2) | vgetq_lane_u32(mask, 3))) {
        return 0;
    }

    /* Compositing stage: m3g_blend_pixels(bg, paint, alpha, blend_mode).
     * For 67/68 the alpha parameter is unused (fg alpha lane is used);
     * for 64/65 the scalar alpha == the paint alpha lane. */
    uint32x4_t bg = vld1q_u32(&color_buf[idx]);
    uint32x4_t final;
    switch (blend_mode) {
    case 68: { /* MODULATE: (fgC*bgC)/255, outA = (bgA*fgA)/255 */
        uint32x4_t pr = vdiv255_u32(vmulq_u32(vchan_r(paint), vchan_r(bg)));
        uint32x4_t pg = vdiv255_u32(vmulq_u32(vchan_g(paint), vchan_g(bg)));
        uint32x4_t pb = vdiv255_u32(vmulq_u32(vchan_b(paint), vchan_b(bg)));
        uint32x4_t pa = vdiv255_u32(vmulq_u32(vchan_a(paint), vchan_a(bg)));
        final = vpack_argb(pa, pr, pg, pb);
        break;
    }
    case 67: { /* REPLACE: out = fg*(fa/255) + bg*(1 - fa/255), float */
        float32x4_t fa = vdivq_f32v7(vcvtq_f32_u32(vchan_a(paint)), vdupq_n_f32(255.0f));
        float32x4_t one_m = vsubq_f32(vdupq_n_f32(1.0f), fa);
        int32x4_t r = vcvtq_s32_f32(vaddq_f32(
            vmulq_f32(vcvtq_f32_u32(vchan_r(paint)), fa),
            vmulq_f32(vcvtq_f32_u32(vchan_r(bg)), one_m)));
        int32x4_t g = vcvtq_s32_f32(vaddq_f32(
            vmulq_f32(vcvtq_f32_u32(vchan_g(paint)), fa),
            vmulq_f32(vcvtq_f32_u32(vchan_g(bg)), one_m)));
        int32x4_t b = vcvtq_s32_f32(vaddq_f32(
            vmulq_f32(vcvtq_f32_u32(vchan_b(paint)), fa),
            vmulq_f32(vcvtq_f32_u32(vchan_b(bg)), one_m)));
        int32x4_t oa = vcvtq_s32_f32(vaddq_f32(
            vcvtq_f32_u32(vchan_a(paint)),
            vmulq_f32(vcvtq_f32_u32(vchan_a(bg)), one_m)));
        const int32x4_t c255s = vdupq_n_s32(255), z0 = vdupq_n_s32(0);
        r = vminq_s32(vmaxq_s32(r, z0), c255s);
        g = vminq_s32(vmaxq_s32(g, z0), c255s);
        b = vminq_s32(vmaxq_s32(b, z0), c255s);
        oa = vminq_s32(vmaxq_s32(oa, z0), c255s);
        final = vpack_argb(vreinterpretq_u32_s32(oa), vreinterpretq_u32_s32(r),
                           vreinterpretq_u32_s32(g), vreinterpretq_u32_s32(b));
        break;
    }
    case 64: { /* ALPHA: aN = alpha/255; fgC*aN + bgC*(1-aN); outA same */
        float32x4_t aN = vdivq_f32v7(vcvtq_f32_u32(vchan_a(paint)), vdupq_n_f32(255.0f));
        float32x4_t one_m = vsubq_f32(vdupq_n_f32(1.0f), aN);
        int32x4_t r = vcvtq_s32_f32(vaddq_f32(
            vmulq_f32(vcvtq_f32_u32(vchan_r(paint)), aN),
            vmulq_f32(vcvtq_f32_u32(vchan_r(bg)), one_m)));
        int32x4_t g = vcvtq_s32_f32(vaddq_f32(
            vmulq_f32(vcvtq_f32_u32(vchan_g(paint)), aN),
            vmulq_f32(vcvtq_f32_u32(vchan_g(bg)), one_m)));
        int32x4_t b = vcvtq_s32_f32(vaddq_f32(
            vmulq_f32(vcvtq_f32_u32(vchan_b(paint)), aN),
            vmulq_f32(vcvtq_f32_u32(vchan_b(bg)), one_m)));
        int32x4_t oa = vcvtq_s32_f32(vaddq_f32(
            vmulq_f32(vcvtq_f32_u32(vchan_a(bg)), one_m),
            vmulq_f32(vcvtq_f32_u32(vchan_a(paint)), aN)));
        const int32x4_t c255s = vdupq_n_s32(255), z0 = vdupq_n_s32(0);
        r = vminq_s32(vmaxq_s32(r, z0), c255s);
        g = vminq_s32(vmaxq_s32(g, z0), c255s);
        b = vminq_s32(vmaxq_s32(b, z0), c255s);
        oa = vminq_s32(vmaxq_s32(oa, z0), c255s);
        final = vpack_argb(vreinterpretq_u32_s32(oa), vreinterpretq_u32_s32(r),
                           vreinterpretq_u32_s32(g), vreinterpretq_u32_s32(b));
        break;
    }
    default: { /* 65 ALPHA_ADD: outC = min(255, fgC*aN + bgC);
                  outA = min(255, bgA + (int)(alpha*(1 - bgA/255))) */
        float32x4_t aN = vdivq_f32v7(vcvtq_f32_u32(vchan_a(paint)), vdupq_n_f32(255.0f));
        int32x4_t r = vcvtq_s32_f32(vminq_f32(vdupq_n_f32(255.0f), vaddq_f32(
            vmulq_f32(vcvtq_f32_u32(vchan_r(paint)), aN),
            vcvtq_f32_u32(vchan_r(bg)))));
        int32x4_t g = vcvtq_s32_f32(vminq_f32(vdupq_n_f32(255.0f), vaddq_f32(
            vmulq_f32(vcvtq_f32_u32(vchan_g(paint)), aN),
            vcvtq_f32_u32(vchan_g(bg)))));
        int32x4_t b = vcvtq_s32_f32(vminq_f32(vdupq_n_f32(255.0f), vaddq_f32(
            vmulq_f32(vcvtq_f32_u32(vchan_b(paint)), aN),
            vcvtq_f32_u32(vchan_b(bg)))));
        float32x4_t one_m_bg = vsubq_f32(vdupq_n_f32(1.0f),
            vdivq_f32v7(vcvtq_f32_u32(vchan_a(bg)), vdupq_n_f32(255.0f)));
        int32x4_t inner = vcvtq_s32_f32(vmulq_f32(vcvtq_f32_u32(vchan_a(paint)), one_m_bg));
        int32x4_t oa = vcvtq_s32_f32(vminq_f32(vdupq_n_f32(255.0f),
            vcvtq_f32_s32(vaddq_s32(vcvtq_s32_f32(vcvtq_f32_u32(vchan_a(bg))), inner))));
        const int32x4_t z0 = vdupq_n_s32(0);
        r = vminq_s32(vmaxq_s32(r, z0), vdupq_n_s32(255));
        g = vminq_s32(vmaxq_s32(g, z0), vdupq_n_s32(255));
        b = vminq_s32(vmaxq_s32(b, z0), vdupq_n_s32(255));
        oa = vminq_s32(vmaxq_s32(oa, z0), vdupq_n_s32(255));
        final = vpack_argb(vreinterpretq_u32_s32(oa), vreinterpretq_u32_s32(r),
                           vreinterpretq_u32_s32(g), vreinterpretq_u32_s32(b));
        break;
    }
    }

    /* Masked stores: untouched lanes keep their old values */
    uint32x4_t out = vbslq_u32(mask, final, bg);
    vst1q_u32(&color_buf[idx], out);
    if (do_depth_write) {
        if (!do_depth_test) dep = vld1q_f32(&depth_buf[idx]);
        float32x4_t newd = vbslq_f32(mask, zq, dep); /* selector is uint32x4_t */
        vst1q_f32(&depth_buf[idx], newd);
    }
    return vmask_count(mask);
}
#endif /* RENDER_HAVE_NEON */

#if RENDER_HAVE_SSE2
/* ============================================================================
 * v34.51: SSE2 rasterizer span (x86-64) — mirror of the NEON span above.
 *
 * Bit-exactness contract vs the scalar loop:
 *   - barycentric lanes use SEQUENTIAL scalar adds (lane_steps4), so each
 *     lane holds the same bits the scalar w0/w1/w2 would hold;
 *   - all float math uses per-lane IEEE-754 mul/add — and unlike the ARM
 *     fastdiv shortcut, division is _mm_div_ps: correctly-rounded per
 *     lane, BIT-IDENTICAL to the scalar '/' operator;
 *   - the /255 integer identities match the scalar exactly;
 *   - garbage lanes are selected away by the coverage mask and never
 *     stored; gathers are clamped so garbage lanes cannot read OOB.
 * ==========================================================================*/
static inline __m128i sbsl_u32(__m128i m, __m128i x, __m128i y) {
    return _mm_or_si128(_mm_and_si128(m, x), _mm_andnot_si128(m, y));
}
static inline __m128 sbsl_f32(__m128i m, __m128 x, __m128 y) {
    return _mm_or_ps(_mm_and_ps(_mm_castsi128_ps(m), x),
                    _mm_andnot_ps(_mm_castsi128_ps(m), y));
}
/* floorf per lane: trunc (cvttps) then subtract 1 where t > v. */
static inline __m128 sfloor(__m128 v) {
    __m128 t = _mm_cvtepi32_ps(_mm_cvttps_epi32(v));
    __m128 gt = _mm_cmpgt_ps(t, v);
    return _mm_sub_ps(t, _mm_and_ps(gt, _mm_set1_ps(1.0f)));
}
/* clamp x to [0, 255] on epi32 lanes (SSE2 has no min_epi32). */
static inline __m128i sclamp255(__m128i x) {
    __m128i hi = _mm_cmpgt_epi32(x, _mm_set1_epi32(255));
    __m128i neg = _mm_cmpgt_epi32(_mm_set1_epi32(-1), x); /* true when x < 0 */
    return _mm_or_si128(_mm_and_si128(hi, _mm_set1_epi32(255)),
                        _mm_and_si128(_mm_andnot_si128(hi, x),
                                      _mm_andnot_si128(neg, _mm_set1_epi32(-1))));
}
/* (v * t) / 255 for channel lanes (each <= 255): the 16-bit product is
 * exact (<= 65025), widened back for the exact (x+(x>>8)+1)>>8 division. */
static inline __m128i smod255(__m128i v32, __m128i t32) {
    __m128i v16 = _mm_packs_epi32(v32, v32);
    __m128i t16 = _mm_packs_epi32(t32, t32);
    __m128i p16 = _mm_mullo_epi16(v16, t16);
    return sdiv255(_mm_unpacklo_epi16(p16, _mm_setzero_si128()));
}

static int m3g_span4_sse2(uint32_t* color_buf, float* depth_buf,
                          int row_idx, int x,
                          const float* s0, const float* s1, const float* s2,
                          float w0, float w1, float w2,
                          float step0, float step1, float step2,
                          const float* t0, const float* t1, const float* t2,
                          const uint8_t* c0, const uint8_t* c1, const uint8_t* c2,
                          M3GTexture2D* tex,
                          int tex_replace, int tex_smooth, int blend_mode,
                          int do_depth_test, int do_depth_write, float z_offset)
{
    const __m128 zero4 = _mm_setzero_ps();

    /* Barycentric lanes — sequential adds, bit-identical to scalar */
    __m128 w0q, w1q, w2q;
    {
        const float b0 = w0, b1 = w1, b2 = w2;
        const float l0_1 = b0 + step0, l0_2 = l0_1 + step0, l0_3 = l0_2 + step0;
        const float l1_1 = b1 + step1, l1_2 = l1_1 + step1, l1_3 = l1_2 + step1;
        const float l2_1 = b2 + step2, l2_2 = l2_1 + step2, l2_3 = l2_2 + step2;
        w0q = _mm_setr_ps(b0, l0_1, l0_2, l0_3);
        w1q = _mm_setr_ps(b1, l1_1, l1_2, l1_3);
        w2q = _mm_setr_ps(b2, l2_1, l2_2, l2_3);
    }

    /* Coverage: w0 >= 0 && w1 >= 0 && w2 >= 0 */
    __m128i mask = _mm_castps_si128(_mm_and_ps(_mm_and_ps(
        _mm_cmpge_ps(w0q, zero4), _mm_cmpge_ps(w1q, zero4)),
        _mm_cmpge_ps(w2q, zero4)));

    /* z = s0[2]*w0 + s1[2]*w1 + s2[2]*w2 + z_offset */
    __m128 zq = _mm_add_ps(_mm_add_ps(_mm_add_ps(
        _mm_mul_ps(w0q, _mm_set1_ps(s0[2])), _mm_mul_ps(w1q, _mm_set1_ps(s1[2]))),
        _mm_mul_ps(w2q, _mm_set1_ps(s2[2]))), _mm_set1_ps(z_offset));

    const int idx = row_idx + x;

    /* Depth test: pass when depth >= z */
    __m128 dep = zero4;
    if (do_depth_test) {
        dep = _mm_loadu_ps(&depth_buf[idx]);
        mask = _mm_and_si128(mask, _mm_castps_si128(
            _mm_cmpge_ps(dep, zq)));
    }
    if (!_mm_movemask_epi8(mask)) return 0;

    /* Perspective-correct UV (m3g_interpolate_texcoord order) */
    const float iw0 = s0[3], iw1 = s1[3], iw2 = s2[3];
    __m128 denq = _mm_add_ps(_mm_add_ps(
        _mm_mul_ps(w0q, _mm_set1_ps(iw0)), _mm_mul_ps(w1q, _mm_set1_ps(iw1))),
        _mm_mul_ps(w2q, _mm_set1_ps(iw2)));
    __m128i deg = _mm_castps_si128(_mm_cmple_ps(denq, _mm_set1_ps(1e-9f)));

    const float a0u = t0[0] * iw0, a1u = t1[0] * iw1, a2u = t2[0] * iw2;
    const float a0v = t0[1] * iw0, a1v = t1[1] * iw1, a2v = t2[1] * iw2;
    __m128 uq = _mm_div_ps(_mm_add_ps(_mm_add_ps(
        _mm_mul_ps(w0q, _mm_set1_ps(a0u)), _mm_mul_ps(w1q, _mm_set1_ps(a1u))),
        _mm_mul_ps(w2q, _mm_set1_ps(a2u))), denq);
    __m128 vq = _mm_div_ps(_mm_add_ps(_mm_add_ps(
        _mm_mul_ps(w0q, _mm_set1_ps(a0v)), _mm_mul_ps(w1q, _mm_set1_ps(a1v))),
        _mm_mul_ps(w2q, _mm_set1_ps(a2v))), denq);
    /* Degenerate denominator: fall back to vertex-0 texcoords */
    uq = _mm_castsi128_ps(sbsl_u32(deg, _mm_set1_epi32((int)t0[0]), _mm_castps_si128(uq)));
    vq = _mm_castsi128_ps(sbsl_u32(deg, _mm_set1_epi32((int)t0[1]), _mm_castps_si128(vq)));

    /* Wrap modes (m3g_sample_texture order) */
    if (tex->blend_s) {
        uq = _mm_min_ps(_mm_max_ps(uq, zero4), _mm_set1_ps(0.999999f));
    } else {
        uq = _mm_sub_ps(uq, sfloor(uq));
        uq = _mm_add_ps(uq, _mm_and_ps(
            _mm_cmplt_ps(uq, zero4), _mm_set1_ps(1.0f)));
        uq = _mm_or_ps(_mm_and_ps(
            _mm_cmpge_ps(uq, _mm_set1_ps(1.0f)), _mm_set1_ps(0.999999f)),
            _mm_andnot_ps(_mm_cmpge_ps(uq, _mm_set1_ps(1.0f)), uq));
    }
    if (tex->blend_t) {
        vq = _mm_min_ps(_mm_max_ps(vq, zero4), _mm_set1_ps(0.999999f));
    } else {
        vq = _mm_sub_ps(vq, sfloor(vq));
        vq = _mm_add_ps(vq, _mm_and_ps(
            _mm_cmplt_ps(vq, zero4), _mm_set1_ps(1.0f)));
        vq = _mm_or_ps(_mm_and_ps(
            _mm_cmpge_ps(vq, _mm_set1_ps(1.0f)), _mm_set1_ps(0.999999f)),
            _mm_andnot_ps(_mm_cmpge_ps(vq, _mm_set1_ps(1.0f)), vq));
    }

    /* v34.79 SMOOTH (j2me_texture_smoothing): integer bilinear span sampler —
     * SSE2 mirror of the NEON tex_smooth block and of the scalar
     * m3g_sample_texture_smooth (same float prologue per lane, cvttps =
     * truncation like the C cast; same 7-bit two-stage lerp in 16-bit
     * lanes — u16-safe). cvttps garbage lanes can be INT_MIN (indefinite),
     * so indices/weights are clamped in int arrays before the gathers,
     * exactly like the nearest path below. */
    __m128i texel;
    if (tex_smooth) {
        const int tw = tex->width, th = tex->height;
        __m128 fxq = _mm_sub_ps(_mm_mul_ps(uq, _mm_set1_ps((float)tw)), _mm_set1_ps(0.5f));
        __m128 fyq = _mm_sub_ps(_mm_mul_ps(vq, _mm_set1_ps((float)th)), _mm_set1_ps(0.5f));
        fxq = _mm_max_ps(fxq, zero4);   /* NaN lane -> 0 (MAXPS yields SRC2) */
        fyq = _mm_max_ps(fyq, zero4);
        __m128i x0v = _mm_cvttps_epi32(fxq);
        __m128i y0v = _mm_cvttps_epi32(fyq);
        __m128 frxq = _mm_sub_ps(fxq, _mm_cvtepi32_ps(x0v));
        __m128 fryq = _mm_sub_ps(fyq, _mm_cvtepi32_ps(y0v));
        frxq = _mm_min_ps(frxq, _mm_set1_ps(1.0f));
        fryq = _mm_min_ps(fryq, _mm_set1_ps(1.0f));
        int32_t x0i[4], y0i[4], wxi[4], wyi[4];
        _mm_storeu_si128((__m128i*)wxi, _mm_cvttps_epi32(_mm_mul_ps(frxq, _mm_set1_ps(128.0f))));
        _mm_storeu_si128((__m128i*)wyi, _mm_cvttps_epi32(_mm_mul_ps(fryq, _mm_set1_ps(128.0f))));
        _mm_storeu_si128((__m128i*)x0i, x0v);
        _mm_storeu_si128((__m128i*)y0i, y0v);
        uint32_t t00i[4], t10i[4], t01i[4], t11i[4];
        for (int j = 0; j < 4; j++) {
            int x0 = x0i[j], y0 = y0i[j];
            if (x0 < 0) x0 = 0; else if (x0 > tw - 1) x0 = tw - 1;
            if (y0 < 0) y0 = 0; else if (y0 > th - 1) y0 = th - 1;
            int x1 = x0 + 1; if (x1 > tw - 1) x1 = tw - 1;
            int y1 = y0 + 1; if (y1 > th - 1) y1 = th - 1;
            if (wxi[j] < 0) wxi[j] = 0; else if (wxi[j] > 128) wxi[j] = 128;
            if (wyi[j] < 0) wyi[j] = 0; else if (wyi[j] > 128) wyi[j] = 128;
            const int r0 = y0 * tw, r1 = y1 * tw;
            t00i[j] = tex->pixels[(size_t)r0 + x0];
            t10i[j] = tex->pixels[(size_t)r0 + x1];
            t01i[j] = tex->pixels[(size_t)r1 + x0];
            t11i[j] = tex->pixels[(size_t)r1 + x1];
        }
        __m128i t00 = _mm_loadu_si128((const __m128i*)t00i);
        __m128i t10 = _mm_loadu_si128((const __m128i*)t10i);
        __m128i t01 = _mm_loadu_si128((const __m128i*)t01i);
        __m128i t11 = _mm_loadu_si128((const __m128i*)t11i);
        /* Split ARGB into 16-bit channel lanes: lo = (B,R), hi = (G,A) */
        const __m128i mff = _mm_set1_epi32((int)0x00FF00FFu);
        __m128i l00 = _mm_and_si128(t00, mff);
        __m128i h00 = _mm_and_si128(_mm_srli_epi32(t00, 8), mff);
        __m128i l10 = _mm_and_si128(t10, mff);
        __m128i h10 = _mm_and_si128(_mm_srli_epi32(t10, 8), mff);
        __m128i l01 = _mm_and_si128(t01, mff);
        __m128i h01 = _mm_and_si128(_mm_srli_epi32(t01, 8), mff);
        __m128i l11 = _mm_and_si128(t11, mff);
        __m128i h11 = _mm_and_si128(_mm_srli_epi32(t11, 8), mff);
        /* Duplicate each 32-bit lane's weight into both of its u16 lanes:
         * wx | (wx << 16) — the epi16 view then carries wx in every lane. */
        __m128i wxo = _mm_loadu_si128((const __m128i*)wxi);
        __m128i wyo = _mm_loadu_si128((const __m128i*)wyi);
        wxo = _mm_or_si128(wxo, _mm_slli_epi32(wxo, 16));
        wyo = _mm_or_si128(wyo, _mm_slli_epi32(wyo, 16));
        __m128i wxc = _mm_sub_epi16(_mm_set1_epi16(128), wxo);
        __m128i wyc = _mm_sub_epi16(_mm_set1_epi16(128), wyo);
        /* Two-stage lerp — the scalar formula, stage X then stage Y, >>7
         * each stage (u16-safe: weighted sums <= 255*128). */
        __m128i la0 = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(l00, wxc), _mm_mullo_epi16(l10, wxo)), 7);
        __m128i ha0 = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(h00, wxc), _mm_mullo_epi16(h10, wxo)), 7);
        __m128i la1 = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(l01, wxc), _mm_mullo_epi16(l11, wxo)), 7);
        __m128i ha1 = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(h01, wxc), _mm_mullo_epi16(h11, wxo)), 7);
        __m128i lo = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(la0, wyc), _mm_mullo_epi16(la1, wyo)), 7);
        __m128i hi = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(ha0, wyc), _mm_mullo_epi16(ha1, wyo)), 7);
        /* Repack: ARGB = lo | (hi << 8) — restores A<<24|R<<16|G<<8|B. */
        texel = _mm_or_si128(lo, _mm_slli_epi16(hi, 8));
    } else {
    /* Nearest texel indices + gather (scalar: cvttps garbage lanes can be
     * INT_MIN, so clamp in the int array before reading the texture). */
    int32_t txi[4], tyi[4];
    {
        __m128i txv = _mm_cvttps_epi32(_mm_mul_ps(uq, _mm_set1_ps((float)tex->width)));
        __m128i tyv = _mm_cvttps_epi32(_mm_mul_ps(vq, _mm_set1_ps((float)tex->height)));
        _mm_storeu_si128((__m128i*)txi, txv);
        _mm_storeu_si128((__m128i*)tyi, tyv);
    }
    const int tw = tex->width, th = tex->height;
    uint32_t texel4[4];
    for (int j = 0; j < 4; j++) {
        int tx = txi[j], ty = tyi[j];
        if (tx < 0) tx = 0; else if (tx > tw - 1) tx = tw - 1;
        if (ty < 0) ty = 0; else if (ty > th - 1) ty = th - 1;
        texel4[j] = tex->pixels[(size_t)ty * tw + tx];
    }
    texel = _mm_loadu_si128((const __m128i*)texel4);
    }

    /* Paint pixel: REPLACE keeps the texel; MODULATE multiplies the
     * perspective-correct interpolated vertex color. */
    __m128i paint;
    if (tex_replace || !c0) {
        paint = texel;
    } else {
        __m128i col[4];
        for (int j = 0; j < 4; j++) {
            const float A0 = (float)c0[j] * iw0;
            const float A1 = (float)c1[j] * iw1;
            const float A2 = (float)c2[j] * iw2;
            __m128 num = _mm_add_ps(_mm_add_ps(
                _mm_mul_ps(w0q, _mm_set1_ps(A0)), _mm_mul_ps(w1q, _mm_set1_ps(A1))),
                _mm_mul_ps(w2q, _mm_set1_ps(A2)));
            __m128 val = _mm_div_ps(num, denq);
            __m128 cl = _mm_add_ps(_mm_min_ps(_mm_max_ps(val, zero4),
                _mm_set1_ps(255.0f)), _mm_set1_ps(0.5f));
            __m128 raw = _mm_add_ps(_mm_add_ps(
                _mm_mul_ps(w0q, _mm_set1_ps((float)c0[j])),
                _mm_mul_ps(w1q, _mm_set1_ps((float)c1[j]))),
                _mm_mul_ps(w2q, _mm_set1_ps((float)c2[j])));
            val = sbsl_f32(deg, raw, cl);
            col[j] = _mm_cvttps_epi32(val);
        }
        /* texfunc MODULATE: (vC * tC) / 255 per channel */
        __m128i pr = smod255(col[0], svchan_r(texel));
        __m128i pg = smod255(col[1], svchan_g(texel));
        __m128i pb = smod255(col[2], svchan_b(texel));
        __m128i pa = smod255(col[3], svchan_a(texel));
        paint = _mm_or_si128(_mm_or_si128(
            _mm_slli_epi32(_mm_and_si128(pr, _mm_set1_epi32(0xFF)), 16),
            _mm_slli_epi32(_mm_and_si128(pg, _mm_set1_epi32(0xFF)), 8)),
            _mm_and_si128(pb, _mm_set1_epi32(0xFF)));
        paint = _mm_or_si128(paint,
            _mm_slli_epi32(_mm_and_si128(pa, _mm_set1_epi32(0xFF)), 24));
    }

    /* Skip fully-transparent paint */
    {
        __m128i pa = _mm_and_si128(svchan_a(paint), _mm_set1_epi32(0xFF));
        mask = _mm_andnot_si128(_mm_cmpeq_epi32(pa, _mm_setzero_si128()), mask);
        if (!_mm_movemask_epi8(mask)) return 0;
    }

    /* Compositing stage */
    __m128i bg = _mm_loadu_si128((const __m128i*)&color_buf[idx]);
    __m128i final;
    switch (blend_mode) {
    case 68: { /* MODULATE: (fgC*bgC)/255, outA = (bgA*fgA)/255 */
        __m128i pr = smod255(svchan_r(paint), svchan_r(bg));
        __m128i pg = smod255(svchan_g(paint), svchan_g(bg));
        __m128i pb = smod255(svchan_b(paint), svchan_b(bg));
        __m128i pa = smod255(svchan_a(paint), svchan_a(bg));
        final = _mm_or_si128(_mm_or_si128(
            _mm_slli_epi32(_mm_and_si128(pr, _mm_set1_epi32(0xFF)), 16),
            _mm_slli_epi32(_mm_and_si128(pg, _mm_set1_epi32(0xFF)), 8)),
            _mm_and_si128(pb, _mm_set1_epi32(0xFF)));
        final = _mm_or_si128(final,
            _mm_slli_epi32(_mm_and_si128(pa, _mm_set1_epi32(0xFF)), 24));
        break;
    }
    case 67: { /* REPLACE: out = fg*(fa/255) + bg*(1 - fa/255), float */
        __m128 fa = _mm_div_ps(_mm_cvtepi32_ps(svchan_a(paint)),
                               _mm_set1_ps(255.0f));
        __m128 one_m = _mm_sub_ps(_mm_set1_ps(1.0f), fa);
        __m128i r = _mm_cvttps_epi32(_mm_add_ps(
            _mm_mul_ps(_mm_cvtepi32_ps(svchan_r(paint)), fa),
            _mm_mul_ps(_mm_cvtepi32_ps(svchan_r(bg)), one_m)));
        __m128i g = _mm_cvttps_epi32(_mm_add_ps(
            _mm_mul_ps(_mm_cvtepi32_ps(svchan_g(paint)), fa),
            _mm_mul_ps(_mm_cvtepi32_ps(svchan_g(bg)), one_m)));
        __m128i b = _mm_cvttps_epi32(_mm_add_ps(
            _mm_mul_ps(_mm_cvtepi32_ps(svchan_b(paint)), fa),
            _mm_mul_ps(_mm_cvtepi32_ps(svchan_b(bg)), one_m)));
        __m128i oa = _mm_cvttps_epi32(_mm_add_ps(
            _mm_cvtepi32_ps(svchan_a(paint)),
            _mm_mul_ps(_mm_cvtepi32_ps(svchan_a(bg)), one_m)));
        r = sclamp255(r); g = sclamp255(g); b = sclamp255(b); oa = sclamp255(oa);
        final = _mm_or_si128(_mm_or_si128(
            _mm_slli_epi32(_mm_and_si128(r, _mm_set1_epi32(0xFF)), 16),
            _mm_slli_epi32(_mm_and_si128(g, _mm_set1_epi32(0xFF)), 8)),
            _mm_and_si128(b, _mm_set1_epi32(0xFF)));
        final = _mm_or_si128(final,
            _mm_slli_epi32(_mm_and_si128(oa, _mm_set1_epi32(0xFF)), 24));
        break;
    }
    case 64: { /* ALPHA: aN = alpha/255; fgC*aN + bgC*(1-aN); outA same */
        __m128 aN = _mm_div_ps(_mm_cvtepi32_ps(svchan_a(paint)),
                               _mm_set1_ps(255.0f));
        __m128 one_m = _mm_sub_ps(_mm_set1_ps(1.0f), aN);
        __m128i r = _mm_cvttps_epi32(_mm_add_ps(
            _mm_mul_ps(_mm_cvtepi32_ps(svchan_r(paint)), aN),
            _mm_mul_ps(_mm_cvtepi32_ps(svchan_r(bg)), one_m)));
        __m128i g = _mm_cvttps_epi32(_mm_add_ps(
            _mm_mul_ps(_mm_cvtepi32_ps(svchan_g(paint)), aN),
            _mm_mul_ps(_mm_cvtepi32_ps(svchan_g(bg)), one_m)));
        __m128i b = _mm_cvttps_epi32(_mm_add_ps(
            _mm_mul_ps(_mm_cvtepi32_ps(svchan_b(paint)), aN),
            _mm_mul_ps(_mm_cvtepi32_ps(svchan_b(bg)), one_m)));
        __m128i oa = _mm_cvttps_epi32(_mm_add_ps(
            _mm_mul_ps(_mm_cvtepi32_ps(svchan_a(bg)), one_m),
            _mm_mul_ps(_mm_cvtepi32_ps(svchan_a(paint)), aN)));
        r = sclamp255(r); g = sclamp255(g); b = sclamp255(b); oa = sclamp255(oa);
        final = _mm_or_si128(_mm_or_si128(
            _mm_slli_epi32(_mm_and_si128(r, _mm_set1_epi32(0xFF)), 16),
            _mm_slli_epi32(_mm_and_si128(g, _mm_set1_epi32(0xFF)), 8)),
            _mm_and_si128(b, _mm_set1_epi32(0xFF)));
        final = _mm_or_si128(final,
            _mm_slli_epi32(_mm_and_si128(oa, _mm_set1_epi32(0xFF)), 24));
        break;
    }
    default: { /* 65 ALPHA_ADD */
        __m128 aN = _mm_div_ps(_mm_cvtepi32_ps(svchan_a(paint)),
                               _mm_set1_ps(255.0f));
        __m128i r = _mm_cvttps_epi32(_mm_min_ps(_mm_set1_ps(255.0f), _mm_add_ps(
            _mm_mul_ps(_mm_cvtepi32_ps(svchan_r(paint)), aN),
            _mm_cvtepi32_ps(svchan_r(bg)))));
        __m128i g = _mm_cvttps_epi32(_mm_min_ps(_mm_set1_ps(255.0f), _mm_add_ps(
            _mm_mul_ps(_mm_cvtepi32_ps(svchan_g(paint)), aN),
            _mm_cvtepi32_ps(svchan_g(bg)))));
        __m128i b = _mm_cvttps_epi32(_mm_min_ps(_mm_set1_ps(255.0f), _mm_add_ps(
            _mm_mul_ps(_mm_cvtepi32_ps(svchan_b(paint)), aN),
            _mm_cvtepi32_ps(svchan_b(bg)))));
        __m128 one_m_bg = _mm_sub_ps(_mm_set1_ps(1.0f),
            _mm_div_ps(_mm_cvtepi32_ps(svchan_a(bg)), _mm_set1_ps(255.0f)));
        __m128i inner = _mm_cvttps_epi32(_mm_mul_ps(
            _mm_cvtepi32_ps(svchan_a(paint)), one_m_bg));
        __m128i oa = _mm_cvttps_epi32(_mm_min_ps(_mm_set1_ps(255.0f),
            _mm_cvtepi32_ps(_mm_add_epi32(svchan_a(bg), inner))));
        r = sclamp255(r); g = sclamp255(g); b = sclamp255(b); oa = sclamp255(oa);
        final = _mm_or_si128(_mm_or_si128(
            _mm_slli_epi32(_mm_and_si128(r, _mm_set1_epi32(0xFF)), 16),
            _mm_slli_epi32(_mm_and_si128(g, _mm_set1_epi32(0xFF)), 8)),
            _mm_and_si128(b, _mm_set1_epi32(0xFF)));
        final = _mm_or_si128(final,
            _mm_slli_epi32(_mm_and_si128(oa, _mm_set1_epi32(0xFF)), 24));
        break;
    }
    }

    /* Masked stores: untouched lanes keep their old values */
    __m128i out = sbsl_u32(mask, final, bg);
    _mm_storeu_si128((__m128i*)&color_buf[idx], out);
    if (do_depth_write) {
        if (!do_depth_test) dep = _mm_loadu_ps(&depth_buf[idx]);
        __m128 newd = sbsl_f32(mask, zq, dep);
        _mm_storeu_ps(&depth_buf[idx], newd);
    }
    const int bits = _mm_movemask_ps(_mm_castsi128_ps(mask)); /* 1 bit/lane */
    return (bits & 1) + ((bits >> 1) & 1) + ((bits >> 2) & 1) + ((bits >> 3) & 1);
}
#endif /* RENDER_HAVE_SSE2 */

/* Rasterize a single triangle - matches FreeJ2ME Graphics3D scanline rendering
 * Optimized for ARMv7: incremental edge functions, pre-computed row pointers,
 * cached inverse area, and local buffer stride to avoid repeated struct access.
 * v34.18: fog_d0/1/2 carry the per-vertex VIEW-SPACE DEPTH (parallel-camera
 * fog fix, audit item 1.3); NULL falls back to the v14 1/w proxy. */
/* v34.42 PERF: resolve the per-MESH raster state (see render.h M3GRastCtx).
 * Everything computed here was previously re-computed on EVERY triangle
 * inside m3g_rasterize_triangle — the gprof Asphalt profile attributed 35%
 * total CPU to that function's SELF time, most of it this per-triangle
 * fixed overhead. Values are identical to the ones the old per-triangle
 * prologue produced (same sources, same order); snapshot timing is safe:
 * g_m3g.* flags/buffers only change between meshes (set by
 * m3g_render_single_mesh before its triangle loop / bindTarget). */
void m3g_raster_begin_mesh(M3GRastCtx* ctx, M3GAppearance* appearance) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->appearance = appearance;
    ctx->culling_enabled = g_m3g.culling_enabled;
    /* v34.88 self-heal: fresh per-mesh facing statistics */
    g_m3g_mesh_facing_submitted = 0;
    g_m3g_mesh_facing_culled = 0;
    g_m3g_mesh_drawn = 0;
    ctx->depth_test = g_m3g.depth_test_enabled;
    ctx->depth_write = g_m3g.depth_write_enabled;
    ctx->color_buf = g_m3g.color_buffer;
    ctx->depth_buf = g_m3g.depth_buffer;
    ctx->buf_w = g_m3g.buffer_width;
    ctx->buf_h = g_m3g.buffer_height;
    ctx->stride = g_m3g.buffer_width;
    /* v34.54: viewport rect for span clipping (see render.h). Guard against
     * a stale/invalid viewport larger than the buffer: clamp to the buffer. */
    {
        int vx0 = g_m3g.viewport_x;
        int vy0 = g_m3g.viewport_y;
        int vx1 = g_m3g.viewport_x + g_m3g.viewport_width - 1;
        int vy1 = g_m3g.viewport_y + g_m3g.viewport_height - 1;
        if (vx0 < 0) vx0 = 0;
        if (vy0 < 0) vy0 = 0;
        if (vx1 > ctx->buf_w - 1) vx1 = ctx->buf_w - 1;
        if (vy1 > ctx->buf_h - 1) vy1 = ctx->buf_h - 1;
        if (vx1 < vx0 || vy1 < vy0) { vx0 = vy0 = 0; vx1 = ctx->buf_w - 1; vy1 = ctx->buf_h - 1; }
        ctx->vp_x0 = vx0; ctx->vp_y0 = vy0; ctx->vp_x1 = vx1; ctx->vp_y1 = vy1;
    }
    ctx->rast_trace_on = g_m3g_rast_trace_on;

    /* FIX 43: per-mesh CompositingMode (was re-read per triangle) */
    ctx->blend_mode = 64;               /* ALPHA */
    ctx->texture_blend = 68;           /* FUNC_MODULATE (JSR-184 default) */
    ctx->alpha_threshold = 0.0f;
    ctx->alpha_factor = 1.0f;
    if (appearance) {
        if (appearance->blend_mode >= 0) {
            ctx->blend_mode = appearance->blend_mode;
            /* v12: defensive file-index mapping (see CompositingMode parser fix) */
            if (ctx->blend_mode >= 0 && ctx->blend_mode <= 5) {
                static const uint8_t blend_map[6] = { 64, 65, 68, 69, 67, 70 };
                ctx->blend_mode = blend_map[ctx->blend_mode];
            }
        }
        if (appearance->alpha_threshold >= 0) {
            ctx->alpha_threshold = (float)appearance->alpha_threshold / 255.0f;
        }
        if (appearance->texture_blend > 0) ctx->texture_blend = appearance->texture_blend;
        /* v34.18 CRITICAL FIX (audit item 1.3, latent bug): propagate the
         * per-mesh fog block (was dead code when read per-triangle into a
         * memset-zero rstate; semantics preserved verbatim here). */
        if (appearance->fog_enabled) {
            ctx->has_fog = 1;
            ctx->fog_mode = appearance->fog_mode;
            ctx->fog_density = appearance->fog_density;
            ctx->fog_near = appearance->fog_near;
            ctx->fog_far = appearance->fog_far;
            ctx->fog_color = appearance->fog_color;
        }
        ctx->two_sided_lighting = appearance->two_sided_lighting;
        ctx->winding = appearance->winding;
        ctx->cull_front = appearance->cull_front;
        ctx->cull_flip = appearance->cull_flip;
        /* v30 (JSR-184 §Node.setAlphaFactor / §CompositingMode.setDepthOffset) */
        ctx->alpha_factor = appearance->alpha_factor;
        if (!(ctx->alpha_factor >= 0.0f && ctx->alpha_factor <= 1.0f)) ctx->alpha_factor = 1.0f;
        ctx->depth_offset_factor = appearance->depth_offset_factor;
        ctx->z_offset_units_part = appearance->depth_offset_units * (1.0f / 16777216.0f);
        ctx->mesh_has_texture1 = (appearance->texture1 && appearance->tex1_coords) ? 1 : 0;
    }

    /* v34.79 (j2me_texture_smoothing): resolve the filter override once per
     * mesh — span_smooth tells the SIMD span to run its bilinear sampler,
     * and the eligibility below becomes mode-aware so the span and the
     * scalar sampler can never disagree on the effective filter:
     *   auto(0): span is nearest-only → LINEAR-requested textures stay on
     *            the scalar legacy float path (bit-identical to v34.78);
     *   nearest(1): sampler forces nearest → span nearest matches exactly,
     *            so LINEAR textures become span-eligible too;
     *   smooth(2): span samples integer bilinear, bit-exact with the
     *            scalar m3g_sample_texture_smooth → everything eligible. */
    const int filt_ov = m3g_texture_filter_override_resolved();
    ctx->span_smooth = (filt_ov == 2);

    /* v34.24 NEON / v34.51 SSE2 span eligibility — per-mesh (appearance)
     * part; the per-triangle texcoord presence (t0/t1/t2 non-NULL) is ANDed
     * in the raster body. v34.42: the ~15-branch check now runs once per
     * mesh. The M3GRastCtx neon_* fields carry the SIMD span state on both
     * ARM (NEON) and x86-64 (SSE2) builds. */
#if RENDER_HAVE_NEON || RENDER_HAVE_SSE2
    {
        if (g_m3g_neon_force_scalar < 0)
            g_m3g_neon_force_scalar = getenv("NOJME_NEON_SCALAR") ? 1 : 0;
        if (!g_m3g_neon_force_scalar && !ctx->rast_trace_on &&
            appearance && appearance->texture &&
            !appearance->texture->has_uv_transform &&
            (filt_ov != 0 ||
             (appearance->texture->filter_level != 209 &&
              appearance->texture->filter_level != 211)) &&
            /* texture validity: the scalar sampler returns white for
             * NULL/empty textures; the SIMD gather must not read them
             * (SU-30 renders meshes with not-yet-linked Image2D). */
            appearance->texture->pixels &&
            appearance->texture->width > 0 &&
            appearance->texture->height > 0 &&
            !(ctx->alpha_threshold > 0.0f) && ctx->alpha_factor == 1.0f &&
            !ctx->has_fog && !ctx->mesh_has_texture1 &&
            (ctx->texture_blend == 68 || ctx->texture_blend == 67) &&
            (ctx->blend_mode == 67 || ctx->blend_mode == 68 ||
             ctx->blend_mode == 64 || ctx->blend_mode == 65)) {
            ctx->neon_tex = appearance->texture;
            ctx->neon_core = 1;
        }
        ctx->neon_tex_replace = (ctx->texture_blend == 67);
    }
#endif
}

/* Context-fed rasterizer: the ORIGINAL body of m3g_rasterize_triangle with
 * the per-mesh prologue replaced by M3GRastCtx reads (same local names, so
 * the span/pixel loops below are untouched — bit-identical by construction). */
M3G_HOT void m3g_rasterize_triangle_ctx(const M3GRastCtx* R,
                                    const float* v0, const float* v1, const float* v2,
                                    const float* t0, const float* t1, const float* t2,
                                    const float* t1_0, const float* t1_1, const float* t1_2,
                                    const uint8_t* c0, const uint8_t* c1, const uint8_t* c2,
                                    const float* fog_d0, const float* fog_d1, const float* fog_d2) {
    M3GAppearance* appearance = R->appearance;   /* per-pixel sampler block reads */
    { static int once = 1; if (once && getenv("NOJME_NEON_STATS")) { once = 0; j2me_log_ungated("[NEON-STAT] FIRST rasterize call!\n"); } }
    /* Screen coordinates */
    float s0[4], s1[4], s2[4];
    if (m3g_clip_to_screen(s0, v0) < 0) { g_m3g_rj_clip2screen++; return; }
    if (m3g_clip_to_screen(s1, v1) < 0) { g_m3g_rj_clip2screen++; return; }
    if (m3g_clip_to_screen(s2, v2) < 0) { g_m3g_rj_clip2screen++; return; }

    /* Skip triangles outside viewport (depth-based) */
    if (s0[2] < 0 && s1[2] < 0 && s2[2] < 0) { g_m3g_rj_depth++; return; }
    if (s0[2] > 1 && s1[2] > 1 && s2[2] > 1) { g_m3g_rj_depth++; return; }

    /* Back-face culling (see the v32 winding FIX comment in the original
     * prologue — semantics identical, flags now hoisted into R). */
    if (R->culling_enabled) {
        int is_two_sided = R->two_sided_lighting;
        if (!is_two_sided) {
            float e = m3g_edge_function(s0, s1, s2);
            int winding = R->winding;
            int is_front;
            if (winding == 0) { /* CCW front face (Y-down screen): positive e = front */
                is_front = (e > 0.0f);
            } else { /* CW front face: negative e = front */
                is_front = (e < 0.0f);
            }
            /* v34.88 self-heal: meshes detected as authored for the OPPOSITE
             * winding convention (every visible triangle back-facing for
             * several consecutive renders — Brick Breaker Revolution 3D's
             * arena/paddle/bricks under CULLING_BACK+WINDING_CCW) get their
             * facing decision inverted per-PolygonMode. Spec-conform meshes
             * always render a mix of front/back triangles and never trip the
             * detector, so this is a no-op for them. */
            if (R->cull_flip) is_front = !is_front;
            g_m3g_mesh_facing_submitted++;
            int cull_front = R->cull_front;
            if (cull_front) {
                /* CULL_FRONT: discard front-facing triangles */
                if (is_front) { g_m3g_rj_cull++; g_m3g_mesh_facing_culled++; return; }
            } else {
                /* CULL_BACK: discard back-facing triangles */
                if (!is_front) { g_m3g_rj_cull++; g_m3g_mesh_facing_culled++; return; }
            }
        }
    }

    /* Bounding box */
    float fmin_x = s0[0] < s1[0] ? (s0[0] < s2[0] ? s0[0] : s2[0]) : (s1[0] < s2[0] ? s1[0] : s2[0]);
    float fmax_x = s0[0] > s1[0] ? (s0[0] > s2[0] ? s0[0] : s2[0]) : (s1[0] > s2[0] ? s1[0] : s2[0]);
    float fmin_y = s0[1] < s1[1] ? (s0[1] < s2[1] ? s0[1] : s2[1]) : (s1[1] < s2[1] ? s1[1] : s2[1]);
    float fmax_y = s0[1] > s1[1] ? (s0[1] > s2[1] ? s0[1] : s2[1]) : (s1[1] > s2[1] ? s1[1] : s2[1]);

    int min_x = (int)fmin_x;
    int max_x = (int)fmax_x;
    int min_y = (int)fmin_y;
    int max_y = (int)fmax_y;

    /* Clip to viewport - v34.54: JSR-184 rasterization is bounded by the
     * VIEWPORT (the bindTarget clip), not the raw buffer. Prevents spans
     * from spilling over HUD bands / outside the game's screen layout. */
    int vp_x0 = R->vp_x0, vp_y0 = R->vp_y0, vp_x1 = R->vp_x1, vp_y1 = R->vp_y1;

    /* Early exit for fully offscreen triangles */
    if (max_x < vp_x0 || min_x > vp_x1 || max_y < vp_y0 || min_y > vp_y1) { g_m3g_rj_offscreen++; return; }

    if (min_x < vp_x0) min_x = vp_x0;
    if (max_x > vp_x1) max_x = vp_x1;
    if (min_y < vp_y0) min_y = vp_y0;
    if (max_y > vp_y1) max_y = vp_y1;

    /* Area for barycentric - pre-compute inverse to avoid division per pixel */
    float area = m3g_edge_function(s0, s1, s2);
    if (fabsf(area) < 0.001f) { g_m3g_rj_area++; return; }
    g_m3g_rj_pass++;
    g_m3g_mesh_drawn++;   /* v34.88 self-heal: reached the fill stage */
    float inv_area = 1.0f / area;

    /* v34.42: render state comes from the per-mesh context (was a memset+
     * re-read per triangle — see m3g_raster_begin_mesh). */
    int do_depth_test = R->depth_test;
    int do_depth_write = R->depth_write;
    int blend_mode = R->blend_mode;
    int texture_blend = R->texture_blend;
    float alpha_threshold = R->alpha_threshold;
    float alpha_factor = R->alpha_factor;
    int has_fog = R->has_fog;
    int fog_mode = R->fog_mode;
    float fog_density = R->fog_density;
    float fog_near = R->fog_near;
    float fog_far = R->fog_far;
    int fog_color = R->fog_color;

    /* Cache buffer pointers locally for the inner loop */
    uint32_t* color_buf = R->color_buf;
    float* depth_buf = R->depth_buf;
    int stride = R->stride;  /* pixels per row */

    /* Pre-compute edge function increments for scanline optimization. */
    float row0_step_x = s2[1] - s1[1];  /* dw0/dx */
    float row0_step_y = -(s2[0] - s1[0]); /* dw0/dy */
    float row1_step_x = s0[1] - s2[1];  /* dw1/dx */
    float row1_step_y = -(s0[0] - s2[0]); /* dw1/dy */
    float row2_step_x = s1[1] - s0[1];  /* dw2/dx */
    float row2_step_y = -(s1[0] - s0[0]); /* dw2/dy */

    /* Pre-compute edge function at top-left corner (min_x + 0.5, min_y + 0.5) */
    float start_px = min_x + 0.5f;
    float start_py = min_y + 0.5f;
    float row_w0 = ((start_px - s1[0]) * (s2[1] - s1[1]) - (start_py - s1[1]) * (s2[0] - s1[0]));
    float row_w1 = ((start_px - s2[0]) * (s0[1] - s2[1]) - (start_py - s2[1]) * (s0[0] - s2[0]));
    float row_w2 = ((start_px - s0[0]) * (s1[1] - s0[1]) - (start_py - s0[1]) * (s1[0] - s0[0]));

    /* Has vertex colors flag - hoisted out of inner loop */
    int has_vertex_colors = (c0 && c1 && c2);
    int has_texture = (appearance && appearance->texture && t0 && t1 && t2);
    /* v30: second texture unit — active only with both a unit-1 Texture2D AND
     * per-vertex unit-1 coordinates (Appearance.setTexture(1) +
     * VertexBuffer.setTexCoords(1)). mesh_has_texture1 hoisted per mesh;
     * t1_* NULLness stays per triangle. */
    int has_texture1 = (R->mesh_has_texture1 && t1_0 && t1_1 && t1_2);
    /* v30 (JSR-184 §CompositingMode.setDepthOffset): polygon offset —
     * factor*max(depth slope) + units*(1/2^24). The units part is
     * per-mesh (R); the slope needs this triangle's area — per-triangle. */
    float z_offset = R->z_offset_units_part;
    if (R->depth_offset_factor != 0.0f) {
        /* v34.54 DIAG (Ferrari road sliver): NOJME_NO_DEPTH_OFFSET=1
         * zeroes the factor part — A/B test whether the road mesh's
         * huge grazing-angle offsets reject its own near triangles. */
        static int no_doff = -1;
        if (no_doff < 0) no_doff = getenv("NOJME_NO_DEPTH_OFFSET") ? 1 : 0;
        if (!no_doff) {
        /* per-triangle depth slope from the plane equation:
         * dz/dx, dz/dy over screen space; area == 2*signed area (edge fn). */
        float dzdx = ((s1[2] - s0[2]) * (s2[1] - s0[1]) -
                      (s2[2] - s0[2]) * (s1[1] - s0[1])) / area;
        float dzdy = ((s2[2] - s0[2]) * (s1[0] - s0[0]) -
                      (s1[2] - s0[2]) * (s2[0] - s0[0])) / area;
        float slope = dzdx > dzdy ? (dzdx > 0.0f ? dzdx : -dzdx)
                                  : (dzdy > 0.0f ? dzdy : -dzdy);
        z_offset += R->depth_offset_factor * slope;
        }
    }

    /* Rasterize - incremental edge functions avoid redundant multiplications per pixel */
    int pixels_this_tri = 0;
    /* v32 DIAG (NOJME_TRACE_BBOX): record rasterized-pixel bounds to compare
     * with the projected vertex bbox (exposes clip->screen mismatches). */
    extern int g_m3g_rast_trace_on;
    int rast_x0 = 1 << 29, rast_x1 = -(1 << 29), rast_y0 = 1 << 29, rast_y1 = -(1 << 29);

#if RENDER_HAVE_NEON || RENDER_HAVE_SSE2
    /* v34.41 DIAG (NOJME_NEON_STATS): eligibility hit-rate counters.
     * v34.42: getenv resolved ONCE (was: once per triangle — a libc
     * hash-table lookup on the hottest path). */
    static long st_total, st_neon, st_rej_notex, st_rej_cfg, st_rej_fog, st_rej_uv;
    static int st_on = -1;
    if (st_on < 0) st_on = getenv("NOJME_NEON_STATS") ? 1 : 0;
    if (st_on) {
        st_total++;
        if (st_total % 100000 == 1)
            j2me_log_ungated("[NEON-STAT] tri=%ld (neon=%ld) rej: notex=%ld cfg=%ld fog=%ld uv=%ld\n",
                    st_total, st_neon, st_rej_notex, st_rej_cfg, st_rej_fog, st_rej_uv);
        if (!has_texture || !appearance || !appearance->texture) st_rej_notex++;
        else if (has_texture1) st_rej_cfg++;
        else if (R->has_fog) st_rej_fog++;
        else if (appearance->texture->has_uv_transform) st_rej_uv++;
        else if (!(texture_blend == 68 || texture_blend == 67) ||
                 !(blend_mode == 67 || blend_mode == 68 || blend_mode == 64 || blend_mode == 65) ||
                 (R->span_smooth == 0 &&
                  (appearance->texture->filter_level == 209 ||
                   appearance->texture->filter_level == 211))) st_rej_cfg++;
    }
    /* v34.24 NEON fast-path eligibility: the per-mesh core (R->neon_core,
     * resolved by m3g_raster_begin_mesh) ANDed with the per-triangle
     * texcoord presence. Anything more exotic (fog, multitexture, UV
     * transform, auto-mode bilinear filter, alpha threshold, alphaFactor,
     * DECAL/BLEND/ADD texfuncs) keeps the scalar loop below. v34.79: with
     * j2me_texture_smoothing=nearest/smooth the filter-level rejection is
     * lifted (the span sampler matches the overridden scalar sampler). */
    int use_neon_span = 0;
    M3GTexture2D* neon_tex = NULL;
    {
        if (R->neon_core && t0 && t1 && t2 && !has_texture1) {
            neon_tex = R->neon_tex;
            use_neon_span = 1;
        }
        if (st_on && use_neon_span) {
            st_neon++;
            if (st_neon % 100000 == 1)
                j2me_log_ungated("[NEON-STAT] tri=%ld neon=%ld (%.1f%%) rej: notex=%ld cfg=%ld fog=%ld uv=%ld\n",
                        st_total, st_neon, st_neon * 100.0 / st_total,
                        st_rej_notex, st_rej_cfg, st_rej_fog, st_rej_uv);
        }
    }
    const int neon_tex_replace = R->neon_tex_replace;
    /* v34.79: R->span_smooth — the SIMD span bilinear sampler flag (see
     * m3g_raster_begin_mesh); loop-invariant, read once per triangle. */
    const int span_smooth = R->span_smooth;
    /* Same value the scalar loop adds on every x-step (loop-invariant). */
    const float neon_step0 = row0_step_x * inv_area;
    const float neon_step1 = row1_step_x * inv_area;
    const float neon_step2 = row2_step_x * inv_area;
#endif

    for (int y = min_y; y <= max_y; y++) {
        /* Reset w0/w1/w2 for this row using incremental step from previous row */
        float w0 = row_w0 * inv_area;
        float w1 = row_w1 * inv_area;
        float w2 = row_w2 * inv_area;

        int row_idx = y * stride;

        /* v34.35 PERF (⑤ scanline prefetch): the span loop below reads
         * depth_buf/color_buf of THIS row and the NEXT row's lines are not
         * in cache yet (rows are walked top→down; a 64-byte line holds 16
         * u32). Prefetch the next row's span region at row start so the
         * loads hit L1 by the time the loop reaches them. Pure hint —
         * no architectural state, bit-identical output. */
        if (y + 1 <= max_y) {
            __builtin_prefetch(&depth_buf[(y + 1) * stride + min_x], 1, 1);
            __builtin_prefetch(&color_buf[(y + 1) * stride + min_x], 1, 0);
        }

        int x = min_x;
#if RENDER_HAVE_NEON || RENDER_HAVE_SSE2
        if (use_neon_span) {
            /* 4-wide spans for the bulk of the row; the (up to 3) tail
             * pixels fall through to the scalar loop below. Barycentrics
             * advance with four sequential adds — bit-identical to the
             * scalar stepping. */
            int quads = (max_x - min_x + 1) >> 2;
            for (int q = 0; q < quads; q++) {
#if RENDER_HAVE_NEON
                pixels_this_tri += m3g_span4_neon(color_buf, depth_buf, row_idx,
                        min_x + (q << 2), s0, s1, s2, w0, w1, w2,
                        neon_step0, neon_step1, neon_step2,
                        t0, t1, t2, c0, c1, c2,
                        neon_tex, neon_tex_replace, span_smooth, blend_mode,
                        do_depth_test, do_depth_write, z_offset);
#else /* RENDER_HAVE_SSE2: bit-identical mirror, IEEE per-lane division */
                pixels_this_tri += m3g_span4_sse2(color_buf, depth_buf, row_idx,
                        min_x + (q << 2), s0, s1, s2, w0, w1, w2,
                        neon_step0, neon_step1, neon_step2,
                        t0, t1, t2, c0, c1, c2,
                        neon_tex, neon_tex_replace, span_smooth, blend_mode,
                        do_depth_test, do_depth_write, z_offset);
#endif
                w0 += neon_step0; w0 += neon_step0; w0 += neon_step0; w0 += neon_step0;
                w1 += neon_step1; w1 += neon_step1; w1 += neon_step1; w1 += neon_step1;
                w2 += neon_step2; w2 += neon_step2; w2 += neon_step2; w2 += neon_step2;
            }
            x = min_x + (quads << 2);
        }
#endif
        for (; x <= max_x; x++) {
            /* Inside triangle? (all barycentric >= 0) */
            if (w0 >= 0 && w1 >= 0 && w2 >= 0) {
                /* Depth test (v30: z + polygon offset for decals). */
                float z = s0[2] * w0 + s1[2] * w1 + s2[2] * w2 + z_offset;
                int idx = row_idx + x;
                
                if (do_depth_test && depth_buf[idx] < z) {
                    /* Existing pixel is closer, skip */
                } else {
                    if (g_m3g_rast_trace_on) {
                        if (x < rast_x0) rast_x0 = x;
                        if (x > rast_x1) rast_x1 = x;
                        if (y < rast_y0) rast_y0 = y;
                        if (y > rast_y1) rast_y1 = y;
                    }
                    /* Perspective correction; 1/w interpolated here is also
                     * the fog distance FALLBACK: w_clip = view distance for
                     * the standard perspective projection (v14 fog fix).
                     * v34.18 (audit item 1.3): when the caller supplied
                     * per-vertex VIEW DEPTHS (fog_d*), interpolate those
                     * perspective-correctly instead — this is the only
                     * correct path for PARALLEL cameras, where w is a
                     * constant 1.0 and the 1/w proxy degenerates to a
                     * constant (fog "wall" / disabled). */
                    float fog_dist;
                    if (fog_d0 && fog_d1 && fog_d2) {
                        float iw_sum = w0 * s0[3] + w1 * s1[3] + w2 * s2[3];
                        if (iw_sum > 1e-9f) {
                            fog_dist = (w0 * fog_d0[0] * s0[3] +
                                        w1 * fog_d1[0] * s1[3] +
                                        w2 * fog_d2[0] * s2[3]) / iw_sum;
                        } else {
                            fog_dist = fog_d0[0];
                        }
                        if (fog_dist < 0.0f) fog_dist = 0.0f;
                    } else {
                        float one_over_w = s0[3] * w0 + s1[3] * w1 + s2[3] * w2;
                        fog_dist = (one_over_w > 1e-9f) ? (1.0f / one_over_w) : 0.0f;
                    }
                    
                    /* Interpolate color (v14: perspective-correct) */
                    uint8_t color[4] = {255, 255, 255, 255};
                    if (has_vertex_colors) {
                        m3g_interpolate_color(color, c0, c1, c2, w0, w1, w2,
                                              s0[3], s1[3], s2[3]);
                    }
                    
                    /* Determine the paint pixel */
                    uint32_t paint_pixel;
                    
                    /* Texture sampling */
                    if (has_texture) {
                        float uv[2];
                        /* v14 FIX: pass per-vertex 1/w (screen[3]) for true
                         * perspective-correct UV interpolation. The old code
                         * multiplied screen-linear UV by the interpolated 1/w,
                         * collapsing the whole texture into a few texels
                         * (Nescube black cube) and stretching it nonlinearly
                         * across faces (SU30 / M3GTest scene 4). */
                        m3g_interpolate_texcoord(uv, t0, t1, t2, w0, w1, w2,
                                                 s0[3], s1[3], s2[3]);
                        /* v14: Texture2D.setTransform — affine UV transform
                         * (row-major 4x4; typical usage is UV scrolling). */
                        if (appearance->texture->has_uv_transform) {
                            const float* M = appearance->texture->uv_transform;
                            float tu = M[0] * uv[0] + M[1] * uv[1] + M[3];
                            float tv = M[4] * uv[0] + M[5] * uv[1] + M[7];
                            uv[0] = tu;
                            uv[1] = tv;
                        }
                        uint32_t tex_color = m3g_sample_texture(appearance->texture, uv[0], uv[1]);
                        /* v14 DIAG: first texture samples per run (v34.24:
                         * gated — silent in release, NOJME_TRACE_RAST=1 or
                         * debug mode re-enables) */
                        {
                            static int sample_diag = 0;
                            /* v39: budget lifted when explicitly tracing */
                            static int sample_budget = -1;
                            if (sample_budget < 0) {
                                const char* sb = getenv("NOJME_TRACE_RAST");
                                sample_budget = (sb && sb[0] == '2') ? 200000 : 10;
                            }
                            if (render_diag_on() && sample_diag < sample_budget) {
                                sample_diag++;
                                fprintf(stderr, "[M3G-SAMP] uv=(%.4f,%.4f) texel=%08X tblend=%d cblend=%d vcol=%d\n",
                                        uv[0], uv[1], tex_color, texture_blend, blend_mode,
                                        has_vertex_colors ? 1 : 0);
                            }
                        }
                        
                        paint_pixel = tex_color;
                        
                        /* Check alpha threshold */
                        int tex_alpha = (tex_color >> 24) & 0xFF;
                        if (tex_alpha < (int)(alpha_threshold * 255)) {
                            /* Skip transparent pixels below threshold */
                        } else if (has_vertex_colors) {
                            uint32_t vert_color = ((uint32_t)(color[3]) <<  24) | (color[0] << 16) | (color[1] << 8) | (color[2]);
                            /* v31: JSR-184 texture-stage blending via the dedicated
                             * texfunc blender. The old code reused CompositingMode
                             * semantics - MODULATE max()'d the alpha (opaque glow
                             * panes), DECAL/BLEND/ADD used compositing formulas.
                             * blend_color feeds the FUNC_BLEND function. */
                            int bc = appearance->texture ? appearance->texture->blend_color : 0;
                            paint_pixel = m3g_texfunc_blend(vert_color, tex_color, texture_blend,
                                                            (uint32_t)bc);
                            
                            /* v30: second texture unit MODULATE (JSR-184
                             * multitexturing) then Node.alphaFactor. */
                            if (has_texture1) {
                                float uv1[2];
                                m3g_interpolate_texcoord(uv1, t1_0, t1_1, t1_2, w0, w1, w2,
                                                         s0[3], s1[3], s2[3]);
                                if (appearance->texture1->has_uv_transform) {
                                    const float* M1 = appearance->texture1->uv_transform;
                                    float tu = M1[0] * uv1[0] + M1[1] * uv1[1] + M1[3];
                                    float tv = M1[4] * uv1[0] + M1[5] * uv1[1] + M1[7];
                                    uv1[0] = tu;
                                    uv1[1] = tv;
                                }
                                uint32_t t1c = m3g_sample_texture(appearance->texture1, uv1[0], uv1[1]);
                                int mr = (int)(((paint_pixel >> 16) & 0xFF) * ((t1c >> 16) & 0xFF) / 255);
                                int mg = (int)(((paint_pixel >> 8) & 0xFF) * ((t1c >> 8) & 0xFF) / 255);
                                int mb = (int)((paint_pixel & 0xFF) * (t1c & 0xFF) / 255);
                                if (mr > 255) mr = 255;
                                if (mg > 255) mg = 255;
                                if (mb > 255) mb = 255;
                                paint_pixel = (paint_pixel & 0xFF000000u) |
                                              ((uint32_t)mr << 16) | ((uint32_t)mg << 8) | (uint32_t)mb;
                            }
                            int paint_alpha = (int)(((paint_pixel >> 24) & 0xFF) * alpha_factor + 0.5f);
                            if (paint_alpha < 0) paint_alpha = 0;
                            if (paint_alpha > 255) paint_alpha = 255;
                            if (paint_alpha == 0) { /* fully transparent — no write */ }
                            else {
                            if (has_fog) {
                                float fog_factor = 0.0f;
                                if (fog_mode == 1) { /* LINEAR */
                                    float range = fog_far - fog_near;
                                    if (range > 0.0001f) {
                                        /* v14 FIX: use VIEW distance (1/interp 1/w),
                                         * not NDC depth (0..1). NDC z made fog
                                         * distances meaningless. */
                                        fog_factor = (fog_far - fog_dist) / range;
                                        if (fog_factor < 0.0f) fog_factor = 0.0f;
                                        if (fog_factor > 1.0f) fog_factor = 1.0f;
                                    }
                                } else { /* EXPONENTIAL */
                                    fog_factor = expf(-fog_density * fog_dist);
                                    if (fog_factor < 0.0f) fog_factor = 0.0f;
                                    if (fog_factor > 1.0f) fog_factor = 1.0f;
                                }
                                paint_pixel = m3g_blend_fog(paint_pixel, fog_color, fog_factor);
                            }
                            
                            uint32_t bg_pixel = color_buf[idx];
                            uint32_t final_pixel = m3g_blend_pixels(bg_pixel, paint_pixel, paint_alpha, blend_mode);
                            color_buf[idx] = final_pixel;
                            if (do_depth_write) depth_buf[idx] = z;
                            pixels_this_tri++;
                            }
                        } else {
                            /* v30: unit-1 modulate + alphaFactor (see branch above). */
                            if (has_texture1) {
                                float uv1[2];
                                m3g_interpolate_texcoord(uv1, t1_0, t1_1, t1_2, w0, w1, w2,
                                                         s0[3], s1[3], s2[3]);
                                if (appearance->texture1->has_uv_transform) {
                                    const float* M1 = appearance->texture1->uv_transform;
                                    float tu = M1[0] * uv1[0] + M1[1] * uv1[1] + M1[3];
                                    float tv = M1[4] * uv1[0] + M1[5] * uv1[1] + M1[7];
                                    uv1[0] = tu;
                                    uv1[1] = tv;
                                }
                                uint32_t t1c = m3g_sample_texture(appearance->texture1, uv1[0], uv1[1]);
                                int mr = (int)(((paint_pixel >> 16) & 0xFF) * ((t1c >> 16) & 0xFF) / 255);
                                int mg = (int)(((paint_pixel >> 8) & 0xFF) * ((t1c >> 8) & 0xFF) / 255);
                                int mb = (int)((paint_pixel & 0xFF) * (t1c & 0xFF) / 255);
                                if (mr > 255) mr = 255;
                                if (mg > 255) mg = 255;
                                if (mb > 255) mb = 255;
                                paint_pixel = (paint_pixel & 0xFF000000u) |
                                              ((uint32_t)mr << 16) | ((uint32_t)mg << 8) | (uint32_t)mb;
                            }
                            int paint_alpha = (int)(((paint_pixel >> 24) & 0xFF) * alpha_factor + 0.5f);
                            if (paint_alpha < 0) paint_alpha = 0;
                            if (paint_alpha > 255) paint_alpha = 255;
                            if (paint_alpha == 0) { /* fully transparent — no write */ }
                            else {
                            if (has_fog) {
                                float fog_factor = 0.0f;
                                if (fog_mode == 1) {
                                    float range = fog_far - fog_near;
                                    if (range > 0.0001f) {
                                        fog_factor = (fog_far - fog_dist) / range; /* v14: view distance */
                                        if (fog_factor < 0.0f) fog_factor = 0.0f;
                                        if (fog_factor > 1.0f) fog_factor = 1.0f;
                                    }
                                } else {
                                    fog_factor = expf(-fog_density * fog_dist); /* v14 */
                                    if (fog_factor < 0.0f) fog_factor = 0.0f;
                                    if (fog_factor > 1.0f) fog_factor = 1.0f;
                                }
                                paint_pixel = m3g_blend_fog(paint_pixel, fog_color, fog_factor);
                            }
                            uint32_t bg_pixel = color_buf[idx];
                            uint32_t final_pixel = m3g_blend_pixels(bg_pixel, paint_pixel, paint_alpha, blend_mode);
                            color_buf[idx] = final_pixel;
                            if (do_depth_write) depth_buf[idx] = z;
                            pixels_this_tri++;
                            }
                        }
                    } else {
                        /* No texture - use vertex color directly */
                        paint_pixel = ((uint32_t)(color[3]) <<  24) | (color[0] << 16) | (color[1] << 8) | color[2];
                        /* v30: Node.alphaFactor (fade in/out). */
                        int paint_alpha = (int)(((paint_pixel >> 24) & 0xFF) * alpha_factor + 0.5f);
                        if (paint_alpha < 0) paint_alpha = 0;
                        if (paint_alpha > 255) paint_alpha = 255;
                        if (paint_alpha == 0) { /* fully transparent — no write */ }
                        else {
                        if (has_fog) {
                            float fog_factor = 0.0f;
                            if (fog_mode == 1) {
                                float range = fog_far - fog_near;
                                if (range > 0.0001f) {
                                    fog_factor = (fog_far - fog_dist) / range; /* v14: view distance */
                                    if (fog_factor < 0.0f) fog_factor = 0.0f;
                                    if (fog_factor > 1.0f) fog_factor = 1.0f;
                                }
                            } else {
                                fog_factor = expf(-fog_density * fog_dist); /* v14 */
                                if (fog_factor < 0.0f) fog_factor = 0.0f;
                                if (fog_factor > 1.0f) fog_factor = 1.0f;
                            }
                            paint_pixel = m3g_blend_fog(paint_pixel, fog_color, fog_factor);
                        }
                        
                        uint32_t bg_pixel = color_buf[idx];
                        uint32_t final_pixel = m3g_blend_pixels(bg_pixel, paint_pixel, paint_alpha, blend_mode);
                        /* v14 DIAG: first untextured writes — trace bg-colored
                         * mesh paints (scene 7 / black-cube reports) */
                        {
                            static int wc = 0;
                            wc++;
                            if (render_diag_on() && (wc <= 8 || (wc % 40000) < 3)) {
                                fprintf(stderr, "[M3G-WPIX] #%d paint=%08X alpha=%d bg=%08X final=%08X cblend=%d vcol=%d\n",
                                        wc, paint_pixel, paint_alpha, bg_pixel, final_pixel, blend_mode,
                                        has_vertex_colors ? 1 : 0);
                            }
                        }
                        color_buf[idx] = final_pixel;
                        if (do_depth_write) depth_buf[idx] = z;
                        pixels_this_tri++;
                        }  /* v30: alpha_factor > 0 guard */
                    }      /* no-texture branch */
                }          /* depth-test passed */
            }              /* inside triangle */
            
            /* Incremental edge function step for x+1 */
            w0 += row0_step_x * inv_area;
            w1 += row1_step_x * inv_area;
            w2 += row2_step_x * inv_area;
        }
        
        /* Incremental edge function step for next scanline (y+1) */
        row_w0 += row0_step_y;
        row_w1 += row1_step_y;
        row_w2 += row2_step_y;
    }
    
    g_m3g.triangles_rendered++;
    g_m3g_raster_pixels_written += pixels_this_tri;
    
    /* Diagnostic: log first 3 triangles with screen coords (v34.24: gated) */
    static int diag_count = 0;
    if (render_diag_on() && diag_count < 3) {
        diag_count++;
        fprintf(stderr, "[M3G-RAST] tri#%d: s0=(%.1f,%.1f,z=%.3f) s1=(%.1f,%.1f,z=%.3f) s2=(%.1f,%.1f,z=%.3f) bbox=(%d,%d)-(%d,%d) c0=%s pixels=%d\n",
                diag_count, s0[0], s0[1], s0[2], s1[0], s1[1], s1[2], s2[0], s2[1], s2[2],
                min_x, min_y, max_x, max_y,
                c0 ? "yes" : "(none)", pixels_this_tri);
    }
    if (g_m3g_rast_trace_on && rast_x1 >= rast_x0) {
        extern int g_m3g_rast_acc_x0, g_m3g_rast_acc_x1, g_m3g_rast_acc_y0, g_m3g_rast_acc_y1;
        if (rast_x0 < g_m3g_rast_acc_x0) g_m3g_rast_acc_x0 = rast_x0;
        if (rast_x1 > g_m3g_rast_acc_x1) g_m3g_rast_acc_x1 = rast_x1;
        if (rast_y0 < g_m3g_rast_acc_y0) g_m3g_rast_acc_y0 = rast_y0;
        if (rast_y1 > g_m3g_rast_acc_y1) g_m3g_rast_acc_y1 = rast_y1;
    }
}

/* v34.42: classic entry — build the per-mesh context and delegate. Kept for
 * the NEON self-test (direct calls with locally-built appearances), the
 * mascot3d immediate path and any future caller that renders one-off
 * triangles. m3g_render_single_mesh builds the context ONCE per mesh and
 * calls m3g_rasterize_triangle_ctx directly. */
M3G_HOT void m3g_rasterize_triangle(const float* v0, const float* v1, const float* v2,
                                    const float* t0, const float* t1, const float* t2,
                                    const float* t1_0, const float* t1_1, const float* t1_2,
                                    const uint8_t* c0, const uint8_t* c1, const uint8_t* c2,
                                    M3GAppearance* appearance,
                                    const float* fog_d0, const float* fog_d1, const float* fog_d2) {
    M3GRastCtx ctx;
    m3g_raster_begin_mesh(&ctx, appearance);
    m3g_rasterize_triangle_ctx(&ctx, v0, v1, v2, t0, t1, t2, t1_0, t1_1, t1_2,
                               c0, c1, c2, fog_d0, fog_d1, fog_d2);
}

/* v34.42 PERF: batch vertex transform (see render.h). NEON builds the whole
 * 4-component output of one vertex per vmulq_n/vaddq chain with the exact
 * scalar rounding order ((((m0*x)+m4*y)+m8*z)+m12*1.0) — vmulq_n and vaddq
 * each round once per lane, NO fused multiply-add, so every intermediate
 * is bit-identical to m3g_transform_point (the self-test's transform
 * section A/B-compares the two on ARM). w is the implicit 1.0: x*1.0 == x
 * exactly in IEEE-754, so the last term is a plain add of column 3. */
void m3g_transform_vertices_batch(float* __restrict__ out,
                                  const float* __restrict__ verts,
                                  int count, const M3GTransform* t) {
    if (!out || !verts || count <= 0 || !t) return;
    const float* m = t->m;
#if RENDER_HAVE_NEON
    if (g_m3g_neon_force_scalar < 0)
        g_m3g_neon_force_scalar = getenv("NOJME_NEON_SCALAR") ? 1 : 0;
    if (!g_m3g_neon_force_scalar) {
        /* Column-major layout: m[col*4+row]. Column j as a q-register:
         * vmulq_n(col, comp) contributes comp*m[4j+row] to output row. */
        const float32x4_t c0 = vld1q_f32(&m[0]);   /* (m0, m1, m2, m3)   */
        const float32x4_t c1 = vld1q_f32(&m[4]);   /* (m4, m5, m6, m7)   */
        const float32x4_t c2 = vld1q_f32(&m[8]);
        const float32x4_t c3 = vld1q_f32(&m[12]);
        for (int v = 0; v < count; v++, verts += 3, out += 4) {
            float32x4_t acc = vmulq_n_f32(c0, verts[0]);          /* m_col0 * x  */
            acc = vaddq_f32(acc, vmulq_n_f32(c1, verts[1]));     /* + m_col1 * y */
            acc = vaddq_f32(acc, vmulq_n_f32(c2, verts[2]));     /* + m_col2 * z */
            acc = vaddq_f32(acc, c3);                            /* + m_col3 * 1.0 */
            vst1q_f32(out, acc);
        }
        return;
    }
#endif
    for (int v = 0; v < count; v++) {
        const float x = verts[v * 3 + 0], y = verts[v * 3 + 1], z = verts[v * 3 + 2];
        float* o = &out[v * 4];
        o[0] = m[0]*x + m[4]*y + m[8]*z + m[12];
        o[1] = m[1]*x + m[5]*y + m[9]*z + m[13];
        o[2] = m[2]*x + m[6]*y + m[10]*z + m[14];
        o[3] = m[3]*x + m[7]*y + m[11]*z + m[15];
    }
}

/* ============================================================================
 * v34.24 NEON SELF-TEST (env NOJME_NEON_SELFTEST=1)
 * ============================================================================
 * A/B verification of every vectorized path against the scalar reference:
 *   1) neon_fill_u32 / neon_fill_f32 vs scalar loops
 *   2) neon_blend_row_const vs the fill_rect scalar formula
 *   3) neon_src_over_span vs the drawImage scalar formula
 *   4) neon_copy_or32 vs the drawRGB opaque formula
 *   5) m3g_rasterize_triangle with the NEON span path forced OFF vs ON,
 *      across all supported compositing modes with textures + vertex
 *      colors + depth test/write, compared byte-for-byte.
 * On non-NEON hosts the rasterizer A/B degenerates to scalar-vs-scalar
 * (proves the harness); on ARM hardware/qemu-user it validates the real
 * NEON code. Output always goes to stderr via ALWAYS_LOG — an explicitly
 * requested test must never be silenced by the log gate.
 * Returns the number of FAILED checks (0 = all pass).
 * ============================================================================ */
int render_neon_selftest(void) {
    int fails = 0;
    ALWAYS_LOG("[NEON-TEST] renderer self-test (NEON compiled: %s)\n",
              RENDER_HAVE_NEON ? "yes" : "no");
    (void)render_diag_on; /* silence unused warning when NEON is off */
#if RENDER_HAVE_NEON
    /* v34.41: FASTDIV теперь по умолчанию ON (≈0.5-1 ULP на UV/цвете).
     * Самотест сравнивает NEON против скаляра ПОБИТОВО — на время A/B
     * форсируем строгое IEEE-деление, затем честно печатаем runtime-состояние
     * флага, чтобы пользователь знал, что проверено, а что работает с
     * приближением. */
    {
        int save_fd = g_m3g_neon_fastdiv;
        g_m3g_neon_fastdiv = 0;
        const char* fd = getenv("NOJME_NEON_FASTDIV");
        int runtime_fd = (fd && fd[0] == '0' && fd[1] == '\0') ? 0 : 1;
        ALWAYS_LOG("[NEON-TEST] fastdiv: A/B runs forced EXACT; runtime default %s\n",
                  runtime_fd ? "FAST (ON)" : "IEEE (OFF)");
        g_m3g_neon_fastdiv = save_fd;
    }
#endif

    /* deterministic LCG */
    unsigned long lcg = 0x12345678UL;
    #define LCG_NEXT() (lcg = lcg * 6364136223846793005UL + 1442695040888963407UL)

    const int N = 4096;
    uint32_t* a = (uint32_t*)malloc(N * sizeof(uint32_t));
    uint32_t* b = (uint32_t*)malloc(N * sizeof(uint32_t));
    float* fa = (float*)malloc(N * sizeof(float));
    float* fb = (float*)malloc(N * sizeof(float));

    /* --- 0: v34.42 batch vertex transform (NEON vs m3g_transform_point) --- */
    {
        const int NV = 1000;
        float* vin  = (float*)malloc(NV * 3 * sizeof(float));
        float* vref = (float*)malloc(NV * 4 * sizeof(float));
        float* vgot = (float*)malloc(NV * 4 * sizeof(float));
        if (vin && vref && vgot) {
            M3GTransform t;
            for (int i = 0; i < 16; i++) {
                uint32_t bits = (uint32_t)(LCG_NEXT());
                /* mix magnitudes: subnormals..1e9, incl. negatives */
                t.m[i] = (float)((int)(bits % 2000001) - 1000000) *
                         ((bits & 0x80000000u) ? 0.0001731f : 88.7f);
            }
            for (int i = 0; i < NV * 3; i++) {
                uint32_t bits = (uint32_t)(LCG_NEXT());
                vin[i] = (float)((int)(bits % 140001) - 70000) * 0.0137f;
            }
            /* reference: exact m3g_transform_point semantics (w=1; the
             * static prototype lives in mobile3d.c, so replicate its
             * expression order verbatim — this is also the scalar tail
             * of m3g_transform_vertices_batch) */
            for (int v = 0; v < NV; v++) {
                const float x = vin[v*3], y = vin[v*3+1], z = vin[v*3+2];
                float* o = &vref[v*4];
                o[0] = t.m[0]*x + t.m[4]*y + t.m[8]*z + t.m[12];
                o[1] = t.m[1]*x + t.m[5]*y + t.m[9]*z + t.m[13];
                o[2] = t.m[2]*x + t.m[6]*y + t.m[10]*z + t.m[14];
                o[3] = t.m[3]*x + t.m[7]*y + t.m[11]*z + t.m[15];
            }
            m3g_transform_vertices_batch(vgot, vin, NV, &t);
            int bad = 0;
            for (int i = 0; i < NV * 4; i++) {
                if (memcmp(&vref[i], &vgot[i], sizeof(float)) != 0) { bad++; }
            }
#if RENDER_HAVE_NEON
            ALWAYS_LOG("[NEON-TEST] batch_xform     %s (mismatches=%d)\n", bad ? "FAIL" : "PASS", bad);
#else
            /* scalar-vs-scalar harness check (same loop) */
            ALWAYS_LOG("[NEON-TEST] batch_xform     %s (scalar harness, mismatches=%d)\n", bad ? "FAIL" : "PASS", bad);
#endif
            fails += bad ? 1 : 0;
        }
        free(vin); free(vref); free(vgot);
    }
    if (!a || !b || !fa || !fb) { ALWAYS_LOG("[NEON-TEST] alloc failed\n"); return 1; }

    /* --- 1: fills --- */
    {
        uint32_t v = (uint32_t)(LCG_NEXT());
        for (int i = 0; i < N; i++) a[i] = v;
#if RENDER_HAVE_SIMD
        memset(b, 0xAA, N * 4);
        span_fill_u32(b, N, v);
        int bad = 0;
        for (int i = 0; i < N; i++) if (a[i] != b[i]) bad++;
        ALWAYS_LOG("[NEON-TEST] fill_u32       %s (mismatches=%d)\n", bad ? "FAIL" : "PASS", bad);
        fails += bad ? 1 : 0;
#endif
        float fv = (float)(LCG_NEXT() % 1000) / 7.0f;
        for (int i = 0; i < N; i++) fa[i] = fv;
#if RENDER_HAVE_SIMD
        memset(fb, 0, N * 4);
        span_fill_f32(fb, N, fv);
        int badf = 0;
        for (int i = 0; i < N; i++) if (fa[i] != fb[i]) badf++;
        ALWAYS_LOG("[NEON-TEST] fill_f32       %s (mismatches=%d)\n", badf ? "FAIL" : "PASS", badf);
        fails += badf ? 1 : 0;
#endif
    }

    /* --- 2: fill_rect constant-color alpha blend --- */
    {
        lcg = 0x12345678UL;   /* deterministic reset: expected + restore share it */
        uint32_t color = 0x80A0C0E0u;
        const int a8 = (color >> 24) & 0xFF, ia8 = 255 - a8;
        const int sr = (color >> 16) & 0xFF, sg = (color >> 8) & 0xFF, sb = color & 0xFF;
        for (int i = 0; i < N; i++) {
            uint32_t d = (uint32_t)(LCG_NEXT());
            uint32_t r = (sr * a8 + ((d >> 16) & 0xFF) * ia8 + 128) >> 8;
            uint32_t g = (sg * a8 + ((d >> 8) & 0xFF) * ia8 + 128) >> 8;
            uint32_t bch = (sb * a8 + (d & 0xFF) * ia8 + 128) >> 8;
            a[i] = ((uint32_t)a8 << 24) | (r << 16) | (g << 8) | bch;
            b[i] = d;
        }
#if RENDER_HAVE_SIMD
        /* restore the same dst values into b, then blend */
        lcg = 0x12345678UL;
        for (int i = 0; i < N; i++) { (void)LCG_NEXT(); b[i] = (uint32_t)lcg; }
        span_blend_row_const(b, N, color);
        int bad = 0;
        for (int i = 0; i < N; i++) if (a[i] != b[i]) bad++;
        ALWAYS_LOG("[NEON-TEST] blend_row_const %s (mismatches=%d)\n", bad ? "FAIL" : "PASS", bad);
        fails += bad ? 1 : 0;
#endif
    }

    /* --- 3: src-over span --- */
    {
        uint32_t* src = (uint32_t*)malloc(N * sizeof(uint32_t));
        for (int i = 0; i < N; i++) {
            uint32_t alpha = (uint32_t)(LCG_NEXT() % 300); /* 0..299: hits 0, 255, partial */
            if (alpha > 255) alpha = 255;
            src[i] = (alpha << 24) | ((uint32_t)(LCG_NEXT() & 0xFFFFFF));
        }
        lcg = 0x87654321UL;
        for (int i = 0; i < N; i++) {
            uint32_t dc = (uint32_t)(LCG_NEXT());
            b[i] = dc;
            uint32_t sc = src[i];
            uint32_t sa = (sc >> 24) & 0xFF;
            if (sa == 255) a[i] = sc;
            else if (sa > 0) {
                uint32_t ia = 255 - sa;
                uint32_t r = (((sc >> 16) & 0xFF) * sa + ((dc >> 16) & 0xFF) * ia + 128) >> 8;
                uint32_t g = (((sc >> 8) & 0xFF) * sa + ((dc >> 8) & 0xFF) * ia + 128) >> 8;
                uint32_t bc = ((sc & 0xFF) * sa + (dc & 0xFF) * ia + 128) >> 8;
                a[i] = 0xFF000000u | (r << 16) | (g << 8) | bc;
            } else {
                a[i] = dc;
            }
        }
#if RENDER_HAVE_SIMD
        lcg = 0x87654321UL;
        for (int i = 0; i < N; i++) { (void)LCG_NEXT(); b[i] = (uint32_t)lcg; }
        span_src_over_span(b, src, N);
        int bad = 0;
        for (int i = 0; i < N; i++) if (a[i] != b[i]) bad++;
        ALWAYS_LOG("[NEON-TEST] src_over_span  %s (mismatches=%d)\n", bad ? "FAIL" : "PASS", bad);
        fails += bad ? 1 : 0;
#else
        (void)src;
#endif
    }

    /* --- 4: copy-or32 --- */
    {
        for (int i = 0; i < N; i++) {
            uint32_t s = (uint32_t)(LCG_NEXT());
            a[i] = 0xFF000000u | (s & 0x00FFFFFFu);
        }
#if RENDER_HAVE_SIMD
        memset(b, 0, N * 4);
        span_copy_or32(b, a, N);
        int bad = 0;
        for (int i = 0; i < N; i++) if (b[i] != a[i]) bad++;
        ALWAYS_LOG("[NEON-TEST] copy_or32      %s (mismatches=%d)\n", bad ? "FAIL" : "PASS", bad);
        fails += bad ? 1 : 0;
#endif
    }

    /* --- 4b: reversed copy (drawRegion MIRROR/ROT180 opaque spans) --- */
    {
        for (int i = 0; i < N; i++) a[i] = (uint32_t)(LCG_NEXT()) | 0xFF000000u;
#if RENDER_HAVE_SIMD
        memset(b, 0, N * 4);
        span_copy_rev32(b, a, N);
        int bad = 0;
        for (int i = 0; i < N; i++) if (b[i] != a[N - 1 - i]) bad++;
        ALWAYS_LOG("[NEON-TEST] copy_rev32     %s (mismatches=%d)\n", bad ? "FAIL" : "PASS", bad);
        fails += bad ? 1 : 0;
#endif
    }

    /* --- 5: rasterizer A/B through the public entry point --- */
    {
        const int W = 64, H = 32, PIX = W * H;
        uint32_t* cb_scalar = (uint32_t*)malloc(PIX * 4);
        uint32_t* cb_neon = (uint32_t*)malloc(PIX * 4);
        float* db_scalar = (float*)malloc(PIX * 4);
        float* db_neon = (float*)malloc(PIX * 4);

        /* 8x8 texture with varied alpha + colors */
        uint32_t texel[64];
        for (int i = 0; i < 64; i++) {
            unsigned alpha = (unsigned)(i * 37 % 300);
            if (alpha > 255) alpha = 255;
            unsigned r = (unsigned)(i * 11 % 256);
            unsigned g = (unsigned)(i * 5 % 256);
            unsigned bl = (unsigned)(i * 3 % 256);
            texel[i] = (alpha << 24) | (r << 16) | (g << 8) | bl;
        }
        M3GTexture2D tex;
        memset(&tex, 0, sizeof(tex));
        tex.width = 8; tex.height = 8; tex.pixels = texel;
        tex.filter_level = 208; /* nearest */

        /* triangle: clip-space vertices, varied w for perspective */
        const float v0[4] = { -0.8f, -0.7f, 0.2f, 1.0f };
        const float v1[4] = {  0.8f, -0.6f, 0.6f, 1.4f };
        const float v2[4] = { -0.2f,  0.8f, 0.4f, 0.85f };
        const float t0[2] = { -0.3f, 1.7f };
        const float t1[2] = { 1.2f, -0.4f };
        const float t2[2] = { 0.5f, 0.9f };
        const uint8_t c0[4] = { 255, 10, 60, 255 };
        const uint8_t c1[4] = { 30, 240, 90, 128 };
        const uint8_t c2[4] = { 200, 200, 0, 40 };

        struct { int tex_blend; int blend_mode; int vcol; int filter_ov; int tex_filter; const char* name; } cfg[] = {
            { 67, 67, 0, 0, 208, "REPLACE tex / REPLACE cmp" },
            { 67, 64, 0, 0, 208, "REPLACE tex / ALPHA cmp" },
            { 67, 65, 0, 0, 208, "REPLACE tex / ALPHA_ADD cmp" },
            { 68, 67, 1, 0, 208, "MODULATE tex+vcol / REPLACE cmp" },
            { 68, 68, 1, 0, 208, "MODULATE tex+vcol / MODULATE cmp" },
            { 68, 64, 1, 0, 208, "MODULATE tex+vcol / ALPHA cmp" },
            /* v34.79 (j2me_texture_smoothing=smooth): SIMD span bilinear vs
             * the scalar integer sampler m3g_sample_texture_smooth — the
             * core bit-exactness contract of the SMOOTH mode. */
            { 67, 67, 0, 2, 208, "SMOOTH REPLACE / REPLACE cmp" },
            { 67, 64, 0, 2, 208, "SMOOTH REPLACE / ALPHA cmp" },
            { 68, 67, 1, 2, 208, "SMOOTH MODULATE / REPLACE cmp" },
            { 68, 64, 1, 2, 208, "SMOOTH MODULATE / ALPHA cmp" },
            /* nearest override on a LINEAR-requested texture: span becomes
             * eligible and both paths sample nearest. */
            { 67, 64, 0, 1, 209, "NEAREST-override LINEAR tex" },
            /* AUTO + LINEAR texture: span must stay REJECTED (scalar legacy
             * float bilinear on both passes) — guards the eligibility
             * against drift that would show span-nearest vs scalar-float. */
            { 67, 64, 0, 0, 209, "AUTO + LINEAR tex (span off)" },
        };

        M3GContext saved = g_m3g;
        int save_force = g_m3g_neon_force_scalar;
        if (g_m3g_neon_force_scalar < 0) g_m3g_neon_force_scalar = 0;
        int save_fov = g_m3g_texture_filter_override;
#if RENDER_HAVE_NEON
        /* v34.41: A/B должен быть побитовым — временно выключаем fastdiv
         * (по умолчанию ON с этой версии; ≈0.5-1 ULP на UV/цвете). */
        int save_fastdiv = g_m3g_neon_fastdiv;
        g_m3g_neon_fastdiv = 0;
#endif

        for (unsigned ci = 0; ci < sizeof(cfg) / sizeof(cfg[0]); ci++) {
            M3GAppearance app;
            memset(&app, 0, sizeof(app));
            app.texture = &tex;
            app.blend_mode = cfg[ci].blend_mode;
            app.texture_blend = cfg[ci].tex_blend;
            app.alpha_threshold = -1;
            app.alpha_factor = 1.0f;
            tex.filter_level = cfg[ci].tex_filter;
            g_m3g_texture_filter_override = cfg[ci].filter_ov;

            int badc = 0, badd = 0;
            for (int pass = 0; pass < 2; pass++) {
                memset(pass ? cb_neon : cb_scalar, 0x33, PIX * 4);
                for (int i = 0; i < PIX; i++) (pass ? db_neon : db_scalar)[i] = 0.9f;

                memset(&g_m3g, 0, sizeof(g_m3g));
                g_m3g.buffer_width = W; g_m3g.buffer_height = H;
                g_m3g.viewport_width = W; g_m3g.viewport_height = H;
                g_m3g.color_buffer = pass ? cb_neon : cb_scalar;
                g_m3g.depth_buffer = pass ? db_neon : db_scalar;
                g_m3g.depth_test_enabled = 1;
                g_m3g.depth_write_enabled = 1;
                g_m3g.culling_enabled = 0;

                g_m3g_neon_force_scalar = (pass == 0); /* pass 0 scalar, pass 1 NEON */

                m3g_rasterize_triangle(v0, v1, v2,
                        t0, t1, t2, NULL, NULL, NULL,
                        cfg[ci].vcol ? c0 : NULL,
                        cfg[ci].vcol ? c1 : NULL,
                        cfg[ci].vcol ? c2 : NULL,
                        &app, NULL, NULL, NULL);
            }
            for (int i = 0; i < PIX; i++) if (cb_scalar[i] != cb_neon[i]) badc++;
            for (int i = 0; i < PIX; i++) if (db_scalar[i] != db_neon[i]) badd++;
            ALWAYS_LOG("[NEON-TEST] raster %-28s %s (color diff=%d, depth diff=%d)\n",
                      cfg[ci].name, (badc || badd) ? "FAIL" : "PASS", badc, badd);
            fails += (badc || badd) ? 1 : 0;
        }

        /* v34.79: SMOOTH invariant — a CONSTANT texture must sample to
         * exactly itself under bilinear (both weight pairs sum to 128), so
         * a texture-REPLACE + compositing-REPLACE render of a solid
         * texture must paint that exact ARGB in every covered pixel. This
         * catches channel-swap/repack and weight-duplication bugs that
         * would sit on BOTH sides of the A/B above (scalar and SIMD making
         * the same mistake) — the A/B proves they agree, this proves they
         * are also RIGHT. Runs through the SIMD span (force_scalar=0). */
        {
            uint32_t ctexel[64];
            const uint32_t C = 0xFFC81F73u; /* alpha 255: REPLACE is exact */
            for (int i = 0; i < 64; i++) ctexel[i] = C;
            M3GTexture2D ctex;
            memset(&ctex, 0, sizeof(ctex));
            ctex.width = 8; ctex.height = 8; ctex.pixels = ctexel;
            ctex.filter_level = 208;
            M3GAppearance capp;
            memset(&capp, 0, sizeof(capp));
            capp.texture = &ctex;
            capp.blend_mode = 67;      /* compositing REPLACE */
            capp.texture_blend = 67;  /* texture REPLACE */
            capp.alpha_threshold = -1;
            capp.alpha_factor = 1.0f;

            g_m3g_texture_filter_override = 2;
            g_m3g_neon_force_scalar = 0;
            memset(cb_neon, 0x33, PIX * 4);
            for (int i = 0; i < PIX; i++) db_neon[i] = 0.9f;
            memset(&g_m3g, 0, sizeof(g_m3g));
            g_m3g.buffer_width = W; g_m3g.buffer_height = H;
            g_m3g.viewport_width = W; g_m3g.viewport_height = H;
            g_m3g.color_buffer = cb_neon;
            g_m3g.depth_buffer = db_neon;
            g_m3g.depth_test_enabled = 1;
            g_m3g.depth_write_enabled = 1;
            g_m3g.culling_enabled = 0;
            m3g_rasterize_triangle(v0, v1, v2, t0, t1, t2, NULL, NULL, NULL,
                                   NULL, NULL, NULL, &capp, NULL, NULL, NULL);
            int bad_const = 0, covered = 0;
            for (int i = 0; i < PIX; i++) {
                if (cb_neon[i] == 0x33333333u) continue; /* untouched bg */
                covered++;
                if (cb_neon[i] != C) bad_const++;
            }
            ALWAYS_LOG("[NEON-TEST] smooth const-texel       %s (covered=%d, mismatches=%d)\n",
                      (bad_const || covered == 0) ? "FAIL" : "PASS", covered, bad_const);
            fails += (bad_const || covered == 0) ? 1 : 0;
        }

        g_m3g = saved;
        g_m3g_neon_force_scalar = save_force;
        g_m3g_texture_filter_override = save_fov;
#if RENDER_HAVE_NEON
        g_m3g_neon_fastdiv = save_fastdiv;
#endif
        free(cb_scalar); free(cb_neon); free(db_scalar); free(db_neon);
    }

    /* --- 6: v34.28 RGB565 frame conversion (NEON vs scalar formula) --- */
    {
        const int W6 = 61, H6 = 17; /* odd sizes: exercise 8/4 blocks + tail */
        const int N6 = W6 * H6;
        uint32_t* src6 = (uint32_t*)malloc(N6 * 4);
        uint16_t* got6 = (uint16_t*)malloc(N6 * 2);
        uint16_t* ref6 = (uint16_t*)malloc(N6 * 2);
        if (src6 && got6 && ref6) {
            /* patterned + LCG pixels: all 8-bit channel ramps and edge values */
            for (int i = 0; i < N6; i++) {
                uint32_t p = (i < 256)
                    ? (uint32_t)i * 0x01010101u     /* 0..255 uniform channels */
                    : (uint32_t)(LCG_NEXT());
                src6[i] = p;
                uint32_t r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
                ref6[i] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
            }
            memset(got6, 0x55, N6 * 2);
            nojme_convert_xrgb8888_to_rgb565(src6, got6, W6, H6);
            int bad6 = 0;
            for (int i = 0; i < N6; i++) if (got6[i] != ref6[i]) bad6++;
            ALWAYS_LOG("[NEON-TEST] rgb565 convert     %s (mismatches=%d)\n",
                      bad6 ? "FAIL" : "PASS", bad6);
            fails += bad6 ? 1 : 0;
        } else {
            fails++;
        }
        free(src6); free(got6); free(ref6);
    }

    /* --- 7..10: v34.28 optimized 2D algorithms vs their NOJME_2D_SCALAR
     * reference implementations, exercised through the PUBLIC entry points
     * (fill_triangle span intervals, drawRegion transforms 1/2/3, batched
     * glyphs, hoisted getRGB) on a 320x240 canvas with random clips.
     * Per-case random parameters are drawn ONCE and replayed for both
     * passes so A (reference) and B (fast) see identical inputs. --- */
    {
        static const char* st_strs[] = {
            "Hello MIDP 2.0!",
            "0123456789 ABC xyz",
            "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82", /* Cyrillic UTF-8 */
            "drawRegion fillTriangle getRGB",
        };
        const int W7 = 320, H7 = 240, PIX7 = W7 * H7;
        uint32_t* fb_a = (uint32_t*)malloc(PIX7 * 4);
        uint32_t* fb_b = (uint32_t*)malloc(PIX7 * 4);
        MidpImage* img7 = (MidpImage*)malloc(sizeof(MidpImage));
        jint* rgb_ref = (jint*)malloc(128 * 32 * sizeof(jint));
        jint* rgb_got = (jint*)malloc(128 * 32 * sizeof(jint));
        if (rgb_ref) memset(rgb_ref, 0, 128 * 32 * sizeof(jint));
        if (rgb_got) memset(rgb_got, 0, 128 * 32 * sizeof(jint));
        int have7 = fb_a && fb_b && img7 && rgb_ref && rgb_got;
        if (have7) {
            img7->width = 96; img7->height = 64;
            img7->mutable = false; img7->alpha = true;
            img7->alpha_scan_valid = JNI_FALSE; /* v34.51 fields: unknown →
                draw path scans lazily, exactly like a real decoded image */
            img7->alpha_all_opaque = JNI_FALSE;
            img7->pixels = (uint32_t*)malloc(96 * 64 * 4);
            have7 = img7->pixels != NULL;
        }

        if (have7) {
            MidpGraphics ga, gb;
            const int save_2d = g_2d_force_scalar;
            int bad_ftri = 0, bad_xf = 0, bad_glyph = 0, bad_getrgb = 0;
            int n_ftri = 0, n_xf = 0, n_glyph = 0, n_getrgb = 0;

            /* deterministic image: alpha mix of 0 / 255 / partial */
            for (int i = 0; i < 96 * 64; i++) {
                unsigned long rv = LCG_NEXT();
                uint32_t al = (rv % 5 == 0) ? 0 : (rv % 5 == 1 ? 255u : (uint32_t)(rv % 256));
                img7->pixels[i] = (al << 24) | ((uint32_t)(LCG_NEXT() & 0xFFFFFF));
            }

            for (int rep = 0; rep < 400; rep++) {
                /* shared gfx state + per-case params, drawn ONCE */
                const uint32_t base_fill = (uint32_t)(LCG_NEXT());
                const int tx = (int)(LCG_NEXT() % 64) - 32;
                const int ty = (int)(LCG_NEXT() % 64) - 32;
                const int cx = (int)(LCG_NEXT() % (W7 + 1)) - 40;
                const int cy = (int)(LCG_NEXT() % (H7 + 1)) - 40;
                const int cw = (int)(LCG_NEXT() % 200) + 40;
                const int chh = (int)(LCG_NEXT() % 200) + 40;
                const uint32_t col = (uint32_t)(LCG_NEXT());
                const int alpha = (int)(LCG_NEXT() % 256);
                const int kind = rep % 4;
                int p[7];
                for (int k = 0; k < 7; k++) p[k] = (int)(LCG_NEXT() % 500) - 80;

                for (int i = 0; i < PIX7; i++) fb_a[i] = base_fill ^ (uint32_t)(i * 2654435761u);
                memcpy(fb_b, fb_a, PIX7 * 4);

                for (int pass = 0; pass < 2; pass++) {
                    MidpGraphics* g = pass ? &gb : &ga;
                    midp_graphics_init(g, pass ? fb_b : fb_a, W7, H7);
                    midp_graphics_set_clip(g, cx, cy, cw, chh);
                    g->translate_x = tx; g->translate_y = ty;
                    midp_graphics_set_color(g, (int)(col & 0xFFFFFF), alpha);
                    g_2d_force_scalar = (pass == 0) ? 1 : 0; /* A: reference, B: fast */

                    switch (kind) {
                        case 0: /* fill_triangle span intervals */
                            midp_graphics_fill_triangle(g, p[0], p[1], p[2], p[3], p[4], p[5]);
                            break;
                        case 1: /* drawRegion transforms 1/2/3 */
                            midp_graphics_draw_region(g, img7, 0, 0, 96, 64,
                                                      1 + (rep % 3), p[0], p[1], 4 /*LEFT|TOP*/);
                            break;
                        case 2: /* glyphs (ASCII + Cyrillic), all anchors */
                            midp_graphics_draw_string(g, st_strs[p[6] & 3], p[0], p[1],
                                                      (int)(p[2] & 0x7F));
                            break;
                        case 3: /* getRGB hoisted bounds */
                            if (pass == 0) {
                                midp_graphics_get_rgb(g, rgb_ref, 3, 128, p[0], p[1], 120, 28);
                            } else {
                                midp_graphics_get_rgb(g, rgb_got, 3, 128, p[0], p[1], 120, 28);
                            }
                            break;
                    }
                }

                int diff = 0;
                for (int i = 0; i < PIX7; i++) if (fb_a[i] != fb_b[i]) { diff = 1; break; }
                if (kind == 0) { bad_ftri += diff; n_ftri++; }
                else if (kind == 1) { bad_xf += diff; n_xf++; }
                else if (kind == 2) { bad_glyph += diff; n_glyph++; }
                else {
                    bad_getrgb += (memcmp(rgb_ref, rgb_got, 128 * 32 * sizeof(jint)) != 0);
                    n_getrgb++;
                }
            }
            g_2d_force_scalar = save_2d;

            ALWAYS_LOG("[NEON-TEST] fillTriangle span %s (%d/%d cases diff)\n",
                      bad_ftri ? "FAIL" : "PASS", bad_ftri, n_ftri);
            ALWAYS_LOG("[NEON-TEST] drawRegion xf1/2/3 %s (%d/%d cases diff)\n",
                      bad_xf ? "FAIL" : "PASS", bad_xf, n_xf);
            ALWAYS_LOG("[NEON-TEST] glyph rows       %s (%d/%d cases diff)\n",
                      bad_glyph ? "FAIL" : "PASS", bad_glyph, n_glyph);
            ALWAYS_LOG("[NEON-TEST] getRGB hoisted   %s (%d/%d cases diff)\n",
                      bad_getrgb ? "FAIL" : "PASS", bad_getrgb, n_getrgb);
            fails += (bad_ftri || bad_xf || bad_glyph || bad_getrgb) ? 1 : 0;

            free(img7->pixels);
        } else {
            fails++;
        }
        free(fb_a); free(fb_b); free(img7);
        free(rgb_ref); free(rgb_got);
    }

    /* --- 11: v34.51 PINK-FIX — exact per-image opacity vs the retired
     * corner-sampling heuristic. A sprite whose four REGION CORNERS are
     * opaque but whose interior holds transparent (alpha==0) pink
     * color-key pixels used to take the memcpy path and leak the raw
     * pink to the framebuffer. The exact scan must (a) route it to the
     * src-over path — dst under alpha==0 pixels UNTOUCHED, (b) still
     * memcpy a fully-opaque image — dst == src everywhere. Runs on every
     * arch (no SIMD involved). --- */
    {
        enum { PW = 48, PH = 32, PC = PW * PH };
        uint32_t* sfb = (uint32_t*)malloc(PC * 4);
        uint32_t* pfb = (uint32_t*)malloc(PC * 4);
        uint32_t* spix = (uint32_t*)malloc(PC * 4);
        MidpImage* pimg = (MidpImage*)malloc(sizeof(MidpImage));
        const int have11 = sfb && pfb && spix && pimg;
        if (have11) {
            pimg->width = PW; pimg->height = PH;
            pimg->mutable = JNI_FALSE; pimg->alpha = JNI_TRUE;
            pimg->alpha_scan_valid = JNI_FALSE; /* lazily scanned */
            pimg->alpha_all_opaque = JNI_FALSE;
            pimg->pixels = spix;

            /* sprite: opaque 2px border, alpha==0 PINK interior key,
 * a few semi-transparent pixels mixed in */
            for (int yy = 0; yy < PH; yy++) {
                for (int xx = 0; xx < PW; xx++) {
                    const int border = (xx < 2 || yy < 2 || xx >= PW - 2 || yy >= PH - 2);
                    uint32_t px;
                    if (border) {
                        px = 0xFF2040A0u;
                    } else if ((xx ^ yy) & 1) {
                        px = 0x00FF00FFu; /* transparent PINK key */
                    } else {
                        px = 0x8030D070u; /* semi-transparent */
                    }
                    spix[yy * PW + xx] = px;
                }
            }

            int bad_pink = 0;
            /* (a) drawImage with exact scan */
            {
                for (int i = 0; i < PC; i++) {
                    sfb[i] = 0xFF3366CCu; /* visible dst background */
                    pfb[i] = 0xFF3366CCu;
                }
                MidpGraphics g11;
                midp_graphics_init(&g11, sfb, PW, PH);
                midp_graphics_draw_image(&g11, pimg, 0, 0, 20 /*TOP|LEFT*/);
                for (int yy = 0; yy < PH && !bad_pink; yy++) {
                    for (int xx = 0; xx < PW; xx++) {
                        const uint32_t s = spix[yy * PW + xx];
                        const uint32_t got = sfb[yy * PW + xx];
                        const uint8_t sa = (uint8_t)(s >> 24);
                        if (sa == 0) {
                            if (got != 0xFF3366CCu) { bad_pink = 1; break; } /* dst leaked */
                        } else if (sa == 255) {
                            if (got != s) { bad_pink = 1; break; }
                        }
                        /* semi pixels: just require dst != raw pink source */
                    }
                }
                if (sfb[0] != 0xFF2040A0u) bad_pink |= 1; /* border must copy */
            }
            /* (b) drawRegion TRANS_NONE — the exact site of the retired
 * two-corner (top-left first!) sample */
            {
                for (int i = 0; i < PC; i++) {
                    sfb[i] = 0xFF3366CCu;
                }
                MidpGraphics g11;
                midp_graphics_init(&g11, sfb, PW, PH);
                midp_graphics_draw_region(&g11, pimg, 0, 0, PW, PH,
                                         0 /*TRANS_NONE*/, 0, 0, 20);
                /* check ONLY the alpha==0 pink-key pixels: (xx^yy)&1 == 1
                 * and not on the opaque border */
                for (int yy = 2; yy < PH - 2 && !bad_pink; yy++) {
                    for (int xx = 2; xx < PW - 2; xx++) {
                        if (((xx ^ yy) & 1) == 1 &&
                            sfb[yy * PW + xx] != 0xFF3366CCu) { bad_pink = 1; break; }
                    }
                }
            }
            /* (c) fully-opaque image still memcpys */
            {
                for (int i = 0; i < PC; i++) {
                    spix[i] = 0xFF123456u + (uint32_t)i;
                    sfb[i] = 0;
                }
                pimg->alpha = JNI_TRUE;
                pimg->alpha_scan_valid = JNI_FALSE; /* rescan */
                MidpGraphics g11;
                midp_graphics_init(&g11, sfb, PW, PH);
                midp_graphics_draw_image(&g11, pimg, 0, 0, 20);
                for (int i = 0; i < PC; i++) {
                    if (sfb[i] != spix[i]) { bad_pink = 1; break; }
                }
                /* midp_image_all_opaque must now be cached TRUE */
                if (!midp_image_all_opaque(pimg)) bad_pink = 1;
            }
            ALWAYS_LOG("[NEON-TEST] pink-fix opacity  %s (leaks=%d)\n",
                      bad_pink ? "FAIL" : "PASS", bad_pink);
            fails += bad_pink ? 1 : 0;
        } else {
            fails++;
        }
        free(sfb); free(pfb); free(spix); free(pimg);
    }

    ALWAYS_LOG("[NEON-TEST] %s (%d failure%s)\n",
              fails ? "FAILED" : "ALL PASS", fails, fails == 1 ? "" : "s");
    free(a); free(b); free(fa); free(fb);
    return fails;
}

/* Honest scalar reference for the bench: noinline + runtime dimensions,
 * mirroring how the OLD code executed inside retro_run (runtime w*h ->
 * not auto-vectorized under -O2's very-cheap model). An inline loop with
 * compile-time constant bounds would be unfairly ~2x faster on x86. */
__attribute__((noinline))
static void bench_rgb565_scalar_ref(const uint32_t* restrict src, uint16_t* restrict dst,
                                    int width, int height) {
    const int total = width * height;
    for (int i = 0; i < total; i++) {
        uint32_t pixel = src[i];
        uint8_t r = (pixel >> 16) & 0xFF;
        uint8_t g = (pixel >> 8) & 0xFF;
        uint8_t b = pixel & 0xFF;
        dst[i] = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    }
}

/* ============================================================================
 * v34.28 2D MICRO-BENCHMARK (env NOJME_2D_BENCH=1)
 * ============================================================================
 * Times every optimized 2D primitive with the fast path OFF vs ON on a
 * 240x320 canvas (the real J2ME screen size) and reports Mpx/s and the
 * speedup. NEON-related gains are only visible on ARM hardware/qemu; the
 * algorithmic wins (fill_triangle spans, hoisted clip/switch) show up on
 * every host. Output goes through ALWAYS_LOG (explicitly requested test).
 * ==========================================================================*/
static double bench_now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
}

typedef struct {
    const char* name;
    double mpx_scalar, mpx_fast;
    double px_per_call;
} BenchRow;

static void bench_report(const BenchRow* r) {
    if (r->mpx_fast <= 0) {
        ALWAYS_LOG("[2D-BENCH] %-28s (too fast to measure)\n", r->name);
        return;
    }
    if (r->mpx_scalar > 0) {
        ALWAYS_LOG("[2D-BENCH] %-28s scalar %7.1f Mpx/s   fast %7.1f Mpx/s   %5.2fx\n",
                  r->name, r->mpx_scalar, r->mpx_fast, r->mpx_fast / r->mpx_scalar);
    } else {
        /* compile-time NEON path (no runtime A/B on this build): fast only.
         * Cross-check by comparing x86 vs ARM builds of the same row. */
        ALWAYS_LOG("[2D-BENCH] %-28s fast  %7.1f Mpx/s (NEON is compile-time)\n",
                  r->name, r->mpx_fast);
    }
}

#define BENCH_ITERS 200

int nojme_2d_bench(void) {
    const int W = 240, H = 320, PIX = W * H;
    uint32_t* fb = (uint32_t*)malloc(PIX * 4);
    uint16_t* rgb565 = (uint16_t*)malloc(PIX * 2);
    MidpImage img;
    img.width = W; img.height = H; img.mutable = false; img.alpha = true;
    img.alpha_scan_valid = false; /* v34.51: lazily scanned on first draw */
    img.alpha_all_opaque = false;
    img.pixels = (uint32_t*)malloc(PIX * 4);
    jint* rgbarr = (jint*)malloc(PIX * 4);
    if (!fb || !rgb565 || !img.pixels || !rgbarr) {
        ALWAYS_LOG("[2D-BENCH] alloc failed\n");
        free(fb); free(rgb565); free(img.pixels); free(rgbarr);
        return 1;
    }
    /* deterministic content */
    for (int i = 0; i < PIX; i++) {
        uint32_t v = (uint32_t)(i * 2654435761u);
        img.pixels[i] = ((v % 7) ? ((v % 256) << 24) : 0xFF000000u) | (v & 0xFFFFFF);
        fb[i] = v;
    }

    const int save_2d = g_2d_force_scalar;
    MidpGraphics g;
    midp_graphics_init(&g, fb, W, H);
    midp_graphics_set_color(&g, 0x30C0F0, 255);

    double t0, t1;
    ALWAYS_LOG("[2D-BENCH] canvas %dx%d, %d iterations per primitive\n", W, H, BENCH_ITERS);

    /* 1: fill_rect opaque (clear) — compile-time NEON path */
    {
        double best = 1e9;
        for (int rep = 0; rep < 3; rep++) {
            t0 = bench_now_ms();
            for (int it = 0; it < BENCH_ITERS; it++)
                midp_graphics_fill_rect(&g, 0, 0, W, H);
            t1 = bench_now_ms();
            if (t1 - t0 < best) best = t1 - t0;
        }
        BenchRow r = { "fillRect opaque", 0, (double)PIX * BENCH_ITERS / (best / 1000.0) / 1e6, (double)PIX };
        bench_report(&r);
    }

    double res = 0; (void)res;

    /* 2: fill_rect alpha */
    {
        midp_graphics_set_color(&g, 0x30C0F0, 160);
        t0 = bench_now_ms();
        for (int it = 0; it < BENCH_ITERS; it++)
            midp_graphics_fill_rect(&g, 0, 0, W, H);
        t1 = bench_now_ms();
        res = t1 - t0;
        BenchRow r = { "fillRect alpha=160", 0, (double)PIX * BENCH_ITERS / (res / 1000.0) / 1e6, (double)PIX };
        bench_report(&r);
        midp_graphics_set_color(&g, 0x30C0F0, 255);
    }

    /* 3: draw_image alpha blend (full screen sprite) */
    {
        t0 = bench_now_ms();
        for (int it = 0; it < BENCH_ITERS; it++)
            midp_graphics_draw_image(&g, &img, 0, 0, 20 /*TOP|LEFT*/);
        t1 = bench_now_ms();
        res = t1 - t0;
        BenchRow r = { "drawImage alpha span", 0, (double)PIX * BENCH_ITERS / (res / 1000.0) / 1e6, (double)PIX };
        bench_report(&r);
    }

    /* 4: draw_region TRANS_MIRROR (alpha) */
    {
        double ts = 0, tf = 0;
        for (int pass = 0; pass < 2; pass++) {
            g_2d_force_scalar = pass; /* 0 = fast, 1 = scalar reference */
            t0 = bench_now_ms();
            for (int it = 0; it < BENCH_ITERS; it++)
                midp_graphics_draw_region(&g, &img, 0, 0, W, H, 2, 0, 0, 20);
            t1 = bench_now_ms();
            if (pass == 0) tf = t1 - t0; else ts = t1 - t0;
        }
        BenchRow r = { "drawRegion MIRROR a", (double)PIX * BENCH_ITERS / (ts / 1000.0) / 1e6,
                       (double)PIX * BENCH_ITERS / (tf / 1000.0) / 1e6, (double)PIX };
        bench_report(&r);
    }

    /* 5: draw_region TRANS_ROT180 (alpha) */
    {
        double ts = 0, tf = 0;
        for (int pass = 0; pass < 2; pass++) {
            g_2d_force_scalar = pass;
            t0 = bench_now_ms();
            for (int it = 0; it < BENCH_ITERS; it++)
                midp_graphics_draw_region(&g, &img, 0, 0, W, H, 3, 0, 0, 20);
            t1 = bench_now_ms();
            if (pass == 0) tf = t1 - t0; else ts = t1 - t0;
        }
        BenchRow r = { "drawRegion ROT180 a", (double)PIX * BENCH_ITERS / (ts / 1000.0) / 1e6,
                       (double)PIX * BENCH_ITERS / (tf / 1000.0) / 1e6, (double)PIX };
        bench_report(&r);
    }

    /* 6: fill_triangle (screen-covering triangle) */
    {
        double ts = 0, tf = 0;
        for (int pass = 0; pass < 2; pass++) {
            g_2d_force_scalar = pass;
            t0 = bench_now_ms();
            for (int it = 0; it < BENCH_ITERS; it++)
                midp_graphics_fill_triangle(&g, 0, 0, W - 1, 0, 0, H - 1);
            t1 = bench_now_ms();
            if (pass == 0) tf = t1 - t0; else ts = t1 - t0;
        }
        BenchRow r = { "fillTriangle", (double)PIX * BENCH_ITERS / (ts / 1000.0) / 1e6,
                       (double)PIX * BENCH_ITERS / (tf / 1000.0) / 1e6, (double)PIX };
        bench_report(&r);
    }

    /* 7: draw_string (text wall: 12 rows x 38 chars) */
    {
        const char* txt = "The quick brown fox jumps over 0123456789";
        double ts = 0, tf = 0;
        for (int pass = 0; pass < 2; pass++) {
            g_2d_force_scalar = pass;
            t0 = bench_now_ms();
            for (int it = 0; it < 40; it++) {
                for (int row = 0; row < 12; row++)
                    midp_graphics_draw_string(&g, txt, 2, 2 + row * 9, 20);
            }
            t1 = bench_now_ms();
            if (pass == 0) tf = t1 - t0; else ts = t1 - t0;
        }
        const double px = 12.0 * 38 * 40 * 40.0; /* rough glyph pixel count */
        BenchRow r = { "drawString 12x38", px / (ts / 1000.0) / 1e6, px / (tf / 1000.0) / 1e6, px };
        bench_report(&r);
    }

    /* 8: get_rgb full screen */
    {
        double ts = 0, tf = 0;
        for (int pass = 0; pass < 2; pass++) {
            g_2d_force_scalar = pass;
            t0 = bench_now_ms();
            for (int it = 0; it < BENCH_ITERS; it++)
                midp_graphics_get_rgb(&g, rgbarr, 0, W, 0, 0, W, H);
            t1 = bench_now_ms();
            if (pass == 0) tf = t1 - t0; else ts = t1 - t0;
        }
        BenchRow r = { "getRGB full screen", (double)PIX * BENCH_ITERS / (ts / 1000.0) / 1e6,
                       (double)PIX * BENCH_ITERS / (tf / 1000.0) / 1e6, (double)PIX };
        bench_report(&r);
    }

    /* 9: blit_rgb alpha (drawRGB) */
    {
        t0 = bench_now_ms();
        for (int it = 0; it < BENCH_ITERS; it++)
            midp_blit_rgb(&g, rgbarr, PIX, 0, W, 0, 0, W, H, 1);
        t1 = bench_now_ms();
        res = t1 - t0;
        BenchRow r = { "drawRGB alpha", 0, (double)PIX * BENCH_ITERS / (res / 1000.0) / 1e6, (double)PIX };
        bench_report(&r);
    }

    /* 10: RGB565 frame conversion */
    {
        double ts = 0, tf = 0;
        /* volatile dims: block IPA constant propagation so BOTH sides run
         * with runtime bounds — exactly like the retro_run call sites. */
        volatile int vw = W, vh = H;
        for (int pass = 0; pass < 2; pass++) {
            if (pass == 0) {
                /* scalar reference: the ORIGINAL retro_run conversion shape */
                t0 = bench_now_ms();
                for (int it = 0; it < BENCH_ITERS; it++) {
                    bench_rgb565_scalar_ref(fb, rgb565, vw, vh);
                }
                t1 = bench_now_ms();
                ts = t1 - t0;
            } else {
                t0 = bench_now_ms();
                for (int it = 0; it < BENCH_ITERS; it++)
                    nojme_convert_xrgb8888_to_rgb565(fb, rgb565, vw, vh);
                t1 = bench_now_ms();
                tf = t1 - t0;
            }
        }
        BenchRow r = { "RGB565 conversion", (double)PIX * BENCH_ITERS / (ts / 1000.0) / 1e6,
                       (double)PIX * BENCH_ITERS / (tf / 1000.0) / 1e6, (double)PIX };
        bench_report(&r);
    }

    g_2d_force_scalar = save_2d;
    free(fb); free(rgb565); free(img.pixels); free(rgbarr);
    ALWAYS_LOG("[2D-BENCH] done\n");
    return 0;
}
