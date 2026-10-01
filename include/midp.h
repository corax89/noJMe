/*
 * J2ME Emulator - MIDP2 API
 * Mobile Information Device Profile 2.0 native implementations
 */

#ifndef MIDP_H
#define MIDP_H

#include "jvm.h"

/*
 * MIDP2 Screen dimensions (configurable)
 */
#define MIDP_DEFAULT_WIDTH  240
#define MIDP_DEFAULT_HEIGHT 320

/*
 * Graphics constants
 */
#define GFX_ALPHA_OPAQUE      255
#define GFX_ALPHA_TRANSLUCENT 0
#define GFX_ALPHA_BITMASK     1

/* Anchor points for text and image drawing */
#define GFX_TOP_LEFT          0
#define GFX_TOP_HCENTER       1
#define GFX_TOP_RIGHT         2
#define GFX_VCENTER_LEFT      3
#define GFX_VCENTER_HCENTER   4
#define GFX_VCENTER_RIGHT     5
#define GFX_BOTTOM_LEFT       6
#define GFX_BOTTOM_HCENTER    7
#define GFX_BOTTOM_RIGHT      8
#define GFX_BASELINE_LEFT     9
#define GFX_BASELINE_HCENTER  10
#define GFX_BASELINE_RIGHT    11

/* Face values for fonts */
#define FONT_FACE_SYSTEM       0
#define FONT_FACE_MONOSPACE    32
#define FONT_FACE_PROPORTIONAL 64

/* Style values for fonts */
#define FONT_STYLE_PLAIN      0
#define FONT_STYLE_BOLD       1
#define FONT_STYLE_ITALIC     2
#define FONT_STYLE_UNDERLINED 4

/* Size values for fonts */
#define FONT_SIZE_SMALL       8
#define FONT_SIZE_MEDIUM      0
#define FONT_SIZE_LARGE       16

/*
 * Game API constants
 */
#define GAME_UP     1
#define GAME_DOWN   6
#define GAME_LEFT   2
#define GAME_RIGHT  5
#define GAME_FIRE   8
#define GAME_A      9
#define GAME_B      10
#define GAME_C      11
#define GAME_D      12

/*
 * Display constants
 */
#define DISPLAY_WIDTH    240
#define DISPLAY_HEIGHT   320
#define DISPLAY_IS_COLOR true
#define DISPLAY_NUM_ALPHA_LEVELS 256
#define DISPLAY_NUM_COLORS 65536

/*
 * MIDP2 Event types
 */
typedef enum {
    MIDP_EVENT_KEY_PRESSED,
    MIDP_EVENT_KEY_RELEASED,
    MIDP_EVENT_KEY_REPEATED,
    MIDP_EVENT_POINTER_PRESSED,
    MIDP_EVENT_POINTER_RELEASED,
    MIDP_EVENT_POINTER_DRAGGED,
    MIDP_EVENT_COMMAND,
    MIDP_EVENT_SHOW_NOTIFY,
    MIDP_EVENT_HIDE_NOTIFY,
    MIDP_EVENT_SIZE_CHANGED,
    MIDP_EVENT_REPAINT
} MidpEventType;

/*
 * MIDP2 Event structure
 */
typedef struct {
    MidpEventType type;
    jint param1;
    jint param2;
    jint param3;
    void* data;
} MidpEvent;

/*
 * Graphics context (internal representation)
 * v34.51: owner_image is non-NULL only for Graphics objects obtained via
 * Image.getGraphics() — the 2D write ops use it to invalidate the image's
 * cached alpha scan whenever they can store a pixel with alpha != 0xFF
 * (see MidpImage.alpha_scan_valid below).
 */
typedef struct {
    uint32_t* pixels;
    jint width;
    jint height;
    jint clip_x;
    jint clip_y;
    jint clip_width;
    jint clip_height;
    jint translate_x;
    jint translate_y;
    jint rgb_color;
    jint alpha;
    jint stroke_style;  /* 0 = SOLID, 1 = DOTTED */
    /* v36.29: full pointer, NOT jint — the old (jint)(intptr_t) round-trip
     * silently truncated 64-bit heap addresses (setFont/getFont + the
     * SIZE_LARGE scale lookup dereferenced garbage). */
    void* font;
    struct MidpImage* owner_image; /* NULL for screen/canvas graphics */
} MidpGraphics;

/*
 * Image structure
 * v34.51 PINK-FIX: the drawImage/drawRegion opacity heuristic used to
 * sample the 4 corner pixels of the blitted region and, when all were
 * alpha==0xFF, memcpy'd the whole sprite — copying raw (255,0,255,0)
 * color-key pixels that the presentation stage shows as PINK. The two
 * fields below replace that heuristic with EXACT knowledge:
 *   alpha == JNI_FALSE          -> image can never contain transparency
 *                                 (RGB PNG / createRGBImage(false) / ...)
 *   alpha == JNI_TRUE:
 *     alpha_scan_valid == FALSE  -> never scanned: scan on demand
 *     alpha_scan_valid == TRUE:
 *       alpha_all_opaque == TRUE  -> every pixel alpha==0xFF (memcpy safe)
 *       alpha_all_opaque == FALSE -> at least one pixel alpha!=0xFF
 * Decoding (midp_image_create_from_data / _from_rgb) folds the scan into
 * its existing conversion loop, so immutable images are born with a valid
 * scan. Mutable images are lazily scanned and their scan is INVALIDATED by
 * every graphics op that can write alpha != 0xFF (fill_rect blend branch,
 * setPixel/drawLine/glyphs with gfx->alpha != 255, drawRGB(processAlpha),
 * M3G bindTarget render-to-image) via midp_image_invalidate_alpha_scan().
 */
typedef struct MidpImage {
    uint32_t* pixels;
    jint width;
    jint height;
    jboolean mutable;
    jboolean alpha;
    jboolean alpha_scan_valid;
    jboolean alpha_all_opaque;
} MidpImage;

/*
 * Font structure
 */
typedef struct {
    jint face;
    jint style;
    jint size;
    jint height;
    jint baseline;
    void* native_font;  /* Platform-specific font handle */
} MidpFont;

/*
 * Initialize MIDP2 native methods
 */
int midp_init(JVM* jvm);

/*
 * Graphics API (javax.microedition.lcdui.Graphics)
 */
void init_javax_microedition_lcdui_graphics(JVM* jvm);

/* Native graphics operations */
void midp_graphics_init(MidpGraphics* gfx, uint32_t* pixels, int width, int height);
void midp_graphics_set_clip(MidpGraphics* gfx, int x, int y, int width, int height);
void midp_graphics_clip_rect(MidpGraphics* gfx, int x, int y, int width, int height);
void midp_graphics_translate(MidpGraphics* gfx, int x, int y);
void midp_graphics_set_color(MidpGraphics* gfx, int rgb, int alpha);
void midp_graphics_draw_line(MidpGraphics* gfx, int x1, int y1, int x2, int y2);
/* v34.8: Graphics.fillTriangle (Bounce Tales) */
void midp_graphics_fill_triangle(MidpGraphics* gfx, int x1, int y1, int x2, int y2,
                                 int x3, int y3);
void midp_graphics_fill_rect(MidpGraphics* gfx, int x, int y, int w, int h);
void midp_graphics_draw_rect(MidpGraphics* gfx, int x, int y, int w, int h);
void midp_graphics_draw_arc(MidpGraphics* gfx, int x, int y, int w, int h, 
                            int start_angle, int arc_angle);
void midp_graphics_fill_arc(MidpGraphics* gfx, int x, int y, int w, int h,
                            int start_angle, int arc_angle);
void midp_graphics_draw_round_rect(MidpGraphics* gfx, int x, int y, int w, int h,
                                   int arc_w, int arc_h);
void midp_graphics_fill_round_rect(MidpGraphics* gfx, int x, int y, int w, int h,
                                   int arc_w, int arc_h);
void midp_graphics_draw_string(MidpGraphics* gfx, const char* str, 
                               int x, int y, int anchor);
void midp_graphics_draw_chars(MidpGraphics* gfx, const jchar* chars, int offset,
                              int length, int x, int y, int anchor);
void midp_graphics_draw_image(MidpGraphics* gfx, MidpImage* img,
                              int x, int y, int anchor);
void midp_graphics_draw_region(MidpGraphics* gfx, MidpImage* img,
                               int x_src, int y_src, int w, int h,
                               int transform, int x_dest, int y_dest, int anchor);
void midp_graphics_copy_area(MidpGraphics* gfx, int x_src, int y_src,
                             int w, int h, int x_dest, int y_dest, int anchor);
void midp_graphics_get_rgb(MidpGraphics* gfx, jint* rgb_data,
                           int offset, int scanlength, int x, int y, int w, int h);

/*
 * Image API (javax.microedition.lcdui.Image)
 */
void init_javax_microedition_lcdui_image(JVM* jvm);

MidpImage* midp_image_create(int width, int height, bool mutable);
MidpImage* midp_image_create_from_rgb(const jint* rgb, int width, int height, 
                                       bool process_alpha);
MidpImage* midp_image_create_from_data(const uint8_t* data, int offset, int length);
void midp_image_destroy(MidpImage* img);
/* v36.12 PEER-REGISTRY (see src/midp/graphics.c): every lcdui native peer
 * (MidpImage/MidpGraphics/MidpFont) is registered at creation; peers whose
 * Java owners died with the session heap are destroyed by the sweep in
 * midp_session_reset(). midp_graphics_free is the unregistering free for
 * Graphics peers; midp_graphics_register admits externally created ones
 * (GameCanvas context — NEVER stack/value graphics). */
void midp_graphics_free(MidpGraphics* gfx);
void midp_graphics_register(MidpGraphics* gfx);
void midp_peer_session_reset(void);
/* v34.51 PINK-FIX helpers (see the MidpImage comment above) */
bool midp_image_all_opaque(MidpImage* img);            /* exact, lazily scans */
void midp_image_invalidate_alpha_scan(MidpImage* img); /* call after ANY write
                                                            of a pixel with
                                                            alpha != 0xFF */
void midp_image_get_rgb(MidpImage* img, jint* rgb, int offset, int scanlength,
                        int x, int y, int width, int height);
MidpGraphics* midp_image_get_graphics(MidpImage* img);

/* Helper functions to get native objects from Java objects */
MidpImage* get_image_from_object(JavaObject* obj);
MidpGraphics* get_graphics_from_object(JavaObject* obj);

/*
 * Font API (javax.microedition.lcdui.Font)
 */
void init_javax_microedition_lcdui_font(JVM* jvm);

#ifdef J2ME_HEADLESS
/* v34.20: headless text capture hook (impl in graphics.c, called from
 * render.c drawString — the 2D rasterizer lives in the render module). */
void midp_headless_log_text(const char* text, int x, int y, uint32_t color);
#endif

MidpFont* midp_font_get_default(void);
MidpFont* midp_font_get(int face, int style, int size);
int midp_font_string_width(MidpFont* font, const char* str);
int midp_font_char_width(MidpFont* font, jchar ch);
int midp_font_height(MidpFont* font);
int midp_font_baseline_position(MidpFont* font);
/* v36.29 [FONT-2X]: per-font pixel scale of the shared 5x7 bitmap font.
 * SIZE_LARGE renders and measures 2x (see midp_font_string_width). */
int midp_font_pixel_scale(const MidpFont* font);

/* v36.29 [FONT-2X]: scaled canvas text. midp_graphics_draw_string renders
 * with the scale implied by the CURRENT font of the graphics (SIZE_LARGE
 * -> 2x); the _scaled/_2x variants FORCE the scale regardless of font.
 * Anchor semantics identical to drawString (BASELINE uses ascent*scale). */
void midp_graphics_draw_string(MidpGraphics* gfx, const char* str,
                               int x, int y, int anchor);
void midp_graphics_draw_string_scaled(MidpGraphics* gfx, const char* str,
                                      int x, int y, int anchor, int scale);
void midp_graphics_draw_string_2x(MidpGraphics* gfx, const char* str,
                                  int x, int y, int anchor);

/*
 * Display API (javax.microedition.lcdui.Display)
 */
void init_javax_microedition_lcdui_display(JVM* jvm);

void midp_display_get_dimensions(int* width, int* height);
void midp_set_screen_dimensions(int width, int height);  /* Set runtime screen dimensions */
bool midp_display_is_color(void);
int midp_display_num_colors(void);
int midp_display_num_alpha_levels(void);
void midp_display_set_current(void* displayable);
void* midp_display_get_current(void);
void midp_display_repaint(int x, int y, int width, int height);
void midp_display_call_serially(void (*callback)(void*), void* data);

/* Soft button handling for Canvas */
bool midp_has_commands(void);  /* Check if current displayable has commands */
bool midp_handle_soft_button(JVM* jvm, int button_index);
bool midp_handle_menu_navigation(int direction);
bool midp_is_command_menu_open(void);

/* Repaint processing - called from main loop */
int midp_process_repaints(JVM* jvm);
void midp_close_command_menu(void);
void midp_set_full_screen_mode(bool full_screen);

/* v34.61: settled-frame scanout buffer (see display.c). Snapshots are taken
 * at every point where a COMPLETE frame has just been produced (repaint pump
 * tails incl. soft buttons + M3G force-render, GameCanvas flushGraphics);
 * presenters copy the latest settled frame instead of reading the live
 * framebuffer. Presentation can no longer starve on an in-flight paint nor
 * show a torn/partial state. */
void midp_present_settled_snapshot(void);
uint32_t midp_present_stable_copy(uint32_t* dst, int dst_px);  /* 0 = nothing settled yet */
uint32_t midp_present_stable_seq(void);                        /* seq of latest settled frame */

/* v35.04: [NO-FRAME] watchdog context (racy diagnostics snapshot) */
void midp_noframe_state(int* out_keyq, int* out_paint_pending,
                        int* out_pump_active, char* cls_buf, int cls_cap);

/* v50: vsync pacing for Canvas.serviceRepaints() on game threads */
void midp_vsync_tick(void);
uint32_t midp_vsync_count(void);

/* Alert timeout handling */
bool midp_check_alert_timeout(JVM* jvm);

/* Check if current displayable is a Canvas subclass (for auto-redraw) */
bool midp_has_active_canvas(void);

/*
 * Game Canvas API (javax.microedition.lcdui.game.GameCanvas)
 */
void init_javax_microedition_lcdui_game_gamecanvas(JVM* jvm);

int midp_game_get_key_states(void);
MidpGraphics* midp_game_get_graphics(void);
void midp_game_flush_graphics(int x, int y, int width, int height);
void midp_game_flush_graphics_all(void);

/*
 * Sprite API (javax.microedition.lcdui.game.Sprite)
 */
void init_javax_microedition_lcdui_game_sprite(JVM* jvm);

/*
 * TiledLayer API (javax.microedition.lcdui.game.TiledLayer)
 */
void init_javax_microedition_lcdui_game_tiledlayer(JVM* jvm);

/*
 * LayerManager API (javax.microedition.lcdui.game.LayerManager)
 */
void init_javax_microedition_lcdui_game_layermanager(JVM* jvm);

/*
 * Record Management System (javax.microedition.rms)
 */
void init_javax_microedition_rms(JVM* jvm);

/* RMS persistent storage - set save directory and game name for disk I/O.
 * save_dir: path from RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY
 * game_name: base filename of the loaded JAR (without extension)
 * Called once from libretro core before running the MIDlet.
 */
void midp_rms_set_save_path(const char* save_dir, const char* game_name);

/* Save all open record stores to disk (call on game unload/deinit) */
void midp_rms_save_all(void);

/* v17: default RMS save directory for non-libretro builds (standalone SDL
 * app, headless runner). Uses $NOJME_RMS_DIR if set, else the per-user data
 * directory, with <game_name> as the per-midlet subdirectory. */
void midp_rms_default_save_path(const char* game_name);

int midp_rms_open_record_store(const char* name, bool create_if_necessary);
void midp_rms_close_record_store(int handle);
int midp_rms_add_record(int handle, const uint8_t* data, int offset, int length);
void midp_rms_set_record(int handle, int record_id, const uint8_t* data, int offset, int length);
uint8_t* midp_rms_get_record(int handle, int record_id, int* length);
void midp_rms_delete_record(int handle, int record_id);
int midp_rms_get_num_records(int handle);
int* midp_rms_get_record_ids(int handle, int* count);
void midp_rms_delete_record_store(const char* name);
char** midp_rms_list_record_stores(int* count);

/*
 * Media API (javax.microedition.media)
 */
void init_javax_microedition_media(JVM* jvm);

/*
 * Event handling
 */
void midp_event_queue(MidpEvent* event);
bool midp_event_poll(MidpEvent* event);
void midp_event_process(JVM* jvm, MidpEvent* event);

/* 
 * Key event handling - call keyPressed/keyReleased on current displayable 
 */
void midp_call_keyPressed(JVM* jvm, int keycode);
void midp_call_keyRepeated(JVM* jvm, int keycode);  /* v18 (audit M-3) */
void midp_call_keyReleased(JVM* jvm, int keycode);

/*
 * Pointer event handling 
 */
void midp_call_pointerPressed(JVM* jvm, int x, int y);
void midp_call_pointerReleased(JVM* jvm, int x, int y);
void midp_call_pointerDragged(JVM* jvm, int x, int y);

/*
 * Platform integration callbacks
 */
typedef struct {
    void (*repaint)(int x, int y, int w, int h);
    void (*vibrate)(int duration);
    void (*flash_backlight)(int duration);
    void (*platform_request)(const char* url);
} MidpPlatformCallbacks;

void midp_set_platform_callbacks(MidpPlatformCallbacks* callbacks);

/*
 * Form API (javax.microedition.lcdui.Form)
 */
void init_javax_microedition_lcdui_form(JVM* jvm);
void midp_render_form(JVM* jvm, JavaObject* form);
void midp_render_list(JVM* jvm, JavaObject* list, int focused_index);
void midp_render_alert(JVM* jvm, MidpGraphics* gfx, JavaObject* alert);
void midp_render_textbox(JVM* jvm, JavaObject* textbox, const char* input_text, int cursor_pos);

/* Form navigation and input handling */
bool midp_form_handle_key(JVM* jvm, int game_action);
void midp_set_current_displayable(JavaObject* displayable);
bool midp_is_vkb_active(void);
bool midp_vkb_process_key(JVM* jvm, int game_action);

/* v36.26 [TOUCH-UI]: touch for the MIDP high-level UI. midp_ui_touch_hit
 * consumes a TAP (press edge) when the standard Java UI owns it — soft
 * keys, command menu, List/Form/TextBox/Alert, virtual keyboard — and
 * returns 1 so the SDL frontend does NOT deliver pointer events to the
 * canvas for that press. midp_ui_touch_hold swallows the matching
 * drag/release of an owned touch sequence. The midp_*_touch helpers are
 * the per-screen implementations (form.c). */
int  midp_ui_touch_hit(JVM* jvm, int x, int y);
int  midp_ui_touch_hold(JVM* jvm, int x, int y);
bool midp_vkb_touch(JVM* jvm, int x, int y);
bool midp_list_touch(JVM* jvm, int x, int y);
bool midp_textbox_touch(JVM* jvm, int x, int y);
bool midp_form_touch(JVM* jvm, int x, int y);

/* v34.27: hierarchy-based kind of the current Displayable (Form/List/
 * TextBox/Alert/Canvas/Other). Replaces fragile strstr(class_name)
 * dispatch: a subclass of List named "...ActivateForm" must be a List. */
typedef enum {
    MIDP_UI_KIND_OTHER = 0,
    MIDP_UI_KIND_FORM,
    MIDP_UI_KIND_LIST,
    MIDP_UI_KIND_TEXTBOX,
    MIDP_UI_KIND_ALERT,
    MIDP_UI_KIND_CANVAS
} MidpDisplayableKind;
int midp_displayable_kind(JavaObject* displayable);

/*
 * Resource loading from JAR
 */
uint8_t* load_jar_resource(const char* filename, size_t* out_size);

/*
 * Helper functions for native peer management (used by DirectUtils and Image)
 */
void ensure_native_peer_field(JavaClass* clazz);
JavaObject* get_object_field_ref(JavaObject* obj, const char* field_name);
void set_object_field_ref(JavaObject* obj, const char* field_name, JavaObject* value);

/*
 * M3G (Mobile 3D Graphics) GC root support
 * The M3G registry stores loaded 3D objects for find() lookup.
 * These must be marked as GC roots during garbage collection.
 */
JavaObject** m3g_registry_get_objects(int* out_count);

/* v34.18: extra native M3G GC roots (pending-render queue nodes/transforms,
 * bound camera/target image, last World, active light objects). Returns an
 * array of *out_count slot ADDRESSES: each slot is a JavaObject* the GC must
 * mark; entries pointing at dead objects are cleared in place by the GC. */
JavaObject*** m3g_gc_extra_roots(int* out_count);

/* Force-render tracking for games that do M3G scene setup but never call bindTarget */
void m3g_reset_paint_tracking(void);
bool m3g_needs_force_render(void);
void m3g_force_render(JVM* jvm, MidpGraphics* screen_gfx);

#endif /* MIDP_H */
