#include "debug.h"
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <signal.h>

#include "kernel/calls.h"
#include "fs/tty.h"
#include "fs/tty-real.h"
#include "fs/devices.h"

// Only /dev/tty1 will be connected, the rest will go to a black hole.
#define REAL_TTY_NUM 1

void real_tty_reset_term(void);

static void *real_tty_read_thread(void *_tty) {
    struct tty *tty = _tty;
    char ch;
    for (;;) {
        ssize_t err = read(STDIN_FILENO, &ch, 1);
        if (err != 1) {
            if (err < 0 && errno == EINTR)
                continue;
            if (err < 0 && errno == EAGAIN) {
                // stdin might have O_NONBLOCK set (inherited from the
                // invoking shell); don't spin on it
                usleep(10000);
                continue;
            }
            // EOF, or an error that won't go away by retrying (e.g. stdin
            // redirected from /dev/null or a directory). Hang up the tty
            // instead of retrying in a tight printk loop.
            if (err == 0)
                printk("real tty: EOF on stdin, hanging up console\n");
            else
                printk("real tty: read failed (%s), hanging up console\n", strerror(errno));
            lock(&tty->lock, 0);
            struct tty_hangup_targets hup = tty_hangup(tty);
            unlock(&tty->lock);
            tty_hangup_notify(hup);
            // Park rather than return: the thread is detached, and
            // real_tty_cleanup's pthread_cancel (which requires the thread
            // to still exist) reclaims it -- pause() is a cancellation
            // point. Can't join in cleanup instead: tty_release calls it
            // holding tty->lock, which this thread takes just above.
            for (;;)
                pause();
        }
        if (ch == '\x1c') {
            // ^\ (so ^C still works for emulated SIGINT)
            real_tty_reset_term();
            raise(SIGINT);
        }
        tty_input(tty, &ch, 1, 0);
    }
    return NULL;
}

// Host speeds indexed by Linux baud code (see tty_baud_index). Only the rates
// every host names: Darwin stops at B230400.
static const speed_t host_speeds[] = {
    B0, B50, B75, B110, B134, B150, B200, B300, B600, B1200, B1800,
    B2400, B4800, B9600, B19200, B38400, B57600, B115200, B230400,
};

dword_t tty_speed_from_host(speed_t speed) {
    for (dword_t i = 1; i < array_size(host_speeds); i++) {
        if (host_speeds[i] == speed)
            return i <= 15 ? i : CBAUDEX_ | (i - 15);
    }
    // Never B0: a guest reads it as "hang up" and ssh(1) forwards it
    return B38400_;
}

speed_t tty_speed_to_host(dword_t cflags) {
    dword_t i = tty_baud_index(cflags);
    return i < array_size(host_speeds) ? host_speeds[i] : B38400;
}

static struct termios_ termios_from_real(struct termios real) {
    struct termios_ fake = {};
#define FLAG(t, x) \
    if (real.c_##t##flag & x) \
        fake.t##flags |= x##_
    FLAG(o, OPOST);
    FLAG(o, ONLCR);
    FLAG(o, OCRNL);
    FLAG(o, ONOCR);
    FLAG(o, ONLRET);
    FLAG(i, INLCR);
    FLAG(i, IGNCR);
    FLAG(i, ICRNL);
    FLAG(l, ISIG);
    FLAG(l, ICANON);
    FLAG(l, ECHO);
    FLAG(l, ECHOE);
    FLAG(l, ECHOK);
    FLAG(l, NOFLSH);
    FLAG(l, ECHOCTL);
    // c_cflag too, so the speed a guest reads back (and ssh forwards) is the
    // real terminal's. BSD keeps that speed in c_ospeed rather than in c_cflag,
    // hence cfgetospeed().
    FLAG(c, CSTOPB);
    FLAG(c, CREAD);
    FLAG(c, PARENB);
    FLAG(c, PARODD);
    FLAG(c, HUPCL);
    FLAG(c, CLOCAL);
#undef FLAG
    switch (real.c_cflag & CSIZE) {
        case CS5: fake.cflags |= CS5_; break;
        case CS6: fake.cflags |= CS6_; break;
        case CS7: fake.cflags |= CS7_; break;
        case CS8: fake.cflags |= CS8_; break;
    }
    fake.cflags |= tty_speed_from_host(cfgetospeed(&real));

#define CC(x) \
    fake.cc[V##x##_] = real.c_cc[V##x]
    CC(INTR);
    CC(QUIT);
    CC(ERASE);
    CC(KILL);
    CC(EOF);
    CC(TIME);
    CC(MIN);
    CC(START);
    CC(STOP);
    CC(SUSP);
    CC(EOL);
    CC(REPRINT);
    CC(DISCARD);
    CC(WERASE);
    CC(LNEXT);
    CC(EOL2);
#undef CC
    return fake;
}

static struct termios old_termios;
static bool real_tty_is_open;
static int real_tty_init(struct tty *tty) {
    if (tty->num != REAL_TTY_NUM)
        return 0;

    struct winsize winsz;
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &winsz) < 0) {
        if (errno == ENOTTY)
            goto notty;
        return errno_map();
    }
    tty->winsize.col = winsz.ws_col;
    tty->winsize.row = winsz.ws_row;
    tty->winsize.xpixel = winsz.ws_xpixel;
    tty->winsize.ypixel = winsz.ws_ypixel;

    struct termios termios;
    if (tcgetattr(STDIN_FILENO, &termios) < 0)
        return errno_map();
    tty->termios = termios_from_real(termios);

    old_termios = termios;
    cfmakeraw(&termios);
#ifdef NO_CRLF
    termios.c_oflag |= OPOST | ONLCR;
#endif
    if (tcsetattr(STDIN_FILENO, TCSANOW, &termios) < 0)
        ERRNO_DIE("failed to set terminal to raw mode");
notty:

    if (pthread_create(&tty->thread, NULL,  real_tty_read_thread, tty) < 0)
        // ok if this actually happened it would be weird AF
        return _EIO;
    pthread_detach(tty->thread);
    real_tty_is_open = true;
    return 0;
}

static int real_tty_write(struct tty *tty, const void *buf, size_t len, bool UNUSED(blocking)) {
    if (tty->num != REAL_TTY_NUM)
        return len;
    return write(STDOUT_FILENO, buf, len);
}

void real_tty_reset_term() {
    if (!real_tty_is_open) return;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &old_termios) < 0 && errno != ENOTTY) {
        printk("WARNING: failed to reset terminal: %s\n", strerror(errno));
        abort();
    }
}

static void real_tty_cleanup(struct tty *tty) {
    if (tty->num != REAL_TTY_NUM)
        return;
    real_tty_reset_term();
    pthread_cancel(tty->thread);
}

struct tty_driver_ops real_tty_ops = {
    .init = real_tty_init,
    .write = real_tty_write,
    .cleanup = real_tty_cleanup,
};
DEFINE_TTY_DRIVER(real_tty_driver, &real_tty_ops, TTY_CONSOLE_MAJOR, 64);
