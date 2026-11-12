#include "state.h"

#include <stdio.h>

#include "config.h"
#include "sounds.h"
#include "ui.h"

/* ------------------------------------------------------------------------ */
/* State persistence (survives reboots; written lazily to limit NAND wear)  */
/* ------------------------------------------------------------------------ */

void state_load(void)
{
    FILE *f;
    int h = cfg.start_h, sound_id = cfg.start_sound;
    f = fopen(cfg.state_file, "r");
    if (f) {
        int a, b, c;
        /* v1 files were "h s v"; v2 is "h sound_id 2" */
        if (fscanf(f, "%d %d %d", &a, &b, &c) == 3) {
            h = a;
            if (c == 2) sound_id = b;
        }
        fclose(f);
    }
    ui.h = ((h % 360) + 360) % 360;
    ui.hf = (float)ui.h;
    ui.sound = sound_index_for_id(sound_id);
}

void state_save(void)
{
    char tmp[160];
    FILE *f;
    snprintf(tmp, sizeof(tmp), "%s.tmp", cfg.state_file);
    f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "%d %d 2\n", ui.h, sounds[ui.sound].id);
    fclose(f);
    rename(tmp, cfg.state_file);
}
