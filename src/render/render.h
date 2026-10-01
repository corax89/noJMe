/* ============================================================================
 * noJME render module — public interface (v34.20)
 *
 * ALL pixel-level rendering lives in src/render/render.c:
 *   - MIDP 2D primitives (lines, rects, triangles, arcs, glyphs, blits)
 *   - M3G (JSR-184) 3D software rasterizer (clip -> screen -> span -> pixel)
 *
 * This header also hosts the M3G data model shared between the scene-graph
 * side (midp/mobile3d.c) and the rasterizer side (render.c): the transform,
 * vertex, light, material, texture, appearance and context structures, plus
 * the shared render state (g_m3g) and raster diagnostic counters.
 *
 * The split is deliberate: render.c is the ONE file to hand-optimize
 * (SIMD/NEON, span batching, cache blocking) without touching JVM or
 * scene-graph code.
 * ============================================================================ */

#ifndef NOJME_RENDER_H
#define NOJME_RENDER_H

#include <stdint.h>
#include <stddef.h>
#include <math.h>

#include "jvm.h"     /* JavaObject, JavaArray */
#include "midp.h"    /* MidpGraphics, MidpImage, jint */

/* ARM-specific optimization hints for hot-path functions */
#if defined(__ARM_ARCH_7A__) || defined(__arm__)
#define M3G_INLINE __attribute__((always_inline)) static inline
#define M3G_HOT __attribute__((hot))
#else
#define M3G_INLINE static inline
#define M3G_HOT
#endif

/* ---------------- M3G data model (moved from mobile3d.c v34.20) ------- */

/* Maximum dimensions for Image2D and render targets to prevent OOM.
 * J2ME M3G typically supports 256x256 or 512x512 textures.
 * Allow up to 1024x1024 for compatibility with newer devices. */
#define M3G_MAX_IMAGE_DIMENSION 1024
#define M3G_MAX_IMAGE_PIXELS (M3G_MAX_IMAGE_DIMENSION * M3G_MAX_IMAGE_DIMENSION)

/* Maximum vertex count for VertexArray to prevent OOM */
#define M3G_MAX_VERTEX_COUNT 65536
#define M3G_MAX_INDEX_COUNT  65536

/* ============================================================================
 * M3G Internal Data Structures
 * ============================================================================ */

/* 4x4 Transformation Matrix (column-major, OpenGL style) */
typedef struct {
    float m[16];  /* Column-major: m[col*4+row] */
} M3GTransform;

/* Vertex data */
typedef struct {
    float* positions;     /* XYZ positions */
    float* normals;       /* XYZ normals (optional) */
    float* texcoords;     /* UV coordinates (optional) */
    uint8_t* colors;      /* RGBA colors (optional) */
    int vertex_count;
    int vertex_stride;    /* Components per vertex */
} M3GVertexArray;

/* Triangle indices */
typedef struct {
    uint16_t* indices;
    int index_count;
    int primitive_type;   /* TRIANGLES=4, LINES=3, POINTS=2 */
} M3GIndexBuffer;

/* Material properties */
typedef struct {
    float ambient[4];     /* RGBA */
    float diffuse[4];
    float specular[4];
    float emissive[4];
    float shininess;
    /* v34.18 (audit item 2.4): PolygonMode.setLocalCameraLightingEnable —
     * when set, the specular view vector is the actual vertex->camera
     * direction (view space: -position) instead of the infinite-viewer
     * approximation V=(0,0,1). */
    int local_camera;
} M3GMaterial;

/* Texture data */
typedef struct {
    uint32_t* pixels;     /* ARGB pixels */
    int width;
    int height;
    int blend_s, blend_t; /* Wrapping mode */
    int filter_level;
    /* v14: cache-refresh bookkeeping — the source Image2D content can change
     * at any time (Image2D.set, releaseTarget into an Image2D target), so
     * each built texture remembers where it came from and the global
     * "image content generation" it was built at. */
    JavaObject* source_image;
    uint32_t generation;
    int blend_color;      /* Texture2D.setBlendColor for FUNC_BLEND (ARGB) */
    int has_uv_transform; /* Texture2D.setTransform applied */
    float uv_transform[16]; /* row-major 4x4 from Texture2D.getTransform */
} M3GTexture2D;

/* Light source */
typedef struct {
    JavaObject* obj;     /* Java Light object that owns this slot */
    int type;             /* AMBIENT, DIRECTIONAL, OMNI, SPOT (internal 1/2/4/8) */
    float color[4];       /* RGBA intensity */
    float direction[4];   /* Local direction (Light.setDirection / explicit dir) */
    float position[4];    /* For omni/spot */
    float attenuation[3]; /* Constant, linear, quadratic */
    float spot_angle;     /* Spot cone angle (degrees) */
    float spot_exponent;
    /* Task 16-a (lighting): per-render evaluated view-space state plus the
     * immediate-mode transform captured by Graphics3D.addLight/setLight. */
    M3GTransform transform; /* World transform given to addLight (immediate mode) */
    int has_transform;      /* 1 = transform valid */
    int g3d_registered;     /* 1 = added via Graphics3D.addLight/setLight */
    float view_pos[3];      /* Evaluated view-space position (omni/spot) */
    float view_dir[3];      /* Evaluated view-space direction (directional/spot) */
} M3GLight;

/* Appearance state */
typedef struct {
    M3GMaterial* material;
    M3GTexture2D* texture;
    int compositing_mode;
    int polygon_mode;
    int layer;
    int winding;              /* 0 = CCW (default), 1 = CW */
    int two_sided_lighting;   /* 0 = one-sided, 1 = two-sided (no culling) */
    int cull_front;           /* 1 = CULL_FRONT (cull front faces), 0 = CULL_BACK */
    int cull_flip;            /* v34.88 self-heal: invert the facing decision
                              * (meshes authored for the opposite winding
                              * convention — Brick Breaker Revolution 3D) */
    int blend_mode;           /* CompositingMode blending, -1 = use default */
    int alpha_threshold;      /* CompositingMode alphaThreshold * 255, -1 = use default */
    int texture_blend;        /* Texture2D blending mode */
    /* Task 16-a: fog / write masks / alpha factor / second texture unit.
     * All defaults keep the previous (v22) unlit, no-fog behavior bit-for-bit. */
    int fog_enabled;          /* 0 = no Fog on this Appearance (default) */
    int fog_mode;             /* internal: 0 = EXPONENTIAL, 1 = LINEAR */
    float fog_density;
    float fog_near;
    float fog_far;
    int fog_color;            /* ARGB */
    int alpha_write;          /* CompositingMode alphaWriteEnable (default 1) */
    int color_write;          /* CompositingMode colorWriteEnable (default 1) */
    float alpha_factor;       /* Node.alphaFactor (default 1.0) */
    M3GTexture2D* texture1;   /* Texture2D unit 1 (modulate), NULL = disabled */
    /* v30 (JSR-184 §CompositingMode.setDepthOffset): fragment depth bias for
     * decals. Applied as z' = z + factor*max_slope + units/(2^24). */
    float depth_offset_factor;
    float depth_offset_units;
    /* v30: unit-1 texture coordinates (per-vertex, decoded alongside the
     * unit-0 set; NULL disables second-unit sampling). */
    float* tex1_coords;
} M3GAppearance;

/* Mesh object */
typedef struct {
    M3GVertexArray* vertices;
    M3GIndexBuffer* indices;
    M3GAppearance* appearance;
    int submesh_count;
} M3GMesh;

/* Camera */
typedef struct {
    float position[3];
    float look_at[3];
    float up[3];
    float fov;            /* Field of view in degrees */
    float aspect;         /* Width / Height */
    float near_plane;
    float far_plane;
    int projection_type;  /* 0=perspective, 1=parallel */
} M3GCamera;

/* Render buffer pool for object reuse - avoids per-frame allocations */
#define M3G_VERTEX_POOL_SIZE (64 * 1024)  /* 64K floats = ~21K vertices */
#define M3G_INDEX_POOL_SIZE (32 * 1024)   /* 32K indices */

typedef struct {
    float* vertex_pool;
    size_t vertex_pool_capacity;  /* in floats */
    size_t vertex_pool_used;
    
    uint16_t* index_pool;
    size_t index_pool_capacity;  /* in indices */
    size_t index_pool_used;
    
    int initialized;
} M3GRenderPool;

/* Rendering context */
typedef struct {
    /* Viewport */
    int viewport_x, viewport_y;
    int viewport_width, viewport_height;
    
    /* Target buffer */
    uint32_t* color_buffer;
    float* depth_buffer;
    int buffer_width, buffer_height;
    int buffers_allocated;  /* Track if buffers are allocated */
    int depth_buffer_bound;  /* v34.19 (Worms Forts 3D): JSR-184 bindTarget(target, depthBuffer, hints) —
                              * 0 = NO depth buffer allocated: per-mesh CompositingMode depth
                              * test/write MUST be ignored (spec: "depth testing is only in
                              * effect if the target has a depth buffer"). The game binds
                              * Graphics with depthBuffer=false, yet its default CompositingMode
                              * (depthTest=1) re-enabled the test over a calloc-zeroed buffer
                              * (Background has depthClearEnable=0, so clear() is a no-op) —
                             * every fragment failed the LESS test -> pixels=0, no 3D. */
    
    /* Target for releaseTarget */
    JavaObject* target_image;       /* Image2D or Graphics Java object */
    MidpGraphics* target_gfx;       /* Native graphics context when target is Graphics */
    int target_is_graphics;         /* 1 if target is a Graphics object, 0 if Image2D */
    
    /* Current transformation */
    M3GTransform modelview;
    M3GTransform projection;
    M3GTransform mvp;     /* ModelView * Projection */
    
    /* Lights (max 8) */
    M3GLight lights[8];
    int light_count;
    float depth_range_near;  /* v24: Graphics3D.setDepthRange */
    float depth_range_far;   /* default 0.0/1.0 -> identity */
    int render_hints;        /* v34.69: JSR-184 Graphics3D.getHints — the hints
                              * bitmask passed to bindTarget(target, depth, hints)
                              * (0 for the single-arg overloads). Reset per bind. */
    
    /* Global ambient */
    float ambient_light[4];
    
    /* Render states */
    int depth_test_enabled;
    int depth_write_enabled;
    int blending_enabled;
    int culling_enabled;    /* Back-face culling */
    
    /* Render buffer pool */
    M3GRenderPool render_pool;
    
    /* Camera state for setCamera/render(Node,Transform) flow */
    JavaObject* camera;
    M3GTransform camera_transform;     /* Camera's composite transform (world space) */
    M3GTransform camera_inverse;       /* Inverse of camera transform (view matrix) */
    int camera_set;                    /* Flag: camera has been set via setCamera */
    
    /* Statistics */
    int triangles_rendered;
    int vertices_processed;
} M3GContext;

/* Task 16-a: appearance render state — filled in by m3g_read_render_state()
 * (scene side, mobile3d.c) and consumed by the rasterizer (render.c). */
/* Appearance render state - holds per-draw compositing/fog params read from Java objects.
 * v23: moved here from below — m3g_render_single_mesh (fog/wire state) uses it. */
typedef struct {
    int blend_mode;         /* CompositingMode blending */
    int depth_test;         /* CompositingMode depth test enabled */
    int depth_write;        /* CompositingMode depth write enabled */
    float alpha_threshold;  /* CompositingMode alpha threshold 0.0-1.0 */
    int fog_enabled;
    int fog_mode;           /* 0=exponential, 1=linear */
    float fog_density;
    float fog_near;
    float fog_far;
    int fog_color;          /* ARGB */
    int texture_blend;      /* Texture2D blending mode */
    /* Task 16-a: write masks honoured in the pixel output stage (defaults on) */
    int alpha_write;        /* CompositingMode alphaWriteEnable */
    int color_write;        /* CompositingMode colorWriteEnable */
} M3GRenderState;

/* ---------------- shared inline math helpers -------------------------- */

/* JSR-184 Light mode constants (as stored in M3G binary files and Java Light class) */
#define M3G_LIGHT_MODE_AMBIENT       128
#define M3G_LIGHT_MODE_DIRECTIONAL   129
#define M3G_LIGHT_MODE_OMNI          130
#define M3G_LIGHT_MODE_SPOT          131

/* Internal light type constants (used by the rasterizer) */
#define M3G_LIGHT_AMBIENT       1
#define M3G_LIGHT_DIRECTIONAL   2  
#define M3G_LIGHT_OMNI          4
#define M3G_LIGHT_SPOT          8
/* Vector operations (static in header: each TU gets a copy; unused-safe) */
__attribute__((unused))
static void m3g_vec3_normalize(float* v) {
    float len = sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
    if (len > 0.0001f) {
        v[0] /= len; v[1] /= len; v[2] /= len;
    }
}

__attribute__((unused))
static void m3g_vec3_cross(float* result, const float* a, const float* b) {
    result[0] = a[1]*b[2] - a[2]*b[1];
    result[1] = a[2]*b[0] - a[0]*b[2];
    result[2] = a[0]*b[1] - a[1]*b[0];
}

__attribute__((unused))
static float m3g_vec3_dot(const float* a, const float* b) {
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}

/* Fast specular approximation using exp2(log2(x) * y).
 * On ARMv7 without FPU, powf() costs ~50+ cycles calling into libm.
 * exp2f(log2f(x) * y) compiles to ~6 VFP instructions.
 * Returns 0 for non-positive inputs (no negative specular highlights). */
M3G_INLINE float m3g_fast_specular(float ndh, float shininess) {
    if (ndh <= 0.0f) return 0.0f;
    if (shininess <= 1.0f) return ndh;
    /* Use exp2(log2(x) * y) — much cheaper than powf on ARM */
    return exp2f(log2f(ndh) * shininess);
}

/* ---------------- shared render state --------------------------------- */

extern M3GContext g_m3g;

/* Task 16-a (lighting): per-render light list evaluated during scene
 * traversal (scene side writes, rasterizer reads). */
#define M3G_MAX_ACTIVE_LIGHTS 8
extern M3GLight g_m3g_active_lights[M3G_MAX_ACTIVE_LIGHTS];
extern int g_m3g_active_light_count;

/* v34.23: 2D draw-op counter for the force-render heuristic (see render.c) */
extern volatile int g_midp_gfx_draw_ops;

/* Raster diagnostics (defined in render.c; reset/printed by mobile3d.c) */
extern int g_m3g_clip_triangles_in;
extern int g_m3g_clip_triangles_out;
extern int g_m3g_raster_pixels_written;
extern long g_m3g_rj_clip2screen, g_m3g_rj_depth, g_m3g_rj_cull, g_m3g_rj_offscreen, g_m3g_rj_area, g_m3g_rj_pass;
extern int g_m3g_rast_trace_on;
extern int g_m3g_rast_acc_x0, g_m3g_rast_acc_x1;
extern int g_m3g_rast_acc_y0, g_m3g_rast_acc_y1;

/* ---------------- 3D rasterizer API (implemented in render.c) --------- */

/* Clear color/depth target buffers honoring Background clear enables. */
M3G_HOT void m3g_clear_ex(float r, float g, float b, float a, float depth,
                          int clear_color, int clear_depth);

/* Sutherland-Hodgman clip against the full 7-plane frustum. Returns the
 * number of output triangles. v34.20: signature uses the real 2D-array
 * out-params (caller allocates M3G_CLIP_MAX_TRIS-sized buffers). */
#define M3G_CLIP_MAX_POLY 10
#define M3G_CLIP_MAX_TRIS (M3G_CLIP_MAX_POLY - 2)
int m3g_clip_triangle_near(const float* in_clip[3], const uint8_t* in_color[3], const float* in_tex[3],
                           float out_clip[M3G_CLIP_MAX_TRIS * 3][4], uint8_t out_color[M3G_CLIP_MAX_TRIS * 3][4],
                           float out_tex[M3G_CLIP_MAX_TRIS * 3][2],
                           const float* in_tex1[3], float out_tex1[M3G_CLIP_MAX_TRIS * 3][2],
                           const float* in_fog[3], float out_fog[M3G_CLIP_MAX_TRIS * 3]);

/* Gouraud per-vertex lighting (JSR-184 Light/Material semantics). */
void m3g_light_vertex(float* result, const float* position,
                      const float* normal, const M3GMaterial* mat,
                      int two_sided);

/* Scanline triangle rasterizer — the heart of the 3D pipeline. */
M3G_HOT void m3g_rasterize_triangle(const float* v0, const float* v1, const float* v2,
                                    const float* t0, const float* t1, const float* t2,
                                    const float* t1_0, const float* t1_1, const float* t1_2,
                                    const uint8_t* c0, const uint8_t* c1, const uint8_t* c2,
                                    M3GAppearance* appearance,
                                    const float* fog_d0, const float* fog_d1, const float* fog_d2);

/* ---------------- v34.42 per-mesh raster context ----------------------- */

/* gprof (Asphalt 3 3D in-race, 60 s): m3g_rasterize_triangle showed 35% of
 * total CPU as SELF time — the per-triangle fixed overhead (render-state
 * memset+reads, blend-mode mapping, NEON-eligibility re-check with ~15
 * branches, getenv(NOJME_NEON_STATS) PER TRIANGLE, buffer/viewports
 * re-loaded) is per-MESH constant, not per-triangle. M3GRastCtx hoists it:
 * m3g_raster_begin_mesh() resolves everything once per mesh; the triangle
 * loop then feeds m3g_rasterize_triangle_ctx(). m3g_rasterize_triangle()
 * (above) remains as a build-ctx-and-call wrapper for the self-test and
 * other callers — bit-identical by construction (same values, computed
 * once). */
typedef struct M3GRastCtx {
    /* resolved render state (was: re-assembled per triangle) */
    int      culling_enabled;     /* g_m3g.culling_enabled snapshot */
    int      depth_test;
    int      depth_write;
    int      blend_mode;          /* post blend_map canonicalization */
    int      texture_blend;
    float    alpha_threshold;     /* 0..1 */
    float    alpha_factor;
    float    z_offset_units_part; /* depthOffsetUnits * (1/2^24) */
    float    depth_offset_factor;
    int      two_sided_lighting;  /* from appearance */
    int      winding;             /* 0=CCW, 1=CW */
    int      cull_front;          /* 1=CULL_FRONT */
    int      cull_flip;           /* v34.88 self-heal facing inversion */
    /* fog (resolved from appearance) */
    int      has_fog;
    int      fog_mode;
    float    fog_density, fog_near, fog_far;
    int      fog_color;
    /* per-mesh texture-unit presence (t0/t1_0 NULLness stays per-triangle) */
    int      mesh_has_texture1;   /* appearance->texture1 && tex1_coords */
    /* NEON span eligibility — the appearance/texture-level part; the
     * per-triangle texcoord presence is ANDed in the raster body */
    int      neon_core;           /* 1 = eligible apart from per-tri NULLs */
    M3GTexture2D* neon_tex;
    int      neon_tex_replace;
    /* v34.79 (j2me_texture_smoothing=smooth): the span bilinear sampler is
     * active — the SIMD span replicates m3g_sample_texture_smooth
     * bit-exactly, so smooth-mode meshes stay on the fast span path
     * instead of falling back to the scalar per-pixel loop. */
    int      span_smooth;
    /* frame buffers (per-bind constant) */
    uint32_t* color_buf;
    float*   depth_buf;
    int      stride;              /* buffer_width */
    int      buf_w, buf_h;
    /* v34.54: 3D viewport rect (inclusive bounds) — JSR-184 bindTarget
     * sets the viewport to the bound Graphics' clip; rasterization is
     * clipped to this rect, NOT the raw buffer bounds. Defaults cover
     * the whole buffer (viewport == buffer in that case). */
    int      vp_x0, vp_y0, vp_x1, vp_y1;
    int      rast_trace_on;        /* g_m3g_rast_trace_on snapshot */
    /* appearance back-reference for the per-pixel sampler block */
    M3GAppearance* appearance;
} M3GRastCtx;

/* Resolve the per-mesh raster state. Safe to call multiple times per mesh. */
void m3g_raster_begin_mesh(M3GRastCtx* ctx, M3GAppearance* appearance);

/* Context-fed rasterizer: same math as m3g_rasterize_triangle, minus the
 * per-mesh setup (hoisted into ctx by m3g_raster_begin_mesh). */
M3G_HOT void m3g_rasterize_triangle_ctx(const M3GRastCtx* R,
                                    const float* v0, const float* v1, const float* v2,
                                    const float* t0, const float* t1, const float* t2,
                                    const float* t1_0, const float* t1_1, const float* t1_2,
                                    const uint8_t* c0, const uint8_t* c1, const uint8_t* c2,
                                    const float* fog_d0, const float* fog_d1, const float* fog_d2);

/* ---------------- v34.42 batch vertex transform ----------------------- */

/* Transform N stride-3 float vertices by t (w=1) into stride-4 clip coords.
 * NEON (ARM): one vertex per vmulq_n/vaddq chain — ~7 vector ops instead of
 * 28 scalar FLOPs, with EXACT scalar rounding order
 * ((((m0*x)+m4*y)+m8*z)+m12*1.0): explicit vmulq_n+vaddq, NO vfma/vmla, so
 * every intermediate rounds exactly like m3g_transform_point (bit-identical,
 * verified by the NOJME_NEON_SELFTEST transform section). Falls back to the
 * scalar loop on non-NEON builds and NOJME_NEON_SCALAR=1. */
void m3g_transform_vertices_batch(float* __restrict__ out,
                                  const float* __restrict__ verts,
                                  int count, const M3GTransform* t);

/* ---------------- 2D blit API (implemented in render.c) --------------- */

/* Graphics.drawRGB pixel stage: clip + opaque/alpha blit of an int[] ARGB
 * block into the target MidpGraphics (applies translate + clip).
 * [BT-CRASH-FIX] src_len = number of jint elements in rgb_data; source
 * indices outside [0, src_len) are SKIPPED (out-of-bounds read guard —
 * the game-side drawRGB window may legally exceed the array when the
 * visible clip is smaller; reads past the Java heap arena were a direct
 * Data Abort vector on Switch). */
void midp_blit_rgb(MidpGraphics* gfx, const jint* rgb_data, jint src_len,
                   jint offset, jint scanlength, jint x, jint y, jint width,
                   jint height, jint processAlpha);

/* Full-surface blit (GameCanvas flushGraphics): copies min(src,dst) size. */
void midp_blit_surface(uint32_t* dst, int dst_width, int dst_height,
                       const uint32_t* src, int src_width, int src_height);

/* Region blit (GameCanvas flushGraphics(x,y,w,h)): same-coordinate copy,
 * clamped to both source and destination bounds. */
void midp_blit_surface_region(uint32_t* dst, int dst_width, int dst_height,
                              const uint32_t* src, int src_width, int src_height,
                              int x, int y, int w, int h);

/* ------------- v36.54/v36.55 [M3G-VIEWPORT-FRESH] -----------------------
 * [M3G-COV-OPAQUE] (восстановлено в v36.62 из потерянных v36.53-v36.56):
 * coverage-маска канвы, которую M3G биндит (Graphics3D.bindTarget(Graphics)).
 * Реальные телефоны реализовали JSR-184 поверх OpenGL ES window-surfaces
 * (EGL buffer-destroyed на swap) — каждый bind-цикл стартовал на СВЕЖЕМ
 * регионе; наш софтверный пайплайн рендерит в приватный color-буфер,
 * засеянный содержимым канваса (v25), и бережно возвращал «вчерашнее 3D»
 * каждым releaseTarget-блитом — вечные следы в играх, не очищающих цвет
 * (Galaxy on Fire: colorClearEnable=FALSE, без fillRect в полёте).
 *
 * Механика: fillRect/drawImage/drawRGB (render.c) отмечают накрытые
 * пиксели на БИНДЯЩЕЙСЯ канве (окно «от bind до bind»: пост-release 2D
 * прошлого кадра + пред-bind 2D этого кадра); bindTarget(Graphics)
 * стартует вьюпорт ЧЁРНЫМ (0xFF000000), КРОМЕ отмеченных (они сеются с
 * канваса — семантика v25 «2D-фон переживает bind» для игр, перекраши-
 * вающих фон каждый кадр); ВНЕ вьюпорта — прежний полный посев (рамки/
 * HUD, нарисованные один раз, живут). drawString отметок не ставит
 * (текст гарантированно свеж). Принцип [M3G-COV-OPAQUE]: пиксель
 * подавляет свежую очистку ТОЛЬКО если 2D полностью его детерминирует
 * (alpha==255; полупрозрачный оверлей поверх неопределённой базы —
 * мусор: аддитивный прицел GoF складывался поверх накопленной базы —
 * ореолы v36.54).
 *
 * Рубильники: NOJME_M3G_BIND_FRESH=0 — весь свежий посев (v25);
 * NOJME_M3G_COV_OPAQUE=0 — полная прямоугольная отметка v36.54.
 * Реализация — render.c (маркеры) + mobile3d.c (посев/сброс). */

/* bindTarget(Graphics) вызывается ПОСЛЕ посева: открыть новое окно
 * coverage (очистить маску, запомнить канву по gfx И pixels-буферу).
 * gfx==NULL/не-Graphics-цель — прекратить слежение (маска гаснет). */
void nojme_m3g_cov_window_begin(MidpGraphics* gfx);

/* Маска для канвы gfx (идентичность по gfx + pixels + размерам), либо
 * NULL — нет окна/другая канва/BIND_FRESH выкл (звонивший делает полный
 * посев v25). Буфер остаётся во владении модуля до следующего окна. */
const uint8_t* nojme_m3g_cov_mask_for(MidpGraphics* gfx);

/* Маркеры из 2D-примитивов (canvas-space координаты ПОСЛЕ translate+clip).
 * Безопасны и почти бесплатны, когда окно не открыто (одно сравнение). */
void nojme_m3g_cov_mark_rect(MidpGraphics* gfx, int x, int y, int w, int h);
/* Пер-пиксельный маркер для бленд-путей: отметить x+i,y только там, где
 * (src[i] >> 24) == 0xFF (src-over с srcA==255 заменяет dst). */
void nojme_m3g_cov_mark_span(MidpGraphics* gfx, int x, int y,
                             const uint32_t* src, int n);

/* Режимы (env, парсится один раз): 1 = вкл (дефолт). */
int nojme_m3g_bind_fresh_mode(void);   /* NOJME_M3G_BIND_FRESH (default on) */
int nojme_m3g_cov_opaque_mode(void);   /* NOJME_M3G_COV_OPAQUE (default on) */

/* m3g_session_reset: освободить маску, погасить слежение. */
void nojme_m3g_cov_reset(void);

/* [M3G-2D-SYNC] v36.54: хук на входе КАЖДОГО 2D-примитива (реализация в
 * mobile3d.c). Если этот gfx — канва с ОТКРЫТЫМ bind-окном и 3D уже
 * рендерилось (buffer-ahead), СНАЧАЛА вылить 3D-буфер на канву (хронология
 * железа: 3D ПОД 2D), затем 2D рисуется поверх; иначе пометить канву
 * canvas-ahead (посев на «3D после 2D» и перед финальным блитом). */
void nojme_m3g_2d_enter(MidpGraphics* gfx);


/* ---------------- v34.24 ARM NEON support ------------------------------ */

/* Compiled-in NEON state (0 on x86 / non-NEON targets). The Makefile m17
 * and linux-armv7 targets build with -mfpu=neon-vfpv4, which defines
 * __ARM_NEON__ and compiles the SIMD paths in render.c. */
extern int g_m3g_neon_force_scalar;   /* NOJME_NEON_SCALAR=1: A/B escape hatch */

/* ---------------- v34.79 texture smoothing (j2me_texture_smoothing) ---- */

/* Global M3G texture-filter override, resolved from the libretro core
 * option j2me_texture_smoothing (auto|nearest|smooth) — libretro.c writes
 * the resolved value directly, mirroring g_m3g_neon_force_scalar.
 *   -1 = not yet resolved (headless builds lazily read NOJME_TEXTURE_FILTER)
 *   v36.11: the Switch frontend resolves the global/per-game settings and
 *   writes 0/1/2 here at game start (and live from the PLUS overlay).
 *    0 = auto: per-texture request (v34.78 behavior, bit-identical default)
 *    1 = nearest: force crisp texels everywhere (fastest — SIMD span
 *        eligible for every texture, including LINEAR-requested ones)
 *    2 = smooth: force integer bilinear everywhere (the fast fixed-point
 *        sampler m3g_sample_texture_smooth + the bit-exact NEON/SSE2 span
 *        bilinear — see render.c). */
extern int g_m3g_texture_filter_override;

/* A/B self-test of every vectorized path vs the scalar reference.
 * Run with env NOJME_NEON_SELFTEST=1 (headless main / libretro init).
 * Returns the number of failed checks (0 = all pass). */
int render_neon_selftest(void);

/* ---------------- v34.28 2D pipeline acceleration ---------------------- */

/* Runtime A/B escape hatch for the v34.28 optimized 2D paths
 * (fill_triangle span intervals, drawRegion transforms 1/2/3 row spans,
 * batched glyph rows, hoisted getRGB bounds, RGB565 frame conversion).
 * NOJME_2D_SCALAR=1 forces the original per-pixel algorithms. -1 = lazy. */
extern int g_2d_force_scalar;
int nojme_2d_scalar_forced(void);

/* XRGB8888 -> RGB565 full-frame conversion (NEON on ARM, scalar tail).
 * Bit-identical to ((r&0xF8)<<8)|((g&0xFC)<<3)|(b>>3) per pixel. */
void nojme_convert_xrgb8888_to_rgb565(const uint32_t* restrict src, uint16_t* restrict dst,
                                      int width, int height);

/* 2D micro-benchmark (env NOJME_2D_BENCH=1): times every optimized 2D
 * primitive scalar-vs-fast on a 240x320 screen and reports Mpx/s.
 * Returns 0. */
int nojme_2d_bench(void);

#endif /* NOJME_RENDER_H */
