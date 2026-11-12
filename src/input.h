/* Ring, button and beeper input devices. */
#ifndef NEST_PICKER_INPUT_H
#define NEST_PICKER_INPUT_H

#include <stdint.h>

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

extern int beeper_fd;

void beep(int hz, int ms);
int evtest(void);  /* --evtest: print ring/button events */

#endif
