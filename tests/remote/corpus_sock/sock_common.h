/*
 * sock_common.h — shared harness for AOK-Workstation socket/IPC CONFORMANCE tests
 * (AF_UNIX + AF_INET, stream + datagram, socketpair, send/recv flags, SO_*
 * options, shutdown, poll/epoll, eventfd, pipe/FIFO, peer credentials).
 *
 * Same philosophy as corpus_signal/sig_common.h, corpus_fs/fs_common.h,
 * corpus_proc/proc_common.h and corpus_time/time_common.h: the right oracle for
 * syscall *semantics* is a REAL LINUX KERNEL (mint's Lima x86_64 VM), not
 * Rosetta. The conductor's `conform` subcommand builds the two Linux musl ELFs
 * and compares every iSH (arch x engine) cell against mint:x86_64 / mint:i386.
 * A divergence between an iSH cell and mint is a candidate Linux nonconformance.
 *
 * NOTE specific to sockets: iSH backs AF_UNIX/AF_INET sockets and pipes with the
 * HOST's real sockets/pipes, so on the dev Mac (and the iOS device) the backing
 * kernel is Darwin. A divergence vs mint therefore usually means Darwin socket
 * semantics are leaking through where iSH should be translating to Linux
 * behavior — a real product bug, since the device is Darwin-backed too. eventfd,
 * the FIFO ring buffer, and the poll/epoll readiness layer are iSH's OWN code
 * (not host-backed) and fully fixable.
 *
 * Each test prints one canonical line per observation:  <key> res=<value>
 * <key> is a stable, arch-independent identifier (no spaces); <value> is a
 * single whitespace-free token compared byte-for-byte across cells. Observations
 * must be ARCH- and RUN-independent: emit errno SYMBOLS, byte counts, message
 * boundaries, readiness bits, derived invariants ("peer cred uid == getuid()" ->
 * 1) — never raw fds, pids, ports, pointers, or addresses that differ per run.
 *
 * CARRY THE cmpxchg LESSON: only assert spec-guaranteed invariants. Where Linux
 * leaves behavior unspecified (exact SO_RCVBUF rounding, scheduling-dependent
 * partial-read sizes, ephemeral port values) do not emit it as a key — emit only
 * the guaranteed fact (e.g. "doubled value is >= what we asked for").
 */
#ifndef ISH_SOCK_COMMON_H
#define ISH_SOCK_COMMON_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <setjmp.h>
#include <signal.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>

/* ---- engine self-selection (guest only; no-ops on real Linux) ----
 * Identical to the other corpora so one binary covers every iSH cell; on mint's
 * real Linux the /proc/ish knobs are absent (harmless no-ops). */
static void sock_write_proc(const char *path, const char *val) {
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return;
    ssize_t w = write(fd, val, strlen(val));
    (void) w;
    close(fd);
}

static void sock_self_comm(char *out, size_t n) {
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

static void sock_apply_engine(const char *spec) {
    if (!spec)
        return;
    if (strcmp(spec, "amd64:jit") == 0)
        sock_write_proc("/proc/ish/amd64_jit", "1");
    else if (strcmp(spec, "amd64:interp") == 0)
        sock_write_proc("/proc/ish/amd64_jit", "0");
    else if (strcmp(spec, "i386:no_cache") == 0) {
        char comm[32]; sock_self_comm(comm, sizeof comm);
        sock_write_proc("/proc/ish/i386_no_cache_comm", comm);
    } else if (strcmp(spec, "i386:single_step") == 0) {
        char comm[32]; sock_self_comm(comm, sizeof comm);
        sock_write_proc("/proc/ish/i386_single_step_comm", comm);
    }
    /* "i386:jit", "oracle", "native", unknown -> default engine, no knob */
}

static int sock_verbose;

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
        case 0:               return "OK";
        case EPERM:           return "EPERM";
        case ENOENT:          return "ENOENT";
        case EINTR:           return "EINTR";
        case EIO:             return "EIO";
        case EBADF:           return "EBADF";
        case EAGAIN:          return "EAGAIN";
        case ENOMEM:          return "ENOMEM";
        case EACCES:          return "EACCES";
        case EFAULT:          return "EFAULT";
        case EBUSY:           return "EBUSY";
        case EEXIST:          return "EEXIST";
        case ENOTDIR:         return "ENOTDIR";
        case EISDIR:          return "EISDIR";
        case EINVAL:          return "EINVAL";
        case ENFILE:          return "ENFILE";
        case EMFILE:          return "EMFILE";
        case ENOSPC:          return "ENOSPC";
        case EPIPE:           return "EPIPE";
        case ERANGE:          return "ERANGE";
        case ENAMETOOLONG:    return "ENAMETOOLONG";
        case ENOSYS:          return "ENOSYS";
        case ENOTSOCK:        return "ENOTSOCK";
        case EDESTADDRREQ:    return "EDESTADDRREQ";
        case EMSGSIZE:        return "EMSGSIZE";
        case EPROTOTYPE:      return "EPROTOTYPE";
        case ENOPROTOOPT:     return "ENOPROTOOPT";
        case EPROTONOSUPPORT: return "EPROTONOSUPPORT";
        case ESOCKTNOSUPPORT: return "ESOCKTNOSUPPORT";
        case EOPNOTSUPP:      return "EOPNOTSUPP";
        case EAFNOSUPPORT:    return "EAFNOSUPPORT";
        case EADDRINUSE:      return "EADDRINUSE";
        case EADDRNOTAVAIL:   return "EADDRNOTAVAIL";
        case ENETUNREACH:     return "ENETUNREACH";
        case ECONNABORTED:    return "ECONNABORTED";
        case ECONNRESET:      return "ECONNRESET";
        case ENOBUFS:         return "ENOBUFS";
        case EISCONN:         return "EISCONN";
        case ENOTCONN:        return "ENOTCONN";
        case ESHUTDOWN:       return "ESHUTDOWN";
        case ETIMEDOUT:       return "ETIMEDOUT";
        case ECONNREFUSED:    return "ECONNREFUSED";
        case EINPROGRESS:     return "EINPROGRESS";
        case EALREADY:        return "EALREADY";
        default:              return "E?";
    }
}

/* result token: "OK" if r >= 0, else the errno symbol */
static const char *res_errno(long r) { return r < 0 ? errno_name(errno) : "OK"; }

/* ---- alarm guard ---------------------------------------------------------
 * Blocking socket/poll waits are guarded by a SIGALRM so that an UNIMPLEMENTED
 * wake (a real bug) surfaces as a clean "TIMEOUT" divergence instead of hanging
 * the whole cell (which would just be reported as HANG with no detail). The
 * handler has no SA_RESTART, so the in-flight syscall returns EINTR. */
static volatile sig_atomic_t sock_alarm_fired;
static void sock_on_alarm(int s) { (void) s; sock_alarm_fired = 1; }
static void sock_arm_alarm(int sec) {
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_handler = sock_on_alarm; sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);
    sock_alarm_fired = 0; alarm(sec);
}
static void sock_disarm_alarm(void) { alarm(0); }

/* ---- per-test scratch directory (for AF_UNIX paths and mkfifo) ----
 * mkdtemp gives a fresh, empty, absolute dir every run so a leftover socket
 * node from a previous cell can never cause a phantom divergence. On mint the
 * VM cwd is a read-only virtiofs mount, so we MUST cd into a writable fs (/tmp);
 * the conform fakefs ships a writable /tmp. The name is never emitted. Kept
 * short so an AF_UNIX sun_path (108 bytes) built from it never overflows. */
static char sock_base[80];

static void sock_setup_dir(const char *name) {
    snprintf(sock_base, sizeof sock_base, "/tmp/skc_%s_XXXXXX", name);
    if (!mkdtemp(sock_base)) { perror("mkdtemp"); exit(2); }
    if (chdir(sock_base)) { perror("chdir"); exit(2); }
}

/* absolute path within the scratch dir */
static const char *sock_path(const char *rel) {
    static char buf[120];
    snprintf(buf, sizeof buf, "%s/%s", sock_base, rel);
    return buf;
}

/* fill a sockaddr_un for a relative name inside the scratch dir */
static socklen_t sock_unix_addr(struct sockaddr_un *sa, const char *rel) {
    memset(sa, 0, sizeof *sa);
    sa->sun_family = AF_UNIX;
    snprintf(sa->sun_path, sizeof sa->sun_path, "%s/%s", sock_base, rel);
    return (socklen_t) (offsetof(struct sockaddr_un, sun_path) + strlen(sa->sun_path) + 1);
}

/* fill a 127.0.0.1 sockaddr_in with the given (host-order) port */
static void sock_inet_addr(struct sockaddr_in *sa, uint16_t port) {
    memset(sa, 0, sizeof *sa);
    sa->sin_family = AF_INET;
    sa->sin_port = htons(port);
    sa->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
}

static pid_t sock_gettid(void) { return (pid_t) syscall(SYS_gettid); }

static void sock_sleep_ms(int ms) {
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* poll one fd for `events` with a bounded timeout; returns poll()'s rc and
 * stores revents. A stable way to ask "is this fd ready?" without blocking. */
static int sock_poll_one(int fd, short events, int timeout_ms, short *revents) {
    struct pollfd pfd = { .fd = fd, .events = events, .revents = 0 };
    int r = poll(&pfd, 1, timeout_ms);
    if (revents)
        *revents = pfd.revents;
    return r;
}

static void sock_init(int argc, char **argv, const char *name) {
    /* Unbuffered stdout: several tests fork a peer that emits its own lines; a
     * buffered stdout would be duplicated at fork and re-flushed by the child. */
    setvbuf(stdout, NULL, _IONBF, 0);
    /* Ignore SIGPIPE so a write to a closed peer surfaces as EPIPE (a value we
     * can compare) instead of killing the cell. Real programs that care use
     * MSG_NOSIGNAL; the EPIPE return is identical on Linux and Darwin. */
    signal(SIGPIPE, SIG_IGN);
    const char *engine = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--engine") && i + 1 < argc)
            engine = argv[++i];
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc)
            i++;                       /* accepted, unused (sock tests are deterministic) */
        else if (!strcmp(argv[i], "--case") && i + 1 < argc)
            i++;                       /* accepted, unused */
        else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose"))
            sock_verbose = 1;
        else if (!strcmp(argv[i], "--list"))
            printf("cases=0\n");       /* no enumerable case list */
    }
    sock_apply_engine(engine);
    if (name)
        sock_setup_dir(name);
}

#endif /* ISH_SOCK_COMMON_H */
