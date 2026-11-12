/* Picker UI state, page rendering and publishing the selection. */
#ifndef NEST_PICKER_UI_H
#define NEST_PICKER_UI_H

#include <stdint.h>

#define HUE_R_IN     134.0f
#define HUE_R_OUT    157.0f
#define SWATCH_R     100.0f
#define MARKER_R     ((HUE_R_IN + HUE_R_OUT) / 2)
#define SOUND_RING_R 134.0f

enum mode { MODE_HUE, MODE_SOUND, MODE_NIGHT };

struct ui_state {
    int h;                  /* 0..359; saturation and brightness are fixed at 100% */
    float hf;               /* fractional accumulator behind h */
    int sound;              /* index into sounds[] */
    int sound_acc;          /* ring counts towards the next sound step */
    enum mode mode;
    int64_t flash_until;    /* "SENT" confirmation */
    int mqtt_ok;
    int anim_idx;           /* night animation frame on screen */
    int64_t anim_next;      /* when to advance to the next frame */
};

extern struct ui_state ui;

uint32_t hue_at(int tenths);
void build_background(void);
void current_rgb(int *r, int *g, int *b);
void render(void);         /* draw the current page (or night frame) and present it */
void publish_color(void);  /* queue <topic>/color for MQTT */
void publish_sound(void);  /* queue <topic>/sound for MQTT */

#endif
