/* Small shared helpers: run flag, clock, sysfs writes. */
#ifndef NEST_PICKER_UTIL_H
#define NEST_PICKER_UTIL_H

#include <signal.h>
#include <stddef.h>
#include <stdint.h>

extern volatile sig_atomic_t running;  /* cleared by SIGINT/SIGTERM/SIGHUP */

void on_signal(int sig);
int64_t now_ms(void);                  /* monotonic milliseconds */
void write_sysfs(const char *path, const char *val);
void copy_str(char *dst, size_t n, const char *src);

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

#endif
