// ptrace_strace_startup: ptrace driven the way strace 6 drives it, one step at
// a time. Every expectation here was measured on Linux 6.12 (x86_64, glibc
// 2.41, -m64 and -m32) before AOK was changed, by running strace itself under
// strace on that machine and by this program.
//
// startup_case
//   strace starts its program stopped (the child raises SIGSTOP), seizes it
//   with TRACESYSGOOD|TRACEEXEC|TRACEEXIT, interrupts it, and sends the
//   SIGCONT itself. Linux then reports, every time: the group-stop
//   (0x80137f), which strace answers with PTRACE_LISTEN; a PTRACE_EVENT_STOP
//   (0x80057f), because the SIGCONT arrived while the tracee sat in that stop
//   (JOBCTL_TRAP_NOTIFY); the SIGCONT's own signal-delivery-stop (0x127f); and
//   only then execve's entry stop, its PTRACE_EVENT_EXEC and its exit stop.
//   AOK had no notify flag: in 12 of 40 runs the tracee ran on from the LISTEN
//   without a single syscall stop, straight through its execve, and strace
//   printed "Stray PTRACE_EVENT_EXEC", hid everything it traced until an
//   execve it never saw, and `strace -c` printed no summary at all. In 26 of
//   the other 28 the SIGCONT got there before the tracee had reported its
//   group-stop, which Linux's attach waits for (JOBCTL_TRAPPING). And no
//   SIGCONT was ever reported: a traced task dropped a signal it ignores.
//
// listen_after_sigcont_case
//   The same race, made to happen every time: the SIGCONT is sent after the
//   group-stop is reported and before PTRACE_LISTEN.
//
// syscall_info_case
//   PTRACE_GET_SYSCALL_INFO, the way strace's own start-up self-test asks it:
//   NONE at a signal stop, then ENTRY and EXIT for chdir(""), gettid and
//   exit_group, with every argument. AOK did not have the request at all.
//
// sigcont_notify_case
//   A SIGCONT reaching a seized tracee that is not stopped -- asleep, or
//   sitting in a syscall stop -- still owes its tracer a PTRACE_EVENT_STOP,
//   before the SIGCONT is delivered.
//
// unknown_request_case
//   A request the kernel does not know is EIO for a stopped tracee (and ESRCH
//   for anything else). AOK answered EPERM.
//
// attach_stopped_permission_case
//   The attach to a stopped task, which now waits for the task's trap, still
//   refuses another uid's process first (ptrace_may_access, 2026-09-24).
//
// Every wait4 here also asks for rusage, and a stop's must be the tracee's
// own usage (Linux's wait_task_stopped fills it with getrusage(RUSAGE_BOTH)).
// AOK copied out an uninitialised struct: host stack bytes, which is where
// strace -c's 134-second syscalls came from.
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "test_common.h"

#ifndef __WALL
#define __WALL 0x40000000
#endif
#ifndef PTRACE_SETOPTIONS
#define PTRACE_SETOPTIONS 0x4200
#endif
#ifndef PTRACE_GETEVENTMSG
#define PTRACE_GETEVENTMSG 0x4201
#endif
#ifndef PTRACE_GETSIGINFO
#define PTRACE_GETSIGINFO 0x4202
#endif
#ifndef PTRACE_SEIZE
#define PTRACE_SEIZE 0x4206
#endif
#ifndef PTRACE_INTERRUPT
#define PTRACE_INTERRUPT 0x4207
#endif
#ifndef PTRACE_LISTEN
#define PTRACE_LISTEN 0x4208
#endif
#define PTRACE_GET_SYSCALL_INFO_ 0x420e
#ifndef PTRACE_O_TRACESYSGOOD
#define PTRACE_O_TRACESYSGOOD 0x01
#endif
#ifndef PTRACE_O_TRACEEXEC
#define PTRACE_O_TRACEEXEC 0x10
#endif
#ifndef PTRACE_O_TRACEEXIT
#define PTRACE_O_TRACEEXIT 0x40
#endif
#ifndef PTRACE_EVENT_EXEC
#define PTRACE_EVENT_EXEC 4
#endif
#ifndef PTRACE_EVENT_EXIT
#define PTRACE_EVENT_EXIT 6
#endif
#ifndef PTRACE_EVENT_STOP
#define PTRACE_EVENT_STOP 128
#endif

#define STOP_STATUS(sig, event) (((event) << 16) | ((sig) << 8) | 0x7f)
#define STRACE_OPTIONS (PTRACE_O_TRACESYSGOOD | PTRACE_O_TRACEEXEC | PTRACE_O_TRACEEXIT)

// musl i386 names the 32-bit-time syscalls apart; none of these are those, but
// an old root's headers may lack gettid's or exit_group's name altogether.
#if !defined(SYS_gettid) && defined(__NR_gettid)
#define SYS_gettid __NR_gettid
#endif
#if !defined(SYS_exit_group) && defined(__NR_exit_group)
#define SYS_exit_group __NR_exit_group
#endif

// struct ptrace_syscall_info (linux/ptrace.h), spelled out: an old root's
// headers predate it.
struct psi {
    uint8_t op;
    uint8_t reserved;
    uint16_t flags;
    uint32_t arch;
    uint64_t instruction_pointer;
    uint64_t stack_pointer;
    union {
        struct {
            uint64_t nr;
            uint64_t args[6];
        } entry;
        struct {
            int64_t rval;
            uint8_t is_error;
        } exit;
        struct {
            uint64_t nr;
            uint64_t args[6];
            uint32_t ret_data;
        } seccomp;
    };
};
#define PSI_NONE 0
#define PSI_ENTRY 1
#define PSI_EXIT 2
#define PSI_SIZE_NONE 24
#define PSI_SIZE_ENTRY 80
#define PSI_SIZE_EXIT 33

#if defined(__x86_64__)
#define EXPECTED_ARCH 0xc000003eu
#elif defined(__i386__)
#define EXPECTED_ARCH 0x40000003u
#elif defined(__aarch64__)
#define EXPECTED_ARCH 0xc00000b7u
#elif defined(__riscv) && __riscv_xlen == 64
#define EXPECTED_ARCH 0xc00000f3u
#else
#define EXPECTED_ARCH 0u
#endif

#define EXEC_TARGET_ARG "--ptrace-strace-startup-target"
#define TARGET_EXIT_CODE 7

static char self_path[PATH_MAX];

static void nap(long ms) {
    struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };
    while (nanosleep(&t, &t) != 0 && errno == EINTR)
        ;
}

static void on_alarm(int sig) { (void) sig; }

// sigaction without SA_RESTART: the alarm exists to break a wait that would
// otherwise hang, and signal() would restart it.
static void install_watchdog(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_alarm;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);
}

static void check(const char *name, const char *what, uint64_t got, uint64_t want) {
    char label[200];
    snprintf(label, sizeof label, "%s: %s", name, what);
    if (got != want)
        failf(label, got, 0, 0, want, 0, 0);
    else
        test_logf("  ok   %-72s %#" PRIx64 "\n", label, got);
}

static void kill_and_reap(pid_t c) {
    if (c <= 0)
        return;
    kill(c, SIGKILL);
    int st;
    alarm(test_watchdog_secs(10));
    while (waitpid(c, &st, __WALL) < 0 && errno == EINTR)
        ;
    alarm(0);
}

// A wait4 that also checks the rusage it reports. A stop, a continue or an
// exit all fill it with the tracee's usage: sane times, and a peak RSS a live
// or just-exited process really has. What AOK wrote for a stop was whatever
// was on its host stack, so the buffer starts poisoned and the check is that
// something plausible replaced it.
static pid_t wait_checked(const char *name, pid_t pid, int *status, time_t started) {
    struct rusage ru;
    memset(&ru, 0x5a, sizeof ru);
    alarm(test_watchdog_secs(10));
    pid_t got = wait4(pid, status, __WALL, &ru);
    int saved = errno;
    alarm(0);
    if (got <= 0) {
        errno = saved;
        return got;
    }
    long elapsed = (long) (time(NULL) - started) + 2;
    bool sane = ru.ru_utime.tv_usec >= 0 && ru.ru_utime.tv_usec < 1000000 &&
        ru.ru_stime.tv_usec >= 0 && ru.ru_stime.tv_usec < 1000000 &&
        ru.ru_utime.tv_sec >= 0 && ru.ru_utime.tv_sec <= elapsed &&
        ru.ru_stime.tv_sec >= 0 && ru.ru_stime.tv_sec <= elapsed &&
        ru.ru_maxrss > 0 && ru.ru_maxrss < 64L * 1024 * 1024;
    if (!sane) {
        char what[120];
        snprintf(what, sizeof what, "wait4 rusage for status %#x is the tracee's", *status);
        failf(what, (uint64_t) ru.ru_utime.tv_sec, (uint64_t) ru.ru_stime.tv_sec,
              (uint64_t) ru.ru_maxrss, 0, 0, 1);
        (void) name;
    }
    return got;
}

static long psi_get(pid_t pid, struct psi *info, size_t size) {
    memset(info, 0xa5, sizeof *info);
    return ptrace(PTRACE_GET_SYSCALL_INFO_, pid, (void *) size, info);
}

// ---- startup_case ------------------------------------------------------------

// What strace's startup_child does, from fork to the first stop it acts on.
static pid_t start_stopped_child(const char *name) {
    fflush(NULL);
    pid_t c = fork();
    if (c == 0) {
        kill(getpid(), SIGSTOP);
        execl(self_path, self_path, EXEC_TARGET_ARG, (char *) NULL);
        _exit(127);
    }
    if (c < 0) {
        check(name, "fork", (uint64_t) errno, 0);
        return -1;
    }
    int st = 0;
    alarm(test_watchdog_secs(10));
    pid_t w = waitpid(c, &st, WSTOPPED);
    alarm(0);
    check(name, "the child stopped itself", (uint64_t) (w == c ? st : -1), STOP_STATUS(SIGSTOP, 0));
    return c;
}

static void startup_case(const char *name, int iterations) {
    for (int iter = 0; iter < iterations; iter++) {
        time_t started = time(NULL);
        pid_t c = start_stopped_child(name);
        if (c < 0)
            return;
        if (ptrace(PTRACE_SEIZE, c, 0, (void *) (long) STRACE_OPTIONS) != 0) {
            check(name, "PTRACE_SEIZE", (uint64_t) errno, 0);
            kill_and_reap(c);
            return;
        }
        if (ptrace(PTRACE_INTERRUPT, c, 0, 0) != 0)
            check(name, "PTRACE_INTERRUPT", (uint64_t) errno, 0);
        kill(c, SIGCONT);

        // The first stops, in order: 0x80137f, 0x80057f, 0x127f, then
        // execve's entry, PTRACE_EVENT_EXEC and execve's exit.
        static const int expected[] = {
            STOP_STATUS(SIGSTOP, PTRACE_EVENT_STOP),
            STOP_STATUS(SIGTRAP, PTRACE_EVENT_STOP),
            STOP_STATUS(SIGCONT, 0),
            STOP_STATUS(SIGTRAP | 0x80, 0),
            STOP_STATUS(SIGTRAP, PTRACE_EVENT_EXEC),
            STOP_STATUS(SIGTRAP | 0x80, 0),
        };
        const int nexpected = (int) (sizeof expected / sizeof expected[0]);
        bool in_syscall = false, in_getppid = false;
        bool first_mismatch_reported = false;
        int step = 0, getppid_calls = 0, exit_status = -1;
        bool saw_exit_event = false, done = false;
        for (int guard = 0; guard < 4000 && !done; guard++) {
            int st = 0;
            pid_t w = wait_checked(name, -1, &st, started);
            if (w != c) {
                check(name, "wait4(-1, __WALL) names the tracee", (uint64_t) w, (uint64_t) c);
                break;
            }
            if (WIFEXITED(st) || WIFSIGNALED(st)) {
                exit_status = st;
                done = true;
                break;
            }
            if (step < nexpected && st != expected[step] && !first_mismatch_reported) {
                char what[80];
                snprintf(what, sizeof what, "stop %d of the start-up (run %d)", step, iter);
                check(name, what, (uint64_t) st, (uint64_t) expected[step]);
                first_mismatch_reported = true;
            }
            int sig = WSTOPSIG(st), event = (unsigned) st >> 16;
            int op = PTRACE_SYSCALL, inject = 0;
            unsigned long msg = 0x5eed;
            ptrace(PTRACE_GETEVENTMSG, c, 0, &msg);
            if (event == PTRACE_EVENT_STOP) {
                if (sig == SIGSTOP) {
                    siginfo_t si;
                    memset(&si, 0, sizeof si);
                    ptrace(PTRACE_GETSIGINFO, c, 0, &si);
                    if (iter == 0)
                        check(name, "group-stop si_code", (uint64_t) (unsigned) si.si_code,
                              (PTRACE_EVENT_STOP << 8) | SIGSTOP);
                    op = PTRACE_LISTEN;
                }
            } else if (event == PTRACE_EVENT_EXEC) {
                if (iter == 0)
                    check(name, "PTRACE_EVENT_EXEC message", (uint64_t) msg, (uint64_t) c);
                // strace's "Stray PTRACE_EVENT_EXEC": the event arriving
                // outside execve.
                if (iter == 0 || !in_syscall)
                    check(name, "PTRACE_EVENT_EXEC comes inside execve", in_syscall, true);
            } else if (event == PTRACE_EVENT_EXIT) {
                saw_exit_event = true;
                if (iter == 0)
                    check(name, "PTRACE_EVENT_EXIT message", (uint64_t) msg,
                          (uint64_t) (TARGET_EXIT_CODE << 8));
            } else if (sig == (SIGTRAP | 0x80)) {
                // Entry and exit alternate, and the message says which.
                uint64_t want_msg = in_syscall ? 2 : 1;
                if (msg != want_msg && !first_mismatch_reported) {
                    check(name, "syscall stop message alternates", (uint64_t) msg, want_msg);
                    first_mismatch_reported = true;
                }
                struct psi info;
                long size = psi_get(c, &info, sizeof info);
                if (!in_syscall) {
                    if (step == 3 && iter == 0) {
                        check(name, "execve entry: GET_SYSCALL_INFO size", (uint64_t) size, PSI_SIZE_ENTRY);
                        check(name, "execve entry: op", info.op, PSI_ENTRY);
                        check(name, "execve entry: nr", info.entry.nr, (uint64_t) SYS_execve);
                        check(name, "execve entry: arch", info.arch, EXPECTED_ARCH);
                    }
                    // The target's own getppid, decoded as strace decodes
                    // every call: its number at the entry...
                    in_getppid = size == PSI_SIZE_ENTRY && info.op == PSI_ENTRY &&
                        info.entry.nr == (uint64_t) SYS_getppid;
                } else {
                    if (step == 5 && iter == 0) {
                        check(name, "execve exit: GET_SYSCALL_INFO size", (uint64_t) size, PSI_SIZE_EXIT);
                        check(name, "execve exit: op", info.op, PSI_EXIT);
                        check(name, "execve exit: is_error", info.exit.is_error, 0);
                        check(name, "execve exit: rval", (uint64_t) info.exit.rval, 0);
                    }
                    // ...and its value, the tracer, at the exit.
                    if (in_getppid && size == PSI_SIZE_EXIT && info.op == PSI_EXIT &&
                            !info.exit.is_error && info.exit.rval == (int64_t) getpid())
                        getppid_calls++;
                    in_getppid = false;
                }
                in_syscall = !in_syscall;
            } else if (event == 0) {
                if (sig == SIGCONT && iter == 0) {
                    siginfo_t si;
                    memset(&si, 0, sizeof si);
                    ptrace(PTRACE_GETSIGINFO, c, 0, &si);
                    check(name, "SIGCONT's si_code", (uint64_t) (unsigned) si.si_code, SI_USER);
                    check(name, "SIGCONT's si_pid", (uint64_t) si.si_pid, (uint64_t) getpid());
                }
                inject = sig;
            }
            step++;
            if (ptrace(op, c, 0, (void *) (long) inject) != 0 && errno != ESRCH)
                check(name, "resume", (uint64_t) errno, 0);
        }
        check(name, "the tracee exited as the program it exec'd",
              (uint64_t) exit_status, (uint64_t) (TARGET_EXIT_CODE << 8));
        if (iter == 0) {
            check(name, "PTRACE_EVENT_EXIT was reported", saw_exit_event, true);
            check(name, "the target's getppid was decoded, entry and exit", (uint64_t) getppid_calls, 1);
        }
        if (!done)
            kill_and_reap(c);
        if (failures_total != 0)
            return;     // one run's worth of detail is enough
    }
}

// ---- listen_after_sigcont_case --------------------------------------------------

static void listen_after_sigcont_case(const char *name) {
    time_t started = time(NULL);
    pid_t c = start_stopped_child(name);
    if (c < 0)
        return;
    if (ptrace(PTRACE_SEIZE, c, 0, (void *) (long) STRACE_OPTIONS) != 0) {
        check(name, "PTRACE_SEIZE", (uint64_t) errno, 0);
        kill_and_reap(c);
        return;
    }
    int st = 0;
    pid_t w = wait_checked(name, c, &st, started);
    check(name, "the group-stop is reported", (uint64_t) (w == c ? st : -1),
          STOP_STATUS(SIGSTOP, PTRACE_EVENT_STOP));
    // The SIGCONT lands while the tracee sits in that stop, before the LISTEN.
    kill(c, SIGCONT);
    nap(50);
    check(name, "PTRACE_LISTEN", (uint64_t) (ptrace(PTRACE_LISTEN, c, 0, 0) == 0 ? 0 : errno), 0);
    w = wait_checked(name, c, &st, started);
    check(name, "LISTEN after the SIGCONT re-traps at once",
          (uint64_t) (w == c ? st : -1), STOP_STATUS(SIGTRAP, PTRACE_EVENT_STOP));
    siginfo_t si;
    memset(&si, 0, sizeof si);
    ptrace(PTRACE_GETSIGINFO, c, 0, &si);
    check(name, "its si_code", (uint64_t) (unsigned) si.si_code, (PTRACE_EVENT_STOP << 8) | SIGTRAP);
    check(name, "its si_pid is the tracee", (uint64_t) si.si_pid, (uint64_t) c);
    // Then the SIGCONT itself, and then execve -- nothing ran in between.
    ptrace(PTRACE_SYSCALL, c, 0, 0);
    w = wait_checked(name, c, &st, started);
    check(name, "then the SIGCONT's delivery stop", (uint64_t) (w == c ? st : -1), STOP_STATUS(SIGCONT, 0));
    ptrace(PTRACE_SYSCALL, c, 0, (void *) (long) SIGCONT);
    w = wait_checked(name, c, &st, started);
    check(name, "then execve's entry stop", (uint64_t) (w == c ? st : -1), STOP_STATUS(SIGTRAP | 0x80, 0));
    kill_and_reap(c);
}

// ---- syscall_info_case -----------------------------------------------------------

static const unsigned long dummy_args[6] = {
    (unsigned long) 0xdad0bef0bad0fed0ULL, (unsigned long) 0xdad1bef1bad1fed1ULL,
    (unsigned long) 0xdad2bef2bad2fed2ULL, (unsigned long) 0xdad3bef3bad3fed3ULL,
    (unsigned long) 0xdad4bef4bad4fed4ULL, (unsigned long) 0xdad5bef5bad5fed5ULL,
};

static void check_entry(const char *name, const char *call, pid_t c, long nr, const unsigned long *args) {
    struct psi info;
    long size = psi_get(c, &info, sizeof info);
    char what[120];
    snprintf(what, sizeof what, "%s entry: size", call);
    check(name, what, (uint64_t) size, PSI_SIZE_ENTRY);
    snprintf(what, sizeof what, "%s entry: op", call);
    check(name, what, info.op, PSI_ENTRY);
    snprintf(what, sizeof what, "%s entry: arch", call);
    check(name, what, info.arch, EXPECTED_ARCH);
    snprintf(what, sizeof what, "%s entry: instruction and stack pointers", call);
    check(name, what, info.instruction_pointer != 0 && info.stack_pointer != 0, true);
    snprintf(what, sizeof what, "%s entry: nr", call);
    check(name, what, info.entry.nr, (uint64_t) nr);
    for (int i = 0; i < 6; i++) {
        snprintf(what, sizeof what, "%s entry: args[%d]", call, i);
        check(name, what, info.entry.args[i], (uint64_t) args[i]);
    }
}

static void check_exit(const char *name, const char *call, pid_t c, int is_error, long rval) {
    struct psi info;
    long size = psi_get(c, &info, sizeof info);
    char what[120];
    snprintf(what, sizeof what, "%s exit: size", call);
    check(name, what, (uint64_t) size, PSI_SIZE_EXIT);
    snprintf(what, sizeof what, "%s exit: op", call);
    check(name, what, info.op, PSI_EXIT);
    snprintf(what, sizeof what, "%s exit: is_error", call);
    check(name, what, info.exit.is_error, (uint64_t) is_error);
    snprintf(what, sizeof what, "%s exit: rval", call);
    check(name, what, (uint64_t) info.exit.rval, (uint64_t) (int64_t) rval);
}

static void syscall_info_case(const char *name) {
    time_t started = time(NULL);
    fflush(NULL);
    pid_t c = fork();
    if (c == 0) {
        pid_t self = getpid();
        if (ptrace(PTRACE_TRACEME, 0, 0, 0) != 0)
            _exit(126);
        kill(self, SIGSTOP);
        syscall(SYS_chdir, "", dummy_args[1], dummy_args[2], dummy_args[3], dummy_args[4], dummy_args[5]);
        syscall(SYS_gettid, dummy_args[0], dummy_args[1], dummy_args[2], dummy_args[3], dummy_args[4], dummy_args[5]);
        syscall(SYS_exit_group, 0, dummy_args[1], dummy_args[2], dummy_args[3], dummy_args[4], dummy_args[5]);
        _exit(1);
    }
    int st = 0;
    pid_t w = wait_checked(name, c, &st, started);
    check(name, "the SIGSTOP", (uint64_t) (w == c ? st : -1), STOP_STATUS(SIGSTOP, 0));

    // At a signal-delivery-stop: NONE, and only as much as the buffer holds.
    struct psi info;
    long size = psi_get(c, &info, sizeof info);
    check(name, "signal stop: size", (uint64_t) size, PSI_SIZE_NONE);
    check(name, "signal stop: op", info.op, PSI_NONE);
    check(name, "signal stop: arch", info.arch, EXPECTED_ARCH);
    check(name, "signal stop: instruction and stack pointers",
          info.instruction_pointer != 0 && info.stack_pointer != 0, true);
    unsigned char buf[sizeof(struct psi)];
    memset(buf, 0xa5, sizeof buf);
    size = ptrace(PTRACE_GET_SYSCALL_INFO_, c, (void *) 4, buf);
    check(name, "a 4-byte buffer: the size it would have taken", (uint64_t) size, PSI_SIZE_NONE);
    check(name, "a 4-byte buffer: nothing past it written", buf[4] == 0xa5 && buf[23] == 0xa5, true);

    if (ptrace(PTRACE_SETOPTIONS, c, 0, (void *) (long) PTRACE_O_TRACESYSGOOD) != 0)
        check(name, "PTRACE_SETOPTIONS", (uint64_t) errno, 0);
    unsigned long chdir_args[6];
    memcpy(chdir_args, dummy_args, sizeof chdir_args);
    // The path is a pointer the child made; any non-zero value will do, and
    // the check below reads it from the stop itself.
    unsigned long gettid_args[6];
    memcpy(gettid_args, dummy_args, sizeof gettid_args);
    unsigned long exit_args[6];
    memcpy(exit_args, dummy_args, sizeof exit_args);
    exit_args[0] = 0;

    for (int stop = 1; stop <= 6; stop++) {
        ptrace(PTRACE_SYSCALL, c, 0, 0);
        w = wait_checked(name, c, &st, started);
        if (stop == 6) {
            check(name, "the child exited 0", (uint64_t) (w == c ? st : -1), 0);
            return;
        }
        if (w != c || st != STOP_STATUS(SIGTRAP | 0x80, 0)) {
            check(name, "a syscall stop", (uint64_t) (w == c ? st : -1), STOP_STATUS(SIGTRAP | 0x80, 0));
            kill_and_reap(c);
            return;
        }
        switch (stop) {
            case 1: {
                struct psi peek;
                psi_get(c, &peek, sizeof peek);
                chdir_args[0] = (unsigned long) peek.entry.args[0];
                check(name, "chdir entry: the path is a pointer", chdir_args[0] != 0, true);
                check_entry(name, "chdir", c, SYS_chdir, chdir_args);
                break;
            }
            case 2: check_exit(name, "chdir", c, 1, -ENOENT); break;
            case 3: check_entry(name, "gettid", c, SYS_gettid, gettid_args); break;
            case 4: check_exit(name, "gettid", c, 0, c); break;
            case 5: check_entry(name, "exit_group", c, SYS_exit_group, exit_args); break;
        }
    }
}

// ---- sigcont_notify_case ----------------------------------------------------------

static pid_t spawn_sleeper(void) {
    fflush(NULL);
    pid_t c = fork();
    if (c == 0) {
        for (;;)
            nap(20);
    }
    return c;
}

static void expect_stop(const char *name, const char *what, pid_t c, int want, time_t started) {
    int st = 0;
    pid_t w = wait_checked(name, c, &st, started);
    check(name, what, (uint64_t) (w == c ? st : -1), (uint64_t) want);
}

static void sigcont_notify_case(const char *name) {
    time_t started = time(NULL);
    // Asleep: the SIGCONT's PTRACE_EVENT_STOP, then the SIGCONT.
    pid_t c = spawn_sleeper();
    nap(50);
    if (ptrace(PTRACE_SEIZE, c, 0, 0) != 0) {
        check(name, "PTRACE_SEIZE", (uint64_t) errno, 0);
        kill_and_reap(c);
        return;
    }
    nap(50);
    kill(c, SIGCONT);
    expect_stop(name, "asleep: the notice first", c, STOP_STATUS(SIGTRAP, PTRACE_EVENT_STOP), started);
    ptrace(PTRACE_CONT, c, 0, 0);
    expect_stop(name, "asleep: then the SIGCONT", c, STOP_STATUS(SIGCONT, 0), started);
    ptrace(PTRACE_CONT, c, 0, (void *) (long) SIGCONT);
    // Nothing else is owed: an interrupt is the next thing reported.
    nap(50);
    ptrace(PTRACE_INTERRUPT, c, 0, 0);
    expect_stop(name, "asleep: nothing more", c, STOP_STATUS(SIGTRAP, PTRACE_EVENT_STOP), started);
    kill_and_reap(c);

    // At a syscall-entry stop: the syscall's exit stop, then the notice, then
    // the SIGCONT.
    c = spawn_sleeper();
    nap(50);
    ptrace(PTRACE_SEIZE, c, 0, (void *) (long) PTRACE_O_TRACESYSGOOD);
    ptrace(PTRACE_INTERRUPT, c, 0, 0);
    expect_stop(name, "syscall stop: the interrupt", c, STOP_STATUS(SIGTRAP, PTRACE_EVENT_STOP), started);
    ptrace(PTRACE_SYSCALL, c, 0, 0);
    expect_stop(name, "syscall stop: an entry", c, STOP_STATUS(SIGTRAP | 0x80, 0), started);
    unsigned long msg = 0;
    ptrace(PTRACE_GETEVENTMSG, c, 0, &msg);
    check(name, "syscall stop: it is an entry", (uint64_t) msg, 1);
    kill(c, SIGCONT);
    nap(50);
    ptrace(PTRACE_SYSCALL, c, 0, 0);
    expect_stop(name, "syscall stop: the exit", c, STOP_STATUS(SIGTRAP | 0x80, 0), started);
    ptrace(PTRACE_SYSCALL, c, 0, 0);
    expect_stop(name, "syscall stop: then the notice", c, STOP_STATUS(SIGTRAP, PTRACE_EVENT_STOP), started);
    ptrace(PTRACE_SYSCALL, c, 0, 0);
    expect_stop(name, "syscall stop: then the SIGCONT", c, STOP_STATUS(SIGCONT, 0), started);
    kill_and_reap(c);
}

// ---- unknown_request_case ------------------------------------------------------------

static void unknown_request_case(const char *name) {
    time_t started = time(NULL);
    pid_t c = spawn_sleeper();
    nap(50);
    ptrace(PTRACE_SEIZE, c, 0, 0);
    ptrace(PTRACE_INTERRUPT, c, 0, 0);
    expect_stop(name, "stopped", c, STOP_STATUS(SIGTRAP, PTRACE_EVENT_STOP), started);
    errno = 0;
    long r = ptrace(0x42ff, c, 0, 0);
    check(name, "unknown request to a stopped tracee: EIO", (uint64_t) (r == -1 ? errno : 0), EIO);
    kill_and_reap(c);
    pid_t other = spawn_sleeper();
    errno = 0;
    r = ptrace(0x42ff, other, 0, 0);
    check(name, "unknown request to a task we do not trace: ESRCH",
          (uint64_t) (r == -1 ? errno : 0), ESRCH);
    kill_and_reap(other);
}

// ---- attach_stopped_permission_case -------------------------------------------------
//
// The attach to a stopped task now waits for its trap -- and the permission
// check (ptrace_may_access, Linux's __ptrace_may_access with
// PTRACE_MODE_ATTACH_REALCREDS) must still come first. A stopped process of
// another uid is EPERM to SEIZE and ATTACH alike; one of the tracer's own uid,
// dumpable, is seized and reports its group-stop to a tracer that is not its
// parent. Needs root to make the two uids, and is skipped without it.

#define OTHER_UID 2346
#define TRACER_UID 2345

static void attach_stopped_permission_case(const char *name) {
    if (getuid() != 0) {
        test_logf("  skip %s: needs root to set up two uids\n", name);
        return;
    }
    for (int same_uid = 0; same_uid <= 1; same_uid++) {
        uid_t target_uid = same_uid ? TRACER_UID : OTHER_UID;
        fflush(NULL);
        pid_t target = fork();
        if (target == 0) {
            if (setgid(target_uid) != 0 || setuid(target_uid) != 0)
                _exit(126);
            // Dropping from root leaves a task undumpable, on Linux too;
            // dumpable again, so that only the uid decides.
            prctl(PR_SET_DUMPABLE, 1);
            kill(getpid(), SIGSTOP);
            _exit(0);
        }
        int st = 0;
        alarm(test_watchdog_secs(10));
        pid_t w = waitpid(target, &st, WSTOPPED);
        alarm(0);
        if (w != target || st != STOP_STATUS(SIGSTOP, 0)) {
            check(name, "the target stopped itself", (uint64_t) st, STOP_STATUS(SIGSTOP, 0));
            kill_and_reap(target);
            continue;
        }
        fflush(NULL);
        pid_t tracer = fork();
        if (tracer == 0) {
            if (setgid(TRACER_UID) != 0 || setuid(TRACER_UID) != 0)
                _exit(126);
            if (!same_uid) {
                long seize = ptrace(PTRACE_SEIZE, target, 0, 0);
                int seize_errno = errno;
                long attach = ptrace(PTRACE_ATTACH, target, 0, 0);
                int attach_errno = errno;
                _exit(seize == -1 && seize_errno == EPERM &&
                      attach == -1 && attach_errno == EPERM ? 0 : 1);
            }
            if (ptrace(PTRACE_SEIZE, target, 0, 0) != 0)
                _exit(2);
            int tst = 0;
            alarm(10);
            pid_t tw = waitpid(target, &tst, __WALL);
            alarm(0);
            if (tw != target || tst != STOP_STATUS(SIGSTOP, PTRACE_EVENT_STOP))
                _exit(3);
            _exit(ptrace(PTRACE_DETACH, target, 0, 0) == 0 ? 0 : 4);
        }
        alarm(test_watchdog_secs(20));
        w = waitpid(tracer, &st, 0);
        alarm(0);
        check(name, same_uid ? "same uid: seized, and its group-stop reported" :
                               "another uid: SEIZE and ATTACH refused with EPERM",
              (uint64_t) (w == tracer ? st : -1), 0);
        kill(target, SIGCONT);
        kill_and_reap(target);
    }
}

int main(int argc, char **argv) {
    // The program strace's tracee execs: a syscall the tracer can pick out,
    // and a known exit status.
    if (argc > 1 && strcmp(argv[1], EXEC_TARGET_ARG) == 0) {
        syscall(SYS_getppid);
        _exit(TARGET_EXIT_CODE);
    }
    test_init(argc, argv);
    install_watchdog();
    ssize_t n = readlink("/proc/self/exe", self_path, sizeof self_path - 1);
    if (n <= 0) {
        printf("ptrace_strace_startup: FAIL cannot read /proc/self/exe\n");
        return 1;
    }
    self_path[n] = '\0';

    startup_case("startup", 30);
    listen_after_sigcont_case("listen after SIGCONT");
    syscall_info_case("GET_SYSCALL_INFO");
    sigcont_notify_case("SIGCONT notice");
    unknown_request_case("unknown request");
    attach_stopped_permission_case("attach to a stopped task: permission");
    return finish_suite("ptrace_strace_startup");
}
