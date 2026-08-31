#include <stdlib.h>
#include <signal.h>
#include <time.h>
#include "util/timer.h"
#include "misc.h"
#include "debug.h"

static bool timer_warning_trace_enabled(void) {
    return false;
}

// The timer's idea of now: its own clock unless a sampler was installed.
static struct timespec timer_now(struct timer *timer) {
    if (timer->clock_now != NULL)
        return timer->clock_now(timer->clock_data);
    return timespec_now(timer->clockid);
}

void timer_set_clock_source(struct timer *timer, timer_clock_fn fn, void *data) {
    lock(&timer->lock, 0);
    timer->clock_now = fn;
    timer->clock_data = data;
    unlock(&timer->lock);
}

struct timer *timer_new(clockid_t clockid, timer_callback_t callback, void *data) {
//    assert(clockid == CLOCK_MONOTONIC || clockid == CLOCK_REALTIME);
    struct timer *timer = malloc(sizeof(struct timer));
    timer->clockid = clockid;
    timer->start = (struct timespec) {};
    timer->end = (struct timespec) {};
    timer->interval = (struct timespec) {};
    timer->callback = callback;
    timer->data = data;
    timer->active = false;
    timer->thread_running = false;
    timer->generation = 0;
    lock_init(&timer->lock, "timer_new\0");
    timer->dead = false;
    if (timer_warning_trace_enabled())
        printk("WARNING: timer_new timer=%p clockid=%d data=%p\n", (void *) timer, (int) clockid, data);
    return timer;
}

void timer_free(struct timer *timer) {
    lock(&timer->lock, 0);
    timer->active = false;
    if (timer->thread_running) {
        timer->dead = true;
        pthread_kill(timer->thread, SIGUSR1);
        unlock(&timer->lock);
    } else {
        unlock(&timer->lock);
        free(timer);
    }
}

static void *timer_thread(void *param) {
    struct timer *timer = param;
    // SIGUSR1 (used by timer_set/timer_free to interrupt our nanosleep) and its
    // backup SIGUSR2 are blocked on entry. Instantiate the thread-local storage
    // the handlers touch on this normal call stack, then unblock them, so no
    // handler has to malloc() a TLV block from async signal context.
    signal_thread_locals_init();
    sigset_t wake_sigs;
    sigemptyset(&wake_sigs);
    sigaddset(&wake_sigs, SIGUSR1);
    sigaddset(&wake_sigs, SIGUSR2); // the backup poke, see util/sync.c
    pthread_sigmask(SIG_UNBLOCK, &wake_sigs, NULL);

    lock(&timer->lock, 1);
    while (true) {
        uint64_t generation = timer->generation;
        struct timespec end = timer->end;
        struct timespec interval = timer->interval;
        struct timespec remaining = timespec_subtract(timer->end, timer_now(timer));
        while (timer->active &&
                timer->generation == generation &&
                timespec_positive(remaining)) {
            unlock(&timer->lock);
            // An effectively-infinite arm (e.g. systemd's TFD_TIMER_CANCEL_ON_SET
            // sentinel at TIME_T_MAX) yields a tv_sec near INT64_MAX; Darwin's
            // nanosleep converts to absolute mach-time nanoseconds, which
            // overflows and can return immediately -- turning this loop into a
            // busy spin. Nap in bounded chunks; the loop re-derives remaining.
            struct timespec nap = remaining;
            if (nap.tv_sec > 86400 || nap.tv_sec < 0) {
                nap.tv_sec = 86400;
                nap.tv_nsec = 0;
            }
            nanosleep(&nap, NULL);
            lock(&timer->lock, 0);
            remaining = timespec_subtract(timer->end, timer_now(timer));
        }
        if (!timer->active)
            break;
        if (timer->generation != generation)
            continue;

        // Only fire the callback for the arm we actually slept on. A later
        // arm/cancel updates the generation and should not inherit this wakeup.
        if (timespec_positive(timespec_subtract(timer->end, timer_now(timer))))
            continue;

        if (timer_warning_trace_enabled()) {
            printk("WARNING: timer_fire timer=%p generation=%llu interval=%lds.%09ld data=%p\n",
                   (void *) timer, (unsigned long long) generation,
                   (long) interval.tv_sec, interval.tv_nsec, timer->data);
        }
        // Callbacks (timerfd_callback, posix_timer_callback, itimer_notify)
        // take their own locks (e.g. the timerfd's fd->lock). Those same
        // locks are taken BEFORE timer->lock on the arming side (see
        // sys_timerfd_settime_common: fd->lock then timer_set's timer->lock),
        // so calling out while still holding timer->lock is an AB-BA
        // lock-order inversion against any arm/cancel racing on another
        // thread. Drop timer->lock across the callback and re-take it after;
        // the generation is re-checked below, so a stale-generation wakeup
        // (arm/cancel raced with us) is still discarded correctly.
        timer_callback_t callback = timer->callback;
        void *data = timer->data;
        unlock(&timer->lock);
        callback(data);
        lock(&timer->lock, 0);
        if (timer->generation != generation)
            continue;
        if (timer->active && timespec_positive(interval)) {
            struct timespec now = timer_now(timer);
            timer->start = end;
            timer->end = timespec_add(timer->start, interval);
            if (!timespec_positive(timespec_subtract(timer->end, now))) {
                // If we fell behind, coalesce missed periods instead of
                // replaying them in a tight burst. Signal-based users like
                // Xtigervnc become unusably slow when we try to "catch up"
                // every expired interval back-to-back.
                timer->start = now;
                timer->end = timespec_add(now, interval);
            }
        } else {
            break;
        }
    }
    timer->thread_running = false;
    if (timer->dead)
        free(timer);
    else
        unlock(&timer->lock);
    return NULL;
}

int timer_set(struct timer *timer, struct timer_spec spec, struct timer_spec *oldspec) {
    lock(&timer->lock, 0);
    struct timespec now = timer_now(timer);
    if (oldspec != NULL) {
        *oldspec = (struct timer_spec) {};
        if (timer->active) {
            oldspec->value = timespec_subtract(timer->end, now);
            if (!timespec_positive(oldspec->value))
                oldspec->value = (struct timespec) {};
            oldspec->interval = timer->interval;
        }
    }

    timer->generation++;
    timer->start = now;
    timer->end = timespec_add(timer->start, spec.value);
    // now + TIME_T_MAX-ish wraps negative, which would read as already
    // expired and fire the callback in a tight loop; pin it far future.
    if (spec.value.tv_sec >= 0 && timer->end.tv_sec < timer->start.tv_sec)
        timer->end = (struct timespec) {.tv_sec = INT64_MAX, .tv_nsec = 0};
    timer->interval = spec.interval;
    timer->active = !timespec_is_zero(spec.value);
    if (timer_warning_trace_enabled()) {
        printk("WARNING: timer_set timer=%p generation=%llu active=%d value=%lds.%09ld interval=%lds.%09ld now=%lds.%09ld end=%lds.%09ld\n",
               (void *) timer, (unsigned long long) timer->generation, timer->active,
               (long) spec.value.tv_sec, spec.value.tv_nsec,
               (long) spec.interval.tv_sec, spec.interval.tv_nsec,
               (long) now.tv_sec, now.tv_nsec,
               (long) timer->end.tv_sec, timer->end.tv_nsec);
    }
    if (timer->thread_running) {
        pthread_kill(timer->thread, SIGUSR1);
    } else if (timer->active) {
        timer->thread_running = true;
        // Born with the wake signals blocked so the timer thread cannot run
        // sigusr1_handler (and lazily malloc() its TLV block from async signal
        // context) before it has instantiated its thread-locals. It unblocks
        // them itself once safe. See signal_thread_locals_init.
        sigset_t wake_sigs, oldmask;
        sigemptyset(&wake_sigs);
        sigaddset(&wake_sigs, SIGUSR1);
        sigaddset(&wake_sigs, SIGUSR2); // same reasoning, see util/sync.c
        pthread_sigmask(SIG_BLOCK, &wake_sigs, &oldmask);
        pthread_create(&timer->thread, NULL, timer_thread, timer);
        pthread_detach(timer->thread);
        pthread_sigmask(SIG_SETMASK, &oldmask, NULL);
    }
    unlock(&timer->lock);
    return 0;
}

// Virtual counter for the arm64 guest's MRS CNTVCT_EL0 (see
// jit/guest-arm64/dpextra.S's mrs_cntvct): host monotonic nanoseconds,
// paired with a constant 1 GHz CNTFRQ_EL0.
uint64_t arm64_cntvct(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000000ull + (uint64_t) ts.tv_nsec;
}
