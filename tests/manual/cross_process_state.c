// Four things that cross a process boundary, and were all found correct.
//
// This test locks in behaviour rather than fixing it. All four were open
// findings in the kernel conformance audit; each measured as already matching
// Linux when it came up for remediation, and each is subtle enough that a
// future change could quietly undo it with nothing else noticing:
//
//   A ptrace write into a MAP_SHARED file page must land in the SHARED page.
//   The failure mode is silent: the poke converts the page to a private copy,
//   so the debugger's change never reaches the file and -- worse -- the
//   tracee's own later stores go to the copy too, so the file stops tracking
//   the process entirely. gdb writing a breakpoint into a shared mapping is
//   exactly this shape.
//
//   Sharing is a property of the MAPPING, not of when protections change. A
//   PROT_NONE MAP_SHARED anonymous region that each process mprotects to
//   read-write afterwards is still one region; if the mprotect materialised
//   private pages instead, two processes that thought they shared memory
//   would each be talking to themselves.
//
//   process_vm_readv that faults partway through must not disturb the
//   target's lifetime. A reference leaked on the error path would leave the
//   target unreapable -- a permanent zombie -- and the only symptom would be
//   a process table that slowly fills up.
//
//   A session leader's death frees the controlling terminal for the whole
//   session, even when other processes in that session are still alive. If it
//   did not, the pts could never be claimed again and every later login on it
//   would come up with no controlling terminal and no job control.
//
// Measured against x86_64 glibc on Linux 6.12.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "test_common.h"

#define PS 4096

static char base[80];

static void ck(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-58s got=%-10ld want=%ld\n", label, got, want);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(150));
    // A pty hangup on the way out must not take the test with it.
    signal(SIGHUP, SIG_IGN);
    snprintf(base, sizeof base, "/tmp/xproc-%d", (int) getpid());
    char cmd[160];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", base);
    if (system(cmd) < 0)
        failures_total++;
    ck("mkdir base", mkdir(base, 0755), 0);

    // ---- process_vm_readv that faults partway through ---------------------
    {
        int pfd[2];
        ck("pipe", pipe(pfd), 0);
        fflush(NULL);
        pid_t target = fork();
        if (target == 0) {
            close(pfd[0]);
            alarm(30);
            static char payload[PS];
            memset(payload, 'P', sizeof payload);
            void *addr = payload;
            ssize_t w = write(pfd[1], &addr, sizeof addr);
            (void) w;
            usleep(900000);
            _exit(3);
        }
        close(pfd[1]);
        void *remote = NULL;
        ssize_t got = read(pfd[0], &remote, sizeof remote);
        close(pfd[0]);
        ck("the target reported an address", got == (ssize_t) sizeof remote ? 1 : 0, 1);
        if (got == (ssize_t) sizeof remote) {
            char *ok = mmap(NULL, PS, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            char *bad = mmap(NULL, PS, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            struct iovec liov[2] = { { ok, 64 }, { bad, 64 } };
            struct iovec riov = { remote, 128 };
            errno = 0;
            ssize_t n = process_vm_readv(target, liov, 2, &riov, 1, 0);
            // Either the readable part is copied and the count is short, or
            // the whole call is EFAULT. Both are legal; what matters is what
            // happens to the target afterwards.
            ck("a faulting read is short or EFAULT", n < 0 || n < 128 ? 1 : 0, 1);
            if (n > 0)
                ck("  and the bytes it did copy are right",
                   ok[0] == 'P' && ok[63] == 'P' ? 1 : 0, 1);
            munmap(ok, PS);
            munmap(bad, PS);
        }
        // The whole point: the target is still reapable.
        int st;
        pid_t r = waitpid(target, &st, 0);
        ck("the target is reaped normally", r == target ? 1 : 0, 1);
        ck("  with its own exit status", WIFEXITED(st) ? WEXITSTATUS(st) : -1, 3);
    }

    // ---- a ptrace write into a MAP_SHARED file page ------------------------
    {
        char f[160];
        snprintf(f, sizeof f, "%s/shared", base);
        int fd = open(f, O_RDWR | O_CREAT | O_TRUNC, 0644);
        ck("stage a one-page file", fd >= 0 ? 1 : 0, 1);
        if (fd >= 0) {
            char page[PS];
            memset(page, 'A', sizeof page);
            ck("  fill it", (long) write(fd, page, sizeof page), PS);

            int pfd[2];
            ck("pipe", pipe(pfd), 0);
            fflush(NULL);
            pid_t child = fork();
            if (child == 0) {
                close(pfd[0]);
                alarm(40);
                ptrace(PTRACE_TRACEME, 0, NULL, NULL);
                char *m = mmap(NULL, PS, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
                if (m == MAP_FAILED)
                    _exit(90);
                void *addr = m;
                ssize_t w = write(pfd[1], &addr, sizeof addr);
                (void) w;
                raise(SIGSTOP);          // the tracer pokes while we are stopped
                m[8] = 'C';              // ...and our own store must land too
                msync(m, PS, MS_SYNC);
                raise(SIGSTOP);
                _exit(0);
            }
            close(pfd[1]);
            void *remote = NULL;
            ssize_t got = read(pfd[0], &remote, sizeof remote);
            close(pfd[0]);
            int st;
            waitpid(child, &st, WUNTRACED);
            ck("the tracee stopped for us", WIFSTOPPED(st) ? 1 : 0, 1);
            if (got == (ssize_t) sizeof remote) {
                errno = 0;
                long word = ptrace(PTRACE_PEEKDATA, child, remote, NULL);
                ck("PEEKDATA reads the mapping", (long) (word & 0xff), 'A');
                errno = 0;
                ck("POKEDATA into it succeeds",
                   ptrace(PTRACE_POKEDATA, child, remote, (word & ~0xffL) | 'B'), 0);
                ptrace(PTRACE_CONT, child, NULL, 0);
                waitpid(child, &st, WUNTRACED);
                char v0 = '?', v8 = '?';
                if (pread(fd, &v0, 1, 0) != 1) v0 = '?';
                if (pread(fd, &v8, 1, 8) != 1) v8 = '?';
                // The poke reached the FILE: the page was not privatised.
                ck("the poke reached the file", v0, 'B');
                // ...and the tracee's own later store did too, so the mapping
                // kept its file association through the poke.
                ck("and the tracee's own store did too", v8, 'C');
            }
            ptrace(PTRACE_KILL, child, NULL, NULL);
            kill(child, SIGKILL);
            waitpid(child, &st, 0);
            close(fd);
        }
    }

    // ---- a PROT_NONE MAP_SHARED anonymous region stays shared -------------
    {
        char *m = mmap(NULL, PS, PROT_NONE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        ck("map PROT_NONE MAP_SHARED anonymous", m == MAP_FAILED ? 0 : 1, 1);
        if (m != MAP_FAILED) {
            int pfd[2];
            ck("pipe", pipe(pfd), 0);
            fflush(NULL);
            pid_t c = fork();
            if (c == 0) {
                close(pfd[0]);
                alarm(30);
                // The child mprotects FIRST, so if a protection change
                // materialised private pages the parent would get its own.
                if (mprotect(m, PS, PROT_READ | PROT_WRITE) != 0)
                    _exit(91);
                m[0] = 'S';
                char one = 'r';
                ssize_t w = write(pfd[1], &one, 1);
                (void) w;
                usleep(600000);
                _exit(0);
            }
            close(pfd[1]);
            char one;
            ssize_t got = read(pfd[0], &one, 1);
            (void) got;
            close(pfd[0]);
            ck("the parent can mprotect it too", mprotect(m, PS, PROT_READ | PROT_WRITE), 0);
            ck("  and sees the child's write", (long) m[0], 'S');
            // The other direction, on the same region.
            m[1] = 'P';
            int st;
            waitpid(c, &st, 0);
            ck("the child exited cleanly", WIFEXITED(st) ? WEXITSTATUS(st) : -1, 0);
            munmap(m, PS);
        }
    }

    // ---- the controlling terminal after the session leader dies -----------
    {
        int m = posix_openpt(O_RDWR | O_NOCTTY);
        ck("open a pty master", m >= 0 ? 1 : 0, 1);
        if (m >= 0 && grantpt(m) == 0 && unlockpt(m) == 0) {
            char sname[128];
            snprintf(sname, sizeof sname, "%s", ptsname(m));
            int pfd[2];
            ck("pipe", pipe(pfd), 0);
            fflush(NULL);
            pid_t leader = fork();
            if (leader == 0) {
                close(pfd[0]);
                alarm(40);
                signal(SIGHUP, SIG_IGN);
                setsid();
                int s = open(sname, O_RDWR);
                if (s < 0)
                    _exit(90);
                ioctl(s, TIOCSCTTY, 0);
                // A survivor in the same session, in its own group: the
                // session outlives its leader.
                pid_t surv = fork();
                if (surv == 0) {
                    signal(SIGHUP, SIG_IGN);
                    setpgid(0, 0);
                    alarm(30);
                    for (;;)
                        pause();
                }
                char one = 'r';
                ssize_t w = write(pfd[1], &one, 1);
                (void) w;
                usleep(500000);
                _exit(0);
            }
            close(pfd[1]);
            char one;
            ssize_t got = read(pfd[0], &one, 1);
            (void) got;
            close(pfd[0]);
            int st;
            waitpid(leader, &st, 0);
            usleep(800000);

            fflush(NULL);
            pid_t claimer = fork();
            if (claimer == 0) {
                alarm(30);
                signal(SIGHUP, SIG_IGN);
                setsid();
                int s = open(sname, O_RDWR);
                if (s < 0)
                    _exit(90);
                _exit(ioctl(s, TIOCSCTTY, 0) == 0 ? 0 : 1);
            }
            waitpid(claimer, &st, 0);
            // 0 = claimed. 1 would mean the dead leader's session still held
            // it, and no later login on this pts could ever get job control.
            ck("a fresh session can claim the freed pts",
               WIFEXITED(st) ? WEXITSTATUS(st) : -1, 0);
            close(m);
        }
    }

    snprintf(cmd, sizeof cmd, "rm -rf '%s'", base);
    if (system(cmd) < 0)
        failures_total++;
    return finish_suite("cross_process_state");
}
