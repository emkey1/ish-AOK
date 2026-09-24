// seccomp: strict mode and BPF filters actually confine.
//
// Both were a stub that reported success and did nothing: a filter that
// should have blocked getppid() did not, strict mode killed nothing, and so
// OpenSSH's pre-authentication sandbox -- and systemd's SystemCallFilter=,
// apt's and man-db's helpers, browsers -- believed they were confined and
// were not.
//
// Each leg runs in a forked child, since a filter cannot be removed, and
// checks what Linux does:
//   - the probes: PR_GET_SECCOMP 0, PR_SET_SECCOMP(FILTER, NULL) EFAULT;
//   - programs the verifier refuses (EINVAL), and no_new_privs or
//     CAP_SYS_ADMIN required (EACCES);
//   - every action: ERRNO (and its data 0 and its cap), KILL_PROCESS,
//     KILL_THREAD alone and beside another thread, TRAP with its siginfo and
//     with SIGSYS blocked, TRACE with and without a tracer, LOG, USER_NOTIF
//     with no listener, an action Linux does not know;
//   - what the program sees: nr, arch, arguments, the length of the data;
//     a division by zero at run time;
//   - stacking (the most restrictive action wins, the newest among equals),
//     fork and exec keeping filters, /proc/self/status, TSYNC and its
//     failure, strict mode's four calls and its SIGKILL.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_common.h"

// <linux/filter.h>, <linux/seccomp.h> and <linux/audit.h>, spelled out so the
// test builds on a root without kernel headers.
struct sf { unsigned short code; unsigned char jt, jf; unsigned int k; };
struct sfprog { unsigned short len; struct sf *filter; };
#define STMT(c, k) { (unsigned short) (c), 0, 0, (unsigned int) (k) }
#define JUMP(c, k, jt, jf) { (unsigned short) (c), (jt), (jf), (unsigned int) (k) }
#define LD_W_ABS 0x20
#define LD_W_LEN 0x80
#define JEQ_K 0x15
#define JA 0x05
#define RET_K 0x06
#define RET_A 0x16
#define LD_IMM 0x00
#define LDX_IMM 0x01
#define LD_MEM 0x60
#define DIV_X 0x3c
#define DIV_K 0x34
#define LSH_K 0x64
#define MOD_K 0x94
#define LD_H_ABS 0x28

#define OFF_NR 0
#define OFF_ARCH 4
#define OFF_ARG0_LO 16
#define OFF_ARG0_HI 20

#define RET_KILL_PROCESS 0x80000000u
#define RET_KILL_THREAD 0x00000000u
#define RET_TRAP 0x00030000u
#define RET_ERRNO 0x00050000u
#define RET_USER_NOTIF 0x7fc00000u
#define RET_TRACE 0x7ff00000u
#define RET_LOG 0x7ffc0000u
#define RET_ALLOW 0x7fff0000u

#define SET_MODE_STRICT 0
#define SET_MODE_FILTER 1
#define GET_ACTION_AVAIL 2
#define FLAG_TSYNC 1u
#define FLAG_TSYNC_ESRCH 16u

#ifndef PR_SET_NO_NEW_PRIVS
#define PR_SET_NO_NEW_PRIVS 38
#endif
#ifndef SYS_seccomp
# if defined(__NR_seccomp)
#  define SYS_seccomp __NR_seccomp
# elif defined(__x86_64__)
#  define SYS_seccomp 317
# elif defined(__i386__)
#  define SYS_seccomp 354
# else
#  define SYS_seccomp 277
# endif
#endif
#define PTRACE_EVENT_SECCOMP_ 7
#define PTRACE_O_TRACESECCOMP_ 0x80

#if defined(__x86_64__)
#define ARCH_NATIVE 0xc000003eu
#elif defined(__i386__)
#define ARCH_NATIVE 0x40000003u
#elif defined(__aarch64__)
#define ARCH_NATIVE 0xc00000b7u
#elif defined(__riscv)
#define ARCH_NATIVE 0xc00000f3u
#else
#error "unknown architecture"
#endif

static const char *self_path;

static int install(struct sf *insns, unsigned short len, unsigned flags) {
    struct sfprog prog = { len, insns };
    return (int) syscall(SYS_seccomp, SET_MODE_FILTER, flags, &prog);
}

// Linux's usual filter shape: check the arch, then act on one number.
static int filter_nr(long nr, unsigned action) {
    struct sf f[] = {
        STMT(LD_W_ABS, OFF_ARCH),
        JUMP(JEQ_K, ARCH_NATIVE, 1, 0),
        STMT(RET_K, RET_KILL_PROCESS),
        STMT(LD_W_ABS, OFF_NR),
        JUMP(JEQ_K, nr, 0, 1),
        STMT(RET_K, action),
        STMT(RET_K, RET_ALLOW),
    };
    return install(f, sizeof f / sizeof *f, 0);
}

static void nnp(void) {
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
        printf("FAIL PR_SET_NO_NEW_PRIVS errno=%d\n", errno);
}

static void check(const char *label, int ok, long got, long want) {
    if (!ok)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-58s %s (got %ld, want %ld)\n", label, ok ? "ok" : "FAIL", got, want);
}

// Run fn in a child and return its wait status.
static int in_child(void (*fn)(void)) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        // The parent's count is its own: a leg that failed earlier must not
        // make every later child report failure.
        failures_total = 0;
        alarm(test_watchdog_secs(20));
        fn();
        fflush(stdout);
        _exit(failures_total ? 1 : 0);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    return st;
}

static void expect_exit0(const char *label, int st) {
    check(label, WIFEXITED(st) && WEXITSTATUS(st) == 0, st, 0);
}

static void expect_signal(const char *label, int st, int sig) {
    check(label, WIFSIGNALED(st) && WTERMSIG(st) == sig, WIFSIGNALED(st) ? WTERMSIG(st) : -st, sig);
}

// ---- probes and refusals -----------------------------------------------------

static void leg_probe(void) {
    errno = 0;
    int r = prctl(PR_GET_SECCOMP, 0, 0, 0, 0);
    check("PR_GET_SECCOMP before", r == 0, r, 0);
    nnp();
    errno = 0;
    r = prctl(PR_SET_SECCOMP, 2, 0, 0, 0);
    check("PR_SET_SECCOMP(FILTER, NULL) is EFAULT", r < 0 && errno == EFAULT, r < 0 ? errno : 0, EFAULT);
    errno = 0;
    r = (int) syscall(SYS_seccomp, 99, 0, 0);
    check("seccomp(unknown op) EINVAL", r < 0 && errno == EINVAL, r < 0 ? errno : 0, EINVAL);
    struct sf allow[] = { STMT(RET_K, RET_ALLOW) };
    errno = 0;
    r = install(allow, 1, 1u << 31);
    check("seccomp(unknown flag) EINVAL", r < 0 && errno == EINVAL, r < 0 ? errno : 0, EINVAL);
    errno = 0;
    r = (int) syscall(SYS_seccomp, SET_MODE_STRICT, 1, 0);
    check("SET_MODE_STRICT with flags EINVAL", r < 0 && errno == EINVAL, r < 0 ? errno : 0, EINVAL);

    static const unsigned actions[] = {RET_KILL_PROCESS, RET_KILL_THREAD, RET_TRAP,
            RET_ERRNO, RET_TRACE, RET_LOG, RET_ALLOW};
    for (unsigned i = 0; i < sizeof actions / sizeof *actions; i++) {
        unsigned a = actions[i];
        errno = 0;
        r = (int) syscall(SYS_seccomp, GET_ACTION_AVAIL, 0, &a);
        char label[64];
        snprintf(label, sizeof label, "GET_ACTION_AVAIL %#x", a);
        check(label, r == 0, r < 0 ? errno : 0, 0);
    }
    unsigned bogus = 0x00010000u;
    errno = 0;
    r = (int) syscall(SYS_seccomp, GET_ACTION_AVAIL, 0, &bogus);
    check("GET_ACTION_AVAIL unknown EOPNOTSUPP", r < 0 && errno == EOPNOTSUPP, r < 0 ? errno : 0, EOPNOTSUPP);
}

static void leg_invalid(void) {
    nnp();
    struct { const char *name; struct sf insns[4]; unsigned short len; } bad[] = {
        {"no return at the end", {STMT(LD_W_ABS, OFF_NR)}, 1},
        {"jump past the end", {JUMP(JEQ_K, 1, 5, 0), STMT(RET_K, RET_ALLOW)}, 2},
        {"JA past the end", {STMT(JA, 1), STMT(RET_K, RET_ALLOW)}, 2},
        {"load past seccomp_data", {STMT(LD_W_ABS, 64), STMT(RET_A, 0)}, 2},
        {"unaligned load", {STMT(LD_W_ABS, 2), STMT(RET_A, 0)}, 2},
        {"halfword load", {STMT(LD_H_ABS, 0), STMT(RET_A, 0)}, 2},
        {"divide by constant 0", {STMT(DIV_K, 0), STMT(RET_A, 0)}, 2},
        {"shift by 32", {STMT(LSH_K, 32), STMT(RET_A, 0)}, 2},
        {"MOD", {STMT(MOD_K, 3), STMT(RET_A, 0)}, 2},
        {"scratch read before write", {STMT(LD_MEM, 3), STMT(RET_A, 0)}, 2},
        {"scratch index 16", {STMT(LD_MEM, 16), STMT(RET_A, 0)}, 2},
        {"unknown opcode", {STMT(0xff, 0), STMT(RET_A, 0)}, 2},
    };
    for (unsigned i = 0; i < sizeof bad / sizeof *bad; i++) {
        errno = 0;
        int r = install(bad[i].insns, bad[i].len, 0);
        char label[96];
        snprintf(label, sizeof label, "refuses: %s", bad[i].name);
        check(label, r < 0 && errno == EINVAL, r < 0 ? errno : 0, EINVAL);
    }
    struct sf one[] = { STMT(RET_K, RET_ALLOW) };
    errno = 0;
    int r = install(one, 0, 0);
    check("refuses: length 0", r < 0 && errno == EINVAL, r < 0 ? errno : 0, EINVAL);
    static struct sf big[4097];
    for (unsigned i = 0; i < 4097; i++)
        big[i] = (struct sf) STMT(RET_K, RET_ALLOW);
    errno = 0;
    r = install(big, 4097, 0);
    check("refuses: 4097 instructions", r < 0 && errno == EINVAL, r < 0 ? errno : 0, EINVAL);
    // Nothing was installed by any of that.
    r = prctl(PR_GET_SECCOMP, 0, 0, 0, 0);
    check("still mode 0 after refusals", r == 0, r, 0);
}

static void leg_needs_nnp(void) {
    // Unprivileged and without no_new_privs: EACCES.
    if (geteuid() == 0) {
        if (setgroups(0, NULL) || setgid(2121) || setuid(2020)) {
            printf("FAIL drop errno=%d\n", errno);
            return;
        }
    }
    struct sf allow[] = { STMT(RET_K, RET_ALLOW) };
    errno = 0;
    int r = install(allow, 1, 0);
    check("filter without no_new_privs is EACCES", r < 0 && errno == EACCES, r < 0 ? errno : 0, EACCES);
    nnp();
    r = install(allow, 1, 0);
    check("filter with no_new_privs", r == 0, r < 0 ? errno : 0, 0);
}

// ---- actions -------------------------------------------------------------------

static void leg_errno(void) {
    nnp();
    check("install ERRNO(EPERM) on getppid", filter_nr(SYS_getppid, RET_ERRNO | EPERM) == 0, errno, 0);
    errno = 0;
    long r = syscall(SYS_getppid);
    check("getppid blocked with EPERM", r == -1 && errno == EPERM, r == -1 ? errno : r, EPERM);
    r = syscall(SYS_getpid);
    check("getpid still allowed", r == getpid(), r, getpid());
    r = prctl(PR_GET_SECCOMP, 0, 0, 0, 0);
    check("PR_GET_SECCOMP in filter mode", r == 2, r, 2);
}

static void leg_errno_zero_not_run(void) {
    nnp();
    int fd = open("/dev/null", O_RDONLY);
    filter_nr(SYS_close, RET_ERRNO | 0);
    long r = syscall(SYS_close, fd);
    check("ERRNO(0) answers 0", r == 0, r, 0);
    r = fcntl(fd, F_GETFD);
    check("...without running the call", r >= 0, r < 0 ? errno : 0, 0);
}

static void leg_errno_cap(void) {
    nnp();
    filter_nr(SYS_getppid, RET_ERRNO | 0xffff);
    errno = 0;
    long r = syscall(SYS_getppid);
    check("ERRNO data capped at 4095", r == -1 && errno == 4095, r == -1 ? errno : r, 4095);
}

static void leg_kill_process(void) {
    nnp();
    filter_nr(SYS_getppid, RET_KILL_PROCESS);
    syscall(SYS_getppid);
    printf("FAIL survived KILL_PROCESS\n");
    _exit(3);
}

static void leg_kill_thread_alone(void) {
    nnp();
    filter_nr(SYS_getppid, RET_KILL_THREAD);
    syscall(SYS_getppid);
    printf("FAIL survived KILL_THREAD\n");
    _exit(3);
}

static volatile pid_t victim_tid;
static volatile int victim_returned;

static void *thread_getppid(void *arg) {
    (void) arg;
    victim_tid = (pid_t) syscall(SYS_gettid);
    syscall(SYS_getppid);
    victim_returned = 1;
    return NULL;
}

static void leg_kill_thread_sibling(void) {
    nnp();
    // Installed before the thread exists, so the thread inherits it.
    filter_nr(SYS_getppid, RET_KILL_THREAD);
    pthread_t t;
    if (pthread_create(&t, NULL, thread_getppid, NULL) != 0) {
        printf("FAIL pthread_create\n");
        return;
    }
    // Not pthread_join: musl's join waits for pthread_exit to mark the
    // thread done, which a thread killed in a syscall never does, so it
    // blocks forever on Linux too. Watch the thread's own id go away.
    while (victim_tid == 0)
        usleep(1000);
    int gone = 0;
    for (int i = 0; i < 5000 && !gone; i++) {
        if (syscall(SYS_tgkill, getpid(), victim_tid, 0) < 0 && errno == ESRCH)
            gone = 1;
        else
            usleep(1000);
    }
    check("KILL_THREAD ended the thread", gone, gone, 1);
    check("...in the call, not after it", !victim_returned, victim_returned, 0);
    // And this thread carries on.
    check("the other thread survives", syscall(SYS_getpid) == getpid(), 0, 0);
    (void) t;
}

static volatile sig_atomic_t trap_count, trap_code, trap_errno, trap_syscall;
static volatile unsigned trap_arch;
static volatile long trap_addr_nonzero;

static void on_sigsys(int sig, siginfo_t *si, void *uc) {
    (void) sig; (void) uc;
    trap_count++;
    trap_code = si->si_code;
    trap_errno = si->si_errno;
#ifdef si_syscall
    trap_syscall = si->si_syscall;
    trap_arch = si->si_arch;
    trap_addr_nonzero = si->si_call_addr != NULL;
#else
    // musl before 1.2 spells them through the union only.
    trap_syscall = si->si_value.sival_int;
#endif
}

static void leg_trap(void) {
    struct sigaction sa = {0};
    sa.sa_sigaction = on_sigsys;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSYS, &sa, NULL);
    nnp();
    filter_nr(SYS_getppid, RET_TRAP | 0x42);
    syscall(SYS_getppid);
    check("TRAP delivered SIGSYS once", trap_count == 1, trap_count, 1);
    check("si_code SYS_SECCOMP", trap_code == 1, trap_code, 1);
    check("si_errno is the filter's data", trap_errno == 0x42, trap_errno, 0x42);
#ifdef si_syscall
    check("si_syscall is the call", trap_syscall == SYS_getppid, trap_syscall, SYS_getppid);
    check("si_arch is the native arch", trap_arch == ARCH_NATIVE, (long) trap_arch, (long) ARCH_NATIVE);
    check("si_call_addr set", trap_addr_nonzero, trap_addr_nonzero, 1);
#endif
    // Execution continued after the handler, and other calls work.
    check("getpid after the trap", syscall(SYS_getpid) == getpid(), 0, 0);
}

static void leg_trap_blocked(void) {
    struct sigaction sa = {0};
    sa.sa_sigaction = on_sigsys;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSYS, &sa, NULL);
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGSYS);
    sigprocmask(SIG_BLOCK, &set, NULL);
    nnp();
    filter_nr(SYS_getppid, RET_TRAP);
    syscall(SYS_getppid);
    printf("FAIL survived TRAP with SIGSYS blocked\n");
    _exit(3);
}

static void leg_trace_no_tracer(void) {
    nnp();
    filter_nr(SYS_getppid, RET_TRACE | 7);
    errno = 0;
    long r = syscall(SYS_getppid);
    check("TRACE without a tracer is ENOSYS", r == -1 && errno == ENOSYS, r == -1 ? errno : r, ENOSYS);
}

static void leg_log(void) {
    nnp();
    filter_nr(SYS_getppid, RET_LOG);
    long r = syscall(SYS_getppid);
    check("LOG lets the call run", r == getppid(), r, getppid());
}

static void leg_user_notif(void) {
    nnp();
    filter_nr(SYS_getppid, RET_USER_NOTIF);
    errno = 0;
    long r = syscall(SYS_getppid);
    check("USER_NOTIF without a listener is ENOSYS", r == -1 && errno == ENOSYS, r == -1 ? errno : r, ENOSYS);
}

static void leg_unknown_action(void) {
    nnp();
    filter_nr(SYS_getppid, 0x00010000u);
    syscall(SYS_getppid);
    printf("FAIL survived an unknown action\n");
    _exit(3);
}

// ---- what the program sees -------------------------------------------------

static void leg_args(void) {
    nnp();
    // write(99, ...) is EPERM; any other fd reaches write.
    struct sf f[] = {
        STMT(LD_W_ABS, OFF_NR),
        JUMP(JEQ_K, SYS_write, 0, 3),
        STMT(LD_W_ABS, OFF_ARG0_LO),
        JUMP(JEQ_K, 99, 0, 1),
        STMT(RET_K, RET_ERRNO | EPERM),
        STMT(RET_K, RET_ALLOW),
    };
    install(f, sizeof f / sizeof *f, 0);
    errno = 0;
    long r = syscall(SYS_write, 99, "x", 1);
    check("arg0 == 99 matched", r == -1 && errno == EPERM, r == -1 ? errno : r, EPERM);
    errno = 0;
    r = syscall(SYS_write, 98, "x", 1);
    check("arg0 == 98 reaches write (EBADF)", r == -1 && errno == EBADF, r == -1 ? errno : r, EBADF);
}

static void leg_arg_high_word(void) {
    nnp();
    // A 32-bit caller's arguments are zero-extended; a 64-bit one's -1 has
    // its high word set. Either way the filter sees the word the ABI gives.
    struct sf f[] = {
        STMT(LD_W_ABS, OFF_NR),
        JUMP(JEQ_K, SYS_getppid, 0, 3),
        STMT(LD_W_ABS, OFF_ARG0_HI),
        JUMP(JEQ_K, 0xffffffffu, 0, 1),
        STMT(RET_K, RET_ERRNO | E2BIG),
        STMT(RET_K, RET_ALLOW),
    };
    install(f, sizeof f / sizeof *f, 0);
    errno = 0;
    long r = syscall(SYS_getppid, -1L);
    int want64 = sizeof(long) == 8;
    if (want64)
        check("64-bit arg high word seen", r == -1 && errno == E2BIG, r == -1 ? errno : r, E2BIG);
    else
        check("32-bit arg zero-extended", r == getppid(), r, getppid());
}

static void leg_len_and_div0(void) {
    nnp();
    // A = sizeof(seccomp_data) must be 64; then 1 / X with X = 0 ends the
    // program returning 0 -- KILL_THREAD -- for getppid only.
    struct sf f[] = {
        STMT(LD_W_LEN, 0),
        JUMP(JEQ_K, 64, 1, 0),
        STMT(RET_K, RET_ERRNO | ENOTSUP),
        STMT(LD_W_ABS, OFF_NR),
        JUMP(JEQ_K, SYS_getppid, 0, 3),
        STMT(LD_IMM, 1),
        STMT(LDX_IMM, 0),
        STMT(DIV_X, 0),
        STMT(RET_K, RET_ALLOW),
    };
    if (install(f, sizeof f / sizeof *f, 0) != 0) {
        printf("FAIL install len/div0 errno=%d\n", errno);
        _exit(4);
    }
    check("LEN is 64 (getpid allowed)", syscall(SYS_getpid) == getpid(), 0, 0);
    syscall(SYS_getppid);
    printf("FAIL survived a division by zero\n");
    _exit(3);
}

// ---- stacking, inheritance, status ----------------------------------------------

static void leg_stack_newest_wins(void) {
    nnp();
    filter_nr(SYS_getppid, RET_ERRNO | EACCES);
    filter_nr(SYS_getppid, RET_ERRNO | EPERM);
    errno = 0;
    long r = syscall(SYS_getppid);
    check("two ERRNOs: the newest wins", r == -1 && errno == EPERM, r == -1 ? errno : r, EPERM);
}

static void leg_stack_kill_wins(void) {
    nnp();
    filter_nr(SYS_getppid, RET_KILL_PROCESS);
    filter_nr(SYS_getppid, RET_ERRNO | EPERM);
    syscall(SYS_getppid);
    printf("FAIL an older KILL lost to a newer ERRNO\n");
    _exit(3);
}

static int status_field(const char *name) {
    FILE *f = fopen("/proc/self/status", "r");
    if (f == NULL)
        return -1;
    char line[256];
    int v = -1;
    size_t n = strlen(name);
    while (fgets(line, sizeof line, f) != NULL)
        if (strncmp(line, name, n) == 0 && line[n] == ':')
            v = atoi(line + n + 1);
    fclose(f);
    return v;
}

static void leg_inherit(void) {
    nnp();
    filter_nr(SYS_getppid, RET_ERRNO | EPERM);
    filter_nr(SYS_getuid, RET_ERRNO | EPERM);
    check("status Seccomp 2", status_field("Seccomp") == 2, status_field("Seccomp"), 2);
    int nf = status_field("Seccomp_filters");
    check("status Seccomp_filters 2", nf == 2, nf, 2);
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        errno = 0;
        long r = syscall(SYS_getppid);
        _exit(r == -1 && errno == EPERM ? 0 : 1);
    }
    int st;
    waitpid(pid, &st, 0);
    expect_exit0("fork keeps the filters", st);
    fflush(stdout);
    pid = fork();
    if (pid == 0) {
        execl(self_path, self_path, "--probe-getppid", (char *) NULL);
        _exit(127);
    }
    waitpid(pid, &st, 0);
    expect_exit0("exec keeps the filters", st);
}

static void *thread_wait_then_getppid(void *arg) {
    volatile int *go = arg;
    while (!*go)
        usleep(1000);
    errno = 0;
    long r = syscall(SYS_getppid);
    return (void *) (intptr_t) (r == -1 && errno == EPERM);
}

static void leg_tsync(void) {
    nnp();
    static volatile int go;
    pthread_t t;
    pthread_create(&t, NULL, thread_wait_then_getppid, (void *) &go);
    struct sf f[] = {
        STMT(LD_W_ABS, OFF_NR),
        JUMP(JEQ_K, SYS_getppid, 0, 1),
        STMT(RET_K, RET_ERRNO | EPERM),
        STMT(RET_K, RET_ALLOW),
    };
    int r = install(f, sizeof f / sizeof *f, FLAG_TSYNC);
    check("TSYNC install", r == 0, r < 0 ? errno : r, 0);
    go = 1;
    void *ret = NULL;
    pthread_join(t, &ret);
    check("TSYNC reached the other thread", ret == (void *) 1, (long) (intptr_t) ret, 1);
}

static volatile int tsync_fail_stage;
static void *thread_own_filter(void *arg) {
    (void) arg;
    filter_nr(SYS_getuid, RET_ERRNO | EPERM);
    tsync_fail_stage = 1;
    while (tsync_fail_stage != 2)
        usleep(1000);
    return NULL;
}

static void leg_tsync_fail(void) {
    nnp();
    pthread_t t;
    pthread_create(&t, NULL, thread_own_filter, NULL);
    while (tsync_fail_stage != 1)
        usleep(1000);
    struct sf allow[] = { STMT(RET_K, RET_ALLOW) };
    errno = 0;
    int r = install(allow, 1, FLAG_TSYNC);
    check("TSYNC with a diverged thread returns its tid", r > 0 && r != getpid(), r, 1);
    errno = 0;
    r = install(allow, 1, FLAG_TSYNC | FLAG_TSYNC_ESRCH);
    check("...or ESRCH with TSYNC_ESRCH", r < 0 && errno == ESRCH, r < 0 ? errno : r, ESRCH);
    tsync_fail_stage = 2;
    pthread_join(t, NULL);
}

// ---- strict mode --------------------------------------------------------------------

static void leg_strict_kill(void) {
    if (prctl(PR_SET_SECCOMP, 1, 0, 0, 0) != 0) {
        printf("FAIL PR_SET_SECCOMP(STRICT) errno=%d\n", errno);
        _exit(4);
    }
    // write is one of the four.
    if (syscall(SYS_write, 1, "", 0) != 0)
        syscall(SYS_exit, 5);
    syscall(SYS_getpid);
    syscall(SYS_exit, 6); // not reached
}

static void leg_strict_exit(void) {
    prctl(PR_SET_SECCOMP, 1, 0, 0, 0);
    syscall(SYS_exit, 7);
}

static void leg_filter_then_strict(void) {
    nnp();
    struct sf allow[] = { STMT(RET_K, RET_ALLOW) };
    install(allow, 1, 0);
    errno = 0;
    int r = prctl(PR_SET_SECCOMP, 1, 0, 0, 0);
    check("strict after a filter is EINVAL", r < 0 && errno == EINVAL, r < 0 ? errno : 0, EINVAL);
}

// ---- a tracer ------------------------------------------------------------------------

static void leg_trace_event(void) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        // Read before the filter: any later getppid() is traced too.
        pid_t parent = getppid();
        ptrace(PTRACE_TRACEME, 0, 0, 0);
        raise(SIGSTOP);
        nnp();
        filter_nr(SYS_getppid, RET_TRACE | 0x42);
        long r = syscall(SYS_getppid);
        _exit(r == parent ? 0 : 1);
    }
    int st;
    waitpid(pid, &st, 0);
    check("tracee stopped", WIFSTOPPED(st), st, 0);
    ptrace(PTRACE_SETOPTIONS, pid, 0, (void *) (long) PTRACE_O_TRACESECCOMP_);
    ptrace(PTRACE_CONT, pid, 0, 0);
    waitpid(pid, &st, 0);
    int event = WIFSTOPPED(st) ? (st >> 16) : -1;
    check("PTRACE_EVENT_SECCOMP stop", event == PTRACE_EVENT_SECCOMP_, event, PTRACE_EVENT_SECCOMP_);
    unsigned long msg = 0;
    ptrace(PTRACE_GETEVENTMSG, pid, 0, &msg);
    check("event message is the filter's data", msg == 0x42, (long) msg, 0x42);
    ptrace(PTRACE_CONT, pid, 0, 0);
    waitpid(pid, &st, 0);
    expect_exit0("the traced call ran after the stop", st);
}

int main(int argc, char **argv) {
    self_path = argv[0];
    if (argc == 2 && strcmp(argv[1], "--probe-getppid") == 0) {
        errno = 0;
        long r = syscall(SYS_getppid);
        return r == -1 && errno == EPERM ? 0 : 1;
    }
    test_init(argc, argv);
    static char abs_self[512];
    if (self_path[0] != '/' && realpath(self_path, abs_self) != NULL)
        self_path = abs_self;

    expect_exit0("probes", in_child(leg_probe));
    expect_exit0("invalid programs", in_child(leg_invalid));
    expect_exit0("no_new_privs rule", in_child(leg_needs_nnp));
    expect_exit0("ERRNO", in_child(leg_errno));
    expect_exit0("ERRNO(0)", in_child(leg_errno_zero_not_run));
    expect_exit0("ERRNO cap", in_child(leg_errno_cap));
    expect_signal("KILL_PROCESS kills with SIGSYS", in_child(leg_kill_process), SIGSYS);
    expect_signal("KILL_THREAD alone kills with SIGSYS", in_child(leg_kill_thread_alone), SIGSYS);
    expect_exit0("KILL_THREAD beside a sibling", in_child(leg_kill_thread_sibling));
    expect_exit0("TRAP", in_child(leg_trap));
    expect_signal("TRAP with SIGSYS blocked kills", in_child(leg_trap_blocked), SIGSYS);
    expect_exit0("TRACE, no tracer", in_child(leg_trace_no_tracer));
    expect_exit0("LOG", in_child(leg_log));
    expect_exit0("USER_NOTIF", in_child(leg_user_notif));
    expect_signal("unknown action kills with SIGSYS", in_child(leg_unknown_action), SIGSYS);
    expect_exit0("arguments", in_child(leg_args));
    expect_exit0("argument high word", in_child(leg_arg_high_word));
    expect_signal("division by zero at run time kills", in_child(leg_len_and_div0), SIGSYS);
    expect_exit0("stacked ERRNOs", in_child(leg_stack_newest_wins));
    expect_signal("stacked: KILL beats ERRNO", in_child(leg_stack_kill_wins), SIGSYS);
    expect_exit0("fork, exec, status", in_child(leg_inherit));
    expect_exit0("TSYNC", in_child(leg_tsync));
    expect_exit0("TSYNC failure", in_child(leg_tsync_fail));
    expect_signal("strict mode kills with SIGKILL", in_child(leg_strict_kill), SIGKILL);
    {
        int st = in_child(leg_strict_exit);
        check("strict mode allows exit", WIFEXITED(st) && WEXITSTATUS(st) == 7, st, 7 << 8);
    }
    expect_exit0("strict after filter", in_child(leg_filter_then_strict));
    leg_trace_event();
    return finish_suite("seccomp_filter");
}
