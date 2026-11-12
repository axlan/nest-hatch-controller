/*
 * nest-picker: turn a Nest Learning Thermostat (Gen 1, "Diamond") into an
 * MQTT color + sound picker (for a Hatch Rest, via hatch-bridge).
 *
 *   - Freezes the stock UI (nlclient) and its software watchdog (nlheartbeatd)
 *     with SIGSTOP. Killing them makes the system reboot; freezing does not.
 *   - Draws directly to the 320x320 framebuffer (/dev/fb0).
 *   - Ring rotation (/dev/input/event1) picks the hue, or the sound on the
 *     sound page. Pressing the display (/dev/input/event2) toggles between the
 *     two pages. A long press re-sends the current color and sound.
 *   - Publishes <topic>/color and <topic>/sound as JSON (MQTT 3.1.1, QoS 0).
 *
 * Built against glibc 2.19 / kernel 2.6.37 (see build/Dockerfile).
 * `nest-picker --render-test DIR` renders both pages to PPM files without
 * touching any hardware (works on a normal Linux PC for previewing).
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

#define VERSION "2.1"

/* ------------------------------------------------------------------------ */
/* Sounds (Hatch Rest gen 1 ids; bird chirps (9) intentionally left out)    */
/* ------------------------------------------------------------------------ */

enum icon {
    ICON_NONE, ICON_STREAM, ICON_NOISE, ICON_DRYER, ICON_OCEAN, ICON_WIND,
    ICON_RAIN, ICON_CRICKETS, ICON_LULLABY, ICON_TWINKLE, ICON_ROCKABYE
};

static const struct sound {
    int id;
    const char *name;   /* MQTT name (matches pyhatchbabyrest) */
    const char *label;  /* on-screen */
    enum icon icon;
} sounds[] = {
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
#define NSOUNDS ((int)(sizeof(sounds) / sizeof(sounds[0])))

static int sound_index_for_id(int id)
{
    int i;
    for (i = 0; i < NSOUNDS; i++)
        if (sounds[i].id == id) return i;
    return 0;
}

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
    int  sound_step;       /* ring counts per sound */
    int  ring_invert;
    int  beep;
    int  takeover;         /* freeze nlclient/nlheartbeatd */
    int  start_h;
    int  start_sound;      /* Hatch sound id */
    char state_file[128];
    char timezone[64];     /* IANA zone, e.g. America/Chicago; empty = system /etc/localtime */
    char night_mode[16];   /* "HH:MM-HH:MM", may wrap past midnight; empty = off */
    char night_anim[128];  /* animation file shown at night (made by tools/gif2anim.py) */
    int  night_backlight;  /* -1 = BACKLIGHT */
    int  night_sleep_secs; /* -1 = SLEEP_SECS */
    int  night_start, night_end; /* minutes after midnight, parsed from night_mode; -1 = off */
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
    cfg.sound_step = 12;
    cfg.ring_invert = 0;
    cfg.beep = 1;
    cfg.takeover = 1;
    cfg.start_h = 30;
    cfg.start_sound = 0;
    strcpy(cfg.night_anim, "/media/data/nest-picker/night.anim");
    cfg.night_backlight = -1;
    cfg.night_sleep_secs = -1;
    cfg.night_start = cfg.night_end = -1;
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
#define IO(k, field) else if (strcmp(key, k) == 0) cfg.field = *val ? atoi(val) : -1
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
        I("SOUND_STEP", sound_step);
        I("RING_INVERT", ring_invert);
        I("BEEP", beep);
        I("TAKEOVER", takeover);
        I("START_H", start_h);
        I("START_SOUND", start_sound);
        S("STATE_FILE", state_file);
        S("TIMEZONE", timezone);
        S("NIGHT_MODE", night_mode);
        S("NIGHT_ANIM", night_anim);
        IO("NIGHT_BACKLIGHT", night_backlight);
        IO("NIGHT_SLEEP_SECS", night_sleep_secs);
#undef S
#undef I
#undef F
#undef IO
    }
    fclose(f);
    if (cfg.backlight < 1) cfg.backlight = 1;
    if (cfg.backlight > 100) cfg.backlight = 100;
    if (cfg.hue_step <= 0) cfg.hue_step = 1.0f;
    if (cfg.sound_step < 1) cfg.sound_step = 1;
    if (cfg.night_backlight > 100) cfg.night_backlight = 100;
    if (cfg.night_mode[0]) {
        int h1, m1, h2, m2;
        char extra;
        if (sscanf(cfg.night_mode, "%d:%d-%d:%d%c", &h1, &m1, &h2, &m2, &extra) == 4 &&
            h1 >= 0 && h1 < 24 && h2 >= 0 && h2 < 24 && m1 >= 0 && m1 < 60 && m2 >= 0 && m2 < 60 &&
            (h1 != h2 || m1 != m2)) {
            cfg.night_start = h1 * 60 + m1;
            cfg.night_end = h2 * 60 + m2;
        } else {
            syslog(LOG_WARNING, "ignoring invalid NIGHT_MODE '%s' (expected HH:MM-HH:MM)", cfg.night_mode);
        }
    }
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
        char path[300], buf[64];
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

static int disp_backlight = -1; /* level for the current mode; -1 = BACKLIGHT */

static void display_power(int on)
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
/* Drawing primitives (all anti-aliased; pixel centres are integer coords)  */
/* ------------------------------------------------------------------------ */

static void put(int x, int y, uint32_t c)
{
    if (x >= 0 && x < W && y >= 0 && y < H) frame[y * W + x] = c;
}

static void put_a(int x, int y, uint32_t c, float a)
{
    if (a <= 0 || x < 0 || x >= W || y < 0 || y >= H) return;
    frame[y * W + x] = blend(frame[y * W + x], c, a);
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void fill_circle(float cx, float cy, float r, uint32_t c)
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

static void draw_ring(float cx, float cy, float r, float w, uint32_t c)
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
static void draw_line(float ax, float ay, float bx, float by, float w, uint32_t c)
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
static void fill_poly(const float *pts, int n, uint32_t c)
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
static void fill_arc(float r_in, float r_out, uint32_t (*color_at)(int tenths))
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

static void draw_icon(enum icon icon, float cx, float cy, float half, uint32_t fg, uint32_t bg)
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

/* ------------------------------------------------------------------------ */
/* Picker UI                                                                */
/* ------------------------------------------------------------------------ */

#define HUE_R_IN     134.0f
#define HUE_R_OUT    157.0f
#define SWATCH_R     100.0f
#define MARKER_R     ((HUE_R_IN + HUE_R_OUT) / 2)
#define SOUND_RING_R 134.0f

enum mode { MODE_HUE, MODE_SOUND, MODE_NIGHT };

static struct {
    int h;                  /* 0..359; saturation and brightness are fixed at 100% */
    float hf;               /* fractional accumulator behind h */
    int sound;              /* index into sounds[] */
    int sound_acc;          /* ring counts towards the next sound step */
    enum mode mode;
    int64_t flash_until;    /* "SENT" confirmation */
    int mqtt_ok;
    int anim_idx;           /* night animation frame on screen */
    int64_t anim_next;      /* when to advance to the next frame */
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

static uint32_t hue_at(int tenths)
{
    int r, g, b;
    hsv_to_rgb(tenths / 10.0f, 1.0f, 1.0f, &r, &g, &b);
    return rgb(r, g, b);
}

static void build_background(void)
{
    uint32_t *save = frame;
    int i;
    frame = background;
    for (i = 0; i < W * H; i++) frame[i] = rgb(0, 0, 0);
    fill_arc(HUE_R_IN, HUE_R_OUT, hue_at);
    frame = save;
}

static void current_rgb(int *r, int *g, int *b)
{
    hsv_to_rgb((float)ui.h, 1.0f, 1.0f, r, g, b);
}

/* ------------------------------------------------------------------------ */
/* Night mode: animation file made by tools/gif2anim.py                     */
/*   header (16 B): "NANM" u16 version=1, u16 w=320, u16 h=320, u16 frames, */
/*                  u32 reserved                                            */
/*   frame: u16 delay_ms, u16 reserved, 256*3 RGB palette, 320*320 indices  */
/* The file is mmap'ed read-only, so frames live in reclaimable page cache. */
/* ------------------------------------------------------------------------ */

#define ANIM_HDR   16
#define ANIM_FRAME (4 + 256 * 3 + W * H)

static const uint8_t *anim;
static size_t anim_len;
static int anim_frames;

static unsigned rd16(const uint8_t *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }

static int anim_load(const char *path)
{
    struct stat st;
    void *m;
    int fd = open(path, O_RDONLY);
    if (fd < 0) { syslog(LOG_WARNING, "night: cannot open %s: %m", path); return -1; }
    if (fstat(fd, &st) < 0 || st.st_size < ANIM_HDR) {
        syslog(LOG_WARNING, "night: %s is too small", path);
        close(fd);
        return -1;
    }
    m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED) { syslog(LOG_WARNING, "night: mmap %s: %m", path); return -1; }
    {
        const uint8_t *h = m;
        int frames = (int)rd16(h + 10);
        if (memcmp(h, "NANM", 4) != 0 || rd16(h + 4) != 1 || rd16(h + 6) != W || rd16(h + 8) != H ||
            frames < 1 || (size_t)st.st_size != ANIM_HDR + (size_t)frames * ANIM_FRAME) {
            syslog(LOG_WARNING, "night: %s is not a valid 320x320 animation (re-run tools/gif2anim.py)", path);
            munmap(m, (size_t)st.st_size);
            return -1;
        }
        anim = h;
        anim_len = (size_t)st.st_size;
        anim_frames = frames;
    }
    return 0;
}

static const uint8_t *anim_frame(int i) { return anim + ANIM_HDR + (size_t)i * ANIM_FRAME; }

static int anim_delay(int i)
{
    int d = (int)rd16(anim_frame(i));
    return d < 20 ? 20 : d;
}

static void render_anim_frame(int i)
{
    const uint8_t *f = anim_frame(i), *pal = f + 4, *px = f + 4 + 256 * 3;
    uint32_t lut[256];
    int k;
    for (k = 0; k < 256; k++) lut[k] = rgb(pal[3 * k], pal[3 * k + 1], pal[3 * k + 2]);
    for (k = 0; k < W * H; k++) frame[k] = lut[px[k]];
    fb_present();
}

/* True if the local time is inside NIGHT_MODE (ranges may wrap past midnight). */
static int is_night(void)
{
    time_t now = time(NULL);
    struct tm tm;
    int m;
    if (cfg.night_start < 0) return 0;
    localtime_r(&now, &tm);
    m = tm.tm_hour * 60 + tm.tm_min;
    if (cfg.night_start < cfg.night_end) return m >= cfg.night_start && m < cfg.night_end;
    return m >= cfg.night_start || m < cfg.night_end;
}

static void render(void)
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

/* ------------------------------------------------------------------------ */
/* State persistence (survives reboots; written lazily to limit NAND wear)  */
/* ------------------------------------------------------------------------ */

static void state_load(void)
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

static void state_save(void)
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

/* ------------------------------------------------------------------------ */
/* MQTT (3.1.1, QoS 0) client thread                                        */
/* ------------------------------------------------------------------------ */

enum { CH_COLOR, CH_SOUND, NCH };

static pthread_mutex_t mq_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t mq_cond;
static struct {
    char topic[160];
    char payload[256];
    unsigned seq;                    /* bumped by the UI on every change */
} mq_ch[NCH];
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
    uint8_t body[600], *p = body, ack[4] = { 0, 0, 0, 0 };
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
    int fd = -1, backoff = 1000, c;
    unsigned sent_seq[NCH];
    int64_t last_tx = 0, last_rx = 0, last_pub[NCH] = { 0, 0 };
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
            for (c = 0; c < NCH; c++) sent_seq[c] = mq_ch[c].seq - 1; /* (re)publish on connect */
        }

        {
            int64_t t = now_ms();
            struct pollfd pfd;
            int wait_ms = 50;

            for (c = 0; c < NCH; c++) {
                char topic[160], payload[256];
                unsigned seq;
                pthread_mutex_lock(&mq_lock);
                seq = mq_ch[c].seq;
                copy_str(topic, sizeof(topic), mq_ch[c].topic);
                copy_str(payload, sizeof(payload), mq_ch[c].payload);
                pthread_mutex_unlock(&mq_lock);
                if (seq == sent_seq[c] || !payload[0]) continue;
                if (t - last_pub[c] >= cfg.publish_min_ms) {
                    if (mq_publish(fd, topic, payload, cfg.mqtt_retain) < 0) goto drop;
                    sent_seq[c] = seq;
                    last_pub[c] = last_tx = t;
                } else {
                    int w = (int)(cfg.publish_min_ms - (t - last_pub[c]));
                    if (w < wait_ms) wait_ms = w;
                }
            }
            if (t - last_tx >= 30000) {
                if (mq_send_packet(fd, 0xC0, NULL, 0) < 0) goto drop; /* PINGREQ */
                last_tx = t;
            }
            if (t - last_rx > 90000) {
                syslog(LOG_WARNING, "mqtt: keepalive timeout");
                goto drop;
            }

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

static void mq_set(int ch, const char *payload)
{
    pthread_mutex_lock(&mq_lock);
    copy_str(mq_ch[ch].payload, sizeof(mq_ch[ch].payload), payload);
    mq_ch[ch].seq++;
    pthread_cond_signal(&mq_cond);
    pthread_mutex_unlock(&mq_lock);
}

static void publish_color(void)
{
    int r, g, b;
    char buf[256];
    current_rgb(&r, &g, &b);
    snprintf(buf, sizeof(buf),
             "{\"hex\":\"#%02x%02x%02x\",\"r\":%d,\"g\":%d,\"b\":%d,\"h\":%d,\"s\":100,\"v\":100}",
             r, g, b, r, g, b, ui.h);
    mq_set(CH_COLOR, buf);
}

static void publish_sound(void)
{
    char buf[256];
    const struct sound *s = &sounds[ui.sound];
    snprintf(buf, sizeof(buf), "{\"id\":%d,\"name\":\"%s\"}", s->id, s->name);
    mq_set(CH_SOUND, buf);
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

static int alloc_buffers(void)
{
    frame = malloc(W * H * 4);
    background = malloc(W * H * 4);
    angle_tbl = malloc(W * H * sizeof(uint16_t));
    radius_tbl = malloc(W * H * sizeof(float));
    return frame && background && angle_tbl && radius_tbl ? 0 : -1;
}

static void write_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    int i;
    if (!f) { perror(path); return; }
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (i = 0; i < W * H; i++) {
        int r, g, b;
        unpack(frame[i], &r, &g, &b);
        fputc(r, f); fputc(g, f); fputc(b, f);
    }
    fclose(f);
}

/* Render every page and sound to PPM files in dir (no hardware access). */
static int render_test(const char *dir, const char *anim_path)
{
    char path[512];
    int i;
    if (alloc_buffers() < 0) return 1;
    build_tables();
    build_background();
    ui.mqtt_ok = 1;
    ui.h = 210;
    ui.sound = 2;
    ui.mode = MODE_HUE;
    render();
    snprintf(path, sizeof(path), "%s/hue.ppm", dir);
    write_ppm(path);
    ui.mode = MODE_SOUND;
    for (i = 0; i < NSOUNDS; i++) {
        ui.sound = i;
        ui.h = (i * 33 + 20) % 360;
        render();
        snprintf(path, sizeof(path), "%s/sound-%02d-%s.ppm", dir, i, sounds[i].name);
        write_ppm(path);
    }
    if (anim_path) {
        if (anim_load(anim_path) < 0) return 1;
        ui.mode = MODE_NIGHT;
        for (i = 0; i < anim_frames && i < 16; i++) {
            ui.anim_idx = i;
            render();
            snprintf(path, sizeof(path), "%s/night-%02d.ppm", dir, i);
            write_ppm(path);
        }
    }
    return 0;
}

/* On wake (and at startup): night animation during NIGHT_MODE hours, else the color page. */
static void choose_wake_mode(int64_t t)
{
    if (anim_frames > 0 && is_night()) {
        ui.mode = MODE_NIGHT;
        ui.anim_idx = 0;
        ui.anim_next = t + anim_delay(0);
        disp_backlight = cfg.night_backlight;
    } else {
        if (ui.mode == MODE_NIGHT) ui.mode = MODE_HUE;
        disp_backlight = -1;
    }
    display_power(1);
}

static void setup_time_and_night(void)
{
    time_t now = time(NULL);
    struct tm tm;
    char buf[64];

    if (cfg.timezone[0]) {
        char zpath[200];
        snprintf(zpath, sizeof(zpath), "/usr/share/zoneinfo/%s", cfg.timezone);
        if (access(zpath, R_OK) != 0)
            syslog(LOG_WARNING, "TIMEZONE '%s' not found in /usr/share/zoneinfo; times will be UTC", cfg.timezone);
        setenv("TZ", cfg.timezone, 1);
    }
    tzset();
    localtime_r(&now, &tm);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M %Z", &tm);
    syslog(LOG_INFO, "local time %s (TIMEZONE=%s)", buf, cfg.timezone[0] ? cfg.timezone : "system");

    if (cfg.night_start < 0) return;
    if (anim_load(cfg.night_anim) == 0)
        syslog(LOG_INFO, "night mode %s with %d-frame animation %s", cfg.night_mode, anim_frames, cfg.night_anim);
    else
        syslog(LOG_WARNING, "night mode %s disabled: no usable animation", cfg.night_mode);
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
        if (strcmp(argv[i], "--render-test") == 0 && i + 1 < argc)
            return render_test(argv[i + 1], i + 2 < argc ? argv[i + 2] : NULL);
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) conf = argv[++i];
        else if (strcmp(argv[i], "--release") == 0) { signal_nest(SIGCONT); return 0; }
    }
    cfg_load(conf);
    setup_time_and_night();
    snprintf(status_topic, sizeof(status_topic), "%s/status", cfg.mqtt_topic);
    snprintf(mq_ch[CH_COLOR].topic, sizeof(mq_ch[CH_COLOR].topic), "%s/color", cfg.mqtt_topic);
    snprintf(mq_ch[CH_SOUND].topic, sizeof(mq_ch[CH_SOUND].topic), "%s/sound", cfg.mqtt_topic);

    lock_fd = open("/tmp/nest-picker.lock", O_CREAT | O_RDWR, 0644);
    if (lock_fd >= 0 && flock(lock_fd, LOCK_EX | LOCK_NB) < 0) {
        syslog(LOG_ERR, "already running");
        return 1;
    }

    if (alloc_buffers() < 0) { syslog(LOG_ERR, "out of memory"); return 1; }
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
    choose_wake_mode(now_ms());

    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&mq_cond, &ca);
    publish_color();
    publish_sound();
    pthread_create(&mq_tid, NULL, mqtt_thread, NULL);

    syslog(LOG_INFO, "nest-picker %s running; broker %s:%d topics %s, %s",
           VERSION, cfg.mqtt_host[0] ? cfg.mqtt_host : "(none)", cfg.mqtt_port,
           mq_ch[CH_COLOR].topic, mq_ch[CH_SOUND].topic);

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
        if (awake && ui.mode == MODE_NIGHT && anim_frames > 1) {
            int64_t wait = ui.anim_next - now_ms();
            if (wait < 0) wait = 0;
            if (wait < timeout) timeout = (int)wait;
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
                awake = 1; dirty = 1; choose_wake_mode(t);
            } else if (ui.mode == MODE_NIGHT) {
                /* Night: input only keeps the screen awake. */
            } else {
                int d = cfg.ring_invert ? -ring_delta : ring_delta;
                if (ui.mode == MODE_HUE) {
                    ui.hf = fmodf(ui.hf + d * cfg.hue_step, 360.0f);
                    if (ui.hf < 0) ui.hf += 360.0f;
                    ui.h = (int)ui.hf % 360;
                    publish_color();
                } else {
                    int steps = 0;
                    ui.sound_acc += d;
                    while (ui.sound_acc >= cfg.sound_step) { ui.sound_acc -= cfg.sound_step; steps++; }
                    while (ui.sound_acc <= -cfg.sound_step) { ui.sound_acc += cfg.sound_step; steps--; }
                    if (steps) {
                        ui.sound = ((ui.sound + steps) % NSOUNDS + NSOUNDS) % NSOUNDS;
                        publish_sound();
                        beep(2400, 4);
                    }
                }
                dirty = 1;
                state_dirty = 1;
                last_state_change = t;
            }
            last_input = t;
        }

        /* Button: short press toggles hue/sound page, long press re-sends */
        for (;;) {
            struct ev e;
            ssize_t n = read(btn_fd, &e, sizeof(e));
            if (n != sizeof(e)) break;
            if (e.type != EV_KEY_T) continue;
            last_input = t;
            if (e.value == 1) {
                if (!awake) {
                    awake = 1; dirty = 1; choose_wake_mode(t);
                    swallow_release = 1;
                } else {
                    press_start = t;
                }
            } else if (e.value == 0) {
                if (swallow_release) { swallow_release = 0; continue; }
                if (ui.mode == MODE_NIGHT) { press_start = 0; continue; }
                if (press_start && t - press_start >= LONG_PRESS_MS) {
                    publish_color();
                    publish_sound();
                    ui.flash_until = t + 1200;
                    beep(1500, 30); usleep(60000); beep(2000, 30);
                } else if (press_start) {
                    ui.mode = ui.mode == MODE_HUE ? MODE_SOUND : MODE_HUE;
                    ui.sound_acc = 0;
                    beep(1000, 20);
                }
                press_start = 0;
                dirty = 1;
            }
        }

        if (ui.mqtt_ok != mq_connected) { ui.mqtt_ok = mq_connected; dirty = 1; }
        if (ui.flash_until && t >= ui.flash_until) { ui.flash_until = 0; dirty = 1; }

        if (awake && ui.mode == MODE_NIGHT && anim_frames > 1 && t >= ui.anim_next) {
            ui.anim_idx = (ui.anim_idx + 1) % anim_frames;
            ui.anim_next = t + anim_delay(ui.anim_idx);
            dirty = 1;
        }

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

        /* Persist the selection 5 s after the last change */
        if (state_dirty && t - last_state_change > 5000) {
            state_save();
            state_dirty = 0;
        }

        {
            int secs = (ui.mode == MODE_NIGHT && cfg.night_sleep_secs >= 0) ? cfg.night_sleep_secs : cfg.sleep_secs;
            if (awake && secs > 0 && t - last_input > (int64_t)secs * 1000) {
                awake = 0;
                ui.mode = MODE_HUE;
                display_power(0);
            }
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
