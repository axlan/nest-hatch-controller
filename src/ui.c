#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "display.h"
#include "draw.h"
#include "icons.h"
#include "mqtt.h"
#include "night.h"
#include "sounds.h"
#include "util.h"

/* ------------------------------------------------------------------------ */
/* Picker UI                                                                */
/* ------------------------------------------------------------------------ */

struct ui_state ui;

uint32_t hue_at(int tenths)
{
    int r, g, b;
    hsv_to_rgb(tenths / 10.0f, 1.0f, 1.0f, &r, &g, &b);
    return rgb(r, g, b);
}

void build_background(void)
{
    uint32_t *save = frame;
    int i;
    frame = background;
    for (i = 0; i < W * H; i++) frame[i] = rgb(0, 0, 0);
    fill_arc(HUE_R_IN, HUE_R_OUT, hue_at);
    frame = save;
}

void current_rgb(int *r, int *g, int *b)
{
    hsv_to_rgb((float)ui.h, 1.0f, 1.0f, r, g, b);
}

void render(void)
{
    int r, g, b, lum, i;
    uint32_t col, fg, black = rgb(0, 0, 0);
    char hex[16];
    float a;

    if (ui.mode == MODE_NIGHT) {
        render_anim_frame(ui.anim_idx);
        return;
    }

    current_rgb(&r, &g, &b);
    col = rgb(r, g, b);
    lum = (r * 299 + g * 587 + b * 114) / 1000;
    fg = lum > 140 ? rgb(0, 0, 0) : rgb(255, 255, 255);

    if (ui.mode == MODE_HUE) {
        float mx, my;
        memcpy(frame, background, W * H * 4);

        /* Hue marker on the outer ring */
        a = ui.h * (float)M_PI / 180.0f;
        mx = CX + sinf(a) * MARKER_R;
        my = CY - cosf(a) * MARKER_R;
        fill_circle(mx, my, 16, black);
        fill_circle(mx, my, 14, rgb(255, 255, 255));
        fill_circle(mx, my, 10, col);

        fill_circle(CX, CY, SWATCH_R + 2, rgb(40, 40, 40));
        fill_circle(CX, CY, SWATCH_R, col);

        snprintf(hex, sizeof(hex), "#%02X%02X%02X", r, g, b);
        draw_text_centered(100, "COLOR", 2, fg);
        draw_text_centered(140, hex, 4, fg);
        if (now_ms() < ui.flash_until) {
            draw_text_centered(188, "SENT", 3, fg);
        } else {
            draw_icon(sounds[ui.sound].icon, CX, 200, 13, fg, col);
            draw_text_centered(222, sounds[ui.sound].label, 1, fg);
        }
        fill_circle(CX, CY + (SWATCH_R + HUE_R_IN) / 2 + 2, 5,
                    ui.mqtt_ok ? rgb(40, 210, 80) : rgb(230, 50, 40));
    } else {
        for (i = 0; i < W * H; i++) frame[i] = black;

        /* Sound choices around the edge, starting at the top, clockwise */
        for (i = 0; i < NSOUNDS; i++) {
            float ang = i * 2.0f * (float)M_PI / NSOUNDS;
            float x = CX + sinf(ang) * SOUND_RING_R, y = CY - cosf(ang) * SOUND_RING_R;
            if (i == ui.sound) {
                fill_circle(x, y, 23, col);
                fill_circle(x, y, 20, rgb(255, 255, 255));
                draw_icon(sounds[i].icon, x, y, 12, rgb(20, 20, 20), rgb(255, 255, 255));
            } else {
                fill_circle(x, y, 19, rgb(48, 48, 48));
                draw_icon(sounds[i].icon, x, y, 11, rgb(190, 190, 190), rgb(48, 48, 48));
            }
        }

        fill_circle(CX, CY, SWATCH_R + 2, rgb(40, 40, 40));
        fill_circle(CX, CY, SWATCH_R, col);
        draw_text_centered(76, "SOUND", 2, fg);
        if (now_ms() < ui.flash_until) {
            draw_text_centered(146, "SENT", 3, fg);
        } else {
            draw_icon(sounds[ui.sound].icon, CX, CY - 4, 36, fg, col);
            draw_text_centered(208, sounds[ui.sound].label, 2, fg);
        }
        fill_circle(CX, CY + SWATCH_R - 12, 4, ui.mqtt_ok ? rgb(40, 210, 80) : rgb(230, 50, 40));
    }

    fb_present();
}

void publish_color(void)
{
    int r, g, b;
    char buf[256];
    current_rgb(&r, &g, &b);
    snprintf(buf, sizeof(buf),
             "{\"hex\":\"#%02x%02x%02x\",\"r\":%d,\"g\":%d,\"b\":%d,\"h\":%d,\"s\":100,\"v\":100}",
             r, g, b, r, g, b, ui.h);
    mq_set(CH_COLOR, buf);
}

void publish_sound(void)
{
    char buf[256];
    const struct sound *s = &sounds[ui.sound];
    snprintf(buf, sizeof(buf), "{\"id\":%d,\"name\":\"%s\"}", s->id, s->name);
    mq_set(CH_SOUND, buf);
}
