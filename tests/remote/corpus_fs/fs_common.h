/*
 * fs_common.h — shared harness for AOK-iSH filesystem/VFS CONFORMANCE tests.
 *
 * Same philosophy as corpus_signal/sig_common.h: the right oracle for syscall
 * *semantics* is a REAL LINUX KERNEL (mint's Lima x86_64 VM), not Rosetta. The
 * conductor's `conform` subcommand builds the two Linux musl ELFs and compares
 * every iSH (arch x engine) cell against mint:x86_64 / mint:i386. A divergence
 * between an iSH cell and mint is a candidate Linux nonconformance.
 *
 * Each test prints one canonical line per observation:
 *
 *     <key> res=<value>
 *
 * <key> is a stable, arch-independent identifier (no spaces); <value> is a
 * single whitespace-free token compared byte-for-byte across cells. Observations
 * must be ARCH- and RUN-independent: emit errno symbols, mode bits, link counts,
 * S_ISxxx results, derived invariants ("two paths share one inode" -> 1) — never
 * raw inode/device numbers, pids, pointers, or timestamps that differ per run.
 *
 * CARRY THE cmpxchg LESSON: only assert spec-guaranteed invariants. Where Linux
 * leaves behavior unspecified (readdir ordering, st_blocks granularity, exact
 * st_dev value), do not emit it as a key — emit only the guaranteed fact.
 */
#ifndef ISH_FS_COMMON_H
#define ISH_FS_COMMON_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>

/* ---- engine self-selection (guest only; no-ops on real Linux) ----
 * Identical to corpus_signal/sig_common.h so one binary covers every iSH cell;
 * on mint's real Linux the /proc/ish knobs are absent (harmless no-ops). */
static void fs_write_proc(const char *path, const char *val) {
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return;
    ssize_t w = write(fd, val, strlen(val));
    (void) w;
    close(fd);
}

static void fs_self_comm(char *out, size_t n) {
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

static void fs_apply_engine(const char *spec) {
    if (!spec)
        return;
    if (strcmp(spec, "amd64:jit") == 0)
        fs_write_proc("/proc/ish/amd64_jit", "1");
    else if (strcmp(spec, "amd64:interp") == 0)
        fs_write_proc("/proc/ish/amd64_jit", "0");
    else if (strcmp(spec, "i386:no_cache") == 0) {
        char comm[32]; fs_self_comm(comm, sizeof comm);
        fs_write_proc("/proc/ish/i386_no_cache_comm", comm);
    } else if (strcmp(spec, "i386:single_step") == 0) {
        char comm[32]; fs_self_comm(comm, sizeof comm);
        fs_write_proc("/proc/ish/i386_single_step_comm", comm);
    }
    /* "i386:jit", "oracle", "native", unknown -> default engine, no knob */
}

static int fs_verbose;

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
        case EINTR:        return "EINTR";
        case EIO:          return "EIO";
        case ENXIO:        return "ENXIO";
        case EBADF:        return "EBADF";
        case EAGAIN:       return "EAGAIN";
        case ENOMEM:       return "ENOMEM";
        case EACCES:       return "EACCES";
        case EFAULT:       return "EFAULT";
        case EBUSY:        return "EBUSY";
        case EEXIST:       return "EEXIST";
        case EXDEV:        return "EXDEV";
        case ENODEV:       return "ENODEV";
        case ENOTDIR:      return "ENOTDIR";
        case EISDIR:       return "EISDIR";
        case EINVAL:       return "EINVAL";
        case ENFILE:       return "ENFILE";
        case EMFILE:       return "EMFILE";
        case ENOTTY:       return "ENOTTY";
        case EFBIG:        return "EFBIG";
        case ENOSPC:       return "ENOSPC";
        case ESPIPE:       return "ESPIPE";
        case EROFS:        return "EROFS";
        case EMLINK:       return "EMLINK";
        case ERANGE:       return "ERANGE";
        case ENAMETOOLONG: return "ENAMETOOLONG";
        case ENOSYS:       return "ENOSYS";
        case ENOTEMPTY:    return "ENOTEMPTY";
        case ELOOP:        return "ELOOP";
        case EOPNOTSUPP:   return "EOPNOTSUPP";
        case EOVERFLOW:    return "EOVERFLOW";
        default:           return "E?";
    }
}

/* result token for "did this syscall succeed; if not, which errno" */
static const char *res_errno(long r) { return r < 0 ? errno_name(errno) : "OK"; }

/* ---- per-test scratch directory ----
 * mkdtemp gives a fresh, empty, absolute dir every run, so leftover state from a
 * previous cell can never cause a phantom divergence. The name is never emitted.
 * On mint the VM cwd is the read-only virtiofs mount, so we MUST cd into a real
 * writable fs (/tmp). On iSH the conform fakefs ships a writable /tmp. */
static char fs_base[200];

static void fs_setup(const char *name) {
    snprintf(fs_base, sizeof fs_base, "/tmp/fsc_%s_XXXXXX", name);
    if (!mkdtemp(fs_base)) { perror("mkdtemp"); exit(2); }
    if (chdir(fs_base)) { perror("chdir"); exit(2); }
}

/* absolute path within the scratch dir */
static const char *fs_path(const char *rel) {
    static char buf[300];
    snprintf(buf, sizeof buf, "%s/%s", fs_base, rel);
    return buf;
}

/* create a regular file with given contents (best effort); returns 0/-1 */
static int fs_mkfile(const char *path, const char *data) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    if (data && *data) {
        ssize_t w = write(fd, data, strlen(data));
        (void) w;
    }
    close(fd);
    return 0;
}

static void fs_init(int argc, char **argv, const char *name) {
    const char *engine = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--engine") && i + 1 < argc)
            engine = argv[++i];
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc)
            i++;                       /* accepted, unused (fs tests are deterministic) */
        else if (!strcmp(argv[i], "--case") && i + 1 < argc)
            i++;                       /* accepted, unused */
        else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose"))
            fs_verbose = 1;
        else if (!strcmp(argv[i], "--list"))
            printf("cases=0\n");       /* no enumerable case list */
    }
    fs_apply_engine(engine);
    fs_setup(name);
}

#endif /* ISH_FS_COMMON_H */
