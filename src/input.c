#include "input.h"

#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "util.h"

/* ------------------------------------------------------------------------ */
/* Input                                                                    */
/* ------------------------------------------------------------------------ */

int beeper_fd = -1;

void beep(int hz, int ms)
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

int evtest(void)
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
