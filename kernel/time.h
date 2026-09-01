#ifndef TIME_H
#define TIME_H
#include "misc.h"
#include <sys/poll.h>
#include "kernel/fs.h"
#include "kernel/abi.h"

struct timespec64 {
    int64_t tv_sec;  // seconds
    long tv_nsec;    // nanoseconds
};

dword_t sys_ppoll_time64(addr_t fds, dword_t nfds, addr_t timeout_addr, addr_t sigmask_addr, dword_t sigsetsize);
dword_t sys_clock_nanosleep(dword_t clock_id, int_t flags, addr_t req_addr, addr_t rem_addr);
dword_t sys_clock_nanosleep_guest(dword_t clock_id, int_t flags, guest_addr_t req_addr, guest_addr_t rem_addr);
dword_t sys_clock_nanosleep_amd64(dword_t clock_id, int_t flags, addr_t req_addr, addr_t rem_addr);
dword_t sys_clock_nanosleep_amd64_guest(dword_t clock_id, int_t flags, guest_addr_t req_addr, guest_addr_t rem_addr);
dword_t sys_clock_nanosleep_time64(dword_t clock_id, int_t flags, addr_t req_addr, addr_t rem_addr);
dword_t sys_clock_nanosleep_time64_guest(dword_t clock_id, int_t flags, guest_addr_t req_addr, guest_addr_t rem_addr);
dword_t sys_clock_gettime64(dword_t clock, addr_t tp);
dword_t sys_clock_gettime64_guest(dword_t clock, guest_addr_t tp);
dword_t sys_clock_settime64(dword_t clock, addr_t tp);
dword_t sys_clock_adjtime(dword_t clock, addr_t tx);
dword_t sys_clock_adjtime64(dword_t clock, addr_t tx);
dword_t sys_clock_adjtime_amd64(dword_t clock, addr_t tx);
dword_t sys_clock_adjtime_amd64_guest(dword_t clock, guest_addr_t tx);
dword_t sys_time(addr_t time_out);
qword_t sys_time_guest(guest_addr_t time_out);
qword_t sys_time_amd64(addr_t time_out);
qword_t sys_time_amd64_guest(guest_addr_t time_out);
dword_t sys_stime(addr_t time);
#define CLOCK_REALTIME_ 0
#define CLOCK_MONOTONIC_ 1
#define CLOCK_PROCESS_CPUTIME_ID_ 2
#define CLOCK_THREAD_CPUTIME_ID_ 3
#define CLOCK_MONOTONIC_RAW_ 4
#define CLOCK_REALTIME_COARSE_ 5
#define CLOCK_MONOTONIC_COARSE_ 6
#define CLOCK_BOOTTIME_ 7
// Linux's alarm clocks. They read exactly like their non-alarm counterparts;
// the difference is only that a TIMER on one of them may wake a suspended
// system, which needs CAP_WAKE_ALARM. clock_gettime/clock_getres on them are
// unprivileged and must not fail.
#define CLOCK_REALTIME_ALARM_ 8
#define CLOCK_BOOTTIME_ALARM_ 9
#define CLOCK_TAI_ 11
dword_t sys_clock_gettime(dword_t clock, addr_t tp);
dword_t sys_clock_gettime_guest(dword_t clock, guest_addr_t tp);
dword_t sys_clock_gettime_amd64(dword_t clock, addr_t tp);
dword_t sys_clock_gettime_amd64_guest(dword_t clock, guest_addr_t tp);
dword_t sys_clock_settime(dword_t clock, addr_t tp);
dword_t sys_clock_getres(dword_t clock, addr_t res_addr);
dword_t sys_clock_getres_guest(dword_t clock, guest_addr_t res_addr);
dword_t sys_clock_getres_amd64(dword_t clock, addr_t res_addr);
dword_t sys_clock_getres_amd64_guest(dword_t clock, guest_addr_t res_addr);
dword_t sys_clock_getres_time64(dword_t clock, addr_t res_addr);
dword_t sys_clock_getres_time64_guest(dword_t clock, guest_addr_t res_addr);

struct timeval_ {
    dword_t sec;
    dword_t usec;
};
struct amd64_timeval_ {
    int64_t sec;
    int64_t usec;
};
// SIGNED, like Linux's struct __kernel_old_timespec, whose tv_sec and tv_nsec
// are both signed 32-bit longs -- and like struct timespec64_ below, which got
// this right. Unsigned, a guest passing tv_sec = -1 arrived as 4294967295, so
// timespec_is_valid()'s ts.tv_sec >= 0 check waved it through and nanosleep
// slept for 136 years instead of returning EINVAL. On the i386 guest that hung
// the whole regression run.
struct timespec_ {
    sdword_t sec;
    sdword_t nsec;
};
struct timespec64_ {
    int64_t sec;
    int64_t nsec;
};
struct timezone_ {
    dword_t minuteswest;
    dword_t dsttime;
};

static inline clock_t_ clock_from_timeval(struct timeval_ timeval) {
    return timeval.sec * 100 + timeval.usec / 10000;
}

size_t guest_timeval_size(enum guest_abi abi);
size_t guest_timespec_size(enum guest_abi abi);
int read_guest_timeval_abi(enum guest_abi abi, guest_addr_t addr, struct timeval *out);
int write_guest_timeval_abi(enum guest_abi abi, guest_addr_t addr, const struct timeval *in);
int read_guest_timespec_abi(enum guest_abi abi, guest_addr_t addr, struct timespec *out);
int write_guest_timespec_abi(enum guest_abi abi, guest_addr_t addr, const struct timespec *in);

#define ITIMER_REAL_ 0
#define ITIMER_VIRTUAL_ 1
#define ITIMER_PROF_ 2
struct itimerval_ {
    struct timeval_ interval;
    struct timeval_ value;
};

struct itimerspec_ {
    struct timespec_ interval;
    struct timespec_ value;
};
struct itimerspec64_ {
    struct timespec64_ interval;
    struct timespec64_ value;
};

struct tms_ {
    clock_t_ tms_utime;  /* user time */
    clock_t_ tms_stime;  /* system time */
    clock_t_ tms_cutime; /* user time of children */
    clock_t_ tms_cstime; /* system time of children */
};

long sys_setitimer(int_t which, addr_t new_val, addr_t old_val);
long sys_setitimer_guest(int_t which, guest_addr_t new_val, guest_addr_t old_val);
long sys_setitimer_amd64(int_t which, addr_t new_val, addr_t old_val);
long sys_setitimer_amd64_guest(int_t which, guest_addr_t new_val, guest_addr_t old_val);
long sys_getitimer(int_t which, addr_t old_val);
long sys_getitimer_guest(int_t which, guest_addr_t old_val);
long sys_getitimer_amd64(int_t which, addr_t old_val);
long sys_getitimer_amd64_guest(int_t which, guest_addr_t old_val);
long sys_alarm(uint_t seconds);
int_t sys_timer_create(dword_t clock, addr_t sigevent_addr, addr_t timer_addr);
int_t sys_timer_create_guest(dword_t clock, guest_addr_t sigevent_addr, guest_addr_t timer_addr);
int_t sys_timer_create_amd64(dword_t clock, addr_t sigevent_addr, addr_t timer_addr);
int_t sys_timer_create_amd64_guest(dword_t clock, guest_addr_t sigevent_addr, guest_addr_t timer_addr);
int_t sys_timer_gettime(dword_t timer_id, addr_t curr_value_addr);
int_t sys_timer_gettime_guest(dword_t timer_id, guest_addr_t curr_value_addr);
int_t sys_timer_getoverrun(dword_t timer_id);
int_t sys_timer_settime(dword_t timer, int_t flags, addr_t new_value_addr, addr_t old_value_addr);
int_t sys_timer_settime_guest(dword_t timer, int_t flags, guest_addr_t new_value_addr, guest_addr_t old_value_addr);
int_t sys_timer_gettime64(dword_t timer_id, addr_t curr_value_addr);
int_t sys_timer_gettime64_guest(dword_t timer_id, guest_addr_t curr_value_addr);
int_t sys_timer_settime64(dword_t timer, int_t flags, addr_t new_value_addr, addr_t old_value_addr);
int_t sys_timer_settime64_guest(dword_t timer, int_t flags, guest_addr_t new_value_addr, guest_addr_t old_value_addr);
int_t sys_timer_delete(dword_t timer_id);
fd_t sys_timerfd_create(int_t clockid, int_t flags);
int_t sys_timerfd_gettime(fd_t f, addr_t curr_value_addr);
int_t sys_timerfd_gettime_guest(fd_t f, guest_addr_t curr_value_addr);
int_t sys_timerfd_settime(fd_t f, int_t flags, addr_t new_value_addr, addr_t old_value_addr);
int_t sys_timerfd_settime_guest(fd_t f, int_t flags, guest_addr_t new_value_addr, guest_addr_t old_value_addr);
int_t sys_timerfd_gettime64(fd_t f, addr_t curr_value_addr);
int_t sys_timerfd_gettime64_guest(fd_t f, guest_addr_t curr_value_addr);
int_t sys_timerfd_settime64(fd_t f, int_t flags, addr_t new_value_addr, addr_t old_value_addr);
int_t sys_timerfd_settime64_guest(fd_t f, int_t flags, guest_addr_t new_value_addr, guest_addr_t old_value_addr);

dword_t sys_times(addr_t tbuf);
dword_t sys_times_guest(guest_addr_t tbuf);
dword_t sys_nanosleep(addr_t req, addr_t rem);
dword_t sys_nanosleep_guest(guest_addr_t req, guest_addr_t rem);
dword_t sys_nanosleep_amd64(addr_t req, addr_t rem);
dword_t sys_nanosleep_amd64_guest(guest_addr_t req, guest_addr_t rem);
dword_t sys_gettimeofday(addr_t tv, addr_t tz);
dword_t sys_gettimeofday_guest(guest_addr_t tv, guest_addr_t tz);
dword_t sys_gettimeofday_amd64(addr_t tv, addr_t tz);
dword_t sys_gettimeofday_amd64_guest(guest_addr_t tv, guest_addr_t tz);
dword_t sys_settimeofday(addr_t tv, addr_t tz);

dword_t sys_adjtimex(addr_t tx_addr);
dword_t sys_adjtimex_guest(guest_addr_t tx_addr);


#endif
