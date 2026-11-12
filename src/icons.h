/* Vector sound icons. */
#ifndef NEST_PICKER_ICONS_H
#define NEST_PICKER_ICONS_H

#include <stdint.h>

enum icon {
    ICON_NONE, ICON_STREAM, ICON_NOISE, ICON_DRYER, ICON_OCEAN, ICON_WIND,
    ICON_RAIN, ICON_CRICKETS, ICON_LULLABY, ICON_TWINKLE, ICON_ROCKABYE
};

/* Draws `icon` centred at (cx, cy), `half` px from centre to edge; `bg` is the colour behind it. */
void draw_icon(enum icon icon, float cx, float cy, float half, uint32_t fg, uint32_t bg);

#endif
