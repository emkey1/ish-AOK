// ptrace_eventmsg: PTRACE_GETEVENTMSG must return the message of the stop the
// tracee is in -- above all, the new task's pid at a clone, fork or vfork event.
//
// gdb could not `run` any program that created a thread or forked:
//
//     ./gdb/linux-nat.c:2054: internal-error: wait returned unexpected PID 18
//
// At PTRACE_EVENT_CLONE/FORK/VFORK gdb reads the new task's pid with
// PTRACE_GETEVENTMSG and then waits for that task's first stop. AOK answered 0,
// so gdb called waitpid(0, ...), which is "any child in my process group", and
// got a different child back. kernel/fork.c did hand the pid to
// ptrace_event_stop, but wait4 zeroed ptrace.eventmsg as it reported the stop.
// A tracer only learns that an event happened by waiting, so no tracer could
// ever read anything but 0. The exec and exit messages were lost the same way.
//
// Linux sets the message in ptrace_stop() at EVERY stop and wait never touches
// it. An event-stop records its event's value; a signal-delivery-stop and a
// group-stop record 0; a syscall stop records PTRACE_EVENTMSG_SYSCALL_ENTRY or
// _EXIT (1 or 2, since Linux 5.3). The sequence case runs each kind of stop
// straight after one with a different message, so a value left over from the
// previous stop fails as surely as a cleared one.
//
// The clone, fork, vfork and exec cases are the tracer that found the bug:
// PTRACE_TRACEME plus raise(SIGSTOP), PTRACE_SETOPTIONS, wait for the event,
// PTRACE_GETEVENTMSG, then waitpid(<that pid>, __WALL), which Linux answers with
// that pid and the new task's SIGSTOP stop, 0x137f.
//
// Fixing the message let gdb run such programs, and showed it still could not
// debug one that vforks: AOK never reported PTRACE_EVENT_VFORK_DONE, which gdb
// waits for. The vfork-done cases cover that.
//
// Every expectation here was measured on Linux 6.12 (x86_64, -m64 and -m32)
// before it was checked against AOK.
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>

#define TEST_WANT_RAW_VFORK   // musl powerpc64: the system call, not a fork
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
#ifndef PTRACE_SEIZE
#define PTRACE_SEIZE 0x4206
#endif
#ifndef PTRACE_INTERRUPT
#define PTRACE_INTERRUPT 0x4207
#endif
#ifndef PTRACE_O_TRACESYSGOOD
#define PTRACE_O_TRACESYSGOOD 0x01
#endif
#ifndef PTRACE_O_TRACEFORK
#define PTRACE_O_TRACEFORK 0x02
#endif
#ifndef PTRACE_O_TRACEVFORK
#define PTRACE_O_TRACEVFORK 0x04
#endif
#ifndef PTRACE_O_TRACECLONE
#define PTRACE_O_TRACECLONE 0x08
#endif
#ifndef PTRACE_O_TRACEEXEC
#define PTRACE_O_TRACEEXEC 0x10
#endif
#ifndef PTRACE_O_TRACEVFORKDONE
#define PTRACE_O_TRACEVFORKDONE 0x20
#endif
#ifndef PTRACE_O_TRACEEXIT
#define PTRACE_O_TRACEEXIT 0x40
#endif
#ifndef PTRACE_EVENT_FORK
#define PTRACE_EVENT_FORK 1
#endif
#ifndef PTRACE_EVENT_VFORK
#define PTRACE_EVENT_VFORK 2
#endif
#ifndef PTRACE_EVENT_CLONE
#define PTRACE_EVENT_CLONE 3
#endif
#ifndef PTRACE_EVENT_EXEC
#define PTRACE_EVENT_EXEC 4
#endif
#ifndef PTRACE_EVENT_VFORK_DONE
#define PTRACE_EVENT_VFORK_DONE 5
#endif
#ifndef PTRACE_EVENT_EXIT
#define PTRACE_EVENT_EXIT 6
#endif
#ifndef PTRACE_EVENT_STOP
#define PTRACE_EVENT_STOP 128
#endif

// <linux/ptrace.h>'s PTRACE_EVENTMSG_SYSCALL_ENTRY/EXIT. Not every libc's
// <sys/ptrace.h> has them, and the two headers cannot both be included.
#define EVENTMSG_SYSCALL_ENTRY 1
#define EVENTMSG_SYSCALL_EXIT  2

// The status word waitpid reports for a stop.
#define STOP_STATUS(sig, event) (((event) << 16) | ((sig) << 8) | 0x7f)

// argv[1] of the image the exec case runs: exit 0 at once.
#define EXEC_TARGET_ARG "--ptrace-eventmsg-exec-target"

// Written by any GETEVENTMSG that fails to write, and recorded by no stop.
#define UNWRITTEN 0x5eedUL

static const char *self_exe = "/proc/self/exe";
static int report_pipe[2] = { -1, -1 };

static void nap(long ms) {
    struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&t, NULL);
}

static void on_alarm(int sig) { (void) sig; }

// No SA_RESTART: the alarm exists to break a wait that would otherwise hang,
// and a restarted wait hangs just the same.
static void install_watchdog(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_alarm;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);
}

static pid_t wait_for(pid_t pid, int *status) {
    alarm(test_watchdog_secs(10));
    pid_t got = waitpid(pid, status, __WALL);
    int saved = errno;
    alarm(0);
    errno = saved;
    return got;
}

static unsigned long read_eventmsg(pid_t pid) {
    unsigned long msg = UNWRITTEN;
    if (ptrace(PTRACE_GETEVENTMSG, pid, 0, &msg) != 0)
        test_logf("    PTRACE_GETEVENTMSG(%d): %s\n", (int) pid, strerror(errno));
    return msg;
}

static void check(const char *name, const char *what, uint64_t got, uint64_t want) {
    char label[160];
    snprintf(label, sizeof label, "%s: %s", name, what);
    if (got != want)
        failf(label, got, 0, 0, want, 0, 0);
    else
        test_logf("  ok   %-58s %#" PRIx64 "\n", label, got);
}

// SIGKILL the tracee and reap every task still reporting to us, including
// traced threads and grandchildren, so the next case starts clean.
static void kill_and_reap(pid_t c) {
    if (c > 0)
        kill(c, SIGKILL);
    int st;
    while (wait_for(-1, &st) > 0)
        ;
}

static void report_id(pid_t id) {
    if (write(report_pipe[1], &id, sizeof id) != (ssize_t) sizeof id)
        _exit(97);
}

// The pid or tid the tracee reported through report_pipe, or 0.
static pid_t read_reported_id(void) {
    pid_t id = 0;
    if (read(report_pipe[0], &id, sizeof id) != (ssize_t) sizeof id)
        id = 0;
    return id;
}

// Fork a child that asks to be traced and stops itself, which is how gdb
// starts the program it runs. `body` runs once the tracer continues it, and
// what it returns is the child's exit status.
static pid_t spawn_traceme(int (*body)(void)) {
    fflush(NULL);
    pid_t c = fork();
    if (c == 0) {
        if (report_pipe[0] >= 0)
            close(report_pipe[0]);
        if (ptrace(PTRACE_TRACEME, 0, 0, 0) != 0)
            _exit(96);
        raise(SIGSTOP);
        _exit(body());
    }
    return c;
}

// Every TRACEME tracee's first stop is its own SIGSTOP: a
// signal-delivery-stop, whose message is 0.
static int initial_stop(const char *name, pid_t c) {
    int st = 0;
    pid_t got = wait_for(c, &st);
    if (got != c) {
        check(name, "first wait returns the tracee", (uint64_t) got, (uint64_t) c);
        return 0;
    }
    check(name, "first stop is SIGSTOP", (uint64_t) st, STOP_STATUS(SIGSTOP, 0));
    if (st != STOP_STATUS(SIGSTOP, 0))
        return 0;
    check(name, "message at the SIGSTOP stop", read_eventmsg(c), 0);
    return 1;
}

static void *clone_worker(void *arg) {
    report_id((pid_t) syscall(SYS_gettid));
    return arg;
}

static int body_clone(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, clone_worker, NULL) != 0)
        return 91;
    pthread_join(t, NULL);
    return 0;
}

static int body_fork(void) {
    pid_t p = fork();
    if (p < 0)
        return 92;
    if (p == 0)
        _exit(0);
    report_id(p);
    int st;
    return waitpid(p, &st, 0) == p ? 0 : 93;
}

static int body_vfork(void) {
    pid_t p = vfork();
    if (p < 0)
        return 94;
    if (p == 0)
        _exit(0);
    report_id(p);
    int st;
    return waitpid(p, &st, 0) == p ? 0 : 95;
}

// One clone, fork or vfork under a tracer with all three options set, as the
// tracer in the report did. The event's message must name the new task: a
// waitpid on it returns it, in its SIGSTOP stop, and it is the id the tracee
// itself saw.
static void event_case(const char *name, int (*body)(void), int want_event) {
    if (pipe(report_pipe) != 0) {
        check(name, "pipe", (uint64_t) errno, 0);
        return;
    }
    pid_t c = spawn_traceme(body);
    close(report_pipe[1]);
    if (c < 0) {
        check(name, "fork", (uint64_t) errno, 0);
        close(report_pipe[0]);
        return;
    }
    if (!initial_stop(name, c)) {
        kill_and_reap(c);
        close(report_pipe[0]);
        return;
    }
    long opts = PTRACE_O_TRACECLONE | PTRACE_O_TRACEFORK | PTRACE_O_TRACEVFORK;
    if (ptrace(PTRACE_SETOPTIONS, c, 0, (void *) opts) != 0 ||
            ptrace(PTRACE_CONT, c, 0, 0) != 0) {
        check(name, "SETOPTIONS + CONT", (uint64_t) errno, 0);
        kill_and_reap(c);
        close(report_pipe[0]);
        return;
    }

    // The new task can report its first stop before its creator reports the
    // event. Hold it stopped, so the event can still be checked against it.
    pid_t held = 0;
    int held_status = 0;
    int events = 0;
    pid_t announced = 0;
    int exit_status = -1;
    for (int i = 0; i < 64; i++) {
        int st = 0;
        pid_t w = wait_for(-1, &st);
        if (w < 0) {
            check(name, "wait (EINTR here is a hang)", (uint64_t) errno, 0);
            break;
        }
        if (w != c) {
            if (WIFSTOPPED(st) && events == 0 && held == 0) {
                held = w;
                held_status = st;
            } else if (WIFSTOPPED(st)) {
                ptrace(PTRACE_CONT, w, 0, 0);
            }
            continue;
        }
        if (WIFEXITED(st) || WIFSIGNALED(st)) {
            exit_status = st;
            break;
        }
        int event = (st >> 16) & 0xff;
        if (event == want_event && events++ == 0) {
            check(name, "event stop status", (uint64_t) st, STOP_STATUS(SIGTRAP, want_event));
            unsigned long msg = read_eventmsg(c);
            // A tracer may ask more than once; reading must not consume it.
            check(name, "message is the same on a second read", read_eventmsg(c), msg);
            announced = (pid_t) msg;
            if (announced <= 0 || announced == c || msg == UNWRITTEN) {
                // Deliberately no waitpid(<message>): with a message of 0 that
                // is waitpid(0, ...), which is precisely what broke gdb. The
                // comparison with the id the tracee saw, below, fails it.
                test_logf("  %s: message %#lx does not name a new task\n", name, msg);
                if (held != 0) {
                    ptrace(PTRACE_CONT, held, 0, 0);
                    held = 0;
                }
            } else if (held == announced) {
                check(name, "new task's first stop (arrived first)", (uint64_t) held_status,
                      STOP_STATUS(SIGSTOP, 0));
                ptrace(PTRACE_CONT, held, 0, 0);
                held = 0;
            } else {
                int st2 = 0;
                pid_t got = wait_for(announced, &st2);
                check(name, "waitpid(<message>, __WALL) returns it", (uint64_t) got,
                      (uint64_t) announced);
                if (got == announced) {
                    check(name, "new task's first stop", (uint64_t) st2, STOP_STATUS(SIGSTOP, 0));
                    ptrace(PTRACE_CONT, got, 0, 0);
                }
                if (held != 0) {
                    ptrace(PTRACE_CONT, held, 0, 0);
                    held = 0;
                }
            }
            ptrace(PTRACE_CONT, c, 0, 0);
            continue;
        }
        // Anything else: keep it running, passing on real signals only.
        int sig = WSTOPSIG(st);
        int deliver = (event != 0 || sig == SIGTRAP || sig == SIGSTOP) ? 0 : sig;
        ptrace(PTRACE_CONT, c, 0, (void *) (long) deliver);
    }
    if (held != 0)
        ptrace(PTRACE_CONT, held, 0, 0);

    check(name, "event stops seen", (uint64_t) events, 1);
    check(name, "tracee exit status", (uint64_t) exit_status, 0);
    // Only once every writer is gone, so the read cannot block.
    kill_and_reap(exit_status == -1 ? c : 0);
    pid_t reported = read_reported_id();
    check(name, "tracee reported the new task's id", (uint64_t) (reported > 0), 1);
    check(name, "message == the id the tracee saw", (uint64_t) announced, (uint64_t) reported);
    close(report_pipe[0]);
    report_pipe[0] = -1;
}

// PTRACE_EVENT_VFORK_DONE: the vfork parent stops again once the child has
// exited or exec'd and no longer shares its memory, and the message is the
// child's pid. gdb removes its breakpoints from a vfork parent and puts them
// back only on this event, running nothing but the vforking thread until then.
// AOK never reported it, so under gdb a breakpoint after a vfork never fired
// and a threaded program that vforked hung. Linux reports it whenever the
// parent has PTRACE_O_TRACEVFORKDONE, whether or not the child is traced too.
static void vfork_done_case(const char *name, long opts) {
    if (pipe(report_pipe) != 0) {
        check(name, "pipe", (uint64_t) errno, 0);
        return;
    }
    pid_t c = spawn_traceme(body_vfork);
    close(report_pipe[1]);
    if (c < 0) {
        check(name, "fork", (uint64_t) errno, 0);
        close(report_pipe[0]);
        return;
    }
    if (!initial_stop(name, c)) {
        kill_and_reap(c);
        close(report_pipe[0]);
        return;
    }
    if (ptrace(PTRACE_SETOPTIONS, c, 0, (void *) opts) != 0 ||
            ptrace(PTRACE_CONT, c, 0, 0) != 0) {
        check(name, "SETOPTIONS + CONT", (uint64_t) errno, 0);
        kill_and_reap(c);
        close(report_pipe[0]);
        return;
    }

    int dones = 0;
    unsigned long msg = UNWRITTEN;
    int exit_status = -1;
    for (int i = 0; i < 64; i++) {
        int st = 0;
        pid_t w = wait_for(-1, &st);
        if (w < 0) {
            check(name, "wait (EINTR here is a hang)", (uint64_t) errno, 0);
            break;
        }
        if (WIFEXITED(st) || WIFSIGNALED(st)) {
            if (w == c) {
                exit_status = st;
                break;
            }
            continue;
        }
        int event = (st >> 16) & 0xff;
        if (w == c && event == PTRACE_EVENT_VFORK_DONE && dones++ == 0) {
            check(name, "vfork-done stop status", (uint64_t) st,
                  STOP_STATUS(SIGTRAP, PTRACE_EVENT_VFORK_DONE));
            msg = read_eventmsg(c);
        }
        int sig = WSTOPSIG(st);
        int deliver = (event != 0 || sig == SIGTRAP || sig == SIGSTOP) ? 0 : sig;
        ptrace(PTRACE_CONT, w, 0, (void *) (long) deliver);
    }

    check(name, "vfork-done stops seen", (uint64_t) dones, 1);
    check(name, "tracee exit status", (uint64_t) exit_status, 0);
    kill_and_reap(exit_status == -1 ? c : 0);
    pid_t reported = read_reported_id();
    check(name, "tracee reported the child's pid", (uint64_t) (reported > 0), 1);
    check(name, "message is the child's pid", msg, (uint64_t) reported);
    close(report_pipe[0]);
    report_pipe[0] = -1;
}

static int body_exec(void) {
    char *const argv[] = { (char *) self_exe, (char *) EXEC_TARGET_ARG, NULL };
    execv(self_exe, argv);
    return 98;
}

// PTRACE_EVENT_EXEC's message is the pid the task had before the exec.
static void exec_case(void) {
    const char *name = "exec";
    pid_t c = spawn_traceme(body_exec);
    if (c < 0) {
        check(name, "fork", (uint64_t) errno, 0);
        return;
    }
    if (!initial_stop(name, c)) {
        kill_and_reap(c);
        return;
    }
    if (ptrace(PTRACE_SETOPTIONS, c, 0, (void *) (long) PTRACE_O_TRACEEXEC) != 0 ||
            ptrace(PTRACE_CONT, c, 0, 0) != 0) {
        check(name, "SETOPTIONS + CONT", (uint64_t) errno, 0);
        kill_and_reap(c);
        return;
    }
    int st = 0;
    pid_t got = wait_for(c, &st);
    check(name, "wait returns the tracee", (uint64_t) got, (uint64_t) c);
    if (got != c) {
        kill_and_reap(c);
        return;
    }
    check(name, "exec event stop status", (uint64_t) st, STOP_STATUS(SIGTRAP, PTRACE_EVENT_EXEC));
    check(name, "message is the pid", read_eventmsg(c), (uint64_t) c);
    ptrace(PTRACE_CONT, c, 0, 0);
    got = wait_for(c, &st);
    check(name, "exec'd image exits 0", (uint64_t) st, 0);
    kill_and_reap(got == c ? 0 : c);
}

static void *exec_worker(void *arg) {
    report_id((pid_t) syscall(SYS_gettid));
    body_exec();
    return arg;
}

static int body_thread_exec(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, exec_worker, NULL) != 0)
        return 91;
    for (;;)
        pause();
}

// The same event from a thread that is not the leader. That thread takes over
// the leader's pid, so the stop is reported under the leader's pid, and the
// message is the thread id it had before the exec. AOK sent the pid it had
// afterwards, which told the tracer nothing it did not already know.
static void thread_exec_case(void) {
    const char *name = "exec from a thread";
    if (pipe(report_pipe) != 0) {
        check(name, "pipe", (uint64_t) errno, 0);
        return;
    }
    pid_t c = spawn_traceme(body_thread_exec);
    close(report_pipe[1]);
    if (c < 0) {
        check(name, "fork", (uint64_t) errno, 0);
        close(report_pipe[0]);
        return;
    }
    if (!initial_stop(name, c)) {
        kill_and_reap(c);
        close(report_pipe[0]);
        return;
    }
    long opts = PTRACE_O_TRACECLONE | PTRACE_O_TRACEEXEC;
    if (ptrace(PTRACE_SETOPTIONS, c, 0, (void *) opts) != 0 ||
            ptrace(PTRACE_CONT, c, 0, 0) != 0) {
        check(name, "SETOPTIONS + CONT", (uint64_t) errno, 0);
        kill_and_reap(c);
        close(report_pipe[0]);
        return;
    }

    int execs = 0;
    unsigned long msg = UNWRITTEN;
    int exit_status = -1;
    for (int i = 0; i < 64; i++) {
        int st = 0;
        pid_t w = wait_for(-1, &st);
        if (w < 0) {
            check(name, "wait (EINTR here is a hang)", (uint64_t) errno, 0);
            break;
        }
        if (WIFEXITED(st) || WIFSIGNALED(st)) {
            if (w == c) {
                exit_status = st;
                break;
            }
            continue;
        }
        int event = (st >> 16) & 0xff;
        if (event == PTRACE_EVENT_EXEC && execs++ == 0) {
            check(name, "exec event is reported under the leader's pid", (uint64_t) w,
                  (uint64_t) c);
            check(name, "exec event stop status", (uint64_t) st,
                  STOP_STATUS(SIGTRAP, PTRACE_EVENT_EXEC));
            msg = read_eventmsg(w);
        }
        int sig = WSTOPSIG(st);
        int deliver = (event != 0 || sig == SIGTRAP || sig == SIGSTOP) ? 0 : sig;
        ptrace(PTRACE_CONT, w, 0, (void *) (long) deliver);
    }

    check(name, "exec event stops seen", (uint64_t) execs, 1);
    check(name, "tracee exit status", (uint64_t) exit_status, 0);
    kill_and_reap(exit_status == -1 ? c : 0);
    pid_t reported = read_reported_id();
    check(name, "thread reported a tid of its own", (uint64_t) (reported > 0 && reported != c), 1);
    check(name, "message is the thread's tid before the exec", msg, (uint64_t) reported);
    close(report_pipe[0]);
    report_pipe[0] = -1;
}

static void on_usr1(int sig) { (void) sig; }

static int body_sequence(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_usr1;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);
    raise(SIGUSR1);
    // Wider than the byte an exit code keeps, so the exit event's message
    // shows whether the kernel dropped the rest.
    return 0x1ff;
}

// Each stop overwrites the previous one's message, whatever the kind:
//   SIGSTOP (0) -> syscall entry (1) -> syscall exit (2) -> SIGUSR1 (0)
//   -> exit event (the wait status: exit(0x1ff) is 0xff00, as for wait)
static void sequence_case(void) {
    const char *name = "sequence";
    pid_t c = spawn_traceme(body_sequence);
    if (c < 0) {
        check(name, "fork", (uint64_t) errno, 0);
        return;
    }
    if (!initial_stop(name, c)) {
        kill_and_reap(c);
        return;
    }
    long opts = PTRACE_O_TRACESYSGOOD | PTRACE_O_TRACEEXIT;
    if (ptrace(PTRACE_SETOPTIONS, c, 0, (void *) opts) != 0) {
        check(name, "SETOPTIONS", (uint64_t) errno, 0);
        kill_and_reap(c);
        return;
    }

    int st = 0;
    ptrace(PTRACE_SYSCALL, c, 0, 0);
    if (wait_for(c, &st) != c)
        goto hung;
    check(name, "syscall-entry stop status", (uint64_t) st, STOP_STATUS(SIGTRAP | 0x80, 0));
    check(name, "message at syscall entry", read_eventmsg(c), EVENTMSG_SYSCALL_ENTRY);

    ptrace(PTRACE_SYSCALL, c, 0, 0);
    if (wait_for(c, &st) != c)
        goto hung;
    check(name, "syscall-exit stop status", (uint64_t) st, STOP_STATUS(SIGTRAP | 0x80, 0));
    check(name, "message at syscall exit", read_eventmsg(c), EVENTMSG_SYSCALL_EXIT);

    ptrace(PTRACE_CONT, c, 0, 0);
    if (wait_for(c, &st) != c)
        goto hung;
    check(name, "SIGUSR1 stop status", (uint64_t) st, STOP_STATUS(SIGUSR1, 0));
    check(name, "message at a signal-delivery-stop", read_eventmsg(c), 0);

    ptrace(PTRACE_CONT, c, 0, (void *) (long) SIGUSR1);
    if (wait_for(c, &st) != c)
        goto hung;
    check(name, "exit event stop status", (uint64_t) st, STOP_STATUS(SIGTRAP, PTRACE_EVENT_EXIT));
    check(name, "message at the exit event", read_eventmsg(c), 0xff00);

    ptrace(PTRACE_CONT, c, 0, 0);
    if (wait_for(c, &st) != c)
        goto hung;
    check(name, "exit status", (uint64_t) st, 0xff00);
    kill_and_reap(0);
    return;

hung:
    check(name, "wait (EINTR here is a hang)", (uint64_t) errno, 0);
    kill_and_reap(c);
}

// A seized tracee's PTRACE_INTERRUPT stop and group-stop are both
// PTRACE_EVENT_STOPs, and neither carries a message: Linux's do_jobctl_trap
// passes 0. AOK used to record the stop signal as the group-stop's message,
// which nothing could see while wait cleared it.
static void seize_case(void) {
    const char *name = "seize";
    int go[2];
    if (pipe(go) != 0) {
        check(name, "pipe", (uint64_t) errno, 0);
        return;
    }
    fflush(NULL);
    pid_t c = fork();
    if (c == 0) {
        close(go[1]);
        char ch;
        // Not retried on EINTR. Linux restarts a read that PTRACE_INTERRUPT
        // broke into once the tracer resumes the task, and so does AOK now; see
        // ptrace_stop_restart and ptrace_seize_trap_stop.
        if (read(go[0], &ch, 1) != 1)
            _exit(99);
        raise(SIGSTOP);
        for (;;)
            nap(10);
    }
    close(go[0]);
    if (c < 0) {
        check(name, "fork", (uint64_t) errno, 0);
        close(go[1]);
        return;
    }
    if (ptrace(PTRACE_SEIZE, c, 0, 0) != 0) {
        check(name, "SEIZE", (uint64_t) errno, 0);
        close(go[1]);
        kill_and_reap(c);
        return;
    }

    int st = 0;
    if (ptrace(PTRACE_INTERRUPT, c, 0, 0) != 0) {
        check(name, "INTERRUPT", (uint64_t) errno, 0);
        close(go[1]);
        kill_and_reap(c);
        return;
    }
    if (wait_for(c, &st) != c)
        goto hung;
    check(name, "interrupt stop status", (uint64_t) st, STOP_STATUS(SIGTRAP, PTRACE_EVENT_STOP));
    check(name, "message at the interrupt stop", read_eventmsg(c), 0);

    ptrace(PTRACE_CONT, c, 0, 0);
    if (write(go[1], "x", 1) != 1)
        goto hung;
    if (wait_for(c, &st) != c)
        goto hung;
    check(name, "SIGSTOP stop status", (uint64_t) st, STOP_STATUS(SIGSTOP, 0));
    check(name, "message at the SIGSTOP stop", read_eventmsg(c), 0);

    // Inject it, so the tracee really does enter group-stop.
    ptrace(PTRACE_CONT, c, 0, (void *) (long) SIGSTOP);
    if (wait_for(c, &st) != c)
        goto hung;
    // The whole status word, stop signal included. This used to mask the
    // signal out, because AOK reported a seized group-stop as SIGTRAP where
    // Linux reports the stop signal that caused it.
    check(name, "group-stop status", (uint64_t) st,
          STOP_STATUS(SIGSTOP, PTRACE_EVENT_STOP));
    check(name, "message at the group-stop", read_eventmsg(c), 0);

    close(go[1]);
    kill_and_reap(c);
    return;

hung:
    check(name, "wait (EINTR here is a hang)", (uint64_t) errno, 0);
    close(go[1]);
    kill_and_reap(c);
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], EXEC_TARGET_ARG) == 0)
        return 0;
    if (argv[0] != NULL && strchr(argv[0], '/') != NULL)
        self_exe = argv[0];
    test_init(argc, argv);
    install_watchdog();

    event_case("clone", body_clone, PTRACE_EVENT_CLONE);
    event_case("fork", body_fork, PTRACE_EVENT_FORK);
    event_case("vfork", body_vfork, PTRACE_EVENT_VFORK);
    vfork_done_case("vfork done, child traced",
                    PTRACE_O_TRACEVFORK | PTRACE_O_TRACEVFORKDONE);
    vfork_done_case("vfork done, child untraced", PTRACE_O_TRACEVFORKDONE);
    exec_case();
    thread_exec_case();
    sequence_case();
    seize_case();

    return finish_suite("ptrace_eventmsg");
}
