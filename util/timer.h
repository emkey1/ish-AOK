#ifndef UTIL_TIMER_H
#define UTIL_TIMER_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>
#include <assert.h>
#include "util/sync.h"

static inline struct timespec timespec_now(clockid_t clockid) {
    // Besides MONOTONIC/REALTIME, guest clock_nanosleep(TIMER_ABSTIME) and
    // POSIX/timerfd timers legitimately reach here with the CPU-time clocks
    // (CLOCK_THREAD_CPUTIME_ID / CLOCK_PROCESS_CPUTIME_ID) and BOOTTIME/RAW,
    // all of which the host clock_gettime supports. Asserting MONOTONIC/REALTIME
    // aborted the whole emulator on stress-ng --cpu-sched. Fall back to
    // MONOTONIC only if the host genuinely rejects the clock.
    struct timespec now = {0};
    if (clock_gettime(clockid, &now) != 0)
        clock_gettime(CLOCK_MONOTONIC, &now);
    return now;
}

static inline struct timespec timespec_add(struct timespec x, struct timespec y) {
    x.tv_sec += y.tv_sec;
    x.tv_nsec += y.tv_nsec;
    if (x.tv_nsec >= 1000000000) {
        x.tv_nsec -= 1000000000;
        x.tv_sec++;
    }
    return x;
}

static inline struct timespec timespec_subtract(struct timespec x, struct timespec y) {
    struct timespec result;
    if (x.tv_nsec < y.tv_nsec) {
        x.tv_sec -= 1;
        x.tv_nsec += 1000000000;
    }
    result.tv_sec = x.tv_sec - y.tv_sec;
    result.tv_nsec = x.tv_nsec - y.tv_nsec;
    return result;
}

static inline bool timespec_is_zero(struct timespec ts) {
    return ts.tv_sec == 0 && ts.tv_nsec == 0;
}

static inline bool timespec_positive(struct timespec ts) {
    return ts.tv_sec > 0 || (ts.tv_sec == 0 && ts.tv_nsec > 0);
}

static inline struct timespec timespec_normalize(struct timespec ts) {
    ts.tv_sec += ts.tv_nsec / 1000000000;
    ts.tv_nsec %= 1000000000;
    return ts;
}

typedef void (*timer_callback_t)(void *data);
// Where a timer reads "now" from. Almost every timer just calls
// timespec_now(clockid) on its own thread, which is right for the wall clocks
// and for the process CPU clock (the timer thread is in the same process). It
// is NOT right for CLOCK_THREAD_CPUTIME_ID: that clock belongs to a specific
// thread, and the timer's own thread is asleep, so its value never advances
// and the deadline never arrives. A timer on that clock supplies a sampler
// that reads the clock of the task that armed it.
typedef struct timespec (*timer_clock_fn)(void *data);

struct timer {
    clockid_t clockid;
    // NULL means timespec_now(clockid).
    timer_clock_fn clock_now;
    void *clock_data;
    struct timespec start;
    struct timespec end;
    struct timespec interval;

    bool active;
    bool thread_running;
    uint64_t generation;
    pthread_t thread;
    timer_callback_t callback;
    void *data;
    lock_t lock;

    bool dead; // set by timer_free, the thread will free the timer if this is set when it finishes
};

struct timer *timer_new(clockid_t clockid, timer_callback_t callback, void *data);
// Override where this timer reads the current time from. Must be called before
// the timer is armed.
void timer_set_clock_source(struct timer *timer, timer_clock_fn fn, void *data);
void timer_free(struct timer *timer);
// value is how long to wait until the next fire
// interval is how long after that to wait until the next fire (if non-zero)
// bizzare interface is based off setitimer, because this is going to be used
// to implement setitimer
struct timer_spec {
    struct timespec value;
    struct timespec interval;
};
int timer_set(struct timer *timer, struct timer_spec spec, struct timer_spec *oldspec);

#endif
