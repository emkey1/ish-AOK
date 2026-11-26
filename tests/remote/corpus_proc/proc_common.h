/*
 * proc_common.h — shared harness for AOK-Workstation process/thread-lifecycle
 * CONFORMANCE tests (fork/vfork/clone, exec, exit, wait4/waitid, sessions and
 * process groups, ptrace).
 *
 * Same philosophy as corpus_signal/sig_common.h and corpus_fs/fs_common.h: the
 * right oracle for syscall *semantics* is a REAL LINUX KERNEL (mint's Lima
 * x86_64 VM), not Rosetta. The conductor's `conform` subcommand builds the two
 * Linux musl ELFs and compares every iSH (arch x engine) cell against
 * mint:x86_64 / mint:i386. A divergence between an iSH cell and mint is a
 * candidate Linux nonconformance.
 *
 * Each test prints one canonical line per observation:
 *
 *     <key> res=<value>
 *
 * <key> is a stable, arch-independent identifier (no spaces); <value> is a
 * single whitespace-free token compared byte-for-byte across cells.
 *
 * Observations must be ARCH- and RUN-independent. In particular:
 *   - emit errno SYMBOLS, not numbers (errno_name);
 *   - emit DERIVED invariants for anything that legitimately varies per run or
 *     per host (pid values, the test's own uid: the mint VM runs as a non-root
 *     user while iSH runs as root, so NEVER emit a raw uid/gid — emit e.g.
 *     "auxval(AT_UID)==getuid()" -> 1);
 *   - decode wait(2) status words into stable W* booleans + the bare code/sig.
 *
 * CARRY THE cmpxchg LESSON: only assert spec-guaranteed invariants. Where Linux
 * leaves behavior unspecified or scheduling-dependent, do not emit it as a key.
 */
#ifndef ISH_PROC_COMMON_H
#define ISH_PROC_COMMON_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>

/* ---- engine self-selection (guest only; no-ops on real Linux) ----
 * Identical to the other corpora so one binary covers every iSH cell; on mint's
 * real Linux the /proc/ish knobs are absent (harmless no-ops). */
static void proc_write_proc(const char *path, const char *val) {
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return;
    ssize_t w = write(fd, val, strlen(val));
    (void) w;
    close(fd);
}

static void proc_self_comm(char *out, size_t n) {
    out[0] = '\0';
    int fd = open("/proc/self/comm", O_RDONLY);
    if (fd < 0)
        return;
    ssize_t r = read(fd, out, n - 1);
    if (r < 0)
        r = 0;
    out[r] = '\0';
    for (char *p = out; *p; p++)
        if (*p == '\n') { *p = '\0'; break; }
}

static void proc_apply_engine(const char *spec) {
    if (!spec)
        return;
    if (strcmp(spec, "amd64:jit") == 0)
        proc_write_proc("/proc/ish/amd64_jit", "1");
    else if (strcmp(spec, "amd64:interp") == 0)
        proc_write_proc("/proc/ish/amd64_jit", "0");
    else if (strcmp(spec, "i386:no_cache") == 0) {
        char comm[32]; proc_self_comm(comm, sizeof comm);
        proc_write_proc("/proc/ish/i386_no_cache_comm", comm);
    } else if (strcmp(spec, "i386:single_step") == 0) {
        char comm[32]; proc_self_comm(comm, sizeof comm);
        proc_write_proc("/proc/ish/i386_single_step_comm", comm);
    }
    /* "i386:jit", "oracle", "native", unknown -> default engine, no knob */
}

static int proc_verbose;

/* ---- canonical observation emit ---- */

__attribute__((format(printf, 2, 3)))
static void emit(const char *key, const char *fmt, ...) {
    printf("%s res=", key);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

static void emit_i(const char *key, long v) { printf("%s res=%ld\n", key, v); }

/* map an errno to a stable symbol so the value is arch/libc-independent */
static const char *errno_name(int e) {
    switch (e) {
        case 0:            return "OK";
        case EPERM:        return "EPERM";
        case ENOENT:       return "ENOENT";
        case ESRCH:        return "ESRCH";
        case EINTR:        return "EINTR";
        case EBADF:        return "EBADF";
        case ECHILD:       return "ECHILD";
        case EAGAIN:       return "EAGAIN";
        case ENOMEM:       return "ENOMEM";
        case EACCES:       return "EACCES";
        case EFAULT:       return "EFAULT";
        case EBUSY:        return "EBUSY";
        case EEXIST:       return "EEXIST";
        case EINVAL:       return "EINVAL";
        case ERANGE:       return "ERANGE";
        case ENAMETOOLONG: return "ENAMETOOLONG";
        case ENOSYS:       return "ENOSYS";
        case ENOEXEC:      return "ENOEXEC";
        case ETXTBSY:      return "ETXTBSY";
        default:           return "E?";
    }
}

/* result token: "OK" if r >= 0, else the errno symbol */
static const char *res_errno(long r) { return r < 0 ? errno_name(errno) : "OK"; }

/* ---- wait(2) status-word decoding into stable, arch-independent tokens ----
 * The raw status word itself IS spec-defined and identical across arches, but a
 * single decoded token ("exit:N", "sig:N", "stop:N", "cont") is more legible in
 * a divergence report and folds the W* macros into one comparison. */
static void emit_wstatus(const char *key, int status) {
    char buf[32];
    if (WIFEXITED(status))
        snprintf(buf, sizeof buf, "exit:%d", WEXITSTATUS(status));
    else if (WIFSIGNALED(status))
        snprintf(buf, sizeof buf, "sig:%d:core=%d", WTERMSIG(status), WCOREDUMP(status) ? 1 : 0);
    else if (WIFSTOPPED(status))
        snprintf(buf, sizeof buf, "stop:%d", WSTOPSIG(status));
    else if (WIFCONTINUED(status))
        snprintf(buf, sizeof buf, "cont");
    else
        snprintf(buf, sizeof buf, "raw:%#x", status);
    emit(key, "%s", buf);
}

/* ---- small helpers shared by several tests ---- */
static pid_t proc_gettid(void) { return (pid_t) syscall(SYS_gettid); }

static void proc_sleep_ms(int ms) {
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* spin until a child changes state (bounded), so tests never hang a cell */
static int proc_wait_blocking(pid_t pid, int *status, int options) {
    int rc;
    do {
        rc = waitpid(pid, status, options);
    } while (rc < 0 && errno == EINTR);
    return rc;
}

static void proc_init(int argc, char **argv) {
    /* Unbuffered stdout: these tests fork heavily and let children emit their
     * own observation lines. A buffered stdout would be DUP'd into the child at
     * fork and re-flushed on the child's exit, double-printing the parent's
     * pending output. Unbuffered side-steps that entire class of bug. */
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *engine = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--engine") && i + 1 < argc)
            engine = argv[++i];
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc)
            i++;                       /* accepted, unused (proc tests are deterministic) */
        else if (!strcmp(argv[i], "--case") && i + 1 < argc)
            i++;                       /* accepted, unused */
        else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose"))
            proc_verbose = 1;
        else if (!strcmp(argv[i], "--list"))
            printf("cases=0\n");       /* no enumerable case list */
    }
    proc_apply_engine(engine);
}

#endif /* ISH_PROC_COMMON_H */
