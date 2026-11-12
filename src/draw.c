#include "draw.h"

#include <math.h>
#include <string.h>

#include "display.h"
#include "font5x7.h"
#include "util.h"

/* ------------------------------------------------------------------------ */
/* Color math                                                               */
/* ------------------------------------------------------------------------ */

void hsv_to_rgb(float h, float s, float v, int *r, int *g, int *b)
{
    float c, x, m, rf, gf, bf, hp;
    h = fmodf(h, 360.0f);
    if (h < 0) h += 360.0f;
    c = v * s;
    hp = h / 60.0f;
    x = c * (1.0f - fabsf(fmodf(hp, 2.0f) - 1.0f));
    m = v - c;
    if (hp < 1)      { rf = c; gf = x; bf = 0; }
    else if (hp < 2) { rf = x; gf = c; bf = 0; }
    else if (hp < 3) { rf = 0; gf = c; bf = x; }
    else if (hp < 4) { rf = 0; gf = x; bf = c; }
    else if (hp < 5) { rf = x; gf = 0; bf = c; }
    else             { rf = c; gf = 0; bf = x; }
    *r = (int)lrintf((rf + m) * 255.0f);
    *g = (int)lrintf((gf + m) * 255.0f);
    *b = (int)lrintf((bf + m) * 255.0f);
}

/* ------------------------------------------------------------------------ */
/* Drawing primitives (all anti-aliased; pixel centres are integer coords)  */
/* ------------------------------------------------------------------------ */

void put(int x, int y, uint32_t c)
{
    if (x >= 0 && x < W && y >= 0 && y < H) frame[y * W + x] = c;
}

void put_a(int x, int y, uint32_t c, float a)
{
    if (a <= 0 || x < 0 || x >= W || y < 0 || y >= H) return;
    frame[y * W + x] = blend(frame[y * W + x], c, a);
}

void fill_circle(float cx, float cy, float r, uint32_t c)
{
    int x0 = clampi((int)(cx - r - 2), 0, W - 1), x1 = clampi((int)(cx + r + 2), 0, W - 1);
    int y0 = clampi((int)(cy - r - 2), 0, H - 1), y1 = clampi((int)(cy + r + 2), 0, H - 1);
    int x, y;
    for (y = y0; y <= y1; y++) {
        for (x = x0; x <= x1; x++) {
            float dx = x - cx, dy = y - cy, d2 = dx * dx + dy * dy;
            if (d2 <= (r - 1) * (r - 1)) { frame[y * W + x] = c; continue; }
            if (d2 >= (r + 1) * (r + 1)) continue;
            put_a(x, y, c, r + 0.5f - sqrtf(d2));
        }
    }
}

void draw_ring(float cx, float cy, float r, float w, uint32_t c)
{
    int x0 = clampi((int)(cx - r - w - 2), 0, W - 1), x1 = clampi((int)(cx + r + w + 2), 0, W - 1);
    int y0 = clampi((int)(cy - r - w - 2), 0, H - 1), y1 = clampi((int)(cy + r + w + 2), 0, H - 1);
    int x, y;
    for (y = y0; y <= y1; y++)
        for (x = x0; x <= x1; x++) {
            float dx = x - cx, dy = y - cy;
            put_a(x, y, c, w / 2 + 0.5f - fabsf(sqrtf(dx * dx + dy * dy) - r));
        }
}

/* Thick line with round caps. */
void draw_line(float ax, float ay, float bx, float by, float w, uint32_t c)
{
    float hw = w / 2;
    int x0 = clampi((int)(fminf(ax, bx) - hw - 2), 0, W - 1), x1 = clampi((int)(fmaxf(ax, bx) + hw + 2), 0, W - 1);
    int y0 = clampi((int)(fminf(ay, by) - hw - 2), 0, H - 1), y1 = clampi((int)(fmaxf(ay, by) + hw + 2), 0, H - 1);
    float vx = bx - ax, vy = by - ay, len2 = vx * vx + vy * vy;
    int x, y;
    for (y = y0; y <= y1; y++)
        for (x = x0; x <= x1; x++) {
            float px = x - ax, py = y - ay, t = len2 > 0 ? (px * vx + py * vy) / len2 : 0, dx, dy;
            if (t < 0) t = 0;
            if (t > 1) t = 1;
            dx = px - t * vx; dy = py - t * vy;
            put_a(x, y, c, hw + 0.5f - sqrtf(dx * dx + dy * dy));
        }
}

/* Filled polygon, 4x supersampled even-odd fill. */
void fill_poly(const float *pts, int n, uint32_t c)
{
    float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
    static const float off[4][2] = { {-0.25f, -0.25f}, {0.25f, -0.25f}, {-0.25f, 0.25f}, {0.25f, 0.25f} };
    int i, x, y, s;
    for (i = 0; i < n; i++) {
        minx = fminf(minx, pts[2 * i]); maxx = fmaxf(maxx, pts[2 * i]);
        miny = fminf(miny, pts[2 * i + 1]); maxy = fmaxf(maxy, pts[2 * i + 1]);
    }
    for (y = clampi((int)miny - 1, 0, H - 1); y <= clampi((int)maxy + 1, 0, H - 1); y++)
        for (x = clampi((int)minx - 1, 0, W - 1); x <= clampi((int)maxx + 1, 0, W - 1); x++) {
            int hits = 0;
            for (s = 0; s < 4; s++) {
                float px = x + off[s][0], py = y + off[s][1];
                int in = 0, j = n - 1;
                for (i = 0; i < n; j = i++) {
                    float xi = pts[2 * i], yi = pts[2 * i + 1], xj = pts[2 * j], yj = pts[2 * j + 1];
                    if ((yi > py) != (yj > py) && px < (xj - xi) * (py - yi) / (yj - yi) + xi) in = !in;
                }
                hits += in;
            }
            put_a(x, y, c, hits / 4.0f);
        }
}

/* Ring (annulus) around the screen centre, colour by angle (tenths of degrees, 0 = top). */
void fill_arc(float r_in, float r_out, uint32_t (*color_at)(int tenths))
{
    int y0 = (int)(CY - r_out - 2), y1 = (int)(CY + r_out + 2);
    int x, y;
    for (y = y0; y <= y1; y++) {
        for (x = y0; x <= y1; x++) {
            int i = y * W + x;
            float d = radius_tbl[i], a;
            if (d < r_in - 1 || d > r_out + 1) continue;
            a = 1.0f;
            if (d > r_out - 0.5f) a = r_out + 0.5f - d;
            else if (d < r_in + 0.5f) a = d - (r_in - 0.5f);
            frame[i] = blend(frame[i], color_at(angle_tbl[i]), a);
        }
    }
}

int text_width(const char *s, int scale)
{
    int n = (int)strlen(s);
    return n ? n * 6 * scale - scale : 0;
}

void draw_text(int x, int y, const char *s, int scale, uint32_t c)
{
    for (; *s; s++, x += 6 * scale) {
        const uint8_t *g = font_lookup(*s);
        int row, col, i, j;
        for (row = 0; row < 7; row++)
            for (col = 0; col < 5; col++)
                if (g[row] & (0x10 >> col))
                    for (i = 0; i < scale; i++)
                        for (j = 0; j < scale; j++)
                            put(x + col * scale + j, y + row * scale + i, c);
    }
}

void draw_text_centered(int y, const char *s, int scale, uint32_t c)
{
    draw_text((W - text_width(s, scale)) / 2, y, s, scale, c);
}
