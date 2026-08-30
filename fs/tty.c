#define DEFAULT_CHANNEL debug
#include "debug.h"
#include <string.h>
#include "kernel/calls.h"
#include "fs/poll.h"
#include "fs/tty.h"
#include "fs/devices.h"
#include "util/sync.h"

extern time_t boot_time;

extern struct tty_driver pty_master;
extern struct tty_driver pty_slave;

struct tty_driver *tty_drivers[256] = {
    [TTY_CONSOLE_MAJOR] = NULL, // will be filled in by create_stdio
    [TTY_PSEUDO_MASTER_MAJOR] = &pty_master,
    [TTY_PSEUDO_SLAVE_MAJOR] = &pty_slave,
};

// lock this before locking a tty
lock_t ttys_lock = LOCK_INITIALIZER;

struct tty *tty_alloc(struct tty_driver *driver, int type, int num) {
    // Zero everything: the driver-specific union at the end (real tty thread,
    // pty state) is only partly filled in by the per-driver init hooks, so a
    // plain malloc leaves fields like pty.packet_mode holding whatever the
    // previous owner of the block left there -- and a recycled "true" turns
    // packet mode on for a pty nobody ever asked it of.
    struct tty *tty = calloc(1, sizeof(struct tty));
    if (tty == NULL)
        return NULL;

    tty->refcount = 0;
    tty->driver = driver;
    tty->type = type;
    tty->num = num;
    tty->atime = (dword_t) boot_time;
    tty->mtime = (dword_t) boot_time;
    tty->ctime = (dword_t) boot_time;
    tty->hung_up = false;
    tty->hangup_gen = 0;
    tty->ever_opened = false;
    tty->session = 0;
    tty->fg_group = 0;
    list_init(&tty->fds);

    tty->termios.iflags = ICRNL_ | IXON_;
    tty->termios.oflags = OPOST_ | ONLCR_;
    // Linux keeps this per driver rather than using one value everywhere:
    // drivers/tty/vt/vt.c takes tty_std_termios as-is for the console, which
    // includes HUPCL, while drivers/tty/pty.c overrides c_cflag to
    // B38400|CS8|CREAD for both the master and the slave. Key off the device
    // type rather than the driver identity: the app drives its terminals
    // through a tty_driver of its own, but pty_open_fake registers them under
    // TTY_PSEUDO_SLAVE_MAJOR, so to the guest they are pty slaves.
    tty->termios.cflags = B38400_ | CS8_ | CREAD_;
    if (type != TTY_PSEUDO_MASTER_MAJOR && type != TTY_PSEUDO_SLAVE_MAJOR)
        tty->termios.cflags |= HUPCL_;
    tty->termios.lflags = ISIG_ | ICANON_ | ECHO_ | ECHOE_ | ECHOK_ | ECHOCTL_ | ECHOKE_ | IEXTEN_;
    // from include/asm-generic/termios.h
    memcpy(tty->termios.cc, "\003\034\177\025\004\0\1\0\021\023\032\0\022\017\027\026\0\0\0", 19);
    tty->winsize = (struct winsize_) {.row = 24, .col = 80};

    lock_init(&tty->lock, "tty_alloc\0");
    lock_init(&tty->fds_lock, "tty_alloc_fds\0");
    cond_init(&tty->produced);
    cond_init(&tty->consumed);
    cond_init(&tty->flow_resumed);
    memset(tty->buf_flag, false, sizeof(tty->buf_flag));
    tty->bufsize = 0;
    tty->packet_flags = 0;

    return tty;
}

struct tty *tty_get(struct tty_driver *driver, int type, int num) {
    lock(&ttys_lock, 0);
    struct tty *tty = driver->ttys[num];
    // pty_reserve_next stores 1 to avoid races on the same tty
    if (tty == NULL || tty == (void *) 1 /* ew */) {
        tty = tty_alloc(driver, type, num);
        if (tty == NULL) {
            unlock(&ttys_lock);
            return ERR_PTR(_ENOMEM);
        }

        if (driver->ops->init) {
            int err = driver->ops->init(tty);
            if (err < 0) {
                unlock(&ttys_lock);
                return ERR_PTR(err);
            }
        }
        driver->ttys[num] = tty;
    }
    lock(&tty->lock, 0);
    tty->refcount++;
    tty->ever_opened = true;
    unlock(&tty->lock);
    unlock(&ttys_lock);
    return tty;
}

static struct tty *get_slave_side_tty(struct tty *tty) {
  if (tty->type == TTY_PSEUDO_MASTER_MAJOR) {
      return tty->pty.other;
  } else {
      return tty;
  }
}

// True only if the tty was hung up AFTER this descriptor was opened. See
// tty_open and struct tty's hangup_gen.
static bool tty_fd_hung_up(struct fd *fd) {
    return fd->tty != NULL && fd->tty_hangup_gen != fd->tty->hangup_gen;
}

static bool tty_has_open_fds(struct tty *tty) {
    lock(&tty->fds_lock, 0);
    bool has_open_fds = !list_empty(&tty->fds);
    unlock(&tty->fds_lock);
    return has_open_fds;
}

static bool pty_slave_closed_by_users(struct tty *tty) {
    return tty->driver == &pty_slave && tty->ever_opened && !tty_has_open_fds(tty);
}

static void tty_poll_wakeup(struct tty *tty, int events) {
    unlock(&tty->lock);
    struct fd *fd;
    lock(&tty->fds_lock, 0);
    list_for_each_entry(&tty->fds, fd, tty_other_fds) {
        poll_wakeup(fd, events);
    }
    unlock(&tty->fds_lock);
    lock(&tty->lock, 0);
}

static void tty_poll_wakeup_unlocked(struct tty *tty, int events) {
    struct fd *fd;
    lock(&tty->fds_lock, 0);
    list_for_each_entry(&tty->fds, fd, tty_other_fds) {
        poll_wakeup(fd, events);
    }
    unlock(&tty->fds_lock);
}

void tty_release(struct tty *tty) {
    lock(&tty->lock, 0);
    if (--tty->refcount == 0) {
        struct tty_driver *driver = tty->driver;
        if (driver->ops->cleanup)
            driver->ops->cleanup(tty);
        driver->ttys[tty->num] = NULL;
        unlock(&tty->lock);
        cond_destroy(&tty->produced);
        free(tty);
    } else {
        // bit of a hack
        struct tty *master = NULL;
        if (tty->driver == &pty_slave && tty->pty.other != NULL)
            master = tty->pty.other;
        unlock(&tty->lock);
        if (master != NULL && pty_slave_closed_by_users(tty)) {
            lock(&master->lock, 0);
            tty_poll_wakeup(master, POLL_READ | POLL_HUP);
            // Also wake a blocking read() on the master (see tty_close).
            notify(&master->produced);
            unlock(&master->lock);
        }
    }
}

// must call with group->lock and tty->lock
static void tty_set_controlling_locked(struct tgroup *group, struct tty *tty) {
    if (group->tty == NULL) {
        tty->refcount++;
        group->tty = tty;
        tty->session = group->sid;
        tty->fg_group = group->pgid;
    }
}

struct tty *tty_lookup_ref(int type, int num, struct tty *expected) {
    if (type < 0 || type > 255 || num < 0)
        return NULL;
    lock(&ttys_lock, 0);
    struct tty *tty = NULL;
    struct tty_driver *driver = tty_drivers[type];
    if (driver != NULL && (unsigned) num < driver->limit) {
        tty = driver->ttys[num];
        // pty_reserve_next parks a 1 in the slot to reserve the number before
        // there is a tty there (see tty_get)
        if (tty == (void *) 1)
            tty = NULL;
        // Compare only -- `expected` may already be freed.
        if (tty != NULL && expected != NULL && tty != expected)
            tty = NULL;
    }
    if (tty != NULL) {
        lock(&tty->lock, 0);
        tty->refcount++;
        unlock(&tty->lock);
    }
    unlock(&ttys_lock);
    return tty;
}

void tty_put(struct tty *tty) {
    if (tty == NULL)
        return;
    lock(&ttys_lock, 0);
    tty_release(tty);
    unlock(&ttys_lock);
}

// by default, /dev/console is /dev/tty1
int console_major = TTY_CONSOLE_MAJOR;
int console_minor = 1;

int tty_open(struct tty *tty, struct fd *fd) {
    // TIOCEXCL means exclusive use: while it is set, only a privileged process
    // may open this terminal again. AOK accepted the ioctl and enforced
    // nothing, which is worse than not implementing it -- a program that sets
    // TIOCEXCL to keep a second reader off its line was told it had succeeded.
    //
    // Linux applies this to /dev/tty as well, so the process that set the flag
    // cannot reopen its own terminal either; dup() is exempt because it is not
    // an open. Both measured against Linux 6.12, both matched here.
    lock(&tty->lock, 0);
    bool excl = tty->excl;
    unlock(&tty->lock);
    if (excl && !superuser())
        return _EBUSY;

    fd->tty = tty;

    // A hangup belongs to the descriptors that were open when it happened, and
    // a fresh open of the same terminal must get a working tty -- that is what
    // it means on Linux. AOK modelled it as one sticky flag on the tty, so the
    // first hangup killed the terminal for good.
    //
    // The System Console is where that showed: something hangs up tty1 early in
    // boot, getty's descriptors correctly go EIO and it exits, init respawns it
    // -- and the NEW getty's fresh open was still EIO, so it died again until
    // init gave up with `Id "1" respawning too fast`. Writing to /dev/tty1 or
    // /dev/console returned EIO with a healthy getty holding both. Restarting
    // the app did not help, because the same hangup happens again every boot.
    lock(&tty->fds_lock, 0);
    list_add(&tty->fds, &fd->tty_other_fds);
    unlock(&tty->fds_lock);
    lock(&tty->lock, 0);
    fd->tty_hangup_gen = tty->hangup_gen;
    unlock(&tty->lock);

    if (!(fd->flags & O_NOCTTY_) && tty->driver != &pty_master) {
        // Make this our controlling terminal if:
        // - the terminal doesn't already have a session
        // - we're a session leader
        lock(&current->group->lock, 0);
        lock(&tty->lock, 0);
        if (tty->session == 0 && tgroup_is_session_leader(current->group))
            tty_set_controlling_locked(current->group, tty);
        unlock(&tty->lock);
        unlock(&current->group->lock);
    }


    return 0;
}

static int tty_device_open(int major, int minor, struct fd *fd) {
    struct tty *tty;
    if (major == TTY_ALTERNATE_MAJOR) {
        if (minor == DEV_TTY_MINOR) {
            lock(&ttys_lock, 0);
            lock(&current->group->lock, 0);
            tty = current->group->tty;
            unlock(&current->group->lock);
            if (tty != NULL) {
                lock(&tty->lock, 0);
                tty->refcount++;
                unlock(&tty->lock);
            }
            unlock(&ttys_lock);
            if (tty == NULL)
                return _ENXIO;
        } else if (minor == DEV_CONSOLE_MINOR) {
            // Linux rule: opening /dev/console NEVER attaches it as the
            // caller's controlling terminal (the kernel forces noctty for
            // this device). That is what keeps PID 1 ctty-less: the kernel
            // hands init console fds, but the console tty's session stays
            // free, so getty@tty1's setsid+TIOCSCTTY can claim it. iSH's
            // auto-attach in tty_open() didn't know about the alias, so a
            // systemd boot left PID 1's session owning tty1 forever and
            // every getty parked pre-exec inside sd's acquire_terminal()
            // wait ("[(agetty)]" in ps, no login prompt on the console,
            // TIOCSCTTY -> EPERM until the end of time). Force the
            // O_NOCTTY_ bit across the underlying open, then restore the
            // caller's flags so it doesn't leak into F_GETFL.
            int had_noctty = fd->flags & O_NOCTTY_;
            fd->flags |= O_NOCTTY_;
            int err = tty_device_open(console_major, console_minor, fd);
            if (!had_noctty)
                fd->flags &= ~O_NOCTTY_;
            return err;
        } else if (minor == DEV_PTMX_MINOR) {
            return ptmx_open(fd);
        } else {
            return _ENXIO;
        }
    } else {
        struct tty_driver *driver = tty_drivers[major];
        assert(driver != NULL);
        tty = tty_get(driver, major, minor);
        if (IS_ERR(tty))
            return (int)PTR_ERR(tty);
    }

    if (tty->driver->ops->open) {
        int err = tty->driver->ops->open(tty);
        if (err < 0) {
            lock(&ttys_lock, 0);
            tty_release(tty);
            unlock(&ttys_lock);
            return err;
        }
    }

    return tty_open(tty, fd);
}

static int tty_close(struct fd *fd) {
    if (fd->tty != NULL) {
        struct tty *wake_master = NULL;
        lock(&fd->tty->fds_lock, 0);
        list_remove_safe(&fd->tty_other_fds);
        if (fd->tty->driver == &pty_slave &&
                fd->tty->ever_opened &&
                list_empty(&fd->tty->fds) &&
                fd->tty->pty.other != NULL)
            wake_master = fd->tty->pty.other;
        unlock(&fd->tty->fds_lock);
        if (wake_master != NULL) {
            lock(&wake_master->lock, 0);
            tty_poll_wakeup(wake_master, POLL_READ | POLL_HUP);
            // Wake a thread blocked in a plain blocking read() on the master too.
            // tty_poll_wakeup only nudges pollers; a reader sleeping in tty_read's
            // wait_for(&produced) must be notified explicitly or it never re-checks
            // pty_is_half_closed_master and hangs forever once the slave is gone
            // (the tmux/script "exit wedges" bug).
            notify(&wake_master->produced);
            unlock(&wake_master->lock);
        }
        lock(&ttys_lock, 0);
        tty_release(fd->tty);
        unlock(&ttys_lock);
    }
    return 0;
}

static void tty_input_wakeup(struct tty *tty) {
    notify(&tty->produced);
    tty_poll_wakeup(tty, POLL_READ);
}

static int tty_push_char(struct tty *tty, char ch, bool flag, int blocking) {
    while (tty->bufsize >= sizeof(tty->buf)) {
        if (!blocking)
            return _EAGAIN;
        if (wait_for(&tty->consumed, &tty->lock, NULL))
            return _EINTR;
    }
    tty->buf[tty->bufsize] = ch;
    tty->buf_flag[tty->bufsize++] = flag;
    return 0;
}

static void tty_echo(struct tty *tty, const char *data, size_t size) {
    tty->driver->ops->write(tty, data, size, false);
}

// Canonical-mode echo is emitted in tiny fragments (each typed char, "\b \b"
// erases, "\r\n", "^" prefixes), and each tty_echo is a full driver write --
// for PTYs, a round-trip per keystroke. Batch the fragments into a caller-owned
// stack buffer and flush in as few writes as possible. Callers must flush
// before waking a reader (so an echoed "\r\n" reaches the terminal before the
// reader runs) and at the end of the input batch.
static void tty_echo_flush(struct tty *tty, char *buf, size_t *len) {
    if (*len > 0) {
        tty_echo(tty, buf, *len);
        *len = 0;
    }
}
static void tty_echo_buffered(struct tty *tty, char *buf, size_t cap, size_t *len, const char *data, size_t size) {
    if (size > cap) {
        tty_echo_flush(tty, buf, len);
        tty_echo(tty, data, size);
        return;
    }
    if (*len + size > cap)
        tty_echo_flush(tty, buf, len);
    memcpy(buf + *len, data, size);
    *len += size;
}

static bool tty_trace_comm(const char *comm) {
    if (comm == NULL)
        return false;
    return strcmp(comm, "apk") == 0 ||
        strcmp(comm, "wget") == 0 ||
        strcmp(comm, "curl") == 0 ||
        strcmp(comm, "ping") == 0 ||
        strcmp(comm, "cat") == 0 ||
        strcmp(comm, "grep") == 0 ||
        strcmp(comm, "which") == 0 ||
        strcmp(comm, "install") == 0 ||
        strncmp(comm, "deboots", 7) == 0 ||
        strncmp(comm, "debootstrap", 11) == 0 ||
        strncmp(comm, "update-ca-certi", 15) == 0;
}

static bool tty_trace_signal_enabled(void) {
    if (current == NULL)
        return false;
    return tty_trace_comm(current->comm);
}

static bool tty_trace_timed_raw_enabled(struct tty *tty) {
    if (current == NULL || tty == NULL)
        return false;
    return !(tty->termios.lflags & ICANON_) && tty->termios.cc[VTIME_] > 0;
}

static void tty_stop_output_locked(struct tty *tty) {
    tty->stopped = true;
}

static void tty_start_output_locked(struct tty *tty) {
    tty->stopped = false;
    notify(&tty->flow_resumed);
}

// XON/XOFF input processing: ^S stops this terminal's output, ^Q restarts it,
// and with IXANY any character at all restarts it. Returns true when the
// character WAS flow control and so must not reach the reader.
//
// AOK never looked at VSTART/VSTOP outside echo rendering, so ^S and ^Q were
// delivered to the program as literal 0x13/0x11 and stopped nothing -- someone
// pressing ^S to pause a scrolling listing got two junk bytes into whatever
// was reading, and no pause.
//
// Not called before the VLNEXT check: ^V quotes a ^S into ordinary data, and
// output keeps flowing. Measured that way round on Linux.
static bool tty_flow_control(struct tty *tty, char ch, dword_t iflags,
                             const unsigned char *cc) {
    if (!(iflags & IXON_) || ch == '\0')
        return false;
    if (ch == (char) cc[VSTART_]) {
        tty_start_output_locked(tty);
        return true;
    }
    if (ch == (char) cc[VSTOP_]) {
        tty_stop_output_locked(tty);
        return true;
    }
    // IXANY: any character resumes, and is still delivered as data.
    if (iflags & IXANY_)
        tty_start_output_locked(tty);
    return false;
}

static bool tty_send_input_signal(struct tty *tty, char ch, sigset_t_ *queue) {
    if (!(tty->termios.lflags & ISIG_))
        return false;
    unsigned char *cc = tty->termios.cc;
    int sig;
    if (ch == '\0')
        return false; // '\0' is used to disable cc entries
    else if (ch == cc[VINTR_])
        sig = SIGINT_;
    else if (ch == cc[VQUIT_])
        sig = SIGQUIT_;
    else if (ch == cc[VSUSP_])
        sig = SIGTSTP_;
    else
        return false;

    if (tty->fg_group != 0) {
        if (!(tty->termios.lflags & NOFLSH_))
            tty->bufsize = 0;
        sigset_add(queue, sig);
        if (sig == SIGINT_ || tty_trace_signal_enabled()) {
            printk("INFO: tty signal input tty=%d fg_group=%d ch=%#x sig=%d current_pid=%d comm=%s\n",
                   tty->num, tty->fg_group, (unsigned char) ch, sig,
                   current != NULL ? current->pid : -1,
                   current != NULL ? current->comm : "<none>");
        }
    }
    return true;
}

ssize_t tty_input(struct tty *tty, const char *input, size_t size, bool blocking) {
    int err = 0;
    size_t done_size = 0;
    sigset_t_ queue = 0; // to prevent having to lock tty->lock and pids_lock at the same time

    lock(&tty->lock, 0);
    dword_t lflags = tty->termios.lflags;
    dword_t iflags = tty->termios.iflags;
    unsigned char *cc = tty->termios.cc;

#define SHOULD_ECHOCTL(ch) \
    (lflags & ECHOCTL_ && \
     ((0 <= ch && ch < ' ') || ch == '\x7f') && \
     !(ch == '\t' || ch == '\n' || ch == cc[VSTART_] || ch == cc[VSTOP_]))

    if (lflags & ICANON_) {
        char echo_buf[512];
        size_t echo_len = 0;
        for (size_t i = 0; i < size; i++) {
            done_size++;
            char ch = input[i];
            bool echo = lflags & ECHO_;

            if (iflags & INLCR_ && ch == '\n')
                ch = '\r';
            else if (iflags & ICRNL_ && ch == '\r')
                ch = '\n';
            if (iflags & IGNCR_ && ch == '\r')
                continue;

            if (tty->lnext_pending) {
                // VLNEXT armed: this character is data, whatever it is.
                tty->lnext_pending = false;
                goto no_special;
            }

            if (tty_flow_control(tty, ch, iflags, cc))
                continue;
            if (ch == '\0') {
                // '\0' is used to disable cc entries
                goto no_special;
            } else if ((lflags & IEXTEN_) && ch == cc[VLNEXT_]) {
                // ^V quotes the next character. It was not implemented at all,
                // so the ^V itself reached the reader as data AND the character
                // it was quoting kept its special meaning -- wrong both ways.
                tty->lnext_pending = true;
                if (echo && (lflags & ECHO_) && (lflags & ECHOCTL_)) {
                    // Linux echoes "^" and then backs over it once the quoted
                    // character arrives; showing the caret alone is the visible
                    // half and needs no cursor bookkeeping.
                    tty_echo_buffered(tty, echo_buf, sizeof(echo_buf), &echo_len, "^", 1);
                    tty_echo_buffered(tty, echo_buf, sizeof(echo_buf), &echo_len, "\b", 1);
                }
                echo = false;
                continue;
            } else if ((lflags & IEXTEN_) && ch == cc[VWERASE_] && (lflags & ICANON_)) {
                // ^W erases the previous WORD: trailing whitespace first, then
                // the run of non-whitespace before it. Not implemented before,
                // so ^W was simply dropped and the word stayed.
                bool visual = (lflags & ECHO_) && (lflags & ECHOE_);
                while (tty->bufsize > 0 && !tty->buf_flag[tty->bufsize - 1] &&
                        (tty->buf[tty->bufsize - 1] == ' ' ||
                         tty->buf[tty->bufsize - 1] == '\t')) {
                    tty->bufsize--;
                    if (visual)
                        tty_echo_buffered(tty, echo_buf, sizeof(echo_buf), &echo_len, "\b \b", 3);
                }
                while (tty->bufsize > 0 && !tty->buf_flag[tty->bufsize - 1] &&
                        tty->buf[tty->bufsize - 1] != ' ' &&
                        tty->buf[tty->bufsize - 1] != '\t') {
                    tty->bufsize--;
                    if (visual) {
                        tty_echo_buffered(tty, echo_buf, sizeof(echo_buf), &echo_len, "\b \b", 3);
                        if (SHOULD_ECHOCTL(tty->buf[tty->bufsize]))
                            tty_echo_buffered(tty, echo_buf, sizeof(echo_buf), &echo_len, "\b \b", 3);
                    }
                }
                echo = false;
                continue;
            } else if (ch == cc[VERASE_] || ch == cc[VKILL_]) {
                echo = lflags & ECHOK_;
                ssize_t count = tty->bufsize;
                // ECHOKE enables erasing the line visually (backspace-space-backspace
                // per character, same as ECHOE below); plain ECHOK without ECHOKE
                // instead echoes the kill character itself followed by a newline,
                // leaving the killed text on screen. There's no non-erasing variant
                // for VERASE itself, so erase_visually stays true on that path.
                bool erase_visually = true;
                if (ch == cc[VERASE_] && tty->bufsize > 0) {
                    echo = lflags & ECHOE_;
                    count = 1;
                } else {
                    erase_visually = lflags & ECHOKE_;
                }
                if (!(lflags & ECHO_))
                    echo = false;
                if (echo && !erase_visually) {
                    char kill_ch = ch;
                    if (SHOULD_ECHOCTL(kill_ch)) {
                        tty_echo_buffered(tty, echo_buf, sizeof(echo_buf), &echo_len, "^", 1);
                        kill_ch ^= '\100';
                    }
                    tty_echo_buffered(tty, echo_buf, sizeof(echo_buf), &echo_len, &kill_ch, 1);
                    tty_echo_buffered(tty, echo_buf, sizeof(echo_buf), &echo_len, "\r\n", 2);
                }
                for (int i = 0; i < count; i++) {
                    // don't delete past a flag
                    if (tty->buf_flag[tty->bufsize - 1])
                        break;
                    tty->bufsize--;
                    if (echo && erase_visually) {
                        tty_echo_buffered(tty, echo_buf, sizeof(echo_buf), &echo_len, "\b \b", 3);
                        if (SHOULD_ECHOCTL(tty->buf[tty->bufsize]))
                            tty_echo_buffered(tty, echo_buf, sizeof(echo_buf), &echo_len, "\b \b", 3);
                    }
                }
                echo = false;
            } else if (ch == cc[VEOF_]) {
                ch = '\0';
                goto canon_wake;
            } else if (ch == '\n' || ch == cc[VEOL_]) {
                // echo it now, before the read call goes through
                if (echo)
                    tty_echo_buffered(tty, echo_buf, sizeof(echo_buf), &echo_len, "\r\n", 2);
canon_wake:
                err = tty_push_char(tty, ch, /*flag*/true, blocking);
                if (err < 0) {
                    done_size--;
                    break;
                }
                echo = false;
                tty_echo_flush(tty, echo_buf, &echo_len);
                tty_input_wakeup(tty);
            } else {
                if (!tty_send_input_signal(tty, ch, &queue)) {
no_special:
                    err = tty_push_char(tty, ch, /*flag*/false, blocking);
                    if (err < 0) {
                        done_size--;
                        break;
                    }
                }
            }

            if (echo) {
                if (SHOULD_ECHOCTL(ch)) {
                    tty_echo_buffered(tty, echo_buf, sizeof(echo_buf), &echo_len, "^", 1);
                    ch ^= '\100';
                }
                tty_echo_buffered(tty, echo_buf, sizeof(echo_buf), &echo_len, &ch, 1);
            }
        }
        tty_echo_flush(tty, echo_buf, &echo_len);
    } else {
        for (size_t i = 0; i < size; i++) {
            done_size++;
            if (tty_flow_control(tty, input[i], iflags, cc))
                continue;
            if (tty_send_input_signal(tty, input[i], &queue))
                continue;
            while (tty->bufsize >= sizeof(tty->buf)) {
                // Wake readers before sleeping for space, otherwise a large
                // blocking PTY write can deadlock waiting on a consumer that
                // was never notified data is available.
                if (tty->bufsize > 0)
                    tty_input_wakeup(tty);
                err = _EAGAIN;
                if (!blocking)
                    break;
                err = wait_for(&tty->consumed, &tty->lock, NULL);
                if (err < 0)
                    break;
            }
            if (err < 0) {
                done_size--;
                break;
            }
            assert(tty->bufsize < sizeof(tty->buf));
            tty->buf[tty->bufsize++] = input[i];
        }
        if (tty->bufsize > 0)
            tty_input_wakeup(tty);
    }

    pid_t_ fg_group = tty->fg_group;
    assert(tty->bufsize <= sizeof(tty->buf));
    unlock(&tty->lock);

    if (fg_group != 0) {
        for (int sig = 1; sig < NUM_SIGS; sig++) {
            if (sigset_has(queue, sig)) {
                send_group_signal(fg_group, sig, SIGINFO_NIL);
            }
        }
    }

    if (done_size > 0) {
        dword_t now = (dword_t) time(NULL);
        lock(&tty->lock, 0);
        tty->atime = now;
        unlock(&tty->lock);
        return done_size;
    }
    return err;
}

// expects bufsize <= tty->bufsize
static void tty_read_into_buf(struct tty *tty, void *buf, size_t bufsize) {
    assert(bufsize <= tty->bufsize);
    memcpy(buf, tty->buf, bufsize);
    tty->bufsize -= bufsize;
    memmove(tty->buf, tty->buf + bufsize, tty->bufsize); // magic!
    memmove(tty->buf_flag, tty->buf_flag + bufsize, tty->bufsize);
    notify(&tty->consumed);
}

static size_t tty_canon_size(struct tty *tty) {
    bool *flag_ptr = memchr(tty->buf_flag, true, tty->bufsize);
    if (flag_ptr == NULL)
        return -1;
    return flag_ptr - tty->buf_flag + 1;
}

static bool pty_is_half_closed_master(struct tty *tty) {
    if (tty->driver != &pty_master)
        return false;

    struct tty *slave = tty->pty.other;
    if (slave == NULL)
        return true;
    return slave->hung_up || pty_slave_closed_by_users(slave);
}

static bool tty_is_current(struct tty *tty) {
    lock(&current->group->lock, 0);
    bool is_current = current->group->tty == tty;
    unlock(&current->group->lock);
    return is_current;
}

// must call with tty->lock
// Linux's tty_check_change: may this process touch the terminal right now?
//
// A process in the terminal's foreground group always may. A BACKGROUND
// process is stopped instead -- SIGTTIN for reading, SIGTTOU for writing or
// for changing the terminal's settings -- so that it cannot race the
// foreground job for the keyboard. Three exceptions, in Linux's order:
//
//   the signal is ignored or blocked: the caller has said it does not want to
//   be stopped, so SIGTTOU simply proceeds. SIGTTIN cannot -- there is no
//   sensible input to hand back -- and becomes EIO;
//
//   the process group is orphaned: nothing outside it could ever continue it,
//   so stopping it would wedge it forever. EIO instead;
//
//   otherwise the whole group is stopped and the syscall restarts afterwards
//   (Linux returns ERESTARTSYS, so the stop is invisible to the caller).
//
// AOK had only a partial version of this on the read path: it signalled the
// calling task alone rather than the group, treated an ignored SIGTTOU as EIO
// rather than as permission, had no orphan test, and returned EINTR where
// Linux restarts. It was not called from the write or ioctl paths at all.
//
// Caller holds tty->lock. It is dropped and retaken here: the ordering in this
// file is pids_lock before tty->lock, and the group signal goes out holding
// neither.
static int tty_check_change_locked(struct tty *tty, int sig) {
    unlock(&tty->lock);
    complex_lockt(&pids_lock, 0);
    lock(&current->group->lock, 0);
    bool is_current = current->group->tty == tty;
    pid_t_ pgid = current->group->pgid;
    pid_t_ sid = current->group->sid;
    unlock(&current->group->lock);

    lock(&tty->lock, 0);
    pid_t_ fg_group = tty->fg_group;
    unlock(&tty->lock);

    int err = 0;
    pid_t_ stop_pgid = 0;
    if (is_current && fg_group != 0 && pgid != fg_group) {
        if (signal_is_ignored_or_blocked(sig))
            err = sig == SIGTTIN_ ? _EIO : 0;
        else if (pgroup_is_orphaned(pgid, sid))
            err = _EIO;
        else
            stop_pgid = pgid, err = _ERESTART;
    }
    unlock(&pids_lock);

    if (stop_pgid != 0)
        send_group_signal(stop_pgid, sig, SIGINFO_NIL);

    lock(&tty->lock, 0);
    return err;
}

static ssize_t tty_read(struct fd *fd, void *buf, size_t bufsize) {
    // important because otherwise we'll block
    if (bufsize == 0)
        return 0;

    int err = 0;
    struct tty *tty = fd->tty;
    lock(&tty->lock, 0);
    if (tty_fd_hung_up(fd)) {
        goto error;
    }

    err = tty_check_change_locked(tty, SIGTTIN_);
    if (err < 0)
        goto error;

    int bufsize_extra = 0;
    if (tty->driver == &pty_master && tty->pty.packet_mode) {
        char *cbuf = buf;
        *cbuf++ = tty->packet_flags;
        bufsize--;
        bufsize_extra++;
        buf = cbuf;
        if (tty->packet_flags != 0) {
            bufsize = 0;
            goto out;
        }

        // check again in case bufsize was 1
        if (bufsize == 0)
            goto out;
    }

    // wait loop(s)
    if (tty->termios.lflags & ICANON_) {
        size_t canon_size;
        while ((canon_size = tty_canon_size(tty)) == (size_t) -1) {
            err = _EIO;
            if (pty_is_half_closed_master(tty))
                goto error;
            err = _EAGAIN;
            if (fd->flags & O_NONBLOCK_)
                goto error;
            err = wait_for(&tty->produced, &tty->lock, NULL);
            if (err < 0)
                goto error;
        }
        // null byte means eof was typed
        if (tty->buf[canon_size-1] == '\0')
            canon_size--;

        if (bufsize > canon_size)
            bufsize = canon_size;
    } else {
        dword_t min = tty->termios.cc[VMIN_];
        dword_t time = tty->termios.cc[VTIME_];
        if (tty_trace_timed_raw_enabled(tty)) {
            printk("INFO: top tty_read enter pid=%d tty=%d:%d bufsize=%zu req=%zu min=%u time=%u flags=%#x\n",
                   current->pid,
                   tty->driver != NULL ? tty->driver->major : -1,
                   tty->num,
                   tty->bufsize,
                   bufsize,
                   min,
                   time,
                   fd->flags);
        }

        struct timespec timeout;
        // time is in tenths of a second
        timeout.tv_sec = time / 10;
        timeout.tv_nsec = (time % 10) * 100000000;
        struct timespec *timeout_ptr = &timeout;
        if (time == 0)
            timeout_ptr = NULL;

        // MIN==0 with TIME>0 is a read-with-timeout: wait up to TIME tenths
        // for the FIRST byte, then return whatever arrived -- possibly none.
        // The loop below is bounded by `min`, so at min==0 it never ran at all
        // and the read came back instantly with nothing, which is the MIN=0
        // TIME=0 (pure poll) behaviour instead.
        if (min == 0 && time > 0) {
            while (tty->bufsize == 0) {
                err = _EIO;
                if (pty_is_half_closed_master(tty))
                    goto error;
                err = _EAGAIN;
                if (fd->flags & O_NONBLOCK_)
                    goto error;
                err = wait_for(&tty->produced, &tty->lock, timeout_ptr);
                if (err == _ETIMEDOUT)
                    break;
                if (err < 0)
                    goto error;
            }
            err = 0;
        }

        while (tty->bufsize < min) {
            err = _EIO;
            if (pty_is_half_closed_master(tty))
                goto error;
            err = _EAGAIN;
            if (fd->flags & O_NONBLOCK_)
                goto error;
            // there should be no timeout for the first character read
            err = wait_for(&tty->produced, &tty->lock, tty->bufsize == 0 ? NULL : timeout_ptr);
            if (tty_trace_timed_raw_enabled(tty)) {
                printk("INFO: top tty_read wait pid=%d tty=%d:%d bufsize=%zu min=%u time=%u err=%d first=%d\n",
                       current->pid,
                       tty->driver != NULL ? tty->driver->major : -1,
                       tty->num,
                       tty->bufsize,
                       min,
                       time,
                       err,
                       tty->bufsize == 0);
            }
            if (err == _ETIMEDOUT)
                break;
            if (err == _EINTR)
                goto error;
        }
    }

    if (bufsize > tty->bufsize)
        bufsize = tty->bufsize;
    tty_read_into_buf(tty, buf, bufsize);
    if (tty->bufsize > 0 && tty->buf[0] == '\0' && tty->buf_flag[0]) {
        // remove the eof so the next read can succeed
        char dummy;
        tty_read_into_buf(tty, &dummy, 1);
    }

out:
    unlock(&tty->lock);
    return bufsize + bufsize_extra;
error:
    unlock(&tty->lock);
    return err;
}

static ssize_t tty_write(struct fd *fd, const void *buf, size_t bufsize) {
    struct tty *tty = fd->tty;
    lock(&tty->lock, 0);
    if (tty_fd_hung_up(fd)) {
        unlock(&tty->lock);
        return _EIO;
    }

    // A background process writing to its controlling terminal is stopped by
    // SIGTTOU, but only when the terminal asks for it with TOSTOP -- unlike the
    // read side, where SIGTTIN is unconditional. There was no check here at
    // all, so a background job scribbled over the foreground one's screen.
    if (tty->termios.lflags & TOSTOP_) {
        int bg = tty_check_change_locked(tty, SIGTTOU_);
        if (bg < 0) {
            unlock(&tty->lock);
            return bg;
        }
    }

    bool blocking = !(fd->flags & O_NONBLOCK_);

    // Output held by ^S (or by tcflow(TCOOFF)) waits here. This is the point
    // of flow control: the writer stops until the reader says go.
    while (tty->stopped) {
        if (!blocking) {
            unlock(&tty->lock);
            return _EAGAIN;
        }
        int ferr = wait_for(&tty->flow_resumed, &tty->lock, NULL);
        if (ferr < 0) {
            unlock(&tty->lock);
            return ferr;
        }
        if (tty_fd_hung_up(fd)) {
            unlock(&tty->lock);
            return _EIO;
        }
    }

    dword_t oflags = tty->termios.oflags;
    // we have to unlock it now to avoid lock ordering problems with ptys
    // the code below is safe because it only accesses tty->driver which is immutable
    // I reviewed real driver and ios driver and they're safe
    unlock(&tty->lock);

    int err = 0;
    // OPOST processing can grow the buffer by at most 2x (every '\n' -> "\r\n").
    // The vast majority of tty writes are small (prompts, echoed input, single
    // lines), so service those from the stack and only fall back to malloc for
    // large writes, keeping the allocator out of the interactive output path.
    char stackbuf[512];
    char *postbuf = NULL;
    const char *outbuf = buf;
    size_t postbufsize = bufsize;
    if (oflags & OPOST_) {
        char *out;
        if (bufsize <= sizeof(stackbuf) / 2) {
            out = stackbuf;
        } else {
            postbuf = malloc(bufsize * 2);
            if (postbuf == NULL)
                return _ENOMEM;
            out = postbuf;
        }
        postbufsize = 0;
        const char *cbuf = buf;
        for (size_t i = 0; i < bufsize; i++) {
            char ch = cbuf[i];
            if (ch == '\r' && oflags & ONLRET_)
                continue;
            else if (ch == '\r' && oflags & OCRNL_)
                ch = '\n';
            else if (ch == '\n' && oflags & ONLCR_)
                out[postbufsize++] = '\r';
            out[postbufsize++] = ch;
        }
        outbuf = out;
    }
    err = tty->driver->ops->write(tty, outbuf, postbufsize, blocking);
    if (postbuf)
        free(postbuf);
    if (err < 0)
        return err;
    return bufsize;
}

static int tty_poll(struct fd *fd) {
    struct tty *tty = fd->tty;
    lock(&tty->lock, 0);
    int types = 0;
    // Writing to a pty delivers into the *peer* tty's input buffer (see
    // pty_write -> tty_input(tty->pty.other, ...)), so it can block once that
    // buffer is full -- unlike a physical tty, which has no such peer to back
    // up against and stays always-writable. The peer bufsize read is unlocked,
    // matching the existing pty_is_half_closed_master peer-field reads below;
    // poll results are advisory/racy by nature regardless.
    if ((tty->driver == &pty_master || tty->driver == &pty_slave) && tty->pty.other != NULL) {
        if (tty->pty.other->bufsize < sizeof(tty->pty.other->buf))
            types |= POLL_WRITE;
    } else {
        types |= POLL_WRITE;
    }
    if (tty_fd_hung_up(fd)) {
        types |= POLL_READ | POLL_WRITE | POLL_ERR | POLL_HUP;
    } else if (pty_is_half_closed_master(tty)) {
        types |= POLL_READ | POLL_HUP;
    } else if (tty->termios.lflags & ICANON_) {
        if (tty_canon_size(tty) != (size_t) -1)
            types |= POLL_READ;
    } else {
        if (tty->bufsize > 0)
            types |= POLL_READ;
    }
    if (tty->driver == &pty_master && tty->packet_flags != 0)
        types |= POLL_PRI;
    if (tty_trace_timed_raw_enabled(tty)) {
        printk("INFO: top tty_poll pid=%d tty=%d:%d lflags=%#x vmin=%u vtime=%u bufsize=%zu hung=%d types=%#x\n",
               current->pid,
               tty->driver != NULL ? tty->driver->major : -1,
               tty->num,
               tty->termios.lflags,
               tty->termios.cc[VMIN_],
               tty->termios.cc[VTIME_],
               tty->bufsize,
               tty->hung_up,
               types);
    }
    unlock(&tty->lock);
    return types;
}

static ssize_t tty_ioctl_size(int cmd) {
    switch (cmd) {
        case TCGETS_: case TCSETS_: case TCSETSF_: case TCSETSW_:
            return sizeof(struct termios_);
        case TCGETS2_: case TCSETS2_: case TCSETSF2_: case TCSETSW2_:
            return sizeof(struct termios2_);
        case TIOCGWINSZ_: case TIOCSWINSZ_:
            return sizeof(struct winsize_);
        case TIOCGSID_:
        case TIOCGPGRP_: case TIOCSPGRP_:
        case TIOCSPTLCK_: case TIOCGPTN_:
        case TIOCPKT_: case TIOCGPKT_:
        case FIONREAD_:
        case TIOCOUTQ_:
        case TIOCGEXCL_:
            return sizeof(dword_t);
        case TCFLSH_: case TIOCSCTTY_: case TIOCCONS_:
        // tcdrain/tcflow/tcsendbreak reach the kernel as these, and take their
        // argument BY VALUE rather than through a pointer -- so size 0, like
        // TCFLSH beside them, and not sizeof(dword_t).
        case TCSBRK_: case TCXONC_:
        case TIOCNOTTY_: case TIOCEXCL_: case TIOCNXCL_:
            return 0;
    }
    return -1;
}

// tcsetpgrp(): make a process group the terminal's foreground group. AOK
// validated nothing -- the literal TODO here said so -- and stored whatever
// number it was handed, so a shell could point a terminal at a process group
// that does not exist, or at one in somebody else's session, and tcgetpgrp
// would then cheerfully report it back. A job-control shell uses that value to
// decide who owns the keyboard; pointing it at a stranger means ^C goes to
// them.
//
// Linux checks three things in this order, all measured: a negative pgid is
// EINVAL, one that names no live process group is ESRCH, and one belonging to
// another session is EPERM. The foreground group is left untouched whenever
// the call fails.
static int tiocspgrp(struct tty *tty, pid_t_ pgid) {
    int err = 0;
    unlock(&tty->lock);
    complex_lockt(&pids_lock, 0);
    lock(&current->group->lock, 0);
    lock(&tty->lock, 0);
    pid_t_ sid = current->group->sid;
    bool is_current = current->group->tty == tty;
    unlock(&current->group->lock);

    if (!is_current || sid != tty->session) {
        err = _ENOTTY;
        goto out;
    }
    if (pgid < 0) {
        err = _EINVAL;
        goto out;
    }

    // A pid nothing lives under any more has an empty pgroup list, and is
    // ESRCH exactly like one that was never allocated. pgid 0 lands here too:
    // Linux looks it up like any other and finds nothing.
    struct pid *pid = pid_get(pgid);
    if (pid == NULL || list_empty(&pid->pgroup)) {
        err = _ESRCH;
        goto out;
    }

    // Every member of a process group shares its session, so one member
    // settles it. Our own group's sid is already in hand and its lock is
    // released, so don't retake it.
    struct tgroup *tgroup;
    list_for_each_entry(&pid->pgroup, tgroup, pgroup) {
        pid_t_ member_sid;
        if (tgroup == current->group) {
            member_sid = sid;
        } else {
            lock(&tgroup->lock, 0);
            member_sid = tgroup->sid;
            unlock(&tgroup->lock);
        }
        if (member_sid != tty->session)
            err = _EPERM;
        break;
    }
    if (err < 0)
        goto out;

    tty->fg_group = pgid;
    STRACE("tty group set to = %d\n", tty->fg_group);

out:
    unlock(&pids_lock);
    return err;
}

static int tiocsctty(struct tty *tty, int force) {
    int err = 0;
    unlock(&tty->lock);
    complex_lockt(&pids_lock, 0);
    lock(&current->group->lock, 0);
    lock(&tty->lock, 0);
    // do nothing if this is already our controlling tty
    if (tgroup_is_session_leader(current->group) && current->group->sid == tty->session)
        goto out;
    // The caller must be a session leader AND not already have a terminal.
    // The leader half was missing, so any process at all could take a terminal
    // that a session was using -- and taking it away from that session hangs
    // it up, which is how the probe for this kept killing its own shell.
    if (!tgroup_is_session_leader(current->group) || current->group->tty != NULL) {
        err = _EPERM;
        goto out;
    }

    if (tty->session) {
        if (force == 1 && superuser()) {
            // steal it
            struct pid *pid = pid_get(tty->session);
            struct tgroup *tgroup;
            list_for_each_entry(&pid->session, tgroup, session) {
                lock(&tgroup->lock, 0);
                if (tgroup->tty == tty) {
                    tgroup->tty = NULL;
                    tty->refcount--;
                }
                unlock(&tgroup->lock);
            }
        } else {
            err = _EPERM;
            goto out;
        }
    }

    tty_set_controlling_locked(current->group, tty);
out:
    unlock(&current->group->lock);
    unlock(&pids_lock);
    return err;
}

// TIOCNOTTY: give up the controlling terminal. Two different operations
// wearing one name, and Linux (no_tty()) does both:
//
//   a session LEADER hangs the session up -- SIGHUP then SIGCONT to the
//   terminal's foreground group, and every process group in the session loses
//   the terminal;
//
//   anyone else drops only their own thread group's reference and leaves the
//   session, its terminal and its other members entirely alone.
//
// Both arms measured against Linux 6.12. AOK implemented neither: TIOCNOTTY
// was ENOTTY, so a process daemonising by hand had no way to detach from its
// terminal and kept getting the session's SIGHUP.
//
// Signals are not sent from in here. Like every other hangup in this file the
// targets go back to the caller, which sends them after dropping tty->lock.
static int tiocnotty(struct tty *tty, struct tty_hangup_targets *hup) {
    int err = 0;
    unlock(&tty->lock);
    complex_lockt(&pids_lock, 0);
    lock(&current->group->lock, 0);
    lock(&tty->lock, 0);

    // It has to be OUR controlling terminal. Holding a descriptor to somebody
    // else's terminal does not entitle us to disconnect them from it.
    if (current->group->tty != tty) {
        err = _ENOTTY;
        goto out;
    }

    if (tgroup_is_session_leader(current->group)) {
        // Only the foreground group is signalled, never the session leader
        // separately: disassociate_ctty() kills the tty's pgrp and nothing
        // else. The leader is usually a member of that group and so is hit
        // anyway, which is what makes the distinction easy to miss.
        hup->fg_group = tty->fg_group;
        hup->session = 0;

        struct pid *pid = pid_get(tty->session);
        if (pid != NULL) {
            struct tgroup *tgroup;
            list_for_each_entry(&pid->session, tgroup, session) {
                // our own group->lock is already held
                bool self = tgroup == current->group;
                if (!self)
                    lock(&tgroup->lock, 0);
                if (tgroup->tty == tty) {
                    tgroup->tty = NULL;
                    tty->refcount--;
                }
                if (!self)
                    unlock(&tgroup->lock);
            }
        }
        tty->session = 0;
        tty->fg_group = 0;
    }

    // The loop above covers us when we are the leader; a non-leader is not
    // reached by it and drops its own reference here. Refcount is adjusted
    // directly rather than through tty_release() -- that wants ttys_lock,
    // and the caller's own descriptor is holding this terminal up regardless.
    if (current->group->tty == tty) {
        current->group->tty = NULL;
        tty->refcount--;
    }

out:
    unlock(&current->group->lock);
    unlock(&pids_lock);
    return err;
}

// The SESSION that owns the terminal, which is not tiocgpgrp's question: the
// foreground process group changes with every job, and the session does not.
// Same shape otherwise, including the "this is not your controlling terminal"
// refusal -- a caller holding a descriptor to someone else's tty is not
// entitled to an answer.
//
// Added because tcgetsid(3) is how a library asks whether it can do job
// control at all, and the shim had nothing to route it to.
static int tiocgsid(struct tty *tty, pid_t_ *out) {
    int err = 0;
    struct tty *slave = get_slave_side_tty(tty);
    if (slave != tty)
        lock(&slave->lock, 0);

    if (tty == slave && (!tty_is_current(slave) || slave->session == 0)) {
        err = _ENOTTY;
        goto out_unlock;
    }
    *out = slave->session;
    STRACE("tty session = %d\n", slave->session);

out_unlock:
    if (slave != tty)
        unlock(&slave->lock);
    return err;
}

static int tiocgpgrp(struct tty *tty, pid_t_ *fg_group) {
    int err = 0;
    struct tty *slave = get_slave_side_tty(tty);
    if (slave != tty) {
        lock(&slave->lock,0);
    }

    if (tty == slave && (!tty_is_current(slave) || slave->fg_group == 0)) {
        err = _ENOTTY;
        goto error_no_ctrl_tty;
    }
    *fg_group = slave->fg_group;
    STRACE("tty group = %d\n", slave->fg_group);

error_no_ctrl_tty:
    if (slave != tty)
        unlock(&slave->lock);
    return err;
}

// These ioctls are separated out because they have to operate on the slave
// side of a pseudoterminal pair even if the master is specified
static int tty_mode_ioctl(struct tty *in_tty, int cmd, void *arg) {
    int err = 0;
    struct tty *tty = in_tty;
    if (in_tty->driver == &pty_master) {
        tty = in_tty->pty.other;
        lock(&tty->lock, 0);
    }

    switch (cmd) {
        case TCGETS_:
            *(struct termios_ *) arg = tty->termios;
            break;
        case TCSETSF_:
            tty->bufsize = 0;
            notify(&tty->consumed);
                FALLTHROUGH;
        case TCSETSW_:
            // we have no output buffer currently
        case TCSETS_:
            tty->termios = *(struct termios_ *) arg;
            // Turning IXON off releases output that a ^S was holding: with no
            // flow control in effect there is nothing left to honour the stop,
            // and a ^Q can no longer arrive to lift it. A tcflow(TCOOFF) stop
            // is not IXON's to release, so it stays. Measured both ways.
            if (!(tty->termios.iflags & IXON_) && tty->stopped && !tty->tco_stopped)
                tty_start_output_locked(tty);
            break;

        // termios2 variants: same fields as termios_ above, plus explicit
        // ispeed/ospeed. iSH's virtual ttys have no real baud rate, so
        // TCGETS2 just reports the nominal speed the CBAUD bits of cflags are
        // initialized to and the SETS2 variants ignore the incoming speed
        // fields entirely -- musl and glibc both read cfget*speed() out of
        // cflags anyway, so that is the field that has to stay sane.
        case TCGETS2_: {
            struct termios2_ *termios2 = arg;
            termios2->iflags = tty->termios.iflags;
            termios2->oflags = tty->termios.oflags;
            termios2->cflags = tty->termios.cflags;
            termios2->lflags = tty->termios.lflags;
            termios2->line = tty->termios.line;
            memcpy(termios2->cc, tty->termios.cc, sizeof(termios2->cc));
            termios2->ispeed = 38400;
            termios2->ospeed = 38400;
            break;
        }
        case TCSETSF2_:
            tty->bufsize = 0;
            notify(&tty->consumed);
                FALLTHROUGH;
        case TCSETSW2_:
            // we have no output buffer currently
        case TCSETS2_: {
            struct termios2_ *termios2 = arg;
            tty->termios.iflags = termios2->iflags;
            tty->termios.oflags = termios2->oflags;
            tty->termios.cflags = termios2->cflags;
            tty->termios.lflags = termios2->lflags;
            tty->termios.line = termios2->line;
            memcpy(tty->termios.cc, termios2->cc, sizeof(tty->termios.cc));
            break;
        }

        case TIOCGWINSZ_:
            *(struct winsize_ *) arg = tty->winsize;
            break;
        case TIOCSWINSZ_:
            tty_set_winsize(tty, *(struct winsize_ *) arg);
            break;

        default:
            err = _ENOTTY;
            break;
    }

    if (in_tty->driver == &pty_master)
        unlock(&tty->lock);
    return err;
}

// Which ioctls count as changing the terminal, and so may not be issued from
// a background process. Linux calls tty_check_change() from inside each of
// these handlers; the set is the same gathered in one place. Note TIOCSWINSZ
// is deliberately absent -- Linux does not check it -- and that unlike a
// write, none of these consult TOSTOP.
static bool tty_ioctl_modifies_terminal(int cmd) {
    switch (cmd) {
        case TCSETS_: case TCSETSW_: case TCSETSF_:
        case TCFLSH_: case TCSBRK_: case TCXONC_:
        case TIOCSPGRP_:
            return true;
    }
    return false;
}

static int tty_ioctl(struct fd *fd, int cmd, void *arg) {
    int err = 0;
    struct tty *tty = fd->tty;
    // Filled in by TIOCNOTTY when it hangs a session up; sent below, after
    // tty->lock is released.
    struct tty_hangup_targets hup = { .fg_group = 0, .session = 0 };
    lock(&tty->lock, 0);
    if (tty_fd_hung_up(fd)) {
        unlock(&tty->lock);
        if (cmd == TIOCSPGRP_)
            return _ENOTTY;
        return _EIO;
    }

    if (tty_ioctl_modifies_terminal(cmd)) {
        err = tty_check_change_locked(tty, SIGTTOU_);
        if (err < 0) {
            unlock(&tty->lock);
            // Only TIOCSPGRP renames the refusal; every other caller sees the
            // EIO that tty_check_change produced.
            if (err == _EIO && cmd == TIOCSPGRP_)
                return _ENOTTY;
            return err;
        }
    }

    switch (cmd) {
        case TCFLSH_:
            // only input flushing is currently useful
            switch ((uintptr_t) arg) {
                case TCIFLUSH_:
                case TCIOFLUSH_:
                    tty->bufsize = 0;
                    notify(&tty->consumed);
                    break;
                case TCOFLUSH_:
                    break;
                default:
                    err = _EINVAL;
                    break;
            };
            break;

        case TCSBRK_:
            // tcdrain(fd) is TCSBRK with arg 1; tcsendbreak() is arg 0, which
            // asks for a break condition on the line. There is no line: a pty
            // has no serial hardware to hold at zero, and a break has no
            // meaning for one. Both were ENOTTY, which is what a real tty
            // never returns -- so a program calling tcdrain() before reading
            // its own output saw a failure where every Linux succeeds.
            //
            // Draining is honest here rather than a stub: tty_write pushes
            // straight through to the other side, so by the time this runs
            // there is nothing buffered on our side left to wait for.
            break;

        case TCXONC_:
            // tcflow(): suspend or resume transmission. TCOON restarts only
            // output that TCOOFF stopped -- it deliberately does NOT clear a
            // ^S, which is why tco_stopped is tracked separately from stopped.
            // Measured: after a ^S, tcflow(TCOON) leaves output stopped, while
            // after a TCOOFF it releases it.
            //
            // TCIOFF/TCION are the input direction: they transmit a STOP or
            // START character to the other end. AOK has no modem to send one
            // to, so they are accepted and do nothing, as they do for any
            // terminal that is not a serial line.
            switch ((uintptr_t) arg) {
                case TCOOFF_:
                    if (!tty->tco_stopped) {
                        tty->tco_stopped = true;
                        tty_stop_output_locked(tty);
                    }
                    break;
                case TCOON_:
                    if (tty->tco_stopped) {
                        tty->tco_stopped = false;
                        tty_start_output_locked(tty);
                    }
                    break;
                case TCIOFF_: case TCION_:
                    break;
                default:
                    err = _EINVAL;
                    break;
            }
            break;

        case TIOCEXCL_:
            tty->excl = true;
            break;
        case TIOCNXCL_:
            tty->excl = false;
            break;
        case TIOCGEXCL_:
            *(dword_t *) arg = tty->excl;
            break;

        case TIOCNOTTY_:
            err = tiocnotty(tty, &hup);
            break;

        case TIOCSCTTY_:
            err = tiocsctty(tty, (uintptr_t) arg);
            break;

        case TIOCCONS_:
            // Redirect-console-output ioctl (bootlogd). iSH has no kernel
            // console stream to redirect, so accept it as a no-op instead of
            // failing with ENOTTY.
            break;

        case TIOCOUTQ_:
            // Bytes still queued for output. tty_write hands data straight to
            // the other side rather than holding it, so the honest answer is
            // zero -- which is also what a caller polling it for drain
            // progress needs to see. It was ENOTTY.
            *(dword_t *) arg = 0;
            break;

        case TIOCGPGRP_:
            err = tiocgpgrp(tty, (pid_t_ *) arg);
            break;

        case TIOCGSID_:
            err = tiocgsid(tty, (pid_t_ *) arg);
            break;

        case TIOCSPGRP_:
            err = tiocspgrp(tty, *(pid_t_ *) arg);
            break;

        case FIONREAD_:
            *(dword_t *) arg = tty->bufsize;
            break;

        default:
            err = tty_mode_ioctl(tty, cmd, arg);
            if (err == _ENOTTY && tty->driver->ops->ioctl)
                err = tty->driver->ops->ioctl(tty, cmd, arg);
    }

    unlock(&tty->lock);
    if (hup.fg_group != 0 || hup.session != 0)
        tty_hangup_notify(hup);
    return err;
}

void tty_set_winsize(struct tty *tty, struct winsize_ winsize) {
    if (winsize.row == 0 || winsize.col == 0)
        return;
    if (tty->winsize.row == winsize.row &&
            tty->winsize.col == winsize.col &&
            tty->winsize.xpixel == winsize.xpixel &&
            tty->winsize.ypixel == winsize.ypixel)
        return;
    tty->winsize = winsize;
    if (tty->fg_group == 0)
        return;
    if (trylock(&pids_lock) != 0)
        return;
    struct pid *pid = pid_get(tty->fg_group);
    if (pid != NULL) {
        struct tgroup *tgroup;
        list_for_each_entry(&pid->pgroup, tgroup, pgroup) {
            send_signal(tgroup->leader, SIGWINCH_, SIGINFO_NIL);
        }
    }
    unlock(&pids_lock);
}

struct tty_hangup_targets tty_hangup(struct tty *tty) {
    // Captured before anything else: the caller may clear these itself (the
    // session leader's own exit does), and the signal has to reflect who was
    // attached when the terminal went away.
    struct tty_hangup_targets targets = {
        .fg_group = tty->fg_group,
        .session = tty->session,
    };
    tty->hung_up = true;
    // Everything open right now is hung up; anything opened after this is not.
    tty->hangup_gen++;
    tty_poll_wakeup(tty, POLL_READ | POLL_WRITE | POLL_ERR | POLL_HUP);
    // Wake blocking readers/writers, not just pollers: a thread asleep in
    // tty_read/tty_write's wait_for() must be notified or it will never observe
    // hung_up. tty->lock is held by all callers, so notifying this side's conds
    // is race-free; the peer is notified best-effort (the woken thread re-checks
    // its conditions, so a missed edge here is harmless).
    notify(&tty->produced);
    notify(&tty->consumed);
    // ...including a writer parked on flow control, which nothing else will
    // ever wake now: the ^Q that would have released it can no longer arrive.
    notify(&tty->flow_resumed);
    if (tty->driver == &pty_slave && tty->pty.other != NULL) {
        tty_poll_wakeup_unlocked(tty->pty.other, POLL_READ | POLL_HUP);
        notify(&tty->pty.other->produced);
        notify(&tty->pty.other->consumed);
    }
    return targets;
}

// A terminal going away is how a shell learns its session is over -- an ssh
// disconnect, a closed terminal window, the last master of a pty closing.
// Linux signals the foreground group and the session leader with SIGHUP and
// then SIGCONT (the SIGCONT so a stopped job runs far enough to notice the
// SIGHUP). AOK woke every reader and poller but signalled nobody, so a shell
// sat in its read loop on a terminal that no longer existed.
void tty_hangup_notify(struct tty_hangup_targets targets) {
    if (targets.fg_group != 0) {
        send_group_signal(targets.fg_group, SIGHUP_, SIGINFO_NIL);
        send_group_signal(targets.fg_group, SIGCONT_, SIGINFO_NIL);
    }
    // The session leader too, unless the foreground group already covered it.
    if (targets.session != 0 && targets.session != targets.fg_group) {
        send_group_signal(targets.session, SIGHUP_, SIGINFO_NIL);
        send_group_signal(targets.session, SIGCONT_, SIGINFO_NIL);
    }
}

bool tty_stat_rdev(dev_t_ rdev, struct statbuf *stat) {
    int major = dev_major(rdev);
    int minor = dev_minor(rdev);
    bool tty_alias = false;
    if (major == TTY_ALTERNATE_MAJOR && minor == DEV_CONSOLE_MINOR) {
        major = console_major;
        minor = console_minor;
        tty_alias = true;
    }

    if (major == TTY_ALTERNATE_MAJOR && minor == DEV_TTY_MINOR)
        tty_alias = true;

    if (!tty_alias &&
            major != TTY_CONSOLE_MAJOR && major != TTY_PSEUDO_MASTER_MAJOR &&
            major != TTY_PSEUDO_SLAVE_MAJOR)
        return false;

    dword_t stamp = (dword_t) boot_time;
    struct tty_driver *driver = tty_drivers[major];
    if (driver == NULL || minor < 0 || (unsigned) minor >= driver->limit) {
        stat->atime = stamp;
        stat->mtime = stamp;
        stat->ctime = stamp;
        return true;
    }

    lock(&ttys_lock, 0);
    struct tty *tty = driver->ttys[minor];
    if (tty != NULL && tty != (void *) 1) {
        lock(&tty->lock, 0);
        stat->atime = tty->atime;
        stat->mtime = tty->mtime;
        stat->ctime = tty->ctime;
        unlock(&tty->lock);
    } else {
        stat->atime = stamp;
        stat->mtime = stamp;
        stat->ctime = stamp;
    }
    unlock(&ttys_lock);
    return true;
}

struct dev_ops tty_dev = {
    .open = tty_device_open,
    .fd.close = tty_close,
    .fd.read = tty_read,
    .fd.write = tty_write,
    .fd.poll = tty_poll,
    .fd.ioctl_size = tty_ioctl_size,
    .fd.ioctl = tty_ioctl,
};
