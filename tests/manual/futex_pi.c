// PI futexes: FUTEX_LOCK_PI, LOCK_PI2, TRYLOCK_PI and UNLOCK_PI, by raw
// syscall (so musl runs it too), and glibc's PTHREAD_PRIO_INHERIT mutexes on
// top of them. Values from Linux 6.12 on camd (x86_64, glibc 2.41).
//
// These were ENOSYS. glibc assumes a kernel has them -- its PI mutexes were
// refused at init (wf-panel's PulseAudio fell back to plain ones), and its
// lock path takes an unexpected failure as having the lock.

#define _GNU_SOURCE
#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

#ifndef FUTEX_LOCK_PI2
#define FUTEX_LOCK_PI2 13
#endif

static void ck(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-62s got=%ld want=%ld\n", label, got, want);
}

// A 32-bit ABI with a 64-bit time_t (musl 1.2, glibc's _TIME_BITS=64) passes
// its timespec to futex_time64, as libc does; SYS_futex would read it as two
// 32-bit fields.
static long fx(_Atomic uint32_t *w, int op, const struct timespec *ts) {
    long nr = SYS_futex;
#ifdef SYS_futex_time64
    if (sizeof(long) == 4 && sizeof(time_t) == 8)
        nr = SYS_futex_time64;
#endif
    return syscall(nr, w, op, 0, ts, NULL, 0) < 0 ? -errno : 0;
}

static uint32_t tid(void) {
    return (uint32_t) syscall(SYS_gettid);
}

static long ms_since(const struct timespec *t0) {
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    return (t1.tv_sec - t0->tv_sec) * 1000 + (t1.tv_nsec - t0->tv_nsec) / 1000000;
}

static struct timespec abs_in(clockid_t clock, long ms) {
    struct timespec t;
    clock_gettime(clock, &t);
    t.tv_nsec += (ms % 1000) * 1000000;
    t.tv_sec += ms / 1000 + t.tv_nsec / 1000000000;
    t.tv_nsec %= 1000000000;
    return t;
}

static _Atomic uint32_t word;
static _Atomic uint32_t locker_tid;
static _Atomic int locker_state;    // 1 about to lock, 2 got it
static long locker_result;

static void *locker(void *arg) {
    (void) arg;
    locker_tid = tid();
    locker_state = 1;
    locker_result = fx(&word, FUTEX_LOCK_PI_PRIVATE, NULL);
    locker_state = 2;
    return NULL;
}

static void *trylocker(void *arg) {
    *(long *) arg = fx(&word, FUTEX_TRYLOCK_PI_PRIVATE, NULL);
    return NULL;
}

// Takes the lock, and exits holding it once told to.
static _Atomic int owner_may_exit;
static void *lock_and_exit(void *arg) {
    (void) arg;
    fx(&word, FUTEX_LOCK_PI_PRIVATE, NULL);
    while (!owner_may_exit)
        usleep(1000);
    return NULL;
}

static void wait_blocked(void) {
    while (locker_state != 1)
        usleep(1000);
    // Blocked in the kernel once the word says a waiter is there.
    for (int i = 0; i < 2000 && !(word & FUTEX_WAITERS); i++)
        usleep(1000);
    usleep(50000);
}

static void on_usr1(int sig) {
    (void) sig;
}

static void raw_protocol(void) {
    word = 0;
    ck("UNLOCK_PI of a free word is EPERM", fx(&word, FUTEX_UNLOCK_PI_PRIVATE, NULL), -EPERM);
    ck("TRYLOCK_PI of a free word", fx(&word, FUTEX_TRYLOCK_PI_PRIVATE, NULL), 0);
    ck("  makes the caller the owner", word, tid());
    ck("TRYLOCK_PI by the owner is EDEADLK", fx(&word, FUTEX_TRYLOCK_PI_PRIVATE, NULL), -EDEADLK);
    ck("LOCK_PI by the owner is EDEADLK", fx(&word, FUTEX_LOCK_PI_PRIVATE, NULL), -EDEADLK);
    ck("LOCK_PI with FUTEX_CLOCK_REALTIME is ENOSYS",
       fx(&word, FUTEX_LOCK_PI_PRIVATE | FUTEX_CLOCK_REALTIME, NULL), -ENOSYS);
    ck("UNLOCK_PI with FUTEX_CLOCK_REALTIME is ENOSYS",
       fx(&word, FUTEX_UNLOCK_PI_PRIVATE | FUTEX_CLOCK_REALTIME, NULL), -ENOSYS);

    pthread_t t;
    long tr = 1;
    pthread_create(&t, NULL, trylocker, &tr);
    pthread_join(t, NULL);
    ck("TRYLOCK_PI by another thread is EAGAIN", tr, -EAGAIN);
    ck("  and leaves FUTEX_WAITERS set", word, tid() | FUTEX_WAITERS);
    ck("UNLOCK_PI with no waiters", fx(&word, FUTEX_UNLOCK_PI_PRIVATE, NULL), 0);
    ck("  frees the word", word, 0);

    // Contended: the unlock hands the lock straight to the waiter.
    word = tid();
    locker_state = 0;
    pthread_create(&t, NULL, locker, NULL);
    wait_blocked();
    ck("a waiter sets FUTEX_WAITERS", word, tid() | FUTEX_WAITERS);
    ck("  and is still waiting", locker_state, 1);
    ck("UNLOCK_PI with a waiter", fx(&word, FUTEX_UNLOCK_PI_PRIVATE, NULL), 0);
    pthread_join(t, NULL);
    ck("  the waiter's LOCK_PI returns 0", locker_result, 0);
    ck("  and it owns the word, FUTEX_WAITERS set", word, locker_tid | FUTEX_WAITERS);
    ck("UNLOCK_PI by a thread that is not the owner is EPERM",
       fx(&word, FUTEX_UNLOCK_PI_PRIVATE, NULL), -EPERM);

    // Timeouts: LOCK_PI's deadline is CLOCK_REALTIME, LOCK_PI2's
    // CLOCK_MONOTONIC unless FUTEX_CLOCK_REALTIME says otherwise.
    struct timespec t0, dl;
    word = locker_tid;  // an exited thread: still "owned"
    ck("LOCK_PI on a word whose owner never existed is ESRCH",
       fx(&word, FUTEX_LOCK_PI_PRIVATE, NULL), -ESRCH);
    word = tid() == 1 ? 2 : 1;  // owned by a live task (init, or 1)
    if (kill(word, 0) == 0 || errno == EPERM) {
        clock_gettime(CLOCK_MONOTONIC, &t0);
        dl = abs_in(CLOCK_REALTIME, 300);
        ck("LOCK_PI with a REALTIME deadline is ETIMEDOUT",
           fx(&word, FUTEX_LOCK_PI_PRIVATE, &dl), -ETIMEDOUT);
        long ms = ms_since(&t0);
        ck("  after the deadline (280..2000 ms)", ms >= 280 && ms < 2000, 1);
        clock_gettime(CLOCK_MONOTONIC, &t0);
        dl = abs_in(CLOCK_MONOTONIC, 300);
        ck("LOCK_PI2 with a MONOTONIC deadline is ETIMEDOUT",
           fx(&word, FUTEX_LOCK_PI2 | FUTEX_PRIVATE_FLAG, &dl), -ETIMEDOUT);
        ms = ms_since(&t0);
        ck("  after the deadline (280..2000 ms)", ms >= 280 && ms < 2000, 1);
        dl = abs_in(CLOCK_REALTIME, 100);
        ck("LOCK_PI2 | FUTEX_CLOCK_REALTIME is accepted, ETIMEDOUT",
           fx(&word, FUTEX_LOCK_PI2 | FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME, &dl), -ETIMEDOUT);
        dl = abs_in(CLOCK_REALTIME, -1000);
        ck("LOCK_PI with a deadline already past is ETIMEDOUT",
           fx(&word, FUTEX_LOCK_PI_PRIVATE, &dl), -ETIMEDOUT);
        struct timespec bad = {0, 1000000000};
        ck("LOCK_PI with tv_nsec out of range is EINVAL",
           fx(&word, FUTEX_LOCK_PI_PRIVATE, &bad), -EINVAL);
    }
    word = 0;
    dl = abs_in(CLOCK_REALTIME, -1000);
    ck("LOCK_PI of a free word takes it even past its deadline",
       fx(&word, FUTEX_LOCK_PI_PRIVATE, &dl), 0);
    word = 0;

    // A signal while waiting runs its handler and the wait goes on: never
    // EINTR, even without SA_RESTART (Linux's ERESTARTNOINTR).
    struct sigaction sa = {.sa_handler = on_usr1};
    sigaction(SIGUSR1, &sa, NULL);
    word = tid();
    locker_state = 0;
    pthread_create(&t, NULL, locker, NULL);
    wait_blocked();
    pthread_kill(t, SIGUSR1);
    usleep(100000);
    ck("a signal handler does not end the wait", locker_state, 1);
    fx(&word, FUTEX_UNLOCK_PI_PRIVATE, NULL);
    pthread_join(t, NULL);
    ck("  which still gets the lock (no EINTR)", locker_result, 0);
    word = 0;

    // An owner that exits holding the lock: a waiter queued before it died
    // takes it over (Linux's exit_pi_state_list). One that comes after finds
    // no owner and gets ESRCH, checked above.
    word = 0;
    owner_may_exit = 0;
    pthread_t owner;
    pthread_create(&owner, NULL, lock_and_exit, NULL);
    while ((word & FUTEX_TID_MASK) == 0)
        usleep(1000);
    locker_state = 0;
    pthread_create(&t, NULL, locker, NULL);
    wait_blocked();
    owner_may_exit = 1;
    pthread_join(owner, NULL);
    for (int i = 0; i < 3000 && locker_state != 2; i++)
        usleep(1000);
    ck("a waiter gets the lock when its owner exits", locker_state, 2);
    if (locker_state == 2) {
        pthread_join(t, NULL);
        ck("  LOCK_PI returns 0", locker_result, 0);
        // Not robust, and still marked: Linux's fixup gives the new owner
        // FUTEX_OWNER_DIED whenever the old one is gone, and keeps
        // FUTEX_WAITERS.
        ck("  and the word is the waiter's, OWNER_DIED and WAITERS set",
           word, locker_tid | FUTEX_OWNER_DIED | FUTEX_WAITERS);
    }
    word = 0;
}

// glibc's PI mutexes on top of the above.
#if defined(__GLIBC__)
static pthread_mutex_t pim;
static long counter;

static void *hammer(void *arg) {
    (void) arg;
    for (int i = 0; i < 20000; i++) {
        pthread_mutex_lock(&pim);
        long c = counter;
        if ((i & 63) == 0)
            sched_yield();
        counter = c + 1;
        pthread_mutex_unlock(&pim);
    }
    return NULL;
}

static pthread_mutex_t robust;
static void *lock_robust_and_exit(void *arg) {
    (void) arg;
    pthread_mutex_lock(&robust);
    return NULL;
}

static void glibc_mutexes(void) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    ck("pthread_mutexattr_setprotocol(PRIO_INHERIT)",
       pthread_mutexattr_setprotocol(&a, PTHREAD_PRIO_INHERIT), 0);
    ck("pthread_mutex_init of a PI mutex", pthread_mutex_init(&pim, &a), 0);
    pthread_t th[4];
    for (int i = 0; i < 4; i++)
        pthread_create(&th[i], NULL, hammer, NULL);
    for (int i = 0; i < 4; i++)
        pthread_join(th[i], NULL);
    ck("4 threads x 20000 increments under a PI mutex", counter, 80000);

    pthread_mutexattr_setrobust(&a, PTHREAD_MUTEX_ROBUST);
    ck("a robust PI mutex", pthread_mutex_init(&robust, &a), 0);
    pthread_t t;
    pthread_create(&t, NULL, lock_robust_and_exit, NULL);
    pthread_join(t, NULL);
    ck("locking it after its owner exited is EOWNERDEAD", pthread_mutex_lock(&robust), EOWNERDEAD);
    ck("  pthread_mutex_consistent", pthread_mutex_consistent(&robust), 0);
    ck("  unlock", pthread_mutex_unlock(&robust), 0);
    ck("  and lock again", pthread_mutex_lock(&robust), 0);
    pthread_mutex_unlock(&robust);
}
#endif

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));
    raw_protocol();
#if defined(__GLIBC__)
    glibc_mutexes();
#else
    test_logf("glibc mutex checks skipped (not glibc)\n");
#endif
    return finish_suite("futex_pi");
}
