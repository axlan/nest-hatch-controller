/* Settings from /etc/nest-picker.conf (shell-style KEY=VALUE). */
#ifndef NEST_PICKER_CONFIG_H
#define NEST_PICKER_CONFIG_H

#define CONF_PATH_DEFAULT "/etc/nest-picker.conf"

struct config {
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
    char night_anim[128];  /* animation file shown at night (made by gif2anim) */
    int  night_backlight;  /* -1 = BACKLIGHT */
    int  night_sleep_secs; /* -1 = SLEEP_SECS */
    int  night_start, night_end; /* minutes after midnight, parsed from night_mode; -1 = off */
};

extern struct config cfg;

void cfg_defaults(void);
void cfg_load(const char *path);

#endif
