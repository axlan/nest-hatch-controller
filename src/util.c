#include "util.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------------ */
/* Helpers                                                                  */
/* ------------------------------------------------------------------------ */

volatile sig_atomic_t running = 1;

void on_signal(int sig)
{
    (void)sig;
    running = 0;
}

int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void write_sysfs(const char *path, const char *val)
{
    int fd = open(path, O_WRONLY);
    if (fd >= 0) {
        if (write(fd, val, strlen(val)) < 0) { /* ignore */ }
        close(fd);
    }
}

void copy_str(char *dst, size_t n, const char *src)
{
    snprintf(dst, n, "%s", src);
}
