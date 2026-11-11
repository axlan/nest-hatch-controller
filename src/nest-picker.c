/*
 * nest-picker: turn a Nest Learning Thermostat (Gen 1, "Diamond") into an
 * MQTT color picker.
 *
 *   - Freezes the stock UI (nlclient) and its software watchdog (nlheartbeatd)
 *     with SIGSTOP. Killing them makes the system reboot; freezing does not.
 *   - Draws directly to the 320x320 framebuffer (/dev/fb0).
 *   - Ring rotation (/dev/input/event1) adjusts the active channel,
 *     pressing the display (/dev/input/event2) cycles Hue -> Saturation ->
 *     Brightness. A long press re-sends the current color.
 *   - Publishes the color as JSON to an MQTT broker (MQTT 3.1.1, QoS 0).
 *
 * Built against glibc 2.19 / kernel 2.6.37 (see build/Dockerfile).
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <math.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "font5x7.h"

#define VERSION "1.0"

/* ------------------------------------------------------------------------ */
/* Configuration                                                            */
/* ------------------------------------------------------------------------ */

#define CONF_PATH_DEFAULT "/etc/nest-picker.conf"

static struct {
    char mqtt_host[128];
    int  mqtt_port;
    char mqtt_user[128];
    char mqtt_pass[128];
    char mqtt_topic[128];
    char mqtt_client_id[64];
    int  mqtt_retain;
    int  publish_min_ms;   /* rate limit while the ring is spinning */
    int  sleep_secs;       /* backlight off after this much idle; 0 = never */
    int  backlight;        /* 0..100 */
    float hue_step;        /* degrees per ring count */
    float pct_step;        /* percent per ring count (sat / brightness) */
    int  ring_invert;
    int  beep;
    int  takeover;         /* freeze nlclient/nlheartbeatd */
    int  start_h, start_s, start_v;
    char state_file[128];
} cfg;

static void cfg_defaults(void)
{
    memset(&cfg, 0, sizeof(cfg));
    cfg.mqtt_port = 1883;
    strcpy(cfg.mqtt_topic, "nest/picker");
    cfg.mqtt_retain = 1;
    cfg.publish_min_ms = 100;
    cfg.sleep_secs = 300;
    cfg.backlight = 80;
    cfg.hue_step = 1.0f;
    cfg.pct_step = 0.5f;
    cfg.ring_invert = 0;
    cfg.beep = 1;
    cfg.takeover = 1;
    cfg.start_h = 30;
    cfg.start_s = 100;
    cfg.start_v = 100;
    strcpy(cfg.state_file, "/media/user-config/nest-picker.state");
}

static void copy_str(char *dst, size_t n, const char *src)
{
    snprintf(dst, n, "%s", src);
}

/* Shell-compatible KEY=VALUE file (the start script sources the same file). */
static void cfg_load(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[512];
    if (!f) {
        syslog(LOG_WARNING, "no config at %s, using defaults", path);
        return;
    }
    while (fgets(line, sizeof(line), f)) {
        char *p = line, *eq, *key, *val, *end;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == 0) continue;
        if (strncmp(p, "export ", 7) == 0) p += 7;
        eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        key = p;
        val = eq + 1;
        end = val + strlen(val);
        while (end > val && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t'))
            *--end = 0;
        if ((*val == '"' || *val == '\'') && end > val + 1 && end[-1] == *val) {
            end[-1] = 0;
            val++;
        }
#define S(k, field) else if (strcmp(key, k) == 0) copy_str(cfg.field, sizeof(cfg.field), val)
#define I(k, field) else if (strcmp(key, k) == 0) cfg.field = atoi(val)
#define F(k, field) else if (strcmp(key, k) == 0) cfg.field = (float)atof(val)
        if (0) {}
        S("MQTT_HOST", mqtt_host);
        I("MQTT_PORT", mqtt_port);
        S("MQTT_USER", mqtt_user);
        S("MQTT_PASS", mqtt_pass);
        S("MQTT_TOPIC", mqtt_topic);
        S("MQTT_CLIENT_ID", mqtt_client_id);
        I("MQTT_RETAIN", mqtt_retain);
        I("PUBLISH_MIN_MS", publish_min_ms);
        I("SLEEP_SECS", sleep_secs);
        I("BACKLIGHT", backlight);
        F("HUE_STEP", hue_step);
        F("PCT_STEP", pct_step);
        I("RING_INVERT", ring_invert);
        I("BEEP", beep);
        I("TAKEOVER", takeover);
        I("START_H", start_h);
        I("START_S", start_s);
        I("START_V", start_v);
        S("STATE_FILE", state_file);
#undef S
#undef I
#undef F
    }
    fclose(f);
    if (cfg.backlight < 1) cfg.backlight = 1;
    if (cfg.backlight > 100) cfg.backlight = 100;
    if (cfg.hue_step <= 0) cfg.hue_step = 1.0f;
    if (cfg.pct_step <= 0) cfg.pct_step = 0.5f;
}

/* ------------------------------------------------------------------------ */
/* Helpers                                                                  */
/* ------------------------------------------------------------------------ */

static volatile sig_atomic_t running = 1;

static void on_signal(int sig)
{
    (void)sig;
    running = 0;
}

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void write_sysfs(const char *path, const char *val)
{
    int fd = open(path, O_WRONLY);
    if (fd >= 0) {
        if (write(fd, val, strlen(val)) < 0) { /* ignore */ }
        close(fd);
    }
}

/* ------------------------------------------------------------------------ */
/* Nest UI takeover                                                         */
/* ------------------------------------------------------------------------ */

static pid_t find_pid(const char *name)
{
    DIR *d = opendir("/proc");
    struct dirent *e;
    pid_t found = 0;
    if (!d) return 0;
    while ((e = readdir(d)) != NULL) {
        char path[64], buf[64];
        int fd;
        ssize_t n;
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        snprintf(path, sizeof(path), "/proc/%s/comm", e->d_name);
        fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0) continue;
        buf[n] = 0;
        if (buf[n - 1] == '\n') buf[n - 1] = 0;
        if (strcmp(buf, name) == 0) { found = (pid_t)atoi(e->d_name); break; }
    }
    closedir(d);
    return found;
}

static pid_t frozen_pids[2];

static void signal_nest(int sig)
{
    /* Heartbeat first when stopping (so it can't notice nlclient go quiet),
     * nlclient first when resuming. */
    const char *order_stop[] = { "nlheartbeatd", "nlclient" };
    const char *order_cont[] = { "nlclient", "nlheartbeatd" };
    const char **order = sig == SIGSTOP ? order_stop : order_cont;
    int i;
    for (i = 0; i < 2; i++) {
        pid_t p = find_pid(order[i]);
        int slot = strcmp(order[i], "nlclient") == 0;
        if (p > 0) {
            kill(p, sig);
            if (sig != SIGSTOP || frozen_pids[slot] != p)
                syslog(LOG_INFO, "%s %s (pid %d)", sig == SIGSTOP ? "froze" : "resumed", order[i], (int)p);
            frozen_pids[slot] = sig == SIGSTOP ? p : 0;
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Display                                                                  */
/* ------------------------------------------------------------------------ */

#define W 320
#define H 320
#define CX 159.5f
#define CY 159.5f

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

static uint32_t *frame;       /* what we compose into */
static uint32_t *background;  /* static hue ring, pre-rendered */
static uint16_t *angle_tbl;   /* angle of each pixel, 0..3599 (tenths of a degree), 0 = top, clockwise */
static float *radius_tbl;     /* distance of each pixel from the centre */

static uint32_t rgb(int r, int g, int b)
{
    return ((uint32_t)0xFF << sh_a) | ((uint32_t)r << sh_r) | ((uint32_t)g << sh_g) | ((uint32_t)b << sh_b);
}

static void unpack(uint32_t c, int *r, int *g, int *b)
{
    *r = (c >> sh_r) & 0xFF;
    *g = (c >> sh_g) & 0xFF;
    *b = (c >> sh_b) & 0xFF;
}

static uint32_t blend(uint32_t dst, uint32_t src, float a)
{
    int dr, dg, db, sr, sg, sb;
    if (a >= 1.0f) return src;
    if (a <= 0.0f) return dst;
    unpack(dst, &dr, &dg, &db);
    unpack(src, &sr, &sg, &sb);
    return rgb((int)(dr + (sr - dr) * a), (int)(dg + (sg - dg) * a), (int)(db + (sb - db) * a));
}

static int fb_open(void)
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
static void fb_present(void)
{
    int p, y;
    struct omapfb_update_window uw;
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

static int backlight_level = -1;

static void display_power(int on)
{
    char buf[16];
    if (on) {
        write_sysfs(BLANK_PATH, "0\n");
        snprintf(buf, sizeof(buf), "%d\n", cfg.backlight);
        write_sysfs(BACKLIGHT_PATH, buf);
        backlight_level = cfg.backlight;
    } else {
        write_sysfs(BACKLIGHT_PATH, "0\n");
        write_sysfs(BLANK_PATH, "1\n");
        backlight_level = 0;
    }
}

/* ------------------------------------------------------------------------ */
/* Color math                                                               */
/* ------------------------------------------------------------------------ */

static void hsv_to_rgb(float h, float s, float v, int *r, int *g, int *b)
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
/* Drawing primitives                                                       */
/* ------------------------------------------------------------------------ */

static void put(int x, int y, uint32_t c)
{
    if (x >= 0 && x < W && y >= 0 && y < H) frame[y * W + x] = c;
}

/* Anti-aliased filled disc. */
static void fill_circle(float cx, float cy, float r, uint32_t c)
{
    int x0 = (int)(cx - r - 2), x1 = (int)(cx + r + 2);
    int y0 = (int)(cy - r - 2), y1 = (int)(cy + r + 2);
    int x, y;
    for (y = y0; y <= y1; y++) {
        if (y < 0 || y >= H) continue;
        for (x = x0; x <= x1; x++) {
            float dx, dy, d2, d, a;
            if (x < 0 || x >= W) continue;
            dx = x - cx; dy = y - cy;
            d2 = dx * dx + dy * dy;
            if (d2 <= (r - 1) * (r - 1)) { frame[y * W + x] = c; continue; }
            if (d2 >= (r + 1) * (r + 1)) continue;
            d = sqrtf(d2);
            a = r + 0.5f - d;
            frame[y * W + x] = blend(frame[y * W + x], c, a);
        }
    }
}

/* Anti-aliased ring (annulus) between r_in and r_out around the screen centre,
 * restricted to the angle range [0, deg_end) measured clockwise from the top. */
static void fill_arc(float r_in, float r_out, int tenths_end, uint32_t (*color_at)(int tenths, void *), void *ctx)
{
    int y0 = (int)(CY - r_out - 2), y1 = (int)(CY + r_out + 2);
    int x, y;
    for (y = y0; y <= y1; y++) {
        for (x = y0; x <= y1; x++) {
            int i = y * W + x;
            float d = radius_tbl[i], a;
            if (d < r_in - 1 || d > r_out + 1) continue;
            if (angle_tbl[i] >= tenths_end) continue;
            a = 1.0f;
            if (d > r_out - 0.5f) a = r_out + 0.5f - d;
            else if (d < r_in + 0.5f) a = d - (r_in - 0.5f);
            frame[i] = blend(frame[i], color_at(angle_tbl[i], ctx), a);
        }
    }
}

static int text_width(const char *s, int scale)
{
    int n = (int)strlen(s);
    return n ? n * 6 * scale - scale : 0;
}

static void draw_text(int x, int y, const char *s, int scale, uint32_t c)
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

static void draw_text_centered(int y, const char *s, int scale, uint32_t c)
{
    draw_text((W - text_width(s, scale)) / 2, y, s, scale, c);
}

/* ------------------------------------------------------------------------ */
/* Picker UI                                                                */
/* ------------------------------------------------------------------------ */

#define HUE_R_IN     134.0f
#define HUE_R_OUT    157.0f
#define GAUGE_R_IN   110.0f
#define GAUGE_R_OUT  124.0f
#define SWATCH_R     100.0f
#define MARKER_R     ((HUE_R_IN + HUE_R_OUT) / 2)

enum mode { MODE_HUE, MODE_SAT, MODE_VAL, MODE_COUNT };
static const char *mode_names[] = { "HUE", "SATURATION", "BRIGHTNESS" };

static struct {
    int h, s, v;            /* 0..359, 0..100, 0..100 */
    enum mode mode;
    int64_t flash_until;    /* "SENT" confirmation */
    int mqtt_ok;
    float hf, sf, vf;       /* fractional accumulators behind h/s/v */
} ui;

static void build_tables(void)
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

static uint32_t hue_at(int tenths, void *ctx)
{
    int r, g, b;
    (void)ctx;
    hsv_to_rgb(tenths / 10.0f, 1.0f, 1.0f, &r, &g, &b);
    return rgb(r, g, b);
}

static void build_background(void)
{
    uint32_t *save = frame;
    int i;
    frame = background;
    for (i = 0; i < W * H; i++) frame[i] = rgb(0, 0, 0);
    fill_arc(HUE_R_IN, HUE_R_OUT, 3600, hue_at, NULL);
    frame = save;
}

static uint32_t gauge_color(int tenths, void *ctx)
{
    /* Gradient along the arc showing what the channel does to the color. */
    int r, g, b;
    float t = tenths / 3600.0f;
    (void)ctx;
    if (ui.mode == MODE_SAT) hsv_to_rgb(ui.h, t, ui.v / 100.0f, &r, &g, &b);
    else                     hsv_to_rgb(ui.h, ui.s / 100.0f, t, &r, &g, &b);
    return rgb(r, g, b);
}

static uint32_t const_color(int tenths, void *ctx)
{
    (void)tenths;
    return *(uint32_t *)ctx;
}

static void render(void)
{
    int r, g, b, lum;
    uint32_t col, fg, dim;
    char hex[16], val[32];
    float a, mx, my;

    memcpy(frame, background, W * H * 4);

    hsv_to_rgb(ui.h, ui.s / 100.0f, ui.v / 100.0f, &r, &g, &b);
    col = rgb(r, g, b);
    lum = (r * 299 + g * 587 + b * 114) / 1000;
    fg = lum > 140 ? rgb(0, 0, 0) : rgb(255, 255, 255);
    dim = rgb(60, 60, 60);

    /* Gauge for saturation / brightness */
    if (ui.mode != MODE_HUE) {
        int pct = ui.mode == MODE_SAT ? ui.s : ui.v;
        fill_arc(GAUGE_R_IN, GAUGE_R_OUT, 3600, const_color, &dim);
        fill_arc(GAUGE_R_IN, GAUGE_R_OUT, pct * 36, gauge_color, NULL);
        if (pct > 0) {
            a = pct * 3.6f * (float)M_PI / 180.0f;
            fill_circle(CX + sinf(a) * (GAUGE_R_IN + GAUGE_R_OUT) / 2,
                        CY - cosf(a) * (GAUGE_R_IN + GAUGE_R_OUT) / 2, 9, rgb(255, 255, 255));
        }
    }

    /* Hue marker on the outer ring */
    a = ui.h * (float)M_PI / 180.0f;
    mx = CX + sinf(a) * MARKER_R;
    my = CY - cosf(a) * MARKER_R;
    fill_circle(mx, my, ui.mode == MODE_HUE ? 16 : 13, rgb(0, 0, 0));
    fill_circle(mx, my, ui.mode == MODE_HUE ? 14 : 11, rgb(255, 255, 255));
    fill_circle(mx, my, ui.mode == MODE_HUE ? 10 : 8, hue_at(ui.h * 10, NULL));

    /* Swatch */
    fill_circle(CX, CY, SWATCH_R + 2, rgb(40, 40, 40));
    fill_circle(CX, CY, SWATCH_R, col);

    snprintf(hex, sizeof(hex), "#%02X%02X%02X", r, g, b);
    if (ui.mode == MODE_HUE) snprintf(val, sizeof(val), "%d", ui.h);
    else snprintf(val, sizeof(val), "%d%%", ui.mode == MODE_SAT ? ui.s : ui.v);

    draw_text_centered(108, mode_names[ui.mode], 2, fg);
    draw_text_centered(146, hex, 4, fg);
    if (now_ms() < ui.flash_until)
        draw_text_centered(190, "SENT", 3, fg);
    else
        draw_text_centered(190, val, 3, fg);

    /* MQTT status dot at the bottom, between the swatch and the hue ring */
    fill_circle(CX, CY + (SWATCH_R + HUE_R_IN) / 2 + 2, 5,
                ui.mqtt_ok ? rgb(40, 210, 80) : rgb(230, 50, 40));

    fb_present();
}

/* ------------------------------------------------------------------------ */
/* State persistence (survives reboots; written lazily to limit NAND wear)  */
/* ------------------------------------------------------------------------ */

static void state_load(void)
{
    FILE *f;
    ui.h = cfg.start_h; ui.s = cfg.start_s; ui.v = cfg.start_v;
    f = fopen(cfg.state_file, "r");
    if (f) {
        int h, s, v;
        if (fscanf(f, "%d %d %d", &h, &s, &v) == 3) { ui.h = h; ui.s = s; ui.v = v; }
        fclose(f);
    }
    ui.h = ((ui.h % 360) + 360) % 360;
    if (ui.s < 0) ui.s = 0;
    if (ui.s > 100) ui.s = 100;
    if (ui.v < 0) ui.v = 0;
    if (ui.v > 100) ui.v = 100;
    ui.hf = ui.h; ui.sf = ui.s; ui.vf = ui.v;
}

static void state_save(void)
{
    char tmp[160];
    FILE *f;
    snprintf(tmp, sizeof(tmp), "%s.tmp", cfg.state_file);
    f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "%d %d %d\n", ui.h, ui.s, ui.v);
    fclose(f);
    rename(tmp, cfg.state_file);
}

/* ------------------------------------------------------------------------ */
/* MQTT (3.1.1, QoS 0) client thread                                        */
/* ------------------------------------------------------------------------ */

static pthread_mutex_t mq_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t mq_cond;
static char mq_payload[256];
static unsigned mq_seq;              /* bumped by the UI on every change */
static volatile int mq_connected;
static char status_topic[160];

static void mq_put_str(uint8_t **p, const char *s)
{
    size_t n = strlen(s);
    *(*p)++ = (uint8_t)(n >> 8);
    *(*p)++ = (uint8_t)n;
    memcpy(*p, s, n);
    *p += n;
}

static size_t mq_put_len(uint8_t *p, size_t len)
{
    size_t i = 0;
    do {
        uint8_t d = len % 128;
        len /= 128;
        if (len) d |= 0x80;
        p[i++] = d;
    } while (len);
    return i;
}

static int send_all(int fd, const uint8_t *buf, size_t len)
{
    while (len) {
        ssize_t n = send(fd, buf, len, MSG_NOSIGNAL);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        buf += n; len -= (size_t)n;
    }
    return 0;
}

static int mq_send_packet(int fd, uint8_t type, const uint8_t *body, size_t len)
{
    uint8_t hdr[5];
    size_t h;
    hdr[0] = type;
    h = 1 + mq_put_len(hdr + 1, len);
    if (send_all(fd, hdr, h) < 0) return -1;
    return len ? send_all(fd, body, len) : 0;
}

static int mq_publish(int fd, const char *topic, const char *payload, int retain)
{
    uint8_t body[512], *p = body;
    size_t tl = strlen(topic), pl = strlen(payload);
    if (tl + pl + 2 > sizeof(body)) return -1;
    mq_put_str(&p, topic);
    memcpy(p, payload, pl);
    p += pl;
    return mq_send_packet(fd, 0x30 | (retain ? 1 : 0), body, (size_t)(p - body));
}

static int tcp_connect(const char *host, int port, int timeout_ms)
{
    struct addrinfo hints, *res = NULL, *ai;
    char portstr[8];
    int fd = -1;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(portstr, sizeof(portstr), "%d", port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) {
        syslog(LOG_WARNING, "mqtt: cannot resolve %s", host);
        return -1;
    }
    for (ai = res; ai; ai = ai->ai_next) {
        struct pollfd pfd;
        int err = 0, one = 1;
        socklen_t el = sizeof(err);
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) < 0 && errno != EINPROGRESS) {
            close(fd); fd = -1; continue;
        }
        pfd.fd = fd; pfd.events = POLLOUT;
        if (poll(&pfd, 1, timeout_ms) != 1 ||
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) < 0 || err) {
            close(fd); fd = -1; continue;
        }
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        break;
    }
    freeaddrinfo(res);
    return fd;
}

static int read_timeout(int fd, uint8_t *buf, size_t len, int timeout_ms)
{
    size_t got = 0;
    while (got < len) {
        struct pollfd pfd;
        ssize_t n;
        pfd.fd = fd; pfd.events = POLLIN;
        if (poll(&pfd, 1, timeout_ms) != 1) return -1;
        n = recv(fd, buf + got, len - got, 0);
        if (n <= 0) return -1;
        got += (size_t)n;
    }
    return 0;
}

static int mq_connect(void)
{
    uint8_t body[600], *p = body, ack[4];
    uint8_t flags = 0x02; /* clean session */
    int fd;
    const char *cid = cfg.mqtt_client_id;
    char cid_buf[64];

    if (!cid[0]) {
        char hn[48] = "nest";
        gethostname(hn, sizeof(hn) - 1);
        snprintf(cid_buf, sizeof(cid_buf), "nest-picker-%s", hn);
        cid = cid_buf;
    }

    fd = tcp_connect(cfg.mqtt_host, cfg.mqtt_port, 5000);
    if (fd < 0) return -1;

    flags |= 0x04 | 0x20; /* will flag, will retain, will QoS 0 */
    if (cfg.mqtt_user[0]) flags |= 0x80;
    if (cfg.mqtt_user[0] && cfg.mqtt_pass[0]) flags |= 0x40;

    mq_put_str(&p, "MQTT");
    *p++ = 4;            /* protocol level 3.1.1 */
    *p++ = flags;
    *p++ = 0; *p++ = 60; /* keepalive seconds */
    mq_put_str(&p, cid);
    mq_put_str(&p, status_topic);
    mq_put_str(&p, "offline");
    if (flags & 0x80) mq_put_str(&p, cfg.mqtt_user);
    if (flags & 0x40) mq_put_str(&p, cfg.mqtt_pass);

    if (mq_send_packet(fd, 0x10, body, (size_t)(p - body)) < 0 ||
        read_timeout(fd, ack, 4, 5000) < 0 || ack[0] != 0x20 || ack[3] != 0) {
        syslog(LOG_WARNING, "mqtt: connect to %s:%d refused (rc=%d)", cfg.mqtt_host, cfg.mqtt_port, ack[3]);
        close(fd);
        return -1;
    }
    mq_publish(fd, status_topic, "online", 1);
    syslog(LOG_INFO, "mqtt: connected to %s:%d as %s", cfg.mqtt_host, cfg.mqtt_port, cid);
    return fd;
}

static void *mqtt_thread(void *arg)
{
    int fd = -1, backoff = 1000;
    unsigned sent_seq = 0;
    int64_t last_tx = 0, last_rx = 0, last_pub = 0;
    (void)arg;

    while (running) {
        if (fd < 0) {
            struct timespec ts;
            if (!cfg.mqtt_host[0]) { sleep(1); continue; }
            fd = mq_connect();
            if (fd < 0) {
                clock_gettime(CLOCK_MONOTONIC, &ts);
                ts.tv_sec += backoff / 1000;
                pthread_mutex_lock(&mq_lock);
                pthread_cond_timedwait(&mq_cond, &mq_lock, &ts);
                pthread_mutex_unlock(&mq_lock);
                backoff = backoff * 2 > 30000 ? 30000 : backoff * 2;
                continue;
            }
            backoff = 1000;
            mq_connected = 1;
            last_tx = last_rx = now_ms();
            sent_seq = mq_seq - 1; /* (re)publish current color on connect */
        }

        {
            char payload[256];
            unsigned seq;
            int64_t t = now_ms();
            struct pollfd pfd;
            int wait_ms;

            pthread_mutex_lock(&mq_lock);
            seq = mq_seq;
            copy_str(payload, sizeof(payload), mq_payload);
            pthread_mutex_unlock(&mq_lock);

            if (seq != sent_seq && payload[0] && t - last_pub >= cfg.publish_min_ms) {
                if (mq_publish(fd, cfg.mqtt_topic, payload, cfg.mqtt_retain) < 0) goto drop;
                sent_seq = seq;
                last_pub = last_tx = t;
            }
            if (t - last_tx >= 30000) {
                if (mq_send_packet(fd, 0xC0, NULL, 0) < 0) goto drop; /* PINGREQ */
                last_tx = t;
            }
            if (t - last_rx > 90000) {
                syslog(LOG_WARNING, "mqtt: keepalive timeout");
                goto drop;
            }

            /* Sleep until socket data, a UI change (poll timeout keeps it short), or ping time. */
            wait_ms = seq != sent_seq ? (int)(cfg.publish_min_ms - (t - last_pub)) : 50;
            if (wait_ms < 1) wait_ms = 1;
            pfd.fd = fd; pfd.events = POLLIN;
            if (poll(&pfd, 1, wait_ms) == 1) {
                uint8_t buf[256];
                ssize_t n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
                if (n <= 0) goto drop;
                last_rx = now_ms();
            }
            continue;
        }
drop:
        syslog(LOG_WARNING, "mqtt: connection lost");
        close(fd);
        fd = -1;
        mq_connected = 0;
    }

    if (fd >= 0) {
        mq_publish(fd, status_topic, "offline", 1);
        mq_send_packet(fd, 0xE0, NULL, 0); /* DISCONNECT */
        close(fd);
    }
    return NULL;
}

static void publish_color(void)
{
    int r, g, b;
    hsv_to_rgb(ui.h, ui.s / 100.0f, ui.v / 100.0f, &r, &g, &b);
    pthread_mutex_lock(&mq_lock);
    snprintf(mq_payload, sizeof(mq_payload),
             "{\"hex\":\"#%02x%02x%02x\",\"r\":%d,\"g\":%d,\"b\":%d,\"h\":%d,\"s\":%d,\"v\":%d}",
             r, g, b, r, g, b, ui.h, ui.s, ui.v);
    mq_seq++;
    pthread_cond_signal(&mq_cond);
    pthread_mutex_unlock(&mq_lock);
}

/* ------------------------------------------------------------------------ */
/* Input                                                                    */
/* ------------------------------------------------------------------------ */

/* struct input_event on 32-bit ARM: 16 bytes */
struct ev {
    uint32_t sec, usec;
    uint16_t type, code;
    int32_t value;
};

#define EV_KEY_T 0x01
#define EV_REL_T 0x02
#define EV_SND_T 0x12
#define SND_TONE_C 0x02

static int beeper_fd = -1;

static void beep(int hz, int ms)
{
    struct ev e;
    if (!cfg.beep || beeper_fd < 0) return;
    memset(&e, 0, sizeof(e));
    e.type = EV_SND_T; e.code = SND_TONE_C; e.value = hz;
    if (write(beeper_fd, &e, sizeof(e)) < 0) return;
    usleep(ms * 1000);
    e.value = 0;
    if (write(beeper_fd, &e, sizeof(e)) < 0) return;
}

static int evtest(void)
{
    struct pollfd pfd[2];
    pfd[0].fd = open("/dev/input/event1", O_RDONLY);
    pfd[1].fd = open("/dev/input/event2", O_RDONLY);
    pfd[0].events = pfd[1].events = POLLIN;
    printf("reading ring (event1) and button (event2), ctrl-c to stop\n");
    fflush(stdout);
    while (running) {
        int i;
        if (poll(pfd, 2, 1000) <= 0) continue;
        for (i = 0; i < 2; i++) {
            struct ev e;
            if ((pfd[i].revents & POLLIN) && read(pfd[i].fd, &e, sizeof(e)) == sizeof(e))
                printf("event%d type=%u code=%u value=%d\n", i + 1, e.type, e.code, e.value);
        }
        fflush(stdout);
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Main                                                                     */
/* ------------------------------------------------------------------------ */

#define LONG_PRESS_MS 700

static void cleanup(void)
{
    if (cfg.takeover) signal_nest(SIGCONT);
}

int main(int argc, char **argv)
{
    const char *conf = CONF_PATH_DEFAULT;
    int lock_fd, ring_fd, btn_fd, i;
    int awake = 1, dirty = 1, state_dirty = 0, swallow_release = 0;
    int64_t last_input, press_start = 0, last_render = 0, last_state_change = 0, last_freeze = 0;
    pthread_t mq_tid;
    pthread_condattr_t ca;
    struct sigaction sa;

    openlog("nest-picker", LOG_PID | LOG_PERROR, LOG_USER);
    cfg_defaults();

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--evtest") == 0) return evtest();
        if (strcmp(argv[i], "--version") == 0) { printf("nest-picker %s\n", VERSION); return 0; }
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) conf = argv[++i];
        else if (strcmp(argv[i], "--release") == 0) { signal_nest(SIGCONT); return 0; }
    }
    cfg_load(conf);
    snprintf(status_topic, sizeof(status_topic), "%s/status", cfg.mqtt_topic);
    {
        char *t = cfg.mqtt_topic; /* color goes to <topic>/color */
        size_t n = strlen(t);
        if (n + 6 < sizeof(cfg.mqtt_topic)) strcat(t, "/color");
    }

    lock_fd = open("/tmp/nest-picker.lock", O_CREAT | O_RDWR, 0644);
    if (lock_fd >= 0 && flock(lock_fd, LOCK_EX | LOCK_NB) < 0) {
        syslog(LOG_ERR, "already running");
        return 1;
    }

    frame = malloc(W * H * 4);
    background = malloc(W * H * 4);
    angle_tbl = malloc(W * H * sizeof(uint16_t));
    radius_tbl = malloc(W * H * sizeof(float));
    if (!frame || !background || !angle_tbl || !radius_tbl) { syslog(LOG_ERR, "out of memory"); return 1; }

    if (fb_open() < 0) return 1;
    ring_fd = open("/dev/input/event1", O_RDONLY | O_NONBLOCK);
    btn_fd = open("/dev/input/event2", O_RDONLY | O_NONBLOCK);
    beeper_fd = open("/dev/input/event0", O_WRONLY);
    if (ring_fd < 0 || btn_fd < 0) { syslog(LOG_ERR, "cannot open input devices: %m"); return 1; }

    build_tables();
    build_background();
    state_load();
    ui.mode = MODE_HUE;

    if (cfg.takeover) {
        signal_nest(SIGSTOP);
        atexit(cleanup);
    }
    display_power(1);

    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&mq_cond, &ca);
    publish_color();
    pthread_create(&mq_tid, NULL, mqtt_thread, NULL);

    syslog(LOG_INFO, "nest-picker %s running; broker %s:%d topic %s",
           VERSION, cfg.mqtt_host[0] ? cfg.mqtt_host : "(none)", cfg.mqtt_port, cfg.mqtt_topic);

    last_input = now_ms();

    while (running) {
        struct pollfd pfd[2];
        int64_t t;
        int timeout = 100;
        int ring_delta = 0;

        if (dirty && awake) {
            /* Cap redraws at ~30 fps; render as soon as allowed. */
            int64_t wait = 33 - (now_ms() - last_render);
            timeout = wait > 0 ? (int)wait : 0;
        }

        pfd[0].fd = ring_fd; pfd[0].events = POLLIN;
        pfd[1].fd = btn_fd;  pfd[1].events = POLLIN;
        poll(pfd, 2, timeout);
        t = now_ms();

        /* Ring: accumulate all queued movement */
        for (;;) {
            struct ev e;
            ssize_t n = read(ring_fd, &e, sizeof(e));
            if (n != sizeof(e)) break;
            if (e.type == EV_REL_T) ring_delta += e.value;
        }
        if (ring_delta) {
            if (!awake) {
                awake = 1; dirty = 1; display_power(1);
            } else {
                int d = cfg.ring_invert ? -ring_delta : ring_delta;
                switch (ui.mode) {
                case MODE_HUE:
                    ui.hf = fmodf(ui.hf + d * cfg.hue_step, 360.0f);
                    if (ui.hf < 0) ui.hf += 360.0f;
                    ui.h = (int)ui.hf % 360;
                    break;
                case MODE_SAT:
                    ui.sf += d * cfg.pct_step;
                    if (ui.sf < 0) ui.sf = 0;
                    if (ui.sf > 100) ui.sf = 100;
                    ui.s = (int)lrintf(ui.sf);
                    break;
                case MODE_VAL:
                    ui.vf += d * cfg.pct_step;
                    if (ui.vf < 0) ui.vf = 0;
                    if (ui.vf > 100) ui.vf = 100;
                    ui.v = (int)lrintf(ui.vf);
                    break;
                default: break;
                }
                publish_color();
                dirty = 1;
                state_dirty = 1;
                last_state_change = t;
            }
            last_input = t;
        }

        /* Button: short press cycles mode, long press re-sends */
        for (;;) {
            struct ev e;
            ssize_t n = read(btn_fd, &e, sizeof(e));
            if (n != sizeof(e)) break;
            if (e.type != EV_KEY_T) continue;
            last_input = t;
            if (e.value == 1) {
                if (!awake) {
                    awake = 1; dirty = 1; display_power(1);
                    swallow_release = 1;
                } else {
                    press_start = t;
                }
            } else if (e.value == 0) {
                if (swallow_release) { swallow_release = 0; continue; }
                if (press_start && t - press_start >= LONG_PRESS_MS) {
                    publish_color();
                    ui.flash_until = t + 1200;
                    beep(1500, 30); usleep(60000); beep(2000, 30);
                } else if (press_start) {
                    ui.mode = (enum mode)((ui.mode + 1) % MODE_COUNT);
                    beep(1000, 20);
                }
                press_start = 0;
                dirty = 1;
            }
        }

        if (ui.mqtt_ok != mq_connected) { ui.mqtt_ok = mq_connected; dirty = 1; }
        if (ui.flash_until && t >= ui.flash_until) { ui.flash_until = 0; dirty = 1; }

        if (dirty && awake && t - last_render >= 33) {
            render();
            last_render = t;
            dirty = 0;
        }

        /* If monit ever restarts nlclient, freeze the new instance and repaint over it. */
        if (cfg.takeover && t - last_freeze > 5000) {
            pid_t a = frozen_pids[0], b = frozen_pids[1];
            signal_nest(SIGSTOP);
            if (frozen_pids[0] != a || frozen_pids[1] != b) { dirty = 1; if (awake) display_power(1); }
            last_freeze = t;
        }

        /* Persist the color 5 s after the last change */
        if (state_dirty && t - last_state_change > 5000) {
            state_save();
            state_dirty = 0;
        }

        if (awake && cfg.sleep_secs > 0 && t - last_input > (int64_t)cfg.sleep_secs * 1000) {
            awake = 0;
            ui.mode = MODE_HUE;
            display_power(0);
        }
    }

    syslog(LOG_INFO, "exiting");
    if (state_dirty) state_save();
    pthread_mutex_lock(&mq_lock);
    pthread_cond_signal(&mq_cond);
    pthread_mutex_unlock(&mq_lock);
    pthread_join(mq_tid, NULL);
    display_power(1);
    return 0;
}
