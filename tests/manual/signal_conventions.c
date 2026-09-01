// Signal and wait behaviours that reported the wrong thing, or nothing.
//
//   A resume is a reportable event in its own right. The SIGCHLD that carries
//   it -- si_code CLD_CONTINUED -- is how a shell learns that a job it
//   backgrounded is running again. AOK woke a WCONTINUED waiter but sent no
//   signal at all, so a parent's handler never fired and only a wait ever
//   noticed. The stop half already worked.
//
//   SIGIO defaulted to ignore. Linux's default-ignore set is exactly SIGCHLD,
//   SIGCONT, SIGURG and SIGWINCH; a process that asked for async I/O
//   notification and then failed to handle it is terminated. Treating it as
//   ignored let such a process keep running as if nothing had happened.
//
//   rt_sigaction accepted signal 0, which has no disposition to set or read,
//   and reported success for a call that did nothing.
//
//   sigaltstack accepted arbitrary ss_flags, installing a stack from a
//   request it had not understood.
//
//   waitid accepted options naming none of WEXITED/WSTOPPED/WCONTINUED --
//   a call that can never report anything -- and answered ECHILD, telling the
//   caller it had no children when the real problem was its own argument.
//
//   kill(-1) returned EPERM. Linux never reports EPERM for the broadcast
//   form: 0 whenever at least one process was considered, even if every send
//   was denied, and ESRCH only when nothing matched at all.
//
// Measured against x86_64 glibc on Linux 6.12.
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_common.h"

static void ck(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-58s got=%-10ld want=%ld\n", label, got, want);
}

static volatile sig_atomic_t last_code, chld_count;
static void chld_handler(int s, siginfo_t *si, void *u) {
    (void) s; (void) u;
    last_code = si->si_code;
    chld_count++;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(90));

    // ---- CLD_STOPPED and CLD_CONTINUED -----------------------------------
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = chld_handler;
        sa.sa_flags = SA_SIGINFO;
        ck("install a SA_SIGINFO SIGCHLD handler", sigaction(SIGCHLD, &sa, NULL), 0);

        fflush(NULL);
        pid_t c = fork();
        if (c == 0) {
            alarm(30);
            for (;;)
                pause();
        }
        usleep(300000);
        last_code = 0;
        ck("stop the child", kill(c, SIGSTOP), 0);
        usleep(400000);
        ck("  the handler saw CLD_STOPPED", last_code, CLD_STOPPED);
        last_code = 0;
        ck("continue it", kill(c, SIGCONT), 0);
        usleep(400000);
        // The one that was missing: a resume raised no SIGCHLD at all.
        ck("  the handler saw CLD_CONTINUED", last_code, CLD_CONTINUED);
        kill(c, SIGKILL);
        int st;
        waitpid(c, &st, 0);
    }

    // ---- SA_NOCLDSTOP suppresses both halves -----------------------------
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = chld_handler;
        sa.sa_flags = SA_SIGINFO | SA_NOCLDSTOP;
        ck("re-install it with SA_NOCLDSTOP", sigaction(SIGCHLD, &sa, NULL), 0);
        fflush(NULL);
        pid_t c = fork();
        if (c == 0) {
            alarm(30);
            for (;;)
                pause();
        }
        usleep(300000);
        chld_count = 0;
        kill(c, SIGSTOP);
        usleep(400000);
        kill(c, SIGCONT);
        usleep(400000);
        // The flag is about stop AND continue, not stops alone.
        ck("no SIGCHLD for the stop or the continue", (long) chld_count, 0);
        kill(c, SIGKILL);
        int st;
        waitpid(c, &st, 0);
        // ...but the exit still raises one, because the flag is about stops.
        usleep(300000);
        ck("  the exit still raised SIGCHLD", chld_count > 0 ? 1 : 0, 1);
        signal(SIGCHLD, SIG_DFL);
    }

    // ---- SIGIO terminates by default --------------------------------------
    {
        fflush(NULL);
        pid_t c = fork();
        if (c == 0) {
            alarm(10);
            raise(SIGIO);
            _exit(9);      // reached only if SIGIO was ignored
        }
        int st;
        waitpid(c, &st, 0);
        ck("SIGIO kills a default-disposition process", WIFSIGNALED(st) ? 1 : 0, 1);
        ck("  with SIGIO itself", WIFSIGNALED(st) ? WTERMSIG(st) : 0, SIGIO);
        // ...while the genuinely-ignored ones still are.
        fflush(NULL);
        c = fork();
        if (c == 0) {
            alarm(10);
            raise(SIGWINCH);
            raise(SIGURG);
            raise(SIGCHLD);
            _exit(9);
        }
        waitpid(c, &st, 0);
        ck("SIGWINCH/SIGURG/SIGCHLD are still ignored",
           WIFEXITED(st) ? WEXITSTATUS(st) : -1, 9);
    }

    // ---- argument validation ---------------------------------------------
    {
        errno = 0;
        long r = syscall(SYS_rt_sigaction, 0, NULL, NULL, 8);
        ck("rt_sigaction on signal 0 is EINVAL", r < 0 ? errno : 0, EINVAL);
        // ...and a real signal still works, so it is a check on 0 and not a
        // blanket refusal.
        struct sigaction old;
        ck("  a real signal still queries", sigaction(SIGUSR1, NULL, &old), 0);

        // SIGSTKSZ is not a constant on modern glibc (it reads a hwcap), so
        // the buffer is sized to a fixed value comfortably above every
        // platform's MINSIGSTKSZ.
        static char stackmem[65536];
        stack_t ss;
        memset(&ss, 0, sizeof ss);
        ss.ss_sp = stackmem;
        ss.ss_size = sizeof stackmem;
        ss.ss_flags = 0x1234;      // nothing defined
        errno = 0;
        ck("sigaltstack with unknown ss_flags is EINVAL",
           sigaltstack(&ss, NULL) < 0 ? errno : 0, EINVAL);
        // The defined values still work.
        ss.ss_flags = 0;
        ck("  ss_flags 0 installs the stack", sigaltstack(&ss, NULL), 0);
        memset(&ss, 0, sizeof ss);
        ss.ss_flags = SS_DISABLE;
        ck("  and SS_DISABLE removes it", sigaltstack(&ss, NULL), 0);

        siginfo_t si;
        memset(&si, 0, sizeof si);
        errno = 0;
        r = syscall(SYS_waitid, P_ALL, 0, &si, WNOHANG, NULL);
        ck("waitid with no W* flag is EINVAL", r < 0 ? errno : 0, EINVAL);
        ck("  and specifically not ECHILD", r < 0 && errno == ECHILD ? 1 : 0, 0);
        // With a W* flag and no children it is ECHILD, which is the answer
        // that was previously given to both.
        memset(&si, 0, sizeof si);
        errno = 0;
        r = syscall(SYS_waitid, P_ALL, 0, &si, WEXITED | WNOHANG, NULL);
        ck("waitid with WEXITED and no children is ECHILD", r < 0 ? errno : 0, ECHILD);
    }

    // ---- kill(-1) -----------------------------------------------------------
    {
        // Two live children we may certainly signal, so the answer does not
        // depend on how many other processes the system happens to have.
        fflush(NULL);
        pid_t a = fork();
        if (a == 0) { alarm(20); pause(); _exit(0); }
        pid_t b = fork();
        if (b == 0) { alarm(20); pause(); _exit(0); }
        usleep(400000);
        errno = 0;
        long r = kill(-1, 0);
        ck("kill(-1, 0) with live children succeeds", r, 0);
        ck("  and never reports EPERM", r < 0 && errno == EPERM ? 1 : 0, 0);
        kill(a, SIGKILL);
        kill(b, SIGKILL);
        int st;
        waitpid(a, &st, 0);
        waitpid(b, &st, 0);
    }

    return finish_suite("signal_conventions");
}
