#ifdef __linux__
#define _GNU_SOURCE
#include <sys/resource.h>
#endif
#include "debug.h"
#include <time.h>
#include <signal.h>
#include <sys/time.h>
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
        case CLOCK_MONOTONIC_: *real = CLOCK_MONOTONIC; break;
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

static dword_t clock_nanosleep_common(dword_t clock, int_t flags, struct timespec req,
        addr_t rem_addr, bool rem_time64) {
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

    int res;
    TASK_MAY_BLOCK {
        res = nanosleep(&req, &rem);
    }
    if (res < 0)
        return errno_map();

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
    struct timespec_ req_ts;
    if (user_get(req_addr, req_ts))
        return _EFAULT;
    STRACE("clock_nanosleep(%d, %#x, {%u, %u}, %#x)", clock_id, flags, req_ts.sec, req_ts.nsec, rem_addr);
    return clock_nanosleep_common(clock_id, flags, timespec_from_guest(req_ts), rem_addr, false);
}

dword_t sys_clock_nanosleep_time64(dword_t clock_id, int_t flags, addr_t req_addr, addr_t rem_addr) {
    struct timespec64_ req_ts;
    if (user_get(req_addr, req_ts))
        return _EFAULT;
    STRACE("clock_nanosleep_time64(%d, %#x, {%lld, %lld}, %#x)", clock_id, flags,
            (long long) req_ts.sec, (long long) req_ts.nsec, rem_addr);
    return clock_nanosleep_common(clock_id, flags, timespec_from_guest64(req_ts), rem_addr, true);
}

dword_t sys_ppoll_time64(addr_t fds, dword_t nfds, addr_t timeout_addr, addr_t sigmask_addr, dword_t sigsetsize) {
    int timeout = -1;
    if (timeout_addr != 0) {
        struct timespec64_ timeout_timespec;
        if (user_get(timeout_addr, timeout_timespec))
            return _EFAULT;
        if (timeout_timespec.sec < 0 || timeout_timespec.nsec < 0 || timeout_timespec.nsec >= 1000000000)
            return _EINVAL;

        int64_t timeout_ms = timeout_timespec.sec * 1000 + timeout_timespec.nsec / 1000000;
        timeout = timeout_ms > INT_MAX ? INT_MAX : (int) timeout_ms;
    }

    sigset_t_ mask;
    bool restore_mask = false;
    if (sigmask_addr != 0) {
        if (sigsetsize != sizeof(sigset_t_))
            return _EINVAL;
        if (user_get(sigmask_addr, mask))
            return _EFAULT;
        sigmask_set_temp(mask);
        restore_mask = true;
    }

    dword_t res = sys_poll(fds, nfds, timeout);
    if (restore_mask)
        sigmask_clear_temp();
    return res;
}

dword_t sys_time(addr_t time_out) {
    dword_t now = (dword_t)time(NULL);
    if (time_out != 0)
        if (user_put(time_out, now))
            return _EFAULT;
    return now;
}

dword_t sys_stime(addr_t UNUSED(time)) {
    return _EPERM;
}

dword_t sys_clock_gettime(dword_t clock, addr_t tp) {
    STRACE("clock_gettime(%d, 0x%x)", clock, tp);

    struct timespec ts;
    if (clock == CLOCK_PROCESS_CPUTIME_ID_) {
        // FIXME this is thread usage, not process usage
        struct rusage_ rusage = rusage_get_current();
        ts.tv_sec = rusage.utime.sec;
        ts.tv_nsec = rusage.utime.usec * 1000;
    } else {
        clockid_t clock_id;
        if (clockid_to_real(clock, &clock_id)) return _EINVAL;
        int err = clock_gettime(clock_id, &ts);
        if (err < 0)
            return errno_map();
    }
    struct timespec_ t = timespec_to_guest(ts);
    
    if (user_put(tp, t))
        return _EFAULT;
    STRACE(" {%lds %ldns}", t.sec, t.nsec);
    return 0;
}

dword_t sys_clock_gettime64(dword_t clock, addr_t tp) {
    STRACE("clock_gettime64(%d, 0x%x)", clock, tp);

    struct timespec ts;
    if (clock == CLOCK_PROCESS_CPUTIME_ID_) {
        // FIXME this is thread usage, not process usage
        struct rusage_ rusage = rusage_get_current();
        ts.tv_sec = rusage.utime.sec;
        ts.tv_nsec = rusage.utime.usec * 1000;
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
    STRACE("clock_getres(%d, %#x)", clock, res_addr);
    clockid_t clock_id;
    if (clockid_to_real(clock, &clock_id)) return _EINVAL;

    struct timespec res;
    int err = clock_getres(clock_id, &res);
    if (err < 0)
        return errno_map();
    struct timespec_ t = timespec_to_guest(res);
    if (user_put(res_addr, t))
        return _EFAULT;
    return 0;
}

dword_t sys_clock_getres_time64(dword_t clock, addr_t res_addr) {
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

dword_t sys_clock_settime(dword_t UNUSED(clock), addr_t UNUSED(tp)) {
    return _EPERM;
}

dword_t sys_clock_settime64(dword_t UNUSED(clock), addr_t UNUSED(tp)) {
    return _EPERM;
}

static void itimer_notify(struct task *task) {
    struct siginfo_ info = {
        .code = SI_TIMER_,
    };
    send_signal(task, SIGALRM_, info);
}

static long itimer_set(struct tgroup *group, int which, struct timer_spec spec, struct timer_spec *old_spec) {
    if (which != ITIMER_REAL_) {
        FIXME("unimplemented setitimer %d", which);
        return _EINVAL;
    }

    if (!group->itimer) {
        struct timer *timer = timer_new(CLOCK_REALTIME, (timer_callback_t) itimer_notify, current);
        if (IS_ERR(timer))
            return PTR_ERR(timer);
        group->itimer = timer;
    }

    return timer_set(group->itimer, spec, old_spec);
}

long sys_setitimer(int_t which, addr_t new_val_addr, addr_t old_val_addr) {
    struct itimerval_ val;
    if (user_get(new_val_addr, val))
        return _EFAULT;
    STRACE("setitimer(%d, {%ds %dus, %ds %dus}, 0x%x)", which, val.value.sec, val.value.usec, val.interval.sec, val.interval.usec, old_val_addr);

    struct timer_spec spec = {
        .interval.tv_sec = val.interval.sec,
        .interval.tv_nsec = val.interval.usec * 1000,
        .value.tv_sec = val.value.sec,
        .value.tv_nsec = val.value.usec * 1000,
    };
    struct timer_spec old_spec;

    struct tgroup *group = current->group;
    lock(&group->lock, 0);
    long err = itimer_set(group, which, spec, &old_spec);
    unlock(&group->lock);
    if (err < 0)
        return err;

    if (old_val_addr != 0) {
        struct itimerval_ old_val;
        old_val.interval.sec = (dword_t)old_spec.interval.tv_sec;
        old_val.interval.usec = (dword_t)old_spec.interval.tv_nsec / 1000;
        old_val.value.sec = (dword_t)old_spec.value.tv_sec;
        old_val.value.usec = (dword_t)old_spec.value.tv_nsec / 1000;
        if (user_put(old_val_addr, old_val))
            return _EFAULT;
    }

    return 0;
}

long sys_alarm(uint_t seconds) {
    STRACE("alarm(%d)", seconds);
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
    struct timespec_ req_ts;
    if (user_get(req_addr, req_ts))
        return _EFAULT;
    STRACE("nanosleep({%d, %d}, 0x%x", req_ts.sec, req_ts.nsec, rem_addr);
    struct timespec req;
    req.tv_sec = req_ts.sec;
    req.tv_nsec = req_ts.nsec;
    struct timespec rem;
   // rem.tv_sec = 0; // Be anal and set both to zero.  -mke
    //rem.tv_nsec = 0;
    int res = 0;
    TASK_MAY_BLOCK {
        res = nanosleep(&req, &rem);
    }
    if (res < 0)
        return errno_map();
    if (rem_addr != 0) {
        struct timespec_ rem_ts;
        rem_ts.sec = (dword_t)rem.tv_sec;
        rem_ts.nsec = (dword_t)rem.tv_nsec;
        if (user_put(rem_addr, rem_ts))
            return _EFAULT;
    }
    return 0;
}

dword_t sys_times(addr_t tbuf) {
    STRACE("times(0x%x)", tbuf);
    if (tbuf) {
        struct tms_ tmp;
        struct rusage_ rusage = rusage_get_current();
        tmp.tms_utime = clock_from_timeval(rusage.utime);
        tmp.tms_stime = clock_from_timeval(rusage.stime);
        tmp.tms_cutime = tmp.tms_utime;
        tmp.tms_cstime = tmp.tms_stime;
        if (user_put(tbuf, tmp))
            return _EFAULT;
    }
    return 0;
}

dword_t sys_gettimeofday(addr_t tv, addr_t tz) {
    STRACE("gettimeofday(0x%x, 0x%x)", tv, tz);
    struct timeval timeval;
    struct timezone timezone;
    if (gettimeofday(&timeval, &timezone) < 0) {
        return errno_map();
    }
    struct timeval_ tv_;
    struct timezone_ tz_;
    tv_.sec = (dword_t)timeval.tv_sec;
    tv_.usec = (dword_t)timeval.tv_usec;
    tz_.minuteswest = timezone.tz_minuteswest;
    tz_.dsttime = timezone.tz_dsttime;
    if ((tv && user_put(tv, tv_)) || (tz && user_put(tz, tz_))) {
        return _EFAULT;
    }
    return 0;
}

dword_t sys_settimeofday(addr_t UNUSED(tv), addr_t UNUSED(tz)) {
    return _EPERM;
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
    lock(&pids_lock,0);
    struct task *thread = pid_get_task(timer->thread_pid);
    // TODO: solve pid reuse. currently we have two ways of referring to a task: pid_t_ and struct task *. pids get reused. task struct pointers get freed on exit or reap. need a third option for cases like this, like a refcount layer.
    if (thread != NULL)
        send_signal(thread, timer->signal, info);
    unlock(&pids_lock);
}

#define SIGEV_SIGNAL_ 0
#define SIGEV_NONE_ 1
#define SIGEV_THREAD_ID_ 4

int_t sys_timer_create(dword_t clock, addr_t sigevent_addr, addr_t timer_addr) {
    STRACE("timer_create(%d, %#x, %#x)", clock, sigevent_addr, timer_addr);
    clockid_t real_clockid;
    if (clockid_to_real(clock, &real_clockid))
        return _EINVAL;
    struct sigevent_ sigev;
    if (user_get(sigevent_addr, sigev))
        return _EFAULT;
    if (sigev.method != SIGEV_SIGNAL_ && sigev.method != SIGEV_NONE_ && sigev.method != SIGEV_THREAD_ID_)
        return _EINVAL;

    if (sigev.method == SIGEV_THREAD_ID_) {
        lock(&pids_lock,0);
        if (pid_get_task(sigev.tid) == NULL)
            return _EINVAL;
        unlock(&pids_lock);
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
        timer->thread_pid = group->leader->pid;
    }
    unlock(&group->lock);
    return 0;
}

static int_t sys_timer_gettime_common(dword_t timer_id, addr_t curr_value_addr, bool time64) {
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

static int_t sys_timer_settime_common(dword_t timer_id, int_t flags, addr_t new_value_addr, addr_t old_value_addr,
        bool time64) {
    STRACE("timer_settime(%d, %d, %#x, %#x)", timer_id, flags, new_value_addr, old_value_addr);
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
    return sys_timer_settime_common(timer_id, flags, new_value_addr, old_value_addr, false);
}

int_t sys_timer_settime64(dword_t timer_id, int_t flags, addr_t new_value_addr, addr_t old_value_addr) {
    return sys_timer_settime_common(timer_id, flags, new_value_addr, old_value_addr, true);
}

int_t sys_timer_gettime64(dword_t timer_id, addr_t curr_value_addr) {
    return sys_timer_gettime_common(timer_id, curr_value_addr, true);
}

int_t sys_timer_delete(dword_t timer_id) {
    STRACE("timer_delete(%d)\n", timer_id);
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

static int_t sys_timerfd_settime_common(fd_t f, int_t flags, addr_t new_value_addr, addr_t old_value_addr,
        bool time64) {
    STRACE("timerfd_settime(%d, %d, %#x, %#x)", f, flags, new_value_addr, old_value_addr);
    if (flags & ~(TIMER_ABSTIME_))
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
    return sys_timerfd_settime_common(f, flags, new_value_addr, old_value_addr, false);
}

int_t sys_timerfd_settime64(fd_t f, int_t flags, addr_t new_value_addr, addr_t old_value_addr) {
    return sys_timerfd_settime_common(f, flags, new_value_addr, old_value_addr, true);
}

static int_t sys_timerfd_gettime_common(fd_t f, addr_t curr_value_addr, bool time64) {
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
    return sys_timerfd_gettime_common(f, curr_value_addr, false);
}

int_t sys_timerfd_gettime64(fd_t f, addr_t curr_value_addr) {
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
    .read = timerfd_read,
    .poll = timerfd_poll,
    .close = timerfd_close,
};
