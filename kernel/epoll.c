#include "kernel/calls.h"
#include "fs/poll.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static struct fd_ops epoll_ops;

static bool epoll_trace_enabled(void) {
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("ISH_TRACE_EPOLL") != NULL ? 1 : 0;
    return enabled == 1;
}

static bool epoll_trace_comm(void) {
    return epoll_trace_enabled() && current != NULL && strcmp(current->comm, "compile") == 0;
}

fd_t sys_epoll_create(int_t flags) {
    STRACE("epoll_create(%#x)", flags);
    if (flags & ~(O_CLOEXEC_))
        return _EINVAL;

    struct fd *fd = adhoc_fd_create(&epoll_ops);
    if (fd == NULL)
        return _ENOMEM;
    struct poll *poll = poll_create();
    if (IS_ERR(poll))
        return PTR_ERR(poll);
    poll->owner_fd = fd;
    fd->epollfd.poll = poll;
    return f_install(fd, flags);
}
fd_t sys_epoll_create0() {
    return sys_epoll_create(0);
}

// Linux applies __attribute__((packed)) to struct epoll_event ONLY under
// __x86_64__ (uapi/linux/eventpoll.h's EPOLL_PACKED). Every other guest
// architecture keeps the naturally-aligned 16-byte layout — `data` at
// offset 8, not the packed 12-byte layout's offset 4. So the on-the-wire
// epoll_event layout is a GUEST-ABI property: i386/amd64 use the packed
// form, arm64 the aligned form below. Marshalling arm64 with the packed
// layout put the guest's `data` word at the wrong offset AND used the
// wrong array stride, so the Go runtime's netpoll read a garbage pollDesc
// pointer and SIGSEGV'd during `go build` (runtime.netpoll <- sysmon).
struct epoll_event_ {
    uint32_t events;
    uint64_t data;
} __attribute__((packed));

struct epoll_event_arm64 {
    uint32_t events;
    uint32_t pad;
    uint64_t data;
};

static bool epoll_event_aligned(void) {
    if (current == NULL)
        return false;
    return current->abi == GUEST_ABI_ARM64 || current->abi == GUEST_ABI_RISCV64;
}

#define EPOLL_CTL_ADD_ 1
#define EPOLL_CTL_DEL_ 2
#define EPOLL_CTL_MOD_ 3
#define EPOLLET_ (1 << 31)
#define EPOLLONESHOT_ (1 << 30)
#define EPOLLEXCLUSIVE_ (1 << 28)

// Linux caps epoll nesting depth at EP_MAX_NESTS (fs/eventpoll.c) and treats
// either a genuine cycle or exceeding that depth as ELOOP. There was no such
// check here at all: adding an epoll fd `fd` that already (transitively)
// watches `target` silently succeeded via poll_add_fd, leaving a live cycle
// in memory. That's exactly what stress-ng's --epoll stressor probes for
// (two epoll instances added into each other) -- since the add "succeeded"
// (returned 0), errno was left untouched from an unrelated earlier syscall,
// so the stressor's failure message showed a stale, misleading errno instead
// of the real problem: the call should have failed with ELOOP and didn't.
// Linux's ceiling on epoll_wait's maxevents (fs/eventpoll.c): anything larger
// is EINVAL there, so match it rather than trying to serve the request.
#define EP_MAX_EVENTS ((int) (INT_MAX / sizeof(struct epoll_event_)))

#define EP_MAX_NESTS 4

// Would adding `target` -> `fd` (i.e. target starts watching fd) create a
// cycle? True if `target` is reachable by following fd's own nested-epoll
// children, or if doing so would exceed Linux's nesting depth limit.
static bool epoll_would_loop(struct fd *target, struct fd *fd, int depth) {
    if (fd->ops != &epoll_ops)
        return false;
    if (depth >= EP_MAX_NESTS)
        return true;
    struct poll *poll = fd->epollfd.poll;
    lock(&poll->lock, 0);
    bool loop = false;
    struct poll_fd *poll_fd;
    list_for_each_entry(&poll->poll_fds, poll_fd, fds) {
        struct fd *child = poll_fd->fd;
        if (child == NULL)
            continue;
        if (child == target || epoll_would_loop(target, child, depth + 1)) {
            loop = true;
            break;
        }
    }
    unlock(&poll->lock);
    return loop;
}

int_t sys_epoll_ctl_guest(fd_t epoll_f, int_t op, fd_t f, guest_addr_t event_addr) {
    STRACE("epoll_ctl(%d, %d, %d, %#x)", epoll_f, op, f, event_addr);
    struct fd *epoll = f_get_retain(epoll_f);
    if (epoll == NULL)
        return _EBADF;
    if (epoll->ops != &epoll_ops) {
        fd_close(epoll);
        return _EINVAL;
    }
    struct fd *fd = f_get_retain(f);
    if (fd == NULL) {
        fd_close(epoll);
        return _EBADF;
    }

    // Linux: targeting the epoll instance itself is EINVAL. (A *different*
    // epoll fd may be nested, but an epoll cannot watch itself.)
    if (fd == epoll) {
        fd_close(fd);
        fd_close(epoll);
        return _EINVAL;
    }
    // Reject unknown ops up front. Without this an unknown op fell through to
    // the MOD path below, so e.g. epoll_ctl(ep, 9999, fd) silently modified a
    // registered fd (or returned ENOENT) instead of EINVAL.
    if (op != EPOLL_CTL_ADD_ && op != EPOLL_CTL_DEL_ && op != EPOLL_CTL_MOD_) {
        fd_close(fd);
        fd_close(epoll);
        return _EINVAL;
    }

    if (op == EPOLL_CTL_DEL_) {
        int_t res = poll_del_fd(epoll->epollfd.poll, fd, f);
        fd_close(fd);
        fd_close(epoll);
        return res;
    }

    uint32_t ev_events;
    uint64_t ev_data;
    if (epoll_event_aligned()) {
        struct epoll_event_arm64 event;
        if (user_get(event_addr, event)) {
            fd_close(fd);
            fd_close(epoll);
            return _EFAULT;
        }
        ev_events = event.events;
        ev_data = event.data;
    } else {
        struct epoll_event_ event;
        if (user_get(event_addr, event)) {
            fd_close(fd);
            fd_close(epoll);
            return _EFAULT;
        }
        ev_events = event.events;
        ev_data = event.data;
    }
    STRACE(" {events: %#x, data: %#llx}", ev_events, (unsigned long long) ev_data);
    if (epoll_trace_comm()) {
        printk("epoll-trace: ctl pid=%d comm=%s epfd=%d op=%d fd=%d real=%d req_events=%#x data=%#llx\n",
               current->pid, current->comm, epoll_f, op, f,
               fd->real_fd, ev_events, (unsigned long long) ev_data);
    }

    // Linux enforces three EPOLLEXCLUSIVE (bit 28) EINVAL rules (man
    // epoll_ctl(2)) that iSH-AOK's straight pass-through of guest event bits
    // never checked. Only the EINVAL validation is implemented here, not the
    // full exclusive-wakeup load-balancing behavior -- stress-ng's --epoll
    // conformance check only exercises the former.
    if (op == EPOLL_CTL_MOD_ && (ev_events & EPOLLEXCLUSIVE_)) {
        // Rule 1: EPOLLEXCLUSIVE is never valid on MOD, independent of
        // whether the target fd is even registered on this epoll instance.
        fd_close(fd);
        fd_close(epoll);
        return _EINVAL;
    }
    if (op == EPOLL_CTL_MOD_ && poll_fd_is_exclusive(epoll->epollfd.poll, fd, f)) {
        // Rule 2: once registered with EPOLLEXCLUSIVE, the registration can
        // never be modified again, even to new bits without EPOLLEXCLUSIVE.
        fd_close(fd);
        fd_close(epoll);
        return _EINVAL;
    }
    if (op == EPOLL_CTL_ADD_ && (ev_events & EPOLLEXCLUSIVE_) && fd->ops == &epoll_ops) {
        // Rule 3: EPOLLEXCLUSIVE on a nested epoll fd is invalid.
        fd_close(fd);
        fd_close(epoll);
        return _EINVAL;
    }

    int_t res;
    if (op == EPOLL_CTL_ADD_) {
        // Not everything can be polled, and Linux answers EPERM for the
        // things that cannot. Accepting them is worse than an error: an event
        // that can never arrive looks exactly like an idle fd, so a program
        // waiting on a file it should have been told to just read blocks
        // forever with no indication why.
        //
        // Linux decides per-file, on whether the entry has an f_op->poll, and
        // its synthetic filesystems are not uniform about it -- measured:
        // /proc/self/mountinfo, /proc/cpuinfo, /proc/uptime and
        // /sys/kernel/profiling are all accepted while /proc/self/stat is
        // EPERM, and every DIRECTORY is EPERM including /proc itself.
        //
        // This is deliberately the coarser rule: a directory always, and a
        // regular file on an ordinary filesystem. That gets every real file
        // and every directory right, keeps procfs and sysfs working (which is
        // what mount-change notification needs -- the guest suite's
        // mountinfo_epollet and epoll_nested both poll /proc/self/mountinfo),
        // and over-permits a handful of procfs entries rather than breaking
        // the ones that matter.
        bool synthetic_fs = fd->mount != NULL &&
            (fd->mount->fs == &procfs || fd->mount->fs == &sysfs);
        if (S_ISDIR(fd->type) || (S_ISREG(fd->type) && !synthetic_fs))
            res = _EPERM;
        else if (poll_has_fd(epoll->epollfd.poll, fd, f))
            res = _EEXIST;
        else if (epoll_would_loop(epoll, fd, 0))
            res = _ELOOP;
        else
            res = poll_add_fd(epoll->epollfd.poll, fd, f, ev_events, (union poll_fd_info) ev_data);
    } else {
        res = poll_mod_fd(epoll->epollfd.poll, fd, f, ev_events, (union poll_fd_info) ev_data);
    }
    fd_close(fd);
    fd_close(epoll);
    return res;
}

int_t sys_epoll_ctl(fd_t epoll_f, int_t op, fd_t f, addr_t event_addr) {
    return sys_epoll_ctl_guest(epoll_f, op, f, event_addr);
}

struct epoll_context {
    struct epoll_event_ *events;
    int n;
    int max_events;
};

static int epoll_callback(void *context, int types, union poll_fd_info info) {
    struct epoll_context *c = context;
    if (c->n >= c->max_events)
        return 0;
    if (epoll_trace_comm()) {
        printk("epoll-trace: callback pid=%d comm=%s slot=%d types=%#x data=%#llx\n",
               current->pid, current->comm, c->n, types,
               (unsigned long long) info.num);
    }
    c->events[c->n++] = (struct epoll_event_) {.events = types, .data = info.num};
    return 1;
}

static int epoll_wait_common(fd_t epoll_f, guest_addr_t events_addr, int_t max_events, struct timespec *timeout_ts_ptr) {
    struct fd *epoll = f_get_retain(epoll_f);
    if (epoll == NULL)
        return _EBADF;
    if (epoll->ops != &epoll_ops) {
        fd_close(epoll);
        return _EINVAL;
    }

    if (max_events <= 0) {
        fd_close(epoll);
        return _EINVAL;
    }
    // Linux's own ceiling, for errno parity: anything above it is EINVAL rather
    // than an allocation attempt.
    if (max_events > EP_MAX_EVENTS) {
        fd_close(epoll);
        return _EINVAL;
    }
    // NOT a VLA. max_events is guest-controlled, and sizing stack storage by it
    // let one epoll_wait(~1e6) fault the host process on the C stack before a
    // single fd was examined -- killing the emulator and every guest process in
    // it. The heap fails an oversized request with ENOMEM instead.
    struct epoll_event_ *events = calloc((size_t) max_events, sizeof(*events));
    if (events == NULL) {
        fd_close(epoll);
        return _ENOMEM;
    }

    struct epoll_context context = {.events = events, .n = 0, .max_events = max_events};
    STRACE("...\n");
    // A guest infinite wait (timeout == -1, i.e. timeout_ts_ptr == NULL) must
    // block until an fd is ready or a signal arrives. Real Linux never returns
    // 0 events from epoll_wait(-1); some guests assert on it -- notably libuv's
    // uv__io_poll does `assert(timeout != -1)` when epoll_wait returns 0, so
    // cmake/node/etc. abort. We still cap each underlying wait at 2 s as a
    // safety net against a missed readiness notification (the reason the Go
    // scheduler originally needed this -- a re-armed wait re-scans readiness and
    // recovers the missed transition), but when the cap expires with nothing
    // ready we wait again instead of reporting a spurious timeout to the guest.
    bool guest_infinite = (timeout_ts_ptr == NULL);
    struct timespec bounded = { .tv_sec = 2 };
    int res;
    do {
        context.n = 0;
        res = poll_wait(epoll->epollfd.poll, epoll_callback, &context,
                        guest_infinite ? &bounded : timeout_ts_ptr);
    } while (guest_infinite && res == 0);
    // epoll_wait is EINTR on a pending signal, full stop -- it does not
    // restart. Linux's ep_poll() breaks out with -EINTR the moment
    // signal_pending() is true and never reaches the restart machinery, while
    // poll and select resume transparently through their restart block. The
    // difference is visible with nothing more than a job-control stop: on
    // Devuan 6 / Linux 6.12, ^Z-ing a process waiting in poll() and resuming
    // leaves the poll completing, and the same treatment of epoll_wait()
    // returns EINTR. AOK shared poll_wait() for both and so restarted both.
    if (res == _ERESTART || res == _ERESTART_NOHAND)
        res = _EINTR;
    STRACE("%d end epoll_wait", current->pid);
    if (res >= 0) {
        for (int i = 0; i < res; i++) {
            STRACE(" {events: %#x, data: %#x}", events[i].events, events[i].data);
        }
        int fault = 0;
        if (res > 0) {
            if (epoll_event_aligned()) {
                struct epoll_event_arm64 *aligned = calloc((size_t) res, sizeof(*aligned));
                if (aligned == NULL) {
                    free(events);
                    fd_close(epoll);
                    return _ENOMEM;
                }
                for (int i = 0; i < res; i++) {
                    aligned[i].events = events[i].events;
                    aligned[i].pad = 0;
                    aligned[i].data = events[i].data;
                }
                fault = user_write(events_addr, aligned, sizeof(*aligned) * (size_t) res);
                free(aligned);
            } else {
                fault = user_write(events_addr, events, sizeof(struct epoll_event_) * res);
            }
        }
        if (fault) {
            free(events);
            fd_close(epoll);
            return _EFAULT;
        }
    }
    free(events);
    fd_close(epoll);
    return res;
}

int_t sys_epoll_wait_guest(fd_t epoll_f, guest_addr_t events_addr, int_t max_events, int_t timeout) {
    STRACE("epoll_wait(%d, %#x, %d, %d)", epoll_f, events_addr, max_events, timeout);
    struct timespec timeout_ts;
    struct timespec *timeout_ts_ptr = NULL;
    if (timeout >= 0) {
        timeout_ts.tv_sec = timeout / 1000;
        timeout_ts.tv_nsec = (timeout % 1000) * 1000000;
        timeout_ts_ptr = &timeout_ts;
    }
    return epoll_wait_common(epoll_f, events_addr, max_events, timeout_ts_ptr);
}

int_t sys_epoll_wait(fd_t epoll_f, addr_t events_addr, int_t max_events, int_t timeout) {
    return sys_epoll_wait_guest(epoll_f, events_addr, max_events, timeout);
}

int_t sys_epoll_pwait_guest(fd_t epoll_f, guest_addr_t events_addr, int_t max_events, int_t timeout, guest_addr_t sigmask_addr, dword_t sigsetsize) {
    sigset_t_ mask;
    if (sigmask_addr != 0) {
        if (sigsetsize != sizeof(sigset_t_))
            return _EINVAL;
        if (user_get(sigmask_addr, mask))
            return _EFAULT;
        sigmask_set_temp(mask);
    }

    struct timespec timeout_ts;
    struct timespec *timeout_ts_ptr = NULL;
    if (timeout >= 0) {
        timeout_ts.tv_sec = timeout / 1000;
        timeout_ts.tv_nsec = (timeout % 1000) * 1000000;
        timeout_ts_ptr = &timeout_ts;
    }

    return epoll_wait_common(epoll_f, events_addr, max_events, timeout_ts_ptr);
}

int_t sys_epoll_pwait(fd_t epoll_f, addr_t events_addr, int_t max_events, int_t timeout,
        addr_t sigmask_addr, dword_t sigsetsize) {
    return sys_epoll_pwait_guest(epoll_f, events_addr, max_events, timeout, sigmask_addr, sigsetsize);
}

int_t sys_epoll_pwait2_guest(fd_t epoll_f, guest_addr_t events_addr, int_t max_events, guest_addr_t timeout_addr, guest_addr_t sigmask_addr, dword_t sigsetsize) {
    sigset_t_ mask;
    if (sigmask_addr != 0) {
        if (sigsetsize != sizeof(sigset_t_))
            return _EINVAL;
        if (user_get(sigmask_addr, mask))
            return _EFAULT;
        sigmask_set_temp(mask);
    }

    struct timespec timeout_ts;
    struct timespec *timeout_ts_ptr = NULL;
    if (timeout_addr != 0) {
        struct timespec64_ timeout_ts64;
        if (user_get(timeout_addr, timeout_ts64))
            return _EFAULT;
        timeout_ts.tv_sec = timeout_ts64.sec;
        timeout_ts.tv_nsec = timeout_ts64.nsec;
        if (timeout_ts.tv_sec < 0 || timeout_ts.tv_nsec < 0 || timeout_ts.tv_nsec >= 1000000000)
            return _EINVAL;
        timeout_ts_ptr = &timeout_ts;
    }

    return epoll_wait_common(epoll_f, events_addr, max_events, timeout_ts_ptr);
}

int_t sys_epoll_pwait2(fd_t epoll_f, addr_t events_addr, int_t max_events,
        addr_t timeout_addr, addr_t sigmask_addr, dword_t sigsetsize) {
    return sys_epoll_pwait2_guest(epoll_f, events_addr, max_events, timeout_addr, sigmask_addr, sigsetsize);
}

static int epoll_close(struct fd *fd) {
    // Stop wakeup cascades from resolving to this dying fd before the poll
    // goes away (poll_wakeup reads owner_fd under poll->lock).
    lock(&fd->epollfd.poll->lock, 0);
    fd->epollfd.poll->owner_fd = NULL;
    unlock(&fd->epollfd.poll->lock);
    poll_destroy(fd->epollfd.poll);
    return 0;
}

// Readiness of the epoll fd itself: Linux reports an epoll fd readable when
// its ready list is non-empty, which is what makes epoll-inside-epoll work.
// systemd relies on it: sd-event's epoll watches libmount's mount-monitor
// epoll (level-triggered), whose only member is /proc/self/mountinfo
// registered EPOLLIN|EPOLLET. Without this callback the outer scan saw the
// epoll fd as never-ready, mount-table edges died inside the inner epoll,
// and systemd's mount units "protocol"-failed on most boots (tmp.mount)
// because the rescan a mountinfo event triggers never ran.
//
// Evaluate members the same way poll_scan_ready_locked would: virtual fds
// via ops->poll masked by the registration's interest + ET suppression.
// Host-backed members (real sockets/files) have no ops->poll; their events
// arrive through the host backend of whichever poll holds the watch and do
// not cascade here -- a nested epoll of purely host fds still needs a
// waiter on the inner epoll (none of the known nested-epoll users do this;
// revisit if one appears).
static int epoll_poll(struct fd *fd) {
    struct poll *poll = fd->epollfd.poll;
    int res = 0;
    lock(&poll->lock, 0);
    struct poll_fd *poll_fd;
    list_for_each_entry(&poll->poll_fds, poll_fd, fds) {
        struct fd *member = poll_fd->fd;
        if (member == NULL || member->ops->poll == NULL)
            continue;
        int types = member->ops->poll(member);
        types &= poll_fd->types | POLL_HUP | POLL_ERR | POLL_NVAL;
        if (poll_fd->types & POLL_EDGETRIGGERED)
            types &= ~poll_fd->triggered_types;
        if (types) {
            res = POLL_READ;
            break;
        }
    }
    unlock(&poll->lock);
    return res;
}

static struct fd_ops epoll_ops = {
    .anon_inode_class = "eventpoll",
    .poll = epoll_poll,
    .close = epoll_close,
};
