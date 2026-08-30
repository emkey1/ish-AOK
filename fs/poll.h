#ifndef FS_POLL_H
#define FS_POLL_H
#include "kernel/fs.h"

struct real_poll {
    int fd;
};

struct poll {
    struct list poll_fds;
    struct real_poll real;
    int notify_pipe[2];
    int waiters; // if nonzero, notify_pipe exists
    bool notify_pending;

    // This is used to solve the race/UaF described here: https://lwn.net/Articles/520012/
    // thread 1: calls poll_wait, real_poll_wait returns an event with a pointer to a poll_fd
    // thread 2: calls poll_del_fd which frees the same poll_fd
    //
    // This can't be solved by adding locks because thread 1 could get
    // suspended after real_poll_wait returns but before it has a chance to
    // lock anything.
    //
    // An attempt was made to solve this with a Linux kernel patch, which
    // almost went in 3.7 but was backed out after discussion at
    // https://lkml.org/lkml/2012/10/16/302, and anyway wouldn't have solved
    // the problem on Darwin. My solution is to just not free poll_fds, and
    // instead move them to a freelist where they can be reused.
    struct list pollfd_freelist;

    // When this poll belongs to an epoll FD (kernel/epoll.c), the owning
    // struct fd -- so a wakeup on a member can cascade to whoever is
    // polling the epoll fd itself (epoll-inside-epoll, e.g. systemd's
    // sd-event epoll watching libmount's mountinfo-monitor epoll). NULL
    // for the transient polls poll(2)/select(2) build. Read/written under
    // this poll's lock; cleared by epoll_close before poll_destroy.
    struct fd *owner_fd;

    lock_t lock;
};

struct poll_fd {
    // locked by containing struct poll
    struct fd *fd;
    // Guest fd number this registration was made under, or -1 when the caller
    // registers per open file description (poll(2)/select(2), which merge
    // duplicate fds up front). Linux keys epoll membership by the (fd number,
    // open file description) pair, so dup'd fds may each carry their own
    // registration on one struct fd; keying by struct fd alone made the
    // second EPOLL_CTL_ADD return EEXIST (Bun registers stdout and stderr,
    // usually dups of one tty, as separate epoll members).
    fd_t guest_fd;
    struct list fds;
    int types;
    union poll_fd_info {
        void *ptr;
        int fd;
        uint64_t num;
    } info;
    // Used to implement edge-triggered notifications. When an event is
    // returned its bits are set here, and those bits are ignored on the next
    // call to poll_wait. The bits are cleared by poll_wakeup.
    int triggered_types;

    // locked by containing struct fd
    struct poll *poll;
    struct list polls;
};

// these are defined in system headers somewhere
#undef POLL_IN
#undef POLL_OUT
#undef POLL_MSG
#undef POLL_ERR
#undef POLL_PRI
#undef POLL_HUP

#define POLL_READ 1
#define POLL_PRI 2
#define POLL_WRITE 4
#define POLL_ERR 8
#define POLL_HUP 16
#define POLL_NVAL 32
// Guest EPOLLRDHUP / POLLRDHUP: the PEER shut down its writing end, which for
// a socket is not the same thing as a hangup and Linux reports separately.
// There was no bit for it at all, so nothing could ever produce one -- and the
// half-close was reported as POLL_HUP instead, on a socket that was still
// perfectly writable. Only delivered when the registration asked for it, which
// is why it is deliberately NOT in POLL_ALWAYS_LISTENING.
#define POLL_RDHUP 0x2000
#define POLL_ONESHOT (1 << 30)
#define POLL_EDGETRIGGERED (1ul << 31)
// Matches guest EPOLLEXCLUSIVE (1 << 28); ADD/MOD store the raw guest event
// bits into poll_fd->types with no translation, so this bit rides along
// automatically once registered -- no new poll_fd field needed, just a way
// to query it back out (see poll_fd_is_exclusive).
#define POLL_EXCLUSIVE (1 << 28)
struct poll_event {
    struct fd *fd;
    int types;
};
struct poll *poll_create(void);
// guest_fd: the guest fd number identifying this registration (see
// poll_fd.guest_fd), or -1 to register per open file description.
bool poll_has_fd(struct poll *poll, struct fd *fd, fd_t guest_fd);
// True if `fd` is currently registered on `poll` with POLL_EXCLUSIVE set.
bool poll_fd_is_exclusive(struct poll *poll, struct fd *fd, fd_t guest_fd);
int poll_add_fd(struct poll *poll, struct fd *fd, fd_t guest_fd, int types, union poll_fd_info info);
int poll_mod_fd(struct poll *poll, struct fd *fd, fd_t guest_fd, int types, union poll_fd_info info);
int poll_del_fd(struct poll *poll, struct fd *fd, fd_t guest_fd);
// Indicates that the specified events have been triggered. Each call will
// generate a new edge-triggered notification.
// please do not call this while holding any locks you would acquire in your poll operation
void poll_wakeup(struct fd *fd, int events);
// Same, but never blocks: skips fds/polls it can't immediately lock instead
// of waiting. Use this instead of poll_wakeup() when the lock-ordering rule
// above can't be honored (see the comment on the definition in poll.c).
void poll_wakeup_trylock(struct fd *fd, int events);
// Waits for events on the fds in this poll, and calls the callback for each one found.
// Returns the number of times the callback returned 1, or negative for error.
typedef int (*poll_callback_t)(void *context, int types, union poll_fd_info info);
int poll_wait(struct poll *poll, poll_callback_t callback, void *context, struct timespec *timeout);
// does not lock the poll because lock ordering, you must ensure no other
// thread will add or remove fds from this poll
void poll_destroy(struct poll *poll);

// for fd_close
void poll_cleanup_fd(struct fd *fd);

#endif
