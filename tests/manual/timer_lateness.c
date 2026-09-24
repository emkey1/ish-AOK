// How late timers and sleeps wake, against how long they were.
//
//   Linux wakes a POSIX timer, itimer or timerfd on its deadline, and a
//   sleeping task within its timer slack (50us by default). AOK's host timers
//   were Darwin sleeps, which the kernel coalesces: it may run one late by a
//   quarter of the time asked for, up to 5ms, and an idle machine takes all of
//   it. Measured on an M4 iPad, a 5ms periodic timer's expiries each came
//   1.0ms after their boundaries and a 50ms timer's 5ms after, every time; a
//   50ms nanosleep woke 5ms late. On Linux 6.12 the same timers were 3-15us
//   late at any period.
//
//   That lateness is what made timer_conventions' overruns check flake on the
//   iPad: a nap of exactly 200 periods took its signal before the timer thread
//   had got round to the 200th expiry, which then arrived as a fresh signal.
//
//   Every other wait with a deadline was late the same way: FUTEX_WAIT (and so
//   every guest pthread_cond_timedwait) and sigtimedwait by a quarter of the
//   wait, poll, select and epoll_wait by 1-2ms.
//
// The signature of coalescing is lateness that GROWS with the interval, so
// that is what is checked: the median lateness of a 40ms timer (and sleep)
// against that of a 2ms one, sampled in alternating blocks so that load
// affects both alike. A loaded machine wakes everything late; only coalescing
// wakes long waits later than short ones. Expiries are watched by polling the
// pending set -- the witness is not the mechanism under test.
#define _GNU_SOURCE
#include <errno.h>
#include <linux/futex.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

static long long now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void ck_eq(const char *label, long long got, long long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-58s got=%-10lld want=%lld\n", label, got, want);
}

static void ck_le(const char *label, long long got, long long most) {
    if (got > most)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) most, 0, 0);
    test_logf("  %-58s got=%-10lld want<=%lld\n", label, got, most);
}

static int cmp_ll(const void *a, const void *b) {
    long long x = *(const long long *) a, y = *(const long long *) b;
    return x < y ? -1 : x > y;
}

static long long median(long long *v, int n) {
    qsort(v, n, sizeof *v, cmp_ll);
    return v[n / 2];
}

// Arm a periodic timer of `period` ns on the blocked SIGRTMIN, take `n`
// expiries by polling for them, and store how long after its boundary each
// was seen. Returns how many were taken, which is `n` unless the timer
// stopped firing.
static int timer_block(long long period, int n, long long *late) {
    struct sigevent sev;
    memset(&sev, 0, sizeof sev);
    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo = SIGRTMIN;
    timer_t t;
    if (timer_create(CLOCK_MONOTONIC, &sev, &t) != 0)
        return 0;
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGRTMIN);
    struct itimerspec its = {
        {period / 1000000000, period % 1000000000},
        {period / 1000000000, period % 1000000000},
    };
    long long t0 = now_ns();
    timer_settime(t, 0, &its, NULL);
    long long due = 0; // expiries accounted for so far
    int got = 0;
    long long give_up = t0 + (n + 10) * period + 2000000000LL;
    while (got < n && now_ns() < give_up) {
        sigset_t pend;
        sigpending(&pend);
        if (!sigismember(&pend, SIGRTMIN))
            continue;
        long long seen = now_ns();
        siginfo_t si;
        struct timespec zero = {0, 0};
        if (sigtimedwait(&set, &si, &zero) != SIGRTMIN)
            continue;
        due += 1 + si.si_overrun;
        late[got++] = seen - (t0 + due * period);
    }
    memset(&its, 0, sizeof its);
    timer_settime(t, 0, &its, NULL);
    timer_delete(t);
    siginfo_t drop;
    struct timespec zero = {0, 0};
    while (sigtimedwait(&set, &drop, &zero) > 0) {
    }
    return got;
}

enum wait_kind { WAIT_NANOSLEEP, WAIT_POLL, WAIT_EPOLL, WAIT_FUTEX, WAIT_SIGTIMEDWAIT };

// Wait `ns` in the way `kind` says, for something that never comes, and store
// how much longer than `ns` it took. Nonzero if the wait did not end as a
// timeout does.
static int wait_once(enum wait_kind kind, long long ns, int ep, long long *late) {
    struct timespec ts = {0, (long) ns};
    static int never; // the futex word, never changed and never woken
    sigset_t usr2;
    sigemptyset(&usr2);
    sigaddset(&usr2, SIGUSR2);
    siginfo_t si;
    struct epoll_event ev;
    long long a = now_ns();
    long r;
    int ok;
    switch (kind) {
    case WAIT_NANOSLEEP:
        r = nanosleep(&ts, NULL);
        ok = r == 0;
        break;
    case WAIT_POLL:
        r = poll(NULL, 0, (int) (ns / 1000000));
        ok = r == 0;
        break;
    case WAIT_EPOLL:
        r = epoll_wait(ep, &ev, 1, (int) (ns / 1000000));
        ok = r == 0;
        break;
    case WAIT_FUTEX:
        r = syscall(SYS_futex, &never, FUTEX_WAIT_PRIVATE, 0, &ts, NULL, 0);
        ok = r < 0 && errno == ETIMEDOUT;
        break;
    case WAIT_SIGTIMEDWAIT:
        r = sigtimedwait(&usr2, &si, &ts);
        ok = r < 0 && errno == EAGAIN;
        break;
    default:
        ok = 0;
    }
    *late = now_ns() - a - ns;
    return !ok;
}

#define SHORT_NS 2000000LL
#define LONG_NS 40000000LL
// Coalescing put the 40ms waits 4-10ms behind the 2ms ones, poll's 1-2ms.
// Linux keeps them level but for its idle states: an x86_64 box with nothing
// to do enters a deeper one for 40ms than for 2ms, and takes ~0.55ms longer to
// come out of it (Linux 6.12, 4 runs in 10 over half a millisecond). So a
// millisecond -- which lets through the iPad's old poll, 0.8ms behind, and
// catches everything else.
#define GROWTH_LIMIT_NS 1000000LL

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGRTMIN);
    sigaddset(&set, SIGUSR2); // sigtimedwait's, never sent
    sigprocmask(SIG_BLOCK, &set, NULL);

    // ---- POSIX timer expiries ---------------------------------------------
    {
        enum { BLOCKS = 4, SHORT_PER = 40, LONG_PER = 5 };
        long long shorts[BLOCKS * SHORT_PER], longs[BLOCKS * LONG_PER];
        int ns = 0, nl = 0;
        for (int b = 0; b < BLOCKS; b++) {
            ns += timer_block(SHORT_NS, SHORT_PER, shorts + ns);
            nl += timer_block(LONG_NS, LONG_PER, longs + nl);
        }
        ck_eq("a 2ms timer fired every time it was waited for", ns, BLOCKS * SHORT_PER);
        ck_eq("a 40ms timer fired every time it was waited for", nl, BLOCKS * LONG_PER);
        if (ns > 0 && nl > 0) {
            long long ms = median(shorts, ns), ml = median(longs, nl);
            test_logf("  timer expiry lateness: 2ms period %lldus, 40ms period %lldus (medians)\n",
                      ms / 1000, ml / 1000);
            ck_le("a 40ms timer expires no later than a 2ms one", ml - ms, GROWTH_LIMIT_NS);
        }
    }

    // ---- sleeps and every other wait with a deadline -------------------------
    {
        int ep = epoll_create1(0);
        const struct { enum wait_kind kind; const char *label; } waits[] = {
            {WAIT_NANOSLEEP, "a 40ms nanosleep wakes no later than a 2ms one"},
            {WAIT_POLL, "a 40ms poll times out no later than a 2ms one"},
            {WAIT_EPOLL, "a 40ms epoll_wait times out no later than a 2ms one"},
            {WAIT_FUTEX, "a 40ms FUTEX_WAIT times out no later than a 2ms one"},
            {WAIT_SIGTIMEDWAIT, "a 40ms sigtimedwait times out no later than a 2ms one"},
        };
        for (unsigned w = 0; w < sizeof(waits) / sizeof(waits[0]); w++) {
            enum { PAIRS = 8 };
            long long shorts[PAIRS], longs[PAIRS];
            int bad = 0;
            for (int i = 0; i < PAIRS; i++) {
                bad |= wait_once(waits[w].kind, SHORT_NS, ep, &shorts[i]);
                bad |= wait_once(waits[w].kind, LONG_NS, ep, &longs[i]);
            }
            long long ms = median(shorts, PAIRS), ml = median(longs, PAIRS);
            test_logf("  lateness: 2ms %lldus, 40ms %lldus (medians)\n", ms / 1000, ml / 1000);
            ck_le(waits[w].label, ml - ms, GROWTH_LIMIT_NS);
            ck_eq("  each wait ending as a timeout does", bad, 0);
        }
        close(ep);
    }

    return finish_suite("timer_lateness");
}
