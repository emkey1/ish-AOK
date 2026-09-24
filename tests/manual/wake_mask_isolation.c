/*
 * wake_mask_isolation.c -- one thread's interrupted wait must not change what
 * any OTHER thread can be woken by.
 *
 * Every guest task is a host thread of one host process, and a task parked in
 * a host call is woken by pthread_kill(SIGUSR1) (kernel/signal.c
 * signal_wake_task). The waits in fs/poll.c, fs/sock.c and fs/real.c plant a
 * sigsetjmp point that the SIGUSR1 handler siglongjmps back to. That point
 * used to save the signal mask (sigsetjmp(buf, 1)), taken while SIGUSR1 was
 * blocked, and on Darwin siglongjmp restores a saved mask with sigprocmask --
 * which there sets the mask of EVERY thread in the process, not the caller's.
 * So each wait a guest signal interrupted blocked SIGUSR1 in every host thread
 * of the app, and each of them was deaf to its next poke.
 *
 * Most waits had a second way out -- the SIGUSR2 backup poke, the poll notify
 * pipe, a one-second recheck cap -- so the fault surfaced as lateness that grew
 * with activity, and as the sleep_repairs and poll_repairs counters in
 * /proc/ish/wake_signals, which count threads found with a wake signal blocked.
 * On an M4 iPad after an hour of the regression suite, signal_process_wake_one
 * saw child exits, SIGALRM and handlers arrive one to two seconds late.
 *
 * The witness was first an interval timer's host thread, which had no second
 * way out: setitimer re-armed a running timer by poking its thread with
 * SIGUSR1 alone, so a thread whose SIGUSR1 another thread had blocked slept on
 * to its OLD deadline. That poke is gone -- the timer thread now waits on a
 * descriptor of its own (util/timer.c timer_wake_open), because a poke could
 * also be lost in plain timing (tests/manual/timer_rearm_wake.c) -- and with
 * it that witness. The re-arm is still checked here, but what now says whether
 * a mask was clobbered is a guest task asleep through the interrupts:
 *
 *   1. arm ITIMER_REAL for 30s, and put a sibling to sleep in nanosleep(1s);
 *   2. interrupt another sibling's poll() with a signal, several times --
 *      each is a siglongjmp out of fs/poll.c's wait;
 *   3. re-arm the timer for 200ms. SIGALRM must come at ~200ms, not at 30s;
 *   4. once the sleeper wakes, /proc/ish/wake_signals must count no repair.
 *
 * A sleeping task checks its own host mask after every slice of its sleep
 * (kernel/time.c host_sleep_interruptible) and repairs, and counts, a wake
 * signal it finds blocked there that was unblocked when it lay down. A clobber
 * during the interrupts blocks the sleeper's SIGUSR1 mid-sleep, and the next
 * slice counts it: sleep_repairs rises. The sleeper must be asleep BEFORE the
 * first interrupt, since one that lies down with SIGUSR1 already blocked is
 * not its own to repair and counts nothing. That file is AOK's; where it is
 * missing (Linux) step 4 is skipped.
 *
 * A control round first does 1 and 3 without 2, so a timer that cannot be
 * re-armed at all is not mistaken for this. The sibling's EINTRs are counted,
 * or a poll that was never interrupted would pass for a fixed kernel.
 *
 * Linux has no such coupling between threads; this passes there trivially.
 *
 * Exits 0 and prints "wake_mask_isolation: PASS" on success.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

/* The first arm: long enough that a lost re-arm cannot hide inside it. */
#define LONG_ARM_S 30
/* The re-arm, and how late its SIGALRM may be. A lost re-arm is 30s late. */
#define REARM_MS 200
#define LATE_MS 800
/* Signals sent to the poller, and the gap between them. */
#define INTERRUPTS 20
#define INTERRUPT_GAP_MS 10
/* Rounds of the interrupted case: the first may be lucky. */
#define ROUNDS 3
/* The sleeper's one sleep, which must outlast the interrupts (200ms) and the
 * re-arm (up to 1s), and how long it is given to lie down before them. */
#define SLEEPER_MS 1500
#define SLEEPER_HEAD_START_MS 60

static int pipe_fds[2];
static atomic_int poller_eintrs;
static atomic_int poller_stop;
static atomic_int poller_ready;
static atomic_int sleeper_woke;
static volatile sig_atomic_t alarm_seen;

static void on_usr1(int sig) { (void) sig; }
static void on_alarm(int sig) { (void) sig; alarm_seen = 1; }

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static void sleep_ms(int ms) {
    struct timespec t = {.tv_sec = ms / 1000, .tv_nsec = (long) (ms % 1000) * 1000000L};
    while (nanosleep(&t, &t) < 0 && errno == EINTR)
        ;
}

/* Waits in poll() for ever; each signal ends the wait with EINTR. SIGALRM is
 * blocked so it goes to the main thread. */
static void *poller(void *arg) {
    (void) arg;
    sigset_t alrm;
    sigemptyset(&alrm);
    sigaddset(&alrm, SIGALRM);
    pthread_sigmask(SIG_BLOCK, &alrm, NULL);
    atomic_store(&poller_ready, 1);
    while (!atomic_load(&poller_stop)) {
        struct pollfd p = {.fd = pipe_fds[0], .events = POLLIN};
        int r = poll(&p, 1, -1);
        if (r < 0 && errno == EINTR)
            atomic_fetch_add(&poller_eintrs, 1);
        else if (r > 0)
            break;   /* the stop byte */
    }
    return NULL;
}

/* Sleeps once, SLEEPER_MS, through the interrupts. Blocks everything a test
 * sends, so nothing but a clobbered mask disturbs its sleep. */
static void *sleeper(void *arg) {
    (void) arg;
    sigset_t all;
    sigemptyset(&all);
    sigaddset(&all, SIGALRM);
    sigaddset(&all, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &all, NULL);
    struct timespec t = {.tv_sec = SLEEPER_MS / 1000,
                         .tv_nsec = (long) (SLEEPER_MS % 1000) * 1000000L};
    while (nanosleep(&t, &t) < 0 && errno == EINTR)
        ;
    atomic_store(&sleeper_woke, 1);
    return NULL;
}

/* sleep_repairs + poll_repairs from /proc/ish/wake_signals, or -1 where there
 * is no such file. */
static long wake_repairs(void) {
    FILE *f = fopen("/proc/ish/wake_signals", "r");
    if (f == NULL)
        return -1;
    long total = 0, n;
    int found = 0;
    char line[256];
    while (fgets(line, sizeof(line), f) != NULL) {
        if (sscanf(line, "sleep_repairs %ld", &n) == 1 ||
                sscanf(line, "poll_repairs %ld", &n) == 1) {
            total += n;
            found++;
        }
    }
    fclose(f);
    if (found != 2) {
        printf("FAIL /proc/ish/wake_signals: found %d of its 2 repair counters\n", found);
        failures_total++;
        return -1;
    }
    return total;
}

static void arm_ms(long ms) {
    struct itimerval it = {0};
    it.it_value.tv_sec = ms / 1000;
    it.it_value.tv_usec = (ms % 1000) * 1000;
    if (setitimer(ITIMER_REAL, &it, NULL) != 0) {
        printf("FAIL setitimer: %s\n", strerror(errno));
        failures_total++;
    }
}

/* Re-arm for REARM_MS and wait for the SIGALRM. Returns how long it took, or
 * -1 if it had not come by LATE_MS past the deadline. */
static double rearm_and_time(void) {
    alarm_seen = 0;
    double start = now_ms();
    arm_ms(REARM_MS);
    while (!alarm_seen && now_ms() - start < REARM_MS + LATE_MS)
        sleep_ms(5);
    double took = now_ms() - start;
    arm_ms(0);
    return alarm_seen ? took : -1;
}

static void run_round(const char *label, bool interrupt, pthread_t poller_thread) {
    long repairs_before = wake_repairs();
    atomic_store(&sleeper_woke, 0);
    pthread_t sleeper_thread;
    if (pthread_create(&sleeper_thread, NULL, sleeper, NULL) != 0) {
        printf("FAIL %s: pthread_create (sleeper)\n", label);
        failures_total++;
        return;
    }
    arm_ms(LONG_ARM_S * 1000L);
    /* Let the timer's host thread reach its 30s sleep, and the sleeper its
     * nanosleep. */
    sleep_ms(SLEEPER_HEAD_START_MS);
    int eintrs_before = atomic_load(&poller_eintrs);
    if (interrupt) {
        for (int i = 0; i < INTERRUPTS; i++) {
            pthread_kill(poller_thread, SIGUSR1);
            sleep_ms(INTERRUPT_GAP_MS);
        }
        int got = atomic_load(&poller_eintrs) - eintrs_before;
        if (got == 0) {
            printf("FAIL %s: the poller's poll() was never interrupted (%d signals sent), "
                   "so nothing was tested\n", label, INTERRUPTS);
            failures_total++;
        }
        test_logf("%s: %d of %d signals interrupted the poll\n", label, got, INTERRUPTS);
    }
    double took = rearm_and_time();
    if (took < 0) {
        printf("FAIL %s: SIGALRM re-armed for %dms had not come after %dms -- the timer "
               "slept on to its old %ds deadline\n",
               label, REARM_MS, REARM_MS + LATE_MS, LONG_ARM_S);
        failures_total++;
    } else if (took < REARM_MS - 10) {
        printf("FAIL %s: SIGALRM re-armed for %dms came at %.0fms, early\n",
               label, REARM_MS, took);
        failures_total++;
    } else {
        test_logf("%s: SIGALRM at %.0fms\n", label, took);
    }

    bool woke_early = atomic_load(&sleeper_woke);
    pthread_join(sleeper_thread, NULL);
    if (woke_early) {
        printf("FAIL %s: the sleeper's %dms nanosleep was over before the interrupts "
               "and the re-arm were, so it watched none of them\n", label, SLEEPER_MS);
        failures_total++;
    }
    long repairs_after = wake_repairs();
    if (repairs_before < 0 || repairs_after < 0) {
        test_logf("%s: no /proc/ish/wake_signals, repair count not checked\n", label);
    } else if (repairs_after != repairs_before) {
        printf("FAIL %s: /proc/ish/wake_signals counted %ld thread(s) found with a wake "
               "signal blocked -- a wait's unwind set other threads' masks\n",
               label, repairs_after - repairs_before);
        failures_total++;
    } else {
        test_logf("%s: no wake signal repaired (%ld before and after)\n", label, repairs_before);
    }
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    setvbuf(stdout, NULL, _IONBF, 0);

    struct sigaction sa = {0};
    sa.sa_handler = on_usr1;   /* no SA_RESTART: poll() must see EINTR */
    sigaction(SIGUSR1, &sa, NULL);
    sa.sa_handler = on_alarm;
    sigaction(SIGALRM, &sa, NULL);

    if (pipe(pipe_fds) != 0) {
        printf("FAIL pipe: %s\n", strerror(errno));
        return 1;
    }
    pthread_t t;
    if (pthread_create(&t, NULL, poller, NULL) != 0) {
        printf("FAIL pthread_create\n");
        return 1;
    }
    while (!atomic_load(&poller_ready))
        sleep_ms(1);
    /* Into its poll(). */
    sleep_ms(100);

    run_round("control (no interrupted wait)", false, t);
    for (int r = 1; r <= ROUNDS; r++) {
        char label[64];
        snprintf(label, sizeof(label), "after %d interrupted polls (round %d)", INTERRUPTS, r);
        run_round(label, true, t);
    }

    atomic_store(&poller_stop, 1);
    (void) !write(pipe_fds[1], "x", 1);
    pthread_join(t, NULL);
    return finish_suite("wake_mask_isolation");
}
