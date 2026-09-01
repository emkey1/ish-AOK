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

// Linux encodes a per-process or per-thread CPU clock into a NEGATIVE clockid:
//
//     MAKE_PROCESS_CPUCLOCK(pid, which) = (~(clockid_t) pid << 3) | which
//
// with the per-thread form ORing in CPUCLOCK_PERTHREAD_MASK.
// clock_getcpuclockid() and pthread_getcpuclockid() hand these out, and the C
// library validates one by calling clock_getres on it -- so rejecting them
// with EINVAL made both of those functions fail outright, and nothing could
// read another process's CPU time.
#define CPUCLOCK_PERTHREAD_MASK 4
#define CPUCLOCK_CLOCK_MASK 3
#define CPUCLOCK_MAX 3

static bool cpuclock_decode(uint_t clock, pid_t_ *pid, bool *perthread) {
    int32_t c = (int32_t) clock;
    if (c >= 0)
        return false;                       // an ordinary CLOCK_* id
    if ((c & CPUCLOCK_CLOCK_MASK) >= CPUCLOCK_MAX)
        return false;
    *pid = (pid_t_) ~(c >> 3);              // arithmetic shift, as Linux does
    *perthread = (c & CPUCLOCK_PERTHREAD_MASK) != 0;
    return true;
}

// The CPU time this clock names. Total user+system, the same thing
// CLOCK_PROCESS_CPUTIME_ID reports.
static int cpuclock_gettime(pid_t_ pid, bool perthread, struct timespec *ts) {
    // Resolve under pids_lock and take a reference, then RELEASE it before
    // reading the usage: rusage_get_group_of takes pids_lock itself and it is
    // not recursive, so holding it across the call deadlocks the caller
    // against itself -- which wedged any process that asked for a CPU clock,
    // including the one asking on its own behalf.
    // pid 0 names the CALLER, exactly as MAKE_PROCESS_CPUCLOCK(0, ...) does on
    // Linux -- and that is the form the C library actually uses.
    // clock_getcpuclockid(0) builds -6 (process) or -2 (per-thread) and then
    // validates it with clock_getres, so pid_get_task_ref(0) finding no task
    // made both come back ESRCH. Measured against Devuan 6 / glibc 2.41, where
    // clock_getcpuclockid(0) returns -6 and reads fine.
    struct task *task;
    bool borrowed = false;
    if (pid == 0) {
        task = current;
    } else {
        task = pid_get_task_ref(pid);
        borrowed = true;
    }
    if (task == NULL)
        return _ESRCH;
    struct rusage_ rusage = perthread ? rusage_get_task(task)
                                      : rusage_get_group_of(task->group);
    if (borrowed)
        task_ref_cnt_mod(task, -1);
    int64_t usec = (int64_t) rusage.utime.sec * 1000000 + rusage.utime.usec
                 + (int64_t) rusage.stime.sec * 1000000 + rusage.stime.usec;
    ts->tv_sec = usec / 1000000;
    ts->tv_nsec = (usec % 1000000) * 1000;
    return 0;
}

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
        case CLOCK_REALTIME_ALARM_:
            // Reads identically to its non-alarm counterpart -- the only
            // difference is that a TIMER on one may wake a suspended system,
            // which is a permission question handled where timers are created,
            // not here. Rejecting these made clock_gettime and clock_getres
            // fail with EINVAL, which Linux never does for any user.
            *real = CLOCK_REALTIME; break;
        case CLOCK_BOOTTIME_ALARM_:
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

// The `abi` argument selects the LAYOUT, and every caller in the tree names it
// explicitly (GUEST_ABI_I386 for the 32-bit struct, GUEST_ABI_AMD64 for the
// 64-bit one) rather than passing the guest's real ABI. Testing for AMD64 by
// name therefore worked -- right up until a new caller passed current->abi,
// which for an arm64 guest is neither, so a 64-bit timespec was read as a
// 32-bit one: tv_nsec came out of the high half of tv_sec, i.e. zero, and a
// semtimedop asked to wait 200ms timed out instantly. Ask whether the ABI is
// 64-bit instead, which is true for the explicit AMD64 callers as well.
int read_guest_timespec_abi(enum guest_abi abi, guest_addr_t addr, struct timespec *out) {
    if (guest_abi_is_64bit(abi)) {
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

// Same as read_guest_timespec_abi above.
int write_guest_timespec_abi(enum guest_abi abi, guest_addr_t addr, const struct timespec *in) {
    if (guest_abi_is_64bit(abi)) {
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
        // Sleeping is a voluntary context switch in the sense getrusage means:
        // the task gave up the CPU rather than being preempted. This sleep
        // does not go through wait_for (it is a host nanosleep in slices), so
        // it has to say so itself or a process whose whole life is sleep+poll
        // reports having never yielded.
        if (current != NULL)
            current->nvcsw++;
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

    // A CPU-time clock measures CPU CONSUMED, not wall time, and sleeping on
    // one has to wait for the process to actually spend it. Sleeping wall time
    // instead meant an idle process returned immediately from a request Linux
    // would never complete, and a busy one returned far too early -- exactly
    // backwards for the thing this clock exists to measure.
    //
    // There is nothing to hand the host: Darwin has no CPU-time sleep. So it
    // is a wait against our own accounting. Polled rather than event-driven,
    // because CPU time is only observable by asking -- 10ms is fine enough not
    // to overshoot meaningfully and coarse enough not to spin, and an idle
    // process simply never reaches its target, as on Linux.
    pid_t_ cpu_pid;
    bool cpu_perthread;
    bool is_cpu_clock = clock == CLOCK_PROCESS_CPUTIME_ID_ ||
        (cpuclock_decode(clock, &cpu_pid, &cpu_perthread) && !cpu_perthread);
    if (is_cpu_clock) {
        if (clock == CLOCK_PROCESS_CPUTIME_ID_)
            cpu_pid = current->pid;
        struct timespec consumed;
        int cpu_err = cpuclock_gettime(cpu_pid, false, &consumed);
        if (cpu_err < 0)
            return cpu_err;
        struct timespec target = (flags & TIMER_ABSTIME_)
            ? req : timespec_add(consumed, req);
        for (;;) {
            cpu_err = cpuclock_gettime(cpu_pid, false, &consumed);
            if (cpu_err < 0)
                return cpu_err;
            if (!timespec_positive(timespec_subtract(target, consumed)))
                return 0;
            struct timespec slice = { .tv_sec = 0, .tv_nsec = 10000000 };
            if (host_sleep_interruptible(slice, &rem) < 0) {
                int err = errno_map();
                if (err == _EINTR)
                    return signal_restart_or_eintr_nohand(err);
                return err;
            }
        }
    }

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

    // Resuming after a job-control stop: sleep out the deadline this call
    // already had rather than the full relative time again. An absolute
    // request needs nothing -- it is already a deadline.
    if (current != NULL && current->sleep_restart_valid) {
        bool usable = !(flags & TIMER_ABSTIME_);
        current->sleep_restart_valid = false;
        if (usable) {
            struct timespec left = timespec_subtract(current->sleep_restart_deadline,
                                                     timespec_now(CLOCK_MONOTONIC));
            req = timespec_positive(left) ? left : (struct timespec) {0};
        }
    }
    struct timespec sleep_deadline = timespec_add(timespec_now(CLOCK_MONOTONIC), req);
    int res = host_sleep_interruptible(req, &rem);
    if (trace_short_sleep) {
        printk("INFO: wait clock_nanosleep exit pid=%d comm=%s res=%d rem=%llds.%09ld\n",
               current != NULL ? current->pid : -1,
               current != NULL ? current->comm : "?",
               res, (long long) rem.tv_sec, rem.tv_nsec);
    }
    if (res < 0) {
        int err = errno_map();
        // ERESTARTNOHAND, exactly as for poll: a job-control stop resumes the
        // sleep transparently (carrying the deadline across), while a handler
        // actually running still gives the guest its EINTR.
        if (err == _EINTR) {
            int restarted = signal_restart_or_eintr_nohand(err);
            if (restarted == _ERESTART_NOHAND) {
                if (current != NULL && !(flags & TIMER_ABSTIME_)) {
                    current->sleep_restart_deadline = sleep_deadline;
                    current->sleep_restart_valid = true;
                }
                return (dword_t) restarted;
            }
        }
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

    // A COMPLETED sleep does not touch rmtp at all. Linux writes it only on
    // the signal-interrupted relative path (the EINTR branch above): there is
    // nothing remaining to report, and the caller's buffer is its own.
    //
    // Writing it here did two wrong things. It clobbered a buffer the caller
    // had left data in -- the usual shape is `struct timespec req = ..., rem;
    // while (nanosleep(&req, &rem)) req = rem;`, and a caller that reuses one
    // buffer for something else between sleeps lost it. Worse, it FAULTED: a
    // sleep that ran to completion returned EFAULT when rmtp was not a valid
    // pointer, which Linux never even looks at, so a caller passing a stale
    // or deliberately-invalid rmtp saw its successful sleep fail.
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
    pid_t_ cpuclock_pid;
    bool cpuclock_perthread;
    if (cpuclock_decode(clock, &cpuclock_pid, &cpuclock_perthread)) {
        int cpuclock_err = cpuclock_gettime(cpuclock_pid, cpuclock_perthread, &ts);
        if (cpuclock_err < 0)
            return cpuclock_err;
    } else if (clock == CLOCK_PROCESS_CPUTIME_ID_) {
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
    pid_t_ cpuclock_pid;
    bool cpuclock_perthread;
    if (cpuclock_decode(clock, &cpuclock_pid, &cpuclock_perthread)) {
        int cpuclock_err = cpuclock_gettime(cpuclock_pid, cpuclock_perthread, &ts);
        if (cpuclock_err < 0)
            return cpuclock_err;
    } else if (clock == CLOCK_PROCESS_CPUTIME_ID_) {
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
    struct timespec res;
    pid_t_ cpuclock_pid;
    bool cpuclock_perthread;
    if (cpuclock_decode(clock, &cpuclock_pid, &cpuclock_perthread)) {
        // This is the call the C library makes to decide whether the id it
        // just computed is usable, so it must answer for a live pid and fail
        // for a dead one. Linux reports 1ns for the CPU clocks.
        struct timespec ignored;
        int cpuclock_err = cpuclock_gettime(cpuclock_pid, cpuclock_perthread, &ignored);
        if (cpuclock_err < 0)
            return cpuclock_err;
        res.tv_sec = 0;
        res.tv_nsec = 1;
        // A NULL res is legal: clock_getres(2) says "if res is NULL, the
        // resolution is not returned", and the call becomes a pure "is this
        // clock usable?" question. That is precisely how the C library uses
        // it -- glibc's clock_getcpuclockid computes the dynamic id and then
        // validates it with clock_getres(id, NULL) -- so faulting on the NULL
        // made clock_getcpuclockid fail with EFAULT on every glibc guest.
        // Verified against Devuan 6 / glibc 2.41, where clock_getres(-6, NULL)
        // returns 0.
        if (res_addr != 0 && write_guest_timespec_abi(abi, res_addr, &res))
            return _EFAULT;
        return 0;
    }
    clockid_t clock_id;
    if (clockid_to_real(clock, &clock_id)) return _EINVAL;

    int err = clock_getres(clock_id, &res);
    if (err < 0)
        return errno_map();
    if (res_addr != 0 && write_guest_timespec_abi(abi, res_addr, &res))
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
    // This is a SECOND body, not a wrapper, so both of the fixes in
    // sys_clock_getres_guest_abi have to exist here too -- and on i386 this is
    // the one that runs. musl there defines only the *_time64 numbers for the
    // clock calls, so every i386 clock_getres arrives at this entry and not
    // the one above.
    struct timespec res;
    pid_t_ cpuclock_pid;
    bool cpuclock_perthread;
    if (cpuclock_decode(clock, &cpuclock_pid, &cpuclock_perthread)) {
        struct timespec ignored;
        int cpuclock_err = cpuclock_gettime(cpuclock_pid, cpuclock_perthread, &ignored);
        if (cpuclock_err < 0)
            return cpuclock_err;
        res.tv_sec = 0;
        res.tv_nsec = 1;
    } else {
        clockid_t clock_id;
        if (clockid_to_real(clock, &clock_id))
            return _EINVAL;
        int err = clock_getres(clock_id, &res);
        if (err < 0)
            return errno_map();
    }
    // NULL means "do not return the resolution", which makes the call a pure
    // validity check -- see the comment in sys_clock_getres_guest_abi.
    if (res_addr == 0)
        return 0;
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

// Linux checks in this order: copy the timespec in (EFAULT), range-check it
// (EINVAL), and only then apply the capability check (EPERM). AOK refused
// first and never looked, so a caller probing with a deliberately-bad pointer
// -- or one whose struct was simply wrong -- was told it lacked permission
// for a call it had got wrong, and went looking for the wrong problem.
static dword_t clock_settime_common(dword_t clock, guest_addr_t tp, bool time64) {
    if (clock != CLOCK_REALTIME_ && clock != CLOCK_REALTIME_ALARM_)
        return _EINVAL;
    // The layout is the CALLER's: clock_settime carries a 32-bit timespec on a
    // 32-bit guest and a 64-bit one on a 64-bit guest, while clock_settime64
    // is always the wide form. Reading the narrow struct out of a 64-bit
    // guest's buffer is how this first reported EFAULT for a well-formed
    // call.
    struct timespec ts;
    if (time64) {
        struct timespec64_ guest_ts;
        if (user_get(tp, guest_ts))
            return _EFAULT;
        ts.tv_sec = (time_t) guest_ts.sec;
        ts.tv_nsec = (long) guest_ts.nsec;
    } else if (read_guest_timespec_abi(current->abi, tp, &ts)) {
        return _EFAULT;
    }
    if (!timespec_is_valid(ts))
        return _EINVAL;
    // iSH cannot move the host (iOS) clock, so a well-formed request is
    // refused the way an unprivileged one is on Linux.
    return _EPERM;
}

dword_t sys_clock_settime(dword_t clock, addr_t tp) {
    if (clock != CLOCK_REALTIME_ && clock != CLOCK_REALTIME_ALARM_)
        return clock_settime_errno(clock);
    return clock_settime_common(clock, tp, false);
}

dword_t sys_clock_settime64(dword_t clock, addr_t tp) {
    if (clock != CLOCK_REALTIME_ && clock != CLOCK_REALTIME_ALARM_)
        return clock_settime_errno(clock);
    return clock_settime_common(clock, tp, true);
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
        // The interval is reported whether or not the timer is armed: Linux's
        // set_cpu_itimer/get_cpu_itimer read and write it->incr
        // unconditionally, so `setitimer(ITIMER_PROF, {interval=7s, value=0})`
        // followed by getitimer reports 7s. Gating it on `armed` reported 0
        // and lost what the caller had just set.
        old_spec->interval = state->interval;
        if (state->armed) {
            struct timespec remaining = timespec_subtract(state->deadline, cpu_now);
            old_spec->value = timespec_positive(remaining) ? remaining : (struct timespec) {};
        }
    }

    // Stored before the disarm check, for the same reason.
    state->interval = spec.interval;
    if (timespec_is_zero(spec.value)) {
        state->armed = false;
        return 0;
    }

    state->armed = true;
    state->deadline = timespec_add(cpu_now, spec.value);

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

    // Disarming ITIMER_REAL discards the interval with it. Linux's
    // do_setitimer sets it_real_incr only when it_value is nonzero and clears
    // it otherwise, so getitimer after a disarm reports 0/0 -- while
    // ITIMER_VIRTUAL and ITIMER_PROF keep theirs (see itimer_vprof_set).
    // AOK had the two exactly the wrong way round.
    if (timespec_is_zero(spec.value))
        spec.interval = (struct timespec) {};
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
        // Unconditionally, matching get_cpu_itimer -- see itimer_vprof_set.
        spec.interval = state->interval;
        if (state->armed) {
            struct timespec remaining = timespec_subtract(state->deadline, cpu_now);
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
    // The same validation clock_nanosleep_common already does, which plain
    // nanosleep never had: a tv_nsec outside [0, 999999999] or a negative
    // tv_sec is EINVAL and nothing is slept. Accepting them returned success
    // without sleeping, so a caller that had computed a bad duration -- which
    // is the whole reason the check exists -- was told its sleep happened.
    if (!timespec_is_valid(req_ts))
        return _EINVAL;
    bool trace_short_sleep = wait_trace_enabled() && req_ts.tv_sec >= 0 && req_ts.tv_sec <= 2;
    if (trace_short_sleep) {
        printk("INFO: wait nanosleep enter pid=%d comm=%s req=%llds.%09ld rem=%#x\n",
               current != NULL ? current->pid : -1,
               current != NULL ? current->comm : "?",
               (long long) req_ts.tv_sec, req_ts.tv_nsec, rem_addr);
    }
    struct timespec rem;
    // Same job-control-stop handling as clock_nanosleep_common; nanosleep() is
    // always relative, so the carried deadline needs no ABSTIME check.
    if (current != NULL && current->sleep_restart_valid) {
        current->sleep_restart_valid = false;
        struct timespec left = timespec_subtract(current->sleep_restart_deadline,
                                                 timespec_now(CLOCK_MONOTONIC));
        req_ts = timespec_positive(left) ? left : (struct timespec) {0};
    }
    struct timespec sleep_deadline = timespec_add(timespec_now(CLOCK_MONOTONIC), req_ts);
    int res = host_sleep_interruptible(req_ts, &rem);
    if (trace_short_sleep) {
        printk("INFO: wait nanosleep exit pid=%d comm=%s res=%d rem=%llds.%09ld\n",
               current != NULL ? current->pid : -1,
               current != NULL ? current->comm : "?",
               res, (long long) rem.tv_sec, rem.tv_nsec);
    }
    if (res < 0) {
        int err = errno_map();
        // ERESTARTNOHAND: a job-control stop resumes the sleep transparently,
        // carrying the deadline across; a handler running still gives EINTR.
        if (err == _EINTR) {
            int restarted = signal_restart_or_eintr_nohand(err);
            if (restarted == _ERESTART_NOHAND) {
                if (current != NULL) {
                    current->sleep_restart_deadline = sleep_deadline;
                    current->sleep_restart_valid = true;
                }
                return (dword_t) restarted;
            }
        }
        // On EINTR report the remaining time (Linux does); best effort.
        if (err == _EINTR && rem_addr != 0)
            (void) write_guest_timespec_abi(abi, rem_addr, &rem);
        return err;
    }
    // A completed sleep does not touch rmtp -- see the same rule and the same
    // reasoning in sys_clock_nanosleep_common. This is the second copy: plain
    // nanosleep and clock_nanosleep each have their own body.
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
        struct rusage_ rusage = rusage_get_group();
        clock_t_ utime = clock_from_timeval(rusage.utime);
        clock_t_ stime = clock_from_timeval(rusage.stime);
        // tms_cutime/tms_cstime are the REAPED CHILDREN's accumulated time,
        // not a second copy of our own -- which is what they were, so a shell
        // or a build tool asking how long its children took was handed its own
        // CPU time instead. The accounting already exists and is already right
        // (getrusage(RUSAGE_CHILDREN) reports it); it just was not being read.
        lock(&current->group->lock, 0);
        struct rusage_ children = current->group->children_rusage;
        unlock(&current->group->lock);
        clock_t_ cutime = clock_from_timeval(children.utime);
        clock_t_ cstime = clock_from_timeval(children.stime);
        // struct tms is four `clock_t` (long) fields, so its layout follows the
        // ABI's word size -- 64-bit on arm64 and riscv64 just as much as on
        // amd64. Testing only for amd64 wrote the 32-bit layout to an arm64
        // guest, which then read four 64-bit fields out of sixteen bytes:
        // tms_utime happened to survive whenever tms_stime was 0, and
        // tms_cutime came back as garbage.
        if (guest_abi_is_64bit(current->abi)) {
            struct amd64_tms { qword_t utime, stime, cutime, cstime; } tmp = {
                .utime = utime, .stime = stime, .cutime = cutime, .cstime = cstime,
            };
            if (user_put(tbuf, tmp))
                return _EFAULT;
        } else {
            struct tms_ tmp;
            tmp.tms_utime = utime;
            tmp.tms_stime = stime;
            tmp.tms_cutime = cutime;
            tmp.tms_cstime = cstime;
            if (user_put(tbuf, tmp))
                return _EFAULT;
        }
    }
    // Linux returns the number of clock ticks since an arbitrary point in the
    // past -- in practice boot -- and callers use the DIFFERENCE between two
    // calls to measure elapsed time. Returning a constant 0 made every such
    // measurement come out as zero, which is what `time` in a shell without a
    // builtin, and any benchmark using times(), reports.
    struct timespec up = timespec_now(CLOCK_MONOTONIC);
    return (dword_t) (up.tv_sec * 100 + up.tv_nsec / 10000000);
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
    // Linux fills tz from its own sys_tz, which is {0, 0} unless a
    // long-obsolete settimeofday set it; tz_dsttime is documented as always
    // 0. Darwin answers with the HOST's timezone, and passing that through
    // leaked the Mac's DST flag to the guest -- a nonzero tz_dsttime is a
    // state no Linux produces, and the few programs that still read this
    // field treat it as one.
    struct timezone_ tz_;
    tz_.minuteswest = 0;
    tz_.dsttime = 0;
    (void) timezone;
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
    // Linux fills tz from its own sys_tz, which is {0, 0} unless a
    // long-obsolete settimeofday set it; tz_dsttime is documented as always
    // 0. Darwin answers with the HOST's timezone, and passing that through
    // leaked the Mac's DST flag to the guest -- a nonzero tz_dsttime is a
    // state no Linux produces, and the few programs that still read this
    // field treat it as one.
    struct timezone_ tz_;
    tz_.minuteswest = 0;
    tz_.dsttime = 0;
    (void) timezone;
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

// Read the owning thread's CPU clock, by pid rather than by pointer: the timer
// outlives nothing, but the thread can exit while the timer is armed, and a
// stored task pointer would be dangling by the time this ran. A thread that is
// gone reports its clock as far in the future, so an armed timer stops
// counting down rather than firing spuriously -- Linux disarms such a timer,
// and never firing is the same observable outcome.
static struct timespec posix_timer_thread_cpu_now(void *data) {
    struct posix_timer *timer = data;
    struct task *task = pid_get_task_ref(timer->cpu_clock_pid);
    if (task == NULL) {
        struct timespec forever = { .tv_sec = INT64_MAX / 2, .tv_nsec = 0 };
        return forever;
    }
    unsigned long utime = 0, stime = 0;
    task_thread_cpu_time(task, &utime, &stime);
    task_ref_cnt_mod(task, -1);
    // task_thread_cpu_time counts in jiffies at USER_HZ = 100.
    unsigned long ticks = utime + stime;
    struct timespec now = {
        .tv_sec = (time_t) (ticks / 100),
        .tv_nsec = (long) ((ticks % 100) * 10000000L),
    };
    return now;
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
        // If the last signal from this timer is still queued, this expiration
        // is an overrun, not a second signal. See
        // signal_timer_count_overrun.
        int overrun = signal_timer_count_overrun(thread, timer->signal, timer->timer_id);
        if (overrun >= 0) {
            timer->last_overrun = overrun;
        } else {
            timer->last_overrun = 0;
            send_signal(thread, timer->signal, info);
        }
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
    // The C library does not hand timer_create the plain CLOCK_*_CPUTIME_ID
    // constant -- it hands over the DYNAMIC cpu-clock encoding, the same one
    // clock_getcpuclockid returns. glibc turns CLOCK_PROCESS_CPUTIME_ID into
    // 0xfffffffa (-6) and CLOCK_THREAD_CPUTIME_ID into 0xfffffffe (-2) before
    // the syscall; strace on Devuan shows exactly that. clockid_to_real knows
    // only the constants, so every CPU-time timer a glibc program created came
    // back EINVAL -- while the same call through musl, which passes the
    // constant, worked. That is why the Alpine test roots never saw it and the
    // Devuan guest did.
    clockid_t real_clockid;
    pid_t_ cpu_pid;
    bool cpu_perthread;
    if (cpuclock_decode(clock, &cpu_pid, &cpu_perthread)) {
        // Only the caller's own clocks. Linux allows a timer on another
        // process's CPU clock only for a process you already control, and
        // nothing in the guest asks for it; EINVAL is what it answers when the
        // clock names someone else.
        if (cpu_pid != 0 && cpu_pid != current->tgid && cpu_pid != current->pid)
            return _EINVAL;
        real_clockid = cpu_perthread ? CLOCK_THREAD_CPUTIME_ID
                                     : CLOCK_PROCESS_CPUTIME_ID;
    } else if (clockid_to_real(clock, &real_clockid)) {
        return _EINVAL;
    }
    struct sigevent_ sigev = {};
    // timer_create(2): "If evp is NULL ... the default sigevent is
    // sigev_notify = SIGEV_SIGNAL, sigev_signo = SIGALRM, and sigev_value.
    // sival_int = timer ID." AOK read the struct unconditionally, so a NULL
    // evp faulted and the call returned EFAULT -- glibc's timer_create(clock,
    // NULL, &t) and every program that takes the documented default got an
    // error Linux never returns. sival_int is filled in below, once the timer
    // id is known.
    bool default_sigevent = sigevent_addr == 0;
    if (default_sigevent) {
        sigev.method = SIGEV_SIGNAL_;
        sigev.signo = SIGALRM_;
    } else if (abi == GUEST_ABI_AMD64) {
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
        // What Linux reports when a process exhausts its own timer bound
        // (RLIMIT_SIGPENDING), and what callers check for. ENOMEM says the
        // kernel is out of memory, which it is not, and sends a caller down
        // an allocation-failure path instead of a back-off-and-retry one.
        unlock(&group->lock);
        return _EAGAIN;
    }
    if (user_put(timer_addr, timer_id)) {
        unlock(&group->lock);
        return _EFAULT;
    }

    struct posix_timer *timer = &group->posix_timers[timer_id];
    timer->timer_id = timer_id;
    // The documented default carries the timer id as sival_int.
    if (default_sigevent)
        sigev.value.sv_ptr = timer_id;
    timer->timer = timer_new(real_clockid, (timer_callback_t) posix_timer_callback, timer);
    // CLOCK_THREAD_CPUTIME_ID belongs to ONE thread, and the timer runs on its
    // own -- which is asleep, so its thread clock never advances and the
    // deadline never arrives. The timer was created and armed and reported
    // success and then simply never fired, which is the worst of the three
    // possible answers. Sample the clock of the thread that created it
    // instead. (The process CPU clock needs none of this: the timer thread is
    // in the same process, so reading it on any thread gives the same number.)
    if (real_clockid == CLOCK_THREAD_CPUTIME_ID) {
        timer->cpu_clock_pid = current->pid;
        timer_set_clock_source(timer->timer, posix_timer_thread_cpu_now, timer);
    }
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
    int_t overrun = timer->last_overrun;
    unlock(&current->group->lock);
    if (!valid)
        return _EINVAL;
    // Hardcoded 0 before, which told a program running a periodic timer that
    // it had never missed a period no matter how far behind it was. See the
    // comment on posix_timer.last_overrun.
    return overrun;
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
    if (clockid == CLOCK_REALTIME_ALARM_ || clockid == CLOCK_BOOTTIME_ALARM_) {
        // The alarm clocks exist for timerfd, but arming one may wake a
        // suspended system, so Linux requires CAP_WAKE_ALARM and answers EPERM
        // without it -- never EINVAL, which is what a caller reads as "this
        // kernel has no such clock" and gives up on entirely.
        if (!current_capable(CAP_WAKE_ALARM_))
            return _EPERM;
    } else if (clockid != CLOCK_REALTIME_ && clockid != CLOCK_MONOTONIC_ &&
               clockid != CLOCK_BOOTTIME_) {
        return _EINVAL;
    }
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
    // Out-of-range nanoseconds are EINVAL, as everywhere else that takes a
    // timespec. Accepting them let a caller arm a timer for a deadline it
    // never asked for -- the value was carried through arithmetic that
    // silently normalised it -- instead of being told its struct was wrong.
    if (!timespec_is_valid(spec.value) || !timespec_is_valid(spec.interval))
        return _EINVAL;

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
