#include "takeover.h"

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

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

pid_t frozen_pids[2];

void signal_nest(int sig)
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
