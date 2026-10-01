/* v36.59 [RACE] probe — см. src/midp/race_probe.c */
#ifndef NOJME_RACE_PROBE_H
#define NOJME_RACE_PROBE_H

#include <stdint.h>

int  race_probe_on(void);
void race_probe_off(void);
void race_probe_setclip(int x, int y, int w, int h,
                        int clip_x, int clip_y, int clip_w, int clip_h,
                        int gw, int gh, int tr_x, int tr_y);
void race_probe_fillrect(int x, int y, int w, int h, int argb);
void race_probe_drawimage(int x, int y, int anchor);
void race_probe_viewport(int x, int y, int w, int h);
void race_probe_bind(int gfx_w, int gfx_h, int clip_x, int clip_y,
                     int clip_w, int clip_h, int vp_x, int vp_y,
                     int vp_w, int vp_h, unsigned canvas_nonblack);
void race_probe_seed(const uint32_t* buf, int w, int h);
void race_probe_imm(int vertex_count, int tri_count, float tx, float ty,
                    float tz);
void race_probe_release(const uint32_t* buf, int w, int h);
void race_probe_flush(int w, int h, const uint32_t* pixels);

#endif
