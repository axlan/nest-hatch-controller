#include "sounds.h"

/* ------------------------------------------------------------------------ */
/* Sounds (Hatch Rest gen 1 ids; bird chirps (9) intentionally left out)    */
/* ------------------------------------------------------------------------ */

const struct sound sounds[] = {
    {  0, "none",     "NONE",        ICON_NONE },
    {  2, "stream",   "STREAM",      ICON_STREAM },
    {  3, "noise",    "WHITE NOISE", ICON_NOISE },
    {  4, "dryer",    "DRYER",       ICON_DRYER },
    {  5, "ocean",    "OCEAN",       ICON_OCEAN },
    {  6, "wind",     "WIND",        ICON_WIND },
    {  7, "rain",     "RAIN",        ICON_RAIN },
    { 10, "crickets", "CRICKETS",    ICON_CRICKETS },
    { 11, "brahms",   "LULLABY",     ICON_LULLABY },
    { 13, "twinkle",  "TWINKLE",     ICON_TWINKLE },
    { 14, "rockabye", "ROCKABYE",    ICON_ROCKABYE },
};
_Static_assert(sizeof(sounds) / sizeof(sounds[0]) == NSOUNDS, "NSOUNDS out of date");

int sound_index_for_id(int id)
{
    int i;
    for (i = 0; i < NSOUNDS; i++)
        if (sounds[i].id == id) return i;
    return 0;
}
