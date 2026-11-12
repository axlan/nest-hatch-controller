/*
 * nest-picker: turn a Nest Learning Thermostat (Gen 1, "Diamond") into an
 * MQTT color + sound picker (for a Hatch Rest, via hatch-ble-bridge or hatch-bridge).
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
 *
 * Source layout:
 *   main.c      arguments, render test, main loop (input -> UI -> MQTT)
 *   config.c    /etc/nest-picker.conf parsing
 *   takeover.c  freezing/resuming the stock Nest UI and its watchdog
 *   display.c   framebuffer, pixel format, backlight, frame buffers
 *   draw.c      colour math, anti-aliased shapes, text (font5x7.h)
 *   icons.c     vector sound icons          sounds.c  Hatch sound table
 *   ui.c        page rendering and publishing the selection
 *   night.c     timezone, night hours, night animation
 *   state.c     saved hue/sound             mqtt.c    MQTT publisher thread
 *   input.c     input events, beeper        util.c    small helpers
 */
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <syslog.h>
#include <unistd.h>

#include "config.h"
#include "display.h"
#include "input.h"
#include "mqtt.h"
#include "night.h"
#include "sounds.h"
#include "state.h"
#include "takeover.h"
#include "ui.h"
#include "util.h"

#define VERSION "2.1"

/* ------------------------------------------------------------------------ */
/* Main                                                                     */
/* ------------------------------------------------------------------------ */

#define LONG_PRESS_MS 700

static void cleanup(void)
{
    if (cfg.takeover) signal_nest(SIGCONT);
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

int main(int argc, char **argv)
{
    const char *conf = CONF_PATH_DEFAULT;
    int lock_fd, ring_fd, btn_fd, i;
    int awake = 1, dirty = 1, state_dirty = 0, swallow_release = 0;
    int64_t last_input, press_start = 0, last_render = 0, last_state_change = 0, last_freeze = 0;
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
    mqtt_init(cfg.mqtt_topic);

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

    publish_color();
    publish_sound();
    mqtt_start();

    syslog(LOG_INFO, "nest-picker %s running; broker %s:%d topics %s, %s",
           VERSION, cfg.mqtt_host[0] ? cfg.mqtt_host : "(none)", cfg.mqtt_port,
           mqtt_topic(CH_COLOR), mqtt_topic(CH_SOUND));

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

        if (ui.mqtt_ok != mqtt_connected()) { ui.mqtt_ok = mqtt_connected(); dirty = 1; }
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
    mqtt_stop();
    display_power(1);
    return 0;
}
