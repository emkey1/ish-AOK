#ifndef TTY_REAL_H
#define TTY_REAL_H

#include <termios.h>
#include "misc.h"

// Between a host speed_t and the guest's CBAUD bits, for the two places that
// bridge a host termios: the CLI's real terminal and native programs. Host
// speeds Linux has no code for, and B0, come in as B38400; guest codes the
// host has no constant for go out as B38400.
dword_t tty_speed_from_host(speed_t speed);
speed_t tty_speed_to_host(dword_t cflags);

#endif
