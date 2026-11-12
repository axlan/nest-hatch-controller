#include "display.h"

#include <fcntl.h>
#include <linux/fb.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <syslog.h>
#include <unistd.h>

#include "config.h"
#include "util.h"

/* ------------------------------------------------------------------------ */
/* Display                                                                  */
/* ------------------------------------------------------------------------ */

#define BACKLIGHT_PATH "/sys/class/backlight/pwm-backlight.0/brightness"
#define BLANK_PATH     "/sys/class/graphics/fb0/blank"

/* OMAP framebuffer manual-update ioctl (harmless if the panel auto-updates) */
struct omapfb_update_window {
    uint32_t x, y, width, height, format;
    uint32_t out_x, out_y, out_width, out_height;
    uint32_t reserved[8];
};
#define OMAPFB_UPDATE_WINDOW _IOW('O', 54, struct omapfb_update_window)

static int fb_fd = -1;
static uint8_t *fb_mem;
static size_t fb_len;
static int fb_pages = 1;
static size_t fb_stride;
static int sh_r = 16, sh_g = 8, sh_b = 0, sh_a = 24;

uint32_t *frame;       /* what we compose into */
uint32_t *background;  /* static hue ring, pre-rendered */
uint16_t *angle_tbl;   /* angle of each pixel, 0..3599 (tenths of a degree), 0 = top, clockwise */
float *radius_tbl;     /* distance of each pixel from the centre */

uint32_t rgb(int r, int g, int b)
{
    return ((uint32_t)0xFF << sh_a) | ((uint32_t)r << sh_r) | ((uint32_t)g << sh_g) | ((uint32_t)b << sh_b);
}

void unpack(uint32_t c, int *r, int *g, int *b)
{
    *r = (c >> sh_r) & 0xFF;
    *g = (c >> sh_g) & 0xFF;
    *b = (c >> sh_b) & 0xFF;
}

uint32_t blend(uint32_t dst, uint32_t src, float a)
{
    int dr, dg, db, sr, sg, sb;
    if (a >= 1.0f) return src;
    if (a <= 0.0f) return dst;
    unpack(dst, &dr, &dg, &db);
    unpack(src, &sr, &sg, &sb);
    return rgb((int)(dr + (sr - dr) * a), (int)(dg + (sg - dg) * a), (int)(db + (sb - db) * a));
}

int fb_open(void)
{
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo fx;
    fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0) { syslog(LOG_ERR, "open /dev/fb0: %m"); return -1; }
    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &v) == 0 && ioctl(fb_fd, FBIOGET_FSCREENINFO, &fx) == 0) {
        if (v.bits_per_pixel != 32 || v.xres != W || v.yres != H) {
            syslog(LOG_ERR, "unexpected framebuffer %ux%u@%u", v.xres, v.yres, v.bits_per_pixel);
            return -1;
        }
        sh_r = v.red.offset; sh_g = v.green.offset; sh_b = v.blue.offset;
        sh_a = v.transp.length ? (int)v.transp.offset : 24;
        fb_stride = fx.line_length;
        fb_pages = (int)(v.yres_virtual / v.yres);
        if (fb_pages < 1) fb_pages = 1;
        fb_len = fx.smem_len;
        syslog(LOG_INFO, "fb0 %ux%u virt %ux%u stride %zu rgba-offsets %d/%d/%d/%d",
               v.xres, v.yres, v.xres_virtual, v.yres_virtual, fb_stride, sh_r, sh_g, sh_b, sh_a);
    } else {
        fb_stride = W * 4;
        fb_pages = 2;
        fb_len = W * H * 4 * 2;
    }
    fb_mem = mmap(NULL, fb_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (fb_mem == MAP_FAILED) { syslog(LOG_ERR, "mmap fb0: %m"); fb_mem = NULL; return -1; }
    return 0;
}

/* Copy the composed frame to every page of the virtual framebuffer, so it is
 * visible regardless of which page nlclient last panned to. */
void fb_present(void)
{
    int p, y;
    struct omapfb_update_window uw;
    if (!fb_mem) return; /* --render-test */
    for (p = 0; p < fb_pages; p++) {
        size_t base = (size_t)p * H * fb_stride;
        if (base + H * fb_stride > fb_len) break;
        if (fb_stride == W * 4) {
            memcpy(fb_mem + base, frame, W * H * 4);
        } else {
            for (y = 0; y < H; y++)
                memcpy(fb_mem + base + y * fb_stride, frame + y * W, W * 4);
        }
    }
    memset(&uw, 0, sizeof(uw));
    uw.width = uw.out_width = W;
    uw.height = uw.out_height = H;
    ioctl(fb_fd, OMAPFB_UPDATE_WINDOW, &uw);
}

int disp_backlight = -1; /* level for the current mode; -1 = BACKLIGHT */

void display_power(int on)
{
    char buf[16];
    if (on) {
        write_sysfs(BLANK_PATH, "0\n");
        snprintf(buf, sizeof(buf), "%d\n", disp_backlight >= 0 ? disp_backlight : cfg.backlight);
        write_sysfs(BACKLIGHT_PATH, buf);
    } else {
        write_sysfs(BACKLIGHT_PATH, "0\n");
        write_sysfs(BLANK_PATH, "1\n");
    }
}

int alloc_buffers(void)
{
    frame = malloc(W * H * 4);
    background = malloc(W * H * 4);
    angle_tbl = malloc(W * H * sizeof(uint16_t));
    radius_tbl = malloc(W * H * sizeof(float));
    return frame && background && angle_tbl && radius_tbl ? 0 : -1;
}

void build_tables(void)
{
    int x, y;
    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            float dx = x - CX, dy = y - CY;
            float a = atan2f(dx, -dy);           /* 0 at top, clockwise */
            int t;
            if (a < 0) a += 2.0f * (float)M_PI;
            t = (int)(a * 1800.0f / (float)M_PI);
            if (t >= 3600) t = 3599;
            angle_tbl[y * W + x] = (uint16_t)t;
            radius_tbl[y * W + x] = sqrtf(dx * dx + dy * dy);
        }
    }
}
