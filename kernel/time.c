#ifdef __linux__
#define _GNU_SOURCE
#include <sys/resource.h>
#endif
#include "debug.h"
#include <time.h>
#include <signal.h>
#include <sys/time.h>
#include <string.h>
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/resource.h"
#include "kernel/time.h"
#include "fs/poll.h"
#include "util/timer.h"
#include <limits.h>
#include <sys/poll.h>

static int clockid_to_real(uint_t clock, clockid_t *real) {
    switch (clock) {
        case CLOCK_REALTIME_:
        case CLOCK_REALTIME_COARSE_:
            *real = CLOCK_REALTIME; break;
        case CLOCK_MONOTONIC_:
        case CLOCK_MONOTONIC_COARSE_:
            *real = CLOCK_MONOTONIC; break;
        case CLOCK_PROCESS_CPUTIME_ID_:
            *real = CLOCK_PROCESS_CPUTIME_ID; break;
        case CLOCK_THREAD_CPUTIME_ID_:
            *real = CLOCK_THREAD_CPUTIME_ID; break;
        case CLOCK_MONOTONIC_RAW_:
#ifdef CLOCK_MONOTONIC_RAW
            *real = CLOCK_MONOTONIC_RAW; break;
#else
            *real = CLOCK_MONOTONIC; break;
#endif
        case CLOCK_BOOTTIME_:
            // CLOCK_BOOTTIME (includes suspend time) is boot-relative like
            // MONOTONIC; use the host's when available, else MONOTONIC.
#ifdef CLOCK_BOOTTIME
            *real = CLOCK_BOOTTIME; break;
#else
            *real = CLOCK_MONOTONIC; break;
#endif
        case CLOCK_TAI_:
            // CLOCK_TAI is EPOCH-based (UTC + leap-second offset), NOT
            // boot-relative — mapping it to MONOTONIC (as before) made
            // clock_gettime(CLOCK_TAI) return boot-relative seconds, decades
            // off real Linux. Map to REALTIME (off by only the ~37s leap
            // offset); iOS/macOS lack a native CLOCK_TAI anyway.
            *real = CLOCK_REALTIME; break;
        default: return _EINVAL;
    }
    return 0;
}

static struct timer_spec timer_spec_to_real(struct itimerspec_ itspec) {
    struct timer_spec spec = {
        .value.tv_sec = itspec.value.sec,
        .value.tv_nsec = itspec.value.nsec,
        .interval.tv_sec = itspec.interval.sec,
        .interval.tv_nsec = itspec.interval.nsec,
    };
    return spec;
};

static struct itimerspec_ timer_spec_from_real(struct timer_spec spec) {
    struct itimerspec_ itspec = {
        .value.sec = (dword_t)spec.value.tv_sec,
        .value.nsec = (dword_t)spec.value.tv_nsec,
        .interval.sec = (dword_t)spec.interval.tv_sec,
        .interval.nsec = (dword_t)spec.interval.tv_nsec,
    };
    return itspec;
};

static struct timer_spec timer_spec_to_real64(struct itimerspec64_ itspec) {
    struct timer_spec spec = {
        .value.tv_sec = itspec.value.sec,
        .value.tv_nsec = itspec.value.nsec,
        .interval.tv_sec = itspec.interval.sec,
        .interval.tv_nsec = itspec.interval.nsec,
    };
    return spec;
};

static struct itimerspec64_ timer_spec_from_real64(struct timer_spec spec) {
    struct itimerspec64_ itspec = {
        .value.sec = spec.value.tv_sec,
        .value.nsec = spec.value.tv_nsec,
        .interval.sec = spec.interval.tv_sec,
        .interval.nsec = spec.interval.tv_nsec,
    };
    return itspec;
};

#define TIMER_ABSTIME_ (1 << 0)
// timerfd_settime only. Linux accepts this flag for any timerfd and merely
// activates clock-set cancellation on CLOCK_REALTIME; we can't observe host
// clock discontinuities, so the timer simply never cancels (the arm itself
// must still succeed -- systemd's sd-event timechange source arms
// TFD_TIMER_ABSTIME|TFD_TIMER_CANCEL_ON_SET at TIME_T_MAX and treats EINVAL
// as fatal to manager allocation, freezing boot as PID 1).
#define TFD_TIMER_CANCEL_ON_SET_ (1 << 1)

static int timespec_is_valid(struct timespec ts) {
    return ts.tv_sec >= 0 && ts.tv_nsec >= 0 && ts.tv_nsec < 1000000000;
}

static struct timespec timespec_from_guest(struct timespec_ ts) {
    return (struct timespec) {
        .tv_sec = ts.sec,
        .tv_nsec = ts.nsec,
    };
}

static struct timespec timespec_from_guest64(struct timespec64_ ts) {
    return (struct timespec) {
        .tv_sec = ts.sec,
        .tv_nsec = ts.nsec,
    };
}

static struct timespec_ timespec_to_guest(struct timespec ts) {
    return (struct timespec_) {
        .sec = (dword_t) ts.tv_sec,
        .nsec = (dword_t) ts.tv_nsec,
    };
}

static struct timespec64_ timespec_to_guest64(struct timespec ts) {
    return (struct timespec64_) {
        .sec = ts.tv_sec,
        .nsec = ts.tv_nsec,
    };
}

size_t guest_timeval_size(enum guest_abi abi) {
    return abi == GUEST_ABI_AMD64 ? sizeof(struct amd64_timeval_) : sizeof(struct timeval_);
}

size_t guest_timespec_size(enum guest_abi abi) {
    return abi == GUEST_ABI_AMD64 ? sizeof(struct timespec64_) : sizeof(struct timespec_);
}

int read_guest_timeval_abi(enum guest_abi abi, guest_addr_t addr, struct timeval *out) {
    if (abi == GUEST_ABI_AMD64) {
        struct amd64_timeval_ guest;
        if (user_get(addr, guest))
            return _EFAULT;
        out->tv_sec = guest.sec;
        out->tv_usec = guest.usec;
    } else {
        struct timeval_ guest;
        if (user_get(addr, guest))
            return _EFAULT;
        out->tv_sec = guest.sec;
        out->tv_usec = guest.usec;
    }
    return 0;
}

int write_guest_timeval_abi(enum guest_abi abi, guest_addr_t addr, const struct timeval *in) {
    if (abi == GUEST_ABI_AMD64) {
        struct amd64_timeval_ guest = {
            .sec = in->tv_sec,
            .usec = in->tv_usec,
        };
        if (user_put(addr, guest))
            return _EFAULT;
    } else {
        struct timeval_ guest = {
            .sec = (dword_t) in->tv_sec,
            .usec = (dword_t) in->tv_usec,
        };
        if (user_put(addr, guest))
            return _EFAULT;
    }
    return 0;
}

int read_guest_timespec_abi(enum guest_abi abi, guest_addr_t addr, struct timespec *out) {
    if (abi == GUEST_ABI_AMD64) {
        struct timespec64_ guest;
        if (user_get(addr, guest))
            return _EFAULT;
        *out = timespec_from_guest64(guest);
    } else {
        struct timespec_ guest;
        if (user_get(addr, guest))
            return _EFAULT;
        *out = timespec_from_guest(guest);
    }
    return 0;
}

int write_guest_timespec_abi(enum guest_abi abi, guest_addr_t addr, const struct timespec *in) {
    if (abi == GUEST_ABI_AMD64) {
        struct timespec64_ guest = timespec_to_guest64(*in);
        if (user_put(addr, guest))
            return _EFAULT;
    } else {
        struct timespec_ guest = timespec_to_guest(*in);
        if (user_put(addr, guest))
            return _EFAULT;
    }
    return 0;
}

// How long a single host sleep slice may run before the task re-checks its own
// pending signals. Sets the worst-case latency between `kill` and the sleeping
// task noticing, and costs one extra host nanosleep per slice -- 20 wakeups a
// second per sleeping task, which is nothing next to what the emulator does per
// second of guest execution.
#define SLEEP_SLICE_NS 50000000L // 50ms
// Count of sleeping threads found deaf to their wake signal and repaired. The
// first one is logged; the rest only bump this, which a debugger can read.
_Atomic long sleep_wedged_repairs;

// Sleep `req` on the host, but in bounded slices with a pending-signal re-check
// between them, and return EINTR the moment the guest has a deliverable signal.
//
// Every other blocking site in the tree already defends itself this way --
// fs/real.c's realfs_wait_readable polls with a 100ms timeout and re-checks
// realfs_guest_signal_pending, fs/poll.c has its notify pipe, fs/sock.c does the
// equivalent -- precisely because the host SIGUSR1 poke that signal_wake_task
// sends is not reliable. This was the one blocking site left holding a bare,
// unbounded host nanosleep with no pending check at all, so a lost poke meant a
// sleeping task never acted on a pending signal, SIGKILL included: it ran the
// full sleep and exited normally, having ignored the kill entirely.
//
// While it is here, a task that finds a wake signal masked in its own host
// thread -- the state a swallowed poke leaves behind, see
// signal_thread_unwedge_wake_sigs -- repairs it, so the thread is receptive
// again for its next wait rather than staying deaf for the rest of its life.
//
// On return: 0 if the full time elapsed, or -1/EINTR with *rem set to the time
// left (relative sleeps report it; Linux does).
static int host_sleep_interruptible(struct timespec req, struct timespec *rem) {
    *rem = (struct timespec) {0};
    if (!timespec_positive(req))
        return 0;

    // Only a task can have guest signals, and only a task thread has a poke to
    // lose. Anything else (bootstrap paths, helper threads) keeps the plain
    // host sleep -- and note TASK_MAY_BLOCK writes current->io_block, so it
    // cannot appear on this branch.
    if (current == NULL)
        return nanosleep(&req, rem);

    // Whether the wake signals are ours to repair. A caller that deliberately
    // blocked one around this sleep (nothing does today, but fs/real.c's arming
    // dance shows the shape) must get its mask back untouched.
    bool own_wake_sigs = signal_thread_wake_sigs_unblocked();

    struct timespec deadline = timespec_add(timespec_now(CLOCK_MONOTONIC), req);
    for (;;) {
        // Check before sleeping as well as after: the signal may have been
        // delivered while we were between slices, and on the first pass it may
        // predate the sleep entirely.
        struct timespec left = timespec_subtract(deadline, timespec_now(CLOCK_MONOTONIC));
        if (task_wake_signal_pending()) {
            if (timespec_positive(left))
                *rem = left;
            errno = EINTR;
            return -1;
        }
        if (!timespec_positive(left))
            return 0;

        struct timespec slice = left;
        if (slice.tv_sec > 0 || slice.tv_nsec > SLEEP_SLICE_NS)
            slice = (struct timespec) {.tv_sec = 0, .tv_nsec = SLEEP_SLICE_NS};

        int res;
        TASK_MAY_BLOCK { res = nanosleep(&slice, NULL); }
        if (res < 0 && errno != EINTR) {
            // Only EINVAL is possible (a bad slice would be our own bug), but
            // don't silently spin on it.
            return -1;
        }

        // A real poke got through, or one was swallowed and left this thread
        // masked. Either way the next loop iteration reads the authoritative
        // guest state; all this does is stop the thread being deaf from here on.
        if (own_wake_sigs && signal_thread_unwedge_wake_sigs()) {
            long n = atomic_fetch_add_explicit(&sleep_wedged_repairs, 1,
                                               memory_order_relaxed) + 1;
            // Say it out loud once. This is a host-level fault, not a guest
            // one, and silently papering over it is how it stayed invisible
            // long enough to be mistaken for a signal-delivery bug in AOK.
            if (n == 1)
                printk("WARNING: host thread went deaf to its wake signal while sleeping "
                       "(pid=%d comm=%s); repaired. Further occurrences are counted, not logged.\n",
                       current->pid, current->comm);
        }

        // `left` is recomputed from the deadline every pass, so an EINTR that
        // turns out not to concern the guest costs no sleep time.
    }
}

static dword_t clock_nanosleep_common(dword_t clock, int_t flags, struct timespec req,
        guest_addr_t rem_addr, bool rem_time64) {
    clockid_t clock_id;
    if (clockid_to_real(clock, &clock_id))
        return _EINVAL;
    if (flags & ~TIMER_ABSTIME_)
        return _EINVAL;
    if (!timespec_is_valid(req))
        return _EINVAL;

    struct timespec rem = {0};
    if (flags & TIMER_ABSTIME_) {
        req = timespec_subtract(req, timespec_now(clock_id));
        if (!timespec_positive(req))
            return 0;
    }

    bool trace_short_sleep = wait_trace_enabled() && req.tv_sec >= 0 && req.tv_sec <= 2;
    if (trace_short_sleep) {
        printk("INFO: wait clock_nanosleep enter pid=%d comm=%s clock=%u flags=%#x req=%llds.%09ld rem=%#x\n",
               current != NULL ? current->pid : -1,
               current != NULL ? current->comm : "?",
               clock, flags, (long long) req.tv_sec, req.tv_nsec, rem_addr);
    }

    int res = host_sleep_interruptible(req, &rem);
    if (trace_short_sleep) {
        printk("INFO: wait clock_nanosleep exit pid=%d comm=%s res=%d rem=%llds.%09ld\n",
               current != NULL ? current->pid : -1,
               current != NULL ? current->comm : "?",
               res, (long long) rem.tv_sec, rem.tv_nsec);
    }
    if (res < 0) {
        int err = errno_map();
        // POSIX: an interrupted *relative* sleep reports the time remaining so
        // the caller (or its libc restart logic) can resume. The host nanosleep
        // already populated `rem`; hand it back. Best effort — still report
        // EINTR even if the rem store faults. (iSH previously dropped rem here,
        // so amd64 nanosleep left the caller's buffer untouched on EINTR.)
        if (err == _EINTR && rem_addr != 0 && !(flags & TIMER_ABSTIME_)) {
            if (rem_time64) {
                struct timespec64_ rem_ts = timespec_to_guest64(rem);
                (void) user_put(rem_addr, rem_ts);
            } else {
                struct timespec_ rem_ts = timespec_to_guest(rem);
                (void) user_put(rem_addr, rem_ts);
            }
        }
        return err;
    }

    if (rem_addr != 0 && !(flags & TIMER_ABSTIME_)) {
        if (rem_time64) {
            struct timespec64_ rem_ts = timespec_to_guest64(rem);
            if (user_put(rem_addr, rem_ts))
                return _EFAULT;
        } else {
            struct timespec_ rem_ts = timespec_to_guest(rem);
            if (user_put(rem_addr, rem_ts))
                return _EFAULT;
        }
    }
    return 0;
}

dword_t sys_clock_nanosleep(dword_t clock_id, int_t flags, addr_t req_addr, addr_t rem_addr) {
    return sys_clock_nanosleep_guest(clock_id, flags, req_addr, rem_addr);
}

static dword_t sys_clock_nanosleep_guest_abi(dword_t clock_id, int_t flags, guest_addr_t req_addr,
        guest_addr_t rem_addr, enum guest_abi abi) {
    struct timespec req_ts;
    if (read_guest_timespec_abi(abi, req_addr, &req_ts))
        return _EFAULT;
    STRACE("clock_nanosleep(%d, %#x, {%lld, %ld}, %#x)", clock_id, flags,
            (long long) req_ts.tv_sec, req_ts.tv_nsec, rem_addr);
    return clock_nanosleep_common(clock_id, flags, req_ts, rem_addr, abi == GUEST_ABI_AMD64);
}

dword_t sys_clock_nanosleep_guest(dword_t clock_id, int_t flags, guest_addr_t req_addr, guest_addr_t rem_addr) {
    return sys_clock_nanosleep_guest_abi(clock_id, flags, req_addr, rem_addr, GUEST_ABI_I386);
}

dword_t sys_clock_nanosleep_amd64(dword_t clock_id, int_t flags, addr_t req_addr, addr_t rem_addr) {
    return sys_clock_nanosleep_guest_abi(clock_id, flags, req_addr, rem_addr, GUEST_ABI_AMD64);
}

dword_t sys_clock_nanosleep_amd64_guest(dword_t clock_id, int_t flags, guest_addr_t req_addr, guest_addr_t rem_addr) {
    return sys_clock_nanosleep_guest_abi(clock_id, flags, req_addr, rem_addr, GUEST_ABI_AMD64);
}

dword_t sys_clock_nanosleep_time64(dword_t clock_id, int_t flags, addr_t req_addr, addr_t rem_addr) {
    return sys_clock_nanosleep_time64_guest(clock_id, flags, req_addr, rem_addr);
}

dword_t sys_clock_nanosleep_time64_guest(dword_t clock_id, int_t flags, guest_addr_t req_addr, guest_addr_t rem_addr) {
    struct timespec64_ req_ts;
    if (user_get(req_addr, req_ts))
        return _EFAULT;
    STRACE("clock_nanosleep_time64(%d, %#x, {%lld, %lld}, %#x)", clock_id, flags,
            (long long) req_ts.sec, (long long) req_ts.nsec, rem_addr);
    return clock_nanosleep_common(clock_id, flags, timespec_from_guest64(req_ts), rem_addr, true);
}

dword_t sys_ppoll_time64(addr_t fds, dword_t nfds, addr_t timeout_addr, addr_t sigmask_addr, dword_t sigsetsize) {
    struct timespec timeout_ts = {};
    const struct timespec *timeout_ptr = NULL;
    int timeout_ms = -1;
    if (timeout_addr != 0) {
        struct timespec64_ timeout_timespec;
        if (user_get(timeout_addr, timeout_timespec))
            return _EFAULT;
        if (timeout_timespec.sec < 0 || timeout_timespec.nsec < 0 || timeout_timespec.nsec >= 1000000000)
            return _EINVAL;

        int64_t timeout_ms64 = timeout_timespec.sec * 1000 + timeout_timespec.nsec / 1000000;
        timeout_ms = timeout_ms64 > INT_MAX ? INT_MAX : (int) timeout_ms64;
        timeout_ts.tv_sec = timeout_timespec.sec;
        timeout_ts.tv_nsec = timeout_timespec.nsec;
        timeout_ptr = &timeout_ts;
    }

    sigset_t_ mask;
    if (sigmask_addr != 0) {
        if (sigsetsize != sizeof(sigset_t_))
            return _EINVAL;
        if (user_get(sigmask_addr, mask))
            return _EFAULT;
        sigmask_set_temp(mask);
    }

    return sys_poll_common(fds, nfds, timeout_ptr, timeout_ms);
}

dword_t sys_time(addr_t time_out) {
    qword_t now = sys_time_guest(time_out);
    if ((dword_t) now != now)
        return _EOVERFLOW;
    return (dword_t) now;
}

qword_t sys_time_guest(guest_addr_t time_out) {
    qword_t now = (qword_t) time(NULL);
    if (time_out != 0) {
        dword_t now32 = (dword_t) now;
        if (user_put(time_out, now32))
            return (qword_t) (sqword_t) _EFAULT;
    }
    return now;
}

qword_t sys_time_amd64_guest(guest_addr_t time_out) {
    qword_t now = (qword_t) time(NULL);
    if (time_out != 0) {
        if (user_put(time_out, now))
            return (qword_t) (sqword_t) _EFAULT;
    }
    return now;
}

qword_t sys_time_amd64(addr_t time_out) {
    return sys_time_amd64_guest(time_out);
}

dword_t sys_stime(addr_t UNUSED(time)) {
    return _EPERM;
}

dword_t sys_clock_gettime(dword_t clock, addr_t tp) {
    return sys_clock_gettime_guest(clock, tp);
}

static dword_t sys_clock_gettime_guest_abi(dword_t clock, guest_addr_t tp, enum guest_abi abi) {
    STRACE("clock_gettime(%d, 0x%x)", clock, tp);

    struct timespec ts;
    if (clock == CLOCK_PROCESS_CPUTIME_ID_) {
        // Real CLOCK_PROCESS_CPUTIME_ID measures total (user+system) CPU time
        // consumed by every thread in the process, not just the caller's.
        struct rusage_ rusage = rusage_get_group();
        int64_t usec = (int64_t) rusage.utime.sec * 1000000 + rusage.utime.usec
                     + (int64_t) rusage.stime.sec * 1000000 + rusage.stime.usec;
        ts.tv_sec = usec / 1000000;
        ts.tv_nsec = (usec % 1000000) * 1000;
    } else {
        clockid_t clock_id;
        if (clockid_to_real(clock, &clock_id)) return _EINVAL;
        int err = clock_gettime(clock_id, &ts);
        if (err < 0)
            return errno_map();
    }
    if (write_guest_timespec_abi(abi, tp, &ts))
        return _EFAULT;
    return 0;
}

dword_t sys_clock_gettime_guest(dword_t clock, guest_addr_t tp) {
    return sys_clock_gettime_guest_abi(clock, tp, GUEST_ABI_I386);
}

dword_t sys_clock_gettime_amd64(dword_t clock, addr_t tp) {
    return sys_clock_gettime_guest_abi(clock, tp, GUEST_ABI_AMD64);
}

dword_t sys_clock_gettime_amd64_guest(dword_t clock, guest_addr_t tp) {
    return sys_clock_gettime_guest_abi(clock, tp, GUEST_ABI_AMD64);
}

dword_t sys_clock_gettime64(dword_t clock, addr_t tp) {
    return sys_clock_gettime64_guest(clock, tp);
}

dword_t sys_clock_gettime64_guest(dword_t clock, guest_addr_t tp) {
    STRACE("clock_gettime64(%d, 0x%x)", clock, tp);

    struct timespec ts;
    if (clock == CLOCK_PROCESS_CPUTIME_ID_) {
        // Real CLOCK_PROCESS_CPUTIME_ID measures total (user+system) CPU time
        // consumed by every thread in the process, not just the caller's.
        struct rusage_ rusage = rusage_get_group();
        int64_t usec = (int64_t) rusage.utime.sec * 1000000 + rusage.utime.usec
                     + (int64_t) rusage.stime.sec * 1000000 + rusage.stime.usec;
        ts.tv_sec = usec / 1000000;
        ts.tv_nsec = (usec % 1000000) * 1000;
    } else {
        clockid_t clock_id;
        if (clockid_to_real(clock, &clock_id)) return _EINVAL;
        int err = clock_gettime(clock_id, &ts);
        if (err < 0)
            return errno_map();
    }
    struct timespec64_ t = timespec_to_guest64(ts);
    
    if (user_put(tp, t))
        return _EFAULT;
    STRACE(" {%ld s %ld ns}", (long)t.sec, (long)t.nsec);
    return 0;
}


dword_t sys_clock_getres(dword_t clock, addr_t res_addr) {
    return sys_clock_getres_guest(clock, res_addr);
}

static dword_t sys_clock_getres_guest_abi(dword_t clock, guest_addr_t res_addr, enum guest_abi abi) {
    STRACE("clock_getres(%d, %#x)", clock, res_addr);
    clockid_t clock_id;
    if (clockid_to_real(clock, &clock_id)) return _EINVAL;

    struct timespec res;
    int err = clock_getres(clock_id, &res);
    if (err < 0)
        return errno_map();
    if (write_guest_timespec_abi(abi, res_addr, &res))
        return _EFAULT;
    return 0;
}

dword_t sys_clock_getres_guest(dword_t clock, guest_addr_t res_addr) {
    return sys_clock_getres_guest_abi(clock, res_addr, GUEST_ABI_I386);
}

dword_t sys_clock_getres_amd64(dword_t clock, addr_t res_addr) {
    return sys_clock_getres_guest_abi(clock, res_addr, GUEST_ABI_AMD64);
}

dword_t sys_clock_getres_amd64_guest(dword_t clock, guest_addr_t res_addr) {
    return sys_clock_getres_guest_abi(clock, res_addr, GUEST_ABI_AMD64);
}

dword_t sys_clock_getres_time64(dword_t clock, addr_t res_addr) {
    return sys_clock_getres_time64_guest(clock, res_addr);
}

dword_t sys_clock_getres_time64_guest(dword_t clock, guest_addr_t res_addr) {
    STRACE("clock_getres_time64(%d, %#x)", clock, res_addr);
    clockid_t clock_id;
    if (clockid_to_real(clock, &clock_id))
        return _EINVAL;

    struct timespec res;
    int err = clock_getres(clock_id, &res);
    if (err < 0)
        return errno_map();
    struct timespec64_ t = timespec_to_guest64(res);
    if (user_put(res_addr, t))
        return _EFAULT;
    return 0;
}

// iSH never has the privilege to change the host wall clock, so settable
// clocks return EPERM. But Linux distinguishes by clock id: a clock that can
// never be set (MONOTONIC, BOOTTIME, the CPU-time and COARSE clocks, ...)
// returns EINVAL regardless of privilege, and an unknown clock id also returns
// EINVAL. Only CLOCK_REALTIME is settable -> EPERM here. (Previously every
// clock returned EPERM, so clock_settime(CLOCK_MONOTONIC) was EPERM not EINVAL.)
static dword_t clock_settime_errno(dword_t clock) {
    return clock == CLOCK_REALTIME_ ? _EPERM : _EINVAL;
}

dword_t sys_clock_settime(dword_t clock, addr_t UNUSED(tp)) {
    return clock_settime_errno(clock);
}

dword_t sys_clock_settime64(dword_t clock, addr_t UNUSED(tp)) {
    return clock_settime_errno(clock);
}

// clock_adjtime / clock_adjtime64 (chronyd, ntpd). iSH runs no kernel NTP loop
// and cannot slew the host (iOS) clock, so any *adjustment* (modes != 0) is
// denied with EPERM -- the same policy as settimeofday / clock_settime. But a
// read-only query (modes == 0) succeeds with a sane, undisciplined state
// (default tick + current time, no frequency/offset correction), so chronyd
// can initialise and run in monitor-only mode rather than failing at startup.
//
// 'modes' is the first int of struct timex in every layout. The legacy timex
// (clock_adjtime, 343) uses 32-bit longs; __kernel_timex (clock_adjtime64, 405)
// uses 64-bit fields. Layouts cross-checked against the kernel uapi headers.
#define CLOCK_ADJ_TICK_USEC 10000 // default kernel tick (100 Hz); chronyd's base
#define STA_UNSYNC_ 0x0040        // clock unsynchronized (no kernel NTP discipline)
#define TIME_ERROR_ 5             // clock not synchronized (adjtimex return state)

struct timex32_ { // legacy struct timex, 32-bit long ABI (i386). 128 bytes.
    uint32_t modes;
    int32_t offset, freq, maxerror, esterror;
    int32_t status;
    int32_t constant, precision, tolerance;
    int32_t time_sec, time_usec; // struct timeval
    int32_t tick;
    int32_t ppsfreq, jitter, shift, stabil, jitcnt, calcnt, errcnt, stbcnt, tai;
    int32_t reserved[11];
};
struct timex64_ { // struct __kernel_timex, 64-bit fields. 208 bytes.
    uint32_t modes; uint32_t pad0_;
    int64_t offset, freq, maxerror, esterror;
    int32_t status; uint32_t pad1_;
    int64_t constant, precision, tolerance;
    int64_t time_sec, time_usec; // __kernel_timex_timeval
    int64_t tick;
    int64_t ppsfreq, jitter;
    int32_t shift; uint32_t pad2_;
    int64_t stabil, jitcnt, calcnt, errcnt, stbcnt;
    int32_t tai;
    int32_t reserved[11];
};

static dword_t clock_adjtime_read(guest_addr_t tx, bool time64) {
    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
        now.tv_sec = 0;
        now.tv_nsec = 0;
    }
    // iSH runs no kernel NTP loop, so the clock is undisciplined: report the
    // default tick and STA_UNSYNC, and return TIME_ERROR -- exactly what real
    // Linux returns for an unsynchronized clock (verified against the mint
    // oracle: ret=5, status=0x40, tick=default). This is still a successful
    // syscall (ret >= 0, no errno); chronyd reads it fine in monitor mode.
    if (time64) {
        struct timex64_ t = {0};
        t.tick = CLOCK_ADJ_TICK_USEC;
        t.status = STA_UNSYNC_;
        t.time_sec = now.tv_sec;
        t.time_usec = now.tv_nsec / 1000;
        if (user_put(tx, t))
            return _EFAULT;
    } else {
        struct timex32_ t = {0};
        t.tick = CLOCK_ADJ_TICK_USEC;
        t.status = STA_UNSYNC_;
        t.time_sec = (int32_t) now.tv_sec;
        t.time_usec = (int32_t) (now.tv_nsec / 1000);
        if (user_put(tx, t))
            return _EFAULT;
    }
    return TIME_ERROR_;
}

dword_t sys_clock_adjtime(dword_t clock, addr_t tx) { // i386 343 (legacy timex)
    if (clock != CLOCK_REALTIME_)
        return _EINVAL;
    uint32_t modes;
    if (user_get(tx, modes)) // 'modes' is the first u32 of the timex
        return _EFAULT;
    if (modes != 0)
        return _EPERM; // can't slew the iOS clock -> caller stays monitor-only
    return clock_adjtime_read(tx, false);
}

dword_t sys_clock_adjtime64(dword_t clock, addr_t tx) { // i386 405 (_time64)
    if (clock != CLOCK_REALTIME_)
        return _EINVAL;
    uint32_t modes;
    if (user_get(tx, modes))
        return _EFAULT;
    if (modes != 0)
        return _EPERM;
    return clock_adjtime_read(tx, true);
}

// amd64 clock_adjtime (305) table fallback. The real handler is the _guest
// variant below, dispatched via handle_amd64_native_memory_syscall with the
// full-width (48-bit) timex pointer; the legacy marshaller would truncate it.
// This entry only has to be non-NULL so the dispatcher reaches the native
// case; if that case were ever removed, EPERM is the safe answer (the legacy
// pointer is truncated, so no user_put here).
dword_t sys_clock_adjtime_amd64(dword_t clock, addr_t UNUSED(tx)) {
    return clock == CLOCK_REALTIME_ ? _EPERM : _EINVAL;
}

// amd64 clock_adjtime (305) real handler: full-width guest pointer, 64-bit
// __kernel_timex layout. modes==0 -> undisciplined read-state (TIME_ERROR +
// STA_UNSYNC), modes!=0 -> EPERM. Enables amd64 chronyd monitor-only mode.
dword_t sys_clock_adjtime_amd64_guest(dword_t clock, guest_addr_t tx) {
    if (clock != CLOCK_REALTIME_)
        return _EINVAL;
    uint32_t modes;
    if (user_get(tx, modes))
        return _EFAULT;
    if (modes != 0)
        return _EPERM;
    return clock_adjtime_read(tx, true);
}

static bool time_warning_trace_enabled(void) {
    return false;
}

// The timer carries the TGROUP, not the arming task. ITIMER_REAL is
// process-directed on Linux: any eligible thread may take SIGALRM, and the
// timer outlives whichever thread happened to arm it. Carrying a struct task
// meant the signal went only to that thread's private queue -- so it was lost
// if that thread had it blocked, and the callback dereferenced a FREED task if
// that thread exited while the timer was still armed. The group is safe to
// hold: exit_tgroup frees these timers when the group dies.
static void itimer_notify(struct tgroup *group) {
    struct siginfo_ info = {
        .code = SI_TIMER_,
    };
    send_signal_to_group(group, SIGALRM_, info);
}

// ITIMER_VIRTUAL/PROF: neither has a native CPU-time clock this codebase's
// timer subsystem can wait on (util/timer.h's struct timer only supports
// CLOCK_MONOTONIC/CLOCK_REALTIME), so a CLOCK_MONOTONIC sampler ticks at a
// fixed period, comparing accumulated CPU time (via rusage_get_group_of,
// summed across the whole thread group like real Linux's VIRTUAL/PROF
// clocks) against a deadline, firing SIGVTALRM/SIGPROF and rearming from
// the interval like a real CPU-time timer would. This trades exact
// delivery timing for zero added cost on the syscall/context-switch hot
// path -- only processes that actually call setitimer(VIRTUAL/PROF) pay
// for the sampler thread, and even then only a fixed, coarse tick rate.
#define ITIMER_VPROF_SAMPLE_MS 20

static struct timespec cpu_time_now_of(struct tgroup *group, bool include_system) {
    struct rusage_ rusage = rusage_get_group_of(group);
    long usec = rusage.utime.usec + (include_system ? rusage.stime.usec : 0);
    struct timespec ts = {
        .tv_sec = rusage.utime.sec + (include_system ? rusage.stime.sec : 0),
        .tv_nsec = usec * 1000,
    };
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_nsec -= 1000000000;
        ts.tv_sec++;
    }
    return ts;
}

// Must be called with group->lock held.
static bool itimer_vprof_maybe_fire(struct cpu_itimer_state *state, struct timespec cpu_now) {
    if (!state->armed)
        return false;
    struct timespec remaining = timespec_subtract(state->deadline, cpu_now);
    if (timespec_positive(remaining))
        return false;
    if (timespec_positive(state->interval))
        state->deadline = timespec_add(cpu_now, state->interval);
    else
        state->armed = false;
    return true;
}

// Same as itimer_notify: the group, not a thread. SIGVTALRM/SIGPROF are
// process-directed too.
static void itimer_vprof_sampler_notify(void *data) {
    struct tgroup *group = data;

    struct timespec cpu_user = cpu_time_now_of(group, false);
    struct timespec cpu_total = cpu_time_now_of(group, true);

    lock(&group->lock, 0);
    bool fire_virtual = itimer_vprof_maybe_fire(&group->itimer_virtual, cpu_user);
    bool fire_prof = itimer_vprof_maybe_fire(&group->itimer_prof, cpu_total);
    unlock(&group->lock);

    struct siginfo_ info = { .code = SI_TIMER_ };
    if (fire_virtual)
        send_signal_to_group(group, SIGVTALRM_, info);
    if (fire_prof)
        send_signal_to_group(group, SIGPROF_, info);
}

// Must be called with group->lock held (matches itimer_set's caller).
// Called with group->lock held (matches itimer_set's callers). Drop it
// around cpu_time_now_of, which takes group->lock itself via
// rusage_get_group_of -- otherwise reentrant on the same (non-recursive)
// lock. The narrow window this opens (another thread's concurrent setitimer
// on the same process seeing state from just before/after this one) is the
// same "drop the lock across a nested lock-taking call" pattern already used
// elsewhere in this codebase (e.g. mm_release around inodes_lock).
static long itimer_vprof_set(struct tgroup *group, int which, struct timer_spec spec, struct timer_spec *old_spec) {
    struct cpu_itimer_state *state = which == ITIMER_VIRTUAL_ ? &group->itimer_virtual : &group->itimer_prof;
    unlock(&group->lock);
    struct timespec cpu_now = cpu_time_now_of(group, which == ITIMER_PROF_);
    lock(&group->lock, 0);

    if (old_spec != NULL) {
        *old_spec = (struct timer_spec) {};
        if (state->armed) {
            struct timespec remaining = timespec_subtract(state->deadline, cpu_now);
            old_spec->value = timespec_positive(remaining) ? remaining : (struct timespec) {};
            old_spec->interval = state->interval;
        }
    }

    if (timespec_is_zero(spec.value)) {
        state->armed = false;
        return 0;
    }

    state->armed = true;
    state->deadline = timespec_add(cpu_now, spec.value);
    state->interval = spec.interval;

    if (group->itimer_vprof_sampler == NULL) {
        struct timer *sampler = timer_new(CLOCK_MONOTONIC, itimer_vprof_sampler_notify, group);
        if (IS_ERR(sampler))
            return PTR_ERR(sampler);
        group->itimer_vprof_sampler = sampler;
    }
    // (Re-)arm the sampler's own recurring tick; harmless if already
    // running. Left running for the group's lifetime once started rather
    // than paused when both VIRTUAL and PROF are disarmed -- see the
    // struct field comment on itimer_vprof_sampler in kernel/task.h.
    struct timer_spec sample_spec = {
        .value = {.tv_nsec = ITIMER_VPROF_SAMPLE_MS * 1000000},
        .interval = {.tv_nsec = ITIMER_VPROF_SAMPLE_MS * 1000000},
    };
    timer_set(group->itimer_vprof_sampler, sample_spec, NULL);
    return 0;
}

static long itimer_set(struct tgroup *group, int which, struct timer_spec spec, struct timer_spec *old_spec) {
    if (which == ITIMER_VIRTUAL_ || which == ITIMER_PROF_)
        return itimer_vprof_set(group, which, spec, old_spec);
    if (which != ITIMER_REAL_)
        return _EINVAL;

    if (!group->itimer) {
        struct timer *timer = timer_new(CLOCK_REALTIME, (timer_callback_t) itimer_notify, group);
        if (IS_ERR(timer))
            return PTR_ERR(timer);
        group->itimer = timer;
        if (time_warning_trace_enabled())
            printk("WARNING: itimer_create pid=%d tgid=%d comm=%s timer=%p which=%d\n",
                   current->pid, current->tgid, current->comm, (void *) timer, which);
    }

    if (time_warning_trace_enabled())
        printk("WARNING: itimer_set pid=%d tgid=%d comm=%s which=%d value=%lds.%09ld interval=%lds.%09ld timer=%p\n",
               current->pid, current->tgid, current->comm, which,
               (long) spec.value.tv_sec, spec.value.tv_nsec,
               (long) spec.interval.tv_sec, spec.interval.tv_nsec,
               (void *) group->itimer);

    return timer_set(group->itimer, spec, old_spec);
}

struct amd64_itimerval_ {
    struct amd64_timeval_ interval;
    struct amd64_timeval_ value;
};

long sys_setitimer(int_t which, addr_t new_val_addr, addr_t old_val_addr) {
    return sys_setitimer_guest(which, new_val_addr, old_val_addr);
}

static long sys_setitimer_guest_abi(int_t which, guest_addr_t new_val_addr, guest_addr_t old_val_addr,
        enum guest_abi abi) {
    struct timer_spec spec = {};
    if (abi == GUEST_ABI_AMD64) {
        struct amd64_itimerval_ val;
        if (user_get(new_val_addr, val))
            return _EFAULT;
        STRACE("setitimer(%d, {%llds %lldus, %llds %lldus}, %#llx)", which,
                (long long) val.value.sec, (long long) val.value.usec,
                (long long) val.interval.sec, (long long) val.interval.usec,
                (unsigned long long) old_val_addr);
        spec = (struct timer_spec) {
            .interval.tv_sec = val.interval.sec,
            .interval.tv_nsec = val.interval.usec * 1000,
            .value.tv_sec = val.value.sec,
            .value.tv_nsec = val.value.usec * 1000,
        };
    } else {
        struct itimerval_ val;
        if (user_get(new_val_addr, val))
            return _EFAULT;
        STRACE("setitimer(%d, {%ds %dus, %ds %dus}, %#llx)", which,
                val.value.sec, val.value.usec, val.interval.sec, val.interval.usec,
                (unsigned long long) old_val_addr);
        spec = (struct timer_spec) {
            .interval.tv_sec = val.interval.sec,
            .interval.tv_nsec = val.interval.usec * 1000,
            .value.tv_sec = val.value.sec,
            .value.tv_nsec = val.value.usec * 1000,
        };
    }
    // Linux rejects tv_usec >= 1e6 and negative seconds with EINVAL. We copied
    // them straight through, so a denormalized value left the deadline
    // permanently in the past and the timer thread spun a host core at 100%
    // for the timer's life -- and a negative one fired immediately and
    // repeatedly. timespec_is_valid already existed for clock_nanosleep; the
    // timer paths simply never called it. usec has been scaled to nsec above,
    // so the nsec bound is the usec bound.
    if (!timespec_is_valid(spec.value) || !timespec_is_valid(spec.interval))
        return _EINVAL;
    struct timer_spec old_spec;

    struct tgroup *group = current->group;
    lock(&group->lock, 0);
    long err = itimer_set(group, which, spec, &old_spec);
    unlock(&group->lock);
    if (err < 0)
        return err;

    if (old_val_addr != 0) {
        if (abi == GUEST_ABI_AMD64) {
            struct amd64_itimerval_ old_val = {
                .interval.sec = old_spec.interval.tv_sec,
                .interval.usec = old_spec.interval.tv_nsec / 1000,
                .value.sec = old_spec.value.tv_sec,
                .value.usec = old_spec.value.tv_nsec / 1000,
            };
            if (user_put(old_val_addr, old_val))
                return _EFAULT;
        } else {
            struct itimerval_ old_val = {
                .interval.sec = (dword_t) old_spec.interval.tv_sec,
                .interval.usec = (dword_t) old_spec.interval.tv_nsec / 1000,
                .value.sec = (dword_t) old_spec.value.tv_sec,
                .value.usec = (dword_t) old_spec.value.tv_nsec / 1000,
            };
            if (user_put(old_val_addr, old_val))
                return _EFAULT;
        }
    }

    return 0;
}

long sys_setitimer_guest(int_t which, guest_addr_t new_val_addr, guest_addr_t old_val_addr) {
    return sys_setitimer_guest_abi(which, new_val_addr, old_val_addr, GUEST_ABI_I386);
}

long sys_setitimer_amd64(int_t which, addr_t new_val_addr, addr_t old_val_addr) {
    return sys_setitimer_guest_abi(which, new_val_addr, old_val_addr, GUEST_ABI_AMD64);
}

long sys_setitimer_amd64_guest(int_t which, guest_addr_t new_val_addr, guest_addr_t old_val_addr) {
    return sys_setitimer_guest_abi(which, new_val_addr, old_val_addr, GUEST_ABI_AMD64);
}

// getitimer: report the interval timer currently armed for `which`. An
// invalid `which` is EINVAL. (getitimer was entirely unwired before: i386
// #105 / amd64 #36 raised SIGSYS, so any program polling its interval timer
// crashed.)
static long sys_getitimer_guest_abi(int_t which, guest_addr_t old_val_addr, enum guest_abi abi) {
    STRACE("getitimer(%d, %#llx)", which, (unsigned long long) old_val_addr);
    if (which != ITIMER_REAL_ && which != ITIMER_VIRTUAL_ && which != ITIMER_PROF_)
        return _EINVAL;

    struct timer_spec spec = {};
    struct tgroup *group = current->group;
    // Computed up front (not under group->lock below): cpu_time_now_of takes
    // group->lock itself via rusage_get_group_of.
    struct timespec cpu_now = {};
    if (which == ITIMER_VIRTUAL_ || which == ITIMER_PROF_)
        cpu_now = cpu_time_now_of(group, which == ITIMER_PROF_);

    lock(&group->lock, 0);
    if (which == ITIMER_REAL_ && group->itimer != NULL) {
        struct timer *timer = group->itimer;
        lock(&timer->lock, 0);
        spec.interval = timer->interval;
        if (timer->active) {
            struct timespec remaining = timespec_subtract(timer->end,
                timespec_now(timer->clockid));
            if (timespec_positive(remaining))
                spec.value = remaining;
        }
        unlock(&timer->lock);
    } else if (which == ITIMER_VIRTUAL_ || which == ITIMER_PROF_) {
        struct cpu_itimer_state *state = which == ITIMER_VIRTUAL_ ? &group->itimer_virtual : &group->itimer_prof;
        if (state->armed) {
            struct timespec remaining = timespec_subtract(state->deadline, cpu_now);
            spec.interval = state->interval;
            if (timespec_positive(remaining))
                spec.value = remaining;
        }
    }
    unlock(&group->lock);

    if (old_val_addr != 0) {
        if (abi == GUEST_ABI_AMD64) {
            struct amd64_itimerval_ val = {
                .interval.sec = spec.interval.tv_sec,
                .interval.usec = spec.interval.tv_nsec / 1000,
                .value.sec = spec.value.tv_sec,
                .value.usec = spec.value.tv_nsec / 1000,
            };
            if (user_put(old_val_addr, val))
                return _EFAULT;
        } else {
            struct itimerval_ val = {
                .interval.sec = (dword_t) spec.interval.tv_sec,
                .interval.usec = (dword_t) (spec.interval.tv_nsec / 1000),
                .value.sec = (dword_t) spec.value.tv_sec,
                .value.usec = (dword_t) (spec.value.tv_nsec / 1000),
            };
            if (user_put(old_val_addr, val))
                return _EFAULT;
        }
    }
    return 0;
}

long sys_getitimer(int_t which, addr_t old_val_addr) {
    return sys_getitimer_guest_abi(which, old_val_addr, GUEST_ABI_I386);
}

long sys_getitimer_guest(int_t which, guest_addr_t old_val_addr) {
    return sys_getitimer_guest_abi(which, old_val_addr, GUEST_ABI_I386);
}

long sys_getitimer_amd64(int_t which, addr_t old_val_addr) {
    return sys_getitimer_guest_abi(which, old_val_addr, GUEST_ABI_AMD64);
}

long sys_getitimer_amd64_guest(int_t which, guest_addr_t old_val_addr) {
    return sys_getitimer_guest_abi(which, old_val_addr, GUEST_ABI_AMD64);
}

long sys_alarm(uint_t seconds) {
    STRACE("alarm(%d)", seconds);
    if (time_warning_trace_enabled())
        printk("WARNING: alarm pid=%d tgid=%d comm=%s seconds=%u\n",
               current->pid, current->tgid, current->comm, seconds);
    struct timer_spec spec = {
        .value.tv_sec = seconds,
    };
    struct timer_spec old_spec;

    struct tgroup *group = current->group;
    lock(&group->lock, 0);
    long err = itimer_set(group, ITIMER_REAL_, spec, &old_spec);
    unlock(&group->lock);
    if (err < 0)
        return err;

    // Round up, and make sure to not return 0 if old_spec is > 0
    seconds = (dword_t)old_spec.value.tv_sec;
    if (old_spec.value.tv_nsec >= 500000000)
        seconds++;
    if (seconds == 0 && !timespec_is_zero(old_spec.value))
        seconds = 1;
    return seconds;
}

dword_t sys_nanosleep(addr_t req_addr, addr_t rem_addr) {
    return sys_nanosleep_guest(req_addr, rem_addr);
}

static dword_t sys_nanosleep_guest_abi(guest_addr_t req_addr, guest_addr_t rem_addr, enum guest_abi abi) {
    struct timespec req_ts;
    if (read_guest_timespec_abi(abi, req_addr, &req_ts))
        return _EFAULT;
    STRACE("nanosleep({%lld, %ld}, 0x%x", (long long) req_ts.tv_sec, req_ts.tv_nsec, rem_addr);
    bool trace_short_sleep = wait_trace_enabled() && req_ts.tv_sec >= 0 && req_ts.tv_sec <= 2;
    if (trace_short_sleep) {
        printk("INFO: wait nanosleep enter pid=%d comm=%s req=%llds.%09ld rem=%#x\n",
               current != NULL ? current->pid : -1,
               current != NULL ? current->comm : "?",
               (long long) req_ts.tv_sec, req_ts.tv_nsec, rem_addr);
    }
    struct timespec rem;
    int res = host_sleep_interruptible(req_ts, &rem);
    if (trace_short_sleep) {
        printk("INFO: wait nanosleep exit pid=%d comm=%s res=%d rem=%llds.%09ld\n",
               current != NULL ? current->pid : -1,
               current != NULL ? current->comm : "?",
               res, (long long) rem.tv_sec, rem.tv_nsec);
    }
    if (res < 0) {
        int err = errno_map();
        // On EINTR report the remaining time (Linux does); best effort.
        if (err == _EINTR && rem_addr != 0)
            (void) write_guest_timespec_abi(abi, rem_addr, &rem);
        return err;
    }
    if (rem_addr != 0 && write_guest_timespec_abi(abi, rem_addr, &rem))
        return _EFAULT;
    return 0;
}

dword_t sys_nanosleep_guest(guest_addr_t req_addr, guest_addr_t rem_addr) {
    return sys_nanosleep_guest_abi(req_addr, rem_addr, GUEST_ABI_I386);
}

dword_t sys_nanosleep_amd64(addr_t req_addr, addr_t rem_addr) {
    return sys_nanosleep_guest_abi(req_addr, rem_addr, GUEST_ABI_AMD64);
}

dword_t sys_nanosleep_amd64_guest(guest_addr_t req_addr, guest_addr_t rem_addr) {
    return sys_nanosleep_guest_abi(req_addr, rem_addr, GUEST_ABI_AMD64);
}

dword_t sys_times_guest(guest_addr_t tbuf) {
    STRACE("times(0x%x)", tbuf);
    if (tbuf) {
        struct rusage_ rusage = rusage_get_current();
        clock_t_ utime = clock_from_timeval(rusage.utime);
        clock_t_ stime = clock_from_timeval(rusage.stime);
        if (current->abi == GUEST_ABI_AMD64) {
            // amd64 struct tms has 64-bit (long) fields, vs the 32-bit i386 layout.
            struct amd64_tms { qword_t utime, stime, cutime, cstime; } tmp = {
                .utime = utime, .stime = stime, .cutime = utime, .cstime = stime,
            };
            if (user_put(tbuf, tmp))
                return _EFAULT;
        } else {
            struct tms_ tmp;
            tmp.tms_utime = utime;
            tmp.tms_stime = stime;
            tmp.tms_cutime = utime;
            tmp.tms_cstime = stime;
            if (user_put(tbuf, tmp))
                return _EFAULT;
        }
    }
    return 0;
}

dword_t sys_times(addr_t tbuf) {
    return sys_times_guest(tbuf);
}

dword_t sys_gettimeofday(addr_t tv, addr_t tz) {
    STRACE("gettimeofday(0x%x, 0x%x)", tv, tz);
    struct timeval timeval;
    struct timezone timezone;
    if (gettimeofday(&timeval, &timezone) < 0) {
        return errno_map();
    }
    struct timezone_ tz_;
    tz_.minuteswest = timezone.tz_minuteswest;
    tz_.dsttime = timezone.tz_dsttime;
    if ((tv && write_guest_timeval_abi(GUEST_ABI_I386, tv, &timeval)) || (tz && user_put(tz, tz_))) {
        return _EFAULT;
    }
    return 0;
}

dword_t sys_gettimeofday_guest(guest_addr_t tv, guest_addr_t tz) {
    return sys_gettimeofday(tv, tz);
}

dword_t sys_gettimeofday_amd64_guest(guest_addr_t tv, guest_addr_t tz) {
    STRACE("gettimeofday(0x%x, 0x%x)", tv, tz);
    struct timeval timeval;
    struct timezone timezone;
    if (gettimeofday(&timeval, &timezone) < 0) {
        return errno_map();
    }
    struct timezone_ tz_;
    tz_.minuteswest = timezone.tz_minuteswest;
    tz_.dsttime = timezone.tz_dsttime;
    if ((tv && write_guest_timeval_abi(GUEST_ABI_AMD64, tv, &timeval)) || (tz && user_put(tz, tz_))) {
        return _EFAULT;
    }
    return 0;
}

dword_t sys_gettimeofday_amd64(addr_t tv, addr_t tz) {
    return sys_gettimeofday_amd64_guest(tv, tz);
}

dword_t sys_settimeofday(addr_t UNUSED(tv), addr_t UNUSED(tz)) {
    return _EPERM;
}

dword_t sys_adjtimex(addr_t tx_addr) {
    return sys_adjtimex_guest(tx_addr);
}

dword_t sys_adjtimex_guest(guest_addr_t tx_addr) {
    uint32_t modes;
    if (user_get(tx_addr, modes))
        return _EFAULT;
    if (modes != 0)
        return _EPERM;
    return clock_adjtime_read(tx_addr, guest_abi_is_64bit(current->abi));
}

static void posix_timer_callback(struct posix_timer *timer) {
    if (timer->tgroup == NULL)
        return;
    struct siginfo_ info = {
        .code = SI_TIMER_,
        .timer.timer = timer->timer_id,
        .timer.overrun = 0,
        .timer.value = timer->sig_value,
    };
    struct task *thread = NULL;
    if (timer->thread_pid != 0) {
        thread = pid_get_task_ref(timer->thread_pid);
    } else if (timer->tgroup->leader != NULL) {
        // SIGEV_SIGNAL targets the process, so fall back to the thread-group leader.
        thread = timer->tgroup->leader;
        task_ref_cnt_mod(thread, 1);
    }
    if (time_warning_trace_enabled())
        printk("WARNING: posix_timer_callback timer_id=%d signal=%d thread_pid=%d target_pid=%d found=%d\n",
               timer->timer_id, timer->signal, timer->thread_pid,
               thread != NULL ? thread->pid : 0, thread != NULL);
    // TODO: solve pid reuse. currently we have two ways of referring to a task: pid_t_ and struct task *. pids get reused. task struct pointers get freed on exit or reap. need a third option for cases like this, like a refcount layer.
    if (thread != NULL) {
        send_signal(thread, timer->signal, info);
        task_ref_cnt_mod(thread, -1);
    }
}

#define SIGEV_SIGNAL_ 0
#define SIGEV_NONE_ 1
#define SIGEV_THREAD_ID_ 4

struct i386_sigevent_marshaled {
    union i386_sigval_ value;
    int_t signo;
    int_t method;
    pid_t_ tid;
};

struct amd64_sigevent_marshaled {
    union sigval_ value;
    int_t signo;
    int_t method;
    pid_t_ tid;
    dword_t __pad;
};

int_t sys_timer_create(dword_t clock, addr_t sigevent_addr, addr_t timer_addr) {
    return sys_timer_create_guest(clock, sigevent_addr, timer_addr);
}

static int_t sys_timer_create_guest_abi(dword_t clock, guest_addr_t sigevent_addr, guest_addr_t timer_addr,
        enum guest_abi abi) {
    STRACE("timer_create(%d, %#x, %#x)", clock, sigevent_addr, timer_addr);
    if (time_warning_trace_enabled())
        printk("WARNING: timer_create pid=%d tgid=%d comm=%s clock=%u sigevent=%#x timer_addr=%#x\n",
               current->pid, current->tgid, current->comm, clock, sigevent_addr, timer_addr);
    clockid_t real_clockid;
    if (clockid_to_real(clock, &real_clockid))
        return _EINVAL;
    struct sigevent_ sigev = {};
    if (abi == GUEST_ABI_AMD64) {
        struct amd64_sigevent_marshaled user_sigev;
        if (user_get(sigevent_addr, user_sigev))
            return _EFAULT;
        sigev = (struct sigevent_) {
            .value = user_sigev.value,
            .signo = user_sigev.signo,
            .method = user_sigev.method,
            .tid = user_sigev.tid,
        };
    } else {
        struct i386_sigevent_marshaled user_sigev;
        if (user_get(sigevent_addr, user_sigev))
            return _EFAULT;
        dword_t raw_value = 0;
        memcpy(&raw_value, &user_sigev.value, sizeof(raw_value));
        sigev = (struct sigevent_) {
            .value.sv_ptr = raw_value,
            .signo = user_sigev.signo,
            .method = user_sigev.method,
            .tid = user_sigev.tid,
        };
    }
    if (sigev.method != SIGEV_SIGNAL_ && sigev.method != SIGEV_NONE_ && sigev.method != SIGEV_THREAD_ID_)
        return _EINVAL;
    // Linux rejects a signo outside 1..64 here. We stored it unchecked, so a
    // guest could create a timer carrying signo 100 with three ordinary
    // syscalls and no privilege; the FIRST expiry then tripped
    // assert(sig >= 1 && sig < NUM_SIGS) in sig_mask and aborted the host
    // process, destroying the whole guest and every process in it.
    if ((sigev.method == SIGEV_SIGNAL_ || sigev.method == SIGEV_THREAD_ID_) &&
            (sigev.signo < 1 || sigev.signo >= NUM_SIGS))
        return _EINVAL;

    if (sigev.method == SIGEV_THREAD_ID_) {
        struct task *target = pid_get_task_ref(sigev.tid);
        if (target == NULL) {
            return _EINVAL;
        }
        task_ref_cnt_mod(target, -1);
    }

    struct tgroup *group = current->group;
    lock(&group->lock, 0);
    unsigned timer_id;
    for (timer_id = 0; timer_id < TIMERS_MAX; timer_id++) {
        if (group->posix_timers[timer_id].timer == NULL)
            break;
    }
    if (timer_id >= TIMERS_MAX) {
        unlock(&group->lock);
        return _ENOMEM;
    }
    if (user_put(timer_addr, timer_id)) {
        unlock(&group->lock);
        return _EFAULT;
    }

    struct posix_timer *timer = &group->posix_timers[timer_id];
    timer->timer_id = timer_id;
    timer->timer = timer_new(real_clockid, (timer_callback_t) posix_timer_callback, timer);
    timer->signal = sigev.signo;
    timer->sig_value = sigev.value;
    timer->tgroup = NULL;
    if (sigev.method == SIGEV_SIGNAL_) {
        timer->tgroup = group;
        timer->thread_pid = 0;
    } else if (sigev.method == SIGEV_THREAD_ID_) {
        timer->tgroup = group;
        timer->thread_pid = sigev.tid;
    }
    unlock(&group->lock);
    return 0;
}

int_t sys_timer_create_guest(dword_t clock, guest_addr_t sigevent_addr, guest_addr_t timer_addr) {
    return sys_timer_create_guest_abi(clock, sigevent_addr, timer_addr, GUEST_ABI_I386);
}

int_t sys_timer_create_amd64(dword_t clock, addr_t sigevent_addr, addr_t timer_addr) {
    return sys_timer_create_guest_abi(clock, sigevent_addr, timer_addr, GUEST_ABI_AMD64);
}

int_t sys_timer_create_amd64_guest(dword_t clock, guest_addr_t sigevent_addr, guest_addr_t timer_addr) {
    return sys_timer_create_guest_abi(clock, sigevent_addr, timer_addr, GUEST_ABI_AMD64);
}

static int_t sys_timer_gettime_common(dword_t timer_id, guest_addr_t curr_value_addr, bool time64) {
    STRACE("timer_gettime(%d, %#x)", timer_id, curr_value_addr);
    if (timer_id >= TIMERS_MAX)
        return _EINVAL;

    lock(&current->group->lock, 0);
    struct posix_timer *timer = &current->group->posix_timers[timer_id];
    if (timer->timer == NULL) {
        unlock(&current->group->lock);
        return _EINVAL;
    }

    struct timer_spec spec = {};
    lock(&timer->timer->lock, 0);
    spec.interval = timer->timer->interval;
    if (timer->timer->active) {
        struct timespec remaining = timespec_subtract(timer->timer->end,
            timespec_now(timer->timer->clockid));
        if (timespec_positive(remaining))
            spec.value = remaining;
    }
    unlock(&timer->timer->lock);
    unlock(&current->group->lock);

    if (!time64) {
        struct itimerspec_ value = timer_spec_from_real(spec);
        if (user_put(curr_value_addr, value))
            return _EFAULT;
    } else {
        struct itimerspec64_ value = timer_spec_from_real64(spec);
        if (user_put(curr_value_addr, value))
            return _EFAULT;
    }
    return 0;
}

int_t sys_timer_gettime(dword_t timer_id, addr_t curr_value_addr) {
    return sys_timer_gettime_guest(timer_id, curr_value_addr);
}

int_t sys_timer_gettime_guest(dword_t timer_id, guest_addr_t curr_value_addr) {
    return sys_timer_gettime_common(timer_id, curr_value_addr, false);
}

int_t sys_timer_getoverrun(dword_t timer_id) {
    if (timer_id >= TIMERS_MAX)
        return _EINVAL;

    lock(&current->group->lock, 0);
    struct posix_timer *timer = &current->group->posix_timers[timer_id];
    bool valid = timer->timer != NULL;
    unlock(&current->group->lock);
    if (!valid)
        return _EINVAL;
    return 0;
}

static int_t sys_timer_settime_common(dword_t timer_id, int_t flags, guest_addr_t new_value_addr,
        guest_addr_t old_value_addr,
        bool time64) {
    STRACE("timer_settime(%d, %d, %#x, %#x)", timer_id, flags, new_value_addr, old_value_addr);
    if (time_warning_trace_enabled())
        printk("WARNING: timer_settime pid=%d tgid=%d comm=%s timer_id=%u flags=%d new=%#x old=%#x time64=%d\n",
               current->pid, current->tgid, current->comm, timer_id, flags, new_value_addr, old_value_addr, time64);
    if (timer_id >= TIMERS_MAX)
        return _EINVAL;

    struct timer_spec spec;
    if (!time64) {
        struct itimerspec_ value;
        if (user_get(new_value_addr, value))
            return _EFAULT;
        spec = timer_spec_to_real(value);
    } else {
        struct itimerspec64_ value;
        if (user_get(new_value_addr, value))
            return _EFAULT;
        spec = timer_spec_to_real64(value);
    }
    // Same validation as setitimer above, for the same reason.
    if (!timespec_is_valid(spec.value) || !timespec_is_valid(spec.interval))
        return _EINVAL;

    lock(&current->group->lock, 0);
    struct posix_timer *timer = &current->group->posix_timers[timer_id];
    if (timer->timer == NULL) {
        unlock(&current->group->lock);
        return _EINVAL;
    }
    struct timer_spec old_spec;
    if (flags & TIMER_ABSTIME_) {
        struct timespec now = timespec_now(timer->timer->clockid);
        spec.value = timespec_subtract(spec.value, now);
    }
    int err = timer_set(timer->timer, spec, &old_spec);
    unlock(&current->group->lock);
    if (err < 0)
        return err;

    if (old_value_addr) {
        if (!time64) {
            struct itimerspec_ old_value = timer_spec_from_real(old_spec);
            if (user_put(old_value_addr, old_value))
                return _EFAULT;
        } else {
            struct itimerspec64_ old_value = timer_spec_from_real64(old_spec);
            if (user_put(old_value_addr, old_value))
                return _EFAULT;
        }
    }
    return 0;
}

int_t sys_timer_settime(dword_t timer_id, int_t flags, addr_t new_value_addr, addr_t old_value_addr) {
    return sys_timer_settime_guest(timer_id, flags, new_value_addr, old_value_addr);
}

int_t sys_timer_settime_guest(dword_t timer_id, int_t flags, guest_addr_t new_value_addr, guest_addr_t old_value_addr) {
    return sys_timer_settime_common(timer_id, flags, new_value_addr, old_value_addr, false);
}

int_t sys_timer_settime64(dword_t timer_id, int_t flags, addr_t new_value_addr, addr_t old_value_addr) {
    return sys_timer_settime64_guest(timer_id, flags, new_value_addr, old_value_addr);
}

int_t sys_timer_settime64_guest(dword_t timer_id, int_t flags, guest_addr_t new_value_addr, guest_addr_t old_value_addr) {
    return sys_timer_settime_common(timer_id, flags, new_value_addr, old_value_addr, true);
}

int_t sys_timer_gettime64(dword_t timer_id, addr_t curr_value_addr) {
    return sys_timer_gettime64_guest(timer_id, curr_value_addr);
}

int_t sys_timer_gettime64_guest(dword_t timer_id, guest_addr_t curr_value_addr) {
    return sys_timer_gettime_common(timer_id, curr_value_addr, true);
}

int_t sys_timer_delete(dword_t timer_id) {
    STRACE("timer_delete(%d)\n", timer_id);
    // The bounds check its three siblings all have (gettime, getoverrun,
    // settime). Without it the guest indexes posix_timers[] out of bounds and
    // hands timer_free() whatever lies past the array -- an attacker-chosen
    // pointer, since adjacent tgroup fields are guest-settable -- taking down
    // the whole emulator and every other guest process with it.
    if (timer_id >= TIMERS_MAX)
        return _EINVAL;
    lock(&current->group->lock, 0);
    struct posix_timer *timer = &current->group->posix_timers[timer_id];
    if (timer->timer == NULL) {
        unlock(&current->group->lock);
        return _EINVAL;
    }
    timer_free(timer->timer);
    timer->timer = NULL;
    unlock(&current->group->lock);
    return 0;
}

static struct fd_ops timerfd_ops;

static void timerfd_callback(struct fd *fd) {
    lock(&fd->lock, 0);
    fd->timerfd.expirations++;
    notify(&fd->cond);
    unlock(&fd->lock);
    poll_wakeup(fd, POLL_READ);
}

fd_t sys_timerfd_create(int_t clockid, int_t flags) {
    STRACE("timerfd_create(%d, %#x)", clockid, flags);
    if (time_warning_trace_enabled())
        printk("WARNING: timerfd_create pid=%d tgid=%d comm=%s clockid=%d flags=%#x\n",
               current->pid, current->tgid, current->comm, clockid, flags);
    // Linux timerfd only accepts the wall/uptime clocks; the CPU-time and
    // COARSE clocks (and unknown ids) are rejected with EINVAL even though
    // clockid_to_real would happily map some of them.
    if (clockid != CLOCK_REALTIME_ && clockid != CLOCK_MONOTONIC_ && clockid != CLOCK_BOOTTIME_)
        return _EINVAL;
    // Only TFD_NONBLOCK / TFD_CLOEXEC are valid (== O_NONBLOCK / O_CLOEXEC);
    // unknown flag bits -> EINVAL (was previously ignored).
    if (flags & ~(O_NONBLOCK_ | O_CLOEXEC_))
        return _EINVAL;
    clockid_t real_clockid;
    if (clockid_to_real(clockid, &real_clockid)) return _EINVAL;

    struct fd *fd = adhoc_fd_create(&timerfd_ops);
    if (fd == NULL)
        return _ENOMEM;

    fd->timerfd.timer = timer_new(real_clockid, (timer_callback_t) timerfd_callback, fd);
    return f_install(fd, flags);
}

static int timerfd_lookup(fd_t f, struct fd **fd_out) {
    struct fd *fd = f_get(f);
    if (fd == NULL)
        return _EBADF;
    if (fd->ops != &timerfd_ops)
        return _EINVAL;
    *fd_out = fd;
    return 0;
}

static struct timer_spec timerfd_current_spec(struct fd *fd) {
    struct timer_spec spec = {};
    lock(&fd->timerfd.timer->lock, 0);
    spec.interval = fd->timerfd.timer->interval;
    if (fd->timerfd.timer->active) {
        struct timespec remaining = timespec_subtract(fd->timerfd.timer->end,
            timespec_now(fd->timerfd.timer->clockid));
        if (timespec_positive(remaining))
            spec.value = remaining;
    }
    unlock(&fd->timerfd.timer->lock);
    return spec;
}

static int_t sys_timerfd_settime_common(fd_t f, int_t flags, guest_addr_t new_value_addr,
        guest_addr_t old_value_addr,
        bool time64) {
    STRACE("timerfd_settime(%d, %d, %#x, %#x)", f, flags, new_value_addr, old_value_addr);
    if (time_warning_trace_enabled())
        printk("WARNING: timerfd_settime pid=%d tgid=%d comm=%s fd=%d flags=%d new=%#x old=%#x time64=%d\n",
               current->pid, current->tgid, current->comm, f, flags, new_value_addr, old_value_addr, time64);
    if (flags & ~(TIMER_ABSTIME_ | TFD_TIMER_CANCEL_ON_SET_))
        return _EINVAL;
    struct fd *fd;
    int err = timerfd_lookup(f, &fd);
    if (err < 0)
        return err;
    struct timer_spec spec;
    if (!time64) {
        struct itimerspec_ value;
        if (user_get(new_value_addr, value))
            return _EFAULT;
        spec = timer_spec_to_real(value);
    } else {
        struct itimerspec64_ value;
        if (user_get(new_value_addr, value))
            return _EFAULT;
        spec = timer_spec_to_real64(value);
    }
    struct timer_spec old_spec;
    if (flags & TIMER_ABSTIME_) {
        struct timespec now = timespec_now(fd->timerfd.timer->clockid);
        spec.value = timespec_subtract(spec.value, now);
    }

    lock(&fd->lock, 0);
    err = timer_set(fd->timerfd.timer, spec, &old_spec);
    // Linux timerfd_settime resets the expiration counter on EVERY call,
    // armed or disarmed -- so a disarm (it_value = 0) also clears readiness.
    // Without this, an expiration that was never read() leaves the fd
    // permanently POLL_READ after a disarm: libwayland's shared timer-heap
    // timerfd is exactly that pattern (fire -> dispatch -> settime(0) with no
    // read when no timers remain), and the stale readiness turned labwc's
    // event loop into a 3000-cycle/s epoll_pwait/timerfd_settime spin on
    // device. Reset after timer_set so anything the old arm counted in the
    // meantime is wiped too.
    if (err >= 0)
        fd->timerfd.expirations = 0;
    unlock(&fd->lock);
    if (err < 0)
        return err;

    if (old_value_addr) {
        if (!time64) {
            struct itimerspec_ old_value = timer_spec_from_real(old_spec);
            if (user_put(old_value_addr, old_value))
                return _EFAULT;
        } else {
            struct itimerspec64_ old_value = timer_spec_from_real64(old_spec);
            if (user_put(old_value_addr, old_value))
                return _EFAULT;
        }
    }

    return 0;
}

int_t sys_timerfd_settime(fd_t f, int_t flags, addr_t new_value_addr, addr_t old_value_addr) {
    return sys_timerfd_settime_guest(f, flags, new_value_addr, old_value_addr);
}

int_t sys_timerfd_settime_guest(fd_t f, int_t flags, guest_addr_t new_value_addr, guest_addr_t old_value_addr) {
    return sys_timerfd_settime_common(f, flags, new_value_addr, old_value_addr, false);
}

int_t sys_timerfd_settime64(fd_t f, int_t flags, addr_t new_value_addr, addr_t old_value_addr) {
    return sys_timerfd_settime64_guest(f, flags, new_value_addr, old_value_addr);
}

int_t sys_timerfd_settime64_guest(fd_t f, int_t flags, guest_addr_t new_value_addr, guest_addr_t old_value_addr) {
    return sys_timerfd_settime_common(f, flags, new_value_addr, old_value_addr, true);
}

static int_t sys_timerfd_gettime_common(fd_t f, guest_addr_t curr_value_addr, bool time64) {
    STRACE("timerfd_gettime(%d, %#x)", f, curr_value_addr);
    struct fd *fd;
    int err = timerfd_lookup(f, &fd);
    if (err < 0)
        return err;
    struct timer_spec spec = timerfd_current_spec(fd);
    if (!time64) {
        struct itimerspec_ value = timer_spec_from_real(spec);
        if (user_put(curr_value_addr, value))
            return _EFAULT;
    } else {
        struct itimerspec64_ value = timer_spec_from_real64(spec);
        if (user_put(curr_value_addr, value))
            return _EFAULT;
    }
    return 0;
}

int_t sys_timerfd_gettime(fd_t f, addr_t curr_value_addr) {
    return sys_timerfd_gettime_guest(f, curr_value_addr);
}

int_t sys_timerfd_gettime_guest(fd_t f, guest_addr_t curr_value_addr) {
    return sys_timerfd_gettime_common(f, curr_value_addr, false);
}

int_t sys_timerfd_gettime64(fd_t f, addr_t curr_value_addr) {
    return sys_timerfd_gettime64_guest(f, curr_value_addr);
}

int_t sys_timerfd_gettime64_guest(fd_t f, guest_addr_t curr_value_addr) {
    return sys_timerfd_gettime_common(f, curr_value_addr, true);
}

static ssize_t timerfd_read(struct fd *fd, void *buf, size_t bufsize) {
    if (bufsize < sizeof(uint64_t))
        return _EINVAL;
    lock(&fd->lock, 0);
    while (fd->timerfd.expirations == 0) {
        if (fd->flags & O_NONBLOCK_) {
            unlock(&fd->lock);
            return _EAGAIN;
        }
        int err = wait_for(&fd->cond, &fd->lock, NULL);
        if (err < 0) {
            unlock(&fd->lock);
            return err;
        }
    }

    *(uint64_t *) buf = fd->timerfd.expirations;
    fd->timerfd.expirations = 0;
    unlock(&fd->lock);
    return sizeof(uint64_t);
}
static int timerfd_poll(struct fd *fd) {
    int res = 0;
    lock(&fd->lock, 0);
    if (fd->timerfd.expirations != 0)
        res |= POLL_READ;
    unlock(&fd->lock);
    return res;
}
static int timerfd_close(struct fd *fd) {
    timer_free(fd->timerfd.timer);
    return 0;
}

static struct fd_ops timerfd_ops = {
    .anon_inode_class = "timerfd",
    .read = timerfd_read,
    .poll = timerfd_poll,
    .close = timerfd_close,
};
