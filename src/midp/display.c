/*
 * J2ME Emulator - MIDP2 Display and Game Canvas
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE  /* For strdup */
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <limits.h>  /* INT_MAX (v34.27 keyq age) */
#include <errno.h>  /* For ETIMEDOUT */
#include <time.h>   /* v50 DIAG: clock_gettime for NOJME_PAINTTIME */

#include "midp.h"
#include "render/render.h"  /* v34.20: midp_blit_rgb / surface blits */
#include "jvm.h"
#include "native.h"

/* v29: paint-depth helpers (defined near midp_process_repaints) */
static void midp_paint_depth_inc(void);
static void midp_paint_depth_dec(void);
#include "heap.h"
#include "sdl_backend.h"
#include "threads.h"
#include "opcodes.h"  /* For ACC_PRIVATE */
#include "debug.h"
#include "bitmap_font.h"  /* For FONT_WIDTH */
#include "jar_reader.h"   /* v34.71: jar_has_entry for the missing-cache guard */
#include "race_probe.h"    /* v36.59 [RACE]: зонд Rally 3D */

/* v35.02 logic-heartbeat counters (defined in switch_trace.c; SDL-family
 * builds only — desktop builds get local no-op shims so display.c compiles
 * and links unchanged there). */
#if defined(__SWITCH__) || defined(NOJME_SWITCH_TRACE)
extern volatile uint32_t g_sd_pp, g_sd_pump_skip, g_sd_csq;
#else
static volatile uint32_t g_sd_pp, g_sd_pump_skip, g_sd_csq;
#endif

/* ИСПРАВЛЕНИЕ: Синхронизация paint() между потоками выполняется
 * процесс-глобальным рекурсивным локом g_midp_ui_lock (см. midp_process_repaints).
 * Старые mutex/cond переменные удалены как неиспользуемые. */
#ifndef _WIN32
#include <pthread.h>
#else
#include <windows.h>
#include "win_thread_shim.h"   /* v58: key-hang watchdog thread (CreateThread) */
#endif

/* v34.10: these prototypes moved OUT of the POSIX-only branch — they are
 * shared API (execute.c's deferred-key pump calls midp_process_pending_keys
 * -> midp_call_keyXXX_impl on Windows too). Leaving them inside
 * #ifndef _WIN32 made the Windows build hit implicit-declaration +
 * conflicting-types errors in midp_process_pending_keys(). */
int midp_defer_keys_enabled(void);
int midp_keyq_peek_count(void);
/* ==== v58 key-handler hang watchdog ====================================
 * midp_call_keyPressed executes game Java code on the harness thread. When
 * that code blocks forever inside a native (Coaster Rush: first key press
 * on the country-select menu), the harness only sees "-> displayable" with
 * no "executed". This watchdog dumps the blocked Java stack after 3s. */
#include <time.h>
static JavaThread* volatile g_key_hang_thread = NULL;
static uint64_t g_key_hang_start_ms = 0;
static volatile int g_key_hang_dumped = 0;

/* v36.02 EXIT-DAEMON-JOIN: the watchdog used to be created DETACHED and ran
 * forever — on HOS it outlived _exit(0) (which returns to HBmenu through
 * the loader unmap instead of killing threads atomically) and its next
 * 500 ms wake executed unmapped NRO code (2168-0001 Instruction Abort,
 * "hbloader" crash report: X20=500000000, twin of the 100 ms eval thread
 * X0/X1=100000000 in threads.c). Now: stop flag + joinable handle; main()
 * joins it before the raw exit. */
static volatile int g_key_hang_quit = 0;
static pthread_t g_key_hang_pthread;
static int g_key_hang_have = 0;

static uint64_t key_hang_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static void key_hang_watchdog_run(void) {
    /* v36.02 EXIT-DAEMON-JOIN: bounded lifetime — 5x100 ms sleeps instead
     * of one 500 ms sleep, so the exit-time join never waits longer than
     * ~100 ms after the stop flag is set. */
    while (!g_key_hang_quit) {
#ifdef _WIN32
        /* v34.66: Win32-native sleep — no nanosleep dependency on plain
         * MinGW toolchains that lack the POSIX.1-1993 wrappers. */
        Sleep(100);
#else
        struct timespec wts = { .tv_sec = 0, .tv_nsec = 100 * 1000 * 1000 };
        nanosleep(&wts, NULL);
#endif
        if (g_key_hang_quit) break;
        JavaThread* t = g_key_hang_thread;
        if (t && !g_key_hang_dumped &&
            g_key_hang_start_ms && key_hang_now_ms() - g_key_hang_start_ms > 3000) {
            g_key_hang_dumped = 1;
            ALWAYS_LOG("[KEY-HANG] keyPressed blocked >3s. Java stack:\n");
            JavaFrame* f = t->current_frame;
            int n = 0;
            while (f && n < 24) {
                ALWAYS_LOG("  [%d] %s.%s%s\n", n,
                    (f->clazz && f->clazz->class_name) ? f->clazz->class_name : "?",
                    f->method ? f->method->name : "?",
                    f->method ? f->method->descriptor : "");
                f = f->prev;
                n++;
            }
            if (!f && n == 0) ALWAYS_LOG("  (no frames - thread idle at C level)\n");
        }
    }
}

#ifndef _WIN32
static void* key_hang_watchdog(void* arg) {
    (void)arg;
    key_hang_watchdog_run();
    return NULL;
}
#else
static DWORD WINAPI key_hang_watchdog_win(LPVOID arg) {
    (void)arg;
    key_hang_watchdog_run();
    return 0;
}
#endif

void keydiag_hang_begin(JavaThread* t) {
    g_key_hang_start_ms = key_hang_now_ms();
    g_key_hang_thread = t;
    g_key_hang_dumped = 0;
    static int started = 0;
    if (!started) {
        started = 1;
#ifdef _WIN32
        /* v34.66 FIX (user report: MinGW build failed — "implicit
         * declaration of function 'pthread_detach'" here): the watchdog
         * no longer depends on the win_thread_shim.h vintage. Trees that
         * were upgraded by overlaying a newer source drop onto an older
         * extraction can still carry a pre-v34.60 shim without
         * pthread_detach. Raw CreateThread + immediate CloseHandle is
         * exactly pthread_detach semantics (Windows threads are
         * independent of their handles; the shim's own pthread_detach
         * implementation does precisely this). */
        HANDLE hwd = CreateThread(NULL, 0, key_hang_watchdog_win, NULL, 0, NULL);
        if (hwd) CloseHandle(hwd);
#else
        /* v36.02 EXIT-DAEMON-JOIN: keep the handle joinable (was detached).
         * The watchdog lives for the whole process; it is joined once, at
         * the process-exit path (keydiag_hang_shutdown). */
        pthread_t wd;
        if (pthread_create(&wd, NULL, key_hang_watchdog, NULL) == 0) {
            g_key_hang_pthread = wd;
            g_key_hang_have = 1;
        }
#endif
    }
}
void keydiag_hang_end(void) {
    g_key_hang_thread = NULL;
    g_key_hang_start_ms = 0;
}

/* v36.02 EXIT-DAEMON-JOIN: process-exit hook (called from main.c, Switch
 * frontend, BEFORE the SDL teardown and the raw _exit(0)). Sets the stop
 * flag and joins the watchdog. Bounded by the 100 ms sleep granularity.
 * Idempotent. Returns 1 if a live thread was joined, 0 otherwise. */
int keydiag_hang_shutdown(void) {
    int joined = 0;
    g_key_hang_quit = 1;
#ifndef _WIN32
    if (g_key_hang_have &&
        !pthread_equal(g_key_hang_pthread, pthread_self())) {
        pthread_join(g_key_hang_pthread, NULL);
        joined = 1;
    }
    g_key_hang_have = 0;
#endif
    return joined;
}

void midp_call_keyPressed_impl(JVM* jvm, int keycode);
void midp_call_keyReleased_impl(JVM* jvm, int keycode);

static volatile bool g_paint_pending = false;
static volatile bool g_paint_complete = false;

/* ============================================================================
 * v35.02 SINGLE-PUMP PAINT PROTOCOL (the "1 Hz race crawl -> world freeze"
 * field fix, libretro-proof architecture).
 * ============================================================================
 * FIELD EVIDENCE (v35.01 trace + user report): during an M3G race the main
 * picture degraded to ~1 fps from race start and froze after lap 1, while
 * the blur border kept changing. The trace was unambiguous: bn=150/5s
 * (binds fine, bd=1ms), fl=150/5s (settles landing 30/s), presents 60/s,
 * tn=1 (ONE live VM thread), ta<=1ms — i.e. the RENDER pipeline was healthy
 * and produced fresh frames of a world that was NOT ADVANCING. The world
 * logic of Gameloft race loops runs as the callSerially Runnable drained on
 * the FRONTEND thread (midp_process_call_serially_queue); its drain is
 * gated on (g_paint_pending || g_canvas_repaint_requested), and those same
 * non-atomic flags were written CONCURRENTLY by the game thread
 * (serviceRepaints) and the frontend (auto-redraw pump + an UNCONDITIONAL
 * midp_clear_pending_repaint() that even ran after a SKIPPED pump, dropping
 * g_dirty_valid and lying to the game thread's handshake). As the UI-lock /
 * paint occupancy of the game thread grew through the session (gc=0 heap
 * growth), the frontend's logic-drain cadence collapsed: 60/s -> ~1/s -> 0
 * — the race crawled, then froze, while renders kept going. libretro is
 * immune because ONE thread pumps everything (retro_run).
 *
 * FIX: make the Switch/SDL frontends behave like libretro:
 *   - The frontend thread becomes the ONLY paint pump (it already pumps
 *     every iteration). Game threads that call serviceRepaints() set the
 *     request flags and WAIT for the frontend to complete the paint
 *     (bounded, GC-aware), then proceed — the two-phase Gameloft pattern
 *     (bind+clear inside paint, render+release after serviceRepaints)
 *     keeps its ordering guarantee.
 *   - g_midp_paint_done_seq increments every time a pump completes with the
 *     UI lock held; the game thread waits for it to move. A 250 ms bail-out
 *     plus a legacy synchronous-pump fallback keeps the VM alive even if
 *     the frontend stops pumping (headless shutdown, fatal paths).
 *   - The frontend's pending-flag clear becomes CONDITIONAL (only after a
 *     pump that actually painted) — no more dropped dirty regions.
 * libretro never arms the external pump (jvm flag off) -> bit-identical
 * legacy behavior there. */
static volatile uint32_t g_midp_paint_done_seq = 0;  /* bumped per completed pump */
static volatile int g_midp_external_pump_active = 0; /* armed by SDL-family frontends */

/* Called once per SDL-family frontend session (Switch UI, Windows/SDL2,
 * headless) — those loops pump repaints from their own thread every
 * iteration. libretro does NOT call this (it pumps from retro_run on the
 * runner thread itself, where waiting would deadlock until the bail-out). */
void midp_set_external_pump_active(int on) {
    g_midp_external_pump_active = on ? 1 : 0;
}

/* Forward declarations for helper functions */
static jint get_object_field_int(JavaObject* obj, const char* field_name);
static void set_object_field_int(JavaObject* obj, const char* field_name, jint value);
/* get_object_field_ref is now public - declared in midp.h */
void set_object_field_ref(JavaObject* obj, const char* field_name, JavaObject* value);

/* Forward declaration for public helper (defined later) */
MidpImage* get_image_from_object(JavaObject* obj);

/* Sprite transform constants - used by Graphics.drawRegion and Image.createImage */
#define SPRITE_TRANS_NONE           0
#define SPRITE_TRANS_MIRROR_ROT180  1
#define SPRITE_TRANS_MIRROR         2
#define SPRITE_TRANS_ROT180         3
#define SPRITE_TRANS_MIRROR_ROT270  4
#define SPRITE_TRANS_ROT90          5
#define SPRITE_TRANS_ROT270         6
#define SPRITE_TRANS_MIRROR_ROT90   7

/* Forward declaration from execute.c */
extern int execute_method(JVM* jvm, JavaThread* thread, JavaMethod* method, JavaValue* args, JavaValue* result);

/* Forward declarations from mobile3d.c (force-render tracking) */
extern void m3g_reset_paint_tracking(void);
extern bool m3g_needs_force_render(void);
extern void m3g_force_render(JVM* jvm, MidpGraphics* screen_gfx);

/* Global Graphics object - reused for all paint() calls */
static JavaObject* g_graphics_object = NULL;
static MidpGraphics g_screen_graphics;  /* Static Graphics for screen rendering */

/* Current displayable - stored as GC root */
static JavaObject* current_displayable_obj = NULL;

/* Soft button area height */
#define SOFT_BUTTON_HEIGHT 25

/* Full screen mode tracking per displayable */
static bool g_full_screen_mode = false;
/* v34.7 FIX: remember WHICH canvas requested fullscreen via
 * Canvas.setFullScreenMode(true). MIDP treats fullscreen as a property of
 * the Canvas, but Display.setCurrent() used to unconditionally reset the
 * global flag to false, so a game that called setFullScreenMode(true) in
 * its constructor BEFORE setCurrent() (Doom RPG: new k() -> setCurrent)
 * silently lost fullscreen: every setClip was clamped to
 * height - SOFT_BUTTON_HEIGHT and the bottom of the screen was cut. */
static JavaObject* g_fullscreen_canvas_obj = NULL;

/* Command menu state */
static bool g_command_menu_open = false;
static int g_command_menu_selected = 0;

/* Command type constants (from MIDP spec) */
#define CMD_SCREEN  1
#define CMD_BACK    2
#define CMD_CANCEL  3
#define CMD_OK      4
#define CMD_HELP    5
#define CMD_STOP    6
#define CMD_EXIT    7
#define CMD_ITEM    8

/* ============================================================
 * v34.72: emulated screen BACK-navigation stack.
 *
 * Serves the BACK/CANCEL soft keys when the midlet never registered a
 * CommandListener (or the listener could not be invoked): the emulator
 * returns to the PREVIOUS screen instead of terminating the whole
 * application (user report: "cancel exits the list, not the app" — on a
 * real phone a dead Cancel button never kills the MIDlet). Only
 * transitions initiated by the midlet's own Display.setCurrent() are
 * pushed; Alert restores and back-navigation itself do not push, so the
 * stack mirrors the user's navigation path. Alerts are never pushed
 * (going "back" into a dismissed Alert would ping-pong with its
 * auto-restore). Slots are lazily registered as GC roots.
 * ============================================================ */
#define MIDP_BACK_STACK_MAX 8
static JavaObject* g_back_stack[MIDP_BACK_STACK_MAX];
static bool g_back_rooted[MIDP_BACK_STACK_MAX] = {false};
static int g_back_depth = 0;

static void display_set_current_internal(JVM* jvm, JavaObject* displayable,
                                         bool push_history);

static bool displayable_is_alert(JavaObject* obj) {
    JavaClass* c = obj ? obj->header.clazz : NULL;
    while (c) {
        if (c->class_name && strcmp(c->class_name, "javax/microedition/lcdui/Alert") == 0) {
            return true;
        }
        c = c->super_class;
    }
    return false;
}

static void display_back_stack_push(JVM* jvm, JavaObject* disp) {
    if (!jvm || !disp || displayable_is_alert(disp)) return;
    if (g_back_depth > 0 && g_back_stack[g_back_depth - 1] == disp) return; /* dedup */
    if (g_back_depth == MIDP_BACK_STACK_MAX) {
        /* drop the OLDEST entry (bounded memory; deep menus only lose the
         * farthest-back target) */
        memmove(&g_back_stack[0], &g_back_stack[1],
                sizeof(JavaObject*) * (MIDP_BACK_STACK_MAX - 1));
        g_back_stack[MIDP_BACK_STACK_MAX - 1] = NULL;
        g_back_depth = MIDP_BACK_STACK_MAX - 1;
    }
    int slot = g_back_depth;
    g_back_stack[slot] = disp;
    if (!g_back_rooted[slot]) {
        gc_add_root(jvm, (void**)&g_back_stack[slot]);
        g_back_rooted[slot] = true;
    }
    g_back_depth++;
}

/* Emulated BACK: pop the most recent history entry that differs from the
 * current screen and make it current (no new history entry). Returns
 * true when a navigation happened, false when the history is empty. */
static bool midp_emulated_back(JVM* jvm) {
    while (g_back_depth > 0) {
        JavaObject* target = g_back_stack[--g_back_depth];
        g_back_stack[g_back_depth] = NULL;
        if (target && target != current_displayable_obj) {
            DISP_DEBUG("[BackNav] emulated BACK -> %p", (void*)target);
            display_set_current_internal(jvm, target, false);
            return true;
        }
    }
    return false;
}

/* Forward declaration */
void ensure_native_peer_field(JavaClass* clazz);

/* Forward declaration for find_field_index */
static int find_field_index(JavaObject* obj, const char* field_name);

/* Get string from Java String object */
static const char* get_string_from_java(JVM* jvm, JavaObject* str_obj) {
    if (!str_obj) return "";
    JavaString* str = (JavaString*)str_obj;
    const char* result = string_utf8(jvm, str);
    return result ? result : "";
}

/* Check if a pointer looks like a valid class pointer */
static bool is_valid_class_ptr(JavaClass* clazz) {
    if (!clazz) return false;
    
    /* Check if the pointer is in a reasonable range (not too small, not kernel space) */
    /* On 32-bit: typical user space is 0x00400000 to 0x7FFFFFFF */
    /* On 64-bit: typical user space is much higher */
    #if defined(_WIN64) || defined(__x86_64__)
    if ((uintptr_t)clazz < 0x10000) return false;  /* Too small */
    #else
    if ((uintptr_t)clazz < 0x10000) return false;  /* Too small for 32-bit */
    #endif
    
    /* Check if class_name is a reasonable pointer */
    if (clazz->class_name) {
        /* class_name should point to static data or heap, not be garbage */
        /* A simple check: if it's too small, it's likely garbage */
        if ((uintptr_t)clazz->class_name < 0x10000) return false;
    }
    
    /* Check fields_count is reasonable */
    if (clazz->fields_count > 1000) return false;
    
    return true;
}

/* Validate a heap object before accessing it */
__attribute__((unused))
static bool validate_heap_object(JavaObject* obj) {
    if (!obj) return false;
    
    /* Check if object is in heap bounds */
    if (!is_heap_ptr_check(obj)) {
        DEBUG_LOG("[VALIDATE] Object %p is NOT in heap bounds [%p, %p)",
                (void*)obj, g_heap_start, g_heap_end);
        return false;
    }
    
    /* Get GC header */
    GCObjectHeader* header = (GCObjectHeader*)obj - 1;
    
    /* Check if GC header is in heap bounds */
    if (!is_heap_ptr_check(header)) {
        DEBUG_LOG("[VALIDATE] GC header %p for object %p is NOT in heap bounds",
                (void*)header, (void*)obj);
        return false;
    }
    
    /* Check header fields for sanity */
    if (header->size == 0 || header->size > 16*1024*1024) {
        DEBUG_LOG("[VALIDATE] Object %p has invalid size: %u",
                (void*)obj, header->size);
        return false;
    }
    
    /* Check if object type is valid */
    if (header->type > OBJ_TYPE_CLASS) {
        DEBUG_LOG("[VALIDATE] Object %p has invalid type: %d",
                (void*)obj, header->type);
        return false;
    }
    
    /* Validate clazz pointer */
    if (!is_valid_class_ptr(header->clazz)) {
        DEBUG_LOG("[VALIDATE] Object %p has invalid clazz pointer: %p",
                (void*)obj, (void*)header->clazz);
        return false;
    }
    
    return true;
}

/* Safely get the label string from a Command object.
 * Returns a default string if the object is invalid or corrupted.
 * Uses soft validation - only checks heap bounds and class pointer. */
static const char* get_command_label_safe(JVM* jvm, JavaObject* cmd) {
    static const char* default_label = "Command";
    
    if (!cmd) return default_label;
    
    /* Soft validation - only check if pointer is in heap */
    if (!is_heap_ptr_check(cmd)) {
        DEBUG_LOG("[CMD_LABEL] Command %p is NOT in heap bounds", (void*)cmd);
        return default_label;
    }
    
    /* Try to access the object's class pointer */
    JavaClass* clazz = cmd->header.clazz;
    if (!clazz) {
        DEBUG_LOG("[CMD_LABEL] Command %p has NULL class", (void*)cmd);
        return default_label;
    }
    
    /* Basic sanity check for class pointer */
    if ((uintptr_t)clazz < 0x10000) {
        DEBUG_LOG("[CMD_LABEL] Command %p has invalid class: %p", (void*)cmd, (void*)clazz);
        return default_label;
    }
    
    /* Try to get label - Command has label at field 0 */
    JavaString* label = NULL;
    
    /* Try to find label field by name first */
    int label_idx = find_field_index(cmd, "label");
    if (label_idx >= 0) {
        label = (JavaString*)cmd->fields[label_idx].ref;
    } else {
        /* Fallback: try field 0 (common case for Command) */
        label = (JavaString*)cmd->fields[0].ref;
    }
    
    if (!label) return default_label;
    
    const char* text = get_string_from_java(jvm, (JavaObject*)label);
    return text ? text : default_label;
}

/* Find field slot by name in class hierarchy.
 * Returns the correct slot index in the object's fields array,
 * or -1 if not found. */
static int find_field_index(JavaObject* obj, const char* field_name) {
    if (!obj) return -1;
    
    /* Soft validation - only check heap bounds */
    if (!is_heap_ptr_check(obj)) {
        DEBUG_LOG("[find_field_index] Object %p is NOT in heap bounds", (void*)obj);
        return -1;
    }
    
    if (!obj->header.clazz) return -1;
    
    JavaClass* clazz = obj->header.clazz;
    
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
        
        /* Validate class before accessing */
        if (!is_valid_class_ptr(current)) {
            DEBUG_LOG("[find_field_index] Invalid class pointer in hierarchy for field '%s'",
                    field_name ? field_name : "(null)");
            break;
        }
        
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

/* Forward declaration for type-based command mapping */
static void get_soft_button_commands(JVM* jvm, JavaObject* displayable,
                                     int* left_idx, int* right_idx);

/* Render soft buttons (commands) at bottom of screen */
static void render_soft_buttons(JVM* jvm, JavaObject* displayable) {
    DISP_DEBUG("[SoftBtn] render_soft_buttons: displayable=%p", (void*)displayable);
    
    if (!displayable) {
        return;
    }
    
    /* Get the screen graphics - ИСПРАВЛЕНО: инициализируем при необходимости */
    MidpGraphics* gfx = &g_screen_graphics;
    
    /* Если pixels не инициализирован, получаем из SDL context */
    if (!gfx->pixels) {
        SdlContext* sdl_ctx = sdl_get_global_context();
        if (sdl_ctx && sdl_ctx->framebuffer) {
            midp_graphics_init(gfx, sdl_ctx->framebuffer, sdl_ctx->width, sdl_ctx->height);
        }
    }
    
    if (!gfx || !gfx->pixels) {
        return;
    }
    
    /* Check if full screen mode is enabled */
    if (g_full_screen_mode) {
        return;  /* Don't draw soft buttons in full screen mode */
    }
    
    /* Find commands field in displayable */
    int commands_idx = find_field_index(displayable, "commands");
    
    if (commands_idx < 0) {
        return;
    }
    
    /* v14: defensive guards — a displayable whose class carries the field
     * NAME but whose object was allocated with a smaller (stale) layout can
     * expose garbage in the ref slot; verify the value is a live heap object
     * before touching it (ASAN SEGV in M3GTest run). */
    if (!displayable->header.clazz ||
        commands_idx >= (int)((displayable->header.clazz->instance_size -
                               sizeof(ObjectHeader)) / sizeof(JavaValue))) {
        return;
    }
    
    JavaArray* commands = (JavaArray*)displayable->fields[commands_idx].ref;
    
    /* v14: the ref slot can hold a non-array value (stale slot from a class
     * layout that was recalculated after the object was allocated) — verify
     * it is at least a live heap object that LOOKS like an array before
     * touching ->length (ASAN SEGV in M3GTest run). */
    if (!is_heap_ptr_check(commands)) {
        return;
    }
    if (commands->length < 0 || commands->length > 4096) {
        return;
    }
    if (commands->length == 0) {
        return;
    }
    
    void** cmd_data = (void**)array_data(commands);
    int cmd_count = commands->length;
    
    //DISP_DEBUG("[SoftBtn] render_soft_buttons: drawing %d commands", cmd_count);
    
    /* Draw soft button bar background */
    int screen_height = gfx->height;
    int screen_width = gfx->width;
    
    /* IMPORTANT: Save current clip state and reset to full screen for soft buttons */
    int saved_clip_x = gfx->clip_x;
    int saved_clip_y = gfx->clip_y;
    int saved_clip_width = gfx->clip_width;
    int saved_clip_height = gfx->clip_height;
    int saved_translate_x = gfx->translate_x;
    int saved_translate_y = gfx->translate_y;
    
    /* Reset clip to full screen area (soft buttons should not be clipped by game) */
    gfx->clip_x = 0;
    gfx->clip_y = 0;
    gfx->clip_width = screen_width;
    gfx->clip_height = screen_height;
    gfx->translate_x = 0;
    gfx->translate_y = 0;
    
    /* If command menu is open, draw the menu overlay */
    if (g_command_menu_open && cmd_count > 2) {
        /* Semi-transparent background overlay */
        midp_graphics_set_color(gfx, 0x000000, 128);
        midp_graphics_fill_rect(gfx, 0, 0, screen_width, screen_height);
        
        /* Menu box */
        int menu_width = screen_width - 20;
        int menu_item_height = 20;
        int menu_height = cmd_count * menu_item_height + 10;
        int menu_x = 10;
        int menu_y = (screen_height - menu_height) / 2;
        
        /* Menu background */
        midp_graphics_set_color(gfx, 0xFFFFFF, 255);
        midp_graphics_fill_rect(gfx, menu_x, menu_y, menu_width, menu_height);
        
        /* Menu border */
        midp_graphics_set_color(gfx, 0x000000, 255);
        midp_graphics_draw_rect(gfx, menu_x, menu_y, menu_width, menu_height);
        
        /* Draw menu items */
        for (int i = 0; i < cmd_count; i++) {
            if (!cmd_data[i]) continue;
            
            JavaObject* cmd = (JavaObject*)cmd_data[i];
            const char* text = get_command_label_safe(jvm, cmd);
            
            int item_y = menu_y + 5 + i * menu_item_height;
            
            /* Highlight selected item */
            if (i == g_command_menu_selected) {
                /* v34.65: MIDP_SEL_BG 0x202020 (was 0x000080 — blended with
                 * the navy title bars; "selection color is close to the form
                 * color" report) */
                midp_graphics_set_color(gfx, 0x202020, 255);
                midp_graphics_fill_rect(gfx, menu_x + 2, item_y, menu_width - 4, menu_item_height - 2);
                midp_graphics_set_color(gfx, 0xFFFFFF, 255);
            } else {
                midp_graphics_set_color(gfx, 0x000000, 255);
            }
            
            midp_graphics_draw_string(gfx, text, menu_x + 5, item_y + 2, 0);
        }
        
        /* Draw soft buttons for menu: "Select" and "Cancel" */
        midp_graphics_set_color(gfx, 0xC0C0C0, 255);
        midp_graphics_fill_rect(gfx, 0, screen_height - SOFT_BUTTON_HEIGHT, 
                                screen_width, SOFT_BUTTON_HEIGHT);
        midp_graphics_set_color(gfx, 0x808080, 255);
        midp_graphics_draw_line(gfx, 0, screen_height - SOFT_BUTTON_HEIGHT,
                                screen_width, screen_height - SOFT_BUTTON_HEIGHT);
        
        /* Left button: Cancel */
        midp_graphics_set_color(gfx, 0x000000, 255);
        midp_graphics_draw_string(gfx, "Cancel", 5, screen_height - SOFT_BUTTON_HEIGHT + 5, 0);
        
        /* Right button: Select */
        midp_graphics_set_color(gfx, 0x000000, 255);
        midp_graphics_draw_string(gfx, "Select", screen_width - 45, screen_height - SOFT_BUTTON_HEIGHT + 5, 0);
        
    } else {
        /* Normal soft button bar */
        midp_graphics_set_color(gfx, 0xC0C0C0, 255);  /* Light gray */
        midp_graphics_fill_rect(gfx, 0, screen_height - SOFT_BUTTON_HEIGHT, 
                                screen_width, SOFT_BUTTON_HEIGHT);
        
        /* Draw top border */
        midp_graphics_set_color(gfx, 0x808080, 255);  /* Dark gray */
        midp_graphics_draw_line(gfx, 0, screen_height - SOFT_BUTTON_HEIGHT,
                                screen_width, screen_height - SOFT_BUTTON_HEIGHT);
        
        if (cmd_count > 2) {
            /* More than 2 commands: show "Menu" on left */
            midp_graphics_set_color(gfx, 0x000000, 255);
            midp_graphics_draw_string(gfx, "Menu", 5, screen_height - SOFT_BUTTON_HEIGHT + 5, 0);
            
            /* Right button: show BACK/EXIT command or first command */
            int left_idx, right_idx;
            get_soft_button_commands(jvm, displayable, &left_idx, &right_idx);
            
            if (right_idx >= 0 && cmd_data[right_idx]) {
                JavaObject* cmd = (JavaObject*)cmd_data[right_idx];
                const char* text = get_command_label_safe(jvm, cmd);
                midp_graphics_set_color(gfx, 0x000000, 255);
                int text_len = strlen(text);
                int text_width = text_len * 6;
                midp_graphics_draw_string(gfx, text, screen_width - text_width - 5, 
                                         screen_height - SOFT_BUTTON_HEIGHT + 5, 0);
            }
        } else {
            /* 1-2 commands: show based on type priority */
            int left_idx, right_idx;
            get_soft_button_commands(jvm, displayable, &left_idx, &right_idx);
            
            /* Left button */
            if (left_idx >= 0 && cmd_data[left_idx]) {
                JavaObject* cmd = (JavaObject*)cmd_data[left_idx];
                const char* text = get_command_label_safe(jvm, cmd);
                midp_graphics_set_color(gfx, 0x000000, 255);
                midp_graphics_draw_string(gfx, text, 5, screen_height - SOFT_BUTTON_HEIGHT + 5, 0);
            }
            
            /* Right button */
            if (right_idx >= 0 && cmd_data[right_idx]) {
                JavaObject* cmd = (JavaObject*)cmd_data[right_idx];
                const char* text = get_command_label_safe(jvm, cmd);
                midp_graphics_set_color(gfx, 0x000000, 255);
                int text_len = strlen(text);
                int text_width = text_len * 6;
                midp_graphics_draw_string(gfx, text, screen_width - text_width - 5, 
                                         screen_height - SOFT_BUTTON_HEIGHT + 5, 0);
            }
        }
    }
    
    /* Restore clip state */
    gfx->clip_x = saved_clip_x;
    gfx->clip_y = saved_clip_y;
    gfx->clip_width = saved_clip_width;
    gfx->clip_height = saved_clip_height;
    gfx->translate_x = saved_translate_x;
    gfx->translate_y = saved_translate_y;
}

/* Call commandAction for a specific command index (0 = left soft button, 1 = right soft button) */

/* v34.72: type-based AMS fallback, shared by EVERY exit path of
 * call_command_action() — with a listener or without one (the old code
 * reached this logic only when commandAction had actually executed, so
 * dead commands on listener-less screens silently did NOTHING).
 *
 * v34.29 FIX (history): MIDP Command types are SCREEN=1, BACK=2, CANCEL=3,
 * OK=4, HELP=5, STOP=6, EXIT=7, ITEM=8. The v34.29 test treated type 1
 * (SCREEN!) as a stop-command — but SCREEN is the most common type for
 * actions like "Start"/"OK"/"Restart" (JBenchmark3D's Start is
 * Command("Start", SCREEN, 1)): pressing it started the benchmark thread
 * and then KILLED the JVM mid-render.
 *
 * v34.72 CRITICAL FIX (user report: "cancel завершает приложение вместо
 * выхода из списка"): the AMS NEVER terminates a MIDlet because a
 * BACK/CANCEL command was dispatched — those are NAVIGATION semantics
 * owned by the midlet's own commandAction. The old fallback stopped the
 * VM for BACK(2) even AFTER a listener had just handled it (the midlet
 * switched back to the menu — and the app died a millisecond later,
 * looking exactly like "Cancel closed the application"). Rules:
 *   - BACK(2)/CANCEL(3) with a listener that ran  -> NEVER stop;
 *     whatever the midlet did stands (phone behavior).
 *   - BACK(2)/CANCEL(3) with NO listener          -> emulated
 *     back-navigation to the previous screen (history stack),
 *     NOT termination; empty history -> stay (a dead Cancel must not
 *     kill the app).
 *   - EXIT(7)/STOP(6) with a listener that ran   -> legacy safety stop
 *     (midlets whose exit handling never calls notifyDestroyed would
 *     otherwise have a dead exit button) — now with the mandatory
 *     destroyApp(true) delivery, exactly like the notifyDestroyed path,
 *     so the midlet can save its state first.
 *   - EXIT(7)/STOP(6) with NO listener            -> do nothing: the
 *     listener may be attached a moment later by a still-running UI
 *     builder thread (Nescube builds commands first, listener last —
 *     v34.67), and killing during that window would terminate a healthy
 *     application (also exactly what a real phone does: unbound
 *     commands are inert). */
static void call_command_action_tail(JVM* jvm, JavaObject* cmd, bool listener_ran) {
    if (!jvm || !cmd || !jvm->running) return;  /* VM already stopped */

    int type_idx = find_field_index(cmd, "type");
    if (type_idx < 0) return;
    jint cmd_type = cmd->fields[type_idx].i;
    DISP_DEBUG("[CmdAction] Command type=%d (listener_ran=%d)",
               (int)cmd_type, (int)listener_ran);

    if (listener_ran && (cmd_type == CMD_EXIT || cmd_type == CMD_STOP)) {
        DISP_DEBUG("[CmdAction] EXIT/STOP command, stopping (destroyApp first)");
        midlet_call_destroy_app(jvm, true);
        jvm->running = false;
        SdlContext* sdl_ctx = sdl_get_global_context();
        if (sdl_ctx) {
            sdl_ctx->running = false;
        }
    } else if (!listener_ran && (cmd_type == CMD_BACK || cmd_type == CMD_CANCEL)) {
        /* Dead Cancel/Back: emulate the phone's back key. */
        if (!midp_emulated_back(jvm)) {
            DISP_DEBUG("[CmdAction] BACK/CANCEL without history — staying");
        }
    }
    /* Everything else (SCREEN/OK/HELP/ITEM, or EXIT/STOP without a
     * listener) — the command is inert here, exactly like a phone. */
}

static void call_command_action(JVM* jvm, JavaObject* displayable, int command_index) {
    DISP_DEBUG("[CmdAction] displayable=%p, cmd_index=%d", (void*)displayable, command_index);
    
    if (!displayable) return;
    
    /* Find commands field in displayable */
    int commands_idx = find_field_index(displayable, "commands");
    if (commands_idx < 0) {
        DISP_DEBUG("[CmdAction] commands field not found");
        return;
    }
    
    JavaArray* commands = (JavaArray*)displayable->fields[commands_idx].ref;
    if (!commands || command_index >= commands->length) {
        DISP_DEBUG("[CmdAction] No command at index %d", command_index);
        return;
    }
    
    void** cmd_data = (void**)array_data(commands);
    JavaObject* cmd = (JavaObject*)cmd_data[command_index];
    if (!cmd) {
        DISP_DEBUG("[CmdAction] Command %d is NULL", command_index);
        return;
    }
    
    /* Get command label for debug */
    const char* cmd_label = get_command_label_safe(jvm, cmd);
    DISP_DEBUG("[CmdAction] Command '%s' at index %d", cmd_label, command_index);
    
    /* Find listener field */
    int listener_idx = find_field_index(displayable, "listener");
    DISP_DEBUG("[CmdAction] listener_idx=%d", listener_idx);
    
    if (listener_idx < 0) {
        DISP_DEBUG("[CmdAction] Trying commandListener field...");
        listener_idx = find_field_index(displayable, "commandListener");
    }
    
    if (listener_idx < 0) {
        DISP_DEBUG("[CmdAction] No listener field found");
        call_command_action_tail(jvm, cmd, false);
        return;
    }
    
    JavaObject* listener = (JavaObject*)displayable->fields[listener_idx].ref;
    DISP_DEBUG("[CmdAction] listener=%p", (void*)listener);
    
    if (!listener) {
        DISP_DEBUG("[CmdAction] No listener set");
        call_command_action_tail(jvm, cmd, false);
        return;
    }
    
    if (!listener->header.clazz) {
        DISP_DEBUG("[CmdAction] Listener has NULL class");
        call_command_action_tail(jvm, cmd, false);
        return;
    }
    
    DISP_DEBUG("[CmdAction] Listener class: %s", listener->header.clazz->class_name);
    
    /* Find and call commandAction(Command, Displayable) method */
    JavaClass* listener_class = listener->header.clazz;
    JavaMethod* method = jvm_resolve_method(jvm, listener_class, "commandAction",
        "(Ljavax/microedition/lcdui/Command;Ljavax/microedition/lcdui/Displayable;)V");
    
    DISP_DEBUG("[CmdAction] method=%p", (void*)method);
    
    /* v34.72: did the midlet's commandAction actually run? Distinguishes
     * "listener owns the semantics" from "dead command — the emulator
     * must emulate" — see the type-based fallback below. */
    bool listener_ran = false;
    
    if (method) {
        DISP_DEBUG("[CmdAction] method is_native=%d", method->is_native);
        
        /* Если код отсутствует, попробуем native lookup */
        if (!method->code.code || method->code.code_length == 0) {
            NativeMethod native_handler = native_find(jvm, listener_class->class_name, "commandAction",
                "(Ljavax/microedition/lcdui/Command;Ljavax/microedition/lcdui/Displayable;)V");
            DISP_DEBUG("[CmdAction] native_handler=%p", (void*)native_handler);
        }
        
        JavaValue args[3];
        args[0].ref = listener;       /* this */
        args[1].ref = cmd;            /* Command */
        args[2].ref = displayable;    /* Displayable */
        
        JavaValue result;
        JavaThread* thread = jvm_current_thread(jvm);
        
        DISP_DEBUG("[CmdAction] Calling execute_method for commandAction...");
        
        int exec_result = execute_method(jvm, thread, method, args, &result);
        listener_ran = true;
        
        DISP_DEBUG("[CmdAction] commandAction executed, result=%d", exec_result);
        
        /* Clear any pending exception to prevent it from poisoning future calls.
         * Some midlets throw exceptions intentionally (e.g., unsupported features).
         * Without clearing, the next execute_method() would immediately return -1.
         */
        if (thread && thread->pending_exception) {
            DISP_DEBUG("[CmdAction] Clearing pending exception after commandAction");
            /* [EXC-DROP-FREE] v36.47: + освобождение C-строк трейса
             * (см. [PLAYERUPDATE-ARGS] в media.c — тот же класс утечки) */
            thread->pending_exception = NULL;
            if (thread->exception_stack_trace) {
                free(thread->exception_stack_trace);
                thread->exception_stack_trace = NULL;
            }
            if (thread->exception_throw_info) {
                free(thread->exception_throw_info);
                thread->exception_throw_info = NULL;
            }
        }
        
        /* Проверяем состояние JVM после commandAction.
         * Если был вызван notifyDestroyed(), jvm->running будет false.
         */
        if (jvm && !jvm->running) {
            DISP_DEBUG("[CmdAction] JVM stopped (notifyDestroyed)");
            SdlContext* sdl_ctx = sdl_get_global_context();
            if (sdl_ctx) {
                sdl_ctx->running = false;
            }
        }
        
        /* v34.72: type-based AMS fallback — see call_command_action_tail()
         * for the full rationale (BACK/CANCEL never terminate). */
        call_command_action_tail(jvm, cmd, listener_ran);
        
        /* Запрашиваем перерисовку после commandAction */
        sdl_request_redraw();
    } else {
        DISP_DEBUG("[CmdAction] commandAction method NOT FOUND in %s", listener_class->class_name);
        call_command_action_tail(jvm, cmd, false);
    }
}

/* Check if current displayable has commands */
bool midp_has_commands(void) {
    if (!current_displayable_obj) {
        return false;
    }
    
    int commands_idx = find_field_index(current_displayable_obj, "commands");
    if (commands_idx < 0) return false;
    
    JavaArray* commands = (JavaArray*)current_displayable_obj->fields[commands_idx].ref;
    if (!commands || commands->length == 0) return false;
    
    return true;
}

/* Check if current displayable is a Canvas subclass (for auto-redraw in main loop) */
bool midp_has_active_canvas(void) {
    if (!current_displayable_obj) {
        return false;
    }
    
    JavaClass* clazz = current_displayable_obj->header.clazz;
    if (!clazz) return false;
    
    /* Check class hierarchy for Canvas */
    JavaClass* check = clazz;
    while (check) {
        if (check->class_name) {
            if (strcmp(check->class_name, "javax/microedition/lcdui/Canvas") == 0) {
                return true;  /* Current displayable is a Canvas or GameCanvas */
            }
        }
        check = check->super_class;
    }
    
    return false;
}

/* Find the best left/right command indices based on type priority.
 * Returns: left_idx and right_idx via pointers.
 * Sets right_idx to -1 if no command for right side.
 * For >2 commands, returns left_idx=-1 (meaning: show menu on left). */
static void get_soft_button_commands(JVM* jvm, JavaObject* displayable, 
                                     int* left_idx, int* right_idx) {
    (void)jvm;
    *left_idx = -1;
    *right_idx = -1;
    
    int commands_idx = find_field_index(displayable, "commands");
    if (commands_idx < 0) return;
    
    JavaArray* commands = (JavaArray*)displayable->fields[commands_idx].ref;
    if (!commands || commands->length == 0) return;
    
    void** cmd_data = (void**)array_data(commands);
    int cmd_count = commands->length;
    
    /* Find the negative command for the right button.
     * v34.67 FIX: CANCEL (3) and STOP (6) are negative commands exactly like
     * BACK (2) / EXIT (7) — MIDP binds them to the right soft key. The old
     * scan knew only BACK/EXIT, so a 3+ command set like [Default(SCREEN),
     * Cancel(CANCEL), Save(OK)] fell into "right = first command" and the
     * RIGHT SOFT KEY ("Отмена") fired the FIRST command instead of the
     * Cancel one — the label drawn on the button and the dispatched command
     * could even differ. Scan order = array order, first negative wins,
     * which keeps the BACK/EXIT behavior byte-identical. */
    int back_exit_idx = -1;
    int ok_idx = -1;
    
    for (int i = 0; i < cmd_count; i++) {
        JavaObject* cmd = (JavaObject*)cmd_data[i];
        if (!cmd || !OBJECT_HAS_FIELDS(cmd, 3)) continue;
        int cmd_type = cmd->fields[1].i;
        
        if ((cmd_type == CMD_BACK || cmd_type == CMD_EXIT ||
             cmd_type == CMD_CANCEL || cmd_type == CMD_STOP) && back_exit_idx < 0) {
            back_exit_idx = i;
        }
        if (cmd_type == CMD_OK && ok_idx < 0) {
            ok_idx = i;
        }
    }
    
    if (cmd_count == 1) {
        /* Single command: show on left */
        *left_idx = 0;
    } else if (cmd_count == 2) {
        /* Two commands: primary left, secondary right */
        if (back_exit_idx >= 0) {
            /* One of them is BACK/EXIT → put on right */
            *right_idx = back_exit_idx;
            *left_idx = (back_exit_idx == 0) ? 1 : 0;
        } else {
            *left_idx = 0;
            *right_idx = 1;
        }
    } else {
        /* 3+ commands: menu on left, right button gets BACK/EXIT or first command */
        if (back_exit_idx >= 0) {
            *right_idx = back_exit_idx;
        } else {
            *right_idx = 0;  /* Default to first command */
        }
        *left_idx = -1;  /* Signal: show "Menu" on left */
    }
}

/* Handle soft button press - called from key handling
 * According to MIDP specification:
 * - If displayable has Commands, call commandAction()
 * - If displayable has no Commands, return false so keyPressed(-6/-7) is passed to game
 */
bool midp_handle_soft_button(JVM* jvm, int button_index) {
    /* v34.67 DIAG: NOJME_SOFTKEY_TRACE=1 — ungated decision trace for the
     * soft-button dispatcher (the libretro/SDL frontends call it BEFORE
     * midp_call_keyPressed; a silent false here drops the ENTIRE command
     * dispatch and looks like a dead soft key / hang). One line per exit. */
    static int softkey_trace = -1;
    if (softkey_trace < 0)
        softkey_trace = (getenv("NOJME_SOFTKEY_TRACE") && getenv("NOJME_SOFTKEY_TRACE")[0] != '0') ? 1 : 0;
#define SOFTKEY_TRACE(why) do { \
    if (softkey_trace) { \
        fprintf(stderr, "[SOFTKEY] btn=%d EXIT:%s disp=%p cls=%s\n", button_index, (why), \
                (void*)current_displayable_obj, \
                (current_displayable_obj && current_displayable_obj->header.clazz && \
                 current_displayable_obj->header.clazz->class_name) ? \
                 current_displayable_obj->header.clazz->class_name : "(null)"); \
    } \
} while (0)

    DISP_DEBUG("[SoftBtnHandle] button_index=%d, fullscreen=%d",
            button_index, g_full_screen_mode);
    
    /* v34.29 FIX: full screen is a PER-CANVAS property in MIDP, but we track
     * it in one global flag. A benchmark/game Canvas that calls
     * setFullScreenMode(true) while a high-level screen (Form/List/...) is
     * current (JBenchmark 3D: the test canvas 'd' is constructed in the
     * MIDlet ctor, before "Start" is ever pressed) used to disable command
     * dispatch for the WHOLE emulator — the welcome Form's soft keys went
     * dead ("Start" could not be pressed). On high-level screens commands
     * ALWAYS live on the soft keys, so only suppress interception when the
     * current displayable is actually a canvas/game screen. */
    if (g_full_screen_mode) {
        int kind = midp_displayable_kind(current_displayable_obj);
        if (kind == MIDP_UI_KIND_CANVAS || kind == MIDP_UI_KIND_OTHER) {
            SOFTKEY_TRACE("fullscreen-canvas");
            return false;  /* Soft buttons not active for fullscreen game screens */
        }
    }
    
    if (!current_displayable_obj) {
        SOFTKEY_TRACE("no-displayable");
        return false;
    }
    
    /* Get command count */
    int commands_idx = find_field_index(current_displayable_obj, "commands");
    DISP_DEBUG("[SoftBtnHandle] commands_idx=%d", commands_idx);
    
    if (commands_idx < 0) { SOFTKEY_TRACE("no-commands-field"); return false; }
    
    JavaArray* commands = (JavaArray*)current_displayable_obj->fields[commands_idx].ref;
    /* v36.30 [ITEM-CMDS]: a NULL/empty FORM commands array is the normal
     * state of info-midlets that hang their actions on ITEMS — the item
     * command path owns the soft keys in that case. */
    if (!commands || commands->length == 0) {
        {
            extern bool midp_form_item_soft_button(JVM* jvm, int button_index);
            if (midp_form_item_soft_button(jvm, button_index)) {
                SOFTKEY_TRACE("item-command-dispatch");
                return true;
            }
        }
        SOFTKEY_TRACE(commands ? "zero-commands" : "null-commands-array");
        return false;
    }
    
    int cmd_count = commands->length;
    DISP_DEBUG("[SoftBtnHandle] cmd_count=%d", cmd_count);
    
    /* If menu is open */
    if (g_command_menu_open && cmd_count > 2) {
        if (button_index == 0) {
            /* Left button: Cancel - close menu */
            g_command_menu_open = false;
            g_command_menu_selected = 0;
            DISP_DEBUG("[SoftBtnHandle] Menu closed (cancel)");
            SOFTKEY_TRACE("menu-closed");
            return true;
        } else if (button_index == 1) {
            /* Right button: Select - execute selected command */
            call_command_action(jvm, current_displayable_obj, g_command_menu_selected);
            g_command_menu_open = false;
            g_command_menu_selected = 0;
            SOFTKEY_TRACE("menu-item-activated");
            return true;
        }
        SOFTKEY_TRACE("menu-open-other-button");
        return false;
    }
    
    /* Normal mode: determine left/right command mapping */
    int left_idx, right_idx;
    get_soft_button_commands(jvm, current_displayable_obj, &left_idx, &right_idx);
    
    if (left_idx < 0) {
        /* Menu mode (3+ commands): left=menu, right=back/exit */
        if (button_index == 0) {
            g_command_menu_open = true;
            g_command_menu_selected = 0;
            DISP_DEBUG("[SoftButton] Menu opened");
            SOFTKEY_TRACE("menu-opened");
            return true;
        } else if (button_index == 1) {
            if (right_idx >= 0) {
                SOFTKEY_TRACE("menu-mode-right-dispatch");
                call_command_action(jvm, current_displayable_obj, right_idx);
                return true;
            }
        }
    } else {
        /* Direct mode (1-2 commands): left and right map directly */
        if (button_index == 0 && left_idx >= 0) {
            SOFTKEY_TRACE("direct-left-dispatch");
            call_command_action(jvm, current_displayable_obj, left_idx);
            return true;
        } else if (button_index == 1 && right_idx >= 0) {
            SOFTKEY_TRACE("direct-right-dispatch");
            call_command_action(jvm, current_displayable_obj, right_idx);
            return true;
        } else if (button_index == 1 && right_idx < 0 && left_idx >= 0) {
            /* Single command: right button also triggers it */
            SOFTKEY_TRACE("single-cmd-right-dispatch");
            call_command_action(jvm, current_displayable_obj, left_idx);
            return true;
        }
    }
    
    SOFTKEY_TRACE("no-mapping-fallthrough");
    return false;
}

/* Handle menu navigation (UP/DOWN) when menu is open */
bool midp_handle_menu_navigation(int direction) {
    if (!g_command_menu_open || !current_displayable_obj) {
        return false;
    }
    
    int commands_idx = find_field_index(current_displayable_obj, "commands");
    if (commands_idx < 0) return false;
    
    JavaArray* commands = (JavaArray*)current_displayable_obj->fields[commands_idx].ref;
    if (!commands) return false;
    
    int cmd_count = commands->length;
    
    if (direction < 0) {
        /* UP */
        g_command_menu_selected--;
        if (g_command_menu_selected < 0) g_command_menu_selected = cmd_count - 1;
    } else {
        /* DOWN */
        g_command_menu_selected++;
        if (g_command_menu_selected >= cmd_count) g_command_menu_selected = 0;
    }
    
    DISP_DEBUG("[SoftButton] Menu selection: %d", g_command_menu_selected);
    return true;
}

/* Check if command menu is open */
bool midp_is_command_menu_open(void) {
    return g_command_menu_open;
}

/* v34.29: read access for renderers (render_form draws the menu overlay for
 * Form screens — display.c's render_soft_buttons only covers Canvas). */
int midp_command_menu_selected(void) {
    return g_command_menu_open ? g_command_menu_selected : -1;
}

/* Close command menu */
void midp_close_command_menu(void) {
    g_command_menu_open = false;
    g_command_menu_selected = 0;
}

/* ============================================
 * v36.26 [TOUCH-UI]: tap dispatcher for the MIDP high-level UI.
 * Вызывается с frame-потока ИЗ sdl_switch_pointer_dispatch ДО отсылки
 * pointer-событий канве. Возвращает 1, если тап ПОГЛОЩЁН стандартным
 * джава-интерфейсом (софт-кнопки, командное меню, List/Form/TextBox/
 * Alert, виртуальная клавиатура) — канва такой press не видит.
 * Порядок приоритетов повторяет midp_call_keyPressed:
 *   1) виртуальная клавиатура (владеет всем экраном),
 *   2) открытое командное меню,
 *   3) полоса софт-кнопок (рисуется при командах и не-fullscreen),
 *   4) содержимое List/Form/TextBox/Alert.
 * Canvas/OTHER -> 0: игры получают pointerPressed как раньше.
 * Все действия — ТОЛЬКО на down edge; drag/release глотает
 * midp_ui_touch_hold (владелец последовательности решён на down edge). */
int midp_ui_touch_hit(JVM* jvm, int x, int y) {
    JavaObject* disp = current_displayable_obj;
    if (!disp) return 0;

    MidpGraphics* gfx = &g_screen_graphics;
    int w = gfx->width, h = gfx->height;
    if (w <= 0 || h <= 0) {
        SdlContext* sctx = sdl_get_global_context();
        if (!sctx) return 0;
        w = sctx->width;
        h = sctx->height;
    }
    if (w <= 0 || h <= 0) return 0;

    /* 1. виртуальная клавиатура активна — весь экран её */
    if (midp_is_vkb_active()) {
        return midp_vkb_touch(jvm, x, y) ? 1 : 1;
    }

    int kind = midp_displayable_kind(disp);

    int commands_idx = find_field_index(disp, "commands");
    JavaArray* commands = (commands_idx >= 0 &&
                           OBJECT_HAS_FIELDS(disp, commands_idx + 1))
                              ? (JavaArray*)disp->fields[commands_idx].ref : NULL;
    int cmd_count = (commands && is_heap_ptr_check(commands) &&
                     commands->length >= 0 && commands->length <= 4096)
                        ? commands->length : 0;

    /* 2. открытое командное меню (3+ команды) */
    if (g_command_menu_open && cmd_count > 2) {
        int menu_w = w - 20, item_h = 20;
        int menu_h = cmd_count * item_h + 10;
        int menu_x = 10, menu_y = (h - menu_h) / 2;
        if (y >= h - SOFT_BUTTON_HEIGHT) {
            /* Cancel / Select в полосе софт-кнопок */
            if (x < w / 2) {
                g_command_menu_open = false;
                g_command_menu_selected = 0;
            } else {
                call_command_action(jvm, disp, g_command_menu_selected);
                g_command_menu_open = false;
                g_command_menu_selected = 0;
            }
            return 1;
        }
        if (x >= menu_x && x < menu_x + menu_w &&
            y >= menu_y && y < menu_y + menu_h) {
            int i = (y - menu_y - 5) / item_h;
            if (i >= 0 && i < cmd_count) {
                void** cmd_data = (void**)array_data(commands);
                if (cmd_data[i]) {
                    g_command_menu_selected = i;
                    call_command_action(jvm, disp, i);
                    g_command_menu_open = false;
                    g_command_menu_selected = 0;
                }
            }
            return 1;
        }
        /* тап мимо меню = отмена */
        g_command_menu_open = false;
        g_command_menu_selected = 0;
        return 1;
    }

    /* 3. полоса софт-кнопок: рисуется при командах вне fullscreen.
     * Левая треть = левая софт-кнопка, правая треть = правая,
     * середина — мёртвая зона (глотаем, чтобы не протекала в канву).
     * v36.30 [ITEM-CMDS]: зона жива и когда у Form команд нет, но команды
     * есть у сфокусированного item (info-мидлеты) или открыт item-меню. */
    {
        int item_cmd_zone = 0;
        if (kind == MIDP_UI_KIND_FORM && cmd_count == 0) {
            extern int midp_form_focused_item_commands(JavaObject** out_cmds, int max);
            extern bool midp_form_item_menu_active(void);
            item_cmd_zone = midp_form_item_menu_active() ||
                            midp_form_focused_item_commands(NULL, 0) > 0;
        }
        if (!g_full_screen_mode && (cmd_count > 0 || item_cmd_zone) &&
            y >= h - SOFT_BUTTON_HEIGHT) {
            if (x < w / 3) {
                midp_handle_soft_button(jvm, 0);
            } else if (x >= (2 * w) / 3) {
                midp_handle_soft_button(jvm, 1);
            }
            return 1;
        }
    }

    /* 4. содержимое high-level экранов */
    switch (kind) {
        case MIDP_UI_KIND_FORM:
            return midp_form_touch(jvm, x, y) ? 1 : 0;
        case MIDP_UI_KIND_LIST:
            return midp_list_touch(jvm, x, y) ? 1 : 0;
        case MIDP_UI_KIND_TEXTBOX:
            return midp_textbox_touch(jvm, x, y) ? 1 : 0;
        case MIDP_UI_KIND_ALERT:
            /* любой тап закрывает Alert (тот же путь, что FIRE) */
            midp_form_handle_key(jvm, 8);
            return 1;
        default:
            break; /* Canvas/OTHER: pointer-события как раньше */
    }
    return 0;
}

/* v36.26: поглотитель drag/release ВЛАДЕЛЬЦА touch-последовательности
 * (решено на down edge в sdl_switch_pointer_dispatch). Действий не
 * выполняет — события high-level UI срабатывают только на нажатии. */
int midp_ui_touch_hold(JVM* jvm, int x, int y) {
    (void)jvm; (void)x; (void)y;
    return 1;
}

/* SELECT_COMMAND singleton for List handling */
static JavaObject* g_display_select_command = NULL;

/* Ensure SELECT_COMMAND exists */
static void display_ensure_select_command(JVM* jvm) {
    if (g_display_select_command) return;
    
    JavaClass* cmd_class = jvm_load_class(jvm, "javax/microedition/lcdui/Command");
    if (!cmd_class) return;
    
    g_display_select_command = jvm_new_object(jvm, cmd_class);
    if (g_display_select_command && OBJECT_HAS_FIELDS(g_display_select_command, 3)) {
        JavaString* label = jvm_new_string(jvm, "Select");
        g_display_select_command->fields[0].ref = label;     /* label */
        g_display_select_command->fields[1].i = 4;          /* SCREEN type */
        g_display_select_command->fields[2].i = 0;          /* priority */
    }
    
    DISP_DEBUG("[display] Created SELECT_COMMAND: %p", (void*)g_display_select_command);
}

/* Helper for List FIRE handling - call commandAction with SELECT_COMMAND 
 * This is a fallback when form.c can't find the listener field directly.
 * Returns true if commandAction was called successfully. */
bool call_command_action_for_list(JVM* jvm, JavaObject* list, int selected_index) {
    if (!jvm || !list) return false;
    
    DISP_DEBUG("[call_command_action_for_list] list=%p, selected=%d", (void*)list, selected_index);
    
    JavaClass* list_class = list->header.clazz;
    if (!list_class) return false;
    
    /* Get commands array */
    int commands_idx = find_field_index(list, "commands");
    if (commands_idx < 0) {
        DISP_DEBUG("[call_command_action_for_list] commands field not found");
        return false;
    }
    
    JavaArray* commands = (JavaArray*)list->fields[commands_idx].ref;
    int cmd_count = commands ? commands->length : 0;
    
    DISP_DEBUG("[call_command_action_for_list] found %d commands", cmd_count);
    
    /* Check the list type */
    int listtype_idx = find_field_index(list, "listType");
    int list_type = 4; /* Default to IMPLICIT */
    if (listtype_idx >= 0) {
        list_type = list->fields[listtype_idx].i;
    }
    
    DISP_DEBUG("[call_command_action_for_list] list_type=%d (IMPLICIT=4)", list_type);
    
    /* Find the CommandListener */
    int listener_idx = find_field_index(list, "listener");
    if (listener_idx < 0) {
        listener_idx = find_field_index(list, "commandListener");
    }
    
    if (listener_idx >= 0) {
        JavaObject* listener = (JavaObject*)list->fields[listener_idx].ref;
        if (listener) {
            DISP_DEBUG("[call_command_action_for_list] found listener at idx %d", listener_idx);
            
            /* Create or get SELECT_COMMAND */
            display_ensure_select_command(jvm);
            
            if (g_display_select_command) {
                JavaClass* listener_class = listener->header.clazz;
                JavaMethod* method = jvm_resolve_method(jvm, listener_class, "commandAction",
                    "(Ljavax/microedition/lcdui/Command;Ljavax/microedition/lcdui/Displayable;)V");
                
                if (method) {
                    JavaValue args[3];
                    args[0].ref = listener;
                    args[1].ref = g_display_select_command;
                    args[2].ref = list;
                    
                    JavaValue result;
                    JavaThread* thread = jvm_current_thread(jvm);
                    execute_method(jvm, thread, method, args, &result);
                    DISP_DEBUG("[call_command_action_for_list] commandAction called successfully");
                    return true;
                }
            }
        }
    }
    
    /* Fallback: For IMPLICIT list with commands, call the first command */
    if (list_type == 4 && cmd_count > 0) {
        call_command_action(jvm, list, 0);
        return true;
    }
    
    DISP_DEBUG("[call_command_action_for_list] failed to call commandAction");
    return false;
}

/* Register global objects as GC roots */
__attribute__((unused))
static void register_gc_roots(JVM* jvm) {
    /* Register g_graphics_object as a GC root */
    gc_add_root(jvm, (void**)&g_graphics_object);
    /* Register current_displayable_obj as a GC root */
    gc_add_root(jvm, (void**)&current_displayable_obj);
}

/* Create a Java Graphics object bound to screen framebuffer */
static JavaObject* create_graphics_object(JVM* jvm) {
    if (g_graphics_object) {
        /* Reset clip to full screen before reusing */
        g_screen_graphics.clip_x = 0;
        g_screen_graphics.clip_y = 0;
        g_screen_graphics.clip_width = g_screen_graphics.width;
        g_screen_graphics.clip_height = g_screen_graphics.height;
        g_screen_graphics.translate_x = 0;
        g_screen_graphics.translate_y = 0;
        return g_graphics_object;
    }
    
    /* Get the Graphics class */
    JavaClass* gfx_class = jvm_load_class(jvm, "javax/microedition/lcdui/Graphics");
    if (!gfx_class) {
        ERROR_LOG("Failed to load Graphics class");
        return NULL;
    }
    
    LOG_SAFE("[GGDIAG] S5 after load_class\n");
    /* Ensure class has space for nativePeer */
    ensure_native_peer_field(gfx_class);
    
    /* Create Graphics object */
    g_graphics_object = jvm_new_object(jvm, gfx_class);
    if (!g_graphics_object) {
        ERROR_LOG("Failed to create Graphics object");
        return NULL;
    }
    
    /* Register as GC root to prevent collection */
    gc_add_root(jvm, (void**)&g_graphics_object);
    
    /* Initialize the MidpGraphics structure for screen */
    SdlContext* sdl_ctx = sdl_get_global_context();
    if (sdl_ctx && sdl_ctx->framebuffer) {
        midp_graphics_init(&g_screen_graphics, sdl_ctx->framebuffer, 
                          sdl_ctx->width, sdl_ctx->height);
    }
    
    /* Store the MidpGraphics* in the nativePeer field */
    set_object_field_ref(g_graphics_object, "nativePeer", (JavaObject*)&g_screen_graphics);
    
    DISP_DEBUG("Created Graphics object: %p, MidpGraphics: %p (%dx%d)", 
            (void*)g_graphics_object, (void*)&g_screen_graphics,
            g_screen_graphics.width, g_screen_graphics.height);
    
    return g_graphics_object;
}

/* Get MidpGraphics from a Java Graphics object */
MidpGraphics* get_graphics_from_object(JavaObject* obj) {
    if (!obj) return NULL;
    /* For the screen graphics object, return the global */
    if (obj == g_graphics_object) {
        return &g_screen_graphics;
    }
    /* For other Graphics objects (from Images), get nativePeer field */
    MidpGraphics* gfx = (MidpGraphics*)get_object_field_ref(obj, "nativePeer");
    if (gfx) return gfx;
    /* FIX-19q DIAG: fallback hides real failures - log it (throttled) */
    {
        static int npe_fallback_count = 0;
        if (npe_fallback_count < 5) {
            npe_fallback_count++;
            fprintf(stderr, "[GFXPEER] nativePeer read FAILED for gfx obj=%p - falling back to SCREEN graphics!\n", (void*)obj);
        }
    }
    /* Fallback to screen graphics */
    return &g_screen_graphics;
}

/* Event queue */
#define MAX_EVENTS 256

static struct {
    MidpEvent events[MAX_EVENTS];
    int head;
    int tail;
    int count;
} event_queue;

/* Repaint queue - MIDP repaint() is asynchronous */
#define MAX_REPAINTS 16
/* Legacy queue structure kept for layout compatibility (superseded by the
 * paint-pending mechanism below); fields are intentionally unused. */
static struct {
    JavaObject* canvas;
    int x, y, w, h;
    bool pending;
} repaint_queue[MAX_REPAINTS] __attribute__((unused));
static int repaint_head __attribute__((unused)) = 0;
static int repaint_tail __attribute__((unused)) = 0;

/* Flag: Canvas.repaint() or repaint(x,y,w,h) was called.
 * Cleared after midp_process_repaints() calls paint().
 * Prevents calling paint() every frame when the MIDlet didn't request it,
 * which would cause flickering (paint() catches offscreen buffer mid-update). */
static volatile bool g_canvas_repaint_requested = false;

/* FIX (task 16-b #22): dirty-rect for Canvas.repaint(x,y,w,h). The regions
 * of the repaints requested since the last pump are unioned here and the
 * next paint() is clipped to the union (device coordinates). A plain
 * repaint() clears the dirty rect (it supersedes any region requests). */
static int g_dirty_x = 0, g_dirty_y = 0, g_dirty_w = 0, g_dirty_h = 0;
static bool g_dirty_valid = false;

static void canvas_dirty_union(jint x, jint y, jint w, jint h) {
    if (w <= 0 || h <= 0) return;
    if (!g_dirty_valid) {
        g_dirty_x = x; g_dirty_y = y; g_dirty_w = w; g_dirty_h = h;
        g_dirty_valid = true;
        return;
    }
    int x2 = (g_dirty_x + g_dirty_w) > (x + w) ? (g_dirty_x + g_dirty_w) : (x + w);
    int y2 = (g_dirty_y + g_dirty_h) > (y + h) ? (g_dirty_y + g_dirty_h) : (y + h);
    if (x < g_dirty_x) g_dirty_x = x;
    if (y < g_dirty_y) g_dirty_y = y;
    g_dirty_w = x2 - g_dirty_x;
    g_dirty_h = y2 - g_dirty_y;
}

/* Game canvas state */
static struct {
    int key_states;
    /* FIX (task 16-b #26): latch of game keys pressed since the last
     * GameCanvas.getKeyStates() poll (see keycode_to_keystate_bit).
     * v36.01: volatile — written by delivery threads, consumed atomically
     * by getKeyStates on game threads. */
    volatile int key_latch;
    /* v35.09 LEVEL state: bits of game keys currently HELD DOWN, maintained
     * from the delivered keyPressed/keyReleased events (all input sources
     * converge there: gamepad edges, real keyboard events, test harnesses).
     * The old getKeyStates() unioned the latch with SDL_GetKeyboardState —
     * a PHYSICAL-KEYBOARD-ONLY source the gamepad synthetics never reach:
     * on a Switch the keyboard is empty, so a game polling getKeyStates()
     * (asia rally: every ~2 ms) saw a held gas/steer for exactly ONE poll
     * (the latch consumed on the first read) — dead controls in-race while
     * event-driven games (Asphalt) worked. Level bits survive across polls
     * until keyReleased, which is the phone-faithful GameCanvas semantics. */
    /* v36.01: volatile + atomic access — the bits are raised/cleared on the
     * frontend key-delivery thread and polled from game threads. */
    volatile int key_level;
    MidpGraphics* graphics;
    bool suppress_key_events;
    /* Offscreen buffer for GameCanvas */
    MidpImage* offscreen_buffer;
    int offscreen_width;
    int offscreen_height;
} game_canvas;

/* GameCanvas key state bit masks */
#define GAME_KEY_UP_PRESSED     (1 << 1)   /* bit 1 = UP */
#define GAME_KEY_DOWN_PRESSED   (1 << 6)   /* bit 6 = DOWN */
#define GAME_KEY_LEFT_PRESSED   (1 << 2)   /* bit 2 = LEFT */
#define GAME_KEY_RIGHT_PRESSED  (1 << 5)   /* bit 5 = RIGHT */
#define GAME_KEY_FIRE_PRESSED   (1 << 8)   /* bit 8 = FIRE */
#define GAME_KEY_A_PRESSED      (1 << 9)   /* bit 9 = GAME_A */
#define GAME_KEY_B_PRESSED      (1 << 10)  /* bit 10 = GAME_B */
#define GAME_KEY_C_PRESSED      (1 << 11)  /* bit 11 = GAME_C */
#define GAME_KEY_D_PRESSED      (1 << 12)  /* bit 12 = GAME_D */

/* FIX (task 16-b #26): map a MIDP game keycode to its GameCanvas key-state
 * bit so keyPressed() events can latch GameCanvas.getKeyStates() state.
 * [KEYSTATE-DIGITS] v36.45 (yetifix2): ITU-T keypad digits raise the SAME
 * bits as the dedicated game keys — phone-faithful (on real handsets '2'
 * IS the UP key: getGameAction('2')=UP, and getKeyStates reflects the
 * pressed key's game action). Required now that the A button sends '5'
 * ([KEY-A-PHONE], sdl_graphics.c): without this, getKeyStates-polled games
 * (Bounce Tales, Asia Rally) would lose the FIRE bit A used to raise via
 * -5. Digits are ALSO what the left stick sends ('2'/'8'/'4'/'6'), so
 * stick-driven getKeyStates games gain working controls on real phones'
 * terms. No collision: bits 9..12 (GAME_A..D) stay reserved for -6..-9. */
static int keycode_to_keystate_bit(int keycode) {
    switch (keycode) {
        case -1: return GAME_KEY_UP_PRESSED;
        case -2: return GAME_KEY_DOWN_PRESSED;
        case -3: return GAME_KEY_LEFT_PRESSED;
        case -4: return GAME_KEY_RIGHT_PRESSED;
        case -5: return GAME_KEY_FIRE_PRESSED;
        case -6: return GAME_KEY_A_PRESSED;
        case -7: return GAME_KEY_B_PRESSED;
        case -8: return GAME_KEY_C_PRESSED;
        case -9: return GAME_KEY_D_PRESSED;
        case 50: return GAME_KEY_UP_PRESSED;     /* '2' = UP (ITU-T) */
        case 56: return GAME_KEY_DOWN_PRESSED;   /* '8' = DOWN (ITU-T) */
        case 52: return GAME_KEY_LEFT_PRESSED;   /* '4' = LEFT (ITU-T) */
        case 54: return GAME_KEY_RIGHT_PRESSED;  /* '6' = RIGHT (ITU-T) */
        case 53: return GAME_KEY_FIRE_PRESSED;   /* '5' = FIRE (ITU-T) */
        default: return 0;
    }
}

/* Platform callbacks */
static MidpPlatformCallbacks platform_callbacks;

/* Forward declarations */
extern int execute_method(JVM* jvm, JavaThread* thread, JavaMethod* method, JavaValue* args, JavaValue* result);

/* ============ v34.9 deferred key delivery ============
 * Key events injected from a foreign (frontend/harness) thread used to run
 * displayable.keyPressed() inline via execute_method(), racing the game's
 * own threads (Bounce Tales: the key raced the loading choreography and the
 * soft-key command bindings never got set / got lost -> the game hung on
 * the level-select screen because Select/FIRE did nothing).
 * Keys are now POSTED to a queue and delivered on a VM thread from
 * thread_yield() (same context as callSerially), serialized with the game
 * logic. Enabled by NOJME_DEFER_KEYS=1 (default ON; =0 restores inline
 * delivery for debugging). */
#define MIDP_KEYQ_SIZE 64
/* [YSFIX] posted_ms: wall-clock stamp of when the entry entered the queue
 * (CLOCK_MONOTONIC ms). The frame-age gate alone cannot tell "the game
 * thread will drain this on its next tick" from "the display-owning
 * thread is idle forever and only the 3-frame fallback pump exists" —
 * Yeti Sports posts keys while its game loop runs on a SECOND thread and
 * the MIDlet thread that called setCurrent() never executes bytecode
 * again, so every key relied solely on the fallback pump. */
static struct { int code; int pressed; int frame_posted; uint64_t posted_ms; } midp_keyq[MIDP_KEYQ_SIZE];
static volatile int midp_keyq_head = 0, midp_keyq_tail = 0;

/* v34.27: the queue now has TWO consumers (the display-owning VM thread
 * drain and the frontend fallback pump) plus the frontend-thread producer.
 * The old unchecked head/tail RMW could hand the same entry to both
 * consumers. A tiny uncontended mutex makes each pop atomic. */
#if defined(_WIN32)
#include "win_thread_shim.h"   /* already included above via display.c top */
#else
#include <pthread.h>
#endif
static pthread_mutex_t midp_keyq_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Pop one entry atomically; returns 0 when empty. out_ms (optional)
 * receives the entry's wall-clock posted_ms stamp. */
static int midp_keyq_pop2(int* out_code, int* out_pressed, uint64_t* out_ms) {
    pthread_mutex_lock(&midp_keyq_mutex);
    if (midp_keyq_head == midp_keyq_tail) {
        pthread_mutex_unlock(&midp_keyq_mutex);
        return 0;
    }
    *out_code = midp_keyq[midp_keyq_head].code;
    *out_pressed = midp_keyq[midp_keyq_head].pressed;
    if (out_ms) *out_ms = midp_keyq[midp_keyq_head].posted_ms;
    midp_keyq_head = (midp_keyq_head + 1) % MIDP_KEYQ_SIZE;
    pthread_mutex_unlock(&midp_keyq_mutex);
    return 1;
}

/* v36.57 [KEY-TRYLOCK]: вернуть запись в ГОЛОВУ очереди (после pop —
 * когда доставка на игровом потоке отложена из-за занятой канвы, ключ
 * должен достаться фронтенд-фоллбеку как СТАРШИЙ). Атомарно, как pop. */
static volatile int midp_frame_counter;  /* tent. def; полное определение ниже по файлу */
static void midp_keyq_push_front(int code, int pressed, uint64_t posted_ms) {
    pthread_mutex_lock(&midp_keyq_mutex);
    int new_head = midp_keyq_head - 1;
    if (new_head < 0) new_head = MIDP_KEYQ_SIZE - 1;
    if (new_head == midp_keyq_tail) {
        /* очередь полна — дропаем САМУЮ СТАРУЮ (tail), освобождая слот */
        midp_keyq_tail = (midp_keyq_tail + 1) % MIDP_KEYQ_SIZE;
    }
    midp_keyq_head = new_head;
    midp_keyq[new_head].code = code;
    midp_keyq[new_head].pressed = pressed;
    midp_keyq[new_head].frame_posted = midp_frame_counter;
    midp_keyq[new_head].posted_ms = posted_ms;
    pthread_mutex_unlock(&midp_keyq_mutex);
}

/* [YSFIX] Wall-clock age (ms) of the oldest queued entry; UINT64_MAX when
 * empty. Used by the fallback pump as a safety net: a Canvas whose
 * display-owning thread is idle (Yeti Sports architecture: setCurrent from
 * startApp, game loop on a second Thread) never drains young entries, so
 * the 3-frame grace alone can silently rot a key if the fallback pump's
 * frame-age gate is the ONLY delivery path. */
#define KEYQ_WALLCLOCK_NET_MS 150
static uint64_t midp_keyq_mono_ms(void) {
#if defined(_WIN32)
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
#endif
}
static uint64_t midp_keyq_front_age_ms(void) {
    pthread_mutex_lock(&midp_keyq_mutex);
    uint64_t age;
    if (midp_keyq_head == midp_keyq_tail) {
        age = UINT64_MAX;
    } else {
        uint64_t posted = midp_keyq[midp_keyq_head].posted_ms;
        uint64_t now = midp_keyq_mono_ms();
        age = (posted && now > posted) ? (now - posted) : 0;
    }
    pthread_mutex_unlock(&midp_keyq_mutex);
    return age;
}

/* v35: frame counter bumped once per frame by the frontend pump. Used to age
 * queued keys so the fallback pump can tell "canvas game will drain this on
 * the game thread within its loop" from "event-driven menu, nobody will ever
 * drain this". */
static volatile int midp_frame_counter = 0;
void midp_frame_tick(void) { midp_frame_counter++; }
int midp_frame_count(void) { return midp_frame_counter; }

/* v50 (Asphalt 3 3D image freeze): vsync sequence, bumped by the presentation
 * path at the END of each frontend frame (libretro_end_frame / headless frame
 * loop). Canvas.serviceRepaints() on a game thread waits for the NEXT tick
 * after completing a paint — the KVM-authentic "block until vblank" that the
 * Gameloft race loop (no sleep of its own in gameplay mode!) relies on to
 * pace itself. Without it the loop spun repaint()->paint at 170+/sec, held
 * the global UI pump lock nearly full-time, and libretro_end_frame's
 * trylock-based presentation starved for up to 185 consecutive frames
 * (~3 seconds of FROZEN IMAGE while the game kept running - the reported
 * "car drives away during the freeze"). */
static volatile uint32_t g_midp_vsync_seq = 0;
void midp_vsync_tick(void) { g_midp_vsync_seq++; }
uint32_t midp_vsync_count(void) { return g_midp_vsync_seq; }

/* ==========================================================================
 * v34.61 (Asphalt 3 3D "rarely delivers 3D frames"): SETTLED-FRAME SCANOUT
 * BUFFER.
 *
 * SYMPTOM (user report): the game runs fast, but 3D frames are delivered
 * RARELY; sometimes a presented frame is only partially filled (background
 * only). Suspected thread interaction - confirmed below.
 *
 * ROOT CAUSES (all in the presentation path):
 *  (1) serviceRepaints() pacing was a no-op for SLOW paints: the vsync
 *      sequence was snapshotted BEFORE the paint, so a 3D paint longer than
 *      one frontend frame (15-40ms on ARMv7 vs a 16.6ms retro_run period)
 *      outlived one or more ticks; the wait then exited IMMEDIATELY and the
 *      game painted back-to-back. The UI pump lock (and the M3G bound /
 *      paint-depth flags) stayed closed nearly full-time.
 *  (2) libretro_end_frame() consulted (a) a trylock of the UI pump lock and
 *      (b) the mid-frame gates BEFORE copying the canvas to the display
 *      buffer. With (1) holding the lock ~100% of the time, presentation
 *      starved: only frames whose end_frame luckily landed in the game's
 *      tiny logic gap were shown - "rarely delivers 3D frames". A tick fired
 *      BETWEEN the trylock and the gate check also woke the game thread
 *      right before the decision, letting its unpaced retry (lock busy ->
 *      "retry fast") re-close the gates before the copy.
 *  (3) On real hardware the display controller scans out the framebuffer at
 *      every vblank and NEVER waits for the painter; painting never blocks
 *      scanout. Our presenter DID wait for the painter (that was its only
 *      anti-tearing tool). Partially filled frames (background only) were
 *      torn states leaking through the remaining gate windows (e.g. the
 *      GameCanvas flushGraphics path takes no UI lock and sets no flags).
 *
 * FIX - KVM-authentic scanout semantics:
 *  - Every code path that COMPLETES a frame (the repaint pump's Canvas /
 *    List / Form / TextBox / Alert tails, incl. soft buttons + M3G
 *    force-render, and both GameCanvas flushGraphics variants) snapshots the
 *    framebuffer into a private "stable" buffer under a tiny dedicated lock
 *    (~300KB memcpy, 30-100us) and bumps a sequence number.
 *  - The presenters (libretro end_frame / SDL window texture update) copy
 *    the LATEST settled snapshot into the display buffer and NEVER read the
 *    live framebuffer. Presentation can no longer be starved by an
 *    in-flight paint, and can no longer show a torn/partial state - both
 *    are impossible by construction (a snapshot is taken only after a
 *    complete frame exists).
 *  - serviceRepaints() now waits for the first tick that starts AFTER its
 *    paint completed (see native_canvas_serviceRepaints) - slow 3D paints
 *    pace at exactly one repaint per frontend frame, like blocking until
 *    vblank on a phone.
 *  - The vsync tick moved to AFTER the presentation copy (see
 *    libretro_end_frame), so a woken game thread can neither tear nor veto
 *    the frame it just produced.
 * ======================================================================== */
static uint32_t* g_midp_stable_pixels = NULL;   /* ARGB8888 snapshot */
static int g_midp_stable_w = 0, g_midp_stable_h = 0;
static uint32_t g_midp_stable_seq = 0;          /* 0 = nothing settled yet */
static pthread_mutex_t g_midp_stable_lock = PTHREAD_MUTEX_INITIALIZER;

/* v34.98 BOUNDED STABLE-LOCK: this mutex sits on EVERY frame-completion
 * snapshot AND on every presenter copy. A wedged holder would freeze the
 * frontend inside paint() with an INVISIBLE raw-mutex wait (no monenter,
 * no M3G-LOCK-WAIT, sp=0 — the unexplained stage=repaints class). Same
 * treatment as the M3G locks (v34.96): owner tracking, wait log at 2 s and
 * every 5 s, self-heal (mutex reinit) after 10 s — a torn snapshot is
 * always better than a frozen console. */
static volatile int g_stable_locked = 0;
static volatile long long g_stable_holder_ms = 0;
static char g_stable_holder_site[24];
static volatile long long g_stable_last_settle_ms = 0;
static pthread_mutexattr_t g_stable_attr;
static int g_stable_attr_ready = 0;

static long long stable_mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static void stable_lock_bounded(const char* site) {
    long long t0 = stable_mono_ms(), last_log = 0;
    while (pthread_mutex_trylock(&g_midp_stable_lock) != 0) {
        long long now = stable_mono_ms(), wait = now - t0;
        if (wait >= 2000 && now - last_log >= 5000) {
            last_log = now;
            LOG_SAFE("[STABLE-LOCK-WAIT] %s: blocked %lld ms; holder[%s held=%lldms]\n",
                     site, wait,
                     g_stable_locked ? g_stable_holder_site : "(none)",
                     (g_stable_locked && g_stable_holder_ms) ? now - g_stable_holder_ms : -1LL);
        }
        if (wait >= 10000) {
            LOG_SAFE("[STABLE-LOCK-RECOVER] %s: blocked %lld ms — reinitializing "
                     "orphaned stable lock (last holder %s)\n",
                     site, wait, g_stable_holder_site);
            if (!g_stable_attr_ready) {
                pthread_mutexattr_init(&g_stable_attr);
                g_stable_attr_ready = 1;
            }
            pthread_mutex_destroy(&g_midp_stable_lock);
            pthread_mutex_init(&g_midp_stable_lock, &g_stable_attr);
            g_stable_locked = 0;
            break;  /* proceed holding the fresh lock */
        }
        {
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 500000 };
            nanosleep(&ts, NULL);
        }
    }
    g_stable_locked = 1;
    g_stable_holder_ms = stable_mono_ms();
    snprintf(g_stable_holder_site, sizeof(g_stable_holder_site), "%.20s", site);
}

static void stable_unlock(void) {
    g_stable_locked = 0;
    pthread_mutex_unlock(&g_midp_stable_lock);
}

/* v34.98 STUCK-forensics: ms since the last settled frame (huge when the
 * game stopped completing frames — the 1 Hz degradation signature). */
uint64_t midp_present_stable_age_ms(void) {
    long long last = g_stable_last_settle_ms;
    long long now = stable_mono_ms();
    return (last == 0) ? 0xFFFFFFFFFFFFFFFFULL : (uint64_t)(now - last);
}

/* Snapshot the CURRENT framebuffer as a settled frame. Call ONLY at points
 * where a complete frame has just been produced (all drawing for the frame
 * is done). Safe from any thread; cheap; never blocks painters for more
 * than the memcpy. */
void midp_present_settled_snapshot(void) {
    SdlContext* ctx = sdl_get_global_context();
    if (!ctx || !ctx->framebuffer || ctx->width <= 0 || ctx->height <= 0) return;
    int w = ctx->width, h = ctx->height;
    long px = (long)w * (long)h;
    if (px <= 0 || px > 8192L * 8192L) return;  /* sanity: max 8K x 8K */

    stable_lock_bounded("settle");
    if (g_midp_stable_w != w || g_midp_stable_h != h) {
        /* screen geometry changed (rotation / landscape mode) - resize */
        uint32_t* nb = (uint32_t*)realloc(g_midp_stable_pixels,
                                          (size_t)px * sizeof(uint32_t));
        if (!nb) { stable_unlock(); return; }
        g_midp_stable_pixels = nb;
        g_midp_stable_w = w;
        g_midp_stable_h = h;
    }
    memcpy(g_midp_stable_pixels, ctx->framebuffer, (size_t)px * sizeof(uint32_t));
    g_midp_stable_seq++;
    g_stable_last_settle_ms = stable_mono_ms();
    stable_unlock();
}

/* Copy the latest settled frame into dst (dst_px pixels). Returns the
 * snapshot's sequence number, or 0 if no frame has settled yet (caller
 * falls back to its legacy gated direct-copy path). If the settled frame
 * is smaller than dst (screen resized up between snapshot and copy), the
 * tail is zero-padded. */
uint32_t midp_present_stable_copy(uint32_t* dst, int dst_px) {
    if (!dst || dst_px <= 0) return 0;
    stable_lock_bounded("copy");
    uint32_t seq = g_midp_stable_seq;
    if (g_midp_stable_pixels && g_midp_stable_w > 0 && g_midp_stable_h > 0) {
        int src_px = g_midp_stable_w * g_midp_stable_h;
        int n = src_px < dst_px ? src_px : dst_px;
        memcpy(dst, g_midp_stable_pixels, (size_t)n * sizeof(uint32_t));
        if (n < dst_px)
            memset(dst + n, 0, (size_t)(dst_px - n) * sizeof(uint32_t));
        stable_unlock();
        return seq;
    }
    stable_unlock();
    return 0;
}

/* Sequence of the latest settled frame (0 = none) - presentation-side
 * diagnostics (NOJME_STALL_DIAG counts frames with an unchanged seq). */
uint32_t midp_present_stable_seq(void) {
    return g_midp_stable_seq;
}

/* ---- v35.06 PHONE-FAITHFUL DIRECT-RENDER DELIVERY -----------------------
 * Field question (race 2D results screen, "falling money"): the blurred
 * border fill seemed to animate ~10x smoother than the sharp game view,
 * user hypothesis: "frames are dropped at the final render stage". Since
 * v35.01 both consumers are fed the SAME settled snapshot, so on the
 * current build they cannot diverge — but the question exposed a REAL
 * gap in the settled-frame model: games that draw into the SCREEN
 * framebuffer OUTSIDE the repaint pump (a Graphics object cached from an
 * earlier paint(), Nokia DirectGraphics-era direct rendering) never
 * settle between pumps, so everything they draw is invisible until the
 * next pump-driven paint. On a real phone that drawing hits the LCD
 * immediately — the emulator was silently throttling those games to the
 * pump cadence. Asphalt 3 does not (all draws inside paint(), see the
 * decompiled L()/ae() — pure logic), but the class exists and the diag
 * could not see it.
 *
 * Two primitives, both cheap (64 sampled pixels, sub-microsecond):
 *   midp_present_live_changed  - sampled hash of the live framebuffer
 *       changed since the previous call. The frontend calls it once per
 *       present; the diag lvc= field counts per period. lvc == fl means
 *       EVERYTHING the game drew reached the screen (nothing dropped);
 *       lvc >> fl means the live framebuffer animates faster than the
 *       settled snapshot — frames ARE being withheld at scanout.
 *   midp_present_settle_if_stale - the delivery path: when (a) a settled
 *       snapshot exists, (b) NO frame is in flight (midp_canvas_mid_frame:
 *       not inside paint(), no open M3G bind — the green-blob tearing
 *       class stays impossible by construction), and (c) the live
 *       framebuffer differs from the settled snapshot at the sample
 *       sites — re-snapshot right here (full memcpy + seq bump). The
 *       next stable_copy in the same present then scans out the fresh
 *       content. Gated + called once per present = at most one extra
 *       settle per frontend frame, pump-paced games are unaffected
 *       (their live framebuffer only changes inside the pump, which
 *       settles at paint completion before the present runs). */
#define MIDP_STALE_SAMPLES 64

extern int midp_canvas_mid_frame(void);  /* defined below (paint depth + M3G bind gate) */

static uint32_t midp_stale_sample_hash(const uint32_t* px, int w, int h) {
    uint32_t hash = 2166136261u;
    int i;
    for (i = 0; i < MIDP_STALE_SAMPLES; i++) {
        /* Deterministic 8x8 grid of sample sites spread over the screen. */
        int r = (i >> 3) * h / 8;
        int c = (i & 7) * w / 8;
        uint32_t p = px[(size_t)r * w + c];
        hash ^= p; hash *= 16777619u;
    }
    return hash;
}

static uint32_t s_live_last_hash = 0;
static int s_live_hash_valid = 0;

int midp_present_live_changed(void) {
    SdlContext* ctx = sdl_get_global_context();
    uint32_t hash;
    if (!ctx || !ctx->framebuffer || ctx->width <= 0 || ctx->height <= 0)
        return 0;
    hash = midp_stale_sample_hash(ctx->framebuffer, ctx->width, ctx->height);
    if (!s_live_hash_valid) {
        s_live_hash_valid = 1;
        s_live_last_hash = hash;
        return 0;
    }
    if (hash != s_live_last_hash) {
        s_live_last_hash = hash;
        return 1;
    }
    return 0;
}

int midp_present_settle_if_stale(void) {
    SdlContext* ctx;
    int stale = 0;
    if (g_midp_stable_seq == 0) return 0;   /* bootstrap: presenter shows live */
    if (midp_canvas_mid_frame()) return 0;  /* paint()/M3G bind in flight */
    ctx = sdl_get_global_context();
    if (!ctx || !ctx->framebuffer || ctx->width <= 0 || ctx->height <= 0)
        return 0;
    stable_lock_bounded("stale");
    if (g_midp_stable_pixels && g_midp_stable_w == ctx->width &&
        g_midp_stable_h == ctx->height) {
        int w = ctx->width, h = ctx->height;
        uint32_t a = midp_stale_sample_hash(g_midp_stable_pixels, w, h);
        uint32_t b = midp_stale_sample_hash(ctx->framebuffer, w, h);
        if (a != b) {
            /* Divergence confirmed at the sample sites: deliver the live
             * content as the new settled frame. Racy reads of the live
             * buffer are accepted here BY DESIGN — this is exactly the
             * asynchronous LCD scanout a real phone performs while the
             * game thread draws; the pump-driven paint path stays fully
             * protected by midp_canvas_mid_frame(). */
            long pxl = (long)w * (long)h;
            memcpy(g_midp_stable_pixels, ctx->framebuffer,
                   (size_t)pxl * sizeof(uint32_t));
            g_midp_stable_seq++;
            g_stable_last_settle_ms = stable_mono_ms();
            stale = 1;
        }
    }
    stable_unlock();
    return stale;
}

/* v35.04 [NO-FRAME] watchdog context (read from the SDL timer thread; all
 * reads are deliberately racy snapshots — diagnostics only):
 *   out_keyq         - pending key events in the deferred queue (a game
 *                      waiting for input with a full queue = delivery bug;
 *                      an empty queue = the game has not polled yet)
 *   out_paint_pending - a repaint is requested but not pumped (game stuck
 *                      on the paint handshake)
 *   out_pump_active  - the v35.02 external-pump protocol is armed
 *   cls_buf          - class name of the CURRENT displayable (which screen
 *                      the game is actually on) */
void midp_noframe_state(int* out_keyq, int* out_paint_pending,
                        int* out_pump_active, char* cls_buf, int cls_cap) {
    if (out_keyq) {
        pthread_mutex_lock(&midp_keyq_mutex);
        *out_keyq = (midp_keyq_tail - midp_keyq_head + MIDP_KEYQ_SIZE) % MIDP_KEYQ_SIZE;
        pthread_mutex_unlock(&midp_keyq_mutex);
    }
    if (out_paint_pending) *out_paint_pending = g_paint_pending ? 1 : 0;
    if (out_pump_active) *out_pump_active = g_midp_external_pump_active ? 1 : 0;
    if (cls_buf && cls_cap > 0) {
        cls_buf[0] = '\0';
        JavaObject* d = current_displayable_obj; /* racy snapshot */
        if (d && d->header.clazz && d->header.clazz->class_name)
            snprintf(cls_buf, (size_t)cls_cap, "%.40s", d->header.clazz->class_name);
    }
}

void midp_post_key_event(int keycode, int pressed) {
    pthread_mutex_lock(&midp_keyq_mutex);
    int next = (midp_keyq_tail + 1) % MIDP_KEYQ_SIZE;
    if (next == midp_keyq_head) {
        pthread_mutex_unlock(&midp_keyq_mutex);
        return; /* overflow: drop */
    }
    midp_keyq[midp_keyq_tail].code = keycode;
    midp_keyq[midp_keyq_tail].pressed = pressed;
    midp_keyq[midp_keyq_tail].frame_posted = midp_frame_counter;
    midp_keyq[midp_keyq_tail].posted_ms = midp_keyq_mono_ms(); /* [YSFIX] */
    midp_keyq_tail = next;
    pthread_mutex_unlock(&midp_keyq_mutex);
}

int midp_keyq_peek_count(void) {
    pthread_mutex_lock(&midp_keyq_mutex);
    int n = (midp_keyq_tail - midp_keyq_head + MIDP_KEYQ_SIZE) % MIDP_KEYQ_SIZE;
    pthread_mutex_unlock(&midp_keyq_mutex);
    return n;
}

volatile int midp_game_thread_id = -1;   /* v34.9: thread that owns the Display */

int midp_defer_keys_enabled(void) {
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("NOJME_DEFER_KEYS");
        v = (e && e[0] == '0') ? 0 : 1;
    }
    return v;
}

/* Age (in frames) of the oldest queued entry; INT_MAX when empty. */
static int midp_keyq_front_age(int now_frame) {
    pthread_mutex_lock(&midp_keyq_mutex);
    int age;
    if (midp_keyq_head == midp_keyq_tail) {
        age = INT_MAX;
    } else {
        age = now_frame - midp_keyq[midp_keyq_head].frame_posted;
    }
    pthread_mutex_unlock(&midp_keyq_mutex);
    return age;
}

/* [YSFIX2] v36.40: device-trace helper for the midp layer. stderr never
 * reaches log.txt on Switch (only the sw_trace sink writes that file —
 * proved by the ysfix1 field log that showed ZERO [KEYQ-SOFT] lines while
 * the soft keys were being pressed), so the delivery trace must go through
 * sw_trace_force. Weak-linked: headless builds without the Switch frontend
 * resolve the symbol to 0 and skip. Mirrors RS_TRACE in native.c. */
#define KEYQ_TRACE(...) do { \
    extern void sw_trace_force(const char* fmt, ...) __attribute__((weak)); \
    if (&sw_trace_force && sw_trace_force) sw_trace_force(__VA_ARGS__); \
} while (0)

/* Called from the display-owning VM thread (execute.c poll). Drains the
 * pending key queue, invoking keyPressed/keyReleased impls serialized with
 * the game thread's own logic. */
void midp_process_pending_keys(JVM* jvm) {
    int code, pressed;
    uint64_t pms;
    /* v36.57 [KEY-TRYLOCK]: доставка отложенных клавиш на ИГРОВОМ потоке
     * больше НИКОГДА не ждёт монитор канвы. Ходовой ABBA-дедлок Gravity
     * Defied (хост-репро /tmp/gd_repro_gdb3, полевой класс «постоянные
     * подвисания по 5-10 с»):
     *   главный поток: держит КАНАВУ (i.keyPressed ACC_SYNC, стадия
     *     timers) -> m.case -> e.if -> ждёт ФИЗИКУ (b.case ACC_SYNC),
     *     которую держит игровой поток;
     *   игровой поток: держит ФИЗИКУ (b.do/b.case) -> его собственный
     *     slow-check дренажа клавиш вызывает i.keyReleased (ACC_SYNC
     *     КАНАВА) -> БЛОКИРУЕТСЯ -> цикл замкнут, оба мертвы до
     *     MON-STEAL (20 с) или вечно.
     * На реальном KVM клавиши доставляет ТОЛЬКО системный поток —
     * игровой поток никогда не входит в keyPressed/keyReleased изнутри
     * своей логики. Теперь: если монитор текущего дисплея занят — ключ
     * ОСТАЁТСЯ в очереди (его доставит фронтенд-фоллбек
     * midp_pump_deferred_keys через 3 кадра / wall-clock сеть), игровой
     * поток продолжает логику. Если монитор свободен — берём его НА ВСЁ
     * время доставки (ACC_SYNC-вход обработчика реентерантен) — окно
     * гонки между tryenter и вызовом обработчика закрыто. */
    int runner_trylock = 0;
    {
        extern int jvm_current_os_thread_is_vm_runner(void);
        if (jvm_current_os_thread_is_vm_runner()) {
            runner_trylock = 1;
        }
    }
    while (midp_keyq_pop2(&code, &pressed, &pms)) {
        if (runner_trylock && current_displayable_obj) {
            if (monitor_tryenter(jvm, (JavaObject*)current_displayable_obj) != JNI_OK) {
                /* Канва занята (обычно — главный поток в своём key-
                 * диспетче). Ключ возвращаем в ГОЛОВУ очереди и уходим:
                 * фоллбек-насос фронтенда доставит его владельцу. */
                midp_keyq_push_front(code, pressed, pms);
                {
                    static unsigned s_key_skip_logs = 0;
                    if (++s_key_skip_logs <= 40 || (s_key_skip_logs % 500) == 0) {
                        extern void sw_trace_force(const char* fmt, ...)
                            __attribute__((weak));
                        if (sw_trace_force) {
                            sw_trace_force("[KEYQ-TRYLOCK] canvas busy — game-thread key delivery deferred to frontend pump (queue head %s %d)",
                                           pressed ? "press" : "release", code);
                        }
                    }
                }
                return;
            }
        }
        /* [YSFIX] field trace: soft key reached a game-thread drain.
         * [YSFIX2] v36.40: + sw_trace (device log; stderr kept for host). */
        if (code == -6 || code == -7) {
            uint64_t now = midp_keyq_mono_ms();
            KEYQ_TRACE("[KEYQ-SOFT] %s(%d) via game-thread drain, waited %llums",
                       pressed ? "keyPressed" : "keyReleased", code,
                       (unsigned long long)((pms && now > pms) ? (now - pms) : 0));
            fprintf(stderr, "[KEYQ-SOFT] %s(%d) via game-thread drain, waited %llums\n",
                    pressed ? "keyPressed" : "keyReleased", code,
                    (unsigned long long)((pms && now > pms) ? (now - pms) : 0));
        }
        if (pressed) {
            midp_call_keyPressed_impl(jvm, code);
        } else {
            midp_call_keyReleased_impl(jvm, code);
        }
        if (runner_trylock && current_displayable_obj) {
            monitor_exit(jvm, (JavaObject*)current_displayable_obj);
        }
    }
}

/* v35 FIX (Bounce128-style event-driven menus: "loads to menu, menu draws
 * but ignores every key"): midlets whose menu is a high-level List/Form
 * return from startApp and then execute NO bytecode at all - the menu is
 * passive, only repaints are pumped by the frontend. The v34.9 design
 * delivers deferred keys exclusively from the interpreter poll of the
 * display-owning thread, which never runs again for such midlets, so every
 * posted key silently rots in the queue (reproducible headlessly: KEYSCRIPT
 * DOWN/DOWN did not move the List selection; NOJME_DEFER_KEYS=0 made the
 * same script work, proving the queue was the dead stage).
 *
 * The frontend frame pump now calls this once per frame. Entries younger
 * than KEYQ_FALLBACK_AGE frames are still left for the game thread (canvas
 * game loops drain them there, serialized with game logic - the whole point
 * of v34.9). Aged entries are delivered here because nobody else ever will:
 * a canvas loop runs every frame, so if the key survived KEYQ_FALLBACK_AGE
 * frames, there is no active game thread drain for the current screen. */
#define KEYQ_FALLBACK_AGE 3
void midp_pump_deferred_keys(JVM* jvm) {
    if (!jvm) return;
    if (!midp_defer_keys_enabled()) return;
    /* Don't deliver from inside a paint: keyPressed may repaint, recursing
     * the pump (same guard thread_yield() uses for event pumping). */
    {
        extern int midp_is_inside_repaint_pump(void);
        if (midp_is_inside_repaint_pump()) return;
    }
    int now = midp_frame_counter;

    /* v34.27: when the CURRENT displayable is a passive high-level screen
     * (Form/List/TextBox/Alert), no game loop will ever drain young entries
     * - the 3-frame wait was pure input lag (~50ms at 60fps) on every menu
     * navigation. Deliver immediately. Canvas stays age-gated so canvas
     * game loops keep their serialized drain. */
    int front_age = midp_keyq_front_age(now);
    if (front_age == INT_MAX) return;
    bool immediate =
        (midp_displayable_kind((JavaObject*)current_displayable_obj) != MIDP_UI_KIND_CANVAS &&
         midp_displayable_kind((JavaObject*)current_displayable_obj) != MIDP_UI_KIND_OTHER);
    if (!immediate && front_age < KEYQ_FALLBACK_AGE) {
        /* [YSFIX] WALL-CLOCK SAFETY NET. The 3-frame grace assumes the
         * display-owning VM thread is alive and will drain young entries
         * serialized with game logic. That assumption fails for the
         * Yeti-Sports architecture (Display.setCurrent() called from the
         * MIDlet thread which then goes idle forever; the game loop runs
         * on a SECOND thread that is not the registered drain owner), and
         * any frame-pump hiccup then turns into a dead key. A live game
         * thread drains within one tick (~16-50 ms); if the head entry
         * has waited KEYQ_WALLCLOCK_NET_MS of wall time, nobody is
         * coming — deliver now. */
        uint64_t head_age_ms = midp_keyq_front_age_ms();
        if (head_age_ms == UINT64_MAX || head_age_ms < KEYQ_WALLCLOCK_NET_MS)
            return;
        DISP_DEBUG("[KEYQ] wall-clock net fired: head waited %llu ms", (unsigned long long)head_age_ms);
    }

    int delivered = 0;
    int code, pressed;
    uint64_t pms;
    while (midp_keyq_pop2(&code, &pressed, &pms)) {
        delivered++;
        /* [YSFIX] field trace: soft key reached the fallback pump.
         * [YSFIX2] v36.40: + sw_trace (device log; stderr kept for host). */
        if (code == -6 || code == -7) {
            uint64_t now = midp_keyq_mono_ms();
            KEYQ_TRACE("[KEYQ-SOFT] %s(%d) via fallback pump, waited %llums",
                       pressed ? "keyPressed" : "keyReleased", code,
                       (unsigned long long)((pms && now > pms) ? (now - pms) : 0));
            fprintf(stderr, "[KEYQ-SOFT] %s(%d) via fallback pump, waited %llums\n",
                    pressed ? "keyPressed" : "keyReleased", code,
                    (unsigned long long)((pms && now > pms) ? (now - pms) : 0));
        }
        if (pressed) {
            midp_call_keyPressed_impl(jvm, code);
        } else {
            midp_call_keyReleased_impl(jvm, code);
        }
        /* Stop when a keyPressed switched us into a canvas whose game loop
         * should serialize the rest (or the queue emptied - the pop checks). */
        if (midp_displayable_kind((JavaObject*)current_displayable_obj) == MIDP_UI_KIND_CANVAS &&
            midp_keyq_front_age(now) < KEYQ_FALLBACK_AGE) {
            break;
        }
    }
    if (delivered) {
        DISP_DEBUG("[KEYQ] fallback delivered %d key(s) (frame %d)", delivered, now);
    }
}

/* v34.68: forward declaration — the key path requests a repaint through
 * the same queue the game's Canvas.repaint() uses (see the fix below). */
static void canvas_request_repaint(JavaObject* canvas, bool clear_dirty);

/* [YS-STATE] v36.46 (yetifix3): диагностика Yeti Sports — при игровой
 * клавише на канве 'm' дампим статические поля состояния ввода классов
 * o (очередь событий k, held i, released h, счётчик красок g, тик
 * последнего LSK c:I, свап софтов c:Z, цели софтов l/m, флаг занятости
 * b:Z) и l (режим a:I, загрузка m:Z, есть-выбор j:Z, есть-назад k:Z).
 * Назначение: найти, кто глотает софт-клавишу на диалоге звука (экран 20).
 * Гейт: env NOJME_YS_STATE=1; вывод в LOG_SAFE (stderr на хосте). */
static void ys_state_dump(JVM* jvm, const char* when, int keycode) {
    static int on = -1;
    if (on < 0)
        on = getenv("NOJME_YS_STATE") && getenv("NOJME_YS_STATE")[0] != '0';
    if (!on) return;
    if (!current_displayable_obj ||
        !current_displayable_obj->header.clazz ||
        !current_displayable_obj->header.clazz->class_name ||
        strcmp(current_displayable_obj->header.clazz->class_name, "m") != 0)
        return;
    struct { const char* cls; const char* name; const char* desc; } want[] = {
        {"o", "k", "I"}, {"o", "i", "I"}, {"o", "h", "I"}, {"o", "g", "I"},
        {"o", "c", "I"}, {"o", "l", "I"}, {"o", "m", "I"}, {"o", "b", "Z"},
        {"o", "c", "Z"}, {"l", "a", "I"}, {"l", "m", "Z"}, {"l", "j", "Z"},
        {"l", "k", "Z"}, {"l", "j", "I"}, {"l", "d", "I"},
    };
    char buf[512];
    int len = snprintf(buf, sizeof(buf), "[YS-STATE] %s key=%d", when, keycode);
    for (unsigned w = 0; w < sizeof(want) / sizeof(want[0]) && len < (int)sizeof(buf) - 24; w++) {
        JavaClass* c = jvm_load_class(jvm, want[w].cls);
        int found = 0;
        if (c && c->static_fields) {
            for (int s = 0; s < c->static_fields_count; s++) {
                if (c->static_fields[s].name && c->static_fields[s].descriptor &&
                    strcmp(c->static_fields[s].name, want[w].name) == 0 &&
                    strcmp(c->static_fields[s].descriptor, want[w].desc) == 0) {
                    len += snprintf(buf + len, sizeof(buf) - len, " %s.%s%s=%d",
                                    want[w].cls, want[w].name, want[w].desc,
                                    c->static_fields[s].value.i);
                    found = 1;
                    break;
                }
            }
        }
        if (!found)
            len += snprintf(buf + len, sizeof(buf) - len, " %s.%s%s=?",
                            want[w].cls, want[w].name, want[w].desc);
    }
    LOG_SAFE("%s\n", buf);
}

void midp_call_keyPressed_impl(JVM* jvm, int keycode) {
    /* v36.01 KEY-HOLD HARDENING: raise the held/latch bits FIRST — before
     * any early return. A press delivered while the displayable pointer is
     * mid-transition (setCurrent, pause overlay, midlet switch) must still
     * be seen by getKeyStates(); the v35.09 placement after the displayable
     * checks silently DROPPED such presses (bit never raised -> dead key
     * until the next re-press). Atomic OR: producers run on the frontend
     * key-delivery thread, the consumer (getKeyStates) on game threads. */
    if (keycode < 0) {
        int bit = keycode_to_keystate_bit(keycode);
        if (bit) {
            __sync_fetch_and_or(&game_canvas.key_latch, bit);
            __sync_fetch_and_or(&game_canvas.key_level, bit);
        }
    }

    if (!jvm || !current_displayable_obj) {
        DISP_DEBUG("[KEY] keyPressed: no current displayable (jvm=%p, current=%p)",
                (void*)jvm, (void*)current_displayable_obj);
        return;
    }
    
    JavaObject* displayable = current_displayable_obj;
    JavaClass* clazz = displayable->header.clazz;
    
    if (!clazz) {
        DISP_DEBUG("[KEY] keyPressed: displayable has no class");
        return;
    }
    
    /* FIX (task 16-b #26): latch GameCanvas key state. Event-driven latching
     * makes keys tapped between two getKeyStates() polls observable. Placed
     * BEFORE the GameCanvas suppression check: suppression only stops the
     * keyPressed() callback, the physical event still happened. */
    /* v35.09: also raise the LEVEL bit — getKeyStates() must report the key
     * as DOWN until keyReleased (the latch alone was consumed by the game's
     * first poll, killing held controls in getKeyStates-polled games).
     * v36.01: both bits are now raised unconditionally at the TOP of this
     * function (before every early return) — see the hardening note there. */
    
    /* DEBUG: Log keyPressed for class 'a' (Bobby Carrot) */
    static int key_log_count = 0;
    if (clazz->class_name && strcmp(clazz->class_name, "a") == 0 && key_log_count < 10) {
        DISP_DEBUG("[KEY_EVENT] keyPressed(%d) on class 'a'", keycode);
        key_log_count++;
    }

    /* [KEYDIAG] v34.9: trace every delivered key event (first 60, then
     * every 500th) to diagnose menu/hang key-flow issues (Bounce Tales). */
    {
        static int keydiag_n = 0;
        int n = ++keydiag_n;
        if (n <= 60 || (n % 500) == 0) {
            LOG_SAFE("[KEYDIAG] keyPressed(%d) -> displayable=%s suppress=%d\n",
                     keycode,
                     (clazz->class_name && clazz->class_name[0]) ? clazz->class_name : "?",
                     game_canvas.suppress_key_events ? 1 : 0);
        }
    }

    DISP_DEBUG("[KEY] Current displayable class: %s",
            clazz->class_name ? clazz->class_name : "NULL");

    /* Check if this is a GameCanvas with suppressed key events */
    if (game_canvas.suppress_key_events && keycode < 0) {
        /* Game actions (negative keycodes) are suppressed for GameCanvas */
        bool is_game_canvas = false;
        JavaClass* check = clazz;
        while (check) {
            if (check->class_name && strcmp(check->class_name, "javax/microedition/lcdui/game/GameCanvas") == 0) {
                is_game_canvas = true;
                break;
            }
            check = check->super_class;
        }
        if (is_game_canvas) {
            DISP_DEBUG("[KEY] keyPressed: suppressed for GameCanvas (keycode=%d)", keycode);
            LOG_SAFE("[KEYDIAG] keyPressed(%d) SUPPRESSED by suppress_key_events\n", keycode);
            return;
        }
    }
    
    /* Check if this is a high-level UI that needs form key handling */
    if (clazz->class_name) {
        bool is_high_level_ui = false;
        JavaClass* check = clazz;
        while (check) {
            if (check->class_name) {
                if (strcmp(check->class_name, "javax/microedition/lcdui/Form") == 0 ||
                    strcmp(check->class_name, "javax/microedition/lcdui/List") == 0 ||
                    strcmp(check->class_name, "javax/microedition/lcdui/TextBox") == 0 ||
                    strcmp(check->class_name, "javax/microedition/lcdui/Alert") == 0) {
                    is_high_level_ui = true;
                    break;
                }
            }
            check = check->super_class;
        }
        
        if (is_high_level_ui) {
            /* Handle key for high-level UI (forms, lists, virtual keyboard) */
            DISP_DEBUG("keyPressed: high-level UI detected (%s)", clazz->class_name);
            
            /* Convert keycode to MIDP game action constant */
            /* Keycodes: -1=UP, -2=DOWN, -3=LEFT, -4=RIGHT, -5=FIRE, -6=SOFT1, -7=SOFT2 */
            /* Game actions: UP=1, DOWN=6, LEFT=2, RIGHT=5, FIRE=8 */
            int game_action = 0;
            switch (keycode) {
                case -1: game_action = 1; break;  /* GAME_UP */
                case -2: game_action = 6; break;  /* GAME_DOWN */
                case -3: game_action = 2; break;  /* GAME_LEFT */
                case -4: game_action = 5; break;  /* GAME_RIGHT */
                case -5: game_action = 8; break;  /* GAME_FIRE */
                /* v34.65: ITU-T keypad navigation for high-level UIs — on
                 * real phones '2'/'8'/'4'/'6'/'5' also drive List/Form
                 * traversal (Nescube menus are Lists). Canvas screens are
                 * unaffected: they receive the RAW keyCode below. */
                case 50: game_action = 1; break;  /* '2' = UP */
                case 56: game_action = 6; break;  /* '8' = DOWN */
                case 52: game_action = 2; break;  /* '4' = LEFT */
                case 54: game_action = 5; break;  /* '6' = RIGHT */
                case 53: game_action = 8; break;  /* '5' = FIRE */
                default: game_action = keycode; break;
            }
            
            /* First check if command menu is open (for both SDL and libretro) */
            extern bool midp_is_command_menu_open(void);
            extern bool midp_handle_menu_navigation(int direction);
            if (midp_is_command_menu_open()) {
                if (game_action == 1) { /* UP */
                    midp_handle_menu_navigation(-1);
                    return;
                } else if (game_action == 6) { /* DOWN */
                    midp_handle_menu_navigation(1);
                    return;
                } else if (game_action == 8) { /* FIRE - confirm selection */
                    midp_handle_soft_button(jvm, 1);
                    return;
                }
            }
            /* v36.30 [ITEM-CMDS]: item command menu (3+ commands on the
             * focused item) — same navigation semantics as above. */
            {
                extern bool midp_form_item_menu_active(void);
                extern bool midp_form_item_menu_nav(int dir);
                extern bool midp_form_item_menu_select(JVM* jvm);
                if (midp_form_item_menu_active()) {
                    if (game_action == 1) { /* UP */
                        midp_form_item_menu_nav(-1);
                        return;
                    } else if (game_action == 6) { /* DOWN */
                        midp_form_item_menu_nav(1);
                        return;
                    } else if (game_action == 8) { /* FIRE - confirm */
                        midp_form_item_menu_select(jvm);
                        return;
                    }
                }
            }
            
            /* v34.29 FIX (Form soft keys): soft keys on high-level UIs must
             * activate COMMANDS — on MIDP the AMS binds commands to the soft
             * keys and Canvas.keyPressed() is not called. The SDL and
             * libretro backends pre-intercept -6/-7 and call
             * midp_handle_soft_button(), but direct callers of
             * midp_call_keyPressed (the headless key script/keyseq harness,
             * the deferred-key pump) bypassed that interception: on
             * JBenchmark 3D's welcome Form the "Start" soft key press died
             * silently (midp_form_handle_key has no -6/-7 case, and Form
             * has no keyPressed method to deliver to). Route -6/-7 through
             * the soft-button dispatcher here; if it declines (no
             * commands), the key falls through to normal form handling,
             * preserving pass-through semantics. */
            if (keycode == -6 || keycode == -7) {
                extern bool midp_handle_soft_button(JVM* jvm, int button_index);
                if (midp_handle_soft_button(jvm, keycode == -6 ? 0 : 1)) {
                    LOG_SAFE("[KEYDIAG] soft key %d -> command dispatched\n", keycode);
                    return;
                }
            }
            
            /* Then check if virtual keyboard is active */
            if (midp_is_vkb_active()) {
                midp_vkb_process_key(jvm, game_action);
                /* Re-render the form/textbox. v34.27: kind-based dispatch -
                 * the old strstr(class_name, "Form") re-rendered LISTS
                 * whose class name merely contains "Form"
                 * (glomoRegForms/ActivateForm) as if they were Forms. */
                if (midp_is_vkb_active()) {
                    /* Keyboard still active, re-render */
                    int k = midp_displayable_kind(displayable);
                    if (k == MIDP_UI_KIND_FORM) {
                        midp_render_form(jvm, displayable);
                    } else if (k == MIDP_UI_KIND_TEXTBOX) {
                        midp_render_textbox(jvm, displayable, "", 0);
                    }
                } else {
                    /* Keyboard closed, re-render form */
                    midp_render_form(jvm, displayable);
                }
                return;
            }
            
            /* Handle form navigation */
            if (midp_form_handle_key(jvm, game_action)) {
                DISP_DEBUG("Key handled by form handler");
                return;
            }
            
            /* Key not handled, fall through to default behavior */
        }
    }
    
    /* Find keyPressed method */
    JavaMethod* method = jvm_resolve_method(jvm, clazz, "keyPressed", "(I)V");
    if (!method) {
        LOG_SAFE("[KEYDIAG] keyPressed(%d): resolve FAILED on class %s\n",
                 keycode, clazz->class_name ? clazz->class_name : "?");
        return;
    }

    /* Prepare arguments: this + keycode */
    JavaValue args[2];
    args[0].ref = displayable;       /* this */
    args[1].i = keycode;             /* keyCode */

    JavaValue result;
    JavaThread* thread = jvm_current_thread(jvm);

    /* v58 HANG-WATCHDOG: if the key handler never returns, dump the blocked
     * Java stack once (the Coaster Rush country-menu hang: keyPressed
     * enters nanosleep forever inside some native). */
    {
        extern void keydiag_hang_begin(JavaThread* t);
        keydiag_hang_begin(thread);
    }
    ys_state_dump(jvm, "pre", keycode);
    int ret = execute_method(jvm, thread, method, args, &result);
    ys_state_dump(jvm, "post", keycode);
    {
        extern void keydiag_hang_end(void);
        keydiag_hang_end();
    }
    DISP_DEBUG("[Input] keyPressed returned %d", ret);
    {
        static int keydiag_exec_n = 0;
        int n = ++keydiag_exec_n;
        if (ret != 0 || n <= 20 || (n % 200) == 0) {
            LOG_SAFE("[KEYDIAG] keyPressed(%d) executed on %s ret=%d m=%p flags=0x%x len=%d\n",
                     keycode, clazz->class_name ? clazz->class_name : "?", ret,
                     (void*)method, method->access_flags,
                     method->code.code ? method->code.code_length : 0);
        }
    }
    
    /* v34.69 FIX (Captain Fatal 3D black screen): NO repaint request after
     * keyPressed - MIDP never implies a redraw after key delivery, the
     * MIDlet repaints itself (repaint()/serviceRepaints() from its own
     * handlers; Stalker's b.keyPressed handlers all end with Main.a.d()).
     * v34.68 had replaced the old synchronous paint() with a QUEUED
     * request here - safer than the sync paint (which re-entered Stalker's
     * loader mid-paint -> crash-loop), but still not phone-faithful: a
     * "draw-once" canvas (Captain Fatal's splash draws each state's image
     * exactly once, nulls the field, auto-advances via Timer) got a SECOND
     * paint of the same state from every key event -> the nulled image
     * field NPE'd -> fillRect(black) already executed -> black screen +
     * NPE loop on every later repaint. Removing the request restores
     * hardware semantics; games that repaint on keys still get their own
     * repaint() through the pump. */
}

/* FIX (audit M-3, v18): deliver key repeat as keyRepeated() — and only to
 * canvases that actually implement it. The SDL layer previously re-sent
 * keyPressed(), so games got phantom duplicate presses (auto-fire on menus).
 * The repeat is delivered only if some class BELOW the framework Canvas stub
 * really implements keyRepeated(I)V with bytecode; framework stubs declare
 * the method as ACC_NATIVE with no code. */
void midp_call_keyRepeated(JVM* jvm, int keycode) {
    if (!jvm || !current_displayable_obj) return;
    
    JavaObject* canvas = current_displayable_obj;
    JavaClass* clazz = canvas->header.clazz;
    if (!clazz) return;
    
    /* Walk the hierarchy ourselves */
    JavaMethod* method = NULL;
    for (JavaClass* check = clazz; check && check->class_name; check = check->super_class) {
        bool is_framework_stub =
            strcmp(check->class_name, "javax/microedition/lcdui/Canvas") == 0 ||
            strcmp(check->class_name, "javax/microedition/lcdui/Screen") == 0 ||
            strcmp(check->class_name, "javax/microedition/lcdui/Displayable") == 0;
        
        for (uint16_t i = 0; i < check->methods_count; i++) {
            JavaMethod* m = &check->methods[i];
            if (!m->name || !m->descriptor) continue;
            if (strcmp(m->name, "keyRepeated") != 0 || strcmp(m->descriptor, "(I)V") != 0) continue;
            
            if (!is_framework_stub && !m->is_native && m->code.code_length > 0) {
                method = m;   /* real user override found */
            }
        }
        if (is_framework_stub) break;  /* stop at the base stub */
    }
    
    if (!method) {
        DISP_DEBUG("[KEY] keyRepeated dropped - displayable does not implement keyRepeated");
        return;
    }
    
    DISP_DEBUG("[Input] Calling keyRepeated(%d) on %s", keycode,
               clazz->class_name ? clazz->class_name : "?");
    
    JavaValue args[2];
    args[0].ref = canvas;
    args[1].i = keycode;
    
    JavaValue result;
    JavaThread* thread = jvm_current_thread(jvm);
    execute_method(jvm, thread, method, args, &result);
}

/* Call keyReleased on the current displayable */
void midp_call_keyReleased_impl(JVM* jvm, int keycode) {
    /* v36.01 KEY-HOLD HARDENING: clear the held bit FIRST — before every
     * early return. The v35.09 clear sat AFTER the jvm/displayable checks,
     * so a release delivered while current_displayable_obj was NULL (pause
     * overlay open, setCurrent transition, midlet switch) left the held bit
     * STUCK: the game then read a permanently-held key, and a press lost to
     * the same early return made the control dead until a re-press. The
     * physical event happened regardless of displayable state — mirror of
     * the press-side placement below. Atomic: delivery thread vs game thread
     * polling. */
    if (keycode < 0) {
        int rbit = keycode_to_keystate_bit(keycode);
        if (rbit) __sync_fetch_and_and(&game_canvas.key_level, ~rbit);
    }

    if (!jvm || !current_displayable_obj) return;
    
    JavaObject* canvas = current_displayable_obj;
    JavaClass* clazz = canvas->header.clazz;
    if (!clazz) return;
    
    /* v35.09: drop the LEVEL bit before any early return — a held key must
     * read as UP from the next getKeyStates() even when the keyReleased()
     * callback is suppressed for GameCanvas (the physical event happened).
     * v36.01: the unconditional clear above already covers every path; this
     * spot only guards the suppressed-callback branch now. */
    
    /* Check if this is a GameCanvas with suppressed key events */
    if (game_canvas.suppress_key_events && keycode < 0) {
        /* Game actions (negative keycodes) are suppressed for GameCanvas */
        bool is_game_canvas = false;
        JavaClass* check = clazz;
        while (check) {
            if (check->class_name && strcmp(check->class_name, "javax/microedition/lcdui/game/GameCanvas") == 0) {
                is_game_canvas = true;
                break;
            }
            check = check->super_class;
        }
        if (is_game_canvas) {
            DISP_DEBUG("keyReleased: suppressed for GameCanvas (keycode=%d)", keycode);
            return;
        }
    }
    
    JavaMethod* method = jvm_resolve_method(jvm, clazz, "keyReleased", "(I)V");
    if (!method) return;
    
    DISP_DEBUG("[Input] Calling keyReleased(%d) on %s", keycode, clazz->class_name);
    
    JavaValue args[2];
    args[0].ref = canvas;
    args[1].i = keycode;
    
    JavaValue result;
    JavaThread* thread = jvm_current_thread(jvm);
    execute_method(jvm, thread, method, args, &result);
}

/* Call pointerPressed on the current displayable */
/* v34.9: public entry — defers to the VM-thread queue unless disabled. */
void midp_call_keyPressed(JVM* jvm, int keycode) {
    if (midp_defer_keys_enabled()) {
        midp_post_key_event(keycode, 1);
    } else {
        midp_call_keyPressed_impl(jvm, keycode);
    }
}

void midp_call_keyReleased(JVM* jvm, int keycode) {
    if (midp_defer_keys_enabled()) {
        midp_post_key_event(keycode, 0);
    } else {
        midp_call_keyReleased_impl(jvm, keycode);
    }
}

void midp_call_pointerPressed(JVM* jvm, int x, int y) {
    if (!jvm || !current_displayable_obj) return;
    
    JavaObject* canvas = current_displayable_obj;
    JavaClass* clazz = canvas->header.clazz;
    if (!clazz) return;
    
    JavaMethod* method = jvm_resolve_method(jvm, clazz, "pointerPressed", "(II)V");
    if (!method) {
        DISP_DEBUG("[Input] pointerPressed method not found in %s", clazz->class_name);
        return;
    }
    
    DISP_DEBUG("[Input] Calling pointerPressed(%d, %d) on %s", x, y, clazz->class_name);
    
    JavaValue args[3];
    args[0].ref = canvas;
    args[1].i = x;
    args[2].i = y;
    
    JavaValue result;
    JavaThread* thread = jvm_current_thread(jvm);
    execute_method(jvm, thread, method, args, &result);
}

/* Call pointerReleased on the current displayable */
void midp_call_pointerReleased(JVM* jvm, int x, int y) {
    if (!jvm || !current_displayable_obj) return;
    
    JavaObject* canvas = current_displayable_obj;
    JavaClass* clazz = canvas->header.clazz;
    if (!clazz) return;
    
    JavaMethod* method = jvm_resolve_method(jvm, clazz, "pointerReleased", "(II)V");
    if (!method) {
        DISP_DEBUG("[Input] pointerReleased method not found in %s", clazz->class_name);
        return;
    }
    
    DISP_DEBUG("[Input] Calling pointerReleased(%d, %d) on %s", x, y, clazz->class_name);
    
    JavaValue args[3];
    args[0].ref = canvas;
    args[1].i = x;
    args[2].i = y;
    
    JavaValue result;
    JavaThread* thread = jvm_current_thread(jvm);
    execute_method(jvm, thread, method, args, &result);
}

/* Call pointerDragged on the current displayable */
void midp_call_pointerDragged(JVM* jvm, int x, int y) {
    if (!jvm || !current_displayable_obj) return;
    
    JavaObject* canvas = current_displayable_obj;
    JavaClass* clazz = canvas->header.clazz;
    if (!clazz) return;
    
    JavaMethod* method = jvm_resolve_method(jvm, clazz, "pointerDragged", "(II)V");
    if (!method) {
        DISP_DEBUG("[Input] pointerDragged method not found in %s", clazz->class_name);
        return;
    }
    
    DISP_DEBUG("[Input] Calling pointerDragged(%d, %d) on %s", x, y, clazz->class_name);
    
    JavaValue args[3];
    args[0].ref = canvas;
    args[1].i = x;
    args[2].i = y;
    
    JavaValue result;
    JavaThread* thread = jvm_current_thread(jvm);
    execute_method(jvm, thread, method, args, &result);
}

/* v34.33: DEFERRED showNotify (MIDP spec §Display.setCurrent: "the request
 * is generally processed asynchronously" — showNotify fires on the event
 * loop, NOT inside the setCurrent call).
 * Rally 3D (Nokia) does setCurrent(canvas) then Thread(loader).start() —
 * its /title PNG files load asynchronously in run(). With the old
 * synchronous showNotify, k() ran drawImage(null) before the loader
 * thread got a single slice -> NullPointerException -> startApp
 * aborted -> white screen, 2.8K instructions total.
 * The pending showNotify is delivered by midp_pump_pending_shownotify()
 * from the frontend frame loop (headless + libretro), BEFORE repaints and
 * callSerially, so the canvas still sees showNotify before its first
 * paint. Superseded requests coalesce (only the latest displayable gets
 * the callback; an older pending one is dropped silently). */
static JavaObject* g_pending_shownotify_displayable = NULL;

void midp_call_showNotify(JVM* jvm);  /* defined below */

void midp_defer_shownotify(JavaObject* displayable) {
    g_pending_shownotify_displayable = displayable;
}

void midp_pump_pending_shownotify(JVM* jvm) {
    JavaObject* target = g_pending_shownotify_displayable;
    if (!target) return;
    g_pending_shownotify_displayable = NULL;
    if (target != current_displayable_obj) return;  /* superseded by a newer setCurrent */
    midp_call_showNotify(jvm);
}

/* v36.58 [SHOWNOTIFY-EX]: UI-thread isolation for the lifecycle callbacks.
 * The pump executes showNotify/hideNotify/sizeChanged OUTSIDE any Java
 * frame — an escaping exception (Rally 3D field log v36.57: the stale
 * deferred showNotify(d) hit d.showNotify AFTER the splash thread had
 * nulled its images -> NullPointerException at d.k PC=42) used to LINGER
 * in thread->pending_exception and detonate inside unrelated frontend
 * Java (timers, callSerially, the next paint) — the paint pump got this
 * isolation in v34, the lifecycle callbacks did not. Same pattern: log,
 * clear, keep the MIDlet alive. */
static void midp_lifecycle_exc_clear(JVM* jvm, JavaThread* th,
                                     const char* cb, const char* cls) {
    if (!th || !th->pending_exception) return;
    JavaObject* exc = th->pending_exception;
    const char* exc_cls = (exc && exc->header.clazz && exc->header.clazz->class_name)
                          ? exc->header.clazz->class_name : "?";
    LOG_SAFE("[DISPLAY] %s() threw %s on %s - cleared (UI-thread isolation)\n",
             cb, exc_cls, cls ? cls : "?");
    th->pending_exception = NULL;
    if (th->exception_stack_trace) {
        free(th->exception_stack_trace);
        th->exception_stack_trace = NULL;
    }
    if (th->exception_throw_info) {
        free(th->exception_throw_info);
        th->exception_throw_info = NULL;
    }
    (void)jvm;
}

/* Call showNotify on the current displayable */
void midp_call_showNotify(JVM* jvm) {
    if (!jvm || !current_displayable_obj) return;

    JavaObject* displayable = current_displayable_obj;
    JavaClass* clazz = displayable->header.clazz;
    if (!clazz) return;

    JavaMethod* method = jvm_resolve_method(jvm, clazz, "showNotify", "()V");
    if (!method) return;  /* Not all classes implement this */

    fprintf(stderr, "[DISPLAY] Calling showNotify() on %s\n", clazz->class_name);

    JavaValue args[1];
    args[0].ref = displayable;

    JavaValue result;
    JavaThread* thread = jvm_current_thread(jvm);
    if (execute_method(jvm, thread, method, args, &result) != 0) {
        midp_lifecycle_exc_clear(jvm, thread, "showNotify",
                                 clazz->class_name ? clazz->class_name : "?");
    }
    fprintf(stderr, "[DISPLAY] showNotify() completed\n");
}

/* Call hideNotify on the current displayable */
void midp_call_hideNotify(JVM* jvm) {
    if (!jvm || !current_displayable_obj) return;

    JavaObject* displayable = current_displayable_obj;
    JavaClass* clazz = displayable->header.clazz;
    if (!clazz) return;

    JavaMethod* method = jvm_resolve_method(jvm, clazz, "hideNotify", "()V");
    if (!method) return;  /* Not all classes implement this */

    DISP_DEBUG("Calling hideNotify() on %s", clazz->class_name);

    JavaValue args[1];
    args[0].ref = displayable;

    JavaValue result;
    JavaThread* thread = jvm_current_thread(jvm);
    if (execute_method(jvm, thread, method, args, &result) != 0) {
        midp_lifecycle_exc_clear(jvm, thread, "hideNotify",
                                 clazz->class_name ? clazz->class_name : "?");
    }
}

/* Call sizeChanged on the current displayable */
void midp_call_sizeChanged(JVM* jvm, int w, int h) {
    if (!jvm || !current_displayable_obj) return;
    
    JavaObject* displayable = current_displayable_obj;
    JavaClass* clazz = displayable->header.clazz;
    if (!clazz) return;
    
    JavaMethod* method = jvm_resolve_method(jvm, clazz, "sizeChanged", "(II)V");
    if (!method) return;  /* Not all classes implement this */
    
    DISP_DEBUG("Calling sizeChanged(%d, %d) on %s", w, h, clazz->class_name);
    
    JavaValue args[3];
    args[0].ref = displayable;
    args[1].i = w;
    args[2].i = h;

    JavaValue result;
    JavaThread* thread = jvm_current_thread(jvm);
    if (execute_method(jvm, thread, method, args, &result) != 0) {
        midp_lifecycle_exc_clear(jvm, thread, "sizeChanged",
                                 clazz->class_name ? clazz->class_name : "?");
    }
}

/* v23 (task 16-b #24): called by graphics.c when the screen geometry
 * changes — delivers sizeChanged to the current displayable and schedules a
 * repaint so the new size takes effect on screen. */
void midp_display_dimensions_changed(JVM* jvm, int w, int h) {
    if (!jvm) return;
    midp_call_sizeChanged(jvm, w, h);
    /* Repaint so the frame buffer matches the new geometry */
    midp_display_repaint(0, 0, w, h);
}

/* Queue event */
void midp_event_queue(MidpEvent* event) {
    if (event_queue.count >= MAX_EVENTS) return;
    
    event_queue.events[event_queue.tail] = *event;
    event_queue.tail = (event_queue.tail + 1) % MAX_EVENTS;
    event_queue.count++;
}

/* Poll event */
bool midp_event_poll(MidpEvent* event) {
    if (event_queue.count == 0) return false;
    
    *event = event_queue.events[event_queue.head];
    event_queue.head = (event_queue.head + 1) % MAX_EVENTS;
    event_queue.count--;
    
    return true;
}

/* Process event */
void midp_event_process(JVM* jvm, MidpEvent* event) {
    if (!jvm || !event) return;
    
    /* TODO: Find the current Displayable and call appropriate methods */
    switch (event->type) {
        case MIDP_EVENT_KEY_PRESSED:
            /* Call keyPressed(int keyCode) */
            break;
            
        case MIDP_EVENT_KEY_RELEASED:
            /* Call keyReleased(int keyCode) */
            break;
            
        case MIDP_EVENT_KEY_REPEATED:
            /* Call keyRepeated(int keyCode) */
            break;
            
        case MIDP_EVENT_POINTER_PRESSED:
            /* Call pointerPressed(int x, int y) */
            break;
            
        case MIDP_EVENT_POINTER_RELEASED:
            /* Call pointerReleased(int x, int y) */
            break;
            
        case MIDP_EVENT_POINTER_DRAGGED:
            /* Call pointerDragged(int x, int y) */
            break;
            
        case MIDP_EVENT_COMMAND:
            /* Call commandAction */
            break;
            
        case MIDP_EVENT_SHOW_NOTIFY:
            /* Call showNotify */
            break;
            
        case MIDP_EVENT_HIDE_NOTIFY:
            /* Call hideNotify */
            break;
            
        case MIDP_EVENT_SIZE_CHANGED:
            /* Call sizeChanged */
            break;
            
        case MIDP_EVENT_REPAINT:
            /* Call paint */
            break;
    }
}

/* Set platform callbacks */
void midp_set_platform_callbacks(MidpPlatformCallbacks* callbacks) {
    if (callbacks) {
        platform_callbacks = *callbacks;
    }
}

/*
 * Display native methods
 */

/* Singleton Display instance */
static JavaObject* g_display_instance = NULL;

static JavaValue native_display_getDisplay(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    (void)args;  /* MIDlet parameter */
    
    DISP_DEBUG("getDisplay() called");
    
    /* Return singleton Display instance */
    if (!g_display_instance) {
        /* Create Display instance */
        JavaClass* display_class = jvm_load_class(jvm, "javax/microedition/lcdui/Display");
        if (display_class) {
            g_display_instance = jvm_new_object(jvm, display_class);
            /* Register as GC root to prevent collection */
            gc_add_root(jvm, (void**)&g_display_instance);
            DISP_DEBUG("Created Display instance: %p", (void*)g_display_instance);
        }
    }
    
    return NATIVE_RETURN_OBJECT(g_display_instance);
}

/* Shared setCurrent body (defined below) - used by both overloads (#5).
 * v34.72: push_history — see the emulated BACK-navigation stack above. */
static void display_set_current_internal(JVM* jvm, JavaObject* displayable,
                                         bool push_history);

static JavaValue native_display_setCurrent(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* display = (JavaObject*)args[0].ref;
    { extern volatile int midp_game_thread_id; midp_game_thread_id = thread ? thread->id : -1; }
    JavaObject* displayable = (JavaObject*)args[1].ref;
    (void)display;
    
    /* MIDP spec: setCurrent(null) clears the current displayable */
    if (!displayable) {
        DISP_DEBUG("setCurrent(null) — clearing current displayable");
        if (current_displayable_obj) {
            current_displayable_obj = NULL;
            midp_call_hideNotify(jvm);
            current_displayable_obj = NULL;
            midp_set_current_displayable(NULL);
        }
        return NATIVE_RETURN_VOID();
    }
    
    display_set_current_internal(jvm, displayable, true);
    return NATIVE_RETURN_VOID();
}

/* FIX (task 16-b #5): the two-arg overload Display.setCurrent(Alert, next)
 * was NOT registered, so form-based midlets fell into the unhooked-stub
 * fallback and never displayed anything. Registering it lets us remember
 * the next Displayable and return to it when the Alert is dismissed
 * (timeout or key press) - see midp_alert_restore_next_displayable(). */
static JavaObject* g_alert_next_displayable = NULL;
static bool g_alert_next_rooted = false;

static JavaValue native_display_setCurrent_alert(JVM* jvm, JavaThread* thread,
                                                 JavaValue* args, int arg_count) {
    (void)arg_count;
    /* v35: track display owner from the Alert overload too, otherwise keys
     * posted while an Alert is current are never picked up by the poll. */
    { extern volatile int midp_game_thread_id; midp_game_thread_id = thread ? thread->id : -1; }
    JavaObject* alert = (JavaObject*)args[1].ref;
    JavaObject* next = (JavaObject*)args[2].ref;
    
    DISP_DEBUG("setCurrent(Alert=%p, next=%p)", (void*)alert, (void*)next);
    
    /* Remember the follow-up screen; rooted lazily so the GC keeps it. */
    g_alert_next_displayable = next;
    if (next && !g_alert_next_rooted) {
        gc_add_root(jvm, (void**)&g_alert_next_displayable);
        g_alert_next_rooted = true;
    }
    
    if (!alert) return NATIVE_RETURN_VOID();
    display_set_current_internal(jvm, alert, true);
    return NATIVE_RETURN_VOID();
}

/* Restore the Displayable stored by setCurrent(Alert, next). Called after
 * the DISMISS commandAction was delivered (timeout / key press). No-op when
 * the application already switched away itself. */
void midp_alert_restore_next_displayable(JVM* jvm, JavaObject* alert) {
    JavaObject* next = g_alert_next_displayable;
    g_alert_next_displayable = NULL;
    if (!jvm || !next) return;
    if (current_displayable_obj && current_displayable_obj != alert) {
        DISP_DEBUG("[ALERT] next-screen restore skipped: app changed current");
        return;
    }
    DISP_DEBUG("[ALERT] restoring next displayable after dismiss");
    display_set_current_internal(jvm, next, false);
}

/* Shared body of Display.setCurrent(Displayable) - also used to restore the
 * next Displayable after an Alert is dismissed (task 16-b #5). */
static void display_set_current_internal(JVM* jvm, JavaObject* displayable,
                                        bool push_history) {
    /* v34.65: single-arg setCurrent(alert) has no remembered follow-up
     * screen — remember the CURRENT one so the timeout/key dismiss can
     * return to it (Alerts shown this way used to strand the UI: the
     * timeout fired, nobody switched screens, the Alert never closed). */
    {
        JavaClass* c = displayable ? displayable->header.clazz : NULL;
        bool new_is_alert = false;
        while (c) {
            if (c->class_name && strcmp(c->class_name, "javax/microedition/lcdui/Alert") == 0) {
                new_is_alert = true;
                break;
            }
            c = c->super_class;
        }
        if (new_is_alert && !g_alert_next_displayable && current_displayable_obj &&
            current_displayable_obj != displayable) {
            g_alert_next_displayable = current_displayable_obj;
            if (!g_alert_next_rooted) {
                gc_add_root(jvm, (void**)&g_alert_next_displayable);
                g_alert_next_rooted = true;
            }
        }
    }
    /* v23 CRITICAL FIX: the #5 refactor of setCurrent lost the lines that
     * made the new Displayable actually CURRENT (current_displayable_obj,
     * its GC root, form.c's g_current_displayable) and the hideNotify call
     * on the old screen. Every Canvas-based app then ran with
     * current_displayable_obj == NULL: isShown() returned false forever,
     * the game loop stopped painting and the repaint pump was dead
     * (M3GTest froze after scene 1, Nescube showed no cube). */
    /* Call hideNotify() on the OLD displayable before switching.
     * MIDP spec: hideNotify() must be called when a Displayable is removed
     * from the screen, BEFORE showNotify on the new one. */
    if (current_displayable_obj && current_displayable_obj != displayable) {
        midp_call_hideNotify(jvm);
    }

    /* v34.72: remember the outgoing screen for emulated BACK-navigation
     * (dead Cancel/Back soft keys). Only midlet-initiated transitions
     * push; the restore paths pass push_history=false. */
    if (push_history && current_displayable_obj && current_displayable_obj != displayable) {
        display_back_stack_push(jvm, current_displayable_obj);
    }

    /* Store current displayable - register as GC root if first time */
    if (!current_displayable_obj) {
        gc_add_root(jvm, (void**)&current_displayable_obj);
    }
    current_displayable_obj = displayable;

    /* Also update form.c's g_current_displayable for key handling */
    midp_set_current_displayable(displayable);

    /* Call showNotify() when displayable becomes current
     * MIDP spec: showNotify() must be called when a Displayable is
     * shown on the screen.
     * v34.33: deferred to the frame pump (see midp_defer_shownotify) —
     * MIDP processes setCurrent asynchronously; the synchronous call
     * raced game loading threads (Rally 3D startup NPE). */
    if (displayable) {
        midp_defer_shownotify(displayable);
    }
    
    /* For Canvas, we need to call paint() automatically */
    if (displayable) {
        JavaClass* clazz = displayable->header.clazz;
        if (clazz && clazz->class_name) {
            fprintf(stderr, "[DISPLAY] Displayable class: %s\n", clazz->class_name);
            
            /* Check if this is a Form - ИСПРАВЛЕНО: проверяем только по иерархии классов */
            bool is_form = false;
            bool is_list = false;
            bool is_alert = false;
            bool is_textbox = false;
            bool is_canvas = false;
            
            JavaClass* check = clazz;
            while (check) {
                if (check->class_name) {
                    /* ИСПРАВЛЕНО: Используем только точное сравнение имен классов */
                    if (strcmp(check->class_name, "javax/microedition/lcdui/Form") == 0) {
                        is_form = true;
                        break;
                    }
                    if (strcmp(check->class_name, "javax/microedition/lcdui/List") == 0) {
                        is_list = true;
                        break;
                    }
                    if (strcmp(check->class_name, "javax/microedition/lcdui/Alert") == 0) {
                        is_alert = true;
                        break;
                    }
                    if (strcmp(check->class_name, "javax/microedition/lcdui/TextBox") == 0) {
                        is_textbox = true;
                        break;
                    }
                    if (strcmp(check->class_name, "javax/microedition/lcdui/Canvas") == 0) {
                        is_canvas = true;
                        break;
                    }
                }
                check = check->super_class;
            }
            
            if (is_form) {
                g_full_screen_mode = false;
                fprintf(stderr, "[DISPLAY] Form detected in setCurrent (is_form=true), calling midp_render_form...\n");
                midp_set_current_displayable(displayable);
                midp_render_form(jvm, displayable);
                /* ИСПРАВЛЕНО: Запрашиваем перерисовку */
                sdl_request_redraw();
                fprintf(stderr, "[DISPLAY] Form rendering complete, redraw requested\n");
            } else if (is_list) {
                DISP_DEBUG("List detected, rendering...");
                midp_set_current_displayable(displayable);
                midp_render_list(jvm, displayable, 0);
                /* ИСПРАВЛЕНО: Запрашиваем перерисовку */
                sdl_request_redraw();
            } else if (is_alert) {
                DISP_DEBUG("Alert detected, rendering...");
                midp_set_current_displayable(displayable);
                JavaObject* gfx_obj = create_graphics_object(jvm);
                if (gfx_obj) {
                    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
                    if (gfx) {
                        midp_render_alert(jvm, gfx, displayable);
                        /* ИСПРАВЛЕНО: Запрашиваем перерисовку */
                        sdl_request_redraw();
                    }
                }
            } else if (is_textbox) {
                DISP_DEBUG("TextBox detected, rendering...");
                midp_set_current_displayable(displayable);
                midp_render_textbox(jvm, displayable, "", 0);
                /* ИСПРАВЛЕНО: Запрашиваем перерисовку */
                sdl_request_redraw();
            } else if (is_canvas) {
                DISP_DEBUG("Canvas detected, calling paint()...");
                
                /* Nokia FullCanvas: auto-enable full screen mode.
                 * FullCanvas is a Nokia extension that removes the title/command bar
                 * and gives the game the full screen. The game expects soft keys to
                 * be passed directly to keyPressed(), not intercepted by Command system. */
                bool is_full_canvas = false;
                {
                    JavaClass* fc_check = clazz;
                    while (fc_check) {
                        if (fc_check->class_name &&
                            strcmp(fc_check->class_name, "com/nokia/mid/ui/FullCanvas") == 0) {
                            is_full_canvas = true;
                            break;
                        }
                        fc_check = fc_check->super_class;
                    }
                }
                if (is_full_canvas) {
                    g_full_screen_mode = true;
                    DISP_DEBUG("Nokia FullCanvas detected, full screen mode enabled");
                } else if (displayable == g_fullscreen_canvas_obj && g_fullscreen_canvas_obj != NULL) {
                    /* v34.7 FIX: this canvas already called setFullScreenMode(true)
                     * (Doom RPG does it in its GameCanvas constructor BEFORE
                     * setCurrent()). MIDP treats fullscreen as a Canvas
                     * property, so setCurrent() must NOT downgrade it.
                     * The old unconditional reset cut the bottom 25px via the
                     * soft-button clip clamp and drew the emulator soft bar
                     * over the game's own bottom UI. */
                    g_full_screen_mode = true;
                    DISP_DEBUG("Canvas kept remembered full screen mode across setCurrent");
                } else {
                    g_full_screen_mode = false;
                }
                
                /* CRITICAL FIX: Clear screen before Canvas paint()!
                 * When switching from List/Form to Canvas, the old content
                 * remains in the framebuffer. We must clear it first. */
                SdlContext* sdl_ctx = sdl_get_global_context();
                if (sdl_ctx) {
                    sdl_clear(sdl_ctx, 0xFFFFFFFF);  /* Clear to white */
                }
                
                /* v34 FIX (Stalker: NPE in b.paint during Main.<init>):
                 * Do NOT call paint() synchronously inside setCurrent().
                 * Real devices deliver paint() from the UI/event loop AFTER
                 * setCurrent() returns. Games routinely call setCurrent() in
                 * the MIDlet constructor BEFORE the canvas' screen state is
                 * initialized (Stalker: Main.<init> runs setCurrent(canvas)
                 * BEFORE Main.a(...) assigns the canvas' current screen field
                 * 'b.a'; paint() dereferences it via invokeinterface -> NPE
                 * propagated INTO the constructor and killed the MIDlet with
                 * "Constructor failed ... NullPointerException").
                 * Request a deferred repaint instead: the main loop's
                 * midp_process_repaints() calls paint() on the next iteration,
                 * when MIDlet state is consistent - exactly like a phone UI
                 * thread. The one-shot initial paint for loop-less MIDlets is
                 * preserved (the request stays pending until the pump runs). */
                DISP_DEBUG("Canvas detected, paint() deferred to repaint pump");
                g_canvas_repaint_requested = true;
                sdl_request_redraw();
            }
        }
    }
}

/* Display.getCurrent() - return current displayable */
static JavaValue native_display_getCurrent(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* display = (JavaObject*)args[0].ref;
    (void)display;  /* Display instance not used */
    
    DISP_DEBUG("getCurrent() -> displayable=%p", (void*)current_displayable_obj);
    
    return NATIVE_RETURN_OBJECT(current_displayable_obj);
}

/*
 * ============================================
 * CallSerially Queue Implementation
 * Stores Runnables for deferred execution on UI thread
 * ============================================
 */

#define CALLSERIALLY_QUEUE_SIZE 64

static struct {
    JavaObject* runnables[CALLSERIALLY_QUEUE_SIZE];
    int head;
    int tail;
    int count;
    bool gc_registered;
} g_call_serially_queue;

/* Queue a Runnable for later execution */
static bool call_serially_enqueue(JVM* jvm, JavaObject* runnable) {
    if (g_call_serially_queue.count >= CALLSERIALLY_QUEUE_SIZE) {
        DISP_DEBUG("[CallSerially] Queue full, cannot enqueue");
        return false;
    }
    
    g_call_serially_queue.runnables[g_call_serially_queue.tail] = runnable;
    g_call_serially_queue.tail = (g_call_serially_queue.tail + 1) % CALLSERIALLY_QUEUE_SIZE;
    g_call_serially_queue.count++;
    
    /* Register all queue slots as GC roots (only once) */
    if (!g_call_serially_queue.gc_registered) {
        for (int i = 0; i < CALLSERIALLY_QUEUE_SIZE; i++) {
            gc_add_root(jvm, (void**)&g_call_serially_queue.runnables[i]);
        }
        g_call_serially_queue.gc_registered = true;
    }
    
    return true;
}

/* FIX (task 16-b #23): callSerially Runnables must run AFTER all pending
 * repaints have completed. The platform main loops drain this queue BEFORE
 * calling midp_process_repaints(), so when a repaint is still pending we
 * defer the drain by one opportunity (bounded - the flag guarantees the
 * queue is drained on the next call even if repaints keep coming, which
 * prevents starvation in games that repaint every frame). */
static int g_callserially_deferred = 0;

/* Dequeue and execute all pending Runnables - called from event loop */
void midp_process_call_serially_queue(JVM* jvm) {
    if (!g_callserially_deferred &&
        (g_paint_pending || g_canvas_repaint_requested)) {
        /* A repaint is still pending: defer serially-called Runnables to
         * the next opportunity, after the repaint pump has run. */
        g_callserially_deferred = 1;
        return;
    }
    g_callserially_deferred = 0;

    JavaThread* thread = jvm_current_thread(jvm);
    
    while (g_call_serially_queue.count > 0) {
        uint32_t count_before = g_call_serially_queue.count;
        JavaObject* runnable = g_call_serially_queue.runnables[g_call_serially_queue.head];
        g_call_serially_queue.runnables[g_call_serially_queue.head] = NULL;  /* Clear slot for GC */
        g_call_serially_queue.head = (g_call_serially_queue.head + 1) % CALLSERIALLY_QUEUE_SIZE;
        g_call_serially_queue.count--;
        
        if (runnable && runnable->header.clazz) {
            JavaClass* runnable_class = runnable->header.clazz;
            JavaMethod* run_method = jvm_resolve_method(jvm, runnable_class, "run", "()V");

            if (run_method) {
                JavaValue run_args[1];
                run_args[0].ref = runnable;
                JavaValue result;
                execute_method(jvm, thread, run_method, run_args, &result);
                /* v35.02: the game's LOGIC heartbeat — Gameloft race loops
                 * re-queue their frame Runnable via callSerially, so this
                 * counter IS the world-advance rate the user sees. Exported
                 * as cs= in the diag line next to bn= (renders): healthy
                 * session has cs tracking the game's frame rate; cs
                 * collapsing while bn stays up = logic starved, renders
                 * alive — the exact freeze form from the field. */
                __sync_fetch_and_add(&g_sd_csq, 1);
            }
        }
        
        /* v34.29 FIX (JBenchmark spin): a Runnable that re-enqueued work while
         * running (the canonical J2ME game loop is exactly
         * run(){ repaint(); display.callSerially(this); }) must NOT be
         * re-drained within this same pump. MIDP runs it once per
         * serialization point — after the NEXT paint cycle. Continuing the
         * drain here turned the queue into a synchronous self-spin:
         * measured on JBenchmark, b.run() looped ~150K times/sec (24M
         * instr/s of pure bytecode) inside ONE call of this function, paint()
         * was starved to ~2 frames in 15 seconds, and the headless/libretro
         * event loop stalled seconds per iteration (retro_run on armv7 spent
         * its whole frame budget in this loop — the "VM stutters and drops
         * frames" symptom). Break when the queue did not shrink; the next
         * pump call (after midp_process_repaints processed the repaint the
         * runnable requested) drains it again. Multiple distinct runnables
         * still run in FIFO order within one pump; only self-requeue loops
         * are serialized to one iteration per frame. */
        if ((uint32_t)g_call_serially_queue.count >= count_before) {
            break;
        }
    }
}

/* Display.callSerially(Runnable) - queue Runnable for later execution */
static JavaValue native_display_callSerially(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* display = (JavaObject*)args[0].ref;
    JavaObject* runnable = (JavaObject*)args[1].ref;
    (void)display;
    
    if (!runnable) {
        DISP_DEBUG("callSerially: Runnable is NULL");
        return NATIVE_RETURN_VOID();
    }
    
    DISP_DEBUG("callSerially: queueing Runnable %p for deferred execution", (void*)runnable);
    fprintf(stderr, "[CALLSERIALLY] queued Runnable class=%s\n",
            runnable && runnable->header.clazz && runnable->header.clazz->class_name ? runnable->header.clazz->class_name : "?");
    
    /* Queue the Runnable instead of executing immediately */
    if (!call_serially_enqueue(jvm, runnable)) {
        DISP_DEBUG("callSerially: failed to queue Runnable");
    }
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_display_getWidth(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    int width, height;
    midp_display_get_dimensions(&width, &height);
    return NATIVE_RETURN_INT(width);
}

/* Canvas.getGameAction(int keyCode) - convert key code to game action */
static JavaValue native_canvas_getGameAction(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    /* args[0] = this (Canvas object), args[1] = keyCode (first method argument) */
    jint keyCode = args[1].i;

    /* Конвертируем keyCode в game action:
     * - keyCode=-1 (FULL_UP) → gameAction=1 (UP)
     * - keyCode=-2 (FULL_DOWN) → gameAction=6 (DOWN)
     * - keyCode=-3 (FULL_LEFT) → gameAction=2 (LEFT)
     * - keyCode=-4 (FULL_RIGHT) → gameAction=5 (RIGHT)
     * - keyCode=-5 (FULL_FIRE) → gameAction=8 (FIRE)
     * - keyCode=-6/-7 (SOFT_LEFT/SOFT_RIGHT) → 0 — НЕТ game action
     * - keyCode=-8 → gameAction=11 (GAME_C)
     * - keyCode=-9 → gameAction=12 (GAME_D)
     *
     * v36.46 [SOFT-GAMEACTION] (yetifix3): софт-клавиши -6/-7 больше НЕ
     * возвращают FIRE. На реальных телефонах (Nokia S40/S60, SE, Moto) у
     * софт-клавиш НЕТ game action — getGameAction(-6) даёт 0 (или
     * IllegalArgumentException), и игры строят софт-выбор на САМОМ keyCode
     * в keyPressed, а не на getGameAction. Старый маппинг -6/-7→FIRE ломал
     * класс игр с софт-меню на девайс-таблицах клавиш: Yeti Sports
     * (In-Fusio 2008) в o.d(I) вызывает getGameAction как FALLBACK для
     * незнакомого кода, получал для -6 «FIRE»→бит 0x10 и УХОДИЛ в
     * held-ветку ДО софт-обработчика (k|=0x20000): диалог звука не
     * отвечал на On/Off, редактор имени — на ABC/Back, а RSK на экранах
     * «Really Exit?» работала как FIRE-выбор (подтверждала выход!).
     * Диагностика: [YS-STATE]-дамп (NOJME_YS_STATE=1) показал после
     * keyPressed(-6): o.k=0 (событие НЕ поставлено), o.i=0x10 (FIRE-бит).
     * Если игре нужен FIRE — она получает его от '5'/'Enter'/-5, как на
     * телефоне; канвасы с Command по-прежнему перехватывают -6/-7 до
     * keyPressed (midp_handle_soft_button, ветка v34.29 в dispatch). */
    
    /* Nokia FullCanvas keyCode to game action mapping */
    switch (keyCode) {
        case -1:  /* FULL_UP */
            return NATIVE_RETURN_INT(1);
        case -2:  /* FULL_DOWN */
            return NATIVE_RETURN_INT(6);
        case -3:  /* FULL_LEFT */
            return NATIVE_RETURN_INT(2);
        case -4:  /* FULL_RIGHT */
            return NATIVE_RETURN_INT(5);
        case -5:  /* FULL_FIRE */
            return NATIVE_RETURN_INT(8);
        case -6:  /* SOFT_LEFT: нет game action — как на реальном телефоне */
        case -7:  /* SOFT_RIGHT: нет game action — как на реальном телефоне */
            return NATIVE_RETURN_INT(0);
        case -8:
            return NATIVE_RETURN_INT(11);
        case -9:
            return NATIVE_RETURN_INT(12);
    }
    
    /* ITU-T key codes (standard MIDP phone keypad):
     * Key 2 (keyCode 50) = UP, Key 4 (52) = LEFT,
     * Key 5 (53) = FIRE, Key 6 (54) = RIGHT, Key 8 (56) = DOWN.
     * Many MIDlets check raw keyCode (n == 53) instead of getGameAction(). */
    switch (keyCode) {
        case 50: /* ITU-T key '2' = UP */
            return NATIVE_RETURN_INT(1);
        case 52: /* ITU-T key '4' = LEFT */
            return NATIVE_RETURN_INT(2);
        case 53: /* ITU-T key '5' = FIRE */
            return NATIVE_RETURN_INT(8);
        case 54: /* ITU-T key '6' = RIGHT */
            return NATIVE_RETURN_INT(5);
        case 56: /* ITU-T key '8' = DOWN */
            return NATIVE_RETURN_INT(6);
    }
    
    /* Для цифр и других клавиш game action не определён */
    /* Но некоторые игры могут передавать gameAction как keyCode */
    if (keyCode >= 1 && keyCode <= 12) {
        return NATIVE_RETURN_INT(keyCode);
    }
    
    /* No game action for this key */
    return NATIVE_RETURN_INT(0);
}

/* Canvas.getKeyCode(int gameAction) - convert game action to key code */
static JavaValue native_canvas_getKeyCode(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    /* args[0] = this (Canvas object), args[1] = gameAction */
    jint gameAction = args[1].i;
    
    /* ИСПРАВЛЕНО: Возвращаем keyCode по стандарту Nokia FullCanvas:
     * UP(1) → keyCode=-1, LEFT(2) → keyCode=-3, RIGHT(5) → keyCode=-4,
     * DOWN(6) → keyCode=-2, FIRE(8) → keyCode=-5
     * GAME_A(9) → keyCode=-6, GAME_B(10) → keyCode=-7,
     * GAME_C(11) → keyCode=-8, GAME_D(12) → keyCode=-9
     */
    switch (gameAction) {
        case 1:  /* UP */
            DISP_DEBUG("getKeyCode(UP) -> -1");
            return NATIVE_RETURN_INT(-1);
        case 2:  /* LEFT */
            DISP_DEBUG("getKeyCode(LEFT) -> -3");
            return NATIVE_RETURN_INT(-3);
        case 5:  /* RIGHT */
            DISP_DEBUG("getKeyCode(RIGHT) -> -4");
            return NATIVE_RETURN_INT(-4);
        case 6:  /* DOWN */
            DISP_DEBUG("getKeyCode(DOWN) -> -2");
            return NATIVE_RETURN_INT(-2);
        case 8:  /* FIRE */
            DISP_DEBUG("getKeyCode(FIRE) -> -5");
            return NATIVE_RETURN_INT(-5);
        case 9:  /* GAME_A */
            DISP_DEBUG("getKeyCode(GAME_A) -> -6");
            return NATIVE_RETURN_INT(-6);
        case 10: /* GAME_B */
            DISP_DEBUG("getKeyCode(GAME_B) -> -7");
            return NATIVE_RETURN_INT(-7);
        case 11: /* GAME_C */
            DISP_DEBUG("getKeyCode(GAME_C) -> -8");
            return NATIVE_RETURN_INT(-8);
        case 12: /* GAME_D */
            DISP_DEBUG("getKeyCode(GAME_D) -> -9");
            return NATIVE_RETURN_INT(-9);
        default:
            /* FIX-19p: MIDP spec - getKeyCode() throws IllegalArgumentException
             * for an invalid game action (VmTest: getKeyCode(-1234)) */
            DISP_DEBUG("getKeyCode(%d) -> IAE (invalid game action)", gameAction);
            native_throw_iae(jvm, thread, "invalid game action");
            return NATIVE_RETURN_INT(0);
    }
}

/* Canvas.getKeyName(int keyCode) - get name for a key */
static JavaValue native_canvas_getKeyName(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    /* args[0] = this (Canvas object), args[1] = keyCode */
    jint keyCode = args[1].i;
    
    const char* keyName = NULL;
    
    /* Handle game keys (Nokia FullCanvas keyCode values) */
    /* -1=UP, -2=DOWN, -3=LEFT, -4=RIGHT, -5=FIRE */
    /* -6=GAME_A, -7=GAME_B, -8=GAME_C, -9=GAME_D */
    switch (keyCode) {
        case -1:  keyName = "UP"; break;
        case -2:  keyName = "DOWN"; break;
        case -3:  keyName = "LEFT"; break;
        case -4:  keyName = "RIGHT"; break;
        case -5:  keyName = "FIRE"; break;
        case -6:  keyName = "SOFTKEY1"; break;
        case -7:  keyName = "SOFTKEY2"; break;
        case -8:  keyName = "GAME_C"; break;
        case -9:  keyName = "GAME_D"; break;
    }
    
    /* Handle standard ITU-T keys */
    if (!keyName) {
        if (keyCode >= '0' && keyCode <= '9') {
            static char num_name[2] = {0, 0};
            num_name[0] = (char)keyCode;
            keyName = num_name;
        }
        else if (keyCode == '*') {
            keyName = "STAR";
        }
        else if (keyCode == '#') {
            keyName = "POUND";
        }
        else {
            keyName = "Unknown";
        }
    }
    
    DISP_DEBUG("getKeyName(%d) -> \"%s\"", keyCode, keyName);
    
    /* Create Java string */
    JavaString* str = jvm_new_string(jvm, (char*)keyName);
    if (!str) {
        ERROR_LOG("getKeyName: failed to create string");
        str = jvm_new_string(jvm, "Unknown");
        if (!str) {
            /* Fallback - should never happen, but return null */
            return NATIVE_RETURN_NULL();
        }
    }
    
    return NATIVE_RETURN_OBJECT(str);
}

/* Canvas.hasPointerEvents() - check if device supports pointer events */
static JavaValue native_canvas_hasPointerEvents(JVM* jvm, JavaThread* thread,
                                                 JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    /* Desktop emulators always support pointer events (mouse) */
    return NATIVE_RETURN_INT(1);  /* true */
}

/* Canvas.hasPointerMotionEvents() - check if device supports pointer drag */
static JavaValue native_canvas_hasPointerMotionEvents(JVM* jvm, JavaThread* thread,
                                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    /* Desktop emulators always support pointer motion events (mouse drag) */
    return NATIVE_RETURN_INT(1);  /* true */
}

/* Canvas.hasRepeatEvents() - check if device supports key repeat */
static JavaValue native_canvas_hasRepeatEvents(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    /* Desktop emulators support key repeat events */
    return NATIVE_RETURN_INT(1);  /* true */
}

/* Canvas.isDoubleBuffered() - check if Canvas is double buffered */
static JavaValue native_canvas_isDoubleBuffered(JVM* jvm, JavaThread* thread,
                                                 JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    /* Our implementation is always double buffered via SDL */
    return NATIVE_RETURN_INT(1);  /* true */
}

static JavaValue native_display_getHeight(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    int width, height;
    midp_display_get_dimensions(&width, &height);
    return NATIVE_RETURN_INT(height);
}

/* Canvas.getHeight() - returns available drawing height (minus soft buttons when not fullscreen) */
static JavaValue native_canvas_getHeight(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    int width, height;
    midp_display_get_dimensions(&width, &height);
    
    /* If not in fullscreen mode and soft buttons are visible, reduce height */
    if (!g_full_screen_mode && current_displayable_obj) {
        /* Check if current displayable has commands (soft buttons would be shown) */
        int commands_idx = find_field_index(current_displayable_obj, "commands");
        if (commands_idx >= 0) {
            JavaArray* commands = (JavaArray*)current_displayable_obj->fields[commands_idx].ref;
            if (commands && is_heap_ptr_check(commands) && ((uintptr_t)commands > 0x10000) && commands->length > 0) {
                /* Soft buttons would be shown, reduce canvas height */
                height -= SOFT_BUTTON_HEIGHT;
            }
        }
    }
    
    return NATIVE_RETURN_INT(height);
}

/* Displayable.isShown() - returns true if this displayable is currently visible */
static JavaValue native_displayable_isShown(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* displayable = (JavaObject*)args[0].ref;
    
    if (!displayable) {
        return NATIVE_RETURN_INT(0);  /* false */
    }
    
    /* Check if this is the current displayable */
    if (displayable == current_displayable_obj) {
        return NATIVE_RETURN_INT(1);  /* true */
    }
    
    return NATIVE_RETURN_INT(0);  /* false */
}

/* Canvas.setFullScreenMode(boolean) - enable/disable fullscreen mode */
static JavaValue native_canvas_setFullScreenMode(JVM* jvm, JavaThread* thread,
                                                  JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* canvas = (JavaObject*)args[0].ref;
    jboolean mode = args[1].i;
    
    /* Store the mode globally */
    bool old_mode = g_full_screen_mode;
    g_full_screen_mode = mode ? true : false;
    /* v34.7 FIX: remember who requested fullscreen so Display.setCurrent()
     * can restore the flag instead of resetting it to false. */
    g_fullscreen_canvas_obj = mode ? canvas : NULL;
    
    /* If mode changed, we need to trigger a sizeChanged event and repaint */
    if (old_mode != g_full_screen_mode && canvas && current_displayable_obj == canvas) {
        /* Call sizeChanged with new dimensions */
        JavaClass* clazz = canvas->header.clazz;
        if (clazz) {
            JavaMethod* method = jvm_resolve_method(jvm, clazz, "sizeChanged", "(II)V");
            if (method) {
                int width, height;
                midp_display_get_dimensions(&width, &height);
                
                /* Adjust height based on fullscreen mode */
                if (!g_full_screen_mode) {
                    height -= SOFT_BUTTON_HEIGHT;
                }
                
                JavaValue size_args[3];
                size_args[0].ref = canvas;
                size_args[1].i = width;
                size_args[2].i = height;
                
                JavaValue result;
                execute_method(jvm, thread, method, size_args, &result);
            }
        }
        
        /* Trigger repaint */
        JavaMethod* repaint_method = jvm_resolve_method(jvm, clazz, "repaint", "()V");
        if (repaint_method) {
            JavaValue repaint_args[1];
            repaint_args[0].ref = canvas;
            JavaValue result;
            execute_method(jvm, thread, repaint_method, repaint_args, &result);
        }
    }
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_display_isColor(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    return NATIVE_RETURN_INT(midp_display_is_color() ? 1 : 0);
}

static JavaValue native_display_numColors(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    return NATIVE_RETURN_INT(midp_display_num_colors());
}

static JavaValue native_display_numAlphaLevels(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    return NATIVE_RETURN_INT(midp_display_num_alpha_levels());
}

static JavaValue native_display_vibrate(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    /* FIX (task 16-b #29): instance method - args[0] is `this` (Display),
     * the duration is args[1]. The old code read args[0].i (garbage). */
    jint duration = args[1].i;
    
    if (platform_callbacks.vibrate) {
        platform_callbacks.vibrate(duration);
    }
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_display_flashBacklight(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    /* FIX (task 16-b #29): instance method - duration is args[1] (see vibrate). */
    jint duration = args[1].i;
    
    if (platform_callbacks.flash_backlight) {
        platform_callbacks.flash_backlight(duration);
    }
    
    return NATIVE_RETURN_VOID();
}

/* Display.getColor(int colorSpecifier) - returns default display colors */
static JavaValue native_display_getColor(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jint specifier = args[0].i;
    switch (specifier) {
        case 0: return NATIVE_RETURN_INT(0);          /* COLOR_BACKGROUND */
        case 1: return NATIVE_RETURN_INT(0xFFFFFF);   /* COLOR_FOREGROUND */
        case 2: return NATIVE_RETURN_INT(0xFFFFFF);   /* COLOR_HIGHLIGHTED_BACKGROUND */
        case 3: return NATIVE_RETURN_INT(0);          /* COLOR_HIGHLIGHTED_FOREGROUND */
        case 4: return NATIVE_RETURN_INT(0x808080);   /* COLOR_BORDER */
        case 5: return NATIVE_RETURN_INT(0xFFFFFF);   /* COLOR_HIGHLIGHTED_BORDER */
        default: return NATIVE_RETURN_INT(0);
    }
}

/* Display.getBestImageWidth(int imageType) */
static JavaValue native_display_getBestImageWidth(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count; (void)args;
    int w, h;
    midp_display_get_dimensions(&w, &h);
    return NATIVE_RETURN_INT(w > 0 ? w : 240);
}

/* Display.getBestImageHeight(int imageType) */
static JavaValue native_display_getBestImageHeight(JVM* jvm, JavaThread* thread,
                                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count; (void)args;
    return NATIVE_RETURN_INT(20);  /* FreeJ2ME: returns 20 */
}

/* Display.getBorderStyle(boolean highlighted) */
static JavaValue native_display_getBorderStyle(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count; (void)args;
    /* FreeJ2ME: always returns SIMPLE = 0 */
    return NATIVE_RETURN_INT(0);
}

/* Shared tail of repaint()/repaint(x,y,w,h): remember the canvas and ask
 * for the next pump to call paint(). */
static void canvas_request_repaint(JavaObject* canvas, bool clear_dirty) {
    if (!canvas) return;
    /* v34.13 FIX (Bounce 128x128 "menu drawn wrong / ignores keys"):
     * repaint() must NEVER change the current Displayable. The old code
     * did `current_displayable_obj = canvas;` unconditionally, so a game
     * timer that keeps calling Canvas.repaint() while an LCDUI List/Form
     * menu is on screen (Bounce calls it every 30ms) silently switched
     * the "current" screen back to the hidden canvas:
     *  - the repaint pump re-painted the canvas over the menu, and
     *  - key events were routed to the canvas' keyPressed() instead of
     *    the List navigation/commandAction - the menu seemed dead.
     * MIDP semantics: repaint() only schedules paint() of THAT canvas and
     * only while it is actually shown. When the canvas is not current the
     * request is simply dropped; as soon as the MIDlet does
     * setCurrent(canvas) again (e.g. after menu select), the running game
     * loop's next repaint() resumes painting. */
    if (canvas != current_displayable_obj) {
        return;
    }
    if (clear_dirty) g_dirty_valid = false;
    /* Mark that repaint was requested - midp_process_repaints() will only
     * call paint() when this flag (or g_paint_pending) is set */
    g_canvas_repaint_requested = true;
    /* Request redraw - this will trigger paint() via midp_process_repaints() */
    sdl_request_redraw();
}

/* v23: midp.h-declared repaint request (was declared but never defined —
 * link error). Unions the region into the dirty rect and schedules a
 * repaint pump; used by geometry changes (sizeChanged path). */
void midp_display_repaint(int x, int y, int width, int height) {
    canvas_dirty_union(x, y, width, height);
    g_canvas_repaint_requested = true;
    sdl_request_redraw();
}

/* Canvas.repaint() - schedule a repaint for later (DO NOT call paint() immediately!) */
static JavaValue native_canvas_repaint(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* canvas = (JavaObject*)args[0].ref;
    
    DISP_DEBUG("[REPAINT] native_canvas_repaint() called! canvas=%p", (void*)canvas);
    
    
    /* In MIDP, repaint() is ASYNCHRONOUS - it schedules paint() for later.
     * The game expects paint2Buffer() to be called BEFORE paint().
     * 
     * WRONG: calling paint() immediately breaks this sequence:
     *   1. createNewLevel() - fills buffer with tiles
     *   2. repaint() - SHOULD NOT call paint() yet!
     *   3. Next run() iteration: paint2Buffer() - copies buffer to mFullScreenBuffer
     *   4. repaint() - now paint() can draw the filled mFullScreenBuffer
     *
     * We store the canvas and request a redraw. The main loop will call paint()
     * via midp_process_repaints() after timers have run.
     */
    
    if (canvas) {
        fprintf(stderr, "[REPAINT] repaint() called, canvas=%p class=%s\n", 
                (void*)canvas, canvas->header.clazz && canvas->header.clazz->class_name ? canvas->header.clazz->class_name : "?");
    }
    
    canvas_request_repaint(canvas, true);
    
    DISP_DEBUG("[REPAINT] native_canvas_repaint() done, sdl_request_redraw() called");
    
    
    return NATIVE_RETURN_VOID();
}

/* Canvas.repaint(int x, int y, int w, int h) - repaint a region.
 * FIX (task 16-b #22): previously forwarded to the full-repaint handler;
 * now the region is unioned into the dirty rect and the next paint() is
 * clipped to it (see midp_process_repaints_impl). */
static JavaValue native_canvas_repaint_region(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* canvas = (JavaObject*)args[0].ref;
    jint x = args[1].i;
    jint y = args[2].i;
    jint w = args[3].i;
    jint h = args[4].i;
    
    DISP_DEBUG("[REPAINT] repaint(%d,%d,%d,%d) canvas=%p", x, y, w, h, (void*)canvas);
    
    canvas_dirty_union(x, y, w, h);
    canvas_request_repaint(canvas, false);
    
    return NATIVE_RETURN_VOID();
}

/* Canvas.serviceRepaints() - process any pending repaints immediately.
 *
 * Many J2ME games (including SU-30) use a two-phase rendering pattern:
 *   Phase 1 (inside paint()): bindTarget(Graphics) + clear(Background)
 *   Phase 2 (after serviceRepaints returns): render(World) + releaseTarget()
 *
 * If serviceRepaints() returns without calling paint(), Phase 2 runs before
 * Phase 1 — render/releaseTarget execute before bindTarget was ever called.
 *
 * In libretro mode, this runs inside execute_frame() within the JVM instruction
 * loop. We call midp_process_repaints() synchronously here so paint() executes
 * BEFORE serviceRepaints() returns to Java code. The main loop's later call to
 * midp_process_repaints() becomes a no-op (flags already cleared).
 *
 * v50 (Asphalt 3 3D image freeze): KVM-authentic vblank pacing. On a real
 * phone serviceRepaints() blocks until the repaint is delivered AND the next
 * vblank — Gameloft game loops (Asphalt/AC race modes) have NO sleep of their
 * own and rely entirely on this to pace 60..15fps. Our synchronous pump
 * returned in ~5ms, so the race loop spun at 170+ repaints/sec: the game
 * thread held the global UI pump lock nearly full-time and the frontend's
 * trylock-based presentation (libretro_end_frame) starved for up to 185
 * consecutive frames = multi-second FROZEN IMAGE while the game ran on.
 * After a paint completes HERE on a VM runner thread, now wait for the next
 * presentation tick (g_midp_vsync_seq, bumped at the end of every frontend
 * frame) — i.e., exactly one serviceRepaints per frontend frame, like
 * waiting for vblank. The frontend/main thread is never paced (it would
 * stall retro_run); skipped pumps (lock busy) are not paced (retry fast);
 * a 250ms bail-out protects against a non-presenting driver; the wait is
 * GC-safepoint aware and aborts when the JVM stops. */
static JavaValue native_canvas_serviceRepaints(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)thread; (void)args; (void)arg_count;

    g_canvas_repaint_requested = true;
    g_paint_pending = true;
    g_paint_complete = false;

    /* Process repaints so paint() runs before serviceRepaints returns.
     * This is critical for games that split M3G rendering across paint() and a
     * post-serviceRepaints callback (bindTarget in paint, render after).
     *
     * v35.02 SINGLE-PUMP: on the SDL-family frontends (external pump armed)
     * a VM RUNNER thread no longer paints here — it WANTS for the frontend
     * thread to complete the paint (bounded, GC-aware). Two game threads
     * racing for the UI lock + non-atomic paint flags was the lost-logic-
     * tick machine behind the "race crawls at ~1 Hz then freezes" report;
     * one pump thread restores the libretro property that made the core
     * immune. The paint-completed handshake (g_midp_paint_done_seq) keeps
     * the two-phase ordering guarantee: paint() has run before we return.
     * Bail-out + legacy fallback pump: if the frontend stops pumping
     * (shutdown, fatal path) the game thread paints itself after 250 ms
     * instead of hanging — never worse than the old behavior. */
    if (g_midp_external_pump_active && jvm && jvm->running) {
        extern int jvm_current_os_thread_is_vm_runner(void);
        if (jvm_current_os_thread_is_vm_runner()) {
            uint32_t done_at_entry = g_midp_paint_done_seq;
            uint32_t deadline_guard = 0;
            /* Exit = a pump completed AFTER our request AND consumed it
             * (g_paint_pending cleared). A bare seq bump is not enough: the
             * frontend pumps every ~16 ms, and a pump that raced between our
             * flag-set and its own check completes without seeing us. */
            while (jvm->running &&
                   (g_midp_paint_done_seq == done_at_entry || g_paint_pending)) {
                if (deadline_guard >= 250) break;  /* frontend not pumping */
                deadline_guard += 2;
                {
                    extern volatile int g_gc_safepoint_request;
                    if (g_gc_safepoint_request) {
                        extern void jvm_gc_safepoint_park(void);
                        jvm_gc_safepoint_park();
                    }
                }
#ifdef _WIN32
                Sleep(2);
#else
                struct timespec sr = { .tv_sec = 0, .tv_nsec = 2000000 };
                nanosleep(&sr, NULL);
#endif
            }
            if (g_paint_pending) {
                /* Frontend did not deliver within the window (or delivered a
                 * pump that consumed nothing): fall back to painting here,
                 * exactly like the pre-v35.02 path. */
                midp_process_repaints(jvm);
            }
        } else {
            /* Frontend thread itself (or a non-runner): legacy sync pump. */
            midp_process_repaints(jvm);
        }
    } else {
        midp_process_repaints(jvm);
    }

    /* v50: pace ONLY when (a) this OS thread is a VM runner (a game thread),
     * not the frontend/main thread, and (b) this call actually completed a
     * paint here (flags cleared) — a skipped pump (UI lock busy elsewhere)
     * must retry immediately, and there is nothing to wait for. */
    if (!g_paint_pending) {
        extern int jvm_current_os_thread_is_vm_runner(void);
        if (jvm_current_os_thread_is_vm_runner() && jvm && jvm->running) {
            /* v34.61 FIX (Asphalt 3 3D "rarely delivers 3D frames"): wait for
             * the first presentation tick that starts AFTER the paint
             * completed. The v50 code snapshotted g_midp_vsync_seq BEFORE
             * the paint (vsync_at_entry): a paint LONGER than one frontend
             * frame (3D race: 15-40ms on ARMv7 vs a 16.6ms retro_run period)
             * outlived one or more ticks, the wait condition
             * (seq != vsync_at_entry) was already true on entry, and the game
             * painted BACK-TO-BACK. The UI pump lock + mid-frame gates then
             * stayed closed nearly full-time and the trylock-based presenter
             * starved — the game ran fast while 3D frames were delivered
             * rarely. Snapshot AFTER the paint so every repaint is followed
             * by exactly one frontend frame boundary, like vblank on a phone. */
            uint32_t vsync_after_paint = g_midp_vsync_seq;
            uint32_t deadline_guard = 0;
            while (jvm->running && g_midp_vsync_seq == vsync_after_paint) {
                if (deadline_guard >= 250) break;  /* driver not presenting: don't hang the game */
                deadline_guard += 2;
                {
                    extern volatile int g_gc_safepoint_request;
                    if (g_gc_safepoint_request) {
                        extern void jvm_gc_safepoint_park(void);
                        jvm_gc_safepoint_park();
                    }
                }
#ifdef _WIN32
                Sleep(2);
#else
                struct timespec vs = { .tv_sec = 0, .tv_nsec = 2000000 };
                nanosleep(&vs, NULL);
#endif
            }
        }
    }

    return NATIVE_RETURN_VOID();
}

/* Проверка есть ли отложенный repaint - вызывается из главного цикла */
bool midp_has_pending_repaint(void) {
    return g_paint_pending;
}

/* Сброс флага отложенного repaint */
void midp_clear_pending_repaint(void) {
    g_paint_pending = false;
    g_paint_complete = true;
    /* FIX (task 16-b #22): the dirty region was consumed by this pump */
    g_dirty_valid = false;
}

/* Process pending repaints - called from main loop after timers */
/* FIX(reentrant-paint): Guard against paint<->repaint recursion storms.
 * Games (e.g. SU-30's 'j.a(Graphics)') call repaint()/serviceRepaints from
 * inside paint(); thread_yield() then pumped the queue again from the nested
 * paint, recursing until the 501-frame cap and, under load, corrupting heap
 * state that later manifested as random glibc aborts. While a pump is active
 * on THIS OS thread, nested pumps and event pumping from thread_yield() are
 * suppressed; dirty regions stay queued for the next outer pump. */
static __thread int tls_pump_depth = 0;
int midp_is_inside_repaint_pump(void) { return tls_pump_depth > 0; }

/* FIX(mt-ui): one process-wide RECURSIVE lock guarding repaint pumps.
 * Without it the main pump and a game thread's synchronous serviceRepaints()
 * executed paint()/M3G code concurrently and interleaved their buffer work,
 * producing rare-but-fatal corruption inside libc locks (tpp assert). */
#if defined(_WIN32)
/* Windows: CRITICAL_SECTION is recursive natively; once-init via CAS */
static CRITICAL_SECTION g_midp_ui_lock;
static volatile LONG g_midp_ui_once = 0;
static int midp_ui_trylock_or_skip_init(void) {
    if (!g_midp_ui_once) {
        if (InterlockedCompareExchange(&g_midp_ui_once, 1, 0) == 0) {
            InitializeCriticalSection(&g_midp_ui_lock);
            InterlockedExchange(&g_midp_ui_once, 2);
        } else {
            while (InterlockedCompareExchange(&g_midp_ui_once, 0, 0) != 2)
                SwitchToThread();
        }
    }
    return TryEnterCriticalSection(&g_midp_ui_lock) ? 0 : -1;
}
static void midp_ui_unlock(void) { LeaveCriticalSection(&g_midp_ui_lock); }
#else
static pthread_mutex_t g_midp_ui_lock;
/* v35.00 KILL-ONCE: pthread_once removed from the MIDP UI lock init, same
 * rationale as mobile3d.c — the devkitA64 pthread_once blocked silently
 * forever in the v34.99 field trace, and the repaint pump (this file)
 * runs on the frozen frontend thread, so a wedge here is invisible.
 * CAS-based one-shot with a bounded, loud wait. 0=uninit 1=init 2=ready. */
static volatile int g_midp_ui_lock_state = 0;
static void midp_ui_lock_init(void) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
#ifdef PTHREAD_PRIO_NONE
    pthread_mutexattr_setprotocol(&a, PTHREAD_PRIO_NONE);
#endif
    pthread_mutex_init(&g_midp_ui_lock, &a);
    pthread_mutexattr_destroy(&a);
}
static int midp_ui_trylock_or_skip_init(void) {
    if (__atomic_load_n(&g_midp_ui_lock_state, __ATOMIC_ACQUIRE) == 2)
        return pthread_mutex_trylock(&g_midp_ui_lock);
    {
        long long t0 = stable_mono_ms();
        if (__sync_bool_compare_and_swap(&g_midp_ui_lock_state, 0, 1)) {
            midp_ui_lock_init();
            __atomic_store_n(&g_midp_ui_lock_state, 2, __ATOMIC_RELEASE);
            return pthread_mutex_trylock(&g_midp_ui_lock);
        }
        while (__atomic_load_n(&g_midp_ui_lock_state, __ATOMIC_ACQUIRE) != 2) {
            long long w = stable_mono_ms() - t0;
            if (w > 3000) {
                LOG_SAFE("[UI-INIT-STALL] midp ui lock not ready after %lld ms (initializer wedged) — taking over\n", w);
                midp_ui_lock_init();
                __atomic_store_n(&g_midp_ui_lock_state, 2, __ATOMIC_RELEASE);
                break;
            }
            {
                struct timespec ts = { .tv_sec = 0, .tv_nsec = 500000 };
                nanosleep(&ts, NULL);
            }
        }
        return pthread_mutex_trylock(&g_midp_ui_lock);
    }
}
static void midp_ui_unlock(void) { pthread_mutex_unlock(&g_midp_ui_lock); }
#endif

static void midp_process_repaints_impl(JVM* jvm);

/* ---- v29: settled-frame presentation support ---------------------------
 * A MIDlet frame is "in flight" while paint() is executing (background
 * drawn, 3D pass not yet blitted) or while a Graphics3D bind/release cycle
 * is open. Presenting the canvas mid-flight shows intermediate states:
 * the classic "background frame -> 3D frame -> background frame" flicker
 * (headless snapshots AND the Windows libretro path, where the game runs
 * on a real OS thread and paints asynchronously to retro_run). */
static volatile int g_midp_paint_depth = 0;

static void midp_paint_depth_inc(void) { g_midp_paint_depth++; }
static void midp_paint_depth_dec(void)  { g_midp_paint_depth--; }

/* mobile3d.c maintains this flag alongside its bind/release target state. */
extern volatile int g_m3g_target_bound_flag;

int midp_canvas_mid_frame(void) {
    return (g_midp_paint_depth > 0) || g_m3g_target_bound_flag;
}

/* UI-lock acquire/release for external presenters (libretro end_frame).
 * Non-blocking: returns 0 on success, -1 if another thread holds the lock
 * (which itself means a paint is in flight -> caller should skip). */
int midp_ui_trylock_external(void) {
    if (midp_ui_trylock_or_skip_init() != 0) return -1;
    return 0;
}
void midp_ui_unlock_external(void) {
    midp_ui_unlock();
}

/* v35.02: returns 1 when this call TOOK THE UI LOCK and ran the pump, 0 when
 * it skipped (UI lock busy). Callers that used to clear pending flags after
 * the pump MUST only do so when this returned 1 — clearing after a skipped
 * pump dropped the dirty region and desynchronized the game thread's
 * serviceRepaints handshake (the lost-logic-tick machine). */
int midp_process_repaints(JVM* jvm) {
    /* Same-thread nesting: defer to the outermost pump */
    if (tls_pump_depth > 0) {
        return 0;
    }
    if (midp_ui_trylock_or_skip_init() != 0) {
        /* v50 DIAG (Asphalt 3 3D stall): another OS thread holds the UI lock
         * — its paint() is in flight and OUR dirty regions stay queued. Under
         * NOJME_PAINTTIME report it: a serviceRepaints() that skips like this
         * while the game loop keeps running is exactly the "image frozen but
         * game advances" signature. */
        static int pt_on = -1;
        if (pt_on < 0) {
            const char* e = getenv("NOJME_PAINTTIME");
            pt_on = (e && e[0] && e[0] != '0') ? 1 : 0;
        }
        if (pt_on) {
            static volatile int pt_skip_n = 0;
            int n = ++pt_skip_n;
            if (n <= 40 || (n % 100) == 0) {
                char line[96];
                int ln = snprintf(line, sizeof(line),
                        "[PAINTTIME] pump#%d skipped: UI lock busy (paint in flight elsewhere)\n", n);
                if (ln > 0) fwrite(line, 1, (size_t)ln, stderr);
            }
        }
        __sync_fetch_and_add(&g_sd_pump_skip, 1);
        return 0;
    }
    tls_pump_depth++;
    midp_process_repaints_impl(jvm);
    tls_pump_depth--;
    midp_ui_unlock();
    /* v35.02: publish pump completion (game threads in serviceRepaints wait
     * for this) + diag counter pp= (pumps completed per period). */
    __sync_fetch_and_add(&g_midp_paint_done_seq, 1);
    __sync_fetch_and_add(&g_sd_pp, 1);
    return 1;
}

static void midp_process_repaints_impl(JVM* jvm) {
    if (!current_displayable_obj) {
        return;
    }

    JavaObject* canvas = current_displayable_obj;
    JavaClass* clazz = canvas->header.clazz;
    if (!clazz) return;
    
    /* ИСПРАВЛЕНО: Определяем тип displayable для выбора метода рендеринга.
     * - Canvas/GameCanvas: вызываем paint()
     * - List/Form/TextBox/Alert: вызываем соответствующие render функции
     */
    bool is_game_canvas = false;
    bool is_canvas = false;
    (void)is_game_canvas; (void)is_canvas;  /* Canvas path is the implicit else-branch */
    bool is_list = false;
    bool is_form = false;
    bool is_textbox = false;
    bool is_alert = false;
    
    JavaClass* check = clazz;
    while (check) {
        if (check->class_name) {
            if (strcmp(check->class_name, "javax/microedition/lcdui/game/GameCanvas") == 0) {
                is_game_canvas = true;
                break;
            }
            if (strcmp(check->class_name, "javax/microedition/lcdui/Canvas") == 0) {
                is_canvas = true;
                /* Не break - может быть GameCanvas */
            }
            if (strcmp(check->class_name, "javax/microedition/lcdui/List") == 0) {
                is_list = true;
                break;
            }
            if (strcmp(check->class_name, "javax/microedition/lcdui/Form") == 0) {
                is_form = true;
                break;
            }
            if (strcmp(check->class_name, "javax/microedition/lcdui/TextBox") == 0) {
                is_textbox = true;
                break;
            }
            if (strcmp(check->class_name, "javax/microedition/lcdui/Alert") == 0) {
                is_alert = true;
                break;
            }
        }
        check = check->super_class;
    }
    

    
    /* ИСПРАВЛЕНО: Обрабатываем высокоуровневые UI компоненты (List, Form, TextBox, Alert) */
    if (is_list) {
#ifdef J2ME_HEADLESS
        extern void headless_reset_text_capture(void);
        extern void headless_print_captured_text(void);
        headless_reset_text_capture();
#endif
        /* v34.27: render with the LIVE selection - the hardcoded 0 reset
         * the visible selection on every repaint (see form.c
         * midp_list_get_selection note). */
        {
            extern int midp_list_get_selection(void);
            midp_render_list(jvm, canvas, midp_list_get_selection());
        }
        render_soft_buttons(jvm, canvas);
#ifdef J2ME_HEADLESS
        headless_print_captured_text();
#endif
        midp_present_settled_snapshot();  /* v34.61: settled frame for scanout */
        midp_clear_pending_repaint();
        return;
    }
    
    if (is_form) {
#ifdef J2ME_HEADLESS
        extern void headless_reset_text_capture(void);
        extern void headless_print_captured_text(void);
        headless_reset_text_capture();
#endif
        midp_render_form(jvm, canvas);
        render_soft_buttons(jvm, canvas);
#ifdef J2ME_HEADLESS
        headless_print_captured_text();
#endif
        midp_present_settled_snapshot();  /* v34.61: settled frame for scanout */
        midp_clear_pending_repaint();
        return;
    }
    
    if (is_textbox) {
#ifdef J2ME_HEADLESS
        extern void headless_reset_text_capture(void);
        extern void headless_print_captured_text(void);
        headless_reset_text_capture();
#endif
        midp_render_textbox(jvm, canvas, "", 0);
        render_soft_buttons(jvm, canvas);
#ifdef J2ME_HEADLESS
        headless_print_captured_text();
#endif
        midp_present_settled_snapshot();  /* v34.61: settled frame for scanout */
        midp_clear_pending_repaint();
        return;
    }
    
    if (is_alert) {
        JavaObject* gfx_obj = create_graphics_object(jvm);
        if (gfx_obj) {
            MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
            if (gfx) {
#ifdef J2ME_HEADLESS
                extern void headless_reset_text_capture(void);
                extern void headless_print_captured_text(void);
                headless_reset_text_capture();
#endif
                midp_render_alert(jvm, gfx, canvas);
#ifdef J2ME_HEADLESS
                headless_print_captured_text();
#endif
            }
        }
        render_soft_buttons(jvm, canvas);
        sdl_request_redraw();
        midp_present_settled_snapshot();  /* v34.61: settled frame for scanout */
        midp_clear_pending_repaint();
        return;
    }
    
    /* GameCanvas extends Canvas, so repaint()/paint() must work.
     * Per J2ME spec, calling repaint() on a GameCanvas still triggers paint().
     * flushGraphics() is an additional mechanism, not a replacement.
     * Previously we skipped paint() for GameCanvas assuming games only use
     * flushGraphics(), but many games use the standard Canvas repaint pattern.
     * Fall through to the regular Canvas paint() handling below. */
    
    /* IMPORTANT: Only call paint() if repaint() or serviceRepaints() was called.
     * midp_process_repaints() is invoked every frame by retro_run(). Calling
     * paint() unconditionally would catch the offscreen buffer mid-update by
     * the MIDlet's game thread, causing visible flickering. */
    if (!g_canvas_repaint_requested && !g_paint_pending) {
        return;
    }
    g_canvas_repaint_requested = false;  /* Consume the request */
    
    DISP_DEBUG("[PAINT] Calling paint() on %s", 
            clazz && clazz->class_name ? clazz->class_name : "?");
    
    /* Find and call paint(Graphics g) method */
    JavaMethod* paint_method = jvm_resolve_method(jvm, clazz, "paint", "(Ljavax/microedition/lcdui/Graphics;)V");
    if (!paint_method) {
        /* No paint() method found */
        midp_clear_pending_repaint();  /* Still signal completion */
        return;
    }
    
    /* Create Graphics object bound to screen */
    JavaObject* gfx_obj = create_graphics_object(jvm);
    if (!gfx_obj) {
        fprintf(stderr, "[REPAINT] Failed to create Graphics object!\n");
        midp_clear_pending_repaint();  /* Still signal completion */
        return;
    }
    
    
    /* Call paint() */
    JavaValue paint_args[2];
    paint_args[0].ref = canvas;
    paint_args[1].ref = gfx_obj;
    
    /* Reset M3G force-render tracking before paint() */
    m3g_reset_paint_tracking();
    
#ifdef J2ME_HEADLESS
    /* Reset text capture before paint to only capture new frame's text */
    extern void headless_reset_text_capture(void);
    headless_reset_text_capture();
#endif
    
    JavaValue result;
    JavaThread* th = jvm_current_thread(jvm);
    /* v50 DIAG (Asphalt 3 3D stall): NOJME_PAINTTIME=1 — wall time of each
     * pump-executed paint() plus the executing Java thread id. Paint runs on
     * whichever OS thread called the pump (game runner's serviceRepaints or
     * the frontend retro_run); long paints block THAT thread and hold the
     * global UI pump lock, starving the other side's repaints. */
    static int pt_paint_on = -1;
    /* v34.73 MinGW -Wmaybe-uninitialized: pt_t0 is only read under
     * pt_paint_on, which the compiler cannot prove across the getenv
     * caching; zero-init both stamps (identical values when off). */
    struct timespec pt_t0 = { .tv_sec = 0, .tv_nsec = 0 };
    struct timespec pt_t1 = { .tv_sec = 0, .tv_nsec = 0 };
    if (pt_paint_on < 0) {
        const char* e = getenv("NOJME_PAINTTIME");
        pt_paint_on = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    if (pt_paint_on) clock_gettime(CLOCK_MONOTONIC, &pt_t0);
    /* v34.61 DIAG (Asphalt A/B, faithful weak-device repro): NOJME_TEST_SLOW_PAINT_US
     * =<us> sleeps INSIDE the pump after paint() — simulates the longer 3D
     * rasterization of a weak device (ARMv7 phone: 15-40ms per race paint vs
     * 4-10ms on x86). Test-only knob, off by default; never set it for
     * production use. */
    {
        static long slow_paint_us = -1;
        if (slow_paint_us < 0) {
            const char* e = getenv("NOJME_TEST_SLOW_PAINT_US");
            slow_paint_us = (e && atol(e) > 0) ? atol(e) : 0;
        }
        if (slow_paint_us > 0) {
            struct timespec sp = { .tv_sec = slow_paint_us / 1000000L,
                                   .tv_nsec = (slow_paint_us % 1000000L) * 1000L };
            midp_paint_depth_inc();  /* stay "mid-frame" while slowed */
            nanosleep(&sp, NULL);
            midp_paint_depth_dec();
        }
    }
    midp_paint_depth_inc();
    /* v36.57 [PAINT-BUDGET]: на потоке ФРОНТЕНДА ограничиваем ожидание
     * мониторов внутри paint() (см. threads.c jvm_paint_lock_arm). Если
     * игровой поток держит монитор (спин депенетрации GD — держит
     * synchronized b.do() секундами), paint() прерывается исключением
     * БЫСТРЕЕ, чем весь фронтенд-цикл застрянет в stage=repaints: ввод,
     * пауза и презентация последнего кадра остаются живыми, следующий
     * насос (~16 мс) пробует снова. VM-раннеры (serviceRepaints из игры)
     * не ограничиваем — их блокировки на игровых мониторах суть игровая
     * логика, прерывать её значило бы ломать корректные игры. */
    int paint_aborted = 0;
    {
        extern int jvm_current_os_thread_is_vm_runner(void);
        static long s_paint_lock_ms = -1;
        int pump_armed = 0;
        if (s_paint_lock_ms < 0) {
            const char* e = getenv("NOJME_PAINT_LOCK_MS");
            s_paint_lock_ms = (e && atol(e) >= 0) ? atol(e) : 400;
        }
        if (s_paint_lock_ms > 0 && !jvm_current_os_thread_is_vm_runner()) {
            extern void jvm_paint_lock_arm(uint32_t budget_ms);
            jvm_paint_lock_arm((uint32_t)s_paint_lock_ms);
            pump_armed = 1;
        }
        int exec_result = execute_method(jvm, th, paint_method, paint_args, &result);
        if (pump_armed) {
            extern void jvm_paint_lock_disarm(void);
            extern int jvm_paint_lock_aborted(void);
            extern void jvm_paint_lock_reset(void);
            jvm_paint_lock_disarm();
            if (jvm_paint_lock_aborted()) {
                paint_aborted = 1;
                /* device-log breadcrumb (v35.12 pattern: weak sw_trace_force) */
                {
                    extern void sw_trace_force(const char* fmt, ...)
                        __attribute__((weak));
                    if (sw_trace_force) {
                        sw_trace_force("[PAINT-BUDGET] paint() aborted: monitor held by a "
                                       "game thread longer than %ld ms — frame skipped, "
                                       "frontend alive, retry next pump",
                                       s_paint_lock_ms);
                    }
                }
                LOG_SAFE("[PAINT-BUDGET] paint() aborted after %ld ms lock wait — "
                         "frame skipped, frontend alive\n", s_paint_lock_ms);
                jvm_paint_lock_reset();
            }
        }
        (void)exec_result;  /* failures surface via pending_exception below */
    }
    midp_paint_depth_dec();
    if (pt_paint_on) {
        clock_gettime(CLOCK_MONOTONIC, &pt_t1);
        double pt_ms = (pt_t1.tv_sec - pt_t0.tv_sec) * 1000.0 +
                       (pt_t1.tv_nsec - pt_t0.tv_nsec) / 1e6;
        char line[128];
        int ln = snprintf(line, sizeof(line), "[PAINTTIME] tid=%d paint=%.1fms\n",
                          th ? th->id : -1, pt_ms);
        if (ln > 0) fwrite(line, 1, (size_t)ln, stderr);
    }
    
    /* v34: the pump runs OUTSIDE any Java frame (main loop / retro_run).
     * If paint() threw - a game canvas painting not-yet-initialized state,
     * e.g. Stalker's b.paint dereferencing its null screen field - the
     * pending exception used to LINGER on the thread and detonate inside
     * unrelated Java code on the next scheduled frame. Isolate it: log and
     * clear, keeping the MIDlet alive (phone-UI-thread semantics: the
     * exception never escapes into the application thread). */
    if (th && th->pending_exception) {
        JavaObject* paint_exc = th->pending_exception;
        const char* exc_cls = (paint_exc && paint_exc->header.clazz &&
                               paint_exc->header.clazz->class_name)
                              ? paint_exc->header.clazz->class_name : "?";
        LOG_SAFE("[PAINT] paint() threw %s on the repaint pump - cleared (UI-thread isolation)\n",
                 exc_cls);
        th->pending_exception = NULL;
        if (th->exception_stack_trace) {
            free(th->exception_stack_trace);
            th->exception_stack_trace = NULL;
        }
        if (th->exception_throw_info) {
            free(th->exception_throw_info);
            th->exception_throw_info = NULL;
        }
    }
    
#ifdef J2ME_HEADLESS
    /* Print captured text after paint completes */
    extern void headless_print_captured_text(void);
    headless_print_captured_text();
#endif
    
    
    /* Render soft buttons after paint() */
    /* v36.57 [PAINT-BUDGET]: прерванный paint оставил канву в промежуточном
     * состоянии — НЕ дорисовываем софт-кнопки, НЕ делаем settled-snapshot
     * (полукадр на экране = мерцание) и НЕ сбрасываем pending repaint
     * (следующий насос повторит paint целиком; фронтенд жив и качает
     * ввод/паузу/презентацию последнего ХОРОШЕГО кадра). */
    if (paint_aborted) {
        return;
    }
    render_soft_buttons(jvm, canvas);
    
    /* M3G force-render: Some games (like SU-30) do M3G scene setup 
     * (setCamera, setBackground, etc.) but never call bindTarget/render.
     * After paint() completes, if scene was set up but not rendered,
     * force a render cycle. */
    {
        extern bool m3g_needs_force_render(void);
        extern void m3g_force_render(JVM* jvm, MidpGraphics* screen_gfx);
        if (m3g_needs_force_render()) {
            MidpGraphics* screen_gfx = get_graphics_from_object(gfx_obj);
            if (screen_gfx) {
                m3g_force_render(jvm, screen_gfx);
            }
        }
    }
    
    /* v34.61: the frame is complete now (paint + soft buttons + possible M3G
     * force-render). Snapshot it as the settled frame for the scanout
     * presenter. This runs under the UI pump lock, so the snapshot cannot
     * catch a concurrent pump-paint; the ~300KB memcpy costs 30-100us. */
    midp_present_settled_snapshot();
    
    /* ИСПРАВЛЕНО: Сигнализируем, что paint() завершён.
     * Это разблокирует serviceRepaints() в Java-потоке.
     * БЕЗ ЭТОГО serviceRepaints() ждёт 100ms таймаут, а потом
     * Java-код продолжает и сбрасывает t.b = false ДО того,
     * как paint() будет вызван!
     */
    midp_clear_pending_repaint();
    
    /* repaint done */
    
}

/*
 * ============================================
 * Helper functions for safe field access
 * ИСПРАВЛЕНО: используем native_get/set_field_value для корректного расчета слотов
 * ============================================
 */

/* Helper: Get field value from object by name */
static jint get_object_field_int(JavaObject* obj, const char* field_name) {
    return native_get_field_value(obj, field_name).i;
}

/* Helper: Set field value in object by name */
static void set_object_field_int(JavaObject* obj, const char* field_name, jint value) {
    JavaValue val = { .i = value };
    native_set_field_value(obj, field_name, val);
}

/* Helper: Get object field reference by name */
JavaObject* get_object_field_ref(JavaObject* obj, const char* field_name) {
    return (JavaObject*)native_get_field_value(obj, field_name).ref;
}

/* Helper: Set object field reference by name */
void set_object_field_ref(JavaObject* obj, const char* field_name, JavaObject* value) {
    JavaValue val = { .ref = value };
    native_set_field_value(obj, field_name, val);
}

/*
 * ============================================
 * Sprite native methods
 * ============================================
 */

/* Sprite.<init>(Image image) */
static JavaValue native_sprite_init_image(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    JavaObject* image_obj = (JavaObject*)args[1].ref;

    if (!this_obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }

    /* COMPATIBILITY: Accept null Image (KEmulator behavior).
     * Some midlets pass null when resource loading fails — throwing NPE
     * here prevents the entire game from reaching 3D rendering. */
    if (!image_obj) {
        set_object_field_ref(this_obj, "image", NULL);
        set_object_field_int(this_obj, "width", 0);
        set_object_field_int(this_obj, "height", 0);
        set_object_field_int(this_obj, "frameWidth", 0);
        set_object_field_int(this_obj, "frameHeight", 0);
        set_object_field_int(this_obj, "rawFrameCount", 0);
        set_object_field_int(this_obj, "currentFrame", 0);
        set_object_field_int(this_obj, "collisionX", 0);
        set_object_field_int(this_obj, "collisionY", 0);
        set_object_field_int(this_obj, "collisionWidth", 0);
        set_object_field_int(this_obj, "collisionHeight", 0);
        set_object_field_int(this_obj, "transform", 0);
        return NATIVE_RETURN_VOID();
    }

    DISP_DEBUG("[Sprite] init(Image): this=%p, image=%p",
            (void*)this_obj, (void*)image_obj);

    /* ИСПРАВЛЕНО: используем helper function */
    set_object_field_ref(this_obj, "image", image_obj);

    /* Init width/height and frame dimensions from image */
    MidpImage* img = get_image_from_object(image_obj);
    if (img) {
        set_object_field_int(this_obj, "width", img->width);
        set_object_field_int(this_obj, "height", img->height);
        /* Single frame = the whole image */
        set_object_field_int(this_obj, "frameWidth", img->width);
        set_object_field_int(this_obj, "frameHeight", img->height);
        set_object_field_int(this_obj, "rawFrameCount", 1);
        set_object_field_int(this_obj, "currentFrame", 0);
        /* Initialize collision rectangle to full sprite bounds */
        set_object_field_int(this_obj, "collisionX", 0);
        set_object_field_int(this_obj, "collisionY", 0);
        set_object_field_int(this_obj, "collisionWidth", img->width);
        set_object_field_int(this_obj, "collisionHeight", img->height);
    }

    /* v34.33 FIX (MIDP spec §Layer): a new Layer is VISIBLE by default.
     * Neither Sprite constructor ever set "visible", so it stayed 0 (Java
     * field zero-init) and Sprite.paint() silently skipped every blit.
     * Block 3D 2 (Cocoasoft) paints its whole 400x240 scene through ONE
     * Sprite (rotated TRANS_ROT90 onto the portrait canvas) — the game
     * loop ran but the screen stayed white. */
    set_object_field_int(this_obj, "visible", 1);
    set_object_field_int(this_obj, "refX", 0);
    set_object_field_int(this_obj, "refY", 0);

    return NATIVE_RETURN_VOID();
}

/* Sprite.<init>(Image image, int frameWidth, int frameHeight) */
static JavaValue native_sprite_init_frames(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    JavaObject* image_obj = (JavaObject*)args[1].ref;
    jint frame_width = args[2].i;
    jint frame_height = args[3].i;

    if (!this_obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }

    /* COMPATIBILITY: Accept null Image */
    if (!image_obj) {
        set_object_field_ref(this_obj, "image", NULL);
        set_object_field_int(this_obj, "frameWidth", frame_width);
        set_object_field_int(this_obj, "frameHeight", frame_height);
        set_object_field_int(this_obj, "width", frame_width);
        set_object_field_int(this_obj, "height", frame_height);
        set_object_field_int(this_obj, "rawFrameCount", 0);
        set_object_field_int(this_obj, "currentFrame", 0);
        set_object_field_int(this_obj, "collisionX", 0);
        set_object_field_int(this_obj, "collisionY", 0);
        set_object_field_int(this_obj, "collisionWidth", frame_width);
        set_object_field_int(this_obj, "collisionHeight", frame_height);
        set_object_field_int(this_obj, "transform", 0);
        set_object_field_int(this_obj, "visible", 1);  /* v34.33: Layer visible by default */
        return NATIVE_RETURN_VOID();
    }

    DISP_DEBUG("[Sprite] init(Image, %d, %d): this=%p, image=%p",
            frame_width, frame_height, (void*)this_obj, (void*)image_obj);

    /* ИСПРАВЛЕНО: используем helper functions */
    set_object_field_ref(this_obj, "image", image_obj);
    set_object_field_int(this_obj, "frameWidth", frame_width);
    set_object_field_int(this_obj, "frameHeight", frame_height);

    /* MIDP2 spec: Sprite(Image, frameWidth, frameHeight) - 
     * width = frameWidth, height = frameHeight */
    set_object_field_int(this_obj, "width", frame_width);
    set_object_field_int(this_obj, "height", frame_height);

    /* Calculate raw frame count from full image dimensions */
    MidpImage* img = get_image_from_object(image_obj);
    if (img) {
        if (frame_width > 0 && frame_height > 0 &&
            img->width >= frame_width && img->height >= frame_height) {
            int cols = img->width / frame_width;
            int rows = img->height / frame_height;
            set_object_field_int(this_obj, "rawFrameCount", cols * rows);
        }
    }

    set_object_field_int(this_obj, "currentFrame", 0);
    /* Initialize collision rectangle to full frame bounds */
    set_object_field_int(this_obj, "collisionX", 0);
    set_object_field_int(this_obj, "collisionY", 0);
    set_object_field_int(this_obj, "collisionWidth", frame_width);
    set_object_field_int(this_obj, "collisionHeight", frame_height);

    /* v34.33 FIX: Layer visible by default (see native_sprite_init_image) */
    set_object_field_int(this_obj, "visible", 1);
    set_object_field_int(this_obj, "refX", 0);
    set_object_field_int(this_obj, "refY", 0);

    return NATIVE_RETURN_VOID();
}

/* Sprite.<init>(Sprite sprite) - copy constructor */
static JavaValue native_sprite_init_copy(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    JavaObject* other_sprite = (JavaObject*)args[1].ref;
    
    if (!this_obj || !other_sprite) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    
    DISP_DEBUG("[Sprite] init(copy): this=%p, other=%p", 
            (void*)this_obj, (void*)other_sprite);
    
    /* ИСПРАВЛЕНО: Copy fields using helper functions */
    set_object_field_ref(this_obj, "image", get_object_field_ref(other_sprite, "image"));
    set_object_field_int(this_obj, "x", get_object_field_int(other_sprite, "x"));
    set_object_field_int(this_obj, "y", get_object_field_int(other_sprite, "y"));
    set_object_field_int(this_obj, "width", get_object_field_int(other_sprite, "width"));
    set_object_field_int(this_obj, "height", get_object_field_int(other_sprite, "height"));
    set_object_field_int(this_obj, "frameWidth", get_object_field_int(other_sprite, "frameWidth"));
    set_object_field_int(this_obj, "frameHeight", get_object_field_int(other_sprite, "frameHeight"));
    set_object_field_int(this_obj, "currentFrame", get_object_field_int(other_sprite, "currentFrame"));
    set_object_field_int(this_obj, "visible", get_object_field_int(other_sprite, "visible"));
    set_object_field_int(this_obj, "transform", get_object_field_int(other_sprite, "transform"));
    set_object_field_int(this_obj, "rawFrameCount", get_object_field_int(other_sprite, "rawFrameCount"));
    set_object_field_ref(this_obj, "frameSequence", get_object_field_ref(other_sprite, "frameSequence"));
    set_object_field_int(this_obj, "refX", get_object_field_int(other_sprite, "refX"));
    set_object_field_int(this_obj, "refY", get_object_field_int(other_sprite, "refY"));
    set_object_field_int(this_obj, "collisionX", get_object_field_int(other_sprite, "collisionX"));
    set_object_field_int(this_obj, "collisionY", get_object_field_int(other_sprite, "collisionY"));
    set_object_field_int(this_obj, "collisionWidth", get_object_field_int(other_sprite, "collisionWidth"));
    set_object_field_int(this_obj, "collisionHeight", get_object_field_int(other_sprite, "collisionHeight"));
    
    return NATIVE_RETURN_VOID();
}

/* Sprite.setPosition(int x, int y) */
static JavaValue native_sprite_setPosition(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    jint x = args[1].i;
    jint y = args[2].i;
    
    if (!this_obj) return NATIVE_RETURN_VOID();
    
    /* ИСПРАВЛЕНО: используем helper functions */
    set_object_field_int(this_obj, "x", x);
    set_object_field_int(this_obj, "y", y);
    
    return NATIVE_RETURN_VOID();
}

/* Sprite.move(int dx, int dy) */
static JavaValue native_sprite_move(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    jint dx = args[1].i;
    jint dy = args[2].i;
    
    if (!this_obj) return NATIVE_RETURN_VOID();
    
    /* ИСПРАВЛЕНО: используем helper functions */
    jint cur_x = get_object_field_int(this_obj, "x");
    jint cur_y = get_object_field_int(this_obj, "y");
    set_object_field_int(this_obj, "x", cur_x + dx);
    set_object_field_int(this_obj, "y", cur_y + dy);
    
    return NATIVE_RETURN_VOID();
}

/* Sprite.paint(Graphics g) */
static JavaValue native_sprite_paint(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    JavaObject* gfx_obj = (JavaObject*)args[1].ref;
    
    if (!this_obj || !gfx_obj) return NATIVE_RETURN_VOID();
    
    /* Get MidpGraphics from Java Graphics object */
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    if (!gfx) {
        DISP_DEBUG("[Sprite] paint: failed to get MidpGraphics");
        return NATIVE_RETURN_VOID();
    }
    
    /* Get sprite properties */
    jint x = get_object_field_int(this_obj, "x");
    jint y = get_object_field_int(this_obj, "y");
    jint width = get_object_field_int(this_obj, "width");
    jint height = get_object_field_int(this_obj, "height");
    jint frameWidth = get_object_field_int(this_obj, "frameWidth");
    jint frameHeight = get_object_field_int(this_obj, "frameHeight");
    jint currentFrame = get_object_field_int(this_obj, "currentFrame");
    jint visible = get_object_field_int(this_obj, "visible");
    jint transform = get_object_field_int(this_obj, "transform");
    jint refX = get_object_field_int(this_obj, "refX");
    jint refY = get_object_field_int(this_obj, "refY");
    
    /* Skip if not visible */
    if (!visible) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Get image from sprite */
    JavaObject* image_obj = get_object_field_ref(this_obj, "image");
    if (!image_obj) {
        DISP_DEBUG("[Sprite] paint: no image");
        return NATIVE_RETURN_VOID();
    }
    
    /* Get MidpImage from Image object */
    MidpImage* img = (MidpImage*)get_object_field_ref(image_obj, "nativePeer");
    
    if (!img || !img->pixels) {
        DISP_DEBUG("[Sprite] paint: no image data");
        return NATIVE_RETURN_VOID();
    }
    
    /* Calculate frame offset if using frame animation */
    jint src_x = 0, src_y = 0;
    jint src_w = frameWidth > 0 ? frameWidth : img->width;
    jint src_h = frameHeight > 0 ? frameHeight : img->height;

    /* Resolve actual frame index via frameSequence */
    jint actual_frame = currentFrame;
    JavaArray* frame_seq = (JavaArray*)get_object_field_ref(this_obj, "frameSequence");
    if (frame_seq && frame_seq->length > 0) {
        jint* seq_data = (jint*)array_data(frame_seq);
        if (currentFrame >= 0 && currentFrame < (jint)frame_seq->length) {
            actual_frame = seq_data[currentFrame];
        }
    }

    if (frameWidth > 0 && frameHeight > 0) {
        int frames_per_row = img->width / frameWidth;
        if (frames_per_row > 0) {
            src_x = (actual_frame % frames_per_row) * frameWidth;
            src_y = (actual_frame / frames_per_row) * frameHeight;
        }
    }

    /* v34.33 FIX (JSR-118 Sprite.paint): paint() draws the sprite at its
     * CURRENT Layer position (getX/getY == top-left of the transformed
     * image), with TOP|LEFT semantics — the reference-pixel math already
     * happened in setRefPixelPosition()/setTransform() (they adjust x,y).
     * The old code subtracted the transformed ref-pixel offset AGAIN here,
     * double-shifting every sprite whose position was set through the
     * reference pixel: Block 3D 2 (whole-scene TRANS_ROT90 sprite) landed
     * at dest=(-559,0) and nothing reached the screen. */
    jint dest_x = x;
    jint dest_y = y;
    (void)refX; (void)refY; (void)width; (void)height;

    /* v22 DIAG: env-gated Sprite.paint trace (atlas blits). NOJME_SPRITE_TRACE
     * logs the first N paints per (imgW x imgH, transform) bucket so atlas
     * games (Sprite-of-whole-atlas + clip) can be diagnosed headlessly. */
    {
        static int spr_trace = -1;
        if (spr_trace < 0) spr_trace = getenv("NOJME_SPRITE_TRACE") ? 1 : 0;
        if (spr_trace) {
            static int bucket_count[8][64]; /* [transform][img size bucket] */
            int wbucket = (img->width << 10) | img->height;
            int bidx = wbucket % 64;
            if (transform >= 0 && transform < 8 &&
                bucket_count[transform][bidx]++ < 24) {
                fprintf(stderr,
                        "[SPR-PAINT] img=%dx%d xf=%d pos=(%d,%d) layer=%dx%d "
                        "frame=%dx%d cf=%d ref=(%d,%d) clip=(%d,%d,%d,%d)\n",
                        img->width, img->height, transform, x, y, width, height,
                        frameWidth, frameHeight, currentFrame,
                        refX, refY,
                        gfx->clip_x, gfx->clip_y, gfx->clip_width, gfx->clip_height);
            }
        }
    }

    /* Draw the sprite region to the graphics context */
    midp_graphics_draw_region(gfx, img, src_x, src_y, src_w, src_h,
                               transform, dest_x, dest_y, 0);
    
    return NATIVE_RETURN_VOID();
}

/* Sprite.getX() */
static JavaValue native_sprite_getX(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    
    if (!this_obj) return NATIVE_RETURN_INT(0);
    
    /* ИСПРАВЛЕНО: используем helper function */
    return NATIVE_RETURN_INT(get_object_field_int(this_obj, "x"));
}

/* Sprite.getY() */
static JavaValue native_sprite_getY(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    
    if (!this_obj) return NATIVE_RETURN_INT(0);
    
    /* ИСПРАВЛЕНО: используем helper function */
    return NATIVE_RETURN_INT(get_object_field_int(this_obj, "y"));
}

/* Sprite.getWidth() */
static JavaValue native_sprite_getWidth(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    
    if (!this_obj) return NATIVE_RETURN_INT(0);
    
    /* ИСПРАВЛЕНО: используем helper function */
    return NATIVE_RETURN_INT(get_object_field_int(this_obj, "width"));
}

/* Sprite.getHeight() */
static JavaValue native_sprite_getHeight(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    
    if (!this_obj) return NATIVE_RETURN_INT(0);
    
    /* ИСПРАВЛЕНО: используем helper function */
    return NATIVE_RETURN_INT(get_object_field_int(this_obj, "height"));
}

/* Sprite.setVisible(boolean visible) */
static JavaValue native_sprite_setVisible(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    jboolean visible = args[1].i;
    
    if (!this_obj) return NATIVE_RETURN_VOID();
    
    /* ИСПРАВЛЕНО: используем helper function */
    set_object_field_int(this_obj, "visible", visible);
    
    return NATIVE_RETURN_VOID();
}

/* Sprite.isVisible() */
static JavaValue native_sprite_isVisible(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    
    if (!this_obj) return NATIVE_RETURN_INT(1);  /* Default visible */
    
    /* ИСПРАВЛЕНО: используем helper function */
    return NATIVE_RETURN_INT(get_object_field_int(this_obj, "visible"));
}

/* Sprite.setFrame(int frame) */
static JavaValue native_sprite_setFrame(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    jint frame = args[1].i;
    
    if (!this_obj) return NATIVE_RETURN_VOID();
    
    /* ИСПРАВЛЕНО: используем helper function */
    set_object_field_int(this_obj, "currentFrame", frame);
    
    return NATIVE_RETURN_VOID();
}

/* Sprite.getFrame() */
static JavaValue native_sprite_getFrame(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    
    if (!this_obj) return NATIVE_RETURN_INT(0);
    
    return NATIVE_RETURN_INT(get_object_field_int(this_obj, "currentFrame"));
}

/* Sprite.setTransform(int transform) - rotate/mirror sprite */
static JavaValue native_sprite_setTransform(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    jint transform = args[1].i;
    
    if (!this_obj) return NATIVE_RETURN_VOID();
    
    jint old_transform = get_object_field_int(this_obj, "transform");
    if (old_transform == transform) return NATIVE_RETURN_VOID();
    
    jint refX = get_object_field_int(this_obj, "refX");
    jint refY = get_object_field_int(this_obj, "refY");
    jint cur_w = get_object_field_int(this_obj, "width");
    jint cur_h = get_object_field_int(this_obj, "height");
    
    /* Compute reference pixel offset under OLD transform using OLD dimensions */
    jint old_refPtX, old_refPtY;
    switch (old_transform) {
        case 0: old_refPtX = refX; old_refPtY = refY; break;
        case 1: old_refPtX = refX; old_refPtY = cur_h - 1 - refY; break;
        case 2: old_refPtX = cur_w - 1 - refX; old_refPtY = refY; break;
        case 3: old_refPtX = cur_w - 1 - refX; old_refPtY = cur_h - 1 - refY; break;
        case 4: old_refPtX = refY; old_refPtY = refX; break;
        case 5: old_refPtX = cur_h - 1 - refY; old_refPtY = refX; break;
        case 6: old_refPtX = refY; old_refPtY = cur_w - 1 - refX; break;
        case 7: old_refPtX = cur_h - 1 - refY; old_refPtY = cur_w - 1 - refX; break;
        default: old_refPtX = refX; old_refPtY = refY; break;
    }
    
    /* Check if old transform had swapped dimensions and restore original */
    bool old_swapped = (old_transform == SPRITE_TRANS_ROT90 ||
                        old_transform == SPRITE_TRANS_ROT270 ||
                        old_transform == SPRITE_TRANS_MIRROR_ROT90 ||
                        old_transform == SPRITE_TRANS_MIRROR_ROT270);
    if (old_swapped) {
        jint tmp = cur_w; cur_w = cur_h; cur_h = tmp;
    }
    
    /* Check if new transform swaps dimensions */
    bool new_swapped = (transform == SPRITE_TRANS_ROT90 ||
                        transform == SPRITE_TRANS_ROT270 ||
                        transform == SPRITE_TRANS_MIRROR_ROT90 ||
                        transform == SPRITE_TRANS_MIRROR_ROT270);
    
    /* Compute new dimensions */
    jint new_w = new_swapped ? cur_h : cur_w;
    jint new_h = new_swapped ? cur_w : cur_h;
    
    /* Update transform and dimensions */
    set_object_field_int(this_obj, "transform", transform);
    set_object_field_int(this_obj, "width", new_w);
    set_object_field_int(this_obj, "height", new_h);
    
    /* Compute reference pixel offset under NEW transform using NEW dimensions */
    jint new_refPtX, new_refPtY;
    switch (transform) {
        case 0: new_refPtX = refX; new_refPtY = refY; break;
        case 1: new_refPtX = refX; new_refPtY = new_h - 1 - refY; break;
        case 2: new_refPtX = new_w - 1 - refX; new_refPtY = refY; break;
        case 3: new_refPtX = new_w - 1 - refX; new_refPtY = new_h - 1 - refY; break;
        case 4: new_refPtX = refY; new_refPtY = refX; break;
        case 5: new_refPtX = new_h - 1 - refY; new_refPtY = refX; break;
        case 6: new_refPtX = refY; new_refPtY = new_w - 1 - refX; break;
        case 7: new_refPtX = new_h - 1 - refY; new_refPtY = new_w - 1 - refX; break;
        default: new_refPtX = refX; new_refPtY = refY; break;
    }
    
    /* Adjust position to keep reference pixel at same screen location.
     * screen_ref = x + old_refPt (top-left + offset in drawn image)
     * new_x = screen_ref - new_refPt = x + old_refPt - new_refPt
     */
    jint cur_x = get_object_field_int(this_obj, "x");
    jint cur_y = get_object_field_int(this_obj, "y");
    set_object_field_int(this_obj, "x", cur_x + old_refPtX - new_refPtX);
    set_object_field_int(this_obj, "y", cur_y + old_refPtY - new_refPtY);
    
    return NATIVE_RETURN_VOID();
}

/* Sprite.getTransform() */
static JavaValue native_sprite_getTransform(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    
    if (!this_obj) return NATIVE_RETURN_INT(0);
    
    return NATIVE_RETURN_INT(get_object_field_int(this_obj, "transform"));
}

/* Helper to get Sprite image */
static MidpImage* get_sprite_image(JavaObject* sprite_obj) {
    if (!sprite_obj) return NULL;
    
    JavaObject* image_obj = get_object_field_ref(sprite_obj, "image");
    if (image_obj) {
        return get_image_from_object(image_obj);
    }
    return NULL;
}

/* Sprite.collidesWith(Sprite s, boolean pixelLevel) */
static JavaValue native_sprite_collidesWith_sprite(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    JavaObject* other_sprite = (JavaObject*)args[1].ref;
    jboolean pixel_level = args[2].i;
    
    if (!this_obj || !other_sprite) {
        return NATIVE_RETURN_INT(0); /* false */
    }
    
    /* Check visibility - invisible sprites don't collide */
    if (!get_object_field_int(this_obj, "visible") || !get_object_field_int(other_sprite, "visible")) {
        return NATIVE_RETURN_INT(0);
    }
    
    /* Use collision rectangles instead of full bounds */
    int this_x = get_object_field_int(this_obj, "x") + get_object_field_int(this_obj, "collisionX");
    int this_y = get_object_field_int(this_obj, "y") + get_object_field_int(this_obj, "collisionY");
    int this_w = get_object_field_int(this_obj, "collisionWidth");
    int this_h = get_object_field_int(this_obj, "collisionHeight");
    
    int other_x = get_object_field_int(other_sprite, "x") + get_object_field_int(other_sprite, "collisionX");
    int other_y = get_object_field_int(other_sprite, "y") + get_object_field_int(other_sprite, "collisionY");
    int other_w = get_object_field_int(other_sprite, "collisionWidth");
    int other_h = get_object_field_int(other_sprite, "collisionHeight");
    
    /* Rectangle intersection test */
    int intersect_x1 = this_x > other_x ? this_x : other_x;
    int intersect_y1 = this_y > other_y ? this_y : other_y;
    int intersect_x2 = (this_x + this_w) < (other_x + other_w) ? (this_x + this_w) : (other_x + other_w);
    int intersect_y2 = (this_y + this_h) < (other_y + other_h) ? (this_y + this_h) : (other_y + other_h);
    
    /* No intersection */
    if (intersect_x1 >= intersect_x2 || intersect_y1 >= intersect_y2) {
        return NATIVE_RETURN_INT(0); /* false */
    }
    
    /* If not pixel-level, bounding box collision is enough */
    if (!pixel_level) {
        return NATIVE_RETURN_INT(1); /* true */
    }
    
    /* Pixel-level collision detection */
    MidpImage* this_img = get_sprite_image(this_obj);
    MidpImage* other_img = get_sprite_image(other_sprite);
    
    if (!this_img || !other_img) {
        return NATIVE_RETURN_INT(0); /* false - no images */
    }
    
    /* Get frame info for this sprite */
    jint this_frameWidth = get_object_field_int(this_obj, "frameWidth");
    jint this_frameHeight = get_object_field_int(this_obj, "frameHeight");
    jint this_transform = get_object_field_int(this_obj, "transform");
    jint this_currentFrame = get_object_field_int(this_obj, "currentFrame");
    JavaArray* this_seq = (JavaArray*)get_object_field_ref(this_obj, "frameSequence");
    jint this_actual_frame = this_currentFrame;
    if (this_seq && this_seq->length > 0 && this_currentFrame >= 0 && this_currentFrame < (jint)this_seq->length) {
        this_actual_frame = ((jint*)array_data(this_seq))[this_currentFrame];
    }
    
    /* Get frame info for other sprite */
    jint other_frameWidth = get_object_field_int(other_sprite, "frameWidth");
    jint other_frameHeight = get_object_field_int(other_sprite, "frameHeight");
    jint other_transform = get_object_field_int(other_sprite, "transform");
    jint other_currentFrame = get_object_field_int(other_sprite, "currentFrame");
    JavaArray* other_seq = (JavaArray*)get_object_field_ref(other_sprite, "frameSequence");
    jint other_actual_frame = other_currentFrame;
    if (other_seq && other_seq->length > 0 && other_currentFrame >= 0 && other_currentFrame < (jint)other_seq->length) {
        other_actual_frame = ((jint*)array_data(other_seq))[other_currentFrame];
    }
    
    /* Calculate source region for this sprite's current frame */
    int this_src_x = 0, this_src_y = 0;
    if (this_frameWidth > 0 && this_frameHeight > 0) {
        int this_fpr = this_img->width / this_frameWidth;
        if (this_fpr > 0) {
            this_src_x = (this_actual_frame % this_fpr) * this_frameWidth;
            this_src_y = (this_actual_frame / this_fpr) * this_frameHeight;
        }
    }
    
    /* Calculate source region for other sprite's current frame */
    int other_src_x = 0, other_src_y = 0;
    if (other_frameWidth > 0 && other_frameHeight > 0) {
        int other_fpr = other_img->width / other_frameWidth;
        if (other_fpr > 0) {
            other_src_x = (other_actual_frame % other_fpr) * other_frameWidth;
            other_src_y = (other_actual_frame / other_fpr) * other_frameHeight;
        }
    }
    
    int this_sx = get_object_field_int(this_obj, "x");
    int this_sy = get_object_field_int(this_obj, "y");
    int other_sx = get_object_field_int(other_sprite, "x");
    int other_sy = get_object_field_int(other_sprite, "y");
    
    /* Check each pixel in intersection region */
    for (int y = intersect_y1; y < intersect_y2; y++) {
        for (int x = intersect_x1; x < intersect_x2; x++) {
            /* Map screen coords to this sprite's source image pixel */
            int this_lx = x - this_sx;  /* local coord in sprite's drawn area */
            int this_ly = y - this_sy;
            
            /* Apply inverse transform to get source pixel */
            int this_fw = (this_transform == 5 || this_transform == 4 || this_transform == 6 || this_transform == 7)
                          ? this_frameHeight : this_frameWidth;
            int this_fh = (this_transform == 5 || this_transform == 4 || this_transform == 6 || this_transform == 7)
                          ? this_frameWidth : this_frameHeight;
            
            /* Simple approach: for untransformed sprites, direct mapping */
            int this_img_x = this_src_x + this_lx;
            int this_img_y = this_src_y + this_ly;
            
            if (this_transform != 0) {
                /* For transformed sprites, map through the transform */
                switch (this_transform) {
                    case 2: this_img_x = this_src_x + this_fw - 1 - this_lx; this_img_y = this_src_y + this_ly; break;
                    case 1: this_img_x = this_src_x + this_lx; this_img_y = this_src_y + this_fh - 1 - this_ly; break;
                    case 3: this_img_x = this_src_x + this_fw - 1 - this_lx; this_img_y = this_src_y + this_fh - 1 - this_ly; break;
                    case 5: this_img_x = this_src_x + this_ly; this_img_y = this_src_y + this_fh - 1 - this_lx; break;
                    case 4: this_img_x = this_src_x + this_ly; this_img_y = this_src_y + this_lx; break;
                    case 6: this_img_x = this_src_x + this_fw - 1 - this_ly; this_img_y = this_src_y + this_lx; break;
                    case 7: this_img_x = this_src_x + this_fw - 1 - this_ly; this_img_y = this_src_y + this_fh - 1 - this_lx; break;
                }
            }
            
            /* Map screen coords to other sprite's source image pixel */
            int other_lx = x - other_sx;
            int other_ly = y - other_sy;
            int other_fw = (other_transform == 5 || other_transform == 4 || other_transform == 6 || other_transform == 7)
                           ? other_frameHeight : other_frameWidth;
            int other_fh = (other_transform == 5 || other_transform == 4 || other_transform == 6 || other_transform == 7)
                           ? other_frameWidth : other_frameHeight;
            
            int other_img_x = other_src_x + other_lx;
            int other_img_y = other_src_y + other_ly;
            
            if (other_transform != 0) {
                switch (other_transform) {
                    case 2: other_img_x = other_src_x + other_fw - 1 - other_lx; other_img_y = other_src_y + other_ly; break;
                    case 1: other_img_x = other_src_x + other_lx; other_img_y = other_src_y + other_fh - 1 - other_ly; break;
                    case 3: other_img_x = other_src_x + other_fw - 1 - other_lx; other_img_y = other_src_y + other_fh - 1 - other_ly; break;
                    case 5: other_img_x = other_src_x + other_ly; other_img_y = other_src_y + other_fh - 1 - other_lx; break;
                    case 4: other_img_x = other_src_x + other_ly; other_img_y = other_src_y + other_lx; break;
                    case 6: other_img_x = other_src_x + other_fw - 1 - other_ly; other_img_y = other_src_y + other_lx; break;
                    case 7: other_img_x = other_src_x + other_fw - 1 - other_ly; other_img_y = other_src_y + other_fh - 1 - other_lx; break;
                }
            }
            
            /* Check bounds in source images */
            if (this_img_x < this_src_x || this_img_x >= this_src_x + this_frameWidth) continue;
            if (this_img_y < this_src_y || this_img_y >= this_src_y + this_frameHeight) continue;
            if (other_img_x < other_src_x || other_img_x >= other_src_x + other_frameWidth) continue;
            if (other_img_y < other_src_y || other_img_y >= other_src_y + other_frameHeight) continue;
            
            /* Check if both pixels are non-transparent */
            uint32_t this_pixel = this_img->pixels[this_img_y * this_img->width + this_img_x];
            uint32_t other_pixel = other_img->pixels[other_img_y * other_img->width + other_img_x];
            
            if (((this_pixel >> 24) & 0xFF) > 0 && ((other_pixel >> 24) & 0xFF) > 0) {
                return NATIVE_RETURN_INT(1); /* true - collision found */
            }
        }
    }
    
    return NATIVE_RETURN_INT(0); /* false */
}

/* Sprite.collidesWith(TiledLayer t, boolean pixelLevel) */
static JavaValue native_sprite_collidesWith_tiledlayer(JVM* jvm, JavaThread* thread,
                                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    JavaObject* tiledlayer = (JavaObject*)args[1].ref;
    jboolean pixel_level = args[2].i;
    
    if (!this_obj || !tiledlayer) {
        return NATIVE_RETURN_INT(0);
    }
    
    /* Get sprite bounds */
    int this_x = get_object_field_int(this_obj, "x");
    int this_y = get_object_field_int(this_obj, "y");
    int this_w = get_object_field_int(this_obj, "width");
    int this_h = get_object_field_int(this_obj, "height");
    
    /* Get tiled layer bounds */
    int tl_x = get_object_field_int(tiledlayer, "x");
    int tl_y = get_object_field_int(tiledlayer, "y");
    int tl_w = get_object_field_int(tiledlayer, "width");
    int tl_h = get_object_field_int(tiledlayer, "height");
    
    /* Rectangle intersection test */
    int intersect_x1 = this_x > tl_x ? this_x : tl_x;
    int intersect_y1 = this_y > tl_y ? this_y : tl_y;
    int intersect_x2 = (this_x + this_w) < (tl_x + tl_w) ? (this_x + this_w) : (tl_x + tl_w);
    int intersect_y2 = (this_y + this_h) < (tl_y + tl_h) ? (this_y + this_h) : (tl_y + tl_h);
    
    /* No intersection */
    if (intersect_x1 >= intersect_x2 || intersect_y1 >= intersect_y2) {
        return NATIVE_RETURN_INT(0);
    }
    
    /* If pixel-level is not requested, bounding box is enough for non-empty cells */
    if (!pixel_level) {
        /* Check if any overlapping cell is non-empty (tile index != 0) */
        int tl_columns = get_object_field_int(tiledlayer, "columns");
        int tl_rows = get_object_field_int(tiledlayer, "rows");
        int tl_tile_w = get_object_field_int(tiledlayer, "tileWidth");
        int tl_tile_h = get_object_field_int(tiledlayer, "tileHeight");
        
        /* Get cell map array */
        JavaArray* cell_map = (JavaArray*)get_object_field_ref(tiledlayer, "cellMap");
        if (!cell_map || cell_map->length == 0) {
            return NATIVE_RETURN_INT(0);
        }
        jint* cells = (jint*)array_data(cell_map);
        
        /* Determine which tiles overlap with the sprite */
        int start_col = (intersect_x1 - tl_x) / tl_tile_w;
        int start_row = (intersect_y1 - tl_y) / tl_tile_h;
        int end_col = (intersect_x2 - tl_x - 1) / tl_tile_w;
        int end_row = (intersect_y2 - tl_y - 1) / tl_tile_h;
        
        if (start_col < 0) start_col = 0;
        if (start_row < 0) start_row = 0;
        if (end_col >= tl_columns) end_col = tl_columns - 1;
        if (end_row >= tl_rows) end_row = tl_rows - 1;
        
        for (int row = start_row; row <= end_row; row++) {
            for (int col = start_col; col <= end_col; col++) {
                int tile_idx = cells[row * tl_columns + col];
                if (tile_idx != 0) {
                    return NATIVE_RETURN_INT(1); /* Collision with non-empty cell */
                }
            }
        }
        return NATIVE_RETURN_INT(0); /* All overlapping cells are empty */
    }
    
    /* Pixel-level collision with tiled layer: just check bounding box overlap
     * (full pixel-level tiled layer collision would require rendering each tile
     * and checking individual pixels, which is very expensive) */
    return NATIVE_RETURN_INT(1);
}

/* Sprite.collidesWith(Image image, int x, int y, boolean pixelLevel) */
static JavaValue native_sprite_collidesWith_image(JVM* jvm, JavaThread* thread,
                                                  JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    JavaObject* image_obj = (JavaObject*)args[1].ref;
    jint img_x = args[2].i;
    jint img_y = args[3].i;
    jboolean pixel_level = args[4].i;
    
    if (!this_obj || !image_obj) {
        return NATIVE_RETURN_INT(0);
    }
    
    /* Check visibility */
    if (!get_object_field_int(this_obj, "visible")) {
        return NATIVE_RETURN_INT(0);
    }
    
    /* Use collision rectangle instead of full bounds */
    int this_x = get_object_field_int(this_obj, "x") + get_object_field_int(this_obj, "collisionX");
    int this_y = get_object_field_int(this_obj, "y") + get_object_field_int(this_obj, "collisionY");
    int this_w = get_object_field_int(this_obj, "collisionWidth");
    int this_h = get_object_field_int(this_obj, "collisionHeight");
    
    /* Get image dimensions */
    MidpImage* img = get_image_from_object(image_obj);
    if (!img) return NATIVE_RETURN_INT(0);
    
    /* Rectangle intersection test */
    int intersect_x1 = this_x > img_x ? this_x : img_x;
    int intersect_y1 = this_y > img_y ? this_y : img_y;
    int intersect_x2 = (this_x + this_w) < (img_x + img->width) ? (this_x + this_w) : (img_x + img->width);
    int intersect_y2 = (this_y + this_h) < (img_y + img->height) ? (this_y + this_h) : (img_y + img->height);
    
    if (intersect_x1 >= intersect_x2 || intersect_y1 >= intersect_y2) {
        return NATIVE_RETURN_INT(0);
    }
    
    /* If not pixel-level, bounding box is enough */
    if (!pixel_level) {
        return NATIVE_RETURN_INT(1);
    }
    
    /* Pixel-level collision with the image */
    MidpImage* this_img = get_sprite_image(this_obj);
    if (!this_img) return NATIVE_RETURN_INT(0);
    
    int sprite_sx = get_object_field_int(this_obj, "x");
    int sprite_sy = get_object_field_int(this_obj, "y");
    jint frameWidth = get_object_field_int(this_obj, "frameWidth");
    jint frameHeight = get_object_field_int(this_obj, "frameHeight");
    jint transform = get_object_field_int(this_obj, "transform");
    jint currentFrame = get_object_field_int(this_obj, "currentFrame");
    JavaArray* seq = (JavaArray*)get_object_field_ref(this_obj, "frameSequence");
    jint actual_frame = currentFrame;
    if (seq && seq->length > 0 && currentFrame >= 0 && currentFrame < (jint)seq->length) {
        actual_frame = ((jint*)array_data(seq))[currentFrame];
    }
    
    int src_x = 0, src_y = 0;
    if (frameWidth > 0 && frameHeight > 0) {
        int fpr = this_img->width / frameWidth;
        if (fpr > 0) {
            src_x = (actual_frame % fpr) * frameWidth;
            src_y = (actual_frame / fpr) * frameHeight;
        }
    }
    
    for (int y = intersect_y1; y < intersect_y2; y++) {
        for (int x = intersect_x1; x < intersect_x2; x++) {
            /* Map to sprite source image */
            int lx = x - sprite_sx;
            int ly = y - sprite_sy;
            int s_img_x = src_x + lx;
            int s_img_y = src_y + ly;
            
            if (transform != 0) {
                int fw = (transform == 5 || transform == 4 || transform == 6 || transform == 7)
                         ? frameHeight : frameWidth;
                int fh = (transform == 5 || transform == 4 || transform == 6 || transform == 7)
                         ? frameWidth : frameHeight;
                switch (transform) {
                    case 2: s_img_x = src_x + fw - 1 - lx; s_img_y = src_y + ly; break;
                    case 1: s_img_x = src_x + lx; s_img_y = src_y + fh - 1 - ly; break;
                    case 3: s_img_x = src_x + fw - 1 - lx; s_img_y = src_y + fh - 1 - ly; break;
                    case 5: s_img_x = src_x + ly; s_img_y = src_y + fh - 1 - lx; break;
                    case 4: s_img_x = src_x + ly; s_img_y = src_y + lx; break;
                    case 6: s_img_x = src_x + fw - 1 - ly; s_img_y = src_y + lx; break;
                    case 7: s_img_x = src_x + fw - 1 - ly; s_img_y = src_y + fh - 1 - lx; break;
                }
            }
            
            if (s_img_x < 0 || s_img_x >= this_img->width || s_img_y < 0 || s_img_y >= this_img->height) continue;
            
            int r_img_x = x - img_x;
            int r_img_y = y - img_y;
            if (r_img_x < 0 || r_img_x >= img->width || r_img_y < 0 || r_img_y >= img->height) continue;
            
            uint32_t sp = this_img->pixels[s_img_y * this_img->width + s_img_x];
            uint32_t rp = img->pixels[r_img_y * img->width + r_img_x];
            
            if (((sp >> 24) & 0xFF) > 0 && ((rp >> 24) & 0xFF) > 0) {
                return NATIVE_RETURN_INT(1);
            }
        }
    }
    
    return NATIVE_RETURN_INT(0);
}

/* Sprite.defineCollisionRectangle(int x, int y, int width, int height) */
static JavaValue native_sprite_defineCollisionRectangle(JVM* jvm, JavaThread* thread,
                                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    jint x = args[1].i;
    jint y = args[2].i;
    jint width = args[3].i;
    jint height = args[4].i;
    
    if (!this_obj) return NATIVE_RETURN_VOID();
    
    /* Store collision rectangle parameters in sprite fields */
    set_object_field_int(this_obj, "collisionX", x);
    set_object_field_int(this_obj, "collisionY", y);
    set_object_field_int(this_obj, "collisionWidth", width);
    set_object_field_int(this_obj, "collisionHeight", height);
    
    return NATIVE_RETURN_VOID();
}

/* Sprite.setFrameSequence(int[] sequence) */
static JavaValue native_sprite_setFrameSequence(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    JavaArray* sequence = (JavaArray*)args[1].ref;
    
    if (!this_obj) return NATIVE_RETURN_VOID();
    
    /* Store frame sequence reference */
    set_object_field_ref(this_obj, "frameSequence", (JavaObject*)sequence);
    
    /* Reset current frame index to 0 (MIDP spec) */
    set_object_field_int(this_obj, "currentFrame", 0);
    
    return NATIVE_RETURN_VOID();
}

/* Sprite.nextFrame() */
static JavaValue native_sprite_nextFrame(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    
    if (!this_obj) return NATIVE_RETURN_VOID();
    
    int current_frame = get_object_field_int(this_obj, "currentFrame");
    
    /* Use frameSequence length if set, otherwise rawFrameCount */
    int max_frames;
    JavaArray* frame_seq = (JavaArray*)get_object_field_ref(this_obj, "frameSequence");
    if (frame_seq && frame_seq->length > 0) {
        max_frames = (int)frame_seq->length;
    } else {
        int raw_frame_count = get_object_field_int(this_obj, "rawFrameCount");
        max_frames = raw_frame_count > 0 ? raw_frame_count : 1;
    }
    
    int new_frame = current_frame + 1;
    if (new_frame >= max_frames) new_frame = 0;
    set_object_field_int(this_obj, "currentFrame", new_frame);
    
    return NATIVE_RETURN_VOID();
}

/* Sprite.prevFrame() */
static JavaValue native_sprite_prevFrame(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    
    if (!this_obj) return NATIVE_RETURN_VOID();
    
    int current_frame = get_object_field_int(this_obj, "currentFrame");
    
    /* Use frameSequence length if set, otherwise rawFrameCount */
    int max_frames;
    JavaArray* frame_seq = (JavaArray*)get_object_field_ref(this_obj, "frameSequence");
    if (frame_seq && frame_seq->length > 0) {
        max_frames = (int)frame_seq->length;
    } else {
        int raw_frame_count = get_object_field_int(this_obj, "rawFrameCount");
        max_frames = raw_frame_count > 0 ? raw_frame_count : 1;
    }
    
    int new_frame = current_frame - 1;
    if (new_frame < 0) new_frame = max_frames - 1;
    set_object_field_int(this_obj, "currentFrame", new_frame);
    
    return NATIVE_RETURN_VOID();
}

/* Sprite.defineReferencePixel(int x, int y) */
static JavaValue native_sprite_defineReferencePixel(JVM* jvm, JavaThread* thread,
                                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    jint refX = args[1].i;
    jint refY = args[2].i;
    
    if (!this_obj) return NATIVE_RETURN_VOID();
    
    set_object_field_int(this_obj, "refX", refX);
    set_object_field_int(this_obj, "refY", refY);
    
    return NATIVE_RETURN_VOID();
}

/* Sprite.setRefPixelPosition(int x, int y) - moves sprite so ref pixel is at (x,y) */
static JavaValue native_sprite_setRefPixelPosition(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    jint x = args[1].i;
    jint y = args[2].i;
    
    if (!this_obj) return NATIVE_RETURN_VOID();
    
    jint refX = get_object_field_int(this_obj, "refX");
    jint refY = get_object_field_int(this_obj, "refY");
    jint transform = get_object_field_int(this_obj, "transform");
    jint w = get_object_field_int(this_obj, "width");
    jint h = get_object_field_int(this_obj, "height");

    /* v34.33 FIX: width/height are the POST-transform Layer dimensions; the
     * ref-pixel transform table maps SOURCE coordinates, so un-swap the
     * dims first when the active transform rotates by 90/270 (matches the
     * src-coordinate math in midp_graphics_draw_region). */
    if (transform == 4 || transform == 5 || transform == 6 || transform == 7) {
        jint tmp = w; w = h; h = tmp;
    }
    
    /* Compute transformed reference pixel offset */
    jint transformed_refX, transformed_refY;
    switch (transform) {
        case 0: transformed_refX = refX; transformed_refY = refY; break;
        case 1: transformed_refX = refX; transformed_refY = h - 1 - refY; break;
        case 2: transformed_refX = w - 1 - refX; transformed_refY = refY; break;
        case 3: transformed_refX = w - 1 - refX; transformed_refY = h - 1 - refY; break;
        case 4: transformed_refX = refY; transformed_refY = refX; break;
        case 5: transformed_refX = h - 1 - refY; transformed_refY = refX; break;
        case 6: transformed_refX = refY; transformed_refY = w - 1 - refX; break;
        case 7: transformed_refX = h - 1 - refY; transformed_refY = w - 1 - refX; break;
        default: transformed_refX = refX; transformed_refY = refY; break;
    }
    
    /* Position sprite so that the reference pixel appears at (x, y) */
    set_object_field_int(this_obj, "x", x - transformed_refX);
    set_object_field_int(this_obj, "y", y - transformed_refY);
    
    return NATIVE_RETURN_VOID();
}

/* Sprite.getRefPixelX() */
static JavaValue native_sprite_getRefPixelX(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    
    if (!this_obj) return NATIVE_RETURN_INT(0);
    
    jint refX = get_object_field_int(this_obj, "refX");
    jint refY = get_object_field_int(this_obj, "refY");
    jint transform = get_object_field_int(this_obj, "transform");
    jint w = get_object_field_int(this_obj, "width");
    jint h = get_object_field_int(this_obj, "height");
    jint sprite_x = get_object_field_int(this_obj, "x");
    
    /* Compute transformed reference pixel offset */
    jint transformed_refX;
    switch (transform) {
        case 0: transformed_refX = refX; break;
        case 1: transformed_refX = refX; break;
        case 2: transformed_refX = w - 1 - refX; break;
        case 3: transformed_refX = w - 1 - refX; break;
        case 4: transformed_refX = refY; break;
        case 5: transformed_refX = h - 1 - refY; break;
        case 6: transformed_refX = refY; break;
        case 7: transformed_refX = h - 1 - refY; break;
        default: transformed_refX = refX; break;
    }
    
    return NATIVE_RETURN_INT(sprite_x + transformed_refX);
}

/* Sprite.getRefPixelY() */
static JavaValue native_sprite_getRefPixelY(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    
    if (!this_obj) return NATIVE_RETURN_INT(0);
    
    jint refX = get_object_field_int(this_obj, "refX");
    jint refY = get_object_field_int(this_obj, "refY");
    jint transform = get_object_field_int(this_obj, "transform");
    jint w = get_object_field_int(this_obj, "width");
    jint h = get_object_field_int(this_obj, "height");
    jint sprite_y = get_object_field_int(this_obj, "y");
    
    /* Compute transformed reference pixel offset */
    jint transformed_refY;
    switch (transform) {
        case 0: transformed_refY = refY; break;
        case 1: transformed_refY = h - 1 - refY; break;
        case 2: transformed_refY = refY; break;
        case 3: transformed_refY = h - 1 - refY; break;
        case 4: transformed_refY = refX; break;
        case 5: transformed_refY = refX; break;
        case 6: transformed_refY = w - 1 - refX; break;
        case 7: transformed_refY = w - 1 - refX; break;
        default: transformed_refY = refY; break;
    }
    
    return NATIVE_RETURN_INT(sprite_y + transformed_refY);
}

/* Sprite.setImage(Image img, int frameWidth, int frameHeight) */
static JavaValue native_sprite_setImage(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    JavaObject* image_obj = (JavaObject*)args[1].ref;
    jint frame_width = args[2].i;
    jint frame_height = args[3].i;
    
    if (!this_obj || !image_obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    
    MidpImage* img = get_image_from_object(image_obj);
    if (!img) return NATIVE_RETURN_VOID();
    
    /* FIX (audit S-18, v18): JSR-118 requires setImage to PRESERVE the active
     * transform, custom frame sequence, current index and collision rectangle
     * wherever they remain valid for the new atlas. The old code wiped
     * everything — games that recolor sprites on the fly lost rotations,
     * hitboxes and animation state mid-frame. */
    
    jint old_transform = get_object_field_int(this_obj, "transform");
    JavaArray* old_sequence = (JavaArray*)get_object_field_ref(this_obj, "frameSequence");
    jint old_seq_len = 0;
    jint* old_seq_data = NULL;
    if (old_sequence && old_sequence->element_type == T_INT) {
        old_seq_len = old_sequence->length;
        old_seq_data = (jint*)array_data(old_sequence);
    }
    jint old_collision_x = get_object_field_int(this_obj, "collisionX");
    jint old_collision_y = get_object_field_int(this_obj, "collisionY");
    jint old_collision_w = get_object_field_int(this_obj, "collisionWidth");
    jint old_collision_h = get_object_field_int(this_obj, "collisionHeight");
    
    /* Update image */
    set_object_field_ref(this_obj, "image", image_obj);
    set_object_field_int(this_obj, "frameWidth", frame_width);
    set_object_field_int(this_obj, "frameHeight", frame_height);
    
    /* Preserve the transform — its bounding box logic adapts automatically. */
    set_object_field_int(this_obj, "transform", old_transform);
    
    /* v34.33 FIX (JSR-118 §Sprite.setImage / §Layer): the Layer's
     * width/height are the POST-transform dimensions — when the ACTIVE
     * transform is a 90/270 rotation they must be swapped, exactly like
     * setTransform() does. The old code reset them to the unrotated frame
     * size; with the transform already active setTransform() early-returns
     * (same value) and never re-swaps, so every paint computed the
     * reference-pixel offset from the wrong dimensions. Block 3D 2
     * (Cocoasoft) blits its whole 400x240 scene through ONE
     * setTransform(TRANS_ROT90) sprite and calls setImage() EVERY frame:
     * dest_x drifted to -239 and only a 1-pixel column reached the screen
     * (the rest of the "white screen" game). */
    {
        bool dim_swap = (old_transform == 4 || old_transform == 5 ||
                         old_transform == 6 || old_transform == 7);
        set_object_field_int(this_obj, "width",  dim_swap ? frame_height : frame_width);
        set_object_field_int(this_obj, "height", dim_swap ? frame_width  : frame_height);
    }
    
    /* Recalculate raw frame count */
    jint new_raw_count = 0;
    if (frame_width > 0 && frame_height > 0 &&
        img->width >= frame_width && img->height >= frame_height) {
        int cols = img->width / frame_width;
        int rows = img->height / frame_height;
        new_raw_count = cols * rows;
    }
    set_object_field_int(this_obj, "rawFrameCount", new_raw_count);
    
    /* Keep the custom frame sequence when every index is still valid;
     * otherwise fall back to the default sequence and rewind to frame 0. */
    bool seq_valid = false;
    if (old_seq_data && old_seq_len > 0) {
        seq_valid = true;
        for (jint i = 0; i < old_seq_len; i++) {
            if (old_seq_data[i] < 0 || old_seq_data[i] >= new_raw_count) {
                seq_valid = false;
                break;
            }
        }
    }
    if (!seq_valid) {
        set_object_field_ref(this_obj, "frameSequence", NULL);
        set_object_field_int(this_obj, "currentFrame", 0);
    } else {
        set_object_field_ref(this_obj, "frameSequence", (JavaObject*)old_sequence);
        /* keep currentFrame as-is (still within sequence bounds by definition) */
    }
    
    /* Preserve the collision rectangle where it stays inside the new frame;
     * clip or reset to full-frame size only when it would stick out. */
    if (old_collision_w > 0 && old_collision_h > 0 &&
        old_collision_x + old_collision_w <= frame_width &&
        old_collision_y + old_collision_h <= frame_height) {
        set_object_field_int(this_obj, "collisionX", old_collision_x);
        set_object_field_int(this_obj, "collisionY", old_collision_y);
        set_object_field_int(this_obj, "collisionWidth", old_collision_w);
        set_object_field_int(this_obj, "collisionHeight", old_collision_h);
    } else {
        set_object_field_int(this_obj, "collisionX", 0);
        set_object_field_int(this_obj, "collisionY", 0);
        set_object_field_int(this_obj, "collisionWidth", frame_width);
        set_object_field_int(this_obj, "collisionHeight", frame_height);
    }
    
    return NATIVE_RETURN_VOID();
}

/* Sprite.getRawFrameCount() */
static JavaValue native_sprite_getRawFrameCount(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    
    if (!this_obj) return NATIVE_RETURN_INT(0);
    
    jint count = get_object_field_int(this_obj, "rawFrameCount");
    return NATIVE_RETURN_INT(count > 0 ? count : 1);
}

/* Sprite.getFrameSequenceLength() */
static JavaValue native_sprite_getFrameSequenceLength(JVM* jvm, JavaThread* thread,
                                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    
    if (!this_obj) return NATIVE_RETURN_INT(1);
    
    /* If a custom sequence is set, return its length; otherwise return rawFrameCount */
    JavaArray* sequence = (JavaArray*)get_object_field_ref(this_obj, "frameSequence");
    if (sequence && sequence->length > 0) {
        return NATIVE_RETURN_INT(sequence->length);
    }
    
    jint count = get_object_field_int(this_obj, "rawFrameCount");
    return NATIVE_RETURN_INT(count > 0 ? count : 1);
}

/*
 * ============================================
 * Layer native methods (abstract base class)
 * ============================================
 */

/* Layer.getX() */
static JavaValue native_layer_getX(JVM* jvm, JavaThread* thread,
                                   JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    
    if (!this_obj) return NATIVE_RETURN_INT(0);
    
    return NATIVE_RETURN_INT(get_object_field_int(this_obj, "x"));
}

/* Layer.getY() */
static JavaValue native_layer_getY(JVM* jvm, JavaThread* thread,
                                   JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    
    if (!this_obj) return NATIVE_RETURN_INT(0);
    
    return NATIVE_RETURN_INT(get_object_field_int(this_obj, "y"));
}

/* Layer.getWidth() */
static JavaValue native_layer_getWidth(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    
    if (!this_obj) return NATIVE_RETURN_INT(0);
    
    return NATIVE_RETURN_INT(get_object_field_int(this_obj, "width"));
}

/* Layer.getHeight() */
static JavaValue native_layer_getHeight(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    
    if (!this_obj) return NATIVE_RETURN_INT(0);
    
    return NATIVE_RETURN_INT(get_object_field_int(this_obj, "height"));
}

/* Layer.setPosition(int x, int y) */
static JavaValue native_layer_setPosition(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    jint x = args[1].i;
    jint y = args[2].i;
    
    if (!this_obj) return NATIVE_RETURN_VOID();
    
    set_object_field_int(this_obj, "x", x);
    set_object_field_int(this_obj, "y", y);
    
    return NATIVE_RETURN_VOID();
}

/* Layer.move(int dx, int dy) */
static JavaValue native_layer_move(JVM* jvm, JavaThread* thread,
                                   JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    jint dx = args[1].i;
    jint dy = args[2].i;
    
    if (!this_obj) return NATIVE_RETURN_VOID();
    
    jint x = get_object_field_int(this_obj, "x");
    jint y = get_object_field_int(this_obj, "y");
    set_object_field_int(this_obj, "x", x + dx);
    set_object_field_int(this_obj, "y", y + dy);
    
    return NATIVE_RETURN_VOID();
}

/* Layer.setVisible(boolean visible) */
static JavaValue native_layer_setVisible(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    jboolean visible = args[1].i;
    
    if (!this_obj) return NATIVE_RETURN_VOID();
    
    set_object_field_int(this_obj, "visible", visible);
    
    return NATIVE_RETURN_VOID();
}

/* Layer.isVisible() */
static JavaValue native_layer_isVisible(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    
    if (!this_obj) return NATIVE_RETURN_INT(1);
    
    return NATIVE_RETURN_INT(get_object_field_int(this_obj, "visible"));
}

/*
 * ============================================
 * LayerManager native methods
 * ============================================
 */

/* LayerManager.<init>() */
static JavaValue native_layermanager_init(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    
    if (!this_obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    
    DISP_DEBUG("[LayerManager] init: this=%p", (void*)this_obj);
    
    /* Initialize layers array as empty */
    JavaArray* empty_layers = jvm_new_array(jvm, DESC_OBJECT, 0, NULL);
    if (empty_layers) {
        set_object_field_ref(this_obj, "layers", (JavaObject*)empty_layers);
    }
    
    /* Set default view window to MAX_VALUE dimensions */
    set_object_field_int(this_obj, "viewX", 0);
    set_object_field_int(this_obj, "viewY", 0);
    set_object_field_int(this_obj, "viewWidth", 0x7FFFFFFF);
    set_object_field_int(this_obj, "viewHeight", 0x7FFFFFFF);
    
    return NATIVE_RETURN_VOID();
}

/* LayerManager.append(Layer layer) */
static JavaValue native_layermanager_append(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    JavaObject* layer = (JavaObject*)args[1].ref;
    
    if (!this_obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_INT(-1);
    }
    
    /* Get current layers array */
    JavaArray* layers = (JavaArray*)get_object_field_ref(this_obj, "layers");
    int old_count = layers ? layers->length : 0;
    int new_count = old_count + 1;
    
    /* Create new layers array with one more slot */
    JavaArray* new_layers = jvm_new_array(jvm, DESC_OBJECT, new_count, NULL);
    if (!new_layers) {
        DISP_DEBUG("[LayerManager] append: failed to allocate new array");
        return NATIVE_RETURN_INT(-1);
    }
    
    /* Copy old layers */
    if (layers && layers->length > 0) {
        void** old_data = (void**)array_data(layers);
        void** new_data = (void**)array_data(new_layers);
        for (int i = 0; i < old_count; i++) {
            new_data[i] = old_data[i];
        }
    }
    
    /* Add new layer at end */
    void** new_data = (void**)array_data(new_layers);
    new_data[old_count] = layer;
    
    /* Update LayerManager's layers array */
    set_object_field_ref(this_obj, "layers", (JavaObject*)new_layers);
    
    DISP_DEBUG("[LayerManager] append: layer=%p at index %d, count=%d", (void*)layer, old_count, new_count);
    
    return NATIVE_RETURN_INT(old_count);  /* Return layer index */
}

/* LayerManager.insert(Layer layer, int index) */
static JavaValue native_layermanager_insert(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    JavaObject* layer = (JavaObject*)args[1].ref;
    jint index = args[2].i;
    
    if (!this_obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    
    /* Get current layers array */
    JavaArray* layers = (JavaArray*)get_object_field_ref(this_obj, "layers");
    int old_count = layers ? layers->length : 0;
    int new_count = old_count + 1;
    
    /* Clamp index */
    if (index < 0) index = 0;
    if (index > old_count) index = old_count;
    
    /* Create new layers array with one more slot */
    JavaArray* new_layers = jvm_new_array(jvm, DESC_OBJECT, new_count, NULL);
    if (!new_layers) {
        DISP_DEBUG("[LayerManager] insert: failed to allocate new array");
        return NATIVE_RETURN_VOID();
    }
    
    /* Copy old layers, inserting new layer at index */
    void** old_data = (layers && layers->length > 0) ? (void**)array_data(layers) : NULL;
    void** new_data = (void**)array_data(new_layers);
    for (int i = 0; i < new_count; i++) {
        if (i < index) {
            new_data[i] = old_data ? old_data[i] : NULL;
        } else if (i == index) {
            new_data[i] = layer;
        } else {
            new_data[i] = old_data ? old_data[i - 1] : NULL;
        }
    }
    
    /* Update LayerManager's layers array */
    set_object_field_ref(this_obj, "layers", (JavaObject*)new_layers);
    
    DISP_DEBUG("[LayerManager] insert: layer=%p at %d, count=%d", (void*)layer, index, new_count);
    
    return NATIVE_RETURN_VOID();
}

/* LayerManager.remove(Layer layer) */
static JavaValue native_layermanager_remove(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    JavaObject* layer = (JavaObject*)args[1].ref;
    
    if (!this_obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    
    /* Get current layers array */
    JavaArray* layers = (JavaArray*)get_object_field_ref(this_obj, "layers");
    if (!layers || layers->length == 0) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Find the layer in the array */
    void** layer_data = (void**)array_data(layers);
    int found_idx = -1;
    for (int i = 0; i < layers->length; i++) {
        if (layer_data[i] == layer) {
            found_idx = i;
            break;
        }
    }
    
    if (found_idx < 0) {
        DISP_DEBUG("[LayerManager] remove: layer %p not found", (void*)layer);
        return NATIVE_RETURN_VOID();
    }
    
    /* Create new array with one less element */
    int new_len = layers->length - 1;
    if (new_len == 0) {
        set_object_field_ref(this_obj, "layers", (JavaObject*)jvm_new_array(jvm, DESC_OBJECT, 0, NULL));
        DISP_DEBUG("[LayerManager] remove: removed last layer");
        return NATIVE_RETURN_VOID();
    }
    
    JavaArray* new_layers = jvm_new_array(jvm, DESC_OBJECT, new_len, NULL);
    if (!new_layers) {
        DISP_DEBUG("[LayerManager] remove: failed to allocate new array");
        return NATIVE_RETURN_VOID();
    }
    
    /* Copy layers, skipping the removed one */
    void** old_data = (void**)array_data(layers);
    void** new_data = (void**)array_data(new_layers);
    for (int i = 0; i < new_len; i++) {
        new_data[i] = old_data[i < found_idx ? i : i + 1];
    }
    
    /* Update LayerManager's layers array */
    set_object_field_ref(this_obj, "layers", (JavaObject*)new_layers);
    
    DISP_DEBUG("[LayerManager] remove: removed layer=%p at index %d, count=%d", (void*)layer, found_idx, new_len);
    
    return NATIVE_RETURN_VOID();
}

/* LayerManager.getLayerAt(int index) */
static JavaValue native_layermanager_getLayerAt(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    jint index = args[1].i;
    
    if (!this_obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_NULL();
    }
    
    /* Get layers array */
    JavaArray* layers = (JavaArray*)get_object_field_ref(this_obj, "layers");
    if (!layers || index < 0 || index >= layers->length) {
        return NATIVE_RETURN_NULL();
    }
    
    void** layer_data = (void**)array_data(layers);
    JavaObject* result = (JavaObject*)layer_data[index];
    
    DISP_DEBUG("[LayerManager] getLayerAt: %d -> %p", index, (void*)result);
    
    JavaValue ret = { .ref = result };
    return ret;
}

/* LayerManager.getSize() */
static JavaValue native_layermanager_getSize(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    
    if (!this_obj) return NATIVE_RETURN_INT(0);
    
    /* LayerManager has a layers[] array field */
    JavaArray* layers = (JavaArray*)get_object_field_ref(this_obj, "layers");
    if (layers) {
        return NATIVE_RETURN_INT(layers->length);
    }
    
    return NATIVE_RETURN_INT(0);
}

/* Helper: Paint a single layer (Sprite or TiledLayer) */
static void paint_layer(JVM* jvm, JavaObject* layer, JavaObject* gfx_obj, int x, int y) {
    (void)jvm; (void)x; (void)y;
    if (!layer || !gfx_obj) return;
    
    JavaClass* clazz = layer->header.clazz;
    if (!clazz || !clazz->class_name) return;
    
    /* Find the paint method on the layer */
    JavaMethod* paint_method = jvm_resolve_method(jvm, clazz, "paint", "(Ljavax/microedition/lcdui/Graphics;)V");
    if (paint_method) {
        JavaValue paint_args[2];
        paint_args[0].ref = layer;
        paint_args[1].ref = gfx_obj;
        
        JavaValue result;
        JavaThread* th = jvm_current_thread(jvm);
        midp_paint_depth_inc();
        execute_method(jvm, th, paint_method, paint_args, &result);
        midp_paint_depth_dec();
        return;
    }

    /* v34.76 CRITICAL FIX: stub-built layer classes (Sprite, TiledLayer)
     * carry FIELDS ONLY - stubs.c generates no Java "paint" method for them,
     * their paint() is a registered NATIVE. jvm_resolve_method() returned
     * NULL and the layer was silently skipped, so LayerManager.paint()
     * rendered NOTHING for any stub layer (Roboros: 16 layers appended, all
     * invisible - "HUD on black screen"). Fall back to the native registry,
     * walking the class chain exactly like the invoke opcodes do. */
    {
        static const char* const PAINT_DESC = "(Ljavax/microedition/lcdui/Graphics;)V";
        NativeMethod native = NULL;
        JavaClass* sc = clazz;
        while (sc && !native) {
            if (sc->class_name) {
                native = native_find(jvm, sc->class_name, "paint", PAINT_DESC);
            }
            if (!native) sc = sc->super_class;
        }
        if (native) {
            JavaValue paint_args[2];
            paint_args[0].ref = layer;
            paint_args[1].ref = gfx_obj;
            JavaThread* th = jvm_current_thread(jvm);
            midp_paint_depth_inc();
            native(jvm, th, paint_args, 2);
            midp_paint_depth_dec();
        }
    }
}

/* LayerManager.paint(Graphics g, int x, int y) */
static JavaValue native_layermanager_paint(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    JavaObject* gfx_obj = (JavaObject*)args[1].ref;
    jint x = args[2].i;
    jint y = args[3].i;
    
    if (!this_obj || !gfx_obj) return NATIVE_RETURN_VOID();
    
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    if (!gfx) {
        DISP_DEBUG("[LayerManager] paint: failed to get MidpGraphics");
        return NATIVE_RETURN_VOID();
    }
    
    /* Get view window parameters */
    jint viewX = get_object_field_int(this_obj, "viewX");
    jint viewY = get_object_field_int(this_obj, "viewY");
    jint viewWidth = get_object_field_int(this_obj, "viewWidth");
    jint viewHeight = get_object_field_int(this_obj, "viewHeight");
    
    /* Default view window to screen size if not set */
    if (viewWidth <= 0) viewWidth = gfx->width;
    if (viewHeight <= 0) viewHeight = gfx->height;
    
    /* Get layers array */
    JavaArray* layers = (JavaArray*)get_object_field_ref(this_obj, "layers");
    if (!layers || layers->element_type != DESC_OBJECT) {
        return NATIVE_RETURN_VOID();
    }
    
    void** layer_data = (void**)array_data(layers);
    int layer_count = layers->length;
    
    /* Apply translation for the paint offset */
    int old_tx = gfx->translate_x;
    int old_ty = gfx->translate_y;
    gfx->translate_x += x - viewX;
    gfx->translate_y += y - viewY;
    
    /* Clip to view window.
     * v34.7: clip is kept in DEVICE space now, so intersect with the
     * device-space position of the view window: a manager point p is drawn
     * at device p + translate_new, hence viewX lands at old_tx + x. */
    int old_clip_x = gfx->clip_x;
    int old_clip_y = gfx->clip_y;
    int old_clip_w = gfx->clip_width;
    int old_clip_h = gfx->clip_height;

    midp_graphics_clip_rect(gfx, old_tx + x, old_ty + y, viewWidth, viewHeight);
    
    /* v23 FIX: paint layers from index count-1 down to 0. Per the MIDP
     * game API spec, index 0 is the CLOSEST layer to the user and must be
     * painted LAST; the old loop painted index 0 first, inverting the
     * z-order. */
    for (int i = layer_count - 1; i >= 0; i--) {
        JavaObject* layer = (JavaObject*)layer_data[i];
        if (layer) {
            /* Check if layer is visible */
            jint visible = get_object_field_int(layer, "visible");
            if (visible) {
                paint_layer(jvm, layer, gfx_obj, x, y);
            }
        }
    }
    
    /* Restore graphics state */
    gfx->translate_x = old_tx;
    gfx->translate_y = old_ty;
    gfx->clip_x = old_clip_x;
    gfx->clip_y = old_clip_y;
    gfx->clip_width = old_clip_w;
    gfx->clip_height = old_clip_h;
    
    return NATIVE_RETURN_VOID();
}

/* LayerManager.setViewWindow(int x, int y, int width, int height) */
static JavaValue native_layermanager_setViewWindow(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    jint x = args[1].i;
    jint y = args[2].i;
    jint width = args[3].i;
    jint height = args[4].i;
    
    if (!this_obj) return NATIVE_RETURN_VOID();
    
    /* Store view window in LayerManager fields */
    set_object_field_int(this_obj, "viewX", x);
    set_object_field_int(this_obj, "viewY", y);
    set_object_field_int(this_obj, "viewWidth", width);
    set_object_field_int(this_obj, "viewHeight", height);
    
    return NATIVE_RETURN_VOID();
}

/*
 * ============================================
 * TiledLayer native methods
 * ============================================
 */

/* TiledLayer.<init>(int columns, int rows, Image image, int tileWidth, int tileHeight) */
static JavaValue native_tiledlayer_init(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    jint columns = args[1].i;
    jint rows = args[2].i;
    JavaObject* image = (JavaObject*)args[3].ref;
    jint tile_width = args[4].i;
    jint tile_height = args[5].i;

    if (!this_obj || !image) {
        DISP_DEBUG("[TiledLayer] init: NPE - this=%p, image=%p",
                (void*)this_obj, (void*)image);
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }

    /* FIX (audit U-3, v18): JSR-118 TiledLayer constructor must reject
     * non-positive column/row/tile dimensions with IllegalArgumentException. */
    if (columns <= 0 || rows <= 0 || tile_width <= 0 || tile_height <= 0) {
        native_throw_iae(jvm, thread,
                "TiledLayer dimensions and tile size must be positive");
        return NATIVE_RETURN_VOID();
    }

    /* Store properties in TiledLayer fields */
    set_object_field_int(this_obj, "columns", columns);
    set_object_field_int(this_obj, "rows", rows);
    set_object_field_int(this_obj, "tileWidth", tile_width);
    set_object_field_int(this_obj, "tileHeight", tile_height);

    /* FIX (audit U-3, v18): initialize the inherited Layer width/height.
     * They were left at zero, so getWidth()/getHeight() returned 0 —
     * tile-based levels drew nothing AND Sprite.collidesWith(TiledLayer)
     * built a 0x0 bounding box that ALWAYS reported false (players walked
     * through walls). getWidth() = columns*tileWidth,
     * getHeight() = rows*tileHeight per JSR-118. */
    set_object_field_int(this_obj, "width", columns * tile_width);
    set_object_field_int(this_obj, "height", rows * tile_height);

    /* Store image reference */
    set_object_field_ref(this_obj, "image", image);
    
    /* Create cell map array */
    JavaArray* cell_map = jvm_new_array(jvm, T_INT, columns * rows, NULL);
    if (cell_map) {
        /* Initialize all cells to 0 (no tile) */
        jint* cells = (jint*)array_data(cell_map);
        for (int i = 0; i < columns * rows; i++) {
            cells[i] = 0;
        }
        /* Store in field */
        set_object_field_ref(this_obj, "cellMap", (JavaObject*)cell_map);
    }
    
    DISP_DEBUG("[TiledLayer] init: %dx%d tiles, image=%p, tile=%dx%d", 
            columns, rows, (void*)image, tile_width, tile_height);
    
    /* v34.33: Layer visible by default (spec) */
    set_object_field_int(this_obj, "visible", 1);
    return NATIVE_RETURN_VOID();
}

/* TiledLayer.setCell(int col, int row, int tileIndex) */
static JavaValue native_tiledlayer_setCell(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    jint col = args[1].i;
    jint row = args[2].i;
    jint tile_index = args[3].i;
    
    if (!this_obj) return NATIVE_RETURN_VOID();
    
    jint columns = get_object_field_int(this_obj, "columns");
    jint rows = get_object_field_int(this_obj, "rows");
    
    if (col < 0 || col >= columns || row < 0 || row >= rows) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Get cell map array */
    JavaArray* cell_map = (JavaArray*)get_object_field_ref(this_obj, "cellMap");
    
    if (cell_map && cell_map->element_type == T_INT) {
        jint* cells = (jint*)array_data(cell_map);
        cells[row * columns + col] = tile_index;
    }
    
    return NATIVE_RETURN_VOID();
}

/* TiledLayer.getCell(int col, int row) */
static JavaValue native_tiledlayer_getCell(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    jint col = args[1].i;
    jint row = args[2].i;
    
    if (!this_obj) return NATIVE_RETURN_INT(0);
    
    jint columns = get_object_field_int(this_obj, "columns");
    jint rows = get_object_field_int(this_obj, "rows");
    
    if (col < 0 || col >= columns || row < 0 || row >= rows) {
        return NATIVE_RETURN_INT(0);
    }
    
    /* Get cell map array */
    JavaArray* cell_map = (JavaArray*)get_object_field_ref(this_obj, "cellMap");
    
    if (cell_map && cell_map->element_type == T_INT) {
        jint* cells = (jint*)array_data(cell_map);
        return NATIVE_RETURN_INT(cells[row * columns + col]);
    }
    
    return NATIVE_RETURN_INT(0);
}

/* TiledLayer.fillCells(int col, int row, int numCols, int numRows, int tileIndex) */
static JavaValue native_tiledlayer_fillCells(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    jint col = args[1].i;
    jint row = args[2].i;
    jint num_cols = args[3].i;
    jint num_rows = args[4].i;
    jint tile_index = args[5].i;
    
    if (!this_obj) return NATIVE_RETURN_VOID();
    
    jint columns = get_object_field_int(this_obj, "columns");
    jint rows = get_object_field_int(this_obj, "rows");
    
    /* Validate bounds */
    if (col < 0 || row < 0 || num_cols < 0 || num_rows < 0) {
        return NATIVE_RETURN_VOID();
    }
    if (col + num_cols > columns || row + num_rows > rows) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Get cell map array */
    JavaArray* cell_map = (JavaArray*)get_object_field_ref(this_obj, "cellMap");
    
    if (cell_map && cell_map->element_type == T_INT) {
        jint* cells = (jint*)array_data(cell_map);
        /* Fill the rectangular region */
        for (int r = row; r < row + num_rows; r++) {
            for (int c = col; c < col + num_cols; c++) {
                cells[r * columns + c] = tile_index;
            }
        }
    }
    
    DISP_DEBUG("[TiledLayer] fillCells: (%d,%d) %dx%d = tile %d",
            col, row, num_cols, num_rows, tile_index);
    
    return NATIVE_RETURN_VOID();
}

/* TiledLayer.createAnimatedTile(int staticTileIndex) */
static JavaValue native_tiledlayer_createAnimatedTile(JVM* jvm, JavaThread* thread,
                                                       JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    jint static_tile_index = args[1].i;
    
    if (!this_obj) return NATIVE_RETURN_INT(0);
    
    /* Get or create animated tiles array */
    JavaArray* anim_tiles = (JavaArray*)get_object_field_ref(this_obj, "animatedTiles");
    
    /* Create array if not exists (initial capacity: 10) */
    if (!anim_tiles) {
        anim_tiles = jvm_new_array(jvm, T_INT, 10, NULL);
        if (!anim_tiles) return NATIVE_RETURN_INT(0);
        
        /* Store in field */
        set_object_field_ref(this_obj, "animatedTiles", (JavaObject*)anim_tiles);
    }
    
    /* Get current count */
    jint anim_count = get_object_field_int(this_obj, "animatedTileCount");
    
    /* Check if we need to grow the array */
    if (anim_count >= (jint)anim_tiles->length) {
        /* Create larger array */
        int new_size = anim_tiles->length * 2;
        JavaArray* new_array = jvm_new_array(jvm, T_INT, new_size, NULL);
        if (!new_array) return NATIVE_RETURN_INT(0);
        
        /* Copy old data */
        jint* old_data = (jint*)array_data(anim_tiles);
        jint* new_data = (jint*)array_data(new_array);
        for (int i = 0; i < anim_count; i++) {
            new_data[i] = old_data[i];
        }
        
        /* Replace array */
        set_object_field_ref(this_obj, "animatedTiles", (JavaObject*)new_array);
        anim_tiles = new_array;
    }
    
    /* Store static tile index */
    jint* anim_data = (jint*)array_data(anim_tiles);
    anim_data[anim_count] = static_tile_index;
    
    /* Update count */
    anim_count++;
    set_object_field_int(this_obj, "animatedTileCount", anim_count);
    
    /* Return negative animated tile index (-1 for first, -2 for second, etc.) */
    /* Animated tile index is 1-based, so first animated tile is -1 */
    jint result = -anim_count;
    
    DISP_DEBUG("[TiledLayer] createAnimatedTile: static=%d -> animated=%d",
            static_tile_index, result);
    
    return NATIVE_RETURN_INT(result);
}

/* TiledLayer.setAnimatedTile(int animatedTileIndex, int staticTileIndex) */
static JavaValue native_tiledlayer_setAnimatedTile(JVM* jvm, JavaThread* thread,
                                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    jint anim_tile_index = args[1].i;  /* Negative value */
    jint static_tile_index = args[2].i;
    
    if (!this_obj) return NATIVE_RETURN_VOID();
    
    /* Convert negative index to 0-based array index */
    /* -1 -> 0, -2 -> 1, etc. */
    if (anim_tile_index >= 0) return NATIVE_RETURN_VOID();  /* Invalid index */
    
    int array_index = -anim_tile_index - 1;
    
    /* Get animated tiles array */
    JavaArray* anim_tiles = (JavaArray*)get_object_field_ref(this_obj, "animatedTiles");
    
    if (!anim_tiles || array_index >= (int)anim_tiles->length) {
        return NATIVE_RETURN_VOID();
    }
    
    jint* anim_data = (jint*)array_data(anim_tiles);
    anim_data[array_index] = static_tile_index;
    
    DISP_DEBUG("[TiledLayer] setAnimatedTile: anim=%d -> static=%d",
            anim_tile_index, static_tile_index);
    
    return NATIVE_RETURN_VOID();
}

/* TiledLayer.getAnimatedTile(int animatedTileIndex) */
static JavaValue native_tiledlayer_getAnimatedTile(JVM* jvm, JavaThread* thread,
                                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    jint anim_tile_index = args[1].i;  /* Negative value */
    
    if (!this_obj) return NATIVE_RETURN_INT(0);
    
    /* Convert negative index to 0-based array index */
    if (anim_tile_index >= 0) return NATIVE_RETURN_INT(0);  /* Invalid index */
    
    int array_index = -anim_tile_index - 1;
    
    /* Get animated tiles array */
    JavaArray* anim_tiles = (JavaArray*)get_object_field_ref(this_obj, "animatedTiles");
    
    if (!anim_tiles || array_index >= (int)anim_tiles->length) {
        return NATIVE_RETURN_INT(0);
    }
    
    jint* anim_data = (jint*)array_data(anim_tiles);
    return NATIVE_RETURN_INT(anim_data[array_index]);
}

/* TiledLayer.paint(Graphics g) */
static JavaValue native_tiledlayer_paint(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    JavaObject* gfx_obj = (JavaObject*)args[1].ref;
    
    if (!this_obj || !gfx_obj) return NATIVE_RETURN_VOID();
    
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    if (!gfx) {
        DISP_DEBUG("[TiledLayer] paint: failed to get MidpGraphics");
        return NATIVE_RETURN_VOID();
    }
    
    /* Get TiledLayer properties */
    jint x = get_object_field_int(this_obj, "x");
    jint y = get_object_field_int(this_obj, "y");
    jint columns = get_object_field_int(this_obj, "columns");
    jint rows = get_object_field_int(this_obj, "rows");
    jint tile_width = get_object_field_int(this_obj, "tileWidth");
    jint tile_height = get_object_field_int(this_obj, "tileHeight");
    
    if (columns <= 0 || rows <= 0 || tile_width <= 0 || tile_height <= 0) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Get image */
    JavaObject* image_obj = get_object_field_ref(this_obj, "image");
    if (!image_obj) return NATIVE_RETURN_VOID();
    
    MidpImage* img = (MidpImage*)get_object_field_ref(image_obj, "nativePeer");
    if (!img || !img->pixels) return NATIVE_RETURN_VOID();
    
    /* Get cell map */
    JavaArray* cell_map = (JavaArray*)get_object_field_ref(this_obj, "cellMap");
    
    if (!cell_map || cell_map->element_type != T_INT) {
        return NATIVE_RETURN_VOID();
    }
    
    jint* cells = (jint*)array_data(cell_map);
    
    /* Get animated tiles array for resolving animated tile indices */
    JavaArray* anim_tiles = (JavaArray*)get_object_field_ref(this_obj, "animatedTiles");
    jint* anim_data = anim_tiles ? (jint*)array_data(anim_tiles) : NULL;
    
    /* Calculate tiles per row in source image */
    int tiles_per_row = img->width / tile_width;
    if (tiles_per_row <= 0) tiles_per_row = 1;
    
    /* Paint each cell */
    for (int row = 0; row < rows; row++) {
        for (int col = 0; col < columns; col++) {
            int tile_index = cells[row * columns + col];
            
            if (tile_index != 0) {
                /* Handle animated tiles (negative index) */
                if (tile_index < 0) {
                    /* Convert negative animated index to array index: -1 -> 0, -2 -> 1, etc. */
                    int anim_array_index = -tile_index - 1;
                    if (anim_data && anim_tiles && anim_array_index < (int)anim_tiles->length) {
                        tile_index = anim_data[anim_array_index];
                    } else {
                        tile_index = 0;  /* Invalid animated tile, skip */
                    }
                }
                
                if (tile_index > 0) {
                    /* Tile index is 1-based, convert to 0-based */
                    tile_index--;
                    
                    /* Calculate source position in tile set */
                    int src_col = tile_index % tiles_per_row;
                    int src_row = tile_index / tiles_per_row;
                    int src_x = src_col * tile_width;
                    int src_y = src_row * tile_height;
                    
                    /* Calculate destination position */
                    int dest_x = x + col * tile_width;
                    int dest_y = y + row * tile_height;
                    
                    /* Draw the tile */
                    midp_graphics_draw_region(gfx, img, src_x, src_y, 
                                              tile_width, tile_height,
                                              0, dest_x, dest_y, 0);
                }
            }
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* TiledLayer.getColumns() */
static JavaValue native_tiledlayer_getColumns(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    if (!this_obj) return NATIVE_RETURN_INT(0);
    return NATIVE_RETURN_INT(get_object_field_int(this_obj, "columns"));
}

/* TiledLayer.getRows() */
static JavaValue native_tiledlayer_getRows(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    if (!this_obj) return NATIVE_RETURN_INT(0);
    return NATIVE_RETURN_INT(get_object_field_int(this_obj, "rows"));
}

/* TiledLayer.getCellWidth() */
static JavaValue native_tiledlayer_getCellWidth(JVM* jvm, JavaThread* thread,
                                                 JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    if (!this_obj) return NATIVE_RETURN_INT(0);
    return NATIVE_RETURN_INT(get_object_field_int(this_obj, "tileWidth"));
}

/* TiledLayer.getCellHeight() */
static JavaValue native_tiledlayer_getCellHeight(JVM* jvm, JavaThread* thread,
                                                  JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    if (!this_obj) return NATIVE_RETURN_INT(0);
    return NATIVE_RETURN_INT(get_object_field_int(this_obj, "tileHeight"));
}

/* TiledLayer.setStaticTileSet(Image image, int tileWidth, int tileHeight)
 * v34.76: was NOT REGISTERED at all - the only JSR-118 game-layer method
 * missing from the registry (games swapping tile themes mid-level hit the
 * [INVOKE-MISSING] no-op and kept rendering the OLD tile image).
 *
 * JSR-118 semantics (Oracle javadoc): "Replaces the current static tile
 * set with a new static tile set... If the new static tile set has as
 * many or more tiles than the previous static tile set, the animated
 * tiles and cell contents will be preserved. If not, the contents of the
 * grid will be cleared (all cells will contain index 0) and all animated
 * tiles will be deleted."
 * Throws: NPE if image is null; IllegalArgumentException if tileWidth or
 * tileHeight is less than 1, or if the image width/height is not an
 * integer multiple of tileWidth/tileHeight. The number of rows and
 * columns (and therefore the Layer's width/height) is unchanged. */
static JavaValue native_tiledlayer_setStaticTileSet(JVM* jvm, JavaThread* thread,
                                                     JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    JavaObject* image = (JavaObject*)args[1].ref;
    jint tile_width = args[2].i;
    jint tile_height = args[3].i;

    if (!this_obj) return NATIVE_RETURN_VOID();
    if (!image) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    if (tile_width < 1 || tile_height < 1) {
        native_throw_iae(jvm, thread,
                "tileWidth and tileHeight must be at least 1");
        return NATIVE_RETURN_VOID();
    }

    MidpImage* img = (MidpImage*)get_object_field_ref(image, "nativePeer");
    if (!img || !img->pixels) {
        native_throw_iae(jvm, thread,
                "setStaticTileSet: image has no raster");
        return NATIVE_RETURN_VOID();
    }
    if (img->width % tile_width != 0) {
        native_throw_iae(jvm, thread,
                "image width is not an integer multiple of tileWidth");
        return NATIVE_RETURN_VOID();
    }
    if (img->height % tile_height != 0) {
        native_throw_iae(jvm, thread,
                "image height is not an integer multiple of tileHeight");
        return NATIVE_RETURN_VOID();
    }

    jint columns = get_object_field_int(this_obj, "columns");
    jint rows = get_object_field_int(this_obj, "rows");

    /* Tile count of the OLD set (0 if the old image was unusable). */
    int old_tiles = 0;
    JavaObject* old_image_obj = get_object_field_ref(this_obj, "image");
    if (old_image_obj) {
        MidpImage* old_img = (MidpImage*)get_object_field_ref(old_image_obj, "nativePeer");
        jint old_tw = get_object_field_int(this_obj, "tileWidth");
        jint old_th = get_object_field_int(this_obj, "tileHeight");
        if (old_img && old_tw > 0 && old_th > 0) {
            old_tiles = (old_img->width / old_tw) * (old_img->height / old_th);
        }
    }
    int new_tiles = (img->width / tile_width) * (img->height / tile_height);

    /* Swap the set. Grid size and Layer width/height are unchanged. */
    set_object_field_ref(this_obj, "image", image);
    set_object_field_int(this_obj, "tileWidth", tile_width);
    set_object_field_int(this_obj, "tileHeight", tile_height);

    if (new_tiles >= old_tiles) {
        /* Animated tiles and cell contents are preserved (spec). */
        DISP_DEBUG("[TiledLayer] setStaticTileSet: %d tiles (was %d) - grid and animated tiles preserved",
                new_tiles, old_tiles);
    } else {
        /* Fewer tiles: clear the grid and delete all animated tiles. */
        JavaArray* cell_map = (JavaArray*)get_object_field_ref(this_obj, "cellMap");
        if (cell_map && cell_map->element_type == T_INT &&
            columns > 0 && rows > 0) {
            jint* cells = (jint*)array_data(cell_map);
            int n = columns * rows;
            if (n > (int)cell_map->length) n = (int)cell_map->length;
            for (int i = 0; i < n; i++) cells[i] = 0;
        }
        set_object_field_ref(this_obj, "animatedTiles", (JavaObject*)NULL);
        set_object_field_int(this_obj, "animatedTileCount", 0);
        DISP_DEBUG("[TiledLayer] setStaticTileSet: %d tiles < old %d - grid cleared, animated tiles deleted",
                new_tiles, old_tiles);
    }

    return NATIVE_RETURN_VOID();
}

static JavaValue native_image_createImage_from_bytes(JVM* jvm, JavaThread* thread,
                                                      JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaArray* data = (JavaArray*)args[0].ref;
    jint offset = args[1].i;
    jint length = args[2].i;
    
    if (!data) {
        /* COMPATIBILITY (kept from v34.15): some midlets pass null when
         * resource loading fails — return null rather than NPE. Saboteur 3D
         * never reaches this path: its wrapper NPEs on arraylength (null
         * bytes) before calling createImage. */
        GFX_DEBUG("createImage: WARNING - null data, returning null for compatibility");
        return NATIVE_RETURN_NULL();
    }
    
    uint8_t* img_data = (uint8_t*)array_data(data) + offset;
    
    /* TEMP-DIAG (session 36): trace createImage([BII) calls */
    {
        static int cnt = 0;
        JavaFrame* cf = (thread && thread->current_frame) ? thread->current_frame : NULL;
        if (cnt < 60) {
            cnt++;
            uint32_t magic = length >= 4 ? *(uint32_t*)img_data : 0;
            LOG_SAFE("[CRTIMG] #%d len=%d magic=%08x caller=%s.%s\n", cnt, (int)length, magic,
                     (cf && cf->clazz && cf->clazz->class_name) ? cf->clazz->class_name : "?",
                     (cf && cf->method && cf->method->name) ? cf->method->name : "?");
        }
    }
    
    /* Decode image using stb_image */
    MidpImage* img = midp_image_create_from_data(img_data, 0, length);
    if (!img) {
        /* v34.17 FIX (Saboteur 3D): MIDP 2.0 spec — createImage(byte[],int,int)
         * must throw IllegalArgumentException when the data cannot be decoded.
         * The silent null return broke the standard loader pattern:
         *   try { Image.createImage(raw) } catch { raw = xorDecrypt(raw);
         *   Image.createImage(raw) }
         * used by NET Lizard / Saboteur 3D (and many similar obfuscated J2ME
         * games): the catch never fired, the XOR-decrypted PNG was never
         * retried, and every wall texture stayed null. */
        extern void native_throw_iae(JVM* jvm, JavaThread* thread, const char* message);
        GFX_DEBUG("createImage_from_bytes: decode failed (len=%d) -> IllegalArgumentException\n", length);
        native_throw_iae(jvm, thread, "image data could not be decoded");
        return NATIVE_RETURN_NULL();
    }
    
    /* Create Java Image object */
    JavaClass* image_class = jvm_load_class(jvm, "javax/microedition/lcdui/Image");
    if (!image_class) {
        GFX_DEBUG("createImage_from_bytes: Failed to load Image class");
        midp_image_destroy(img);
        return NATIVE_RETURN_NULL();
    }
    
    /* Ensure class has space for nativePeer */
    ensure_native_peer_field(image_class);
    
    JavaObject* image_obj = jvm_new_object(jvm, image_class);
    if (!image_obj) {
        GFX_DEBUG("createImage_from_bytes: Failed to create Image object");
        midp_image_destroy(img);
        return NATIVE_RETURN_NULL();
    }
    
    set_object_field_ref(image_obj, "nativePeer", (JavaObject*)img);
    
    GFX_DEBUG("createImage_from_bytes: Created Image %dx%d, obj=%p, native=%p", 
            img->width, img->height, (void*)image_obj, (void*)img);
    
    return NATIVE_RETURN_OBJECT(image_obj);
}

/*
 * GameCanvas native methods
 */

/* GameCanvas key state bit masks moved to the top of the file (next to the
 * game_canvas state) so the keyPressed() latch path can use them (#26).
 * v36.01: compute_key_states() (the v35.09 shim over the delivered-event
 * level bits) folded into native_gamecanvas_getKeyStates below — the mask
 * and the atomic latch consume live there now. */

static JavaValue native_gamecanvas_getKeyStates(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    
    /* v36.01: atomic consume of the tap latch (producers = delivery thread,
     * this consumer = a game thread; the old non-atomic read-then-clear could
     * lose a tap that landed between the two statements). HELD bits are NOT
     * cleared here — they clear on keyReleased delivery only (v35.09 level
     * semantics). Result masked to the valid GameCanvas bit set:
     * UP=1<<1 LEFT=1<<2 RIGHT=1<<5 DOWN=1<<6 FIRE=1<<8 A=1<<9 B=1<<10
     * C=1<<11 D=1<<12 => 0x1F66. */
    int latch = __sync_fetch_and_and(&game_canvas.key_latch, 0);
    int result = (latch | game_canvas.key_level) & 0x1F66u;
    return NATIVE_RETURN_INT(result);
}

static JavaValue native_gamecanvas_suppressKeyEvents(JVM* jvm, JavaThread* thread,
                                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jboolean suppress = args[0].i;
    game_canvas.suppress_key_events = suppress != 0;
    DISP_DEBUG("[GameCanvas] suppressKeyEvents(%s) called", suppress ? "true" : "false");
    return NATIVE_RETURN_VOID();
}

/* GameCanvas.getGraphics() - returns Graphics bound to screen framebuffer */
static JavaValue native_gamecanvas_getGraphics(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    (void)jvm; (void)args;
    
    DISP_DEBUG("[GameCanvas] getGraphics() called");
    {
        static volatile int gg_n = 0;
        int n = ++gg_n;
        if (n <= 30 || (n % 500) == 0)
            LOG_SAFE("[GGDIAG] S1 enter\n");
    }
    
    /* Get the SDL context to determine screen size */
    SdlContext* sdl_ctx = sdl_get_global_context();
    if (!sdl_ctx) {
        DISP_DEBUG("[GameCanvas] getGraphics: no SDL context");
        return NATIVE_RETURN_NULL();
    }
    
    /* Determine the size for offscreen buffer */
    int buffer_width = sdl_ctx->width;
    int buffer_height = sdl_ctx->height;
    
    {
        static volatile int gg_n2 = 0;
        int n = ++gg_n2;
        if (n <= 30 || (n % 500) == 0)
            LOG_SAFE("[GGDIAG] S2 after sdl_ctx\n");
    }
    /* Check if we need to create or resize the offscreen buffer */
    if (!game_canvas.offscreen_buffer || 
        game_canvas.offscreen_width != buffer_width || 
        game_canvas.offscreen_height != buffer_height) {
        
        /* Destroy old buffer if exists */
        if (game_canvas.offscreen_buffer) {
            midp_image_destroy(game_canvas.offscreen_buffer);
        }
        
        /* Create new offscreen buffer */
        game_canvas.offscreen_buffer = midp_image_create(buffer_width, buffer_height, true);
        if (!game_canvas.offscreen_buffer) {
            DISP_DEBUG("[GameCanvas] getGraphics: failed to create offscreen buffer");
            return NATIVE_RETURN_NULL();
        }
        game_canvas.offscreen_width = buffer_width;
        game_canvas.offscreen_height = buffer_height;
        
        DISP_DEBUG("[GameCanvas] getGraphics: created offscreen buffer %dx%d, pixels=%p", 
                buffer_width, buffer_height, (void*)game_canvas.offscreen_buffer->pixels);
    }
    
    LOG_SAFE("[GGDIAG] S3 after buffer ensure\n");
    /* FIX (task 16-b #25): getGraphics() used to malloc a fresh MidpGraphics
     * context on every call (leaked, since the Java wrapper only references
     * it). Cache one context per GameCanvas; re-create only when the display
     * size changed. The context is bounded (one GameCanvas is supported) and
     * stays valid for the process lifetime, mirroring real device behavior
     * where the offscreen Graphics is stable. */
    MidpGraphics* gfx = game_canvas.graphics;
    if (gfx && (gfx->width != buffer_width || gfx->height != buffer_height ||
                gfx->pixels != game_canvas.offscreen_buffer->pixels)) {
        midp_graphics_free(gfx); /* v36.12: unregistering free */
        gfx = NULL;
    }
    if (!gfx) {
        gfx = (MidpGraphics*)malloc(sizeof(MidpGraphics));
        if (!gfx) {
            DISP_DEBUG("[GameCanvas] getGraphics: failed to allocate graphics");
            return NATIVE_RETURN_NULL();
        }
        /* v36.12 PEER-REGISTRY: GameCanvas Graphics context is one of the
         * per-session peers the sweep must reclaim (its Java wrapper is
         * gone with the heap; nothing frees this pointer otherwise). */
        midp_graphics_register(gfx);
        game_canvas.graphics = gfx;
    }
    
    midp_graphics_init(gfx, game_canvas.offscreen_buffer->pixels, buffer_width, buffer_height);
    
    /* Reset clip to available area */
    gfx->clip_x = 0;
    gfx->clip_y = 0;
    gfx->clip_width = buffer_width;
    gfx->clip_height = buffer_height;
    gfx->translate_x = 0;
    gfx->translate_y = 0;
    
    /* Limit clip if soft buttons are shown */
    if (!g_full_screen_mode && current_displayable_obj) {
        int commands_idx = find_field_index(current_displayable_obj, "commands");
        if (commands_idx >= 0) {
            JavaArray* commands = (JavaArray*)current_displayable_obj->fields[commands_idx].ref;
            if (commands && is_heap_ptr_check(commands) && ((uintptr_t)commands > 0x10000) && commands->length > 0) {
                gfx->clip_height = buffer_height - SOFT_BUTTON_HEIGHT;
            }
        }
    }
    
    DISP_DEBUG("[GameCanvas] getGraphics: gfx=%p, pixels=%p, %dx%d, clip_height=%d", 
            (void*)gfx, (void*)gfx->pixels, gfx->width, gfx->height, gfx->clip_height);
    
    LOG_SAFE("[GGDIAG] S4 before load_class(Graphics)\n");
    /* Create a Java Graphics object to wrap the native graphics */
    JavaClass* gfx_class = jvm_load_class(jvm, "javax/microedition/lcdui/Graphics");
    if (!gfx_class) {
        DISP_DEBUG("[GameCanvas] getGraphics: failed to load Graphics class");
        midp_graphics_free(gfx); /* v36.12: unregistering free */
        game_canvas.graphics = NULL; /* cache must not dangle */
        return NATIVE_RETURN_NULL();
    }
    
    /* Ensure class has space for nativePeer */
    ensure_native_peer_field(gfx_class);
    
    LOG_SAFE("[GGDIAG] S6 before new_object\n");
    JavaObject* gfx_obj = jvm_new_object(jvm, gfx_class);
    if (!gfx_obj) {
        DISP_DEBUG("[GameCanvas] getGraphics: failed to create Graphics object");
        midp_graphics_free(gfx); /* v36.12: unregistering free */
        game_canvas.graphics = NULL; /* cache must not dangle */
        return NATIVE_RETURN_NULL();
    }
    
    /* Store the MidpGraphics* in the nativePeer field */
    set_object_field_ref(gfx_obj, "nativePeer", (JavaObject*)gfx);
    
    DISP_DEBUG("[GameCanvas] getGraphics: returning Java Graphics obj=%p (native=%p)",
            (void*)gfx_obj, (void*)gfx);
    return NATIVE_RETURN_OBJECT(gfx_obj);
}

/* v36.58 [SNAP]: env-gated canvas dump (NOJME_SNAP_DIR=dir[,[first,]last])
 * — writes the GameCanvas offscreen buffer to dir/frame_N.ppm at
 * flushGraphics, frame numbers first..last (default 0..0 = first frame
 * only; NOJME_SNAP_DIR=dir,ALL = every frame). The "blind menu" companion
 * of NOJME_TEXTLOG: shows EXACTLY what the game drew (pre-presentation),
 * host and device alike. Default off. */
static void snap_maybe_dump_canvas(void) {
    static char s_dir[256] = {0};
    static long s_first = -1, s_last = -1;
    static long s_count = -1;
    static int s_inited = 0;
    if (!s_inited) {
        s_inited = 1;
        const char* e = getenv("NOJME_SNAP_DIR");
        if (e && e[0]) {
            const char* rng = strchr(e, ',');
            size_t dlen = rng ? (size_t)(rng - e) : strlen(e);
            if (dlen >= sizeof(s_dir)) dlen = sizeof(s_dir) - 1;
            memcpy(s_dir, e, dlen);
            s_dir[dlen] = '\0';
            s_first = 0; s_last = 0;
            if (rng) {
                if (!strcmp(rng + 1, "ALL")) { s_first = 0; s_last = 1L << 30; }
                else {
                    long a = -1, b = -1;
                    if (sscanf(rng + 1, "%ld,%ld", &a, &b) == 2) { s_first = a; s_last = b; }
                    else if (sscanf(rng + 1, "%ld", &a) == 1) { s_first = a; s_last = a; }
                }
            }
            s_count = 0;
        }
    }
    if (!s_dir[0] || !game_canvas.offscreen_buffer || !game_canvas.offscreen_buffer->pixels)
        return;
    long n = s_count++;
    if (n < s_first || n > s_last) return;
    char path[320];
    snprintf(path, sizeof(path), "%s/frame_%ld.ppm", s_dir, n);
    FILE* f = fopen(path, "wb");
    if (!f) return;
    int w = game_canvas.offscreen_width, h = game_canvas.offscreen_height;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    /* Canvas pixels are ARGB8888 (uint32); PPM wants RGB bytes. */
    const uint32_t* src = game_canvas.offscreen_buffer->pixels;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t p = src[y * w + x];
            fputc((int)((p >> 16) & 0xFF), f);
            fputc((int)((p >> 8) & 0xFF), f);
            fputc((int)(p & 0xFF), f);
        }
    }
    fclose(f);
}

/* GameCanvas.flushGraphics() - flush the entire buffer to the screen */
static JavaValue native_gamecanvas_flushGraphics(JVM* jvm, JavaThread* thread,
                                                 JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* gamecanvas = (JavaObject*)args[0].ref;

    snap_maybe_dump_canvas();
    race_probe_flush(game_canvas.offscreen_width, game_canvas.offscreen_height,
                     (game_canvas.offscreen_buffer && game_canvas.offscreen_buffer->pixels)
                         ? game_canvas.offscreen_buffer->pixels : NULL);

    DISP_DEBUG("[GameCanvas] flushGraphics() called, offscreen_buffer=%p",
            (void*)game_canvas.offscreen_buffer);
    
    /* If we have an offscreen buffer, copy it to the screen */
    if (game_canvas.offscreen_buffer && game_canvas.offscreen_buffer->pixels) {
        SdlContext* sdl_ctx = sdl_get_global_context();
        if (sdl_ctx && sdl_ctx->framebuffer) {
            uint32_t* dst_pixels = NULL;
            int dst_width = sdl_ctx->width;
            
            MidpGraphics* screen_gfx = sdl_get_graphics(sdl_ctx);
            if (screen_gfx && screen_gfx->pixels) {
                dst_pixels = screen_gfx->pixels;
                dst_width = screen_gfx->width;
            } else if (sdl_ctx->headless || !screen_gfx) {
                /* Headless mode or missing screen graphics: copy directly to framebuffer */
                dst_pixels = sdl_ctx->framebuffer;
                dst_width = sdl_ctx->width;
            }
            
            if (dst_pixels) {
                /* Copy offscreen buffer to screen */
                int copy_width = game_canvas.offscreen_width < dst_width ? 
                                 game_canvas.offscreen_width : dst_width;
                int copy_height = game_canvas.offscreen_height < (sdl_ctx->height) ? 
                                  game_canvas.offscreen_height : (sdl_ctx->height);
                
                DISP_DEBUG("[GameCanvas] flushGraphics: blitting %dx%d from offscreen to screen",
                        copy_width, copy_height);
                
                /* v34.20: blit loop factored into render.c midp_blit_surface() */
                midp_blit_surface(dst_pixels, dst_width, sdl_ctx->height,
                                  game_canvas.offscreen_buffer->pixels,
                                  game_canvas.offscreen_width,
                                  game_canvas.offscreen_height);
            }
        }
    }
    
    /* Render soft buttons (commands) after game rendering */
    if (gamecanvas && !g_full_screen_mode && gamecanvas == current_displayable_obj) {
        render_soft_buttons(jvm, gamecanvas);
    }
    
    /* v34.61: flushGraphics completes a frame OUTSIDE the repaint pump (and
     * takes no UI lock, sets no mid-frame flags - the presenter's gates
     * never knew about it, which is how torn/partial frames leaked to the
     * screen). Snapshot the flushed state as the settled frame. */
    midp_present_settled_snapshot();
    
    /* Request redraw */
    sdl_request_redraw();
    
    return NATIVE_RETURN_VOID();
}

/* GameCanvas.flushGraphics(int x, int y, int w, int h) - flush a region */
static JavaValue native_gamecanvas_flushGraphicsRegion(JVM* jvm, JavaThread* thread,
                                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* gamecanvas = (JavaObject*)args[0].ref;
    jint x = args[1].i;
    jint y = args[2].i;
    jint w = args[3].i;
    jint h = args[4].i;
    
    DISP_DEBUG("[GameCanvas] flushGraphics(%d, %d, %d, %d) called", x, y, w, h);
    
    /* If we have an offscreen buffer, copy the region to the screen */
    if (game_canvas.offscreen_buffer && game_canvas.offscreen_buffer->pixels) {
        SdlContext* sdl_ctx = sdl_get_global_context();
        if (sdl_ctx && sdl_ctx->framebuffer) {
            MidpGraphics* screen_gfx = sdl_get_graphics(sdl_ctx);
            if (screen_gfx && screen_gfx->pixels) {
                /* Clip region to valid bounds */
                if (x < 0) { w += x; x = 0; }
                if (y < 0) { h += y; y = 0; }
                if (x + w > game_canvas.offscreen_width) w = game_canvas.offscreen_width - x;
                if (y + h > game_canvas.offscreen_height) h = game_canvas.offscreen_height - y;
                if (x + w > screen_gfx->width) w = screen_gfx->width - x;
                if (y + h > screen_gfx->height) h = screen_gfx->height - y;
                
                if (w > 0 && h > 0) {
                    /* v34.20: blit loop factored into render.c
                     * midp_blit_surface_region() */
                    midp_blit_surface_region(screen_gfx->pixels,
                                             screen_gfx->width, screen_gfx->height,
                                             game_canvas.offscreen_buffer->pixels,
                                             game_canvas.offscreen_width,
                                             game_canvas.offscreen_height,
                                             x, y, w, h);
                }
            }
        }
    }
    
    /* Render soft buttons */
    if (gamecanvas && !g_full_screen_mode && gamecanvas == current_displayable_obj) {
        render_soft_buttons(jvm, gamecanvas);
    }
    
    /* v34.61: same as flushGraphics() - region flush completes a frame
     * outside the pump; snapshot it as the settled state. */
    midp_present_settled_snapshot();
    
    /* Request redraw */
    sdl_request_redraw();
    
    return NATIVE_RETURN_VOID();
}

/*
 * Image native methods
 */

/* Helper to get MidpImage from Java Image object */
MidpImage* get_image_from_object(JavaObject* obj) {
    if (!obj) {
        return NULL;
    }
    
    if (!obj->header.clazz) {
        return NULL;
    }
    
    /* The MidpImage* is stored in the nativePeer field */
    return (MidpImage*)get_object_field_ref(obj, "nativePeer");
}

/* Ensure a class has at least one field for storing native pointer */
void ensure_native_peer_field(JavaClass* clazz) {
    if (!clazz) return;
    
    /* Minimum size needed: ObjectHeader + 1 field (JavaValue)
     * Note: nativePeer is stored as a reference (1 slot) on our heap,
     * even though Java 'long' would normally take 2 slots. */
    size_t min_size = sizeof(ObjectHeader) + sizeof(JavaValue);
    
    /* 1. Check if "nativePeer" field specifically exists */
    bool has_native_peer = false;
    if (clazz->fields_count > 0 && clazz->fields) {
        for (int i = 0; i < clazz->fields_count; i++) {
            if (clazz->fields[i].name && strcmp(clazz->fields[i].name, "nativePeer") == 0) {
                has_native_peer = true;
                break;
            }
        }
    }
    
    /* 2. If nativePeer already exists and size is sufficient, nothing to do */
    if (has_native_peer && clazz->instance_size >= min_size) {
        return;
    }
    
    /* 3. If no fields at all, add nativePeer */
    if (clazz->fields_count == 0) {
        JavaField* new_fields = (JavaField*)calloc(1, sizeof(JavaField));
        if (!new_fields) return; /* OOM */
        
        clazz->fields = new_fields;
        clazz->fields_count = 1;
        
        clazz->fields[0].name = strdup("nativePeer");
        clazz->fields[0].descriptor = strdup("Ljava/lang/Object;");
        clazz->fields[0].access_flags = ACC_PRIVATE;
    }
    /* 4. If fields exist but no nativePeer, add it (reallocate fields array) */
    else if (!has_native_peer) {
        int old_count = clazz->fields_count;
        int new_count = old_count + 1;
        
        JavaField* new_fields = (JavaField*)realloc(clazz->fields, new_count * sizeof(JavaField));
        if (!new_fields) return; /* OOM */
        
        clazz->fields = new_fields;
        clazz->fields_count = new_count;
        
        /* Initialize the new field */
        memset(&clazz->fields[old_count], 0, sizeof(JavaField));
        clazz->fields[old_count].name = strdup("nativePeer");
        clazz->fields[old_count].descriptor = strdup("Ljava/lang/Object;");
        clazz->fields[old_count].access_flags = ACC_PRIVATE;
    }
    
    /* 5. Update instance_size if needed */
    size_t computed_size = sizeof(ObjectHeader) + clazz->fields_count * sizeof(JavaValue);
    if (computed_size > clazz->instance_size) {
        clazz->instance_size = computed_size;
    }
}

static JavaValue native_image_createImage(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)arg_count;
    jint width = args[0].i;
    jint height = args[1].i;
    
    GFX_DEBUG("createImage(%d, %d)", width, height);

    /* v34.77 FIX (Asphalt 4 / 3D FnF): MIDP spec — non-positive dimensions
     * are IllegalArgumentException (catchable), NOT OutOfMemoryError. The
     * old OOM mapping escaped games' catch(Exception) blocks and froze
     * their paint reentrancy guards. */
    if (width <= 0 || height <= 0) {
        native_throw_iae(jvm, thread, (width <= 0)
            ? "width must be greater than 0"
            : "height must be greater than 0");
        return NATIVE_RETURN_NULL();
    }
    
    /* Create the native image */
    MidpImage* img = midp_image_create(width, height, true);
    if (!img) {
        native_throw_oome(jvm, thread);
        return NATIVE_RETURN_NULL();
    }
    
    /* Diagnostic: track image creation */
    DISP_DEBUG("[createImage] Created mutable image: %dx%d, native=%p, pixels=%p",
            width, height, (void*)img, (void*)img->pixels);
    
    
    /* Create a Java Image object to wrap it */
    JavaClass* image_class = jvm_load_class(jvm, "javax/microedition/lcdui/Image");
    if (!image_class) {
        GFX_DEBUG("Failed to load Image class");
        midp_image_destroy(img);
        return NATIVE_RETURN_NULL();
    }
    
    /* Ensure class has space for nativePeer */
    ensure_native_peer_field(image_class);
    
    JavaObject* image_obj = jvm_new_object(jvm, image_class);
    if (!image_obj) {
        GFX_DEBUG("Failed to create Image object");
        midp_image_destroy(img);
        return NATIVE_RETURN_NULL();
    }
    
    /* Store the MidpImage* in the nativePeer field */
    set_object_field_ref(image_obj, "nativePeer", (JavaObject*)img);
    
    DISP_DEBUG("[createImage] Java Image obj=%p -> native=%p", (void*)image_obj, (void*)img);
    

    return NATIVE_RETURN_OBJECT(image_obj);
}

/* Cache for missing resources to avoid repeated failed lookups */
#define MISSING_CACHE_SIZE 64
static char* g_missing_cache[MISSING_CACHE_SIZE];
static int g_missing_cache_count = 0;

/* v34.71: JAR image access for the entry-existence probe. Both builds keep
 * the archive in different storage: the app/headless in main.c statics, the
 * libretro core on the JVM's class_loader. NULL when unavailable (probe
 * then conservatively reports "not found" -> old blacklist behavior). */
static const uint8_t* display_jar_image(size_t* out_size) {
    if (out_size) *out_size = 0;
#ifdef LIBRETRO
    extern JVM* g_jvm;   /* libretro.c (non-static) */
    if (g_jvm && g_jvm->class_loader.jar_data && g_jvm->class_loader.jar_size) {
        *out_size = g_jvm->class_loader.jar_size;
        return g_jvm->class_loader.jar_data;
    }
    return NULL;
#else
    extern const uint8_t* get_jar_data(size_t* size);   /* main.c */
    return get_jar_data(out_size);
#endif
}

/* Check if resource is in missing cache */
static bool is_in_missing_cache(const char* name) {
    for (int i = 0; i < g_missing_cache_count; i++) {
        if (g_missing_cache[i] && strcmp(g_missing_cache[i], name) == 0) {
            return true;
        }
    }
    return false;
}

/* Add to missing cache */
static void add_to_missing_cache(const char* name) {
    if (g_missing_cache_count >= MISSING_CACHE_SIZE) {
        /* Free oldest entry */
        free(g_missing_cache[0]);
        for (int i = 0; i < MISSING_CACHE_SIZE - 1; i++) {
            g_missing_cache[i] = g_missing_cache[i + 1];
        }
        g_missing_cache_count--;
    }
    g_missing_cache[g_missing_cache_count++] = strdup(name);
}

/* Create a placeholder image for missing resources */
static JavaObject* create_placeholder_image(JVM* jvm) {
    /* Create a small 8x8 checkerboard placeholder */
    int w = 8, h = 8;
    MidpImage* img = midp_image_create(w, h, true);
    if (!img) return NULL;
    
    /* Fill with checkerboard pattern (magenta/black) */
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t color = ((x + y) % 2 == 0) ? 0xFFFF00FF : 0xFF000000; /* Magenta/Black */
            img->pixels[y * w + x] = color;
        }
    }
    
    /* Create Java Image object */
    JavaClass* image_class = jvm_load_class(jvm, "javax/microedition/lcdui/Image");
    if (!image_class) {
        midp_image_destroy(img);
        return NULL;
    }
    
    ensure_native_peer_field(image_class);
    JavaObject* image_obj = jvm_new_object(jvm, image_class);
    if (!image_obj) {
        midp_image_destroy(img);
        return NULL;
    }
    
    set_object_field_ref(image_obj, "nativePeer", (JavaObject*)img);
    return image_obj;
}

/* Image.createImage(String) - load image from JAR resource */
static JavaValue native_image_createImage_from_string(JVM* jvm, JavaThread* thread,
                                                       JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaString* name_str = (JavaString*)args[0].ref;
    
    if (!name_str) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_NULL();
    }
    
    /* Get the resource name */
    const char* name = string_utf8(jvm, name_str);
    if (!name) {
        return NATIVE_RETURN_NULL();
    }
    
    /* Skip leading slash if present (JAR resources don't have leading slash) */
    const char* resource_name = name;
    if (resource_name[0] == '/') {
        resource_name++;
    }
    
    /* Check missing cache first to avoid repeated failed lookups */
    if (is_in_missing_cache(resource_name)) {
        /* Return placeholder silently without logging */
        JavaObject* placeholder = create_placeholder_image(jvm);
        if (placeholder) {
            return NATIVE_RETURN_OBJECT(placeholder);
        }
        return NATIVE_RETURN_NULL();
    }
    
    /* Load the resource from JAR */
    size_t data_size;
    uint8_t* data = load_jar_resource(resource_name, &data_size);

    /* FALLBACK: If not found and name ends with .jpg, try without .jpg extension */
    if (!data) {
        size_t name_len = strlen(resource_name);
        if (name_len > 4 && strcmp(resource_name + name_len - 4, ".jpg") == 0) {
            char* try_name = strdup(resource_name);
            if (try_name) {
                try_name[name_len - 4] = '\0';
                data = load_jar_resource(try_name, &data_size);
                free(try_name);
            }
        }
    }
    
    /* FALLBACK 2: Try with .png extension */
    if (!data) {
        size_t name_len = strlen(resource_name);
        char* try_name = malloc(name_len + 5);
        if (try_name) {
            strcpy(try_name, resource_name);
            if (name_len > 4 && strcmp(try_name + name_len - 4, ".jpg") == 0) {
                strcpy(try_name + name_len - 4, ".png");
            } else {
                strcat(try_name, ".png");
            }
            data = load_jar_resource(try_name, &data_size);
            free(try_name);
        }
    }

    if (!data) {
        /* Log once and add to missing cache */
        DISP_DEBUG("[Image] Resource not found: %s (throwing IOException)", resource_name);
        /* FIX-19x: unconditional throttled log - missing images are the
         * single most visible class of game bugs (magenta placeholders) */
        {
            static int missing_img_count = 0;
            if (missing_img_count < 300) {
                missing_img_count++;
                MISSING_LOG("[IMG-MISSING] '%s'\n", resource_name);
            }
        }
        /* v34.71 CRITICAL FIX (Stalker "текстуры пропадают через какое-то
         * время"): blacklist ONLY resources that are genuinely ABSENT from
         * the JAR. jar_read_file() also returns NULL on TRANSIENT failures
         * (inflate OOM under memory pressure, realloc fail) — the old code
         * poisoned the missing-cache with those, and every later
         * createImage() of a PERFECTLY PRESENT resource (texobj1.png!)
         * silently returned the magenta placeholder. With the fixed M3G
         * texture-cache leak this is defense in depth, but transient
         * failures must NEVER become permanent. */
        {
            size_t jar_sz = 0;
            const uint8_t* jar_img = display_jar_image(&jar_sz);
            if (jar_img && jar_sz &&
                (jar_has_entry(jar_img, jar_sz, resource_name) ||
                 jar_has_entry(jar_img, jar_sz, name))) {
                DISP_DEBUG("[Image] '%s' EXISTS in JAR but extraction failed "
                           "(transient) — NOT blacklisted, will retry", resource_name);
            } else {
                add_to_missing_cache(resource_name);
            }
        }
        
        /* FIX-19z: MIDP spec - createImage(String) throws IOException when
         * the resource cannot be found. Returning a magenta placeholder here
         * BROKE games whose loaders do
         *   try { img = Image.createImage("res/"+name); }
         *   catch (IOException e) { img = cutFromAtlas(name); }
         * - the catch never fired, so every sprite missing from res/ was
         * drawn as an 8x8 magenta checkerboard (SU-30 interface bars). */
        jvm_throw_by_name(jvm, "java/io/IOException", resource_name);
        if (thread) thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_NULL();
    }

    /* Decode PNG */
    MidpImage* img = midp_image_create_from_data(data, 0, data_size);
    free(data);

    if (!img) {
        DISP_DEBUG("[Image] Failed to decode: %s (throwing IOException)", resource_name);
        /* v34.71: decode failure is (almost always) TRANSIENT — OOM for the
         * pixel buffer under memory pressure. Do NOT blacklist: the next
         * attempt (after the GC / the texture-cache fix frees memory) will
         * succeed. Blacklisting here produced the permanent "textures
         * disappeared" state from ONE unlucky failure. */
        
        /* FIX-19z: decode failure is also an IOException per spec */
        jvm_throw_by_name(jvm, "java/io/IOException", resource_name);
        if (thread) thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_NULL();
    }
    
    /* Create a Java Image object to wrap it */
    JavaClass* image_class = jvm_load_class(jvm, "javax/microedition/lcdui/Image");
    if (!image_class) {
        midp_image_destroy(img);
        return NATIVE_RETURN_NULL();
    }
    
    /* Ensure class has space for nativePeer */
    ensure_native_peer_field(image_class);
    
    JavaObject* image_obj = jvm_new_object(jvm, image_class);
    if (!image_obj) {
        midp_image_destroy(img);
        return NATIVE_RETURN_NULL();
    }
    
    /* Store the MidpImage* in the nativePeer field */
    set_object_field_ref(image_obj, "nativePeer", (JavaObject*)img);
    
    GFX_DEBUG("Created Image %dx%d from %s", img->width, img->height, name);
    
    return NATIVE_RETURN_OBJECT(image_obj);
}

static JavaValue native_image_createRGBImage(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)arg_count;
    JavaArray* rgb_array = (JavaArray*)args[0].ref;
    jint width = args[1].i;
    jint height = args[2].i;
    jboolean process_alpha = args[3].i;
    
    if (!rgb_array) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_NULL();
    }

    /* v34.77 FIX (Asphalt 4 freeze): MIDP spec says non-positive width/
     * height (or an rgb array shorter than w*h) is IllegalArgumentException
     * — a CATCHABLE Exception. The old code funneled every failure into
     * OutOfMemoryError (an ERROR): games catching only Exception (GLLib's
     * e.a paint wrapper does) let it escape paint(), their reentrancy
     * guard stayed set and every later paint() returned instantly ->
     * permanent black/loading freeze. Also check the source array length
     * before midp_image_create_from_rgb reads it. */
    if (width <= 0 || height <= 0) {
        native_throw_iae(jvm, thread, (width <= 0)
            ? "width must be greater than 0"
            : "height must be greater than 0");
        return NATIVE_RETURN_NULL();
    }
    if ((jlong)width * (jlong)height > (jlong)rgb_array->length) {
        native_throw_aioobe(jvm, thread, (jint)(rgb_array->length - 1));
        return NATIVE_RETURN_NULL();
    }
    
    jint* rgb = (jint*)array_data(rgb_array);
    MidpImage* img = midp_image_create_from_rgb(rgb, width, height, process_alpha != 0);
    
    if (!img) {
        native_throw_oome(jvm, thread);
        return NATIVE_RETURN_NULL();
    }
    
    /* Create a Java Image object to wrap it */
    JavaClass* image_class = jvm_load_class(jvm, "javax/microedition/lcdui/Image");
    if (!image_class) {
        midp_image_destroy(img);
        return NATIVE_RETURN_NULL();
    }
    
    /* Ensure class has space for nativePeer */
    ensure_native_peer_field(image_class);
    
    JavaObject* image_obj = jvm_new_object(jvm, image_class);
    if (!image_obj) {
        midp_image_destroy(img);
        return NATIVE_RETURN_NULL();
    }
    
    /* Store the MidpImage* in the nativePeer field */
    set_object_field_ref(image_obj, "nativePeer", (JavaObject*)img);
    
    return NATIVE_RETURN_OBJECT(image_obj);
}

static JavaValue native_image_getWidth(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    MidpImage* img = get_image_from_object(obj);
    return NATIVE_RETURN_INT(img ? img->width : 0);
}

static JavaValue native_image_getHeight(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    MidpImage* img = get_image_from_object(obj);
    return NATIVE_RETURN_INT(img ? img->height : 0);
}

static JavaValue native_image_isMutable(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    MidpImage* img = get_image_from_object(obj);
    return NATIVE_RETURN_INT(img && img->mutable ? 1 : 0);
}

static JavaValue native_image_getRGB(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    MidpImage* img = get_image_from_object(obj);
    JavaArray* rgb_data = (JavaArray*)args[1].ref;
    jint offset = args[2].i;
    jint scanlength = args[3].i;
    jint x = args[4].i;
    jint y = args[5].i;
    jint width = args[6].i;
    jint height = args[7].i;
    
    if (!img || !rgb_data) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }

    /* v34.77 FIX (3D FnF The Movie heap smash -> SIGSEGV): getRGB used to
     * write past the end of the destination array whenever offset/
     * scanlength/width/height described more pixels than the array could
     * hold. RGBA pixel data then overwrote the GC headers of adjacent heap
     * blocks (sweep found magic 0xDEADBEEF replaced by pixel bytes), the
     * corrupted object graph later jumped through a garbage class pointer
     * (SIGSEGV at a pixel-valued PC). MIDP spec: throw
     * ArrayIndexOutOfBoundsException, write NOTHING. */
    if (width > 0 && height > 0) {
        jlong row_last = (jlong)offset + (jlong)(height - 1) * (jlong)scanlength;
        jlong lo = (jlong)offset;
        jlong hi = (jlong)offset + (jlong)width - 1;
        if (row_last < lo) lo = row_last;
        if (row_last + (jlong)width - 1 > hi) hi = row_last + (jlong)width - 1;
        if (lo < 0 || hi >= (jlong)rgb_data->length) {
            static int getrgb_diag = 0;
            if (getrgb_diag < 5) {
                getrgb_diag++;
                fprintf(stderr, "[GETRGB-AIOOBE] dst_len=%d offset=%d scanlen=%d "
                        "rect %dx%d @ (%d,%d) needs [%lld..%lld]\n",
                        (int)rgb_data->length, (int)offset, (int)scanlength,
                        (int)width, (int)height, (int)x, (int)y,
                        (long long)lo, (long long)hi);
                fflush(stderr);
            }
            native_throw_aioobe(jvm, thread, (jint)(lo < 0 ? lo : hi));
            return NATIVE_RETURN_VOID();
        }
    }
    
    /* FIX-19q DIAG: which buffer does getRGB read (throttled)? */
    {
        static int rgbdiag = 0;
        if (rgbdiag < 8) {
            rgbdiag++;
            fprintf(stderr, "[RGBDIAG] img=%p pixels=%p %dx%d first_px=0x%08X\n",
                    (void*)img, (void*)img->pixels, img->width, img->height,
                    (img->pixels) ? img->pixels[0] : 0);
        }
    }
    
    midp_image_get_rgb(img, (jint*)array_data(rgb_data), offset, scanlength, x, y, width, height);
    
    return NATIVE_RETURN_VOID();
}

/* Image.createImage(Image source, int x, int y, int width, int height, int transform)
 * CRITICAL: Creates a sub-image with optional transformation
 */
static JavaValue native_image_createImage_region(JVM* jvm, JavaThread* thread,
                                                 JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* src_image_obj = (JavaObject*)args[0].ref;
    jint x = args[1].i;
    jint y = args[2].i;
    jint width = args[3].i;
    jint height = args[4].i;
    jint transform = args[5].i;
    
    if (!src_image_obj) {
        return NATIVE_RETURN_NULL();
    }
    
    MidpImage* src_img = get_image_from_object(src_image_obj);
    if (!src_img) {
        GFX_DEBUG("createImage_region: source image has no native data");
        return NATIVE_RETURN_NULL();
    }
    
    /* Bounds check */
    if (x < 0 || y < 0 || width <= 0 || height <= 0) {
        return NATIVE_RETURN_NULL();
    }
    if (x + width > src_img->width || y + height > src_img->height) {
        GFX_DEBUG("createImage_region: region out of bounds");
        return NATIVE_RETURN_NULL();
    }
    
    /* Calculate output dimensions based on transform */
    int dest_w = width, dest_h = height;
    if (transform == SPRITE_TRANS_ROT90 || transform == SPRITE_TRANS_ROT270 ||
        transform == SPRITE_TRANS_MIRROR_ROT90 || transform == SPRITE_TRANS_MIRROR_ROT270) {
        dest_w = height;
        dest_h = width;
    }
    
    /* Allocate new image */
    MidpImage* new_img = (MidpImage*)calloc(1, sizeof(MidpImage));
    if (!new_img) return NATIVE_RETURN_NULL();
    
    new_img->width = dest_w;
    new_img->height = dest_h;
    new_img->mutable = false;
    new_img->alpha = src_img->alpha;
    /* v34.51 PINK-FIX: pixels are copied verbatim from src (crop/flip/rot
     * never changes any alpha byte), so src's EXACT opacity knowledge is
     * still true for the snapshot. When src is unscanned (mutable), the
     * new image simply stays lazily-scanned. */
    new_img->alpha_scan_valid = src_img->alpha_scan_valid;
    new_img->alpha_all_opaque = src_img->alpha_all_opaque;
    new_img->pixels = (uint32_t*)malloc(dest_w * dest_h * sizeof(uint32_t));
    if (!new_img->pixels) {
        free(new_img);
        return NATIVE_RETURN_NULL();
    }
    
    /* Copy pixels with transformation
     * ИСПРАВЛЕНО: Формулы пересчитаны по спецификации MIDP2
     * См. комментарии в graphics.c midp_graphics_draw_region()
     */
    for (int dy = 0; dy < height; dy++) {
        for (int dx = 0; dx < width; dx++) {
            int src_idx = (y + dy) * src_img->width + (x + dx);
            uint32_t pixel = src_img->pixels[src_idx];
            
            int dest_x, dest_y;
            switch (transform) {
                case SPRITE_TRANS_NONE:           /* 0 */
                    dest_x = dx; dest_y = dy; 
                    break;
                case SPRITE_TRANS_MIRROR_ROT180:  /* 1 - вертикальный flip */
                    dest_x = dx; dest_y = height - 1 - dy; 
                    break;
                case SPRITE_TRANS_MIRROR:         /* 2 - горизонтальный flip */
                    dest_x = width - 1 - dx; dest_y = dy; 
                    break;
                case SPRITE_TRANS_ROT180:         /* 3 */
                    dest_x = width - 1 - dx; dest_y = height - 1 - dy; 
                    break;
                case SPRITE_TRANS_MIRROR_ROT270:  /* 4 - transpose */
                    dest_x = dy; dest_y = dx; 
                    break;
                case SPRITE_TRANS_ROT90:          /* 5 */
                    dest_x = height - 1 - dy; dest_y = dx; 
                    break;
                case SPRITE_TRANS_ROT270:         /* 6 */
                    dest_x = dy; dest_y = width - 1 - dx; 
                    break;
                case SPRITE_TRANS_MIRROR_ROT90:   /* 7 */
                    dest_x = height - 1 - dy; dest_y = width - 1 - dx; 
                    break;
                default:
                    dest_x = dx; dest_y = dy; 
                    break;
            }
            
            int dest_idx = dest_y * dest_w + dest_x;
            new_img->pixels[dest_idx] = pixel;
        }
    }
    
    /* Create Java Image object */
    JavaClass* image_class = jvm_load_class(jvm, "javax/microedition/lcdui/Image");
    if (!image_class) {
        free(new_img->pixels);
        free(new_img);
        return NATIVE_RETURN_NULL();
    }
    
    ensure_native_peer_field(image_class);
    JavaObject* image_obj = jvm_new_object(jvm, image_class);
    if (!image_obj) {
        free(new_img->pixels);
        free(new_img);
        return NATIVE_RETURN_NULL();
    }
    
    set_object_field_ref(image_obj, "nativePeer", (JavaObject*)new_img);
    
    GFX_DEBUG("Created region image: %dx%d (transform=%d)", dest_w, dest_h, transform);
    return NATIVE_RETURN_OBJECT(image_obj);
}

/* Image.createImage(Image source) - creates a copy */
static JavaValue native_image_createImage_copy(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* src_image_obj = (JavaObject*)args[0].ref;
    
    if (!src_image_obj) {
        return NATIVE_RETURN_NULL();
    }
    
    MidpImage* src_img = get_image_from_object(src_image_obj);
    if (!src_img) {
        return NATIVE_RETURN_NULL();
    }
    
    /* Create copy with transform=NONE */
    JavaValue region_args[6];
    region_args[0].ref = src_image_obj;
    region_args[1].i = 0;
    region_args[2].i = 0;
    region_args[3].i = src_img->width;
    region_args[4].i = src_img->height;
    region_args[5].i = SPRITE_TRANS_NONE;
    
    return native_image_createImage_region(jvm, thread, region_args, 6);
}

/* Image.createImage(InputStream stream) - load from stream
 * 
 * CRITICAL FIX: Previously this was a stub that returned a 1x1 placeholder image.
 * Now it properly reads all bytes from the InputStream and decodes the image.
 */
static JavaValue native_image_createImage_stream(JVM* jvm, JavaThread* thread,
                                                 JavaValue* args, int arg_count) {
    (void)arg_count;
    JavaObject* stream_obj = (JavaObject*)args[0].ref;
    
    if (!stream_obj) {
        GFX_DEBUG("createImage(InputStream): stream is NULL");
        return NATIVE_RETURN_NULL();
    }
    
    GFX_DEBUG("createImage(InputStream): reading from stream...");
    
    /* Get the stream's class */
    JavaClass* stream_class = stream_obj->header.clazz;
    if (!stream_class) {
        GFX_DEBUG("createImage(InputStream): stream has no class");
        return NATIVE_RETURN_NULL();
    }
    
    /* Find the read([BII)I method on the stream */
    JavaMethod* read_method = jvm_resolve_method(jvm, stream_class, "read", "([BII)I");
    bool has_read_3arg = (read_method != NULL);
    
    if (!read_method) {
        /* Fallback: try read([B)I */
        read_method = jvm_resolve_method(jvm, stream_class, "read", "([B)I");
        if (!read_method) {
            GFX_DEBUG("createImage(InputStream): no read method found on %s", 
                    stream_class->class_name ? stream_class->class_name : "?");
            return NATIVE_RETURN_NULL();
        }
    }
    
    /* Allocate a dynamic buffer to hold all image data */
    int buffer_capacity = 4096;
    int buffer_size = 0;
    uint8_t* image_data = (uint8_t*)malloc(buffer_capacity);
    if (!image_data) {
        GFX_DEBUG("createImage(InputStream): failed to allocate buffer");
        return NATIVE_RETURN_NULL();
    }
    
    /* Create a temporary byte array for reading chunks */
    JavaArray* chunk_array = jvm_new_array(jvm, T_BYTE, 4096, NULL);
    if (!chunk_array) {
        free(image_data);
        GFX_DEBUG("createImage(InputStream): failed to create chunk array");
        return NATIVE_RETURN_NULL();
    }
    
    /* Read all bytes from the stream */
    while (1) {
        /* (v34.84: 'this' is supplied by jvm_invoke_virtual itself — the
         * args below exclude it; read_args[] kept only for the 2-arg
         * compatibility comment. See the FIX note at the invoke.) */
        JavaValue read_result;
        memset(&read_result, 0, sizeof(read_result));
        int ret;
        
        /* v34.84 FIX (C&C4 Tiberian Twilight emulator crash — heap
         * corruption): route the re-entrant read() through
         * jvm_invoke_virtual's deposit-harvest contract instead of a raw
         * execute_method. Two defects when the stream's read() is real
         * BYTECODE (C&C4's custom java-level stream class):
         *   1) execute_method never fills the out result for bytecode
         *      methods (op_ireturn deposits into frame->prev instead), so
         *      bytes_read was UNINITIALIZED stack garbage;
         *   2) op_ireturn did prev->stack_top++ WITHOUT popping anything
         *      from the caller — a phantom operand accumulated PER CHUNK
         *      READ. The native dispatcher had already popped the
         *      createImage() args, and would push its own return value on
         *      top later: every read() iteration inflated the game frame's
         *      stack_top by one slot until it wrote PAST max_stack (ASAN:
         *      heap-buffer-overflow WRITE of 8 in op_ireturn, 0 bytes after
         *      a 56-byte frame) — corrupting the malloc heap and crashing
         *      the emulator minutes later inside libc malloc (SIGSEGV).
         * jvm_invoke_virtual snapshots the caller's stack_top, harvests the
         * deposited result into *result and RESTORES the depth — the
         * documented C-entry contract (see execute.c comment).
         * Native-stub streams (ByteArrayInputStream etc.) are unaffected:
         * the wrapper's registry fallback dispatches native_call the same
         * way. */
        if (has_read_3arg) {
            JavaValue vargs[3];
            vargs[0].ref = (JavaObject*)chunk_array;  /* buffer */
            vargs[1].i = 0;                            /* offset */
            vargs[2].i = 4096;                         /* length */
            ret = jvm_invoke_virtual(jvm, stream_obj, "read", "([BII)I",
                                     vargs, &read_result);
        } else {
            /* read([B)I only takes the buffer */
            JavaValue vargs[1];
            vargs[0].ref = (JavaObject*)chunk_array;
            ret = jvm_invoke_virtual(jvm, stream_obj, "read", "([B)I",
                                     vargs, &read_result);
        }
        
        if (ret != 0 || thread->pending_exception) {
            /* Method execution failed or exception thrown */
            GFX_DEBUG("createImage(InputStream): read() failed or threw exception");
            break;
        }
        
        jint bytes_read = read_result.i;
        
        if (bytes_read <= 0) {
            /* End of stream (-1) or no data (0) */
            break;
        }
        
        /* Grow buffer if needed */
        if (buffer_size + bytes_read > buffer_capacity) {
            buffer_capacity = buffer_capacity * 2 + bytes_read;
            uint8_t* new_data = (uint8_t*)realloc(image_data, buffer_capacity);
            if (!new_data) {
                free(image_data);
                GFX_DEBUG("createImage(InputStream): failed to grow buffer");
                return NATIVE_RETURN_NULL();
            }
            image_data = new_data;
        }
        
        /* Copy chunk to our buffer */
        uint8_t* chunk_data = (uint8_t*)array_data(chunk_array);
        memcpy(image_data + buffer_size, chunk_data, bytes_read);
        buffer_size += bytes_read;
        
        /* Safety limit: 16MB max */
        if (buffer_size > 16 * 1024 * 1024) {
            GFX_DEBUG("createImage(InputStream): image too large (>16MB)");
            free(image_data);
            return NATIVE_RETURN_NULL();
        }
    }
    
    GFX_DEBUG("createImage(InputStream): read %d bytes", buffer_size);
    
    /* Check if we got any data */
    if (buffer_size < 8) {
        GFX_DEBUG("createImage(InputStream): not enough data (%d bytes)", buffer_size);
        free(image_data);
        return NATIVE_RETURN_NULL();
    }
    
    /* Decode the image */
    MidpImage* img = midp_image_create_from_data(image_data, 0, buffer_size);
    free(image_data);
    
    if (!img) {
        GFX_DEBUG("createImage(InputStream): failed to decode image");
        return NATIVE_RETURN_NULL();
    }
    
    /* Create Java Image object */
    JavaClass* image_class = jvm_load_class(jvm, "javax/microedition/lcdui/Image");
    if (!image_class) {
        GFX_DEBUG("createImage(InputStream): failed to load Image class");
        midp_image_destroy(img);
        return NATIVE_RETURN_NULL();
    }
    
    ensure_native_peer_field(image_class);
    JavaObject* image_obj = jvm_new_object(jvm, image_class);
    if (!image_obj) {
        GFX_DEBUG("createImage(InputStream): failed to create Image object");
        midp_image_destroy(img);
        return NATIVE_RETURN_NULL();
    }
    
    set_object_field_ref(image_obj, "nativePeer", (JavaObject*)img);
    
    GFX_DEBUG("createImage(InputStream): SUCCESS - created %dx%d image", img->width, img->height);
    
    return NATIVE_RETURN_OBJECT(image_obj);
}

/* Image.getGraphics() - returns Graphics for mutable images */
static JavaValue native_image_getGraphics(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* image_obj = (JavaObject*)args[0].ref;
    
    /* ALWAYS log getGraphics calls */
    DISP_DEBUG("[getGraphics] CALLED on Image obj=%p", (void*)image_obj);
    
    
    if (!image_obj) {
        DISP_DEBUG("[getGraphics] ERROR: null image object, throwing NPE");
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_NULL();
    }
    
    MidpImage* img = get_image_from_object(image_obj);
    if (!img) {
        DISP_DEBUG("[getGraphics] ERROR: no native image for obj=%p", (void*)image_obj);
        return NATIVE_RETURN_NULL();
    }
    
    DISP_DEBUG("[getGraphics] native img=%p (%dx%d, mutable=%d, pixels=%p)",
            (void*)img, img->width, img->height, img->mutable, (void*)img->pixels);
    
    if (!img->mutable) {
        DISP_DEBUG("[getGraphics] ERROR: image is not mutable");
        /* IllegalStateException for immutable image */
        jvm_throw_by_name(jvm, "java/lang/IllegalStateException", "Image is not mutable");
        return NATIVE_RETURN_NULL();
    }
    
    /* Create a Graphics object for this image */
    MidpGraphics* gfx = midp_image_get_graphics(img);
    if (!gfx) {
        DISP_DEBUG("[getGraphics] ERROR: failed to create graphics context");
        return NATIVE_RETURN_NULL();
    }
    
    /* Diagnostic: log the image-graphics binding */
    DISP_DEBUG("[getGraphics] SUCCESS: Created gfx=%p with pixels=%p for %dx%d image", 
            (void*)gfx, (void*)gfx->pixels, img->width, img->height);
    
    
    /* Create a Java Graphics object */
    JavaClass* gfx_class = jvm_load_class(jvm, "javax/microedition/lcdui/Graphics");
    if (!gfx_class) {
        DISP_DEBUG("[getGraphics] ERROR: failed to load Graphics class");
        midp_graphics_free(gfx); /* v36.12: unregistering free */
        return NATIVE_RETURN_NULL();
    }
    
    /* Ensure class has space for nativePeer */
    ensure_native_peer_field(gfx_class);
    
    JavaObject* gfx_obj = jvm_new_object(jvm, gfx_class);
    if (!gfx_obj) {
        DISP_DEBUG("[getGraphics] ERROR: failed to create Graphics object");
        midp_graphics_free(gfx); /* v36.12: unregistering free */
        return NATIVE_RETURN_NULL();
    }
    
    /* Store the MidpGraphics* in the nativePeer field */
    set_object_field_ref(gfx_obj, "nativePeer", (JavaObject*)gfx);
    
    DISP_DEBUG("[getGraphics] Returning Graphics obj=%p (native gfx=%p)", 
            (void*)gfx_obj, (void*)gfx);
    
    
    return NATIVE_RETURN_OBJECT(gfx_obj);
}

/*
 * Graphics native methods
 */

static JavaValue native_graphics_setColor(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    jint rgb = args[1].i;
    
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    
    /*fprintf(stderr, "[Graphics] setColor(RGB): obj=%p, gfx=%p, RGB=0x%06X", 
            (void*)gfx_obj, (void*)gfx, rgb & 0xFFFFFF);
    */
    
    if (gfx) {
        midp_graphics_set_color(gfx, rgb, 255);
    }
    
    return NATIVE_RETURN_VOID();
}

/* setColor(int red, int green, int blue) - separate RGB components */
static JavaValue native_graphics_setColorRGB(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    jint red = args[1].i;
    jint green = args[2].i;
    jint blue = args[3].i;
    
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    
    jint rgb = ((red & 0xFF) << 16) | ((green & 0xFF) << 8) | (blue & 0xFF);
    
    /*fprintf(stderr, "[Graphics] setColor(R,G,B): obj=%p, gfx=%p, R=%d, G=%d, B=%d -> 0x%06X", 
            (void*)gfx_obj, (void*)gfx, red, green, blue, rgb);
    */
    
    if (gfx) {
        midp_graphics_set_color(gfx, rgb, 255);
    }
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_graphics_drawLine(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    jint x1 = args[1].i;
    jint y1 = args[2].i;
    jint x2 = args[3].i;
    jint y2 = args[4].i;
    
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    
    /*fprintf(stderr, "[Graphics] drawLine: gfx=%p, (%d,%d)->(%d,%d)", 
            (void*)gfx, x1, y1, x2, y2);
    */
    
    if (gfx) {
        midp_graphics_draw_line(gfx, x1, y1, x2, y2);
    }
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_graphics_fillRect(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    jint x = args[1].i;
    jint y = args[2].i;
    jint w = args[3].i;
    jint h = args[4].i;
    
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    
    if (gfx) {
        midp_graphics_fill_rect(gfx, x, y, w, h);
        race_probe_fillrect(x, y, w, h,
                            (int)((uint32_t)(gfx->rgb_color & 0xFFFFFF) |
                                  ((uint32_t)(gfx->alpha & 0xFF) << 24)));
        /* FIX-19q DIAG: track image-target fills (throttled) */
        {
            static int fr_diag = 0;
            if (fr_diag < 8 && gfx != &g_screen_graphics) {
                fr_diag++;
                fprintf(stderr, "[FILLDIAG] gfx=%p pixels=%p %dx%d fill(%d,%d,%d,%d) color=0x%06X first_px=0x%08X\n",
                        (void*)gfx, (void*)gfx->pixels, gfx->width, gfx->height,
                        x, y, w, h, gfx->rgb_color,
                        (gfx->pixels && gfx->width > 0) ? gfx->pixels[0] : 0);
            }
        }
    }
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_graphics_drawRect(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    jint x = args[1].i;
    jint y = args[2].i;
    jint w = args[3].i;
    jint h = args[4].i;
    
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    
    if (gfx) {
        midp_graphics_draw_rect(gfx, x, y, w, h);
    }
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_graphics_drawImage(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    JavaObject* image_obj = (JavaObject*)args[1].ref;
    jint x = args[2].i;
    jint y = args[3].i;

    /* v22 DIAG: env-gated drawImage trace (first N calls). */
    {
        static int img_trace = -1;
        if (img_trace < 0) img_trace = getenv("NOJME_GFX_TRACE") ? 1 : 0;
        if (img_trace) {
            static int img_logged = 0;
            if (image_obj && img_logged < 150) {
                img_logged++;
                MidpImage* im = (MidpImage*)get_object_field_ref(image_obj, "nativePeer");
                fprintf(stderr, "[GFX-IMG] drawImage img=%dx%d at (%d,%d)\n",
                        im ? im->width : -1, im ? im->height : -1, x, y);
            }
        }
    }
    jint anchor = args[4].i;
    race_probe_drawimage(x, y, anchor);
    
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    MidpImage* img = get_image_from_object(image_obj);
    
    if (!gfx || !img) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    
    /* Diagnostic: track image-to-image and image-to-screen drawing */
    static int draw_count = 0;
    draw_count++;
    if (draw_count <= 200) {
        DISP_DEBUG("[GFX.drawImage] #%d: src_img=%p (%dx%d, pixels=%p), dst_gfx=%p (%dx%d, pixels=%p)",
                draw_count, (void*)img, img->width, img->height, (void*)img->pixels,
                (void*)gfx, gfx->width, gfx->height, (void*)gfx->pixels);
        DISP_DEBUG("[GFX.drawImage] #%d: pos=(%d,%d), anchor=%d, first_src_pixel=0x%08X",
                draw_count, x, y, anchor, img->pixels[0]);
        
    }
    
    midp_graphics_draw_image(gfx, img, x, y, anchor);
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_graphics_setClip(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    jint x = args[1].i;
    jint y = args[2].i;
    jint w = args[3].i;
    jint h = args[4].i;
    
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    
    /*fprintf(stderr, "[Graphics] setClip: (%d,%d) %dx%d, gfx=%p", x, y, w, h, (void*)gfx);
    */
    
    if (gfx) {
        /* v34.7 FIX (MIDP clip semantics, JSR-118): setClip coordinates are
         * relative to the CURRENT TRANSLATION, and the resulting device clip
         * is frozen at the moment of the call (later translate() calls do
         * not move it). We previously stored the raw coords, so a game that
         * did translate(0,48) + setClip(0,0,240,206) (Doom RPG viewport)
         * got a device clip of rows 0..205 instead of 48..253 — the bottom
         * of the 3D view and everything below it was cut off.
         * NOTE: midp_graphics_set_clip takes DEVICE coords. */
        int dx = x + gfx->translate_x;
        int dy = y + gfx->translate_y;

        /* Limit clip to available area (excluding soft buttons when not fullscreen) */
        /* Check if this is screen graphics */
        int max_height = gfx->height;
        /* v34.7 FIX: never apply the soft-button clamp to the GameCanvas
         * offscreen buffer. In headless/libretro backends
         * sdl_is_screen_graphics() returns true for EVERY gfx, which made
         * the clamp corrupt game canvas clips (bottom rows lost). */
        bool is_gamecanvas_gfx = (game_canvas.offscreen_buffer &&
                                  gfx->pixels == game_canvas.offscreen_buffer->pixels);
        if (!g_full_screen_mode && !is_gamecanvas_gfx && sdl_is_screen_graphics(gfx)) {
            /* Screen graphics - limit to area above soft buttons if commands exist */
            if (current_displayable_obj) {
                int commands_idx = find_field_index(current_displayable_obj, "commands");
                if (commands_idx >= 0) {
                    JavaArray* commands = (JavaArray*)current_displayable_obj->fields[commands_idx].ref;
                    if (commands && is_heap_ptr_check(commands) && ((uintptr_t)commands > 0x10000) && commands->length > 0) {
                        max_height = gfx->height - SOFT_BUTTON_HEIGHT;
                    }
                }
            }
        }

        /* Clamp the clip region (device coords) */
        if (dy + h > max_height) {
            h = max_height - dy;
            if (h < 0) h = 0;
        }

        midp_graphics_set_clip(gfx, dx, dy, w, h);
        race_probe_setclip(x, y, w, h, gfx->clip_x, gfx->clip_y,
                           gfx->clip_width, gfx->clip_height,
                           gfx->width, gfx->height,
                           gfx->translate_x, gfx->translate_y);
    }

    return NATIVE_RETURN_VOID();
}

static JavaValue native_graphics_translate(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    jint x = args[1].i;
    jint y = args[2].i;

    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);

    if (gfx) {
        midp_graphics_translate(gfx, x, y);
    }

    return NATIVE_RETURN_VOID();
}

static JavaValue native_graphics_getTranslateX(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    return NATIVE_RETURN_INT(gfx ? gfx->translate_x : 0);
}

static JavaValue native_graphics_getTranslateY(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    return NATIVE_RETURN_INT(gfx ? gfx->translate_y : 0);
}

static JavaValue native_graphics_getClipX(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    /* v34.7: getClipX/Y return the clip in the CURRENT translated
     * coordinate system (JSR-118): device clip minus translation. */
    return NATIVE_RETURN_INT(gfx ? gfx->clip_x - gfx->translate_x : 0);
}

static JavaValue native_graphics_getClipY(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    return NATIVE_RETURN_INT(gfx ? gfx->clip_y - gfx->translate_y : 0);
}

static JavaValue native_graphics_getClipWidth(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    return NATIVE_RETURN_INT(gfx ? gfx->clip_width : 0);
}

static JavaValue native_graphics_getClipHeight(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    return NATIVE_RETURN_INT(gfx ? gfx->clip_height : 0);
}

static JavaValue native_graphics_getColor(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    return NATIVE_RETURN_INT(gfx ? gfx->rgb_color : 0);
}

/* Forward declaration for get_font_from_object (defined in Font section below) */
static MidpFont* get_font_from_object(JavaObject* obj);

static JavaValue native_graphics_setFont(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    JavaObject* font_obj = (JavaObject*)args[1].ref;
    MidpFont* font = get_font_from_object(font_obj);
    
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    
    /*fprintf(stderr, "[Graphics] setFont: gfx=%p, font_obj=%p, font=%p", 
            (void*)gfx, (void*)font_obj, (void*)font);
    */
    
    if (gfx) {
        if (font) {
            /* v36.29: store the full pointer (MidpGraphics.font is void* —
             * the old (jint)(intptr_t) cast truncated 64-bit addresses) */
            gfx->font = font;
        } else {
            /* COMPATIBILITY FIX: If font is NULL, use default font.
             * This happens when Font.getFont() returns NULL or game passes null.
             * J2ME games expect setFont(null) to work like setFont(Font.getDefaultFont()).
             */
            MidpFont* default_font = midp_font_get_default();
            gfx->font = default_font;
            NATIVE_DEBUG("setFont(null) -> using default font");
        }
    }
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_graphics_clipRect(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    jint x = args[1].i;
    jint y = args[2].i;
    jint w = args[3].i;
    jint h = args[4].i;
    
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);

    if (gfx) {
        /* v34.7 FIX: clipRect coordinates are also relative to the current
         * translation (JSR-118). Convert to device space before intersect. */
        midp_graphics_clip_rect(gfx, x + gfx->translate_x, y + gfx->translate_y, w, h);

        /* Limit clip to available area (excluding soft buttons when not fullscreen) */
        /* Check if this is screen graphics */
        {
            bool is_gamecanvas_gfx = (game_canvas.offscreen_buffer &&
                                      gfx->pixels == game_canvas.offscreen_buffer->pixels);
            if (!g_full_screen_mode && !is_gamecanvas_gfx && sdl_is_screen_graphics(gfx)) {
                if (current_displayable_obj) {
                    int commands_idx = find_field_index(current_displayable_obj, "commands");
                    if (commands_idx >= 0) {
                        JavaArray* commands = (JavaArray*)current_displayable_obj->fields[commands_idx].ref;
                        if (commands && is_heap_ptr_check(commands) && ((uintptr_t)commands > 0x10000) && commands->length > 0) {
                            int max_height = gfx->height - SOFT_BUTTON_HEIGHT;
                            if (gfx->clip_y + gfx->clip_height > max_height) {
                                gfx->clip_height = max_height - gfx->clip_y;
                                if (gfx->clip_height < 0) gfx->clip_height = 0;
                            }
                        }
                    }
                }
            }
        }
    }

    return NATIVE_RETURN_VOID();
}

static JavaValue native_graphics_drawRoundRect(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    jint x = args[1].i;
    jint y = args[2].i;
    jint w = args[3].i;
    jint h = args[4].i;
    jint arc_w = args[5].i;
    jint arc_h = args[6].i;
    
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    
    if (gfx) {
        midp_graphics_draw_round_rect(gfx, x, y, w, h, arc_w, arc_h);
    }
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_graphics_fillRoundRect(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    jint x = args[1].i;
    jint y = args[2].i;
    jint w = args[3].i;
    jint h = args[4].i;
    jint arc_w = args[5].i;
    jint arc_h = args[6].i;
    
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    
    if (gfx) {
        midp_graphics_fill_round_rect(gfx, x, y, w, h, arc_w, arc_h);
    }
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_graphics_drawArc(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    jint x = args[1].i;
    jint y = args[2].i;
    jint w = args[3].i;
    jint h = args[4].i;
    jint start = args[5].i;
    jint arc = args[6].i;
    
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    
    if (gfx) {
        midp_graphics_draw_arc(gfx, x, y, w, h, start, arc);
    }
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_graphics_fillArc(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    jint x = args[1].i;
    jint y = args[2].i;
    jint w = args[3].i;
    jint h = args[4].i;
    jint start = args[5].i;
    jint arc = args[6].i;
    
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    
    if (gfx) {
        midp_graphics_fill_arc(gfx, x, y, w, h, start, arc);
    }
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_graphics_drawString(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    const char* str = native_get_string_utf8(jvm, args, 1);
    jint x = args[2].i;
    jint y = args[3].i;
    jint anchor = args[4].i;
    
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);

    /* v34.8 DIAG: Bounce Tales "текст не рисуется" — трассируем drawString
     * (первые 40 вызовов; NOJME_DSTR_TRACE=2 — без лимита). */
    {
        static int dstr_trace = -1;
        if (dstr_trace < 0) {
            const char* v = getenv("NOJME_DSTR_TRACE");
            dstr_trace = (v && v[0] == '2') ? 2 : (v ? 1 : 0);
        }
        if (dstr_trace) {
            static int dstr_logged = 0;
            if (dstr_trace == 2 || dstr_logged < 40) {
                dstr_logged++;
                fprintf(stderr, "[DSTR] gfx=%p str=%s at (%d,%d) anchor=%d tx=%d ty=%d\n",
                        (void*)gfx, str ? str : "(null)", x, y, anchor,
                        gfx ? gfx->translate_x : -999, gfx ? gfx->translate_y : -999);
            }
        }
    }

    if (gfx && str) {
        midp_graphics_draw_string(gfx, str, x, y, anchor);
    }
    
    return NATIVE_RETURN_VOID();
}


/* =======================================================================
 * v35: Graphics API-completeness. drawChar/drawChars/getFont and the RGB
 * component getters were unregistered -> silent default returns: glyph-by-
 * glyph text rendered nothing and Font save/restore round-trips broke.
 * ======================================================================= */
static JavaObject* create_font_object(JVM* jvm, MidpFont* font);  /* fwd: defined below */
static JavaValue native_graphics_drawChar(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    jint ch = args[1].i;
    jint x = args[2].i;
    jint y = args[3].i;
    jint anchor = args[4].i;
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    if (!gfx) return NATIVE_RETURN_VOID();
    char buf[8];
    int o = 0;
    unsigned int c = (unsigned int)ch;
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
    buf[o] = '\0';
    if (o > 0) {
        midp_graphics_draw_string(gfx, buf, x, y, anchor);
    }
    return NATIVE_RETURN_VOID();
}

static JavaValue native_graphics_drawChars(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    JavaArray* data = (JavaArray*)args[1].ref;
    jint offset = args[2].i;
    jint length = args[3].i;
    jint x = args[4].i;
    jint y = args[5].i;
    jint anchor = args[6].i;
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    if (!gfx) return NATIVE_RETURN_VOID();
    if (!data || length <= 0 || data->element_type != T_CHAR) {
        return NATIVE_RETURN_VOID();
    }
    if (offset < 0) offset = 0;
    if (offset + length > data->length) length = data->length - offset;
    if (length <= 0) return NATIVE_RETURN_VOID();
    jchar* chars = (jchar*)array_data(data);
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
    midp_graphics_draw_string(gfx, buf, x, y, anchor);
    free(buf);
    return NATIVE_RETURN_VOID();
}

static JavaValue native_graphics_getFont(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    MidpFont* font = gfx ? (MidpFont*)gfx->font : NULL;
    if (!font) font = midp_font_get_default();
    if (!font) return NATIVE_RETURN_NULL();
    JavaObject* font_obj = create_font_object(jvm, font);
    if (!font_obj) return NATIVE_RETURN_NULL();
    return NATIVE_RETURN_OBJECT(font_obj);
}

/* Graphics.getRedComponent/getGreenComponent/getBlueComponent (MIDP 2.0) */
static JavaValue native_graphics_getRedComponent(JVM* jvm, JavaThread* thread,
                                                 JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    MidpGraphics* gfx = get_graphics_from_object((JavaObject*)args[0].ref);
    return NATIVE_RETURN_INT(gfx ? (gfx->rgb_color >> 16) & 0xFF : 0);
}

static JavaValue native_graphics_getGreenComponent(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    MidpGraphics* gfx = get_graphics_from_object((JavaObject*)args[0].ref);
    return NATIVE_RETURN_INT(gfx ? (gfx->rgb_color >> 8) & 0xFF : 0);
}

static JavaValue native_graphics_getBlueComponent(JVM* jvm, JavaThread* thread,
                                                  JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    MidpGraphics* gfx = get_graphics_from_object((JavaObject*)args[0].ref);
    return NATIVE_RETURN_INT(gfx ? gfx->rgb_color & 0xFF : 0);
}

/* Helper to apply transform to source coordinates */
__attribute__((unused))
static void transform_coord(int transform, int src_x, int src_y, int src_w, int src_h,
                           int *out_x, int *out_y) {
    switch (transform) {
        case SPRITE_TRANS_NONE:           /* No transform */
            *out_x = src_x;
            *out_y = src_y;
            break;
        case SPRITE_TRANS_MIRROR_ROT180:  /* Mirror vertical + rotate 180 */
            *out_x = src_w - 1 - src_x;
            *out_y = src_h - 1 - src_y;
            break;
        case SPRITE_TRANS_MIRROR:         /* Mirror vertical */
            *out_x = src_x;
            *out_y = src_h - 1 - src_y;
            break;
        case SPRITE_TRANS_ROT180:         /* Rotate 180 */
            *out_x = src_w - 1 - src_x;
            *out_y = src_h - 1 - src_y;
            break;
        case SPRITE_TRANS_MIRROR_ROT270:  /* Mirror + rotate 270 (90 CCW) */
            *out_x = src_h - 1 - src_y;
            *out_y = src_x;
            break;
        case SPRITE_TRANS_ROT90:          /* Rotate 90 CW */
            *out_x = src_h - 1 - src_y;
            *out_y = src_w - 1 - src_x;
            break;
        case SPRITE_TRANS_ROT270:         /* Rotate 270 CW (90 CCW) */
            *out_x = src_y;
            *out_y = src_x;
            break;
        case SPRITE_TRANS_MIRROR_ROT90:   /* Mirror + rotate 90 */
            *out_x = src_y;
            *out_y = src_w - 1 - src_x;
            break;
        default:
            *out_x = src_x;
            *out_y = src_y;
            break;
    }
}

/* Get output dimensions after transform */
__attribute__((unused))
static void get_transformed_size(int transform, int src_w, int src_h, int *out_w, int *out_h) {
    switch (transform) {
        case SPRITE_TRANS_ROT90:
        case SPRITE_TRANS_ROT270:
        case SPRITE_TRANS_MIRROR_ROT270:
        case SPRITE_TRANS_MIRROR_ROT90:
            *out_w = src_h;
            *out_h = src_w;
            break;
        default:
            *out_w = src_w;
            *out_h = src_h;
            break;
    }
}

/* Graphics.drawRegion(Image src, int x_src, int y_src, int width, int height, 
 *                    int transform, int x_dest, int y_dest, int anchor)
 * CRITICAL for game compatibility!
 * Delegate to midp_graphics_draw_region which has correct transform mapping.
 */
static JavaValue native_graphics_drawRegion(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    JavaObject* src_image_obj = (JavaObject*)args[1].ref;
    jint x_src = args[2].i;
    jint y_src = args[3].i;
    jint width = args[4].i;
    jint height = args[5].i;
    jint transform = args[6].i;
    jint x_dest = args[7].i;
    jint y_dest = args[8].i;
    jint anchor = args[9].i;
    
    if (!gfx_obj || !src_image_obj) {
        return NATIVE_RETURN_VOID();
    }
    
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    MidpImage* src_img = get_image_from_object(src_image_obj);

    /* v34.8 DIAG: drawRegion trace (Bounce Tales text glyphs) */
    {
        static int dreg_trace = -1;
        if (dreg_trace < 0) dreg_trace = getenv("NOJME_DREG_TRACE") ? 1 : 0;
        if (dreg_trace && width <= 20 && height <= 20) {
            static int dreg_logged = 0;
            if (dreg_logged < 60) {
                dreg_logged++;
                fprintf(stderr, "[DREG] img=%dx%d px=%p src=(%d,%d) %dx%d tr=%d dst=(%d,%d) a=%d%s\n",
                        src_img ? src_img->width : -1, src_img ? src_img->height : -1,
                        (src_img && src_img->pixels) ? "ok" : "NULL",
                        x_src, y_src, width, height, transform, x_dest, y_dest, anchor,
                        (!gfx) ? " [NO GFX]" : (!src_img ? " [NO IMG]" :
                        (!src_img->pixels ? " [NO PIX]" :
                        (x_src < 0 || y_src < 0 || width <= 0 || height <= 0 ? " [BAD SRC]" :
                         (x_src + width > src_img->width || y_src + height > src_img->height
                          ? " [OOB]" : "")))));
            }
        }
    }

    if (!gfx || !src_img || !src_img->pixels) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Bounds check source region */
    if (x_src < 0 || y_src < 0 || width <= 0 || height <= 0) {
        return NATIVE_RETURN_VOID();
    }
    if (x_src + width > src_img->width || y_src + height > src_img->height) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Delegate to the correct implementation in graphics.c */
    midp_graphics_draw_region(gfx, src_img, x_src, y_src, width, height,
                               transform, x_dest, y_dest, anchor);
    
    return NATIVE_RETURN_VOID();
}

/* v34.8 FIX (Bounce Tales): Graphics.fillTriangle — был тихим стабом. */
static JavaValue native_graphics_fillTriangle(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    if (!gfx) return NATIVE_RETURN_VOID();
    midp_graphics_fill_triangle(gfx, args[1].i, args[2].i, args[3].i, args[4].i,
                                args[5].i, args[6].i);
    return NATIVE_RETURN_VOID();
}

/* Graphics.drawSubstring(String, int offset, int length, int x, int y, int anchor)
 * v34.8 FIX (Bounce Tales): метод отсутствовал — invokeinterface-стаб глушил
 * вызов, и ВЕСЬ текст игры (меню/диалоги — рендерер o.a PC=177/224) не
 * рисовался. По JSR-118 эквивалентен drawString(str.substring(o, o+l), ...). */
static JavaValue native_graphics_drawSubstring(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    const char* str = native_get_string_utf8(jvm, args, 1);
    jint offset = args[2].i;
    jint length = args[3].i;
    jint x = args[4].i;
    jint y = args[5].i;
    jint anchor = args[6].i;

    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);

    /* Spec: NullPointerException for null str; IllegalStateException for
     * bad offset/length or invalid anchor. Мягкий вариант: игнор. */
    if (!gfx || !str) {
        if (!str) {
            jvm_throw_by_name(jvm, "java/lang/NullPointerException", NULL);
            thread->pending_exception = jvm_exception_pending(jvm);
        }
        return NATIVE_RETURN_VOID();
    }
    int total = (int)strlen(str);
    if (offset < 0 || length < 0 || offset + length > total) {
        jvm_throw_by_name(jvm, "java/lang/IllegalStateException", "drawSubstring range");
        thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_VOID();
    }
    if (length == 0) return NATIVE_RETURN_VOID();

    char* sub = (char*)malloc(length + 1);
    if (!sub) return NATIVE_RETURN_VOID();
    memcpy(sub, str + offset, length);
    sub[length] = '\0';

    /* v34.8 DIAG: same trace switch as drawString */
    {
        static int dstr_trace = -1;
        if (dstr_trace < 0) dstr_trace = getenv("NOJME_DSTR_TRACE") ? 1 : 0;
        if (dstr_trace) {
            static int dsub_logged = 0;
            if (dsub_logged < 40) {
                dsub_logged++;
                fprintf(stderr, "[DSTR] (substring) str=%s at (%d,%d) anchor=%d\n",
                        sub, x, y, anchor);
            }
        }
    }

    midp_graphics_draw_string(gfx, sub, x, y, anchor);
    free(sub);
    return NATIVE_RETURN_VOID();
}

/* Graphics.drawImage(Image img, int x, int y, int anchor) - simplified version */
static JavaValue native_graphics_drawImage_anchor(JVM* jvm, JavaThread* thread,
                                                  JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    JavaObject* image_obj = (JavaObject*)args[1].ref;
    jint x = args[2].i;
    jint y = args[3].i;
    jint anchor = args[4].i;
    
    if (!gfx_obj || !image_obj) {
        return NATIVE_RETURN_VOID();
    }
    
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    MidpImage* img = get_image_from_object(image_obj);
    
    if (!gfx || !img || !img->pixels) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Calculate anchor offset */
    int anchor_x = 0, anchor_y = 0;
    if (anchor & 0x02) anchor_x = -img->width;      /* RIGHT */
    else if (anchor & 0x01) anchor_x = -img->width/2; /* HCENTER */
    if (anchor & 0x20) anchor_y = -img->height;     /* BOTTOM */
    else if (anchor & 0x10) anchor_y = -img->height/2; /* VCENTER */
    
    int draw_x = x + anchor_x + gfx->translate_x;
    int draw_y = y + anchor_y + gfx->translate_y;
    
    /* Clip to graphics bounds and clip region */
    int clip_x1 = gfx->clip_x;
    int clip_y1 = gfx->clip_y;
    int clip_x2 = gfx->clip_x + gfx->clip_width;
    int clip_y2 = gfx->clip_y + gfx->clip_height;
    
    for (int py = 0; py < img->height; py++) {
        for (int px = 0; px < img->width; px++) {
            int dest_x = draw_x + px;
            int dest_y = draw_y + py;
            
            /* Clip check */
            if (dest_x < clip_x1 || dest_x >= clip_x2 ||
                dest_y < clip_y1 || dest_y >= clip_y2) continue;
            
            /* Bounds check */
            if (dest_x < 0 || dest_x >= gfx->width ||
                dest_y < 0 || dest_y >= gfx->height) continue;
            
            uint32_t pixel = img->pixels[py * img->width + px];
            uint8_t alpha = (pixel >> 24) & 0xFF;
            
            /* Skip transparent pixels */
            if (alpha == 0) continue;
            
            int dest_idx = dest_y * gfx->width + dest_x;
            
            /* Alpha blending */
            if (alpha == 255 || !img->alpha) {
                gfx->pixels[dest_idx] = pixel;
            } else {
                uint32_t dest_pixel = gfx->pixels[dest_idx];
                uint8_t dr = (dest_pixel >> 16) & 0xFF;
                uint8_t dg = (dest_pixel >> 8) & 0xFF;
                uint8_t db = dest_pixel & 0xFF;
                uint8_t sr = (pixel >> 16) & 0xFF;
                uint8_t sg = (pixel >> 8) & 0xFF;
                uint8_t sb = pixel & 0xFF;
                gfx->pixels[dest_idx] = ((uint32_t)(255) <<  24) |
                    (((sr * alpha + dr * (255 - alpha)) / 255) << 16) |
                    (((sg * alpha + dg * (255 - alpha)) / 255) << 8) |
                    ((sb * alpha + db * (255 - alpha)) / 255);
            }
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* Graphics.copyArea(int x_src, int y_src, int width, int height, int x_dest, int y_dest, int anchor) */
static JavaValue native_graphics_copyArea(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    jint x_src = args[1].i;
    jint y_src = args[2].i;
    jint width = args[3].i;
    jint height = args[4].i;
    jint x_dest = args[5].i;
    jint y_dest = args[6].i;
    jint anchor = args[7].i;
    
    if (!gfx_obj) {
        return NATIVE_RETURN_VOID();
    }
    
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    if (!gfx || !gfx->pixels) {
        return NATIVE_RETURN_VOID();
    }
    
    /* FIX (audit S-17, v18): anchor masks were scrambled (0x02/0x01/0x10/0x20
     * mixup mapped RIGHT onto nothing and VCENTER horizontally). Standard
     * Graphics constants are LEFT=4, RIGHT=8, HCENTER=1, TOP=16, BOTTOM=32,
     * VCENTER=2. Also per JSR-118 copyArea must throw IllegalArgumentException
     * when the source region falls outside the surface instead of silently
     * clamping. */
    if (width <= 0 || height <= 0) {
        native_throw_iae(jvm, thread, "copyArea: width and height must be positive");
        return NATIVE_RETURN_VOID();
    }
    
    int sx_dev = x_src + gfx->translate_x;
    int sy_dev = y_src + gfx->translate_y;
    if (sx_dev < 0 || sy_dev < 0 ||
        sx_dev + width > gfx->width || sy_dev + height > gfx->height) {
        native_throw_iae(jvm, thread, "copyArea: source region out of bounds");
        return NATIVE_RETURN_VOID();
    }
    
    /* Calculate anchor offset with the correct masks */
    int anchor_x = 0, anchor_y = 0;
    if (anchor & 0x08)      anchor_x = -width;       /* RIGHT */
    else if (anchor & 0x01) anchor_x = -width / 2;   /* HCENTER */
    /* else LEFT (4) or default: no offset */
    
    if (anchor & 0x20)      anchor_y = -height;      /* BOTTOM */
    else if (anchor & 0x02) anchor_y = -height / 2;  /* VCENTER */
    /* else TOP (0x10): no offset */
    
    int dest_x = x_dest + anchor_x + gfx->translate_x;
    int dest_y = y_dest + anchor_y + gfx->translate_y;
    
    /* Destination blit is subject to the current clip (device space) */
    int clip_x1 = gfx->clip_x > 0 ? gfx->clip_x : 0;
    int clip_y1 = gfx->clip_y > 0 ? gfx->clip_y : 0;
    int clip_x2 = gfx->clip_x + gfx->clip_width < gfx->width ?
                  gfx->clip_x + gfx->clip_width : gfx->width;
    int clip_y2 = gfx->clip_y + gfx->clip_height < gfx->height ?
                  gfx->clip_y + gfx->clip_height : gfx->height;
    
    /* Copy region (need temp buffer for overlapping copy) */
    uint32_t* temp = (uint32_t*)malloc((size_t)width * height * sizeof(uint32_t));
    if (!temp) return NATIVE_RETURN_VOID();
    
    /* Copy source to temp (device coords, bounds validated above) */
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            temp[y * width + x] = gfx->pixels[(sy_dev + y) * gfx->width + (sx_dev + x)];
        }
    }
    
    /* Copy temp to destination, clipped */
    for (int y = 0; y < height; y++) {
        int dy = dest_y + y;
        if (dy < clip_y1 || dy >= clip_y2) continue;
        for (int x = 0; x < width; x++) {
            int dx = dest_x + x;
            if (dx < clip_x1 || dx >= clip_x2) continue;
            gfx->pixels[dy * gfx->width + dx] = temp[y * width + x];
        }
    }
    
    free(temp);
    return NATIVE_RETURN_VOID();
}

/* Graphics.getDisplayColor(int color) - returns the color that will be displayed */
static JavaValue native_graphics_getDisplayColor(JVM* jvm, JavaThread* thread,
                                                 JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jint color = args[1].i;
    /* On color display, return the same color */
    return NATIVE_RETURN_INT(color);
}

/* Graphics.setStrokeStyle(int style) - SOLID=0, DOTTED=1 */
static JavaValue native_graphics_setStrokeStyle(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    jint style = args[1].i;
    
    if (!gfx_obj) return NATIVE_RETURN_VOID();
    
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    if (gfx) {
        gfx->stroke_style = style;
    }
    
    return NATIVE_RETURN_VOID();
}

/* Graphics.getStrokeStyle() */
static JavaValue native_graphics_getStrokeStyle(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    
    if (!gfx_obj) return NATIVE_RETURN_INT(0);
    
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    if (gfx) {
        return NATIVE_RETURN_INT(gfx->stroke_style);
    }
    
    return NATIVE_RETURN_INT(0);
}

/* Graphics.setGrayScale(int value) */
static JavaValue native_graphics_setGrayScale(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    jint value = args[1].i;
    
    if (!gfx_obj) return NATIVE_RETURN_VOID();
    
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    if (gfx) {
        /* Convert grayscale to RGB */
        uint8_t gray = value & 0xFF;
        gfx->rgb_color = 0xFF000000 | (gray << 16) | (gray << 8) | gray;
    }
    
    return NATIVE_RETURN_VOID();
}

/* Graphics.getGrayScale() */
static JavaValue native_graphics_getGrayScale(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    
    if (!gfx_obj) return NATIVE_RETURN_INT(0);
    
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    if (gfx) {
        uint8_t r = (gfx->rgb_color >> 16) & 0xFF;
        uint8_t g = (gfx->rgb_color >> 8) & 0xFF;
        uint8_t b = gfx->rgb_color & 0xFF;
        /* ITU-R BT.601 luminance: 0.299*R + 0.587*G + 0.114*B, using integer math */
        int gray = (19595 * r + 38470 * g + 7471 * b + 32768) >> 16;
        if (gray > 255) gray = 255;
        return NATIVE_RETURN_INT(gray);
    }
    
    return NATIVE_RETURN_INT(0);
}

/*
 * Font native methods
 */

/* Helper to get MidpFont from Java Font object */
static MidpFont* get_font_from_object(JavaObject* obj) {
    if (!obj) {
        NATIVE_DEBUG("get_font_from_object: NULL object");
        return NULL;
    }
    
    /* Check if object has fields */
    if (!obj->header.clazz) {
        NATIVE_DEBUG("get_font_from_object: object has NULL class");
        return NULL;
    }
    
    JavaClass* clazz = obj->header.clazz;
    
    if (clazz->fields_count == 0) {
        NATIVE_DEBUG("get_font_from_object: class has no fields!");
        return NULL;
    }
    
    /* The MidpFont* is stored in the nativePeer field */
    MidpFont* font = (MidpFont*)get_object_field_ref(obj, "nativePeer");
    return font;
}

/* Cached default Font Java object */
static JavaObject* g_default_font_object = NULL;

/* Helper to create a Java Font object wrapping a MidpFont */
static JavaObject* create_font_object(JVM* jvm, MidpFont* font) {
    if (!font) return NULL;
    
    /* Get the Font class */
    JavaClass* font_class = jvm_load_class(jvm, "javax/microedition/lcdui/Font");
    if (!font_class) {
        NATIVE_DEBUG("Failed to load Font class");
        return NULL;
    }
    
    /* Ensure class has space for nativePeer */
    ensure_native_peer_field(font_class);
    
    /* Create Font object */
    JavaObject* font_obj = jvm_new_object(jvm, font_class);
    if (!font_obj) {
        NATIVE_DEBUG("Failed to create Font object");
        return NULL;
    }
    
    /* Store the MidpFont* in the nativePeer field */
    set_object_field_ref(font_obj, "nativePeer", (JavaObject*)font);
    
    NATIVE_DEBUG("Created Font object: %p, native: %p", (void*)font_obj, (void*)font);
    
    return font_obj;
}

/* v35: non-static wrapper so other UI files (form.c StringItem.getFont)
 * can obtain the default Font object without reaching into statics. */
JavaObject* midp_font_default_object(JVM* jvm) {
    MidpFont* font = midp_font_get_default();
    if (!font) return NULL;
    JavaObject* obj = create_font_object(jvm, font);
    return obj;
}

static JavaValue native_font_getDefaultFont(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)thread; (void)args; (void)arg_count;
    
    NATIVE_DEBUG("getDefaultFont() called");
    
    /* Return cached default Font object */
    if (!g_default_font_object) {
        MidpFont* default_font = midp_font_get_default();
        g_default_font_object = create_font_object(jvm, default_font);
        /* Register as GC root to prevent collection */
        if (g_default_font_object) {
            gc_add_root(jvm, (void**)&g_default_font_object);
        }
    }
    
    return NATIVE_RETURN_OBJECT(g_default_font_object);
}

static JavaValue native_font_stringWidth(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* font_obj = (JavaObject*)args[0].ref;
    MidpFont* font = get_font_from_object(font_obj);
    const char* str = native_get_string_utf8(jvm, args, 1);
    
    return NATIVE_RETURN_INT(midp_font_string_width(font, str));
}

static JavaValue native_font_height(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* font_obj = (JavaObject*)args[0].ref;
    MidpFont* font = get_font_from_object(font_obj);
    
    NATIVE_DEBUG("getHeight() called, obj=%p, font=%p, height=%d", 
            (void*)font_obj, (void*)font, font ? font->height : 0);
    
    return NATIVE_RETURN_INT(midp_font_height(font));
}

/* Font.charWidth(char ch) */
static JavaValue native_font_charWidth(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* font_obj = (JavaObject*)args[0].ref;
    jchar ch = (jchar)args[1].i;
    
    MidpFont* font = get_font_from_object(font_obj);
    if (!font) return NATIVE_RETURN_INT(0);
    
    /* For bitmap fonts, character width is typically fixed or based on character */
    int width = midp_font_char_width(font, ch);
    return NATIVE_RETURN_INT(width);
}

/* Font.charsWidth(char[] ch, int offset, int length) */
static JavaValue native_font_charsWidth(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* font_obj = (JavaObject*)args[0].ref;
    JavaArray* char_array = (JavaArray*)args[1].ref;
    jint length = args[3].i;  /* bitmap font: fixed width, offset unused */
    
    MidpFont* font = get_font_from_object(font_obj);
    if (!font || !char_array) return NATIVE_RETURN_INT(0);
    
    /* Width includes inter-character spacing, consistent with stringWidth
     * (v36.29: respects the SIZE_LARGE 2x scale) */
    return NATIVE_RETURN_INT(length > 0 ? length * (midp_font_char_width(font, ' ') + 1) - 1 : 0);
}

/* Font.substringWidth(String str, int offset, int len) */
static JavaValue native_font_substringWidth(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* font_obj = (JavaObject*)args[0].ref;
    JavaString* str = (JavaString*)args[1].ref;
    jint len = args[3].i;  /* bitmap font: fixed width, offset unused */
    
    MidpFont* font = get_font_from_object(font_obj);
    if (!font || !str) return NATIVE_RETURN_INT(0);
    
    /* Width includes inter-character spacing, consistent with stringWidth
     * (v36.29: respects the SIZE_LARGE 2x scale) */
    return NATIVE_RETURN_INT(len > 0 ? len * (midp_font_char_width(font, ' ') + 1) - 1 : 0);
}

/* Font.getBaselinePosition() */
static JavaValue native_font_getBaselinePosition(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* font_obj = (JavaObject*)args[0].ref;
    MidpFont* font = get_font_from_object(font_obj);
    
    if (!font) return NATIVE_RETURN_INT(0);
    
    /* Baseline is typically near the bottom of the font height */
    return NATIVE_RETURN_INT(font->baseline);
}

/* Font.getFace() */
static JavaValue native_font_getFace(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* font_obj = (JavaObject*)args[0].ref;
    MidpFont* font = get_font_from_object(font_obj);
    
    if (!font) return NATIVE_RETURN_INT(0);
    return NATIVE_RETURN_INT(font->face);
}

/* Font.getStyle() */
static JavaValue native_font_getStyle(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* font_obj = (JavaObject*)args[0].ref;
    MidpFont* font = get_font_from_object(font_obj);
    
    if (!font) return NATIVE_RETURN_INT(0);
    return NATIVE_RETURN_INT(font->style);
}

/* Font.getSize() */
static JavaValue native_font_getSize(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* font_obj = (JavaObject*)args[0].ref;
    MidpFont* font = get_font_from_object(font_obj);
    
    if (!font) return NATIVE_RETURN_INT(0);
    return NATIVE_RETURN_INT(font->size);
}

/* Font.isPlain(), isBold(), isItalic(), isUnderlined() */
static JavaValue native_font_isBold(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* font_obj = (JavaObject*)args[0].ref;
    MidpFont* font = get_font_from_object(font_obj);
    
    if (!font) return NATIVE_RETURN_INT(0);
    return NATIVE_RETURN_INT((font->style & FONT_STYLE_BOLD) != 0);
}

static JavaValue native_font_isItalic(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* font_obj = (JavaObject*)args[0].ref;
    MidpFont* font = get_font_from_object(font_obj);
    
    if (!font) return NATIVE_RETURN_INT(0);
    return NATIVE_RETURN_INT((font->style & FONT_STYLE_ITALIC) != 0);
}

static JavaValue native_font_isUnderlined(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* font_obj = (JavaObject*)args[0].ref;
    MidpFont* font = get_font_from_object(font_obj);
    
    if (!font) return NATIVE_RETURN_INT(0);
    return NATIVE_RETURN_INT((font->style & FONT_STYLE_UNDERLINED) != 0);
}

static JavaValue native_font_isPlain(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* font_obj = (JavaObject*)args[0].ref;
    MidpFont* font = get_font_from_object(font_obj);
    
    if (!font) return NATIVE_RETURN_INT(1);
    return NATIVE_RETURN_INT(font->style == FONT_STYLE_PLAIN);
}

static JavaValue native_font_getFont(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    jint face = args[0].i;
    jint style = args[1].i;
    jint size = args[2].i;
    
    NATIVE_DEBUG("getFont(%d, %d, %d) called", face, style, size);
    
    /* Get or create native font */
    MidpFont* font = midp_font_get(face, style, size);
    
    /* COMPATIBILITY FIX: Font.getFont() should NEVER return null.
     * If the requested font cannot be created, return the default font.
     * This is consistent with J2ME specification and prevents NPE in games.
     */
    if (!font) {
        NATIVE_DEBUG("getFont: failed to create font, using default");
        font = midp_font_get_default();
    }
    
    /* Create Java Font object to wrap it */
    JavaObject* font_obj = create_font_object(jvm, font);
    
    /* If still NULL, return the cached default font object */
    if (!font_obj) {
        NATIVE_DEBUG("getFont: failed to create Font object, returning default");
        return native_font_getDefaultFont(jvm, thread, args, arg_count);
    }
    
    return NATIVE_RETURN_OBJECT(font_obj);
}

/*
 * Initialize native methods
 */

void init_javax_microedition_lcdui_display(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"javax/microedition/lcdui/Display", "getDisplay", "(Ljavax/microedition/midlet/MIDlet;)Ljavax/microedition/lcdui/Display;", native_display_getDisplay},
        {"javax/microedition/lcdui/Display", "setCurrent", "(Ljavax/microedition/lcdui/Displayable;)V", native_display_setCurrent},
        /* FIX (task 16-b #5): 2-arg overload - remember the next Displayable
         * so dismissal (timeout/key) can return to it after commandAction. */
        {"javax/microedition/lcdui/Display", "setCurrent", "(Ljavax/microedition/lcdui/Alert;Ljavax/microedition/lcdui/Displayable;)V", native_display_setCurrent_alert},
        {"javax/microedition/lcdui/Display", "getCurrent", "()Ljavax/microedition/lcdui/Displayable;", native_display_getCurrent},
        {"javax/microedition/lcdui/Display", "callSerially", "(Ljava/lang/Runnable;)V", native_display_callSerially},
        {"javax/microedition/lcdui/Display", "getWidth", "()I", native_display_getWidth},
        {"javax/microedition/lcdui/Display", "getHeight", "()I", native_display_getHeight},
        {"javax/microedition/lcdui/Display", "isColor", "()Z", native_display_isColor},
        {"javax/microedition/lcdui/Display", "numColors", "()I", native_display_numColors},
        {"javax/microedition/lcdui/Display", "numAlphaLevels", "()I", native_display_numAlphaLevels},
        {"javax/microedition/lcdui/Display", "vibrate", "(I)Z", native_display_vibrate},
        {"javax/microedition/lcdui/Display", "flashBacklight", "(I)Z", native_display_flashBacklight},
        {"javax/microedition/lcdui/Display", "getColor", "(I)I", native_display_getColor},
        {"javax/microedition/lcdui/Display", "getBestImageWidth", "(I)I", native_display_getBestImageWidth},
        {"javax/microedition/lcdui/Display", "getBestImageHeight", "(I)I", native_display_getBestImageHeight},
        {"javax/microedition/lcdui/Display", "getBorderStyle", "(Z)I", native_display_getBorderStyle},
        /* Canvas/Displayable methods */
        {"javax/microedition/lcdui/Canvas", "getWidth", "()I", native_display_getWidth},
        {"javax/microedition/lcdui/Canvas", "getHeight", "()I", native_canvas_getHeight},
        {"javax/microedition/lcdui/Canvas", "getGameAction", "(I)I", native_canvas_getGameAction},
        {"javax/microedition/lcdui/Canvas", "getKeyCode", "(I)I", native_canvas_getKeyCode},
        {"javax/microedition/lcdui/Canvas", "getKeyName", "(I)Ljava/lang/String;", native_canvas_getKeyName},
        {"javax/microedition/lcdui/Canvas", "hasPointerEvents", "()Z", native_canvas_hasPointerEvents},
        {"javax/microedition/lcdui/Canvas", "hasPointerMotionEvents", "()Z", native_canvas_hasPointerMotionEvents},
        {"javax/microedition/lcdui/Canvas", "hasRepeatEvents", "()Z", native_canvas_hasRepeatEvents},
        {"javax/microedition/lcdui/Canvas", "isDoubleBuffered", "()Z", native_canvas_isDoubleBuffered},
        {"javax/microedition/lcdui/Canvas", "setFullScreenMode", "(Z)V", native_canvas_setFullScreenMode},
        {"javax/microedition/lcdui/Canvas", "repaint", "()V", native_canvas_repaint},
        {"javax/microedition/lcdui/Canvas", "repaint", "(IIII)V", native_canvas_repaint_region},
        {"javax/microedition/lcdui/Canvas", "serviceRepaints", "()V", native_canvas_serviceRepaints},
        {"javax/microedition/lcdui/Displayable", "getWidth", "()I", native_display_getWidth},
        {"javax/microedition/lcdui/Displayable", "getHeight", "()I", native_display_getHeight},
        {"javax/microedition/lcdui/Displayable", "isShown", "()Z", native_displayable_isShown},
        {"javax/microedition/lcdui/Displayable", "repaint", "()V", native_canvas_repaint},
        {"javax/microedition/lcdui/Displayable", "repaint", "(IIII)V", native_canvas_repaint_region},
        {"javax/microedition/lcdui/Canvas", "isShown", "()Z", native_displayable_isShown},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
}

void init_javax_microedition_lcdui_game_gamecanvas(JVM* jvm) {
    NativeMethodEntry methods[] = {
        /* GameCanvas */
        {"javax/microedition/lcdui/game/GameCanvas", "getKeyStates", "()I", native_gamecanvas_getKeyStates},
        {"javax/microedition/lcdui/game/GameCanvas", "suppressKeyEvents", "(Z)V", native_gamecanvas_suppressKeyEvents},
        {"javax/microedition/lcdui/game/GameCanvas", "getGraphics", "()Ljavax/microedition/lcdui/Graphics;", native_gamecanvas_getGraphics},
        {"javax/microedition/lcdui/game/GameCanvas", "flushGraphics", "()V", native_gamecanvas_flushGraphics},
        {"javax/microedition/lcdui/game/GameCanvas", "flushGraphics", "(IIII)V", native_gamecanvas_flushGraphicsRegion},
        
        /* Sprite constructors */
        {"javax/microedition/lcdui/game/Sprite", "<init>", "(Ljavax/microedition/lcdui/Image;)V", native_sprite_init_image},
        {"javax/microedition/lcdui/game/Sprite", "<init>", "(Ljavax/microedition/lcdui/Image;II)V", native_sprite_init_frames},
        {"javax/microedition/lcdui/game/Sprite", "<init>", "(Ljavax/microedition/lcdui/game/Sprite;)V", native_sprite_init_copy},
        
        /* Sprite methods */
        {"javax/microedition/lcdui/game/Sprite", "setPosition", "(II)V", native_sprite_setPosition},
        {"javax/microedition/lcdui/game/Sprite", "move", "(II)V", native_sprite_move},
        {"javax/microedition/lcdui/game/Sprite", "paint", "(Ljavax/microedition/lcdui/Graphics;)V", native_sprite_paint},
        {"javax/microedition/lcdui/game/Sprite", "getX", "()I", native_sprite_getX},
        {"javax/microedition/lcdui/game/Sprite", "getY", "()I", native_sprite_getY},
        {"javax/microedition/lcdui/game/Sprite", "getWidth", "()I", native_sprite_getWidth},
        {"javax/microedition/lcdui/game/Sprite", "getHeight", "()I", native_sprite_getHeight},
        {"javax/microedition/lcdui/game/Sprite", "setVisible", "(Z)V", native_sprite_setVisible},
        {"javax/microedition/lcdui/game/Sprite", "isVisible", "()Z", native_sprite_isVisible},
        {"javax/microedition/lcdui/game/Sprite", "setFrame", "(I)V", native_sprite_setFrame},
        {"javax/microedition/lcdui/game/Sprite", "getFrame", "()I", native_sprite_getFrame},
        /* CRITICAL: Sprite collision and transform methods */
        {"javax/microedition/lcdui/game/Sprite", "setTransform", "(I)V", native_sprite_setTransform},
        {"javax/microedition/lcdui/game/Sprite", "getTransform", "()I", native_sprite_getTransform},
        {"javax/microedition/lcdui/game/Sprite", "collidesWith", "(Ljavax/microedition/lcdui/game/Sprite;Z)Z", native_sprite_collidesWith_sprite},
        {"javax/microedition/lcdui/game/Sprite", "collidesWith", "(Ljavax/microedition/lcdui/game/TiledLayer;Z)Z", native_sprite_collidesWith_tiledlayer},
        {"javax/microedition/lcdui/game/Sprite", "collidesWith", "(Ljavax/microedition/lcdui/Image;IIZ)Z", native_sprite_collidesWith_image},
        {"javax/microedition/lcdui/game/Sprite", "defineCollisionRectangle", "(IIII)V", native_sprite_defineCollisionRectangle},
        {"javax/microedition/lcdui/game/Sprite", "setFrameSequence", "([I)V", native_sprite_setFrameSequence},
        {"javax/microedition/lcdui/game/Sprite", "nextFrame", "()V", native_sprite_nextFrame},
        {"javax/microedition/lcdui/game/Sprite", "prevFrame", "()V", native_sprite_prevFrame},
        {"javax/microedition/lcdui/game/Sprite", "getRawFrameCount", "()I", native_sprite_getRawFrameCount},
        {"javax/microedition/lcdui/game/Sprite", "getFrameSequenceLength", "()I", native_sprite_getFrameSequenceLength},
        {"javax/microedition/lcdui/game/Sprite", "defineReferencePixel", "(II)V", native_sprite_defineReferencePixel},
        {"javax/microedition/lcdui/game/Sprite", "setRefPixelPosition", "(II)V", native_sprite_setRefPixelPosition},
        {"javax/microedition/lcdui/game/Sprite", "getRefPixelX", "()I", native_sprite_getRefPixelX},
        {"javax/microedition/lcdui/game/Sprite", "getRefPixelY", "()I", native_sprite_getRefPixelY},
        {"javax/microedition/lcdui/game/Sprite", "setImage", "(Ljavax/microedition/lcdui/Image;II)V", native_sprite_setImage},
        
        /* Layer methods (inherited by Sprite and TiledLayer) */
        {"javax/microedition/lcdui/game/Layer", "getX", "()I", native_layer_getX},
        {"javax/microedition/lcdui/game/Layer", "getY", "()I", native_layer_getY},
        {"javax/microedition/lcdui/game/Layer", "getWidth", "()I", native_layer_getWidth},
        {"javax/microedition/lcdui/game/Layer", "getHeight", "()I", native_layer_getHeight},
        {"javax/microedition/lcdui/game/Layer", "setPosition", "(II)V", native_layer_setPosition},
        {"javax/microedition/lcdui/game/Layer", "move", "(II)V", native_layer_move},
        {"javax/microedition/lcdui/game/Layer", "setVisible", "(Z)V", native_layer_setVisible},
        {"javax/microedition/lcdui/game/Layer", "isVisible", "()Z", native_layer_isVisible},
        
        /* LayerManager */
        {"javax/microedition/lcdui/game/LayerManager", "<init>", "()V", native_layermanager_init},
        {"javax/microedition/lcdui/game/LayerManager", "append", "(Ljavax/microedition/lcdui/game/Layer;)I", native_layermanager_append},
        {"javax/microedition/lcdui/game/LayerManager", "insert", "(Ljavax/microedition/lcdui/game/Layer;I)V", native_layermanager_insert},
        {"javax/microedition/lcdui/game/LayerManager", "remove", "(Ljavax/microedition/lcdui/game/Layer;)V", native_layermanager_remove},
        {"javax/microedition/lcdui/game/LayerManager", "getLayerAt", "(I)Ljavax/microedition/lcdui/game/Layer;", native_layermanager_getLayerAt},
        {"javax/microedition/lcdui/game/LayerManager", "getSize", "()I", native_layermanager_getSize},
        {"javax/microedition/lcdui/game/LayerManager", "paint", "(Ljavax/microedition/lcdui/Graphics;II)V", native_layermanager_paint},
        {"javax/microedition/lcdui/game/LayerManager", "setViewWindow", "(IIII)V", native_layermanager_setViewWindow},
        
        /* TiledLayer */
        {"javax/microedition/lcdui/game/TiledLayer", "<init>", "(IILjavax/microedition/lcdui/Image;II)V", native_tiledlayer_init},
        {"javax/microedition/lcdui/game/TiledLayer", "setCell", "(III)V", native_tiledlayer_setCell},
        {"javax/microedition/lcdui/game/TiledLayer", "getCell", "(II)I", native_tiledlayer_getCell},
        {"javax/microedition/lcdui/game/TiledLayer", "fillCells", "(IIIII)V", native_tiledlayer_fillCells},
        {"javax/microedition/lcdui/game/TiledLayer", "createAnimatedTile", "(I)I", native_tiledlayer_createAnimatedTile},
        {"javax/microedition/lcdui/game/TiledLayer", "setAnimatedTile", "(II)V", native_tiledlayer_setAnimatedTile},
        {"javax/microedition/lcdui/game/TiledLayer", "getAnimatedTile", "(I)I", native_tiledlayer_getAnimatedTile},
        {"javax/microedition/lcdui/game/TiledLayer", "getColumns", "()I", native_tiledlayer_getColumns},
        {"javax/microedition/lcdui/game/TiledLayer", "getRows", "()I", native_tiledlayer_getRows},
        {"javax/microedition/lcdui/game/TiledLayer", "getCellWidth", "()I", native_tiledlayer_getCellWidth},
        {"javax/microedition/lcdui/game/TiledLayer", "getCellHeight", "()I", native_tiledlayer_getCellHeight},
        {"javax/microedition/lcdui/game/TiledLayer", "setStaticTileSet", "(Ljavax/microedition/lcdui/Image;II)V", native_tiledlayer_setStaticTileSet},
        {"javax/microedition/lcdui/game/TiledLayer", "paint", "(Ljavax/microedition/lcdui/Graphics;)V", native_tiledlayer_paint},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
}

void init_javax_microedition_lcdui_image(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"javax/microedition/lcdui/Image", "createImage", "(II)Ljavax/microedition/lcdui/Image;", native_image_createImage},
        {"javax/microedition/lcdui/Image", "createImage", "(Ljava/lang/String;)Ljavax/microedition/lcdui/Image;", native_image_createImage_from_string},
        {"javax/microedition/lcdui/Image", "createImage", "([BII)Ljavax/microedition/lcdui/Image;", native_image_createImage_from_bytes},
        /* CRITICAL: createImage with transform and from Image source */
        {"javax/microedition/lcdui/Image", "createImage", "(Ljavax/microedition/lcdui/Image;IIIII)Ljavax/microedition/lcdui/Image;", native_image_createImage_region},
        {"javax/microedition/lcdui/Image", "createImage", "(Ljavax/microedition/lcdui/Image;)Ljavax/microedition/lcdui/Image;", native_image_createImage_copy},
        {"javax/microedition/lcdui/Image", "createImage", "(Ljava/io/InputStream;)Ljavax/microedition/lcdui/Image;", native_image_createImage_stream},
        {"javax/microedition/lcdui/Image", "createRGBImage", "([IIIZ)Ljavax/microedition/lcdui/Image;", native_image_createRGBImage},
        {"javax/microedition/lcdui/Image", "getWidth", "()I", native_image_getWidth},
        {"javax/microedition/lcdui/Image", "getHeight", "()I", native_image_getHeight},
        {"javax/microedition/lcdui/Image", "isMutable", "()Z", native_image_isMutable},
        /* FIX-19r: descriptor had 6 ints instead of 7 - getRGB(rgb, offset,
         * scanlength, x, y, width, height) is 7 params. The mismatch made
         * every getRGB call resolve to a do-nothing stub (silently zero
         * data), breaking all pixel probing in games. */
        {"javax/microedition/lcdui/Image", "getRGB", "([IIIIIII)V", native_image_getRGB},
        {"javax/microedition/lcdui/Image", "getRGB", "([IIIIII)V", native_image_getRGB},
        {"javax/microedition/lcdui/Image", "getGraphics", "()Ljavax/microedition/lcdui/Graphics;", native_image_getGraphics},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
}

/* Graphics.drawRGB(int[] rgbData, int offset, int scanlength, int x, int y, int width, int height, boolean processAlpha) */
static int g_drawrgb_calls = 0;   /* v34.6 diag: call counter (first 24 + every 600th) */

/* v37 DIAG: ring buffer state for drawRGB (see native_graphics_drawRGB) */
int v37_dump_ring_on = 0;
int v37_dump_registered = 0;
int g_v37_rgb_total_calls = 0;
struct v37_rgb_ent { int no, x, y, w, h, off, scan, blacks, n; };
struct v37_rgb_ent g_v37_rgb_ring[512];
static void v37_dump_rgb_ring(void) {
    int n = g_v37_rgb_total_calls < 512 ? g_v37_rgb_total_calls : 512;
    j2me_log_ungated("[V37-RGB] last %d drawRGB calls (of %d total):\n",
                     n, g_v37_rgb_total_calls);
    for (int i = 0; i < n; i++) {
        struct v37_rgb_ent* e = &g_v37_rgb_ring[i];
        j2me_log_ungated("[V37-RGB] #%d dest=(%d,%d %dx%d) off=%d scan=%d blacks=%d/%d\n",
                         e->no, e->x, e->y, e->w, e->h, e->off, e->scan, e->blacks, e->n);
    }
}

static JavaValue native_graphics_drawRGB(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    /* v38: [V37-DIAG] drawRGB breadcrumb removed — the [V37-RGB] ring dump
     * (NOJME_RGB_RING=1) and NOJME_INVOKE_TRACE=Graphics cover this path. */
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    JavaArray* rgb_array = (JavaArray*)args[1].ref;
    jint offset = args[2].i;
    jint scanlength = args[3].i;
    jint x = args[4].i;
    jint y = args[5].i;
    jint width = args[6].i;
    jint height = args[7].i;
    jint processAlpha = args[8].i;

    int callno = ++g_drawrgb_calls;
    /* v37 DIAG: ring buffer of the last 512 drawRGB calls (geometry + black
     * fraction), dumped at exit (NOJME_RGB_RING=1) — pinpoints which blit
     * paints the black triangle artifact in the Asphalt software race. */
    {
        if (!v37_dump_ring_on && getenv("NOJME_RGB_RING")) {
            v37_dump_ring_on = 1;
            v37_dump_registered = 1;
            atexit(v37_dump_rgb_ring);
        }
        if (v37_dump_ring_on) {
            int blacks = 0, total = 0;
            if (rgb_array && rgb_array->element_type == T_INT && width > 0 && height > 0 &&
                width < 1024 && height < 1024 && scanlength != 0) {
                jint* d = (jint*)array_data(rgb_array);
                for (int yy = 0; yy < height; yy++) {
                    for (int xx = 0; xx < width; xx++) {
                        jlong idx = (jlong)offset + (jlong)yy * scanlength + xx;
                        if (idx >= 0 && idx < (jlong)rgb_array->length) {
                            jint p = d[idx];
                            total++;
                            if (((p >> 16) & 0xFF) < 12 && ((p >> 8) & 0xFF) < 12 && (p & 0xFF) < 12) blacks++;
                        }
                    }
                }
            }
            static int v37ri = 0;
            int slot = v37ri % 512;
            g_v37_rgb_ring[slot].no = callno;
            g_v37_rgb_ring[slot].x = (int)x;
            g_v37_rgb_ring[slot].y = (int)y;
            g_v37_rgb_ring[slot].w = (int)width;
            g_v37_rgb_ring[slot].h = (int)height;
            g_v37_rgb_ring[slot].off = (int)offset;
            g_v37_rgb_ring[slot].scan = (int)scanlength;
            g_v37_rgb_ring[slot].blacks = blacks;
            g_v37_rgb_ring[slot].n = total;
            v37ri++;
            g_v37_rgb_total_calls = v37ri;
        }
    }
    if (callno <= 8 || (callno % 3600) == 0) {
        /* v34.6: print the Java caller chain (top 3 frames) to see which
         * game method drives the blit and with which translate state. */
        const char* c1 = "?" ; const char* c2 = ""; const char* c3 = "";
        if (thread) {
            JavaFrame* f = thread->current_frame; /* deepest = native's caller's parent chain */
            JavaFrame* chain[4]; int cn = 0;
            JavaFrame* it = f;
            while (it && cn < 4) { chain[cn++] = it; it = it->prev; }
            /* chain[0] = frame of the native dispatch (Graphics.drawRGB stub
             * frame) or the invoking frame depending on call path; print
             * the first three with names. */
            if (cn > 1 && chain[1]->clazz && chain[1]->clazz->class_name)
                c1 = chain[1]->clazz->class_name, c2 = chain[1]->method && chain[1]->method->name ? chain[1]->method->name : "";
            if (cn > 2 && chain[2]->clazz && chain[2]->clazz->class_name)
                c3 = chain[2]->clazz->class_name;
        }
        fprintf(stderr, "[DRAWRGB] #%d arr=%p len=%d off=%d scan=%d x=%d y=%d w=%d h=%d alpha=%d caller=%s.%s up=%s\n",
                callno, (void*)rgb_array,
                rgb_array ? rgb_array->length : -1,
                offset, scanlength, x, y, width, height, processAlpha,
                c1, c2, c3);
    }

    if (!gfx_obj) {
        if (callno <= 8) fprintf(stderr, "[DRAWRGB] #%d SKIP: gfx_obj NULL\n", callno);
        return NATIVE_RETURN_VOID();
    }

    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    if (!gfx) {
        if (callno <= 8) fprintf(stderr, "[DRAWRGB] #%d SKIP: no native gfx peer\n", callno);
        return NATIVE_RETURN_VOID();
    }
    if (callno <= 4 || (callno % 3600) == 0) {
        fprintf(stderr, "[DRAWRGB] #%d gfx=%p px=%p %dx%d clip=(%d,%d %dx%d) tr=(%d,%d)\n",
                callno, (void*)gfx, (void*)gfx->pixels, gfx->width, gfx->height,
                gfx->clip_x, gfx->clip_y, gfx->clip_width, gfx->clip_height,
                gfx->translate_x, gfx->translate_y);
    }

    if (!rgb_array || width <= 0 || height <= 0) return NATIVE_RETURN_VOID();

    /* [BT-CRASH-FIX] pass the source array length down: midp_blit_rgb's
     * read loop (srcRow = offset + row*scanlength + startX) had NO bounds
     * validation — a drawRGB window exceeding the int[] length read past
     * the array into adjacent heap blocks (garbage pixels), and with a
     * huge/negative scanlength could leave the heap arena entirely
     * (unmapped page -> direct Data Abort on Switch). The blit now skips
     * out-of-bounds source indices (same clamp discipline as getRGB). */
    if (rgb_array->element_type != T_INT) return NATIVE_RETURN_VOID();

    jint* rgb_data = (jint*)array_data(rgb_array);
    if (!rgb_data) return NATIVE_RETURN_VOID();

    /* v34.20: pixel stage factored into render.c midp_blit_rgb()
     * (clip + opaque/alpha blit loops live in the render module now).
     * [BT-CRASH-FIX] src_len passed so the blit skips out-of-bounds
     * source indices (see render.c). */
    midp_blit_rgb(gfx, rgb_data, rgb_array->length, offset, scanlength,
                  x, y, width, height, processAlpha);

    return NATIVE_RETURN_VOID();
}

void init_javax_microedition_lcdui_graphics(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"javax/microedition/lcdui/Graphics", "setColor", "(I)V", native_graphics_setColor},
        {"javax/microedition/lcdui/Graphics", "setColor", "(III)V", native_graphics_setColorRGB},
        {"javax/microedition/lcdui/Graphics", "getColor", "()I", native_graphics_getColor},
        {"javax/microedition/lcdui/Graphics", "setFont", "(Ljavax/microedition/lcdui/Font;)V", native_graphics_setFont},
        {"javax/microedition/lcdui/Graphics", "drawLine", "(IIII)V", native_graphics_drawLine},
        {"javax/microedition/lcdui/Graphics", "fillRect", "(IIII)V", native_graphics_fillRect},
        {"javax/microedition/lcdui/Graphics", "drawRect", "(IIII)V", native_graphics_drawRect},
        {"javax/microedition/lcdui/Graphics", "fillRoundRect", "(IIIIII)V", native_graphics_fillRoundRect},
        {"javax/microedition/lcdui/Graphics", "drawRoundRect", "(IIIIII)V", native_graphics_drawRoundRect},
        {"javax/microedition/lcdui/Graphics", "fillArc", "(IIIIII)V", native_graphics_fillArc},
        {"javax/microedition/lcdui/Graphics", "drawArc", "(IIIIII)V", native_graphics_drawArc},
        {"javax/microedition/lcdui/Graphics", "drawString", "(Ljava/lang/String;III)V", native_graphics_drawString},
        /* v34.8 FIX (Bounce Tales): рисование текста частями строки */
        {"javax/microedition/lcdui/Graphics", "drawSubstring", "(Ljava/lang/String;IIIII)V", native_graphics_drawSubstring},
        /* v35: API-completeness */
        {"javax/microedition/lcdui/Graphics", "drawChar", "(III)V", native_graphics_drawChar},
        {"javax/microedition/lcdui/Graphics", "drawChars", "([CIIIII)V", native_graphics_drawChars},
        {"javax/microedition/lcdui/Graphics", "getFont", "()Ljavax/microedition/lcdui/Font;", native_graphics_getFont},
        {"javax/microedition/lcdui/Graphics", "getRedComponent", "()I", native_graphics_getRedComponent},
        {"javax/microedition/lcdui/Graphics", "getGreenComponent", "()I", native_graphics_getGreenComponent},
        {"javax/microedition/lcdui/Graphics", "getBlueComponent", "()I", native_graphics_getBlueComponent},
        /* v34.8 FIX (Bounce Tales): заливка треугольника */
        {"javax/microedition/lcdui/Graphics", "fillTriangle", "(IIIIII)V", native_graphics_fillTriangle},
        {"javax/microedition/lcdui/Graphics", "drawImage", "(Ljavax/microedition/lcdui/Image;III)V", native_graphics_drawImage},
        /* CRITICAL: drawRegion - used by most games for sprite rendering */
        {"javax/microedition/lcdui/Graphics", "drawRegion", "(Ljavax/microedition/lcdui/Image;IIIIIIII)V", native_graphics_drawRegion},
        /* Additional drawing methods */
        {"javax/microedition/lcdui/Graphics", "drawImage", "(Ljavax/microedition/lcdui/Image;IIII)V", native_graphics_drawImage_anchor},
        {"javax/microedition/lcdui/Graphics", "copyArea", "(IIIIIII)V", native_graphics_copyArea},
        /* Clip methods */
        {"javax/microedition/lcdui/Graphics", "setClip", "(IIII)V", native_graphics_setClip},
        {"javax/microedition/lcdui/Graphics", "clipRect", "(IIII)V", native_graphics_clipRect},
        {"javax/microedition/lcdui/Graphics", "getClipX", "()I", native_graphics_getClipX},
        {"javax/microedition/lcdui/Graphics", "getClipY", "()I", native_graphics_getClipY},
        {"javax/microedition/lcdui/Graphics", "getClipWidth", "()I", native_graphics_getClipWidth},
        {"javax/microedition/lcdui/Graphics", "getClipHeight", "()I", native_graphics_getClipHeight},
        /* Translate methods */
        {"javax/microedition/lcdui/Graphics", "translate", "(II)V", native_graphics_translate},
        {"javax/microedition/lcdui/Graphics", "getTranslateX", "()I", native_graphics_getTranslateX},
        {"javax/microedition/lcdui/Graphics", "getTranslateY", "()I", native_graphics_getTranslateY},
        /* Stroke style */
        {"javax/microedition/lcdui/Graphics", "setStrokeStyle", "(I)V", native_graphics_setStrokeStyle},
        {"javax/microedition/lcdui/Graphics", "getStrokeStyle", "()I", native_graphics_getStrokeStyle},
        /* Gray scale */
        {"javax/microedition/lcdui/Graphics", "setGrayScale", "(I)V", native_graphics_setGrayScale},
        {"javax/microedition/lcdui/Graphics", "getGrayScale", "()I", native_graphics_getGrayScale},
        /* Display color */
        {"javax/microedition/lcdui/Graphics", "getDisplayColor", "(I)I", native_graphics_getDisplayColor},
        /* drawRGB - pixel-level rendering.
         * v34.6 FIX: descriptor was "([IIIIIZ)V" - that parses as
         * int[] + 4*int + boolean = 6 params, one int[] too few!
         * The real MIDP 2.0 signature is
         *   drawRGB(int[] rgbData, int offset, int scanlength, int x, int y,
         *           int width, int height, boolean processAlpha)
         *   = int[] + 6*int + boolean = 8 params => "([IIIIIIIZ)V".
         * With the broken descriptor the native lookup never matched, the
         * call silently fell into the stub no-op path and Doom RPG's 3D
         * viewport (raycaster -> drawRGB -> flushGraphics) never rendered -
         * the loading screen stayed on screen forever. */
        {"javax/microedition/lcdui/Graphics", "drawRGB", "([IIIIIIIZ)V", native_graphics_drawRGB},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
}

void init_javax_microedition_lcdui_font(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"javax/microedition/lcdui/Font", "getDefaultFont", "()Ljavax/microedition/lcdui/Font;", native_font_getDefaultFont},
        {"javax/microedition/lcdui/Font", "getFont", "(III)Ljavax/microedition/lcdui/Font;", native_font_getFont},
        {"javax/microedition/lcdui/Font", "stringWidth", "(Ljava/lang/String;)I", native_font_stringWidth},
        {"javax/microedition/lcdui/Font", "getHeight", "()I", native_font_height},
        /* Additional Font methods */
        {"javax/microedition/lcdui/Font", "charWidth", "(C)I", native_font_charWidth},
        {"javax/microedition/lcdui/Font", "charsWidth", "([CII)I", native_font_charsWidth},
        {"javax/microedition/lcdui/Font", "substringWidth", "(Ljava/lang/String;II)I", native_font_substringWidth},
        {"javax/microedition/lcdui/Font", "getBaselinePosition", "()I", native_font_getBaselinePosition},
        {"javax/microedition/lcdui/Font", "getFace", "()I", native_font_getFace},
        {"javax/microedition/lcdui/Font", "getStyle", "()I", native_font_getStyle},
        {"javax/microedition/lcdui/Font", "getSize", "()I", native_font_getSize},
        {"javax/microedition/lcdui/Font", "isBold", "()Z", native_font_isBold},
        {"javax/microedition/lcdui/Font", "isItalic", "()Z", native_font_isItalic},
        {"javax/microedition/lcdui/Font", "isUnderlined", "()Z", native_font_isUnderlined},
        {"javax/microedition/lcdui/Font", "isPlain", "()Z", native_font_isPlain},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
}


/* =========================================================================
 * v17: DeviceControl / Clipboard / user-activity support
 *
 * Backend-neutral core used by com.nokia.mid.ui.DeviceControl and
 * com.nokia.mid.ui.Clipboard natives. Platforms plug in real effects:
 *  - libretro: retro_rumble_interface for vibra, no system clipboard
 *  - SDL app:  SDL clipboard, haptic when available
 * Inactivity tracking is driven by midp_input_activity_mark() calls from
 * the platform input paths (libretro key polling / SDL event loop).
 * ========================================================================= */

#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>


static uint64_t devicecontrol_now_ms(void) {
#if defined(_WIN32)
    LARGE_INTEGER freq, counter;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return (uint64_t)(counter.QuadPart * 1000000ULL / (uint64_t)freq.QuadPart) / 1000ULL;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
#endif
}

/* ---- User inactivity ---- */

static uint64_t g_last_input_activity_ms = 0;
static uint64_t g_inactivity_reset_base_ms = 0;

void midp_input_activity_mark(void) {
    g_last_input_activity_ms = devicecontrol_now_ms();
}

/* Milliseconds since the last user input (or last reset), capped at 1 day */
jlong midp_input_inactivity_ms(void) {
    uint64_t now = devicecontrol_now_ms();
    if (g_last_input_activity_ms == 0) return 0; /* no input seen yet */
    if (g_inactivity_reset_base_ms > g_last_input_activity_ms) {
        return (jlong)(now - g_inactivity_reset_base_ms);
    }
    return (jlong)(now - g_last_input_activity_ms);
}

void midp_input_inactivity_reset(void) {
    g_inactivity_reset_base_ms = devicecontrol_now_ms();
}

/* ---- Vibra (backend hook) ---- */

static void (*g_vibra_start_hook)(int freq, int duration_ms) = NULL;
static void (*g_vibra_stop_hook)(void) = NULL;

void midp_set_vibra_hooks(void (*start_fn)(int freq, int duration_ms),
                          void (*stop_fn)(void)) {
    g_vibra_start_hook = start_fn;
    g_vibra_stop_hook = stop_fn;
}

bool midp_vibra_start(int freq, int duration_ms) {
    if (g_vibra_start_hook) {
        g_vibra_start_hook(freq, duration_ms);
        return true;
    }
    return false;
}

bool midp_vibra_stop(void) {
    if (g_vibra_stop_hook) {
        g_vibra_stop_hook();
        return true;
    }
    return false;
}

/* ---- Backlight state (best-effort emulation) ---- */

static int g_lights_level[4] = {0, 0, 0, 0};

int midp_lights_set(int num, int level) {
    if (num < 0 || num >= (int)(sizeof(g_lights_level) / sizeof(g_lights_level[0]))) {
        return -1;
    }
    if (level < 0 || level > 100) return -1;
    g_lights_level[num] = level;
    return 0;
}

int midp_lights_get(int num) {
    if (num < 0 || num >= (int)(sizeof(g_lights_level) / sizeof(g_lights_level[0]))) {
        return -1;
    }
    return g_lights_level[num];
}

/* ---- Clipboard (internal buffer + optional backend bridge) ---- */

#define MIDP_CLIPBOARD_MAX 4096
static char g_clipboard[MIDP_CLIPBOARD_MAX];
static bool g_clipboard_valid = false;

static void (*g_clipboard_set_hook)(const char* text) = NULL;
static char* (*g_clipboard_get_hook)(void) = NULL;

void midp_set_clipboard_hooks(void (*set_fn)(const char* text),
                              char* (*get_fn)(void)) {
    g_clipboard_set_hook = set_fn;
    g_clipboard_get_hook = get_fn;
}

void midp_clipboard_copy(const char* text) {
    if (!text) { g_clipboard_valid = false; g_clipboard[0] = '\0'; }
    else {
        strncpy(g_clipboard, text, MIDP_CLIPBOARD_MAX - 1);
        g_clipboard[MIDP_CLIPBOARD_MAX - 1] = '\0';
        g_clipboard_valid = true;
    }
    /* Forward to the platform clipboard when a backend provides one */
    if (g_clipboard_set_hook) g_clipboard_set_hook(g_clipboard);
}

const char* midp_clipboard_paste(void) {
    /* Prefer the platform clipboard when a backend provides one */
    if (g_clipboard_get_hook) {
        char* sys = g_clipboard_get_hook();
        if (sys) {
            strncpy(g_clipboard, sys, MIDP_CLIPBOARD_MAX - 1);
            g_clipboard[MIDP_CLIPBOARD_MAX - 1] = '\0';
            g_clipboard_valid = true;
            free(sys);
        }
    }
    return g_clipboard_valid ? g_clipboard : NULL;
}

/* ============================================================================
 * v35.08 MULTI-SESSION: per-session static reset (called from
 * midp_session_reset() <- jvm_destroy, VM threads already quiet).
 *
 * The Switch frontend runs game after game in ONE process. Every static
 * below pointed into the PREVIOUS session's Java heap; the central
 * gc_roots_reset_all() (jvm_destroy) NULLs the ROOT-REGISTERED ones
 * mechanically, but this layer also keeps unrooted object pointers,
 * "already rooted" latches, queue indices and UI booleans that must start
 * clean or the next game inherits the previous game's state (field-repro:
 * game two died in Display.setCurrent on a stale Display singleton; the
 * latches would also have skipped re-rooting, letting the next session's
 * GC sweep live UI objects).
 * ============================================================================ */
void midp_display_session_reset(void) {
    /* Paint/display singletons (root-registered: nulled by the root wipe
     * too - belt and braces for builds where they were unrooted). */
    g_graphics_object = NULL;
    current_displayable_obj = NULL;
    g_display_instance = NULL;
    g_default_font_object = NULL;
    g_pending_shownotify_displayable = NULL;   /* unrooted - was dangling */
    g_display_select_command = NULL;           /* unrooted - was dangling */

    /* Fullscreen property belongs to a session object that no longer exists */
    g_full_screen_mode = false;
    g_fullscreen_canvas_obj = NULL;            /* unrooted - was dangling */

    /* Command (soft-key) menu state */
    g_command_menu_open = false;
    g_command_menu_selected = 0;

    /* v35.09: GameCanvas key state belongs to the old session — a stale
     * held/latched bit from the previous game must not leak into the next.
     * (Session teardown: no delivery threads running — plain stores OK.) */
    game_canvas.key_latch = 0;
    game_canvas.key_level = 0;

    /* v36.12 PEER-REGISTRY: drop the session's GameCanvas peer pointers
     * BEFORE the sweep in midp_peer_session_reset() (called last from
     * midp_session_reset) frees the registered peers themselves. Without
     * this the next session's getGraphics would read freed memory through
     * the stale game_canvas.graphics/offscreen_buffer. The buffer/context
     * are re-created on demand next session (existing ensure paths). */
    game_canvas.graphics = NULL;
    game_canvas.offscreen_buffer = NULL;
    game_canvas.offscreen_width = 0;
    game_canvas.offscreen_height = 0;

    /* Emulated BACK stack: object pointers + "already rooted" latches */
    for (int i = 0; i < MIDP_BACK_STACK_MAX; i++) {
        g_back_stack[i] = NULL;
        g_back_rooted[i] = false;
    }
    g_back_depth = 0;

    /* Alert follow-up screen + its root latch */
    g_alert_next_displayable = NULL;
    g_alert_next_rooted = false;

    /* callSerially queue: indices, contents, and the roots latch */
    for (int i = 0; i < CALLSERIALLY_QUEUE_SIZE; i++)
        g_call_serially_queue.runnables[i] = NULL;
    g_call_serially_queue.head = 0;
    g_call_serially_queue.tail = 0;
    g_call_serially_queue.count = 0;
    g_call_serially_queue.gc_registered = false;

    /* Deferred key queue: never leak key events across games */
    pthread_mutex_lock(&midp_keyq_mutex);
    midp_keyq_head = 0;
    midp_keyq_tail = 0;
    pthread_mutex_unlock(&midp_keyq_mutex);

    /* v34.9: the Display-owning VM thread of the previous session is gone */
    midp_game_thread_id = -1;

    /* v35.13 CRITICAL (field: "на экране отображался экран предыдущего
     * мидлета"): the settled snapshot belongs to the ENDED session. It used
     * to survive the teardown, so the presenter booted the NEXT session from
     * the PREVIOUS midlet's last frame and kept showing it until the new
     * game completed its first settled frame — on a VM-paced loader that is
     * tens of seconds of a frozen old screen (looks like "the game did not
     * load"). Reset the snapshot so the presenter starts from the loading
     * screen instead. Same thread as the presenter (frontend teardown) —
     * the lock is belt and braces. */
    {
        uint32_t was_seq = g_midp_stable_seq;
        stable_lock_bounded("session-reset");
        free(g_midp_stable_pixels);
        g_midp_stable_pixels = NULL;
        g_midp_stable_w = 0;
        g_midp_stable_h = 0;
        g_midp_stable_seq = 0;
        g_stable_last_settle_ms = 0;
        stable_unlock();
        /* device-log breadcrumb (v35.12 pattern: weak sw_trace_force) */
        {
            extern void sw_trace_force(const char* fmt, ...)
                __attribute__((weak));
            if (sw_trace_force)
                sw_trace_force("session-reset: settled frame cleared (was seq %u)",
                               (unsigned)was_seq);
        }
    }

    /* v35.13: the key-hang watchdog daemon lives for the whole process; its
     * thread pointer must never stay aimed into the destroyed heap. */
    g_key_hang_thread = NULL;
    g_key_hang_start_ms = 0;
    g_key_hang_dumped = 0;
}
