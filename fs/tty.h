#ifndef TTY_H
#define TTY_H

#include "kernel/fs.h"
#include "fs/dev.h"

struct winsize_ {
    word_t row;
    word_t col;
    word_t xpixel;
    word_t ypixel;
};

// This is the definition of __kernel_termios from glibc
struct termios_ {
    dword_t iflags;
    dword_t oflags;
    dword_t cflags;
    dword_t lflags;
    byte_t line;
    byte_t cc[19];
};

// struct termios2 (used by TCGETS2/TCSETS2/TCSETSW2/TCSETSF2): same layout as
// termios_ above plus explicit input/output speed fields. Newer glibc/
// util-linux prefer this over the legacy TCGETS/TCSETS family; a guest
// program that calls TCGETS2 to sanity-check its controlling terminal (e.g.
// util-linux login's isatty-style check) saw ENOTTY -- an unimplemented
// ioctl, not "this really isn't a tty" -- and bailed out immediately with
// "FATAL: bad tty" without ever attempting ttyname() resolution, even though
// the fd was a perfectly good pty and the legacy TCGETS path worked fine.
struct termios2_ {
    dword_t iflags;
    dword_t oflags;
    dword_t cflags;
    dword_t lflags;
    byte_t line;
    byte_t cc[19];
    dword_t ispeed;
    dword_t ospeed;
};

#define VINTR_ 0
#define VQUIT_ 1
#define VERASE_ 2
#define VKILL_ 3
#define VEOF_ 4
#define VTIME_ 5
#define VMIN_ 6
#define VSWTC_ 7
#define VSTART_ 8
#define VSTOP_ 9
#define VSUSP_ 10
#define VEOL_ 11
#define VREPRINT_ 12
#define VDISCARD_ 13
#define VWERASE_ 14
#define VLNEXT_ 15
#define VEOL2_ 16

#define ISIG_ (1 << 0)
#define ICANON_ (1 << 1)
#define ECHO_ (1 << 3)
#define ECHOE_ (1 << 4)
#define ECHOK_ (1 << 5)
#define NOFLSH_ (1 << 7)
#define ECHOCTL_ (1 << 9)
// Real Linux/glibc termios c_lflag bit (0004000 octal); was previously
// misdefined as (1 << 6), which is actually ECHONL's position -- that bug
// meant real ECHOKE from a guest program's tcsetattr was silently ignored
// (and a guest setting ECHONL would have been misread as ECHOKE instead).
#define ECHOKE_ (1 << 11)
#define IEXTEN_ (1 << 15)

#define INLCR_ (1 << 6)
#define IGNCR_ (1 << 7)
#define ICRNL_ (1 << 8)
#define IXON_ (1 << 10)

#define OPOST_ (1 << 0)
#define ONLCR_ (1 << 2)
#define OCRNL_ (1 << 3)
#define ONOCR_ (1 << 4)
#define ONLRET_ (1 << 5)

// c_cflag bits, from Linux's asm-generic/termbits.h. iSH's ttys have no real
// line discipline hardware, so nothing here changes how a tty behaves -- but
// guests do read these bits back, and the baud rate in particular is not
// cosmetic: musl's and glibc's cfgetospeed() return c_cflag & CBAUD, and B0
// means "hang up the line". A tty reporting B0 makes ssh(1) send ospeed 0 in
// its pty-req, and a BSD sshd honours that by SIGHUPing the session leader --
// so the remote shell died the instant it started (exit status 129).
#define CBAUD_ 0010017
#define B0_ 0000000
#define B38400_ 0000017
#define CSIZE_ 0000060
#define CS8_ 0000060
#define CREAD_ 0000200
#define PARENB_ 0000400
#define HUPCL_ 0002000

#define TCGETS_ 0x5401
#define TCSETS_ 0x5402
#define TCSETSW_ 0x5403
#define TCSETSF_ 0x5404
// Generic ioctl encoding (_IOR/_IOW('T', ..., struct termios2)), identical
// across every guest ABI iSH supports (i386/amd64/arm64/riscv64) -- unlike
// the legacy numbers above, which are old BSD-style constants kept for compat.
#define TCGETS2_ 0x802c542a
#define TCSETS2_ 0x402c542b
#define TCSETSW2_ 0x402c542c
#define TCSETSF2_ 0x402c542d
// tcflow() actions, passed to TCXONC by value.
#define TCOOFF_ 0
#define TCOON_  1
#define TCIOFF_ 2
#define TCION_  3
#define TCSBRK_ 0x5409
#define TCXONC_ 0x540a
#define TCFLSH_ 0x540b
#define TIOCSCTTY_ 0x540e
#define TIOCGSID_ 0x5429
#define TIOCGPGRP_ 0x540f
#define TIOCSPGRP_ 0x5410
#define TIOCGWINSZ_ 0x5413
#define TIOCSWINSZ_ 0x5414
#define TIOCCONS_ 0x541d
#define TIOCPKT_ 0x5420
#define TIOCGPTN_ 0x80045430
#define TIOCSPTLCK_ 0x40045431
#define TIOCGPKT_ 0x80045438
#define TIOCOUTQ_ 0x5411
#define TIOCNOTTY_ 0x5422
#define TIOCEXCL_ 0x540c
#define TIOCNXCL_ 0x540d
#define TIOCGEXCL_ 0x80045440

#define TCIFLUSH_ 0
#define TCOFLUSH_ 1
#define TCIOFLUSH_ 2

struct tty_driver {
    const struct tty_driver_ops *ops;
    int major;
    struct tty **ttys;
    unsigned limit;
};

#define DEFINE_TTY_DRIVER(name, driver_ops, _major, size) \
    static struct tty *name##_ttys[size]; \
    struct tty_driver name = {.ops = driver_ops, .major = _major, .ttys = name##_ttys, .limit = size}

struct tty_driver_ops {
    int (*init)(struct tty *tty);
    int (*open)(struct tty *tty);
    int (*write)(struct tty *tty, const void *buf, size_t len, bool blocking);
    int (*ioctl)(struct tty *tty, int cmd, void *arg);
    void (*cleanup)(struct tty *tty);
};

// indexed by major number
extern struct tty_driver *tty_drivers[256];
extern struct tty_driver real_tty_driver;

struct tty {
    unsigned refcount;
    struct tty_driver *driver;
    bool hung_up;
    // Bumped by every hangup. A descriptor records this at open and is hung up
    // only if the tty has been hung up SINCE -- which is what a hangup means on
    // Linux: it belongs to the descriptors that were open at the time, and a
    // fresh open of the same terminal gets a working one. Modelling it as a
    // single sticky flag made a hung-up console permanently dead: see
    // tests/manual/tty_hangup_reopen.c.
    unsigned hangup_gen;
    bool ever_opened;

#define TTY_BUF_SIZE 4096
    char buf[TTY_BUF_SIZE];
    // A flag is a marker indicating the end of a canonical mode input. Flags
    // are created by EOL and EOF characters. You can't backspace past a flag.
    bool buf_flag[TTY_BUF_SIZE];
    // VLNEXT (^V) latch: the NEXT input character is taken literally, with no
    // special meaning at all. One-shot, cleared as soon as it is consumed.
    bool lnext_pending;
    dword_t bufsize;
    uint8_t packet_flags;
    cond_t produced;
    cond_t consumed;

    struct winsize_ winsize;
    struct termios_ termios;
    int type;
    int num;
    dword_t atime;
    dword_t mtime;
    dword_t ctime;

    // TIOCEXCL: while set, only a privileged process may open this terminal
    // again. Guarded by tty->lock like the rest of this struct.
    bool excl;
    pid_t_ session;
    pid_t_ fg_group;

    struct list fds;
    // only locks fds, to keep the lock order
    lock_t fds_lock;

    // this never nests with itself, except in pty_is_half_closed_master
    lock_t lock;

    union {
        pthread_t thread; // for real tty driver
        struct {
            struct tty *other;
            mode_t_ perms;
            uid_t_ uid;
            uid_t_ gid;
            bool locked;
            bool packet_mode;
        } pty;
        void *data;
    };
};

// if blocking, may return _EINTR, otherwise, may return _EAGAIN
ssize_t tty_input(struct tty *tty, const char *input, size_t len, bool blocking);
void tty_set_winsize(struct tty *tty, struct winsize_ winsize);
// Who to signal for a hangup. Captured while tty->lock is held (tty_hangup),
// then handed to tty_hangup_notify once every tty lock has been dropped --
// sending a signal takes pids_lock, and fs/tty.c's input path already
// establishes that signals go out only after tty->lock is released.
struct tty_hangup_targets {
    pid_t_ fg_group;
    pid_t_ session;
};
struct tty_hangup_targets tty_hangup(struct tty *tty);
// SIGHUP (then SIGCONT, as Linux does) to the foreground group and session
// leader of a terminal that has gone away. Call with no tty lock held.
void tty_hangup_notify(struct tty_hangup_targets targets);
bool tty_stat_rdev(dev_t_ rdev, struct statbuf *stat);

// public for the benefit of ptys
struct tty *tty_get(struct tty_driver *driver, int type, int num);
struct tty *tty_alloc(struct tty_driver *driver, int type, int num);
int tty_open(struct tty *tty, struct fd *fd);
extern lock_t ttys_lock;
void tty_release(struct tty *tty); // must be called with ttys_lock

// Take a reference to an already-registered tty, found by device id, without
// creating one and without the caller needing to already hold a reference.
//
// This exists for the iOS app: a Terminal keeps only an unowned back-pointer to
// its tty, which tty_release frees out from under it (that free runs with
// ttys_lock held, which is exactly what makes the lookup here safe). Doing the
// lookup and the refcount bump under ttys_lock means the struct cannot be freed
// between finding it and using it, so callers get a pointer that stays valid
// until they tty_put() it.
//
// `expected`, when non-NULL, must match the registered tty or the lookup fails.
// It is only ever compared, never dereferenced, so a caller may pass a stale
// pointer safely -- that is the point: it keeps a recycled device number from
// silently handing back some *other* terminal's tty.
struct tty *tty_lookup_ref(int type, int num, struct tty *expected);
// Drop a reference taken by tty_lookup_ref. Takes ttys_lock itself, so unlike
// tty_release it must be called WITHOUT it.
void tty_put(struct tty *tty);

extern struct dev_ops tty_dev;
extern struct dev_ops ptmx_dev;

int ptmx_open(struct fd *fd);
// Should call with a driver declared *without* DEFINE_TTY_DRIVER, as it overwrites the ttys field.
struct tty *pty_open_fake(struct tty_driver *driver);

#endif
