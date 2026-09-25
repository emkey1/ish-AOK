// Three futex defects, one of which disabled an entire POSIX feature.
//
//   get_robust_list(pid = 0) returned EPERM. pid 0 means the calling thread
//   everywhere else in the API, but it was looked up like any other pid and
//   pid 0 is never allocated. musl gates ALL robust-mutex support on exactly
//   this probe succeeding once, so no musl program on AOK could create a
//   robust mutex at all -- which also meant the missing death handling below
//   was unreachable, and looked fine.
//
//   Robust lists were never walked at thread death. A thread dying while
//   holding a robust mutex left every waiter blocked for good, which is
//   precisely what a robust mutex exists to prevent: Linux writes
//   FUTEX_OWNER_DIED into the lock word, clears the TID, and wakes a waiter,
//   so a blocking lock returns EOWNERDEAD instead of hanging.
//
//   FUTEX_CMP_REQUEUE was wrong in three ways at once. It compared *uaddr
//   against `val` -- the number to WAKE -- instead of val3, so an ordinary
//   caller got a spurious EAGAIN while one whose word happened to equal the
//   wake count sailed past the check. It woke nobody, only requeued, so a
//   broadcast lost every wakeup. And it returned only the requeued count where
//   Linux returns woken + requeued. Current musl is unaffected (it uses plain
//   FUTEX_REQUEUE), which is why this needed a probe rather than showing up.
//
//   Two more in the same call, found once a requeue between processes could
//   find its waiter (futex_shared_mapping). A waiter it moved left its
//   reference behind, so the futex it was moved to -- made by the call itself
//   when nobody else waited there -- was freed with the waiter on its queue:
//   the assert in futex_put_unlocked aborted the whole emulator the first time
//   a CMP_REQUEUE moved anyone, which the check above never does (it wakes
//   one and moves none). That is the pre-2.25 glibc broadcast. And requeueing
//   a word onto itself moved each waiter to the back of its own queue, over and
//   over until the requeue count ran out: INT_MAX, over 90 seconds under the
//   global futex lock. Linux counts them and moves nothing.
//
// The raw-protocol half matters: it tests the kernel directly rather than
// through whatever the C library decided to support, which is what made the
// original finding trustworthy.
//
// Measured against x86_64 glibc on Linux 6.12.
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>

#include "test_common.h"

// Not in the musl sysroot; stable kernel ABI.
#ifndef FUTEX_WAIT
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_CMP_REQUEUE 4
#endif
#define OWNER_DIED 0x40000000u

static void ck(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-52s got=%-11ld want=%ld\n", label, got, want);
}

static int fx(int *a, int op, int val, void *ts, int *a2, int val3) {
    return (int) syscall(SYS_futex, a, op, val, ts, a2, val3);
}

static int word1, word2;
static volatile int waiter_ret, waiter_errno, waiter_ready;
static void *waiter(void *arg) {
    (void) arg;
    waiter_ready = 1;
    struct timespec to = { 2, 0 };
    errno = 0;
    waiter_ret = fx(&word1, FUTEX_WAIT, 1, &to, NULL, 0);
    waiter_errno = errno;
    return NULL;
}

static volatile int pair_ret[2], pair_ready;
static void *pair_waiter(void *arg) {
    int i = (int) (long) arg;
    __atomic_add_fetch(&pair_ready, 1, __ATOMIC_SEQ_CST);
    struct timespec to = { 2, 0 };
    errno = 0;
    int r = fx(&word1, FUTEX_WAIT, 1, &to, NULL, 0);
    pair_ret[i] = r == 0 ? 0 : errno;
    return NULL;
}

struct rl { struct rl *next; };
struct rlh { struct rl list; long off; struct rl *pending; };
static struct rlh head;
static struct rl entry;
static volatile unsigned lockword;
static void *dier(void *arg) {
    (void) arg;
    pid_t tid = (pid_t) syscall(SYS_gettid);
    lockword = (unsigned) tid;
    entry.next = &head.list;
    head.list.next = &entry;
    head.off = (char *) &lockword - (char *) &entry;
    head.pending = NULL;
    if (syscall(SYS_set_robust_list, &head, sizeof head) != 0)
        test_logf("    (set_robust_list failed: %s)\n", strerror(errno));
    return NULL;                    // exits still "holding" the lock
}

static pthread_mutex_t rm;
static volatile int locked_ready;
static void *holder(void *arg) {
    (void) arg;
    pthread_mutex_lock(&rm);
    locked_ready = 1;
    return NULL;                    // exits still holding it
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(90));

    test_logf("[89] FUTEX_CMP_REQUEUE compares val3, not val\n");
    {
        word1 = 42;
        errno = 0;
        // the ordinary case: word matches val3
        int r = fx(&word1, FUTEX_CMP_REQUEUE, 1, (void *)(long) 0, &word2, 42);
        ck("  word == val3 succeeds", r < 0 ? errno : 0, 0);
        errno = 0;
        // and a mismatch is EAGAIN
        r = fx(&word1, FUTEX_CMP_REQUEUE, 42, (void *)(long) 0, &word2, 99);
        ck("  word != val3 is EAGAIN", r < 0 ? errno : 0, EAGAIN);
    }

    test_logf("[89] and it actually WAKES\n");
    {
        word1 = 1;
        waiter_ready = 0; waiter_ret = -99;
        pthread_t t;
        pthread_create(&t, NULL, waiter, NULL);
        while (!waiter_ready) usleep(1000);
        usleep(200000);                       // let it reach the wait
        struct timespec a, b;
        clock_gettime(CLOCK_MONOTONIC, &a);
        errno = 0;
        int r = fx(&word1, FUTEX_CMP_REQUEUE, 1, (void *)(long) 0, &word2, 1);
        pthread_join(t, NULL);
        clock_gettime(CLOCK_MONOTONIC, &b);
        double ms = (b.tv_sec-a.tv_sec)*1000.0 + (b.tv_nsec-a.tv_nsec)/1e6;
        test_logf("    requeue rc=%d  waiter ret=%d errno=%d after %.0fms\n",
               r, waiter_ret, waiter_errno, ms);
        ck("  one waiter is woken (rc counts it)", r, 1);
        ck("  and it returns success, not a timeout", waiter_ret, 0);
        ck("  promptly", ms < 1500, 1);
    }

    test_logf("[89] and a waiter it MOVES is woken at the target\n");
    {
        word1 = 1;
        waiter_ready = 0; waiter_ret = -99;
        pthread_t t;
        pthread_create(&t, NULL, waiter, NULL);
        while (!waiter_ready) usleep(1000);
        usleep(200000);                       // let it reach the wait
        errno = 0;
        // Wake none and move one, to a word nobody else waits on.
        int r = fx(&word1, FUTEX_CMP_REQUEUE, 0, (void *)(long) 1, &word2, 1);
        ck("  it is moved (rc counts it)", r, 1);
        int w = fx(&word2, FUTEX_WAKE, 1, NULL, NULL, 0);
        pthread_join(t, NULL);
        ck("  a wake at the target finds it", w, 1);
        ck("  and it returns success, not a timeout", waiter_ret, 0);
    }

    test_logf("[89] requeueing a word onto itself counts, and moves nothing\n");
    {
        word1 = 1;
        pair_ready = 0;
        pair_ret[0] = pair_ret[1] = -99;
        pthread_t t[2];
        for (long i = 0; i < 2; i++)
            pthread_create(&t[i], NULL, pair_waiter, (void *) i);
        while (pair_ready < 2) usleep(1000);
        usleep(200000);
        struct timespec a, b;
        clock_gettime(CLOCK_MONOTONIC, &a);
        int r = fx(&word1, FUTEX_CMP_REQUEUE, 0, (void *)(long) INT_MAX, &word1, 1);
        clock_gettime(CLOCK_MONOTONIC, &b);
        double ms = (b.tv_sec-a.tv_sec)*1000.0 + (b.tv_nsec-a.tv_nsec)/1e6;
        test_logf("    requeue rc=%d after %.0fms\n", r, ms);
        ck("  both waiters are counted", r, 2);
        ck("  promptly", ms < 500, 1);
        int w = fx(&word1, FUTEX_WAKE, INT_MAX, NULL, NULL, 0);
        for (int i = 0; i < 2; i++)
            pthread_join(t[i], NULL);
        ck("  and a wake still finds both", w, 2);
        ck("  the first returns success", pair_ret[0], 0);
        ck("  the second returns success", pair_ret[1], 0);
    }

    test_logf("[88] musl/glibc can create a robust mutex\n");
    {
        pthread_mutexattr_t at;
        pthread_mutexattr_init(&at);
        errno = 0;
        int r = pthread_mutexattr_setrobust(&at, PTHREAD_MUTEX_ROBUST);
        ck("  pthread_mutexattr_setrobust", r, 0);
        pthread_mutex_t m;
        ck("  and the mutex initialises", pthread_mutex_init(&m, &at), 0);
        pthread_mutex_destroy(&m);
        pthread_mutexattr_destroy(&at);
    }
    test_logf("[87] raw robust list: the kernel marks the word at thread death\n");
    {
        pthread_t t;
        pthread_create(&t, NULL, dier, NULL);
        pthread_join(t, NULL);
        usleep(200000);
        test_logf("    lockword after death = %#x\n", lockword);
        ck("  FUTEX_OWNER_DIED is set", (lockword & OWNER_DIED) != 0, 1);
        ck("  and the TID field is cleared", (lockword & 0x3fffffff) == 0, 1);
    }

    test_logf("[87] and a robust mutex reports EOWNERDEAD to the next locker\n");
    {
        pthread_mutexattr_t at;
        pthread_mutexattr_init(&at);
        if (pthread_mutexattr_setrobust(&at, PTHREAD_MUTEX_ROBUST) != 0) {
            test_logf("    (this libc will not make robust mutexes here)\n");
            failures_total++;
        } else if (pthread_mutex_init(&rm, &at) != 0) {
            test_logf("    (mutex init failed)\n");
            failures_total++;
        } else {
            pthread_t t;
            locked_ready = 0;
            pthread_create(&t, NULL, holder, NULL);
            while (!locked_ready) usleep(1000);
            pthread_join(t, NULL);
            struct timespec a, b;
            clock_gettime(CLOCK_MONOTONIC, &a);
            struct timespec to;
            clock_gettime(CLOCK_REALTIME, &to);
            to.tv_sec += 2;
            int r = pthread_mutex_timedlock(&rm, &to);
            clock_gettime(CLOCK_MONOTONIC, &b);
            double ms = (b.tv_sec-a.tv_sec)*1000.0 + (b.tv_nsec-a.tv_nsec)/1e6;
            test_logf("    timedlock rc=%d (%s) after %.0fms\n", r, strerror(r), ms);
            ck("  it returns EOWNERDEAD", r, EOWNERDEAD);
            ck("  immediately, not after the timeout", ms < 1000, 1);
            if (r == EOWNERDEAD) {
                ck("  and the lock can be made consistent", pthread_mutex_consistent(&rm), 0);
                pthread_mutex_unlock(&rm);
            }
        }
        pthread_mutexattr_destroy(&at);
    }

    return finish_suite("futex_robust_requeue");
}
