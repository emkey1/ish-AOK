/*
 * sig_common.h — shared harness for AOK-Workstation signal/syscall CONFORMANCE tests.
 *
 * Unlike the CPU-instruction corpus (corpus/diff_common.h), the right oracle
 * for signal/syscall *semantics* is a REAL LINUX KERNEL, not Rosetta (which
 * runs a macOS Mach-O with Darwin signal semantics). The conductor's `conform`
 * subcommand therefore builds ONLY the two Linux musl ELFs (i386 + x86_64) and
 * compares the iSH cells against mint's real Linux VM (mint:x86_64 / mint:i386).
 * There is no macOS oracle cell for these tests.
 *
 * A conformance test executes a controlled sequence of signal operations and
 * prints one canonical line per observation:
 *
 *     <key> res=<value>
 *
 * <key> is a stable, arch-independent identifier (no spaces); <value> is a
 * single whitespace-free token. The conductor's key-based compare requires
 * byte-identical <value> for matching <key> across every cell. Observations
 * must therefore be ARCH-INDEPENDENT (si_code, signo, errno symbol, delivery
 * order, masks) — never raw pointers or pids that differ per run. Where a real
 * value is unavoidably per-run (a pid), emit a derived invariant ("matches
 * getpid" -> 1) instead.
 *
 * CARRY THE cmpxchg LESSON: only assert spec-guaranteed invariants. Where Linux
 * leaves behavior unspecified (standard-signal delivery order among different
 * numbers, scheduling races), do not emit an order-dependent key — emit only
 * the guaranteed fact (membership, count). Real-time signal ordering
 * (lowest-numbered first, same-number FIFO) IS POSIX-guaranteed and asserted.
 */
#ifndef ISH_SIG_COMMON_H
#define ISH_SIG_COMMON_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/types.h>

/* ---- engine self-selection (guest only; silently no-ops on real Linux) ----
 * Mirrors corpus/diff_common.h so a signal test compiled once covers every
 * (arch x engine) iSH cell. On mint's real Linux the /proc/ish knobs are
 * absent, so these are harmless no-ops. */
static void sig_write_proc(const char *path, const char *val) {
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return;
    ssize_t w = write(fd, val, strlen(val));
    (void) w;
    close(fd);
}

static void sig_self_comm(char *out, size_t n) {
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

static void sig_apply_engine(const char *spec) {
    if (!spec)
        return;
    if (strcmp(spec, "amd64:jit") == 0)
        sig_write_proc("/proc/ish/amd64_jit", "1");
    else if (strcmp(spec, "amd64:interp") == 0)
        sig_write_proc("/proc/ish/amd64_jit", "0");
    else if (strcmp(spec, "i386:no_cache") == 0) {
        char comm[32]; sig_self_comm(comm, sizeof comm);
        sig_write_proc("/proc/ish/i386_no_cache_comm", comm);
    } else if (strcmp(spec, "i386:single_step") == 0) {
        char comm[32]; sig_self_comm(comm, sizeof comm);
        sig_write_proc("/proc/ish/i386_single_step_comm", comm);
    }
    /* "i386:jit", "oracle", "native", unknown -> default engine, no knob */
}

static int sig_verbose;

static void sig_init(int argc, char **argv) {
    const char *engine = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--engine") && i + 1 < argc)
            engine = argv[++i];
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc)
            i++;                       /* accepted, unused (signal tests are deterministic) */
        else if (!strcmp(argv[i], "--case") && i + 1 < argc)
            i++;                       /* accepted, unused */
        else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose"))
            sig_verbose = 1;
        else if (!strcmp(argv[i], "--list")) {
            /* no enumerable case list; report 0 so the conductor's minimizer is a no-op */
            printf("cases=0\n");
        }
    }
    sig_apply_engine(engine);
}

/* ---- canonical observation emit ---- */

/* emit a key with a preformatted single-token value */
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

/* map a returned errno (negative or via errno) to a stable symbol so the value
 * is arch- and libc-independent. Pass the positive errno value. */
static const char *errno_name(int e) {
    switch (e) {
        case 0:          return "OK";
        case EAGAIN:     return "EAGAIN";
        case EINTR:      return "EINTR";
        case EINVAL:     return "EINVAL";
        case ECHILD:     return "ECHILD";
        case EPERM:      return "EPERM";
        case ESRCH:      return "ESRCH";
        case ENOMEM:     return "ENOMEM";
        case EFAULT:     return "EFAULT";
        case ETIMEDOUT:  return "ETIMEDOUT";
        case ERANGE:     return "ERANGE";
        default:         return "E?";
    }
}

/* small helpers shared by several tests */
static pid_t sig_gettid(void) { return (pid_t) syscall(SYS_gettid); }

static void sig_sleep_ms(int ms) {
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

#endif /* ISH_SIG_COMMON_H */
