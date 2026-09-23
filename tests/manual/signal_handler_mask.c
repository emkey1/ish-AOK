/*
 * signal_handler_mask.c -- a handler's mask is in force for the rest of the
 * delivery pass that started it.
 *
 * Linux delivers pending signals one at a time: exit_to_user_mode_loop calls
 * get_signal once per signal, and each handler it sets up changes the mask
 * before the next one is chosen -- signal_delivered blocks the handler's
 * sa_mask and, without SA_NODEFER, the signal itself. So a second pending
 * signal the FIRST handler's mask blocks is not delivered in that pass. It
 * waits for that handler's sigreturn, and runs after it. One the first
 * handler does not block is stacked on top of it and runs FIRST, which is why
 * handlers of signals that become deliverable together run highest-first.
 *
 * AOK read the mask once, before its delivery loop, and delivered everything
 * that mask let through in one pass. Measured (x86_64): SIGUSR1's handler
 * blocks SIGUSR2, both are raised while blocked and unblocked together; the
 * handlers ran in the order 1,2 on Linux 6.12 and 2,1 on alpine-amd64-test.
 *
 *   - sa_mask: that case, and three signals where the first handler blocks
 *     the third but not the second. Linux runs the second (stacked), the
 *     first, and only then the third.
 *   - realtime twice: two queued instances of one realtime signal. Without
 *     SA_NODEFER the second waits for the first handler to finish (1,2);
 *     with it the second is stacked and runs first (2,1).
 *   - sigsuspend: the mask sigsuspend waits with stays in force while the
 *     signals it let through are delivered, and the mask from before the
 *     call is what each handler's sigreturn puts back. Both pending signals
 *     run in the one sigsuspend when their handlers do not block each other,
 *     the first handler runs with the temporary mask plus its own, and the
 *     mask after sigsuspend is the old one again -- a realtime bit in it
 *     included, which on i386 travels in a different word of the old-style
 *     frame. When the first handler blocks the second, the second stays
 *     pending past sigsuspend, blocked by the mask it returns to.
 *   - sigsuspend, a signal the temporary mask blocks: a second thread sends
 *     SIGUSR2 (blocked by the temporary mask, not by the old one) and then
 *     SIGUSR1 (which ends the wait). SIGUSR1's handler runs first, with
 *     SIGUSR2 still blocked, and SIGUSR2's runs when it returns.
 *   - hand-off: a process's SIGCHLD is queued for the thread that forked the
 *     child, which then handles a SIGUSR1 whose handler blocks SIGCHLD.
 *     Linux's signal_delivered hands the SIGCHLD to a sibling that can take
 *     it (retarget_shared_pending), which runs its handler while SIGUSR1's is
 *     still running. With no such sibling it waits for SIGUSR1's handler to
 *     return and runs in the forking thread.
 *
 *     Getting there on Linux takes care: the forking thread must be the one
 *     told about the SIGCHLD, so it must not block it when it is sent, and it
 *     must not act on it before the SIGUSR1 arrives. A vfork wait is killable
 *     but not interruptible, so the forking thread sits in one -- a
 *     clone(CLONE_VFORK) child with no exit signal -- while a helper thread
 *     that blocks everything lets the other child exit, sees SIGCHLD in its
 *     own sigpending(), sends SIGUSR1, and lets the vfork child go.
 *   - sigfillset handler: SIGKILL and SIGSTOP are dropped from a handler's
 *     mask as it is installed, so one built with sigfillset -- common --
 *     neither reports them nor blocks them, and a SIGKILL sent from inside it
 *     ends the process at once. AOK kept them, and the SIGKILL waited for the
 *     handler to return.
 *
 * Each scenario runs in a process of its own. Handlers record their order,
 * the mask they ran with, what was pending, and the thread they ran in.
 *
 * Checked against Linux 6.12 (Devuan 6, x86_64 and -m32, unprivileged).
 *
 * Exits 0 and prints "signal_handler_mask: PASS" on success.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

#ifndef __WALL
#define __WALL 0x40000000
#endif
#ifndef CLONE_VFORK
#define CLONE_VFORK 0x00004000
#endif

#define MAX_EVENTS 8

/* What each handler run saw, in the order the handlers ran. */
static volatile int nevents;
static char events[MAX_EVENTS + 1];
static sigset_t event_mask[MAX_EVENTS];
static sigset_t event_pending[MAX_EVENTS];
static pid_t event_tid[MAX_EVENTS];

static pid_t current_tid(void) {
    return (pid_t) syscall(SYS_gettid);
}

static char letter_of(int sig) {
    if (sig == SIGUSR1)
        return 'a';
    if (sig == SIGUSR2)
        return 'b';
    if (sig == SIGURG)
        return 'c';
    if (sig == SIGCHLD)
        return 'd';
    return '?';
}

/* Async-signal-safe: pthread_sigmask and sigpending are. The slot is claimed
 * atomically, since the hand-off scenario records from two threads. */
static int record(char c) {
    int i = __atomic_fetch_add(&nevents, 1, __ATOMIC_SEQ_CST);
    if (i >= MAX_EVENTS)
        return -1;
    pthread_sigmask(SIG_BLOCK, NULL, &event_mask[i]);
    sigpending(&event_pending[i]);
    event_tid[i] = current_tid();
    events[i] = c;
    return i;
}

static void events_reset(void) {
    nevents = 0;
    memset(events, 0, sizeof events);
}

static const char *events_str(void) {
    int n = nevents < MAX_EVENTS ? nevents : MAX_EVENTS;
    events[n] = '\0';
    return events;
}

static void on_letter(int sig) {
    record(letter_of(sig));
}

static void on_letter_info(int sig, siginfo_t *info, void *uc) {
    (void) info;
    (void) uc;
    record(letter_of(sig));
}

/* A realtime instance records the value it was queued with. */
static void on_rt_value(int sig, siginfo_t *info, void *uc) {
    (void) sig;
    (void) uc;
    record((char) ('0' + info->si_value.sival_int));
}

static void install(int sig, bool siginfo, const sigset_t *mask, int flags) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    if (siginfo) {
        sa.sa_sigaction = on_letter_info;
        flags |= SA_SIGINFO;
    } else {
        sa.sa_handler = on_letter;
    }
    if (mask != NULL)
        sa.sa_mask = *mask;
    else
        sigemptyset(&sa.sa_mask);
    sa.sa_flags = flags;
    sigaction(sig, &sa, NULL);
}

static sigset_t set_of(int a, int b, int c) {
    sigset_t set;
    sigemptyset(&set);
    if (a != 0)
        sigaddset(&set, a);
    if (b != 0)
        sigaddset(&set, b);
    if (c != 0)
        sigaddset(&set, c);
    return set;
}

/* 1 when `set` holds exactly the signals among `of` that `want` names -- the
 * signals outside `of` are none of this test's business (libc may hold its
 * own reserved ones). */
static bool same_among(const sigset_t *set, const sigset_t *want, const int *of, int n) {
    for (int i = 0; i < n; i++)
        if (sigismember(set, of[i]) != sigismember(want, of[i]))
            return false;
    return true;
}

static uint64_t bits_among(const sigset_t *set, const int *of, int n) {
    uint64_t bits = 0;
    for (int i = 0; i < n; i++)
        if (sigismember(set, of[i]) == 1)
            bits |= 1ull << (of[i] - 1);
    return bits;
}

static int watched[6];
static int nwatched;

static void watch_init(void) {
    watched[0] = SIGUSR1;
    watched[1] = SIGUSR2;
    watched[2] = SIGURG;
    watched[3] = SIGCHLD;
    watched[4] = SIGRTMIN;
    watched[5] = SIGRTMIN + 2;
    nwatched = 6;
}

static void expect_events(const char *label, const char *want) {
    const char *got = events_str();
    if (strcmp(got, want) != 0) {
        printf("FAIL %s: handlers ran \"%s\", expected \"%s\"\n", label, got, want);
        failures_total++;
    } else {
        test_logf("%s: handlers ran \"%s\"\n", label, got);
    }
}

static void expect_set(const char *label, const sigset_t *got, sigset_t want) {
    if (!same_among(got, &want, watched, nwatched)) {
        printf("FAIL %s: %#llx, expected %#llx\n", label,
               (unsigned long long) bits_among(got, watched, nwatched),
               (unsigned long long) bits_among(&want, watched, nwatched));
        failures_total++;
    } else {
        test_logf("%s: %#llx\n", label,
                  (unsigned long long) bits_among(got, watched, nwatched));
    }
}

static void expect_event_mask(const char *label, int i, sigset_t want) {
    if (i >= nevents) {
        printf("FAIL %s: handler %d never ran\n", label, i);
        failures_total++;
        return;
    }
    expect_set(label, &event_mask[i], want);
}

static void expect_event_pending(const char *label, int i, sigset_t want) {
    if (i >= nevents) {
        printf("FAIL %s: handler %d never ran\n", label, i);
        failures_total++;
        return;
    }
    expect_set(label, &event_pending[i], want);
}

static void expect_now(const char *label, sigset_t want_mask, sigset_t want_pending) {
    char buf[96];
    sigset_t mask, pending;
    pthread_sigmask(SIG_BLOCK, NULL, &mask);
    sigpending(&pending);
    snprintf(buf, sizeof buf, "%s: mask", label);
    expect_set(buf, &mask, want_mask);
    snprintf(buf, sizeof buf, "%s: pending", label);
    expect_set(buf, &pending, want_pending);
}

static void check(bool ok, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (!ok) {
        printf("FAIL ");
        vprintf(fmt, ap);
        printf("\n");
        failures_total++;
    } else if (test_verbose) {
        vprintf(fmt, ap);
        printf("\n");
    }
    va_end(ap);
}

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000 + (uint64_t) ts.tv_nsec / 1000000;
}

/* ------------------------------------------------------------------ sa_mask */

static void scenario_samask(bool siginfo) {
    const char *how = siginfo ? "SA_SIGINFO" : "plain";
    char label[96];
    sigset_t none;
    sigemptyset(&none);

    /* The measured case. SIGUSR1's handler blocks SIGUSR2, so SIGUSR2 waits
     * for it to return. */
    sigset_t usr2 = set_of(SIGUSR2, 0, 0);
    install(SIGUSR1, siginfo, &usr2, 0);
    install(SIGUSR2, siginfo, NULL, 0);
    sigset_t both = set_of(SIGUSR1, SIGUSR2, 0);
    sigprocmask(SIG_BLOCK, &both, NULL);
    raise(SIGUSR2);
    raise(SIGUSR1);
    events_reset();
    sigprocmask(SIG_UNBLOCK, &both, NULL);
    snprintf(label, sizeof label, "sa_mask %s: USR1 blocks USR2", how);
    expect_events(label, "ab");
    snprintf(label, sizeof label, "sa_mask %s: USR1's handler mask", how);
    expect_event_mask(label, 0, set_of(SIGUSR1, SIGUSR2, 0));
    snprintf(label, sizeof label, "sa_mask %s: pending in USR1's handler", how);
    expect_event_pending(label, 0, set_of(SIGUSR2, 0, 0));
    snprintf(label, sizeof label, "sa_mask %s: USR2's handler mask", how);
    expect_event_mask(label, 1, set_of(SIGUSR2, 0, 0));

    /* The same two without the mask: stacked, the higher one runs first. */
    install(SIGUSR1, siginfo, NULL, 0);
    sigprocmask(SIG_BLOCK, &both, NULL);
    raise(SIGUSR2);
    raise(SIGUSR1);
    events_reset();
    sigprocmask(SIG_UNBLOCK, &both, NULL);
    snprintf(label, sizeof label, "sa_mask %s: no mask", how);
    expect_events(label, "ba");
    snprintf(label, sizeof label, "sa_mask %s: USR2's handler mask, stacked", how);
    expect_event_mask(label, 0, set_of(SIGUSR1, SIGUSR2, 0));

    /* Three: SIGUSR1's handler blocks SIGURG but not SIGUSR2. SIGUSR2 is
     * stacked on SIGUSR1 and runs first, then SIGUSR1's handler, and SIGURG
     * only once that has returned. */
    sigset_t urg = set_of(SIGURG, 0, 0);
    install(SIGUSR1, siginfo, &urg, 0);
    install(SIGUSR2, siginfo, NULL, 0);
    install(SIGURG, siginfo, NULL, 0);
    sigset_t three = set_of(SIGUSR1, SIGUSR2, SIGURG);
    sigprocmask(SIG_BLOCK, &three, NULL);
    raise(SIGURG);
    raise(SIGUSR2);
    raise(SIGUSR1);
    events_reset();
    sigprocmask(SIG_UNBLOCK, &three, NULL);
    snprintf(label, sizeof label, "sa_mask %s: USR1 blocks URG, not USR2", how);
    expect_events(label, "bac");
    snprintf(label, sizeof label, "sa_mask %s: USR2's handler mask (three)", how);
    expect_event_mask(label, 0, set_of(SIGUSR1, SIGUSR2, SIGURG));
    snprintf(label, sizeof label, "sa_mask %s: USR1's handler mask (three)", how);
    expect_event_mask(label, 1, set_of(SIGUSR1, SIGURG, 0));
    snprintf(label, sizeof label, "sa_mask %s: pending in USR1's handler (three)", how);
    expect_event_pending(label, 1, set_of(SIGURG, 0, 0));
    snprintf(label, sizeof label, "sa_mask %s: URG's handler mask (three)", how);
    expect_event_mask(label, 2, set_of(SIGURG, 0, 0));
    snprintf(label, sizeof label, "sa_mask %s: after", how);
    expect_now(label, none, none);
}

/* ---------------------------------------------------------- realtime twice */

static void scenario_realtime(bool nodefer) {
    const char *how = nodefer ? "SA_NODEFER" : "default";
    char label[96];
    int rt = SIGRTMIN;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_rt_value;
    sa.sa_flags = SA_SIGINFO | (nodefer ? SA_NODEFER : 0);
    sigemptyset(&sa.sa_mask);
    sigaction(rt, &sa, NULL);

    sigset_t set = set_of(rt, 0, 0);
    sigprocmask(SIG_BLOCK, &set, NULL);
    for (int v = 1; v <= 2; v++) {
        union sigval value;
        value.sival_int = v;
        if (sigqueue(getpid(), rt, value) != 0) {
            printf("FAIL realtime %s: sigqueue: %s\n", how, strerror(errno));
            failures_total++;
        }
    }
    events_reset();
    sigprocmask(SIG_UNBLOCK, &set, NULL);
    snprintf(label, sizeof label, "realtime twice, %s", how);
    /* Without SA_NODEFER the first instance's handler blocks the signal, so
     * the second waits for it. With it the second is stacked and runs first. */
    expect_events(label, nodefer ? "21" : "12");
    snprintf(label, sizeof label, "realtime twice, %s: first handler's mask", how);
    sigset_t none;
    sigemptyset(&none);
    expect_event_mask(label, 0, nodefer ? none : set);
}

/* --------------------------------------------------------------- sigsuspend */

static void scenario_sigsuspend(bool siginfo) {
    const char *how = siginfo ? "SA_SIGINFO" : "plain";
    char label[96];
    int rt2 = SIGRTMIN + 2;
    sigset_t none;
    sigemptyset(&none);
    /* The mask sigsuspend returns to. The realtime bit is past the first 32,
     * which an i386 old-style frame keeps in a word of its own. */
    sigset_t old = set_of(SIGUSR1, SIGUSR2, rt2);

    /* Neither handler blocks the other: both run in the one sigsuspend, the
     * second stacked on the first. The first runs with the temporary (empty)
     * mask plus its own signal -- SIGUSR2 and the realtime signal unblocked. */
    install(SIGUSR1, siginfo, NULL, 0);
    install(SIGUSR2, siginfo, NULL, 0);
    sigprocmask(SIG_SETMASK, &old, NULL);
    raise(SIGUSR2);
    raise(SIGUSR1);
    events_reset();
    errno = 0;
    int rc = sigsuspend(&none);
    int err = errno;
    snprintf(label, sizeof label, "sigsuspend %s: returned", how);
    check(rc == -1 && err == EINTR, "%s rc=%d errno=%d", label, rc, err);
    snprintf(label, sizeof label, "sigsuspend %s: both", how);
    expect_events(label, "ba");
    snprintf(label, sizeof label, "sigsuspend %s: USR2's handler mask", how);
    expect_event_mask(label, 0, set_of(SIGUSR1, SIGUSR2, 0));
    snprintf(label, sizeof label, "sigsuspend %s: USR1's handler mask", how);
    expect_event_mask(label, 1, set_of(SIGUSR1, 0, 0));
    snprintf(label, sizeof label, "sigsuspend %s: after", how);
    expect_now(label, old, none);

    /* SIGUSR1's handler blocks SIGUSR2. SIGUSR2 is not delivered in that
     * pass, and the mask SIGUSR1's sigreturn puts back blocks it too, so it
     * is still pending when sigsuspend returns. */
    sigset_t usr2 = set_of(SIGUSR2, 0, 0);
    install(SIGUSR1, siginfo, &usr2, 0);
    raise(SIGUSR2);
    raise(SIGUSR1);
    events_reset();
    errno = 0;
    rc = sigsuspend(&none);
    err = errno;
    snprintf(label, sizeof label, "sigsuspend %s, USR1 blocks USR2: returned", how);
    check(rc == -1 && err == EINTR, "%s rc=%d errno=%d", label, rc, err);
    snprintf(label, sizeof label, "sigsuspend %s, USR1 blocks USR2", how);
    expect_events(label, "a");
    snprintf(label, sizeof label, "sigsuspend %s, USR1 blocks USR2: USR1's handler mask", how);
    expect_event_mask(label, 0, set_of(SIGUSR1, SIGUSR2, 0));
    snprintf(label, sizeof label, "sigsuspend %s, USR1 blocks USR2: after", how);
    expect_now(label, old, usr2);
    events_reset();
    sigprocmask(SIG_UNBLOCK, &usr2, NULL);
    snprintf(label, sizeof label, "sigsuspend %s, USR1 blocks USR2: unblocked later", how);
    expect_events(label, "b");
}

/* ------------------------------------ sigsuspend, a signal its mask blocks */

static pid_t suspender_tid;
static sigset_t suspend_mask;

/* The SigBlk line of `tid`'s status: the mask it has right now. */
static bool read_sigblk(pid_t tid, uint64_t *out) {
    char path[64], buf[4096];
    snprintf(path, sizeof path, "/proc/self/task/%d/status", (int) tid);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0)
        return false;
    buf[n] = '\0';
    char *line = strstr(buf, "SigBlk:");
    if (line == NULL)
        return false;
    *out = strtoull(line + strlen("SigBlk:"), NULL, 16);
    return true;
}

static uint64_t sigset_bits(const sigset_t *set) {
    uint64_t bits = 0;
    for (int sig = 1; sig <= 64; sig++)
        if (sigismember(set, sig) == 1)
            bits |= 1ull << (sig - 1);
    return bits;
}

static void *suspend_sender(void *arg) {
    int *ok = arg;
    sigset_t all;
    sigfillset(&all);
    pthread_sigmask(SIG_SETMASK, &all, NULL);
    /* The suspender is in sigsuspend once its mask is the temporary one: it
     * holds that mask nowhere else. */
    uint64_t want = sigset_bits(&suspend_mask);
    uint64_t deadline = now_ms() + 5000;
    uint64_t got = 0;
    while (now_ms() < deadline) {
        if (read_sigblk(suspender_tid, &got) && got == want)
            break;
        usleep(1000);
    }
    *ok = got == want;
    /* SIGUSR2 first: the temporary mask blocks it, so it only waits. Then
     * SIGUSR1, which ends the wait. */
    syscall(SYS_tgkill, getpid(), suspender_tid, SIGUSR2);
    syscall(SYS_tgkill, getpid(), suspender_tid, SIGUSR1);
    return NULL;
}

static void scenario_sigsuspend_temp_blocks(bool siginfo) {
    const char *how = siginfo ? "SA_SIGINFO" : "plain";
    char label[96];
    sigset_t none;
    sigemptyset(&none);
    sigset_t old = set_of(SIGUSR1, 0, 0);
    suspend_mask = set_of(SIGUSR2, 0, 0);
    install(SIGUSR1, siginfo, NULL, 0);
    install(SIGUSR2, siginfo, NULL, 0);
    sigprocmask(SIG_SETMASK, &old, NULL);
    suspender_tid = current_tid();

    int ok = 0;
    pthread_t sender;
    pthread_create(&sender, NULL, suspend_sender, &ok);
    events_reset();
    errno = 0;
    int rc = sigsuspend(&suspend_mask);
    int err = errno;
    pthread_join(sender, NULL);
    snprintf(label, sizeof label, "sigsuspend %s, temporary mask blocks USR2: seen waiting", how);
    check(ok, "%s", label);
    snprintf(label, sizeof label, "sigsuspend %s, temporary mask blocks USR2: returned", how);
    check(rc == -1 && err == EINTR, "%s rc=%d errno=%d", label, rc, err);
    /* SIGUSR1 alone in the pass, with SIGUSR2 still blocked by the temporary
     * mask; SIGUSR2 once SIGUSR1's sigreturn puts back the old mask. */
    snprintf(label, sizeof label, "sigsuspend %s, temporary mask blocks USR2", how);
    expect_events(label, "ab");
    snprintf(label, sizeof label, "sigsuspend %s, temporary mask blocks USR2: USR1's handler mask", how);
    expect_event_mask(label, 0, set_of(SIGUSR1, SIGUSR2, 0));
    snprintf(label, sizeof label, "sigsuspend %s, temporary mask blocks USR2: pending in USR1's handler", how);
    expect_event_pending(label, 0, set_of(SIGUSR2, 0, 0));
    snprintf(label, sizeof label, "sigsuspend %s, temporary mask blocks USR2: USR2's handler mask", how);
    expect_event_mask(label, 1, set_of(SIGUSR1, SIGUSR2, 0));
    snprintf(label, sizeof label, "sigsuspend %s, temporary mask blocks USR2: after", how);
    expect_now(label, old, none);
}

/* ----------------------------------------------------------------- hand-off */

static int release_child[2];    /* the child whose SIGCHLD is handed off */
static int release_vfork[2];    /* the vfork child the forking thread waits on */
static int vfork_running[2];
static int release_parked[2];
static pid_t forker_tid;
static pid_t parked_tid;
static volatile int parked_ready;
static volatile int chld_count;
static volatile int usr1_saw_chld_first;
static volatile int usr1_saw_chld_during;
static unsigned usr1_wait_ms;
static volatile int witness_ok;
/* How far the driver had got: the vfork child is let go at DRIVER_DONE. */
enum { DRIVER_CHILD_EXITING = 1, DRIVER_SAW_QUEUED, DRIVER_SENT_USR1, DRIVER_DONE };
static volatile int driver_stage;
static volatile int chld_saw_stage;

/* SIGUSR1's handler blocks SIGCHLD, and waits a while for someone else to run
 * SIGCHLD's. */
static void handoff_usr1(int sig) {
    (void) sig;
    record('a');
    usr1_saw_chld_first = chld_count != 0;
    uint64_t deadline = now_ms() + usr1_wait_ms;
    while (chld_count == 0 && now_ms() < deadline)
        sched_yield();
    usr1_saw_chld_during = chld_count != 0;
}

static void handoff_chld(int sig) {
    (void) sig;
    record('d');
    chld_saw_stage = driver_stage;
    __atomic_fetch_add(&chld_count, 1, __ATOMIC_SEQ_CST);
}

/* A sibling that can take SIGCHLD and nothing else, parked in a read. */
static void *parked(void *arg) {
    (void) arg;
    sigset_t mask;
    sigfillset(&mask);
    sigdelset(&mask, SIGCHLD);
    pthread_sigmask(SIG_SETMASK, &mask, NULL);
    parked_tid = current_tid();
    __atomic_store_n(&parked_ready, 1, __ATOMIC_SEQ_CST);
    char c;
    while (read(release_parked[0], &c, 1) < 0 && errno == EINTR)
        continue;
    return NULL;
}

/* Blocks everything. Lets the child exit while the forking thread is held in
 * its vfork wait, sees the SIGCHLD queued, sends SIGUSR1, lets it go. */
static void *driver(void *arg) {
    (void) arg;
    sigset_t all;
    sigfillset(&all);
    pthread_sigmask(SIG_SETMASK, &all, NULL);
    char c;
    while (read(vfork_running[0], &c, 1) < 0 && errno == EINTR)
        continue;
    driver_stage = DRIVER_CHILD_EXITING;
    if (write(release_child[1], "x", 1) != 1)
        return NULL;
    uint64_t deadline = now_ms() + 5000;
    bool seen = false;
    while (!seen && now_ms() < deadline) {
        sigset_t pending;
        sigpending(&pending);
        seen = sigismember(&pending, SIGCHLD) == 1;
        if (!seen)
            usleep(1000);
    }
    witness_ok = seen;
    driver_stage = DRIVER_SAW_QUEUED;
    syscall(SYS_tgkill, getpid(), forker_tid, SIGUSR1);
    driver_stage = DRIVER_SENT_USR1;
    /* Before the vfork child can go: the forking thread's signals are not
     * looked at until it has. */
    driver_stage = DRIVER_DONE;
    if (write(release_vfork[1], "x", 1) != 1)
        return NULL;
    return NULL;
}

static void scenario_handoff(bool sibling) {
    const char *how = sibling ? "to a sibling" : "no sibling can take it";
    char label[128];
    usr1_wait_ms = sibling ? 3000 : 300;
    chld_count = 0;
    usr1_saw_chld_first = usr1_saw_chld_during = 0;
    witness_ok = 0;
    driver_stage = chld_saw_stage = 0;

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = handoff_usr1;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaddset(&sa.sa_mask, SIGCHLD);
    sigaction(SIGUSR1, &sa, NULL);
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = handoff_chld;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGCHLD, &sa, NULL);

    if (pipe(release_child) != 0 || pipe(release_vfork) != 0 ||
            pipe(vfork_running) != 0 || pipe(release_parked) != 0) {
        printf("FAIL hand-off: pipe: %s\n", strerror(errno));
        failures_total++;
        return;
    }
    forker_tid = current_tid();

    pid_t child = fork();
    if (child == 0) {
        char c;
        while (read(release_child[0], &c, 1) < 0 && errno == EINTR)
            continue;
        _exit(0);
    }

    pthread_t parked_thread, driver_thread;
    if (sibling) {
        pthread_create(&parked_thread, NULL, parked, NULL);
        while (!__atomic_load_n(&parked_ready, __ATOMIC_SEQ_CST))
            usleep(1000);
    }
    pthread_create(&driver_thread, NULL, driver, NULL);

    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);
    events_reset();
    /* No exit signal: this child's own exit must not queue a second SIGCHLD. */
    long vchild = syscall(SYS_clone, CLONE_VFORK, 0, 0, 0, 0);
    if (vchild == 0) {
        char c;
        if (write(vfork_running[1], "x", 1) != 1)
            syscall(SYS_exit_group, 1);
        while (read(release_vfork[0], &c, 1) < 0 && errno == EINTR)
            continue;
        syscall(SYS_exit_group, 0);
    }
    /* SIGUSR1's handler has run by now, on the way out of clone. */
    int saved_nevents = nevents;
    if (vchild < 0) {
        printf("FAIL hand-off %s: clone(CLONE_VFORK): %s\n", how, strerror(errno));
        failures_total++;
    }

    pthread_join(driver_thread, NULL);
    /* The handed-off SIGCHLD has certainly been taken once this returns. */
    uint64_t deadline = now_ms() + 3000;
    while (chld_count == 0 && now_ms() < deadline)
        usleep(1000);

    snprintf(label, sizeof label, "hand-off %s: SIGCHLD seen queued before SIGUSR1 was sent", how);
    check(witness_ok, "%s", label);
    snprintf(label, sizeof label, "hand-off %s: SIGUSR1's handler ran on the way out of clone", how);
    check(saved_nevents >= 1, "%s (%d handlers)", label, saved_nevents);
    snprintf(label, sizeof label, "hand-off %s: SIGCHLD's handler ran once", how);
    check(chld_count == 1, "%s (%d)", label, chld_count);
    /* Taken no sooner than the forking thread's own signals were looked at,
     * which is only once the vfork child has gone: nobody else was told. */
    snprintf(label, sizeof label, "hand-off %s: SIGCHLD's handler ran after the vfork child went", how);
    check(chld_saw_stage == DRIVER_DONE, "%s (driver stage %d)", label, chld_saw_stage);

    int usr1 = -1, chld = -1;
    for (int i = 0; i < nevents && i < MAX_EVENTS; i++) {
        if (events[i] == 'a' && usr1 < 0)
            usr1 = i;
        if (events[i] == 'd' && chld < 0)
            chld = i;
    }
    snprintf(label, sizeof label, "hand-off %s: SIGUSR1's handler ran in the forking thread", how);
    check(usr1 >= 0 && event_tid[usr1] == forker_tid, "%s (tid %d, forker %d)", label,
          usr1 >= 0 ? (int) event_tid[usr1] : -1, (int) forker_tid);
    if (sibling) {
        /* retarget_shared_pending: the sibling takes it while SIGUSR1's
         * handler is still running. It is handed on as SIGUSR1's handler is
         * set up, on Linux as here, so the sibling may even have run SIGCHLD's
         * before SIGUSR1's first line; which one got there first is a race. */
        snprintf(label, sizeof label, "hand-off %s: SIGCHLD's handler ran while SIGUSR1's did", how);
        check(usr1_saw_chld_during, "%s%s", label,
              usr1_saw_chld_first ? " (before its first line)" : "");
        snprintf(label, sizeof label, "hand-off %s: SIGCHLD's handler ran in the sibling", how);
        check(chld >= 0 && event_tid[chld] == parked_tid, "%s (tid %d, sibling %d, forker %d)",
              label, chld >= 0 ? (int) event_tid[chld] : -1, (int) parked_tid, (int) forker_tid);
    } else {
        /* Nobody else can take it: it waits for SIGUSR1's handler to return
         * and runs in the forking thread. */
        snprintf(label, sizeof label, "hand-off %s: SIGCHLD's handler ran after SIGUSR1's", how);
        check(!usr1_saw_chld_first && !usr1_saw_chld_during && usr1 >= 0 && chld > usr1,
              "%s (order \"%s\")", label, events_str());
        snprintf(label, sizeof label, "hand-off %s: SIGCHLD's handler ran in the forking thread", how);
        check(chld >= 0 && event_tid[chld] == forker_tid, "%s (tid %d, forker %d)", label,
              chld >= 0 ? (int) event_tid[chld] : -1, (int) forker_tid);
    }

    int status;
    if (vchild > 0)
        waitpid((pid_t) vchild, &status, __WALL);
    waitpid(child, &status, 0);
    if (sibling) {
        if (write(release_parked[1], "x", 1) != 1)
            printf("hand-off: could not release the parked thread\n");
        pthread_join(parked_thread, NULL);
    }
}

/* ------------------------------------------------------------------ SIGKILL */

static int from_handler[2];

/* Reports whether its mask holds SIGKILL or SIGSTOP, then kills its own
 * process -- which must end it at once, whatever the mask. */
static void kill_self(int sig) {
    (void) sig;
    sigset_t mask;
    pthread_sigmask(SIG_BLOCK, NULL, &mask);
    char held = (char) ('0' + (sigismember(&mask, SIGKILL) == 1) +
            2 * (sigismember(&mask, SIGSTOP) == 1));
    if (write(from_handler[1], &held, 1) != 1)
        _exit(3);
    kill(getpid(), SIGKILL);
    uint64_t until = now_ms() + 300;
    while (now_ms() < until)
        continue;
    if (write(from_handler[1], "x", 1) != 1)
        _exit(3);
}

static void scenario_sigkill(bool unused) {
    (void) unused;
    struct sigaction sa, old;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = kill_self;
    sigfillset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);
    /* Linux drops SIGKILL and SIGSTOP from a handler's mask as it is
     * installed, and reports it so. */
    sigaction(SIGUSR1, NULL, &old);
    check(sigismember(&old.sa_mask, SIGKILL) == 0 && sigismember(&old.sa_mask, SIGSTOP) == 0,
          "sigfillset handler: installed mask holds neither SIGKILL nor SIGSTOP");
    check(sigismember(&old.sa_mask, SIGUSR2) == 1 && sigismember(&old.sa_mask, SIGRTMIN) == 1,
          "sigfillset handler: installed mask holds the rest");

    if (pipe(from_handler) != 0) {
        printf("FAIL sigfillset handler: pipe: %s\n", strerror(errno));
        failures_total++;
        return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        close(from_handler[0]);
        raise(SIGUSR1);
        _exit(0);
    }
    close(from_handler[1]);
    char held = '?', after = 0;
    ssize_t n1 = read(from_handler[0], &held, 1);
    ssize_t n2 = read(from_handler[0], &after, 1);
    int status = 0;
    waitpid(pid, &status, 0);
    check(n1 == 1 && held == '0',
          "sigfillset handler: runs without SIGKILL or SIGSTOP blocked (%c)", held);
    check(n2 == 0, "sigfillset handler: SIGKILL sent from it ends it at once%s",
          n2 == 1 ? " (it ran on)" : "");
    check(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL,
          "sigfillset handler: killed by SIGKILL (status %#x)", (unsigned) status);
}

/* ------------------------------------------------------------------- driver */

static void run(const char *name, void (*fn)(bool), bool arg) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        /* This scenario's own failures, not the ones counted before it. */
        failures_total = 0;
        alarm(test_watchdog_secs(30));
        fn(arg);
        fflush(stdout);
        _exit(failures_total == 0 ? 0 : 1);
    }
    int status = 0;
    if (pid < 0 || waitpid(pid, &status, 0) != pid) {
        printf("FAIL %s: could not run\n", name);
        failures_total++;
        return;
    }
    if (WIFSIGNALED(status)) {
        printf("FAIL %s: killed by signal %d\n", name, WTERMSIG(status));
        failures_total++;
    } else if (WEXITSTATUS(status) != 0) {
        failures_total++;
    }
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    watch_init();
    setvbuf(stdout, NULL, _IOLBF, 0);

    run("sa_mask plain", scenario_samask, false);
    run("sa_mask SA_SIGINFO", scenario_samask, true);
    run("realtime twice", scenario_realtime, false);
    run("realtime twice, SA_NODEFER", scenario_realtime, true);
    run("sigsuspend plain", scenario_sigsuspend, false);
    run("sigsuspend SA_SIGINFO", scenario_sigsuspend, true);
    run("sigsuspend temporary mask, plain", scenario_sigsuspend_temp_blocks, false);
    run("sigsuspend temporary mask, SA_SIGINFO", scenario_sigsuspend_temp_blocks, true);
    run("hand-off to a sibling", scenario_handoff, true);
    run("hand-off, no sibling", scenario_handoff, false);
    run("sigfillset handler", scenario_sigkill, false);

    return finish_suite("signal_handler_mask");
}
