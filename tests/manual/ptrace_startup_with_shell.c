// ptrace_startup_with_shell: ptrace driven the way gdb starts a program.
//
// gdb's fork_inferior forks, and the child puts itself in a process group of
// its own, asks for PTRACE_TRACEME and execs `$SHELL -c 'exec PROG'` -- the
// default, `set startup-with-shell on`. startup_inferior then counts one
// SIGTRAP per exec, two here, continuing through anything else, and only after
// the second takes the program to be the one it is debugging. Measured on Linux
// 6.12 (bash and dash, -m64 and -m32): two stops of status 0x57f, si_code
// SI_USER from the tracee's own pid, the first in the shell and the second in
// PROG, the same pid throughout.
//
// shell_case
//   The same with each shell there is: the guest's /bin/sh, and on iSH-AOK the
//   native shells, /AOK/native/zsh, /AOK/native/dash and /AOK/native/sh, which
//   are compiled into the app and run as host code. zsh is the login shell the
//   555 release notes recommend, and it was the user's $SHELL when gdb could
//   not start programs through it but could with startup-with-shell off. A
//   native program's exec reported nothing to the tracer, so there was no first
//   trap; and its own exec was spawn-then-wait, so the traced task never became
//   PROG at all -- an untraced child ran it -- and there was no second trap
//   either. gdb said "During startup program exited normally", every time.
//   Skipped where a shell does not exist, as the native ones do not on Linux.
//
// hw_debug_case (arm64)
//   What gdb asks next, on arm64: how many hardware breakpoints and watchpoints
//   there are, through the NT_ARM_HW_BREAK and NT_ARM_HW_WATCH register sets.
//   It accepts the answer only with a debug architecture it knows (ARMv8 and
//   later), and otherwise warns on every run -- as it did on AOK, which had
//   neither set. AOK has no debug registers: its answer is ARMv8 with none.
//   And the thread pointer, NT_ARM_TLS, which libthread_db finds a thread by
//   (gdb's ps_get_thread_area): without it every session printed "Cannot find
//   user-level thread for LWP" and ran without thread debugging.
//
// regset_length_case
//   A register set is read in whole slots -- 8 bytes on a 64-bit guest, 4 on
//   i386 -- and any other length is EINVAL, which is how a debugger probes a
//   set's shape. gdb on riscv64 asks for NT_PRFPREG in 4-byte slots first and
//   takes EINVAL to mean "8-byte registers"; AOK answered every length, and gdb
//   printed "bfd requires flen 8, but target has flen 4" and could not insert a
//   breakpoint. A length shorter than the set reads as much as that.
//
// exec_trap_order_case
//   The post-exec SIGTRAP is a SIGNAL, queued by the exec (Linux's send_sig in
//   ptrace_event) and delivered on the way out of execve: a PTRACE_SYSCALL
//   tracer sees execve's entry stop, its exit stop, and only then the SIGTRAP.
//   AOK stopped for it inside execve, between the two.
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "test_common.h"

#ifndef __WALL
#define __WALL 0x40000000
#endif
#ifndef PTRACE_GETEVENTMSG
#define PTRACE_GETEVENTMSG 0x4201
#endif
#ifndef PTRACE_GETSIGINFO
#define PTRACE_GETSIGINFO 0x4202
#endif

#define STOP_STATUS(sig) (((sig) << 8) | 0x7f)
#define EXEC_TARGET_ARG "--ptrace-startup-with-shell-target"
#define TARGET_EXIT_CODE 9

static char self_path[PATH_MAX];

static void on_alarm(int sig) { (void) sig; }

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

static pid_t wait_for(pid_t pid, int *status) {
    alarm(test_watchdog_secs(20));
    pid_t got = waitpid(pid, status, __WALL);
    int saved = errno;
    alarm(0);
    errno = saved;
    return got;
}

static void kill_and_reap(pid_t c) {
    if (c <= 0)
        return;
    kill(c, SIGKILL);
    int st;
    while (wait_for(c, &st) < 0 && errno == EINTR)
        ;
}

// Whether /proc/<pid>/exe names this test's own binary: the program the
// tracee was asked to exec in the end.
static bool exe_is_self(pid_t pid) {
    char path[64], exe[PATH_MAX];
    snprintf(path, sizeof path, "/proc/%d/exe", (int) pid);
    ssize_t n = readlink(path, exe, sizeof exe - 1);
    if (n <= 0)
        return false;
    exe[n] = '\0';
    return strcmp(exe, self_path) == 0;
}

static void hw_debug_case(const char *name, pid_t c);
static void regset_length_case(const char *name, pid_t c);

// ---- shell_case --------------------------------------------------------------

static void shell_case(const char *shell) {
    char name[96];
    snprintf(name, sizeof name, "startup with %s", shell);
    if (access(shell, X_OK) != 0) {
        test_logf("  skip %s: not here\n", name);
        return;
    }
    char command[PATH_MAX + 64];
    snprintf(command, sizeof command, "exec %s %s", self_path, EXEC_TARGET_ARG);

    fflush(NULL);
    pid_t c = fork();
    if (c == 0) {
        // fork_inferior's child: its own group, then TRACEME, then the shell.
        setpgid(0, 0);
        if (ptrace(PTRACE_TRACEME, 0, 0, 0) != 0)
            _exit(126);
        execl(shell, shell, "-c", command, (char *) NULL);
        _exit(127);
    }
    if (c < 0) {
        check(name, "fork", (uint64_t) errno, 0);
        return;
    }

    // startup_inferior: count SIGTRAPs, pass every other signal on.
    int pending_execs = 2, traps = 0;
    bool exited = false;
    for (int guard = 0; guard < 100 && pending_execs > 0; guard++) {
        int st = 0;
        pid_t w = wait_for(c, &st);
        if (w != c) {
            check(name, "waitpid names the tracee (EINTR here is a hang)",
                  (uint64_t) (w < 0 ? errno : w), (uint64_t) c);
            kill_and_reap(c);
            return;
        }
        if (WIFEXITED(st) || WIFSIGNALED(st)) {
            // gdb: "During startup program exited".
            check(name, "SIGTRAPs before the program exited", (uint64_t) traps, 2);
            exited = true;
            break;
        }
        int sig = WSTOPSIG(st);
        if (sig != SIGTRAP) {
            ptrace(PTRACE_CONT, c, 0, (void *) (long) sig);
            continue;
        }
        traps++;
        char what[80];
        snprintf(what, sizeof what, "trap %d: status", traps);
        check(name, what, (uint64_t) st, STOP_STATUS(SIGTRAP));
        siginfo_t si;
        memset(&si, 0, sizeof si);
        long r = ptrace(PTRACE_GETSIGINFO, c, 0, &si);
        snprintf(what, sizeof what, "trap %d: PTRACE_GETSIGINFO", traps);
        check(name, what, (uint64_t) (r == 0 ? 0 : errno), 0);
        snprintf(what, sizeof what, "trap %d: si_code is SI_USER", traps);
        check(name, what, (uint64_t) (unsigned) si.si_code, SI_USER);
        snprintf(what, sizeof what, "trap %d: si_pid is the tracee", traps);
        check(name, what, (uint64_t) si.si_pid, (uint64_t) c);
        if (--pending_execs == 0)
            break;
        ptrace(PTRACE_CONT, c, 0, 0);
    }
    if (exited)
        return;
    check(name, "SIGTRAPs counted", (uint64_t) traps, 2);
    // The same pid is now the program, and still traced: a syscall stop
    // proves the second, and /proc names the first.
    check(name, "the tracee is now the program", exe_is_self(c), true);
    hw_debug_case(name, c);
    regset_length_case(name, c);
    ptrace(PTRACE_SYSCALL, c, 0, 0);
    int st = 0;
    pid_t w = wait_for(c, &st);
    check(name, "and still traced: its next stop is a syscall stop",
          (uint64_t) (w == c ? st : -1), STOP_STATUS(SIGTRAP));
    unsigned long msg = 0;
    ptrace(PTRACE_GETEVENTMSG, c, 0, &msg);
    check(name, "that stop is a syscall entry", (uint64_t) msg, 1);
    ptrace(PTRACE_CONT, c, 0, 0);
    w = wait_for(c, &st);
    check(name, "it exits as the program", (uint64_t) (w == c ? st : -1),
          (uint64_t) (TARGET_EXIT_CODE << 8));
}

// ---- hw_debug_case ----------------------------------------------------------------

#if defined(__aarch64__)
#define NT_ARM_TLS_ 0x401
#define NT_ARM_HW_BREAK_ 0x402
#define NT_ARM_HW_WATCH_ 0x403
struct hwdebug_state {
    uint32_t dbg_info;
    uint32_t pad;
    struct {
        uint64_t addr;
        uint32_t ctrl;
        uint32_t pad;
    } dbg_regs[16];
};

// gdb's aarch64_linux_get_debug_reg_capacity: the regset must answer, with a
// debug architecture from ARMv8 (6) to ARMv8.9 (0xb); the slot count is the
// hardware's, and may be none.
static void hw_debug_case(const char *name, pid_t c) {
    static const struct { int note; const char *what; } sets[] = {
        { NT_ARM_HW_WATCH_, "NT_ARM_HW_WATCH" },
        { NT_ARM_HW_BREAK_, "NT_ARM_HW_BREAK" },
    };
    for (size_t i = 0; i < sizeof sets / sizeof sets[0]; i++) {
        struct hwdebug_state state;
        memset(&state, 0xa5, sizeof state);
        struct iovec iov = { &state, sizeof state };
        char what[80];
        long r = ptrace(PTRACE_GETREGSET, c, (void *) (long) sets[i].note, &iov);
        snprintf(what, sizeof what, "%s answers", sets[i].what);
        check(name, what, (uint64_t) (r == 0 ? 0 : errno), 0);
        snprintf(what, sizeof what, "%s is the whole set", sets[i].what);
        check(name, what, (uint64_t) iov.iov_len, sizeof state);
        unsigned arch = (state.dbg_info >> 8) & 0xff, slots = state.dbg_info & 0xff;
        snprintf(what, sizeof what, "%s: a debug architecture gdb knows", sets[i].what);
        check(name, what, arch >= 6 && arch <= 0xb, true);
        snprintf(what, sizeof what, "%s: at most 16 slots", sets[i].what);
        check(name, what, slots <= 16, true);
    }
    // TPIDR_EL0 and TPIDR2_EL0, 16 bytes; gdb asks for the first 8.
    uint64_t tls[2] = { 0, 0 };
    struct iovec iov = { tls, sizeof tls };
    long r = ptrace(PTRACE_GETREGSET, c, (void *) (long) NT_ARM_TLS_, &iov);
    check(name, "NT_ARM_TLS answers", (uint64_t) (r == 0 ? 0 : errno), 0);
    check(name, "NT_ARM_TLS is both registers", (uint64_t) iov.iov_len, sizeof tls);
    check(name, "NT_ARM_TLS: the thread pointer is set", tls[0] != 0, true);
}
#else
static void hw_debug_case(const char *name, pid_t c) { (void) name; (void) c; }
#endif

// ---- regset_length_case -------------------------------------------------------------

#ifndef NT_PRSTATUS
#define NT_PRSTATUS 1
#endif
#ifndef NT_PRFPREG
#define NT_PRFPREG 2
#endif

static void regset_length_case(const char *name, pid_t c) {
    uint64_t regs[64];
    size_t slot = sizeof(long);
    struct iovec iov = { regs, slot / 2 };
    long r = ptrace(PTRACE_GETREGSET, c, (void *) (long) NT_PRSTATUS, &iov);
    check(name, "NT_PRSTATUS in half a slot: EINVAL", (uint64_t) (r == 0 ? 0 : errno), EINVAL);
    iov.iov_len = slot;
    r = ptrace(PTRACE_GETREGSET, c, (void *) (long) NT_PRSTATUS, &iov);
    check(name, "NT_PRSTATUS in one slot: read", (uint64_t) (r == 0 ? 0 : errno), 0);
    check(name, "NT_PRSTATUS in one slot: one slot's length", (uint64_t) iov.iov_len, slot);
#if defined(__riscv)
    // gdb's riscv_linux_read_features, exactly: 33 floats, then 33 doubles.
    iov.iov_len = 33 * 4;
    r = ptrace(PTRACE_GETREGSET, c, (void *) (long) NT_PRFPREG, &iov);
    check(name, "NT_PRFPREG as 33 floats: EINVAL", (uint64_t) (r == 0 ? 0 : errno), EINVAL);
    iov.iov_len = 33 * 8;
    r = ptrace(PTRACE_GETREGSET, c, (void *) (long) NT_PRFPREG, &iov);
    check(name, "NT_PRFPREG as 33 doubles: read", (uint64_t) (r == 0 ? 0 : errno), 0);
    check(name, "NT_PRFPREG as 33 doubles: all of it", (uint64_t) iov.iov_len, 33 * 8);
#endif
}

// ---- exec_trap_order_case --------------------------------------------------------

static void exec_trap_order_case(void) {
    const char *name = "exec SIGTRAP order";
    fflush(NULL);
    pid_t c = fork();
    if (c == 0) {
        if (ptrace(PTRACE_TRACEME, 0, 0, 0) != 0)
            _exit(126);
        kill(getpid(), SIGSTOP);
        execl(self_path, self_path, EXEC_TARGET_ARG, (char *) NULL);
        _exit(127);
    }
    int st = 0;
    pid_t w = wait_for(c, &st);
    check(name, "the SIGSTOP", (uint64_t) (w == c ? st : -1), STOP_STATUS(SIGSTOP));
    // No TRACESYSGOOD, as a TRACEME tracer starts: syscall stops and the exec's
    // SIGTRAP both report 0x57f, and only the siginfo tells them apart --
    // si_code SIGTRAP for a syscall stop, SI_USER for the signal.
    ptrace(PTRACE_SYSCALL, c, 0, 0);
    int order[3] = {0, 0, 0};
    unsigned long msgs[3] = {0, 0, 0};
    for (int i = 0; i < 3; i++) {
        w = wait_for(c, &st);
        if (w != c || st != STOP_STATUS(SIGTRAP)) {
            check(name, "a SIGTRAP stop", (uint64_t) (w == c ? st : -1), STOP_STATUS(SIGTRAP));
            kill_and_reap(c);
            return;
        }
        siginfo_t si;
        memset(&si, 0, sizeof si);
        ptrace(PTRACE_GETSIGINFO, c, 0, &si);
        order[i] = si.si_code;
        ptrace(PTRACE_GETEVENTMSG, c, 0, &msgs[i]);
        ptrace(PTRACE_SYSCALL, c, 0, 0);
    }
    check(name, "execve's entry stop first", (uint64_t) (unsigned) order[0], SIGTRAP);
    check(name, "  (an entry)", (uint64_t) msgs[0], 1);
    check(name, "then execve's exit stop", (uint64_t) (unsigned) order[1], SIGTRAP);
    check(name, "  (an exit)", (uint64_t) msgs[1], 2);
    check(name, "then the exec's SIGTRAP", (uint64_t) (unsigned) order[2], SI_USER);
    kill_and_reap(c);
}

int main(int argc, char **argv) {
    // The program the shell execs: a known exit status, nothing else.
    if (argc > 1 && strcmp(argv[1], EXEC_TARGET_ARG) == 0) {
        syscall(SYS_getppid);
        _exit(TARGET_EXIT_CODE);
    }
    test_init(argc, argv);
    install_watchdog();
    ssize_t n = readlink("/proc/self/exe", self_path, sizeof self_path - 1);
    if (n <= 0) {
        printf("ptrace_startup_with_shell: FAIL cannot read /proc/self/exe\n");
        return 1;
    }
    self_path[n] = '\0';

    shell_case("/bin/sh");
    shell_case("/AOK/native/zsh");
    shell_case("/AOK/native/dash");
    shell_case("/AOK/native/sh");
    exec_trap_order_case();
    return finish_suite("ptrace_startup_with_shell");
}
