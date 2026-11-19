/*
 * time_common.h — shared harness for AOK-iSH time/clock/timer CONFORMANCE
 * tests (clock_gettime/getres, nanosleep/clock_nanosleep, POSIX timers,
 * setitimer/getitimer, timerfd, gettimeofday/time).
 *
 * Same philosophy as corpus_signal/sig_common.h, corpus_fs/fs_common.h and
 * corpus_proc/proc_common.h: the right oracle for syscall *semantics* is a REAL
 * LINUX KERNEL (mint's Lima x86_64 VM), not Rosetta. The conductor's `conform`
 * subcommand builds the two Linux musl ELFs and compares every iSH (arch x
 * engine) cell against mint:x86_64 / mint:i386. A divergence between an iSH cell
 * and mint is a candidate Linux nonconformance.
 *
 * TIME IS NONDETERMINISTIC, so we NEVER compare raw timestamps. Every emitted
 * key is a VERIFIABLE INVARIANT that holds on any correct kernel regardless of
 * the wall clock:
 *   - monotonicity / ordering (t2 >= t1; MONOTONIC < REALTIME);
 *   - "a sleep lasts at least the requested duration" (elapsed >= requested);
 *   - return value / errno-symbol correctness, flag handling, EINVAL corners;
 *   - which clock ids are supported, and whether a clock reads epoch- vs
 *     boot-relative time (TAI/REALTIME are epoch-based; MONOTONIC/BOOTTIME are
 *     boot-relative) — a structural property, not a specific value.
 * This is the cmpxchg->ZF lesson applied to time: compare the reliable
 * invariant, mask the unpredictable magnitude.
 *
 * Each test prints one canonical line per observation:  <key> res=<value>
 * <key> is a stable, arch-independent identifier (no spaces); <value> is a
 * single whitespace-free token compared byte-for-byte across cells.
 */
#ifndef ISH_TIME_COMMON_H
#define ISH_TIME_COMMON_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>

/* ---- clock ids (Linux numeric values; identical i386/amd64) ----
 * Defined locally so the test passes the exact id the guest ABI uses even when
 * a libc header lacks one (e.g. CLOCK_TAI on older musl). */
#define CLK_REALTIME          0
#define CLK_MONOTONIC         1
#define CLK_PROCESS_CPUTIME   2
#define CLK_THREAD_CPUTIME    3
#define CLK_MONOTONIC_RAW     4
#define CLK_REALTIME_COARSE   5
#define CLK_MONOTONIC_COARSE  6
#define CLK_BOOTTIME          7
#define CLK_TAI               11

#ifndef TIMER_ABSTIME
#define TIMER_ABSTIME 1
#endif

/* ---- engine self-selection (guest only; no-ops on real Linux) ----
 * Identical to the other corpora so one binary covers every iSH cell; on mint's
 * real Linux the /proc/ish knobs are absent (harmless no-ops). */
static void time_write_proc(const char *path, const char *val) {
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return;
    ssize_t w = write(fd, val, strlen(val));
    (void) w;
    close(fd);
}

static void time_self_comm(char *out, size_t n) {
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

static void time_apply_engine(const char *spec) {
    if (!spec)
        return;
    if (strcmp(spec, "amd64:jit") == 0)
        time_write_proc("/proc/ish/amd64_jit", "1");
    else if (strcmp(spec, "amd64:interp") == 0)
        time_write_proc("/proc/ish/amd64_jit", "0");
    else if (strcmp(spec, "i386:no_cache") == 0) {
        char comm[32]; time_self_comm(comm, sizeof comm);
        time_write_proc("/proc/ish/i386_no_cache_comm", comm);
    } else if (strcmp(spec, "i386:single_step") == 0) {
        char comm[32]; time_self_comm(comm, sizeof comm);
        time_write_proc("/proc/ish/i386_single_step_comm", comm);
    }
    /* "i386:jit", "oracle", "native", unknown -> default engine, no knob */
}

static int time_verbose;

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
static void emit_b(const char *key, int cond) { printf("%s res=%d\n", key, cond ? 1 : 0); }

/* map an errno to a stable symbol so the value is arch/libc-independent */
static const char *errno_name(int e) {
    switch (e) {
        case 0:            return "OK";
        case EPERM:        return "EPERM";
        case EINTR:        return "EINTR";
        case EAGAIN:       return "EAGAIN";
        case EFAULT:       return "EFAULT";
        case EBUSY:        return "EBUSY";
        case EEXIST:       return "EEXIST";
        case EINVAL:       return "EINVAL";
        case ERANGE:       return "ERANGE";
        case ENOSYS:       return "ENOSYS";
        case EOVERFLOW:    return "EOVERFLOW";
        case ENODEV:       return "ENODEV";
        case ENOTSUP:      return "ENOTSUP";
        case ETIMEDOUT:    return "ETIMEDOUT";
        default:           return "E?";
    }
}

/* result token: "OK" if r >= 0, else the errno symbol */
static const char *res_errno(long r) { return r < 0 ? errno_name(errno) : "OK"; }

/* ---- SIGSYS guard --------------------------------------------------------
 * iSH delivers SIGSYS for a syscall it does not implement (kill the process by
 * default). To turn a "missing syscall" into a CLEAN divergence (ENOSYS vs the
 * oracle's real result) WITHOUT crashing the rest of the cell, run the call
 * under sysguard(): a SIGSYS handler siglongjmps back and we emit ENOSYS.
 *
 *   TIME_TRY { ... call ... emit(...); } TIME_CATCH { emit(key,"ENOSYS"); }
 *
 * Real Linux never raises SIGSYS for these, so the guard is a transparent
 * no-op there. */
static sigjmp_buf time_sysjmp;
static volatile sig_atomic_t time_sigsys_hit;
static void time_on_sigsys(int sig) { (void) sig; time_sigsys_hit = 1; siglongjmp(time_sysjmp, 1); }

static void time_install_sigsys_guard(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = time_on_sigsys;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSYS, &sa, NULL);
}

/* ---- timespec helpers ---- */
static int64_t ts_to_ns(struct timespec ts) {
    return (int64_t) ts.tv_sec * 1000000000LL + ts.tv_nsec;
}
static int64_t elapsed_ns(struct timespec a, struct timespec b) { /* b - a */
    return ts_to_ns(b) - ts_to_ns(a);
}
static struct timespec ms_ts(long ms) {
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L };
    return ts;
}
/* read a clock via the libc wrapper (musl selects the right syscall per arch:
 * clock_gettime64/403 on i386, clock_gettime/228 on amd64). */
static int clk_now(int id, struct timespec *ts) { return clock_gettime((clockid_t) id, ts); }

static void time_init(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *engine = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--engine") && i + 1 < argc)
            engine = argv[++i];
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc)
            i++;                       /* accepted, unused (time tests are deterministic) */
        else if (!strcmp(argv[i], "--case") && i + 1 < argc)
            i++;                       /* accepted, unused */
        else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose"))
            time_verbose = 1;
        else if (!strcmp(argv[i], "--list"))
            printf("cases=0\n");       /* no enumerable case list */
    }
    time_apply_engine(engine);
}

#endif /* ISH_TIME_COMMON_H */
