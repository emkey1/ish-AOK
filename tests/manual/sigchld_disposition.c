// What a parent's SIGCHLD disposition changes about its children.
//
// Four things AOK did not do:
//
//   SIGCHLD == SIG_IGN, and SA_NOCLDWAIT on a handler, both mean "I will never
//   wait" -- so Linux leaves no zombie and the parent's wait() returns ECHILD.
//   AOK left the zombie regardless, so a parent using that very common idiom
//   accumulated one per child for its whole life.
//
//   SA_NOCLDSTOP asks not to be told when a child merely stops OR continues.
//   AOK sent the CLD_STOPPED SIGCHLD anyway (the continue side raised no
//   signal at all until later; see tests/manual/signal_conventions.c).
//
//   PR_SET_CHILD_SUBREAPER was accepted and discarded -- the worst of both,
//   since a service manager set it, believed it, and then lost every orphaned
//   grandchild to init.
//
//   Signal 64 (SIGRTMAX) did not exist: every bound spells a valid signal as
//   1 <= sig < NUM_SIGS and NUM_SIGS was 64.
//
// Measured against x86_64 glibc on Linux 6.12.
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>

#include "test_common.h"

static void check(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-46s got=%ld want=%ld\n", label, got, want);
}
static void nap(long ms) {
    struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&t, NULL);
}
static void set_sigchld(void (*h)(int), int flags) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = h;
    sa.sa_flags = flags;
    sigaction(SIGCHLD, &sa, NULL);
}
static volatile sig_atomic_t hits, last_code;
static void counter(int s) { (void) s; hits++; }
static void info_counter(int s, siginfo_t *si, void *u) {
    (void) s; (void) u; hits++; last_code = si->si_code;
}

// Reap anything outstanding, so one case cannot contaminate the next.
static void drain_children(void) {
    int st;
    while (waitpid(-1, &st, WNOHANG) > 0)
        continue;
}

static void case_autoreap(void) {
    // SIG_IGN: no zombie at all.
    set_sigchld(SIG_IGN, 0);
    fflush(NULL);
    pid_t c = fork();
    if (c == 0) _exit(3);
    nap(400);
    errno = 0;
    int st;
    pid_t w = waitpid(c, &st, 0);
    check("SIG_IGN: waitpid rc", w, -1);
    check("SIG_IGN: errno", w < 0 ? errno : 0, ECHILD);

    // SA_NOCLDWAIT: the handler still runs, but still no zombie.
    hits = 0;
    set_sigchld(counter, SA_NOCLDWAIT);
    fflush(NULL);
    c = fork();
    if (c == 0) _exit(4);
    nap(400);
    errno = 0;
    w = waitpid(c, &st, 0);
    check("SA_NOCLDWAIT: waitpid rc", w, -1);
    check("SA_NOCLDWAIT: errno", w < 0 ? errno : 0, ECHILD);
    check("SA_NOCLDWAIT: handler still ran", hits > 0, 1);

    // Default: the zombie is there to be reaped, as always.
    set_sigchld(SIG_DFL, 0);
    fflush(NULL);
    c = fork();
    if (c == 0) _exit(5);
    nap(300);
    st = 0;
    w = waitpid(c, &st, 0);
    check("SIG_DFL: waitpid returns the child", w == c, 1);
    check("SIG_DFL: exit status", WIFEXITED(st) ? WEXITSTATUS(st) : -1, 5);
    drain_children();
}

static void case_nocldstop(void) {
    // Without the flag a child stop raises SIGCHLD with si_code CLD_STOPPED.
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = info_counter;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGCHLD, &sa, NULL);
    hits = 0; last_code = 0;
    fflush(NULL);
    pid_t c = fork();
    if (c == 0) { for (int i = 0; i < 200; i++) nap(50); _exit(0); }
    nap(300);
    kill(c, SIGSTOP);
    nap(500);
    check("stop raises SIGCHLD", hits > 0, 1);
    check("  with si_code CLD_STOPPED", (long) last_code, CLD_STOPPED);
    int st;
    // wait still reports the stop -- the flag is about the signal, not this.
    check("WUNTRACED still reports it", waitpid(c, &st, WUNTRACED | WNOHANG) == c, 1);
    kill(c, SIGCONT); kill(c, SIGKILL);
    waitpid(c, &st, 0);

    // With SA_NOCLDSTOP, no signal for the stop.
    sa.sa_flags = SA_SIGINFO | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);
    fflush(NULL);
    c = fork();
    if (c == 0) { for (int i = 0; i < 200; i++) nap(50); _exit(0); }
    nap(300);
    // Zeroed HERE rather than before the fork. The SIGCONT above raises a
    // CLD_CONTINUED SIGCHLD from the resumed child's own context -- Linux
    // does the same, from get_signal -- so it can still be in flight when
    // this case starts, and would be counted as the stop notification this
    // case is about. The previous child's exit signal is in the same
    // position.
    hits = 0;
    kill(c, SIGSTOP);
    nap(500);
    check("SA_NOCLDSTOP: no SIGCHLD for the stop", (long) hits, 0);
    check("  but WUNTRACED still reports it",
          waitpid(c, &st, WUNTRACED | WNOHANG) == c, 1);
    kill(c, SIGCONT); kill(c, SIGKILL);
    waitpid(c, &st, 0);
    set_sigchld(SIG_DFL, 0);
    drain_children();
}

static void case_subreaper(void) {
    if (prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) != 0) {
        printf("  subreaper: SKIP (prctl unsupported)\n");
        return;
    }
    fflush(NULL);
    pid_t outer = fork();
    if (outer == 0) {
        pid_t inner = fork();
        if (inner == 0) { nap(600); _exit(9); }
        _exit(0);                        // orphan the grandchild at once
    }
    int st;
    waitpid(outer, &st, 0);
    // As a subreaper the orphan is ours, not init's.
    errno = 0;
    pid_t w = waitpid(-1, &st, 0);
    check("subreaper inherits the orphaned grandchild", w > 0, 1);
    check("  and reaps its exit status", WIFEXITED(st) ? WEXITSTATUS(st) : -1, 9);
    prctl(PR_SET_CHILD_SUBREAPER, 0, 0, 0, 0);
    drain_children();
}

static void case_sigrtmax(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_IGN;
    errno = 0;
    check("sigaction(SIGRTMAX)", sigaction(SIGRTMAX, &sa, NULL), 0);
    errno = 0;
    check("kill(self, SIGRTMAX)", kill(getpid(), SIGRTMAX), 0);
    sa.sa_handler = SIG_DFL;
    sigaction(SIGRTMAX, &sa, NULL);
}

static void case_setname(void) {
    char saved[32] = {0};
    prctl(PR_GET_NAME, saved, 0, 0, 0);
    errno = 0;
    // Longer than the 16-byte comm field: Linux truncates, it does not fail.
    check("PR_SET_NAME(26 chars) rc",
          prctl(PR_SET_NAME, "abcdefghijklmnopqrstuvwxyz", 0, 0, 0), 0);
    char got[32] = {0};
    prctl(PR_GET_NAME, got, 0, 0, 0);
    check("  truncated to 15 chars", (long) strlen(got), 15);
    prctl(PR_SET_NAME, saved, 0, 0, 0);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(180));
    case_autoreap();
    case_nocldstop();
    case_subreaper();
    case_sigrtmax();
    case_setname();
    return finish_suite("sigchld_disposition");
}
