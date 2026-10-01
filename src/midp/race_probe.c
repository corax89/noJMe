/* ============================================================================
 * v36.59 [RACE] probe — Rally 3D «гонка после загрузки сохранения: видна
 * только модель машины» (field snaps.zip, build v36.58).
 *
 * ПОЛЕВОЙ ФАКТ (snaps 0-80 меню/loading = полная сцена; 81+ гонка = чёрный
 * канвас + машина + HUD, ни неба, ни дороги, ни билбордов):
 *   внутри h.d(Graphics) НЕ рисуются 2D-небо (drawImage+fillRect) И меши
 *   дороги, НО машина (M3G) и HUD (после setClip(0,0,d,u)) рисуются.
 *   Единственная структурная разница внутри метода — ВЬЮПОРТ-клип
 *   (cg,ce,C,at): небо/дорога рисуются под setClip(cg,ce,C,at), HUD — под
 *   полным клипом; M3G-вьюпорт при вырожденном клипе ОТКАТЫВАЕТСЯ к полному
 *   канвасу (m3g_context_init сбрасывает, гвард clip-adoption отвергает
 *   вырожденный) — машина потому и видна.
 *
 * Прибор пишет В ЗНАЧЕНИЯ в момент вызова (в log.txt через sw_trace_force
 * + stderr на хосте): setViewport/setClip (значения!), fillRect/drawImage
 * (первые N + счётчики), каждый bindTarget (клип/вьюпорт/размер цели/канва
 * до сида), сид (небол. пикселей), немедленные render(vb,ib,app,t)
 * (vertexCount/tris), releaseTarget (после блита), flushGraphics (небол.
 * пикселей канвы + счётчики 2D-операций с прошлого flush).
 *
 * Гейт: NOJME_RACE_PROBE=1 (env) или пустой файл-флаг
 * sdmc:/switch/j2me/race.flag (DIAG-FLAGS в main.c). Работает на хосте и
 * на устройстве одинаково.
 * ============================================================================ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

#include "race_probe.h"

/* switch_trace.c — ТОЛЬКО в сборках switch-семейства (switchui/verify);
 * headless-цели её не линкуют, поэтому ссылка СЛАБАЯ (как у [TEXT] в
 * render.c): там, где функции нет, зонд пишет только в stderr. */
extern void sw_trace_force(const char* fmt, ...) __attribute__((weak));

static int  s_on = -1;      /* -1 = не проверен */
static int  s_shutdown = 0;
static long s_setclip_n = 0;
static long s_vp_n = 0;
static long s_bind_n = 0;
static long s_seed_n = 0;
static long s_rel_n = 0;
static long s_imm_n = 0;
static long s_fill_n = 0;
static long s_fill_log = 0;
static long s_img_n = 0;
static long s_img_log = 0;
static long s_flush_n = 0;
static long s_last_flush_fill = 0, s_last_flush_img = 0;
static long s_imm_since_flush = 0, s_bind_since_flush = 0;

int race_probe_on(void) {
    if (s_on < 0) {
        const char* e = getenv("NOJME_RACE_PROBE");
        s_on = (e && e[0] && e[0] != '0') ? 1 : 0;
        if (s_on) {
            if (&sw_trace_force && sw_trace_force)
                sw_trace_force("[RACE] probe enabled (NOJME_RACE_PROBE)\n");
            fprintf(stderr, "[RACE] probe enabled\n");
        }
    }
    return s_on && !s_shutdown;
}

void race_probe_off(void) { s_shutdown = 1; }

static void rp_log(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (&sw_trace_force && sw_trace_force) sw_trace_force("%s", buf);
    fprintf(stderr, "%s", buf);
}

/* --- выборочная оценка «небелых» пикселей (шаг 8 по обеим осям) --------- */
static unsigned sample_nonblack(const uint32_t* px, int w, int h) {
    if (!px || w <= 0 || h <= 0) return 0xFFFFFFFFu; /* unknown */
    unsigned hits = 0, total = 0;
    for (int y = 0; y < h; y += 8) {
        const uint32_t* row = px + (size_t)y * w;
        for (int x = 0; x < w; x += 8) {
            uint32_t p = row[x];
            total++;
            if ((p & 0x00FFFFFFu) != 0) hits++;
        }
    }
    if (!total) return 0xFFFFFFFFu;
    /* normalize to per-1024-sample density for compact printing */
    return (unsigned)((uint64_t)hits * 1024u / total);
}

/* --- Graphics.setClip ----------------------------------------------------- */
void race_probe_setclip(int x, int y, int w, int h,
                        int clip_x, int clip_y, int clip_w, int clip_h,
                        int gw, int gh, int tr_x, int tr_y) {
    if (!race_probe_on()) return;
    if (s_setclip_n < 600) {
        s_setclip_n++;
        rp_log("[RACE] g:setClip(%d,%d %dx%d) tr=(%d,%d) gfx=%dx%d -> clip=(%d,%d %dx%d)\n",
               x, y, w, h, tr_x, tr_y, gw, gh, clip_x, clip_y, clip_w, clip_h);
    }
}

/* --- Graphics.fillRect / drawImage --------------------------------------- */
void race_probe_fillrect(int x, int y, int w, int h, int argb) {
    if (!race_probe_on()) return;
    s_fill_n++;
    s_last_flush_fill++;
    if (s_fill_log < 200) {
        s_fill_log++;
        rp_log("[RACE] g:fillRect(%d,%d %dx%d) color=0x%08X\n", x, y, w, h,
               (unsigned)argb);
    }
}

void race_probe_drawimage(int x, int y, int anchor) {
    if (!race_probe_on()) return;
    s_img_n++;
    s_last_flush_img++;
    if (s_img_log < 200) {
        s_img_log++;
        rp_log("[RACE] g:drawImage(%d,%d anchor=%d)\n", x, y, anchor);
    }
}

/* --- Graphics3D.setViewport ---------------------------------------------- */
void race_probe_viewport(int x, int y, int w, int h) {
    if (!race_probe_on()) return;
    if (s_vp_n < 600) {
        s_vp_n++;
        rp_log("[RACE] m3d:setViewport(%d,%d %dx%d)\n", x, y, w, h);
    }
}

/* --- bindTarget: состояние цели ДО сида ---------------------------------- */
void race_probe_bind(int gfx_w, int gfx_h, int clip_x, int clip_y,
                     int clip_w, int clip_h, int vp_x, int vp_y,
                     int vp_w, int vp_h, unsigned canvas_nonblack) {
    if (!race_probe_on()) return;
    s_bind_n++;
    s_bind_since_flush++;
    if (s_bind_n <= 1200) {
        rp_log("[RACE] bind#%ld: gfx=%dx%d clip=(%d,%d %dx%d) vp=(%d,%d %dx%d) canvas_nb=%u/1024\n",
               s_bind_n, gfx_w, gfx_h, clip_x, clip_y, clip_w, clip_h,
               vp_x, vp_y, vp_w, vp_h, canvas_nonblack);
    }
}

/* --- сид цветового буфера ------------------------------------------------ */
void race_probe_seed(const uint32_t* buf, int w, int h) {
    if (!race_probe_on()) return;
    s_seed_n++;
    if (s_seed_n <= 1200) {
        rp_log("[RACE] seed#%ld: buf_nb=%u/1024 (%dx%d)\n", s_seed_n,
               sample_nonblack(buf, w, h), w, h);
    }
}

/* --- немедленный render(vb,ib,app,t) ------------------------------------- */
void race_probe_imm(int vertex_count, int tri_count, float tx, float ty,
                    float tz) {
    if (!race_probe_on()) return;
    s_imm_n++;
    s_imm_since_flush++;
    if (s_imm_n <= 3000) {
        rp_log("[RACE] imm#%ld: verts=%d tris=%d t=(%.1f,%.1f,%.1f)\n",
               s_imm_n, vertex_count, tri_count, tx, ty, tz);
    }
}

/* --- releaseTarget: буфер после блита ------------------------------------ */
void race_probe_release(const uint32_t* buf, int w, int h) {
    if (!race_probe_on()) return;
    s_rel_n++;
    if (s_rel_n <= 1200) {
        rp_log("[RACE] rel#%ld: buf_nb=%u/1024 (%dx%d)\n", s_rel_n,
               sample_nonblack(buf, w, h), w, h);
    }
}

/* --- flushGraphics: итог кадра -------------------------------------------- */
void race_probe_flush(int w, int h, const uint32_t* pixels) {
    if (!race_probe_on()) return;
    s_flush_n++;
    rp_log("[RACE] flush#%ld: %dx%d canvas_nb=%u/1024 2d[fill=%ld img=%ld] m3g[bind=%ld imm=%ld]\n",
           s_flush_n, w, h, sample_nonblack(pixels, w, h),
           s_last_flush_fill, s_last_flush_img,
           s_bind_since_flush, s_imm_since_flush);
    s_last_flush_fill = 0;
    s_last_flush_img = 0;
    s_imm_since_flush = 0;
    s_bind_since_flush = 0;
}
