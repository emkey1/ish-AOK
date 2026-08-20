#include "kernel/task.h"
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include "misc.h"
#include "util/list.h"
#include "util/timer.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "fs/fd.h"
#include "fs/poll.h"

extern const struct fd_ops socket_fdops;
#include "fs/real.h"
#include "fs/sock.h"
#include "fs/sockrestart.h"

#if defined(__linux__)
#include <sys/epoll.h>
#define HAVE_EPOLL 1
#elif defined(__APPLE__)
#include <sys/event.h>
#define HAVE_KQUEUE 1
#endif

static int real_poll_init(struct real_poll *real);
static void real_poll_close(struct real_poll *real);
struct real_poll_event {
#if HAVE_EPOLL
    struct epoll_event real;
#elif HAVE_KQUEUE
    struct kevent real;
#endif
};
static void *rpe_data(struct real_poll_event *rpe);
static int rpe_events(struct real_poll_event *rpe, struct poll_fd *pfd);
static int real_poll_wait(struct real_poll *real, struct real_poll_event *events, int max, struct timespec *timeout);
static int real_poll_update(struct real_poll *real, int fd, int types, void *data);
static inline bool poll_fd_has_host_wait(struct poll_fd *pollfd);
static int poll_sync_host_locked(struct poll *poll, struct fd *fd);
static void poll_fd_free(struct poll_fd *poll_fd);

static bool poll_fd_needs_periodic_host_rescan(struct poll_fd *poll_fd) {
#if defined(__APPLE__)
    if (poll_fd == NULL || poll_fd->fd == NULL)
        return false;
    if (poll_fd->fd->ops != &realfs_fdops)
        return false;
    if (!(poll_fd->types & POLL_WRITE))
        return false;
    return is_adhoc_fd(poll_fd->fd) && S_ISFIFO(poll_fd->fd->stat.mode);
#else
    (void) poll_fd;
    return false;
#endif
}

static bool poll_needs_periodic_host_rescan(struct poll *poll_) {
    struct poll_fd *poll_fd;
    list_for_each_entry(&poll_->poll_fds, poll_fd, fds) {
        if (poll_fd_needs_periodic_host_rescan(poll_fd))
            return true;
    }
    return false;
}

static bool poll_deadline_remaining(const struct timespec *deadline, struct timespec *remaining) {
    if (deadline == NULL || remaining == NULL)
        return false;
    struct timespec now = timespec_now(CLOCK_MONOTONIC);
    if ((deadline->tv_sec < now.tv_sec) ||
            (deadline->tv_sec == now.tv_sec && deadline->tv_nsec <= now.tv_nsec)) {
        *remaining = (struct timespec) {0};
        return false;
    }
    *remaining = timespec_subtract(*deadline, now);
    return timespec_positive(*remaining);
}

static bool poll_trace_comm(const char *comm) {
    if (comm == NULL)
        return false;
    return strcmp(comm, "apk") == 0 ||
        strcmp(comm, "apt") == 0 ||
        strcmp(comm, "apt-get") == 0 ||
        strncmp(comm, "http", 4) == 0 ||
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

static bool poll_wait_trace_enabled(void) {
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("ISH_TRACE_POLL_WAIT") != NULL ? 1 : 0;
    if (!enabled)
        return false;
    if (current == NULL)
        return false;
    return poll_trace_comm(current->comm);
}

static bool poll_epoll_trace_enabled(void) {
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("ISH_TRACE_EPOLL") != NULL ? 1 : 0;
    if (!enabled)
        return false;
    if (current == NULL)
        return false;
    return strcmp(current->comm, "compile") == 0;
}

static void poll_wait_trace_fd(struct poll_fd *poll_fd, int host_events, const char *phase) {
    if (!poll_wait_trace_enabled() || poll_fd == NULL || poll_fd->fd == NULL)
        return;

    char path[MAX_PATH];
    path[0] = '\0';
    generic_getpath(poll_fd->fd, path);
    fprintf(stderr, "ish-pollwait: %s pid=%d comm=%s real=%d types=%#x host=%#x path=%s\n",
           phase, current->pid, current->comm, poll_fd->fd->real_fd,
           poll_fd->types, host_events, path);
}

static void poll_wait_trace_raw_event(struct poll *poll_, struct real_poll_event *event, const char *phase) {
    if (!poll_wait_trace_enabled() || event == NULL)
        return;
#if HAVE_KQUEUE
    fprintf(stderr, "ish-pollwait: %s pid=%d comm=%s ident=%llu filter=%d flags=%#x fflags=%#x data=%lld udata=%p notify_fd=%d\n",
           phase, current->pid, current->comm,
           (unsigned long long) event->real.ident, event->real.filter,
           event->real.flags, event->real.fflags,
           (long long) event->real.data, event->real.udata,
           poll_ != NULL ? poll_->notify_pipe[0] : -1);
#elif HAVE_EPOLL
    fprintf(stderr, "ish-pollwait: %s pid=%d comm=%s events=%#x udata=%p notify_fd=%d\n",
           phase, current->pid, current->comm,
           event->real.events, event->real.data.ptr,
           poll_ != NULL ? poll_->notify_pipe[0] : -1);
#endif
}

static void poll_drop_unknown_event(struct poll *poll_, struct real_poll_event *event) {
#if HAVE_KQUEUE
    if (poll_ == NULL || event == NULL)
        return;
    int ident = (int) event->real.ident;
    if (ident < 0 || ident == poll_->notify_pipe[0])
        return;
    int err = real_poll_update(&poll_->real, ident, 0, NULL);
    if (poll_wait_trace_enabled()) {
        fprintf(stderr, "ish-pollwait: drop-raw pid=%d comm=%s ident=%d err=%d errno=%d\n",
               current->pid, current->comm, ident, err, err < 0 ? errno : 0);
    }
#else
    (void) poll_;
    (void) event;
#endif
}

static int poll_deliver_ready_locked(struct poll *poll_, struct poll_fd *poll_fd,
                                     int poll_types, poll_callback_t callback,
                                     void *context, const char *phase) {
    struct fd *fd = poll_fd->fd;

    if (poll_fd->types & POLL_EDGETRIGGERED)
        poll_types &= ~poll_fd->triggered_types;
    if (!poll_types)
        return 0;

    int handled = callback(context, poll_types, poll_fd->info);
    if (poll_wait_trace_enabled()) {
        fprintf(stderr, "ish-pollwait: %s pid=%d comm=%s real=%d events=%#x handled=%d\n",
               phase, current->pid, current->comm,
               fd != NULL ? fd->real_fd : -1, poll_types, handled);
    }
    int res = handled == 1 ? 1 : 0;

    // The real poll does not actually get the FDs set as oneshot.
    // But this loop is done while holding the lock, so only one
    // thread can get each oneshot event. This doesn't solve the
    // thundering herd problem at all, but at least the semantics
    // are right. I'll just leave that as a TODO.
    if (poll_fd->types & POLL_ONESHOT) {
        // Linux EPOLLONESHOT: after one delivery the fd is DISARMED (it stops
        // reporting) but stays REGISTERED in the epoll so EPOLL_CTL_MOD can
        // re-arm it. We used to remove and free the registration here, which
        // made a later re-arm MOD return ENOENT: ivykis's iv_fd_epoll uses an
        // EPOLLONESHOT eventfd as its cross-thread wakeup and re-arms it with
        // MOD, so this aborted syslog-ng (iv_fatal). The premature free also
        // fed a use-after-free in the multi-epoll close path. Disarm by
        // clearing the watched read/write interest; poll_mod_fd restores it on
        // re-arm. For host-backed fds, drop the host-side watch but keep the
        // poll_fd registered.
        poll_fd->types &= ~(POLL_READ | POLL_WRITE);
        if (poll_fd_has_host_wait(poll_fd))
            poll_sync_host_locked(poll_, fd);
        return res;
    }

    if (poll_fd->types & POLL_EDGETRIGGERED)
        poll_fd->triggered_types |= poll_types;
    return res;
}

static int poll_scan_ready_locked(struct poll *poll_, poll_callback_t callback, void *context) {
    int res = 0;
    struct poll_fd *poll_fd, *tmp;
    list_for_each_entry_safe(&poll_->poll_fds, poll_fd, tmp, fds) {
        struct fd *fd = poll_fd->fd;
        int raw_poll_types = 0;
        if (fd->ops->poll)
            raw_poll_types = fd->ops->poll(fd);
        int poll_types = raw_poll_types;
        poll_types &= poll_fd->types | POLL_HUP | POLL_ERR | POLL_NVAL;
        if (poll_fd->types & POLL_EDGETRIGGERED)
            poll_types &= ~poll_fd->triggered_types;
        if (poll_wait_trace_enabled()) {
            char path[MAX_PATH];
            path[0] = '\0';
            if (fd != NULL)
                generic_getpath(fd, path);
            fprintf(stderr, "ish-pollwait: scan pid=%d comm=%s real=%d raw=%#x masked=%#x types=%#x path=%s\n",
                   current->pid, current->comm,
                   fd != NULL ? fd->real_fd : -1,
                   raw_poll_types, poll_types,
                   poll_fd->types, path);
        }
        if (poll_epoll_trace_enabled() && fd != NULL && fd->real_fd < 0 &&
                (raw_poll_types != 0 || poll_types != 0)) {
            char path[MAX_PATH];
            path[0] = '\0';
            generic_getpath(fd, path);
            printk("epoll-trace: scan pid=%d comm=%s real=%d raw=%#x masked=%#x req=%#x path=%s ops=%p\n",
                   current->pid, current->comm, fd->real_fd,
                   raw_poll_types, poll_types, poll_fd->types, path, fd->ops);
        }
        if (!poll_types)
            continue;

        res += poll_deliver_ready_locked(poll_, poll_fd, poll_types,
                                         callback, context, "callback");
    }
    return res;
}

// lock order: fd, then poll

struct poll *poll_create(void) {
    struct poll *poll = malloc(sizeof(struct poll));
    if (poll == NULL)
        return ERR_PTR(_ENOMEM);
    int err = real_poll_init(&poll->real);
    if (err < 0)
        return ERR_PTR(errno_map());
    poll->waiters = 0;
    poll->notify_pipe[0] = -1;
    poll->notify_pipe[1] = -1;
    poll->notify_pending = false;
    list_init(&poll->poll_fds);
    list_init(&poll->pollfd_freelist);
    poll->owner_fd = NULL;
    lock_init(&poll->lock, "poll_create\0");
    return poll;
}

static inline bool poll_fd_has_host_wait(struct poll_fd *pollfd) {
    if (pollfd->fd == NULL || pollfd->fd->real_fd < 0)
        return false;
    return pollfd->fd->ops == &realfs_fdops || pollfd->fd->ops == &socket_fdops;
}

// does not do its own locking
static struct poll_fd *poll_find_fd(struct poll *poll, struct fd *fd, fd_t guest_fd) {
    struct poll_fd *poll_fd, *tmp;
    list_for_each_entry_safe(&poll->poll_fds, poll_fd, tmp, fds) {
        if (poll_fd->fd == fd && poll_fd->guest_fd == guest_fd)
            return poll_fd;
    }
    return NULL;
}

// Program the host backend with the union of every registration of `fd` in
// this poll. dup'd guest fds share one struct fd and thus one host fd, so
// letting each registration issue its own real_poll_update would clobber the
// others' udata and interest set. udata is the first registration in list
// order; poll_wait fans a host event out to every registration of that fd.
// Caller must hold poll->lock. Returns real_poll_update's result (-1 with
// errno set on failure).
static int poll_sync_host_locked(struct poll *poll, struct fd *fd) {
    int types = 0;
    bool all_edge_triggered = true;
    struct poll_fd *canonical = NULL, *poll_fd;
    list_for_each_entry(&poll->poll_fds, poll_fd, fds) {
        if (poll_fd->fd != fd || !poll_fd_has_host_wait(poll_fd))
            continue;
        if (canonical == NULL)
            canonical = poll_fd;
        types |= poll_fd->types & ~(POLL_EDGETRIGGERED | POLL_ONESHOT);
        if (!(poll_fd->types & POLL_EDGETRIGGERED))
            all_edge_triggered = false;
    }
    if (canonical == NULL)
        return real_poll_update(&poll->real, fd->real_fd, 0, NULL);
    // Only arm the host edge-triggered when every registration is: a
    // level-triggered sibling must keep seeing repeat notifications.
    if (all_edge_triggered)
        types |= POLL_EDGETRIGGERED;
    return real_poll_update(&poll->real, fd->real_fd, types, canonical);
}

// See comment on pollfd_freelist for context
static void poll_fd_free(struct poll_fd *poll_fd) {
    struct poll *poll = poll_fd->poll;
    memset(poll_fd, 0xba, sizeof(*poll_fd));
    poll_fd->poll = NULL; // used to mark it as free
    list_add(&poll->pollfd_freelist, &poll_fd->fds);
}

// Host poll backends can return stale udata pointers after a watched fd has
// been removed. Only trust pointers that still correspond to poll_fd storage
// owned by this poll instance.
static struct poll_fd *poll_find_ptr(struct poll *poll, struct poll_fd *candidate) {
    struct poll_fd *poll_fd;
    list_for_each_entry(&poll->poll_fds, poll_fd, fds) {
        if (poll_fd == candidate)
            return poll_fd;
    }
    list_for_each_entry(&poll->pollfd_freelist, poll_fd, fds) {
        if (poll_fd == candidate)
            return poll_fd;
    }
    return NULL;
}

bool poll_has_fd(struct poll *poll, struct fd *fd, fd_t guest_fd) {
    return poll_find_fd(poll, fd, guest_fd) != NULL;
}

bool poll_fd_is_exclusive(struct poll *poll, struct fd *fd, fd_t guest_fd) {
    struct poll_fd *poll_fd = poll_find_fd(poll, fd, guest_fd);
    return poll_fd != NULL && (poll_fd->types & POLL_EXCLUSIVE) != 0;
}

// Wake any thread currently blocked in poll_wait on this poll so it re-scans fd
// readiness. Caller must hold poll->lock. Emulated fds (eventfd, pipe, etc.)
// have no host-side wait queue and no periodic rescan, so a blocked epoll_wait
// only re-checks readiness when the notify pipe is poked -- which normally only
// happens on the fd's own read/write (poll_wakeup), not on epoll_ctl. When a
// caller arms an already-ready emulated fd via EPOLL_CTL_ADD/MOD (as ivykis
// does re-arming an eventfd it never writes), poke here so the waiter notices
// instead of sleeping until its (possibly very long) timeout. No-op when no one
// is waiting (notify_pipe closed), so single-threaded epoll users pay nothing.
static void poll_poke_notify_locked(struct poll *poll) {
    if (poll->notify_pipe[1] == -1)
        return;
    ssize_t wrote;
    do {
        wrote = write(poll->notify_pipe[1], "", 1);
    } while (wrote < 0 && errno == EINTR);
    if (wrote >= 0 || errno == EAGAIN)
        poll->notify_pending = true;
}

// Whether an emulated fd (no host wait queue) is *currently* satisfying the
// interest just armed via ADD/MOD. Mirrors Linux's ep_insert/ep_modify, which
// synchronously polls the file and only wakes waiters when the newly-armed
// events are already pending -- not on every epoll_ctl call. Without this
// check, poll_poke_notify_locked fired on every ADD/MOD regardless of actual
// readiness, so a library that re-arms an emulated fd on a tight/frequent
// cadence for unrelated bookkeeping (e.g. ivykis's cross-thread eventfd
// oneshot re-arm) turned every one of those calls into a wake+rescan+resleep
// of any blocked epoll_wait, burning host CPU/syscalls with nothing ready.
static bool poll_fd_currently_ready(struct poll_fd *poll_fd) {
    struct fd *fd = poll_fd->fd;
    if (fd == NULL || fd->ops->poll == NULL)
        return false;
    int raw = fd->ops->poll(fd);
    return (raw & (poll_fd->types | POLL_HUP | POLL_ERR | POLL_NVAL)) != 0;
}

int poll_add_fd(struct poll *poll, struct fd *fd, fd_t guest_fd, int types, union poll_fd_info info) {
    int err;
    lock(&fd->poll_lock, 0);
    lock(&poll->lock, 0);

    struct poll_fd *poll_fd;
    bool from_freelist = !list_empty(&poll->pollfd_freelist);
    if (from_freelist) {
        poll_fd = list_first_entry(&poll->pollfd_freelist, struct poll_fd, fds);
        list_remove(&poll_fd->fds);
    } else {
        poll_fd = malloc(sizeof(struct poll_fd));
        if (poll_fd == NULL) {
            err = _ENOMEM;
            goto out;
        }
    }
    poll_fd->fd = fd;
    poll_fd->guest_fd = guest_fd;
    poll_fd->poll = poll;
    poll_fd->types = types;
    poll_fd->info = info;
    poll_fd->triggered_types = 0;

    list_add(&fd->poll_fds, &poll_fd->polls);
    list_add(&poll->poll_fds, &poll_fd->fds);

    if (poll_fd_has_host_wait(poll_fd)) {
        err = poll_sync_host_locked(poll, fd);
        if (err < 0) {
            list_remove(&poll_fd->polls);
            list_remove(&poll_fd->fds);
            if (from_freelist)
                poll_fd_free(poll_fd);
            else
                free(poll_fd);
            err = errno_map();
            goto out;
        }
    }

    // An emulated fd added while ready can't surface through the host kevent;
    // wake a blocked poller so it re-scans. (See poll_poke_notify_locked.) Only
    // when it's actually ready now -- see poll_fd_currently_ready.
    if (!poll_fd_has_host_wait(poll_fd) && poll_fd_currently_ready(poll_fd))
        poll_poke_notify_locked(poll);

    err = 0;
out:
    unlock(&poll->lock);
    unlock(&fd->poll_lock);
    return err;
}

int poll_del_fd(struct poll *poll, struct fd *fd, fd_t guest_fd) {
    int err;
    lock(&fd->poll_lock, 0);
    lock(&poll->lock, 0);
    struct poll_fd *poll_fd = poll_find_fd(poll, fd, guest_fd);
    if (poll_fd == NULL) {
        err = _ENOENT;
        goto out;
    }

    bool had_host_wait = poll_fd_has_host_wait(poll_fd);
    list_remove(&poll_fd->polls);
    list_remove(&poll_fd->fds);
    poll_fd_free(poll_fd);
    // Reprogram the host with whatever sibling registrations remain (or drop
    // the watch if this was the last). The guest-side removal above is the
    // authoritative part; a host-side failure here just leaves a stale watch
    // whose events no longer resolve to a registration.
    if (had_host_wait)
        poll_sync_host_locked(poll, fd);

    err = 0;
out:
    unlock(&poll->lock);
    unlock(&fd->poll_lock);
    return err;
}

int poll_mod_fd(struct poll *poll, struct fd *fd, fd_t guest_fd, int types, union poll_fd_info info) {
    int err;
    lock(&fd->poll_lock, 0);
    lock(&poll->lock, 0);
    struct poll_fd *poll_fd = poll_find_fd(poll, fd, guest_fd);
    if (poll_fd == NULL) {
        err = _ENOENT;
        goto out;
    }

    int old_types = poll_fd->types;
    union poll_fd_info old_info = poll_fd->info;
    poll_fd->types = types;
    poll_fd->info = info;
    if (poll_fd_has_host_wait(poll_fd)) {
        err = poll_sync_host_locked(poll, fd);
        if (err < 0) {
            poll_fd->types = old_types;
            poll_fd->info = old_info;
            err = errno_map();
            goto out;
        }
    }

    poll_fd->triggered_types &= types;

    // Arming an already-ready emulated fd via MOD must wake a blocked poll_wait;
    // it won't get a host event or a poll_wakeup otherwise, so it would sleep
    // until timeout. (This is the lost wakeup that wedged syslog-ng/ivykis.)
    // Only poke when it's actually ready now -- see poll_fd_currently_ready --
    // otherwise every MOD (e.g. a oneshot re-arm with no new data) spuriously
    // wakes any blocked waiter for nothing to report.
    if (!poll_fd_has_host_wait(poll_fd) && poll_fd_currently_ready(poll_fd))
        poll_poke_notify_locked(poll);

    err = 0;
out:
    unlock(&poll->lock);
    unlock(&fd->poll_lock);
    return err;
}

void poll_cleanup_fd(struct fd *fd) {
    lock(&fd->poll_lock, 0);
    struct poll_fd *poll_fd, *tmp;
    list_for_each_entry_safe(&fd->poll_fds, poll_fd, tmp, polls) {
        struct poll *poll = poll_fd->poll;
        lock(&poll->lock, 0);
        bool had_host_wait = poll_fd_has_host_wait(poll_fd);
        list_remove(&poll_fd->polls);
        list_remove(&poll_fd->fds);
        poll_fd_free(poll_fd);
        // Recomputes from the siblings still registered; the last removal for
        // this fd drops the host watch.
        if (had_host_wait)
            poll_sync_host_locked(poll, fd);
        unlock(&poll->lock);
    }
    unlock(&fd->poll_lock);
}

void poll_wakeup(struct fd *fd, int events) {
    struct poll_fd *poll_fd;
    lock(&fd->poll_lock, 0);
    list_for_each_entry(&fd->poll_fds, poll_fd, polls) {
        struct poll *poll = poll_fd->poll;
        lock(&poll->lock,0);
        if (poll_fd->types & POLL_EDGETRIGGERED)
            poll_fd->triggered_types &= ~events;
        if (poll->notify_pipe[1] != -1) {
            ssize_t wrote;
            do {
                wrote = write(poll->notify_pipe[1], "", 1);
            } while (wrote < 0 && errno == EINTR);
            if (wrote >= 0 || errno == EAGAIN) {
                poll->notify_pending = true;
            } else if (wrote < 0) {
                FIXME("poll wake notify write failed: %s", strerror(errno));
            }
        }
        // Epoll-inside-epoll: this poll belongs to an epoll FD that may
        // itself be registered in an outer epoll (systemd's sd-event epoll
        // holds libmount's mountinfo-monitor epoll). Becoming ready must
        // propagate outward or the outer epoll_wait sleeps through the
        // event: with no waiter on the inner epoll, the notify pipe above
        // reaches nobody, and before epoll fds had a .poll callback the
        // outer scan couldn't see the readiness either -- that lost edge
        // protocol-failed systemd's tmp.mount on most boots. trylock
        // variant on purpose: we hold this poll's lock, and a blocking
        // wakeup could AB-BA against an outer scan calling epoll's .poll
        // (outer lock -> inner lock). On a lost race the outer scanner is
        // awake anyway and recomputes readiness through .poll.
        struct fd *owner = poll->owner_fd;
        if (owner != NULL)
            poll_wakeup_trylock(owner, POLL_READ);
        unlock(&poll->lock);
        // oneshot?
    }
    unlock(&fd->poll_lock);
}

// Non-blocking counterpart to poll_wakeup(), for a caller that cannot honor
// the "don't call while holding a lock your poll operation acquires"
// contract above. signalfd_wakeup_task (kernel/signal.c) is called with
// task->sighand->lock held, and signalfd's fd_ops.poll (signalfd_poll) takes
// current->sighand->lock -- the same lock, shared by every thread in a
// tgroup. poll_scan_ready_locked() calls fd->ops->poll() while holding
// poll->lock (see poll_wait), so that path's order is poll->lock ->
// sighand->lock. A blocking poll_wakeup() from signalfd_wakeup_task takes
// fd->poll_lock -> poll->lock while sighand->lock is already held, i.e.
// sighand->lock -> poll->lock: the reverse order. Two threads hitting these
// paths at once AB-BA deadlock (observed on-device: an exiting child
// delivering SIGCHLD while the parent's event-loop thread was mid-epoll_wait
// scanning the same signalfd). Best-effort here is fine: deliver_signal
// already wakes the signal's target task via SIGUSR1 + poll_notify_fd
// independent of this notify-pipe poke, and a signalfd's readiness is
// re-checked on the next scan/timeout regardless.
void poll_wakeup_trylock(struct fd *fd, int events) {
    struct poll_fd *poll_fd;
    if (trylock(&fd->poll_lock) != 0)
        return;
    list_for_each_entry(&fd->poll_fds, poll_fd, polls) {
        struct poll *poll = poll_fd->poll;
        if (trylock(&poll->lock) != 0)
            continue;
        if (poll_fd->types & POLL_EDGETRIGGERED)
            poll_fd->triggered_types &= ~events;
        if (poll->notify_pipe[1] != -1) {
            ssize_t wrote;
            do {
                wrote = write(poll->notify_pipe[1], "", 1);
            } while (wrote < 0 && errno == EINTR);
            if (wrote >= 0 || errno == EAGAIN) {
                poll->notify_pending = true;
            } else if (wrote < 0) {
                FIXME("poll wake notify write failed: %s", strerror(errno));
            }
        }
        // Cascade to an owning epoll fd, same as poll_wakeup (recursion is
        // bounded: epoll nesting is acyclic by the epoll_ctl loop check).
        struct fd *owner = poll->owner_fd;
        if (owner != NULL)
            poll_wakeup_trylock(owner, POLL_READ);
        unlock(&poll->lock);
    }
    unlock(&fd->poll_lock);
}

int poll_wait(struct poll *poll_, poll_callback_t callback, void *context, struct timespec *timeout) {
    lock(&poll_->lock, 0);

    // acquire the pipe
    if (poll_->waiters++ == 0) {
        assert(poll_->notify_pipe[0] == -1 && poll_->notify_pipe[1] == -1);
        if (pipe(poll_->notify_pipe) < 0) {
            poll_->waiters--;
            unlock(&poll_->lock);
            return errno_map();
        }
        fcntl(poll_->notify_pipe[0], F_SETFL, O_NONBLOCK);
        fcntl(poll_->notify_pipe[1], F_SETFL, O_NONBLOCK);
        real_poll_update(&poll_->real, poll_->notify_pipe[0], POLL_READ, NULL);
    }

    // Publish this poll's notify-pipe write end so a concurrent guest-signal
    // delivery can wake us through the (non-lossy) pipe in addition to SIGUSR1.
    // SIGUSR1 doubles as the TLB/quiesce poke and as the guest-signal wake; under
    // heavy poke traffic the wake SIGUSR1 can be coalesced or consumed in a
    // window where it does nothing, letting real_poll_wait run to its timeout
    // and return 0 instead of EINTR. The pipe write is not lost, so the host
    // wait is torn out and the loop re-checks pending. Guarded by sighand->lock
    // (the same lock deliver_signal_unlocked_locked reads the fd under); we
    // already hold poll_->lock, matching the poll->then->sighand order used by
    // the pending checks below.
    lock(&current->sighand->lock, 0);
    current->poll_notify_fd = poll_->notify_pipe[1];
    unlock(&current->sighand->lock);

    struct timespec deadline_storage = {0};
    struct timespec *deadline = NULL;
    if (timeout != NULL) {
        deadline_storage = timespec_add(timespec_now(CLOCK_MONOTONIC), *timeout);
        deadline = &deadline_storage;
    }

    int res = 0;
    while (true) {
        // check if any fds are ready
        struct poll_fd *poll_fd;
        res += poll_scan_ready_locked(poll_, callback, context);
        if (res > 0)
            break;

        // Snapshot while poll_->lock is still held: poll_->poll_fds is
        // mutated (under this same lock) by poll_add_fd/poll_del_fd/
        // poll_mod_fd/poll_cleanup_fd from other threads, including fd
        // teardown that frees the struct fd a poll_fd points to. The
        // lock is dropped below (to let real_poll_wait block without
        // pinning it), and the check used to run unlocked after that --
        // an unsynchronized read of a list other threads splice/free
        // entries out of concurrently. Under heavy concurrent fd churn
        // (many OS threads opening/closing sockets and pipes -- routine
        // for a Go program's runtime) that races poll_cleanup_fd's
        // list_remove and crashes dereferencing a freed struct fd's
        // ->ops (seen on device as a SIGSEGV in a Go build's own
        // netpoller thread, address a freed-memory poison pattern).
        bool needs_periodic_host_rescan = poll_needs_periodic_host_rescan(poll_);

        lock(&current->sighand->lock,0);
        bool signal_pending = !!((current->pending | current->sighand->pending) & ~task_wake_blocked(current));
        unlock(&current->sighand->lock);
        if (signal_pending) {
            res = _EINTR;
            break;
        }

        // wait for a ready notification
        list_for_each_entry(&poll_->poll_fds, poll_fd, fds) {
            sockrestart_begin_listen_wait(poll_fd->fd);
        }
        int err;
        struct real_poll_event e[4];
        do {
            unlock(&poll_->lock);
            sigset_t sigusr1, oldmask;
            sigemptyset(&sigusr1);
            sigaddset(&sigusr1, SIGUSR1);
            pthread_sigmask(SIG_BLOCK, &sigusr1, &oldmask);
            if (sigunwind_start()) {
                pthread_sigmask(SIG_SETMASK, &oldmask, NULL);
                errno = EINTR;
                err = -1;
            } else {
                lock(&current->sighand->lock, 0);
                bool signal_pending = !!((current->pending | current->sighand->pending) & ~task_wake_blocked(current));
                unlock(&current->sighand->lock);
                if (signal_pending) {
                    sigunwind_end();
                    pthread_sigmask(SIG_SETMASK, &oldmask, NULL);
                    errno = EINTR;
                    err = -1;
                } else {
                    struct timespec remaining_timeout = {0};
                    struct timespec *wait_timeout = NULL;
                    struct timespec periodic_rescan_timeout = {
                        .tv_sec = 0,
                        .tv_nsec = 100 * 1000 * 1000L,
                    };
                    if (deadline != NULL) {
                        if (!poll_deadline_remaining(deadline, &remaining_timeout)) {
                            pthread_sigmask(SIG_SETMASK, &oldmask, NULL);
                            errno = 0;
                            err = 0;
                            sigunwind_end();
                            goto poll_wait_done;
                        }
                        wait_timeout = &remaining_timeout;
                    }
                    if (needs_periodic_host_rescan) {
                        if (wait_timeout == NULL ||
                                wait_timeout->tv_sec > periodic_rescan_timeout.tv_sec ||
                                (wait_timeout->tv_sec == periodic_rescan_timeout.tv_sec &&
                                 wait_timeout->tv_nsec > periodic_rescan_timeout.tv_nsec)) {
                            wait_timeout = &periodic_rescan_timeout;
                        }
                    }
                    pthread_sigmask(SIG_SETMASK, &oldmask, NULL);
                    // pselect/ppoll set only the guest blocked mask (not the
                    // host pthread mask), so SIGUSR1 may still be blocked at
                    // the host level even when the guest wants it unblocked.
                    // Force-unblock SIGUSR1 so the kevent wait is interruptible.
                    pthread_sigmask(SIG_UNBLOCK, &sigusr1, NULL);
                    if (poll_wait_trace_enabled()) {
                        fprintf(stderr, "ish-pollwait: sleep pid=%d comm=%s timeout=%lds.%09ld waiters=%d\n",
                               current->pid, current->comm,
                               wait_timeout != NULL ? wait_timeout->tv_sec : -1L,
                               wait_timeout != NULL ? wait_timeout->tv_nsec : -1L,
                               poll_->waiters);
                    }
                    err = real_poll_wait(&poll_->real, e, sizeof(e)/sizeof(e[0]), wait_timeout);
                    sigunwind_end();
                }
            }
poll_wait_done:
            lock(&poll_->lock, 0);
        } while (sockrestart_should_restart_listen_wait(1) && errno == EINTR);
        if (poll_wait_trace_enabled()) {
            fprintf(stderr, "ish-pollwait: wake pid=%d comm=%s err=%d errno=%d notify_pending=%d\n",
                   current->pid, current->comm, err, err < 0 ? errno : 0, poll_->notify_pending);
            if (err > 0) {
                for (int i = 0; i < err; i++) {
                    struct poll_fd *candidate = rpe_data(&e[i]);
                    struct poll_fd *triggered_poll_fd = poll_find_ptr(poll_, candidate);
                    if (triggered_poll_fd != NULL)
                        poll_wait_trace_fd(triggered_poll_fd, rpe_events(&e[i], candidate), "host-event");
                    else {
                        poll_wait_trace_raw_event(poll_, &e[i], "raw-event");
                        poll_drop_unknown_event(poll_, &e[i]);
                    }
                }
            }
        }
        list_for_each_entry(&poll_->poll_fds, poll_fd, fds) {
            sockrestart_end_listen_wait(poll_fd->fd);
        }

        if (err < 0) {
            // A spurious host SIGUSR1 (e.g. from task_poke_shared_mem for TLB
            // invalidation) can land here without any guest signal pending.
            // Only treat this as EINTR if a real guest signal is waiting.
            lock(&current->sighand->lock, 0);
            bool signal_pending = !!((current->pending | current->sighand->pending) & ~task_wake_blocked(current));
            unlock(&current->sighand->lock);
            if (!signal_pending)
                continue;
            res = errno_map();
            break;
        }
        if (err == 0) {
            // Re-probe once before timing out. The host wait backend can miss
            // a transition even when a direct readiness probe already says the
            // fd is ready.
            res += poll_scan_ready_locked(poll_, callback, context);
            // If nothing is ready but a guest signal slipped in while we were
            // blocked, the wait was interrupted, not idle: return EINTR rather
            // than a 0 (timeout). The notify-pipe wake normally tears us out
            // promptly, but this also covers the case where the wake was lost
            // and only the timeout fired -- a pending unblocked signal must win
            // over a timeout, matching Linux poll()/select() semantics.
            if (res == 0) {
                lock(&current->sighand->lock, 0);
                bool signal_pending = !!((current->pending | current->sighand->pending) & ~task_wake_blocked(current));
                unlock(&current->sighand->lock);
                if (signal_pending)
                    res = _EINTR;
            }
            break;
        }

        // Deliver host readiness notifications directly. fd->ops->poll() is
        // still the preferred readiness source, but Darwin can report EOF/HUP
        // through kqueue when a follow-up zero-time probe returns no bits. If
        // we only rescan, that host event can wake us forever without ever
        // reaching the guest.
        //
        // A single fd is registered across up to three independent kqueue
        // filters (EVFILT_READ/WRITE/EXCEPT -- see real_poll_update), so one
        // kevent() batch can return more than one raw entry for the same fd,
        // e.g. a read-ready entry AND a separate except/error entry. Linux
        // epoll_wait() guarantees at most one epoll_event per fd per call,
        // with every ready condition OR'd into a single events mask, and
        // epoll consumers rely on that (they treat each returned array entry
        // as an independent, self-contained readiness report). Delivering
        // raw per-filter entries 1:1 split a healthy, actively-readable
        // socket's events into a bare-EPOLLERR entry plus a separate EPOLLIN
        // entry; consumers that saw the bare-error entry first (or ran their
        // per-entry error handler unconditionally) tore down perfectly good
        // connections -- this is what made rtorrent/libtorrent's peer
        // connections die within a second of a normal read/write exchange.
        // Coalesce by underlying fd before delivering so each fd gets at most
        // one combined event mask per batch, matching real epoll semantics.
        // The host watch carries a single udata (see poll_sync_host_locked),
        // but dup'd guest fds may hold several registrations of that fd on
        // this poll, so delivery fans each host event out to every
        // registration, masked by that registration's own interest.
        struct fd *batch_fds[4] = {0};
        int batch_types[4] = {0};
        int batch_count = 0;
        for (int i = 0; i < err; i++) {
            struct poll_fd *candidate = rpe_data(&e[i]);
            struct poll_fd *triggered_poll_fd = poll_find_ptr(poll_, candidate);
            if (triggered_poll_fd == NULL || triggered_poll_fd->poll != poll_)
                continue;
            int host_events = rpe_events(&e[i], candidate);
            if (poll_epoll_trace_enabled()) {
                struct fd *fd = triggered_poll_fd->fd;
                char path[MAX_PATH];
                path[0] = '\0';
                if (fd != NULL)
                    generic_getpath(fd, path);
                printk("epoll-trace: host pid=%d comm=%s real=%d host=%#x req=%#x path=%s ops=%p\n",
                       current->pid, current->comm,
                       fd != NULL ? fd->real_fd : -1, host_events,
                       triggered_poll_fd->types, path,
                       fd != NULL ? (void *) fd->ops : NULL);
            }
            int slot = -1;
            for (int j = 0; j < batch_count; j++) {
                if (batch_fds[j] == triggered_poll_fd->fd) {
                    slot = j;
                    break;
                }
            }
            if (slot < 0) {
                slot = batch_count++;
                batch_fds[slot] = triggered_poll_fd->fd;
                batch_types[slot] = 0;
            }
            batch_types[slot] |= host_events;
        }
        for (int i = 0; i < batch_count; i++) {
            struct poll_fd *fan_poll_fd, *fan_tmp;
            list_for_each_entry_safe(&poll_->poll_fds, fan_poll_fd, fan_tmp, fds) {
                if (fan_poll_fd->fd != batch_fds[i])
                    continue;
                int host_events = batch_types[i];
                if (fan_poll_fd->types & POLL_EDGETRIGGERED)
                    fan_poll_fd->triggered_types &= ~host_events;
                int poll_types = host_events & (fan_poll_fd->types | POLL_HUP | POLL_ERR | POLL_NVAL);
                if (!poll_types)
                    continue;
                res += poll_deliver_ready_locked(poll_, fan_poll_fd, poll_types,
                                                 callback, context, "host-callback");
            }
        }

        while (poll_->notify_pipe[0] != -1) {
            char byte;
            ssize_t drained = read(poll_->notify_pipe[0], &byte, 1);
            if (drained > 0)
                continue;
            if (drained < 0 && errno != EAGAIN) {
                res = errno_map();
                break;
            }
            poll_->notify_pending = false;
            break;
        }
        if (res < 0)
            break;
        if (res > 0)
            break;
    }

    // Stop advertising the notify pipe before dropping our waiter reference, so
    // it is cleared while the pipe is still open (the last waiter only closes it
    // after every waiter has cleared its fd). A signal sender reads this under
    // sighand->lock, so it either sees a valid still-open fd or -1, never a
    // closed/recycled one.
    lock(&current->sighand->lock, 0);
    current->poll_notify_fd = -1;
    unlock(&current->sighand->lock);

    // release the pipe
    if (--poll_->waiters == 0) {
        close(poll_->notify_pipe[0]);
        close(poll_->notify_pipe[1]);
        poll_->notify_pipe[0] = -1;
        poll_->notify_pipe[1] = -1;
        poll_->notify_pending = false;
    }

    unlock(&poll_->lock);
    return res;
}

void poll_destroy(struct poll *poll) {
    struct poll_fd *poll_fd;
    struct poll_fd *tmp;

    list_for_each_entry_safe(&poll->poll_fds, poll_fd, tmp, fds) {
        lock(&poll_fd->fd->poll_lock, 0);
        list_remove(&poll_fd->polls);
        list_remove(&poll_fd->fds);
        unlock(&poll_fd->fd->poll_lock);
        free(poll_fd);
    }

    list_for_each_entry_safe(&poll->pollfd_freelist, poll_fd, tmp, fds) {
        list_remove(&poll_fd->fds);
        free(poll_fd);
    }

    real_poll_close(&poll->real);
    free(poll);
}

// Platform-specific real_poll implementations

#if HAVE_EPOLL

static int real_poll_init(struct real_poll *real) {
    real->fd = epoll_create1(0);
    if (real->fd < 0)
        return -1;
    return 0;
}

static int real_poll_wait(struct real_poll *real, struct real_poll_event *events, int max, struct timespec *timeout) {
    int timeout_millis = -1;
    if (timeout != NULL)
        timeout_millis = timeout->tv_sec * 1000 + timeout->tv_nsec / 1000000;
    return epoll_wait(real->fd, (struct epoll_event *) events, max, timeout_millis);
}

static int real_poll_update(struct real_poll *real, int fd, int types, void *data) {
    types &= ~EPOLLONESHOT;
    if (types == 0)
        return epoll_ctl(real->fd, EPOLL_CTL_DEL, fd, NULL);
    struct epoll_event epevent = {.events = types, .data.ptr = data};
    int err = epoll_ctl(real->fd, EPOLL_CTL_MOD, fd, &epevent);
    if (err < 0 && errno == ENOENT)
        err = epoll_ctl(real->fd, EPOLL_CTL_ADD, fd, &epevent);
    return err;
}

static void *rpe_data(struct real_poll_event *rpe) {
    return rpe->real.data.ptr;
}
static int rpe_events(struct real_poll_event *rpe, struct poll_fd *UNUSED(pfd)) {
    return rpe->real.events;
}

#elif HAVE_KQUEUE

static int real_poll_init(struct real_poll *real) {
    real->fd = kqueue();
    if (real->fd < 0)
        return -1;
    return 0;
}

static int real_poll_check_receipts(struct kevent *events, int count) {
    for (int i = 0; i < count; i++) {
        if (!(events[i].flags & EV_ERROR))
            continue;
        if (events[i].data == 0)
            continue;
        // Deleting a filter that was never installed is harmless.
        if (events[i].data == ENOENT)
            continue;
        // EVFILT_EXCEPT is not supported for all Darwin fd types, including
        // regular files. Treat that as "filter unavailable" rather than
        // failing the whole poll registration.
        if (events[i].filter == EVFILT_EXCEPT &&
                (events[i].data == EINVAL || events[i].data == ENOTSUP || events[i].data == EPERM))
            continue;
        errno = (int) events[i].data;
        return -1;
    }
    return 0;
}

static int real_poll_update(struct real_poll *real, int fd, int types, void *data) {
    struct kevent e[3] = {
        {.filter = EVFILT_READ, .flags = types & (POLL_READ | POLL_HUP) ? EV_ADD : EV_DELETE},
        {.filter = EVFILT_WRITE, .flags = types & POLL_WRITE ? EV_ADD : EV_DELETE},
        {.filter = EVFILT_EXCEPT, .flags = types & POLL_ERR ? EV_ADD : EV_DELETE},
    };
    if (!(types & POLL_READ) && types & POLL_HUP) {
        // Set the low water mark really high so we'll only get woken up on a hangup
        e[0].fflags = NOTE_LOWAT;
        e[0].data = INT_MAX;
    }
    for (int i = 0; i < 3; i++) {
        e[i].ident = fd;
        e[i].udata = data;
        e[i].flags |= EV_RECEIPT;
        if (types & POLL_EDGETRIGGERED)
            e[i].flags |= EV_CLEAR;
    }

    int count = kevent(real->fd, e, 3, e, 3, NULL);
    if (count < 0)
        return -1;
    return real_poll_check_receipts(e, count);
}

static int real_poll_wait(struct real_poll *real, struct real_poll_event *events, int max, struct timespec *timeout) {
    return kevent(real->fd, NULL, 0, (struct kevent *) events, max, timeout);
}

static void *rpe_data(struct real_poll_event *rpe) {
    return rpe->real.udata;
}

static int rpe_events(struct real_poll_event *rpe, struct poll_fd *pfd) {
    if (rpe->real.flags & EV_ERROR) {
        int err = (int) rpe->real.data;
        if (err == 0)
            return POLL_ERR;
        if (err == EBADF || err == ENOENT)
            return POLL_NVAL;
        return POLL_ERR;
    }
    if (rpe->real.filter == EVFILT_READ) {
        int events = 0;
        if (rpe->real.data > 0)
            events |= POLL_READ;
        if (rpe->real.flags & EV_EOF)
            events |= POLL_HUP;
        return events;
    }
    if (rpe->real.filter == EVFILT_WRITE) return POLL_WRITE;
    if (rpe->real.filter == EVFILT_EXCEPT) {
        // Darwin's EVFILT_EXCEPT fires on ordinary, healthy TCP sockets under
        // normal traffic with no real error condition -- confirmed live
        // against a BitTorrent peer exchange, SO_ERROR read back as 0 on
        // every firing. Unlike Linux, where the kernel only ever sets
        // EPOLLERR on a genuine socket error, blindly mapping EVFILT_EXCEPT
        // to POLL_ERR handed the guest a false error on perfectly good,
        // actively-transmitting sockets. epoll consumers correctly treat
        // EPOLLERR as fatal (rtorrent/libtorrent among them), so this tore
        // down healthy peer connections within about a second of connecting.
        // Confirm there is an actual pending error before reporting one; if
        // getsockopt fails (e.g. a non-socket fd), don't invent an error.
        int so_error = 0;
        socklen_t so_error_len = sizeof(so_error);
        if (getsockopt((int) rpe->real.ident, SOL_SOCKET, SO_ERROR, &so_error, &so_error_len) < 0 || so_error == 0)
            return 0;
        // That getsockopt READ-AND-CLEARED the host's SO_ERROR, and this used
        // to throw the value away -- so the error existed only long enough for
        // the poll to report POLL_ERR, and the guest's recv() that followed
        // found nothing. On a connected UDP socket that took an ICMP
        // port-unreachable, the ECONNREFUSED simply vanished whenever a poll
        // happened to run first, which is why it arrived only about two thirds
        // of the time and always on the very first poll when it did.
        //
        // Stash it, exactly as socket_tcp_connect_write_ready() does for the
        // stream case: fd.h's contract for host_connect_error is that AOK's own
        // internal probes record what they consumed so the guest-facing call
        // can still see it.
        if (pfd != NULL && pfd->fd != NULL && pfd->fd->ops == &socket_fdops &&
                pfd->fd->socket.host_connect_error == 0)
            pfd->fd->socket.host_connect_error = so_error;
        return POLL_ERR;
    }
    return 0;
}

#endif

static void real_poll_close(struct real_poll *real) {
    close(real->fd);
}
