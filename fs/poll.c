#include "kernel/checkpoint.h"
#include "kernel/task.h"
#include "kernel/signal.h"
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdatomic.h>
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
// `precise`: the timeout is the guest's own deadline rather than one of
// poll_wait's caps, and is worth waking on time for.
static int real_poll_wait(struct real_poll *real, struct real_poll_event *events, int max,
        struct timespec *timeout, bool precise);
static int real_poll_update(struct real_poll *real, int fd, int types, void *data);
// Whatever the host has queued, without waiting; *more when there may be more.
static int real_poll_collect(struct real_poll *real, struct real_poll_event *events, int max,
        bool *more);
static inline bool poll_fd_has_host_wait(struct poll_fd *pollfd);
static int poll_sync_host_locked(struct poll *poll, struct fd *fd);
static void poll_fd_free(struct poll_fd *poll_fd);


static _Atomic bool poll_stuck_logged;
// Bumped by every return to the foreground; see poll_note_host_resume.
static _Atomic unsigned poll_host_resume_gen;
_Atomic long poll_wedged_repairs;
_Atomic long poll_capped_waits;

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

// The hardcoded list below is the package-manager/download set the tracer was
// originally written for. It is useless for the bug the tracer is most needed
// on -- a daemon whose poll loop burns 100% CPU -- because chronyd, rsyslogd
// and sshd-session are not in it, and adding a name meant editing this file
// and rebuilding the app.
//
// ISH_TRACE_POLL_WAIT_COMM overrides the list at runtime: a comma-separated
// set of comms, or "*" for every process. Prefix matching, so
// "sshd" catches "sshd-session" and "in:imuxsock" can be reached as "in:".
static bool poll_trace_comm_env(const char *comm) {
    static const char *list = NULL;
    static int looked_up = 0;
    if (!looked_up) {
        list = getenv("ISH_TRACE_POLL_WAIT_COMM");
        looked_up = 1;
    }
    if (list == NULL || *list == '\0')
        return false;
    if (strcmp(list, "*") == 0)
        return true;
    size_t comm_len = strlen(comm);
    const char *p = list;
    while (*p != '\0') {
        const char *end = strchr(p, ',');
        size_t n = end != NULL ? (size_t) (end - p) : strlen(p);
        if (n > 0 && n <= comm_len && strncmp(comm, p, n) == 0)
            return true;
        if (end == NULL)
            break;
        p = end + 1;
    }
    return false;
}

static bool poll_trace_comm(const char *comm) {
    if (comm == NULL)
        return false;
    if (getenv("ISH_TRACE_POLL_WAIT_COMM") != NULL)
        return poll_trace_comm_env(comm);
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

// An event on the file a registration watches: its own wakeup
// (poll_wakeup), or one the host reported for it. Linux's ep_poll_callback
// queues a registration for any event its mask names -- EPOLLERR and EPOLLHUP
// always count -- and ignores the rest, and the wait that then reports it
// polls the file and reports everything ready, not only what changed. So an
// event makes an edge-triggered registration forget ALL it has reported, not
// just the bits the event names: an EPOLLIN|EPOLLOUT registration that was
// told EPOLLOUT and then gets data is told EPOLLIN|EPOLLOUT, as on Linux, where
// forgetting only EPOLLIN told it EPOLLIN alone.
static void poll_fd_note_event_locked(struct poll_fd *poll_fd, int events) {
    int interest = poll_fd->types & ~(POLL_EDGETRIGGERED | POLL_ONESHOT | POLL_EXCLUSIVE);
    if ((poll_fd->types & POLL_EDGETRIGGERED) && (events & (interest | POLL_HUP | POLL_ERR)))
        poll_fd->triggered_types = 0;
}

// *refused (when asked): the caller had no room for a report that was due.
static int poll_deliver_ready_locked(struct poll *poll_, struct poll_fd *poll_fd,
                                     int poll_types, poll_callback_t callback,
                                     void *context, const char *phase, bool *refused) {
    struct fd *fd = poll_fd->fd;
    if (refused != NULL)
        *refused = false;

    // A oneshot registration that has reported says nothing more until it is
    // re-armed -- not even a hangup, which Linux's disarm (the mask cleared to
    // its private bits, EPOLLHUP and EPOLLERR with the rest) silences too.
    if (poll_fd->disarmed)
        return 0;
    // Edge-triggered: nothing to report unless something is ready that has
    // not been reported since the last event, and then report all that is
    // ready (see poll_fd_note_event_locked).
    if ((poll_fd->types & POLL_EDGETRIGGERED) && !(poll_types & ~poll_fd->triggered_types))
        return 0;
    if (!poll_types)
        return 0;

    int handled = callback(context, poll_types, poll_fd->info);
    if (poll_wait_trace_enabled()) {
        fprintf(stderr, "ish-pollwait: %s pid=%d comm=%s real=%d events=%#x handled=%d\n",
               phase, current->pid, current->comm,
               fd != NULL ? fd->real_fd : -1, poll_types, handled);
    }
    // The callback returns how many *results* this readiness produced, not
    // just whether it produced any: select counts an fd once per descriptor
    // set it is ready in, and poll counts it once per pollfd entry naming it,
    // so one fd can legitimately contribute more than one to the return value
    // (measured on Linux 6.12: an fd ready for read and write in both sets
    // makes select return 2, and the same fd in three pollfd entries makes
    // poll return 3). Clamping to 1 here undercounted both.
    int res = handled > 0 ? handled : 0;
    // Not taken: the caller had no room left (epoll_wait's maxevents). Nothing
    // was reported, so the next wait must still report it -- leave the
    // registration as it was. Marking it reported here lost the event for
    // good on an edge-triggered or oneshot registration, which reports again
    // only after a new event: two ready and maxevents 1, and the second one
    // was never heard of again.
    if (res == 0) {
        if (refused != NULL)
            *refused = true;
        return 0;
    }

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
        poll_fd->disarmed = true;
        if (poll_fd_has_host_wait(poll_fd))
            poll_sync_host_locked(poll_, fd);
        return res;
    }

    if (poll_fd->types & POLL_EDGETRIGGERED)
        poll_fd->triggered_types |= poll_types;
    return res;
}

static int poll_scan_ready_locked(struct poll *poll_, poll_callback_t callback, void *context) {
    poll_drain_host_locked(poll_);
    int res = 0;
    struct poll_fd *poll_fd, *tmp;
    list_for_each_entry_safe(&poll_->poll_fds, poll_fd, tmp, fds) {
        struct fd *fd = poll_fd->fd;
        if (poll_fd->disarmed) {
            poll_fd->host_events = 0;
            continue;
        }
        // Nothing collected from the host, so nothing has happened -- whatever
        // the file looks like now. Looking anyway is how the same data got
        // reported twice: data that arrived after the collection was reported
        // by this look, and then its event, still queued, by the next wait.
        if (poll_fd->host_edges && poll_fd->host_events == 0)
            continue;
        int raw_poll_types = poll_fd->host_events;
        if (fd->ops->poll)
            raw_poll_types |= fd->ops->poll(fd);
        int poll_types = raw_poll_types;
        poll_types &= poll_fd->types | POLL_HUP | POLL_ERR | POLL_NVAL;
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
        if (!poll_types) {
            poll_fd->host_events = 0;
            continue;
        }

        bool refused;
        res += poll_deliver_ready_locked(poll_, poll_fd, poll_types,
                                         callback, context, "callback", &refused);
        // Collected events stay until a report that has room takes them.
        if (!refused)
            poll_fd->host_events = 0;
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
    poll->host_resume_seen = atomic_load_explicit(&poll_host_resume_gen, memory_order_relaxed);
    lock_init(&poll->lock, "poll_create\0");
    return poll;
}

static inline bool poll_fd_has_host_wait(struct poll_fd *pollfd) {
    struct fd *fd = pollfd->fd;
    if (fd == NULL || fd->real_fd < 0)
        return false;
    if (fd->ops == &realfs_fdops || fd->ops == &socket_fdops)
        return true;
    // A named FIFO on the root filesystem is a host FIFO too, opened through
    // fakefs's copy of the realfs operations -- which the test above did not
    // recognise, so nothing ever watched one. Its readiness then came only
    // from rescans: a wait already blocked on it slept out the whole of
    // POLL_WAKE_RECHECK_NS before seeing a write, and an edge-triggered
    // registration, told once, was never told again, because only a host
    // event could say anything new had arrived.
    return fd->ops->poll == realfs_poll && S_ISFIFO(fd->stat.mode);
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
    // An edge-triggered watch of a socket, pipe or FIFO is one whose events
    // can be trusted to be all there is to know (see poll_fd.host_edges).
    // Anything else -- a device, a mix of level- and edge-triggered
    // registrations, whose host watch is level-triggered -- keeps the scan's
    // look at the file.
    bool host_edges = all_edge_triggered &&
        (S_ISSOCK(fd->stat.mode) || S_ISFIFO(fd->stat.mode));
    list_for_each_entry(&poll->poll_fds, poll_fd, fds) {
        if (poll_fd->fd == fd && poll_fd_has_host_wait(poll_fd))
            poll_fd->host_edges = host_edges;
    }
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

// Every registration of the file a host event is for hears of it; see
// poll_fd_note_event_locked. The host bits are kept for the next readiness
// scan as well, which adds them to what the file's own poll reports -- the
// host can know of an end of file a fresh look does not show (see the
// delivery after the host wait in poll_wait).
static void poll_note_host_event_locked(struct poll *poll_, struct real_poll_event *event) {
    struct poll_fd *candidate = rpe_data(event);
    struct poll_fd *owner = poll_find_ptr(poll_, candidate);
    // The notify pipe (no udata), and a watch of a registration since removed.
    if (owner == NULL || owner->poll != poll_)
        return;
    int host_events = rpe_events(event, candidate);
    struct poll_fd *poll_fd;
    list_for_each_entry(&poll_->poll_fds, poll_fd, fds) {
        if (poll_fd->fd != owner->fd)
            continue;
        poll_fd_note_event_locked(poll_fd, host_events);
        poll_fd->host_events |= host_events;
    }
}

// Take whatever the host has queued for this poll, without waiting, before a
// readiness scan.
//
// An edge-triggered registration of a host socket, pipe or FIFO is watched
// EV_CLEAR (EPOLLET on a Linux host): the host keeps one event queued per
// change until a wait retrieves it, and those events are the only way to
// know anything new happened. The scan used to report such a registration
// from its own look at the file, which retrieved nothing: the event stayed
// queued, the next wait took it for a new one, and the same data was
// reported again, and again for every event nobody had retrieved. That is
// epoll_wait acting level-triggered for EPOLLET -- a second wait with no new
// data reporting the same file -- which is a spin for an event loop that
// trusts the edge, as Go's and tokio's do. Now such a registration reports
// only events collected here or by the host wait, each of them once.
//
// Only a poll with such a watch needs it; every other poll, and every
// poll(2) and select(2), pays nothing.
static void poll_fd_recheck_locked(struct poll_fd *poll_fd);

void poll_drain_host_locked(struct poll *poll_) {
    bool edge_watched = false;
    struct poll_fd *poll_fd;
    list_for_each_entry(&poll_->poll_fds, poll_fd, fds) {
        if (poll_fd->host_edges) {
            edge_watched = true;
            break;
        }
    }
    if (!edge_watched)
        return;
    // Back from a suspension: look once at every host-edge registration,
    // since the host may have changed a socket there without an event -- see
    // poll_note_host_resume.
    unsigned gen = atomic_load_explicit(&poll_host_resume_gen, memory_order_relaxed);
    if (poll_->host_resume_seen != gen) {
        poll_->host_resume_seen = gen;
        list_for_each_entry(&poll_->poll_fds, poll_fd, fds) {
            if (poll_fd->host_edges && !poll_fd->disarmed)
                poll_fd_recheck_locked(poll_fd);
        }
    }
    // A level-triggered watch in the same poll is returned by every
    // collection for as long as it stays ready, so "until the host has
    // nothing" is not a way to stop: bound the rounds instead.
    for (int round = 0; round < 8; round++) {
        struct real_poll_event e[32];
        bool more = false;
        int n = real_poll_collect(&poll_->real, e, sizeof(e) / sizeof(e[0]), &more);
        for (int i = 0; i < n; i++)
            poll_note_host_event_locked(poll_, &e[i]);
        if (!more)
            break;
    }
}

// Arming an edge-triggered registration (ADD, or MOD, which re-arms it) is an
// event when the file is ready at that moment: Linux's ep_insert and
// ep_modify queue a ready registration, and that is how a program asks to hear
// again about data it left unread. For a host-edge registration the host
// queues one for most such states as the watch is programmed -- collected
// here, with anything else waiting -- but not for all: a FIFO whose writer has
// already gone is at end of file, and Darwin says nothing about it (measured).
// So what the file reports now counts too. Both land in the same collected
// bits, so the arm is reported once.
static void poll_arm_host_edges_locked(struct poll *poll_, struct poll_fd *poll_fd) {
    if (!poll_fd->host_edges)
        return;
    poll_drain_host_locked(poll_);
    poll_fd_recheck_locked(poll_fd);
}

// What the file reports now, taken for an event: for a host-edge registration
// at a moment the host may have said nothing (the callers above and below).
static void poll_fd_recheck_locked(struct poll_fd *poll_fd) {
    struct fd *fd = poll_fd->fd;
    int ready = fd->ops->poll != NULL ? fd->ops->poll(fd) : 0;
    ready &= poll_fd->types | POLL_HUP | POLL_ERR;
    if (ready) {
        poll_fd_note_event_locked(poll_fd, ready);
        poll_fd->host_events |= ready;
    }
}

// The app is back in the foreground (fs/sockrestart.c's resume runs on every
// return). A suspension is when iOS makes sockets defunct, and nothing says a
// defunct socket queues a host event -- the listeners sockrestart rebuilds
// lost their watches outright (poll_rearm_host_fd) -- so an edge-triggered
// registration, which hears only what the host says, could sit on a dead
// connection for good. The next scan of every poll with such a registration
// looks once at each of them (poll_drain_host_locked): a wait already blocked
// gets there within POLL_WAKE_RECHECK_NS. What it finds ready is reported
// once; for a live connection that costs one spare wake.
void poll_note_host_resume(void) {
    atomic_fetch_add_explicit(&poll_host_resume_gen, 1, memory_order_relaxed);
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
    poll_fd->host_events = 0;
    poll_fd->host_edges = false;
    poll_fd->disarmed = false;

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
        poll_arm_host_edges_locked(poll, poll_fd);
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

    // MOD re-arms: Linux's ep_modify polls the file and queues the
    // registration if it is ready, edge-triggered or not, and that is how a
    // program asks to be told again about data it left unread. Keeping what
    // had been reported (the old `&= types`) made a same-mask MOD a no-op for
    // every emulated fd. A host fd happened to work, because re-programming
    // the host watch queues a fresh event when the fd is ready (measured on
    // Darwin); that event is still what a scan now collects first, so the MOD
    // reports once, not twice.
    poll_fd->triggered_types = 0;
    poll_fd->disarmed = false;
    if (poll_fd_has_host_wait(poll_fd))
        poll_arm_host_edges_locked(poll, poll_fd);

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
        poll_fd_note_event_locked(poll_fd, events);
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

// The host object under `fd` has been replaced -- fs/sockrestart.c rebuilds a
// listener iOS killed and dup2()s the new socket over the old descriptor
// number. Closing the old one took every host watch of it with it, in every
// poll, so nothing would ever report the new one: an edge-triggered
// registration hears of a connection only from the host, and a Go or tokio
// server stopped accepting after the phone woke. Put each watch back and
// treat it as armed afresh, as a MOD would, and wake whoever is waiting.
void poll_rearm_host_fd(struct fd *fd) {
    struct poll_fd *poll_fd;
    lock(&fd->poll_lock, 0);
    list_for_each_entry(&fd->poll_fds, poll_fd, polls) {
        struct poll *poll = poll_fd->poll;
        lock(&poll->lock, 0);
        if (poll_fd_has_host_wait(poll_fd) && poll_sync_host_locked(poll, fd) == 0) {
            poll_fd->triggered_types = 0;
            poll_arm_host_edges_locked(poll, poll_fd);
        }
        poll_poke_notify_locked(poll);
        unlock(&poll->lock);
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
        poll_fd_note_event_locked(poll_fd, events);
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

// Whether a host wait of `wait` (NULL: none) should be cut to `cap`. Not when
// it is the guest's deadline and only just longer: a capped wait is not kept
// precise (real_poll_wait) and may end a few milliseconds late, which from
// just short of the deadline would carry the guest past it. So a deadline up
// to 10ms beyond the cap is waited for in one, precisely.
static bool poll_timeout_exceeds_cap(const struct timespec *wait, struct timespec cap) {
    if (wait == NULL)
        return true;
    struct timespec margin = {.tv_sec = 0, .tv_nsec = 10 * 1000 * 1000L};
    return timespec_positive(timespec_subtract(*wait, timespec_add(cap, margin)));
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

    // Rounds of this wait that expired with a signal raised the task reads as
    // blocked, for the stuck report in the timeout arm. It advances only on
    // those rounds, so it measures how long THAT state has lasted rather than
    // how long the wait has -- an idle poll is not the thing worth reporting.
    // Outside the loop because it has to count across iterations.
    unsigned capped_rounds = 0;

    // Whether the wake signals are this function's to repair after a wait.
    // Sampled HERE: before the wait, because asked afterwards "the signal is
    // blocked" cannot tell a caller that deliberately blocked it from a thread
    // that had it swallowed, and that is the whole distinction; and once per
    // CALL rather than per loop pass, because a program spinning through an
    // event loop on poll(fds, -1) would otherwise pay a sigprocmask per event.
    // Only for a wait the caller gave no timeout, which is the only kind that
    // can hang -- a poll WITH a timeout, the common case, pays nothing.
    // Same guard, same reason, as host_sleep_interruptible's own_wake_sigs in
    // kernel/time.c.
    bool own_wake_sigs = timeout == NULL && signal_thread_wake_sigs_unblocked();

    struct timespec deadline_storage = {0};
    struct timespec *deadline = NULL;
    if (timeout != NULL) {
        // Resuming after a job-control stop: keep the deadline this wait
        // already had. Restarting the relative timeout from zero would give
        // the guest a longer wait than it asked for every time it is stopped.
        if (current->poll_restart_valid) {
            deadline_storage = current->poll_restart_deadline;
            current->poll_restart_valid = false;
        } else {
            deadline_storage = timespec_add(timespec_now(CLOCK_MONOTONIC), *timeout);
        }
        deadline = &deadline_storage;
    } else {
        current->poll_restart_valid = false;
    }

    int res = 0;
    // Set by the two exits below; see poll_restart_deadline in kernel/task.h.
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
        // A CHECKPOINT FREEZE ends the wait as a signal does. It is not a
        // signal, so it has to be asked about separately -- these three tests
        // exist to IGNORE bare pokes (a TLB shootdown), which is right for
        // those and wrong for a freeze: the freeze needs the syscall to return
        // so the dispatcher can rewind over it and the task can park. The
        // EINTR never reaches the guest: poll_wait leaves as a restart while
        // the freeze is on, carrying its deadline (see its end).
        // A PTRACE_EVENT_STOP the task owes its tracer is the same kind of
        // thing, and it restarts the call the same way.
        bool signal_pending = checkpoint_freeze_pending() ||
            task_trap_stop_pending(current) ||
            !!((current->pending | task_group_pending(current)) & ~task_wake_blocked(current));
        unlock(&current->sighand->lock);
        if (signal_pending) {
            // ERESTARTNOHAND: a running handler still gives the guest its
            // EINTR, but a job-control stop must resume transparently.
            res = signal_restart_or_eintr_nohand(_EINTR);
            break;
        }

        // wait for a ready notification
        list_for_each_entry(&poll_->poll_fds, poll_fd, fds) {
            sockrestart_begin_listen_wait(poll_fd->fd);
        }
        int err;
        // Whether the host wait below ended because the deadline the GUEST
        // asked for has passed, as opposed to the cap this function imposes on
        // itself (see POLL_WAKE_RECHECK_NS). Only the first is a timeout the
        // guest may be told about; the second has to go round the loop again,
        // or a `select(..., NULL)` that must block for ever would return 0.
        bool deadline_reached = false;
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
                // A CHECKPOINT FREEZE ends the wait as a signal does. It is not a
        // signal, so it has to be asked about separately -- these three tests
        // exist to IGNORE bare pokes (a TLB shootdown), which is right for
        // those and wrong for a freeze: the freeze needs the syscall to return
        // so the dispatcher can rewind over it and the task can park. The
        // EINTR never reaches the guest: poll_wait leaves as a restart while
        // the freeze is on, carrying its deadline (see its end).
        // A PTRACE_EVENT_STOP the task owes its tracer is the same kind of
        // thing, and it restarts the call the same way.
        bool signal_pending = checkpoint_freeze_pending() ||
            task_trap_stop_pending(current) ||
            !!((current->pending | task_group_pending(current)) & ~task_wake_blocked(current));
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
                    struct timespec wake_recheck_timeout = {
                        .tv_sec = POLL_WAKE_RECHECK_NS / 1000000000L,
                        .tv_nsec = POLL_WAKE_RECHECK_NS % 1000000000L,
                    };
                    if (deadline != NULL) {
                        if (!poll_deadline_remaining(deadline, &remaining_timeout)) {
                            pthread_sigmask(SIG_SETMASK, &oldmask, NULL);
                            errno = 0;
                            err = 0;
                            deadline_reached = true;
                            sigunwind_end();
                            goto poll_wait_done;
                        }
                        wait_timeout = &remaining_timeout;
                    }
                    if (needs_periodic_host_rescan &&
                            poll_timeout_exceeds_cap(wait_timeout, periodic_rescan_timeout))
                        wait_timeout = &periodic_rescan_timeout;
                    // Never block on the host without a bound. An unbounded
                    // kevent/epoll_wait here has exactly two ways out -- the
                    // notify pipe and a wake signal -- and both can be missed,
                    // in which case the wait is permanent:
                    //
                    //   * kernel/signal.c's deliver_signal_unlocked_locked
                    //     skips signal_wake_task() entirely for a signal it
                    //     reads as blocked, so neither poke is even sent if
                    //     the sender's view of the mask differs from ours;
                    //   * the SIGUSR1/SIGUSR2 pokes are the ones
                    //     signal_thread_unwedge_wake_sigs() exists to repair --
                    //     a thread whose mask some OTHER thread set (Darwin's
                    //     sigprocmask sets them all; util/sync.c has the cause
                    //     that was fixed) is deaf to every later poke;
                    //   * and the notify pipe only carries fd readiness, so a
                    //     wait with no fds at all -- which is exactly what
                    //     zsh's `sigsuspend` becomes, pselect6(0, NULL, NULL,
                    //     NULL, NULL, mask), waiting for SIGCHLD -- has nothing
                    //     to write to it.
                    //
                    // That combination is what wedged every shell on a device
                    // after a memory-pressure run: each one parked in that
                    // pselect for ever, with its child already a zombie, while
                    // the app itself stayed healthy. kernel/time.c's sleep loop
                    // took the same medicine for the same reason, and its
                    // comment lists every other blocking site as already
                    // bounded -- crediting this one with "fs/poll.c has its
                    // notify pipe", which is true only when there are fds.
                    //
                    // The cap costs one wakeup per capped waiter per second and
                    // buys back nothing but latency: on expiry the loop rescans,
                    // re-reads the pending set (which is where a lost wake is
                    // actually noticed -- see the err == 0 arm) and waits again.
                    // A LONG timeout is as unbounded as no timeout, for
                    // every reason listed above: the ways out of the host wait
                    // are the notify pipe and a poke, and a poke that is
                    // swallowed leaves the wait running for as long as the
                    // guest asked. A daemon sitting in select() with a 30s
                    // timeout therefore could not be frozen inside the
                    // checkpoint's 5s, and that is exactly what happened --
                    // rsyslogd and chronyd on an M4 iPad, parked in kevent
                    // here, while every shape reproducible on macOS (where the
                    // poke lands) parked correctly.
                    //
                    // Capping it costs the same one wakeup per second the NULL
                    // case already pays, and the guest cannot tell: the loop
                    // recomputes the remaining deadline at the top and only
                    // reports a timeout once it has really passed.
                    if (poll_timeout_exceeds_cap(wait_timeout, wake_recheck_timeout)) {
                        wait_timeout = &wake_recheck_timeout;
                        atomic_fetch_add_explicit(&poll_capped_waits, 1,
                                memory_order_relaxed);
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
                    err = real_poll_wait(&poll_->real, e, sizeof(e)/sizeof(e[0]), wait_timeout,
                                         wait_timeout == &remaining_timeout);
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
            // A CHECKPOINT FREEZE ends the wait as a signal does. It is not a
        // signal, so it has to be asked about separately -- these three tests
        // exist to IGNORE bare pokes (a TLB shootdown), which is right for
        // those and wrong for a freeze: the freeze needs the syscall to return
        // so the dispatcher can rewind over it and the task can park. The
        // EINTR never reaches the guest: poll_wait leaves as a restart while
        // the freeze is on, carrying its deadline (see its end).
        // A PTRACE_EVENT_STOP the task owes its tracer is the same kind of
        // thing, and it restarts the call the same way.
        bool signal_pending = checkpoint_freeze_pending() ||
            task_trap_stop_pending(current) ||
            !!((current->pending | task_group_pending(current)) & ~task_wake_blocked(current));
            unlock(&current->sighand->lock);
            if (!signal_pending)
                continue;
            // The host wait was torn out and a guest signal is waiting. Same
            // ERESTARTNOHAND rule as the other two exits: a handler about to
            // run gives the guest its EINTR, a job-control stop does not.
            res = signal_restart_or_eintr_nohand(errno_map());
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
                sigset_t_ raised = current->pending | task_group_pending(current);
                sigset_t_ masked = task_wake_blocked(current);
                bool signal_pending = !!(raised & ~masked) ||
                    task_trap_stop_pending(current);
                // The whole of the process's queue, told or not: a signal
                // queued there that this thread blocks is exactly what the
                // warning below is for.
                raised = current->pending | current->sighand->pending;
                sigset_t_ stuck = raised & masked;
                unlock(&current->sighand->lock);
                if (signal_pending)
                    res = signal_restart_or_eintr_nohand(_EINTR);
                // Say what a long wait is actually stuck on, once.
                //
                // A task idle in poll for hours is normal and says nothing, so
                // the trigger is not the waiting -- it is waiting a long time
                // with a signal RAISED that this task believes is blocked. That
                // is the state a shell wedged in `sigsuspend` looks like from
                // the inside when the mask it was given has not taken effect,
                // and it is indistinguishable from healthy idling in every
                // counter the kernel keeps. Naming it costs one line per stuck
                // wait and would have turned a day of bisecting a device into
                // one look at dmesg.
                else if (stuck != 0 && ++capped_rounds == POLL_STUCK_ROUNDS &&
                         !atomic_exchange_explicit(&poll_stuck_logged, true,
                                                   memory_order_relaxed)) {
                    printk("WARNING: %d(%s) has waited %ds in poll with signals raised that it "
                           "reads as blocked: raised=%#llx blocked=%#llx stuck=%#llx. "
                           "Logged once for the life of the process.\n",
                           current->pid, current->comm,
                           (int) (POLL_STUCK_ROUNDS * (POLL_WAKE_RECHECK_NS / 1000000000L)),
                           (unsigned long long) raised,
                           (unsigned long long) masked,
                           (unsigned long long) stuck);
                }
            }
            // A wait that ended at its timeout is the only way a wedged
            // thread ever gets here -- a swallowed poke means nothing else
            // could have woken it -- so the repair belongs on this arm and not
            // on the wakeup path, where it would cost a sigprocmask per event.
            // Must run on a normal call stack, never from a handler, whose
            // sigreturn would put the wedged bit straight back. The cap is what
            // rescued THIS wait; this is what stops the next one needing to.
            if (own_wake_sigs && signal_thread_unwedge_wake_sigs()) {
                long n = atomic_fetch_add_explicit(&poll_wedged_repairs, 1,
                                                   memory_order_relaxed) + 1;
                if (n == 1)
                    printk("WARNING: host thread went deaf to its wake signal while polling "
                           "(pid=%d comm=%s); repaired. Further occurrences are counted in "
                           "/proc/ish/wake_signals, not logged.\n",
                           current->pid, current->comm);
            }
            // Nothing ready, no signal, and the guest's own deadline has not
            // passed: this expiry was the self-imposed cap (or the periodic
            // rescan), so go round again rather than reporting a timeout the
            // caller never asked for. Reporting one would break the very case
            // the cap was added for -- an infinite select would return 0 every
            // second -- and it is also what the periodic rescan has been doing
            // to an infinite poll on the fds it applies to.
            if (res == 0 && !deadline_reached)
                continue;
            break;
        }

        // What the host woke us for goes to its registrations, and the scan
        // reports it: fd->ops->poll() is still the preferred readiness source,
        // but Darwin can report EOF/HUP through kqueue when a follow-up
        // zero-time probe returns no bits, so the scan adds the host's own
        // bits to what it sees (poll_note_host_event_locked). If we only
        // rescanned, that host event could wake us forever without ever
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
        // Noting each entry against its registration and reporting from the
        // scan gives every registration one combined mask. The host watch
        // carries a single udata (see poll_sync_host_locked), but dup'd guest
        // fds may hold several registrations of that fd on this poll, so the
        // note fans each host event out to every one of them.
        //
        // And an edge-triggered registration reports everything that is ready
        // when its event comes, not only the filter that fired (see
        // poll_fd_note_event_locked), which is the scan's look at the file.
        for (int i = 0; i < err; i++) {
            if (poll_epoll_trace_enabled()) {
                struct poll_fd *candidate = rpe_data(&e[i]);
                struct poll_fd *owner = poll_find_ptr(poll_, candidate);
                if (owner != NULL && owner->poll == poll_) {
                    struct fd *fd = owner->fd;
                    char path[MAX_PATH];
                    path[0] = '\0';
                    if (fd != NULL)
                        generic_getpath(fd, path);
                    printk("epoll-trace: host pid=%d comm=%s real=%d host=%#x req=%#x path=%s ops=%p\n",
                           current->pid, current->comm,
                           fd != NULL ? fd->real_fd : -1, rpe_events(&e[i], candidate),
                           owner->types, path,
                           fd != NULL ? (void *) fd->ops : NULL);
                }
            }
            poll_note_host_event_locked(poll_, &e[i]);
        }
        res += poll_scan_ready_locked(poll_, callback, context);

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
    // A checkpoint freeze is a restart too: the three exits above see it as a
    // bare EINTR, which syscall_result_should_restart restarts anyway -- but
    // then nothing had carried the deadline, and the re-executed call waited
    // its whole timeout again: a plain save two seconds into a six-second
    // poll made it take eight, and a restore started every select() timeout
    // over. Reported as the restart it is, the deadline goes with it below,
    // and into the image with the task (kernel/checkpoint.c).
    if (res == _EINTR && checkpoint_freeze_pending())
        res = _ERESTART_NOHAND;
    // Restarting after a job-control stop: hand the deadline we already
    // computed to the re-executed syscall, which cannot see the time this
    // call spent waiting (it only gets the guest's original relative
    // timeout). An untimed wait has nothing to carry.
    if (res == _ERESTART_NOHAND && deadline != NULL) {
        current->poll_restart_deadline = *deadline;
        current->poll_restart_valid = true;
    }
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

static int real_poll_wait(struct real_poll *real, struct real_poll_event *events, int max,
        struct timespec *timeout, bool UNUSED(precise)) {
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

static int real_poll_collect(struct real_poll *real, struct real_poll_event *events, int max,
        bool *more) {
    int count;
    do {
        count = epoll_wait(real->fd, (struct epoll_event *) events, max, 0);
    } while (count < 0 && errno == EINTR);
    *more = count == max;
    return count < 0 ? 0 : count;
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
        // Darwin's kqueue does not implement every filter for every fd type:
        // EVFILT_EXCEPT is missing on regular files, and character devices
        // other than ttys (/dev/null, /dev/zero, /dev/random) and directories
        // support no filter at all. Treat that as "filter unavailable" rather
        // than failing the whole registration -- returning EINVAL out of the
        // guest's poll()/select()/epoll_ctl() is not something Linux ever does
        // for a valid fd, and it made musl's AT_SECURE startup (which polls
        // fds 0, 1 and 2 and a_crash()es if that fails) kill every setuid
        // binary whose stdio was a host device node, e.g. `sudo ... >/dev/null`
        // under the command-line build. Nothing is lost by not registering:
        // realfs_poll reports exactly these objects as permanently ready, so
        // poll_wait's readiness scan returns before it ever blocks, and no
        // wakeup is needed to notice a readiness that is always there.
        if ((events[i].data == EINVAL || events[i].data == ENOTSUP || events[i].data == EPERM) &&
                (events[i].filter == EVFILT_EXCEPT ||
                 events[i].filter == EVFILT_READ ||
                 events[i].filter == EVFILT_WRITE))
            continue;
        errno = (int) events[i].data;
        return -1;
    }
    return 0;
}

static int real_poll_update(struct real_poll *real, int fd, int types, void *data) {
    // The write filter is registered for HUP and RDHUP as well as for WRITE:
    // it is the only thing that distinguishes a half-close from a full one.
    // kqueue sets EV_EOF on EVFILT_READ as soon as the peer stops writing, and
    // on EVFILT_WRITE once our own direction is down -- by the peer's close,
    // or by our own shutdown(SHUT_WR) -- so the pair together says which
    // happened. Measured on Darwin. (A socket's EOF is answered by sock_poll,
    // which asks each direction for itself; see rpe_events.)
    bool want_read = types & (POLL_READ | POLL_HUP | POLL_RDHUP);
    bool want_write = types & (POLL_WRITE | POLL_HUP | POLL_RDHUP);
    struct kevent e[3] = {
        {.filter = EVFILT_READ, .flags = want_read ? EV_ADD : EV_DELETE},
        {.filter = EVFILT_WRITE, .flags = want_write ? EV_ADD : EV_DELETE},
        {.filter = EVFILT_EXCEPT, .flags = types & POLL_ERR ? EV_ADD : EV_DELETE},
    };
    // Set the low water mark really high so we'll only get woken up on a hangup.
    //
    // ...except Darwin does not honour it on every object. Measured with
    // ISH_TRACE_POLL_WAIT on a plain AF_UNIX socketpair: a registration of
    // READ|ERR|HUP|NVAL (a guest polling POLLIN, which implies HUP) arms
    // EVFILT_WRITE for the hangup, NOTE_LOWAT and all, and kqueue then reports
    // it writable immediately and forever. poll_wait wakes, masks the event
    // against what the guest asked for, gets nothing, and sleeps again -- and
    // is woken again at once, because EVFILT_WRITE is level-triggered and the
    // send buffer is still empty. That is a 100%-CPU spin for the whole
    // duration of an ordinary blocking poll() on a quiet socket, and it is why
    // an idle chronyd, an idle rsyslogd and an idle sshd-session each pinned a
    // core on device while sitting in a poll they were entirely right to make.
    //
    // EV_CLEAR is the fix that keeps the hangup: the filter fires on the
    // transition rather than on the level, so "still writable" is reported
    // once and then stays quiet, while an actual hangup is a new transition
    // and still wakes us. Applied only to a filter registered SOLELY for the
    // hangup -- a registration that genuinely wants POLL_READ or POLL_WRITE
    // must stay level-triggered, or a guest that polls without draining would
    // miss the readiness it never consumed.
    if (!(types & POLL_READ) && want_read) {
        e[0].fflags = NOTE_LOWAT;
        e[0].data = INT_MAX;
        e[0].flags |= EV_CLEAR;
    }
    if (!(types & POLL_WRITE) && want_write) {
        e[1].fflags = NOTE_LOWAT;
        e[1].data = INT_MAX;
        e[1].flags |= EV_CLEAR;
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

// A kevent timeout is coalesced like any other Darwin sleep (see
// host_nanosleep_precise in util/timer.c): a guest poll, select or epoll_wait
// with a timeout came back 1-2ms after it, on the Mac and on an M4 iPad, where
// Linux is within its 50us of slack. So a timeout that is the guest's own
// deadline is kept by a one-shot kqueue timer marked NOTE_CRITICAL, added to
// the same wait; the timeout stays as the backstop, and a timer that fires
// comes back as an event that is nobody's fd.
//
// An epoll instance can have more than one thread waiting on its kqueue, and
// the timer's event can reach any of them. Each thread drops every deadline
// event it is given, its own or not: one taken from a neighbour only costs the
// neighbour its precision, back to the backstop. A wait a poke unwinds out of
// (sigunwind_start) leaves its timer behind, to fire once and be dropped the
// same way.
static char real_poll_deadline_tag;
#define REAL_POLL_DEADLINE_MIN_NS 200000L

static int real_poll_wait(struct real_poll *real, struct real_poll_event *events, int max,
        struct timespec *timeout, bool precise) {
    struct kevent timer;
    uintptr_t ident = 0;
    if (precise && timeout != NULL &&
            (timeout->tv_sec > 0 || timeout->tv_nsec >= REAL_POLL_DEADLINE_MIN_NS)) {
        static _Atomic uintptr_t deadline_idents;
        ident = atomic_fetch_add_explicit(&deadline_idents, 1, memory_order_relaxed) + 1;
        // poll_wait's caps keep a timeout under a few seconds.
        int64_t ns = (int64_t) timeout->tv_sec * 1000000000 + timeout->tv_nsec;
        EV_SET(&timer, ident, EVFILT_TIMER, EV_ADD | EV_ONESHOT, NOTE_NSECONDS | NOTE_CRITICAL,
               ns, &real_poll_deadline_tag);
    }
    int count = kevent(real->fd, &timer, ident != 0 ? 1 : 0, (struct kevent *) events, max, timeout);
    int saved = errno;
    bool fired = false, refused = false;
    if (count > 0) {
        int kept = 0;
        for (int i = 0; i < count; i++) {
            if (events[i].real.udata != &real_poll_deadline_tag) {
                events[kept++] = events[i];
                continue;
            }
            if (events[i].real.ident == ident) {
                if (events[i].real.flags & EV_ERROR)
                    refused = true;
                else
                    fired = true;
            }
        }
        count = kept;
    }
    if (refused && count == 0) {
        // The timer was refused, which kevent reports without waiting at all.
        // Wait the ordinary way, or poll_wait would come straight back here.
        return kevent(real->fd, NULL, 0, (struct kevent *) events, max, timeout);
    }
    // The wait is over, and what it returned has left the kqueue: an
    // edge-triggered event will not be reported again. poll_wait ends the
    // poke's unwind (sigunwind_start) once this returns, but a poke that
    // unwound the syscall below would throw these events away, so end it now.
    sigunwind_end();
    if (ident != 0 && !fired && !refused) {
        EV_SET(&timer, ident, EVFILT_TIMER, EV_DELETE, 0, 0, NULL);
        kevent(real->fd, &timer, 1, NULL, 0, NULL);
    }
    errno = saved;
    return count;
}

static int real_poll_collect(struct real_poll *real, struct real_poll_event *events, int max,
        bool *more) {
    struct timespec zero = {0, 0};
    int count;
    do {
        count = kevent(real->fd, NULL, 0, (struct kevent *) events, max, &zero);
    } while (count < 0 && errno == EINTR);
    *more = count == max;
    // A deadline timer a poked wait left behind (real_poll_wait) is nobody's
    // event.
    int kept = 0;
    for (int i = 0; i < count; i++) {
        if (events[i].real.udata != &real_poll_deadline_tag)
            events[kept++] = events[i];
    }
    return kept;
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
    // Whether end-of-input is a HANGUP depends on what this is. For a pipe or
    // a fifo the writer closing is the hangup, full stop. For a SOCKET it is
    // only half of one: the peer may have shut down writing and still be
    // reading, and Linux says EPOLLRDHUP for that and reserves EPOLLHUP for
    // both directions being down. Reporting HUP for a half-close was provably
    // wrong -- the same socket was still writable -- and EPOLLHUP is what a
    // program treats as "connection over".
    bool is_socket = pfd != NULL && pfd->fd != NULL && S_ISSOCK(pfd->fd->type);
    // On a socket, EV_EOF means the connection's state changed, and the filter
    // that saw it only knows its own half. What Linux reports for that state
    // needs both: a peer that closed is readable (the read returns 0, and
    // Linux sets EPOLLIN whenever RCV_SHUTDOWN is), and it is a HUP only once
    // both directions are down. EVFILT_READ alone cannot tell, and an EPOLLIN
    // registration watches only EVFILT_READ. So ask sock_poll, which takes a
    // fresh look and gives the answer a poll made after the close gets.
    //
    // Before this, a poll(POLLIN) already blocked when the peer closed woke
    // with POLLHUP alone, revents 0x10 where Linux gives 0x11 (docs/
    // build_556_musts.md item 3): EVFILT_WRITE's EOF supplied the HUP and
    // nothing supplied the IN. A poll made after the close never came
    // through here, which is why the case looked fixed. epoll(EPOLLIN) got
    // 0x11 only by luck. The RDHUP its EVFILT_READ event produced matched
    // nothing it asked for, so poll_wait went round again, and the rescan
    // asked sock_poll. Mapping EVFILT_READ's EOF to POLL_READ instead fixed
    // poll and broke epoll, which then woke with EPOLLIN alone (measured),
    // because that one event cannot know the write side is down too.
    if (is_socket && (rpe->real.flags & EV_EOF) && pfd->fd->ops->poll != NULL &&
            (rpe->real.filter == EVFILT_READ || rpe->real.filter == EVFILT_WRITE))
        return pfd->fd->ops->poll(pfd->fd);
    if (rpe->real.filter == EVFILT_READ) {
        int events = 0;
        if (rpe->real.data > 0)
            events |= POLL_READ;
        if (rpe->real.flags & EV_EOF)
            events |= is_socket ? POLL_RDHUP : POLL_HUP;
        return events;
    }
    if (rpe->real.filter == EVFILT_WRITE) {
        // EV_EOF here means our own direction is down. That is not by itself
        // the end of the connection -- our own shutdown(SHUT_WR) does it too,
        // and Linux calls that merely writable -- which is why a socket with
        // a poll operation was answered by it above. Only a socket without
        // one gets this guess.
        if (is_socket && (rpe->real.flags & EV_EOF))
            return POLL_WRITE | POLL_HUP | POLL_RDHUP;
        return POLL_WRITE;
    }
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
        // ...nor one only Darwin raises: an AF_UNIX datagram socket whose
        // peer closed is idle on Linux, and poll(events=0) said POLLERR for
        // it here. See sock_host_error_is_peer_gone.
        if (pfd != NULL && sock_host_error_is_peer_gone(pfd->fd, so_error))
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
