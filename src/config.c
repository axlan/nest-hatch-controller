#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include "util.h"

/* ------------------------------------------------------------------------ */
/* Configuration                                                            */
/* ------------------------------------------------------------------------ */

struct config cfg;

void cfg_defaults(void)
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

/* Shell-compatible KEY=VALUE file (the start script sources the same file). */
void cfg_load(const char *path)
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
