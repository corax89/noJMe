/*
 * Nokia M3D Software 3D Renderer - Native Implementation
 *
 * Implements com.nokia.mid.m3d.M3D for the nojme J2ME emulator.
 * Ported from the FreeJ2ME reference implementation, then completed in v17:
 *
 *  - bindTexture: real texturing. The Texture object's lcdui Image is
 *    sampled (nearest, wrap via mask) with screen-linear UV interpolation
 *    and GL_MODULATE-style multiplication into the current color.
 *  - drawArrays: real immediate-mode rendering (TRIANGLES / STRIP / FAN,
 *    plus LINES/POINTS), replacing the "fill rectangle downward" hack.
 *  - Matrix stack: 16 levels per matrix mode (was depth 1).
 *  - matrixMode: MODELVIEW / PROJECTION / TEXTURE targets honored for
 *    loadIdentity / push / pop / scale / translate / rotate.
 *  - enable / disable / cullFace: CULL_FACE, TEXTURE_2D, BLEND, ALPHA_TEST
 *    tracked; back/front face culling by signed screen-area winding.
 *  - viewport: output region within the framebuffer (was ignored).
 *  - alpha: color4ub alpha is kept; fragments with alpha < 255 are
 *    alpha-blended onto the framebuffer (src-over) instead of dropped.
 *  - rotatexi: arbitrary axis (Rodrigues) instead of single-axis selector.
 *  - vertexPointerub / texCoordPointerub: size & stride honored; vertices
 *    are no longer transformed in place (repeat draws were double-transformed).
 *
 *  The projection math (m3d_projection, frustumxi argument order, Y mapping)
 *  is preserved exactly from the FreeJ2ME port - several shipped games depend
 *  on it. Do not "fix" the Y direction without visual re-verification.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "native.h"
#include "jvm.h"
#include "heap.h"
#include "midp.h"

/* ========================================================================= */
/* ARM compiler hints                                                         */
/* ========================================================================= */

#if defined(__ARM_ARCH_7A__) || defined(__arm__)
#define M3D_INLINE __attribute__((always_inline)) static inline
#else
#define M3D_INLINE static inline
#endif

/** Fast float-to-int conversion: on ARMv7, avoids slow libgcc helper */
M3D_INLINE int m3d_float_to_int(float f) {
#if defined(__ARM_ARCH_7A__) || defined(__arm__)
    int result;
    __asm__ volatile("vcvt.s32.f32 %0, %1" : "=t"(result) : "t"(f));
    return result;
#else
    return (int)f;
#endif
}

/* ========================================================================= */
/* Constants                                                                  */
/* ========================================================================= */

#define MAX_M3D_INSTANCES  16
#define DEG2RAD            0.017453292519943295f  /* pi/180 */

/* Anchor constants (from javax.microedition.lcdui.Graphics) */
#define ANCHOR_TOP_LEFT    0x04  /* TOP | LEFT */

/* Buffer capacities (floats) */
#define M3D_MAX_VERT_FLOATS 3072   /* up to 1024 xyz vertices */
#define M3D_MAX_UV_FLOATS   2048   /* up to 1024 uv pairs */

/* Matrix modes (GL constants used by games) */
#define M3D_MODE_MODELVIEW   0x1700
#define M3D_MODE_PROJECTION  0x1701
#define M3D_MODE_TEXTURE     0x1702

/* glEnable constants */
#define M3D_GL_ALPHA_TEST  0x0BC0
#define M3D_GL_CULL_FACE   0x0B44
#define M3D_GL_LIGHTING    0x0B50
#define M3D_GL_FOG         0x0B60
#define M3D_GL_DEPTH_TEST  0x0B71
#define M3D_GL_BLEND       0x0BE2
#define M3D_GL_TEXTURE_2D  0x0DE1

/* cullFace constants */
#define M3D_GL_FRONT           0x0404
#define M3D_GL_BACK            0x0405
#define M3D_GL_FRONT_AND_BACK  0x0408

/* drawArrays / drawElements modes */
#define M3D_POINTS          0
#define M3D_LINES           1
#define M3D_LINE_LOOP       2
#define M3D_LINE_STRIP      3
#define M3D_TRIANGLES       4
#define M3D_TRIANGLE_STRIP  5
#define M3D_TRIANGLE_FAN    6

#define M3D_STACK_DEPTH 16

/* ========================================================================= */
/* Per-instance state                                                         */
/* ========================================================================= */

typedef struct {
    /* Transformation matrices (column-major, OpenGL style) */
    float matrix[16];            /* modelview */
    float projm[16];             /* projection */
    float texmat[16];            /* texture matrix */
    int   matrix_mode;           /* current matrixMode target */

    /* Matrix stacks (per target; depth 16 - was 1) */
    float mv_stack[M3D_STACK_DEPTH][16];
    int   mv_sp;
    float pr_stack[M3D_STACK_DEPTH][16];
    int   pr_sp;
    float tx_stack[M3D_STACK_DEPTH][16];
    int   tx_sp;

    /* Raw vertex/UV arrays (transformed at draw time, never in place) */
    float verts[M3D_MAX_VERT_FLOATS];
    int   vertCount;             /* number of floats stored */
    int   vp_size;               /* components per vertex (3 or 4) */
    int   vp_stride;             /* byte stride (0 = tightly packed) */

    float UVs[M3D_MAX_UV_FLOATS];
    int   uvCount;               /* number of floats stored */
    int   tp_size;
    int   tp_stride;

    /* Draw-time scratch (modelview-transformed copy + projected uvs) */
    float tmp_xyz[M3D_MAX_VERT_FLOATS];
    float tmp_uv[M3D_MAX_UV_FLOATS];

    /* Framebuffer */
    int width, height;
    float z_near, z_far;   /* renamed: `near`/`far` are empty macros in MinGW windows.h */
    float* zbuffer;
    MidpImage* framebuffer;
    MidpGraphics* gc;

    /* Viewport within the framebuffer (default = full image) */
    int vp_x, vp_y, vp_w, vp_h;

    /* Drawing state */
    unsigned int color;      /* ARGB packed color (alpha honored since v17) */
    unsigned int clearcolor; /* ARGB packed color for clearing */

    /* GL state flags */
    int boundTexture;        /* legacy flag: a Texture object is bound */
    bool enable_texture2d;   /* effective: texture applies */
    bool explicit_tex_off;   /* disable(GL_TEXTURE_2D) seen */
    bool enable_cull;
    bool enable_blend;
    bool enable_alpha_test;
    int  cull_mode;          /* M3D_GL_BACK etc. */

    /* Bound texture (from com.nokia.mid.m3d.Texture) */
    MidpImage* tex_img;
    int tex_w, tex_h;

    int active;              /* whether this slot is in use */
} M3DState;

/* Instance pool */
static M3DState   m3d_states[MAX_M3D_INSTANCES];
static JavaObject* m3d_objects[MAX_M3D_INSTANCES];

/* ========================================================================= */
/* Instance lookup / allocation                                               */
/* ========================================================================= */

static void m3d_free_state(M3DState* state);

static M3DState* m3d_get_state(JavaObject* obj) {
    for (int i = 0; i < MAX_M3D_INSTANCES; i++) {
        if (m3d_states[i].active && m3d_objects[i] == obj) {
            return &m3d_states[i];
        }
        /* Guard against collected Java objects (v15 lesson) */
        if (m3d_states[i].active && m3d_objects[i] && !heap_java_object_valid(m3d_objects[i])) {
            m3d_free_state(&m3d_states[i]);
            m3d_objects[i] = NULL;
        }
    }
    return NULL;
}

static M3DState* m3d_alloc_state(JavaObject* obj) {
    /* Try to find an existing slot for this object first */
    for (int i = 0; i < MAX_M3D_INSTANCES; i++) {
        if (m3d_states[i].active && m3d_objects[i] == obj) {
            return &m3d_states[i];
        }
    }
    /* Allocate a new slot */
    for (int i = 0; i < MAX_M3D_INSTANCES; i++) {
        if (!m3d_states[i].active) {
            memset(&m3d_states[i], 0, sizeof(M3DState));
            m3d_objects[i] = obj;
            m3d_states[i].active = 1;
            m3d_states[i].color = 0xFF000000;       /* opaque black */
            m3d_states[i].clearcolor = 0xFFFFFFFF;  /* opaque white */
            return &m3d_states[i];
        }
    }
    return NULL;
}

static void m3d_free_state(M3DState* state) {
    if (!state) return;
    if (state->zbuffer) {
        free(state->zbuffer);
        state->zbuffer = NULL;
    }
    /* Note: framebuffer is freed by the MIDP image subsystem */
    state->framebuffer = NULL;
    state->gc = NULL;
    state->width = 0;
    state->height = 0;
    state->active = 0;
}

/* ========================================================================= */
/* Matrix math helpers                                                        */
/* ========================================================================= */

/** Set a 4x4 matrix to identity (column-major) */
M3D_INLINE void m3d_identity(float m[16]) {
    memset(m, 0, sizeof(float) * 16);
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

/** Copy src matrix to dst */
M3D_INLINE void m3d_clone(float dst[16], const float src[16]) {
    memcpy(dst, src, sizeof(float) * 16);
}

/** Multiply a = a * b (column-major) */
M3D_INLINE void m3d_matmul(float a[16], const float b[16]) {
    float result[16];
    for (int j = 0; j < 4; j++) {
        for (int i = 0; i < 4; i++) {
            result[j * 4 + i] = a[j * 4 + 0] * b[0 * 4 + i]
                               + a[j * 4 + 1] * b[1 * 4 + i]
                               + a[j * 4 + 2] * b[2 * 4 + i]
                               + a[j * 4 + 3] * b[3 * 4 + i];
        }
    }
    memcpy(a, result, sizeof(float) * 16);
}

/** Set up perspective projection matrix */
M3D_INLINE void m3d_projection(float m[16], float w, float h, float n, float f) {
    memset(m, 0, sizeof(float) * 16);
    float fn = 2.0f * n;
    m[0]  = fn / w;
    m[5]  = fn / h;
    m[10] = -(f + n) / (f - n);
    m[11] = -1.0f;
    m[14] = -(2.0f * f * n) / (f - n);
    /* m[15] = 0 (already zeroed) */
}

/** Select the active matrix target by matrixMode */
static float* m3d_current_matrix(M3DState* state) {
    switch (state->matrix_mode) {
        case M3D_MODE_PROJECTION: return state->projm;
        case M3D_MODE_TEXTURE:    return state->texmat;
        case M3D_MODE_MODELVIEW:
        default:                  return state->matrix;
    }
}

M3D_INLINE void m3d_transform_point(float* out, const float m[16],
                                    float x, float y, float z) {
    out[0] = x * m[0] + y * m[4] + z * m[8]  + m[12];
    out[1] = x * m[1] + y * m[5] + z * m[9]  + m[13];
    out[2] = x * m[2] + y * m[6] + z * m[10] + m[14];
}

/* ========================================================================= */
/* Scanline triangle rasterizer with Z-buffer, texturing and alpha            */
/* ========================================================================= */

/** Sample the bound texture (nearest, wrap-by-mask for power-of-two sizes) */
M3D_INLINE uint32_t m3d_sample_texture(const M3DState* state, float u, float v) {
    const MidpImage* img = state->tex_img;
    if (!img || !img->pixels || state->tex_w <= 0 || state->tex_h <= 0) return state->color;

    int tw = state->tex_w, th = state->tex_h;
    int tu, tv;

    if ((tw & (tw - 1)) == 0) tu = ((int)u) & (tw - 1);
    else { tu = (int)u; if (tu < 0) tu = 0; if (tu >= tw) tu = tw - 1; }
    if ((th & (th - 1)) == 0) tv = ((int)v) & (th - 1);
    else { tv = (int)v; if (tv < 0) tv = 0; if (tv >= th) tv = th - 1; }

    return img->pixels[tv * tw + tu];
}

/**
 * Filled triangle with per-pixel Z-buffer. UVs (when texturing is active)
 * are interpolated screen-linearly (byte-texel semantics of M3D), texture is
 * modulated by the current color; alpha < 255 blends src-over.
 */
M3D_INLINE void m3d_fill_triangle_uv(M3DState* state,
                                     float x1, float y1, float z1, float u1, float v1,
                                     float x2, float y2, float z2, float u2, float v2,
                                     float x3, float y3, float z3, float u3, float v3) {
    int w = state->width;
    int h = state->height;
    if (w <= 0 || h <= 0) return;

    uint32_t* pixels = state->framebuffer->pixels;
    float* zbuffer = state->zbuffer;
    unsigned int color = state->color;
    bool textured = (state->enable_texture2d && state->tex_img &&
                     state->tex_img->pixels && state->boundTexture);

    uint8_t col_a = (color >> 24) & 0xFF;
    uint8_t col_r = (color >> 16) & 0xFF;
    uint8_t col_g = (color >> 8) & 0xFF;
    uint8_t col_b = color & 0xFF;

    /* Back-face culling by signed screen area (when enabled) */
    if (state->enable_cull) {
        float area = (x2 - x1) * (y3 - y1) - (x3 - x1) * (y2 - y1);
        bool is_back = area < 0.0f;
        if ((state->cull_mode == M3D_GL_BACK && is_back) ||
            (state->cull_mode == M3D_GL_FRONT && !is_back) ||
            (state->cull_mode == M3D_GL_FRONT_AND_BACK)) {
            return;
        }
    }

    /* Sort vertices by Y coordinate (top to bottom), carrying UVs */
    #define M3D_SWAP(a, b) do { \
        float tx_, ty_, tz_, tu_, tv_; \
        tx_ = x##a; x##a = x##b; x##b = tx_; \
        ty_ = y##a; y##a = y##b; y##b = ty_; \
        tz_ = z##a; z##a = z##b; z##b = tz_; \
        tu_ = u##a; u##a = u##b; u##b = tu_; \
        tv_ = v##a; v##a = v##b; v##b = tv_; \
    } while (0)
    if (y1 > y2) M3D_SWAP(1, 2);
    if (y1 > y3) M3D_SWAP(1, 3);
    if (y2 > y3) M3D_SWAP(2, 3);
    #undef M3D_SWAP

    int iy1 = m3d_float_to_int(y1), iy2 = m3d_float_to_int(y2), iy3 = m3d_float_to_int(y3);

    /* Clip to screen bounds */
    int miny = (iy1 < 0) ? 0 : iy1;
    int maxy = (iy3 >= h) ? h - 1 : iy3;
    if (miny > maxy) return;

    for (int y = miny; y <= maxy; y++) {
        float xl, xr, zl, zr, ul, ur, vl, vr;

        if (y <= iy2) {
            float dy = (iy2 != iy1) ? (float)(y - iy1) / (float)(iy2 - iy1) : 0.0f;
            if (iy2 == iy1) {
                xl = x1; xr = x3; zl = z1; zr = z3; ul = u1; ur = u3; vl = v1; vr = v3;
            } else {
                xl = x1 + (x2 - x1) * dy; zl = z1 + (z2 - z1) * dy;
                ul = u1 + (u2 - u1) * dy; vl = v1 + (v2 - v1) * dy;
            }
            {
                float dy3 = (iy3 != iy1) ? (float)(y - iy1) / (float)(iy3 - iy1) : 0.0f;
                xr = x1 + (x3 - x1) * dy3; zr = z1 + (z3 - z1) * dy3;
                ur = u1 + (u3 - u1) * dy3; vr = v1 + (v3 - v1) * dy3;
            }
            if (xl > xr) {
                float t; t=xl;xl=xr;xr=t; t=zl;zl=zr;zr=t;
                t=ul;ul=ur;ur=t; t=vl;vl=vr;vr=t;
            }
        } else {
            float dy23 = (iy3 != iy2) ? (float)(y - iy2) / (float)(iy3 - iy2) : 0.0f;
            xl = x2 + (x3 - x2) * dy23; zl = z2 + (z3 - z2) * dy23;
            ul = u2 + (u3 - u2) * dy23; vl = v2 + (v3 - v2) * dy23;
            {
                float dy13 = (iy3 != iy1) ? (float)(y - iy1) / (float)(iy3 - iy1) : 0.0f;
                xr = x1 + (x3 - x1) * dy13; zr = z1 + (z3 - z1) * dy13;
                ur = u1 + (u3 - u1) * dy13; vr = v1 + (v3 - v1) * dy13;
            }
            if (xl > xr) {
                float t; t=xl;xl=xr;xr=t; t=zl;zl=zr;zr=t;
                t=ul;ul=ur;ur=t; t=vl;vl=vr;vr=t;
            }
        }

        int ixl = m3d_float_to_int(xl);
        int ixr = m3d_float_to_int(xr);
        if (ixl < 0) ixl = 0;
        if (ixr >= w) ixr = w - 1;

        int span = ixr - ixl;
        if (span <= 0) continue;

        float inv_span = 1.0f / (float)span;
        float dzdx = (zr - zl) * inv_span;
        float dudx = (ur - ul) * inv_span;
        float dvdx = (vr - vl) * inv_span;

        uint32_t* row_pixels = pixels + y * w;
        float* row_zbuf = zbuffer + y * w;
        float z = zl;
        float u = ul;
        float v = vl;

        for (int ix = ixl; ix <= ixr; ix++) {
            if (z < row_zbuf[ix]) {
                uint32_t out_pixel;

                if (textured) {
                    uint32_t texel = m3d_sample_texture(state, u, v);
                    uint8_t t_a = (texel >> 24) & 0xFF;
                    uint8_t t_r = (texel >> 16) & 0xFF;
                    uint8_t t_g = (texel >> 8) & 0xFF;
                    uint8_t t_b = texel & 0xFF;
                    /* MODULATE: texel * current color */
                    uint8_t o_a = (uint8_t)((t_a * col_a) / 255);
                    uint8_t o_r = (uint8_t)((t_r * col_r) / 255);
                    uint8_t o_g = (uint8_t)((t_g * col_g) / 255);
                    uint8_t o_b = (uint8_t)((t_b * col_b) / 255);
                    out_pixel = ((uint32_t)o_a << 24) | ((uint32_t)o_r << 16) |
                                ((uint32_t)o_g << 8) | o_b;
                } else {
                    out_pixel = color;
                }

                uint8_t out_a = (out_pixel >> 24) & 0xFF;
                if (out_a >= 255) {
                    row_zbuf[ix] = z;
                    row_pixels[ix] = out_pixel | 0xFF000000u;
                } else if (out_a > 0) {
                    /* src-over alpha blend (M3D games rely on alpha < 255
                     * even without an explicit glEnable(GL_BLEND)) */
                    uint32_t dst = row_pixels[ix];
                    uint32_t ia = 255 - out_a;
                    uint8_t r = ((out_pixel >> 16) & 0xFF) * out_a / 255 + ((dst >> 16) & 0xFF) * ia / 255;
                    uint8_t g = ((out_pixel >> 8) & 0xFF) * out_a / 255 + ((dst >> 8) & 0xFF) * ia / 255;
                    uint8_t b = (out_pixel & 0xFF) * out_a / 255 + (dst & 0xFF) * ia / 255;
                    row_pixels[ix] = 0xFF000000u | (r << 16) | (g << 8) | b;
                    row_zbuf[ix] = z;
                }
            }
            z += dzdx;
            u += dudx;
            v += dvdx;
        }
    }
}

/* Flat-color convenience wrapper (lines etc. use color4ub path anyway) */
M3D_INLINE void m3d_fill_triangle(M3DState* state,
                                  float x1, float y1, float z1,
                                  float x2, float y2, float z2,
                                  float x3, float y3, float z3) {
    m3d_fill_triangle_uv(state, x1, y1, z1, 0, 0, x2, y2, z2, 0, 0, x3, y3, z3, 0, 0);
}

/* Draw a single line in screen space with current color (no depth test) */
static void m3d_draw_line(M3DState* state, float x1, float y1, float x2, float y2) {
    MidpGraphics* gc = state->gc;
    if (!gc) return;
    midp_graphics_set_color(gc, (int)state->color,
                            (int)((state->color >> 24) & 0xFF));
    midp_graphics_draw_line(gc, m3d_float_to_int(x1), m3d_float_to_int(y1),
                                m3d_float_to_int(x2), m3d_float_to_int(y2));
}

/* ========================================================================= */
/* Shared transform + project + rasterize pipeline                            */
/* ========================================================================= */

/**
 * Render vertex data through the full pipeline:
 *   modelview -> near cull -> projection -> viewport -> rasterize
 * xyz is count*3 floats (already modelview-transformed); uv may be NULL.
 */
static void m3d_render_triangles(M3DState* state, const float* xyz, const float* uv,
                                 const unsigned char* idx, int ntri) {
    if (!state || !state->framebuffer || !state->gc || !xyz || ntri <= 0) return;

    float ox = state->vp_w * 0.5f;
    float oy = state->vp_h * 0.5f;
    float vcx = state->vp_x + ox;
    float vcy = state->vp_y + oy;

    for (int t = 0; t < ntri; t++) {
        int i0, i1, i2;
        if (idx) {
            i0 = idx[t * 3] * 3;
            i1 = idx[t * 3 + 1] * 3;
            i2 = idx[t * 3 + 2] * 3;
        } else {
            i0 = (t * 3) * 3;
            i1 = (t * 3 + 1) * 3;
            i2 = (t * 3 + 2) * 3;
        }

        /* Near-plane culling (preserve FreeJ2ME semantics: skip when ALL
         * vertices are behind the near plane; vertices in front have
         * negative z) */
        if (xyz[i0 + 2] > state->z_near && xyz[i1 + 2] > state->z_near &&
            xyz[i2 + 2] > state->z_near) {
            continue;
        }

        float p0[4], p1[4], p2[4];
        m3d_transform_point(p0, state->projm, xyz[i0], xyz[i0 + 1], xyz[i0 + 2]);
        m3d_transform_point(p1, state->projm, xyz[i1], xyz[i1 + 1], xyz[i1 + 2]);
        m3d_transform_point(p2, state->projm, xyz[i2], xyz[i2 + 1], xyz[i2 + 2]);

        /* Perspective divide (preserve original guard semantics) */
        float w0 = xyz[i0] * state->projm[3] + xyz[i0 + 1] * state->projm[7] +
                   xyz[i0 + 2] * state->projm[11] + state->projm[15];
        float w1 = xyz[i1] * state->projm[3] + xyz[i1 + 1] * state->projm[7] +
                   xyz[i1 + 2] * state->projm[11] + state->projm[15];
        float w2 = xyz[i2] * state->projm[3] + xyz[i2 + 1] * state->projm[7] +
                   xyz[i2 + 2] * state->projm[11] + state->projm[15];
        if (w0 != 0.0f && w0 != 1.0f) { p0[0] /= w0; p0[1] /= w0; p0[2] /= w0; }
        if (w1 != 0.0f && w1 != 1.0f) { p1[0] /= w1; p1[1] /= w1; p1[2] /= w1; }
        if (w2 != 0.0f && w2 != 1.0f) { p2[0] /= w2; p2[1] /= w2; p2[2] /= w2; }

        /* NDC -> framebuffer coordinates inside the viewport */
        float sx0 = p0[0] * ox + vcx, sy0 = p0[1] * oy + vcy;
        float sx1 = p1[0] * ox + vcx, sy1 = p1[1] * oy + vcy;
        float sx2 = p2[0] * ox + vcx, sy2 = p2[1] * oy + vcy;

        float u0 = 0, v0 = 0, u1c = 0, v1c = 0, u2c = 0, v2c = 0;
        if (uv) {
            int j0 = i0 / 3 * 2, j1 = i1 / 3 * 2, j2 = i2 / 3 * 2;
            u0 = uv[j0]; v0 = uv[j0 + 1];
            u1c = uv[j1]; v1c = uv[j1 + 1];
            u2c = uv[j2]; v2c = uv[j2 + 1];
            /* Apply the texture matrix when TEXTURE mode has been customized */
            static const float ident[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
            if (memcmp(state->texmat, ident, sizeof(ident)) != 0) {
                float* tm = state->texmat;
                float nu0 = u0 * tm[0] + v0 * tm[4] + tm[12];
                float nv0 = u0 * tm[1] + v0 * tm[5] + tm[13];
                float nu1 = u1c * tm[0] + v1c * tm[4] + tm[12];
                float nv1 = u1c * tm[1] + v1c * tm[5] + tm[13];
                float nu2 = u2c * tm[0] + v2c * tm[4] + tm[12];
                float nv2 = u2c * tm[1] + v2c * tm[5] + tm[13];
                u0 = nu0; v0 = nv0; u1c = nu1; v1c = nv1; u2c = nu2; v2c = nv2;
            }
        }

        m3d_fill_triangle_uv(state,
                             sx0, sy0, p0[2], u0, v0,
                             sx1, sy1, p1[2], u1c, v1c,
                             sx2, sy2, p2[2], u2c, v2c);
    }
}

/* ========================================================================= */
/* Native method handlers                                                     */
/* ========================================================================= */

/*
 * Static method: M3D.createInstance()Lcom/nokia/mid/m3d/M3D;
 */
static JavaValue native_m3d_createInstance(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)thread; (void)args; (void)arg_count;

    JavaClass* m3d_class = jvm_load_class(jvm, "com/nokia/mid/m3d/M3D");
    if (!m3d_class) {
        return NATIVE_RETURN_NULL();
    }

    JavaObject* obj = jvm_new_object(jvm, m3d_class);
    if (!obj) {
        return NATIVE_RETURN_NULL();
    }

    M3DState* state = m3d_alloc_state(obj);
    if (!state) {
        return NATIVE_RETURN_NULL();
    }

    /* Initialize with defaults */
    state->color = 0xFF000000;
    state->clearcolor = 0xFFFFFFFF;
    m3d_identity(state->matrix);
    m3d_identity(state->projm);
    m3d_identity(state->texmat);
    state->matrix_mode = M3D_MODE_MODELVIEW;
    state->enable_texture2d = true;   /* bindTexture implies texturing (v17) */
    state->explicit_tex_off = false;
    state->enable_cull = false;
    state->enable_blend = false;
    state->enable_alpha_test = false;
    state->cull_mode = M3D_GL_BACK;

    return NATIVE_RETURN_OBJECT(obj);
}

/*
 * M3D.setupBuffers(III)V
 */
static JavaValue native_m3d_setupBuffers(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;

    JavaObject* obj = (JavaObject*)args[0].ref;
    M3DState* state = m3d_get_state(obj);
    if (!state) return NATIVE_RETURN_VOID();

    jint flags  = args[1].i;
    jint width  = args[2].i;
    jint height = args[3].i;
    (void)flags;

    /* Free previous buffers if any */
    if (state->zbuffer) {
        free(state->zbuffer);
        state->zbuffer = NULL;
    }
    state->framebuffer = NULL;
    state->gc = NULL;

    /* Validate dimensions */
    if (width <= 0 || height <= 0) return NATIVE_RETURN_VOID();
    if (width > 4096 || height > 4096) return NATIVE_RETURN_VOID();

    state->width  = width;
    state->height = height;

    /* Viewport defaults to the full framebuffer */
    state->vp_x = 0;
    state->vp_y = 0;
    state->vp_w = width;
    state->vp_h = height;

    /* Create framebuffer image */
    state->framebuffer = midp_image_create(width, height, 1);
    if (!state->framebuffer) return NATIVE_RETURN_VOID();

    /* Get graphics context for the framebuffer */
    state->gc = midp_image_get_graphics(state->framebuffer);
    if (!state->gc) {
        state->framebuffer = NULL;
        return NATIVE_RETURN_VOID();
    }

    /* Allocate Z-buffer (float: 4 bytes/pixel) */
    state->zbuffer = (float*)calloc((size_t)width * height, sizeof(float));
    if (!state->zbuffer) {
        state->framebuffer = NULL;
        state->gc = NULL;
        return NATIVE_RETURN_VOID();
    }

    /* Initialize Z-buffer to far distance */
    {
        int total = width * height;
        float far_val = -128.0f;
        for (int i = 0; i < total; i++) {
            state->zbuffer[i] = far_val;
        }
    }

    /* Do an initial clear */
    if (state->gc) {
        midp_graphics_set_color(state->gc, (int)state->clearcolor, 0xFF);
        midp_graphics_fill_rect(state->gc, 0, 0, width, height);
    }

    m3d_identity(state->matrix);
    m3d_identity(state->projm);
    m3d_identity(state->texmat);
    state->mv_sp = state->pr_sp = state->tx_sp = 0;
    state->boundTexture = 0;
    state->tex_img = NULL;
    state->tex_w = state->tex_h = 0;

    return NATIVE_RETURN_VOID();
}

/*
 * M3D.removeBuffers()V
 */
static JavaValue native_m3d_removeBuffers(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;

    JavaObject* obj = (JavaObject*)args[0].ref;
    M3DState* state = m3d_get_state(obj);
    if (!state) return NATIVE_RETURN_VOID();

    m3d_free_state(state);
    return NATIVE_RETURN_VOID();
}

/*
 * M3D.clear(I)V
 */
static JavaValue native_m3d_clear(JVM* jvm, JavaThread* thread,
                                  JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;

    JavaObject* obj = (JavaObject*)args[0].ref;
    M3DState* state = m3d_get_state(obj);
    if (!state || !state->gc) return NATIVE_RETURN_VOID();

    /* Clear the framebuffer with clear color */
    midp_graphics_set_color(state->gc, (int)state->clearcolor, 0xFF);
    midp_graphics_fill_rect(state->gc, 0, 0, state->width, state->height);

    /* Reset drawing color */
    midp_graphics_set_color(state->gc, (int)state->color, 0xFF);

    /* Reset Z-buffer */
    if (state->zbuffer) {
        int total = state->width * state->height;
        float far_val = -128.0f;
        for (int i = 0; i < total; i++) {
            state->zbuffer[i] = far_val;
        }
    }

    return NATIVE_RETURN_VOID();
}

/*
 * M3D.blit(Ljavax/microedition/lcdui/Graphics;IIII)V
 */
static JavaValue native_m3d_blit(JVM* jvm, JavaThread* thread,
                                 JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;

    JavaObject* obj = (JavaObject*)args[0].ref;
    M3DState* state = m3d_get_state(obj);
    if (!state || !state->framebuffer) return NATIVE_RETURN_VOID();

    JavaObject* gfx_obj = (JavaObject*)args[1].ref;
    jint x = args[2].i;
    jint y = args[3].i;
    /* args[4] = w, args[5] = h - not used (draw full framebuffer) */

    MidpGraphics* screen_gfx = get_graphics_from_object(gfx_obj);
    if (!screen_gfx) return NATIVE_RETURN_VOID();

    midp_graphics_draw_image(screen_gfx, state->framebuffer, x, y, ANCHOR_TOP_LEFT);

    return NATIVE_RETURN_VOID();
}

/*
 * M3D.bindTexture(ILcom/nokia/mid/m3d/Texture;)V - v17: REAL texturing.
 * Reads the lcdui Image out of the Texture object and caches its pixels.
 */
static JavaValue native_m3d_bindTexture(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;

    JavaObject* obj = (JavaObject*)args[0].ref;
    M3DState* state = m3d_get_state(obj);
    if (!state) return NATIVE_RETURN_VOID();

    JavaObject* texture_obj = (JavaObject*)args[2].ref;
    if (!texture_obj || !heap_java_object_valid(texture_obj)) {
        state->boundTexture = 0;
        state->tex_img = NULL;
        state->tex_w = state->tex_h = 0;
        return NATIVE_RETURN_VOID();
    }

    /* Texture stub fields (stubs.c): [0]=image ref, [1]=format int */
    JavaObject* image_obj = NULL;
    if (texture_obj->header.clazz) {
        JavaClass* clazz = texture_obj->header.clazz;
        for (int i = 0; i < clazz->fields_count; i++) {
            if (clazz->fields[i].name && strcmp(clazz->fields[i].name, "image") == 0) {
                image_obj = (JavaObject*)texture_obj->fields[i].ref;
                break;
            }
        }
    }

    state->tex_img = NULL;
    state->tex_w = state->tex_h = 0;

    if (image_obj && heap_java_object_valid(image_obj)) {
        extern MidpImage* get_image_from_object(JavaObject* obj);
        MidpImage* img = get_image_from_object(image_obj);
        if (img && img->pixels && img->width > 0 && img->height > 0) {
            state->tex_img = img;
            state->tex_w = img->width;
            state->tex_h = img->height;
            state->boundTexture = 1;
            state->enable_texture2d = true;
            state->explicit_tex_off = false;
            return NATIVE_RETURN_VOID();
        }
    }

    /* No usable image: keep the legacy flag so rendering still runs flat */
    state->boundTexture = 0;
    return NATIVE_RETURN_VOID();
}

/*
 * M3D.loadIdentity()V - applies to the current matrixMode target
 */
static JavaValue native_m3d_loadIdentity(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;

    JavaObject* obj = (JavaObject*)args[0].ref;
    M3DState* state = m3d_get_state(obj);
    if (!state) return NATIVE_RETURN_VOID();

    m3d_identity(m3d_current_matrix(state));

    return NATIVE_RETURN_VOID();
}

/*
 * M3D.pushMatrix()V - per-mode stack, depth 16
 */
static JavaValue native_m3d_pushMatrix(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;

    JavaObject* obj = (JavaObject*)args[0].ref;
    M3DState* state = m3d_get_state(obj);
    if (!state) return NATIVE_RETURN_VOID();

    switch (state->matrix_mode) {
        case M3D_MODE_PROJECTION:
            if (state->pr_sp < M3D_STACK_DEPTH) {
                m3d_clone(state->pr_stack[state->pr_sp], state->projm);
                state->pr_sp++;
            }
            break;
        case M3D_MODE_TEXTURE:
            if (state->tx_sp < M3D_STACK_DEPTH) {
                m3d_clone(state->tx_stack[state->tx_sp], state->texmat);
                state->tx_sp++;
            }
            break;
        default:
            if (state->mv_sp < M3D_STACK_DEPTH) {
                m3d_clone(state->mv_stack[state->mv_sp], state->matrix);
                state->mv_sp++;
            }
            break;
    }

    return NATIVE_RETURN_VOID();
}

/*
 * M3D.popMatrix()V
 */
static JavaValue native_m3d_popMatrix(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;

    JavaObject* obj = (JavaObject*)args[0].ref;
    M3DState* state = m3d_get_state(obj);
    if (!state) return NATIVE_RETURN_VOID();

    switch (state->matrix_mode) {
        case M3D_MODE_PROJECTION:
            if (state->pr_sp > 0) {
                state->pr_sp--;
                m3d_clone(state->projm, state->pr_stack[state->pr_sp]);
            }
            break;
        case M3D_MODE_TEXTURE:
            if (state->tx_sp > 0) {
                state->tx_sp--;
                m3d_clone(state->texmat, state->tx_stack[state->tx_sp]);
            }
            break;
        default:
            if (state->mv_sp > 0) {
                state->mv_sp--;
                m3d_clone(state->matrix, state->mv_stack[state->mv_sp]);
            }
            break;
    }

    return NATIVE_RETURN_VOID();
}

/*
 * M3D.frustumxi(IIIIII)V
 * Values are fixed-point 16.16. Argument order preserved EXACTLY from the
 * FreeJ2ME port (verified against shipping games): r, l, t, b, n, f.
 */
static JavaValue native_m3d_frustumxi(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;

    JavaObject* obj = (JavaObject*)args[0].ref;
    M3DState* state = m3d_get_state(obj);
    if (!state) return NATIVE_RETURN_VOID();

    float r = args[1].i / 65536.0f;
    float l = args[2].i / 65536.0f;
    float t = args[3].i / 65536.0f;
    float b = args[4].i / 65536.0f;
    float n = args[5].i / 65536.0f;
    float f = args[6].i / 65536.0f;

    state->z_near = -n;
    state->z_far  = -f;

    m3d_projection(state->projm, r - l, t - b, n, f);

    return NATIVE_RETURN_VOID();
}

/*
 * M3D.scalexi(III)V - applies to the current matrix target
 */
static JavaValue native_m3d_scalexi(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;

    JavaObject* obj = (JavaObject*)args[0].ref;
    M3DState* state = m3d_get_state(obj);
    if (!state) return NATIVE_RETURN_VOID();

    float sx = args[1].i / 65536.0f;
    float sy = args[2].i / 65536.0f;
    float sz = args[3].i / 65536.0f;

    float scale[16] = {
        sx, 0.0f, 0.0f, 0.0f,
        0.0f, sy,   0.0f, 0.0f,
        0.0f, 0.0f, sz,   0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    };

    float* m = m3d_current_matrix(state);
    m3d_matmul(scale, m);
    m3d_clone(m, scale);

    return NATIVE_RETURN_VOID();
}

/*
 * M3D.translatexi(III)V - applies to the current matrix target
 */
static JavaValue native_m3d_translatexi(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;

    JavaObject* obj = (JavaObject*)args[0].ref;
    M3DState* state = m3d_get_state(obj);
    if (!state) return NATIVE_RETURN_VOID();

    float tx = args[1].i / 65536.0f;
    float ty = args[2].i / 65536.0f;
    float tz = args[3].i / 65536.0f;

    float trans[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        tx,   ty,   tz,   1.0f
    };

    float* m = m3d_current_matrix(state);
    m3d_matmul(trans, m);
    m3d_clone(m, trans);

    return NATIVE_RETURN_VOID();
}

/*
 * M3D.rotatexi(IIII)V - v17: arbitrary axis via Rodrigues formula.
 * Handedness matches the legacy single-axis matrices (verified: the
 * axis=(1,0,0)/(0,1,0)/(0,0,1) cases reproduce the old matrices exactly).
 */
static JavaValue native_m3d_rotatexi(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;

    JavaObject* obj = (JavaObject*)args[0].ref;
    M3DState* state = m3d_get_state(obj);
    if (!state) return NATIVE_RETURN_VOID();

    float angle = (args[1].i / 65536.0f) * DEG2RAD;
    float ax = (float)args[2].i;
    float ay = (float)args[3].i;
    float az = (float)args[4].i;

    float rotm[16];
    memset(rotm, 0, sizeof(rotm));

    float len = sqrtf(ax * ax + ay * ay + az * az);
    if (len < 1e-8f || angle == 0.0f) {
        m3d_identity(rotm);
    } else {
        ax /= len; ay /= len; az /= len;
        float c = cosf(angle);
        float s = sinf(angle);
        float ic = 1.0f - c;

        /* Row-vector convention: v' = v * R
         * R = c*I - s*[a]x + (1-c)*(a a^T), where [a]x w = a x w
         * Rows of (a x e_j): cross(a, e_j) per column j, negated by -s. */
        /* Build explicitly: R[i][j] = c*dij - s*(a x e_j)[i] + ic*a[i]*a[j]
         * a x e0 = (ay*0 - az*0, az*1 - ax*0, ax*0 - ay*1) = (0, az, -ay)
         * a x e1 = (az*0? ...) compute per component:
         * a x e1 = (ay*0 - az*1? no: a x e1 = (a_y*e1_z - a_z*e1_y, a_z*e1_x - a_x*e1_z, a_x*e1_y - a_y*e1_x)
         *        = (0 - az*1?? let's be careful: e1=(0,1,0): a x e1 = (a_y*0 - a_z*1, a_z*0 - a_x*0, a_x*1 - a_y*0) = (-az, 0, ax)
         * e0=(1,0,0): a x e0 = (a_y*0 - a_z*0, a_z*1 - a_x*0, a_x*0 - a_y*1) = (0, az, -ay)
         * e2=(0,0,1): a x e2 = (a_y*1 - a_z*0, a_z*0 - a_x*1, a_x*0 - a_y*0) = (ay, -ax, 0)
         */
        float R[3][3];
        /* Column j of R (row-vector math): v'_i = sum_j v_j * R[i][j] */
        float ex[3][3] = { {1,0,0}, {0,1,0}, {0,0,1} };
        for (int j = 0; j < 3; j++) {
            /* cross(a, e_j) */
            float cx = ay * ex[j][2] - az * ex[j][1];
            float cy = az * ex[j][0] - ax * ex[j][2];
            float cz = ax * ex[j][1] - ay * ex[j][0];
            float aej[3];
            aej[0] = cx; aej[1] = cy; aej[2] = cz;
            float av[3] = { ax, ay, az };
            for (int i = 0; i < 3; i++) {
                float dij = (i == j) ? c : 0.0f;
                float aai = av[i] * av[j];
                R[i][j] = dij - s * aej[i] + ic * aai;
            }
        }

        rotm[0]  = R[0][0]; rotm[4]  = R[0][1]; rotm[8]  = R[0][2]; rotm[12] = 0.0f;
        rotm[1]  = R[1][0]; rotm[5]  = R[1][1]; rotm[9]  = R[1][2]; rotm[13] = 0.0f;
        rotm[2]  = R[2][0]; rotm[6]  = R[2][1]; rotm[10] = R[2][2]; rotm[14] = 0.0f;
        rotm[3]  = 0.0f;    rotm[7]  = 0.0f;    rotm[11] = 0.0f;    rotm[15] = 1.0f;
    }

    float* m = m3d_current_matrix(state);
    m3d_matmul(rotm, m);
    m3d_clone(m, rotm);

    return NATIVE_RETURN_VOID();
}

/*
 * M3D.color4ub(BBBB)V - alpha is KEPT (v17) and used for blending
 */
static JavaValue native_m3d_color4ub(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;

    JavaObject* obj = (JavaObject*)args[0].ref;
    M3DState* state = m3d_get_state(obj);
    if (!state) return NATIVE_RETURN_VOID();

    unsigned int r = (unsigned int)(args[1].i & 0xFF);
    unsigned int g = (unsigned int)(args[2].i & 0xFF);
    unsigned int b = (unsigned int)(args[3].i & 0xFF);
    unsigned int a = (unsigned int)(args[4].i & 0xFF);

    state->color = (a << 24) | (r << 16) | (g << 8) | b;

    return NATIVE_RETURN_VOID();
}

/*
 * M3D.clearColor4ub(BBBB)V
 */
static JavaValue native_m3d_clearColor4ub(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;

    JavaObject* obj = (JavaObject*)args[0].ref;
    M3DState* state = m3d_get_state(obj);
    if (!state) return NATIVE_RETURN_VOID();

    unsigned int r = (unsigned int)(args[1].i & 0xFF);
    unsigned int g = (unsigned int)(args[2].i & 0xFF);
    unsigned int b = (unsigned int)(args[3].i & 0xFF);
    unsigned int a = (unsigned int)(args[4].i & 0xFF);

    state->clearcolor = (a << 24) | (r << 16) | (g << 8) | b;

    return NATIVE_RETURN_VOID();
}

/*
 * M3D.vertexPointerub(II[B)V - v17: size & stride honored, raw storage
 */
static JavaValue native_m3d_vertexPointerub(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;

    JavaObject* obj = (JavaObject*)args[0].ref;
    M3DState* state = m3d_get_state(obj);
    if (!state) return NATIVE_RETURN_VOID();

    jint size = args[1].i;    /* components per vertex (3 or 4) */
    jint stride = args[2].i;  /* byte stride, 0 = tightly packed */
    JavaArray* vertex_array = (JavaArray*)args[3].ref;
    if (!vertex_array) { state->vertCount = 0; return NATIVE_RETURN_VOID(); }
    if (size < 2 || size > 4) size = 3;

    jbyte* vdata = (jbyte*)array_data(vertex_array);
    int len = (int)vertex_array->length;
    int eff_stride = (stride > 0) ? stride : size; /* bytes per vertex */

    int vert_floats = 0;
    for (int off = 0; off + size <= len && vert_floats + size <= M3D_MAX_VERT_FLOATS;
         off += eff_stride) {
        for (int c = 0; c < size; c++) {
            state->verts[vert_floats++] = (float)vdata[off + c];
        }
        /* Vertices supplied as 4 components: drop w (M3D xyz convention) */
        if (vert_floats + 3 > M3D_MAX_VERT_FLOATS) break;
    }

    state->vertCount = vert_floats;
    state->vp_size = size;
    state->vp_stride = stride;

    return NATIVE_RETURN_VOID();
}

/*
 * M3D.texCoordPointerub(II[B)V - v17: size & stride honored, raw storage
 */
static JavaValue native_m3d_texCoordPointerub(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;

    JavaObject* obj = (JavaObject*)args[0].ref;
    M3DState* state = m3d_get_state(obj);
    if (!state) return NATIVE_RETURN_VOID();

    jint size = args[1].i;    /* 2 for (u,v) */
    jint stride = args[2].i;
    JavaArray* uv_array = (JavaArray*)args[3].ref;
    if (!uv_array) { state->uvCount = 0; return NATIVE_RETURN_VOID(); }
    if (size < 1 || size > 3) size = 2;

    jbyte* udata = (jbyte*)array_data(uv_array);
    int len = (int)uv_array->length;
    int eff_stride = (stride > 0) ? stride : size;

    int uv_floats = 0;
    for (int off = 0; off + size <= len && uv_floats + 2 <= M3D_MAX_UV_FLOATS;
         off += eff_stride) {
        state->UVs[uv_floats++] = (float)udata[off];
        state->UVs[uv_floats++] = (size >= 2) ? (float)udata[off + 1] : 0.0f;
    }

    state->uvCount = uv_floats;
    state->tp_size = size;
    state->tp_stride = stride;

    return NATIVE_RETURN_VOID();
}

/*
 * Shared: transform raw vertices by the modelview matrix into tmp_xyz,
 * carrying UVs into tmp_uv. Returns the number of VERTICES (not floats).
 */
static int m3d_transform_to_tmp(M3DState* state) {
    int src_floats = state->vertCount;
    /* Vertices are stored as xyz triples */
    int nvert = src_floats / 3;
    if (nvert <= 0) return 0;
    if (src_floats > M3D_MAX_VERT_FLOATS) src_floats = M3D_MAX_VERT_FLOATS;

    const float* m = state->matrix;
    for (int v = 0; v < nvert; v++) {
        float x = state->verts[v * 3];
        float y = state->verts[v * 3 + 1];
        float z = state->verts[v * 3 + 2];
        state->tmp_xyz[v * 3]     = x * m[0] + y * m[4] + z * m[8]  + m[12];
        state->tmp_xyz[v * 3 + 1] = x * m[1] + y * m[5] + z * m[9]  + m[13];
        state->tmp_xyz[v * 3 + 2] = x * m[2] + y * m[6] + z * m[10] + m[14];
    }

    /* Copy UVs (per-vertex pairs) when available */
    int uvs_copied = state->uvCount / 2;
    if (uvs_copied > nvert) uvs_copied = nvert;
    for (int u = 0; u < uvs_copied; u++) {
        state->tmp_uv[u * 2]     = state->UVs[u * 2];
        state->tmp_uv[u * 2 + 1] = state->UVs[u * 2 + 1];
    }
    /* Zero-fill the rest so untextured verts don't sample garbage */
    for (int u = uvs_copied; u < nvert; u++) {
        state->tmp_uv[u * 2] = 0.0f;
        state->tmp_uv[u * 2 + 1] = 0.0f;
    }

    return nvert;
}

/*
 * M3D.drawElementsub(II[B)V
 * args[0] = this, args[1] = mode, args[2] = count, args[3] = byte[] facelist
 */
static JavaValue native_m3d_drawElementsub(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;

    JavaObject* obj = (JavaObject*)args[0].ref;
    M3DState* state = m3d_get_state(obj);
    if (!state || !state->gc) return NATIVE_RETURN_VOID();

    JavaArray* face_array = (JavaArray*)args[3].ref;
    if (!face_array) return NATIVE_RETURN_VOID();

    jbyte* fdata = (jbyte*)array_data(face_array);
    int flen = (int)face_array->length;

    int nvert = m3d_transform_to_tmp(state);
    if (nvert <= 0) return NATIVE_RETURN_VOID();

    /* Convert index list to triples */
    int ntri = flen / 3;
    if (ntri <= 0) return NATIVE_RETURN_VOID();

    unsigned char* idx = (unsigned char*)malloc((size_t)ntri * 3);
    if (!idx) return NATIVE_RETURN_VOID();
    for (int i = 0; i < ntri * 3; i++) {
        idx[i] = (unsigned char)(fdata[i] & 0xFF);
        if (idx[i] >= nvert) idx[i] = 0; /* clamp out-of-range indices */
    }

    m3d_render_triangles(state, state->tmp_xyz, state->tmp_uv, idx, ntri);

    free(idx);
    return NATIVE_RETURN_VOID();
}

/*
 * M3D.drawArrays(III)V - v17: real immediate-mode rendering.
 * args[0] = this, args[1] = mode, args[2] = first, args[3] = count
 */
static JavaValue native_m3d_drawArrays(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;

    JavaObject* obj = (JavaObject*)args[0].ref;
    M3DState* state = m3d_get_state(obj);
    if (!state || !state->gc) return NATIVE_RETURN_VOID();

    jint mode = args[1].i;
    jint first = args[2].i;
    jint count = args[3].i;
    if (count <= 0 || first < 0) return NATIVE_RETURN_VOID();

    int nvert = m3d_transform_to_tmp(state);
    if (nvert <= 0) return NATIVE_RETURN_VOID();
    if (first >= nvert) return NATIVE_RETURN_VOID();
    if (first + count > nvert) count = nvert - first;

    const float* xyz = state->tmp_xyz + first * 3;
    const float* uv = state->tmp_uv + first * 2;

    switch (mode) {
        case M3D_TRIANGLES: {
            m3d_render_triangles(state, xyz, uv, NULL, count / 3);
            break;
        }
        case M3D_TRIANGLE_STRIP: {
            int ntri = count - 2;
            if (ntri > 0) {
                /* Expand strip to triangle list in scratch space:
                 * build a de-indexed xyz/uv copy per triangle. */
                int max_tris = ntri;
                float* txyz = (float*)malloc(sizeof(float) * (size_t)max_tris * 9);
                float* tuv = (float*)malloc(sizeof(float) * (size_t)max_tris * 6);
                if (txyz && tuv) {
                    for (int t = 0; t < ntri; t++) {
                        int i0 = t, i1 = t + 1, i2 = t + 2;
                        if (t & 1) { int tmpi = i1; i1 = i2; i2 = tmpi; } /* strip winding */
                        for (int c = 0; c < 3; c++) {
                            txyz[t * 9 + 0 + c] = xyz[i0 * 3 + c];
                            txyz[t * 9 + 3 + c] = xyz[i1 * 3 + c];
                            txyz[t * 9 + 6 + c] = xyz[i2 * 3 + c];
                        }
                        tuv[t * 6 + 0] = uv[i0 * 2];
                        tuv[t * 6 + 1] = uv[i0 * 2 + 1];
                        tuv[t * 6 + 2] = uv[i1 * 2];
                        tuv[t * 6 + 3] = uv[i1 * 2 + 1];
                        tuv[t * 6 + 4] = uv[i2 * 2];
                        tuv[t * 6 + 5] = uv[i2 * 2 + 1];
                    }
                    m3d_render_triangles(state, txyz, tuv, NULL, ntri);
                }
                free(txyz);
                free(tuv);
            }
            break;
        }
        case M3D_TRIANGLE_FAN: {
            int ntri = count - 2;
            if (ntri > 0) {
                float* txyz = (float*)malloc(sizeof(float) * (size_t)ntri * 9);
                float* tuv = (float*)malloc(sizeof(float) * (size_t)ntri * 6);
                if (txyz && tuv) {
                    for (int t = 0; t < ntri; t++) {
                        int i0 = 0, i1 = t + 1, i2 = t + 2;
                        for (int c = 0; c < 3; c++) {
                            txyz[t * 9 + 0 + c] = xyz[i0 * 3 + c];
                            txyz[t * 9 + 3 + c] = xyz[i1 * 3 + c];
                            txyz[t * 9 + 6 + c] = xyz[i2 * 3 + c];
                        }
                        tuv[t * 6 + 0] = uv[i0 * 2];
                        tuv[t * 6 + 1] = uv[i0 * 2 + 1];
                        tuv[t * 6 + 2] = uv[i1 * 2];
                        tuv[t * 6 + 3] = uv[i1 * 2 + 1];
                        tuv[t * 6 + 4] = uv[i2 * 2];
                        tuv[t * 6 + 5] = uv[i2 * 2 + 1];
                    }
                    m3d_render_triangles(state, txyz, tuv, NULL, ntri);
                }
                free(txyz);
                free(tuv);
            }
            break;
        }
        case M3D_LINES: {
            for (int l = 0; l + 1 < count; l += 2) {
                m3d_draw_line(state, xyz[l * 3], xyz[l * 3 + 1], xyz[(l + 1) * 3], xyz[(l + 1) * 3 + 1]);
            }
            break;
        }
        case M3D_LINE_STRIP:
        case M3D_LINE_LOOP: {
            for (int l = 0; l + 1 < count; l++) {
                m3d_draw_line(state, xyz[l * 3], xyz[l * 3 + 1], xyz[(l + 1) * 3], xyz[(l + 1) * 3 + 1]);
            }
            if (mode == M3D_LINE_LOOP && count > 2) {
                m3d_draw_line(state, xyz[(count - 1) * 3], xyz[(count - 1) * 3 + 1],
                                    xyz[0], xyz[1]);
            }
            break;
        }
        case M3D_POINTS:
        default: {
            /* Points: 1x1 pixels via a degenerate fill (fast enough for UI) */
            for (int p = 0; p < count; p++) {
                float px = xyz[p * 3] * state->vp_w * 0.5f + state->vp_x + state->vp_w * 0.5f;
                float py = xyz[p * 3 + 1] * state->vp_h * 0.5f + state->vp_y + state->vp_h * 0.5f;
                int ix = m3d_float_to_int(px);
                int iy = m3d_float_to_int(py);
                if (ix >= 0 && ix < state->width && iy >= 0 && iy < state->height) {
                    state->framebuffer->pixels[iy * state->width + ix] = state->color | 0xFF000000u;
                }
            }
            break;
        }
    }

    return NATIVE_RETURN_VOID();
}

/* ---- v17: GL state implemented ---- */

/* M3D.viewport(IIII)V - output region within the framebuffer */
static JavaValue native_m3d_viewport(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;

    JavaObject* obj = (JavaObject*)args[0].ref;
    M3DState* state = m3d_get_state(obj);
    if (!state) return NATIVE_RETURN_VOID();

    jint x = args[1].i;
    jint y = args[2].i;
    jint w = args[3].i;
    jint h = args[4].i;

    if (w <= 0 || h <= 0) return NATIVE_RETURN_VOID();

    state->vp_x = x;
    state->vp_y = y;
    state->vp_w = w;
    state->vp_h = h;

    return NATIVE_RETURN_VOID();
}

/* M3D.cullFace(I)V - select front/back for GL_CULL_FACE */
static JavaValue native_m3d_cullFace(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;

    JavaObject* obj = (JavaObject*)args[0].ref;
    M3DState* state = m3d_get_state(obj);
    if (!state) return NATIVE_RETURN_VOID();

    jint mode = args[1].i;
    if (mode == M3D_GL_FRONT || mode == M3D_GL_BACK || mode == M3D_GL_FRONT_AND_BACK) {
        state->cull_mode = mode;
    }

    return NATIVE_RETURN_VOID();
}

/* M3D.matrixMode(I)V - select MODELVIEW / PROJECTION / TEXTURE */
static JavaValue native_m3d_matrixMode(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;

    JavaObject* obj = (JavaObject*)args[0].ref;
    M3DState* state = m3d_get_state(obj);
    if (!state) return NATIVE_RETURN_VOID();

    jint mode = args[1].i;
    if (mode == M3D_MODE_MODELVIEW || mode == M3D_MODE_PROJECTION || mode == M3D_MODE_TEXTURE) {
        state->matrix_mode = mode;
    }

    return NATIVE_RETURN_VOID();
}

/* M3D.enableClientState(I)V / disableClientState(I)V
 * Client arrays (vertex/texture coord) are always resident in our
 * software pipeline; the calls validate and accept. */
static JavaValue native_m3d_enableClientState(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    return NATIVE_RETURN_VOID();
}

static JavaValue native_m3d_disableClientState(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    return NATIVE_RETURN_VOID();
}

/* M3D.enable(I)V / disable(I)V - CULL_FACE, TEXTURE_2D, BLEND, ALPHA_TEST */
static JavaValue native_m3d_enable(JVM* jvm, JavaThread* thread,
                                   JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;

    JavaObject* obj = (JavaObject*)args[0].ref;
    M3DState* state = m3d_get_state(obj);
    if (!state) return NATIVE_RETURN_VOID();

    jint cap = args[1].i;
    switch (cap) {
        case M3D_GL_CULL_FACE:  state->enable_cull = true; break;
        case M3D_GL_BLEND:      state->enable_blend = true; break;
        case M3D_GL_ALPHA_TEST: state->enable_alpha_test = true; break;
        case M3D_GL_TEXTURE_2D:
            state->enable_texture2d = true;
            state->explicit_tex_off = false;
            break;
        default: /* LIGHTING / DEPTH_TEST / FOG: no software equivalent yet */
            break;
    }

    return NATIVE_RETURN_VOID();
}

static JavaValue native_m3d_disable(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;

    JavaObject* obj = (JavaObject*)args[0].ref;
    M3DState* state = m3d_get_state(obj);
    if (!state) return NATIVE_RETURN_VOID();

    jint cap = args[1].i;
    switch (cap) {
        case M3D_GL_CULL_FACE:  state->enable_cull = false; break;
        case M3D_GL_BLEND:      state->enable_blend = false; break;
        case M3D_GL_ALPHA_TEST: state->enable_alpha_test = false; break;
        case M3D_GL_TEXTURE_2D:
            state->enable_texture2d = false;
            state->explicit_tex_off = true;
            break;
        default:
            break;
    }

    return NATIVE_RETURN_VOID();
}

/* ========================================================================= */
/* Registration                                                               */
/* ========================================================================= */

/**
 * Register all Nokia M3D native method implementations.
 *
 * IMPORTANT: This function registers the real handlers. If init_nokia_misc()
 * in native.c has already registered NULL stubs for M3D methods, this function
 * must be called BEFORE init_nokia_misc() so that native_find() discovers
 * the real handlers first (it returns the first matching entry).
 */
void init_nokia_m3d_impl(JVM* jvm) {
    static const NativeMethodEntry m3d_methods[] = {
        /* Instance management */
        {"com/nokia/mid/m3d/M3D", "createInstance",
         "()Lcom/nokia/mid/m3d/M3D;", native_m3d_createInstance},
        {"com/nokia/mid/m3d/M3D", "setupBuffers",
         "(III)V", native_m3d_setupBuffers},
        {"com/nokia/mid/m3d/M3D", "removeBuffers",
         "()V", native_m3d_removeBuffers},
        {"com/nokia/mid/m3d/M3D", "clear",
         "(I)V", native_m3d_clear},

        /* Rendering output */
        {"com/nokia/mid/m3d/M3D", "blit",
         "(Ljavax/microedition/lcdui/Graphics;IIII)V", native_m3d_blit},

        /* Texture binding (real texturing since v17) */
        {"com/nokia/mid/m3d/M3D", "bindTexture",
         "(ILcom/nokia/mid/m3d/Texture;)V", native_m3d_bindTexture},

        /* Matrix operations */
        {"com/nokia/mid/m3d/M3D", "loadIdentity",
         "()V", native_m3d_loadIdentity},
        {"com/nokia/mid/m3d/M3D", "pushMatrix",
         "()V", native_m3d_pushMatrix},
        {"com/nokia/mid/m3d/M3D", "popMatrix",
         "()V", native_m3d_popMatrix},
        {"com/nokia/mid/m3d/M3D", "frustumxi",
         "(IIIIII)V", native_m3d_frustumxi},
        {"com/nokia/mid/m3d/M3D", "scalexi",
         "(III)V", native_m3d_scalexi},
        {"com/nokia/mid/m3d/M3D", "translatexi",
         "(III)V", native_m3d_translatexi},
        {"com/nokia/mid/m3d/M3D", "rotatexi",
         "(IIII)V", native_m3d_rotatexi},

        /* Drawing state */
        {"com/nokia/mid/m3d/M3D", "color4ub",
         "(BBBB)V", native_m3d_color4ub},
        {"com/nokia/mid/m3d/M3D", "clearColor4ub",
         "(BBBB)V", native_m3d_clearColor4ub},

        /* Vertex and index data */
        {"com/nokia/mid/m3d/M3D", "vertexPointerub",
         "(II[B)V", native_m3d_vertexPointerub},
        {"com/nokia/mid/m3d/M3D", "texCoordPointerub",
         "(II[B)V", native_m3d_texCoordPointerub},

        /* Drawing commands */
        {"com/nokia/mid/m3d/M3D", "drawElementsub",
         "(II[B)V", native_m3d_drawElementsub},
        {"com/nokia/mid/m3d/M3D", "drawArrays",
         "(III)V", native_m3d_drawArrays},

        /* GL state (implemented since v17) */
        {"com/nokia/mid/m3d/M3D", "viewport",
         "(IIII)V", native_m3d_viewport},
        {"com/nokia/mid/m3d/M3D", "cullFace",
         "(I)V", native_m3d_cullFace},
        {"com/nokia/mid/m3d/M3D", "matrixMode",
         "(I)V", native_m3d_matrixMode},
        {"com/nokia/mid/m3d/M3D", "enableClientState",
         "(I)V", native_m3d_enableClientState},
        {"com/nokia/mid/m3d/M3D", "disableClientState",
         "(I)V", native_m3d_disableClientState},
        {"com/nokia/mid/m3d/M3D", "enable",
         "(I)V", native_m3d_enable},
        {"com/nokia/mid/m3d/M3D", "disable",
         "(I)V", native_m3d_disable},
    };

    native_register_methods(jvm, m3d_methods,
                            sizeof(m3d_methods) / sizeof(m3d_methods[0]));
}
