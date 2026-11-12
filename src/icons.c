#include "icons.h"

#include <math.h>

#include "draw.h"

/* ------------------------------------------------------------------------ */
/* Sound icons: vector drawings in a unit box (-1..1), y down               */
/* ------------------------------------------------------------------------ */

static float icx, icy, isz;     /* icon centre and half-size in pixels */
static uint32_t ifg, ibg;       /* icon ink and the colour behind it */

#define IX(u) (icx + (u) * isz)
#define IY(v) (icy + (v) * isz)
static void L(float u0, float v0, float u1, float v1, float w) { draw_line(IX(u0), IY(v0), IX(u1), IY(v1), w * isz, ifg); }
static void C(float u, float v, float r) { fill_circle(IX(u), IY(v), r * isz, ifg); }
static void CB(float u, float v, float r) { fill_circle(IX(u), IY(v), r * isz, ibg); }
static void R(float u, float v, float r, float w) { draw_ring(IX(u), IY(v), r * isz, w * isz, ifg); }

static void WAVE(float v, float amp, float periods, float w, float u0, float u1)
{
    int i, n = 18;
    float pu = u0, pv = v;
    for (i = 1; i <= n; i++) {
        float t = (float)i / n, u = u0 + (u1 - u0) * t;
        float vv = v - amp * sinf(t * periods * 2.0f * (float)M_PI);
        L(pu, pv, u, vv, w);
        pu = u; pv = vv;
    }
}

/* Arc as a polyline; angles in degrees, 0 = right, 90 = down. */
static void ARC(float u, float v, float r, float a0, float a1, float w)
{
    int i, n = 12;
    float pu = u + r * cosf(a0 * (float)M_PI / 180), pv = v + r * sinf(a0 * (float)M_PI / 180);
    for (i = 1; i <= n; i++) {
        float a = (a0 + (a1 - a0) * i / n) * (float)M_PI / 180;
        float nu = u + r * cosf(a), nv = v + r * sinf(a);
        L(pu, pv, nu, nv, w);
        pu = nu; pv = nv;
    }
}

void draw_icon(enum icon icon, float cx, float cy, float half, uint32_t fg, uint32_t bg)
{
    int i;
    icx = cx; icy = cy; isz = half; ifg = fg; ibg = bg;
    switch (icon) {
    case ICON_NONE:
        R(0, 0, 0.78f, 0.17f);
        L(-0.55f, 0.55f, 0.55f, -0.55f, 0.17f);
        break;
    case ICON_STREAM:
        WAVE(-0.5f, 0.12f, 1.5f, 0.14f, -0.85f, 0.85f);
        WAVE(-0.1f, 0.12f, 1.5f, 0.14f, -0.85f, 0.85f);
        C(-0.5f, 0.56f, 0.26f);
        C(0.08f, 0.62f, 0.2f);
        C(0.58f, 0.56f, 0.24f);
        break;
    case ICON_NOISE: {
        static const float h[7] = { 0.35f, 0.75f, 0.5f, 0.92f, 0.6f, 0.82f, 0.4f };
        for (i = 0; i < 7; i++) {
            float u = -0.78f + i * 0.26f;
            L(u, -h[i], u, h[i], 0.14f);
        }
        break;
    }
    case ICON_DRYER:
        L(-0.72f, -0.82f, 0.72f, -0.82f, 0.13f);
        L(0.72f, -0.82f, 0.72f, 0.85f, 0.13f);
        L(0.72f, 0.85f, -0.72f, 0.85f, 0.13f);
        L(-0.72f, 0.85f, -0.72f, -0.82f, 0.13f);
        L(-0.72f, -0.5f, 0.72f, -0.5f, 0.1f);
        C(-0.45f, -0.66f, 0.07f);
        C(-0.22f, -0.66f, 0.07f);
        R(0, 0.2f, 0.4f, 0.13f);
        ARC(0, 0.2f, 0.2f, 200, 290, 0.09f);
        break;
    case ICON_OCEAN:
        WAVE(-0.5f, 0.16f, 1.5f, 0.15f, -0.85f, 0.85f);
        WAVE(0.0f, 0.16f, 1.5f, 0.15f, -0.85f, 0.85f);
        WAVE(0.5f, 0.16f, 1.5f, 0.15f, -0.85f, 0.85f);
        break;
    case ICON_WIND:
        L(-0.85f, -0.3f, 0.3f, -0.3f, 0.13f);
        ARC(0.3f, -0.52f, 0.22f, 90, -200, 0.13f);
        L(-0.85f, 0.05f, 0.7f, 0.05f, 0.13f);
        L(-0.85f, 0.4f, 0.1f, 0.4f, 0.13f);
        ARC(0.1f, 0.62f, 0.22f, -90, 160, 0.13f);
        break;
    case ICON_RAIN:
        C(-0.4f, -0.22f, 0.28f);
        C(0.05f, -0.4f, 0.36f);
        C(0.44f, -0.2f, 0.28f);
        L(-0.42f, -0.06f, 0.46f, -0.06f, 0.34f);
        L(-0.38f, 0.38f, -0.48f, 0.64f, 0.13f);
        L(0.05f, 0.38f, -0.05f, 0.64f, 0.13f);
        L(0.48f, 0.38f, 0.38f, 0.64f, 0.13f);
        L(-0.16f, 0.78f, -0.2f, 0.88f, 0.13f);
        L(0.28f, 0.78f, 0.24f, 0.88f, 0.13f);
        break;
    case ICON_CRICKETS: { /* pine tree: three stacked tiers and a trunk */
        static const float tiers[3][6] = {
            { 0.0f, -0.95f,  0.36f, -0.42f, -0.36f, -0.42f },
            { 0.0f, -0.62f,  0.56f,  0.06f, -0.56f,  0.06f },
            { 0.0f, -0.26f,  0.78f,  0.56f, -0.78f,  0.56f },
        };
        float trunk[8] = { IX(-0.14f), IY(0.5f), IX(0.14f), IY(0.5f), IX(0.14f), IY(0.92f), IX(-0.14f), IY(0.92f) };
        for (i = 0; i < 3; i++) {
            float p[6];
            int k;
            for (k = 0; k < 3; k++) {
                p[2 * k] = IX(tiers[i][2 * k]);
                p[2 * k + 1] = IY(tiers[i][2 * k + 1]);
            }
            fill_poly(p, 3, ifg);
        }
        fill_poly(trunk, 4, ifg);
        break;
    }
    case ICON_LULLABY:
        C(-0.45f, 0.55f, 0.24f);
        C(0.42f, 0.42f, 0.24f);
        L(-0.24f, 0.52f, -0.24f, -0.58f, 0.12f);
        L(0.63f, 0.39f, 0.63f, -0.72f, 0.12f);
        L(-0.24f, -0.62f, 0.63f, -0.76f, 0.24f);
        break;
    case ICON_TWINKLE: {
        float p[20];
        for (i = 0; i < 10; i++) {
            float a = (-90.0f + i * 36.0f) * (float)M_PI / 180, r = (i & 1) ? 0.38f : 0.92f;
            p[2 * i] = IX(r * cosf(a));
            p[2 * i + 1] = IY(0.08f + r * sinf(a));
        }
        fill_poly(p, 10, ifg);
        break;
    }
    case ICON_ROCKABYE:
        C(-0.05f, 0.05f, 0.75f);
        CB(0.33f, -0.22f, 0.6f);
        C(0.62f, 0.52f, 0.09f);
        C(0.8f, 0.12f, 0.06f);
        break;
    }
}
