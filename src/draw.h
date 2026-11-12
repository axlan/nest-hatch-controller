/* Color math and anti-aliased drawing primitives into `frame`. */
#ifndef NEST_PICKER_DRAW_H
#define NEST_PICKER_DRAW_H

#include <stdint.h>

void hsv_to_rgb(float h, float s, float v, int *r, int *g, int *b);

void put(int x, int y, uint32_t c);
void put_a(int x, int y, uint32_t c, float a);
void fill_circle(float cx, float cy, float r, uint32_t c);
void draw_ring(float cx, float cy, float r, float w, uint32_t c);
void draw_line(float ax, float ay, float bx, float by, float w, uint32_t c);  /* round caps */
void fill_poly(const float *pts, int n, uint32_t c);                          /* pts: x,y pairs */
/* Annulus around the screen centre, coloured by angle (tenths of degrees, 0 = top). */
void fill_arc(float r_in, float r_out, uint32_t (*color_at)(int tenths));

int text_width(const char *s, int scale);
void draw_text(int x, int y, const char *s, int scale, uint32_t c);
void draw_text_centered(int y, const char *s, int scale, uint32_t c);

#endif
