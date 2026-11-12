/* Freezing the stock Nest UI (nlclient) and its watchdog (nlheartbeatd). */
#ifndef NEST_PICKER_TAKEOVER_H
#define NEST_PICKER_TAKEOVER_H

#include <sys/types.h>

extern pid_t frozen_pids[2];  /* [0] nlheartbeatd, [1] nlclient; 0 = not frozen by us */

/* SIGSTOP freezes both (watchdog first); SIGCONT resumes them (UI first). */
void signal_nest(int sig);

#endif
