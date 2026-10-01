/* ============================================================================
 * noJME — MascotCapsule micro3d v3 (com.mascotcapsule.micro3d.v3)
 *
 * Software implementation of the HI Corporation MascotCapsule v3 3D API,
 * rendered through the existing JSR-184 rasterizer (src/render/render.c).
 *
 * Semantics follow the official micro3d v3 javadoc and the reference
 * open-source implementations (JL-Mod by Yury Kharchenko /
 * j2me-preservation/MascotCapsule research):
 *
 *   - Angles: 360 degrees == 4096 units (2*pi == 4096).
 *   - AffineTrans: 3x4 matrix, rotation part in 4.12 fixed point
 *     (4096 == 1.0), translation column m03/m13/m23 in RAW model/world
 *     units. m00..m23 are PUBLIC int fields the game may poke directly.
 *   - mul(a): this = this * a. mul(a1, a2): this = a1 * a2.
 *   - lookAt(pos, look, up): view matrix, camera looks along +Z in view
 *     space; look/up are normalized internally.
 *   - FigureLayout.setPerspective(zNear, zFar, angle): angle in 4096-units
 *     for a full circle (fov), near/far in world units.
 *   - renderPrimitives(texture, x, y, layout, effect, command,
 *     numPrimitives, vertexCoords, normals, textureCoords, colors):
 *     command = PRIMITVE_* | PATTR_* | PDATA_*; quads are 4 verts x (x,y,z)
 *     interleaved; normals per face (3) or per vertex; texcoords are
 *     (u,v) texel pairs; colors 0x00RRGGBB per face/command.
 *   - Texture: 8bpp palettized Windows BMP resource; palette index 0 is
 *     the color-key (transparent) texel; raster flipped to top-down so
 *     texcoord v=0 is the image top.
 *   - Figure: MBAC (MB v3/v4/v5) geometry; T4 quads triangulate as
 *     (a,b,c) + (c,b,d); per-polygon material bits: TRANSPARENT=1,
 *     BLEND_HALF=2, BLEND_ADD=4, BLEND_SUB=6, DOUBLE_FACE=16,
 *     LIGHTING=32, SPECULAR=64.
 *   - Lighting (NORMAL_SHADING): light = min(ambient + dirIntensity *
 *     max(0, dot(normal, -normalize(lightDir))), 1.0), intensities 4096
 *     based (dir may reach 16384).
 *
 * Rendering model: bind(Graphics) seeds the shared rasterizer context
 * (g_m3g) from the bound Graphics; renderPrimitives/renderFigure draw
 * immediately into the offscreen buffer; flush() blits the buffer to the
 * Graphics; release(Graphics) unbinds.  Semi-transparent primitives
 * (PATTR_BLEND_HALF/ADD/SUB) neither depth-test nor depth-write (classic
 * painter ordering); opaque and color-keyed primitives depth-test+write.
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#include "jvm.h"
#include "native.h"
#include "heap.h"
#include "opcodes.h"    /* T_* array type constants */
#include "midp.h"       /* load_jar_resource, MidpGraphics */
#include "sdl_backend.h" /* get_graphics_from_object */
#include "render/render.h" /* g_m3g + m3g_rasterize_triangle */

#define MC_TAG "[MC3D] "

/* ----------------------------------------------------------------------------
 * micro3d v3 constants (Graphics3D javadoc values)
 * -------------------------------------------------------------------------- */
#define MC_PRIMITVE_POINTS          0x01000000
#define MC_PRIMITVE_LINES           0x02000000
#define MC_PRIMITVE_TRIANGLES       0x03000000
#define MC_PRIMITVE_QUADS           0x04000000
#define MC_PRIMITVE_POINT_SPRITES   0x05000000
#define MC_PRIMITVE_TYPE_MASK       0x07000000

#define MC_PATTR_LIGHTING           1
#define MC_PATTR_SPHERE_MAP         2
#define MC_PATTR_COLORKEY           16
#define MC_PATTR_BLEND_HALF         32
#define MC_PATTR_BLEND_ADD          64
#define MC_PATTR_BLEND_SUB          96
#define MC_PATTR_BLEND_MASK         96

#define MC_PDATA_COLOR_NONE            0
#define MC_PDATA_COLOR_PER_COMMAND     1024
#define MC_PDATA_COLOR_PER_FACE        2048
#define MC_PDATA_COLOR_MASK            0x0C00 /* v34.32: official mask (0x400|0x800) */
#define MC_PDATA_NORMAL_NONE           0
#define MC_PDATA_NORMAL_PER_FACE       512
#define MC_PDATA_NORMAL_PER_VERTEX     768
#define MC_PDATA_NORMAL_MASK           0x300
#define MC_PDATA_TEXURE_COORD          0x3000
#define MC_PDATA_TEXCOORD_MASK         0x3000

/* Effect3D shading types */
#define MC_NORMAL_SHADING 0
#define MC_TOON_SHADING   1

/* MBAC polygon material bits (Loader reference) */
#define MC_POLY_TRANSPARENT 1
#define MC_POLY_BLEND_MASK  6
#define MC_POLY_BLEND_HALF  2
#define MC_POLY_BLEND_ADD   4
#define MC_POLY_BLEND_SUB   6
#define MC_POLY_DOUBLE_FACE 16
#define MC_POLY_LIGHTING    32
#define MC_POLY_SPECULAR    64

/* ----------------------------------------------------------------------------
 * Java-object field access (name-based, same slot layout as opcodes.c)
 * -------------------------------------------------------------------------- */

static int mc_find_field_slot(JavaObject* obj, const char* field_name) {
    if (!obj || !field_name) return -1;
    if (!heap_java_object_valid(obj)) return -1;
    JavaClass* clazz = obj->header.clazz;
    if (!clazz) return -1;

    size_t header_size = sizeof(ObjectHeader);
    if (clazz->instance_size <= header_size) return -1;
    int max_slots = (int)((clazz->instance_size - header_size) / sizeof(JavaValue));
    if (max_slots <= 0) return -1;

    JavaClass* hierarchy[64];
    int depth = 0;
    JavaClass* c = clazz;
    while (c && depth < 64) {
        hierarchy[depth++] = c;
        c = c->super_class;
    }

    int slot = 0;
    for (int h = depth - 1; h >= 0; h--) {
        JavaClass* current = hierarchy[h];
        if (!current->fields) continue;
        for (int i = 0; i < current->fields_count; i++) {
            JavaField* field = &current->fields[i];
            if (field->access_flags & ACC_STATIC) continue;
            if (field->name && strcmp(field->name, field_name) == 0) {
                return (slot < max_slots) ? slot : -1;
            }
            slot++;
            if (field->descriptor &&
                (field->descriptor[0] == 'J' || field->descriptor[0] == 'D')) {
                slot++;
            }
        }
    }
    return -1;
}

static jint mc_get_int(JavaObject* obj, const char* name, jint def) {
    int slot = mc_find_field_slot(obj, name);
    return (slot >= 0) ? obj->fields[slot].i : def;
}

static void mc_set_int(JavaObject* obj, const char* name, jint v) {
    int slot = mc_find_field_slot(obj, name);
    if (slot >= 0) {
        obj->fields[slot].i = v;
    } else {
        /* v36.22 [MC-RESET] DIAG: a failed field WRITE used to be silent —
         * the ctor printed a valid handle while the object kept 0 (Treasure
         * Towers relaunch: same obj pointer, ctor said 28, getter read 0). */
        static int s_set_fail_prints = -1;
        if (s_set_fail_prints < 0) s_set_fail_prints = 0;
        if (s_set_fail_prints < 6 && obj) {
            s_set_fail_prints++;
            fprintf(stderr, MC_TAG "mc_set_int('%s') FAILED: no slot, obj=%p "
                    "clazz=%s instance_size=%ld fields_count=%d\n", name, (void*)obj,
                    obj->header.clazz && obj->header.clazz->class_name
                        ? obj->header.clazz->class_name : "?",
                    obj->header.clazz ? (long)obj->header.clazz->instance_size : -1L,
                    obj->header.clazz ? obj->header.clazz->fields_count : -1);
        }
    }
}

static JavaObject* mc_get_ref(JavaObject* obj, const char* name) {
    int slot = mc_find_field_slot(obj, name);
    if (slot < 0) return NULL;
    JavaObject* ref = (JavaObject*)obj->fields[slot].ref;
    if (ref && !heap_java_object_valid(ref)) return NULL;
    return ref;
}

static void mc_set_ref(JavaObject* obj, const char* name, JavaObject* v) {
    int slot = mc_find_field_slot(obj, name);
    if (slot >= 0) obj->fields[slot].ref = v;
}

/* AffineTrans field quick access: reads the 12 public fields */
typedef struct {
    int m[12]; /* m00 m01 m02 m03  m10 m11 m12 m13  m20 m21 m22 m23 */
} McA;

static const char* const mc_affine_names[12] = {
    "m00","m01","m02","m03",
    "m10","m11","m12","m13",
    "m20","m21","m22","m23"
};

static void mc_affine_read(JavaObject* obj, McA* out) {
    for (int i = 0; i < 12; i++) {
        out->m[i] = mc_get_int(obj, mc_affine_names[i], 0);
    }
}

static void mc_affine_write(JavaObject* obj, const McA* a) {
    for (int i = 0; i < 12; i++) {
        mc_set_int(obj, mc_affine_names[i], a->m[i]);
    }
}

/* ----------------------------------------------------------------------------
 * Texture pool: 8bpp BMP resources decoded to ARGB with color-key alpha
 * -------------------------------------------------------------------------- */
#define MC_MAX_TEXTURES 128

typedef struct {
    int used;
    M3GTexture2D tex; /* rasterizer texture (pixels ARGB, w, h) */
} MCTexture;

static MCTexture mc_textures[MC_MAX_TEXTURES];

static MCTexture* mc_texture_from_handle(int handle) {
    if (handle <= 0 || handle > MC_MAX_TEXTURES) return NULL;
    MCTexture* t = &mc_textures[handle - 1];
    return t->used ? t : NULL;
}

/* Decode an 8bpp palettized Windows BMP (micro3d v3 "meta texture").
 * Returns an ARGB raster, v=0 at image top, index-0 texels alpha 0. */
static int mc_decode_bmp(const uint8_t* data, size_t size, M3GTexture2D* out) {
    if (!data || size < 54) return -1;
    if (data[0] != 'B' || data[1] != 'M') return -1;

    uint32_t raster_off = (uint32_t)data[10] | ((uint32_t)data[11] << 8) |
                          ((uint32_t)data[12] << 16) | ((uint32_t)data[13] << 24);
    uint32_t dib = (uint32_t)data[14] | ((uint32_t)data[15] << 8) |
                   ((uint32_t)data[16] << 16) | ((uint32_t)data[17] << 24);

    int width, height, bpp, top_down;
    if (dib == 12) { /* BITMAPCOREHEADER */
        if (size < 26) return -1;
        width = data[18] | (data[19] << 8);
        height = data[20] | (data[21] << 8);
        bpp = data[24] | (data[25] << 8);
        top_down = 0;
    } else { /* BITMAPINFOHEADER (40) and friends */
        if (size < 34 + 14u) return -1;
        width = (int32_t)((uint32_t)data[18] | ((uint32_t)data[19] << 8) |
                          ((uint32_t)data[20] << 16) | ((uint32_t)data[21] << 24));
        int32_t h = (int32_t)((uint32_t)data[22] | ((uint32_t)data[23] << 8) |
                              ((uint32_t)data[24] << 16) | ((uint32_t)data[25] << 24));
        top_down = (h < 0);
        height = top_down ? -h : h;
        bpp = data[28] | (data[29] << 8);
        if (dib >= 40) {
            uint32_t compression = (uint32_t)data[30] | ((uint32_t)data[31] << 8) |
                                   ((uint32_t)data[32] << 16) | ((uint32_t)data[33] << 24);
            if (compression != 0) return -1; /* BI_RGB only */
        }
    }

    if (bpp != 8) return -1;                 /* micro3d textures are 8bpp */
    if (width <= 0 || height <= 0) return -1;
    if (width > 1024 || height > 1024) return -1;

    size_t palette_off = 14 + dib;
    size_t palette_entries = (dib == 12) ? 256 : 256;
    if (dib >= 40) {
        uint32_t ncolors = (uint32_t)data[46] | ((uint32_t)data[47] << 8) |
                           ((uint32_t)data[48] << 16) | ((uint32_t)data[49] << 24);
        if (ncolors == 0 || ncolors > 256) ncolors = 256;
        palette_entries = ncolors;
    }
    if (palette_off + palette_entries * 4 > size) return -1;
    if (raster_off == 0) raster_off = (uint32_t)(palette_off + palette_entries * 4);
    if (raster_off >= size) return -1;

    uint32_t* pixels = (uint32_t*)malloc((size_t)width * height * sizeof(uint32_t));
    if (!pixels) return -1;

    int stride = (width + 3) & ~3;
    for (int y = 0; y < height; y++) {
        /* Output v=0 at image top. Bottom-up BMPs (positive height) store
         * the LAST image row first in the file. */
        int src_row = top_down ? y : (height - 1 - y);
        size_t row_off = raster_off + (size_t)src_row * stride;
        if (row_off + (size_t)width > size) {
            /* tolerate short files: fill transparent */
            for (int x = 0; x < width; x++) pixels[(size_t)y * width + x] = 0;
            continue;
        }
        for (int x = 0; x < width; x++) {
            uint8_t idx = data[row_off + x];
            const uint8_t* pe = data + palette_off + (size_t)(idx & 0xFF) * 4;
            uint8_t b = pe[0], g = pe[1], r = pe[2];
            uint8_t a = (idx == 0) ? 0 : 0xFF; /* palette index 0 = color key */
            pixels[(size_t)y * width + x] =
                ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        }
    }

    memset(out, 0, sizeof(*out));
    out->pixels = pixels;
    out->width = width;
    out->height = height;
    out->blend_s = 1; /* clamp */
    out->blend_t = 1;
    out->filter_level = 208; /* nearest (FILTER_BASE_LEVEL) */
    return 0;
}

static MCTexture* mc_texture_alloc(void) {
    for (int i = 0; i < MC_MAX_TEXTURES; i++) {
        if (!mc_textures[i].used) {
            memset(&mc_textures[i], 0, sizeof(MCTexture));
            mc_textures[i].used = 1;
            return &mc_textures[i];
        }
    }
    /* Pool exhausted (long sessions with per-level reloads): recycle the
     * whole pool. Games rebuild their textures per level anyway; stale
     * handles in dead Java objects degrade gracefully (NULL texture). */
    fprintf(stderr, MC_TAG "texture pool full (%d) — recycling\n", MC_MAX_TEXTURES);
    for (int i = 0; i < MC_MAX_TEXTURES; i++) {
        free(mc_textures[i].tex.pixels);
        memset(&mc_textures[i], 0, sizeof(MCTexture));
    }
    mc_textures[0].used = 1;
    return &mc_textures[0];
}

/* ----------------------------------------------------------------------------
 * MBAC figure parser (versions 3/4/5, vertex formats 1/2, polygon 1/2/3)
 * -------------------------------------------------------------------------- */
/* v34.33: raised from 32 — Treasure Towers builds more than 32 Figures
 * (6 stage sets x several towers/props); same handle-invalidation hazard
 * as MC_MAX_TRAS above. */
#define MC_MAX_FIGURES 128
#define MC_MAX_VERTS   21845

typedef struct {
    int idx[4];
    int uv[8];     /* texel u,v per vertex (4 pairs) */
    int material;
    int pattern;
    int face;      /* texture index, -1 = none */
    int is_quad;
} MCPoly;

/* Skeleton bone: contiguous vertex range + bind matrix (row-major 3x4). */
typedef struct {
    int first, count;
    int parent;      /* -1 = root */
    float bind[12];
} MCBone;

typedef struct {
    int used;
    float* verts;     /* 3 floats per vertex, current pose */
    float* verts_bind;/* 3 floats per vertex, bind pose (bones not applied) */
    float* normals;   /* 3 floats per vertex, normalized; NULL if none */
    float* normals_bind;
    MCBone* bones;
    int num_bones;
    int vert_count;
    MCPoly* polys;
    int poly_count;
    int num_patterns;
} MCFigure;

static MCFigure mc_figures[MC_MAX_FIGURES];

static void mc_figure_free(MCFigure* f);

static MCFigure* mc_figure_from_handle(int handle) {
    if (handle <= 0 || handle > MC_MAX_FIGURES) return NULL;
    MCFigure* f = &mc_figures[handle - 1];
    return f->used ? f : NULL;
}

typedef struct {
    const uint8_t* data;
    size_t size;
    size_t pos;      /* byte position for byte-aligned reads */
    uint32_t acc;    /* bit accumulator */
    int bits;        /* bits available in acc */
} McBits;

static int mcb_byte(McBits* b) {
    if (b->pos >= b->size) return -1;
    return b->data[b->pos++];
}

static uint32_t mcb_bits(McBits* b, int n) {
    while (b->bits < n) {
        int byte = mcb_byte(b);
        if (byte < 0) { byte = 0; }
        b->acc |= (uint32_t)byte << b->bits;
        b->bits += 8;
    }
    uint32_t v = b->acc & ((1u << n) - 1u);
    b->acc >>= n;
    b->bits -= n;
    return v;
}

static int32_t mcb_sbits(McBits* b, int n) {
    uint32_t v = mcb_bits(b, n);
    if (v & (1u << (n - 1))) return (int32_t)(v - (1u << n));
    return (int32_t)v;
}

static void mcb_align(McBits* b) {
    /* drop whole accumulated bytes, round pos up to byte boundary */
    b->acc = 0;
    b->bits = 0;
}

static uint16_t mcb_u16(McBits* b) {
    uint16_t lo = (uint16_t)mcb_byte(b);
    uint16_t hi = (uint16_t)mcb_byte(b);
    return (uint16_t)(lo | (hi << 8));
}

static int16_t mcb_s16(McBits* b) {
    return (int16_t)mcb_u16(b);
}

static uint32_t mcb_u32(McBits* b) {
    uint32_t lo = (uint32_t)mcb_u16(b);
    uint32_t hi = (uint32_t)mcb_u16(b);
    return lo | (hi << 16);
}

/* Fixed 4.12 helpers */
static int mc_isin(int angle) {
    double rad = (double)angle * 3.14159265358979323846 / 2048.0;
    return (int)lround(sin(rad) * 4096.0);
}

static int mc_icos(int angle) { return mc_isin(angle + 1024); }

/* Load MBAC data. Returns 0 on success. Mirrors the reference parser:
 * little-endian fields, packed vertex/normal/polygon bitstreams. */
static int mc_load_mbac(const uint8_t* data, size_t size, MCFigure* fig) {
    McBits br = { data, size, 0, 0, 0 };
    if (mcb_byte(&br) != 'M' || mcb_byte(&br) != 'B') return -1;
    int version = mcb_byte(&br);
    if (mcb_byte(&br) != 0) return -1;
    if (version < 3 || version > 5) return -1;

    int vertexFormat = 1, normalFormat = 0, polygonFormat = 1, boneFormat = 1;
    if (version > 3) {
        vertexFormat = mcb_byte(&br);
        normalFormat = mcb_byte(&br);
        polygonFormat = mcb_byte(&br);
        boneFormat = mcb_byte(&br);
    }
    (void)boneFormat;

    int numVertices = mcb_u16(&br);
    int numPolyT3 = mcb_u16(&br);
    int numPolyT4 = mcb_u16(&br);
    int numBones = mcb_u16(&br);

    int numPolyC3 = 0, numPolyC4 = 0, numTextures = 1, numPatterns = 1, numColors = 0;
    /* patterns[i][j] = {polyC3cnt/polyC4cnt, polyT3/polyT4} */
    static int patterns[33][17][2];
    memset(patterns, 0, sizeof(patterns));

    if (polygonFormat >= 3) {
        numPolyC3 = mcb_u16(&br);
        numPolyC4 = mcb_u16(&br);
        numTextures = mcb_u16(&br);
        numPatterns = mcb_u16(&br);
        numColors = mcb_u16(&br);
        if (numVertices > MC_MAX_VERTS || numTextures > 16 ||
            numPatterns > 33 || numColors > 256) {
            return -1;
        }
        if (version == 5) {
            for (int i = 0; i < numPatterns; i++) {
                for (int j = 0; j <= numTextures; j++) {
                    patterns[i][j][0] = mcb_u16(&br);
                    patterns[i][j][1] = mcb_u16(&br);
                }
            }
        } else {
            patterns[0][0][0] = numPolyC3;
            patterns[0][0][1] = numPolyC4;
            patterns[0][1][0] = numPolyT3;
            patterns[0][1][1] = numPolyT4;
        }
    } else {
        patterns[0][0][0] = numPolyC3;
        patterns[0][0][1] = numPolyC4;
        patterns[0][1][0] = numPolyT3;
        patterns[0][1][1] = numPolyT4;
    }

    if (numVertices <= 0) return -1;

    /* --- vertices --- */
    float* verts = (float*)calloc((size_t)numVertices * 3, sizeof(float));
    if (!verts) return -1;
    if (vertexFormat == 1) {
        for (int i = 0; i < numVertices * 3; i++) {
            verts[i] = (float)mcb_s16(&br);
        }
    } else if (vertexFormat == 2) {
        int have = 0;
        mcb_align(&br);
        while (have < numVertices) {
            int chunk = (int)mcb_bits(&br, 8);
            int type = chunk >> 6;
            int count = (chunk & 0x3F) + 1;
            if (count > numVertices - have) count = numVertices - have;
            static const int sizes[4] = { 8, 10, 13, 16 };
            for (int i = 0; i < count; i++) {
                verts[(size_t)(have + i) * 3 + 0] = (float)mcb_sbits(&br, sizes[type]);
                verts[(size_t)(have + i) * 3 + 1] = (float)mcb_sbits(&br, sizes[type]);
                verts[(size_t)(have + i) * 3 + 2] = (float)mcb_sbits(&br, sizes[type]);
            }
            have += count;
        }
        mcb_align(&br);
    } else {
        free(verts);
        return -1;
    }

    /* --- normals --- */
    float* normals = NULL;
    if (normalFormat != 0) {
        normals = (float*)calloc((size_t)numVertices * 3, sizeof(float));
        if (!normals) { free(verts); return -1; }
        if (normalFormat == 1) {
            for (int i = 0; i < numVertices * 3; i++) {
                normals[i] = (float)mcb_s16(&br) / 4096.0f;
            }
        } else {
            mcb_align(&br);
            static const int pool[6] = { 0, 0, 4096, -4096, 0, 0 };
            for (int i = 0; i < numVertices; i++) {
                int x = (int)mcb_bits(&br, 7);
                int y, zsign;
                if (x == 64) {
                    int type = (int)mcb_bits(&br, 3);
                    if (type > 5) type = 5;
                    /* direction table (z, y, x triplets like the reference) */
                    static const int dirs[6][3] = {
                        {4096,0,0},{-4096,0,0},{0,4096,0},{0,-4096,0},{0,0,4096},{0,0,-4096}
                    };
                    normals[(size_t)i * 3 + 0] = dirs[type][0] / 4096.0f;
                    normals[(size_t)i * 3 + 1] = dirs[type][1] / 4096.0f;
                    normals[(size_t)i * 3 + 2] = dirs[type][2] / 4096.0f;
                } else {
                    x = (x << 25) >> 19; /* sign-extend 7 bits, scale 64 */
                    y = ((int)mcb_bits(&br, 7) << 25) >> 19;
                    zsign = (int)mcb_bits(&br, 1);
                    float fx = x / 4096.0f, fy = y / 4096.0f;
                    float d = 1.0f - fx * fx - fy * fy;
                    float fz = (d > 0.0f) ? sqrtf(d) : 0.0f;
                    if (zsign) fz = -fz;
                    normals[(size_t)i * 3 + 0] = fx;
                    normals[(size_t)i * 3 + 1] = fy;
                    normals[(size_t)i * 3 + 2] = fz;
                }
            }
            mcb_align(&br);
            (void)pool;
        }
    }

    /* --- colored polygons (untextured) — parsed, rendered as flat color --- */
    int totalC = numPolyC3 + numPolyC4;
    int totalT = numPolyT3 + numPolyT4;

    /* --- textured polygons --- */
    MCPoly* polys = (MCPoly*)calloc((size_t)(totalC + totalT), sizeof(MCPoly));
    if (!polys) { free(verts); free(normals); return -1; }
    int poly_count = 0;

    if (totalC > 0 && polygonFormat >= 3) {
        int materialBits = (int)mcb_bits(&br, 8);
        int vertexIndexBits = (int)mcb_bits(&br, 8);
        int colorBits = (int)mcb_bits(&br, 8);
        int colorIdBits = (int)mcb_bits(&br, 8);
        (void)mcb_bits(&br, 8);
        (void)materialBits; (void)colorIdBits;
        /* skip color table */
        for (int i = 0; i < numColors * 3; i++) mcb_bits(&br, colorBits);
        for (int i = 0; i < numPolyC3; i++) {
            (void)mcb_bits(&br, materialBits);
            int a = (int)mcb_bits(&br, vertexIndexBits);
            int b = (int)mcb_bits(&br, vertexIndexBits);
            int c = (int)mcb_bits(&br, vertexIndexBits);
            (void)mcb_bits(&br, colorIdBits);
            if (a >= numVertices || b >= numVertices || c >= numVertices) continue;
            MCPoly* p = &polys[poly_count++];
            p->idx[0] = a; p->idx[1] = b; p->idx[2] = c;
            p->is_quad = 0;
            p->material = 0;
            p->face = -1;
        }
        for (int i = 0; i < numPolyC4; i++) {
            (void)mcb_bits(&br, materialBits);
            int a = (int)mcb_bits(&br, vertexIndexBits);
            int b = (int)mcb_bits(&br, vertexIndexBits);
            int c = (int)mcb_bits(&br, vertexIndexBits);
            int d = (int)mcb_bits(&br, vertexIndexBits);
            (void)mcb_bits(&br, colorIdBits);
            if (a >= numVertices || b >= numVertices ||
                c >= numVertices || d >= numVertices) continue;
            MCPoly* p = &polys[poly_count++];
            p->idx[0] = a; p->idx[1] = b; p->idx[2] = c; p->idx[3] = d;
            p->is_quad = 1;
            p->material = 0;
            p->face = -1;
        }
    }

    if (totalT > 0) {
        int materialBits = 16, vertexIndexBits = 16, uvBits = 8;
        if (polygonFormat >= 2) {
            mcb_align(&br);
            materialBits = (int)mcb_bits(&br, 8);
            vertexIndexBits = (int)mcb_bits(&br, 8);
            if (polygonFormat == 3) {
                uvBits = (int)mcb_bits(&br, 8);
                (void)mcb_bits(&br, 8);
            } else {
                uvBits = 7;
            }
        }
        for (int i = 0; i < numPolyT3; i++) {
            int material = (int)mcb_bits(&br, materialBits);
            int a = (int)mcb_bits(&br, vertexIndexBits);
            int b = (int)mcb_bits(&br, vertexIndexBits);
            int c = (int)mcb_bits(&br, vertexIndexBits);
            int uv[6];
            for (int k = 0; k < 6; k++) uv[k] = (int)mcb_bits(&br, uvBits);
            if (a >= numVertices || b >= numVertices || c >= numVertices) continue;
            MCPoly* p = &polys[poly_count++];
            p->idx[0] = a; p->idx[1] = b; p->idx[2] = c;
            p->is_quad = 0;
            p->material = material;
            p->face = -1;
            for (int k = 0; k < 6; k++) p->uv[k] = uv[k];
        }
        for (int i = 0; i < numPolyT4; i++) {
            int material = (int)mcb_bits(&br, materialBits);
            int a = (int)mcb_bits(&br, vertexIndexBits);
            int b = (int)mcb_bits(&br, vertexIndexBits);
            int c = (int)mcb_bits(&br, vertexIndexBits);
            int d = (int)mcb_bits(&br, vertexIndexBits);
            int uv[8];
            for (int k = 0; k < 8; k++) uv[k] = (int)mcb_bits(&br, uvBits);
            if (a >= numVertices || b >= numVertices ||
                c >= numVertices || d >= numVertices) continue;
            MCPoly* p = &polys[poly_count++];
            p->idx[0] = a; p->idx[1] = b; p->idx[2] = c; p->idx[3] = d;
            p->is_quad = 1;
            p->material = material;
            p->face = -1;
            for (int k = 0; k < 8; k++) p->uv[k] = uv[k];
        }
    }

    /* --- pattern / texture assignment (Loader reference order) --- */
    {
        int c3 = 0, c4 = numPolyC3, t3 = 0, t4 = numPolyT3;
        for (int i = 0; i < numPatterns && i < 33; i++) {
            int p = (i == 0) ? 0 : (1 << i);
            int cnt = patterns[i][0][0];
            while (cnt-- > 0 && c3 < poly_count) polys[c3++].pattern = p;
            cnt = patterns[i][0][1];
            while (cnt-- > 0 && c4 < poly_count) polys[c4++].pattern = p;
            for (int j = 0; j < numTextures && j < 16; j++) {
                cnt = patterns[i][j + 1][0];
                while (cnt-- > 0 && t3 < poly_count) {
                    polys[t3].pattern = p;
                    polys[t3].face = j;
                    t3++;
                }
                cnt = patterns[i][j + 1][1];
                while (cnt-- > 0 && t4 < poly_count) {
                    polys[t4].pattern = p;
                    polys[t4].face = j;
                    t4++;
                }
            }
        }
    }

    /* raw bind-pose copies (pre-skeleton, reference originalVertices) */
    fig->verts_bind = (float*)calloc((size_t)numVertices * 3, sizeof(float));
    if (fig->verts_bind) {
        memcpy(fig->verts_bind, verts, (size_t)numVertices * 3 * sizeof(float));
    }
    if (normals) {
        fig->normals_bind = (float*)calloc((size_t)numVertices * 3, sizeof(float));
        if (fig->normals_bind) {
            memcpy(fig->normals_bind, normals, (size_t)numVertices * 3 * sizeof(float));
        }
    }

    /* --- bones: cumulative bind matrices applied to the bind pose ---
     * v34.32: the skeleton is kept (per-bone vertex range, parent, bind
     * matrix) so ActionTable postures can re-pose the figure; the bind
     * chain is baked into fig->verts for the default pose, and normals
     * are rotated by the same chain (reference utils.cpp transformVector). */
    if (numBones > 0) {
        mcb_align(&br);
        /* cumulative row-major 3x4 matrices in float */
        float (*cum)[12] = (float(*)[12])calloc((size_t)numBones, sizeof(float[12]));
        MCBone* bones = (MCBone*)calloc((size_t)numBones, sizeof(MCBone));
        int* boneVerts = (int*)calloc((size_t)numBones, sizeof(int));
        if (!cum || !bones || !boneVerts) {
            free(cum); free(bones); free(boneVerts);
            free(verts); free(normals); free(polys);
            return -1;
        }
        for (int i = 0; i < numBones; i++) {
            boneVerts[i] = mcb_u16(&br);
            int parent = mcb_s16(&br);
            int m[12];
            for (int k = 0; k < 12; k++) m[k] = mcb_s16(&br);
            /* bind matrix: rotation 4.12 -> float, translation raw */
            for (int k = 0; k < 12; k++) {
                bones[i].bind[k] = (k % 4 == 3) ? (float)m[k] : (float)m[k] / 4096.0f;
            }
            bones[i].parent = parent;
            /* cum = parentCum * m (float affine multiply) */
            const float* pm = (parent < 0) ? NULL : cum[parent];
            if (!pm) {
                memcpy(cum[i], bones[i].bind, sizeof(float[12]));
            } else {
                for (int r = 0; r < 3; r++) {
                    for (int col = 0; col < 4; col++) {
                        float v = 0.0f;
                        for (int k = 0; k < 3; k++) {
                            v += pm[r * 4 + k] * bones[i].bind[k * 4 + col];
                        }
                        cum[i][r * 4 + col] = v + (col == 3 ? pm[r * 4 + 3] : 0.0f);
                    }
                }
            }
        }
        /* vertex ranges (contiguous, in file order) */
        int start = 0;
        for (int i = 0; i < numBones; i++) {
            bones[i].first = start;
            bones[i].count = boneVerts[i];
            start += boneVerts[i];
            if (start > numVertices) {
                bones[i].count -= start - numVertices;
                start = numVertices;
                if (bones[i].count < 0) bones[i].count = 0;
            }
        }
        /* default pose: bake the bind chain into verts/normals */
        for (int i = 0; i < numBones; i++) {
            for (int v = bones[i].first; v < bones[i].first + bones[i].count; v++) {
                float x = verts[(size_t)v * 3], y = verts[(size_t)v * 3 + 1],
                      z = verts[(size_t)v * 3 + 2];
                verts[(size_t)v * 3 + 0] =
                    cum[i][0] * x + cum[i][1] * y + cum[i][2] * z + cum[i][3];
                verts[(size_t)v * 3 + 1] =
                    cum[i][4] * x + cum[i][5] * y + cum[i][6] * z + cum[i][7];
                verts[(size_t)v * 3 + 2] =
                    cum[i][8] * x + cum[i][9] * y + cum[i][10] * z + cum[i][11];
                if (normals) {
                    float nx = normals[(size_t)v * 3], ny = normals[(size_t)v * 3 + 1],
                          nz = normals[(size_t)v * 3 + 2];
                    float ox = cum[i][0] * nx + cum[i][1] * ny + cum[i][2] * nz;
                    float oy = cum[i][4] * nx + cum[i][5] * ny + cum[i][6] * nz;
                    float oz = cum[i][8] * nx + cum[i][9] * ny + cum[i][10] * nz;
                    float len = sqrtf(ox * ox + oy * oy + oz * oz);
                    if (len > 1e-9f) { ox /= len; oy /= len; oz /= len; }
                    normals[(size_t)v * 3 + 0] = ox;
                    normals[(size_t)v * 3 + 1] = oy;
                    normals[(size_t)v * 3 + 2] = oz;
                }
            }
        }
        free(cum);
        free(boneVerts);
        fig->bones = bones;
        fig->num_bones = numBones;
    }


    fig->verts = verts;
    fig->normals = normals;
    fig->vert_count = numVertices;
    fig->polys = polys;
    fig->poly_count = poly_count;
    fig->num_patterns = numPatterns;
    return 0;
}

static MCFigure* mc_figure_alloc(void) {
    for (int i = 0; i < MC_MAX_FIGURES; i++) {
        if (!mc_figures[i].used) {
            memset(&mc_figures[i], 0, sizeof(MCFigure));
            mc_figures[i].used = 1;
            return &mc_figures[i];
        }
    }
    fprintf(stderr, MC_TAG "figure pool full (%d) — recycling\n", MC_MAX_FIGURES);
    for (int i = 0; i < MC_MAX_FIGURES; i++) mc_figure_free(&mc_figures[i]);
    mc_figures[0].used = 1;
    return &mc_figures[0];
}

static void mc_figure_free(MCFigure* f) {
    if (!f) return;
    free(f->verts);
    free(f->verts_bind);
    free(f->normals);
    free(f->normals_bind);
    free(f->bones);
    free(f->polys);
    memset(f, 0, sizeof(MCFigure));
}

/* ----------------------------------------------------------------------------
 * ActionTable (MTRA) — animation data, reference: woesss/JL-Mod Loader
 * -------------------------------------------------------------------------- */
/* v34.33: raised from 16 — Treasure Towers loads 41 ActionTables from
 * actions.bin (plus per-model ones); the old pool of 16 hit the "recycle
 * the whole pool" path, which INVALIDATED the handles already stored in
 * live Java ActionTable objects. getNumFrames then returned 0 and the
 * game divided by zero in b.p() at startup (ArithmeticException, midlet
 * died before painting a single frame). */
#define MC_MAX_TRAS 96

typedef struct {
    int kf;
    float v[3];
} MCAnimKey;

typedef struct {
    int count;
    MCAnimKey* keys;
} MCAnim;          /* 3-channel animated value (translate/rotate/scale) */

typedef struct {
    int count;
    int* kf;
    float* val;
} MCRollAnim;      /* 1-channel animated roll */

typedef struct {
    int type;                  /* 0..6 */
    float mat[12];             /* type 0: static matrix */
    MCAnim translate, scale, rotate;
    MCRollAnim roll;
    float staticTrans[3];      /* types 3: static channels */
    float staticRoll;
} MCBoneAnim;

typedef struct {
    int keyframes;             /* keyframe count; API returns keyframes << 16 */
    MCBoneAnim* bones;
    int num_bones;
    int dyn_count;             /* version >= 5: dynamic pattern timeline */
    int* dyn_frame;
    int* dyn_pattern;
} MCAction;

typedef struct {
    int used;
    int num_actions;
    MCAction* actions;
} MCTra;

static MCTra mc_tras[MC_MAX_TRAS];

static MCTra* mc_tra_from_handle(int handle) {
    if (handle <= 0 || handle > MC_MAX_TRAS) return NULL;
    MCTra* t = &mc_tras[handle - 1];
    return t->used ? t : NULL;
}

static void mc_tra_free(MCTra* t) {
    if (!t) return;
    for (int a = 0; a < t->num_actions; a++) {
        MCAction* act = &t->actions[a];
        for (int b = 0; b < act->num_bones; b++) {
            free(act->bones[b].translate.keys);
            free(act->bones[b].scale.keys);
            free(act->bones[b].rotate.keys);
            free(act->bones[b].roll.kf);
            free(act->bones[b].roll.val);
        }
        free(act->bones);
        free(act->dyn_frame);
        free(act->dyn_pattern);
    }
    free(t->actions);
    memset(t, 0, sizeof(MCTra));
}

static MCTra* mc_tra_alloc(void) {
    for (int i = 0; i < MC_MAX_TRAS; i++) {
        if (!mc_tras[i].used) {
            memset(&mc_tras[i], 0, sizeof(MCTra));
            mc_tras[i].used = 1;
            return &mc_tras[i];
        }
    }
    /* recycle the whole pool (same policy as figures/textures) */
    fprintf(stderr, MC_TAG "mtra pool full (%d) — recycling\n", MC_MAX_TRAS);
    for (int i = 0; i < MC_MAX_TRAS; i++) mc_tra_free(&mc_tras[i]);
    memset(&mc_tras[0], 0, sizeof(MCTra));
    mc_tras[0].used = 1;
    return &mc_tras[0];
}

/* --- MTRA parsing (byte-aligned reads; the trailing 20-byte trailer is
 * left unread like the reference loader) --- */
static int mc_load_mtra(const uint8_t* data, size_t size, MCTra* out) {
    McBits br = { data, size, 0, 0, 0 };
    if (mcb_byte(&br) != 'M' || mcb_byte(&br) != 'T') return -1;
    int version = mcb_byte(&br);
    if (mcb_byte(&br) != 0 || version < 2 || version > 5) return -1;

    int numActions = mcb_u16(&br);
    int numBones = mcb_u16(&br);
    for (int i = 0; i < 8; i++) (void)mcb_u16(&br); /* transTypeCounts */
    (void)mcb_u32(&br);                              /* dataSize */
    if (numActions < 0 || numActions > 1024 || numBones < 0 || numBones > 256) {
        return -1;
    }

    MCAction* actions = (MCAction*)calloc((size_t)(numActions > 0 ? numActions : 1),
                                          sizeof(MCAction));
    if (!actions) return -1;

    for (int a = 0; a < numActions; a++) {
        MCAction* act = &actions[a];
        act->keyframes = mcb_u16(&br);
        act->num_bones = numBones;
        act->bones = numBones > 0
            ? (MCBoneAnim*)calloc((size_t)numBones, sizeof(MCBoneAnim))
            : NULL;
        if (numBones > 0 && !act->bones) goto fail;
        for (int b = 0; b < numBones; b++) {
            MCBoneAnim* ba = &act->bones[b];
            ba->type = mcb_byte(&br);
            switch (ba->type) {
                case 0: {
                    /* static: rotation 4.12, translation raw */
                    for (int k = 0; k < 12; k++) {
                        int v = mcb_s16(&br);
                        ba->mat[k] = (k % 4 == 3) ? (float)v : (float)v / 4096.0f;
                    }
                    break;
                }
                case 1:
                    break; /* identity */
                case 2: {
                    /* translate (raw ints), scale (4.12), rotate (axis),
                     * roll (angle, 4096 = 2*pi) — all keyframed */
                    int n = mcb_u16(&br);
                    if (n > 0) {
                        ba->translate.keys = (MCAnimKey*)calloc((size_t)n, sizeof(MCAnimKey));
                        if (!ba->translate.keys) goto fail;
                        ba->translate.count = n;
                        for (int j = 0; j < n; j++) {
                            ba->translate.keys[j].kf = mcb_u16(&br);
                            ba->translate.keys[j].v[0] = (float)mcb_s16(&br);
                            ba->translate.keys[j].v[1] = (float)mcb_s16(&br);
                            ba->translate.keys[j].v[2] = (float)mcb_s16(&br);
                        }
                    }
                    n = mcb_u16(&br);
                    if (n > 0) {
                        ba->scale.keys = (MCAnimKey*)calloc((size_t)n, sizeof(MCAnimKey));
                        if (!ba->scale.keys) goto fail;
                        ba->scale.count = n;
                        for (int j = 0; j < n; j++) {
                            ba->scale.keys[j].kf = mcb_u16(&br);
                            ba->scale.keys[j].v[0] = (float)mcb_s16(&br) / 4096.0f;
                            ba->scale.keys[j].v[1] = (float)mcb_s16(&br) / 4096.0f;
                            ba->scale.keys[j].v[2] = (float)mcb_s16(&br) / 4096.0f;
                        }
                    }
                    n = mcb_u16(&br);
                    if (n > 0) {
                        ba->rotate.keys = (MCAnimKey*)calloc((size_t)n, sizeof(MCAnimKey));
                        if (!ba->rotate.keys) goto fail;
                        ba->rotate.count = n;
                        for (int j = 0; j < n; j++) {
                            ba->rotate.keys[j].kf = mcb_u16(&br);
                            ba->rotate.keys[j].v[0] = (float)mcb_s16(&br);
                            ba->rotate.keys[j].v[1] = (float)mcb_s16(&br);
                            ba->rotate.keys[j].v[2] = (float)mcb_s16(&br);
                        }
                    }
                    n = mcb_u16(&br);
                    if (n > 0) {
                        ba->roll.kf = (int*)calloc((size_t)n, sizeof(int));
                        ba->roll.val = (float*)calloc((size_t)n, sizeof(float));
                        if (!ba->roll.kf || !ba->roll.val) goto fail;
                        ba->roll.count = n;
                        for (int j = 0; j < n; j++) {
                            ba->roll.kf[j] = mcb_u16(&br);
                            ba->roll.val[j] = (float)mcb_s16(&br) * (float)(3.14159265358979 / 2048.0);
                        }
                    }
                    break;
                }
                case 3: {
                    ba->staticTrans[0] = (float)mcb_s16(&br);
                    ba->staticTrans[1] = (float)mcb_s16(&br);
                    ba->staticTrans[2] = (float)mcb_s16(&br);
                    int n = mcb_u16(&br);
                    if (n > 0) {
                        ba->rotate.keys = (MCAnimKey*)calloc((size_t)n, sizeof(MCAnimKey));
                        if (!ba->rotate.keys) goto fail;
                        ba->rotate.count = n;
                        for (int j = 0; j < n; j++) {
                            ba->rotate.keys[j].kf = mcb_u16(&br);
                            ba->rotate.keys[j].v[0] = (float)mcb_s16(&br);
                            ba->rotate.keys[j].v[1] = (float)mcb_s16(&br);
                            ba->rotate.keys[j].v[2] = (float)mcb_s16(&br);
                        }
                    }
                    ba->staticRoll = (float)mcb_s16(&br) * (float)(3.14159265358979 / 2048.0);
                    break;
                }
                case 4:
                case 5:
                case 6: {
                    if (ba->type != 5) { /* types 4,6: translate first */
                        if (ba->type == 6) {
                            int n = mcb_u16(&br);
                            if (n > 0) {
                                ba->translate.keys = (MCAnimKey*)calloc((size_t)n, sizeof(MCAnimKey));
                                if (!ba->translate.keys) goto fail;
                                ba->translate.count = n;
                                for (int j = 0; j < n; j++) {
                                    ba->translate.keys[j].kf = mcb_u16(&br);
                                    ba->translate.keys[j].v[0] = (float)mcb_s16(&br);
                                    ba->translate.keys[j].v[1] = (float)mcb_s16(&br);
                                    ba->translate.keys[j].v[2] = (float)mcb_s16(&br);
                                }
                            }
                        }
                    }
                    {
                        int n = mcb_u16(&br);
                        if (n > 0) {
                            ba->rotate.keys = (MCAnimKey*)calloc((size_t)n, sizeof(MCAnimKey));
                            if (!ba->rotate.keys) goto fail;
                            ba->rotate.count = n;
                            for (int j = 0; j < n; j++) {
                                ba->rotate.keys[j].kf = mcb_u16(&br);
                                ba->rotate.keys[j].v[0] = (float)mcb_s16(&br);
                                ba->rotate.keys[j].v[1] = (float)mcb_s16(&br);
                                ba->rotate.keys[j].v[2] = (float)mcb_s16(&br);
                            }
                        }
                    }
                    if (ba->type != 5) { /* types 4,6: roll */
                        int n = mcb_u16(&br);
                        if (n > 0) {
                            ba->roll.kf = (int*)calloc((size_t)n, sizeof(int));
                            ba->roll.val = (float*)calloc((size_t)n, sizeof(float));
                            if (!ba->roll.kf || !ba->roll.val) goto fail;
                            ba->roll.count = n;
                            for (int j = 0; j < n; j++) {
                                ba->roll.kf[j] = mcb_u16(&br);
                                ba->roll.val[j] = (float)mcb_s16(&br) * (float)(3.14159265358979 / 2048.0);
                            }
                        }
                    }
                    break;
                }
                default:
                    goto fail;
            }
        }
        if (version >= 5) {
            int n = mcb_u16(&br);
            if (n > 0) {
                act->dyn_frame = (int*)calloc((size_t)n, sizeof(int));
                act->dyn_pattern = (int*)calloc((size_t)n, sizeof(int));
                if (!act->dyn_frame || !act->dyn_pattern) goto fail;
                act->dyn_count = n;
                for (int j = 0; j < n; j++) {
                    act->dyn_frame[j] = mcb_u16(&br);
                    act->dyn_pattern[j] = (int)mcb_u32(&br);
                }
            }
        }
    }

    out->actions = actions;
    out->num_actions = numActions;
    return 0;
fail:
    for (int a = 0; a <= (int)(numActions - 1) && a >= 0; a++) {
        MCAction* act = &actions[a];
        if (!act->bones) continue;
        for (int b = 0; b < act->num_bones; b++) {
            free(act->bones[b].translate.keys);
            free(act->bones[b].scale.keys);
            free(act->bones[b].rotate.keys);
            free(act->bones[b].roll.kf);
            free(act->bones[b].roll.val);
        }
        free(act->bones);
        free(act->dyn_frame);
        free(act->dyn_pattern);
    }
    free(actions);
    return -1;
}

/* --- animation evaluation (reference Action.java) --- */
static void mc_anim_get(const MCAnim* a, float kgf, float out[3]) {
    out[0] = out[1] = out[2] = 0.0f;
    if (!a || a->count == 0) return;
    int max = a->count - 1;
    if (kgf >= (float)a->keys[max].kf) {
        out[0] = a->keys[max].v[0];
        out[1] = a->keys[max].v[1];
        out[2] = a->keys[max].v[2];
        return;
    }
    for (int i = max - 1; i >= 0; i--) {
        int prevKf = a->keys[i].kf;
        if ((float)prevKf > kgf) continue;
        float x = a->keys[i].v[0], y = a->keys[i].v[1], z = a->keys[i].v[2];
        if ((float)prevKf == kgf) { out[0] = x; out[1] = y; out[2] = z; return; }
        int nextKf = a->keys[i + 1].kf;
        float delta = (kgf - (float)prevKf) / (float)(nextKf - prevKf);
        out[0] = x + (a->keys[i + 1].v[0] - x) * delta;
        out[1] = y + (a->keys[i + 1].v[1] - y) * delta;
        out[2] = z + (a->keys[i + 1].v[2] - z) * delta;
        return;
    }
}

static float mc_roll_get(const MCRollAnim* r, float kgf) {
    if (!r || r->count == 0) return 0.0f;
    int max = r->count - 1;
    if (kgf >= (float)r->kf[max]) return r->val[max];
    for (int i = max - 1; i >= 0; i--) {
        int key = r->kf[i];
        if ((float)key > kgf) continue;
        float v = r->val[i];
        if ((float)key == kgf) return v;
        int nextKey = r->kf[i + 1];
        return v + (r->val[i + 1] - v) / (float)(nextKey - key) * (kgf - (float)key);
    }
    return 0.0f;
}

/* rotate matrix so +Z points along (x,y,z) — reference Action.Bone.rotate */
static void mc_mat_rotate_to_z(float* m, float x, float y, float z) {
    float len = sqrtf(x * x + y * y + z * z);
    if (len > 1e-12f) { x /= len; y /= len; z /= len; }
    float xx = x * x, yy = y * y;
    if (xx > 0.0f || yy > 0.0f) {
        float a = (1.0f - z) / (yy + xx);
        float b = -a * (x * y);
        m[0] = z + yy * a;  m[1] = b;          m[2] = x;
        m[4] = b;           m[5] = z + xx * a;  m[6] = y;
        m[8] = -x;          m[9] = -y;         m[10] = z;
    } else {
        m[0] = 1.0f; m[1] = 0.0f; m[2] = 0.0f;
        m[4] = 0.0f; m[5] = z;    m[6] = 0.0f;
        m[8] = 0.0f; m[9] = 0.0f; m[10] = z;
    }
}

/* roll around Z — reference Action.Bone.roll(angle radians) */
static void mc_mat_roll(float* m, float angle) {
    float s = sinf(angle), c = cosf(angle);
    float m00 = m[0], m01 = m[1], m10 = m[4], m11 = m[5], m20 = m[8], m21 = m[9];
    m[0] = m00 * c + m01 * s;
    m[1] = m01 * c - m00 * s;
    m[4] = m10 * c + m11 * s;
    m[5] = m11 * c - m10 * s;
    m[8] = m20 * c + m21 * s;
    m[9] = m21 * c - m20 * s;
}

/* row-major 3x4 multiply: out = l * r (translation column carried) */
static void mc_mat_mul12(float* out, const float* l, const float* r) {
    float res[12];
    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < 3; col++) {
            float v = 0.0f;
            for (int k = 0; k < 3; k++) v += l[row * 4 + k] * r[k * 4 + col];
            res[row * 4 + col] = v;
        }
        res[row * 4 + 3] = l[row * 4 + 0] * r[3] + l[row * 4 + 1] * r[7] +
                           l[row * 4 + 2] * r[11] + l[row * 4 + 3];
    }
    memcpy(out, res, sizeof(float[12]));
}

/* Evaluate one bone's animated matrix at kgf (frame/65536). */
static void mc_bone_anim_matrix(const MCBoneAnim* ba, float kgf, float* m) {
    memset(m, 0, sizeof(float[12]));
    float arr[3];
    switch (ba->type) {
        case 0:
            memcpy(m, ba->mat, sizeof(float[12]));
            break;
        case 1:
            m[0] = m[5] = m[10] = 1.0f;
            break;
        case 2: {
            mc_anim_get(&ba->translate, kgf, arr);
            m[3] = arr[0]; m[7] = arr[1]; m[11] = arr[2];
            mc_anim_get(&ba->rotate, kgf, arr);
            mc_mat_rotate_to_z(m, arr[0], arr[1], arr[2]);
            mc_mat_roll(m, mc_roll_get(&ba->roll, kgf));
            mc_anim_get(&ba->scale, kgf, arr);
            for (int r = 0; r < 3; r++) {
                m[r * 4 + 0] *= arr[0];
                m[r * 4 + 1] *= arr[1];
                m[r * 4 + 2] *= arr[2];
            }
            break;
        }
        case 3: {
            m[3] = ba->staticTrans[0]; m[7] = ba->staticTrans[1]; m[11] = ba->staticTrans[2];
            mc_anim_get(&ba->rotate, kgf, arr);
            mc_mat_rotate_to_z(m, arr[0], arr[1], arr[2]);
            mc_mat_roll(m, ba->staticRoll);
            break;
        }
        case 4:
        case 5:
        case 6: {
            if (ba->type == 6) {
                mc_anim_get(&ba->translate, kgf, arr);
                m[3] = arr[0]; m[7] = arr[1]; m[11] = arr[2];
            }
            mc_anim_get(&ba->rotate, kgf, arr);
            mc_mat_rotate_to_z(m, arr[0], arr[1], arr[2]);
            if (ba->type != 5) mc_mat_roll(m, mc_roll_get(&ba->roll, kgf));
            break;
        }
        default:
            m[0] = m[5] = m[10] = 1.0f;
            break;
    }
}

/* Apply an action pose to a figure: per bone
 *   final = (parentFinal * bind) * action
 * and transform the bone's vertex range + normals (reference utils.cpp). */
static void mc_apply_posture(MCFigure* fig, const MCAction* act, int frame16) {
    if (!fig || !act || !fig->bones || fig->num_bones <= 0) return;
    if (!fig->verts_bind) return;
    float kgf = (float)(frame16 < 0 ? 0 : frame16) / 65536.0f;
    int nb = fig->num_bones;
    float* final = (float*)calloc((size_t)nb * 12, sizeof(float));
    float* action = (float*)calloc((size_t)nb * 12, sizeof(float));
    if (!final || !action) { free(final); free(action); return; }

    for (int i = 0; i < nb; i++) {
        mc_bone_anim_matrix(&act->bones[i], kgf, action + (size_t)i * 12);
    }
    /* bones are stored in file order; parents always precede children */
    for (int i = 0; i < nb; i++) {
        const MCBone* b = &fig->bones[i];
        float* dst = final + (size_t)i * 12;
        if (b->parent < 0 || b->parent >= i) {
            mc_mat_mul12(dst, b->bind, action + (size_t)i * 12);
        } else {
            float tmp[12];
            mc_mat_mul12(tmp, final + (size_t)b->parent * 12, b->bind);
            mc_mat_mul12(dst, tmp, action + (size_t)i * 12);
        }
        const float* m = dst;
        for (int v = b->first; v < b->first + b->count && v < fig->vert_count; v++) {
            float x = fig->verts_bind[(size_t)v * 3];
            float y = fig->verts_bind[(size_t)v * 3 + 1];
            float z = fig->verts_bind[(size_t)v * 3 + 2];
            fig->verts[(size_t)v * 3 + 0] = m[0] * x + m[1] * y + m[2] * z + m[3];
            fig->verts[(size_t)v * 3 + 1] = m[4] * x + m[5] * y + m[6] * z + m[7];
            fig->verts[(size_t)v * 3 + 2] = m[8] * x + m[9] * y + m[10] * z + m[11];
            if (fig->normals && fig->normals_bind) {
                float nx = fig->normals_bind[(size_t)v * 3];
                float ny = fig->normals_bind[(size_t)v * 3 + 1];
                float nz = fig->normals_bind[(size_t)v * 3 + 2];
                float ox = m[0] * nx + m[1] * ny + m[2] * nz;
                float oy = m[4] * nx + m[5] * ny + m[6] * nz;
                float oz = m[8] * nx + m[9] * ny + m[10] * nz;
                float len = sqrtf(ox * ox + oy * oy + oz * oz);
                if (len > 1e-9f) { ox /= len; oy /= len; oz /= len; }
                fig->normals[(size_t)v * 3 + 0] = ox;
                fig->normals[(size_t)v * 3 + 1] = oy;
                fig->normals[(size_t)v * 3 + 2] = oz;
            }
        }
    }
    free(final);
    free(action);
}

/* ----------------------------------------------------------------------------
 * MC render context (no Java refs — GC-safe)
 * -------------------------------------------------------------------------- */
/* per-session command histograms (always on, printed at exit-ish moments) */
static long mc_cmd_hist[16]; /* indexed by (command>>16)&0xF after masking type+attrs */
static long mc_fig_renders = 0;
static long mc_fig_creates = 0;
static long mc_tex_creates = 0;
static long mc_tra_creates = 0;

/* v36.22 [MC-RESET] DIAG: process-global ctor counters silenced session 2+
 * of a multi-session frontend run ("Figure#N parsed" printed only while
 * mc_fig_creates <= 6) — exactly the launches a field bug report is about.
 * Reset them at the session boundary so EVERY session prints the startup
 * diagnostics again. Pure logging state — no native data is freed here
 * (the pools themselves intentionally survive for handles still alive). */
void mc_diag_session_reset(void) {
    mc_fig_creates = 0;
    mc_tra_creates = 0;
}

static int mc_trace_on(void); /* forward decl (defined near renderPrimitives) */

static void mc_hist_note(int command) {
    int type = (command & MC_PRIMITVE_TYPE_MASK) >> 24; /* 1..5 */
    if (type >= 1 && type <= 5) mc_cmd_hist[type]++;
    else mc_cmd_hist[0]++;
}

typedef struct {
    MidpGraphics* gfx;   /* bound target (stable C pointer) */
    int bound;
    int vw, vh;
    int frames;          /* diagnostics */
    long tris, pixels;
} McCtx;

static McCtx mc;

static int mc_trace_on(void);     /* forward decl (defined near renderPrimitives) */
static int mc_trace_from(void);   /* forward decl */
static int mc_trace_batches;      /* forward decl */

static void mc_diag_frame(void) {
    mc.frames++;
    if (mc_trace_on() && mc_trace_from() > 0) {
        /* flush marker so batch traces can be grouped per 3D frame */
        fprintf(stderr, MC_TAG "FLUSH frame=%d batches_so_far=%d\n",
                mc.frames, mc_trace_batches);
    }
    if (mc.frames <= 4 || (mc.frames % 600) == 0) {
        fprintf(stderr, MC_TAG "frame %d: tris=%ld pixels=%d RJ(c2s=%ld depth=%ld cull=%ld off=%ld area=%ld pass=%ld) buf0=%08X\n",
                mc.frames, mc.tris, g_m3g_raster_pixels_written,
                g_m3g_rj_clip2screen, g_m3g_rj_depth, g_m3g_rj_cull, g_m3g_rj_offscreen,
                g_m3g_rj_area, g_m3g_rj_pass,
                g_m3g.color_buffer ? g_m3g.color_buffer[0] : 0);
        if (mc_trace_on() && (mc.frames % 600) == 0) {
            fprintf(stderr, MC_TAG "HIST frames=%d points=%ld lines=%ld tris=%ld quads=%ld sprites=%ld other=%ld figures=%ld\n",
                    mc.frames, mc_cmd_hist[1], mc_cmd_hist[2], mc_cmd_hist[3], mc_cmd_hist[4],
                    mc_cmd_hist[5], mc_cmd_hist[0], mc_fig_renders);
        }
    }
}

/* (Re)allocate the shared rasterizer buffers for the bound target and
 * seed the color buffer from the Graphics content (2D background shows
 * through transparent regions, like M3G bindTarget v25). */
static int mc_bind_buffers(void) {
    if (!mc.gfx || !mc.gfx->pixels || mc.gfx->width <= 0 || mc.gfx->height <= 0) {
        return -1;
    }
    int w = mc.gfx->width, h = mc.gfx->height;
    if (w > M3G_MAX_IMAGE_DIMENSION) w = M3G_MAX_IMAGE_DIMENSION;
    if (h > M3G_MAX_IMAGE_DIMENSION) h = M3G_MAX_IMAGE_DIMENSION;

    if (!g_m3g.color_buffer || !g_m3g.depth_buffer ||
        g_m3g.buffer_width != w || g_m3g.buffer_height != h ||
        !g_m3g.buffers_allocated) {
        free(g_m3g.color_buffer);
        free(g_m3g.depth_buffer);
        g_m3g.color_buffer = (uint32_t*)malloc((size_t)w * h * sizeof(uint32_t));
        g_m3g.depth_buffer = (float*)malloc((size_t)w * h * sizeof(float));
        if (!g_m3g.color_buffer || !g_m3g.depth_buffer) {
            free(g_m3g.color_buffer); free(g_m3g.depth_buffer);
            g_m3g.color_buffer = NULL; g_m3g.depth_buffer = NULL;
            g_m3g.buffers_allocated = 0;
            return -1;
        }
        g_m3g.buffer_width = w;
        g_m3g.buffer_height = h;
        g_m3g.buffers_allocated = 1;
    }

    /* Fresh frame: far depth, 2D content as base */
    for (int i = 0; i < w * h; i++) g_m3g.depth_buffer[i] = 1.0f;
    for (int y = 0; y < h; y++) {
        memcpy(g_m3g.color_buffer + (size_t)y * w,
               mc.gfx->pixels + (size_t)y * mc.gfx->width,
               (size_t)w * sizeof(uint32_t));
    }

    g_m3g.viewport_x = 0;
    g_m3g.viewport_y = 0;
    g_m3g.viewport_width = w;
    g_m3g.viewport_height = h;
    g_m3g.depth_test_enabled = 1;
    g_m3g.depth_write_enabled = 1;
    g_m3g.culling_enabled = 0; /* micro3d primitives are double-sided */

    mc.vw = w;
    mc.vh = h;
    return 0;
}

/* NOJME_MC_DUMP=N: dump the 3D color buffer every N-th flush (debug) */
static int mc_dump_every(void) {
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("NOJME_MC_DUMP");
        v = e ? atoi(e) : 0;
    }
    return v;
}

static void mc_blit_to_gfx(void) {
    if (!mc.gfx || !mc.gfx->pixels || !g_m3g.color_buffer) return;
    int w = g_m3g.buffer_width < mc.gfx->width ? g_m3g.buffer_width : mc.gfx->width;
    int h = g_m3g.buffer_height < mc.gfx->height ? g_m3g.buffer_height : mc.gfx->height;
    if (mc_dump_every() > 0 && (mc.frames % mc_dump_every()) == 0) {
        char path[64];
        snprintf(path, sizeof(path), "/tmp/mc3d_%06d.ppm", mc.frames);
        FILE* f = fopen(path, "wb");
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", g_m3g.buffer_width, g_m3g.buffer_height);
            for (int y = 0; y < g_m3g.buffer_height; y++) {
                for (int x = 0; x < g_m3g.buffer_width; x++) {
                    uint32_t p = g_m3g.color_buffer[(size_t)y * g_m3g.buffer_width + x];
                    fputc((int)(p >> 16) & 0xFF, f);
                    fputc((int)(p >> 8) & 0xFF, f);
                    fputc((int)p & 0xFF, f);
                }
            }
            fclose(f);
        }
    }
    for (int y = 0; y < h; y++) {
        memcpy(mc.gfx->pixels + (size_t)y * mc.gfx->width,
               g_m3g.color_buffer + (size_t)y * g_m3g.buffer_width,
               (size_t)w * sizeof(uint32_t));
    }
}

/* ----------------------------------------------------------------------------
 * Projection / transform pipeline
 * -------------------------------------------------------------------------- */

/* View matrix snapshot: rows in float (rotation divided by 4096,
 * translation raw). view = layout.affine (4.12 java fields). */
typedef struct {
    float r[3][3];
    float t[3];
} MCView;

static void mc_view_from_affine(const McA* a, MCView* v) {
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) v->r[r][c] = a->m[r * 4 + c] / 4096.0f;
        v->t[r] = (float)a->m[r * 4 + 3];
    }
}

/* Projection snapshot from the FigureLayout object */
typedef struct {
    int mode;        /* 0 parallel-scale, 1 parallel-size, 2 persp-fov, 3 persp-wh */
    float sx, sy;    /* screen pixel scale for parallel / clip scale for persp */
    float near, far;
    int cx, cy;      /* projection center (screen px) */
    int ox, oy;      /* per-call x/y offset */
    int scale_x, scale_y;
    int par_w, par_h;
    int persp_w, persp_h;
    int angle;
} MCProj;

static void mc_proj_finalize(MCProj* p); /* forward decl */
static void mc_proj_from_layout(JavaObject* layout, MCProj* p) {
    memset(p, 0, sizeof(*p));
    p->cx = mc_get_int(layout, "centerX", 0);
    p->cy = mc_get_int(layout, "centerY", 0);
    p->scale_x = mc_get_int(layout, "scaleX", 512);
    p->scale_y = mc_get_int(layout, "scaleY", 512);
    p->par_w = mc_get_int(layout, "parallelWidth", 512);
    p->par_h = mc_get_int(layout, "parallelHeight", 512);
    p->near = (float)mc_get_int(layout, "near", 1);
    p->far = (float)mc_get_int(layout, "far", 10000);
    p->persp_w = mc_get_int(layout, "perspectiveWidth", 0);
    p->persp_h = mc_get_int(layout, "perspectiveHeight", 0);
    p->angle = mc_get_int(layout, "angle", 0);

    int projection = mc_get_int(layout, "projection", 0);
    switch (projection) {
        case (int)0x91000000: p->mode = 1; break; /* COMMAND_PARALLEL_SIZE */
        case (int)0x92000000: p->mode = 2; break; /* COMMAND_PERSPECTIVE_FOV */
        case (int)0x93000000: p->mode = 3; break; /* COMMAND_PERSPECTIVE_WH */
        default: p->mode = 0; break;              /* scale (default 512) */
    }

    mc_proj_finalize(p);
}

/* (Re)compute the clip scales for the current MCProj (extracted so
 * drawCommandList state commands can reconfigure the projection). */
static void mc_proj_finalize(MCProj* p) {
    /* Precompute per-viewport clip scales.
     * Perspective FOV: K = vw / (2*tan(angle/2)); clip_x scale 2K/vw,
     * clip_y scale 2K/vh (square pixels), depth linear near..far.
     * The viewport center offset is applied through g_m3g.viewport_*,
     * which is restored per draw call (MC centers vary per layout). */
    if (p->mode == 2) {
        float half = (float)p->angle * 3.14159265f / 4096.0f; /* half-fov rad */
        float t = tanf(half);
        if (t < 0.0001f) t = 0.0001f;
        float K = (float)mc.vw / (2.0f * t);
        p->sx = 2.0f * K / (float)mc.vw;   /* = 1/tan(half) */
        p->sy = 2.0f * K / (float)mc.vh;
    } else if (p->mode == 3) {
        /* near-surface width/height in 4.12 units of world distance */
        float wu = p->persp_w / 4096.0f, hu = p->persp_h / 4096.0f;
        if (wu < 1e-6f) wu = 1.0f;
        if (hu < 1e-6f) hu = wu * (float)mc.vh / (float)mc.vw;
        p->sx = 2.0f * p->near / wu;
        p->sy = 2.0f * p->near / hu;
    } else if (p->mode == 1) {
        /* parallel size: world width w spans the viewport */
        float wu = p->par_w / 4096.0f;
        if (wu < 1e-6f) wu = 1.0f;
        float hu = p->par_h / 4096.0f;
        if (hu < 1e-6f) hu = wu * (float)mc.vh / (float)mc.vw;
        p->sx = 2.0f / wu;
        p->sy = 2.0f / hu;
    } else {
        p->sx = (float)p->scale_x / 4096.0f; /* pixels per world unit */
        p->sy = (float)p->scale_y / 4096.0f;
    }
}

/* Effect snapshot */
typedef struct {
    int shading;
    int transparency;    /* colorkey skip enabled */
    int toon_threshold, toon_high, toon_low;
    int has_light;
    float light_dir[3];  /* normalized NEGATED direction (lambert vector) */
    float amb;           /* 0..1 */
    float diri;          /* 0..4 */
} MCEffect;

static void mc_effect_from_obj(JavaObject* effect, MCEffect* e) {
    memset(e, 0, sizeof(*e));
    if (!effect) return;
    e->shading = mc_get_int(effect, "shadingType", 0);
    e->transparency = mc_get_int(effect, "transparency", 1);
    e->toon_threshold = mc_get_int(effect, "toonThreshold", -1);
    e->toon_high = mc_get_int(effect, "toonHigh", 255);
    e->toon_low = mc_get_int(effect, "toonLow", 0);
    JavaObject* light = mc_get_ref(effect, "light");
    if (light) {
        JavaObject* dirv = mc_get_ref(light, "lightDir");
        if (!dirv) dirv = mc_get_ref(light, "direction");
        float dx = 0, dy = 0, dz = 4096;
        if (dirv) {
            dx = (float)mc_get_int(dirv, "x", 0);
            dy = (float)mc_get_int(dirv, "y", 0);
            dz = (float)mc_get_int(dirv, "z", 4096);
        }
        float len = sqrtf(dx * dx + dy * dy + dz * dz);
        if (len > 1e-9f) {
            /* reference: uLightDir = -normalize(light.direction) */
            e->light_dir[0] = -dx / len;
            e->light_dir[1] = -dy / len;
            e->light_dir[2] = -dz / len;
        } else {
            e->light_dir[0] = 0; e->light_dir[1] = 0; e->light_dir[2] = -1;
        }
        float di = (float)mc_get_int(light, "dirIntensity", 4096);
        float ai = (float)mc_get_int(light, "ambIntensity", 0);
        if (di < 0) di = 0;
        if (di > 16384) di = 16384;
        if (ai < 0) ai = 0;
        if (ai > 4096) ai = 4096;
        e->diri = di / 4096.0f;
        e->amb = ai / 4096.0f;
        e->has_light = 1;
    }
}

/* ---- vertex pipeline ----------------------------------------------------- */

typedef struct {
    float view[3];
    float uv[2];    /* normalized */
    uint8_t col[4]; /* gouraud color */
    int has_uv;
} MCRVertex;

/* Transform + project one view vertex into CLIP coordinates for the
 * rasterizer (m3g_clip_to_screen convention). Returns 0 ok. */
static int mc_project(const MCProj* p, const float v[3], float out[4]) {
    if (p->mode >= 2) {
        /* Perspective: w = view z (positive forward, +z into the screen).
         * Linear depth mapped into NDC [-1, 1] across near..far. */
        float z = v[2];
        if (z < 0.001f) z = 0.001f;
        out[0] = v[0] * p->sx;
        /* v34.31 FIX: micro3d v3 view +Y is screen-DOWN. lookAt builds
         * the up row as look x right = -Y for a Y-up world, so a floor
         * below the eye lands at POSITIVE view y and must render BELOW
         * the projection center: negate clip_y (clip_to_screen maps
         * ndc_y +1 to the top). */
        out[1] = -v[1] * p->sy;
        float range = p->far - p->near;
        if (range < 1.0f) range = 1.0f;
        float ndc_z = 2.0f * (z - p->near) / range - 1.0f;
        out[2] = ndc_z * z;
        out[3] = z;
    } else {
        /* Parallel: w = 1. PARALLEL_SIZE uses raw world units for the
         * surface; PARALLEL_SCALE (default 512) spans
         * vw*4096/scaleX world units across the viewport. */
        float fx, fy;
        if (p->mode == 1) {
            float wu = (float)p->par_w;
            float hu = (float)p->par_h;
            if (wu < 1e-6f) wu = 1.0f;
            if (hu < 1e-6f) hu = wu * (float)mc.vh / (float)mc.vw;
            fx = 2.0f / wu;
            fy = 2.0f / hu;
        } else {
            fx = 2.0f * (p->scale_x / 4096.0f) / (float)mc.vw;
            fy = 2.0f * (p->scale_y / 4096.0f) / (float)mc.vh;
        }
        out[0] = v[0] * fx;
        out[1] = -v[1] * fy; /* view +y is screen-down (see perspective) */
        /* parallel depth: tiny linear z like the reference (1/65536) */
        out[2] = v[2] / 65536.0f;
        out[3] = 1.0f;
    }
    return 0;
}

/* Set the rasterizer viewport so projected NDC lands at layout center. */
static void mc_set_viewport(const MCProj* p) {
    g_m3g.viewport_x = p->cx + p->ox - mc.vw / 2;
    g_m3g.viewport_y = p->cy + p->oy - mc.vh / 2;
    g_m3g.viewport_width = mc.vw;
    g_m3g.viewport_height = mc.vh;
    /* clip_to_screen maps NDC y to (1-y)/2*vh + vy; MC +y is up:
     * our clip_y is built with +sy so NDC y already encodes "up" the
     * GL way — no extra flip needed. */
}

/* Lighting for one vertex normal (normalized floats). Returns 0..1. */
static float mc_light_vertex(const MCEffect* e, const float n[3]) {
    if (!e->has_light) return 1.0f;
    float d = n[0] * e->light_dir[0] + n[1] * e->light_dir[1] + n[2] * e->light_dir[2];
    if (d < 0) d = 0;
    float l = e->amb + e->diri * d;
    return l > 1.0f ? 1.0f : l;
}

/* Toon quantization */
static float mc_toon(const MCEffect* e, float light) {
    if (e->toon_threshold < 0 || e->shading != MC_TOON_SHADING) return light;
    return (light * 255.0f < (float)e->toon_threshold)
               ? e->toon_low / 255.0f : e->toon_high / 255.0f;
}

/* ---- near/far clipping (view space, attributes interpolated linearly) --- */

typedef struct {
    float pos[3];
    float uv[2];
    float col[4]; /* rgba 0..255, gouraud */
} MCV;

/* Sutherland-Hodgman against z >= znear and z <= zfar. Returns the number
 * of output vertices (fan: v0, v[i], v[i+1]), or 0 if fully clipped. */
static int mc_clip_vert_fan(const MCV* in, int n, MCV* out, float znear, float zfar) {
    MCV tmp[16];
    int tn = 0;
    /* plane 1: z >= znear */
    for (int i = 0; i < n; i++) {
        const MCV* a = &in[i];
        const MCV* b = &in[(i + 1) % n];
        float da = a->pos[2] - znear;
        float db = b->pos[2] - znear;
        if (da >= 0.0f) tmp[tn++] = *a;
        if ((da >= 0.0f) != (db >= 0.0f)) {
            float t = da / (da - db);
            MCV* v = &tmp[tn++];
            for (int k = 0; k < 3; k++) v->pos[k] = a->pos[k] + (b->pos[k] - a->pos[k]) * t;
            for (int k = 0; k < 2; k++) v->uv[k] = a->uv[k] + (b->uv[k] - a->uv[k]) * t;
            for (int k = 0; k < 4; k++) v->col[k] = a->col[k] + (b->col[k] - a->col[k]) * t;
        }
    }
    if (tn < 3) return 0;
    /* plane 2: z <= zfar */
    int on = 0;
    for (int i = 0; i < tn; i++) {
        const MCV* a = &tmp[i];
        const MCV* b = &tmp[(i + 1) % tn];
        float da = zfar - a->pos[2];
        float db = zfar - b->pos[2];
        if (da >= 0.0f) out[on++] = *a;
        if ((da >= 0.0f) != (db >= 0.0f)) {
            float t = da / (da - db);
            MCV* v = &out[on++];
            for (int k = 0; k < 3; k++) v->pos[k] = a->pos[k] + (b->pos[k] - a->pos[k]) * t;
            for (int k = 0; k < 2; k++) v->uv[k] = a->uv[k] + (b->uv[k] - a->uv[k]) * t;
            for (int k = 0; k < 4; k++) v->col[k] = a->col[k] + (b->col[k] - a->col[k]) * t;
        }
    }
    return (on >= 3) ? on : 0;
}

/* Per-draw-call state (set by the render entry points before triangles) */
static const MCProj* mc_cur_proj = NULL;
static MCProj mc_cur_proj_snap; /* storage so mc_cur_proj never dangles */
static int mc_cur_colorkey = 0;

/* Draw one fully-specified triangle through the shared rasterizer.
 * v34.32: ck = immediate-mode (renderPrimitives) colorkey/semi-transparent
 * primitive. Duke Nukem 3D proved the real-engine rule: such prims DEPTH
 * TEST but do NOT WRITE (classic transparent-geometry rule). The game draws
 * its sky quad FIRST at view z=48 (nearest) as a full-screen backdrop and
 * then renders the sorted opaque level on top; with a depth write the sky
 * stars would reject every wall (z-farther) and the starfield would show
 * THROUGH the level = the "noise overlay over the 3D scene" bug.
 * renderFigure passes ck=0 (figures keep full z-buffer semantics, verified
 * in v34.31). */
static void mc_raster_tri(const MCV* v0, const MCV* v1, const MCV* v2,
                          M3GTexture2D* texture, int texture_blend,
                          int use_vertex_colors, int blend_half,
                          int add, int sub, int ck) {
    MCV fan[3] = { *v0, *v1, *v2 };
    MCV clipped[16];

    float znear = 1.0f, zfar = 1e9f;
    /* perspective layouts have real near/far; parallel keeps 1..huge */
    if (mc_cur_proj) {
        znear = mc_cur_proj->near > 0.0f ? mc_cur_proj->near : 1.0f;
        zfar = mc_cur_proj->far > znear ? mc_cur_proj->far : 1e9f;
    }

    int n = mc_clip_vert_fan(fan, 3, clipped, znear, zfar);
    if (n < 3) return;

    M3GAppearance app;
    memset(&app, 0, sizeof(app));
    app.texture = texture;
    app.blend_mode = 64; /* ALPHA (src-over with paint alpha) */
    app.texture_blend = texture_blend;
    app.alpha_threshold = mc_cur_colorkey ? 128 : -1;
    app.alpha_factor = blend_half ? 0.5f : 1.0f;
    app.two_sided_lighting = 1; /* no culling for micro3d primitives */
    app.winding = 0;

    int saved_test = g_m3g.depth_test_enabled;
    int saved_write = g_m3g.depth_write_enabled;
    /* Classic transparent-primitive rules: blended prims neither test nor
     * write depth (painter ordering). ADD/SUB also disable both.
     * v34.32: colorkey'd/semi-transparent immediate primitives keep the
     * depth TEST (so sprites stay occluded by nearer opaque geometry) but
     * skip the depth WRITE (so a full-screen key'd backdrop like the sky
     * cannot reject the opaque level drawn afterwards). */
    if (blend_half || add || sub) {
        g_m3g.depth_test_enabled = 0;
        g_m3g.depth_write_enabled = 0;
        if (add) app.blend_mode = 65;      /* ALPHA_ADD */
        else if (sub) app.blend_mode = 69; /* subtract proxy */
    } else if (ck) {
        g_m3g.depth_write_enabled = 0;
    }

    for (int i = 1; i + 1 < n; i++) {
        float c0[4], c1[4], c2[4];
        mc_project(mc_cur_proj, clipped[0].pos, c0);
        mc_project(mc_cur_proj, clipped[i].pos, c1);
        mc_project(mc_cur_proj, clipped[i + 1].pos, c2);

        float t0[2] = { clipped[0].uv[0], clipped[0].uv[1] };
        float t1[2] = { clipped[i].uv[0], clipped[i].uv[1] };
        float t2[2] = { clipped[i + 1].uv[0], clipped[i + 1].uv[1] };

        uint8_t col0[4], col1[4], col2[4];
        for (int k = 0; k < 4; k++) {
            int a = (int)clipped[0].col[k]; if (a < 0) a = 0; if (a > 255) a = 255;
            int b = (int)clipped[i].col[k]; if (b < 0) b = 0; if (b > 255) b = 255;
            int c = (int)clipped[i + 1].col[k]; if (c < 0) c = 0; if (c > 255) c = 255;
            col0[k] = (uint8_t)a; col1[k] = (uint8_t)b; col2[k] = (uint8_t)c;
        }

        m3g_rasterize_triangle(c0, c1, c2,
                               t0, t1, t2,
                               NULL, NULL, NULL,
                               use_vertex_colors ? col0 : NULL,
                               use_vertex_colors ? col1 : NULL,
                               use_vertex_colors ? col2 : NULL,
                               &app, NULL, NULL, NULL);
        mc.tris++;
    }

    g_m3g.depth_test_enabled = saved_test;
    g_m3g.depth_write_enabled = saved_write;
}

/* ----------------------------------------------------------------------------
 * Graphics3D entry points (native handlers)
 * -------------------------------------------------------------------------- */

static JavaValue native_mc_g3d_init(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    return NATIVE_RETURN_VOID();
}

static JavaValue native_mc_g3d_bind(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* gobj = (JavaObject*)args[1].ref;
    if (!gobj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    extern MidpGraphics* get_graphics_from_object(JavaObject* obj);
    MidpGraphics* gfx = get_graphics_from_object(gobj);
    if (!gfx) {
        fprintf(stderr, MC_TAG "bind: no MidpGraphics for target\n");
        return NATIVE_RETURN_VOID();
    }
    mc.gfx = gfx;
    mc.bound = 1;
    if (mc_bind_buffers() != 0) {
        fprintf(stderr, MC_TAG "bind: buffer alloc failed (%dx%d)\n",
                gfx ? gfx->width : 0, gfx ? gfx->height : 0);
        mc.bound = 0;
    }
    return NATIVE_RETURN_VOID();
}

static JavaValue native_mc_g3d_flush(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    if (mc.bound) {
        mc_blit_to_gfx();
        mc_diag_frame();
    }
    return NATIVE_RETURN_VOID();
}

static JavaValue native_mc_g3d_release(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    if (mc.bound) {
        mc_blit_to_gfx();
        mc_diag_frame();
    }
    mc.gfx = NULL;
    mc.bound = 0;
    return NATIVE_RETURN_VOID();
}

/* --- renderPrimitives ------------------------------------------------------ */

static int mc_arr_ints(JavaArray* arr, jint** out) {
    if (!arr || arr->element_type != T_INT) return -1;
    *out = (jint*)array_data((JavaArray*)arr);
    return (int)arr->length;
}

static void mc_vcol_from_rgb(float col[4], jint rgb, float light, int half) {
    col[0] = ((rgb >> 16) & 0xFF) * light;
    col[1] = ((rgb >> 8) & 0xFF) * light;
    col[2] = (rgb & 0xFF) * light;
    col[3] = half ? 128.0f : 255.0f;
}

static void mc_vcol_light(float col[4], float light, int half) {
    col[0] = col[1] = col[2] = 255.0f * light;
    col[3] = half ? 128.0f : 255.0f;
}

/* per-session trace for the first few primitive batches (NOJME_TRACE_MC=1).
 * NOJME_TRACE_MC_FROM/N lets the window start at a later frame so gameplay
 * (rather than the session start) is traced. */
static int mc_trace_on(void) {
    static int v = -1;
    if (v < 0) v = getenv("NOJME_TRACE_MC") ? 1 : 0;
    return v;
}
static int mc_trace_from(void) {
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("NOJME_TRACE_MC_FROM");
        v = e ? atoi(e) : 0;
    }
    return v;
}
static int mc_trace_batches = 0;
#define MC_TRACE_BATCH_LIMIT (mc_trace_from() ? 100000 : 6)

/* --- POINTS / LINES rasterization (v34.32; colored, depth-aware) ---------
 * Reference Render.java: points and lines ignore textures/normals and are
 * drawn with per-face/per-command colors only. Blended (HALF/ADD/SUB)
 * primitives and colorkey'd ones follow the same depth rules as triangles. */
static void mc_plot_px(int x, int y, jint rgb, int blend_half, int add, int sub,
                       int depth_test, int depth_write, float sd) {
    if (x < 0 || y < 0 || x >= g_m3g.buffer_width || y >= g_m3g.buffer_height) return;
    size_t i = (size_t)y * g_m3g.buffer_width + x;
    if (depth_test && g_m3g.depth_buffer) {
        if (sd >= g_m3g.depth_buffer[i]) return;
    }
    int r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    uint32_t dst = g_m3g.color_buffer[i];
    int dr = (int)(dst >> 16) & 0xFF, dg = (int)(dst >> 8) & 0xFF, db = (int)dst & 0xFF;
    if (blend_half) {
        r = (r + dr) / 2; g = (g + dg) / 2; b = (b + db) / 2;
    } else if (add) {
        r += dr; g += dg; b += db;
        if (r > 255) r = 255;
        if (g > 255) g = 255;
        if (b > 255) b = 255;
    } else if (sub) {
        r = dr - r; g = dg - g; b = db - b;
        if (r < 0) r = 0;
        if (g < 0) g = 0;
        if (b < 0) b = 0;
    }
    g_m3g.color_buffer[i] = 0xFF000000u | ((uint32_t)r << 16) |
                            ((uint32_t)g << 8) | (uint32_t)b;
    if (depth_write && g_m3g.depth_buffer) g_m3g.depth_buffer[i] = sd;
    mc.tris++; /* count as raster work for the diagnostics */
}

static void mc_draw_point(const MCView* view, const MCProj* proj, const MCV* v,
                          jint color, int blend_half, int add, int sub,
                          int colorkey) {
    (void)colorkey; /* points are untextured: key n/a */
    (void)view;     /* view transform already applied to v */
    float clip[4];
    mc_project(proj, v->pos, clip);
    if (clip[3] <= 0.001f) return;
    float ndc_z = clip[2] / clip[3];
    float sd = (ndc_z + 1.0f) * 0.5f;
    if (sd < 0.0f || sd > 1.0f) return;
    int sx = (int)(((clip[0] / clip[3] + 1.0f) * 0.5f * mc.vw) +
                   (proj->cx + proj->ox - mc.vw / 2) + 0.5f);
    int sy = (int)(((1.0f - clip[1] / clip[3]) * 0.5f * mc.vh) +
                   (proj->cy + proj->oy - mc.vh / 2) + 0.5f);
    if (sx < 0 || sy < 0 || sx >= mc.vw || sy >= mc.vh) return;
    int dt = g_m3g.depth_test_enabled, dw = g_m3g.depth_write_enabled;
    if (blend_half || add || sub) { dt = 0; dw = 0; }
    mc_plot_px(sx, sy, color, blend_half, add, sub, dt, dw, sd);
}

static void mc_draw_line(const MCView* view, const MCProj* proj,
                         const MCV* v0, const MCV* v1, jint color,
                         int blend_half, int add, int sub, int colorkey) {
    (void)colorkey;
    float c0[4], c1[4];
    mc_project(proj, v0->pos, c0);
    mc_project(proj, v1->pos, c1);
    if (c0[3] <= 0.001f || c1[3] <= 0.001f) return;
    /* near-plane clip in screen space is approximate: skip fully-behind lines */
    float x0 = (c0[0] / c0[3] + 1.0f) * 0.5f * mc.vw + (proj->cx + proj->ox - mc.vw / 2);
    float y0 = (1.0f - c0[1] / c0[3]) * 0.5f * mc.vh + (proj->cy + proj->oy - mc.vh / 2);
    float x1 = (c1[0] / c1[3] + 1.0f) * 0.5f * mc.vw + (proj->cx + proj->ox - mc.vw / 2);
    float y1 = (1.0f - c1[1] / c1[3]) * 0.5f * mc.vh + (proj->cy + proj->oy - mc.vh / 2);
    float z0 = (c0[2] / c0[3] + 1.0f) * 0.5f;
    float z1 = (c1[2] / c1[3] + 1.0f) * 0.5f;
    int ix0 = (int)(x0 + 0.5f), iy0 = (int)(y0 + 0.5f);
    int ix1 = (int)(x1 + 0.5f), iy1 = (int)(y1 + 0.5f);
    int dx = ix1 > ix0 ? ix1 - ix0 : ix0 - ix1;
    int dy = iy1 > iy0 ? iy1 - iy0 : iy0 - iy1;
    int sx = ix1 > ix0 ? 1 : -1;
    int sy = iy1 > iy0 ? 1 : -1;
    int err = dx - dy;
    int steps = dx > dy ? dx : dy;
    int dt = g_m3g.depth_test_enabled, dw = g_m3g.depth_write_enabled;
    if (blend_half || add || sub) { dt = 0; dw = 0; }
    int x = ix0, y = iy0;
    for (int i = 0; i <= steps; i++) {
        float t = steps > 0 ? (float)i / (float)steps : 0.0f;
        float sd = z0 + (z1 - z0) * t;
        mc_plot_px(x, y, color, blend_half, add, sub, dt, dw, sd);
        int e2 = err * 2;
        if (e2 > -dy) { err -= dy; x += sx; }
        if (e2 < dx) { err += dx; y += sy; }
    }
    (void)view; /* view transform already applied to v0/v1 */
}

/* ----------------------------------------------------------------------------
 * Immediate-primitive batch renderer (shared by renderPrimitives and
 * drawCommandList — v34.32). All Java state is already resolved.
 * -------------------------------------------------------------------------- */
static void mc_draw_prim_batch(const MCView* view, const MCProj* proj,
                               const MCEffect* eff, MCTexture* mctex,
                               jint command, jint numPrims,
                               const jint* verts, int vertsLen,
                               const jint* normals, int normalsLen,
                               const jint* texcoords, int texLen,
                               const jint* colors, int colLen) {
    if (!view || !proj || !verts || numPrims <= 0 || numPrims >= 256) return;

    int prim_type = command & MC_PRIMITVE_TYPE_MASK;
    int pattr_lighting = (command & MC_PATTR_LIGHTING) != 0;
    int blend_half = (command & MC_PATTR_BLEND_MASK) == MC_PATTR_BLEND_HALF;
    int blend_add = (command & MC_PATTR_BLEND_MASK) == MC_PATTR_BLEND_ADD;
    int blend_sub = (command & MC_PATTR_BLEND_MASK) == MC_PATTR_BLEND_SUB;
    int colorkey = (command & MC_PATTR_COLORKEY) != 0;
    int has_tex = (command & MC_PDATA_TEXCOORD_MASK) == MC_PDATA_TEXURE_COORD;
    int normal_mode = command & MC_PDATA_NORMAL_MASK; /* 0 / 0x200 / 0x300 */
    int color_mode = command & MC_PDATA_COLOR_MASK;   /* 0 / 0x400 / 0x800 */

    if (has_tex && !mctex) return; /* textured command, missing texture */

    mc_cur_proj_snap = *proj;
    mc_cur_proj = &mc_cur_proj_snap;
    mc_cur_colorkey = colorkey;
    if (!colorkey && eff && eff->transparency) mc_cur_colorkey = 1;
    colorkey = mc_cur_colorkey;

    jint cmd_color = 0xFFFFFF;
    if (color_mode == MC_PDATA_COLOR_PER_COMMAND && colors && colLen > 0) {
        cmd_color = colors[0];
    }

    mc_set_viewport(proj);

    float tex_w = mctex ? (float)mctex->tex.width : 1.0f;
    float tex_h = mctex ? (float)mctex->tex.height : 1.0f;
    int lit = pattr_lighting && eff && eff->has_light &&
              (normal_mode == MC_PDATA_NORMAL_PER_FACE ||
               normal_mode == MC_PDATA_NORMAL_PER_VERTEX);
    int texture_blend = lit ? 68 /* MODULATE */ : 67 /* REPLACE */;
    int use_vcol = lit || !has_tex; /* untextured primitives carry the color */

    int verts_per_prim = 1;
    switch (prim_type) {
        case MC_PRIMITVE_LINES: verts_per_prim = 2; break;
        case MC_PRIMITVE_TRIANGLES: verts_per_prim = 3; break;
        case MC_PRIMITVE_QUADS: verts_per_prim = 4; break;
        default: verts_per_prim = 1; break; /* points / point sprites */
    }

    if (mc_trace_on() && (unsigned)mc.frames >= (unsigned)mc_trace_from() &&
        mc_trace_batches < MC_TRACE_BATCH_LIMIT) {
        mc_trace_batches++;
        static int mc_trace_summary = -1;
        if (mc_trace_summary < 0) {
            const char* e = getenv("NOJME_TRACE_MC_SUMMARY");
            mc_trace_summary = e ? atoi(e) : 0;
        }
        if (mc_trace_summary) {
            fprintf(stderr, MC_TAG "RP #%d cmd=%08X n=%d key=%d half=%d texw=%d\n",
                    mc_trace_batches, command, numPrims, colorkey, blend_half,
                    mctex ? mctex->tex.width : -1);
        } else {
            fprintf(stderr, MC_TAG "RP batch#%d cmd=%08X n=%d type=%06X tex=%s lit=%d half=%d key=%d "
                    "proj(mode=%d near=%d far=%d angle=%d cx=%d cy=%d)\n",
                    mc_trace_batches, command, numPrims, prim_type,
                    mctex ? "yes" : "no", lit, blend_half, colorkey,
                    proj->mode, (int)proj->near, (int)proj->far, proj->angle, proj->cx, proj->cy);
        }
    }

    for (int p = 0; p < numPrims; p++) {
        const jint* pv = verts + (size_t)p * verts_per_prim * 3;
        if ((size_t)p * verts_per_prim * 3 + verts_per_prim * 3 > (size_t)vertsLen) break;

        jint face_color = cmd_color;
        if (color_mode == MC_PDATA_COLOR_PER_FACE && colors && p < colLen) {
            face_color = colors[p];
        }

        /* per-face normal lighting factor */
        float light = 1.0f;
        if (lit && normal_mode == MC_PDATA_NORMAL_PER_FACE && normals &&
                   (size_t)(p + 1) * 3 <= (size_t)normalsLen) {
            float n[3] = {
                normals[p * 3 + 0] / 4096.0f,
                normals[p * 3 + 1] / 4096.0f,
                normals[p * 3 + 2] / 4096.0f
            };
            light = mc_toon(eff, mc_light_vertex(eff, n));
        }

        MCV out[4];
        for (int v = 0; v < verts_per_prim; v++) {
            MCV* o = &out[v];
            float wx = (float)pv[v * 3 + 0], wy = (float)pv[v * 3 + 1],
                  wz = (float)pv[v * 3 + 2];
            o->pos[0] = view->r[0][0] * wx + view->r[0][1] * wy + view->r[0][2] * wz + view->t[0];
            o->pos[1] = view->r[1][0] * wx + view->r[1][1] * wy + view->r[1][2] * wz + view->t[1];
            o->pos[2] = view->r[2][0] * wx + view->r[2][1] * wy + view->r[2][2] * wz + view->t[2];

            if (has_tex && mctex && texcoords &&
                (size_t)p * verts_per_prim * 2 + v * 2 + 1 < (size_t)texLen) {
                o->uv[0] = texcoords[(size_t)p * verts_per_prim * 2 + v * 2 + 0] / tex_w;
                o->uv[1] = texcoords[(size_t)p * verts_per_prim * 2 + v * 2 + 1] / tex_h;
            } else {
                o->uv[0] = o->uv[1] = 0.0f;
            }

            if (lit && normal_mode == MC_PDATA_NORMAL_PER_VERTEX && normals) {
                size_t idx = (size_t)p * verts_per_prim * 3 + v * 3;
                if (idx + 2 < (size_t)normalsLen) {
                    float n[3] = {
                        normals[idx + 0] / 4096.0f,
                        normals[idx + 1] / 4096.0f,
                        normals[idx + 2] / 4096.0f
                    };
                    float vl = mc_toon(eff, mc_light_vertex(eff, n));
                    if (has_tex) mc_vcol_light(o->col, vl, blend_half);
                    else mc_vcol_from_rgb(o->col, face_color, vl, blend_half);
                    continue;
                }
            }
            if (has_tex) mc_vcol_light(o->col, light, blend_half);
            else mc_vcol_from_rgb(o->col, face_color, 1.0f, blend_half);
        }

        M3GTexture2D* texture = mctex ? &mctex->tex : NULL;

        if (prim_type == MC_PRIMITVE_QUADS) {
            /* (A,B,C) + (A,C,D) */
            mc_raster_tri(&out[0], &out[1], &out[2], texture, texture_blend,
                          use_vcol, blend_half, blend_add, blend_sub, colorkey);
            mc_raster_tri(&out[0], &out[2], &out[3], texture, texture_blend,
                          use_vcol, blend_half, blend_add, blend_sub, colorkey);
        } else if (prim_type == MC_PRIMITVE_TRIANGLES) {
            mc_raster_tri(&out[0], &out[1], &out[2], texture, texture_blend,
                          use_vcol, blend_half, blend_add, blend_sub, colorkey);
        } else if (prim_type == MC_PRIMITVE_LINES) {
            mc_draw_line(view, proj, &out[0], &out[1], face_color, blend_half,
                         blend_add, blend_sub, colorkey);
        } else if (prim_type == MC_PRIMITVE_POINTS) {
            mc_draw_point(view, proj, &out[0], face_color, blend_half,
                          blend_add, blend_sub, colorkey);
        } else {
            /* point sprites: textured billboards sized by the 8-value
             * param blocks; not used by known v3 games through this path */
        }
    }
}

static JavaValue native_mc_renderPrimitives(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    if (!mc.bound || !mc.gfx) {
        /* real API: IllegalStateException; games that render before bind
         * are broken anyway — just ignore. */
        return NATIVE_RETURN_VOID();
    }

    JavaObject* tex_obj = (JavaObject*)args[1].ref;
    jint off_x = args[2].i;
    jint off_y = args[3].i;
    JavaObject* layout = (JavaObject*)args[4].ref;
    JavaObject* effect = (JavaObject*)args[5].ref;
    jint command = args[6].i;
    jint numPrims = args[7].i;
    JavaArray* vertsArr = (JavaArray*)args[8].ref;
    JavaArray* normalsArr = (JavaArray*)args[9].ref;
    JavaArray* texcoordsArr = (JavaArray*)args[10].ref;
    JavaArray* colorsArr = (JavaArray*)args[11].ref;

    if (!layout || !vertsArr) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    if (numPrims <= 0 || numPrims >= 256) return NATIVE_RETURN_VOID();
    mc_hist_note(command);

    jint* verts = NULL; int vertsLen = mc_arr_ints(vertsArr, &verts);
    jint* normals = NULL; int normalsLen = normalsArr ? mc_arr_ints(normalsArr, &normals) : -1;
    jint* texcoords = NULL; int texLen = texcoordsArr ? mc_arr_ints(texcoordsArr, &texcoords) : -1;
    jint* colors = NULL; int colLen = colorsArr ? mc_arr_ints(colorsArr, &colors) : -1;
    if (vertsLen < 0) return NATIVE_RETURN_VOID();

    MCTexture* mctex = NULL;
    if (tex_obj) {
        mctex = mc_texture_from_handle(mc_get_int(tex_obj, "texHandle", 0));
    }

    /* resolve layout: current affine (may be a selected array entry) */
    JavaObject* affine = mc_get_ref(layout, "affine");
    if (!affine) affine = mc_get_ref(layout, "affineArraySel");
    McA a;
    if (affine) {
        mc_affine_read(affine, &a);
    } else {
        for (int i = 0; i < 12; i++) a.m[i] = (i % 5 == 0) ? 4096 : 0; /* identity */
    }
    MCView view;
    mc_view_from_affine(&a, &view);

    MCProj proj;
    mc_proj_from_layout(layout, &proj);
    proj.ox = off_x;
    proj.oy = off_y;

    MCEffect eff;
    mc_effect_from_obj(effect, &eff);

    mc_draw_prim_batch(&view, &proj, &eff, mctex, command, numPrims,
                       verts, vertsLen, normals, normalsLen,
                       texcoords, texLen, colors, colLen);
    return NATIVE_RETURN_VOID();
}

/* --- renderFigure ----------------------------------------------------------- */

static MCTexture* mc_figure_texture(JavaObject* figure, int face) {
    JavaObject* arr = mc_get_ref(figure, "textures");
    if (!arr) return NULL;
    JavaArray* ta = (JavaArray*)arr;
    if (ta->element_type != DESC_OBJECT || ta->length <= 0) return NULL;
    int idx = mc_get_int(figure, "selTexture", -1);
    if (face >= 0 && face < ta->length) idx = face;
    if (idx < 0) idx = 0;
    if (idx >= ta->length) return NULL;
    JavaObject** elems = (JavaObject**)array_data(ta);
    JavaObject* texobj = elems ? elems[idx] : NULL;
    if (!texobj) return NULL;
    return mc_texture_from_handle(mc_get_int(texobj, "texHandle", 0));
}

static JavaValue native_mc_renderFigure(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    if (!mc.bound || !mc.gfx) return NATIVE_RETURN_VOID();

    JavaObject* figure = (JavaObject*)args[1].ref;
    jint off_x = args[2].i;
    jint off_y = args[3].i;
    JavaObject* layout = (JavaObject*)args[4].ref;
    JavaObject* effect = (JavaObject*)args[5].ref;

    if (!figure || !layout) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }

    MCFigure* fig = mc_figure_from_handle(mc_get_int(figure, "figHandle", 0));
    mc_fig_renders++;
    if (mc_trace_on() && mc_fig_renders <= 5) {
        fprintf(stderr, MC_TAG "renderFigure #%ld: fig=%p handle=%d polys=%d\n",
                mc_fig_renders, (void*)figure, mc_get_int(figure, "figHandle", 0),
                fig ? fig->poly_count : -1);
    }
    if (mc_trace_on() && (unsigned)mc.frames >= (unsigned)mc_trace_from() &&
        mc_trace_batches < MC_TRACE_BATCH_LIMIT) {
        mc_trace_batches++;
        fprintf(stderr, MC_TAG "RF figure=%p polys=%d verts=%d pattern=%d near=%d far=%dn",
                (void*)fig, fig ? fig->poly_count : 0, fig ? fig->vert_count : 0,
                mc_get_int(figure, "pattern", 0),
                (int)mc_get_int(layout, "near", 0), (int)mc_get_int(layout, "far", 0));
    }
    if (!fig || !fig->polys || fig->poly_count <= 0) return NATIVE_RETURN_VOID();

    JavaObject* affine = mc_get_ref(layout, "affine");
    McA a;
    if (affine) mc_affine_read(affine, &a);
    else for (int i = 0; i < 12; i++) a.m[i] = (i % 5 == 0) ? 4096 : 0;
    MCView view;
    mc_view_from_affine(&a, &view);

    MCProj proj;
    mc_proj_from_layout(layout, &proj);
    proj.ox = off_x;
    proj.oy = off_y;
    mc_cur_proj_snap = proj;
    mc_cur_proj = &mc_cur_proj_snap;

    MCEffect eff;
    mc_effect_from_obj(effect, &eff);
    mc_cur_colorkey = eff.transparency ? 1 : 0;

    mc_set_viewport(&proj);

    int pattern = mc_get_int(figure, "pattern", 0);

    for (int i = 0; i < fig->poly_count; i++) {
        MCPoly* poly = &fig->polys[i];
        if ((poly->pattern & pattern) != poly->pattern) continue;

        int material = poly->material;
        int colorkey = (material & MC_POLY_TRANSPARENT) || mc_cur_colorkey;
        mc_cur_colorkey = colorkey;
        int blend_half = (material & MC_POLY_BLEND_MASK) == MC_POLY_BLEND_HALF;
        int blend_add = (material & MC_POLY_BLEND_MASK) == MC_POLY_BLEND_ADD;
        int blend_sub = (material & MC_POLY_BLEND_MASK) == MC_POLY_BLEND_SUB;

        MCTexture* mctex = mc_figure_texture(figure, poly->face);
        if (!mctex && fig->polys[i].face != -1) continue; /* textured poly w/o texture */

        float tex_w = mctex ? (float)mctex->tex.width : 1.0f;
        float tex_h = mctex ? (float)mctex->tex.height : 1.0f;

        int nidx = poly->is_quad ? 4 : 3;
        MCV out[4];
        int lit = (material & MC_POLY_LIGHTING) && fig->normals && eff.has_light;

        for (int v = 0; v < nidx; v++) {
            int vi = poly->idx[v];
            if (vi < 0 || vi >= fig->vert_count) { vi = 0; }
            MCV* o = &out[v];
            float wx = fig->verts[(size_t)vi * 3 + 0];
            float wy = fig->verts[(size_t)vi * 3 + 1];
            float wz = fig->verts[(size_t)vi * 3 + 2];
            o->pos[0] = view.r[0][0] * wx + view.r[0][1] * wy + view.r[0][2] * wz + view.t[0];
            o->pos[1] = view.r[1][0] * wx + view.r[1][1] * wy + view.r[1][2] * wz + view.t[1];
            o->pos[2] = view.r[2][0] * wx + view.r[2][1] * wy + view.r[2][2] * wz + view.t[2];
            o->uv[0] = poly->uv[v * 2 + 0] / tex_w;
            o->uv[1] = poly->uv[v * 2 + 1] / tex_h;
            if (lit) {
                float n[3] = {
                    fig->normals[(size_t)vi * 3 + 0],
                    fig->normals[(size_t)vi * 3 + 1],
                    fig->normals[(size_t)vi * 3 + 2]
                };
                float l = mc_toon(&eff, mc_light_vertex(&eff, n));
                mc_vcol_light(o->col, l, blend_half);
            } else {
                mc_vcol_light(o->col, 1.0f, blend_half);
            }
        }

        M3GTexture2D* texture = mctex ? &mctex->tex : NULL;
        int texture_blend = lit ? 68 : 67;
        int use_vcol = lit || !texture;

        if (poly->is_quad) {
            /* reference triangulation: (a,b,c) + (c,b,d) */
            mc_raster_tri(&out[0], &out[1], &out[2], texture, texture_blend,
                          use_vcol, blend_half, blend_add, blend_sub, 0);
            mc_raster_tri(&out[2], &out[1], &out[3], texture, texture_blend,
                          use_vcol, blend_half, blend_add, blend_sub, 0);
        } else {
            mc_raster_tri(&out[0], &out[1], &out[2], texture, texture_blend,
                          use_vcol, blend_half, blend_add, blend_sub, 0);
        }
    }

    return NATIVE_RETURN_VOID();
}

/* ----------------------------------------------------------------------------
 * AffineTrans natives (4.12 fixed point, public m00..m23 fields)
 * -------------------------------------------------------------------------- */

static void mc_affine_identity(McA* a) {
    for (int i = 0; i < 12; i++) a->m[i] = 0;
    a->m[0] = a->m[5] = a->m[10] = 4096;
}

/* this = a1 * a2 (reference mulA2 semantics) */
static void mc_affine_mul(McA* out, const McA* a1, const McA* a2) {
    McA r;
    for (int row = 0; row < 3; row++) {
        const int* l = a1->m + row * 4;
        for (int col = 0; col < 4; col++) {
            int64_t v = 0;
            for (int k = 0; k < 3; k++) {
                v += (int64_t)l[k] * (int64_t)a2->m[k * 4 + col];
            }
            if (col < 3) r.m[row * 4 + col] = (int)((v + 2048) >> 12);
            else r.m[row * 4 + 3] = (int)((v + 2048) >> 12) + l[3];
        }
    }
    *out = r;
}

static void mc_affine_rotX(McA* a, int r) {
    int c = mc_icos(r), s = mc_isin(r);
    a->m[0] = 4096; a->m[1] = 0; a->m[2] = 0;
    a->m[4] = 0; a->m[5] = c; a->m[6] = -s;
    a->m[8] = 0; a->m[9] = s; a->m[10] = c;
    a->m[3] = a->m[7] = a->m[11] = 0;
}

static void mc_affine_rotY(McA* a, int r) {
    int c = mc_icos(r), s = mc_isin(r);
    a->m[0] = c; a->m[1] = 0; a->m[2] = s;
    a->m[4] = 0; a->m[5] = 4096; a->m[6] = 0;
    a->m[8] = -s; a->m[9] = 0; a->m[10] = c;
    a->m[3] = a->m[7] = a->m[11] = 0;
}

static void mc_affine_rotZ(McA* a, int r) {
    int c = mc_icos(r), s = mc_isin(r);
    a->m[0] = c; a->m[1] = -s; a->m[2] = 0;
    a->m[4] = s; a->m[5] = c; a->m[6] = 0;
    a->m[8] = 0; a->m[9] = 0; a->m[10] = 4096;
    a->m[3] = a->m[7] = a->m[11] = 0;
}

/* Vector cross/dot on raw ints */
static void mc_cross(int out[3], const int a[3], const int b[3]) {
    out[0] = (int)((int64_t)a[1] * b[2] - (int64_t)a[2] * b[1]);
    out[1] = (int)((int64_t)a[2] * b[0] - (int64_t)a[0] * b[2]);
    out[2] = (int)((int64_t)a[0] * b[1] - (int64_t)a[1] * b[0]);
}

static int64_t mc_dot3(const int a[3], const int b[3]) {
    return (int64_t)a[0] * b[0] + (int64_t)a[1] * b[1] + (int64_t)a[2] * b[2];
}

/* normalize raw ints to 4.12 magnitude (reference Vector3D.unit) */
static void mc_unit(int v[3]) {
    int64_t x = v[0], y = v[1], z = v[2];
    int64_t m = x < 0 ? -x : x, t;
    t = y < 0 ? -y : y; if (t > m) m = t;
    t = z < 0 ? -z : z; if (t > m) m = t;
    if (m == 0) { v[0] = 0; v[1] = 0; v[2] = 4096; return; }
    int shift = 0;
    while (m < (1 << 20)) { m <<= 1; shift++; }
    x = (x << shift) >> 0; y = (y << shift); z = (z << shift);
    /* keep within int32 */
    if (x > 0x7FFFFFFFLL) x = 0x7FFFFFFFLL;
    if (x < -0x80000000LL) x = -0x80000000LL;
    if (y > 0x7FFFFFFFLL) y = 0x7FFFFFFFLL;
    if (y < -0x80000000LL) y = -0x80000000LL;
    if (z > 0x7FFFFFFFLL) z = 0x7FFFFFFFLL;
    if (z < -0x80000000LL) z = -0x80000000LL;
    int64_t len2 = x * x + y * y + z * z;
    if (len2 <= 0) { v[0] = 0; v[1] = 0; v[2] = 4096; return; }
    double len = sqrt((double)len2);
    if (len < 1.0) len = 1.0;
    v[0] = (int)(x * 4096.0 / len);
    v[1] = (int)(y * 4096.0 / len);
    v[2] = (int)(z * 4096.0 / len);
}

static void mc_affine_lookAt(McA* a, JavaObject* pos, JavaObject* look, JavaObject* up) {
    int p[3], l[3], u[3];
    p[0] = mc_get_int(pos, "x", 0); p[1] = mc_get_int(pos, "y", 0); p[2] = mc_get_int(pos, "z", 0);
    l[0] = mc_get_int(look, "x", 0); l[1] = mc_get_int(look, "y", 0); l[2] = mc_get_int(look, "z", 0);
    u[0] = mc_get_int(up, "x", 0); u[1] = mc_get_int(up, "y", 0); u[2] = mc_get_int(up, "z", 0);

    int negp[3] = { -p[0], -p[1], -p[2] };

    int r0[3];
    mc_cross(r0, l, u);
    mc_unit(r0);
    a->m[0] = r0[0]; a->m[1] = r0[1]; a->m[2] = r0[2];
    a->m[3] = (int)((mc_dot3(negp, r0) + 2048) >> 12);

    int r1[3];
    mc_cross(r1, l, r0);
    mc_unit(r1);
    a->m[4] = r1[0]; a->m[5] = r1[1]; a->m[6] = r1[2];
    a->m[7] = (int)((mc_dot3(negp, r1) + 2048) >> 12);

    int r2[3] = { l[0], l[1], l[2] };
    mc_unit(r2);
    a->m[8] = r2[0]; a->m[9] = r2[1]; a->m[10] = r2[2];
    a->m[11] = (int)((mc_dot3(negp, r2) + 2048) >> 12);
}

static JavaValue mc_affine_init(JVM* jvm, JavaThread* thread,
                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    if (!obj) return NATIVE_RETURN_VOID();
    McA a;
    mc_affine_identity(&a);
    mc_affine_write(obj, &a);
    return NATIVE_RETURN_VOID();
}

static JavaValue mc_affine_init_copy(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaObject* src = (JavaObject*)args[1].ref;
    if (!obj || !src) return NATIVE_RETURN_VOID();
    McA a;
    mc_affine_read(src, &a);
    mc_affine_write(obj, &a);
    return NATIVE_RETURN_VOID();
}

static JavaValue mc_affine_init_arr(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaArray* arr = (JavaArray*)args[1].ref;
    if (!obj || !arr || arr->element_type != T_INT || arr->length < 12) {
        if (obj) mc_affine_init(jvm, thread, args, arg_count);
        return NATIVE_RETURN_VOID();
    }
    jint* d = (jint*)array_data(arr);
    McA a;
    for (int i = 0; i < 12; i++) a.m[i] = d[i];
    mc_affine_write(obj, &a);
    return NATIVE_RETURN_VOID();
}

static JavaValue mc_affine_init_12(JVM* jvm, JavaThread* thread,
                                   JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    if (!obj) return NATIVE_RETURN_VOID();
    McA a;
    for (int i = 0; i < 12; i++) a.m[i] = args[1 + i].i;
    mc_affine_write(obj, &a);
    return NATIVE_RETURN_VOID();
}

static JavaValue mc_affine_setIdentity(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    if (!obj) return NATIVE_RETURN_VOID();
    McA a;
    mc_affine_identity(&a);
    mc_affine_write(obj, &a);
    return NATIVE_RETURN_VOID();
}

static JavaValue mc_affine_set12(JVM* jvm, JavaThread* thread,
                                 JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    if (!obj || arg_count < 13) return NATIVE_RETURN_VOID();
    McA a;
    for (int i = 0; i < 12; i++) a.m[i] = args[1 + i].i;
    mc_affine_write(obj, &a);
    return NATIVE_RETURN_VOID();
}

static JavaValue mc_affine_set_arr(JVM* jvm, JavaThread* thread,
                                   JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    (void)args;
    /* set(int[]) / set(int[][]) — copy elements */
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaArray* arr = (JavaArray*)args[1].ref;
    if (!obj || !arr) return NATIVE_RETURN_VOID();
    McA a;
    if (arr->element_type == T_INT && arr->length >= 12) {
        jint* d = (jint*)array_data(arr);
        for (int i = 0; i < 12; i++) a.m[i] = d[i];
    } else {
        /* int[][] */
        JavaObject** rows = (JavaObject**)array_data(arr);
        int k = 0;
        for (int r = 0; r < 3 && r < arr->length; r++) {
            JavaArray* row = (JavaArray*)rows[r];
            if (!row || row->element_type != T_INT) return NATIVE_RETURN_VOID();
            jint* d = (jint*)array_data(row);
            for (int c = 0; c < 4 && c < row->length; c++) a.m[k++] = d[c];
        }
        while (k < 12) a.m[k++] = 0;
    }
    mc_affine_write(obj, &a);
    return NATIVE_RETURN_VOID();
}

static JavaValue mc_affine_set_offset(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaArray* arr = (JavaArray*)args[1].ref;
    jint offset = args[2].i;
    if (!obj || !arr || arr->element_type != T_INT) return NATIVE_RETURN_VOID();
    if (offset < 0 || (size_t)offset + 12 > (size_t)arr->length) {
        native_throw_iae(jvm, thread, "set: array too small");
        return NATIVE_RETURN_VOID();
    }
    jint* d = (jint*)array_data(arr);
    McA a;
    for (int i = 0; i < 12; i++) a.m[i] = d[offset + i];
    mc_affine_write(obj, &a);
    return NATIVE_RETURN_VOID();
}

static JavaValue mc_affine_set_copy(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    return mc_affine_init_copy(jvm, thread, args, arg_count);
}

static JavaValue mc_affine_get_arr(JVM* jvm, JavaThread* thread,
                                   JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaArray* arr = (JavaArray*)args[1].ref;
    if (!obj || !arr || arr->element_type != T_INT) return NATIVE_RETURN_VOID();
    McA a;
    mc_affine_read(obj, &a);
    jint* d = (jint*)array_data(arr);
    for (int i = 0; i < 12 && i < arr->length; i++) d[i] = a.m[i];
    return NATIVE_RETURN_VOID();
}

static JavaValue mc_affine_get_offset(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaArray* arr = (JavaArray*)args[1].ref;
    jint offset = args[2].i;
    if (!obj || !arr || arr->element_type != T_INT) return NATIVE_RETURN_VOID();
    if (offset < 0 || (size_t)offset + 12 > (size_t)arr->length) return NATIVE_RETURN_VOID();
    McA a;
    mc_affine_read(obj, &a);
    jint* d = (jint*)array_data(arr);
    for (int i = 0; i < 12; i++) d[offset + i] = a.m[i];
    return NATIVE_RETURN_VOID();
}

static JavaValue mc_affine_mul1(JVM* jvm, JavaThread* thread,
                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaObject* other = (JavaObject*)args[1].ref;
    if (!obj) return NATIVE_RETURN_VOID();
    McA a, b;
    mc_affine_read(obj, &a);
    if (other) mc_affine_read(other, &b); else mc_affine_identity(&b);
    mc_affine_mul(&a, &a, &b);
    mc_affine_write(obj, &a);
    return NATIVE_RETURN_VOID();
}

static JavaValue mc_affine_mul2(JVM* jvm, JavaThread* thread,
                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaObject* a1 = (JavaObject*)args[1].ref;
    JavaObject* a2 = (JavaObject*)args[2].ref;
    if (!obj) return NATIVE_RETURN_VOID();
    McA x, y, r;
    mc_affine_read(a1 ? a1 : obj, &x);
    mc_affine_read(a2 ? a2 : obj, &y);
    mc_affine_mul(&r, &x, &y);
    mc_affine_write(obj, &r);
    return NATIVE_RETURN_VOID();
}

static JavaValue mc_affine_rot(JVM* jvm, JavaThread* thread,
                               JavaValue* args, int arg_count, int axis) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint r = args[1].i;
    if (!obj) return NATIVE_RETURN_VOID();
    McA a;
    if (axis == 0) mc_affine_rotX(&a, r);
    else if (axis == 1) mc_affine_rotY(&a, r);
    else mc_affine_rotZ(&a, r);
    mc_affine_write(obj, &a);
    return NATIVE_RETURN_VOID();
}

static JavaValue mc_affine_rotationX(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    return mc_affine_rot(jvm, thread, args, arg_count, 0);
}
static JavaValue mc_affine_rotationY(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    return mc_affine_rot(jvm, thread, args, arg_count, 1);
}
static JavaValue mc_affine_rotationZ(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    return mc_affine_rot(jvm, thread, args, arg_count, 2);
}

static JavaValue mc_affine_lookAt_native(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaObject* pos = (JavaObject*)args[1].ref;
    JavaObject* look = (JavaObject*)args[2].ref;
    JavaObject* up = (JavaObject*)args[3].ref;
    if (!obj) return NATIVE_RETURN_VOID();
    McA a;
    mc_affine_identity(&a);
    if (pos && look && up) mc_affine_lookAt(&a, pos, look, up);
    mc_affine_write(obj, &a);
    return NATIVE_RETURN_VOID();
}

static JavaValue mc_affine_setRotationV(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaObject* v = (JavaObject*)args[1].ref;
    jint r = args[2].i;
    if (!obj || !v) return NATIVE_RETURN_VOID();
    int x = mc_get_int(v, "x", 0), y = mc_get_int(v, "y", 0), z = mc_get_int(v, "z", 4096);
    int c = mc_icos(r), s = mc_isin(r);
    /* normalize to 4.12 */
    int axis[3] = { x, y, z };
    mc_unit(axis);
    int64_t xs = ((int64_t)axis[0] * s + 2048) >> 12;
    int64_t ys = ((int64_t)axis[1] * s + 2048) >> 12;
    int64_t zs = ((int64_t)axis[2] * s + 2048) >> 12;
    int nc = 4096 - c;
    int64_t xync = ((((int64_t)axis[0] * axis[1] + 2048) >> 12) * nc + 2048) >> 12;
    int64_t yznc = ((((int64_t)axis[1] * axis[2] + 2048) >> 12) * nc + 2048) >> 12;
    int64_t zxnc = ((((int64_t)axis[2] * axis[0] + 2048) >> 12) * nc + 2048) >> 12;
    McA a;
    a.m[0] = (int)(c + (((((int64_t)axis[0] * axis[0] + 2048) >> 12) * nc + 2048) >> 12));
    a.m[1] = (int)(xync - zs);
    a.m[2] = (int)(zxnc + ys);
    a.m[4] = (int)(zs + xync);
    a.m[5] = (int)(c + (((((int64_t)axis[1] * axis[1] + 2048) >> 12) * nc + 2048) >> 12));
    a.m[6] = (int)(yznc - xs);
    a.m[8] = (int)(zxnc - ys);
    a.m[9] = (int)(xs + yznc);
    a.m[10] = (int)(c + (((((int64_t)axis[2] * axis[2] + 2048) >> 12) * nc + 2048) >> 12));
    a.m[3] = a.m[7] = a.m[11] = 0;
    mc_affine_write(obj, &a);
    return NATIVE_RETURN_VOID();
}

static JavaValue mc_affine_transform(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaObject* v = (JavaObject*)args[1].ref;
    if (!obj || !v) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_NULL();
    }
    McA a;
    mc_affine_read(obj, &a);
    int x = mc_get_int(v, "x", 0), y = mc_get_int(v, "y", 0), z = mc_get_int(v, "z", 0);
    int nx = (int)(((int64_t)a.m[0] * x + (int64_t)a.m[1] * y + (int64_t)a.m[2] * z + 2048) >> 12) + a.m[3];
    int ny = (int)(((int64_t)a.m[4] * x + (int64_t)a.m[5] * y + (int64_t)a.m[6] * z + 2048) >> 12) + a.m[7];
    int nz = (int)(((int64_t)a.m[8] * x + (int64_t)a.m[9] * y + (int64_t)a.m[10] * z + 2048) >> 12) + a.m[11];
    mc_set_int(v, "x", nx);
    mc_set_int(v, "y", ny);
    mc_set_int(v, "z", nz);
    return NATIVE_RETURN_OBJECT(v);
}

/* ----------------------------------------------------------------------------
 * Vector3D natives
 * -------------------------------------------------------------------------- */

static JavaValue mc_vec_init(JVM* jvm, JavaThread* thread,
                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    if (!obj) return NATIVE_RETURN_VOID();
    mc_set_int(obj, "x", 0);
    mc_set_int(obj, "y", 0);
    mc_set_int(obj, "z", 0);
    return NATIVE_RETURN_VOID();
}

static JavaValue mc_vec_init3(JVM* jvm, JavaThread* thread,
                              JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    if (!obj) return NATIVE_RETURN_VOID();
    mc_set_int(obj, "x", args[1].i);
    mc_set_int(obj, "y", args[2].i);
    mc_set_int(obj, "z", args[3].i);
    return NATIVE_RETURN_VOID();
}

static JavaValue mc_vec_init_copy(JVM* jvm, JavaThread* thread,
                                  JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaObject* src = (JavaObject*)args[1].ref;
    if (!obj || !src) return NATIVE_RETURN_VOID();
    mc_set_int(obj, "x", mc_get_int(src, "x", 0));
    mc_set_int(obj, "y", mc_get_int(src, "y", 0));
    mc_set_int(obj, "z", mc_get_int(src, "z", 0));
    return NATIVE_RETURN_VOID();
}

static JavaValue mc_vec_set3(JVM* jvm, JavaThread* thread,
                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    if (!obj) return NATIVE_RETURN_VOID();
    mc_set_int(obj, "x", args[1].i);
    mc_set_int(obj, "y", args[2].i);
    mc_set_int(obj, "z", args[3].i);
    return NATIVE_RETURN_VOID();
}

static JavaValue mc_vec_setv(JVM* jvm, JavaThread* thread,
                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaObject* v = (JavaObject*)args[1].ref;
    if (!obj || !v) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    mc_set_int(obj, "x", mc_get_int(v, "x", 0));
    mc_set_int(obj, "y", mc_get_int(v, "y", 0));
    mc_set_int(obj, "z", mc_get_int(v, "z", 0));
    return NATIVE_RETURN_VOID();
}

static JavaValue mc_vec_getX(JVM* jvm, JavaThread* thread,
                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    return NATIVE_RETURN_INT(mc_get_int((JavaObject*)args[0].ref, "x", 0));
}
static JavaValue mc_vec_getY(JVM* jvm, JavaThread* thread,
                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    return NATIVE_RETURN_INT(mc_get_int((JavaObject*)args[0].ref, "y", 0));
}
static JavaValue mc_vec_getZ(JVM* jvm, JavaThread* thread,
                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    return NATIVE_RETURN_INT(mc_get_int((JavaObject*)args[0].ref, "z", 0));
}

static JavaValue mc_vec_setX(JVM* jvm, JavaThread* thread,
                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    mc_set_int((JavaObject*)args[0].ref, "x", args[1].i);
    return NATIVE_RETURN_VOID();
}
static JavaValue mc_vec_setY(JVM* jvm, JavaThread* thread,
                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    mc_set_int((JavaObject*)args[0].ref, "y", args[1].i);
    return NATIVE_RETURN_VOID();
}
static JavaValue mc_vec_setZ(JVM* jvm, JavaThread* thread,
                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    mc_set_int((JavaObject*)args[0].ref, "z", args[1].i);
    return NATIVE_RETURN_VOID();
}

static JavaValue mc_vec_inner(JVM* jvm, JavaThread* thread,
                              JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* a = (JavaObject*)args[0].ref;
    JavaObject* b = (JavaObject*)args[1].ref;
    if (!a || !b) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_INT(0);
    }
    int64_t v = (int64_t)mc_get_int(a, "x", 0) * mc_get_int(b, "x", 0) +
                (int64_t)mc_get_int(a, "y", 0) * mc_get_int(b, "y", 0) +
                (int64_t)mc_get_int(a, "z", 0) * mc_get_int(b, "z", 0);
    return NATIVE_RETURN_INT((int)v);
}

static JavaValue mc_vec_inner_static(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    /* static innerProduct(v1, v2) — args[0]=v1, args[1]=v2 (no this) */
    (void)arg_count;
    JavaObject* a = (JavaObject*)args[0].ref;
    JavaObject* b = (JavaObject*)args[1].ref;
    if (!a || !b) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_INT(0);
    }
    int64_t v = (int64_t)mc_get_int(a, "x", 0) * mc_get_int(b, "x", 0) +
                (int64_t)mc_get_int(a, "y", 0) * mc_get_int(b, "y", 0) +
                (int64_t)mc_get_int(a, "z", 0) * mc_get_int(b, "z", 0);
    return NATIVE_RETURN_INT((int)v);
}

static JavaValue mc_vec_outer(JVM* jvm, JavaThread* thread,
                              JavaValue* args, int arg_count) {
    /* instance outerProduct(v): this = this x v */
    (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaObject* v = (JavaObject*)args[1].ref;
    if (!obj || !v) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    int x = mc_get_int(obj, "x", 0), y = mc_get_int(obj, "y", 0), z = mc_get_int(obj, "z", 0);
    int vx = mc_get_int(v, "x", 0), vy = mc_get_int(v, "y", 0), vz = mc_get_int(v, "z", 0);
    mc_set_int(obj, "x", (int)((int64_t)y * vz - (int64_t)z * vy));
    mc_set_int(obj, "y", (int)((int64_t)z * vx - (int64_t)x * vz));
    mc_set_int(obj, "z", (int)((int64_t)x * vy - (int64_t)y * vx));
    return NATIVE_RETURN_VOID();
}

static JavaValue mc_vec_outer_static(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    /* static outerProduct(v1, v2) -> Vector3D; args[0]=v1, args[1]=v2 */
    (void)arg_count;
    JavaObject* a = (JavaObject*)args[0].ref;
    JavaObject* b = (JavaObject*)args[1].ref;
    if (!a || !b) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_NULL();
    }
    JavaClass* cls = jvm_load_class(jvm, "com/mascotcapsule/micro3d/v3/Vector3D");
    if (!cls) return NATIVE_RETURN_NULL();
    JavaObject* out = jvm_new_object(jvm, cls);
    if (!out) return NATIVE_RETURN_NULL();
    int ax = mc_get_int(a, "x", 0), ay = mc_get_int(a, "y", 0), az = mc_get_int(a, "z", 0);
    int bx = mc_get_int(b, "x", 0), by = mc_get_int(b, "y", 0), bz = mc_get_int(b, "z", 0);
    mc_set_int(out, "x", (int)((int64_t)ay * bz - (int64_t)az * by));
    mc_set_int(out, "y", (int)((int64_t)az * bx - (int64_t)ax * bz));
    mc_set_int(out, "z", (int)((int64_t)ax * by - (int64_t)ay * bx));
    return NATIVE_RETURN_OBJECT(out);
}

static JavaValue mc_vec_unit(JVM* jvm, JavaThread* thread,
                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    if (!obj) return NATIVE_RETURN_VOID();
    int v[3] = { mc_get_int(obj, "x", 0), mc_get_int(obj, "y", 0), mc_get_int(obj, "z", 0) };
    mc_unit(v);
    mc_set_int(obj, "x", v[0]);
    mc_set_int(obj, "y", v[1]);
    mc_set_int(obj, "z", v[2]);
    return NATIVE_RETURN_VOID();
}

/* ----------------------------------------------------------------------------
 * FigureLayout natives
 * -------------------------------------------------------------------------- */

static JavaValue mc_layout_init(JVM* jvm, JavaThread* thread,
                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    if (!obj) return NATIVE_RETURN_VOID();
    mc_set_int(obj, "scaleX", 512);
    mc_set_int(obj, "scaleY", 512);
    mc_set_int(obj, "centerX", 0);
    mc_set_int(obj, "centerY", 0);
    mc_set_int(obj, "parallelWidth", 512);
    mc_set_int(obj, "parallelHeight", 512);
    mc_set_int(obj, "projection", 0);
    mc_set_int(obj, "near", 0);
    mc_set_int(obj, "far", 0);
    mc_set_int(obj, "angle", 0);
    return NATIVE_RETURN_VOID();
}

static JavaValue mc_layout_init5(JVM* jvm, JavaThread* thread,
                                 JavaValue* args, int arg_count) {
    mc_layout_init(jvm, thread, args, arg_count);
    JavaObject* obj = (JavaObject*)args[0].ref;
    if (!obj) return NATIVE_RETURN_VOID();
    mc_set_ref(obj, "affine", (JavaObject*)args[1].ref);
    mc_set_int(obj, "scaleX", args[2].i);
    mc_set_int(obj, "scaleY", args[3].i);
    mc_set_int(obj, "centerX", args[4].i);
    mc_set_int(obj, "centerY", args[5].i);
    mc_set_int(obj, "parallelWidth", args[2].i);
    mc_set_int(obj, "parallelHeight", args[3].i);
    return NATIVE_RETURN_VOID();
}

static JavaValue mc_layout_setAffine(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaObject* trans = (JavaObject*)args[1].ref;
    if (!obj) return NATIVE_RETURN_VOID();
    if (trans) {
        mc_set_ref(obj, "affine", trans);
        mc_set_ref(obj, "affineArray", NULL);
    } else {
        mc_set_ref(obj, "affine", NULL);
    }
    return NATIVE_RETURN_VOID();
}

static JavaValue mc_layout_setAffineArr(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaObject* arr = (JavaObject*)args[1].ref;
    if (!obj || !arr) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    mc_set_ref(obj, "affineArray", arr);
    /* keep current affine if present; otherwise select 0 */
    if (!mc_get_ref(obj, "affine")) {
        JavaArray* a = (JavaArray*)arr;
        if (a->element_type == DESC_OBJECT && a->length > 0) {
            JavaObject** elems = (JavaObject**)array_data(a);
            mc_set_ref(obj, "affine", elems[0]);
        }
    }
    return NATIVE_RETURN_VOID();
}

static JavaValue mc_layout_selectAffine(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint idx = args[1].i;
    JavaObject* arr = mc_get_ref(obj, "affineArray");
    if (!arr) {
        native_throw_iae(jvm, thread, "selectAffineTrans: no array");
        return NATIVE_RETURN_VOID();
    }
    JavaArray* a = (JavaArray*)arr;
    if (idx < 0 || idx >= a->length || a->element_type != DESC_OBJECT) {
        native_throw_iae(jvm, thread, "selectAffineTrans: bad index");
        return NATIVE_RETURN_VOID();
    }
    JavaObject** elems = (JavaObject**)array_data(a);
    mc_set_ref(obj, "affine", elems[idx]);
    return NATIVE_RETURN_VOID();
}

static JavaValue mc_layout_getAffine(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    if (!obj) return NATIVE_RETURN_NULL();
    JavaObject* affine = mc_get_ref(obj, "affine");
    if (affine) return NATIVE_RETURN_OBJECT(affine);
    /* materialize a default identity */
    JavaClass* cls = jvm_load_class(jvm, "com/mascotcapsule/micro3d/v3/AffineTrans");
    if (!cls) return NATIVE_RETURN_NULL();
    JavaObject* out = jvm_new_object(jvm, cls);
    if (out) {
        McA a;
        mc_affine_identity(&a);
        mc_affine_write(out, &a);
        mc_set_ref(obj, "affine", out);
    }
    return NATIVE_RETURN_OBJECT(out);
}

static JavaValue mc_layout_setCenter(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    if (!obj) return NATIVE_RETURN_VOID();
    mc_set_int(obj, "centerX", args[1].i);
    mc_set_int(obj, "centerY", args[2].i);
    return NATIVE_RETURN_VOID();
}

static JavaValue mc_layout_setScale(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    if (!obj) return NATIVE_RETURN_VOID();
    mc_set_int(obj, "scaleX", args[1].i);
    mc_set_int(obj, "scaleY", args[2].i);
    mc_set_int(obj, "projection", 0);
    return NATIVE_RETURN_VOID();
}

static JavaValue mc_layout_setParallelSize(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    if (!obj) return NATIVE_RETURN_VOID();
    if (args[1].i < 0 || args[2].i < 0) {
        native_throw_iae(jvm, thread, "setParallelSize: negative");
        return NATIVE_RETURN_VOID();
    }
    mc_set_int(obj, "parallelWidth", args[1].i);
    mc_set_int(obj, "parallelHeight", args[2].i);
    mc_set_int(obj, "projection", (int)0x91000000);
    return NATIVE_RETURN_VOID();
}

static JavaValue mc_layout_setPerspective(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    if (!obj) return NATIVE_RETURN_VOID();
    jint zn = args[1].i, zf = args[2].i, angle = args[3].i;
    if (zn >= zf || zn < 1 || zf > 32767 || angle < 1 || angle > 2047) {
        native_throw_iae(jvm, thread, "setPerspective: bad params");
        return NATIVE_RETURN_VOID();
    }
    mc_set_int(obj, "near", zn);
    mc_set_int(obj, "far", zf);
    mc_set_int(obj, "angle", angle);
    mc_set_int(obj, "projection", (int)0x92000000);
    return NATIVE_RETURN_VOID();
}

static JavaValue mc_layout_setPerspectiveWH(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    if (!obj) return NATIVE_RETURN_VOID();
    jint zn = args[1].i, zf = args[2].i, w = args[3].i, h = args[4].i;
    if (zn >= zf || zn < 1 || zf > 32767 || w < 0 || h < 0) {
        native_throw_iae(jvm, thread, "setPerspective: bad params");
        return NATIVE_RETURN_VOID();
    }
    mc_set_int(obj, "near", zn);
    mc_set_int(obj, "far", zf);
    mc_set_int(obj, "perspectiveWidth", w);
    mc_set_int(obj, "perspectiveHeight", h);
    mc_set_int(obj, "projection", (int)0x93000000);
    return NATIVE_RETURN_VOID();
}

/* getters */
static JavaValue mc_layout_getScaleX(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    return NATIVE_RETURN_INT(mc_get_int((JavaObject*)args[0].ref, "scaleX", 512));
}
static JavaValue mc_layout_getScaleY(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    return NATIVE_RETURN_INT(mc_get_int((JavaObject*)args[0].ref, "scaleY", 512));
}
static JavaValue mc_layout_getCenterX(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    return NATIVE_RETURN_INT(mc_get_int((JavaObject*)args[0].ref, "centerX", 0));
}
static JavaValue mc_layout_getCenterY(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    return NATIVE_RETURN_INT(mc_get_int((JavaObject*)args[0].ref, "centerY", 0));
}
static JavaValue mc_layout_getParW(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    return NATIVE_RETURN_INT(mc_get_int((JavaObject*)args[0].ref, "parallelWidth", 512));
}
static JavaValue mc_layout_getParH(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    return NATIVE_RETURN_INT(mc_get_int((JavaObject*)args[0].ref, "parallelHeight", 512));
}

/* ----------------------------------------------------------------------------
 * Effect3D natives
 * -------------------------------------------------------------------------- */

static JavaValue mc_effect_init(JVM* jvm, JavaThread* thread,
                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    if (!obj) return NATIVE_RETURN_VOID();
    mc_set_int(obj, "shadingType", 0);      /* NORMAL_SHADING */
    mc_set_int(obj, "transparency", 1);     /* default: enabled */
    mc_set_int(obj, "toonThreshold", -1);
    mc_set_int(obj, "toonHigh", 255);
    mc_set_int(obj, "toonLow", 0);
    return NATIVE_RETURN_VOID();
}

static JavaValue mc_effect_init4(JVM* jvm, JavaThread* thread,
                                 JavaValue* args, int arg_count) {
    mc_effect_init(jvm, thread, args, arg_count);
    JavaObject* obj = (JavaObject*)args[0].ref;
    if (!obj) return NATIVE_RETURN_VOID();
    mc_set_ref(obj, "light", (JavaObject*)args[1].ref);
    mc_set_int(obj, "shadingType", args[2].i);
    mc_set_int(obj, "transparency", args[3].i != 0 ? 1 : 0);
    mc_set_ref(obj, "sphereTexture", (JavaObject*)args[4].ref);
    return NATIVE_RETURN_VOID();
}

static JavaValue mc_effect_setLight(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    if (!obj) return NATIVE_RETURN_VOID();
    mc_set_ref(obj, "light", (JavaObject*)args[1].ref);
    return NATIVE_RETURN_VOID();
}

static JavaValue mc_effect_getLight(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    if (!obj) return NATIVE_RETURN_NULL();
    return NATIVE_RETURN_OBJECT(mc_get_ref(obj, "light"));
}

static JavaValue mc_effect_setShadingType(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    if (!obj) return NATIVE_RETURN_VOID();
    mc_set_int(obj, "shadingType", args[1].i);
    return NATIVE_RETURN_VOID();
}

static JavaValue mc_effect_getShadingType(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    return NATIVE_RETURN_INT(mc_get_int((JavaObject*)args[0].ref, "shadingType", 0));
}

static JavaValue mc_effect_setTransparency(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    if (!obj) return NATIVE_RETURN_VOID();
    mc_set_int(obj, "transparency", args[1].i != 0 ? 1 : 0);
    return NATIVE_RETURN_VOID();
}

static JavaValue mc_effect_isTransparency(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    return NATIVE_RETURN_INT(mc_get_int((JavaObject*)args[0].ref, "transparency", 1));
}

static JavaValue mc_effect_setToonParams(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    if (!obj) return NATIVE_RETURN_VOID();
    mc_set_int(obj, "toonThreshold", args[1].i);
    mc_set_int(obj, "toonHigh", args[2].i);
    mc_set_int(obj, "toonLow", args[3].i);
    return NATIVE_RETURN_VOID();
}

/* ----------------------------------------------------------------------------
 * Light natives
 * -------------------------------------------------------------------------- */

static JavaValue mc_light_init(JVM* jvm, JavaThread* thread,
                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    if (!obj) return NATIVE_RETURN_VOID();
    /* default: direction (0,0,4096), dirIntensity 4096, ambient 0 */
    JavaClass* cls = jvm_load_class(jvm, "com/mascotcapsule/micro3d/v3/Vector3D");
    if (cls) {
        JavaObject* d = jvm_new_object(jvm, cls);
        if (d) {
            mc_set_int(d, "x", 0);
            mc_set_int(d, "y", 0);
            mc_set_int(d, "z", 4096);
            mc_set_ref(obj, "lightDir", d);
        }
    }
    mc_set_int(obj, "dirIntensity", 4096);
    mc_set_int(obj, "ambIntensity", 0);
    return NATIVE_RETURN_VOID();
}

static JavaValue mc_light_init3(JVM* jvm, JavaThread* thread,
                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaObject* dir = (JavaObject*)args[1].ref;
    if (!obj || !dir) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    mc_set_ref(obj, "lightDir", dir);
    mc_set_int(obj, "dirIntensity", args[2].i);
    mc_set_int(obj, "ambIntensity", args[3].i);
    return NATIVE_RETURN_VOID();
}

static JavaValue mc_light_setDir(JVM* jvm, JavaThread* thread,
                                 JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaObject* v = (JavaObject*)args[1].ref;
    if (!obj || !v) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    mc_set_ref(obj, "lightDir", v);
    return NATIVE_RETURN_VOID();
}

static JavaValue mc_light_getDir(JVM* jvm, JavaThread* thread,
                                 JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    if (!obj) return NATIVE_RETURN_NULL();
    return NATIVE_RETURN_OBJECT(mc_get_ref(obj, "lightDir"));
}

static JavaValue mc_light_setDirI(JVM* jvm, JavaThread* thread,
                                  JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    mc_set_int((JavaObject*)args[0].ref, "dirIntensity", args[1].i);
    return NATIVE_RETURN_VOID();
}
static JavaValue mc_light_getDirI(JVM* jvm, JavaThread* thread,
                                  JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    return NATIVE_RETURN_INT(mc_get_int((JavaObject*)args[0].ref, "dirIntensity", 4096));
}
static JavaValue mc_light_setAmbI(JVM* jvm, JavaThread* thread,
                                  JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    mc_set_int((JavaObject*)args[0].ref, "ambIntensity", args[1].i);
    return NATIVE_RETURN_VOID();
}
static JavaValue mc_light_getAmbI(JVM* jvm, JavaThread* thread,
                                  JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    return NATIVE_RETURN_INT(mc_get_int((JavaObject*)args[0].ref, "ambIntensity", 0));
}

/* ----------------------------------------------------------------------------
 * Texture natives
 * -------------------------------------------------------------------------- */

static JavaValue mc_texture_init_str(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaString* name_str = (JavaString*)args[1].ref;
    if (!obj || !name_str) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    const char* name = string_utf8(jvm, name_str);
    if (!name) return NATIVE_RETURN_VOID();

    const char* res = name;
    if (res[0] == '/') res++;

    size_t size = 0;
    uint8_t* data = load_jar_resource(res, &size);
    if (!data) {
        jvm_throw_by_name(jvm, "java/io/IOException", res);
        if (thread) thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_VOID();
    }

    MCTexture* t = mc_texture_alloc();
    if (!t || mc_decode_bmp(data, size, &t->tex) != 0) {
        free(data);
        if (t) t->used = 0;
        jvm_throw_by_name(jvm, "java/lang/RuntimeException", "Texture: bad BMP data");
        if (thread) thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_VOID();
    }
    free(data);

    int handle = (int)(t - mc_textures) + 1;
    mc_set_int(obj, "texHandle", handle);
    mc_set_int(obj, "width", t->tex.width);
    mc_set_int(obj, "height", t->tex.height);
    mc_tex_creates++;
    if (mc_tex_creates <= 10)
        fprintf(stderr, MC_TAG "Texture#%ld '%s': %dx%d\n", mc_tex_creates,
                name ? name : "?", t->tex.width, t->tex.height);
    return NATIVE_RETURN_VOID();
}

static JavaValue mc_texture_init_bytes(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaArray* arr = (JavaArray*)args[1].ref;
    if (!obj || !arr || arr->element_type != T_BYTE) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    MCTexture* t = mc_texture_alloc();
    uint8_t* d = (uint8_t*)array_data(arr);
    if (!t || mc_decode_bmp(d, (size_t)arr->length, &t->tex) != 0) {
        if (t) t->used = 0;
        jvm_throw_by_name(jvm, "java/lang/RuntimeException", "Texture: bad BMP data");
        if (thread) thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_VOID();
    }
    int handle = (int)(t - mc_textures) + 1;
    mc_set_int(obj, "texHandle", handle);
    mc_set_int(obj, "width", t->tex.width);
    mc_set_int(obj, "height", t->tex.height);
    return NATIVE_RETURN_VOID();
}

static JavaValue mc_texture_dispose(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    if (!obj) return NATIVE_RETURN_VOID();
    MCTexture* t = mc_texture_from_handle(mc_get_int(obj, "texHandle", 0));
    if (t) {
        free(t->tex.pixels);
        t->used = 0;
    }
    mc_set_int(obj, "texHandle", 0);
    return NATIVE_RETURN_VOID();
}

/* ----------------------------------------------------------------------------
 * Figure natives
 * -------------------------------------------------------------------------- */

static JavaValue mc_figure_init_bytes(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaArray* arr = (JavaArray*)args[1].ref;
    if (!obj || !arr || arr->element_type != T_BYTE) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    MCFigure* f = mc_figure_alloc();
    uint8_t* d = (uint8_t*)array_data(arr);
    if (mc_trace_on() && mc_fig_creates < 8) {
        fprintf(stderr, MC_TAG "Figure ctor: %d bytes, first=%02X %02X %02X %02X\n",
                (int)arr->length, d[0], d[1], d[2], d[3]);
    }
    if (!f || mc_load_mbac(d, (size_t)arr->length, f) != 0) {
        if (mc_fig_creates < 8)
            fprintf(stderr, MC_TAG "Figure ctor FAILED for %d bytes\n", (int)arr->length);
        mc_fig_creates++;
        if (f) f->used = 0;
        jvm_throw_by_name(jvm, "java/lang/RuntimeException", "Figure: bad MBAC data");
        if (thread) thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_VOID();
    }
    mc_set_int(obj, "figHandle", (int)(f - mc_figures) + 1);
    mc_set_int(obj, "pattern", 0);
    mc_set_int(obj, "selTexture", -1);
    mc_fig_creates++;
    if (mc_fig_creates <= 6)
        fprintf(stderr, MC_TAG "Figure#%ld parsed: %d verts, %d polys, %d patterns\n",
                mc_fig_creates, f->vert_count, f->poly_count, f->num_patterns);
    (void)mc_trace_on;
    return NATIVE_RETURN_VOID();
}

static JavaValue mc_figure_init_range(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaArray* arr = (JavaArray*)args[1].ref;
    jint offset = args[2].i;
    jint length = args[3].i;
    if (!obj || !arr || arr->element_type != T_BYTE) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    if (offset < 0 || length < 0 || (size_t)offset + (size_t)length > (size_t)arr->length) {
        jvm_throw_by_name(jvm, "java/lang/ArrayIndexOutOfBoundsException", "Figure data range");
        if (thread) thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_VOID();
    }
    MCFigure* f = mc_figure_alloc();
    uint8_t* d = (uint8_t*)array_data(arr);
    if (!f || mc_load_mbac(d + offset, (size_t)length, f) != 0) {
        if (f) f->used = 0;
        jvm_throw_by_name(jvm, "java/lang/RuntimeException", "Figure: bad MBAC data");
        if (thread) thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_VOID();
    }
    mc_set_int(obj, "figHandle", (int)(f - mc_figures) + 1);
    mc_set_int(obj, "pattern", 0);
    mc_set_int(obj, "selTexture", -1);
    return NATIVE_RETURN_VOID();
}

static JavaValue mc_figure_init_str(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaString* name_str = (JavaString*)args[1].ref;
    if (!obj || !name_str) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    const char* name = string_utf8(jvm, name_str);
    if (!name) return NATIVE_RETURN_VOID();
    const char* res = name;
    if (res[0] == '/') res++;
    size_t size = 0;
    uint8_t* data = load_jar_resource(res, &size);
    if (!data) {
        jvm_throw_by_name(jvm, "java/io/IOException", res);
        if (thread) thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_VOID();
    }
    MCFigure* f = mc_figure_alloc();
    if (!f || mc_load_mbac(data, size, f) != 0) {
        free(data);
        if (f) f->used = 0;
        jvm_throw_by_name(jvm, "java/lang/RuntimeException", "Figure: bad MBAC data");
        if (thread) thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_VOID();
    }
    free(data);
    mc_set_int(obj, "figHandle", (int)(f - mc_figures) + 1);
    mc_set_int(obj, "pattern", 0);
    mc_set_int(obj, "selTexture", -1);
    return NATIVE_RETURN_VOID();
}

static JavaValue mc_figure_setTexture1(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaObject* tex = (JavaObject*)args[1].ref;
    if (!obj || !tex) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    JavaArray* arr = jvm_new_array(jvm, DESC_OBJECT, 1, NULL);
    if (!arr) return NATIVE_RETURN_VOID();
    JavaObject** elems = (JavaObject**)array_data(arr);
    elems[0] = tex;
    mc_set_ref(obj, "textures", (JavaObject*)arr);
    mc_set_int(obj, "selTexture", 0);
    return NATIVE_RETURN_VOID();
}

static JavaValue mc_figure_setTextureArr(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaObject* arr = (JavaObject*)args[1].ref;
    if (!obj || !arr) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    mc_set_ref(obj, "textures", arr);
    mc_set_int(obj, "selTexture", 0);
    return NATIVE_RETURN_VOID();
}

static JavaValue mc_figure_selectTexture(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint idx = args[1].i;
    JavaObject* arr = mc_get_ref(obj, "textures");
    int n = 0;
    if (arr) n = (int)((JavaArray*)arr)->length;
    if (idx < 0 || idx >= n) {
        native_throw_iae(jvm, thread, "selectTexture: index");
        return NATIVE_RETURN_VOID();
    }
    mc_set_int(obj, "selTexture", idx);
    return NATIVE_RETURN_VOID();
}

static JavaValue mc_figure_setPattern(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    if (!obj) return NATIVE_RETURN_VOID();
    mc_set_int(obj, "pattern", args[1].i);
    return NATIVE_RETURN_VOID();
}

static JavaValue mc_figure_getNumPattern(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    MCFigure* f = mc_figure_from_handle(mc_get_int(obj, "figHandle", 0));
    return NATIVE_RETURN_INT(f ? f->num_patterns : 0);
}

static JavaValue mc_figure_getNumTextures(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaObject* arr = mc_get_ref(obj, "textures");
    return NATIVE_RETURN_INT(arr ? (int)((JavaArray*)arr)->length : 0);
}

static JavaValue mc_figure_dispose(JVM* jvm, JavaThread* thread,
                                   JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    if (!obj) return NATIVE_RETURN_VOID();
    MCFigure* f = mc_figure_from_handle(mc_get_int(obj, "figHandle", 0));
    if (f) mc_figure_free(f);
    mc_set_int(obj, "figHandle", 0);
    return NATIVE_RETURN_VOID();
}

/* ActionTable.dispose() — release the parsed MTRA pool entry. */
static JavaValue mc_action_dispose(JVM* jvm, JavaThread* thread,
                                   JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    if (!obj) return NATIVE_RETURN_VOID();
    MCTra* tra = mc_tra_from_handle(mc_get_int(obj, "mtraHandle", 0));
    if (tra) mc_tra_free(tra);
    mc_set_int(obj, "mtraHandle", 0);
    mc_set_int(obj, "numActions", 0);
    return NATIVE_RETURN_VOID();
}

/* Figure.getTexture() — the currently selected Texture or null
 * (reference Figure.java: textures[textureIndex], null when index < 0). */
static JavaValue mc_figure_getTexture(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    if (!obj) return NATIVE_RETURN_NULL();
    JavaObject* arr = mc_get_ref(obj, "textures");
    if (!arr) return NATIVE_RETURN_NULL();
    JavaArray* ta = (JavaArray*)arr;
    int idx = mc_get_int(obj, "selTexture", -1);
    if (idx < 0 || idx >= (int)ta->length || ta->element_type != DESC_OBJECT) {
        return NATIVE_RETURN_NULL();
    }
    JavaObject** elems = (JavaObject**)array_data(ta);
    return NATIVE_RETURN_OBJECT(elems ? elems[idx] : NULL);
}

/* Effect3D toon getters + sphere aliases */
static JavaValue mc_effect_getIntField(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count,
                                       const char* field, int dflt) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    return NATIVE_RETURN_INT(obj ? mc_get_int(obj, field, dflt) : dflt);
}
static JavaValue mc_effect_getToonThreshold(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    return mc_effect_getIntField(jvm, thread, args, arg_count, "toonThreshold", 0);
}
static JavaValue mc_effect_getToonHigh(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    return mc_effect_getIntField(jvm, thread, args, arg_count, "toonHigh", 255);
}
static JavaValue mc_effect_getToonLow(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    return mc_effect_getIntField(jvm, thread, args, arg_count, "toonLow", 0);
}

static JavaValue mc_effect_setSphereMap(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    if (!obj) return NATIVE_RETURN_VOID();
    mc_set_ref(obj, "sphereTexture", (JavaObject*)args[1].ref);
    return NATIVE_RETURN_VOID();
}

/* ActionTable aliases (deprecated singular names in the official API) */
static JavaValue mc_action_getNumActions(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    MCTra* tra = mc_tra_from_handle(mc_get_int(obj, "mtraHandle", 0));
    return NATIVE_RETURN_INT(tra ? tra->num_actions : 0);
}

/* ----------------------------------------------------------------------------
 * ActionTable (minimal: header parse + counts)
 * -------------------------------------------------------------------------- */

static JavaValue mc_action_init(JVM* jvm, JavaThread* thread,
                                JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaArray* arr = (JavaArray*)args[1].ref;
    if (!obj || !arr || arr->element_type != T_BYTE) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    uint8_t* d = (uint8_t*)array_data(arr);
    size_t n = (size_t)arr->length;
    /* (byte[], offset, length) overload */
    if (arg_count >= 4) {
        jint off = args[2].i, len = args[3].i;
        if (off < 0 || len < 0 || (size_t)off + (size_t)len > n) {
            native_throw_iae(jvm, thread, "ActionTable: range");
            return NATIVE_RETURN_VOID();
        }
        d += off;
        n = (size_t)len;
    }
    int handle = 0;
    int num_actions = 0;
    if (n >= 8 && d[0] == 'M' && d[1] == 'T') {
        num_actions = d[4] | (d[5] << 8);
        MCTra* t = mc_tra_alloc();
        if (t && mc_load_mtra(d, n, t) == 0) {
            handle = (int)(t - mc_tras) + 1;
            num_actions = t->num_actions;
            mc_tra_creates++;
            if (mc_tra_creates <= 64)
                fprintf(stderr, MC_TAG "ActionTable#%ld ok: actions=%d handle=%d bytes=%d obj=%p clazz=%s\n",
                        mc_tra_creates, num_actions, handle, (int)n, (void*)obj,
                        obj->header.clazz && obj->header.clazz->class_name
                            ? obj->header.clazz->class_name : "?");
        } else if (t) {
            /* v36.22 [MC-RESET] DIAG: this fallback used to be FULLY silent —
             * handle stayed 0 and the game later divided getNumFrames()==0
             * into zero (Treasure Towers c.d PC=23 "/ by zero" on RELAUNCH). */
            fprintf(stderr, MC_TAG "ActionTable ctor MTRA FAIL: bytes=%d hdr=%02X %02X %02X %02X "
                    "ver=%d numActions=%d numBones=%d\n", (int)n, d[0], d[1], d[2], d[3],
                    d[2], num_actions, d[6] | (d[7] << 8));
            t->used = 0; /* header-only fallback */
            mc_tra_free(t);
        }
    } else {
        /* v36.22 [MC-RESET] DIAG: header mismatch used to be silent too. */
        fprintf(stderr, MC_TAG "ActionTable ctor BAD HEADER: bytes=%d first=%02X %02X %02X %02X\n",
                (int)n, n >= 1 ? d[0] : 0, n >= 2 ? d[1] : 0,
                n >= 3 ? d[2] : 0, n >= 4 ? d[3] : 0);
    }
    mc_set_int(obj, "numActions", num_actions);
    mc_set_int(obj, "mtraHandle", handle);
    return NATIVE_RETURN_VOID();
}

static JavaValue mc_action_getNumAction(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    return NATIVE_RETURN_INT(mc_get_int((JavaObject*)args[0].ref, "numActions", 0));
}

static JavaValue mc_action_getNumFrames(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    int h = mc_get_int(obj, "mtraHandle", 0);
    MCTra* tra = mc_tra_from_handle(h);
    jint idx = args[1].i;
    if (!tra) {
        /* v36.22 [MC-RESET] DIAG: a 0 return here silently fed a division
         * in Treasure Towers (c.d "/ by zero"). Name the handle once per
         * session so the log shows WHY: 0 = ctor never parsed / slot unused. */
        static int s_bad_frames_prints = -1;
        if (s_bad_frames_prints < 0) s_bad_frames_prints = 0;
        if (s_bad_frames_prints < 4) {
            s_bad_frames_prints++;
            int used = 0;
            for (int i = 0; i < MC_MAX_TRAS; i++) used += mc_tras[i].used ? 1 : 0;
            fprintf(stderr, MC_TAG "getNumFrames: handle=%d INVALID (used slots=%d/%d) "
                    "obj=%p clazz=%s — returning 0\n", h, used, MC_MAX_TRAS, (void*)obj,
                    obj && obj->header.clazz && obj->header.clazz->class_name
                        ? obj->header.clazz->class_name : "?");
        }
        return NATIVE_RETURN_INT(0);
    }
    if (idx < 0 || idx >= tra->num_actions) {
        native_throw_iae(jvm, thread, "getNumFrames: index");
        return NATIVE_RETURN_INT(0);
    }
    /* reference ActTableImpl: keyframes count returned in 16.16 form */
    return NATIVE_RETURN_INT(tra->actions[idx].keyframes << 16);
}

/* Figure.setPosture(ActionTable, action, frame[, pattern]) — v34.32 full
 * implementation: evaluates the MTRA bone animation at the 16.16 frame and
 * re-poses the figure; dynamic patterns (version >= 5) override the figure
 * pattern per the reference FigureImpl.setPosture. */
static JavaValue mc_figure_setPosture(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* figure = (JavaObject*)args[0].ref;
    JavaObject* actobj = (JavaObject*)args[1].ref;
    jint action = args[2].i;
    jint frame = args[3].i;
    if (!figure || !actobj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    MCFigure* fig = mc_figure_from_handle(mc_get_int(figure, "figHandle", 0));
    MCTra* tra = mc_tra_from_handle(mc_get_int(actobj, "mtraHandle", 0));
    if (!fig || !tra) return NATIVE_RETURN_VOID();
    if (action < 0 || action >= tra->num_actions) {
        native_throw_iae(jvm, thread, "setPosture: action index");
        return NATIVE_RETURN_VOID();
    }
    MCAction* act = &tra->actions[action];
    if (frame < 0) frame = 0;
    /* dynamic pattern timeline (last entry with dyn_frame <= frame>>16) */
    int ifr = frame >> 16;
    for (int i = act->dyn_count - 1; i >= 0; i--) {
        if (act->dyn_frame[i] <= ifr) {
            if (arg_count >= 5) {
                /* explicit pattern argument (Motorola-style overload) wins */
                mc_set_int(figure, "pattern", args[4].i);
            } else {
                mc_set_int(figure, "pattern", act->dyn_pattern[i]);
            }
            break;
        }
    }
    if (arg_count >= 5 && act->dyn_count == 0) {
        mc_set_int(figure, "pattern", args[4].i);
    }
    mc_apply_posture(fig, act, frame);
    return NATIVE_RETURN_VOID();
}

/* ----------------------------------------------------------------------------
 * Util3D
 * -------------------------------------------------------------------------- */

static JavaValue mc_util_sin(JVM* jvm, JavaThread* thread,
                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    return NATIVE_RETURN_INT(mc_isin(args[0].i));
}
static JavaValue mc_util_cos(JVM* jvm, JavaThread* thread,
                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    return NATIVE_RETURN_INT(mc_icos(args[0].i));
}
static JavaValue mc_util_sqrt(JVM* jvm, JavaThread* thread,
                              JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jint x = args[0].i;
    if (x < 0) {
        native_throw_iae(jvm, thread, "sqrt: negative");
        return NATIVE_RETURN_INT(0);
    }
    return NATIVE_RETURN_INT((jint)lround(sqrt((double)x)));
}

/* ----------------------------------------------------------------------------
 * Graphics3D.getInstance + drawCommandList stub
 * -------------------------------------------------------------------------- */

static JavaValue mc_g3d_getInstance(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    (void)thread; (void)args; (void)arg_count;
    JavaClass* cls = jvm_load_class(jvm, "com/mascotcapsule/micro3d/v3/Graphics3D");
    if (!cls) return NATIVE_RETURN_NULL();
    for (int i = 0; i < cls->static_fields_count; i++) {
        if (strcmp(cls->static_fields[i].name, "instance") == 0 &&
            cls->static_fields[i].value.ref) {
            return NATIVE_RETURN_OBJECT(cls->static_fields[i].value.ref);
        }
    }
    JavaObject* instance = jvm_new_object(jvm, cls);
    if (!instance) return NATIVE_RETURN_NULL();
    for (int i = 0; i < cls->static_fields_count; i++) {
        if (strcmp(cls->static_fields[i].name, "instance") == 0) {
            cls->static_fields[i].value.ref = instance;
            break;
        }
    }
    return NATIVE_RETURN_OBJECT(instance);
}

/* ---- drawCommandList (v34.32: full interpreter, reference woesss/JL-Mod
 * Render.drawCommandList). Layout of the int[]:
 *   [0] = COMMAND_LIST_VERSION_1_0, then commands until COMMAND_END (or the
 *   end of the array). State commands mutate a local draw env (center, clip,
 *   projection, light, toon, texture, affine selection); primitive commands
 *   carry their vertex/normal/texcoord/color data inline and render through
 *   the shared immediate batch renderer. */
#define MC_CMD_LIST_VERSION_1_0 0xFE000001
#define MC_CMD_END              0x80000000
#define MC_CMD_NOP              0x81000000
#define MC_CMD_FLUSH            0x82000000
#define MC_CMD_ATTRIBUTE        0x83000000
#define MC_CMD_CLIP             0x84000000
#define MC_CMD_CENTER           0x85000000
#define MC_CMD_TEXTURE_INDEX    0x86000000
#define MC_CMD_AFFINE_INDEX     0x87000000
#define MC_CMD_PARALLEL_SCALE   0x90000000
#define MC_CMD_PARALLEL_SIZE    0x91000000
#define MC_CMD_PERSPECTIVE_FOV  0x92000000
#define MC_CMD_PERSPECTIVE_WH   0x93000000
#define MC_CMD_AMBIENT_LIGHT    0xA0000000
#define MC_CMD_DIRECTION_LIGHT  0xA1000000
#define MC_CMD_THRESHOLD        0xAF000000
#define MC_ENV_LIGHTING         1
#define MC_ENV_SPHERE_MAP       2
#define MC_ENV_TOON_SHADING     4
#define MC_ENV_SEMI_TRANS       8

static const int MC_PRIM_SIZES[6] = { 0, 1, 2, 3, 4, 1 };

static JavaValue mc_g3d_drawCommandList(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    if (!mc.bound || !mc.gfx) return NATIVE_RETURN_VOID();

    JavaObject* tex_obj = (JavaObject*)args[1].ref;   /* Texture or Texture[] */
    jint off_x = args[2].i;
    jint off_y = args[3].i;
    JavaObject* layout = (JavaObject*)args[4].ref;
    JavaObject* effect = (JavaObject*)args[5].ref;
    JavaArray* cmdsArr = (JavaArray*)args[6].ref;
    if (!layout || !effect || !cmdsArr) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    if (cmdsArr->element_type != T_INT) return NATIVE_RETURN_VOID();
    jint* cmds = (jint*)array_data(cmdsArr);
    int n = (int)cmdsArr->length;
    if (n < 1 || (jint)(uint32_t)cmds[0] != (jint)MC_CMD_LIST_VERSION_1_0) {
        native_throw_iae(jvm, thread, "drawCommandList: version");
        return NATIVE_RETURN_VOID();
    }

    /* texture argument: single Texture or Texture[] */
    JavaArray* texArr = NULL;
    if (tex_obj && object_is_array(tex_obj)) {
        texArr = (JavaArray*)tex_obj;
        tex_obj = NULL;
    }
    MCTexture* cur_tex = NULL;
    if (texArr && texArr->element_type == DESC_OBJECT) {
        JavaObject** elems = (JavaObject**)array_data(texArr);
        if (elems && texArr->length > 0 && elems[0]) {
            cur_tex = mc_texture_from_handle(mc_get_int(elems[0], "texHandle", 0));
        }
    } else if (tex_obj) {
        cur_tex = mc_texture_from_handle(mc_get_int(tex_obj, "texHandle", 0));
    }

    /* env: view/proj from layout, effect snapshot from Effect3D */
    JavaObject* affine = mc_get_ref(layout, "affine");
    if (!affine) affine = mc_get_ref(layout, "affineArraySel");
    JavaArray* affine_arr = (JavaArray*)mc_get_ref(layout, "affineArray");
    McA a;
    if (affine) mc_affine_read(affine, &a);
    else for (int i = 0; i < 12; i++) a.m[i] = (i % 5 == 0) ? 4096 : 0;
    MCView view;
    mc_view_from_affine(&a, &view);

    MCProj proj;
    mc_proj_from_layout(layout, &proj);
    proj.ox = off_x;
    proj.oy = off_y;

    MCEffect eff;
    mc_effect_from_obj(effect, &eff);

    /* save raster clip to restore after COMMAND_CLIP mutations */
    int saved_vx = g_m3g.viewport_x, saved_vy = g_m3g.viewport_y;
    int saved_vw = g_m3g.viewport_width, saved_vh = g_m3g.viewport_height;
    int clip_x = 0, clip_y = 0, clip_w = saved_vw, clip_h = saved_vh;
    int env_attrs = 0;

    for (int i = 1; i < n; ) {
        jint cmd = cmds[i++];
        switch ((int)((uint32_t)cmd & 0xFF000000u)) {
            case MC_CMD_END:
                goto done;
            case MC_CMD_NOP:
                i += cmd & 0xFFFFFF;
                break;
            case MC_CMD_FLUSH:
                if (mc.bound) {
                    mc_blit_to_gfx();
                    mc_diag_frame();
                }
                break;
            case MC_CMD_ATTRIBUTE:
                env_attrs = cmd & 0xFFFFFF;
                eff.has_light = (env_attrs & MC_ENV_LIGHTING) != 0 && eff.has_light;
                eff.shading = (env_attrs & MC_ENV_TOON_SHADING) ? 1 : eff.shading;
                if (env_attrs & MC_ENV_SEMI_TRANS) eff.transparency = 1;
                break;
            case MC_CMD_AFFINE_INDEX: {
                int idx = cmd & 0xFFFFFF;
                if (affine_arr && affine_arr->element_type == DESC_OBJECT &&
                    idx >= 0 && idx < (int)affine_arr->length) {
                    JavaObject** elems = (JavaObject**)array_data(affine_arr);
                    JavaObject* sel = elems ? elems[idx] : NULL;
                    if (sel) {
                        mc_affine_read(sel, &a);
                        mc_view_from_affine(&a, &view);
                    }
                }
                break;
            }
            case MC_CMD_TEXTURE_INDEX: {
                int tid = cmd & 0xFFFFFF;
                if (texArr && texArr->element_type == DESC_OBJECT &&
                    tid >= 0 && tid < (int)texArr->length) {
                    JavaObject** elems = (JavaObject**)array_data(texArr);
                    JavaObject* sel = elems ? elems[tid] : NULL;
                    cur_tex = sel
                        ? mc_texture_from_handle(mc_get_int(sel, "texHandle", 0))
                        : NULL;
                } else if (tex_obj && tid == 0) {
                    cur_tex = mc_texture_from_handle(mc_get_int(tex_obj, "texHandle", 0));
                }
                break;
            }
            case MC_CMD_CENTER:
                if (i + 1 < n) {
                    proj.cx = cmds[i++];
                    proj.cy = cmds[i++];
                }
                break;
            case MC_CMD_CLIP:
                if (i + 3 < n) {
                    int cx = cmds[i++], cy = cmds[i++], cw = cmds[i++], ch = cmds[i++];
                    int x0 = cx > clip_x ? cx : clip_x;
                    int y0 = cy > clip_y ? cy : clip_y;
                    int x1 = (cx + cw < clip_x + clip_w) ? cx + cw : clip_x + clip_w;
                    int y1 = (cy + ch < clip_y + clip_h) ? cy + ch : clip_y + clip_h;
                    clip_x = x0; clip_y = y0;
                    clip_w = x1 > x0 ? x1 - x0 : 0;
                    clip_h = y1 > y0 ? y1 - y0 : 0;
                    g_m3g.viewport_x = clip_x;
                    g_m3g.viewport_y = clip_y;
                    g_m3g.viewport_width = clip_w;
                    g_m3g.viewport_height = clip_h;
                }
                break;
            case MC_CMD_PARALLEL_SCALE:
                if (i + 1 < n) {
                    /* setOrthographicScale(sx, sy): parallel, pixels/unit */
                    proj.mode = 0;
                    proj.scale_x = cmds[i++];
                    proj.scale_y = cmds[i++];
                    if (proj.scale_x == 0) proj.scale_x = 512;
                    if (proj.scale_y == 0) proj.scale_y = 512;
                    mc_proj_finalize(&proj);
                }
                break;
            case MC_CMD_PARALLEL_SIZE:
                if (i + 1 < n) {
                    proj.mode = 1;
                    proj.par_w = cmds[i++];
                    proj.par_h = cmds[i++];
                    mc_proj_finalize(&proj);
                }
                break;
            case MC_CMD_PERSPECTIVE_FOV:
                if (i + 2 < n) {
                    proj.mode = 2;
                    proj.near = (float)cmds[i++];
                    proj.far = (float)cmds[i++];
                    proj.angle = cmds[i++];
                    mc_proj_finalize(&proj);
                }
                break;
            case MC_CMD_PERSPECTIVE_WH:
                if (i + 3 < n) {
                    proj.mode = 3;
                    proj.near = (float)cmds[i++];
                    proj.far = (float)cmds[i++];
                    proj.persp_w = cmds[i++];
                    proj.persp_h = cmds[i++];
                    mc_proj_finalize(&proj);
                }
                break;
            case MC_CMD_AMBIENT_LIGHT:
                if (i < n) {
                    eff.amb = (float)cmds[i++] / 4096.0f;
                    if (eff.amb < 0.0f) eff.amb = 0.0f;
                    if (eff.amb > 1.0f) eff.amb = 1.0f;
                    eff.has_light = 1;
                }
                break;
            case MC_CMD_DIRECTION_LIGHT:
                if (i + 3 < n) {
                    float x = (float)cmds[i++];
                    float y = (float)cmds[i++];
                    float z = (float)cmds[i++];
                    float di = (float)cmds[i++] / 4096.0f;
                    float len = sqrtf(x * x + y * y + z * z);
                    if (len > 1e-9f) {
                        eff.light_dir[0] = -x / len;
                        eff.light_dir[1] = -y / len;
                        eff.light_dir[2] = -z / len;
                    }
                    eff.diri = di;
                    eff.has_light = 1;
                }
                break;
            case MC_CMD_THRESHOLD:
                if (i + 2 < n) {
                    eff.toon_threshold = cmds[i++];
                    eff.toon_high = cmds[i++];
                    eff.toon_low = cmds[i++];
                }
                break;
            default: {
                /* primitive rendering command: type byte 0x01..0x05 */
                int type = (int)((uint32_t)cmd & 0x07000000u);
                if (type == 0 || (int)((uint32_t)cmd >> 31)) {
                    native_throw_iae(jvm, thread, "drawCommandList: command");
                    goto done;
                }
                int tbyte = (int)((uint32_t)cmd >> 24) & 7;
                if (tbyte > 5) {
                    native_throw_iae(jvm, thread, "drawCommandList: primitive");
                    goto done;
                }
                int num = (int)(((uint32_t)cmd >> 16) & 0xFF);
                int sizeOf = MC_PRIM_SIZES[tbyte];
                int len = num * 3 * sizeOf;
                /* vertices */
                int vo = i; i += len;
                /* normals */
                int no = i, normalsLen = 0;
                int normal_mode = (int)((uint32_t)cmd & 0x300u);
                if (normal_mode == 0x200) { normalsLen = num * 3; i += normalsLen; }
                else if (normal_mode == 0x300) { normalsLen = len; i += len; }
                /* texture coords / sprite params */
                int to = i, texLen = 0;
                int texcoord_mode = (int)((uint32_t)cmd & 0x3000u);
                if (tbyte == 5) { /* point sprites */
                    if (texcoord_mode == 0x1000) { texLen = 8; i += 8; }
                    else if (texcoord_mode != 0) { texLen = num * 8; i += texLen; }
                } else if (texcoord_mode == 0x3000) {
                    texLen = num * 2 * sizeOf; i += texLen;
                }
                /* colors */
                int co = i, colLen = 0;
                int color_mode = (int)((uint32_t)cmd & 0xC00u);
                if (color_mode == 0x400) { colLen = 1; i += 1; }
                else if (color_mode == 0x800) { colLen = num; i += num; }
                if (i > n) {
                    native_throw_iae(jvm, thread, "drawCommandList: overflow");
                    goto done;
                }
                mc_hist_note(cmd);
                if (num > 0) {
                    mc_draw_prim_batch(&view, &proj, &eff, cur_tex, cmd, num,
                                       cmds + vo, len,
                                       cmds + no, normalsLen,
                                       cmds + to, texLen,
                                       cmds + co, colLen);
                }
                break;
            }
        }
    }
done:
    g_m3g.viewport_x = saved_vx;
    g_m3g.viewport_y = saved_vy;
    g_m3g.viewport_width = saved_vw;
    g_m3g.viewport_height = saved_vh;
    return NATIVE_RETURN_VOID();
}

/* ----------------------------------------------------------------------------
 * Stub class setup: instance fields + static constants
 * -------------------------------------------------------------------------- */

static void mc_add_field(JavaClass* clazz, const char* name, const char* desc) {
    if (!clazz || !name || !desc) return;
    for (int i = 0; i < clazz->fields_count; i++) {
        if (clazz->fields[i].name && strcmp(clazz->fields[i].name, name) == 0) return;
    }
    int n = clazz->fields_count + 1;
    JavaField* nf = (JavaField*)realloc(clazz->fields, (size_t)n * sizeof(JavaField));
    if (!nf) return;
    clazz->fields = nf;
    JavaField* f = &clazz->fields[clazz->fields_count];
    memset(f, 0, sizeof(JavaField));
    f->name = strdup(name);
    f->descriptor = strdup(desc);
    f->access_flags = ACC_PUBLIC;
    clazz->fields_count = n;
}

static void mc_add_static_int(JavaClass* clazz, const char* name, jint value) {
    if (!clazz || !name) return;
    /* idempotent */
    for (int i = 0; i < clazz->static_fields_count; i++) {
        if (clazz->static_fields[i].name &&
            strcmp(clazz->static_fields[i].name, name) == 0) return;
    }
    if (!clazz->static_fields) {
        clazz->static_fields = (JavaStaticField*)calloc(16, sizeof(JavaStaticField));
        if (!clazz->static_fields) return;
        clazz->static_fields_count = 0;
        clazz->static_fields_capacity = 16;
    }
    if (clazz->static_fields_count >= clazz->static_fields_capacity) {
        int cap = clazz->static_fields_capacity * 2;
        JavaStaticField* nf = (JavaStaticField*)realloc(
            clazz->static_fields, (size_t)cap * sizeof(JavaStaticField));
        if (!nf) return;
        memset(nf + clazz->static_fields_capacity, 0,
               (size_t)(cap - clazz->static_fields_capacity) * sizeof(JavaStaticField));
        clazz->static_fields = nf;
        clazz->static_fields_capacity = cap;
    }
    JavaStaticField* sf = &clazz->static_fields[clazz->static_fields_count++];
    memset(sf, 0, sizeof(JavaStaticField));
    sf->name = strdup(name);
    sf->descriptor = strdup("I");
    sf->value.i = value;
}

static int mc_class_has_instance_field(JavaClass* clazz, const char* name) {
    if (!clazz || !name) return 0;
    for (int i = 0; i < clazz->fields_count; i++) {
        if (clazz->fields[i].name && strcmp(clazz->fields[i].name, name) == 0) return 1;
    }
    return 0;
}

static int init_mc_stub_classes(JVM* jvm) {
    if (!jvm) return 0;
    extern JavaClass* get_or_create_stub_class(JVM* jvm, const char* class_name);

    const char* pkg = "com/mascotcapsule/micro3d/v3/";
    JavaClass* cls_affine = get_or_create_stub_class(jvm, "com/mascotcapsule/micro3d/v3/AffineTrans");
    JavaClass* cls_vector = get_or_create_stub_class(jvm, "com/mascotcapsule/micro3d/v3/Vector3D");
    JavaClass* cls_g3d = get_or_create_stub_class(jvm, "com/mascotcapsule/micro3d/v3/Graphics3D");
    JavaClass* cls_layout = get_or_create_stub_class(jvm, "com/mascotcapsule/micro3d/v3/FigureLayout");
    JavaClass* cls_effect = get_or_create_stub_class(jvm, "com/mascotcapsule/micro3d/v3/Effect3D");
    JavaClass* cls_light = get_or_create_stub_class(jvm, "com/mascotcapsule/micro3d/v3/Light");
    JavaClass* cls_texture = get_or_create_stub_class(jvm, "com/mascotcapsule/micro3d/v3/Texture");
    JavaClass* cls_figure = get_or_create_stub_class(jvm, "com/mascotcapsule/micro3d/v3/Figure");
    JavaClass* cls_action = get_or_create_stub_class(jvm, "com/mascotcapsule/micro3d/v3/ActionTable");
    JavaClass* cls_util = get_or_create_stub_class(jvm, "com/mascotcapsule/micro3d/v3/Util3D");
    (void)pkg;

    if (!cls_affine || !cls_vector || !cls_g3d || !cls_layout || !cls_effect ||
        !cls_light || !cls_texture || !cls_figure || !cls_action || !cls_util) {
        return 0;
    }

    /* v36.22 [MC-RESET]: readiness is PER-JVM now — read from the class
     * objects themselves, not from a process-global latch. The old
     * "mc_stub_classes_ready" latch survived the session teardown: session
     * 2's stub classes were created fresh (fields_count=0,
     * instance_size=header-only) but the latch skipped this whole function,
     * so mtraHandle/figHandle/texHandle writes FAILED silently and every
     * native getter read 0 — Treasure Towers relaunch died in c.d PC=23
     * ("/ by zero" on getNumFrames()==0), Micro3D games were broken after
     * ANY midlet switch. The field-presence probe below is true for THIS
     * JVM's classes iff the fields were already added to them (also makes
     * a double init within one JVM idempotent). */
    if (mc_class_has_instance_field(cls_action, "mtraHandle")) return 1;

    /* AffineTrans: public matrix fields (game bytecode pokes them directly) */
    for (int i = 0; i < 12; i++) mc_add_field(cls_affine, mc_affine_names[i], "I");

    /* Vector3D */
    mc_add_field(cls_vector, "x", "I");
    mc_add_field(cls_vector, "y", "I");
    mc_add_field(cls_vector, "z", "I");

    /* FigureLayout */
    mc_add_field(cls_layout, "affine", "Lcom/mascotcapsule/micro3d/v3/AffineTrans;");
    mc_add_field(cls_layout, "affineArray", "[Lcom/mascotcapsule/micro3d/v3/AffineTrans;");
    mc_add_field(cls_layout, "scaleX", "I");
    mc_add_field(cls_layout, "scaleY", "I");
    mc_add_field(cls_layout, "centerX", "I");
    mc_add_field(cls_layout, "centerY", "I");
    mc_add_field(cls_layout, "parallelWidth", "I");
    mc_add_field(cls_layout, "parallelHeight", "I");
    mc_add_field(cls_layout, "near", "I");
    mc_add_field(cls_layout, "far", "I");
    mc_add_field(cls_layout, "angle", "I");
    mc_add_field(cls_layout, "perspectiveWidth", "I");
    mc_add_field(cls_layout, "perspectiveHeight", "I");
    mc_add_field(cls_layout, "projection", "I");

    /* Effect3D */
    mc_add_field(cls_effect, "light", "Lcom/mascotcapsule/micro3d/v3/Light;");
    mc_add_field(cls_effect, "sphereTexture", "Lcom/mascotcapsule/micro3d/v3/Texture;");
    mc_add_field(cls_effect, "shadingType", "I");
    mc_add_field(cls_effect, "transparency", "I");
    mc_add_field(cls_effect, "toonThreshold", "I");
    mc_add_field(cls_effect, "toonHigh", "I");
    mc_add_field(cls_effect, "toonLow", "I");

    /* Light */
    mc_add_field(cls_light, "lightDir", "Lcom/mascotcapsule/micro3d/v3/Vector3D;");
    mc_add_field(cls_light, "dirIntensity", "I");
    mc_add_field(cls_light, "ambIntensity", "I");

    /* Texture */
    mc_add_field(cls_texture, "texHandle", "I");
    mc_add_field(cls_texture, "width", "I");
    mc_add_field(cls_texture, "height", "I");

    /* Figure */
    mc_add_field(cls_figure, "figHandle", "I");
    mc_add_field(cls_figure, "textures", "[Lcom/mascotcapsule/micro3d/v3/Texture;");
    mc_add_field(cls_figure, "selTexture", "I");
    mc_add_field(cls_figure, "pattern", "I");

    /* ActionTable */
    mc_add_field(cls_action, "mtraData", "[B");
    mc_add_field(cls_action, "numActions", "I");
    mc_add_field(cls_action, "mtraHandle", "I");

    /* Recalculate instance sizes (flat hierarchy — all extend Object) */
    jvm_recalculate_instance_size(jvm, cls_affine);
    jvm_recalculate_instance_size(jvm, cls_vector);
    jvm_recalculate_instance_size(jvm, cls_layout);
    jvm_recalculate_instance_size(jvm, cls_effect);
    jvm_recalculate_instance_size(jvm, cls_light);
    jvm_recalculate_instance_size(jvm, cls_texture);
    jvm_recalculate_instance_size(jvm, cls_figure);
    jvm_recalculate_instance_size(jvm, cls_action);
    jvm_recalculate_instance_size(jvm, cls_g3d);
    jvm_recalculate_instance_size(jvm, cls_util);

    /* Static constants (javadoc values) — Graphics3D */
    mc_add_static_int(cls_g3d, "COMMAND_LIST_VERSION_1_0", (jint)0xFE000001);
    mc_add_static_int(cls_g3d, "COMMAND_END", (jint)0x80000000);
    mc_add_static_int(cls_g3d, "COMMAND_NOP", (jint)0x81000000);
    mc_add_static_int(cls_g3d, "COMMAND_FLUSH", (jint)0x82000000);
    mc_add_static_int(cls_g3d, "COMMAND_ATTRIBUTE", (jint)0x83000000);
    mc_add_static_int(cls_g3d, "COMMAND_CLIP", (jint)0x84000000);
    mc_add_static_int(cls_g3d, "COMMAND_CENTER", (jint)0x85000000);
    mc_add_static_int(cls_g3d, "COMMAND_TEXTURE_INDEX", (jint)0x86000000);
    mc_add_static_int(cls_g3d, "COMMAND_AFFINE_INDEX", (jint)0x87000000);
    mc_add_static_int(cls_g3d, "COMMAND_PARALLEL_SCALE", (jint)0x90000000);
    mc_add_static_int(cls_g3d, "COMMAND_PARALLEL_SIZE", (jint)0x91000000);
    mc_add_static_int(cls_g3d, "COMMAND_PERSPECTIVE_FOV", (jint)0x92000000);
    mc_add_static_int(cls_g3d, "COMMAND_PERSPECTIVE_WH", (jint)0x93000000);
    mc_add_static_int(cls_g3d, "COMMAND_DIRECTION_LIGHT", (jint)0xA1000000);
    mc_add_static_int(cls_g3d, "COMMAND_AMBIENT_LIGHT", (jint)0xA0000000);
    mc_add_static_int(cls_g3d, "COMMAND_THRESHOLD", (jint)0xAF000000);
    mc_add_static_int(cls_g3d, "ENV_ATTR_LIGHTING", 1);
    mc_add_static_int(cls_g3d, "ENV_ATTR_SPHERE_MAP", 2);
    mc_add_static_int(cls_g3d, "ENV_ATTR_TOON_SHADING", 4);
    mc_add_static_int(cls_g3d, "ENV_ATTR_SEMI_TRANSPARENT", 8);
    mc_add_static_int(cls_g3d, "PATTR_BLEND_NORMAL", 0);
    mc_add_static_int(cls_g3d, "PATTR_LIGHTING", 1);
    mc_add_static_int(cls_g3d, "PATTR_SPHERE_MAP", 2);
    mc_add_static_int(cls_g3d, "PATTR_COLORKEY", 16);
    mc_add_static_int(cls_g3d, "PATTR_BLEND_HALF", 32);
    mc_add_static_int(cls_g3d, "PATTR_BLEND_ADD", 64);
    mc_add_static_int(cls_g3d, "PATTR_BLEND_SUB", 96);
    mc_add_static_int(cls_g3d, "PDATA_COLOR_NONE", 0);
    mc_add_static_int(cls_g3d, "PDATA_COLOR_PER_COMMAND", 1024);
    mc_add_static_int(cls_g3d, "PDATA_COLOR_PER_FACE", 2048);
    mc_add_static_int(cls_g3d, "PDATA_NORMAL_NONE", 0);
    mc_add_static_int(cls_g3d, "PDATA_NORMAL_PER_FACE", 512);
    mc_add_static_int(cls_g3d, "PDATA_NORMAL_PER_VERTEX", 768);
    mc_add_static_int(cls_g3d, "PDATA_POINT_SPRITE_PARAMS_PER_CMD", 4096);
    mc_add_static_int(cls_g3d, "PDATA_POINT_SPRITE_PARAMS_PER_FACE", 8192);
    mc_add_static_int(cls_g3d, "PDATA_POINT_SPRITE_PARAMS_PER_VERTEX", 12288);
    mc_add_static_int(cls_g3d, "PDATA_TEXURE_COORD", 12288);
    mc_add_static_int(cls_g3d, "PDATA_TEXURE_COORD_NONE", 0);
    mc_add_static_int(cls_g3d, "POINT_SPRITE_LOCAL_SIZE", 0);
    mc_add_static_int(cls_g3d, "POINT_SPRITE_PIXEL_SIZE", 1);
    mc_add_static_int(cls_g3d, "POINT_SPRITE_NO_PERS", 2);
    mc_add_static_int(cls_g3d, "POINT_SPRITE_PERSPECTIVE", 0);
    mc_add_static_int(cls_g3d, "PRIMITVE_POINTS", MC_PRIMITVE_POINTS);
    mc_add_static_int(cls_g3d, "PRIMITVE_LINES", MC_PRIMITVE_LINES);
    mc_add_static_int(cls_g3d, "PRIMITVE_TRIANGLES", MC_PRIMITVE_TRIANGLES);
    mc_add_static_int(cls_g3d, "PRIMITVE_QUADS", MC_PRIMITVE_QUADS);
    mc_add_static_int(cls_g3d, "PRIMITVE_POINT_SPRITES", MC_PRIMITVE_POINT_SPRITES);

    /* Effect3D constants */
    mc_add_static_int(cls_effect, "NORMAL_SHADING", 0);
    mc_add_static_int(cls_effect, "TOON_SHADING", 1);

    /* Util3D angle helpers */
    mc_add_static_int(cls_util, "PI_2", 2048);   /* not in spec; harmless */
    mc_add_static_int(cls_util, "IANGLE_90", 1024);

    /* v36.22 [MC-RESET]: no process-global latch here anymore — the
     * field-presence probe at the top of this function is the readiness
     * marker, and it is evaluated against THIS JVM's class objects on
     * every call. */
    return 1;
}

/* ----------------------------------------------------------------------------
 * Registration
 * -------------------------------------------------------------------------- */

void init_com_mascotcapsule_micro3d_v3(JVM* jvm) {
    if (!jvm) return;

    static const NativeMethodEntry methods[] = {
        /* Graphics3D */
        {"com/mascotcapsule/micro3d/v3/Graphics3D", "<init>", "()V", native_mc_g3d_init},
        {"com/mascotcapsule/micro3d/v3/Graphics3D", "getInstance", "()Lcom/mascotcapsule/micro3d/v3/Graphics3D;", mc_g3d_getInstance},
        {"com/mascotcapsule/micro3d/v3/Graphics3D", "bind", "(Ljavax/microedition/lcdui/Graphics;)V", native_mc_g3d_bind},
        {"com/mascotcapsule/micro3d/v3/Graphics3D", "flush", "()V", native_mc_g3d_flush},
        {"com/mascotcapsule/micro3d/v3/Graphics3D", "release", "(Ljavax/microedition/lcdui/Graphics;)V", native_mc_g3d_release},
        {"com/mascotcapsule/micro3d/v3/Graphics3D", "dispose", "()V", native_mc_g3d_init},
        {"com/mascotcapsule/micro3d/v3/Graphics3D", "renderPrimitives",
         "(Lcom/mascotcapsule/micro3d/v3/Texture;IILcom/mascotcapsule/micro3d/v3/FigureLayout;Lcom/mascotcapsule/micro3d/v3/Effect3D;II[I[I[I[I)V",
         native_mc_renderPrimitives},
        {"com/mascotcapsule/micro3d/v3/Graphics3D", "renderFigure",
         "(Lcom/mascotcapsule/micro3d/v3/Figure;IILcom/mascotcapsule/micro3d/v3/FigureLayout;Lcom/mascotcapsule/micro3d/v3/Effect3D;)V",
         native_mc_renderFigure},
        {"com/mascotcapsule/micro3d/v3/Graphics3D", "drawFigure",
         "(Lcom/mascotcapsule/micro3d/v3/Figure;IILcom/mascotcapsule/micro3d/v3/FigureLayout;Lcom/mascotcapsule/micro3d/v3/Effect3D;)V",
         native_mc_renderFigure},
        {"com/mascotcapsule/micro3d/v3/Graphics3D", "drawCommandList",
         "(Lcom/mascotcapsule/micro3d/v3/Texture;IILcom/mascotcapsule/micro3d/v3/FigureLayout;Lcom/mascotcapsule/micro3d/v3/Effect3D;[I)V",
         mc_g3d_drawCommandList},
        {"com/mascotcapsule/micro3d/v3/Graphics3D", "drawCommandList",
         "([Lcom/mascotcapsule/micro3d/v3/Texture;IILcom/mascotcapsule/micro3d/v3/FigureLayout;Lcom/mascotcapsule/micro3d/v3/Effect3D;[I)V",
         mc_g3d_drawCommandList},

        /* AffineTrans */
        {"com/mascotcapsule/micro3d/v3/AffineTrans", "<init>", "()V", mc_affine_init},
        {"com/mascotcapsule/micro3d/v3/AffineTrans", "<init>", "(Lcom/mascotcapsule/micro3d/v3/AffineTrans;)V", mc_affine_init_copy},
        {"com/mascotcapsule/micro3d/v3/AffineTrans", "<init>", "([I)V", mc_affine_init_arr},
        {"com/mascotcapsule/micro3d/v3/AffineTrans", "<init>", "([II)V", mc_affine_set_offset},
        {"com/mascotcapsule/micro3d/v3/AffineTrans", "<init>", "([[I)V", mc_affine_init_arr},
        {"com/mascotcapsule/micro3d/v3/AffineTrans", "<init>", "(IIIIIIIIIIII)V", mc_affine_init_12},
        {"com/mascotcapsule/micro3d/v3/AffineTrans", "setIdentity", "()V", mc_affine_setIdentity},
        {"com/mascotcapsule/micro3d/v3/AffineTrans", "set", "(IIIIIIIIIIII)V", mc_affine_set12},
        {"com/mascotcapsule/micro3d/v3/AffineTrans", "set", "(Lcom/mascotcapsule/micro3d/v3/AffineTrans;)V", mc_affine_set_copy},
        {"com/mascotcapsule/micro3d/v3/AffineTrans", "set", "([I)V", mc_affine_set_arr},
        {"com/mascotcapsule/micro3d/v3/AffineTrans", "set", "([[I)V", mc_affine_set_arr},
        {"com/mascotcapsule/micro3d/v3/AffineTrans", "set", "([II)V", mc_affine_set_offset},
        {"com/mascotcapsule/micro3d/v3/AffineTrans", "get", "([I)V", mc_affine_get_arr},
        {"com/mascotcapsule/micro3d/v3/AffineTrans", "get", "([II)V", mc_affine_get_offset},
        {"com/mascotcapsule/micro3d/v3/AffineTrans", "mul", "(Lcom/mascotcapsule/micro3d/v3/AffineTrans;)V", mc_affine_mul1},
        {"com/mascotcapsule/micro3d/v3/AffineTrans", "mul", "(Lcom/mascotcapsule/micro3d/v3/AffineTrans;Lcom/mascotcapsule/micro3d/v3/AffineTrans;)V", mc_affine_mul2},
        {"com/mascotcapsule/micro3d/v3/AffineTrans", "multiply", "(Lcom/mascotcapsule/micro3d/v3/AffineTrans;)V", mc_affine_mul1},
        {"com/mascotcapsule/micro3d/v3/AffineTrans", "multiply", "(Lcom/mascotcapsule/micro3d/v3/AffineTrans;Lcom/mascotcapsule/micro3d/v3/AffineTrans;)V", mc_affine_mul2},
        {"com/mascotcapsule/micro3d/v3/AffineTrans", "rotationX", "(I)V", mc_affine_rotationX},
        {"com/mascotcapsule/micro3d/v3/AffineTrans", "rotationY", "(I)V", mc_affine_rotationY},
        {"com/mascotcapsule/micro3d/v3/AffineTrans", "rotationZ", "(I)V", mc_affine_rotationZ},
        {"com/mascotcapsule/micro3d/v3/AffineTrans", "setRotationX", "(I)V", mc_affine_rotationX},
        {"com/mascotcapsule/micro3d/v3/AffineTrans", "setRotationY", "(I)V", mc_affine_rotationY},
        {"com/mascotcapsule/micro3d/v3/AffineTrans", "setRotationZ", "(I)V", mc_affine_rotationZ},
        {"com/mascotcapsule/micro3d/v3/AffineTrans", "rotationV", "(Lcom/mascotcapsule/micro3d/v3/Vector3D;I)V", mc_affine_setRotationV},
        {"com/mascotcapsule/micro3d/v3/AffineTrans", "setRotation", "(Lcom/mascotcapsule/micro3d/v3/Vector3D;I)V", mc_affine_setRotationV},
        {"com/mascotcapsule/micro3d/v3/AffineTrans", "lookAt", "(Lcom/mascotcapsule/micro3d/v3/Vector3D;Lcom/mascotcapsule/micro3d/v3/Vector3D;Lcom/mascotcapsule/micro3d/v3/Vector3D;)V", mc_affine_lookAt_native},
        {"com/mascotcapsule/micro3d/v3/AffineTrans", "setViewTrans", "(Lcom/mascotcapsule/micro3d/v3/Vector3D;Lcom/mascotcapsule/micro3d/v3/Vector3D;Lcom/mascotcapsule/micro3d/v3/Vector3D;)V", mc_affine_lookAt_native},
        {"com/mascotcapsule/micro3d/v3/AffineTrans", "transform", "(Lcom/mascotcapsule/micro3d/v3/Vector3D;)Lcom/mascotcapsule/micro3d/v3/Vector3D;", mc_affine_transform},
        {"com/mascotcapsule/micro3d/v3/AffineTrans", "transPoint", "(Lcom/mascotcapsule/micro3d/v3/Vector3D;)Lcom/mascotcapsule/micro3d/v3/Vector3D;", mc_affine_transform},

        /* Vector3D */
        {"com/mascotcapsule/micro3d/v3/Vector3D", "<init>", "()V", mc_vec_init},
        {"com/mascotcapsule/micro3d/v3/Vector3D", "<init>", "(III)V", mc_vec_init3},
        {"com/mascotcapsule/micro3d/v3/Vector3D", "<init>", "(Lcom/mascotcapsule/micro3d/v3/Vector3D;)V", mc_vec_init_copy},
        {"com/mascotcapsule/micro3d/v3/Vector3D", "set", "(III)V", mc_vec_set3},
        {"com/mascotcapsule/micro3d/v3/Vector3D", "set", "(Lcom/mascotcapsule/micro3d/v3/Vector3D;)V", mc_vec_setv},
        {"com/mascotcapsule/micro3d/v3/Vector3D", "getX", "()I", mc_vec_getX},
        {"com/mascotcapsule/micro3d/v3/Vector3D", "getY", "()I", mc_vec_getY},
        {"com/mascotcapsule/micro3d/v3/Vector3D", "getZ", "()I", mc_vec_getZ},
        {"com/mascotcapsule/micro3d/v3/Vector3D", "setX", "(I)V", mc_vec_setX},
        {"com/mascotcapsule/micro3d/v3/Vector3D", "setY", "(I)V", mc_vec_setY},
        {"com/mascotcapsule/micro3d/v3/Vector3D", "setZ", "(I)V", mc_vec_setZ},
        {"com/mascotcapsule/micro3d/v3/Vector3D", "innerProduct", "(Lcom/mascotcapsule/micro3d/v3/Vector3D;)I", mc_vec_inner},
        {"com/mascotcapsule/micro3d/v3/Vector3D", "innerProduct", "(Lcom/mascotcapsule/micro3d/v3/Vector3D;Lcom/mascotcapsule/micro3d/v3/Vector3D;)I", mc_vec_inner_static},
        {"com/mascotcapsule/micro3d/v3/Vector3D", "outerProduct", "(Lcom/mascotcapsule/micro3d/v3/Vector3D;)V", mc_vec_outer},
        {"com/mascotcapsule/micro3d/v3/Vector3D", "outerProduct", "(Lcom/mascotcapsule/micro3d/v3/Vector3D;Lcom/mascotcapsule/micro3d/v3/Vector3D;)Lcom/mascotcapsule/micro3d/v3/Vector3D;", mc_vec_outer_static},
        {"com/mascotcapsule/micro3d/v3/Vector3D", "unit", "()V", mc_vec_unit},

        /* FigureLayout */
        {"com/mascotcapsule/micro3d/v3/FigureLayout", "<init>", "()V", mc_layout_init},
        {"com/mascotcapsule/micro3d/v3/FigureLayout", "<init>", "(Lcom/mascotcapsule/micro3d/v3/AffineTrans;IIII)V", mc_layout_init5},
        {"com/mascotcapsule/micro3d/v3/FigureLayout", "setAffineTrans", "(Lcom/mascotcapsule/micro3d/v3/AffineTrans;)V", mc_layout_setAffine},
        {"com/mascotcapsule/micro3d/v3/FigureLayout", "setAffineTrans", "([Lcom/mascotcapsule/micro3d/v3/AffineTrans;)V", mc_layout_setAffineArr},
        {"com/mascotcapsule/micro3d/v3/FigureLayout", "setAffineTransArray", "([Lcom/mascotcapsule/micro3d/v3/AffineTrans;)V", mc_layout_setAffineArr},
        {"com/mascotcapsule/micro3d/v3/FigureLayout", "selectAffineTrans", "(I)V", mc_layout_selectAffine},
        {"com/mascotcapsule/micro3d/v3/FigureLayout", "getAffineTrans", "()Lcom/mascotcapsule/micro3d/v3/AffineTrans;", mc_layout_getAffine},
        {"com/mascotcapsule/micro3d/v3/FigureLayout", "setCenter", "(II)V", mc_layout_setCenter},
        {"com/mascotcapsule/micro3d/v3/FigureLayout", "getCenterX", "()I", mc_layout_getCenterX},
        {"com/mascotcapsule/micro3d/v3/FigureLayout", "getCenterY", "()I", mc_layout_getCenterY},
        {"com/mascotcapsule/micro3d/v3/FigureLayout", "setScale", "(II)V", mc_layout_setScale},
        {"com/mascotcapsule/micro3d/v3/FigureLayout", "getScaleX", "()I", mc_layout_getScaleX},
        {"com/mascotcapsule/micro3d/v3/FigureLayout", "getScaleY", "()I", mc_layout_getScaleY},
        {"com/mascotcapsule/micro3d/v3/FigureLayout", "setParallelSize", "(II)V", mc_layout_setParallelSize},
        {"com/mascotcapsule/micro3d/v3/FigureLayout", "getParallelWidth", "()I", mc_layout_getParW},
        {"com/mascotcapsule/micro3d/v3/FigureLayout", "getParallelHeight", "()I", mc_layout_getParH},
        {"com/mascotcapsule/micro3d/v3/FigureLayout", "setPerspective", "(III)V", mc_layout_setPerspective},
        {"com/mascotcapsule/micro3d/v3/FigureLayout", "setPerspective", "(IIII)V", mc_layout_setPerspectiveWH},

        /* Effect3D */
        {"com/mascotcapsule/micro3d/v3/Effect3D", "<init>", "()V", mc_effect_init},
        {"com/mascotcapsule/micro3d/v3/Effect3D", "<init>", "(Lcom/mascotcapsule/micro3d/v3/Light;IZLcom/mascotcapsule/micro3d/v3/Texture;)V", mc_effect_init4},
        {"com/mascotcapsule/micro3d/v3/Effect3D", "setLight", "(Lcom/mascotcapsule/micro3d/v3/Light;)V", mc_effect_setLight},
        {"com/mascotcapsule/micro3d/v3/Effect3D", "getLight", "()Lcom/mascotcapsule/micro3d/v3/Light;", mc_effect_getLight},
        {"com/mascotcapsule/micro3d/v3/Effect3D", "setShadingType", "(I)V", mc_effect_setShadingType},
        {"com/mascotcapsule/micro3d/v3/Effect3D", "getShadingType", "()I", mc_effect_getShadingType},
        {"com/mascotcapsule/micro3d/v3/Effect3D", "setShading", "(I)V", mc_effect_setShadingType},
        {"com/mascotcapsule/micro3d/v3/Effect3D", "getShading", "()I", mc_effect_getShadingType},
        {"com/mascotcapsule/micro3d/v3/Effect3D", "setTransparency", "(Z)V", mc_effect_setTransparency},
        {"com/mascotcapsule/micro3d/v3/Effect3D", "isTransparency", "()Z", mc_effect_isTransparency},
        {"com/mascotcapsule/micro3d/v3/Effect3D", "setSemiTransparentEnabled", "(Z)V", mc_effect_setTransparency},
        {"com/mascotcapsule/micro3d/v3/Effect3D", "isSemiTransparentEnabled", "()Z", mc_effect_isTransparency},
        {"com/mascotcapsule/micro3d/v3/Effect3D", "setToonParams", "(III)V", mc_effect_setToonParams},
        {"com/mascotcapsule/micro3d/v3/Effect3D", "setThreshold", "(III)V", mc_effect_setToonParams},
        {"com/mascotcapsule/micro3d/v3/Effect3D", "getToonThreshold", "()I", mc_effect_getToonThreshold},
        {"com/mascotcapsule/micro3d/v3/Effect3D", "getToonHigh", "()I", mc_effect_getToonHigh},
        {"com/mascotcapsule/micro3d/v3/Effect3D", "getToonLow", "()I", mc_effect_getToonLow},
        {"com/mascotcapsule/micro3d/v3/Effect3D", "getThreshold", "()I", mc_effect_getToonThreshold},
        {"com/mascotcapsule/micro3d/v3/Effect3D", "getThresholdHigh", "()I", mc_effect_getToonHigh},
        {"com/mascotcapsule/micro3d/v3/Effect3D", "getThresholdLow", "()I", mc_effect_getToonLow},
        {"com/mascotcapsule/micro3d/v3/Effect3D", "setSphereTexture", "(Lcom/mascotcapsule/micro3d/v3/Texture;)V", mc_effect_setSphereMap},
        {"com/mascotcapsule/micro3d/v3/Effect3D", "getSphereTexture", "()Lcom/mascotcapsule/micro3d/v3/Texture;", mc_effect_getLight},
        {"com/mascotcapsule/micro3d/v3/Effect3D", "setSphereMap", "(Lcom/mascotcapsule/micro3d/v3/Texture;)V", mc_effect_setSphereMap},
        {"com/mascotcapsule/micro3d/v3/Effect3D", "getSphereMap", "()Lcom/mascotcapsule/micro3d/v3/Texture;", mc_effect_getLight},

        /* Light */
        {"com/mascotcapsule/micro3d/v3/Light", "<init>", "()V", mc_light_init},
        {"com/mascotcapsule/micro3d/v3/Light", "<init>", "(Lcom/mascotcapsule/micro3d/v3/Vector3D;II)V", mc_light_init3},
        {"com/mascotcapsule/micro3d/v3/Light", "setParallelLightDirection", "(Lcom/mascotcapsule/micro3d/v3/Vector3D;)V", mc_light_setDir},
        {"com/mascotcapsule/micro3d/v3/Light", "getParallelLightDirection", "()Lcom/mascotcapsule/micro3d/v3/Vector3D;", mc_light_getDir},
        {"com/mascotcapsule/micro3d/v3/Light", "setParallelLightIntensity", "(I)V", mc_light_setDirI},
        {"com/mascotcapsule/micro3d/v3/Light", "getParallelLightIntensity", "()I", mc_light_getDirI},
        {"com/mascotcapsule/micro3d/v3/Light", "setAmbientIntensity", "(I)V", mc_light_setAmbI},
        {"com/mascotcapsule/micro3d/v3/Light", "getAmbientIntensity", "()I", mc_light_getAmbI},
        {"com/mascotcapsule/micro3d/v3/Light", "setDirection", "(Lcom/mascotcapsule/micro3d/v3/Vector3D;)V", mc_light_setDir},
        {"com/mascotcapsule/micro3d/v3/Light", "getDirection", "()Lcom/mascotcapsule/micro3d/v3/Vector3D;", mc_light_getDir},
        {"com/mascotcapsule/micro3d/v3/Light", "setDirIntensity", "(I)V", mc_light_setDirI},
        {"com/mascotcapsule/micro3d/v3/Light", "getDirIntensity", "()I", mc_light_getDirI},
        {"com/mascotcapsule/micro3d/v3/Light", "setAmbIntensity", "(I)V", mc_light_setAmbI},
        {"com/mascotcapsule/micro3d/v3/Light", "getAmbIntensity", "()I", mc_light_getAmbI},

        /* Texture */
        {"com/mascotcapsule/micro3d/v3/Texture", "<init>", "(Ljava/lang/String;Z)V", mc_texture_init_str},
        {"com/mascotcapsule/micro3d/v3/Texture", "<init>", "([BZ)V", mc_texture_init_bytes},
        {"com/mascotcapsule/micro3d/v3/Texture", "dispose", "()V", mc_texture_dispose},

        /* Figure */
        {"com/mascotcapsule/micro3d/v3/Figure", "<init>", "([B)V", mc_figure_init_bytes},
        {"com/mascotcapsule/micro3d/v3/Figure", "<init>", "([BII)V", mc_figure_init_range},
        {"com/mascotcapsule/micro3d/v3/Figure", "<init>", "(Ljava/lang/String;)V", mc_figure_init_str},
        {"com/mascotcapsule/micro3d/v3/Figure", "setTexture", "(Lcom/mascotcapsule/micro3d/v3/Texture;)V", mc_figure_setTexture1},
        {"com/mascotcapsule/micro3d/v3/Figure", "setTexture", "([Lcom/mascotcapsule/micro3d/v3/Texture;)V", mc_figure_setTextureArr},
        {"com/mascotcapsule/micro3d/v3/Figure", "selectTexture", "(I)V", mc_figure_selectTexture},
        {"com/mascotcapsule/micro3d/v3/Figure", "setPattern", "(I)V", mc_figure_setPattern},
        {"com/mascotcapsule/micro3d/v3/Figure", "getNumPattern", "()I", mc_figure_getNumPattern},
        {"com/mascotcapsule/micro3d/v3/Figure", "getNumTextures", "()I", mc_figure_getNumTextures},
        {"com/mascotcapsule/micro3d/v3/Figure", "dispose", "()V", mc_figure_dispose},
        {"com/mascotcapsule/micro3d/v3/Figure", "getTexture", "()Lcom/mascotcapsule/micro3d/v3/Texture;", mc_figure_getTexture},
        {"com/mascotcapsule/micro3d/v3/Figure", "setPosture", "(Lcom/mascotcapsule/micro3d/v3/ActionTable;II)V", mc_figure_setPosture},
        {"com/mascotcapsule/micro3d/v3/Figure", "setPosture", "(Lcom/mascotcapsule/micro3d/v3/ActionTable;III)V", mc_figure_setPosture},

        /* ActionTable */
        {"com/mascotcapsule/micro3d/v3/ActionTable", "<init>", "([B)V", mc_action_init},
        {"com/mascotcapsule/micro3d/v3/ActionTable", "<init>", "([BII)V", mc_action_init},
        {"com/mascotcapsule/micro3d/v3/ActionTable", "<init>", "(Ljava/lang/String;)V", mc_action_init},
        {"com/mascotcapsule/micro3d/v3/ActionTable", "dispose", "()V", mc_action_dispose},
        {"com/mascotcapsule/micro3d/v3/ActionTable", "getNumAction", "()I", mc_action_getNumAction},
        {"com/mascotcapsule/micro3d/v3/ActionTable", "getNumActions", "()I", mc_action_getNumActions},
        {"com/mascotcapsule/micro3d/v3/ActionTable", "getNumFrames", "(I)I", mc_action_getNumFrames},
        {"com/mascotcapsule/micro3d/v3/ActionTable", "getNumFrame", "(I)I", mc_action_getNumFrames},

        /* Util3D */
        {"com/mascotcapsule/micro3d/v3/Util3D", "sin", "(I)I", mc_util_sin},
        {"com/mascotcapsule/micro3d/v3/Util3D", "cos", "(I)I", mc_util_cos},
        {"com/mascotcapsule/micro3d/v3/Util3D", "sqrt", "(I)I", mc_util_sqrt},
    };

    native_register_methods(jvm, methods, (int)(sizeof(methods) / sizeof(methods[0])));

    init_mc_stub_classes(jvm);
}
