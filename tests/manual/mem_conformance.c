// mem_conformance.c — self-checking regression lock for the memory-management
// conformance fixes found by differential testing against real Linux (mint).
// Each check asserts the documented Linux behavior that AOK now matches:
//
//   - PROT_NONE is enforced on READS, not just writes: a page mprotect'd from
//     RW down to PROT_NONE faults on read (it used to keep its host backing and
//     silently allow the read).
//   - mprotect PROT_READ makes a writable page fault on write, and re-granting
//     access preserves the original bytes.
//   - mmap MAP_FIXED_NOREPLACE over an existing mapping -> EEXIST (no relocate).
//   - mmap with neither MAP_PRIVATE nor MAP_SHARED -> EINVAL.
//   - mmap with an unaligned (non-FIXED) addr hint -> success (rounded down).
//   - madvise: invalid advice -> EINVAL; unaligned addr -> EINVAL; DONTNEED over
//     an unmapped range -> ENOMEM; DONTNEED zeroes private anon pages.
//   - msync: unknown flag / MS_SYNC|MS_ASYNC -> EINVAL; unmapped range -> ENOMEM.
//   - mincore: mapped -> OK; zero length -> OK; unmapped -> ENOMEM; unaligned ->
//     EINVAL (and it is wired on amd64, not a "missing syscall").
//   - fork copy-on-write: a child's write to a MAP_PRIVATE page leaves the
//     parent's copy unchanged; a MAP_SHARED write is visible in the parent.
//
// The same source is a Tier-0 functional gate (run under AOK, no oracle) and a
// portable check that also passes on a real Linux kernel.
#define _GNU_SOURCE
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include "test_common.h"

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
#ifndef MADV_FREE
#define MADV_FREE 8
#endif
#ifndef MS_ASYNC
#define MS_ASYNC 1
#endif
#ifndef MS_SYNC
#define MS_SYNC 4
#endif

#define RW (PROT_READ | PROT_WRITE)
#define ANON_PRIV (MAP_PRIVATE | MAP_ANONYMOUS)

static long PS = 4096;

static int eq_errno(const char *label, long r, int want) {
    int e = (r < 0) ? errno : 0;
    if (r < 0 && e == want) { test_logf("ok   %s (errno %d)\n", label, e); return 1; }
    printf("FAIL %s: ret=%ld errno=%d, wanted -1/errno=%d\n", label, r, e, want);
    failures_total++;
    return 0;
}
static int is_ok(const char *label, long r) {
    if (r >= 0) { test_logf("ok   %s\n", label); return 1; }
    printf("FAIL %s: ret=%ld errno=%d, wanted success\n", label, r, errno);
    failures_total++;
    return 0;
}
static int is_true(const char *label, int cond) {
    if (cond) { test_logf("ok   %s\n", label); return 1; }
    printf("FAIL %s\n", label);
    failures_total++;
    return 0;
}

// mmap that records the errno symbol indirectly via the returned pointer
static int mmap_failed_with(void *r, int want) {
    return r == MAP_FAILED && errno == want;
}

// ---- fault guard: detect whether an access faults (SIGSEGV/SIGBUS) ----
static sigjmp_buf fault_jb;
static void on_fault(int sig) { (void) sig; siglongjmp(fault_jb, 1); }
static void install_fault_guard(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_fault;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
}
static int faults_read(const volatile void *p) {
    if (sigsetjmp(fault_jb, 1) == 0) { volatile char c = *(const volatile char *) p; (void) c; return 0; }
    return 1;
}
static int faults_write(volatile void *p) {
    if (sigsetjmp(fault_jb, 1) == 0) { *(volatile char *) p = 0x5a; return 0; }
    return 1;
}
static int all_val(const void *p, size_t n, unsigned char v) {
    const unsigned char *b = p;
    for (size_t i = 0; i < n; i++) if (b[i] != v) return 0;
    return 1;
}

static void check_mmap(void) {
    // MAP_FIXED_NOREPLACE over an existing mapping -> EEXIST
    void *occ = mmap(NULL, PS, RW, ANON_PRIV, -1, 0);
    if (occ != MAP_FAILED) {
        memset(occ, 0x44, PS);
        void *r = mmap(occ, PS, RW, ANON_PRIV | MAP_FIXED_NOREPLACE, -1, 0);
        is_true("mmap MAP_FIXED_NOREPLACE over mapping -> EEXIST", mmap_failed_with(r, EEXIST));
        is_true("mmap MAP_FIXED_NOREPLACE leaves original intact", all_val(occ, PS, 0x44));
        if (r != MAP_FAILED && r != occ) munmap(r, PS);
        munmap(occ, PS);
    }

    // neither MAP_PRIVATE nor MAP_SHARED -> EINVAL
    void *n = mmap(NULL, PS, RW, MAP_ANONYMOUS, -1, 0);
    is_true("mmap without sharing flag -> EINVAL", mmap_failed_with(n, EINVAL));
    if (n != MAP_FAILED) munmap(n, PS);

    // unaligned (non-FIXED) addr is a hint -> success
    void *region = mmap(NULL, 2 * PS, RW, ANON_PRIV, -1, 0);
    if (region != MAP_FAILED) {
        munmap(region, 2 * PS);
        void *r = mmap((char *) region + 1, PS, RW, ANON_PRIV, -1, 0);
        is_true("mmap unaligned hint -> success", r != MAP_FAILED);
        if (r != MAP_FAILED) munmap(r, PS);
    }
}

static void check_mprotect(void) {
    void *p = mmap(NULL, PS, RW, ANON_PRIV, -1, 0);
    if (p == MAP_FAILED) { is_true("mprotect setup", 0); return; }
    memset(p, 0x20, PS);

    is_ok("mprotect -> PROT_READ", mprotect(p, PS, PROT_READ));
    is_true("read-only page: read ok", !faults_read(p));
    is_true("read-only page: write faults", faults_write(p));

    is_ok("mprotect -> PROT_NONE", mprotect(p, PS, PROT_NONE));
    is_true("PROT_NONE page: read FAULTS", faults_read(p));   // the read-enforcement fix
    is_true("PROT_NONE page: write faults", faults_write(p));

    is_ok("mprotect -> restore RW", mprotect(p, PS, RW));
    is_true("restored page: content preserved", all_val(p, PS, 0x20));
    is_true("restored page: write ok", !faults_write(p));

    // unmapped range -> ENOMEM. (We do NOT test an unaligned address here: musl's
    // mprotect() rounds the address down to a page boundary before the syscall,
    // so the guest never sees an unaligned value -- it's untestable from C.)
    void *q = mmap(NULL, PS, RW, ANON_PRIV, -1, 0);
    if (q != MAP_FAILED) {
        munmap(q, PS);
        eq_errno("mprotect unmapped -> ENOMEM", mprotect(q, PS, PROT_READ), ENOMEM);
    }
    munmap(p, PS);
}

static void check_madvise(void) {
    void *p = mmap(NULL, 2 * PS, RW, ANON_PRIV, -1, 0);
    if (p == MAP_FAILED) { is_true("madvise setup", 0); return; }
    memset(p, 0xAA, 2 * PS);
    is_ok("madvise MADV_DONTNEED", madvise(p, 2 * PS, MADV_DONTNEED));
    is_true("MADV_DONTNEED zeroes private anon", all_val(p, 2 * PS, 0));

    eq_errno("madvise invalid advice -> EINVAL", madvise(p, PS, 0x7ffe), EINVAL);
    eq_errno("madvise unaligned -> EINVAL", madvise((char *) p + 1, PS, MADV_WILLNEED), EINVAL);
    is_ok("madvise MADV_FREE accepted", madvise(p, PS, MADV_FREE));
    munmap(p, 2 * PS);

    // DONTNEED over an unmapped range -> ENOMEM
    void *q = mmap(NULL, PS, RW, ANON_PRIV, -1, 0);
    if (q != MAP_FAILED) {
        munmap(q, PS);
        eq_errno("madvise DONTNEED unmapped -> ENOMEM", madvise(q, PS, MADV_DONTNEED), ENOMEM);
    }
}

static void check_msync(void) {
    void *a = mmap(NULL, PS, RW, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (a != MAP_FAILED) {
        is_ok("msync MS_SYNC", msync(a, PS, MS_SYNC));
        eq_errno("msync unknown flag -> EINVAL", msync(a, PS, 0x10), EINVAL);
        eq_errno("msync MS_SYNC|MS_ASYNC -> EINVAL", msync(a, PS, MS_SYNC | MS_ASYNC), EINVAL);
        eq_errno("msync unaligned -> EINVAL", msync((char *) a + 1, PS, MS_SYNC), EINVAL);
        munmap(a, PS);
    }
    void *q = mmap(NULL, PS, RW, ANON_PRIV, -1, 0);
    if (q != MAP_FAILED) {
        munmap(q, PS);
        eq_errno("msync unmapped -> ENOMEM", msync(q, PS, MS_SYNC), ENOMEM);
    }
}

static void check_mincore(void) {
    unsigned char vec[8];
    void *p = mmap(NULL, 2 * PS, RW, ANON_PRIV, -1, 0);
    if (p != MAP_FAILED) {
        memset(p, 1, 2 * PS);
        is_ok("mincore mapped", mincore(p, 2 * PS, vec));   // wired on amd64, not ENOSYS
        is_ok("mincore zero length", mincore(p, 0, vec));
        eq_errno("mincore unaligned -> EINVAL", mincore((char *) p + 1, PS, vec), EINVAL);
        munmap(p, 2 * PS);
    }
    void *q = mmap(NULL, PS, RW, ANON_PRIV, -1, 0);
    if (q != MAP_FAILED) {
        munmap(q, PS);
        eq_errno("mincore unmapped -> ENOMEM", mincore(q, PS, vec), ENOMEM);
    }
}

static void check_cow(void) {
    // private: child's write stays private
    void *p = mmap(NULL, PS, RW, ANON_PRIV, -1, 0);
    if (p != MAP_FAILED) {
        memset(p, 0xA1, PS);
        pid_t pid = fork();
        if (pid == 0) { memset(p, 0xB2, PS); _exit(0); }
        waitpid(pid, NULL, 0);
        is_true("fork COW: parent's private page unchanged", all_val(p, PS, 0xA1));
        munmap(p, PS);
    }
    // shared: child's write is visible
    void *s = mmap(NULL, PS, RW, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (s != MAP_FAILED) {
        memset(s, 0xC3, PS);
        pid_t pid = fork();
        if (pid == 0) { memset(s, 0xD4, PS); _exit(0); }
        waitpid(pid, NULL, 0);
        is_true("fork shared: parent sees child's write", all_val(s, PS, 0xD4));
        munmap(s, PS);
    }
}

// MREMAP_FIXED (issue #423 Tier 3): move/grow/shrink to an explicit
// destination address, unmapping whatever was there first.
static void check_mremap_fixed(void) {
    void *src = mmap(NULL, PS * 2, RW, ANON_PRIV, -1, 0);
    void *dest = mmap(NULL, PS * 4, PROT_NONE, ANON_PRIV, -1, 0);
    if (src == MAP_FAILED || dest == MAP_FAILED)
        return;
    munmap(dest, PS * 4); // reserve then release a hole to remap into
    memset(src, 0xAB, PS * 2);

    void *moved = mremap(src, PS * 2, PS * 2, MREMAP_MAYMOVE | MREMAP_FIXED, dest);
    is_true("mremap MREMAP_FIXED move: lands at requested address", moved == dest);
    if (moved != MAP_FAILED)
        is_true("mremap MREMAP_FIXED move: data preserved", all_val(moved, PS * 2, 0xAB));
    if (moved != MAP_FAILED)
        munmap(moved, PS * 2);

    void *src2 = mmap(NULL, PS, RW, ANON_PRIV, -1, 0);
    void *dest2 = mmap(NULL, PS * 3, PROT_NONE, ANON_PRIV, -1, 0);
    if (src2 == MAP_FAILED || dest2 == MAP_FAILED)
        return;
    munmap(dest2, PS * 3);
    memset(src2, 0xCD, PS);
    void *grown = mremap(src2, PS, PS * 2, MREMAP_MAYMOVE | MREMAP_FIXED, dest2);
    if (grown != MAP_FAILED) {
        is_true("mremap MREMAP_FIXED grow: original data preserved", all_val(grown, PS, 0xCD));
        memset((char *) grown + PS, 0xEF, PS);
        is_true("mremap MREMAP_FIXED grow: grown tail is writable", all_val((char *) grown + PS, PS, 0xEF));
        munmap(grown, PS * 2);
    } else {
        is_true("mremap MREMAP_FIXED grow: succeeded", 0);
    }
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    long ps = sysconf(_SC_PAGESIZE);
    if (ps > 0) PS = ps;
    install_fault_guard();

    check_mmap();
    check_mprotect();
    check_madvise();
    check_msync();
    check_mincore();
    check_cow();
    check_mremap_fixed();

    return finish_suite("mem_conformance");
}
