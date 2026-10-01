/*
 * J2ME Emulator - MIDP2 Graphics API
 * Supports PNG, JPEG, BMP images via stb_image
 */

#define _USE_MATH_DEFINES
#include <math.h>
#include "debug.h"
#include "debug_macros.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "midp.h"
#include "jvm.h"
#include "bitmap_font.h"
#include "stb_image.h"
#include "render/render.h" /* v34.28: nojme_2d_scalar_forced() for getRGB A/B */

#include <pthread.h>

/* Current graphics context (reserved) */
__attribute__((unused))
static MidpGraphics* current_gfx = NULL;

/* ============================================================================
 * v36.12 PEER-REGISTRY — the "heap memory is not freed at closing" fix.
 *
 * PROBLEM (measured, scripts/soak_close.sh, Asphalt 3 x5 sessions): every
 * open/close cycle leaked ~430 KB of process malloc: 1060K -> 1490K ->
 * 1905K -> 2311K -> 2713K uord, perfectly linear. Root cause: the lcdui
 * NATIVE PEERS (MidpImage struct + its calloc'd pixel buffer, MidpGraphics
 * context, MidpFont) are malloc'd C memory referenced from Java objects
 * through the nativePeer field. When the Java object dies — at GC or at
 * session-end heap_destroy — NOBODY walks the dead objects to free the
 * peers: there is no finalizer and no peer registry. Every Image a game
 * decoded (menu.png ~97503 bytes -> 176x220x4 pixels...) leaked its peer
 * for the whole process lifetime; the Switch frontend lives for days, so
 * the field [MEM] line ("uord=298005K" after ONE heavy session) is this
 * leak scaled by game asset size.
 *
 * FIX: process-global registry of every peer we hand out (single choke
 * points: midp_image_create / midp_image_get_graphics / midp_font_get /
 * the GameCanvas graphics malloc in display.c). midp_peer_session_reset()
 * (called from midp_session_reset at every jvm_destroy) destroys whatever
 * is still registered — i.e. exactly the peers whose Java owners just died
 * with the heap. Peers freed during the session (midp_image_destroy,
 * midp_graphics_free, GameCanvas buffer re-create) unregister themselves,
 * so the sweep only ever frees live-registered memory: no double-free.
 * M3G is NOT in scope: its Image2D copies pixels into the VM heap (int[])
 * and destroys the temp MidpImage immediately (m3g_make_image2d_from_png),
 * and the M3G texture cache/buffers are freed by m3g_session_reset.
 * ============================================================================ */
typedef enum {
    MIDP_PEER_IMAGE = 0,
    MIDP_PEER_GFX   = 1,
    MIDP_PEER_FONT  = 2
} MidpPeerKind;

typedef struct { void* p; MidpPeerKind kind; } MidpPeerSlot;
static MidpPeerSlot* g_midp_peers = NULL;
static size_t g_midp_peer_count = 0;
static size_t g_midp_peer_cap = 0;
static pthread_mutex_t g_midp_peer_mutex = PTHREAD_MUTEX_INITIALIZER;

static void midp_peer_register(void* p, MidpPeerKind kind) {
    if (!p) return;
    pthread_mutex_lock(&g_midp_peer_mutex);
    if (g_midp_peer_count == g_midp_peer_cap) {
        size_t ncap = g_midp_peer_cap ? g_midp_peer_cap * 2 : 128;
        MidpPeerSlot* ns = (MidpPeerSlot*)realloc(g_midp_peers,
                                                  ncap * sizeof(MidpPeerSlot));
        if (ns) {
            g_midp_peers = ns;
            g_midp_peer_cap = ncap;
        } else {
            /* OOM growing the registry: skip the registration (the peer
             * then leaks — the pre-v36.12 status quo) instead of corrupting
             * the array. Unreachable in practice. */
            pthread_mutex_unlock(&g_midp_peer_mutex);
            return;
        }
    }
    g_midp_peers[g_midp_peer_count].p = p;
    g_midp_peers[g_midp_peer_count].kind = kind;
    g_midp_peer_count++;
    pthread_mutex_unlock(&g_midp_peer_mutex);
}

static void midp_peer_unregister(void* p) {
    if (!p) return;
    pthread_mutex_lock(&g_midp_peer_mutex);
    for (size_t i = 0; i < g_midp_peer_count; i++) {
        if (g_midp_peers[i].p == p) {
            /* swap-remove: order is irrelevant for a destroy-all sweep */
            g_midp_peers[i] = g_midp_peers[g_midp_peer_count - 1];
            g_midp_peer_count--;
            break;
        }
    }
    pthread_mutex_unlock(&g_midp_peer_mutex);
}

/* v36.12: the ONLY sanctioned way to free a MidpGraphics peer (unregisters,
 * so the session sweep cannot double-free it). */
void midp_graphics_free(MidpGraphics* gfx) {
    if (!gfx) return;
    midp_peer_unregister(gfx);
    free(gfx);
}

/* v36.12: register an EXTERNALLY created MidpGraphics peer (display.c
 * GameCanvas context). NEVER wrap stack/value graphics — only heap peers
 * owned by a nativePeer field. */
void midp_graphics_register(MidpGraphics* gfx) {
    midp_peer_register(gfx, MIDP_PEER_GFX);
}

void midp_peer_session_reset(void) {
    /* Quiescent point: jvm_destroy already verified every VM thread idle.
     * Steal the array under the lock, destroy OUTSIDE it — the destroyers
     * call midp_peer_unregister, which would self-deadlock otherwise. */
    pthread_mutex_lock(&g_midp_peer_mutex);
    MidpPeerSlot* slots = g_midp_peers;
    size_t n = g_midp_peer_count;
    g_midp_peers = NULL;
    g_midp_peer_count = 0;
    g_midp_peer_cap = 0;
    pthread_mutex_unlock(&g_midp_peer_mutex);

    if (!n) { free(slots); return; }
    size_t imgs = 0, gfxs = 0, fonts = 0;
    size_t img_bytes = 0;
    for (size_t i = 0; i < n; i++) {
        switch (slots[i].kind) {
            case MIDP_PEER_IMAGE:
                if (slots[i].p) {
                    MidpImage* im = (MidpImage*)slots[i].p;
                    img_bytes += (size_t)im->width * (size_t)im->height
                                 * sizeof(uint32_t) + sizeof(MidpImage);
                }
                midp_image_destroy((MidpImage*)slots[i].p); /* unregister = no-op */
                imgs++;
                break;
            case MIDP_PEER_GFX:
                midp_graphics_free((MidpGraphics*)slots[i].p);
                gfxs++;
                break;
            case MIDP_PEER_FONT:
                free(slots[i].p);
                fonts++;
                break;
        }
    }
    free(slots);
    /* Always-on one-liner (LOG_SAFE survives without logging toggle): this
     * IS the "close freed the game memory" proof for the field log. */
    LOG_SAFE("[PEERS] session reset: freed %zu image, %zu graphics, %zu font peer(s)"
             " (~%zu KB pixels)\n", imgs, gfxs, fonts, img_bytes >> 10);
}

/* Headless text capture - tracks drawn text for console output */
#ifdef J2ME_HEADLESS
typedef struct {
    char text[512];
    int x, y;
    uint32_t color;
} HeadlessTextEntry;

static HeadlessTextEntry g_headless_text_log[2048];
static int g_headless_text_count = 0;
static int g_headless_text_frame = 0; /* Incremented each paint cycle */
static int g_headless_last_logged_frame = -1;
static volatile int g_headless_text_activity = 0; /* Set when new text is drawn */

/* Check and reset text activity flag. Returns 1 if text was drawn since last call. */
int headless_check_text_activity(void) {
    int v = g_headless_text_activity;
    g_headless_text_activity = 0;
    return v;
}

/* Add a text entry to the headless log */
void midp_headless_log_text(const char* text, int x, int y, uint32_t color) {
    if (!text || text[0] == '\0') return;
    
    /* Skip very short or whitespace-only strings */
    int len = strlen(text);
    int printable = 0;
    for (int i = 0; i < len; i++) {
        if (text[i] > ' ') { printable++; }
    }
    if (printable < 2) return;
    
    /* Mark that we have new text activity (for idle detection) */
    g_headless_text_activity = 1;
    
    /* Print immediately to stdout so it's always visible */
    printf("[CANVAS TEXT] (%d,%d) color=0x%06X: %s\n", x, y, color, text);
    fflush(stdout);
    
    if (g_headless_text_count < 2048) {
        HeadlessTextEntry* entry = &g_headless_text_log[g_headless_text_count];
        strncpy(entry->text, text, sizeof(entry->text) - 1);
        entry->text[sizeof(entry->text) - 1] = '\0';
        entry->x = x;
        entry->y = y;
        entry->color = color;
        g_headless_text_count++;
    }
}

/* Print all unique texts captured during the last paint frame to stderr */
void headless_print_captured_text(void) {
    if (g_headless_text_count == 0 || g_headless_last_logged_frame == g_headless_text_frame) return;
    
    /* Deduplicate: only print texts we haven't seen before */
    static char seen_texts[2048][512];
    static int seen_count = 0;
    
    fprintf(stdout, "\n=== Screen text (frame %d) ===\n", g_headless_text_frame);
    
    for (int i = 0; i < g_headless_text_count; i++) {
        char* text = g_headless_text_log[i].text;
        int is_new = 1;
        
        /* Check if we've already logged this text */
        for (int j = 0; j < seen_count && j < 2048; j++) {
            if (strcmp(seen_texts[j], text) == 0) {
                is_new = 0;
                break;
            }
        }
        
        if (is_new && seen_count < 2048) {
            uint32_t c = g_headless_text_log[i].color;
            /* Extract RGB from packed color */
            int r = (c >> 16) & 0xFF;
            int g = (c >> 8) & 0xFF;
            int b = c & 0xFF;
            fprintf(stdout, "  [TEXT] (%d,%d) rgb(%d,%d,%d): %s\n",
                    g_headless_text_log[i].x, g_headless_text_log[i].y, r, g, b, text);
            snprintf(seen_texts[seen_count], 512, "%.511s", text);
            seen_count++;
        }
    }
    
    g_headless_last_logged_frame = g_headless_text_frame;
    fprintf(stdout, "=== End screen text ===\n\n");
    fflush(stdout);
}

/* Reset captured text for new paint cycle */
void headless_reset_text_capture(void) {
    g_headless_text_count = 0;
    g_headless_text_frame++;
}
#endif /* J2ME_HEADLESS */

/* Runtime screen dimensions (can be changed from libretro) */
static int g_midp_screen_width = MIDP_DEFAULT_WIDTH;
static int g_midp_screen_height = MIDP_DEFAULT_HEIGHT;

/* Set the screen dimensions (called from libretro core) */
void midp_set_screen_dimensions(int width, int height) {
    g_midp_screen_width = width;
    g_midp_screen_height = height;
    DEBUG_LOG("[MIDP] Screen dimensions set to %dx%d", width, height);
}

/* v34.20: set_pixel fwd decl removed — the 2D primitives (incl. the
 * set_pixel definition) now live in src/render/render.c. */



/* Initialize MIDP2 */
int midp_init(JVM* jvm) {
    /* Initialize MIDP2 native methods */
    init_javax_microedition_lcdui_graphics(jvm);
    init_javax_microedition_lcdui_display(jvm);
    init_javax_microedition_lcdui_image(jvm);
    init_javax_microedition_lcdui_font(jvm);
    init_javax_microedition_lcdui_game_gamecanvas(jvm);
    init_javax_microedition_lcdui_form(jvm);  /* Form, TextField, ChoiceGroup, etc. */
    init_javax_microedition_rms(jvm);
    
    /* Initialize Java lang classes */
    extern void init_java_lang_integer(JVM* jvm);
    extern void init_java_util_random(JVM* jvm);
    init_java_lang_integer(jvm);
    init_java_util_random(jvm);
    
    /* Seed random number generator */
    srand((unsigned int)time(NULL));
    
    return JNI_OK;
}

/* ============================================================================
 * v35.08 MULTI-SESSION (Switch frontend: menu -> game -> menu -> game):
 * process-global MIDP statics pointed into the PREVIOUS session's Java
 * heap. jvm_destroy() NULLs every root-registered slot centrally
 * (gc_roots_reset_all) and then calls THIS hook (weak) for everything the
 * root wipe cannot reach: unrooted object pointers, "already rooted"
 * latches, queue indices, UI booleans. Field-repro for the bug class:
 * game two launched from the frontend menu died in Display.setCurrent on
 * a stale Display singleton ("Object has NULL or invalid class pointer").
 * ============================================================================ */
void midp_session_reset(void) {
    extern void midp_display_session_reset(void);
    extern void midp_form_session_reset(void);
    extern void m3g_session_reset(void);
    extern void midp_rms_session_reset(void);
    /* v36.22 [MC-RESET] DIAG: Mascot Capsule ctor diagnostics are per-session
     * counters — without this session 2+ printed nothing about its own
     * ActionTable/Figure startup (the Treasure Towers relaunch crash was
     * invisible in the field log). */
    extern void mc_diag_session_reset(void);
    mc_diag_session_reset();
    midp_display_session_reset();
    midp_form_session_reset();
    m3g_session_reset();
    midp_rms_session_reset();
    /* v36.12 PEER-REGISTRY: LAST — the layer resets above just NULL their
     * pointers (game_canvas.graphics/offscreen_buffer etc.); the sweep
     * then frees every still-registered lcdui peer of the dead session.
     * Measured on the v36.11 soak: exactly the ~430 KB/session leak. */
    midp_peer_session_reset();
}

/*
 * Graphics operations
 */

void midp_graphics_init(MidpGraphics* gfx, uint32_t* pixels, int width, int height) {
    gfx->pixels = pixels;
    gfx->width = width;
    gfx->height = height;
    gfx->clip_x = 0;
    gfx->clip_y = 0;
    gfx->clip_width = width;
    gfx->clip_height = height;
    gfx->translate_x = 0;
    gfx->translate_y = 0;
    gfx->rgb_color = 0x000000;
    gfx->alpha = 255;
    gfx->stroke_style = 0;
    gfx->font = 0;
    gfx->owner_image = NULL; /* v34.51: set by midp_image_get_graphics */
}

void midp_graphics_set_clip(MidpGraphics* gfx, int x, int y, int width, int height) {
    /* NOTE: this is a LOW-LEVEL helper: x/y are DEVICE-space coordinates.
     * The Java-facing MIDP translation semantics (clip coords relative to
     * the current translate, JSR-118) are applied in the nativeGraphics
     * setClip/clipRect handlers (display.c) which add gfx->translate_*.
     * Internal emulator paths (soft bar, LayerManager) call this with
     * device coords directly. */
    gfx->clip_x = x;
    gfx->clip_y = y;
    gfx->clip_width = width;
    gfx->clip_height = height;
}

void midp_graphics_clip_rect(MidpGraphics* gfx, int x, int y, int width, int height) {
    /* Intersect with current clip */
    int x1 = gfx->clip_x > x ? gfx->clip_x : x;
    int y1 = gfx->clip_y > y ? gfx->clip_y : y;
    int x2 = (gfx->clip_x + gfx->clip_width) < (x + width) ? 
             (gfx->clip_x + gfx->clip_width) : (x + width);
    int y2 = (gfx->clip_y + gfx->clip_height) < (y + height) ?
             (gfx->clip_y + gfx->clip_height) : (y + height);
    
    int new_w = x2 - x1;
    int new_h = y2 - y1;
    /* Clamp to 0 when there is no overlap */
    if (new_w < 0) new_w = 0;
    if (new_h < 0) new_h = 0;
    
    gfx->clip_x = x1;
    gfx->clip_y = y1;
    gfx->clip_width = new_w;
    gfx->clip_height = new_h;
}

void midp_graphics_translate(MidpGraphics* gfx, int x, int y) {
    gfx->translate_x += x;
    gfx->translate_y += y;
}

void midp_graphics_set_color(MidpGraphics* gfx, int rgb, int alpha) {
    GFX_DEBUG("setColor: RGB=0x%06X, alpha=%d", rgb & 0xFFFFFF, alpha);
    gfx->rgb_color = rgb & 0xFFFFFF;
    gfx->alpha = alpha;
}


/*
 * Image operations
 */

/* Maximum image dimension to prevent OOM from malformed data.
 * v34.77 FIX (Asphalt 4 loading freeze): the old 2048 cap rejected the
 * game's perfectly legal 2500x1 loading-bar strips (GLLib's f.P builds a
 * width x 1 progress bar; real phones impose NO dimension cap — only
 * available memory). The rejection made midp_image_create return NULL,
 * which the JNI layer mapped to OutOfMemoryError — an Error, not an
 * Exception — so the game's catch(Exception) in its paint() wrapper let
 * it escape, the reentrancy guard stayed set and every later paint()
 * returned instantly: permanent loading-screen freeze. Raised to 4096;
 * MIDP_MAX_IMAGE_PIXELS plus the calloc failure path still guard against
 * genuinely absurd (decoder-garbage) requests. */
#define MIDP_MAX_IMAGE_DIMENSION 4096
#define MIDP_MAX_IMAGE_PIXELS (MIDP_MAX_IMAGE_DIMENSION * MIDP_MAX_IMAGE_DIMENSION)

/* [v34.20] All 2D drawing primitives (set_pixel, drawLine, fillRect,
 * arcs, glyphs, drawString, drawImage/drawRegion, copyArea, getRGB)
 * moved to src/render/render.c — see render/render.h. */

MidpImage* midp_image_create(int width, int height, bool mutable) {
    /* Validate dimensions to prevent OOM from malformed images */
    if (width <= 0 || height <= 0) {
        GFX_DEBUG("midp_image_create: Invalid dimensions: %dx%d", width, height);
        return NULL;
    }
    
    if (width > MIDP_MAX_IMAGE_DIMENSION || height > MIDP_MAX_IMAGE_DIMENSION) {
        GFX_DEBUG("midp_image_create: REJECTED - dimensions too large: %dx%d (max %d)", 
                width, height, MIDP_MAX_IMAGE_DIMENSION);
        return NULL;
    }
    
    /* Check for integer overflow */
    size_t pixel_count = (size_t)width * (size_t)height;
    if (pixel_count > MIDP_MAX_IMAGE_PIXELS) {
        GFX_DEBUG("midp_image_create: REJECTED - pixel count overflow: %zu", pixel_count);
        return NULL;
    }
    
    MidpImage* img = (MidpImage*)malloc(sizeof(MidpImage));
    if (!img) return NULL;
    
    img->pixels = (uint32_t*)calloc(pixel_count, sizeof(uint32_t));
    if (!img->pixels) {
        free(img);
        return NULL;
    }
    
    img->width = width;
    img->height = height;
    img->mutable = mutable;
    img->alpha = true;
    /* v34.51 PINK-FIX: mutable images start white-opaque but graphics ops
     * may later write alpha != 0xFF — the scan stays unvalidated (lazily
     * recomputed on first draw, invalidated on alpha writes). */
    img->alpha_scan_valid = false;
    img->alpha_all_opaque = false;
    
    /* MIDP spec: mutable images are initially filled with white pixels */
    if (mutable) {
        memset(img->pixels, 0xFF, pixel_count * sizeof(uint32_t));
    }
    
    /* v36.12 PEER-REGISTRY: single choke point of ALL image creation
     * (create_from_rgb / create_from_data funnel through here). The peer
     * dies either via midp_image_destroy (unregisters) or at the session
     * sweep when its Java owner died with the heap. */
    midp_peer_register(img, MIDP_PEER_IMAGE);
    
    return img;
}

/* v34.51 PINK-FIX: exact per-image opacity.
 *
 * The old drawImage/drawRegion heuristic sampled the corner pixels of the
 * blitted region; when all happened to be opaque it memcpy'd the whole
 * sprite. Sprites whose transparent color-key pixels (pink (255,0,255)
 * with alpha==0 from PNG tRNS) were INSIDE the region — e.g. a car sprite
 * with an opaque outline — passed that corner test and the raw pink pixels
 * were copied to the framebuffer, where the presentation stage (XRGB8888/
 * RGB565) drops alpha -> PINK BOXES on screen.
 *
 * This function replaces the heuristic with the exact cached state:
 * alpha==false means the image can never contain transparency; otherwise
 * a one-time full scan (early-exit on the first non-opaque pixel) decides
 * memcpy vs blend. Immutable images get the scan folded into their decode
 * loops (see below); mutable images rescan after invalidating writes. */
bool midp_image_all_opaque(MidpImage* img) {
    if (!img || !img->pixels) return false;
    if (!img->alpha) return true;            /* no transparency possible */
    if (!img->alpha_scan_valid) {
        const size_t total = (size_t)img->width * (size_t)img->height;
        const uint32_t* p = img->pixels;
        bool all = true;
        for (size_t i = 0; i < total; i++) {
            if ((p[i] >> 24) != 0xFFu) { all = false; break; }
        }
        img->alpha_all_opaque = all ? JNI_TRUE : JNI_FALSE;
        img->alpha_scan_valid = JNI_TRUE;
    }
    return img->alpha_all_opaque ? true : false;
}

void midp_image_invalidate_alpha_scan(MidpImage* img) {
    if (img && img->alpha) img->alpha_scan_valid = JNI_FALSE;
}

MidpImage* midp_image_create_from_rgb(const jint* rgb, int width, int height,
                                       bool process_alpha) {
    /* createRGBImage creates immutable images per MIDP spec */
    MidpImage* img = midp_image_create(width, height, false);
    if (!img) return NULL;
    
    bool has_nonopaque = false; /* v34.51: exact scan, folded into the copy */
    for (int i = 0; i < width * height; i++) {
        if (process_alpha) {
            img->pixels[i] = (uint32_t)rgb[i];
            if ((img->pixels[i] >> 24) != 0xFFu) has_nonopaque = true;
        } else {
            img->pixels[i] = 0xFF000000 | ((uint32_t)rgb[i] & 0xFFFFFF);
        }
    }
    
    img->alpha = process_alpha && has_nonopaque;
    img->alpha_scan_valid = JNI_TRUE;      /* exact knowledge from this loop */
    img->alpha_all_opaque = !img->alpha;
    return img;
}

void midp_image_destroy(MidpImage* img) {
    if (img) {
        /* v36.12 PEER-REGISTRY: unregister FIRST (while the pointer is
         * still the live key), then free — the session sweep must never
         * see (and double-free) an already-destroyed peer. */
        midp_peer_unregister(img);
        free(img->pixels);
        free(img);
    }
}

void midp_image_get_rgb(MidpImage* img, jint* rgb, int offset, int scanlength,
                        int x, int y, int width, int height) {
    if (!img || !rgb) return;

    /* v34.28: hoisted bounds — the in-bounds source window per row is ONE
     * contiguous run; same elements, same 32-bit values, bit-identical to
     * the per-pixel reference (kept for NOJME_2D_SCALAR=1). */
    if (nojme_2d_scalar_forced()) {
        for (int py = 0; py < height; py++) {
            int src_y = y + py;
            if (src_y < 0 || src_y >= img->height) continue;
            
            for (int px = 0; px < width; px++) {
                int src_x = x + px;
                if (src_x < 0 || src_x >= img->width) continue;
                
                rgb[offset + py * scanlength + px] = 
                    (jint)img->pixels[src_y * img->width + src_x];
            }
        }
        return;
    }

    const int sxlo = x > 0 ? x : 0;
    const int sylo = y > 0 ? y : 0;
    const int sxhi = ((long long)x + width < (long long)img->width) ? (x + width) : img->width;
    const int syhi = ((long long)y + height < (long long)img->height) ? (y + height) : img->height;
    const int pxlo = sxlo - x, pxhi = sxhi - x;
    const int pylo = sylo - y, pyhi = syhi - y;
    const int run = pxhi - pxlo;

    for (int py = pylo; py < pyhi; py++) {
        const uint32_t* src_row = img->pixels + (size_t)(y + py) * img->width + sxlo;
        jint* dst_row = rgb + (size_t)offset + (size_t)py * scanlength + pxlo;
        if (run == width && run > 8) {
            memcpy(dst_row, src_row, (size_t)run * sizeof(uint32_t));
        } else {
            for (int px = 0; px < run; px++) {
                dst_row[px] = (jint)src_row[px];
            }
        }
    }
}

MidpGraphics* midp_image_get_graphics(MidpImage* img) {
    if (!img || !img->mutable) {
        return NULL;
    }
    
    MidpGraphics* gfx = (MidpGraphics*)malloc(sizeof(MidpGraphics));
    if (!gfx) return NULL;
    
    midp_graphics_init(gfx, img->pixels, img->width, img->height);
    gfx->owner_image = img; /* v34.51: alpha-scan invalidation hook */
    
    /* v36.12 PEER-REGISTRY: Graphics peers die at the session sweep when
     * their Java wrapper is gone (no GC finalizer for nativePeer fields). */
    midp_peer_register(gfx, MIDP_PEER_GFX);
    
    return gfx;
}

/* ============================================
 * Image decoding via stb_image
 * Supports: PNG, JPEG, BMP
 * ============================================ */

/* Check if data is a PNG */
static bool is_png(const uint8_t* data, int len) {
    static const uint8_t png_sig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    return len >= 8 && memcmp(data, png_sig, 8) == 0;
}

/* Check if data is a JPEG */
static bool is_jpeg(const uint8_t* data, int len) {
    return len >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF;
}

/* Check if data is a BMP */
static bool is_bmp(const uint8_t* data, int len) {
    return len >= 2 && data[0] == 'B' && data[1] == 'M';
}

/* Decode image data to RGBA pixels using stb_image */
MidpImage* midp_image_create_from_data(const uint8_t* data, int offset, int length) {
    if (!data || length < 8) return NULL;
    
    const uint8_t* img_data = data + offset;
    int img_len = length - offset;
    
    /* Check image format for logging */
    const char* format = "unknown";
    if (is_png(img_data, img_len)) format = "PNG";
    else if (is_jpeg(img_data, img_len)) format = "JPEG";
    else if (is_bmp(img_data, img_len)) format = "BMP";
    
    /* Decode using stb_image */
    int width, height, channels;
    uint8_t* pixels = stbi_load_from_memory(img_data, img_len, &width, &height, &channels, 4);
    
    if (!pixels) {
        GFX_DEBUG("Failed to decode %s: %s", format, stbi_failure_reason());
        return NULL;
    }
    
    GFX_DEBUG("Decoded %s: %dx%d (%d channels)", format, width, height, channels);
    
    /* Create MIDP image (resource-loaded images are immutable per MIDP spec) */
    MidpImage* img = midp_image_create(width, height, false);
    if (!img) {
        stbi_image_free(pixels);
        return NULL;
    }
    
    /* Convert from stb_image format (RGBA bytes) to our format (ARGB uint32_t)
     * Optimized: pointer arithmetic, process 4 pixels at a time
     * v34.51 PINK-FIX: the loop already touches every pixel — fold the exact
     * opacity scan in (has_nonopaque). An RGBA PNG whose pixels are ALL
     * alpha==0xFF is marked alpha==false: drawImage/drawRegion then use the
     * memcpy path, exactly like an RGB PNG. A PNG with real transparency
     * (tRNS color-key incl. pink (255,0,255,0) backgrounds) keeps alpha==true
     * and always renders through the src-over path — no more pink boxes. */
    {
        const uint8_t *src = pixels;
        uint32_t *dst = img->pixels;
        int total = width * height;
        int i = 0;
        bool has_nonopaque = false;
        
        /* Process 4 pixels at a time for better instruction pipelining */
        for (; i + 3 < total; i += 4) {
            dst[i]     = ((uint32_t)src[3] << 24) | ((uint32_t)src[0] << 16) | ((uint32_t)src[1] << 8) | src[2];
            dst[i + 1] = ((uint32_t)src[7] << 24) | ((uint32_t)src[4] << 16) | ((uint32_t)src[5] << 8) | src[6];
            dst[i + 2] = ((uint32_t)src[11] << 24) | ((uint32_t)src[8] << 16) | ((uint32_t)src[9] << 8) | src[10];
            dst[i + 3] = ((uint32_t)src[15] << 24) | ((uint32_t)src[12] << 16) | ((uint32_t)src[13] << 8) | src[14];
            if (src[3] != 0xFF || src[7] != 0xFF || src[11] != 0xFF || src[15] != 0xFF)
                has_nonopaque = true;
            src += 16;
        }
        /* Handle remaining pixels */
        for (; i < total; i++) {
            dst[i] = ((uint32_t)src[3] << 24) | ((uint32_t)src[0] << 16) | ((uint32_t)src[1] << 8) | src[2];
            if (src[3] != 0xFF) has_nonopaque = true;
            src += 4;
        }
        
        img->alpha = (channels == 4) && has_nonopaque;
        img->alpha_scan_valid = JNI_TRUE;  /* exact knowledge from the loop */
        img->alpha_all_opaque = !img->alpha;
    }
    
    stbi_image_free(pixels);
    return img;
}

/*
 * Font operations
 */

/* v36.29 [FONT-2X]: pixel scale of the shared 5x7 bitmap font for a font
 * object. SIZE_LARGE renders and measures 2x — the standard J2ME way to
 * get bigger text (FACE/STYLE stay cosmetic). SIZE_SMALL/SIZE_MEDIUM map
 * to scale 1 (there is no smaller bitmap). */
int midp_font_pixel_scale(const MidpFont* font) {
    return (font && font->size == FONT_SIZE_LARGE) ? 2 : 1;
}

/* v36.29 [FONT-HEIGHT]: getHeight() must include the inter-line LEADING.
 * The glyph box is 7px tall and 5x7 glyphs have ink in BOTH the first and
 * the last row (285/328 glyphs ink row 0, all ink row 6), so with the old
 * height==7 games drawing multi-line text with y += getHeight() produced
 * zero pixel gap — "буквы рисуются вплотную друг к другу по высоте".
 * Real devices always return ascent+descent+leading here. 7 + 2 = 9. */
#define MIDP_FONT_LINE_HEIGHT (FONT_HEIGHT + 2)
#define MIDP_FONT_BASELINE    (FONT_HEIGHT - 2)   /* ascent: unchanged */

MidpFont* midp_font_get_default(void) {
    static MidpFont default_font = {
        .face = FONT_FACE_SYSTEM,
        .style = FONT_STYLE_PLAIN,
        .size = FONT_SIZE_MEDIUM,
        .height = MIDP_FONT_LINE_HEIGHT,
        .baseline = MIDP_FONT_BASELINE,
        .native_font = NULL
    };
    return &default_font;
}

MidpFont* midp_font_get(int face, int style, int size) {
    MidpFont* font = (MidpFont*)malloc(sizeof(MidpFont));
    if (!font) return NULL;
    
    font->face = face;
    font->style = style;
    font->size = size;
    /* v36.29: LARGE is the 2x font — metrics double together with the
     * rendering scale (midp_font_pixel_scale); SMALL shares MEDIUM. */
    if (size == FONT_SIZE_LARGE) {
        font->height = MIDP_FONT_LINE_HEIGHT * 2;
        font->baseline = MIDP_FONT_BASELINE * 2;
    } else {
        font->height = MIDP_FONT_LINE_HEIGHT;
        font->baseline = MIDP_FONT_BASELINE;
    }
    font->native_font = NULL;
    
    /* v36.12 PEER-REGISTRY: Font.getFont() had NO destroy path at all —
     * every call leaked a MidpFont for the process lifetime. */
    midp_peer_register(font, MIDP_PEER_FONT);
    
    return font;
}

int midp_font_string_width(MidpFont* font, const char* str) {
    if (!font || !str) return 0;
    /* Each character is FONT_WIDTH pixels + 1 pixel spacing, scaled */
    int scale = midp_font_pixel_scale(font);
    /* Count UTF-8 characters, not bytes */
    int char_count = utf8_strlen(str);
    /* Prevent integer overflow for very long strings */
    if (char_count > 4096) char_count = 4096;
    return char_count > 0 ? char_count * ((FONT_WIDTH + 1) * scale) - scale : 0;
}

int midp_font_char_width(MidpFont* font, jchar ch) {
    (void)ch;
    if (!font) return 0;
    return FONT_WIDTH * midp_font_pixel_scale(font);
}

int midp_font_height(MidpFont* font) {
    return font ? font->height : MIDP_FONT_LINE_HEIGHT;
}

int midp_font_baseline_position(MidpFont* font) {
    return font ? font->baseline : MIDP_FONT_BASELINE;
}

/*
 * Display operations
 */

void midp_display_get_dimensions(int* width, int* height) {
    if (width) *width = g_midp_screen_width;
    if (height) *height = g_midp_screen_height;
}

bool midp_display_is_color(void) {
    return true;
}

int midp_display_num_colors(void) {
    return 16777216;  /* 24-bit color, matches FreeJ2ME */
}

int midp_display_num_alpha_levels(void) {
    return 256;
}
