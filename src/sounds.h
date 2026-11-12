/* Hatch Rest gen 1 sounds offered on the sound page. */
#ifndef NEST_PICKER_SOUNDS_H
#define NEST_PICKER_SOUNDS_H

#include "icons.h"

struct sound {
    int id;
    const char *name;   /* MQTT name (matches pyhatchbabyrest) */
    const char *label;  /* on-screen */
    enum icon icon;
};

#define NSOUNDS 11
extern const struct sound sounds[NSOUNDS];

int sound_index_for_id(int id);  /* index into sounds[], 0 (none) if unknown */

#endif
