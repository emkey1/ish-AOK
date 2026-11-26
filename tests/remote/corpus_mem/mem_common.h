/*
 * mem_common.h — shared harness for AOK-Workstation memory-management CONFORMANCE tests
 * (mmap/munmap, mprotect, mremap, madvise, brk/sbrk, mincore, msync, and COW
 * after fork).
 *
 * Same philosophy as corpus_signal/sig_common.h, corpus_fs/fs_common.h,
 * corpus_proc/proc_common.h and corpus_time/time_common.h: the right oracle for
 * syscall *semantics* is a REAL LINUX KERNEL (mint's Lima x86_64 VM), not
 * Rosetta. The conductor's `conform` subcommand builds the two Linux musl ELFs
 * and compares every iSH (arch x engine) cell against mint:x86_64 / mint:i386.
 * A divergence between an iSH cell and mint is a candidate Linux nonconformance.
 *
 * ADDRESSES VARY PER RUN, so we NEVER compare raw pointers. Every emitted key is
 * a VERIFIABLE INVARIANT that holds on any correct kernel regardless of where the
 * kernel happens to place a mapping:
 *   - did the mapping succeed (MAP_FAILED vs a usable pointer) / which errno;
 *   - are the contents correct (freshly-mapped anon reads back zero; a written
 *     byte reads back; MADV_DONTNEED zeroes private anon; COW keeps the parent's
 *     bytes after a child write);
 *   - is protection ENFORCED (a write to a PROT_READ page, or any access to a
 *     PROT_NONE / unmapped page, must fault) — observed via a SIGSEGV/SIGBUS
 *     guard, emitted as FAULT vs OK, never an address;
 *   - placement RELATIONSHIPS that are spec-guaranteed (MAP_FIXED lands exactly
 *     at the requested page; MAP_FIXED_NOREPLACE over an existing mapping is
 *     EEXIST; a non-MAYMOVE in-place grow that cannot fit is ENOMEM) — expressed
 *     as "== requested" / "!= requested" / errno, not the address itself.
 * This is the cmpxchg->ZF lesson applied to memory: compare the reliable
 * invariant, mask the unpredictable address.
 *
 * Each test prints one canonical line per observation:  <key> res=<value>
 * <key> is a stable, arch-independent identifier (no spaces); <value> is a
 * single whitespace-free token compared byte-for-byte across cells.
 */
#ifndef ISH_MEM_COMMON_H
#define ISH_MEM_COMMON_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>

/* ---- mmap/madvise/msync flag values (Linux numeric; identical i386/amd64) ----
 * Defined locally so the test passes the exact bit the guest ABI uses even when
 * a libc header lacks one (older musl predates MAP_FIXED_NOREPLACE / MADV_FREE). */
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
#ifndef MADV_FREE
#define MADV_FREE 8
#endif
#ifndef MADV_DONTNEED
#define MADV_DONTNEED 4
#endif
#ifndef MADV_WILLNEED
#define MADV_WILLNEED 3
#endif
#ifndef MREMAP_MAYMOVE
#define MREMAP_MAYMOVE 1
#endif
#ifndef MREMAP_FIXED
#define MREMAP_FIXED 2
#endif

/* ---- engine self-selection (guest only; no-ops on real Linux) ----
 * Identical to the other corpora so one binary covers every iSH cell; on mint's
 * real Linux the /proc/ish knobs are absent (harmless no-ops). */
static void mem_write_proc(const char *path, const char *val) {
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return;
    ssize_t w = write(fd, val, strlen(val));
    (void) w;
    close(fd);
}

static void mem_self_comm(char *out, size_t n) {
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

static void mem_apply_engine(const char *spec) {
    if (!spec)
        return;
    if (strcmp(spec, "amd64:jit") == 0)
        mem_write_proc("/proc/ish/amd64_jit", "1");
    else if (strcmp(spec, "amd64:interp") == 0)
        mem_write_proc("/proc/ish/amd64_jit", "0");
    else if (strcmp(spec, "i386:no_cache") == 0) {
        char comm[32]; mem_self_comm(comm, sizeof comm);
        mem_write_proc("/proc/ish/i386_no_cache_comm", comm);
    } else if (strcmp(spec, "i386:single_step") == 0) {
        char comm[32]; mem_self_comm(comm, sizeof comm);
        mem_write_proc("/proc/ish/i386_single_step_comm", comm);
    }
    /* "i386:jit", "oracle", "native", unknown -> default engine, no knob */
}

static int mem_verbose;
static long PS = 4096;   /* page size; set in mem_init */

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
        case ENOENT:       return "ENOENT";
        case EINTR:        return "EINTR";
        case EBADF:        return "EBADF";
        case EAGAIN:       return "EAGAIN";
        case ENOMEM:       return "ENOMEM";
        case EACCES:       return "EACCES";
        case EFAULT:       return "EFAULT";
        case EBUSY:        return "EBUSY";
        case EEXIST:       return "EEXIST";
        case ENODEV:       return "ENODEV";
        case EINVAL:       return "EINVAL";
        case ENFILE:       return "ENFILE";
        case EMFILE:       return "EMFILE";
        case ERANGE:       return "ERANGE";
        case ENOSYS:       return "ENOSYS";
        case EOVERFLOW:    return "EOVERFLOW";
        case ETXTBSY:      return "ETXTBSY";
        default:           return "E?";
    }
}

/* result token for "did this syscall succeed; if not, which errno" */
static const char *res_errno(long r) { return r < 0 ? errno_name(errno) : "OK"; }
/* result token for mmap (which signals failure with MAP_FAILED, not a sign) */
static const char *res_mmap(void *p) { return p == MAP_FAILED ? errno_name(errno) : "OK"; }

/* ---- SIGSEGV/SIGBUS fault guard --------------------------------------------
 * To check that protection is ENFORCED, we attempt an access and observe whether
 * it faults. A handler siglongjmps back so the fault doesn't kill the cell; we
 * report FAULT vs OK. sigsetjmp(.,1) saves/restores the signal mask so SIGSEGV is
 * unblocked again for the next probe (it is auto-blocked while the handler runs).
 *
 * A correct kernel faults on: write to PROT_READ, any access to PROT_NONE, and
 * any access to an unmapped page. If iSH ever failed to enforce a protection,
 * the probe would read OK where mint reads FAULT -> a clean divergence. */
static sigjmp_buf mem_faultjmp;
static void mem_on_fault(int sig) { (void) sig; siglongjmp(mem_faultjmp, 1); }

static void mem_install_fault_guard(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = mem_on_fault;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
}

/* 1 if reading *p faulted, else 0 (and the byte read is discarded) */
static int probe_read(const volatile void *p) {
    if (sigsetjmp(mem_faultjmp, 1) == 0) {
        volatile char c = *(const volatile char *) p;
        (void) c;
        return 0;
    }
    return 1;
}

/* 1 if writing v to *p faulted, else 0 */
static int probe_write(volatile void *p, char v) {
    if (sigsetjmp(mem_faultjmp, 1) == 0) {
        *(volatile char *) p = v;
        return 0;
    }
    return 1;
}

/* FAULT / OK token for a guarded read */
static const char *probe_read_tok(const volatile void *p) { return probe_read(p) ? "FAULT" : "OK"; }
/* FAULT / OK token for a guarded write */
static const char *probe_write_tok(volatile void *p, char v) { return probe_write(p, v) ? "FAULT" : "OK"; }

/* ---- small helpers ---- */

/* are the first n bytes at p all zero? (no fault expected; caller ensures map) */
static int all_zero(const void *p, size_t n) {
    const unsigned char *b = p;
    for (size_t i = 0; i < n; i++)
        if (b[i] != 0)
            return 0;
    return 1;
}

/* fill n bytes with a recognizable pattern */
static void fill_pat(void *p, size_t n, unsigned char seed) {
    unsigned char *b = p;
    for (size_t i = 0; i < n; i++)
        b[i] = (unsigned char) (seed + (i & 0x3f));
}

static int check_pat(const void *p, size_t n, unsigned char seed) {
    const unsigned char *b = p;
    for (size_t i = 0; i < n; i++)
        if (b[i] != (unsigned char) (seed + (i & 0x3f)))
            return 0;
    return 1;
}

/* convenience: anonymous private RW mapping of `pages` pages, or NULL */
static void *map_anon(size_t pages, int prot) {
    void *p = mmap(NULL, pages * (size_t) PS, prot,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

static void mem_init(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *engine = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--engine") && i + 1 < argc)
            engine = argv[++i];
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc)
            i++;                       /* accepted, unused (mem tests are deterministic) */
        else if (!strcmp(argv[i], "--case") && i + 1 < argc)
            i++;                       /* accepted, unused */
        else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose"))
            mem_verbose = 1;
        else if (!strcmp(argv[i], "--list"))
            printf("cases=0\n");       /* no enumerable case list */
    }
    long ps = sysconf(_SC_PAGESIZE);
    if (ps > 0)
        PS = ps;
    mem_apply_engine(engine);
    mem_install_fault_guard();
}

#endif /* ISH_MEM_COMMON_H */
