/* Framebuffer, pixel format, backlight and the shared frame/geometry buffers. */
#ifndef NEST_PICKER_DISPLAY_H
#define NEST_PICKER_DISPLAY_H

#include <stdint.h>

#define W 320
#define H 320
#define CX 159.5f
#define CY 159.5f

extern uint32_t *frame;       /* what we compose into */
extern uint32_t *background;  /* static hue ring, pre-rendered */
extern uint16_t *angle_tbl;   /* angle of each pixel, 0..3599 (tenths of a degree), 0 = top, clockwise */
extern float *radius_tbl;     /* distance of each pixel from the centre */
extern int disp_backlight;    /* level for the current mode; -1 = BACKLIGHT */

uint32_t rgb(int r, int g, int b);
void unpack(uint32_t c, int *r, int *g, int *b);
uint32_t blend(uint32_t dst, uint32_t src, float a);

int alloc_buffers(void);
void build_tables(void);
int fb_open(void);
void fb_present(void);      /* copy `frame` to the screen */
void display_power(int on); /* backlight + blanking */

#endif
