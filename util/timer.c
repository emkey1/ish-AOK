#include <errno.h>
#include <stdlib.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#if __APPLE__
#include <sys/event.h>
#else
#include <poll.h>
#include <stdint.h>
#include <sys/eventfd.h>
#endif
#include "util/timer.h"
#include "misc.h"
#include "debug.h"

#if __APPLE__
int host_nanosleep_precise(struct timespec req, long slack_ns) {
    if (!timespec_positive(req))
        return 0;
    // A kqueue of its own for each sleep: about a microsecond to make and
    // close, less than nanosleep's own floor, and nothing to leak when the
    // thread exits. A kqueue that cannot be had falls back to the coalesced
    // sleep, which is late but still a sleep.
    int kq = kqueue();
    if (kq < 0)
        return nanosleep(&req, NULL);
    // Callers bound their naps (a day at most), so this cannot overflow.
    int64_t ns = (int64_t) req.tv_sec * 1000000000 + req.tv_nsec;
    struct kevent64_s timer, fired;
    EV_SET64(&timer, 1, EVFILT_TIMER, EV_ADD | EV_ONESHOT,
             NOTE_NSECONDS | NOTE_CRITICAL | (slack_ns > 0 ? NOTE_LEEWAY : 0),
             ns, 0, 0, slack_ns > 0 ? (uint64_t) slack_ns : 0);
    int n = kevent64(kq, &timer, 1, &fired, 1, 0, NULL);
    int err = errno;
    close(kq);
    if (n < 0 && err == EINTR) {
        errno = EINTR;
        return -1;
    }
    // Refused (an EV_ERROR receipt, or the call failed outright): sleep the
    // ordinary way rather than not at all.
    if (n < 0 || (n == 1 && (fired.flags & EV_ERROR)))
        return nanosleep(&req, NULL);
    return 0;
}
#else
// Linux hosts already give a sleep 50us of slack and no more.
int host_nanosleep_precise(struct timespec req, long UNUSED(slack_ns)) {
    if (!timespec_positive(req))
        return 0;
    return nanosleep(&req, NULL);
}
#endif

// ---- how timer_set and timer_free reach the timer's thread -----------------
//
// The thread sleeps until the next expiry, and an arming that moves the
// deadline, or a timer_free, has to cut that sleep short. That used to be
// pthread_kill(SIGUSR1), which is lossy: a poke that landed after the thread
// dropped timer->lock but before it blocked ran its handler and was gone, and
// the thread then slept out its OLD nap -- as long as a day. A guest that
// re-armed a 30s itimer to 5ms, the two calls 0-60us apart, lost 4 to 10 of
// 488 SIGALRMs on every test root (tests/manual/timer_rearm_wake.c).
//
// Now each thread waits on a descriptor of its own, which timer_set and
// timer_free raise under timer->lock and which stays raised until the thread
// has seen it. A wake raised in that same window is found as the sleep
// starts, and the sleep ends at once. The descriptor is made with the thread
// (timer_set, before pthread_create) and closed by the thread as it exits,
// under the lock, so no one raises one that is gone. timer->wake_fd is -1
// while there is no thread -- and for a thread that could not be given one,
// the host being out of descriptors, which then sleeps in slices of
// TIMER_UNWOKEN_NAP_NS and looks again after each: late by up to that, but
// never on to an old deadline.
//
// Darwin: a kqueue holding an EVFILT_USER event with EV_CLEAR, which the sleep
// waits on together with its critical EVFILT_TIMER (host_nanosleep_precise
// says why only such a timer is on time). kevent64 throughout: a kqueue that
// has been given kevent() calls as well refuses kevent64() with EINVAL.
// Linux: an eventfd, and ppoll with the nap as its timeout.
#define TIMER_UNWOKEN_NAP_NS 10000000L // 10ms

// A thread with no wake descriptor: a slice at most, then look again.
static void timer_unwoken_nap(struct timespec nap) {
    if (nap.tv_sec > 0 || nap.tv_nsec > TIMER_UNWOKEN_NAP_NS)
        nap = (struct timespec) {.tv_sec = 0, .tv_nsec = TIMER_UNWOKEN_NAP_NS};
    host_nanosleep_precise(nap, 0);
}

#if __APPLE__
#define TIMER_WAKE_IDENT 1
#define TIMER_NAP_IDENT 2

static int timer_wake_open(void) {
    int kq = kqueue();
    if (kq < 0)
        return -1;
    struct kevent64_s wake;
    EV_SET64(&wake, TIMER_WAKE_IDENT, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, 0, 0, 0);
    if (kevent64(kq, &wake, 1, NULL, 0, 0, NULL) < 0) {
        close(kq);
        return -1;
    }
    return kq;
}

static void timer_wake_raise(int kq) {
    if (kq < 0)
        return;
    struct kevent64_s wake;
    EV_SET64(&wake, TIMER_WAKE_IDENT, EVFILT_USER, 0, NOTE_TRIGGER, 0, 0, 0, 0);
    kevent64(kq, &wake, 1, NULL, 0, 0, NULL);
}

// Sleep `nap`, or less if the wake is raised. Returns early on a signal too;
// the caller recomputes what is left either way.
static void timer_wake_wait(int kq, struct timespec nap) {
    if (kq < 0) {
        timer_unwoken_nap(nap);
        return;
    }
    // The caller bounds its naps (a day at most), so this cannot overflow.
    int64_t ns = (int64_t) nap.tv_sec * 1000000000 + nap.tv_nsec;
    // EV_ADD on the ident a woken sleep left behind re-arms that timer rather
    // than adding a second one.
    struct kevent64_s timer, events[2];
    EV_SET64(&timer, TIMER_NAP_IDENT, EVFILT_TIMER, EV_ADD | EV_ONESHOT,
             NOTE_NSECONDS | NOTE_CRITICAL, ns, 0, 0, 0);
    int n = kevent64(kq, &timer, 1, events, 2, 0, NULL);
    bool refused = n < 0 && errno != EINTR;
    for (int i = 0; i < n; i++) {
        if ((events[i].flags & EV_ERROR) && events[i].ident == TIMER_NAP_IDENT)
            refused = true;
    }
    // No critical timer to be had: wait for the wake with a plain timeout,
    // which Darwin may run late but which still ends at the wake.
    if (refused)
        kevent64(kq, NULL, 0, events, 2, 0, &nap);
}
#else
static int timer_wake_open(void) {
    return eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
}

static void timer_wake_raise(int fd) {
    if (fd < 0)
        return;
    uint64_t one = 1;
    ssize_t wrote;
    do {
        wrote = write(fd, &one, sizeof(one));
    } while (wrote < 0 && errno == EINTR);
}

static void timer_wake_wait(int fd, struct timespec nap) {
    if (fd < 0) {
        timer_unwoken_nap(nap);
        return;
    }
    struct pollfd p = {.fd = fd, .events = POLLIN};
    if (ppoll(&p, 1, &nap, NULL) > 0) {
        uint64_t count;
        (void) !read(fd, &count, sizeof(count));
    }
}
#endif

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
    // calloc, not malloc: every field this function does not name must start
    // zeroed. clock_now was added without an init here, and on iOS 15, whose
    // allocator hands back dirty memory, the first alarm() called through a
    // garbage pointer (#595).
    struct timer *timer = calloc(1, sizeof(struct timer));
    timer->clockid = clockid;
    timer->start = (struct timespec) {};
    timer->end = (struct timespec) {};
    timer->interval = (struct timespec) {};
    timer->callback = callback;
    timer->data = data;
    timer->clock_now = NULL;
    timer->clock_data = NULL;
    timer->active = false;
    timer->thread_running = false;
    timer->firing = false;
    timer->fired = 0;
    timer->generation = 0;
    timer->wake_fd = -1;
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
        timer_wake_raise(timer->wake_fd);
        unlock(&timer->lock);
    } else {
        unlock(&timer->lock);
        free(timer);
    }
}

static void *timer_thread(void *param) {
    struct timer *timer = param;
    // Born with the wake signals (SIGUSR1, SIGUSR2) blocked, and they stay
    // blocked: nothing pokes this thread with a signal, since its wake is
    // timer->wake_fd. Its thread-locals are instantiated all the same, so
    // that if another thread's host sigprocmask ever unblocks them here --
    // on Darwin that call sets every thread's mask -- a handler finds them
    // made rather than malloc()ing them from async signal context.
    signal_thread_locals_init();

    lock(&timer->lock, 1);
    while (true) {
        uint64_t generation = timer->generation;
        struct timespec end = timer->end;
        struct timespec interval = timer->interval;
        struct timespec remaining = timespec_subtract(timer->end, timer_now(timer));
        while (timer->active &&
                timer->generation == generation &&
                timespec_positive(remaining)) {
            // Read under the lock; it cannot change while this thread lives.
            // -1 (no descriptor to be had) naps in slices instead.
            int wake_fd = timer->wake_fd;
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
            // No slack: a POSIX timer, itimer or timerfd expires on time on
            // Linux. A nanosleep here fired every expiry a fifth of a period
            // late on iOS, which let a 5ms timer's expiry land just AFTER a
            // guest that slept exactly 200 periods had taken the signal it
            // should have been counted on (timer_conventions' overruns).
            // Ends early when timer_set or timer_free raise the wake, even
            // one raised since the unlock above.
            timer_wake_wait(wake_fd, nap);
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
        timer->firing = true;
        unlock(&timer->lock);
        // ISH_TEST_TIMER_FIRE_DELAY_MS=<ms>: hold every expiry that long
        // between deciding to fire and delivering, which is otherwise a window
        // of microseconds -- so that a checkpoint can be made to land inside
        // it (tests/manual/checkpoint_timers.sh's race leg; see timer_read).
        static int fire_delay_ms = -1;
        if (fire_delay_ms < 0) {
            const char *v = getenv("ISH_TEST_TIMER_FIRE_DELAY_MS");
            fire_delay_ms = v != NULL ? atoi(v) : 0;
        }
        if (fire_delay_ms > 0) {
            struct timespec hold = {.tv_sec = fire_delay_ms / 1000,
                                    .tv_nsec = (long) (fire_delay_ms % 1000) * 1000000};
            nanosleep(&hold, NULL);
        }
        callback(data);
        lock(&timer->lock, 0);
        timer->firing = false;
        timer->fired++;
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
    // Under the lock, where every raise happens, and after thread_running,
    // which is what they check: no one raises a closed descriptor, or a
    // stranger that reused its number.
    if (timer->wake_fd >= 0)
        close(timer->wake_fd);
    timer->wake_fd = -1;
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
        timer_wake_raise(timer->wake_fd);
    } else if (timer->active) {
        // The thread's wake, made before the thread so that it is there for
        // the next timer_set however soon that comes. -1 if the host has no
        // descriptor to spare, and the thread then naps in slices instead
        // (see the comment above timer_wake_open).
        timer->wake_fd = timer_wake_open();
        timer->thread_running = true;
        // Born with the wake signals blocked, which it keeps: see
        // timer_thread. So the timer thread never runs sigusr1_handler, which
        // before its thread-locals exist would malloc() them from async signal
        // context. See signal_thread_locals_init.
        sigset_t wake_sigs, oldmask;
        sigemptyset(&wake_sigs);
        sigaddset(&wake_sigs, SIGUSR1);
        sigaddset(&wake_sigs, SIGUSR2); // same reasoning, see util/sync.c
        pthread_sigmask(SIG_BLOCK, &wake_sigs, &oldmask);
        // pthread_create returns a POSITIVE errno and can genuinely fail --
        // EAGAIN at the host thread limit, which a guest reaches by making
        // enough tasks, since every one of them is a host thread too. Unchecked,
        // the two lines after it were the bug: pthread_detach was handed an
        // UNINITIALISED pthread_t, and thread_running had already been set to
        // true above, so the next timer_set would pthread_kill that same
        // garbage handle. kernel/task.c's task_start had the identical bug and
        // says so at length; this is the same fix in the other place it lives.
        //
        // Build 553 crashes with SIGABRT on exactly this stack -- guest
        // setitimer(2) -> itimer_set -> timer_set -> abort -- with no assert
        // frame, which is what calling into libpthread with a bad handle looks
        // like.
        // Above the guest's own threads (USER_INITIATED, kernel/task.c),
        // for the same reason as util/sync.c's deadline_wake_thread: made
        // without attributes it runs at DEFAULT, and guest threads keeping a
        // device busy would make every expiry late. An expiry is a signal
        // queued or a count raised, microseconds of work.
        pthread_attr_t attr;
        pthread_attr_init(&attr);
#if __APPLE__
        pthread_attr_set_qos_class_np(&attr, QOS_CLASS_USER_INTERACTIVE, 0);
#endif
        int err = pthread_create(&timer->thread, &attr, timer_thread, timer);
        pthread_attr_destroy(&attr);
        if (err == 0)
            pthread_detach(timer->thread);
        pthread_sigmask(SIG_SETMASK, &oldmask, NULL);
        if (err != 0) {
            // No thread means the timer cannot fire, so do not leave it
            // claiming to be armed and running: a later timer_set would signal
            // a thread that was never born, and getitimer would report a
            // deadline nothing is watching.
            timer->thread_running = false;
            timer->active = false;
            if (timer->wake_fd >= 0)
                close(timer->wake_fd);
            timer->wake_fd = -1;
            unlock(&timer->lock);
            return _EAGAIN;
        }
    }
    unlock(&timer->lock);
    return 0;
}

// Wait out a callback in flight. Called, and returns, with timer->lock held.
// The callback runs with the lock dropped and re-takes it to finish, which is
// why this has to let go of it to wait.
static void timer_wait_not_firing_locked(struct timer *timer) {
    while (timer->firing) {
        unlock(&timer->lock);
        struct timespec nap = {.tv_sec = 0, .tv_nsec = 100000};
        nanosleep(&nap, NULL);
        lock(&timer->lock, 0);
    }
}

uint64_t timer_settle(struct timer *timer) {
    lock(&timer->lock, 0);
    timer_wait_not_firing_locked(timer);
    uint64_t fired = timer->fired;
    unlock(&timer->lock);
    return fired;
}

bool timer_read(struct timer *timer, struct timer_spec *spec) {
    lock(&timer->lock, 0);
    // An expiry being delivered right now is neither still to come nor yet
    // delivered: its signal is not queued, its count not raised. Described
    // then, a one-shot came back disarmed with nothing in its place.
    timer_wait_not_firing_locked(timer);
    *spec = (struct timer_spec) {.interval = timer->interval};
    // A one-shot is left `active` by the thread that fires it, which then
    // exits: thread_running is what says an expiry is still to come.
    bool armed = timer->active && timer->thread_running;
    if (armed) {
        spec->value = timespec_subtract(timer->end, timer_now(timer));
        if (!timespec_positive(spec->value))
            spec->value = (struct timespec) {.tv_sec = 0, .tv_nsec = 1};
    }
    unlock(&timer->lock);
    return armed;
}

// Virtual counter for the arm64 guest's MRS CNTVCT_EL0 (see
// jit/guest-arm64/dpextra.S's mrs_cntvct): host monotonic nanoseconds,
// paired with a constant 1 GHz CNTFRQ_EL0.
uint64_t arm64_cntvct(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000000ull + (uint64_t) ts.tv_nsec;
}
