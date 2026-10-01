/*
 * J2ME Emulator - Form and UI Components
 * MIDP2 high-level UI implementation
 */

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include "debug.h"
#include "debug_macros.h"
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>

#include "midp.h"
#include "jvm.h"
#include "native.h"
#include "heap.h"
#include "sdl_backend.h"
#include "threads.h"
#include "opcodes.h"  /* For T_BOOLEAN, DESC_OBJECT */

/* Forward declaration */
extern int execute_method(JVM* jvm, JavaThread* thread, JavaMethod* method, JavaValue* args, JavaValue* result);
extern MidpGraphics* get_graphics_from_object(JavaObject* obj);

/* Forward declaration: find_object_field_slot (defined below, used by render functions) */
static int find_object_field_slot(JavaObject* obj, const char* field_name);

/* Forward declaration for SELECT_COMMAND helper */
static void ensure_select_command(JVM* jvm);

/* Helper function: Get field offset by name, considering inheritance
 * Returns the absolute field index, or -1 if not found.
 * Fields are laid out: superclass fields first, then subclass fields.
 */
static int get_field_offset(JavaClass* clazz, const char* field_name) {
    if (!clazz || !field_name) return -1;
    
    /* Build class hierarchy from Object to actual class */
    JavaClass* hierarchy[64];
    int depth = 0;
    JavaClass* c = clazz;
    while (c && depth < 64) {
        hierarchy[depth++] = c;
        c = c->super_class;
    }
    
    /* Search for field from Object down to actual class */
    int slot = 0;
    for (int h = depth - 1; h >= 0; h--) {
        JavaClass* current = hierarchy[h];
        if (!current->fields) continue;
        
        for (int i = 0; i < current->fields_count; i++) {
            JavaField* field = &current->fields[i];
            
            /* Skip static fields */
            if (field->access_flags & ACC_STATIC) continue;
            
            if (field->name && strcmp(field->name, field_name) == 0) {
                return slot;
            }
            
            slot++;
            /* Long and double take 2 slots */
            if (field->descriptor && 
                (field->descriptor[0] == 'J' || field->descriptor[0] == 'D')) {
                slot++;
            }
        }
    }
    
    return -1;
}

/* Current form state */
static JavaObject* g_current_form = NULL;
static JavaObject* g_current_displayable = NULL;  /* Current List, Form, TextBox, etc. */
static int g_focused_item_index = 0;
static int g_list_selected_index = 0;  /* For List navigation */
static int g_cg_focused_element = 0;  /* Focused element index within current ChoiceGroup */
static int g_form_scroll = 0;         /* v34.65: scroll offset (px) for tall Forms */
/* v36.29 [FORM-SCROLL]: document-space bottom of the rendered content
 * (y after the item pass + g_form_scroll), recorded by midp_render_form.
 * The UP/DOWN scroll fallback in midp_form_handle_key clamps against it —
 * a Form with a long (non-focusable) StringItem must scroll even though
 * focus has nowhere to move. */
static int g_form_doc_h = 0;
static JavaObject* g_select_command = NULL;  /* List.SELECT_COMMAND singleton */
static JavaObject* g_item_state_listener = NULL;  /* ItemStateListener for current form */

/* v34.65 SELECTION COLOR: the old highlight was navy 0x000080 — the SAME
 * hue as Form/List/TextBox TITLE BARS — and the focused-borders were label
 * blue 0x0000FF, i.e. selection graphics blended into the chrome (user
 * report: "цвет выделения близок к цвету форм"). All selection marks now
 * use near-black 0x202020 + white text: maximum distance from white form
 * background, navy title bars and blue item labels on every screen. */
#define MIDP_SEL_BG 0x202020

/* v34.65 (defined before midp_render_form / midp_form_handle_key): */
static int cg_element_count(JavaObject* item);
static bool item_is_interactive(JavaObject* item);

/* v36.30 [ITEM-CMDS] forward decls (ItemExtra lives further down the file;
 * render_form / midp_form_handle_key / midp_form_touch all need these). */
static int form_item_cmd_effective(JavaObject* item, JavaObject** out, int max);
static const char* form_cmd_label_utf8(JVM* jvm, JavaObject* cmd);
static bool form_item_fire_command(JVM* jvm, JavaObject* item, JavaObject* cmd);
int midp_form_focused_item_commands(JavaObject** out_cmds, int max);
/* Item command MENU (3+ item commands — MIDP: they join the soft-key menu).
 * State + helpers live with ItemExtra; render_form draws the overlay. */
static JavaObject* g_item_menu_item = NULL;   /* item whose commands are shown */
static JavaObject* g_item_menu_cmds[8];       /* snapshot of effective commands */
static int g_item_menu_count = 0;
static int g_item_menu_sel = 0;
static bool form_item_menu_active_raw(void) { return g_item_menu_item != NULL && g_item_menu_count > 0; }
static void form_item_menu_close_raw(void) {
    g_item_menu_item = NULL; g_item_menu_count = 0; g_item_menu_sel = 0;
}

/* ============================================
 * v34.27: hierarchy-based UI dispatch.
 *
 * The old dispatch matched SUBSTRINGS of the CONCRETE class name:
 * strstr(name, "Form"), strstr(name, "List"), strstr(name, "TextBox").
 * That broke in two ways, both observed in the wild:
 *  1. glomoRegForms/ActivateForm EXTENDS List, but its name contains "Form"
 *     (even from the package segment) -> it took the Form branch, which
 *     looks for the nonexistent "items" field and every arrow key died
 *     silently. Any Form subclass whose own name lacks the substring
 *     ("MainMenu", "SettingsScreen") lost navigation the same way.
 *  2. Custom Item subclasses ("LoginField extends TextField") fell through
 *     get_item_type() and rendered as blank rows.
 * The correct dispatch walks the super_class chain and compares FULL
 * framework class names - exactly what display.c always did.
 * (Kind constants: MidpDisplayableKind in midp.h.) */

static bool midp_class_is(const JavaClass* clazz, const char* fqcn) {
    for (const JavaClass* c = clazz; c; c = c->super_class) {
        if (c->class_name && strcmp(c->class_name, fqcn) == 0) return true;
    }
    return false;
}

int midp_displayable_kind(JavaObject* obj) {
    if (!obj || !obj->header.clazz) return MIDP_UI_KIND_OTHER;
    JavaClass* c = obj->header.clazz;
    if (midp_class_is(c, "javax/microedition/lcdui/Form"))    return MIDP_UI_KIND_FORM;
    if (midp_class_is(c, "javax/microedition/lcdui/List"))    return MIDP_UI_KIND_LIST;
    if (midp_class_is(c, "javax/microedition/lcdui/TextBox")) return MIDP_UI_KIND_TEXTBOX;
    if (midp_class_is(c, "javax/microedition/lcdui/Alert"))   return MIDP_UI_KIND_ALERT;
    if (midp_class_is(c, "javax/microedition/lcdui/Canvas"))  return MIDP_UI_KIND_CANVAS;
    return MIDP_UI_KIND_OTHER;
}

/* v34.27: the repaint pump (midp_process_repaints) re-renders Lists with a
 * HARDCODED focused index 0 - every scheduled repaint visually reset the
 * selection to the top, so arrow navigation LOOKED dead on screen even
 * though the index had moved (the key handler's own render was overwritten
 * by the next repaint). The pump now asks for the live selection. */
int midp_list_get_selection(void) {
    return g_list_selected_index;
}


/* FIX (audit M-14, v18): itemStateChanged must be delivered from the event
 * path with COALESCING. A listener that writes back into the same Gauge/Field
 * triggered a synchronous recursive dispatch chain that blew the Java stack
 * (real phones never re-enter). One global guard drops nested notifications —
 * the final state is reported by the outermost in-flight notification. */
static int g_ism_dispatching = 0;
static void form_notify_item_state_changed(JVM* jvm, JavaObject* item);

/* Item types */
#define ITEM_STRINGITEM   1
#define ITEM_TEXTFIELD    2
#define ITEM_IMAGEITEM    3
#define ITEM_CHOICEGROUP  4
#define ITEM_GAUGE        5
#define ITEM_SPACER       6
#define ITEM_DATEFIELD    7

/* Choice constants */
#define CHOICE_EXCLUSIVE  1
#define CHOICE_MULTIPLE   2
#define CHOICE_IMPLICIT   3
#define CHOICE_POPUP      4

/* TextField constraints */
#define TEXTFIELD_ANY        0
#define TEXTFIELD_EMAILADDR  1
#define TEXTFIELD_NUMERIC    2
#define TEXTFIELD_PHONENUMBER 3
#define TEXTFIELD_URL        4
#define TEXTFIELD_DECIMAL    5
#define TEXTFIELD_PASSWORD   0x10000

/* ============================================
 * VIRTUAL KEYBOARD FOR TEXT INPUT
 * Simple grid-based character selection
 * ============================================ */

/* Virtual keyboard character set - English letters, numbers, symbols (reserved) */
__attribute__((unused))
static const char* VK_CHARACTERS = 
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"  /* 26 letters */
    "0123456789"                   /* 10 digits */
    " .,!?@_-:/"                   /* 10 symbols */
    "<DEL>"                        /* Delete - special */
    "<OK>";                        /* Confirm - special */

#define VK_CHARS_PER_ROW 10
#define VK_CHAR_COUNT    48  /* 26 + 10 + 10 + DEL + OK */

/* Virtual keyboard state */
static struct {
    bool active;              /* Is keyboard visible? */
    int selected_char;        /* Currently selected character index (0-47) */
    char text_buffer[256];    /* Current text being entered */
    int text_length;          /* Length of text */
    int cursor_pos;           /* Cursor position in text */
    int max_length;           /* Maximum allowed text length */
    int constraints;          /* TextField constraints */
    JavaObject* target_item;  /* TextField or TextBox being edited */
    JavaObject* displayable;  /* Parent form/screen */
} g_vkb;

/* Initialize virtual keyboard for a text field */
static void vkb_start(JavaObject* item, int max_size, int constraints) {
    g_vkb.active = true;
    g_vkb.selected_char = 0;
    g_vkb.text_length = 0;
    g_vkb.cursor_pos = 0;
    g_vkb.max_length = max_size > 255 ? 255 : max_size;
    g_vkb.constraints = constraints;
    g_vkb.target_item = item;
    g_vkb.text_buffer[0] = '\0';
    
    /* If item has existing text, copy it */
    if (item) {
        int text_slot = find_object_field_slot(item, "text");
        if (text_slot >= 0 && OBJECT_HAS_FIELDS(item, text_slot + 1)) {
            JavaString* text_str = (JavaString*)item->fields[text_slot].ref;
            if (text_str && text_str->utf8) {
                strncpy(g_vkb.text_buffer, text_str->utf8, 255);
                g_vkb.text_buffer[255] = '\0';
                g_vkb.text_length = strlen(g_vkb.text_buffer);
                g_vkb.cursor_pos = g_vkb.text_length;
            }
        }
    }
    
    FORM_DEBUG("[VKB] Started: max=%d, constraints=%d", max_size, constraints);
}

/* Close virtual keyboard and save text */
static void vkb_close(JVM* jvm, bool save) {
    if (save && g_vkb.target_item && jvm) {
        /* Create Java String from text buffer */
        JavaString* text_str = jvm_new_string(jvm, g_vkb.text_buffer);
        if (text_str) {
            int text_slot = find_object_field_slot(g_vkb.target_item, "text");
            if (text_slot >= 0 && OBJECT_HAS_FIELDS(g_vkb.target_item, text_slot + 1)) {
                g_vkb.target_item->fields[text_slot].ref = text_str;
                FORM_DEBUG("[VKB] Saved text: '%s'", g_vkb.text_buffer);
                
                /* Notify ItemStateListener about text change */
                if (g_item_state_listener && g_item_state_listener->header.clazz) {
                    JavaMethod* ism = jvm_resolve_method(jvm, g_item_state_listener->header.clazz,
                        "itemStateChanged", "(Ljavax/microedition/lcdui/Item;)V");
                    if (ism) {
                        JavaValue ism_args[2];
                        ism_args[0].ref = g_item_state_listener;
                        ism_args[1].ref = g_vkb.target_item;
                        JavaValue ism_result;
                        execute_method(jvm, jvm_current_thread(jvm), ism, ism_args, &ism_result);
                    }
                }
            }
        }
    }
    
    g_vkb.active = false;
    g_vkb.target_item = NULL;
    
    /* Repaint the form */
    if (g_current_displayable) {
        /* Trigger repaint */
        FORM_DEBUG("[VKB] Closed, should repaint");
    }
}

/* Handle key press in virtual keyboard mode */
static bool vkb_handle_key(JVM* jvm, int game_action) {
    if (!g_vkb.active) return false;
    
    switch (game_action) {
        case 1:  /* UP */
            g_vkb.selected_char -= VK_CHARS_PER_ROW;
            if (g_vkb.selected_char < 0) {
                g_vkb.selected_char += VK_CHAR_COUNT;
            }
            break;
            
        case 6:  /* DOWN */
            g_vkb.selected_char += VK_CHARS_PER_ROW;
            if (g_vkb.selected_char >= VK_CHAR_COUNT) {
                g_vkb.selected_char -= VK_CHAR_COUNT;
            }
            break;
            
        case 2:  /* LEFT */
            g_vkb.selected_char--;
            if (g_vkb.selected_char < 0) {
                g_vkb.selected_char = VK_CHAR_COUNT - 1;
            }
            break;
            
        case 5:  /* RIGHT */
            g_vkb.selected_char++;
            if (g_vkb.selected_char >= VK_CHAR_COUNT) {
                g_vkb.selected_char = 0;
            }
            break;
            
        case 8:  /* FIRE - select character */
            {
                int idx = g_vkb.selected_char;
                
                if (idx >= 46) {  /* <OK> - index 47 */
                    vkb_close(jvm, true);
                } else if (idx >= 45) {  /* <DEL> - index 46 */
                    if (g_vkb.text_length > 0 && g_vkb.cursor_pos > 0) {
                        /* Delete character before cursor */
                        memmove(&g_vkb.text_buffer[g_vkb.cursor_pos - 1],
                                &g_vkb.text_buffer[g_vkb.cursor_pos],
                                g_vkb.text_length - g_vkb.cursor_pos + 1);
                        g_vkb.text_length--;
                        g_vkb.cursor_pos--;
                    }
                } else if (idx < 26) {  /* Letter A-Z */
                    if (g_vkb.text_length < g_vkb.max_length) {
                        memmove(&g_vkb.text_buffer[g_vkb.cursor_pos + 1],
                                &g_vkb.text_buffer[g_vkb.cursor_pos],
                                g_vkb.text_length - g_vkb.cursor_pos + 1);
                        g_vkb.text_buffer[g_vkb.cursor_pos] = 'A' + idx;
                        g_vkb.text_length++;
                        g_vkb.cursor_pos++;
                    }
                } else if (idx < 36) {  /* Digit 0-9 */
                    if (g_vkb.text_length < g_vkb.max_length) {
                        memmove(&g_vkb.text_buffer[g_vkb.cursor_pos + 1],
                                &g_vkb.text_buffer[g_vkb.cursor_pos],
                                g_vkb.text_length - g_vkb.cursor_pos + 1);
                        g_vkb.text_buffer[g_vkb.cursor_pos] = '0' + (idx - 26);
                        g_vkb.text_length++;
                        g_vkb.cursor_pos++;
                    }
                } else {  /* Symbol (36-45) */
                    static const char symbols[] = " .,!?@_-:/";
                    int sym_idx = idx - 36;
                    if (sym_idx < 10 && g_vkb.text_length < g_vkb.max_length) {
                        memmove(&g_vkb.text_buffer[g_vkb.cursor_pos + 1],
                                &g_vkb.text_buffer[g_vkb.cursor_pos],
                                g_vkb.text_length - g_vkb.cursor_pos + 1);
                        g_vkb.text_buffer[g_vkb.cursor_pos] = symbols[sym_idx];
                        g_vkb.text_length++;
                        g_vkb.cursor_pos++;
                    }
                }
            }
            break;
            
        default:
            return false;
    }
    
    return true;  /* Key was handled */
}

/* Render virtual keyboard */
static void vkb_render(MidpGraphics* gfx) {
    if (!g_vkb.active || !gfx) return;
    
    int screen_w = gfx->width;
    int screen_h = gfx->height;
    
    /* Keyboard area - bottom half of screen */
    int kb_y = screen_h - 160;
    int kb_h = 150;
    
    /* Draw keyboard background */
    midp_graphics_set_color(gfx, 0xD0D0D0, 255);
    midp_graphics_fill_rect(gfx, 0, kb_y, screen_w, kb_h);
    
    /* Draw keyboard border */
    midp_graphics_set_color(gfx, 0x000000, 255);
    midp_graphics_draw_rect(gfx, 0, kb_y, screen_w, kb_h);
    
    /* Draw text field */
    int text_y = kb_y + 5;
    midp_graphics_set_color(gfx, 0xFFFFFF, 255);
    midp_graphics_fill_rect(gfx, 5, text_y, screen_w - 10, 20);
    midp_graphics_set_color(gfx, 0x000000, 255);
    midp_graphics_draw_rect(gfx, 5, text_y, screen_w - 10, 20);
    
    /* Draw current text */
    midp_graphics_set_color(gfx, 0x000000, 255);
    midp_graphics_draw_string(gfx, g_vkb.text_buffer, 8, text_y + 3, 0);
    
    /* Draw cursor */
    char temp[256];
    strncpy(temp, g_vkb.text_buffer, g_vkb.cursor_pos);
    temp[g_vkb.cursor_pos] = '\0';
    int cursor_x = 8 + strlen(temp) * 6;  /* Approximate char width */
    midp_graphics_draw_line(gfx, cursor_x, text_y + 2, cursor_x, text_y + 18);
    
    /* Draw character grid */
    int grid_y = text_y + 25;
    int cell_w = (screen_w - 10) / VK_CHARS_PER_ROW;
    int cell_h = 16;
    
    for (int i = 0; i < VK_CHAR_COUNT; i++) {
        int row = i / VK_CHARS_PER_ROW;
        int col = i % VK_CHARS_PER_ROW;
        int cell_x = 5 + col * cell_w;
        int cell_y = grid_y + row * cell_h;
        
        /* Draw selection highlight */
        if (i == g_vkb.selected_char) {
            midp_graphics_set_color(gfx, 0x000080, 255);
            midp_graphics_fill_rect(gfx, cell_x, cell_y, cell_w - 2, cell_h - 2);
            midp_graphics_set_color(gfx, 0xFFFFFF, 255);
        } else {
            midp_graphics_set_color(gfx, 0x000000, 255);
        }
        
        /* Draw character */
        char ch_str[8];
        if (i < 26) {
            ch_str[0] = 'A' + i;
            ch_str[1] = '\0';
        } else if (i < 36) {
            ch_str[0] = '0' + (i - 26);
            ch_str[1] = '\0';
        } else if (i < 46) {
            static const char symbols[] = " .,!?@_-:/";
            ch_str[0] = symbols[i - 36];
            ch_str[1] = '\0';
        } else if (i == 46) {
            strcpy(ch_str, "DEL");
        } else {
            strcpy(ch_str, "OK");
        }
        
        midp_graphics_draw_string(gfx, ch_str, cell_x + 2, cell_y + 1, 0);
    }
    
    /* Draw instructions */
    midp_graphics_set_color(gfx, 0x404040, 255);
    midp_graphics_draw_string(gfx, "Arrows:Move Fire:Select", 5, kb_y + kb_h - 15, 0);
}

/* Check if virtual keyboard is active */
bool midp_is_vkb_active(void) {
    return g_vkb.active;
}

/* Process key event for virtual keyboard */
bool midp_vkb_process_key(JVM* jvm, int game_action) {
    return vkb_handle_key(jvm, game_action);
}

/* ============================================ */

/* Get MidpGraphics for screen.
 * v34.65: exported (was static) — mobile3d.c's releaseTarget compares its
 * blit target against the SCREEN graphics to decide whether the 3D frame
 * just completed on-screen (settled-frame snapshot point). */
MidpGraphics* get_screen_graphics(void) {
    SdlContext* sdl_ctx = sdl_get_global_context();
    if (!sdl_ctx || !sdl_ctx->framebuffer) return NULL;
    
    static MidpGraphics gfx;
    midp_graphics_init(&gfx, sdl_ctx->framebuffer, sdl_ctx->width, sdl_ctx->height);
    return &gfx;
}

/* Get item type from object (v34.27: hierarchy walk - substring matching
 * missed custom subclasses like "LoginField extends TextField" and left
 * them rendering as blank, uninteractable rows). */
static int get_item_type(JavaObject* item) {
    if (!item || !item->header.clazz) return 0;

    JavaClass* c = item->header.clazz;
    if (midp_class_is(c, "javax/microedition/lcdui/TextField"))    return ITEM_TEXTFIELD;
    if (midp_class_is(c, "javax/microedition/lcdui/ChoiceGroup"))  return ITEM_CHOICEGROUP;
    if (midp_class_is(c, "javax/microedition/lcdui/StringItem"))   return ITEM_STRINGITEM;
    if (midp_class_is(c, "javax/microedition/lcdui/ImageItem"))    return ITEM_IMAGEITEM;
    if (midp_class_is(c, "javax/microedition/lcdui/Gauge"))        return ITEM_GAUGE;
    if (midp_class_is(c, "javax/microedition/lcdui/Spacer"))       return ITEM_SPACER;
    if (midp_class_is(c, "javax/microedition/lcdui/DateField"))    return ITEM_DATEFIELD;

    return 0;
}

/* Get string from Java String object - uses proper UTF-8 encoding */
static const char* get_string_from_object(JVM* jvm, JavaObject* str_obj) {
    if (!str_obj) return "";

    /* Validate this is actually a String object, not an array or other type */
    if (!is_heap_ptr_check(str_obj)) {
        WARN_LOG("[get_string_from_object] object %p is not in heap!", (void*)str_obj);
        return "";
    }
    
    /* Check GC header to verify it's a STRING or OBJECT type, not ARRAY */
    GCObjectHeader* gc_header = (GCObjectHeader*)str_obj - 1;
    if (gc_header->type == OBJ_TYPE_ARRAY) {
        WARN_LOG("[get_string_from_object] object %p is an ARRAY, not a String!", (void*)str_obj);
        return "";
    }

    JavaString* str = (JavaString*)str_obj;
    /* Use string_utf8() which has proper UTF-8 encoding and caching */
    const char* result = string_utf8(jvm, str);
    return result ? result : "";
}

/* Render StringItem */
/* v34.65 WORD-WRAP: draw text wrapped to max_width, honoring explicit
 * '\n' breaks, word boundaries, and hard-splitting words longer than the
 * line. Returns the y BELOW the last drawn line. Long Form strings used
 * to run off the right edge of the screen (user report: FPC long strings
 * "не переносятся").
 * v36.26 [TOUCH-UI]: gfx == NULL → MEASURE-ONLY (no drawing, same y
 * walk) — the Form touch hit-test mirrors the render layout through it. */
static int draw_wrapped_text(MidpGraphics* gfx, MidpFont* font, const char* text,
                             int x, int y, int max_width, uint32_t color) {
    if (!text || !text[0]) return y;
    int font_height = font ? midp_font_height(font) : 12;
    int line_h = font_height + 2;
    const char* p = text;

    if (gfx) midp_graphics_set_color(gfx, color, 255);

    while (*p) {
        /* end of the physical line (explicit \n or end of string) */
        const char* eol = p;
        while (*eol && *eol != '\n') eol++;

        /* wrap the [p, eol) paragraph */
        const char* line_start = p;
        while (line_start < eol) {
            /* skip leading spaces */
            while (line_start < eol && *line_start == ' ') line_start++;
            if (line_start >= eol) break;

            /* find the split point: furthest word boundary that fits */
            const char* fit_end = line_start;
            const char* probe = line_start;
            while (probe <= eol) {
                if (probe == eol || *probe == ' ' || *probe == '\n') {
                    /* measure [line_start, probe) via a bounded copy */
                    char buf[256];
                    size_t seg_len = (size_t)(probe - line_start);
                    if (seg_len >= sizeof(buf)) seg_len = sizeof(buf) - 1;
                    memcpy(buf, line_start, seg_len);
                    buf[seg_len] = '\0';
                    if (midp_font_string_width(font, buf) <= max_width) {
                        fit_end = probe;
                        probe++;
                        continue;
                    }
                    /* too wide: stop */
                    break;
                }
                probe++;
            }

            const char* line_end;
            if (fit_end > line_start) {
                line_end = fit_end;
            } else {
                /* first word already overflows: hard-split by characters */
                char buf[64];
                size_t keep = 0;
                while (line_start + keep < eol && keep + 1 < sizeof(buf)) {
                    buf[keep] = line_start[keep];
                    buf[keep + 1] = '\0';
                    if (midp_font_string_width(font, buf) > max_width && keep > 0) {
                        break;
                    }
                    keep++;
                }
                line_end = line_start + (keep > 0 ? keep : 1);
            }

            /* draw [line_start, line_end) */
            char buf[256];
            size_t draw_len = (size_t)(line_end - line_start);
            if (draw_len >= sizeof(buf)) draw_len = sizeof(buf) - 1;
            memcpy(buf, line_start, draw_len);
            buf[draw_len] = '\0';
            if (gfx) midp_graphics_draw_string(gfx, buf, x, y, 0);
            y += line_h;

            line_start = (line_end < eol && *line_end == ' ') ? line_end + 1 : line_end;
        }
        p = (*eol == '\n') ? eol + 1 : eol;
        if (!*p) break;
    }
    return y;
}

static int render_stringitem(JVM* jvm, MidpGraphics* gfx, JavaObject* item, int y) {
    /* StringItem has: label (String), text (String) — use dynamic slot lookup */
    const char* label = "";
    const char* text = "";
    
    int label_slot = find_object_field_slot(item, "label");
    int text_slot = find_object_field_slot(item, "text");
    int max_slot = (label_slot > text_slot) ? label_slot : text_slot;
    
    if (label_slot >= 0 && text_slot >= 0 && OBJECT_HAS_FIELDS(item, max_slot + 1)) {
        JavaString* label_str = (JavaString*)item->fields[label_slot].ref;
        JavaString* text_str = (JavaString*)item->fields[text_slot].ref;
        
        if (label_str) label = get_string_from_object(jvm, (JavaObject*)label_str);
        if (text_str) text = get_string_from_object(jvm, (JavaObject*)text_str);
    }
    
    MidpFont* font = midp_font_get_default();
    int wrap_width = gfx->width - 10;   /* 5px side margins */
    
    /* Draw label (v34.65: word-wrapped) */
    if (label && *label) {
        int y_after = draw_wrapped_text(gfx, font, label, 5, y, wrap_width, 0x0000FF);
        y = y_after + 2;
    }
    
    /* Draw text (v34.65: word-wrapped — long strings used to run off-screen) */
    if (text && *text) {
        y = draw_wrapped_text(gfx, font, text, 5, y, wrap_width, 0x000000) + 4;
    }
    
    return y;
}

/* Render TextField */
static int render_textfield(JVM* jvm, MidpGraphics* gfx, JavaObject* item, int y, bool focused) {
    const char* label = "";
    const char* text = "";
    int max_size = 32;
    (void)focused;
    int constraints = 0;
    
    /* TextField fields: label, text, maxSize, constraints — use dynamic slot lookup */
    int label_slot = find_object_field_slot(item, "label");
    int text_slot = find_object_field_slot(item, "text");
    int maxsize_slot = find_object_field_slot(item, "maxSize");
    int constraints_slot = find_object_field_slot(item, "constraints");
    
    if (label_slot >= 0 && text_slot >= 0 && maxsize_slot >= 0 && constraints_slot >= 0 &&
        OBJECT_HAS_FIELDS(item, constraints_slot + 1)) {
        JavaString* label_str = (JavaString*)item->fields[label_slot].ref;
        JavaString* text_str = (JavaString*)item->fields[text_slot].ref;
        
        if (label_str) label = get_string_from_object(jvm, (JavaObject*)label_str);
        if (text_str) text = get_string_from_object(jvm, (JavaObject*)text_str);
        
        max_size = item->fields[maxsize_slot].i;
        (void)max_size;  /* bitmap renderer has no size limit enforcement */
        constraints = item->fields[constraints_slot].i;
    }
    
    int font_height = 12;
    MidpFont* font = midp_font_get_default();
    if (font) font_height = midp_font_height(font);
    
    int box_height = font_height + 8;
    
    /* Draw label */
    if (label && *label) {
        midp_graphics_set_color(gfx, 0x0000FF, 255);
        midp_graphics_draw_string(gfx, label, 5, y, 0);
        y += font_height + 2;
    }
    
    /* Draw text box */
    int box_y = y;
    /* ИСПРАВЛЕНО: Вычисляем ширину относительно экрана */
    int box_width = gfx->width - 10;  /* 5 пикселей отступ с каждой стороны */
    midp_graphics_set_color(gfx, 0xFFFFFF, 255); /* White background */
    midp_graphics_fill_rect(gfx, 5, box_y, box_width, box_height);
    midp_graphics_set_color(gfx, focused ? MIDP_SEL_BG : 0x808080, 255); /* Border: v34.65 was 0x0000FF — identical to the label blue above it */
    midp_graphics_draw_rect(gfx, 5, box_y, box_width, box_height);
    
    /* Draw text content */
    midp_graphics_set_color(gfx, 0x000000, 255);
    if (constraints & TEXTFIELD_PASSWORD) {
        /* Draw asterisks for password */
        int len = text ? strlen(text) : 0;
        char* stars = (char*)malloc(len + 1);
        if (stars) {
            memset(stars, '*', len);
            stars[len] = '\0';
            midp_graphics_draw_string(gfx, stars, 8, box_y + 3, 0);
            free(stars);
        }
    } else {
        if (text && *text) {
            midp_graphics_draw_string(gfx, text, 8, box_y + 3, 0);
        }
    }
    
    /* Draw cursor if focused */
    if (focused) {
        int cursor_x = 8;
        if (text) {
            cursor_x += midp_font_string_width(font, text);
        }
        midp_graphics_set_color(gfx, 0x000000, 255);
        midp_graphics_draw_line(gfx, cursor_x, box_y + 2, cursor_x, box_y + box_height - 2);
    }
    
    return y + box_height + 6;
}

/* Render ChoiceGroup */
static int render_choicegroup(JVM* jvm, MidpGraphics* gfx, JavaObject* item, int y, bool focused) {
    const char* label = "";
    int choice_type = CHOICE_EXCLUSIVE;
    int num_choices = 0;
    JavaArray* strings = NULL;
    JavaArray* selected = NULL;
    
    /* ChoiceGroup fields: label, choiceType, strings[], selected[] — use dynamic slot lookup */
    int label_slot = find_object_field_slot(item, "label");
    int choicetype_slot = find_object_field_slot(item, "choiceType");
    int strings_slot = find_object_field_slot(item, "strings");
    int selected_slot = find_object_field_slot(item, "selected");
    
    if (label_slot >= 0 && choicetype_slot >= 0 && strings_slot >= 0 && selected_slot >= 0 &&
        OBJECT_HAS_FIELDS(item, selected_slot + 1)) {
        JavaString* label_str = (JavaString*)item->fields[label_slot].ref;
        if (label_str) label = get_string_from_object(jvm, (JavaObject*)label_str);
        
        choice_type = item->fields[choicetype_slot].i;
        strings = (JavaArray*)item->fields[strings_slot].ref;
        selected = (JavaArray*)item->fields[selected_slot].ref;
    }
    
    int font_height = 12;
    MidpFont* font = midp_font_get_default();
    if (font) font_height = midp_font_height(font);
    
    /* Draw label */
    if (label && *label) {
        midp_graphics_set_color(gfx, 0x0000FF, 255);
        midp_graphics_draw_string(gfx, label, 5, y, 0);
        y += font_height + 4;
    }
    
    /* Draw choices */
    if (strings && strings->element_type == DESC_OBJECT) {
        num_choices = strings->length;
        void** strings_data = (void**)array_data(strings);
        jboolean* selected_data = selected ? (jboolean*)array_data(selected) : NULL;
        
        for (int i = 0; i < num_choices; i++) {
            JavaString* choice_str = (JavaString*)strings_data[i];
            const char* choice_text = choice_str ? get_string_from_object(jvm, (JavaObject*)choice_str) : "";
            bool is_selected = false;
            
            if (selected && selected->element_type == T_BOOLEAN) {
                is_selected = selected_data[i] != 0;
            }
            
            /* v34.65: VISIBLE element cursor. v34.27 implemented the FIRE
             * toggle + LEFT/RIGHT element focus but never RENDERED the
             * element focus, and UP/DOWN skipped whole groups — so the
             * user saw checkboxes that "only render, can't be selected"
             * (Nescube settings report). The focused element now carries
             * a List-style solid cursor bar (MIDP_SEL_BG + white). */
            bool elem_focused = focused && (i == g_cg_focused_element);
            if (elem_focused) {
                midp_graphics_set_color(gfx, MIDP_SEL_BG, 255);
                midp_graphics_fill_rect(gfx, 0, y - 2, gfx->width, font_height + 4);
            }
            
            int check_x = 10;
            int check_y = y + font_height / 2;
            
            /* Draw checkbox/radio */
            midp_graphics_set_color(gfx, elem_focused ? 0xFFFFFF : 0x000000, 255);
            
            if (choice_type == CHOICE_EXCLUSIVE || choice_type == CHOICE_POPUP) {
                /* Radio button - circle */
                midp_graphics_draw_arc(gfx, check_x - 5, check_y - 5, 10, 10, 0, 360);
                if (is_selected) {
                    midp_graphics_fill_arc(gfx, check_x - 3, check_y - 3, 6, 6, 0, 360);
                }
            } else {
                /* Checkbox - square */
                midp_graphics_draw_rect(gfx, check_x - 5, check_y - 5, 10, 10);
                if (is_selected) {
                    /* Draw X */
                    midp_graphics_draw_line(gfx, check_x - 3, check_y - 3, check_x + 3, check_y + 3);
                    midp_graphics_draw_line(gfx, check_x - 3, check_y + 3, check_x + 3, check_y - 3);
                }
            }
            
            /* Draw choice text */
            midp_graphics_set_color(gfx, elem_focused ? 0xFFFFFF : 0x000000, 255);
            midp_graphics_draw_string(gfx, choice_text, check_x + 10, y, 0);
            
            y += font_height + 4;
        }
    }
    
    return y + 4;
}

/* Render Gauge */
static int render_gauge(JVM* jvm, MidpGraphics* gfx, JavaObject* item, int y, bool focused) {
    const char* label = "";
    int max_value = 100;
    int current_value = 0;
    bool interactive = false;
    
    /* Gauge fields: label, interactive, maxValue, value — use dynamic slot lookup */
    int label_slot = find_object_field_slot(item, "label");
    int interactive_slot = find_object_field_slot(item, "interactive");
    int maxvalue_slot = find_object_field_slot(item, "maxValue");
    int value_slot = find_object_field_slot(item, "value");
    
    if (label_slot >= 0 && interactive_slot >= 0 && maxvalue_slot >= 0 && value_slot >= 0 &&
        OBJECT_HAS_FIELDS(item, value_slot + 1)) {
        JavaString* label_str = (JavaString*)item->fields[label_slot].ref;
        if (label_str) label = get_string_from_object(jvm, (JavaObject*)label_str);
        
        interactive = item->fields[interactive_slot].i != 0;
        max_value = item->fields[maxvalue_slot].i;
        current_value = item->fields[value_slot].i;
    }
    
    int font_height = 12;
    MidpFont* font = midp_font_get_default();
    if (font) font_height = midp_font_height(font);
    
    /* Draw label */
    if (label && *label) {
        /* v34.67 FIX (invisible Gauge focus): the gauge bar ALWAYS carries a
         * black border, so the v34.65 focus cue (border 0x202020 vs 0x000000)
         * was indistinguishable — "the changeable bars already have a black
         * outline, the selection on them is not visible" (Nescube settings:
         * Base scale / Camera reaction / Saturation / Brightness). The label
         * row now carries the SAME focus plaque as ChoiceGroup elements
         * (MIDP_SEL_BG band + white text), and the bar itself gets the
         * double-frame treatment below — the focus is visible even when the
         * label is empty. */
        if (focused) {
            midp_graphics_set_color(gfx, MIDP_SEL_BG, 255);
            midp_graphics_fill_rect(gfx, 0, y - 2, gfx->width, font_height + 4);
            midp_graphics_set_color(gfx, 0xFFFFFF, 255);
        } else {
            midp_graphics_set_color(gfx, 0x0000FF, 255);
        }
        midp_graphics_draw_string(gfx, label, 5, y, 0);
        y += font_height + 4;
    }
    
    int bar_y = y;
    int bar_height = 20;
    /* ИСПРАВЛЕНО: Вычисляем ширину относительно экрана */
    int bar_width = gfx->width - 20;  /* 10 пикселей отступ с каждой стороны */
    
    /* v34.67: focused gauge gets an OUTER 2px MIDP_SEL_BG frame with a 1px
     * gap — an unmistakable double-frame on the white form, independent of
     * the (always-black) bar border. Drawn FIRST so the bar and its border
     * overpaint the inner edge cleanly. */
    if (focused) {
        midp_graphics_set_color(gfx, MIDP_SEL_BG, 255);
        midp_graphics_draw_rect(gfx, 8, bar_y - 3, bar_width + 5, bar_height + 6);
        midp_graphics_draw_rect(gfx, 7, bar_y - 4, bar_width + 7, bar_height + 8);
    }
    
    /* Draw bar background */
    midp_graphics_set_color(gfx, 0xC0C0C0, 255);
    midp_graphics_fill_rect(gfx, 10, bar_y, bar_width, bar_height);
    
    /* Draw filled portion */
    if (max_value > 0) {
        int fill_width = (current_value * bar_width) / max_value;
        midp_graphics_set_color(gfx, interactive ? 0x0000FF : 0x008000, 255);
        midp_graphics_fill_rect(gfx, 10, bar_y, fill_width, bar_height);
    }
    
    /* Draw border (always black — the focus cue is the outer frame + label
     * plaque above, not a border color change) */
    midp_graphics_set_color(gfx, 0x000000, 255);
    midp_graphics_draw_rect(gfx, 10, bar_y, bar_width, bar_height);
    
    /* Draw value text */
    char val_text[32];
    snprintf(val_text, sizeof(val_text), "%d/%d", current_value, max_value);
    midp_graphics_set_color(gfx, 0x000000, 255);
    midp_graphics_draw_string(gfx, val_text, 100, bar_y + 4, 0x10); /* HCENTER */
    
    return y + bar_height + 8;
}

/* v34.29 FIX (JBenchmark 3D): ImageItems were silently skipped by the Form
 * renderer (fell into the `default: y += 20` branch) — the midlet's logo
 * never appeared and the form looked broken. Render the image (MIDP
 * LAYOUT_LEFT default; LAYOUT_CENTER=8/HCENTER honours the width), the
 * label above it, and the altText when the image is missing/failed. */
static int render_imageitem(JVM* jvm, MidpGraphics* gfx, JavaObject* item, int y) {
    extern MidpImage* get_image_from_object(JavaObject* obj);

    const char* label = "";
    const char* alt = "";
    int layout = 0;

    int label_slot = find_object_field_slot(item, "label");
    int alt_slot = find_object_field_slot(item, "altText");
    int layout_slot = find_object_field_slot(item, "layout");
    int image_slot = find_object_field_slot(item, "image");

    if (label_slot >= 0 && OBJECT_HAS_FIELDS(item, label_slot + 1)) {
        JavaString* label_str = (JavaString*)item->fields[label_slot].ref;
        if (label_str) label = get_string_from_object(jvm, (JavaObject*)label_str);
    }
    if (alt_slot >= 0 && OBJECT_HAS_FIELDS(item, alt_slot + 1)) {
        JavaString* alt_str = (JavaString*)item->fields[alt_slot].ref;
        if (alt_str) alt = get_string_from_object(jvm, (JavaObject*)alt_str);
    }
    if (layout_slot >= 0 && OBJECT_HAS_FIELDS(item, layout_slot + 1)) {
        layout = item->fields[layout_slot].i;
    }

    int font_height = 12;
    MidpFont* font = midp_font_get_default();
    if (font) font_height = midp_font_height(font);

    /* Label above the image (MIDP: label is rendered on its own line) */
    if (label && *label) {
        midp_graphics_set_color(gfx, 0x0000FF, 255);
        midp_graphics_draw_string(gfx, label, 5, y, 0);
        y += font_height + 2;
    }

    JavaObject* image_obj = (image_slot >= 0 &&
                             OBJECT_HAS_FIELDS(item, image_slot + 1))
        ? (JavaObject*)item->fields[image_slot].ref : NULL;
    MidpImage* img = image_obj ? get_image_from_object(image_obj) : NULL;

    if (img && img->width > 0 && img->height > 0) {
        int x = 5;   /* LAYOUT_LEFT (2) is the default */
        if (layout & 8) {   /* LAYOUT_CENTER or HCENTER */
            x = (gfx->width - img->width) / 2;
            if (x < 0) x = 0;
        } else if (layout & 16) {  /* LAYOUT_RIGHT */
            x = gfx->width - img->width - 5;
            if (x < 0) x = 0;
        }
        midp_graphics_draw_image(gfx, img, x, y, 20 /* TOP|LEFT */);
        y += img->height + 4;
    } else {
        /* Image missing: show the alt text (MIDP spec behavior) */
        const char* fallback = (alt && *alt) ? alt : "(image)";
        midp_graphics_set_color(gfx, 0x000000, 255);
        midp_graphics_draw_string(gfx, fallback, 5, y, 0);
        y += font_height + 4;
    }

    return y;
}

/* Render Spacer */
static int render_spacer(JVM* jvm, MidpGraphics* gfx, JavaObject* item, int y) {
    (void)jvm; (void)gfx;   /* Spacer renders nothing; only geometry matters */
    int width = 0, height = 0;
    (void)width;
    
    /* Spacer fields: width, height — use dynamic slot lookup */
    int width_slot = find_object_field_slot(item, "width");
    int height_slot = find_object_field_slot(item, "height");
    if (width_slot >= 0 && height_slot >= 0 &&
        OBJECT_HAS_FIELDS(item, height_slot + 1)) {
        width = item->fields[width_slot].i;
        height = item->fields[height_slot].i;
    }
    
    if (height <= 0) height = 10;
    
    return y + height;
}

/* Render Alert */
static void render_alert(JVM* jvm, MidpGraphics* gfx, JavaObject* alert) {
    const char* title = "Alert";
    const char* text = "";
    int type = 0;
    (void)type;  /* alertType used only for icon selection, not drawn in bitmap mode */
    
    /* Alert fields: title, text, type — use dynamic slot lookup */
    int title_slot = find_object_field_slot(alert, "title");
    int text_slot = find_object_field_slot(alert, "text");
    int type_slot = find_object_field_slot(alert, "alertType");
    int max_slot = title_slot;
    if (text_slot > max_slot) max_slot = text_slot;
    if (type_slot > max_slot) max_slot = type_slot;
    
    if (title_slot >= 0 && text_slot >= 0 && type_slot >= 0 &&
        OBJECT_HAS_FIELDS(alert, max_slot + 1)) {
        JavaString* title_str = (JavaString*)alert->fields[title_slot].ref;
        JavaString* text_str = (JavaString*)alert->fields[text_slot].ref;
        
        if (title_str) title = get_string_from_object(jvm, (JavaObject*)title_str);
        if (text_str) text = get_string_from_object(jvm, (JavaObject*)text_str);
        
        type = alert->fields[type_slot].i;
    }
    
    int screen_width = gfx->width;
    int screen_height = gfx->height;
    
    int box_width = screen_width - 20;
    int box_height = 100;
    int box_x = 10;
    int box_y = (screen_height - box_height) / 2;
    
    /* Draw background */
    midp_graphics_set_color(gfx, 0xFFFFCC, 255); /* Light yellow */
    midp_graphics_fill_rect(gfx, box_x, box_y, box_width, box_height);
    
    /* Draw border */
    midp_graphics_set_color(gfx, 0x000000, 255);
    midp_graphics_draw_rect(gfx, box_x, box_y, box_width, box_height);
    
    /* Draw title */
    midp_graphics_set_color(gfx, 0x0000FF, 255);
    midp_graphics_draw_string(gfx, title, screen_width / 2, box_y + 5, 0x10); /* HCENTER | TOP */
    
    /* Draw text */
    midp_graphics_set_color(gfx, 0x000000, 255);
    midp_graphics_draw_string(gfx, text, screen_width / 2, box_y + 30, 0x10);
    
    /* Draw OK button */
    int btn_width = 60;
    int btn_height = 25;
    int btn_x = (screen_width - btn_width) / 2;
    int btn_y = box_y + box_height - 35;
    
    midp_graphics_set_color(gfx, 0xC0C0C0, 255);
    midp_graphics_fill_rect(gfx, btn_x, btn_y, btn_width, btn_height);
    midp_graphics_set_color(gfx, 0x000000, 255);
    midp_graphics_draw_rect(gfx, btn_x, btn_y, btn_width, btn_height);
    midp_graphics_draw_string(gfx, "OK", screen_width / 2, btn_y + 5, 0x10);
}

/* Render List */
static void render_list(JVM* jvm, MidpGraphics* gfx, JavaObject* list, int focused_index) {
    const char* title = "";
    int list_type = CHOICE_IMPLICIT;
    JavaArray* strings = NULL;
    JavaArray* selected = NULL;
    
    FORM_DEBUG("[render_list] list=%p, clazz=%s, instance_size=%zu",
            (void*)list, list && list->header.clazz ? list->header.clazz->class_name : "NULL",
            list && list->header.clazz ? list->header.clazz->instance_size : 0);
    
    /* Get field offsets considering inheritance (List extends Screen extends Displayable) */
    JavaClass* list_class = list->header.clazz;
    int title_idx = get_field_offset(list_class, "title");
    int listtype_idx = get_field_offset(list_class, "listType");
    int strings_idx = get_field_offset(list_class, "strings");
    int selected_idx = get_field_offset(list_class, "selected");
    
    FORM_DEBUG("[render_list] Field offsets: title=%d, listType=%d, strings=%d, selected=%d",
            title_idx, listtype_idx, strings_idx, selected_idx);
    
    /* List fields: title, listType, strings[], selected[] */
    if (title_idx >= 0 && listtype_idx >= 0 && strings_idx >= 0 && selected_idx >= 0) {
        JavaString* title_str = (JavaString*)list->fields[title_idx].ref;
        if (title_str) title = get_string_from_object(jvm, (JavaObject*)title_str);
        
        list_type = list->fields[listtype_idx].i;
        strings = (JavaArray*)list->fields[strings_idx].ref;
        selected = (JavaArray*)list->fields[selected_idx].ref;
        
        FORM_DEBUG("[render_list] title=%s, type=%d, strings=%p (len=%d), selected=%p",
                title, list_type, (void*)strings, strings ? strings->length : -1, (void*)selected);
    } else {
        FORM_DEBUG("[render_list] Failed to get field offsets! Using defaults.");
    }
    
    int font_height = 12;
    MidpFont* font = midp_font_get_default();
    if (font) font_height = midp_font_height(font);
    
    int y = 30;
    
    /* Draw title */
    midp_graphics_set_color(gfx, 0x0000FF, 255);
    midp_graphics_draw_string(gfx, title, gfx->width / 2, 5, 0x10);
    
    /* Draw separator */
    midp_graphics_set_color(gfx, 0x808080, 255);
    midp_graphics_draw_line(gfx, 0, 25, gfx->width, 25);
    
    /* Draw items */
    if (strings && strings->element_type == DESC_OBJECT) {
        void** strings_data = (void**)array_data(strings);
        jboolean* selected_data = selected ? (jboolean*)array_data(selected) : NULL;
        
        for (int i = 0; i < strings->length; i++) {
            JavaString* item_str = (JavaString*)strings_data[i];
            const char* item_text = item_str ? get_string_from_object(jvm, (JavaObject*)item_str) : "";
            
            /* Highlight focused item */
            if (i == focused_index) {
                /* v34.65: MIDP_SEL_BG (was 0x000080 — same hue as the title
                 * bar; user: "selection color is close to the form color") */
                midp_graphics_set_color(gfx, MIDP_SEL_BG, 255);
                midp_graphics_fill_rect(gfx, 0, y - 2, gfx->width, font_height + 4);
                midp_graphics_set_color(gfx, 0xFFFFFF, 255);
            } else {
                midp_graphics_set_color(gfx, 0x000000, 255);
            }
            
            /* Draw selection indicator for EXCLUSIVE/MULTIPLE */
            int text_x = 5;
            if (list_type == CHOICE_EXCLUSIVE || list_type == CHOICE_MULTIPLE) {
                bool is_selected = false;
                if (selected && selected->element_type == T_BOOLEAN) {
                    is_selected = selected_data[i] != 0;
                }
                
                int check_x = 15;
                int check_y = y + font_height / 2;
                
                if (list_type == CHOICE_EXCLUSIVE) {
                    midp_graphics_draw_arc(gfx, check_x - 5, check_y - 5, 10, 10, 0, 360);
                    if (is_selected) {
                        midp_graphics_fill_arc(gfx, check_x - 3, check_y - 3, 6, 6, 0, 360);
                    }
                } else {
                    midp_graphics_draw_rect(gfx, check_x - 5, check_y - 5, 10, 10);
                    if (is_selected) {
                        midp_graphics_draw_line(gfx, check_x - 3, check_y - 3, check_x + 3, check_y + 3);
                        midp_graphics_draw_line(gfx, check_x - 3, check_y + 3, check_x + 3, check_y - 3);
                    }
                }
                text_x = 30;
            }
            
            midp_graphics_draw_string(gfx, item_text, text_x, y, 0);
            y += font_height + 6;
        }
    }
}

/* Render TextBox */
static void render_textbox(JVM* jvm, MidpGraphics* gfx, JavaObject* textbox, const char* input_text, int cursor_pos) {
    const char* title = "";
    int max_size = 32;
    (void)max_size;  /* bitmap renderer has no size limit enforcement */
    int constraints = 0;
    
    /* TextBox fields: title, text, maxSize, constraints — use dynamic slot lookup */
    int title_slot = find_object_field_slot(textbox, "title");
    int maxsize_slot = find_object_field_slot(textbox, "maxSize");
    int constraints_slot = find_object_field_slot(textbox, "constraints");
    
    if (title_slot >= 0 && maxsize_slot >= 0 && constraints_slot >= 0 &&
        OBJECT_HAS_FIELDS(textbox, constraints_slot + 1)) {
        JavaString* title_str = (JavaString*)textbox->fields[title_slot].ref;
        if (title_str) title = get_string_from_object(jvm, (JavaObject*)title_str);
        
        max_size = textbox->fields[maxsize_slot].i;
        constraints = textbox->fields[constraints_slot].i;
    }
    
    int font_height = 12;
    MidpFont* font = midp_font_get_default();
    if (font) font_height = midp_font_height(font);
    
    /* Draw title bar */
    midp_graphics_set_color(gfx, 0x000080, 255);
    midp_graphics_fill_rect(gfx, 0, 0, gfx->width, 25);
    midp_graphics_set_color(gfx, 0xFFFFFF, 255);
    midp_graphics_draw_string(gfx, title, gfx->width / 2, 5, 0x10);
    
    /* Draw text area */
    int text_y = 30;
    int text_height = gfx->height - 80;
    
    midp_graphics_set_color(gfx, 0xFFFFFF, 255);
    midp_graphics_fill_rect(gfx, 5, text_y, gfx->width - 10, text_height);
    midp_graphics_set_color(gfx, 0x000000, 255);
    midp_graphics_draw_rect(gfx, 5, text_y, gfx->width - 10, text_height);
    
    /* Draw text content */
    midp_graphics_set_color(gfx, 0x000000, 255);
    if (input_text && *input_text) {
        if (constraints & TEXTFIELD_PASSWORD) {
            int len = strlen(input_text);
            char* stars = (char*)malloc(len + 1);
            if (stars) {
                memset(stars, '*', len);
                stars[len] = '\0';
                midp_graphics_draw_string(gfx, stars, 10, text_y + 5, 0);
                free(stars);
            }
        } else {
            midp_graphics_draw_string(gfx, input_text, 10, text_y + 5, 0);
        }
    }
    
    /* Draw cursor */
    if (cursor_pos >= 0) {
        int cursor_x = 10;
        if (input_text && cursor_pos > 0) {
            char* temp = (char*)malloc(cursor_pos + 1);
            if (temp) {
                strncpy(temp, input_text, cursor_pos);
                temp[cursor_pos] = '\0';
                cursor_x += midp_font_string_width(font, temp);
                free(temp);
            }
        }
        midp_graphics_draw_line(gfx, cursor_x, text_y + 5, cursor_x, text_y + font_height + 5);
    }
    
    /* Draw soft buttons */
    int btn_height = 25;
    midp_graphics_set_color(gfx, 0xC0C0C0, 255);
    midp_graphics_fill_rect(gfx, 0, gfx->height - btn_height, gfx->width, btn_height);
    midp_graphics_set_color(gfx, 0x000000, 255);
    midp_graphics_draw_line(gfx, 0, gfx->height - btn_height, gfx->width, gfx->height - btn_height);
    
    /* Cancel and OK buttons */
    midp_graphics_draw_string(gfx, "Cancel", 10, gfx->height - btn_height + 5, 0);
    midp_graphics_draw_string(gfx, "OK", gfx->width - 30, gfx->height - btn_height + 5, 0);
}

/*
 * Native method implementations
 */

/* Form.append(Item) */
static JavaValue native_form_append(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* form = (JavaObject*)args[0].ref;
    JavaObject* item = (JavaObject*)args[1].ref;
    
    if (!form || !item) {
        FORM_DEBUG("append: NULL form or item");
        return NATIVE_RETURN_INT(-1);
    }
    
    FORM_DEBUG("append: form=%p, item=%p (type=%d)", 
            (void*)form, (void*)item, get_item_type(item));
    
    /* Form fields: title, items — use dynamic slot lookup */
    int title_slot = find_object_field_slot(form, "title");
    int items_slot = find_object_field_slot(form, "items");
    if (title_slot < 0 || items_slot < 0 ||
        !OBJECT_HAS_FIELDS(form, items_slot + 1)) {
        FORM_DEBUG("append: Form field slots not found! title=%d, items=%d", title_slot, items_slot);
        return NATIVE_RETURN_INT(-1);
    }
    
    /* Get current items array */
    JavaArray* items = (JavaArray*)form->fields[items_slot].ref;
    int old_count = items ? items->length : 0;
    int new_count = old_count + 1;
    
    /* Create new items array with one more slot */
    JavaArray* new_items = jvm_new_array(jvm, DESC_OBJECT, new_count, NULL);
    if (!new_items) {
        FORM_DEBUG("append: Failed to create new items array");
        return NATIVE_RETURN_INT(-1);
    }
    
    /* Copy old items */
    if (items && items->length > 0) {
        void** old_data = (void**)array_data(items);
        void** new_data = (void**)array_data(new_items);
        for (int i = 0; i < old_count; i++) {
            new_data[i] = old_data[i];
        }
    }
    
    /* Add new item */
    void** new_data = (void**)array_data(new_items);
    new_data[old_count] = item;
    
    /* Update form's items array */
    form->fields[items_slot].ref = new_items;
    
    FORM_DEBUG("append: Added item %p at index %d, new count=%d",
            (void*)item, old_count, new_count);
    
    return NATIVE_RETURN_INT(old_count);  /* Return the index of the new item */
}

/* Form.delete(int) */
static JavaValue native_form_delete(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* form = (JavaObject*)args[0].ref;
    jint index = args[1].i;
    
    FORM_DEBUG("delete: form=%p, index=%d", (void*)form, index);
    
    if (!form) {
        return NATIVE_RETURN_VOID();
    }
    
    int items_slot = find_object_field_slot(form, "items");
    if (items_slot < 0 || !OBJECT_HAS_FIELDS(form, items_slot + 1)) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Get current items array */
    JavaArray* items = (JavaArray*)form->fields[items_slot].ref;
    if (!items || items->length == 0) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Validate index */
    if (index < 0 || index >= items->length) {
        return NATIVE_RETURN_VOID();
    }
    
    /* v34.27: clamp focus if this is the current form (see delete) */
    if (form == g_current_displayable) {
        int new_len = items->length - 1;
        if (index < g_focused_item_index) g_focused_item_index--;
        if (g_focused_item_index > new_len - 1) g_focused_item_index = new_len - 1;
        if (g_focused_item_index < 0) g_focused_item_index = 0;
    }

    /* Create new array with one less element */
    int new_len = items->length - 1;
    if (new_len == 0) {
        /* Remove all items - set to empty array */
        form->fields[items_slot].ref = jvm_new_array(jvm, DESC_OBJECT, 0, NULL);
        return NATIVE_RETURN_VOID();
    }
    
    JavaArray* new_items = jvm_new_array(jvm, DESC_OBJECT, new_len, NULL);
    if (!new_items) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Copy items, skipping the deleted one */
    void** old_data = (void**)array_data(items);
    void** new_data = (void**)array_data(new_items);
    for (int i = 0; i < index; i++) {
        new_data[i] = old_data[i];
    }
    for (int i = index + 1; i < items->length; i++) {
        new_data[i - 1] = old_data[i];
    }
    
    /* Update form's items array */
    form->fields[items_slot].ref = new_items;

    FORM_DEBUG("delete: removed item at index %d, new size=%d", index, new_len);
    
    return NATIVE_RETURN_VOID();
}

/* Form.size() */
static JavaValue native_form_size(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* form = (JavaObject*)args[0].ref;
    
    /* Count items in the form */
    if (form) {
        int items_slot = find_object_field_slot(form, "items");
        if (items_slot >= 0 && OBJECT_HAS_FIELDS(form, items_slot + 1)) {
            JavaArray* items = (JavaArray*)form->fields[items_slot].ref;
            if (items) {
                return NATIVE_RETURN_INT(items->length);
            }
        }
    }
    
    return NATIVE_RETURN_INT(0);
}

/* Form.get(int) - returns the Item at the specified index */
static JavaValue native_form_get(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* form = (JavaObject*)args[0].ref;
    jint index = args[1].i;
    
    if (!form) {
        return NATIVE_RETURN_NULL();
    }
    
    int items_slot = find_object_field_slot(form, "items");
    if (items_slot < 0 || !OBJECT_HAS_FIELDS(form, items_slot + 1)) {
        FORM_DEBUG("get: Form doesn't have enough fields!");
        return NATIVE_RETURN_NULL();
    }
    
    JavaArray* items = (JavaArray*)form->fields[items_slot].ref;
    if (!items || index < 0 || index >= (jint)items->length) {
        FORM_DEBUG("get: Invalid index %d or null items array", index);
        return NATIVE_RETURN_NULL();
    }
    
    /* Get item from array - items array stores JavaObject* pointers */
    void** item_ptrs = (void**)array_data(items);
    JavaObject* item = (JavaObject*)item_ptrs[index];
    
    FORM_DEBUG("get: Returning item at index %d = %p", index, (void*)item);
    return NATIVE_RETURN_OBJECT(item);
}

/* Form.getTitle() */
static JavaValue native_form_getTitle(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* form = (JavaObject*)args[0].ref;
    
    if (form) {
        int title_slot = find_object_field_slot(form, "title");
        if (title_slot >= 0 && OBJECT_HAS_FIELDS(form, title_slot + 1)) {
            JavaString* title = (JavaString*)form->fields[title_slot].ref;
            return NATIVE_RETURN_OBJECT(title);
        }
    }
    
    return NATIVE_RETURN_NULL();
}

/* Form.setTitle(String) */
static JavaValue native_form_setTitle(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* form = (JavaObject*)args[0].ref;
    JavaString* title = (JavaString*)args[1].ref;
    
    if (form) {
        int title_slot = find_object_field_slot(form, "title");
        if (title_slot >= 0 && OBJECT_HAS_FIELDS(form, title_slot + 1)) {
            form->fields[title_slot].ref = title;
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* Form.setItemStateListener(ItemStateListener) */
static JavaValue native_form_setItemStateListener(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* form = (JavaObject*)args[0].ref;
    JavaObject* listener = (JavaObject*)args[1].ref;
    (void)form;
    
    g_item_state_listener = listener;
    FORM_DEBUG("setItemStateListener: %p", (void*)listener);
    
    return NATIVE_RETURN_VOID();
}

/* StringItem.setText(String) */
static JavaValue native_stringitem_setText(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* item = (JavaObject*)args[0].ref;
    JavaString* text = (JavaString*)args[1].ref;
    
    if (item) {
        int text_slot = find_object_field_slot(item, "text");
        if (text_slot >= 0 && OBJECT_HAS_FIELDS(item, text_slot + 1)) {
            item->fields[text_slot].ref = text; /* text field */
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* StringItem.getText() */
static JavaValue native_stringitem_getText(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* item = (JavaObject*)args[0].ref;
    
    if (item) {
        int text_slot = find_object_field_slot(item, "text");
        if (text_slot >= 0 && OBJECT_HAS_FIELDS(item, text_slot + 1)) {
            return NATIVE_RETURN_OBJECT(item->fields[text_slot].ref);
        }
    }
    
    return NATIVE_RETURN_NULL();
}

/* TextField.setString(String) */
static JavaValue native_textfield_setString(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* item = (JavaObject*)args[0].ref;
    JavaString* text = (JavaString*)args[1].ref;
    
    if (item) {
        int text_slot = find_object_field_slot(item, "text");
        if (text_slot >= 0 && OBJECT_HAS_FIELDS(item, text_slot + 1)) {
            item->fields[text_slot].ref = text;
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* TextField.getString() */
static JavaValue native_textfield_getString(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* item = (JavaObject*)args[0].ref;
    
    if (item) {
        int text_slot = find_object_field_slot(item, "text");
        if (text_slot >= 0 && OBJECT_HAS_FIELDS(item, text_slot + 1)) {
            return NATIVE_RETURN_OBJECT(item->fields[text_slot].ref);
        }
    }
    
    return NATIVE_RETURN_NULL();
}

/* TextField.setChars(char[], int, int) */
static JavaValue native_textfield_setChars(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* item = (JavaObject*)args[0].ref;
    JavaArray* chars = (JavaArray*)args[1].ref;
    jint offset = args[2].i;
    jint length = args[3].i;
    
    if (item && chars && chars->element_type == T_CHAR) {
        /* Create new String from char array */
        jchar* chars_data = (jchar*)array_data(chars);
        JavaString* str = jvm_new_string_utf16(jvm, chars_data + offset, length);
        int text_slot = find_object_field_slot(item, "text");
        if (text_slot >= 0 && OBJECT_HAS_FIELDS(item, text_slot + 1)) {
            item->fields[text_slot].ref = str;
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* TextField.size() */
static JavaValue native_textfield_size(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* item = (JavaObject*)args[0].ref;
    
    if (item) {
        int text_slot = find_object_field_slot(item, "text");
        if (text_slot >= 0 && OBJECT_HAS_FIELDS(item, text_slot + 1)) {
            JavaString* text = (JavaString*)item->fields[text_slot].ref;
            if (text) {
                return NATIVE_RETURN_INT(text->length);
            }
        }
    }
    
    return NATIVE_RETURN_INT(0);
}

/* TextField.getMaxSize() */
static JavaValue native_textfield_getMaxSize(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* item = (JavaObject*)args[0].ref;
    
    if (item) {
        int maxsize_slot = find_object_field_slot(item, "maxSize");
        if (maxsize_slot >= 0 && OBJECT_HAS_FIELDS(item, maxsize_slot + 1)) {
            return NATIVE_RETURN_INT(item->fields[maxsize_slot].i);
        }
    }
    
    return NATIVE_RETURN_INT(32);  /* Default */
}

/* TextField.setMaxSize(int) */
static JavaValue native_textfield_setMaxSize(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* item = (JavaObject*)args[0].ref;
    jint max_size = args[1].i;
    
    if (item && max_size > 0) {
        int maxsize_slot = find_object_field_slot(item, "maxSize");
        if (maxsize_slot >= 0 && OBJECT_HAS_FIELDS(item, maxsize_slot + 1)) {
            item->fields[maxsize_slot].i = max_size;
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* TextField.getConstraints() */
static JavaValue native_textfield_getConstraints(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* item = (JavaObject*)args[0].ref;
    
    if (item) {
        int constraints_slot = find_object_field_slot(item, "constraints");
        if (constraints_slot >= 0 && OBJECT_HAS_FIELDS(item, constraints_slot + 1)) {
            return NATIVE_RETURN_INT(item->fields[constraints_slot].i);
        }
    }
    
    return NATIVE_RETURN_INT(0);  /* ANY */
}

/* TextField.setConstraints(int) */
static JavaValue native_textfield_setConstraints(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* item = (JavaObject*)args[0].ref;
    jint constraints = args[1].i;
    
    if (item) {
        int constraints_slot = find_object_field_slot(item, "constraints");
        if (constraints_slot >= 0 && OBJECT_HAS_FIELDS(item, constraints_slot + 1)) {
            item->fields[constraints_slot].i = constraints;
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* TextField.getChars(char[]) */
static JavaValue native_textfield_getChars(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* item = (JavaObject*)args[0].ref;
    JavaArray* dest = (JavaArray*)args[1].ref;
    
    if (!item || !dest || dest->element_type != T_CHAR) {
        return NATIVE_RETURN_VOID();
    }
    
    int text_slot = find_object_field_slot(item, "text");
    if (text_slot >= 0 && OBJECT_HAS_FIELDS(item, text_slot + 1)) {
        JavaString* text = (JavaString*)item->fields[text_slot].ref;
        if (text) {
            const jchar* src_data = string_chars(text);
            jchar* dest_data = (jchar*)array_data(dest);
            int copy_len = text->length < dest->length ? text->length : dest->length;
            for (int i = 0; i < copy_len; i++) {
                dest_data[i] = src_data[i];
            }
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* TextField.delete(int offset, int length) */
static JavaValue native_textfield_delete(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* item = (JavaObject*)args[0].ref;
    jint offset = args[1].i;
    jint length = args[2].i;
    
    if (!item) {
        return NATIVE_RETURN_VOID();
    }
    
    int text_slot = find_object_field_slot(item, "text");
    if (text_slot < 0 || !OBJECT_HAS_FIELDS(item, text_slot + 1)) {
        return NATIVE_RETURN_VOID();
    }
    
    JavaString* text = (JavaString*)item->fields[text_slot].ref;
    if (!text || offset < 0 || length <= 0 || offset >= text->length) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Adjust length if it goes beyond string */
    if (offset + length > text->length) {
        length = text->length - offset;
    }
    
    /* Create new string without the deleted portion */
    const jchar* old_data = string_chars(text);
    int new_len = text->length - length;
    
    JavaArray* new_chars = jvm_new_array(jvm, T_CHAR, new_len, NULL);
    if (!new_chars) return NATIVE_RETURN_VOID();
    
    jchar* new_data = (jchar*)array_data(new_chars);
    
    /* Copy before offset */
    for (int i = 0; i < offset; i++) {
        new_data[i] = old_data[i];
    }
    /* Copy after deleted portion */
    for (int i = offset + length; i < text->length; i++) {
        new_data[i - length] = old_data[i];
    }
    
    /* Create new string */
    JavaString* new_str = jvm_new_string_utf16(jvm, new_data, new_len);
    item->fields[text_slot].ref = new_str;
    
    return NATIVE_RETURN_VOID();
}

/* TextField.insert(String, int) */
static JavaValue native_textfield_insert(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* item = (JavaObject*)args[0].ref;
    JavaString* insert_str = (JavaString*)args[1].ref;
    jint position = args[2].i;
    
    if (!item || !insert_str) {
        return NATIVE_RETURN_VOID();
    }
    
    int text_slot = find_object_field_slot(item, "text");
    int maxsize_slot = find_object_field_slot(item, "maxSize");
    if (text_slot < 0 || !OBJECT_HAS_FIELDS(item, text_slot + 1)) {
        return NATIVE_RETURN_VOID();
    }
    
    JavaString* text = (JavaString*)item->fields[text_slot].ref;
    int old_len = text ? text->length : 0;
    
    /* Check max size */
    int max_size = (maxsize_slot >= 0 && OBJECT_HAS_FIELDS(item, maxsize_slot + 1)) ?
                   item->fields[maxsize_slot].i : 32;
    if (old_len + insert_str->length > max_size) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Clamp position */
    if (position < 0) position = 0;
    if (position > old_len) position = old_len;
    
    /* Create new string */
    int new_len = old_len + insert_str->length;
    JavaArray* new_chars = jvm_new_array(jvm, T_CHAR, new_len, NULL);
    if (!new_chars) return NATIVE_RETURN_VOID();
    
    jchar* new_data = (jchar*)array_data(new_chars);
    const jchar* insert_data = string_chars(insert_str);
    const jchar* old_data = text ? string_chars(text) : NULL;
    
    /* Copy before position */
    for (int i = 0; i < position; i++) {
        new_data[i] = old_data ? old_data[i] : 0;
    }
    /* Copy inserted string */
    for (int i = 0; i < insert_str->length; i++) {
        new_data[position + i] = insert_data[i];
    }
    /* Copy after position */
    for (int i = position; i < old_len; i++) {
        new_data[i + insert_str->length] = old_data ? old_data[i] : 0;
    }
    
    /* Create new string and set it */
    JavaString* new_str = jvm_new_string_utf16(jvm, new_data, new_len);
    item->fields[text_slot].ref = new_str;
    
    return NATIVE_RETURN_VOID();
}

/* TextField.getCaretPosition() - returns current cursor position */
static JavaValue native_textfield_getCaretPosition(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* item = (JavaObject*)args[0].ref;
    
    /* Return end of text as default caret position */
    if (item) {
        int text_slot = find_object_field_slot(item, "text");
        if (text_slot >= 0 && OBJECT_HAS_FIELDS(item, text_slot + 1)) {
            JavaString* text = (JavaString*)item->fields[text_slot].ref;
            if (text) {
                return NATIVE_RETURN_INT(text->length);
            }
        }
    }
    
    return NATIVE_RETURN_INT(0);
}

/* ChoiceGroup.append(String, Image) */
static JavaValue native_choicegroup_append(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* group = (JavaObject*)args[0].ref;
    JavaString* text = (JavaString*)args[1].ref;
    JavaObject* image = (JavaObject*)args[2].ref;
    (void)image;  /* Image not used in basic implementation */
    
    FORM_DEBUG("[ChoiceGroup] append: text=%s", 
            text ? get_string_from_object(jvm, (JavaObject*)text) : "(null)");
    
    if (!group) {
        return NATIVE_RETURN_INT(-1);
    }
    
    /* Get field slots — use dynamic slot lookup */
    int strings_slot = find_object_field_slot(group, "strings");
    int selected_slot = find_object_field_slot(group, "selected");
    if (strings_slot < 0 || selected_slot < 0 ||
        !OBJECT_HAS_FIELDS(group, selected_slot + 1)) {
        return NATIVE_RETURN_INT(-1);
    }
    
    /* Get current strings and selected arrays */
    JavaArray* strings = (JavaArray*)group->fields[strings_slot].ref;
    JavaArray* selected = (JavaArray*)group->fields[selected_slot].ref;
    
    int old_len = strings ? strings->length : 0;
    int new_len = old_len + 1;
    
    /* Create new strings array */
    JavaArray* new_strings = jvm_new_array(jvm, DESC_OBJECT, new_len, NULL);
    if (!new_strings) {
        return NATIVE_RETURN_INT(-1);
    }
    
    /* Create new selected array */
    JavaArray* new_selected = jvm_new_array(jvm, T_BOOLEAN, new_len, NULL);
    if (!new_selected) {
        return NATIVE_RETURN_INT(-1);
    }
    
    /* Copy old strings */
    if (strings && strings->length > 0) {
        void** old_data = (void**)array_data(strings);
        void** new_data = (void**)array_data(new_strings);
        for (int i = 0; i < old_len; i++) {
            new_data[i] = old_data[i];
        }
    }
    
    /* Copy old selected */
    if (selected && selected->length > 0) {
        jboolean* old_sel = (jboolean*)array_data(selected);
        jboolean* new_sel = (jboolean*)array_data(new_selected);
        for (int i = 0; i < old_len; i++) {
            new_sel[i] = old_sel[i];
        }
    }
    
    /* Add new string at the end */
    void** new_str_data = (void**)array_data(new_strings);
    new_str_data[old_len] = text;
    
    /* Initialize new selected as false */
    jboolean* new_sel_data = (jboolean*)array_data(new_selected);
    new_sel_data[old_len] = JNI_FALSE;
    
    /* Update group's arrays */
    group->fields[strings_slot].ref = new_strings;
    group->fields[selected_slot].ref = new_selected;
    
    FORM_DEBUG("[ChoiceGroup] append: added at index %d, new size=%d", old_len, new_len);
    
    return NATIVE_RETURN_INT(old_len);  /* Return index of new element */
}

/* ChoiceGroup.setSelectedIndex(int, boolean) */
static JavaValue native_choicegroup_setSelectedIndex(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* group = (JavaObject*)args[0].ref;
    jint index = args[1].i;
    jboolean selected = args[2].i;
    
    FORM_DEBUG("[ChoiceGroup] setSelectedIndex: index=%d, selected=%d", index, selected);
    
    if (!group) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Get field slots — use dynamic slot lookup */
    int choicetype_slot = find_object_field_slot(group, "choiceType");
    int selected_slot = find_object_field_slot(group, "selected");
    if (choicetype_slot < 0 || selected_slot < 0 ||
        !OBJECT_HAS_FIELDS(group, selected_slot + 1)) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Get selected array */
    JavaArray* selected_arr = (JavaArray*)group->fields[selected_slot].ref;
    if (!selected_arr || selected_arr->element_type != T_BOOLEAN) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Validate index */
    if (index < 0 || index >= selected_arr->length) {
        return NATIVE_RETURN_VOID();
    }
    
    /* For EXCLUSIVE choice type, clear all other selections first */
    jint choice_type = group->fields[choicetype_slot].i;
    if (choice_type == 1) {  /* Choice.EXCLUSIVE = 1 */
        jboolean* sel_data = (jboolean*)array_data(selected_arr);
        for (int i = 0; i < selected_arr->length; i++) {
            sel_data[i] = JNI_FALSE;
        }
    }
    
    /* Set the selected index */
    jboolean* sel_data = (jboolean*)array_data(selected_arr);
    sel_data[index] = selected;
    
    FORM_DEBUG("[ChoiceGroup] setSelectedIndex: set index %d to %d", index, selected);
    
    return NATIVE_RETURN_VOID();
}

/* ChoiceGroup.getSelectedIndex() */
static JavaValue native_choicegroup_getSelectedIndex(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* group = (JavaObject*)args[0].ref;
    
    /* Return first selected index */
    if (group) {
        int selected_slot = find_object_field_slot(group, "selected");
        if (selected_slot >= 0 && OBJECT_HAS_FIELDS(group, selected_slot + 1)) {
            JavaArray* selected = (JavaArray*)group->fields[selected_slot].ref;
            if (selected && selected->element_type == T_BOOLEAN) {
                jboolean* selected_data = (jboolean*)array_data(selected);
                for (int i = 0; i < selected->length; i++) {
                    if (selected_data[i]) {
                        return NATIVE_RETURN_INT(i);
                    }
                }
            }
        }
    }
    
    return NATIVE_RETURN_INT(-1);
}

/* Gauge.setValue(int) */
static JavaValue native_gauge_setValue(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* gauge = (JavaObject*)args[0].ref;
    jint value = args[1].i;
    
    if (gauge) {
        int value_slot = find_object_field_slot(gauge, "value");
        if (value_slot >= 0 && OBJECT_HAS_FIELDS(gauge, value_slot + 1)) {
            jint old_value = gauge->fields[value_slot].i;
            gauge->fields[value_slot].i = value;
            
            /* Notify ItemStateListener if value changed (v18: coalescing) */
            if (old_value != value && g_item_state_listener) {
                form_notify_item_state_changed(jvm, gauge);
            }
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* Notify ItemStateListener with coalescing (audit M-14, v18) */
static void form_notify_item_state_changed(JVM* jvm, JavaObject* item) {
    if (!jvm || !g_item_state_listener || !item) return;
    if (g_ism_dispatching) return;  /* nested notification from a listener */
    
    JavaClass* lc = g_item_state_listener->header.clazz;
    if (!lc) return;
    
    JavaMethod* ism = jvm_resolve_method(jvm, lc, "itemStateChanged",
                                         "(Ljavax/microedition/lcdui/Item;)V");
    if (!ism) return;
    
    g_ism_dispatching = 1;
    JavaValue args[2];
    args[0].ref = g_item_state_listener;
    args[1].ref = item;
    JavaValue result;
    JavaThread* th = jvm_current_thread(jvm);
    execute_method(jvm, th, ism, args, &result);
    g_ism_dispatching = 0;
}

/* Gauge.getValue() */
static JavaValue native_gauge_getValue(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* gauge = (JavaObject*)args[0].ref;
    
    FORM_DEBUG("[Gauge] getValue: gauge=%p", (void*)gauge);
    
    if (gauge) {
        int value_slot = find_object_field_slot(gauge, "value");
        if (value_slot >= 0 && OBJECT_HAS_FIELDS(gauge, value_slot + 1)) {
            FORM_DEBUG("[Gauge] getValue: fields[%d].i = %d", value_slot, gauge->fields[value_slot].i);
            return NATIVE_RETURN_INT(gauge->fields[value_slot].i);
        }
    }
    
    FORM_DEBUG("[Gauge] getValue: returning 0 (not enough fields)");
    return NATIVE_RETURN_INT(0);
}

/* List.append(String, Image) */
static JavaValue native_list_append(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* list = (JavaObject*)args[0].ref;
    JavaString* text = (JavaString*)args[1].ref;
    JavaObject* image = (JavaObject*)args[2].ref;
    
    (void)image;  /* Image not used in basic implementation */
    
    FORM_DEBUG("append: text=%s", 
            text ? get_string_from_object(jvm, (JavaObject*)text) : "(null)");
    
    if (!list) {
        return NATIVE_RETURN_INT(0);
    }
    
    /* Get field offsets considering inheritance */
    JavaClass* list_class = list->header.clazz;
    int strings_idx = get_field_offset(list_class, "strings");
    int selected_idx = get_field_offset(list_class, "selected");
    int old_len = -1;   /* v35: hoisted - returned as the new element index */

    /* List fields: title, listType, strings[], selected[] */
    if (strings_idx >= 0 && selected_idx >= 0) {
        JavaArray* strings = (JavaArray*)list->fields[strings_idx].ref;
        JavaArray* selected = (JavaArray*)list->fields[selected_idx].ref;

        old_len = strings ? strings->length : 0;
        int new_len = old_len + 1;
        
        /* Create new String array with one more element */
        JavaArray* new_strings = jvm_new_array(jvm, DESC_OBJECT, new_len, NULL);
        if (new_strings) {
            void** dst_data = (void**)array_data(new_strings);
            
            /* Copy old elements */
            if (strings && strings->element_type == DESC_OBJECT) {
                void** src_data = (void**)array_data(strings);
                for (int i = 0; i < old_len; i++) {
                    dst_data[i] = src_data[i];
                }
            }
            
            /* Add new element */
            dst_data[old_len] = text;
            list->fields[strings_idx].ref = new_strings;
        }
        
        /* Create new selected array with one more element */
        JavaArray* new_selected = jvm_new_array(jvm, T_BOOLEAN, new_len, NULL);
        if (new_selected) {
            jboolean* dst_sel = (jboolean*)array_data(new_selected);
            
            /* Copy old selected values */
            if (selected && selected->element_type == T_BOOLEAN) {
                jboolean* src_sel = (jboolean*)array_data(selected);
                for (int i = 0; i < old_len; i++) {
                    dst_sel[i] = src_sel[i];
                }
            } else {
                /* Initialize with all false */
                for (int i = 0; i < old_len; i++) {
                    dst_sel[i] = 0;
                }
            }
            
            /* New element is not selected by default */
            dst_sel[old_len] = 0;
            list->fields[selected_idx].ref = new_selected;
        }
        
        FORM_DEBUG("append: added element at index %d, new size=%d", old_len, new_len);
    }
    
    /* v35 FIX: spec says append returns the index of the added element;
     * a constant 0 broke games tracking their menu positions. */
    return NATIVE_RETURN_INT(old_len >= 0 ? old_len : 0);
}

/* List.getSelectedIndex() */
static JavaValue native_list_getSelectedIndex(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* list = (JavaObject*)args[0].ref;
    
    FORM_DEBUG("getSelectedIndex: list=%p, g_current_displayable=%p, g_list_selected_index=%d",
            (void*)list, (void*)g_current_displayable, g_list_selected_index);
    
    /* For IMPLICIT list, return the current focused/selected index from UI */
    if (list == g_current_displayable) {
        /* v34.27: clamp against the live element count - delete() on the
         * current list used to leave a stale out-of-range index here. */
        int list_size = 0;
        int strings_slot = find_object_field_slot(list, "strings");
        if (strings_slot >= 0 && OBJECT_HAS_FIELDS(list, strings_slot + 1)) {
            JavaArray* strings = (JavaArray*)list->fields[strings_slot].ref;
            list_size = strings ? strings->length : 0;
        }
        if (list_size > 0) {
            if (g_list_selected_index > list_size - 1) g_list_selected_index = list_size - 1;
            if (g_list_selected_index < 0) g_list_selected_index = 0;
        }
        FORM_DEBUG("getSelectedIndex: returning g_list_selected_index=%d", g_list_selected_index);
        return NATIVE_RETURN_INT(g_list_selected_index);
    }
    
    /* For other cases, return from selected[] array */
    if (list) {
        JavaClass* list_class = list->header.clazz;
        int selected_idx = get_field_offset(list_class, "selected");
        
        if (selected_idx >= 0) {
            JavaArray* selected = (JavaArray*)list->fields[selected_idx].ref;
            if (selected && selected->element_type == T_BOOLEAN) {
                jboolean* selected_data = (jboolean*)array_data(selected);
                for (int i = 0; i < selected->length; i++) {
                    if (selected_data[i]) {
                        FORM_DEBUG("getSelectedIndex: found selected at index %d in selected[]", i);
                        return NATIVE_RETURN_INT(i);
                    }
                }
            }
        }
    }
    
    FORM_DEBUG("getSelectedIndex: fallback to g_list_selected_index=%d", g_list_selected_index);
    return NATIVE_RETURN_INT(g_list_selected_index);
}

/* List.setSelectedIndex(int, boolean) — v23: real implementation. Updates
 * the selected[] boolean array (the storage getSelectedIndex reads) and the
 * global focused index for IMPLICIT lists that are the current screen. */
static JavaValue native_list_setSelectedIndex(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* list = (JavaObject*)args[0].ref;
    jint index = args[1].i;
    jboolean selected = args[2].i;

    FORM_DEBUG("setSelectedIndex: index=%d, selected=%d", index, selected);

    if (!list) return NATIVE_RETURN_VOID();

    /* Update the selected[] array */
    JavaClass* list_class = list->header.clazz;
    int selected_idx = get_field_offset(list_class, "selected");
    if (selected_idx >= 0) {
        JavaArray* selected_arr = (JavaArray*)list->fields[selected_idx].ref;
        if (selected_arr && selected_arr->element_type == T_BOOLEAN &&
            index >= 0 && index < selected_arr->length) {
            jboolean* sel_data = (jboolean*)array_data(selected_arr);
            /* EXCLUSIVE list: selecting one element clears the others */
            int choice_slot = find_object_field_slot(list, "choiceType");
            jint choice_type = (choice_slot >= 0) ? list->fields[choice_slot].i : 0;
            if (selected && choice_type == 1) {  /* Choice.EXCLUSIVE */
                for (int i = 0; i < selected_arr->length; i++) {
                    sel_data[i] = JNI_FALSE;
                }
            }
            sel_data[index] = selected;
        }
    }

    /* IMPLICIT list that is on screen: sync the focused index */
    if (list == g_current_displayable) {
        if (selected && index >= 0) {
            g_list_selected_index = index;
        }
    }

    return NATIVE_RETURN_VOID();
}

/* List.delete(int elementNum) - delete item at index */
static JavaValue native_list_delete(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* list = (JavaObject*)args[0].ref;
    jint index = args[1].i;
    
    if (!list || index < 0) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Get field offsets considering inheritance */
    JavaClass* list_class = list->header.clazz;
    int strings_idx = get_field_offset(list_class, "strings");
    int selected_idx = get_field_offset(list_class, "selected");
    
    /* List fields: title, listType, strings[], selected[] */
    if (strings_idx >= 0 && selected_idx >= 0) {
        JavaArray* strings = (JavaArray*)list->fields[strings_idx].ref;
        JavaArray* selected = (JavaArray*)list->fields[selected_idx].ref;
        
        if (strings && strings->length > 0 && index < strings->length) {
            /* Create new arrays without the deleted element */
            int new_len = strings->length - 1;
            
            /* Create new String array */
            JavaArray* new_strings = jvm_new_array(jvm, DESC_OBJECT, new_len, NULL);
            if (new_strings && strings->element_type == DESC_OBJECT) {
                void** src_data = (void**)array_data(strings);
                void** dst_data = (void**)array_data(new_strings);
                
                /* Copy elements before index */
                for (int i = 0; i < index; i++) {
                    dst_data[i] = src_data[i];
                }
                /* Copy elements after index */
                for (int i = index; i < new_len; i++) {
                    dst_data[i] = src_data[i + 1];
                }
                list->fields[strings_idx].ref = new_strings;
            }
            
            /* Create new selected array */
            JavaArray* new_selected = jvm_new_array(jvm, T_BOOLEAN, new_len, NULL);
            if (new_selected && selected && selected->element_type == T_BOOLEAN) {
                jboolean* src_sel = (jboolean*)array_data(selected);
                jboolean* dst_sel = (jboolean*)array_data(new_selected);
                
                for (int i = 0; i < index; i++) {
                    dst_sel[i] = src_sel[i];
                }
                for (int i = index; i < new_len; i++) {
                    dst_sel[i] = src_sel[i + 1];
                }
            } else if (new_selected) {
                /* Initialize with all false */
                jboolean* dst_sel = (jboolean*)array_data(new_selected);
                for (int i = 0; i < new_len; i++) {
                    dst_sel[i] = 0;
                }
            }
            list->fields[selected_idx].ref = new_selected;

            /* v34.27: keep the selection index valid on the CURRENT list
             * (stale indexes made getSelectedIndex return out-of-range
             * values and froze DOWN navigation). */
            if (list == g_current_displayable) {
                if (index < g_list_selected_index) {
                    g_list_selected_index--;
                }
                if (g_list_selected_index > new_len - 1) g_list_selected_index = new_len - 1;
                if (g_list_selected_index < 0) g_list_selected_index = 0;
            }

            FORM_DEBUG("delete(%d): new size=%d", index, new_len);
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* List.set(int elementNum, String stringElement, Image imageElement) - replace item at index */
static JavaValue native_list_set(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* list = (JavaObject*)args[0].ref;
    jint index = args[1].i;
    JavaString* text = (JavaString*)args[2].ref;
    JavaObject* image = (JavaObject*)args[3].ref;
    (void)image;  /* Image not used in basic implementation */
    
    if (!list || index < 0) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Get field offset considering inheritance */
    JavaClass* list_class = list->header.clazz;
    int strings_idx = get_field_offset(list_class, "strings");
    
    /* List fields: title, listType, strings[], selected[] */
    if (strings_idx >= 0) {
        JavaArray* strings = (JavaArray*)list->fields[strings_idx].ref;
        
        if (strings && strings->element_type == DESC_OBJECT && index < strings->length) {
            void** strings_data = (void**)array_data(strings);
            strings_data[index] = text;
            
            FORM_DEBUG("[List] set(%d, \"%s\")", index, 
                    text ? get_string_from_object(jvm, (JavaObject*)text) : "null");
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* Alert.setString(String) */
static JavaValue native_alert_setString(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* alert = (JavaObject*)args[0].ref;
    JavaString* text = (JavaString*)args[1].ref;
    
    if (alert) {
        int text_slot = find_object_field_slot(alert, "text");
        if (text_slot >= 0 && OBJECT_HAS_FIELDS(alert, text_slot + 1)) {
            alert->fields[text_slot].ref = text;
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* TextBox.setString(String) */
static JavaValue native_textbox_setString(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* textbox = (JavaObject*)args[0].ref;
    JavaString* text = (JavaString*)args[1].ref;
    
    if (textbox) {
        int text_slot = find_object_field_slot(textbox, "text");
        if (text_slot >= 0 && OBJECT_HAS_FIELDS(textbox, text_slot + 1)) {
            textbox->fields[text_slot].ref = text;
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* TextBox.getString() */
static JavaValue native_textbox_getString(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* textbox = (JavaObject*)args[0].ref;
    
    if (textbox) {
        int text_slot = find_object_field_slot(textbox, "text");
        if (text_slot >= 0 && OBJECT_HAS_FIELDS(textbox, text_slot + 1)) {
            return NATIVE_RETURN_OBJECT(textbox->fields[text_slot].ref);
        }
    }
    
    return NATIVE_RETURN_NULL();
}

/* ============================================
 * HELPER FUNCTIONS FOR FIELD ACCESS
 * ============================================ */

/* Find the slot index for a named field in an object.
 * This correctly traverses the class hierarchy from Object down to the actual class,
 * calculating the cumulative slot index for instance fields.
 * Returns -1 if field not found. */
static int find_object_field_slot(JavaObject* obj, const char* field_name) {
    if (!obj || !field_name) return -1;
    
    JavaClass* clazz = obj->header.clazz;
    if (!clazz) {
        return -1;
    }
    
    /* Build class hierarchy from Object to actual class */
    JavaClass* hierarchy[64];
    int depth = 0;
    JavaClass* c = clazz;
    while (c && depth < 64) {
        hierarchy[depth++] = c;
        c = c->super_class;
    }
    
    /* Search for field and calculate slot from Object down to actual class */
    int slot = 0;
    for (int h = depth - 1; h >= 0; h--) {
        JavaClass* current = hierarchy[h];
        if (!current->fields) continue;
        
        for (int i = 0; i < current->fields_count; i++) {
            JavaField* field = &current->fields[i];
            
            /* Skip static fields */
            if (field->access_flags & ACC_STATIC) continue;
            
            if (field->name && strcmp(field->name, field_name) == 0) {
                FORM_DEBUG("[find_object_field_slot] '%s' -> slot %d in %s",
                        field_name, slot, current->class_name ? current->class_name : "?");
                return slot;
            }
            
            slot++;
            /* Long and double take 2 slots */
            if (field->descriptor && 
                (field->descriptor[0] == 'J' || field->descriptor[0] == 'D')) {
                slot++;
            }
        }
    }
    
    FORM_DEBUG("[find_object_field_slot] '%s' NOT found in '%s'",
            field_name, clazz->class_name ? clazz->class_name : "?");
    return -1;  /* Field not found */
}

/* ============================================ */

/* Item.setLabel(String) */
static JavaValue native_item_setLabel(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* item = (JavaObject*)args[0].ref;
    JavaString* label = (JavaString*)args[1].ref;
    
    if (item && OBJECT_HAS_FIELDS(item, 1)) {
        item->fields[0].ref = label;
    }
    
    return NATIVE_RETURN_VOID();
}

/* Item.getLabel() */
static JavaValue native_item_getLabel(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* item = (JavaObject*)args[0].ref;
    
    if (item && OBJECT_HAS_FIELDS(item, 1)) {
        return NATIVE_RETURN_OBJECT(item->fields[0].ref);
    }
    
    return NATIVE_RETURN_NULL();
}

/* Command constructor */
static JavaValue native_command_init(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* cmd = (JavaObject*)args[0].ref;
    JavaString* label = (JavaString*)args[1].ref;
    jint type = args[2].i;
    jint priority = args[3].i;
    
    if (cmd && OBJECT_HAS_FIELDS(cmd, 3)) {
        cmd->fields[0].ref = label;     /* label */
        cmd->fields[1].i = type;        /* type */
        cmd->fields[2].i = priority;    /* priority */
    }
    
    FORM_DEBUG(" <init>: label=%s, type=%d, priority=%d\n",
            label ? get_string_from_object(jvm, (JavaObject*)label) : "(null)",
            type, priority);
    
    return NATIVE_RETURN_VOID();
}

/* Command.getCommandType() - returns the command type */
static JavaValue native_command_getCommandType(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* cmd = (JavaObject*)args[0].ref;
    
    if (!cmd) {
        return NATIVE_RETURN_INT(0);
    }
    
    /* Find the 'type' field - usually at index 1 */
    int type_slot = find_object_field_slot(cmd, "type");
    if (type_slot < 0) {
        /* Fallback: try known index */
        type_slot = 1;
    }
    
    if (OBJECT_HAS_FIELDS(cmd, type_slot + 1)) {
        jint type = cmd->fields[type_slot].i;
        FORM_DEBUG(" getCommandType: returning %d\n", type);
        return NATIVE_RETURN_INT(type);
    }
    
    return NATIVE_RETURN_INT(0);
}

/* Command.getLabel() - returns the command label string */
static JavaValue native_command_getLabel(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* cmd = (JavaObject*)args[0].ref;
    
    if (!cmd) {
        return NATIVE_RETURN_NULL();
    }
    
    /* Find the 'label' field - usually at index 0 */
    int label_slot = find_object_field_slot(cmd, "label");
    if (label_slot < 0) {
        label_slot = 0;
    }
    
    if (OBJECT_HAS_FIELDS(cmd, label_slot + 1)) {
        JavaString* label = (JavaString*)cmd->fields[label_slot].ref;
        return NATIVE_RETURN_OBJECT(label);
    }
    
    return NATIVE_RETURN_NULL();
}

/* Command.getPriority() - returns the command priority */
static JavaValue native_command_getPriority(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* cmd = (JavaObject*)args[0].ref;
    
    if (!cmd) {
        return NATIVE_RETURN_INT(0);
    }
    
    /* Find the 'priority' field - usually at index 2 */
    int priority_slot = find_object_field_slot(cmd, "priority");
    if (priority_slot < 0) {
        priority_slot = 2;
    }
    
    if (OBJECT_HAS_FIELDS(cmd, priority_slot + 1)) {
        jint priority = cmd->fields[priority_slot].i;
        return NATIVE_RETURN_INT(priority);
    }
    
    return NATIVE_RETURN_INT(0);
}

/* Displayable.addCommand(Command) */
static JavaValue native_displayable_addCommand(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* displayable = (JavaObject*)args[0].ref;
    JavaObject* cmd = (JavaObject*)args[1].ref;
    
    LOG_SAFE("[FORM] addCommand: displayable=%p, class=%s, cmd=%p\n",
            (void*)displayable,
            displayable && displayable->header.clazz && displayable->header.clazz->class_name ?
                displayable->header.clazz->class_name : "N/A",
            (void*)cmd);
    
    if (!displayable || !cmd) {
        return NATIVE_RETURN_VOID();
    }
    
    if (!displayable->header.clazz) {
        LOG_SAFE("[FORM] addCommand: displayable has no class! displayable=%p\n", (void*)displayable);
        return NATIVE_RETURN_VOID();
    }
    
    /* Find the commands field slot using the helper function */
    int commands_slot = find_object_field_slot(displayable, "commands");
    
    if (commands_slot < 0) {
        LOG_SAFE("[FORM] addCommand: 'commands' field not found in class '%s'! "
                "(instance_size=%zu)\n",
                displayable->header.clazz->class_name ? displayable->header.clazz->class_name : "?",
                displayable->header.clazz->instance_size);
        return NATIVE_RETURN_VOID();
    }
    
    FORM_DEBUG(" addCommand: commands field at slot %d\n", commands_slot);
    
    /* Safety check: verify object has enough fields */
    if (!OBJECT_HAS_FIELDS(displayable, commands_slot + 1)) {
        LOG_SAFE("[FORM] addCommand: displayable has insufficient fields! "
                "slot=%d, instance_size=%zu, needed=%zu, class=%s\n",
                commands_slot,
                displayable->header.clazz->instance_size,
                sizeof(ObjectHeader) + (commands_slot + 1) * sizeof(JavaValue),
                displayable->header.clazz->class_name ? displayable->header.clazz->class_name : "?");
        return NATIVE_RETURN_VOID();
    }
    
    /* Get current commands array */
    JavaArray* commands = (JavaArray*)displayable->fields[commands_slot].ref;
    int old_count = commands ? commands->length : 0;
    
    /* If commands is NULL (never initialized), create an empty array */
    if (!commands) {
        commands = jvm_new_array(jvm, DESC_OBJECT, 0, NULL);
        displayable->fields[commands_slot].ref = commands;
        old_count = 0;
    }
    
    FORM_DEBUG(" addCommand: commands_ptr=%p, old_count=%d", (void*)commands, old_count);
    
    /* Check if command already exists (prevent duplicates) */
    if (commands && is_heap_ptr_check(commands) && ((uintptr_t)commands > 0x10000) && commands->length > 0) {
        void* raw_data = array_data(commands);
        if (raw_data) {
            void** old_data = (void**)raw_data;
            for (int i = 0; i < old_count; i++) {
                if (old_data[i] == cmd) {
                    FORM_DEBUG(" addCommand: Command already exists at index %d, skipping\n", i);
                    return NATIVE_RETURN_VOID();  /* Already added */
                }
            }
        }
    }
    
    int new_count = old_count + 1;
    
    /* Create new commands array with one more slot */
    JavaArray* new_commands = jvm_new_array(jvm, DESC_OBJECT, new_count, NULL);
    if (!new_commands) {
        FORM_DEBUG(" addCommand: Failed to create new commands array\n");
        return NATIVE_RETURN_VOID();
    }
    
    LOG_SAFE("[FORM] addCommand: new_commands=%p, sizeof(JavaArray)=%zu\n",
            (void*)new_commands, sizeof(JavaArray));
    
    /* Copy old commands */
    if (old_count > 0) {
        void** old_data = (void**)array_data(commands);
        void** new_data = (void**)array_data(new_commands);
        for (int i = 0; i < old_count; i++) {
            new_data[i] = old_data[i];
        }
    }
    
    /* Add new command */
    void** new_data = (void**)array_data(new_commands);
    new_data[old_count] = cmd;
    
    /* Update displayable's commands array */
    displayable->fields[commands_slot].ref = new_commands;
    
    FORM_DEBUG(" addCommand: Added cmd %p at index %d, new count=%d\n",
            (void*)cmd, old_count, new_count);
    
    return NATIVE_RETURN_VOID();
}

/* Displayable.removeCommand(Command) */
static JavaValue native_displayable_removeCommand(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* displayable = (JavaObject*)args[0].ref;
    JavaObject* cmd = (JavaObject*)args[1].ref;
    
    FORM_DEBUG(" removeCommand: displayable=%p, cmd=%p\n",
            (void*)displayable, (void*)cmd);
    
    if (!displayable || !cmd) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Find the commands field slot */
    int commands_slot = find_object_field_slot(displayable, "commands");
    
    if (commands_slot < 0) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Get current commands array */
    JavaArray* commands = (JavaArray*)displayable->fields[commands_slot].ref;
    if (!commands || commands->length == 0) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Find and remove the command */
    void** old_data = (void**)array_data(commands);
    int old_count = commands->length;
    int found_idx = -1;
    
    for (int i = 0; i < old_count; i++) {
        if (old_data[i] == cmd) {
            found_idx = i;
            break;
        }
    }
    
    if (found_idx < 0) {
        return NATIVE_RETURN_VOID();  /* Command not found */
    }
    
    /* Create new array without the removed command */
    int new_count = old_count - 1;
    if (new_count == 0) {
        /* No commands left, set to empty array */
        JavaArray* empty = jvm_new_array(jvm, DESC_OBJECT, 0, NULL);
        displayable->fields[commands_slot].ref = empty;
        return NATIVE_RETURN_VOID();
    }
    
    JavaArray* new_commands = jvm_new_array(jvm, DESC_OBJECT, new_count, NULL);
    if (!new_commands) {
        return NATIVE_RETURN_VOID();
    }
    
    void** new_data = (void**)array_data(new_commands);
    for (int i = 0, j = 0; i < old_count; i++) {
        if (i != found_idx) {
            new_data[j++] = old_data[i];
        }
    }
    
    displayable->fields[commands_slot].ref = new_commands;
    
    FORM_DEBUG(" removeCommand: Removed cmd %p, new count=%d\n", (void*)cmd, new_count);
    
    return NATIVE_RETURN_VOID();
}

/* Displayable.setCommandListener(CommandListener) */
static JavaValue native_displayable_setCommandListener(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* displayable = (JavaObject*)args[0].ref;
    JavaObject* listener = (JavaObject*)args[1].ref;
    
    FORM_DEBUG(" setCommandListener: displayable=%p, listener=%p\n",
            (void*)displayable, (void*)listener);
    
    if (!displayable) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Find the listener field slot using the helper function */
    int listener_slot = find_object_field_slot(displayable, "listener");
    
    if (listener_slot >= 0) {
        displayable->fields[listener_slot].ref = listener;
        FORM_DEBUG(" Saved listener to field slot %d\n", listener_slot);
    } else {
        FORM_DEBUG(" setCommandListener: 'listener' field not found!\n");
    }
    
    return NATIVE_RETURN_VOID();
}

/* Spacer.setSize(int, int) */
static JavaValue native_spacer_setSize(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* spacer = (JavaObject*)args[0].ref;
    jint width = args[1].i;
    jint height = args[2].i;
    
    if (spacer) {
        int width_slot = find_object_field_slot(spacer, "width");
        int height_slot = find_object_field_slot(spacer, "height");
        if (width_slot >= 0 && height_slot >= 0 &&
            OBJECT_HAS_FIELDS(spacer, height_slot + 1)) {
            spacer->fields[width_slot].i = width;
            spacer->fields[height_slot].i = height;
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* Spacer.setMinimumSize(int, int) */
static JavaValue native_spacer_setMinimumSize(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    return native_spacer_setSize(jvm, thread, args, arg_count);
}

/* ImageItem.setImage(Image) */
static JavaValue native_imageitem_setImage(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* item = (JavaObject*)args[0].ref;
    JavaObject* image = (JavaObject*)args[1].ref;
    
    if (item) {
        int image_slot = find_object_field_slot(item, "image");
        if (image_slot >= 0 && OBJECT_HAS_FIELDS(item, image_slot + 1)) {
            item->fields[image_slot].ref = image;
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/*
 * Render a Form on screen
 * This is called from display.c when setCurrent() is called with a Form
 */
void midp_render_form(JVM* jvm, JavaObject* form) {
    if (!form) {
        LOG_SAFE("[FORM_RENDER] form is NULL!\n");
        return;
    }
    
    LOG_SAFE("[FORM_RENDER] Rendering form, class=%s, instance_size=%zu\n",
            form->header.clazz ? form->header.clazz->class_name : "NULL",
            form->header.clazz ? form->header.clazz->instance_size : 0);
    
    g_current_displayable = form;  /* Track current displayable */
    
    MidpGraphics* gfx = get_screen_graphics();
    if (!gfx) {
        LOG_SAFE("[FORM_RENDER] get_screen_graphics returned NULL! No framebuffer?\n");
        return;
    }
    
    LOG_SAFE("[FORM_RENDER] gfx=%p, pixels=%p, width=%d, height=%d\n",
            (void*)gfx, (void*)gfx->pixels, gfx->width, gfx->height);
    
    /* Clear screen */
    midp_graphics_set_color(gfx, 0xFFFFFF, 255);
    midp_graphics_fill_rect(gfx, 0, 0, gfx->width, gfx->height);
    
    /* Draw title bar */
    const char* title = "";
    int title_slot = find_object_field_slot(form, "title");
    if (title_slot >= 0 && OBJECT_HAS_FIELDS(form, title_slot + 1)) {
        JavaString* title_str = (JavaString*)form->fields[title_slot].ref;
        if (title_str) title = get_string_from_object(jvm, (JavaObject*)title_str);
    }
    
    /* Title background */
    midp_graphics_set_color(gfx, 0x000080, 255);
    midp_graphics_fill_rect(gfx, 0, 0, gfx->width, 20);
    midp_graphics_set_color(gfx, 0xFFFFFF, 255);
    midp_graphics_draw_string(gfx, title, gfx->width / 2, 3, 0x10); /* HCENTER */
    
    /* Draw separator */
    midp_graphics_set_color(gfx, 0x808080, 255);
    midp_graphics_draw_line(gfx, 0, 20, gfx->width, 20);
    
    /* Get items array from form */
    int y = 25;
    
    /* Limit y if keyboard is active */
    int max_y = g_vkb.active ? (gfx->height - 170) : (gfx->height - 30);
    
    int items_slot = find_object_field_slot(form, "items");
    if (items_slot >= 0 && OBJECT_HAS_FIELDS(form, items_slot + 1)) {
        JavaArray* items = (JavaArray*)form->fields[items_slot].ref;
        
        /* v34.27: clamp the focus highlight to the live item count */
        if (items) {
            if (g_focused_item_index >= items->length) g_focused_item_index = items->length - 1;
            if (g_focused_item_index < 0) g_focused_item_index = 0;
        }
        
        /* v34.65: self-heal the focus onto the first interactive item —
         * a focus parked on a label row is invisible (labels draw no
         * cursor), which reads as "nothing is selectable". */
        if (items && items->element_type == DESC_OBJECT) {
            void** items_data_sh = (void**)array_data(items);
            while (g_focused_item_index < items->length - 1 &&
                   !item_is_interactive((JavaObject*)items_data_sh[g_focused_item_index])) {
                g_focused_item_index++;
            }
        }
        
        FORM_DEBUG("[FORM_RENDER] items=%p, length=%d",
                (void*)items, items ? items->length : -1);
        
        if (items && items->element_type == DESC_OBJECT) {
            void** items_data = (void**)array_data(items);
            
            /* v34.65 FORM SCROLLING: tall Forms (Nescube "Display sets":
             * 3 groups + 2 gauges + 7 palettes) clip at max_y — a focus
             * that walks below the fold is INVISIBLE, which compounds the
             * "can't select checkboxes" feel. Items are drawn at
             * y - g_form_scroll inside a clip region (title/soft keys stay
             * fixed), and after the pass the scroll is adjusted so the
             * focused row is on screen, re-rendering once (depth-guarded). */
            int content_top = 21;
            y = 25 - g_form_scroll;
            /* v34.71 FIX (Nescube: "при перемещении вверх курсор уходит за
             * границу экрана"): focused_row_top used -1 as the "not found"
             * sentinel and the adjustment guard was `>= 0` — but a focused row
             * scrolled ABOVE the screen edge has a NEGATIVE y, so exactly when
             * the up-scroll needed to fire, the guard rejected it. Moving DOWN
             * always kept y positive (adjustment worked); moving UP froze the
             * scroll once the row crossed the top — the white cursor kept
             * walking off-screen (reproduced: "Base scale: 30%" focused at
             * y=-16, render loop kept drawing it clipped, scroll stuck).
             * Proper found-flag instead of the -1 sentinel. */
            int focused_row_top = 0, focused_row_bottom = 0;
            bool focused_row_found = false;
            midp_graphics_set_clip(gfx, 0, content_top, gfx->width,
                                   (max_y > content_top) ? (max_y - content_top) : 0);
            
            /* v34.65: render ALL items (no y<max_y stop): the focused row
             * MUST be measured even when it sits below the fold, or the
             * scroll adjustment below never triggers and the cursor walks
             * off-screen forever. The clip region hides the overflow. */
            for (int i = 0; i < items->length; i++) {
                JavaObject* item = (JavaObject*)items_data[i];
                if (!item) continue;
                
                bool focused = (i == g_focused_item_index);
                int item_type = get_item_type(item);
                int item_y0 = y;
                
                switch (item_type) {
                    case ITEM_STRINGITEM:
                        y = render_stringitem(jvm, gfx, item, y);
                        break;
                    case ITEM_IMAGEITEM:
                        y = render_imageitem(jvm, gfx, item, y);
                        break;
                    case ITEM_TEXTFIELD:
                        y = render_textfield(jvm, gfx, item, y, focused);
                        break;
                    case ITEM_CHOICEGROUP:
                        y = render_choicegroup(jvm, gfx, item, y, focused);
                        break;
                    case ITEM_GAUGE:
                        y = render_gauge(jvm, gfx, item, y, focused);
                        break;
                    case ITEM_SPACER:
                        y = render_spacer(jvm, gfx, item, y);
                        break;
                    default:
                        /* Unknown item type - skip */
                        y += 20;
                        break;
                }
                
                if (focused) {
                    focused_row_found = true;
                    focused_row_top = item_y0;
                    focused_row_bottom = y;
                    /* element granularity for groups: the cursor row is
                     * label + elem rows into the item */
                    if (item_type == ITEM_CHOICEGROUP) {
                        int label_slot = find_object_field_slot(item, "label");
                        int label_h = 0;
                        if (label_slot >= 0 && item->fields[label_slot].ref) {
                            label_h = midp_font_height(midp_font_get_default()) + 4;
                        }
                        int font_h = midp_font_height(midp_font_get_default());
                        focused_row_top = item_y0 + label_h +
                                          g_cg_focused_element * (font_h + 4) - 2;
                        focused_row_bottom = focused_row_top + font_h + 4;
                    }
                }
            }
            midp_graphics_set_clip(gfx, 0, 0, gfx->width, gfx->height);
            
            /* v36.29 [FORM-SCROLL]: remember the doc-space content bottom so
             * the UP/DOWN scroll fallback (midp_form_handle_key) can clamp.
             * y is the screen-space end of content; + g_form_scroll converts
             * it back to document space. */
            g_form_doc_h = y + g_form_scroll;
            
            /* v34.65: keep the focused row inside [content_top, max_y].
             *
             * v36.29 [FORM-SCROLL] FIX ("в формах не работает перемотка, я
             * всегда вижу только хвост текста"): the old adjust bottom-pinned
             * the FOCUSED row for EVERY form. Self-heal parks focus on the
             * LAST item whenever a form has no interactive items (help/about
             * screens — one long StringItem), so the view jumped straight to
             * the END of the text and stayed there (UP/DOWN could not move
             * focus, and scroll only followed focus). Two guards now:
             *   - a NON-INTERACTIVE focused row draws no cursor — it must not
             *     steer the view at all (initial view = top of the document);
             *   - a focused row TALLER than the viewport cannot be shown
             *     whole — bottom-pinning it hides its head, so leave the
             *     scroll to the manual fallback as well. */
            if (focused_row_found) {
                JavaObject* fitem = (items && items->element_type == DESC_OBJECT &&
                                     g_focused_item_index >= 0 &&
                                     g_focused_item_index < items->length)
                                    ? (JavaObject*)items_data[g_focused_item_index]
                                    : NULL;
                bool focus_visible = item_is_interactive(fitem);
                int view_h = max_y - content_top;
                int row_h = focused_row_bottom - focused_row_top;
                if (focus_visible && row_h <= view_h) {
                    int new_scroll = g_form_scroll;
                    if (focused_row_bottom > max_y) {
                        new_scroll += focused_row_bottom - max_y;
                    } else if (focused_row_top < content_top + 2 && g_form_scroll > 0) {
                        new_scroll -= content_top + 2 - focused_row_top;
                        if (new_scroll < 0) new_scroll = 0;
                    }
                    if (new_scroll != g_form_scroll) {
                        static int scroll_depth = 0;
                        g_form_scroll = new_scroll;
                        if (scroll_depth < 2) {
                            scroll_depth++;
                            midp_render_form(jvm, form);
                            scroll_depth--;
                            return;
                        }
                    }
                }
            }
        }
    }
    
    /* v34.29 FIX (Form command menu overlay): when the command menu is open
     * (left soft key with 3+ commands — MIDP standard), draw the overlay
     * list exactly like display.c does for Canvas screens. Previously the
     * menu "opened" invisibly on Forms: the state flipped, but render_form
     * redrew the plain form, so the user saw no feedback and JBenchmark3D
     * appeared frozen after pressing "Menu". */
    {
        extern bool midp_is_command_menu_open(void);
        extern int midp_command_menu_selected(void);
        if (midp_is_command_menu_open()) {
            int commands_idx0 = find_object_field_slot(form, "commands");
            JavaArray* commands0 = (commands_idx0 >= 0)
                ? (JavaArray*)form->fields[commands_idx0].ref : NULL;
            int cmd_count0 = (commands0 && is_heap_ptr_check(commands0))
                             ? commands0->length : 0;
            if (cmd_count0 > 2) {
                void** cmd_data0 = (void**)array_data(commands0);
                int sel = midp_command_menu_selected();

                /* Dim background + centered menu box */
                midp_graphics_set_color(gfx, 0x000000, 128);
                midp_graphics_fill_rect(gfx, 0, 0, gfx->width, gfx->height);
                int menu_w = gfx->width - 20;
                int item_h = 20;
                int menu_h = cmd_count0 * item_h + 10;
                int menu_x = 10;
                int menu_y = (gfx->height - menu_h) / 2;
                midp_graphics_set_color(gfx, 0xFFFFFF, 255);
                midp_graphics_fill_rect(gfx, menu_x, menu_y, menu_w, menu_h);
                midp_graphics_set_color(gfx, 0x000000, 255);
                midp_graphics_draw_rect(gfx, menu_x, menu_y, menu_w, menu_h);

                for (int i = 0; i < cmd_count0; i++) {
                    JavaObject* cmd = (JavaObject*)cmd_data0[i];
                    if (!cmd || !OBJECT_HAS_FIELDS(cmd, 3)) continue;
                    JavaString* lbl = (JavaString*)cmd->fields[0].ref;
                    const char* text = lbl ? get_string_from_object(jvm, (JavaObject*)lbl) : "";
                    int item_y = menu_y + 5 + i * item_h;
                    if (i == sel) {
                        midp_graphics_set_color(gfx, MIDP_SEL_BG, 255);
                        midp_graphics_fill_rect(gfx, menu_x + 2, item_y, menu_w - 4, item_h - 2);
                        midp_graphics_set_color(gfx, 0xFFFFFF, 255);
                    } else {
                        midp_graphics_set_color(gfx, 0x000000, 255);
                    }
                    midp_graphics_draw_string(gfx, text, menu_x + 5, item_y + 2, 0);
                }
                return;  /* menu replaces the normal form + softkey bar */
            }
        }
    }

    /* v36.30 [ITEM-CMDS]: the item command menu overlay (3+ commands on the
     * focused item — same layout as the form command menu above). */
    if (form_item_menu_active_raw()) {
        int sel = g_item_menu_sel;

        midp_graphics_set_color(gfx, 0x000000, 128);
        midp_graphics_fill_rect(gfx, 0, 0, gfx->width, gfx->height);
        int menu_w = gfx->width - 20;
        int item_h = 20;
        int menu_h = g_item_menu_count * item_h + 10;
        int menu_x = 10;
        int menu_y = (gfx->height - menu_h) / 2;
        midp_graphics_set_color(gfx, 0xFFFFFF, 255);
        midp_graphics_fill_rect(gfx, menu_x, menu_y, menu_w, menu_h);
        midp_graphics_set_color(gfx, 0x000000, 255);
        midp_graphics_draw_rect(gfx, menu_x, menu_y, menu_w, menu_h);

        for (int i = 0; i < g_item_menu_count; i++) {
            const char* text = form_cmd_label_utf8(jvm, g_item_menu_cmds[i]);
            int item_y = menu_y + 5 + i * item_h;
            if (i == sel) {
                midp_graphics_set_color(gfx, MIDP_SEL_BG, 255);
                midp_graphics_fill_rect(gfx, menu_x + 2, item_y, menu_w - 4, item_h - 2);
                midp_graphics_set_color(gfx, 0xFFFFFF, 255);
            } else {
                midp_graphics_set_color(gfx, 0x000000, 255);
            }
            midp_graphics_draw_string(gfx, text ? text : "?", menu_x + 5, item_y + 2, 0);
        }
        return;  /* menu replaces the normal form + softkey bar */
    }

    /* Draw soft button area with actual command labels (only if keyboard not active) */
    if (!g_vkb.active) {
        midp_graphics_set_color(gfx, 0xC0C0C0, 255);
        midp_graphics_fill_rect(gfx, 0, gfx->height - 25, gfx->width, 25);
        midp_graphics_set_color(gfx, 0x000000, 255);
        midp_graphics_draw_line(gfx, 0, gfx->height - 25, gfx->width, gfx->height - 25);
        
        /* Get commands from displayable to show real labels */
        int commands_idx = find_object_field_slot(form, "commands");
        JavaArray* commands_fb = (commands_idx >= 0)
            ? (JavaArray*)form->fields[commands_idx].ref : NULL;
        int form_cmd_count_fb = (commands_fb && is_heap_ptr_check(commands_fb) &&
                                 ((uintptr_t)commands_fb > 0x10000))
                                ? commands_fb->length : 0;
        if (form_cmd_count_fb > 0) {
            JavaArray* commands = commands_fb;
            if (commands) {
                void** cmd_data = (void**)array_data(commands);
                int cmd_count = commands->length;
                
                /* v34.29 FIX: mirror the input-side soft-key semantics of
                 * get_soft_button_commands()/midp_handle_soft_button().
                 * With 3+ commands the LEFT soft key OPENS THE COMMAND MENU
                 * (MIDP standard) — but this renderer drew the first
                 * command's label there, so the on-screen label ("Start")
                 * promised a direct action while the press opened an
                 * invisible menu (the menu overlay is drawn by
                 * display.c's render_soft_buttons for Canvas screens, not
                 * by render_form). Show "Menu" to match what the key does;
                 * 1-2 commands keep direct labels. */
                int left_cmd_idx = 0;
                int right_cmd_idx = -1;
                
                /* Find BACK or EXIT command for right side */
                for (int ci = 0; ci < cmd_count; ci++) {
                    JavaObject* cmd = (JavaObject*)cmd_data[ci];
                    if (!cmd || !cmd->header.clazz) continue;
                    /* Command fields: label(0), type(1), priority(2) */
                    if (OBJECT_HAS_FIELDS(cmd, 3)) {
                        int cmd_type = cmd->fields[1].i;
                        if ((cmd_type == 2 || cmd_type == 7) && right_cmd_idx < 0) {
                            right_cmd_idx = ci;
                        }
                    }
                }
                
                /* If only 1 command, it goes left */
                if (cmd_count == 1) {
                    left_cmd_idx = 0;
                    right_cmd_idx = -1;
                } else if (right_cmd_idx >= 0 && cmd_count >= 2) {
                    /* Right has BACK/EXIT, left gets first non-right command */
                    left_cmd_idx = (right_cmd_idx == 0) ? 1 : 0;
                } else {
                    /* No BACK/EXIT found: first cmd left, second cmd right */
                    left_cmd_idx = 0;
                    right_cmd_idx = 1;
                }
                
                /* Left button label (v34.29: "Menu" when the key opens the
                 * command menu — 3+ commands) */
                {
                    const char* left_label = NULL;
                    if (cmd_count > 2) {
                        left_label = "Menu";
                    } else if (cmd_data[left_cmd_idx]) {
                        JavaObject* cmd = (JavaObject*)cmd_data[left_cmd_idx];
                        if (OBJECT_HAS_FIELDS(cmd, 3)) {
                            JavaString* label = (JavaString*)cmd->fields[0].ref;
                            if (label) {
                                left_label = string_utf8(jvm, label);
                            }
                        }
                    }
                    if (!left_label || !left_label[0]) {
                        left_label = (cmd_count > 0) ? "OK" : "Menu";
                    }
                    midp_graphics_draw_string(gfx, left_label, 5, gfx->height - 20, 0);
                }
                
                /* Right button label */
                if (right_cmd_idx >= 0 && cmd_data[right_cmd_idx]) {
                    JavaObject* cmd = (JavaObject*)cmd_data[right_cmd_idx];
                    if (OBJECT_HAS_FIELDS(cmd, 3)) {
                        JavaString* label = (JavaString*)cmd->fields[0].ref;
                        if (label) {
                            const char* text = string_utf8(jvm, label);
                            if (text && text[0]) {
                                int text_len = strlen(text);
                                int text_width = text_len * 6;
                                midp_graphics_draw_string(gfx, text,
                                    gfx->width - text_width - 5, gfx->height - 20, 0);
                            } else {
                                midp_graphics_draw_string(gfx, "Back", gfx->width - 40, gfx->height - 20, 0);
                            }
                        } else {
                            midp_graphics_draw_string(gfx, "Back", gfx->width - 40, gfx->height - 20, 0);
                        }
                    }
                }
            }
        } else {
            /* v36.30 [ITEM-CMDS]: у Form своих команд нет — показываем
             * команды СФОКУСИРОВАННОГО item (info-мидлеты вешают Exit/Copy
             * на StringItem'ы). Семантика подписи зеркалит
             * midp_form_item_soft_button(): 1-2 команды — прямые надписи,
             * 3+ — "Menu" слева. */
            JavaObject* icmds[8];
            int in = midp_form_focused_item_commands(icmds, 8);
            if (in > 0) {
                const char* left_label = (in > 2) ? "Menu"
                                       : form_cmd_label_utf8(jvm, icmds[0]);
                if (!left_label || !left_label[0]) left_label = "OK";
                midp_graphics_draw_string(gfx, left_label, 5, gfx->height - 20, 0);
                if (in >= 2) {
                    const char* rl = form_cmd_label_utf8(jvm, icmds[1]);
                    if (rl && rl[0]) {
                        int rw = strlen(rl) * 6;
                        midp_graphics_draw_string(gfx, rl,
                            gfx->width - rw - 5, gfx->height - 20, 0);
                    }
                }
            }
        }
    }
    
    /* Render virtual keyboard if active */
    vkb_render(gfx);
    
    /* ИСПРАВЛЕНО: Запрашиваем перерисовку вместо прямого present */
    sdl_request_redraw();
}

/* Public function: Render Alert */
void midp_render_alert(JVM* jvm, MidpGraphics* gfx, JavaObject* alert) {
    if (!alert || !gfx) return;
    render_alert(jvm, gfx, alert);
}

/* Check if Alert timeout has expired and should be dismissed 
 * Returns true if Alert was dismissed, false otherwise */
bool midp_check_alert_timeout(JVM* jvm) {
    if (!g_current_displayable) return false;
    
    JavaClass* clazz = g_current_displayable->header.clazz;
    if (!clazz || !clazz->class_name) return false;
    
    /* Check if current displayable is an Alert */
    bool is_alert = false;
    JavaClass* check = clazz;
    while (check) {
        if (check->class_name && strcmp(check->class_name, "javax/microedition/lcdui/Alert") == 0) {
            is_alert = true;
            break;
        }
        check = check->super_class;
    }
    
    if (!is_alert) return false;
    
    FORM_DEBUG("[ALERT] Checking timeout for Alert");
    
    /* Get timeout field from Alert — use dynamic slot lookup */
    int timeout_slot = find_object_field_slot(g_current_displayable, "timeout");
    if (timeout_slot < 0 || !OBJECT_HAS_FIELDS(g_current_displayable, timeout_slot + 1)) return false;
    
    jint timeout = g_current_displayable->fields[timeout_slot].i;
    
    FORM_DEBUG("[ALERT] timeout value: %d ms", timeout);
    
    /* FOREVER = -1, meaning never timeout */
    if (timeout == -1) {
        FORM_DEBUG("[ALERT] timeout is FOREVER, not dismissing");
        return false;
    }
    
    /* Default timeout is usually 2000ms if not set */
    if (timeout <= 0) {
        timeout = 2000;  /* Default 2 seconds */
    }
    
    /* Get alert start time - we need to track this */
    static uint64_t alert_start_time = 0;
    static JavaObject* last_alert = NULL;
    
    /* Check if this is a new alert */
    if (last_alert != g_current_displayable) {
        last_alert = g_current_displayable;
        SdlContext* sdl_ctx = sdl_get_global_context();
        alert_start_time = sdl_ctx ? sdl_get_ticks(sdl_ctx) : 0;
        FORM_DEBUG("[ALERT] New Alert displayed, start_time=%llu", (unsigned long long)alert_start_time);
        return false;
    }
    
    /* Check if timeout has expired */
    SdlContext* sdl_ctx = sdl_get_global_context();
    if (!sdl_ctx) return false;
    
    uint64_t now = sdl_get_ticks(sdl_ctx);
    uint64_t elapsed = now - alert_start_time;
    
    FORM_DEBUG("[ALERT] elapsed=%llu ms, timeout=%d ms", (unsigned long long)elapsed, timeout);
    
    if (elapsed >= (uint64_t)timeout) {
        FORM_DEBUG("[ALERT] Timeout expired! Calling commandAction with DISMISS");
        
        /* Call commandAction with DISMISS command if listener exists */
        /* Find listener field in Alert */
        int listener_slot = find_object_field_slot(g_current_displayable, "listener");
        if (listener_slot >= 0) {
            JavaObject* listener = (JavaObject*)g_current_displayable->fields[listener_slot].ref;
            if (listener && listener->header.clazz) {
                /* Create a DISMISS command */
                JavaClass* cmd_class = jvm_load_class(jvm, "javax/microedition/lcdui/Command");
                if (cmd_class) {
                    JavaObject* dismiss_cmd = jvm_new_object(jvm, cmd_class);
                    if (dismiss_cmd && OBJECT_HAS_FIELDS(dismiss_cmd, 3)) {
                        JavaString* label = jvm_new_string(jvm, "Dismiss");
                        dismiss_cmd->fields[0].ref = label;  /* label */
                        dismiss_cmd->fields[1].i = 3;        /* type = DISMISS = 3 */
                        dismiss_cmd->fields[2].i = 0;        /* priority */
                        
                        /* Find and call commandAction(Command, Displayable) */
                        JavaClass* listener_class = listener->header.clazz;
                        JavaMethod* method = jvm_resolve_method(jvm, listener_class, "commandAction",
                            "(Ljavax/microedition/lcdui/Command;Ljavax/microedition/lcdui/Displayable;)V");
                        
                        if (method) {
                            extern int execute_method(JVM* jvm, JavaThread* thread, JavaMethod* method, 
                                                      JavaValue* args, JavaValue* result);
                            JavaValue args[3];
                            args[0].ref = listener;        /* this */
                            args[1].ref = dismiss_cmd;     /* Command */
                            args[2].ref = g_current_displayable;  /* Displayable */
                            
                            JavaValue result;
                            JavaThread* thread = jvm_current_thread(jvm);
                            execute_method(jvm, thread, method, args, &result);
                            FORM_DEBUG("[ALERT] commandAction called successfully");
                        } else {
                            WARN_LOG("[ALERT] commandAction method not found in %s",
                                    listener_class->class_name ? listener_class->class_name : "?");
                        }
                    }
                }
            } else {
                WARN_LOG("[ALERT] No listener set, cannot call commandAction");
            }
        }
        
        last_alert = NULL;

        /* v34.65 (Alert won't close): every CALLER of this function ignored
         * the true return, and the DISMISS notification only helped when
         * the game's own listener switched screens — FPC's listener does
         * nothing, so the Alert stayed on screen forever. Real AMS
         * semantics: after the timeout the Alert is REPLACED by the next
         * Displayable (setCurrent(alert, next)) or, absent one, by the
         * screen that was current before the Alert was shown. Only act if
         * the listener left the Alert on screen. */
        {
            extern void midp_alert_restore_next_displayable(JVM* jvm, JavaObject* alert);
            if (g_current_displayable && g_current_displayable->header.clazz) {
                /* is_alert was computed for the entry-time displayable; the
                 * listener may have already setCurrent()'d elsewhere — recheck
                 * before restoring. */
                JavaClass* now_clazz = g_current_displayable->header.clazz;
                JavaClass* now = now_clazz;
                bool still_this_alert = false;
                while (now) {
                    if (now->class_name && strcmp(now->class_name, "javax/microedition/lcdui/Alert") == 0) {
                        still_this_alert = true;
                        break;
                    }
                    now = now->super_class;
                }
                if (still_this_alert) {
                    midp_alert_restore_next_displayable(jvm, g_current_displayable);
                }
            }
        }
        return true;  /* Signal that alert should be dismissed */
    }
    
    return false;
}

/* Public function: Render List */
void midp_render_list(JVM* jvm, JavaObject* list, int focused_index) {
    if (!list) return;
    
    g_current_displayable = list;  /* Track current displayable */
    
    MidpGraphics* gfx = get_screen_graphics();
    if (!gfx) return;
    
    /* Clear screen */
    midp_graphics_set_color(gfx, 0xFFFFFF, 255);
    midp_graphics_fill_rect(gfx, 0, 0, gfx->width, gfx->height);
    
    render_list(jvm, gfx, list, focused_index);
    
    /* ИСПРАВЛЕНО: Запрашиваем перерисовку вместо прямого present */
    sdl_request_redraw();
}

/* Public function: Render TextBox */
void midp_render_textbox(JVM* jvm, JavaObject* textbox, const char* input_text, int cursor_pos) {
    if (!textbox) return;
    
    g_current_displayable = textbox;  /* Track current displayable */
    
    MidpGraphics* gfx = get_screen_graphics();
    if (!gfx) return;
    
    /* Clear screen */
    midp_graphics_set_color(gfx, 0xFFFFFF, 255);
    midp_graphics_fill_rect(gfx, 0, 0, gfx->width, gfx->height);
    
    render_textbox(jvm, gfx, textbox, input_text, cursor_pos);
    
    /* Render virtual keyboard if active */
    vkb_render(gfx);
    
    /* ИСПРАВЛЕНО: Запрашиваем перерисовку вместо прямого present */
    sdl_request_redraw();
}

/* ============================================
 * FORM NAVIGATION AND INPUT HANDLING
 * ============================================ */

/* Handle key press for current displayable (forms, lists, etc.) */
/* List/Choice per-object extras: custom select command, default command and
 * fit policy. Side table (same approach as ItemExtra) - no stub changes. */
#define LIST_EXTRA_MAX 32
typedef struct {
    JavaObject* list;
    JavaObject* select_command;   /* List.setSelectCommand */
    JavaObject* default_command;  /* List.setDefaultCommand */
    int fit_policy;               /* List.setFitPolicy */
} ListExtra;
static ListExtra g_list_extra[LIST_EXTRA_MAX];
static int g_list_extra_count = 0;

static ListExtra* list_extra_get(JavaObject* list, int create) {
    if (!list) return NULL;
    for (int i = 0; i < g_list_extra_count; i++) {
        if (g_list_extra[i].list == list) return &g_list_extra[i];
    }
    if (!create || g_list_extra_count >= LIST_EXTRA_MAX) return NULL;
    ListExtra* e = &g_list_extra[g_list_extra_count++];
    memset(e, 0, sizeof(*e));
    e->list = list;
    return e;
}

/* v34.65: number of choice elements in a ChoiceGroup item (strings[] is
 * the element-count authority; selected[] may be shorter on stubbed
 * objects). 0 for non-groups. */
static int cg_element_count(JavaObject* item) {
    if (!item) return 0;
    int strings_slot = find_object_field_slot(item, "strings");
    if (strings_slot < 0 || !OBJECT_HAS_FIELDS(item, strings_slot + 1)) return 0;
    JavaArray* strings = (JavaArray*)item->fields[strings_slot].ref;
    return strings ? strings->length : 0;
}

/* v34.65: items that can hold focus. Labels (StringItem/ImageItem/Spacer)
 * never take focus — an invisible focus on a label row is indistinguishable
 * from a dead UI (and matches Nokia traversal semantics). */
static bool item_is_interactive(JavaObject* item) {
    if (!item) return false;
    int t = get_item_type(item);
    if (t == ITEM_TEXTFIELD) return true;
    if (t == ITEM_CHOICEGROUP) return cg_element_count(item) > 0;
    if (t == ITEM_GAUGE) {
        int slot = find_object_field_slot(item, "interactive");
        if (slot < 0 || !OBJECT_HAS_FIELDS(item, slot + 1)) return false;
        return item->fields[slot].i != 0;
    }
    /* v36.30 [ITEM-CMDS]: an item carrying Item commands (StringItem
     * "buttons" of info-midlets) IS a traversal destination — arrows must
     * be able to focus it, or its soft-key labels/FIRE would never apply
     * (field report: «софтовые кнопки не работают» — focus was stuck on a
     * command-less item while the actions lived on StringItems). */
    {
        JavaObject* cbuf[8];
        if (form_item_cmd_effective(item, cbuf, 8) > 0) return true;
    }
    return false;
}

/* v36.29 [FORM-SCROLL]: manual pixel scroll for tall Forms (UP/DOWN when
 * item traversal has nowhere to go). Real phones scroll the FORM itself in
 * that situation; ours bottom-pinned the focused row and froze there
 * ("не работает перемотка, вижу только хвост текста"). Scrolling is gated
 * on real overflow (doc taller than the viewport) so short forms still let
 * the keys fall through to the midlet. Returns true when the view moved. */
static bool form_scroll_view(JVM* jvm, int dir) {
    if (g_form_doc_h <= 0) return false;
    int screen_w = 0, screen_h = 0;
    midp_display_get_dimensions(&screen_w, &screen_h);
    (void)screen_w;
    int max_y = g_vkb.active ? (screen_h - 170) : (screen_h - 30);
    int max_scroll = g_form_doc_h - max_y;
    if (max_scroll <= 0) return false;   /* content fits — midlet owns the key */

    int step = 3 * (midp_font_height(midp_font_get_default()) + 2); /* 3 lines */
    int ns = g_form_scroll + dir * step;
    if (ns > max_scroll) ns = max_scroll;
    if (ns < 0) ns = 0;
    if (ns == g_form_scroll) return false;

    LOG_SAFE("[FORM] scroll %d -> %d (doc_h=%d, dir=%+d)",
             g_form_scroll, ns, g_form_doc_h, dir);
    g_form_scroll = ns;
    if (g_current_displayable) {
        midp_render_form(jvm, g_current_displayable);
    }
    return true;
}

bool midp_form_handle_key(JVM* jvm, int game_action) {
    /* If virtual keyboard is active, send keys to it */
    if (g_vkb.active) {
        return vkb_handle_key(jvm, game_action);
    }
    
    /* Handle based on current displayable type */
    if (!g_current_displayable) {
        FORM_DEBUG("midp_form_handle_key: g_current_displayable is NULL!");
        return false;
    }
    
    JavaClass* clazz = g_current_displayable->header.clazz;
    if (!clazz || !clazz->class_name) {
        FORM_DEBUG("midp_form_handle_key: invalid class!");
        return false;
    }
    
    FORM_DEBUG("midp_form_handle_key: game_action=%d, class=%s", game_action, clazz->class_name);

    /* v34.27: hierarchy dispatch (see the block comment near
     * midp_displayable_kind). This fixes List subclasses whose names
     * contain "Form" (glomoRegForms/ActivateForm) and Form subclasses
     * whose names contain neither word. */
    int ui_kind = midp_displayable_kind(g_current_displayable);

    if (ui_kind == MIDP_UI_KIND_FORM) {
        /* Get items array - use dynamic slot lookup */
        JavaArray* items = NULL;

        int items_slot = find_object_field_slot(g_current_displayable, "items");
        if (items_slot >= 0 && OBJECT_HAS_FIELDS(g_current_displayable, items_slot + 1)) {
            items = (JavaArray*)g_current_displayable->fields[items_slot].ref;
        }
        int item_count = items ? items->length : 0;

        /* v34.27: clamp the focus index - delete/deleteAll could shrink the
         * items array under a stale index (FIRE then read past the array,
         * DOWN went dead until enough UP presses). */
        if (g_focused_item_index >= item_count) g_focused_item_index = item_count - 1;
        if (g_focused_item_index < 0) g_focused_item_index = 0;

        JavaObject* item = NULL;
        int item_type = 0;
        void** items_data = NULL;
        if (items && items->element_type == DESC_OBJECT && item_count > 0) {
            items_data = (void**)array_data(items);
            item = (JavaObject*)items_data[g_focused_item_index];
            item_type = get_item_type(item);
        }

        switch (game_action) {
            case 1:  /* UP */
                /* v34.65: Nokia-style intra-group traversal — within a
                 * focused ChoiceGroup, UP walks the choice ELEMENTS (the
                 * visible cursor row) first, leaving the group only from
                 * element 0. Together with the rendered element cursor
                 * this fixes "checkboxes render but can't be selected"
                 * (Nescube settings): v34.27's UP/DOWN jumped whole
                 * groups while FIRE acted on an invisible element. */
                if (item_type == ITEM_CHOICEGROUP && g_cg_focused_element > 0) {
                    g_cg_focused_element--;
                    midp_render_form(jvm, g_current_displayable);
                    return true;
                }
                if (items_data) {
                    int target = g_focused_item_index - 1;
                    while (target >= 0 && !item_is_interactive((JavaObject*)items_data[target])) {
                        target--;
                    }
                    if (target >= 0) {
                        g_focused_item_index = target;
                        /* entering a ChoiceGroup from below: focus its LAST element */
                        if (get_item_type((JavaObject*)items_data[target]) == ITEM_CHOICEGROUP) {
                            int n = cg_element_count((JavaObject*)items_data[target]);
                            g_cg_focused_element = (n > 0) ? n - 1 : 0;
                        }
                        midp_render_form(jvm, g_current_displayable);
                        return true;
                    }
                }
                /* v36.29 [FORM-SCROLL]: traversal exhausted (first item / no
                 * interactive items above) — scroll the view up instead of
                 * dropping the key (tall help/about Forms must scroll). */
                if (form_scroll_view(jvm, -1)) return true;
                break;

            case 6:  /* DOWN */
                if (item_type == ITEM_CHOICEGROUP) {
                    int n = cg_element_count(item);
                    if (g_cg_focused_element < n - 1) {
                        g_cg_focused_element++;
                        midp_render_form(jvm, g_current_displayable);
                        return true;
                    }
                }
                if (items_data) {
                    int target = g_focused_item_index + 1;
                    while (target < item_count && !item_is_interactive((JavaObject*)items_data[target])) {
                        target++;
                    }
                    if (target < item_count) {
                        g_focused_item_index = target;
                        /* entering a ChoiceGroup from above: focus element 0 */
                        if (get_item_type((JavaObject*)items_data[target]) == ITEM_CHOICEGROUP) {
                            g_cg_focused_element = 0;
                        }
                        midp_render_form(jvm, g_current_displayable);
                        return true;
                    }
                }
                /* v36.29 [FORM-SCROLL]: traversal exhausted (last item / no
                 * interactive items below) — scroll the view down to reveal
                 * the tail of the content instead of dropping the key. */
                if (form_scroll_view(jvm, +1)) return true;
                break;

            case 2:  /* LEFT - v34.27: intra-item horizontal traversal
             * (MIDP: arrows move focus between elements of interactive
             * items). ChoiceGroup: move element focus; Gauge: decrement. */
                if (item_type == ITEM_CHOICEGROUP) {
                    int sel_count = 0;
                    int selected_slot = find_object_field_slot(item, "selected");
                    if (selected_slot >= 0 && OBJECT_HAS_FIELDS(item, selected_slot + 1)) {
                        JavaArray* selected = (JavaArray*)item->fields[selected_slot].ref;
                        sel_count = selected ? selected->length : 0;
                    }
                    if (sel_count > 0) {
                        g_cg_focused_element = (g_cg_focused_element > 0)
                            ? g_cg_focused_element - 1 : sel_count - 1;
                        midp_render_form(jvm, g_current_displayable);
                        return true;
                    }
                } else if (item_type == ITEM_GAUGE) {
                    int value_slot = find_object_field_slot(item, "value");
                    if (value_slot >= 0 && OBJECT_HAS_FIELDS(item, value_slot + 1) &&
                        item->fields[value_slot].i > 0) {
                        item->fields[value_slot].i--;
                        form_notify_item_state_changed(jvm, item);
                        midp_render_form(jvm, g_current_displayable);
                        return true;
                    }
                }
                break;

            case 5:  /* RIGHT - v34.27: intra-item traversal, see LEFT */
                if (item_type == ITEM_CHOICEGROUP) {
                    int sel_count = 0;
                    int selected_slot = find_object_field_slot(item, "selected");
                    if (selected_slot >= 0 && OBJECT_HAS_FIELDS(item, selected_slot + 1)) {
                        JavaArray* selected = (JavaArray*)item->fields[selected_slot].ref;
                        sel_count = selected ? selected->length : 0;
                    }
                    if (sel_count > 0) {
                        g_cg_focused_element = (g_cg_focused_element + 1) % sel_count;
                        midp_render_form(jvm, g_current_displayable);
                        return true;
                    }
                } else if (item_type == ITEM_GAUGE) {
                    int maxvalue_slot = find_object_field_slot(item, "maxValue");
                    int value_slot = find_object_field_slot(item, "value");
                    if (maxvalue_slot >= 0 && value_slot >= 0 &&
                        OBJECT_HAS_FIELDS(item, value_slot + 1) &&
                        item->fields[value_slot].i < item->fields[maxvalue_slot].i) {
                        item->fields[value_slot].i++;
                        form_notify_item_state_changed(jvm, item);
                        midp_render_form(jvm, g_current_displayable);
                        return true;
                    }
                }
                break;

            case 8:  /* FIRE - activate focused item */
                if (item) {
                    /* v36.30 [ITEM-CMDS]: a non-interactive item (StringItem
                     * etc.) with item commands fires its default/first
                     * command on FIRE — real-phone select semantics. The
                     * interactive types below keep their own FIRE actions
                     * (their commands stay reachable via soft keys/touch). */
                    if (item_type != ITEM_TEXTFIELD &&
                        item_type != ITEM_CHOICEGROUP &&
                        item_type != ITEM_GAUGE) {
                        JavaObject* fcmds[8];
                        int fn = form_item_cmd_effective(item, fcmds, 8);
                        if (fn > 0) {
                            form_item_fire_command(jvm, item, fcmds[0]);
                            return true;
                        }
                    }
                    if (item_type == ITEM_TEXTFIELD) {
                        /* Start virtual keyboard for TextField */
                        int max_size = 32;
                        int constraints = 0;

                        int maxsize_slot = find_object_field_slot(item, "maxSize");
                        int constraints_slot = find_object_field_slot(item, "constraints");
                        if (maxsize_slot >= 0 && constraints_slot >= 0 &&
                            OBJECT_HAS_FIELDS(item, constraints_slot + 1)) {
                            max_size = item->fields[maxsize_slot].i;
                            constraints = item->fields[constraints_slot].i;
                        }

                        vkb_start(item, max_size, constraints);
                        midp_render_form(jvm, g_current_displayable);
                        return true;
                    }
                    else if (item_type == ITEM_CHOICEGROUP) {
                        /* v34.27: act on the FOCUSED element (real-phone
                         * semantics). The old code cycled EXCLUSIVE groups
                         * forward on every FIRE and toggled+auto-advanced
                         * MULTIPLE groups, ignoring element focus. */
                        int choicetype_slot = find_object_field_slot(item, "choiceType");
                        int selected_slot = find_object_field_slot(item, "selected");
                        if (choicetype_slot >= 0 && selected_slot >= 0 &&
                            OBJECT_HAS_FIELDS(item, selected_slot + 1)) {
                            int choice_type = item->fields[choicetype_slot].i;
                            JavaArray* selected = (JavaArray*)item->fields[selected_slot].ref;
                            int sel_count = selected ? selected->length : 0;

                            if (selected && sel_count > 0) {
                                jboolean* sel_data = (jboolean*)array_data(selected);
                                int elem = g_cg_focused_element % sel_count;

                                if (choice_type == CHOICE_EXCLUSIVE || choice_type == CHOICE_POPUP) {
                                    /* Select the focused element (clear others) */
                                    for (int si = 0; si < sel_count; si++) {
                                        sel_data[si] = (si == elem) ? 1 : 0;
                                    }
                                } else {
                                    /* MULTIPLE: toggle the focused element */
                                    sel_data[elem] = !sel_data[elem];
                                }

                                /* Notify ItemStateListener */
                                if (g_item_state_listener && g_item_state_listener->header.clazz) {
                                    JavaMethod* ism = jvm_resolve_method(jvm,
                                        g_item_state_listener->header.clazz,
                                        "itemStateChanged", "(Ljavax/microedition/lcdui/Item;)V");
                                    if (ism) {
                                        JavaValue ism_args[2];
                                        ism_args[0].ref = g_item_state_listener;
                                        ism_args[1].ref = item;
                                        JavaValue ism_result;
                                        execute_method(jvm, jvm_current_thread(jvm), ism, ism_args, &ism_result);
                                    }
                                }

                                midp_render_form(jvm, g_current_displayable);
                                return true;
                            }
                        }
                    }
                    else if (item_type == ITEM_GAUGE) {
                        /* Increment gauge value */
                        int maxvalue_slot = find_object_field_slot(item, "maxValue");
                        int value_slot = find_object_field_slot(item, "value");
                        if (maxvalue_slot >= 0 && value_slot >= 0 &&
                            OBJECT_HAS_FIELDS(item, value_slot + 1)) {
                            int max_val = item->fields[maxvalue_slot].i;
                            int cur_val = item->fields[value_slot].i;
                            if (max_val > 0) {
                                cur_val = (cur_val + 1) % (max_val + 1);
                                item->fields[value_slot].i = cur_val;

                                /* Notify ItemStateListener (v18: coalescing) */
                                form_notify_item_state_changed(jvm, item);

                                midp_render_form(jvm, g_current_displayable);
                                return true;
                            }
                        }
                    }
                }
                break;
        }
    }
    /* Check if it's a List */
    else if (ui_kind == MIDP_UI_KIND_LIST) {
        int list_size = 0;
        
        /* List fields — use dynamic slot lookup */
        int strings_slot = find_object_field_slot(g_current_displayable, "strings");
        if (strings_slot >= 0 && OBJECT_HAS_FIELDS(g_current_displayable, strings_slot + 1)) {
            JavaArray* strings = (JavaArray*)g_current_displayable->fields[strings_slot].ref;
            FORM_DEBUG("strings array: %p, element_type=%d", 
                    (void*)strings, strings ? strings->element_type : -1);
            if (strings) list_size = strings->length;
        } else {
            FORM_DEBUG("List field lookup failed for 'strings'!");
        }
        
        FORM_DEBUG("List navigation: game_action=%d, current_index=%d, list_size=%d",
                game_action, g_list_selected_index, list_size);
        
        switch (game_action) {
            case 1:  /* UP */
                if (g_list_selected_index > 0) {
                    g_list_selected_index--;
                    FORM_DEBUG("UP: new index=%d", g_list_selected_index);
                    midp_render_list(jvm, g_current_displayable, g_list_selected_index);
                    return true;
                }
                break;
                
            case 6:  /* DOWN */
                if (g_list_selected_index < list_size - 1) {
                    g_list_selected_index++;
                    FORM_DEBUG("DOWN: new index=%d", g_list_selected_index);
                    midp_render_list(jvm, g_current_displayable, g_list_selected_index);
                    return true;
                }
                break;
                
            case 8:  /* FIRE - select item */
                {
                    FORM_DEBUG("FIRE: selecting item %d", g_list_selected_index);
                    
                    /* Get listener using dynamic field offset lookup */
                    JavaObject* listener = NULL;
                    int listener_idx = get_field_offset(g_current_displayable->header.clazz, "listener");
                    if (listener_idx < 0) {
                        listener_idx = get_field_offset(g_current_displayable->header.clazz, "commandListener");
                    }
                    
                    if (listener_idx >= 0 && OBJECT_HAS_FIELDS(g_current_displayable, listener_idx + 1)) {
                        listener = (JavaObject*)g_current_displayable->fields[listener_idx].ref;
                    }
                    
                    FORM_DEBUG("Listener: %p (field idx: %d)", (void*)listener, listener_idx);
                    
                    if (listener) {
                        /* Use the global SELECT_COMMAND singleton */
                        ensure_select_command(jvm);

                        /* v35: honor List.setSelectCommand if the game set one */
                        JavaObject* sel_cmd = g_select_command;
                        {
                            ListExtra* le = list_extra_get(g_current_displayable, 0);
                            if (le && le->select_command) sel_cmd = le->select_command;
                        }

                        FORM_DEBUG("Using SELECT_COMMAND: %p", (void*)sel_cmd);

                        /* Call listener.commandAction(cmd, displayable) */
                        if (sel_cmd) {
                            JavaClass* listener_class = listener->header.clazz;
                            JavaMethod* method = jvm_resolve_method(jvm, listener_class, "commandAction", 
                                "(Ljavax/microedition/lcdui/Command;Ljavax/microedition/lcdui/Displayable;)V");
                            
                            FORM_DEBUG("commandAction method: %p", (void*)method);
                            
                            if (method) {
                                JavaValue args[3];
                                args[0].ref = listener;                   /* this */
                                args[1].ref = sel_cmd;                    /* SELECT_COMMAND (v35) */
                                args[2].ref = g_current_displayable;      /* Displayable */
                                
                                JavaValue result;
                                JavaThread* thread = jvm_current_thread(jvm);
                                
                                FORM_DEBUG("Calling commandAction(SELECT_COMMAND, List)");
                                execute_method(jvm, thread, method, args, &result);
                            } else {
                                FORM_DEBUG("commandAction method not found!");
                            }
                        }
                    } else {
                        /* Fallback: Try to call commandAction through display.c's helper */
                        FORM_DEBUG("No listener field found, trying call_command_action");
                        extern bool call_command_action_for_list(JVM* jvm, JavaObject* list, int selected_index);
                        if (!call_command_action_for_list(jvm, g_current_displayable, g_list_selected_index)) {
                            FORM_DEBUG("call_command_action_for_list also failed!");
                        }
                    }
                }
                return true;
        }
    }
    /* Check if it's a TextBox */
    else if (ui_kind == MIDP_UI_KIND_TEXTBOX) {
        if (game_action == 8) {  /* FIRE */
            /* Start virtual keyboard for TextBox */
            int max_size = 32;
            int constraints = 0;
            
            int maxsize_slot = find_object_field_slot(g_current_displayable, "maxSize");
            int constraints_slot = find_object_field_slot(g_current_displayable, "constraints");
            if (maxsize_slot >= 0 && constraints_slot >= 0 &&
                OBJECT_HAS_FIELDS(g_current_displayable, constraints_slot + 1)) {
                max_size = g_current_displayable->fields[maxsize_slot].i;
                constraints = g_current_displayable->fields[constraints_slot].i;
            }
            
            vkb_start(g_current_displayable, max_size, constraints);
            midp_render_textbox(jvm, g_current_displayable, g_vkb.text_buffer, g_vkb.cursor_pos);
            return true;
        }
    }
    /* v34.27: Alert branch - NEW. Alerts used to fall through every branch
     * (the old strstr dispatch had no "Alert" case), so no key ever did
     * anything on an Alert: a game stuck on an Alert with FOREVER timeout
     * and no timeout-path listener had no way forward. MIDP: user
     * interaction dismisses an Alert (the DISMISS command is delivered to
     * the listener; with no listener we return to the screen saved by
     * setCurrent(Alert, next)). */
    else if (ui_kind == MIDP_UI_KIND_ALERT) {
        /* v34.27: FIRE or any game key dismisses (soft keys with Commands
         * are handled earlier by midp_handle_soft_button, so reaching here
         * means the Alert has no mapped command for this key). */
        if (game_action == 1 || game_action == 2 || game_action == 5 ||
            game_action == 6 || game_action == 8) {
            JavaObject* alert = g_current_displayable;
            bool delivered = false;

            /* Deliver DISMISS to the CommandListener, if any (same shape as
             * the timeout path in midp_check_alert_timeout). */
            int listener_slot = find_object_field_slot(alert, "listener");
            if (listener_slot >= 0 && OBJECT_HAS_FIELDS(alert, listener_slot + 1)) {
                JavaObject* listener = (JavaObject*)alert->fields[listener_slot].ref;
                if (listener && listener->header.clazz) {
                    JavaClass* cmd_class =
                        jvm_load_class(jvm, "javax/microedition/lcdui/Command");
                    if (cmd_class) {
                        JavaObject* dismiss_cmd = jvm_new_object(jvm, cmd_class);
                        if (dismiss_cmd && OBJECT_HAS_FIELDS(dismiss_cmd, 3)) {
                            JavaString* label = jvm_new_string(jvm, "Dismiss");
                            dismiss_cmd->fields[0].ref = label;  /* label */
                            dismiss_cmd->fields[1].i = 3;        /* type = DISMISS */
                            dismiss_cmd->fields[2].i = 0;        /* priority */

                            JavaMethod* method = jvm_resolve_method(
                                jvm, listener->header.clazz, "commandAction",
                                "(Ljavax/microedition/lcdui/Command;"
                                "Ljavax/microedition/lcdui/Displayable;)V");
                            if (method) {
                                JavaValue cargs[3];
                                cargs[0].ref = listener;
                                cargs[1].ref = dismiss_cmd;
                                cargs[2].ref = alert;
                                JavaValue cresult;
                                execute_method(jvm, jvm_current_thread(jvm),
                                               method, cargs, &cresult);
                                delivered = true;
                            }
                        }
                    }
                }
            }

            /* Return to the screen saved by setCurrent(Alert, next) unless
             * the listener already switched somewhere itself. */
            extern void midp_alert_restore_next_displayable(JVM* jvm, JavaObject* alert);
            midp_alert_restore_next_displayable(jvm, alert);

            FORM_DEBUG("[ALERT] dismissed by key (game_action=%d, listener_notified=%d)",
                       game_action, delivered ? 1 : 0);
            (void)delivered;
            return true;
        }
    }
    
    return false;  /* Key not handled */
}

/* Ensure SELECT_COMMAND singleton is created */
static void ensure_select_command(JVM* jvm) {
    if (g_select_command) return;
    
    JavaClass* cmd_class = jvm_load_class(jvm, "javax/microedition/lcdui/Command");
    if (!cmd_class) return;
    
    g_select_command = jvm_new_object(jvm, cmd_class);
    if (g_select_command && OBJECT_HAS_FIELDS(g_select_command, 3)) {
        /* Command(label, type=4 SCREEN, priority=0) */
        JavaString* label = jvm_new_string(jvm, "Select");
        g_select_command->fields[0].ref = label;     /* label */
        g_select_command->fields[1].i = 4;          /* SCREEN type */
        g_select_command->fields[2].i = 0;          /* priority */
    }
    
    /* Also set the static field in List class */
    JavaClass* list_class = jvm_load_class(jvm, "javax/microedition/lcdui/List");
    if (list_class && list_class->static_fields_count > 0) {
        list_class->static_fields[0].value.ref = g_select_command;
    }
    
    FORM_DEBUG("Created SELECT_COMMAND singleton: %p", (void*)g_select_command);
}

/* Set current displayable */
void midp_set_current_displayable(JavaObject* displayable) {
    g_current_displayable = displayable;
    g_focused_item_index = 0;
    g_list_selected_index = 0;
    g_cg_focused_element = 0;   /* v34.65: element focus + scroll belong to
                                 * the PREVIOUS form — stale values put the
                                 * cursor at a random element of the first
                                 * group on the next screen */
    g_form_scroll = 0;
    g_form_doc_h = 0;
    g_vkb.active = false;
    /* v36.30 [ITEM-CMDS]: item command menu belongs to the previous screen */
    form_item_menu_close_raw();
}

/*
 * Additional native methods for better form support
 */

/* List.<init>(String, int) - constructor for List */
static JavaValue native_list_init(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* list = (JavaObject*)args[0].ref;
    JavaString* title = (JavaString*)args[1].ref;
    jint list_type = args[2].i;
    
    /* Create SELECT_COMMAND singleton on first List creation */
    ensure_select_command(jvm);
    
    if (!list) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Get field offsets considering inheritance */
    JavaClass* list_class = list->header.clazz;
    int title_idx = get_field_offset(list_class, "title");
    int listtype_idx = get_field_offset(list_class, "listType");
    int strings_idx = get_field_offset(list_class, "strings");
    int selected_idx = get_field_offset(list_class, "selected");
    
    /* Initialize List fields: title, listType, strings[], selected[] */
    if (title_idx >= 0 && listtype_idx >= 0 && strings_idx >= 0 && selected_idx >= 0) {
        list->fields[title_idx].ref = title;
        list->fields[listtype_idx].i = list_type;
        
        /* Create empty arrays for strings and selection */
        JavaArray* strings = jvm_new_array(jvm, DESC_OBJECT, 0, NULL);
        list->fields[strings_idx].ref = strings;
        
        /* Create empty boolean array for selection */
        JavaArray* selected = jvm_new_array(jvm, T_BOOLEAN, 0, NULL);
        list->fields[selected_idx].ref = selected;
    }
    
    FORM_DEBUG("<init>: title=%s, type=%d",
            title ? get_string_from_object(jvm, (JavaObject*)title) : "(null)",
            list_type);
    
    return NATIVE_RETURN_VOID();
}

/* List.<init>(String, int, String[], Image[]) - constructor with initial elements */
static JavaValue native_list_init_with_elements(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* list = (JavaObject*)args[0].ref;
    JavaString* title = (JavaString*)args[1].ref;
    jint list_type = args[2].i;
    JavaArray* strings = (JavaArray*)args[3].ref;
    JavaArray* images = (JavaArray*)args[4].ref;
    
    /* Create SELECT_COMMAND singleton on first List creation */
    ensure_select_command(jvm);
    
    if (!list) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Get field offsets considering inheritance */
    JavaClass* list_class = list->header.clazz;
    int title_idx = get_field_offset(list_class, "title");
    int listtype_idx = get_field_offset(list_class, "listType");
    int strings_idx = get_field_offset(list_class, "strings");
    int selected_idx = get_field_offset(list_class, "selected");
    
    /* Initialize List fields: title, listType, strings[], selected[] */
    if (title_idx >= 0 && listtype_idx >= 0 && strings_idx >= 0 && selected_idx >= 0) {
        list->fields[title_idx].ref = title;
        list->fields[listtype_idx].i = list_type;
        list->fields[strings_idx].ref = strings;
        
        /* Create selected[] array with same length as strings */
        if (strings) {
            JavaArray* selected = jvm_new_array(jvm, T_BOOLEAN, strings->length, NULL);
            list->fields[selected_idx].ref = selected;
            
            /* For IMPLICIT, EXCLUSIVE, and POPUP lists, select first item by default */
            if (selected && strings->length > 0 && 
                (list_type == 1 || list_type == 3 || list_type == 4)) { /* EXCLUSIVE=1, POPUP=3, IMPLICIT=4 */
                jboolean* selected_data = (jboolean*)array_data(selected);
                selected_data[0] = 1;
                FORM_DEBUG("<init>: set initial selection at index 0 for type=%d", list_type);
            }
        } else {
            JavaArray* selected = jvm_new_array(jvm, T_BOOLEAN, 0, NULL);
            list->fields[selected_idx].ref = selected;
        }
    }
    
    (void)images; /* Images array not used in basic rendering */
    
    FORM_DEBUG("<init>(with elements): title=%s, type=%d, elements=%d",
            title ? get_string_from_object(jvm, (JavaObject*)title) : "(null)",
            list_type,
            strings ? strings->length : 0);
    
    return NATIVE_RETURN_VOID();
}

/* Alert.<init>(String, String, Image, AlertType) */
static JavaValue native_alert_init(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* alert = (JavaObject*)args[0].ref;
    /* v34.3 FIX: this native now also serves Alert(String title) (the stub
     * bytecode ctor that shadowed it was removed). Guard the optional
     * arguments so a 2-slot invocation cannot read past args[]. */
    JavaString* title = (JavaString*)(arg_count > 1 ? args[1].ref : NULL);
    JavaString* text = (JavaString*)(arg_count > 2 ? args[2].ref : NULL);
    JavaObject* image = (JavaObject*)(arg_count > 3 ? args[3].ref : NULL);
    JavaObject* alert_type = (JavaObject*)(arg_count > 4 ? args[4].ref : NULL);
    
    if (!alert) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Initialize Alert fields — use dynamic slot lookup */
    int title_slot = find_object_field_slot(alert, "title");
    int text_slot = find_object_field_slot(alert, "text");
    int image_slot = find_object_field_slot(alert, "image");
    int type_slot = find_object_field_slot(alert, "alertType");
    int timeout_slot = find_object_field_slot(alert, "timeout");
    
    if (title_slot >= 0 && text_slot >= 0 && image_slot >= 0 &&
        type_slot >= 0 && timeout_slot >= 0 &&
        OBJECT_HAS_FIELDS(alert, timeout_slot + 1)) {
        alert->fields[title_slot].ref = title;
        alert->fields[text_slot].ref = text;
        alert->fields[image_slot].ref = image;
        alert->fields[type_slot].ref = alert_type;
        alert->fields[timeout_slot].i = 3000; /* Default timeout 3 seconds */
    }
    
    FORM_DEBUG("[Alert] <init>: title=%s, text=%s",
            title ? get_string_from_object(jvm, (JavaObject*)title) : "(null)",
            text ? get_string_from_object(jvm, (JavaObject*)text) : "(null)");
    
    return NATIVE_RETURN_VOID();
}

/* Alert.setTimeout(int) */
static JavaValue native_alert_setTimeout(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* alert = (JavaObject*)args[0].ref;
    jint timeout = args[1].i;
    
    if (alert) {
        int timeout_slot = find_object_field_slot(alert, "timeout");
        if (timeout_slot >= 0 && OBJECT_HAS_FIELDS(alert, timeout_slot + 1)) {
            alert->fields[timeout_slot].i = timeout;
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* Alert.getTimeout() - returns the timeout value */
static JavaValue native_alert_getTimeout(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* alert = (JavaObject*)args[0].ref;
    
    if (alert) {
        int timeout_slot = find_object_field_slot(alert, "timeout");
        if (timeout_slot >= 0 && OBJECT_HAS_FIELDS(alert, timeout_slot + 1)) {
            return NATIVE_RETURN_INT(alert->fields[timeout_slot].i);
        }
    }
    
    return NATIVE_RETURN_INT(0);  /* Default timeout */
}

/* Form.setTicker(Ticker) */
static JavaValue native_form_setTicker(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* form = (JavaObject*)args[0].ref;
    JavaObject* ticker = (JavaObject*)args[1].ref;
    
    FORM_DEBUG("form_setTicker: form=%p, ticker=%p", (void*)form, (void*)ticker);
    
    /* Form has fields: title, items[], ticker — use dynamic slot lookup */
    if (form) {
        int ticker_slot = find_object_field_slot(form, "ticker");
        if (ticker_slot >= 0 && OBJECT_HAS_FIELDS(form, ticker_slot + 1)) {
            form->fields[ticker_slot].ref = ticker;
            FORM_DEBUG("form_setTicker: set ticker successfully");
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* Form.getTicker() - returns the ticker */
static JavaValue native_form_getTicker(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* form = (JavaObject*)args[0].ref;
    
    /* Form has fields: title, items[], ticker — use dynamic slot lookup */
    if (form) {
        int ticker_slot = find_object_field_slot(form, "ticker");
        if (ticker_slot >= 0 && OBJECT_HAS_FIELDS(form, ticker_slot + 1)) {
            return NATIVE_RETURN_OBJECT(form->fields[ticker_slot].ref);
        }
    }
    
    return NATIVE_RETURN_NULL();
}

/* TextBox.<init>(String, String, int, int) */
static JavaValue native_textbox_init(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* textbox = (JavaObject*)args[0].ref;
    JavaString* title = (JavaString*)args[1].ref;
    JavaString* text = (JavaString*)args[2].ref;
    jint max_size = args[3].i;
    jint constraints = args[4].i;
    
    if (!textbox) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Initialize TextBox fields — use dynamic slot lookup */
    int title_slot = find_object_field_slot(textbox, "title");
    int text_slot = find_object_field_slot(textbox, "text");
    int maxsize_slot = find_object_field_slot(textbox, "maxSize");
    int constraints_slot = find_object_field_slot(textbox, "constraints");
    
    if (title_slot >= 0 && text_slot >= 0 && maxsize_slot >= 0 && constraints_slot >= 0 &&
        OBJECT_HAS_FIELDS(textbox, constraints_slot + 1)) {
        textbox->fields[title_slot].ref = title;
        textbox->fields[text_slot].ref = text;
        textbox->fields[maxsize_slot].i = max_size;
        textbox->fields[constraints_slot].i = constraints;
    }
    
    LOG_SAFE("[TextBox] <init>: title=%s, maxSize=%d, constraints=%d\n",
            title ? get_string_from_object(jvm, (JavaObject*)title) : "(null)",
            max_size, constraints);
    
    return NATIVE_RETURN_VOID();
}

/* Form.<init>(String) */
static JavaValue native_form_init(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* form = (JavaObject*)args[0].ref;
    JavaString* title = (JavaString*)args[1].ref;
    
    if (!form) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Initialize Form fields: title, items[] — use dynamic slot lookup */
    int title_slot = find_object_field_slot(form, "title");
    int items_slot = find_object_field_slot(form, "items");
    if (title_slot >= 0 && items_slot >= 0 &&
        OBJECT_HAS_FIELDS(form, items_slot + 1)) {
        form->fields[title_slot].ref = title;
        
        /* Create empty items array */
        JavaArray* items = jvm_new_array(jvm, DESC_OBJECT, 0, NULL);
        form->fields[items_slot].ref = items;
    }
    
    FORM_DEBUG("<init>: title=%s",
            title ? get_string_from_object(jvm, (JavaObject*)title) : "(null)");
    
    return NATIVE_RETURN_VOID();
}

/* ChoiceGroup.<init>(String, int) */
static JavaValue native_choicegroup_init(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* group = (JavaObject*)args[0].ref;
    JavaString* label = (JavaString*)args[1].ref;
    jint choice_type = args[2].i;
    
    if (!group) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Initialize ChoiceGroup fields — use dynamic slot lookup */
    int label_slot = find_object_field_slot(group, "label");
    int choicetype_slot = find_object_field_slot(group, "choiceType");
    int strings_slot = find_object_field_slot(group, "strings");
    int selected_slot = find_object_field_slot(group, "selected");
    
    if (label_slot >= 0 && choicetype_slot >= 0 && strings_slot >= 0 && selected_slot >= 0 &&
        OBJECT_HAS_FIELDS(group, selected_slot + 1)) {
        group->fields[label_slot].ref = label;
        group->fields[choicetype_slot].i = choice_type;
        
        /* Create empty arrays */
        JavaArray* strings = jvm_new_array(jvm, DESC_OBJECT, 0, NULL);
        group->fields[strings_slot].ref = strings;
        
        JavaArray* selected = jvm_new_array(jvm, T_BOOLEAN, 0, NULL);
        group->fields[selected_slot].ref = selected;
    }
    
    LOG_SAFE("[ChoiceGroup] <init>: label=%s, type=%d\n",
            label ? get_string_from_object(jvm, (JavaObject*)label) : "(null)",
            choice_type);
    
    return NATIVE_RETURN_VOID();
}

/* ChoiceGroup.<init>(String, int, String[], Image[]) */
static JavaValue native_choicegroup_init_array(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* group = (JavaObject*)args[0].ref;
    JavaString* label = (JavaString*)args[1].ref;
    jint choice_type = args[2].i;
    JavaArray* strings = (JavaArray*)args[3].ref;
    JavaArray* images = (JavaArray*)args[4].ref;
    
    if (!group) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Initialize ChoiceGroup fields — use dynamic slot lookup */
    int label_slot = find_object_field_slot(group, "label");
    int choicetype_slot = find_object_field_slot(group, "choiceType");
    int strings_slot = find_object_field_slot(group, "strings");
    int selected_slot = find_object_field_slot(group, "selected");
    
    if (label_slot >= 0 && choicetype_slot >= 0 && strings_slot >= 0 && selected_slot >= 0 &&
        OBJECT_HAS_FIELDS(group, selected_slot + 1)) {
        group->fields[label_slot].ref = label;
        group->fields[choicetype_slot].i = choice_type;
        group->fields[strings_slot].ref = strings;
        
        /* Create selection array */
        if (strings) {
            JavaArray* selected = jvm_new_array(jvm, T_BOOLEAN, strings->length, NULL);
            group->fields[selected_slot].ref = selected;
        }
    }
    
    (void)images; /* Not used for now */
    
    LOG_SAFE("[ChoiceGroup] <init>: label=%s, type=%d, items=%d\n",
            label ? get_string_from_object(jvm, (JavaObject*)label) : "(null)",
            choice_type, strings ? strings->length : 0);
    
    return NATIVE_RETURN_VOID();
}

/* Gauge.<init>(String, boolean, int, int) */
static JavaValue native_gauge_init(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* gauge = (JavaObject*)args[0].ref;
    JavaString* label = (JavaString*)args[1].ref;
    jboolean interactive = args[2].i;
    jint max_value = args[3].i;
    jint initial_value = args[4].i;
    
    if (!gauge) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Initialize Gauge fields — use dynamic slot lookup */
    int label_slot = find_object_field_slot(gauge, "label");
    int interactive_slot = find_object_field_slot(gauge, "interactive");
    int maxvalue_slot = find_object_field_slot(gauge, "maxValue");
    int value_slot = find_object_field_slot(gauge, "value");
    
    if (label_slot >= 0 && interactive_slot >= 0 && maxvalue_slot >= 0 && value_slot >= 0 &&
        OBJECT_HAS_FIELDS(gauge, value_slot + 1)) {
        gauge->fields[label_slot].ref = label;
        gauge->fields[interactive_slot].i = interactive;
        gauge->fields[maxvalue_slot].i = max_value;
        gauge->fields[value_slot].i = initial_value;
    }
    
    LOG_SAFE("[Gauge] <init>: label=%s, interactive=%d, max=%d, value=%d\n",
            label ? get_string_from_object(jvm, (JavaObject*)label) : "(null)",
            interactive, max_value, initial_value);
    
    return NATIVE_RETURN_VOID();
}

/* StringItem.<init>(String, String) */
static JavaValue native_stringitem_init(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* item = (JavaObject*)args[0].ref;
    JavaString* label = (JavaString*)args[1].ref;
    JavaString* text = (JavaString*)args[2].ref;
    
    if (!item) {
        return NATIVE_RETURN_VOID();
    }
    
    int label_slot = find_object_field_slot(item, "label");
    int text_slot = find_object_field_slot(item, "text");
    if (label_slot >= 0 && text_slot >= 0 &&
        OBJECT_HAS_FIELDS(item, text_slot + 1)) {
        item->fields[label_slot].ref = label;
        item->fields[text_slot].ref = text;
    }
    
    return NATIVE_RETURN_VOID();
}

/* TextField.<init>(String, String, int, int) */
static JavaValue native_textfield_init(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* item = (JavaObject*)args[0].ref;
    JavaString* label = (JavaString*)args[1].ref;
    JavaString* text = (JavaString*)args[2].ref;
    jint max_size = args[3].i;
    jint constraints = args[4].i;
    
    if (!item) {
        return NATIVE_RETURN_VOID();
    }
    
    int label_slot = find_object_field_slot(item, "label");
    int text_slot = find_object_field_slot(item, "text");
    int maxsize_slot = find_object_field_slot(item, "maxSize");
    int constraints_slot = find_object_field_slot(item, "constraints");
    
    if (label_slot >= 0 && text_slot >= 0 && maxsize_slot >= 0 && constraints_slot >= 0 &&
        OBJECT_HAS_FIELDS(item, constraints_slot + 1)) {
        item->fields[label_slot].ref = label;
        item->fields[text_slot].ref = text;
        item->fields[maxsize_slot].i = max_size;
        item->fields[constraints_slot].i = constraints;
    }
    
    LOG_SAFE("[TextField] <init>: label=%s, maxSize=%d\n",
            label ? get_string_from_object(jvm, (JavaObject*)label) : "(null)",
            max_size);
    
    return NATIVE_RETURN_VOID();
}

/* Spacer.<init>(int, int) */
static JavaValue native_spacer_init(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* spacer = (JavaObject*)args[0].ref;
    jint width = args[1].i;
    jint height = args[2].i;
    
    if (!spacer) {
        return NATIVE_RETURN_VOID();
    }
    
    int width_slot = find_object_field_slot(spacer, "width");
    int height_slot = find_object_field_slot(spacer, "height");
    if (width_slot >= 0 && height_slot >= 0 &&
        OBJECT_HAS_FIELDS(spacer, height_slot + 1)) {
        spacer->fields[width_slot].i = width;
        spacer->fields[height_slot].i = height;
    }
    
    return NATIVE_RETURN_VOID();
}

/* ImageItem.<init>(String, Image, int, String) */
static JavaValue native_imageitem_init(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* item = (JavaObject*)args[0].ref;
    JavaString* label = (JavaString*)args[1].ref;
    JavaObject* image = (JavaObject*)args[2].ref;
    jint layout = args[3].i;
    JavaString* alt_text = (JavaString*)args[4].ref;
    
    if (!item) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Initialize ImageItem fields — use dynamic slot lookup */
    int label_slot = find_object_field_slot(item, "label");
    int image_slot = find_object_field_slot(item, "image");
    int layout_slot = find_object_field_slot(item, "layout");
    int alttext_slot = find_object_field_slot(item, "altText");
    
    if (label_slot >= 0 && image_slot >= 0 && layout_slot >= 0 && alttext_slot >= 0 &&
        OBJECT_HAS_FIELDS(item, alttext_slot + 1)) {
        item->fields[label_slot].ref = label;
        item->fields[image_slot].ref = image;
        item->fields[layout_slot].i = layout;
        item->fields[alttext_slot].ref = alt_text;
    }
    
    return NATIVE_RETURN_VOID();
}

/* List.size() */
static JavaValue native_list_size(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* list = (JavaObject*)args[0].ref;
    
    /* Get field offset considering inheritance */
    if (list) {
        JavaClass* list_class = list->header.clazz;
        int strings_idx = get_field_offset(list_class, "strings");
        
        if (strings_idx >= 0) {
            JavaArray* strings = (JavaArray*)list->fields[strings_idx].ref;
            if (strings) {
                return NATIVE_RETURN_INT(strings->length);
            }
        }
    }
    
    return NATIVE_RETURN_INT(0);
}

/* List.getString(int) */
static JavaValue native_list_getString(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* list = (JavaObject*)args[0].ref;
    jint index = args[1].i;
    
    /* Get field offset considering inheritance */
    if (list) {
        JavaClass* list_class = list->header.clazz;
        int strings_idx = get_field_offset(list_class, "strings");
        
        if (strings_idx >= 0) {
            JavaArray* strings = (JavaArray*)list->fields[strings_idx].ref;
            if (strings && index >= 0 && index < strings->length) {
                void** strings_data = (void**)array_data(strings);
                return NATIVE_RETURN_OBJECT(strings_data[index]);
            }
        }
    }
    
    return NATIVE_RETURN_NULL();
}

/* Alert.getTitle() */
static JavaValue native_alert_getTitle(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* alert = (JavaObject*)args[0].ref;
    
    LOG_SAFE("[ALERT_GETTITLE] alert=%p, fields_count=%d\n", 
            (void*)alert, alert ? alert->header.clazz->fields_count : -1);
    
    if (alert) {
        int title_slot = find_object_field_slot(alert, "title");
        if (title_slot >= 0 && OBJECT_HAS_FIELDS(alert, title_slot + 1)) {
            JavaString* title = (JavaString*)alert->fields[title_slot].ref;
            LOG_SAFE("[ALERT_GETTITLE] title=%p, title_str='%s'\n", 
                    (void*)title, title && title->utf8 ? title->utf8 : "NULL");
            return NATIVE_RETURN_OBJECT(alert->fields[title_slot].ref);
        }
    }
    
    LOG_SAFE("[ALERT_GETTITLE] returning NULL\n");
    return NATIVE_RETURN_NULL();
}

/* Alert.getString() */
static JavaValue native_alert_getString(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* alert = (JavaObject*)args[0].ref;
    
    if (alert) {
        int text_slot = find_object_field_slot(alert, "text");
        if (text_slot >= 0 && OBJECT_HAS_FIELDS(alert, text_slot + 1)) {
            return NATIVE_RETURN_OBJECT(alert->fields[text_slot].ref);
        }
    }
    
    return NATIVE_RETURN_NULL();
}

/* Alert.setTitle(String) */
static JavaValue native_alert_setTitle(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* alert = (JavaObject*)args[0].ref;
    JavaString* title = (JavaString*)args[1].ref;
    
    if (alert) {
        int title_slot = find_object_field_slot(alert, "title");
        if (title_slot >= 0 && OBJECT_HAS_FIELDS(alert, title_slot + 1)) {
            alert->fields[title_slot].ref = title;
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* TextBox.getTitle() */
static JavaValue native_textbox_getTitle(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* textbox = (JavaObject*)args[0].ref;
    
    if (textbox) {
        int title_slot = find_object_field_slot(textbox, "title");
        if (title_slot >= 0 && OBJECT_HAS_FIELDS(textbox, title_slot + 1)) {
            return NATIVE_RETURN_OBJECT(textbox->fields[title_slot].ref);
        }
    }
    
    return NATIVE_RETURN_NULL();
}

/* TextBox.setTitle(String) */
static JavaValue native_textbox_setTitle(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* textbox = (JavaObject*)args[0].ref;
    JavaString* title = (JavaString*)args[1].ref;
    
    if (textbox) {
        int title_slot = find_object_field_slot(textbox, "title");
        if (title_slot >= 0 && OBJECT_HAS_FIELDS(textbox, title_slot + 1)) {
            textbox->fields[title_slot].ref = title;
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* TextBox.getMaxSize() */
static JavaValue native_textbox_getMaxSize(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* textbox = (JavaObject*)args[0].ref;
    
    if (textbox) {
        int maxsize_slot = find_object_field_slot(textbox, "maxSize");
        if (maxsize_slot >= 0 && OBJECT_HAS_FIELDS(textbox, maxsize_slot + 1)) {
            return NATIVE_RETURN_INT(textbox->fields[maxsize_slot].i);
        }
    }
    
    return NATIVE_RETURN_INT(32);
}

/* TextBox.getConstraints() */
static JavaValue native_textbox_getConstraints(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* textbox = (JavaObject*)args[0].ref;
    
    if (textbox) {
        int constraints_slot = find_object_field_slot(textbox, "constraints");
        if (constraints_slot >= 0 && OBJECT_HAS_FIELDS(textbox, constraints_slot + 1)) {
            return NATIVE_RETURN_INT(textbox->fields[constraints_slot].i);
        }
    }
    
    return NATIVE_RETURN_INT(0);
}

/* Choice.size() - for both List and ChoiceGroup */
static JavaValue native_choice_size(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* choice = (JavaObject*)args[0].ref;
    
    if (choice) {
        int strings_slot = find_object_field_slot(choice, "strings");
        if (strings_slot >= 0 && OBJECT_HAS_FIELDS(choice, strings_slot + 1)) {
            JavaArray* strings = (JavaArray*)choice->fields[strings_slot].ref;
            if (strings) {
                return NATIVE_RETURN_INT(strings->length);
            }
        }
    }
    
    return NATIVE_RETURN_INT(0);
}

/* Choice.getString(int) */
static JavaValue native_choice_getString(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* choice = (JavaObject*)args[0].ref;
    jint index = args[1].i;
    
    if (choice) {
        int strings_slot = find_object_field_slot(choice, "strings");
        if (strings_slot >= 0 && OBJECT_HAS_FIELDS(choice, strings_slot + 1)) {
            JavaArray* strings = (JavaArray*)choice->fields[strings_slot].ref;
            if (strings && index >= 0 && index < strings->length) {
                void** strings_data = (void**)array_data(strings);
                return NATIVE_RETURN_OBJECT(strings_data[index]);
            }
        }
    }
    
    return NATIVE_RETURN_NULL();
}

/* Choice.isSelected(int) */
static JavaValue native_choice_isSelected(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* choice = (JavaObject*)args[0].ref;
    jint index = args[1].i;
    
    if (choice) {
        JavaClass* choice_class = choice->header.clazz;
        int selected_idx = get_field_offset(choice_class, "selected");
        
        if (selected_idx >= 0) {
            JavaArray* selected = (JavaArray*)choice->fields[selected_idx].ref;
            if (selected && selected->element_type == T_BOOLEAN && 
                index >= 0 && index < selected->length) {
                jboolean* selected_data = (jboolean*)array_data(selected);
                return NATIVE_RETURN_INT(selected_data[index]);
            }
        }
    }
    
    return NATIVE_RETURN_INT(0);
}

/* Form.deleteAll() */
static JavaValue native_form_deleteAll(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* form = (JavaObject*)args[0].ref;
    
    if (form) {
        int items_slot = find_object_field_slot(form, "items");
        if (items_slot >= 0 && OBJECT_HAS_FIELDS(form, items_slot + 1)) {
            /* Create new empty items array */
            JavaArray* items = jvm_new_array(jvm, DESC_OBJECT, 0, NULL);
            form->fields[items_slot].ref = items;
        }

        /* v34.27: reset the focus on the current form - a stale index
         * left navigation dead and could send FIRE out of bounds. */
        if (form == g_current_displayable) {
            g_focused_item_index = 0;
            g_cg_focused_element = 0;
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* Ticker.<init>(String) */
static JavaValue native_ticker_init(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* ticker = (JavaObject*)args[0].ref;
    JavaString* text = (JavaString*)args[1].ref;
    
    if (ticker && OBJECT_HAS_FIELDS(ticker, 1)) {
        ticker->fields[0].ref = text;
    }
    
    return NATIVE_RETURN_VOID();
}

/* Ticker.getString() */
static JavaValue native_ticker_getString(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* ticker = (JavaObject*)args[0].ref;
    
    if (ticker && OBJECT_HAS_FIELDS(ticker, 1)) {
        return NATIVE_RETURN_OBJECT(ticker->fields[0].ref);
    }
    
    return NATIVE_RETURN_NULL();
}

/* Ticker.setString(String) */
static JavaValue native_ticker_setString(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* ticker = (JavaObject*)args[0].ref;
    JavaString* text = (JavaString*)args[1].ref;
    
    if (ticker && OBJECT_HAS_FIELDS(ticker, 1)) {
        ticker->fields[0].ref = text;
    }
    
    return NATIVE_RETURN_VOID();
}

/* DateField.<init>(String, int) */
static JavaValue native_datefield_init(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* field = (JavaObject*)args[0].ref;
    JavaString* label = (JavaString*)args[1].ref;
    jint mode = args[2].i;
    
    if (field) {
        int label_slot = find_object_field_slot(field, "label");
        int mode_slot = find_object_field_slot(field, "mode");
        int date_slot = find_object_field_slot(field, "date");
        if (label_slot >= 0 && mode_slot >= 0 && date_slot >= 0 &&
            OBJECT_HAS_FIELDS(field, date_slot + 1)) {
            field->fields[label_slot].ref = label;
            field->fields[mode_slot].i = mode;
            field->fields[date_slot].j = 0; /* Date value */
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* Re-register with additional methods */
/* =========================================================================
 * v23: MIDP 2.0 Item API — layout, preferred sizes, item commands.
 * State is kept in a C-side side table keyed by the Item object so no
 * stub-class field changes are needed.
 * ========================================================================= */
#define ITEM_EXTRA_MAX 96
#define ITEM_CMD_MAX 6
typedef struct {
    JavaObject* item;
    int layout;                 /* Item.setLayout */
    int pref_w, pref_h;         /* setPreferredSize; -1 = unset */
    JavaObject* default_command;
    JavaObject* item_cmd_listener;
    JavaObject* commands[ITEM_CMD_MAX];   /* v35: Item.addCommand */
    int command_count;
    JavaObject* font;                     /* v35: StringItem.setFont */
} ItemExtra;
static ItemExtra g_item_extra[ITEM_EXTRA_MAX];
static int g_item_extra_count = 0;

static ItemExtra* item_extra_get(JavaObject* item, int create) {
    if (!item) return NULL;
    for (int i = 0; i < g_item_extra_count; i++) {
        if (g_item_extra[i].item == item) return &g_item_extra[i];
    }
    if (!create || g_item_extra_count >= ITEM_EXTRA_MAX) return NULL;
    ItemExtra* e = &g_item_extra[g_item_extra_count++];
    memset(e, 0, sizeof(*e));
    e->item = item;
    e->layout = 0;   /* LAYOUT_DEFAULT */
    e->pref_w = -1;
    e->pref_h = -1;
    return e;
}

static JavaValue native_item_setLayout(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* item = (JavaObject*)args[0].ref;
    jint layout = args[1].i;
    int old = 0;
    ItemExtra* e = item_extra_get(item, 1);
    if (e) { old = e->layout; e->layout = layout; }
    return NATIVE_RETURN_INT(old);
}

/* v34.29 FIX: the MIDP spec signature is `public void setLayout(int)` —
 * the registry only had the KEmulator-style `(I)I` variant, so every
 * spec-conformant caller (JBenchmark 3D: Item.setLayout(51) in <init>)
 * hit INVOKE-MISSING and the layout was dropped. Store it the same way
 * and return void. */
static JavaValue native_item_setLayout_void(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* item = (JavaObject*)args[0].ref;
    if (!item) return NATIVE_RETURN_VOID();
    jint layout = args[1].i;
    ItemExtra* e = item_extra_get(item, 1);
    if (e) e->layout = layout;
    return NATIVE_RETURN_VOID();
}

static JavaValue native_item_getLayout(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    ItemExtra* e = item_extra_get((JavaObject*)args[0].ref, 0);
    return NATIVE_RETURN_INT(e ? e->layout : 0);
}

static JavaValue native_item_setPreferredSize(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    ItemExtra* e = item_extra_get((JavaObject*)args[0].ref, 1);
    if (e) {
        e->pref_w = args[1].i;
        e->pref_h = args[2].i;
    }
    return NATIVE_RETURN_VOID();
}

static int item_min_width(void)  { return 40; }
static int item_min_height(void) { return 24; }

static JavaValue native_item_getPreferredWidth(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    ItemExtra* e = item_extra_get((JavaObject*)args[0].ref, 0);
    if (e && e->pref_w >= 0) return NATIVE_RETURN_INT(e->pref_w);
    return NATIVE_RETURN_INT(item_min_width());
}

static JavaValue native_item_getPreferredHeight(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    ItemExtra* e = item_extra_get((JavaObject*)args[0].ref, 0);
    if (e && e->pref_h >= 0) return NATIVE_RETURN_INT(e->pref_h);
    return NATIVE_RETURN_INT(item_min_height());
}

static JavaValue native_item_getMinimumWidth(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    return NATIVE_RETURN_INT(item_min_width());
}

static JavaValue native_item_getMinimumHeight(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    return NATIVE_RETURN_INT(item_min_height());
}

static JavaValue native_item_setDefaultCommand(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    ItemExtra* e = item_extra_get((JavaObject*)args[0].ref, 1);
    if (e) e->default_command = (JavaObject*)args[1].ref;
    return NATIVE_RETURN_VOID();
}

static JavaValue native_item_getDefaultCommand(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    ItemExtra* e = item_extra_get((JavaObject*)args[0].ref, 0);
    return NATIVE_RETURN_OBJECT(e ? e->default_command : NULL);
}

static JavaValue native_item_setItemCommandListener(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    ItemExtra* e = item_extra_get((JavaObject*)args[0].ref, 1);
    if (e) e->item_cmd_listener = (JavaObject*)args[1].ref;
    return NATIVE_RETURN_VOID();
}

static JavaValue native_item_notifyStateChanged(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* item = (JavaObject*)args[0].ref;
    if (item) {
        /* Reuses the v18 coalescing dispatcher (audit M-14) */
        form_notify_item_state_changed(jvm, item);
    }
    return NATIVE_RETURN_VOID();
}

/* StringItem(label, text, layout) — 3-arg MIDP 2.0 constructor */
static JavaValue native_stringitem_init3(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)arg_count;
    /* args layout matches the 2-arg ctor for the first three slots */
    native_stringitem_init(jvm, thread, args, 2);
    ItemExtra* e = item_extra_get((JavaObject*)args[0].ref, 1);
    if (e) e->layout = args[3].i;
    return NATIVE_RETURN_VOID();
}

/* Gauge max-value API (fields maxValue/value already exist in the stub) */
static JavaValue native_gauge_setMaxValue(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* gauge = (JavaObject*)args[0].ref;
    jint max = args[1].i;
    if (gauge && max >= 1) {
        int slot = find_object_field_slot(gauge, "maxValue");
        if (slot >= 0 && OBJECT_HAS_FIELDS(gauge, slot + 1)) {
            gauge->fields[slot].i = max;
        }
        /* clamp current value */
        int vslot = find_object_field_slot(gauge, "value");
        if (vslot >= 0 && OBJECT_HAS_FIELDS(gauge, vslot + 1)) {
            if (gauge->fields[vslot].i > max) gauge->fields[vslot].i = max;
        }
    }
    return NATIVE_RETURN_VOID();
}

static JavaValue native_gauge_getMaxValue(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* gauge = (JavaObject*)args[0].ref;
    if (gauge) {
        int slot = find_object_field_slot(gauge, "maxValue");
        if (slot >= 0 && OBJECT_HAS_FIELDS(gauge, slot + 1)) {
            return NATIVE_RETURN_INT(gauge->fields[slot].i);
        }
    }
    return NATIVE_RETURN_INT(100);
}

static JavaValue native_gauge_isInteractive(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* gauge = (JavaObject*)args[0].ref;
    if (gauge) {
        int slot = find_object_field_slot(gauge, "interactive");
        if (slot >= 0 && OBJECT_HAS_FIELDS(gauge, slot + 1)) {
            return NATIVE_RETURN_INT(gauge->fields[slot].i ? 1 : 0);
        }
    }
    return NATIVE_RETURN_INT(0);
}

/* DateField date/inputMode (field "date" already exists; inputMode stored
 * via side-table-free direct field if present) */
static JavaValue native_datefield_setDate(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* field = (JavaObject*)args[0].ref;
    jlong date = args[1].j;
    if (field) {
        int slot = find_object_field_slot(field, "date");
        if (slot >= 0 && OBJECT_HAS_FIELDS(field, slot + 1)) {
            field->fields[slot].j = date;
        }
    }
    return NATIVE_RETURN_VOID();
}

static JavaValue native_datefield_getDate(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* field = (JavaObject*)args[0].ref;
    if (field) {
        int slot = find_object_field_slot(field, "date");
        if (slot >= 0 && OBJECT_HAS_FIELDS(field, slot + 1)) {
            return NATIVE_RETURN_LONG(field->fields[slot].j);
        }
    }
    return NATIVE_RETURN_LONG(0);
}

static JavaValue native_datefield_setInputMode(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* field = (JavaObject*)args[0].ref;
    jint mode = args[1].i;
    if (field) {
        int slot = find_object_field_slot(field, "inputMode");
        if (slot >= 0 && OBJECT_HAS_FIELDS(field, slot + 1)) {
            field->fields[slot].i = mode;
        }
    }
    return NATIVE_RETURN_VOID();
}

static JavaValue native_datefield_getInputMode(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* field = (JavaObject*)args[0].ref;
    if (field) {
        int slot = find_object_field_slot(field, "inputMode");
        if (slot >= 0 && OBJECT_HAS_FIELDS(field, slot + 1)) {
            return NATIVE_RETURN_INT(field->fields[slot].i);
        }
    }
    return NATIVE_RETURN_INT(1);  /* DATE */
}

/* Alert image/type/indicator accessors (fields image/type exist) */
static JavaValue native_alert_setImage(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* alert = (JavaObject*)args[0].ref;
    JavaObject* img = (JavaObject*)args[1].ref;
    if (alert) {
        int slot = find_object_field_slot(alert, "image");
        if (slot >= 0 && OBJECT_HAS_FIELDS(alert, slot + 1)) {
            alert->fields[slot].ref = img;
        }
    }
    return NATIVE_RETURN_VOID();
}

static JavaValue native_alert_getImage(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* alert = (JavaObject*)args[0].ref;
    if (alert) {
        int slot = find_object_field_slot(alert, "image");
        if (slot >= 0 && OBJECT_HAS_FIELDS(alert, slot + 1)) {
            return NATIVE_RETURN_OBJECT(alert->fields[slot].ref);
        }
    }
    return NATIVE_RETURN_NULL();
}

static JavaValue native_alert_setType(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* alert = (JavaObject*)args[0].ref;
    jint type = args[1].i;
    if (alert) {
        int slot = find_object_field_slot(alert, "type");
        if (slot >= 0 && OBJECT_HAS_FIELDS(alert, slot + 1)) {
            alert->fields[slot].i = type;
        }
    }
    return NATIVE_RETURN_VOID();
}

static JavaValue native_alert_getType(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* alert = (JavaObject*)args[0].ref;
    if (alert) {
        int slot = find_object_field_slot(alert, "type");
        if (slot >= 0 && OBJECT_HAS_FIELDS(alert, slot + 1)) {
            return NATIVE_RETURN_INT(alert->fields[slot].i);
        }
    }
    return NATIVE_RETURN_INT(0);
}

/* Command(shortLabel, longLabel, type, priority) 4-arg MIDP 2.0 ctor +
 * getLongLabel — longLabel stored in the side table (Command has no such
 * stub field). */
static JavaValue native_command_init4(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* cmd = (JavaObject*)args[0].ref;
    /* Delegate label/type/priority storage to the 3-arg ctor logic */
    native_command_init(jvm, thread, args, arg_count);
    JavaObject* long_label = (JavaObject*)args[2].ref;
    if (cmd) {
        ItemExtra* e = item_extra_get(cmd, 1);
        if (e) {
            e->default_command = NULL;
            e->item_cmd_listener = long_label;  /* reuse slot as long-label holder */
            e->pref_w = args[4].i;              /* priority kept here too */
        }
    }
    return NATIVE_RETURN_VOID();
}

static JavaValue native_command_getLongLabel(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* cmd = (JavaObject*)args[0].ref;
    if (cmd) {
        ItemExtra* e = item_extra_get(cmd, 0);
        if (e && e->item_cmd_listener) {
            return NATIVE_RETURN_OBJECT(e->item_cmd_listener);
        }
    }
    return NATIVE_RETURN_NULL();
}

/* ChoiceGroup/List multi-select + image accessors */
static JavaValue native_choice_getSelectedFlags(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* choice = (JavaObject*)args[0].ref;
    JavaArray* flags = (JavaArray*)args[1].ref;
    jint selected_count = 0;
    if (choice) {
        int selected_slot = find_object_field_slot(choice, "selected");
        if (selected_slot >= 0 && OBJECT_HAS_FIELDS(choice, selected_slot + 1)) {
            JavaArray* selected = (JavaArray*)choice->fields[selected_slot].ref;
            if (selected && selected->element_type == T_BOOLEAN) {
                jboolean* sd = (jboolean*)array_data(selected);
                for (int i = 0; i < selected->length; i++) {
                    if (sd[i]) selected_count++;
                    if (flags && i < flags->length) {
                        ((jboolean*)array_data(flags))[i] = sd[i];
                    }
                }
            }
        }
    }
    return NATIVE_RETURN_INT(selected_count);
}

static JavaValue native_choice_setSelectedFlags(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* choice = (JavaObject*)args[0].ref;
    JavaArray* flags = (JavaArray*)args[1].ref;
    if (choice && flags && flags->element_type == T_BOOLEAN) {
        int selected_slot = find_object_field_slot(choice, "selected");
        if (selected_slot >= 0 && OBJECT_HAS_FIELDS(choice, selected_slot + 1)) {
            JavaArray* selected = (JavaArray*)choice->fields[selected_slot].ref;
            if (selected && selected->element_type == T_BOOLEAN) {
                jboolean* sd = (jboolean*)array_data(selected);
                jboolean* fd = (jboolean*)array_data(flags);
                int n = selected->length < flags->length ? selected->length : flags->length;
                for (int i = 0; i < n; i++) sd[i] = fd[i];
            }
        }
    }
    return NATIVE_RETURN_VOID();
}

static JavaValue native_choice_getImage(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* choice = (JavaObject*)args[0].ref;
    jint index = args[1].i;
    if (choice) {
        int images_slot = find_object_field_slot(choice, "images");
        if (images_slot >= 0 && OBJECT_HAS_FIELDS(choice, images_slot + 1)) {
            JavaArray* images = (JavaArray*)choice->fields[images_slot].ref;
            if (images && index >= 0 && index < images->length) {
                return NATIVE_RETURN_OBJECT(((JavaObject**)array_data(images))[index]);
            }
        }
    }
    return NATIVE_RETURN_NULL();
}


/* =======================================================================
 * v35: API-completeness batch. These MIDP 2.0 methods were previously
 * unregistered: native_call() silently returned default values, so form
 * menus lost their text (Form.append(String) -> 0), item commands were
 * dropped, and TextBox/List/Choice editing was a no-op.
 * ======================================================================= */

/* Shared core: insert element at position for any Choice-like object
 * (List, ChoiceGroup - both keep "strings"[] + "selected"[] fields). */
static int choice_insert_at(JVM* jvm, JavaObject* choice, int index,
                            JavaString* text, int* out_index) {
    int strings_slot = find_object_field_slot(choice, "strings");
    int selected_slot = find_object_field_slot(choice, "selected");
    if (strings_slot < 0 || selected_slot < 0 ||
        !OBJECT_HAS_FIELDS(choice, strings_slot + 1)) {
        return -1;
    }
    JavaArray* strings = (JavaArray*)choice->fields[strings_slot].ref;
    JavaArray* selected = (JavaArray*)choice->fields[selected_slot].ref;
    int old_len = strings ? strings->length : 0;
    if (index < 0) index = 0;
    if (index > old_len) index = old_len;
    int new_len = old_len + 1;

    JavaArray* new_strings = jvm_new_array(jvm, DESC_OBJECT, new_len, NULL);
    if (!new_strings) return -1;
    void** dst = (void**)array_data(new_strings);
    if (strings && strings->element_type == DESC_OBJECT) {
        void** s = (void**)array_data(strings);
        for (int i = 0; i < index; i++) dst[i] = s[i];
        for (int i = index; i < old_len; i++) dst[i + 1] = s[i];
    }
    dst[index] = text;
    choice->fields[strings_slot].ref = new_strings;

    JavaArray* new_selected = jvm_new_array(jvm, T_BOOLEAN, new_len, NULL);
    if (new_selected) {
        jboolean* dsel = (jboolean*)array_data(new_selected);
        if (selected && selected->element_type == T_BOOLEAN) {
            jboolean* ssel = (jboolean*)array_data(selected);
            for (int i = 0; i < index; i++) dsel[i] = ssel[i];
            for (int i = index; i < old_len; i++) dsel[i + 1] = ssel[i];
        } else {
            for (int i = 0; i < new_len; i++) dsel[i] = 0;
        }
        dsel[index] = 0;
        choice->fields[selected_slot].ref = new_selected;
    }
    if (out_index) *out_index = index;
    return 0;
}

/* Form.append(String) - wraps the string into a StringItem and appends.
 * MISSING before v35: Bounce128 menu called exactly this; the silent
 * default-0 return left the Form empty ("menu draws incorrectly"). */
static JavaValue native_form_append_string(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)arg_count;
    JavaObject* form = (JavaObject*)args[0].ref;
    JavaString* text = (JavaString*)args[1].ref;
    if (!form) return NATIVE_RETURN_INT(-1);
    if (!text) return NATIVE_RETURN_INT(-1);

    JavaClass* si_class = jvm_load_class(jvm, "javax/microedition/lcdui/StringItem");
    if (!si_class) {
        FORM_DEBUG("append(String): StringItem stub class not available");
        return NATIVE_RETURN_INT(-1);
    }
    JavaObject* item = jvm_new_object(jvm, si_class);
    if (!item) return NATIVE_RETURN_INT(-1);

    /* StringItem fields: label, text */
    int label_slot = find_object_field_slot(item, "label");
    int text_slot = find_object_field_slot(item, "text");
    if (label_slot >= 0 && text_slot >= 0 &&
        OBJECT_HAS_FIELDS(item, text_slot + 1)) {
        item->fields[label_slot].ref = NULL;   /* null label */
        item->fields[text_slot].ref = text;
    }

    /* Delegate to the Item append implementation */
    JavaValue sub[2];
    sub[0].ref = form;
    sub[1].ref = item;
    JavaValue r = native_form_append(jvm, thread, sub, 2);
    LOG_SAFE("[FORM] append(String) -> wrapped StringItem %p, index=%d\n",
             (void*)item, r.i);
    return r;
}

/* Form.append(Image) - v34.65: wraps the image into an ImageItem and
 * appends. MISSING before: FPCpackage/FPC.commandAction hit the
 * INVOKE-MISSING stub — the no-op pushed default 0, the element never
 * entered the items[] array and the picture never showed. */
static JavaValue native_form_append_image(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)arg_count;
    JavaObject* form = (JavaObject*)args[0].ref;
    JavaObject* image = (JavaObject*)args[1].ref;
    if (!form) return NATIVE_RETURN_INT(-1);
    if (!image) return NATIVE_RETURN_INT(-1);

    JavaClass* ii_class = jvm_load_class(jvm, "javax/microedition/lcdui/ImageItem");
    if (!ii_class) {
        FORM_DEBUG("append(Image): ImageItem stub class not available");
        return NATIVE_RETURN_INT(-1);
    }
    JavaObject* item = jvm_new_object(jvm, ii_class);
    if (!item) return NATIVE_RETURN_INT(-1);

    /* ImageItem fields: label, image, layout, altText (stubs.c order-free
     * — use dynamic slot lookup like every other native here). */
    int label_slot = find_object_field_slot(item, "label");
    int image_slot = find_object_field_slot(item, "image");
    int layout_slot = find_object_field_slot(item, "layout");
    if (label_slot >= 0 && image_slot >= 0 && layout_slot >= 0 &&
        OBJECT_HAS_FIELDS(item, layout_slot + 1)) {
        item->fields[label_slot].ref = NULL;   /* null label (MIDP default) */
        item->fields[image_slot].ref = image;
        item->fields[layout_slot].i = 0;       /* LAYOUT_DEFAULT */
    }

    /* Delegate to the Item append implementation */
    JavaValue sub[2];
    sub[0].ref = form;
    sub[1].ref = item;
    JavaValue r = native_form_append(jvm, thread, sub, 2);
    LOG_SAFE("[FORM] append(Image) -> wrapped ImageItem %p, index=%d\n",
             (void*)item, r.i);
    return r;
}

/* Form.set(int, Item) */
static JavaValue native_form_set(JVM* jvm, JavaThread* thread,
                                 JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* form = (JavaObject*)args[0].ref;
    jint index = args[1].i;
    JavaObject* item = (JavaObject*)args[2].ref;
    if (!form || !item) return NATIVE_RETURN_VOID();
    int items_slot = find_object_field_slot(form, "items");
    if (items_slot < 0 || !OBJECT_HAS_FIELDS(form, items_slot + 1)) {
        return NATIVE_RETURN_VOID();
    }
    JavaArray* items = (JavaArray*)form->fields[items_slot].ref;
    if (!items || index < 0 || index >= items->length) {
        return NATIVE_RETURN_VOID();
    }
    void** data = (void**)array_data(items);
    data[index] = item;
    return NATIVE_RETURN_VOID();
}

/* Form.insert(int, Item) */
static JavaValue native_form_insert(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* form = (JavaObject*)args[0].ref;
    jint index = args[1].i;
    JavaObject* item = (JavaObject*)args[2].ref;
    if (!form || !item) return NATIVE_RETURN_VOID();
    int items_slot = find_object_field_slot(form, "items");
    if (items_slot < 0 || !OBJECT_HAS_FIELDS(form, items_slot + 1)) {
        return NATIVE_RETURN_VOID();
    }
    JavaArray* items = (JavaArray*)form->fields[items_slot].ref;
    int old_len = items ? items->length : 0;
    if (index < 0) index = 0;
    if (index > old_len) index = old_len;
    int new_len = old_len + 1;

    JavaArray* new_items = jvm_new_array(jvm, DESC_OBJECT, new_len, NULL);
    if (!new_items) return NATIVE_RETURN_VOID();
    void** dst = (void**)array_data(new_items);
    if (items && items->element_type == DESC_OBJECT) {
        void** s = (void**)array_data(items);
        for (int i = 0; i < index; i++) dst[i] = s[i];
        for (int i = index; i < old_len; i++) dst[i + 1] = s[i];
    }
    dst[index] = item;
    form->fields[items_slot].ref = new_items;

    /* v34.27: keep the focused item stable across an insert above it */
    if (form == g_current_displayable && index <= g_focused_item_index) {
        g_focused_item_index++;
    }
    return NATIVE_RETURN_VOID();
}

/* Item.addCommand / removeCommand / getItemCommandListener (MIDP 2.0) */
static JavaValue native_item_addCommand(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* item = (JavaObject*)args[0].ref;
    JavaObject* cmd = (JavaObject*)args[1].ref;
    ItemExtra* e = item_extra_get(item, 1);
    if (e && cmd) {
        for (int i = 0; i < e->command_count; i++) {
            if (e->commands[i] == cmd) return NATIVE_RETURN_VOID(); /* dup */
        }
        if (e->command_count < ITEM_CMD_MAX) {
            e->commands[e->command_count++] = cmd;
        } else {
            LOG_SAFE("[ITEM] addCommand: table full (%d), dropping\n", ITEM_CMD_MAX);
        }
    }
    return NATIVE_RETURN_VOID();
}

static JavaValue native_item_removeCommand(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* item = (JavaObject*)args[0].ref;
    JavaObject* cmd = (JavaObject*)args[1].ref;
    ItemExtra* e = item_extra_get(item, 0);
    if (e && cmd) {
        for (int i = 0; i < e->command_count; i++) {
            if (e->commands[i] == cmd) {
                for (int j = i; j < e->command_count - 1; j++) {
                    e->commands[j] = e->commands[j + 1];
                }
                e->commands[--e->command_count] = NULL;
                break;
            }
        }
    }
    return NATIVE_RETURN_VOID();
}

static JavaValue native_item_getItemCommandListener(JVM* jvm, JavaThread* thread,
                                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    ItemExtra* e = item_extra_get((JavaObject*)args[0].ref, 0);
    return NATIVE_RETURN_OBJECT(e ? e->item_cmd_listener : NULL);
}

/* =========================================================================
 * v36.30 [ITEM-CMDS]: DISPATCH for Item-level commands.
 *
 * FIELD REPORT (Sys Info (MH)(1.1.0) class of midlets): «софтовые левая и
 * правая кнопки не работают». Info-утилиты вешают свои действия (Exit /
 * Copy / Refresh...) на САМИ ITEMS (Item.addCommand + setDefaultCommand +
 * ItemCommandListener), а не на Form. Хранилище команд появилось ещё в v35
 * (native_item_addCommand), но НИКТО не вызывал их: cmd_count у Form == 0
 * -> софтбар не рисуется, X/B молча отклоняются (zero-commands), тап по
 * item дропался в !item_is_interactive. Теперь команды item'ов:
 *   - рисуются на софтбаре, когда у Form своих команд нет;
 *   - dispatch'ятся X/B (левый/правый софт), FIRE на сфокусированном item
 *     и тапом по item;
 *   - при 3+ командах левый софт открывает командное меню (оверлей как у
 *     form-команд).
 * ========================================================================= */

/* Effective command list: default_command first (MIDP: it is the "select"
 * action), then addCommand order, dedup. Returns the count. */
static int form_item_cmd_effective(JavaObject* item, JavaObject** out, int max) {
    if (!item) return 0;
    ItemExtra* e = item_extra_get(item, 0);
    if (!e) return 0;
    int n = 0;
    if (e->default_command && n < max) out[n++] = e->default_command;
    for (int i = 0; i < e->command_count && n < max; i++) {
        JavaObject* c = e->commands[i];
        if (!c) continue;
        int dup = 0;
        for (int j = 0; j < n; j++) {
            if (out[j] == c) { dup = 1; break; }
        }
        if (!dup) out[n++] = c;
    }
    return n;
}

/* Command label as UTF-8 (Command fields: label(0), type(1), priority(2)) */
static const char* form_cmd_label_utf8(JVM* jvm, JavaObject* cmd) {
    if (!cmd || !OBJECT_HAS_FIELDS(cmd, 1)) return NULL;
    JavaString* lbl = (JavaString*)cmd->fields[0].ref;
    if (!lbl) return NULL;
    return string_utf8(jvm, lbl);   /* signature takes JavaString* */
}

/* Fire one item command through the ItemCommandListener (signature:
 * commandAction(Command c, Item item) — NOTE the Item parameter, unlike
 * the Displayable-level CommandListener). Returns true when dispatched. */
static bool form_item_fire_command(JVM* jvm, JavaObject* item, JavaObject* cmd) {
    if (!jvm || !item || !cmd) return false;
    ItemExtra* e = item_extra_get(item, 0);
    if (!e || !e->item_cmd_listener || !e->item_cmd_listener->header.clazz) {
        LOG_SAFE("[ITEMCMD] no ItemCommandListener for item — command inert\n");
        return false;
    }
    JavaMethod* m = jvm_resolve_method(jvm, e->item_cmd_listener->header.clazz,
        "commandAction",
        "(Ljavax/microedition/lcdui/Command;Ljavax/microedition/lcdui/Item;)V");
    if (!m) {
        LOG_SAFE("[ITEMCMD] commandAction(Command,Item) not found\n");
        return false;
    }
    const char* lbl = form_cmd_label_utf8(jvm, cmd);
    LOG_SAFE("[ITEMCMD] fire '%s'\n", lbl ? lbl : "?");
    JavaValue args[3];
    args[0].ref = e->item_cmd_listener;
    args[1].ref = cmd;
    args[2].ref = item;
    JavaValue result;
    JavaThread* thread = jvm_current_thread(jvm);
    int rc = execute_method(jvm, thread, m, args, &result);
    if (thread && thread->pending_exception) {
        /* [EXC-DROP-FREE] v36.47: + освобождение C-строк трейса
         * (см. [PLAYERUPDATE-ARGS] в media.c — тот же класс утечки) */
        thread->pending_exception = NULL;  /* don't poison later dispatches */
        if (thread->exception_stack_trace) {
            free(thread->exception_stack_trace);
            thread->exception_stack_trace = NULL;
        }
        if (thread->exception_throw_info) {
            free(thread->exception_throw_info);
            thread->exception_throw_info = NULL;
        }
    }
    /* The listener may have exited the app (notifyDestroyed) or changed the
     * screen — both handled by the existing paths; just redraw if alive. */
    if (jvm->running && g_current_displayable) {
        midp_render_form(jvm, g_current_displayable);
    }
    (void)rc;
    return true;
}

/* ---- Item command menu (3+ commands) ------------------------------------ */

static bool form_item_menu_open_for(JavaObject* item) {
    JavaObject* cmds[8];
    int n = form_item_cmd_effective(item, cmds, 8);
    if (n < 3) return false;
    g_item_menu_item = item;
    for (int i = 0; i < n; i++) g_item_menu_cmds[i] = cmds[i];
    g_item_menu_count = n;
    g_item_menu_sel = 0;
    LOG_SAFE("[ITEMCMD] menu open (%d commands)\n", n);
    return true;
}

bool midp_form_item_menu_active(void) { return form_item_menu_active_raw(); }
void midp_form_item_menu_close(void) { form_item_menu_close_raw(); }
int midp_form_item_menu_selected(void) {
    return form_item_menu_active_raw() ? g_item_menu_sel : -1;
}
JavaObject* midp_form_item_menu_item(void) {
    return form_item_menu_active_raw() ? g_item_menu_item : NULL;
}
JavaObject* midp_form_item_menu_command(int idx) {
    if (!form_item_menu_active_raw() || idx < 0 || idx >= g_item_menu_count) {
        return NULL;
    }
    return g_item_menu_cmds[idx];
}

bool midp_form_item_menu_nav(int dir) {
    if (!form_item_menu_active_raw()) return false;
    g_item_menu_sel += (dir < 0) ? -1 : 1;
    if (g_item_menu_sel < 0) g_item_menu_sel = g_item_menu_count - 1;
    if (g_item_menu_sel >= g_item_menu_count) g_item_menu_sel = 0;
    return true;
}

bool midp_form_item_menu_select(JVM* jvm) {
    if (!form_item_menu_active_raw()) return false;
    JavaObject* item = g_item_menu_item;
    JavaObject* cmd = g_item_menu_cmds[g_item_menu_sel];
    form_item_menu_close_raw();
    return form_item_fire_command(jvm, item, cmd);
}

/* Soft-key dispatch for the FOCUSED item's commands (called from
 * display.c's midp_handle_soft_button when the FORM itself has no
 * commands). Mirrors the form-level semantics:
 *   1-2 commands: left = first, right = second (single: both fire it)
 *   3+ commands:  left opens the menu, right fires the first */
bool midp_form_item_soft_button(JVM* jvm, int button_index) {
    if (!g_current_displayable || g_vkb.active) return false;
    if (midp_displayable_kind(g_current_displayable) != MIDP_UI_KIND_FORM) {
        return false;
    }
    /* open menu first (it owns the soft keys while visible) */
    if (form_item_menu_active_raw()) {
        if (button_index == 0) {
            form_item_menu_close_raw();
            if (g_current_displayable) midp_render_form(jvm, g_current_displayable);
            return true;
        }
        return midp_form_item_menu_select(jvm);
    }

    int items_slot = find_object_field_slot(g_current_displayable, "items");
    if (items_slot < 0 || !OBJECT_HAS_FIELDS(g_current_displayable, items_slot + 1)) {
        return false;
    }
    JavaArray* items = (JavaArray*)g_current_displayable->fields[items_slot].ref;
    if (!items || items->element_type != DESC_OBJECT || items->length <= 0) {
        return false;
    }
    if (g_focused_item_index < 0 || g_focused_item_index >= items->length) {
        return false;
    }
    JavaObject* item = (JavaObject*)((void**)array_data(items))[g_focused_item_index];
    if (!item) return false;

    JavaObject* cmds[8];
    int n = form_item_cmd_effective(item, cmds, 8);
    if (n <= 0) return false;

    if (n <= 2) {
        int pick = button_index;                 /* 0 -> first, 1 -> second */
        if (pick >= n) pick = 0;                 /* single command: both keys */
        return form_item_fire_command(jvm, item, cmds[pick]);
    }
    if (button_index == 0) {
        if (form_item_menu_open_for(item)) {
            if (g_current_displayable) midp_render_form(jvm, g_current_displayable);
            return true;
        }
        return false;
    }
    return form_item_fire_command(jvm, item, cmds[0]);
}

/* Item commands on the CURRENT focused item (count>0 means the softbar
 * should show them when the Form has no own commands). out_cmds may be
 * NULL (count-only query — an internal scratch buffer is used). */
int midp_form_focused_item_commands(JavaObject** out_cmds, int max) {
    JavaObject* scratch[8];
    if (!out_cmds || max <= 0 || max > 8) {
        out_cmds = scratch;
        max = 8;
    }
    if (!g_current_displayable) return 0;
    if (midp_displayable_kind(g_current_displayable) != MIDP_UI_KIND_FORM) return 0;
    int items_slot = find_object_field_slot(g_current_displayable, "items");
    if (items_slot < 0 || !OBJECT_HAS_FIELDS(g_current_displayable, items_slot + 1)) return 0;
    JavaArray* items = (JavaArray*)g_current_displayable->fields[items_slot].ref;
    if (!items || items->element_type != DESC_OBJECT || items->length <= 0) return 0;
    if (g_focused_item_index < 0 || g_focused_item_index >= items->length) return 0;
    JavaObject* item = (JavaObject*)((void**)array_data(items))[g_focused_item_index];
    if (!item) return 0;
    return form_item_cmd_effective(item, out_cmds, max);
}


/* StringItem.getFont/setFont (MIDP 2.0). Font is stored on the side table;
 * the bitmap renderer ignores per-item fonts, but getFont() must round-trip
 * the object - games save/restore it around custom drawing. */
static JavaValue native_stringitem_setFont(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    ItemExtra* e = item_extra_get((JavaObject*)args[0].ref, 1);
    if (e) e->font = (JavaObject*)args[1].ref;
    return NATIVE_RETURN_VOID();
}

static JavaValue native_stringitem_getFont(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    ItemExtra* e = item_extra_get((JavaObject*)args[0].ref, 0);
    if (e && e->font) return NATIVE_RETURN_OBJECT(e->font);
    /* Spec default: Font.getFont(FACE_SYSTEM, STYLE_PLAIN, SIZE_MEDIUM) */
    extern JavaObject* midp_font_default_object(JVM* jvm);
    JavaObject* font_obj = midp_font_default_object(jvm);
    if (font_obj) return NATIVE_RETURN_OBJECT(font_obj);
    return NATIVE_RETURN_NULL();
}

/* List.insert(int, String, Image); also used for ChoiceGroup.insert */
static JavaValue native_list_insert(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* choice = (JavaObject*)args[0].ref;
    jint index = args[1].i;
    JavaString* text = (JavaString*)args[2].ref;
    if (!choice) return NATIVE_RETURN_VOID();
    int pos = 0;
    if (choice_insert_at(jvm, choice, index, text, &pos) == 0) {
        /* v34.27: keep the selection stable across an insert above it */
        if (choice == g_current_displayable && index <= g_list_selected_index) {
            g_list_selected_index++;
        }
    } else {
        FORM_DEBUG("insert: failed (choice=%p, index=%d)", (void*)choice, index);
    }
    return NATIVE_RETURN_VOID();
}

/* List.setSelectCommand / setDefaultCommand / fit policy */
static JavaValue native_list_setSelectCommand(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    ListExtra* e = list_extra_get((JavaObject*)args[0].ref, 1);
    if (e) e->select_command = (JavaObject*)args[1].ref;
    return NATIVE_RETURN_VOID();
}

static JavaValue native_list_setDefaultCommand(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    ListExtra* e = list_extra_get((JavaObject*)args[0].ref, 1);
    if (e) e->default_command = (JavaObject*)args[1].ref;
    return NATIVE_RETURN_VOID();
}

static JavaValue native_list_getFitPolicy(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    ListExtra* e = list_extra_get((JavaObject*)args[0].ref, 0);
    return NATIVE_RETURN_INT(e ? e->fit_policy : 0);
}

static JavaValue native_list_setFitPolicy(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    ListExtra* e = list_extra_get((JavaObject*)args[0].ref, 1);
    if (e) e->fit_policy = args[1].i;
    return NATIVE_RETURN_VOID();
}

/* ---- TextBox string editing (MIDP 2.0 convenience API) ----
 * TextBox "text" field holds a JavaString; edits rebuild it. */

static int textbox_replace_text(JVM* jvm, JavaObject* tb, const char* new_text) {
    int text_slot = find_object_field_slot(tb, "text");
    if (text_slot < 0 || !OBJECT_HAS_FIELDS(tb, text_slot + 1)) return -1;
    JavaString* s = jvm_new_string(jvm, new_text ? new_text : "");
    if (!s) return -1;
    tb->fields[text_slot].ref = s;
    return 0;
}

static JavaValue native_textbox_insert(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* tb = (JavaObject*)args[0].ref;
    JavaString* src = (JavaString*)args[1].ref;
    jint pos = args[2].i;
    if (!tb || !src) return NATIVE_RETURN_VOID();
    int text_slot = find_object_field_slot(tb, "text");
    if (text_slot < 0 || !OBJECT_HAS_FIELDS(tb, text_slot + 1)) {
        return NATIVE_RETURN_VOID();
    }
    const char* old = get_string_from_object(jvm,
        (JavaObject*)tb->fields[text_slot].ref);
    const char* ins = get_string_from_object(jvm, (JavaObject*)src);
    int old_len = old ? (int)strlen(old) : 0;
    if (pos < 0) pos = 0;
    if (pos > old_len) pos = old_len;
    int ins_len = ins ? (int)strlen(ins) : 0;
    char* buf = (char*)malloc(old_len + ins_len + 1);
    if (!buf) return NATIVE_RETURN_VOID();
    memcpy(buf, old ? old : "", pos);
    memcpy(buf + pos, ins ? ins : "", ins_len);
    memcpy(buf + pos + ins_len, old ? old + pos : "", old_len - pos);
    buf[old_len + ins_len] = '\0';
    textbox_replace_text(jvm, tb, buf);
    free(buf);
    return NATIVE_RETURN_VOID();
}

static JavaValue native_textbox_delete(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* tb = (JavaObject*)args[0].ref;
    jint offset = args[1].i;
    jint length = args[2].i;
    if (!tb || length <= 0) return NATIVE_RETURN_VOID();
    int text_slot = find_object_field_slot(tb, "text");
    if (text_slot < 0 || !OBJECT_HAS_FIELDS(tb, text_slot + 1)) {
        return NATIVE_RETURN_VOID();
    }
    const char* old = get_string_from_object(jvm,
        (JavaObject*)tb->fields[text_slot].ref);
    int old_len = old ? (int)strlen(old) : 0;
    if (offset < 0 || offset >= old_len) return NATIVE_RETURN_VOID();
    if (offset + length > old_len) length = old_len - offset;
    char* buf = (char*)malloc(old_len - length + 1);
    if (!buf) return NATIVE_RETURN_VOID();
    memcpy(buf, old, offset);
    memcpy(buf + offset, old + offset + length, old_len - offset - length);
    buf[old_len - length] = '\0';
    textbox_replace_text(jvm, tb, buf);
    free(buf);
    return NATIVE_RETURN_VOID();
}

static JavaValue native_textbox_setChars(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* tb = (JavaObject*)args[0].ref;
    JavaArray* data = (JavaArray*)args[1].ref;
    jint offset = args[2].i;
    jint length = args[3].i;
    if (!tb) return NATIVE_RETURN_VOID();
    if (!data || length <= 0) {
        textbox_replace_text(jvm, tb, "");
        return NATIVE_RETURN_VOID();
    }
    if (offset < 0 || data->element_type != T_CHAR) return NATIVE_RETURN_VOID();
    if (offset + length > data->length) length = data->length - offset;
    if (length < 0) length = 0;
    jchar* chars = (jchar*)array_data(data);
    /* UTF-8 worst case 3 bytes per BMP char */
    char* buf = (char*)malloc((size_t)length * 3 + 1);
    if (!buf) return NATIVE_RETURN_VOID();
    int o = 0;
    for (int i = 0; i < length; i++) {
        unsigned int c = chars[offset + i];
        if (c < 0x80) {
            buf[o++] = (char)c;
        } else if (c < 0x800) {
            buf[o++] = (char)(0xC0 | (c >> 6));
            buf[o++] = (char)(0x80 | (c & 0x3F));
        } else {
            buf[o++] = (char)(0xE0 | (c >> 12));
            buf[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            buf[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    buf[o] = '\0';
    textbox_replace_text(jvm, tb, buf);
    free(buf);
    return NATIVE_RETURN_VOID();
}

static JavaValue native_textbox_size(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* tb = (JavaObject*)args[0].ref;
    if (!tb) return NATIVE_RETURN_INT(0);
    int text_slot = find_object_field_slot(tb, "text");
    if (text_slot < 0 || !OBJECT_HAS_FIELDS(tb, text_slot + 1)) {
        return NATIVE_RETURN_INT(0);
    }
    JavaString* s = (JavaString*)tb->fields[text_slot].ref;
    const char* str = s ? get_string_from_object(jvm, (JavaObject*)s) : NULL;
    return NATIVE_RETURN_INT(str ? (jint)strlen(str) : 0);
}

static JavaValue native_textbox_getCaretPosition(JVM* jvm, JavaThread* thread,
                                                 JavaValue* args, int arg_count) {
    /* No caret model in the bitmap renderer: report end-of-text like the
     * stub VKB leaves it after every edit. */
    extern JavaValue native_textbox_size(JVM*, JavaThread*, JavaValue*, int);
    return native_textbox_size(jvm, thread, args, arg_count);
}

static JavaValue native_textbox_setInitialInputMode(JVM* jvm, JavaThread* thread,
                                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    return NATIVE_RETURN_VOID(); /* hint only, no hardware keyboard modes */
}

/* ---- Displayable title/ticker (moved from Screen to Displayable in MIDP2).
 * "title" exists per stub class (Form/List/TextBox/Alert); Canvas subclasses
 * have none -> setTitle is a harmless no-op there. Ticker goes to a small
 * side table. ---- */
#define TICKER_TABLE_MAX 16
static struct { JavaObject* displayable; JavaObject* ticker; } g_ticker_table[TICKER_TABLE_MAX];
static int g_ticker_table_count = 0;

static JavaValue native_displayable_setTitle(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* d = (JavaObject*)args[0].ref;
    JavaString* title = (JavaString*)args[1].ref;
    if (!d) return NATIVE_RETURN_VOID();
    int slot = find_object_field_slot(d, "title");
    if (slot >= 0 && OBJECT_HAS_FIELDS(d, slot + 1)) {
        d->fields[slot].ref = title;
    }
    return NATIVE_RETURN_VOID();
}

static JavaValue native_displayable_getTitle(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* d = (JavaObject*)args[0].ref;
    if (d) {
        int slot = find_object_field_slot(d, "title");
        if (slot >= 0 && OBJECT_HAS_FIELDS(d, slot + 1)) {
            return NATIVE_RETURN_OBJECT(d->fields[slot].ref);
        }
    }
    return NATIVE_RETURN_NULL();
}

static JavaValue native_displayable_setTicker(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* d = (JavaObject*)args[0].ref;
    JavaObject* t = (JavaObject*)args[1].ref;
    if (!d) return NATIVE_RETURN_VOID();
    for (int i = 0; i < g_ticker_table_count; i++) {
        if (g_ticker_table[i].displayable == d) {
            g_ticker_table[i].ticker = t;
            return NATIVE_RETURN_VOID();
        }
    }
    if (g_ticker_table_count < TICKER_TABLE_MAX) {
        g_ticker_table[g_ticker_table_count].displayable = d;
        g_ticker_table[g_ticker_table_count].ticker = t;
        g_ticker_table_count++;
    }
    return NATIVE_RETURN_VOID();
}

static JavaValue native_displayable_getTicker(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* d = (JavaObject*)args[0].ref;
    if (d) {
        for (int i = 0; i < g_ticker_table_count; i++) {
            if (g_ticker_table[i].displayable == d) {
                return NATIVE_RETURN_OBJECT(g_ticker_table[i].ticker);
            }
        }
    }
    return NATIVE_RETURN_NULL();
}

void init_javax_microedition_lcdui_form(JVM* jvm) {
    /* Register global objects as GC roots */
    gc_add_root(jvm, (void**)&g_current_form);
    gc_add_root(jvm, (void**)&g_current_displayable);
    gc_add_root(jvm, (void**)&g_select_command);
    gc_add_root(jvm, (void**)&g_item_state_listener);
    
    NativeMethodEntry methods[] = {
        /* Form methods */
        {"javax/microedition/lcdui/Form", "<init>", "(Ljava/lang/String;)V", native_form_init},
        {"javax/microedition/lcdui/Form", "append", "(Ljavax/microedition/lcdui/Item;)I", native_form_append},
        {"javax/microedition/lcdui/Form", "delete", "(I)V", native_form_delete},
        {"javax/microedition/lcdui/Form", "deleteAll", "()V", native_form_deleteAll},
        {"javax/microedition/lcdui/Form", "size", "()I", native_form_size},
        {"javax/microedition/lcdui/Form", "get", "(I)Ljavax/microedition/lcdui/Item;", native_form_get},
        {"javax/microedition/lcdui/Form", "getTitle", "()Ljava/lang/String;", native_form_getTitle},
        {"javax/microedition/lcdui/Form", "setTitle", "(Ljava/lang/String;)V", native_form_setTitle},
        {"javax/microedition/lcdui/Form", "setItemStateListener", "(Ljavax/microedition/lcdui/ItemStateListener;)V", native_form_setItemStateListener},
        {"javax/microedition/lcdui/Form", "setTicker", "(Ljavax/microedition/lcdui/Ticker;)V", native_form_setTicker},
        {"javax/microedition/lcdui/Form", "getTicker", "()Ljavax/microedition/lcdui/Ticker;", native_form_getTicker},
        
        /* Item methods */
        {"javax/microedition/lcdui/Item", "setLabel", "(Ljava/lang/String;)V", native_item_setLabel},
        {"javax/microedition/lcdui/Item", "getLabel", "()Ljava/lang/String;", native_item_getLabel},
        
        /* StringItem methods */
        {"javax/microedition/lcdui/StringItem", "<init>", "(Ljava/lang/String;Ljava/lang/String;)V", native_stringitem_init},
        {"javax/microedition/lcdui/StringItem", "setText", "(Ljava/lang/String;)V", native_stringitem_setText},
        {"javax/microedition/lcdui/StringItem", "getText", "()Ljava/lang/String;", native_stringitem_getText},
        
        /* TextField methods */
        {"javax/microedition/lcdui/TextField", "<init>", "(Ljava/lang/String;Ljava/lang/String;II)V", native_textfield_init},
        {"javax/microedition/lcdui/TextField", "setString", "(Ljava/lang/String;)V", native_textfield_setString},
        {"javax/microedition/lcdui/TextField", "getString", "()Ljava/lang/String;", native_textfield_getString},
        {"javax/microedition/lcdui/TextField", "setChars", "([CII)V", native_textfield_setChars},
        {"javax/microedition/lcdui/TextField", "size", "()I", native_textfield_size},
        {"javax/microedition/lcdui/TextField", "getMaxSize", "()I", native_textfield_getMaxSize},
        {"javax/microedition/lcdui/TextField", "setMaxSize", "(I)I", native_textfield_setMaxSize},
        {"javax/microedition/lcdui/TextField", "getConstraints", "()I", native_textfield_getConstraints},
        {"javax/microedition/lcdui/TextField", "setConstraints", "(I)V", native_textfield_setConstraints},
        {"javax/microedition/lcdui/TextField", "getChars", "([C)V", native_textfield_getChars},
        {"javax/microedition/lcdui/TextField", "delete", "(II)V", native_textfield_delete},
        {"javax/microedition/lcdui/TextField", "insert", "(Ljava/lang/String;I)V", native_textfield_insert},
        {"javax/microedition/lcdui/TextField", "getCaretPosition", "()I", native_textfield_getCaretPosition},
        
        /* ChoiceGroup methods */
        {"javax/microedition/lcdui/ChoiceGroup", "<init>", "(Ljava/lang/String;I)V", native_choicegroup_init},
        {"javax/microedition/lcdui/ChoiceGroup", "<init>", "(Ljava/lang/String;I[Ljava/lang/String;[Ljavax/microedition/lcdui/Image;)V", native_choicegroup_init_array},
        {"javax/microedition/lcdui/ChoiceGroup", "append", "(Ljava/lang/String;Ljavax/microedition/lcdui/Image;)I", native_choicegroup_append},
        {"javax/microedition/lcdui/ChoiceGroup", "setSelectedIndex", "(IZ)V", native_choicegroup_setSelectedIndex},
        {"javax/microedition/lcdui/ChoiceGroup", "getSelectedIndex", "()I", native_choicegroup_getSelectedIndex},
        {"javax/microedition/lcdui/ChoiceGroup", "size", "()I", native_choice_size},
        {"javax/microedition/lcdui/ChoiceGroup", "getString", "(I)Ljava/lang/String;", native_choice_getString},
        {"javax/microedition/lcdui/ChoiceGroup", "isSelected", "(I)Z", native_choice_isSelected},
        
        /* Gauge methods */
        {"javax/microedition/lcdui/Gauge", "<init>", "(Ljava/lang/String;ZII)V", native_gauge_init},
        {"javax/microedition/lcdui/Gauge", "setValue", "(I)V", native_gauge_setValue},
        {"javax/microedition/lcdui/Gauge", "getValue", "()I", native_gauge_getValue},
        
        /* List methods */
        {"javax/microedition/lcdui/List", "<init>", "(Ljava/lang/String;I)V", native_list_init},
        {"javax/microedition/lcdui/List", "<init>", "(Ljava/lang/String;I[Ljava/lang/String;[Ljavax/microedition/lcdui/Image;)V", native_list_init_with_elements},
        {"javax/microedition/lcdui/List", "append", "(Ljava/lang/String;Ljavax/microedition/lcdui/Image;)I", native_list_append},
        {"javax/microedition/lcdui/List", "getSelectedIndex", "()I", native_list_getSelectedIndex},
        {"javax/microedition/lcdui/List", "setSelectedIndex", "(IZ)V", native_list_setSelectedIndex},
        {"javax/microedition/lcdui/List", "delete", "(I)V", native_list_delete},
        {"javax/microedition/lcdui/List", "set", "(ILjava/lang/String;Ljavax/microedition/lcdui/Image;)V", native_list_set},
        {"javax/microedition/lcdui/List", "size", "()I", native_list_size},
        {"javax/microedition/lcdui/List", "getString", "(I)Ljava/lang/String;", native_list_getString},
        {"javax/microedition/lcdui/List", "isSelected", "(I)Z", native_choice_isSelected},
        
        /* Alert methods */
        /* v34.3: single-title ctor was previously shadowed by a no-op stub
         * bytecode constructor; without it the error dialog showed an empty
         * Alert (title/text lost). */
        {"javax/microedition/lcdui/Alert", "<init>", "(Ljava/lang/String;)V", native_alert_init},
        {"javax/microedition/lcdui/Alert", "<init>", "(Ljava/lang/String;Ljava/lang/String;Ljavax/microedition/lcdui/Image;Ljavax/microedition/lcdui/AlertType;)V", native_alert_init},
        {"javax/microedition/lcdui/Alert", "setString", "(Ljava/lang/String;)V", native_alert_setString},
        {"javax/microedition/lcdui/Alert", "getString", "()Ljava/lang/String;", native_alert_getString},
        {"javax/microedition/lcdui/Alert", "setTitle", "(Ljava/lang/String;)V", native_alert_setTitle},
        {"javax/microedition/lcdui/Alert", "getTitle", "()Ljava/lang/String;", native_alert_getTitle},
        {"javax/microedition/lcdui/Alert", "setTimeout", "(I)V", native_alert_setTimeout},
        {"javax/microedition/lcdui/Alert", "getTimeout", "()I", native_alert_getTimeout},
        
        /* TextBox methods */
        {"javax/microedition/lcdui/TextBox", "<init>", "(Ljava/lang/String;Ljava/lang/String;II)V", native_textbox_init},
        {"javax/microedition/lcdui/TextBox", "setString", "(Ljava/lang/String;)V", native_textbox_setString},
        {"javax/microedition/lcdui/TextBox", "getString", "()Ljava/lang/String;", native_textbox_getString},
        {"javax/microedition/lcdui/TextBox", "setTitle", "(Ljava/lang/String;)V", native_textbox_setTitle},
        {"javax/microedition/lcdui/TextBox", "getTitle", "()Ljava/lang/String;", native_textbox_getTitle},
        {"javax/microedition/lcdui/TextBox", "getMaxSize", "()I", native_textbox_getMaxSize},
        {"javax/microedition/lcdui/TextBox", "getConstraints", "()I", native_textbox_getConstraints},
        
        /* Spacer methods */
        {"javax/microedition/lcdui/Spacer", "<init>", "(II)V", native_spacer_init},
        {"javax/microedition/lcdui/Spacer", "setMinimumSize", "(II)V", native_spacer_setMinimumSize},
        
        /* ImageItem methods */
        {"javax/microedition/lcdui/ImageItem", "<init>", "(Ljava/lang/String;Ljavax/microedition/lcdui/Image;ILjava/lang/String;)V", native_imageitem_init},
        {"javax/microedition/lcdui/ImageItem", "setImage", "(Ljavax/microedition/lcdui/Image;)V", native_imageitem_setImage},
        
        /* Command methods */
        {"javax/microedition/lcdui/Command", "<init>", "(Ljava/lang/String;II)V", native_command_init},
        {"javax/microedition/lcdui/Command", "getCommandType", "()I", native_command_getCommandType},
        {"javax/microedition/lcdui/Command", "getLabel", "()Ljava/lang/String;", native_command_getLabel},
        {"javax/microedition/lcdui/Command", "getPriority", "()I", native_command_getPriority},
        
        /* Displayable methods */
        {"javax/microedition/lcdui/Displayable", "addCommand", "(Ljavax/microedition/lcdui/Command;)V", native_displayable_addCommand},
        {"javax/microedition/lcdui/Displayable", "removeCommand", "(Ljavax/microedition/lcdui/Command;)V", native_displayable_removeCommand},
        {"javax/microedition/lcdui/Displayable", "setCommandListener", "(Ljavax/microedition/lcdui/CommandListener;)V", native_displayable_setCommandListener},
        
        /* Ticker methods */
        {"javax/microedition/lcdui/Ticker", "<init>", "(Ljava/lang/String;)V", native_ticker_init},
        {"javax/microedition/lcdui/Ticker", "getString", "()Ljava/lang/String;", native_ticker_getString},
        {"javax/microedition/lcdui/Ticker", "setString", "(Ljava/lang/String;)V", native_ticker_setString},
        
        /* DateField methods */
        {"javax/microedition/lcdui/DateField", "<init>", "(Ljava/lang/String;I)V", native_datefield_init},

        /* v23: MIDP 2.0 Item API */
        {"javax/microedition/lcdui/Item", "setLayout", "(I)V", native_item_setLayout_void},
        {"javax/microedition/lcdui/Item", "setLayout", "(I)I", native_item_setLayout},
        {"javax/microedition/lcdui/Item", "getLayout", "()I", native_item_getLayout},
        {"javax/microedition/lcdui/Item", "setPreferredSize", "(II)V", native_item_setPreferredSize},
        {"javax/microedition/lcdui/Item", "getPreferredWidth", "(I)I", native_item_getPreferredWidth},
        {"javax/microedition/lcdui/Item", "getPreferredHeight", "(I)I", native_item_getPreferredHeight},
        {"javax/microedition/lcdui/Item", "getMinimumWidth", "()I", native_item_getMinimumWidth},
        {"javax/microedition/lcdui/Item", "getMinimumHeight", "()I", native_item_getMinimumHeight},
        {"javax/microedition/lcdui/Item", "setDefaultCommand", "(Ljavax/microedition/lcdui/Command;)V", native_item_setDefaultCommand},
        {"javax/microedition/lcdui/Item", "getDefaultCommand", "()Ljavax/microedition/lcdui/Command;", native_item_getDefaultCommand},
        {"javax/microedition/lcdui/Item", "setItemCommandListener", "(Ljavax/microedition/lcdui/ItemCommandListener;)V", native_item_setItemCommandListener},
        {"javax/microedition/lcdui/Item", "notifyStateChanged", "()V", native_item_notifyStateChanged},
        {"javax/microedition/lcdui/StringItem", "<init>", "(Ljava/lang/String;Ljava/lang/String;I)V", native_stringitem_init3},
        {"javax/microedition/lcdui/Gauge", "setMaxValue", "(I)V", native_gauge_setMaxValue},
        {"javax/microedition/lcdui/Gauge", "getMaxValue", "()I", native_gauge_getMaxValue},
        {"javax/microedition/lcdui/Gauge", "isInteractive", "()Z", native_gauge_isInteractive},
        {"javax/microedition/lcdui/DateField", "setDate", "(J)V", native_datefield_setDate},
        {"javax/microedition/lcdui/DateField", "getDate", "()J", native_datefield_getDate},
        {"javax/microedition/lcdui/DateField", "setInputMode", "(I)V", native_datefield_setInputMode},
        {"javax/microedition/lcdui/DateField", "getInputMode", "()I", native_datefield_getInputMode},
        {"javax/microedition/lcdui/Alert", "setImage", "(Ljavax/microedition/lcdui/Image;)V", native_alert_setImage},
        {"javax/microedition/lcdui/Alert", "getImage", "()Ljavax/microedition/lcdui/Image;", native_alert_getImage},
        {"javax/microedition/lcdui/Alert", "setType", "(Ljavax/microedition/lcdui/AlertType;)V", native_alert_setType},
        {"javax/microedition/lcdui/Alert", "getType", "()Ljavax/microedition/lcdui/AlertType;", native_alert_getType},
        {"javax/microedition/lcdui/Command", "<init>", "(Ljava/lang/String;Ljava/lang/String;II)V", native_command_init4},
        {"javax/microedition/lcdui/Command", "getLongLabel", "()Ljava/lang/String;", native_command_getLongLabel},
        /* v35: API-completeness batch */
        {"javax/microedition/lcdui/Form", "append", "(Ljava/lang/String;)I", native_form_append_string},
        {"javax/microedition/lcdui/Form", "append", "(Ljavax/microedition/lcdui/Image;)I", native_form_append_image},
        {"javax/microedition/lcdui/Form", "set", "(ILjavax/microedition/lcdui/Item;)V", native_form_set},
        {"javax/microedition/lcdui/Form", "insert", "(ILjavax/microedition/lcdui/Item;)V", native_form_insert},
        {"javax/microedition/lcdui/Item", "addCommand", "(Ljavax/microedition/lcdui/Command;)V", native_item_addCommand},
        {"javax/microedition/lcdui/Item", "removeCommand", "(Ljavax/microedition/lcdui/Command;)V", native_item_removeCommand},
        {"javax/microedition/lcdui/Item", "getItemCommandListener", "()Ljavax/microedition/lcdui/ItemCommandListener;", native_item_getItemCommandListener},
        {"javax/microedition/lcdui/StringItem", "setFont", "(Ljavax/microedition/lcdui/Font;)V", native_stringitem_setFont},
        {"javax/microedition/lcdui/StringItem", "getFont", "()Ljavax/microedition/lcdui/Font;", native_stringitem_getFont},
        {"javax/microedition/lcdui/List", "insert", "(ILjava/lang/String;Ljavax/microedition/lcdui/Image;)V", native_list_insert},
        {"javax/microedition/lcdui/List", "setSelectCommand", "(Ljavax/microedition/lcdui/Command;)V", native_list_setSelectCommand},
        {"javax/microedition/lcdui/List", "setDefaultCommand", "(Ljavax/microedition/lcdui/Command;)V", native_list_setDefaultCommand},
        {"javax/microedition/lcdui/List", "getFitPolicy", "()I", native_list_getFitPolicy},
        {"javax/microedition/lcdui/List", "setFitPolicy", "(I)V", native_list_setFitPolicy},
        {"javax/microedition/lcdui/ChoiceGroup", "delete", "(I)V", native_list_delete},
        {"javax/microedition/lcdui/ChoiceGroup", "insert", "(ILjava/lang/String;Ljavax/microedition/lcdui/Image;)V", native_list_insert},
        {"javax/microedition/lcdui/ChoiceGroup", "set", "(ILjava/lang/String;Ljavax/microedition/lcdui/Image;)V", native_list_set},
        {"javax/microedition/lcdui/TextBox", "insert", "(Ljava/lang/String;I)V", native_textbox_insert},
        {"javax/microedition/lcdui/TextBox", "delete", "(II)V", native_textbox_delete},
        {"javax/microedition/lcdui/TextBox", "setChars", "([CII)V", native_textbox_setChars},
        {"javax/microedition/lcdui/TextBox", "size", "()I", native_textbox_size},
        {"javax/microedition/lcdui/TextBox", "getCaretPosition", "()I", native_textbox_getCaretPosition},
        {"javax/microedition/lcdui/TextBox", "setInitialInputMode", "(Ljava/lang/String;)V", native_textbox_setInitialInputMode},
        {"javax/microedition/lcdui/Displayable", "setTitle", "(Ljava/lang/String;)V", native_displayable_setTitle},
        {"javax/microedition/lcdui/Displayable", "getTitle", "()Ljava/lang/String;", native_displayable_getTitle},
        {"javax/microedition/lcdui/Displayable", "setTicker", "(Ljavax/microedition/lcdui/Ticker;)V", native_displayable_setTicker},
        {"javax/microedition/lcdui/Displayable", "getTicker", "()Ljavax/microedition/lcdui/Ticker;", native_displayable_getTicker},
        {"javax/microedition/lcdui/Screen", "getTitle", "()Ljava/lang/String;", native_displayable_getTitle},
        {"javax/microedition/lcdui/Screen", "setTitle", "(Ljava/lang/String;)V", native_displayable_setTitle},
        {"javax/microedition/lcdui/ChoiceGroup", "getSelectedFlags", "([Z)I", native_choice_getSelectedFlags},
        {"javax/microedition/lcdui/ChoiceGroup", "setSelectedFlags", "([Z)V", native_choice_setSelectedFlags},
        {"javax/microedition/lcdui/ChoiceGroup", "getImage", "(I)Ljavax/microedition/lcdui/Image;", native_choice_getImage},
        {"javax/microedition/lcdui/List", "getSelectedFlags", "([Z)I", native_choice_getSelectedFlags},
        {"javax/microedition/lcdui/List", "setSelectedFlags", "([Z)V", native_choice_setSelectedFlags},
        {"javax/microedition/lcdui/List", "getImage", "(I)Ljavax/microedition/lcdui/Image;", native_choice_getImage},
    };
    
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
    
    FORM_DEBUG("Registered %zu UI native methods", sizeof(methods) / sizeof(methods[0]));
    
    /* Initialize List.SELECT_COMMAND static field */
    JavaClass* list_class = jvm_load_class(jvm, "javax/microedition/lcdui/List");
    if (list_class && list_class->static_fields_count > 0) {
        for (int i = 0; i < list_class->static_fields_count; i++) {
            if (list_class->static_fields[i].name && 
                strcmp(list_class->static_fields[i].name, "SELECT_COMMAND") == 0) {
                /* Create SELECT_COMMAND = new Command("", 4, 0) */
                JavaClass* cmd_class = jvm_load_class(jvm, "javax/microedition/lcdui/Command");
                if (cmd_class) {
                    JavaObject* select_cmd = jvm_new_object(jvm, cmd_class);
                    if (select_cmd && OBJECT_HAS_FIELDS(select_cmd, 3)) {
                        JavaString* empty_label = jvm_new_string(jvm, "");
                        select_cmd->fields[0].ref = empty_label;  /* label */
                        select_cmd->fields[1].i = 4;              /* SCREEN type */
                        select_cmd->fields[2].i = 0;              /* priority */
                        
                        list_class->static_fields[i].value.ref = select_cmd;
                        FORM_DEBUG("Initialized List.SELECT_COMMAND = %p", (void*)select_cmd);
                    }
                }
                break;
            }
        }
    }
}

/* ============================================================================
 * v35.08 MULTI-SESSION: per-session static reset (midp_session_reset <-
 * jvm_destroy). The four rooted globals are NULLed by the central root wipe
 * as well; the latches/indices/ticker table/virtual keyboard here are NOT
 * reachable by it and would leak the previous game's state into the next.
 * ============================================================================ */
void midp_form_session_reset(void) {
    g_current_form = NULL;
    g_current_displayable = NULL;
    g_select_command = NULL;       /* SELECT_COMMAND singleton - session object */
    g_item_state_listener = NULL;

    g_focused_item_index = 0;
    g_list_selected_index = 0;
    g_cg_focused_element = 0;
    g_form_scroll = 0;
    g_form_doc_h = 0;
    form_item_menu_close_raw();   /* v36.30 [ITEM-CMDS] */

    /* Ticker table held displayable/ticker object pairs of the old session */
    for (int i = 0; i < TICKER_TABLE_MAX; i++) {
        g_ticker_table[i].displayable = NULL;
        g_ticker_table[i].ticker = NULL;
    }
    g_ticker_table_count = 0;

    /* Virtual keyboard: hide + drop object references of the old session */
    g_vkb.active = false;
    g_vkb.selected_char = 0;
    g_vkb.text_buffer[0] = '\0';
    g_vkb.text_length = 0;
    g_vkb.cursor_pos = 0;
    g_vkb.target_item = NULL;
    g_vkb.displayable = NULL;
}

/* ============================================
 * v36.26 [TOUCH-UI]: touch for the MIDP high-level UI
 * Тап по стандартному джава-интерфейсу мидлета: виртуальная клавиатура,
 * List, Form (TextField/ChoiceGroup/Gauge), TextBox, Alert. Все функции
 * действуют НА НАЖАТИИ (down edge);.drag/release глотается вызывающим
 * (display.c midp_ui_touch_hold), поэтому двойных срабатываний нет.
 * ============================================ */

/* --- virtual keyboard: tap on the on-screen grid --- */
bool midp_vkb_touch(JVM* jvm, int x, int y) {
    if (!g_vkb.active) return false;
    MidpGraphics* gfx = get_screen_graphics();
    if (!gfx) return true; /* keyboard is up: swallow regardless */
    int w = gfx->width, h = gfx->height;
    int kb_y = h - 160;
    int grid_y = kb_y + 30; /* text field occupies kb_y+5 .. kb_y+25 */
    int cell_w = (w - 10) / VK_CHARS_PER_ROW;
    int cell_h = 16;
    if (cell_w <= 0) return true;
    if (y >= grid_y && y < grid_y + 5 * cell_h &&
        x >= 5 && x < 5 + cell_w * VK_CHARS_PER_ROW) {
        int col = (x - 5) / cell_w;
        int row = (y - grid_y) / cell_h;
        int idx = row * VK_CHARS_PER_ROW + col;
        /* the last row is ragged (48 = 4*10 + 8): empty cells do nothing */
        if (idx >= 0 && idx < VK_CHAR_COUNT &&
            col < (VK_CHAR_COUNT - row * VK_CHARS_PER_ROW)) {
            g_vkb.selected_char = idx;
            vkb_handle_key(jvm, 8); /* FIRE on the tapped cell */
        }
    }
    /* any tap while the keyboard is up belongs to the keyboard — never
     * leak to the canvas as a pointer event */
    return true;
}

/* --- List: tap = select a row (IMPLICIT: + SELECT_COMMAND, как FIRE) --- */
bool midp_list_touch(JVM* jvm, int x, int y) {
    (void)x; /* строки во всю ширину экрана */
    if (!g_current_displayable) return false;
    if (midp_displayable_kind(g_current_displayable) != MIDP_UI_KIND_LIST) return false;
    JavaArray* strings = NULL;
    int strings_slot = find_object_field_slot(g_current_displayable, "strings");
    if (strings_slot >= 0 && OBJECT_HAS_FIELDS(g_current_displayable, strings_slot + 1))
        strings = (JavaArray*)g_current_displayable->fields[strings_slot].ref;
    if (!strings) return false;
    MidpGraphics* gfx = get_screen_graphics();
    if (!gfx) return false;
    int font_height = 12;
    MidpFont* font = midp_font_get_default();
    if (font) font_height = midp_font_height(font);
    int row_h = font_height + 6;   /* render_list: y += font_height + 6 */
    int top = 30;                  /* render_list: first row at y=30 */
    if (y < top) return false;
    int idx = (y - top) / row_h;
    if (idx >= strings->length) return false; /* ниже последней строки */

    int list_type = CHOICE_IMPLICIT;
    int lt_slot = find_object_field_slot(g_current_displayable, "listType");
    if (lt_slot >= 0 && OBJECT_HAS_FIELDS(g_current_displayable, lt_slot + 1))
        list_type = g_current_displayable->fields[lt_slot].i;

    if (list_type == CHOICE_EXCLUSIVE || list_type == CHOICE_MULTIPLE ||
        list_type == CHOICE_POPUP) {
        /* EXCLUSIVE/MULTIPLE/POPUP: тап = выбор элемента (FIRE шлёт команду) */
        JavaArray* selected = NULL;
        int sel_slot = find_object_field_slot(g_current_displayable, "selected");
        if (sel_slot >= 0 && OBJECT_HAS_FIELDS(g_current_displayable, sel_slot + 1))
            selected = (JavaArray*)g_current_displayable->fields[sel_slot].ref;
        if (selected && selected->element_type == T_BOOLEAN) {
            jboolean* sd = (jboolean*)array_data(selected);
            if (list_type == CHOICE_EXCLUSIVE || list_type == CHOICE_POPUP) {
                for (int i = 0; i < selected->length; i++) sd[i] = (i == idx) ? 1 : 0;
            } else {
                sd[idx] = !sd[idx];
            }
        }
        g_list_selected_index = idx;
        midp_render_list(jvm, g_current_displayable, idx);
        return true;
    }

    /* IMPLICIT: тап = выбрать строку и выполнить SELECT_COMMAND (как FIRE) */
    g_list_selected_index = idx;
    midp_render_list(jvm, g_current_displayable, idx);
    return midp_form_handle_key(jvm, 8);
}

/* --- TextBox: тап открывает виртуальную клавиатуру (как FIRE) --- */
bool midp_textbox_touch(JVM* jvm, int x, int y) {
    (void)x; (void)y;
    if (!g_current_displayable) return false;
    if (midp_displayable_kind(g_current_displayable) != MIDP_UI_KIND_TEXTBOX) return false;
    if (g_vkb.active) return true;
    int max_size = 32, constraints = 0;
    int maxsize_slot = find_object_field_slot(g_current_displayable, "maxSize");
    int constraints_slot = find_object_field_slot(g_current_displayable, "constraints");
    if (maxsize_slot >= 0 && constraints_slot >= 0 &&
        OBJECT_HAS_FIELDS(g_current_displayable, constraints_slot + 1)) {
        max_size = g_current_displayable->fields[maxsize_slot].i;
        constraints = g_current_displayable->fields[constraints_slot].i;
    }
    vkb_start(g_current_displayable, max_size, constraints);
    midp_render_textbox(jvm, g_current_displayable, g_vkb.text_buffer, g_vkb.cursor_pos);
    return true;
}

/* --- Form: layout mirror of midp_render_form + tap hit-test --- */
typedef struct {
    int item;        /* item index (>=0) or -1 — no hit */
    int elem;        /* ChoiceGroup element row or -1 */
    int gauge_bar_y; /* Gauge: bar top (value from tap x) */
} FormTouchHit;

/* Same field lookups and y accumulation as midp_render_form (item pass).
 * Returns 1 if (tap_x, tap_y) lands on an item row; hit->item >= 0. */
static int form_touch_layout(JVM* jvm, JavaObject* form,
                             int tap_x, int tap_y, FormTouchHit* out) {
    (void)jvm;
    (void)tap_x; /* разметка строк зависит только от y; x использует вызывающий */
    MidpGraphics* gfx = get_screen_graphics();
    if (!gfx) return 0;
    int w = gfx->width, h = gfx->height;

    int items_slot = find_object_field_slot(form, "items");
    if (items_slot < 0 || !OBJECT_HAS_FIELDS(form, items_slot + 1)) return 0;
    JavaArray* items = (JavaArray*)form->fields[items_slot].ref;
    if (!items || items->element_type != DESC_OBJECT) return 0;

    int font_height = 12;
    MidpFont* font = midp_font_get_default();
    if (font) font_height = midp_font_height(font);

    int max_y = g_vkb.active ? (h - 170) : (h - 30);
    int content_top = 21;
    if (tap_y < content_top || tap_y >= max_y) return 0;

    int y = 25 - g_form_scroll; /* render_form: y = 25 - scroll */
    void** items_data = (void**)array_data(items);

    out->item = -1;
    out->elem = -1;
    out->gauge_bar_y = 0;

    for (int i = 0; i < items->length; i++) {
        JavaObject* item = (JavaObject*)items_data[i];
        if (!item) continue;
        int item_y0 = y;
        int item_type = get_item_type(item);

        switch (item_type) {
            case ITEM_STRINGITEM: {
                const char* label = "";
                const char* text = "";
                int label_slot = find_object_field_slot(item, "label");
                int text_slot = find_object_field_slot(item, "text");
                int max_slot = (label_slot > text_slot) ? label_slot : text_slot;
                if (label_slot >= 0 && text_slot >= 0 &&
                    OBJECT_HAS_FIELDS(item, max_slot + 1)) {
                    JavaString* ls = (JavaString*)item->fields[label_slot].ref;
                    JavaString* ts = (JavaString*)item->fields[text_slot].ref;
                    if (ls) label = get_string_from_object(jvm, (JavaObject*)ls);
                    if (ts) text = get_string_from_object(jvm, (JavaObject*)ts);
                }
                int wrap_width = w - 10; /* render_stringitem: 5px margins */
                if (label && *label) {
                    y = draw_wrapped_text(NULL, font, label, 5, y, wrap_width, 0) + 2;
                }
                if (text && *text) {
                    y = draw_wrapped_text(NULL, font, text, 5, y, wrap_width, 0) + 4;
                }
                break;
            }
            case ITEM_IMAGEITEM: {
                const char* label = "";
                int label_slot = find_object_field_slot(item, "label");
                if (label_slot >= 0 && OBJECT_HAS_FIELDS(item, label_slot + 1) &&
                    item->fields[label_slot].ref) {
                    label = get_string_from_object(jvm,
                        (JavaObject*)item->fields[label_slot].ref);
                }
                if (label && *label) y += font_height + 2;
                JavaObject* image_obj = NULL;
                int image_slot = find_object_field_slot(item, "image");
                if (image_slot >= 0 && OBJECT_HAS_FIELDS(item, image_slot + 1))
                    image_obj = (JavaObject*)item->fields[image_slot].ref;
                MidpImage* img = NULL;
                if (image_obj) {
                    extern MidpImage* get_image_from_object(JavaObject* obj);
                    img = get_image_from_object(image_obj);
                }
                if (img && img->width > 0 && img->height > 0) y += img->height + 4;
                else y += font_height + 4;
                break;
            }
            case ITEM_TEXTFIELD: {
                const char* label = "";
                int label_slot = find_object_field_slot(item, "label");
                if (label_slot >= 0 && OBJECT_HAS_FIELDS(item, label_slot + 1) &&
                    item->fields[label_slot].ref) {
                    label = get_string_from_object(jvm,
                        (JavaObject*)item->fields[label_slot].ref);
                }
                if (label && *label) y += font_height + 2;
                y += font_height + 8 + 6; /* box_height + 6 (render_textfield) */
                break;
            }
            case ITEM_CHOICEGROUP: {
                const char* label = "";
                int label_slot = find_object_field_slot(item, "label");
                if (label_slot >= 0 && OBJECT_HAS_FIELDS(item, label_slot + 1) &&
                    item->fields[label_slot].ref) {
                    label = get_string_from_object(jvm,
                        (JavaObject*)item->fields[label_slot].ref);
                }
                int label_h = 0;
                if (label && *label) { y += font_height + 4; label_h = font_height + 4; }
                int n = cg_element_count(item);
                y += n * (font_height + 4) + 4;
                /* element row under the tap */
                if (tap_y >= item_y0 + label_h) {
                    int e = (tap_y - (item_y0 + label_h)) / (font_height + 4);
                    if (e >= 0 && e < n) out->elem = e;
                }
                break;
            }
            case ITEM_GAUGE: {
                const char* label = "";
                int label_slot = find_object_field_slot(item, "label");
                if (label_slot >= 0 && OBJECT_HAS_FIELDS(item, label_slot + 1) &&
                    item->fields[label_slot].ref) {
                    label = get_string_from_object(jvm,
                        (JavaObject*)item->fields[label_slot].ref);
                }
                if (label && *label) y += font_height + 4;
                out->gauge_bar_y = y;      /* bar_y (bar_height = 20) */
                y += 20 + 8;               /* bar_height + 8 (render_gauge) */
                break;
            }
            case ITEM_SPACER: {
                int height = 10;
                int height_slot = find_object_field_slot(item, "height");
                if (height_slot >= 0 && OBJECT_HAS_FIELDS(item, height_slot + 1) &&
                    item->fields[height_slot].i > 0)
                    height = item->fields[height_slot].i;
                y += height;
                break;
            }
            default:
                y += 20; /* render_form default branch */
                break;
        }

        if (tap_y >= item_y0 && tap_y < y && out->item < 0) {
            out->item = i;
        }
        if (y >= max_y + g_form_scroll && out->item >= 0) break; /* below the fold */
    }
    return out->item >= 0;
}

bool midp_form_touch(JVM* jvm, int x, int y) {
    if (!g_current_displayable) return false;
    if (midp_displayable_kind(g_current_displayable) != MIDP_UI_KIND_FORM) return false;

    FormTouchHit hit;
    if (!form_touch_layout(jvm, g_current_displayable, x, y, &hit)) return false;

    int items_slot = find_object_field_slot(g_current_displayable, "items");
    if (items_slot < 0 || !OBJECT_HAS_FIELDS(g_current_displayable, items_slot + 1)) return false;
    JavaArray* items = (JavaArray*)g_current_displayable->fields[items_slot].ref;
    if (!items || items->element_type != DESC_OBJECT) return false;
    void** items_data = (void**)array_data(items);
    if (hit.item >= items->length) return false;
    JavaObject* item = (JavaObject*)items_data[hit.item];
    if (!item) return false;

    /* v36.30 [ITEM-CMDS]: an item WITH item commands is tappable even when
     * it is not "interactive" in the focus sense (StringItem buttons of
     * info-midlets). Focus it (so the softbar shows its commands) and fire
     * the default/first command; 3+ commands open the item command menu.
     * Interactive types keep their own tap semantics below (their item
     * commands stay reachable via the soft keys). */
    {
        int t_type = get_item_type(item);
        if (t_type != ITEM_TEXTFIELD && t_type != ITEM_CHOICEGROUP &&
            t_type != ITEM_GAUGE) {
            JavaObject* tcmds[8];
            int tn = form_item_cmd_effective(item, tcmds, 8);
            if (tn > 0) {
                g_focused_item_index = hit.item;
                if (tn >= 3) {
                    form_item_menu_open_for(item);
                } else {
                    form_item_fire_command(jvm, item, tcmds[0]);
                }
                midp_render_form(jvm, g_current_displayable);
                return true;
            }
        }
    }

    /* labels/спейсеры фокуса не держат: тап мимо интерактивного — ничего */
    if (!item_is_interactive(item)) return false;

    int item_type = get_item_type(item);
    bool already = (hit.item == g_focused_item_index);

    if (item_type == ITEM_TEXTFIELD) {
        if (already) {
            /* как FIRE у focused TextField: открыть виртуальную клавиатуру */
            int max_size = 32, constraints = 0;
            int maxsize_slot = find_object_field_slot(item, "maxSize");
            int constraints_slot = find_object_field_slot(item, "constraints");
            if (maxsize_slot >= 0 && constraints_slot >= 0 &&
                OBJECT_HAS_FIELDS(item, constraints_slot + 1)) {
                max_size = item->fields[maxsize_slot].i;
                constraints = item->fields[constraints_slot].i;
            }
            vkb_start(item, max_size, constraints);
            midp_render_form(jvm, g_current_displayable);
            return true;
        }
        g_focused_item_index = hit.item;
        midp_render_form(jvm, g_current_displayable);
        return true;
    }

    if (item_type == ITEM_CHOICEGROUP) {
        int n = cg_element_count(item);
        if (n <= 0) return false;
        if (!already) {
            g_focused_item_index = hit.item;
            g_cg_focused_element = (hit.elem >= 0 && hit.elem < n) ? hit.elem : 0;
            midp_render_form(jvm, g_current_displayable);
            return true;
        }
        if (hit.elem >= 0 && hit.elem != g_cg_focused_element) {
            g_cg_focused_element = hit.elem; /* перенос курсора элемента */
            midp_render_form(jvm, g_current_displayable);
            return true;
        }
        /* тап по УЖЕ выделенному элементу = выбор/переключение (как FIRE) */
        if (hit.elem < 0) return true;
        int choicetype_slot = find_object_field_slot(item, "choiceType");
        int selected_slot = find_object_field_slot(item, "selected");
        if (choicetype_slot < 0 || selected_slot < 0 ||
            !OBJECT_HAS_FIELDS(item, selected_slot + 1)) return true;
        int choice_type = item->fields[choicetype_slot].i;
        JavaArray* selected = (JavaArray*)item->fields[selected_slot].ref;
        int sel_count = selected ? selected->length : 0;
        if (!selected || sel_count <= 0) return true;
        jboolean* sel_data = (jboolean*)array_data(selected);
        int elem = g_cg_focused_element % sel_count;
        if (choice_type == CHOICE_EXCLUSIVE || choice_type == CHOICE_POPUP) {
            for (int si = 0; si < sel_count; si++) sel_data[si] = (si == elem) ? 1 : 0;
        } else {
            sel_data[elem] = !sel_data[elem];
        }
        if (g_item_state_listener && g_item_state_listener->header.clazz) {
            JavaMethod* ism = jvm_resolve_method(jvm,
                g_item_state_listener->header.clazz,
                "itemStateChanged", "(Ljavax/microedition/lcdui/Item;)V");
            if (ism) {
                JavaValue ism_args[2];
                ism_args[0].ref = g_item_state_listener;
                ism_args[1].ref = item;
                JavaValue ism_result;
                execute_method(jvm, jvm_current_thread(jvm), ism, ism_args, &ism_result);
            }
        }
        midp_render_form(jvm, g_current_displayable);
        return true;
    }

    if (item_type == ITEM_GAUGE) {
        if (!already) {
            g_focused_item_index = hit.item;
            midp_render_form(jvm, g_current_displayable);
            return true;
        }
        /* тап по полосе = установить значение по позиции пальца */
        MidpGraphics* gfx = get_screen_graphics();
        int maxvalue_slot = find_object_field_slot(item, "maxValue");
        int value_slot = find_object_field_slot(item, "value");
        if (gfx && maxvalue_slot >= 0 && value_slot >= 0 &&
            OBJECT_HAS_FIELDS(item, value_slot + 1)) {
            int max_value = item->fields[maxvalue_slot].i;
            int bar_w = gfx->width - 20;
            if (max_value > 0 && bar_w > 0 && hit.gauge_bar_y > 0 &&
                y >= hit.gauge_bar_y - 2 && y < hit.gauge_bar_y + 22) {
                int v = (x - 10) * max_value / bar_w;
                if (v < 0) v = 0;
                if (v > max_value) v = max_value;
                item->fields[value_slot].i = v;
                form_notify_item_state_changed(jvm, item);
                midp_render_form(jvm, g_current_displayable);
                return true;
            }
        }
        return true; /* тап по метке gauge — просто фокус */
    }

    return false;
}
