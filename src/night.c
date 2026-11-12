#include "night.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "display.h"

/* ------------------------------------------------------------------------ */
/* Night mode: animation file made by gif2anim (python/)                    */
/*   header (16 B): "NANM" u16 version=1, u16 w=320, u16 h=320, u16 frames, */
/*                  u32 reserved                                            */
/*   frame: u16 delay_ms, u16 reserved, 256*3 RGB palette, 320*320 indices  */
/* The file is mmap'ed read-only, so frames live in reclaimable page cache. */
/* ------------------------------------------------------------------------ */

#define ANIM_HDR   16
#define ANIM_FRAME (4 + 256 * 3 + W * H)

static const uint8_t *anim;
static size_t anim_len;
int anim_frames;

static unsigned rd16(const uint8_t *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }

int anim_load(const char *path)
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
            syslog(LOG_WARNING, "night: %s is not a valid 320x320 animation (re-run gif2anim)", path);
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

int anim_delay(int i)
{
    int d = (int)rd16(anim_frame(i));
    return d < 20 ? 20 : d;
}

void render_anim_frame(int i)
{
    const uint8_t *f = anim_frame(i), *pal = f + 4, *px = f + 4 + 256 * 3;
    uint32_t lut[256];
    int k;
    for (k = 0; k < 256; k++) lut[k] = rgb(pal[3 * k], pal[3 * k + 1], pal[3 * k + 2]);
    for (k = 0; k < W * H; k++) frame[k] = lut[px[k]];
    fb_present();
}

/* True if the local time is inside NIGHT_MODE (ranges may wrap past midnight). */
int is_night(void)
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

void setup_time_and_night(void)
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
