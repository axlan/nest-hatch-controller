/* Timezone, night hours and the night animation (python/ gif2anim format). */
#ifndef NEST_PICKER_NIGHT_H
#define NEST_PICKER_NIGHT_H

#include <stdint.h>

extern int anim_frames;  /* 0 = no animation loaded */

void setup_time_and_night(void);  /* apply TIMEZONE, log local time, load NIGHT_ANIM */
int anim_load(const char *path);
int anim_delay(int i);            /* ms to show frame i */
void render_anim_frame(int i);
int is_night(void);               /* local time inside NIGHT_MODE */

#endif
