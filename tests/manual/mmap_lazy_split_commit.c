// A MAP_FIXED commit into the middle of a large PROT_NONE reservation must give
// the guest pages it can use.
//
// This is how a JVM lays out its heap: reserve the whole -Xmx PROT_NONE, then
// commit generations RW with MAP_FIXED. The old generation starts part way in
// and does not reach the end. AOK records mappings of 64 MiB and more as lazy
// reservations with no page-table entries (emu/memory.h, struct
// mem_lazy_map), and that commit used to materialise the whole PROT_NONE
// reservation -- the commit's own range included -- and then record the RW
// reservation over those entries. A reservation is never consulted where
// entries exist, so every page of the commit faulted SEGV_ACCERR while mmap
// reported success. Gradle's daemon (-XX:+UseSerialGC) and any ParallelGC JVM
// died at old-gen start + 0xc (#572). 48 MiB commits worked, and so did one
// ending exactly at the reservation end.
//
// The same root cause reached further: a large MAP_FIXED anonymous mapping
// over pages that already had entries kept those pages, so a remap read the
// old bytes and a PROT_NONE uncommit left them writable.
//
// A commit that splits its reservation should also not materialise the rest
// of it, and splits must not use up the slots that keep later large mappings
// reservations. On AOK VmRSS counts page-table entries and a reservation has
// none, so VmRSS shows which happened. Those witnesses are checked only after
// an eager commit has been seen to move it, since Linux counts touched pages
// and would not move at all.
//
// Splits leave the remainders of a reservation reserved, where materialising
// used to give them page-table entries, so everything that reads entries has
// to read reservations too. mremap did not: it wanted an entry for every source
// page, and returned EFAULT for a remainder, and for any large mapping not yet
// touched all the way through. And /proc/self/smaps listed only the committed
// page of a reserved heap while maps listed all three regions.
//
// mlock and munlock returned ENOMEM on a remainder or a grown mremap tail, and
// mlockall left those pages unlocked. A shared futex in a remainder was keyed
// by its address while the page was reserved and by its page-table entry once
// touched, so a wait before the first touch never saw the wake after it; and
// the remainder faulting in 2M chunks made two words 2M apart one futex. Every
// mlock that has to succeed stays within the 8 MiB RLIMIT_MEMLOCK of an
// unprivileged Linux process, where the test also runs; mlockall there needs
// privilege.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

#define MB (1024UL * 1024)
// Above MEM_LAZY_SPLIT_LIMIT (32) whatever else the process has reserved.
#define FILL_REGIONS 40
// Far more one-page commits than the split limit allows splits.
#define SPLIT_COMMITS 120

static void check(int cond, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (!cond) {
        printf("FAIL ");
        vprintf(fmt, ap);
        printf("\n");
        failures_total++;
    } else if (test_verbose) {
        printf("ok ");
        vprintf(fmt, ap);
        printf("\n");
    }
    va_end(ap);
}

static sigjmp_buf fault_jb;
static volatile sig_atomic_t fault_sig, fault_code;

static void on_fault(int sig, siginfo_t *si, void *ctx) {
    (void) ctx;
    fault_sig = sig;
    fault_code = si->si_code;
    siglongjmp(fault_jb, 1);
}

// 0 if the access worked, else the si_code of the SIGSEGV it took (-1 for any
// other signal).
static int try_write(char *p, char v) {
    if (sigsetjmp(fault_jb, 1) == 0) {
        *p = v;
        return 0;
    }
    return fault_sig == SIGSEGV ? fault_code : -1;
}

static int try_read(char *p, char *out) {
    if (sigsetjmp(fault_jb, 1) == 0) {
        *out = *p;
        return 0;
    }
    return fault_sig == SIGSEGV ? fault_code : -1;
}

// VmRSS in kB, read without stdio so reading it maps nothing.
static long vm_rss_kb(void) {
    char buf[4096];
    int fd = open("/proc/self/status", O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return -1;
    buf[n] = '\0';
    char *line = strstr(buf, "VmRSS:");
    return line != NULL ? strtol(line + 6, NULL, 10) : -1;
}

static int rss_is_entries;  // set once an eager commit has moved VmRSS

static void check_lazy(const char *what, long before, long after) {
    if (!rss_is_entries)
        return;
    check(after - before < (long) (8 * MB / 1024),
          "%s: split its reservation rather than materialising it (VmRSS %ld -> %ld kB)",
          what, before, after);
}

// For a range expected to stay reserved: VmRSS moved by less than 8 MiB.
static void check_rss_flat(const char *what, long before, long after) {
    if (!rss_is_entries)
        return;
    check(after - before < (long) (8 * MB / 1024),
          "%s: stayed reserved (VmRSS %ld -> %ld kB)", what, before, after);
}

static char *reserve(size_t len) {
    char *p = mmap(NULL, len, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

static int commit(char *at, size_t len, int prot) {
    return mmap(at, len, prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == at;
}

// Fresh pages read zero, and take a write that reads back, at the first, middle
// and last byte of the range.
static void check_rw(const char *what, char *p, size_t len) {
    size_t offs[3] = { 12, len / 2, len - 1 };
    for (int i = 0; i < 3; i++) {
        char v = 0x55;
        int r = try_read(p + offs[i], &v);
        check(r == 0 && v == 0, "%s: +%#zx reads zero (fault=%d value=%#x)",
              what, offs[i], r, (unsigned char) v);
        r = try_write(p + offs[i], (char) (0x41 + i));
        v = 0;
        int r2 = try_read(p + offs[i], &v);
        check(r == 0 && r2 == 0 && v == (char) (0x41 + i),
              "%s: +%#zx write reads back (fault=%d/%d value=%#x)",
              what, offs[i], r, r2, (unsigned char) v);
    }
}

static void check_fault(const char *what, char *p, int want_code) {
    int r = try_write(p, 1);
    check(r == want_code, "%s: write faults %s (got %d)", what,
          want_code == SEGV_ACCERR ? "SEGV_ACCERR" : "SEGV_MAPERR", r);
}

static void commit_inside(size_t len) {
    char what[64];
    snprintf(what, sizeof(what), "%zuM commit inside a 512M reservation", (size_t) (len / MB));
    char *base = reserve(512 * MB);
    if (base == NULL) {
        check(0, "%s: reserve", what);
        return;
    }
    char *at = base + 256 * MB;
    long before = vm_rss_kb();
    int ok = commit(at, len, PROT_READ | PROT_WRITE);
    long after = vm_rss_kb();
    check(ok, "%s: mmap lands where asked", what);
    if (len == 48 * MB)
        rss_is_entries = before >= 0 && after - before >= (long) (len / 1024);
    if (len >= 64 * MB)
        check_lazy(what, before, after);
    check_rw(what, at, len);
    check_fault("PROT_NONE page below the commit", at - 1, SEGV_ACCERR);
    check_fault("PROT_NONE page above the commit", at + len, SEGV_ACCERR);
    munmap(base, 512 * MB);
}

// The commit's position against the reservation: the start, ending exactly at
// the end, and crossing the end into a hole.
static void commit_at_edges(void) {
    char *base = reserve(512 * MB);
    if (base == NULL) {
        check(0, "edge cases: reserve");
        return;
    }
    check(commit(base, 64 * MB, PROT_READ | PROT_WRITE), "commit at the start");
    check_rw("commit at the start", base, 64 * MB);
    check_fault("PROT_NONE page above a commit at the start", base + 64 * MB, SEGV_ACCERR);
    char *tail = base + 448 * MB;
    check(commit(tail, 64 * MB, PROT_READ | PROT_WRITE), "commit ending at the end");
    check_rw("commit ending at the end", tail, 64 * MB);
    check_fault("PROT_NONE page below a commit ending at the end", tail - 1, SEGV_ACCERR);
    munmap(base, 512 * MB);

    // Reserve 640M and give the top 128M back, so what lies past the 512M
    // reservation is a hole this test owns. Nothing may print in between: a
    // first printf can allocate, and could land in that hole.
    base = reserve(640 * MB);
    if (base == NULL) {
        check(0, "crossing the end: reserve");
        return;
    }
    int unmapped = munmap(base + 512 * MB, 128 * MB) == 0;
    int ok = commit(base + 448 * MB, 128 * MB, PROT_READ | PROT_WRITE);
    check(unmapped, "crossing the end: munmap the tail");
    check(ok, "commit crossing the end");
    check_rw("commit crossing the end", base + 448 * MB, 128 * MB);
    check_fault("PROT_NONE page below a commit crossing the end", base + 448 * MB - 1,
                SEGV_ACCERR);
    munmap(base, 640 * MB);
}

// Commits into what earlier splits left, then mprotect and munmap across them.
static void commit_into_remainders(void) {
    char *base = reserve(512 * MB);
    if (base == NULL) {
        check(0, "remainders: reserve");
        return;
    }
    char *c1 = base + 128 * MB, *c2 = base + 320 * MB, *c3 = base + 16 * MB;
    check(commit(c1, 64 * MB, PROT_READ | PROT_WRITE), "first commit");
    check_rw("first commit", c1, 64 * MB);
    long before = vm_rss_kb();
    check(commit(c2, 64 * MB, PROT_READ | PROT_WRITE), "second commit, right remainder");
    long mid = vm_rss_kb();
    check(commit(c3, 64 * MB, PROT_READ | PROT_WRITE), "third commit, left remainder");
    long after = vm_rss_kb();
    check_lazy("second commit", before, mid);
    check_lazy("third commit", mid, after);
    check_rw("second commit", c2, 64 * MB);
    check_rw("third commit", c3, 64 * MB);
    char v = 0;
    int got = try_read(c1 + 12, &v);
    check(got == 0 && v == 0x41, "first commit keeps its data (%#x)", (unsigned char) v);
    check_fault("PROT_NONE below the third commit", base, SEGV_ACCERR);
    check_fault("PROT_NONE between third and first", base + 100 * MB, SEGV_ACCERR);
    check_fault("PROT_NONE between first and second", base + 200 * MB, SEGV_ACCERR);
    check_fault("PROT_NONE above the second commit", base + 500 * MB, SEGV_ACCERR);

    // mprotect back to PROT_NONE, and back again: the bytes survive.
    check(mprotect(c1, 64 * MB, PROT_NONE) == 0, "mprotect first commit PROT_NONE");
    check_fault("first commit after PROT_NONE", c1 + 12, SEGV_ACCERR);
    check(mprotect(c1, 64 * MB, PROT_READ | PROT_WRITE) == 0, "mprotect it RW again");
    v = 0;
    got = try_read(c1 + 12, &v);
    check(got == 0 && v == 0x41, "its data survived (%#x)", (unsigned char) v);
    v = 0;
    got = try_read(c2 + 12, &v);
    check(got == 0 && v == 0x41, "second commit untouched by that (%#x)", (unsigned char) v);

    // munmap part of a commit, and part of a PROT_NONE remainder.
    check(munmap(c2 + 24 * MB, 16 * MB) == 0, "munmap 16M inside the second commit");
    check_fault("the hole in the second commit", c2 + 32 * MB, SEGV_MAPERR);
    v = 0;
    got = try_read(c2 + 12, &v);
    check(got == 0 && v == 0x41, "below that hole keeps its data (%#x)", (unsigned char) v);
    v = 0;
    got = try_read(c2 + 64 * MB - 1, &v);
    check(got == 0 && v == 0x43, "above that hole keeps its data (%#x)", (unsigned char) v);
    check_rw("above that hole, untouched", c2 + 48 * MB, 8 * MB);
    check(munmap(base + 400 * MB, 16 * MB) == 0, "munmap 16M of PROT_NONE remainder");
    check_fault("the hole in the remainder", base + 408 * MB, SEGV_MAPERR);
    check_fault("remainder above that hole", base + 420 * MB, SEGV_ACCERR);
    check_fault("remainder below that hole", base + 399 * MB, SEGV_ACCERR);
    check(commit(base + 400 * MB, 16 * MB, PROT_READ | PROT_WRITE), "commit into that hole");
    check_rw("commit into that hole", base + 400 * MB, 16 * MB);
    munmap(base, 512 * MB);
}

// A large MAP_FIXED anonymous mapping over pages that already have entries.
static void map_over_entries(void) {
    // mprotect materialises the reservation it touches, in full.
    char *base = reserve(512 * MB);
    if (base == NULL) {
        check(0, "over entries: reserve");
        return;
    }
    check(mprotect(base, 4096, PROT_READ | PROT_WRITE) == 0, "mprotect one page RW");
    check(commit(base + 256 * MB, 64 * MB, PROT_READ | PROT_WRITE),
          "commit into a materialised reservation");
    check_rw("commit into a materialised reservation", base + 256 * MB, 64 * MB);
    check_fault("materialised PROT_NONE below it", base + 256 * MB - 1, SEGV_ACCERR);
    munmap(base, 512 * MB);

    char *p = mmap(NULL, 128 * MB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        check(0, "over entries: map 128M");
        return;
    }
    size_t offs[3] = { 12, 64 * MB, 128 * MB - 1 };
    for (int i = 0; i < 3; i++)
        p[offs[i]] = 0x5a;
    check(commit(p, 128 * MB, PROT_READ | PROT_WRITE), "fresh 128M over a dirty one");
    check_rw("fresh 128M over a dirty one", p, 128 * MB);
    check(mmap(p, 128 * MB, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE,
               -1, 0) == p, "PROT_NONE uncommit over it");
    check_fault("uncommitted page", p + 64 * MB, SEGV_ACCERR);
    check(commit(p, 128 * MB, PROT_READ | PROT_WRITE), "recommit");
    check_rw("recommit", p, 128 * MB);
    munmap(p, 128 * MB);
}

static const char *mremap_result(void *p) {
    return p == MAP_FAILED ? strerror(errno) : "ok";
}

// mremap of reserved address space: remainders a split left, and large
// mappings touched only in part. Reserved pages move as reservations, and a
// grown tail joins them, so none of these builds page tables for untouched
// memory.
static void mremap_reservations(void) {
    // A: grow 16M of the PROT_NONE remainder above a one-page commit.
    char *b = reserve(256 * MB);
    if (b == NULL) {
        check(0, "mremap A: reserve");
        return;
    }
    char *c = b + 64 * MB;
    check(commit(c, 4096, PROT_READ | PROT_WRITE), "mremap A: commit one page");
    c[7] = 0x3c;
    long before = vm_rss_kb();
    char *q = mremap(b + 128 * MB, 16 * MB, 32 * MB, MREMAP_MAYMOVE);
    long after = vm_rss_kb();
    check(q != MAP_FAILED, "mremap A: grow 16M of a PROT_NONE remainder to 32M (%s)",
          mremap_result(q));
    if (q != MAP_FAILED) {
        check_rss_flat("mremap A: the moved range", before, after);
        check_fault("mremap A: moved, still PROT_NONE", q, SEGV_ACCERR);
        check_fault("mremap A: grown tail, PROT_NONE", q + 32 * MB - 1, SEGV_ACCERR);
        check_fault("mremap A: where it was", b + 136 * MB, SEGV_MAPERR);
        check_fault("mremap A: the remainder below it", b + 127 * MB, SEGV_ACCERR);
        check_fault("mremap A: the remainder above it", b + 144 * MB, SEGV_ACCERR);
        check(mprotect(q, 32 * MB, PROT_READ | PROT_WRITE) == 0, "mremap A: mprotect it RW");
        check_rw("mremap A: moved and grown, made RW", q, 32 * MB);
        munmap(q, 32 * MB);
    }
    // A shrink in place inside a remainder.
    q = mremap(b + 200 * MB, 16 * MB, 8 * MB, 0);
    check(q == b + 200 * MB, "mremap A: shrink 16M of a remainder in place (%s)",
          mremap_result(q));
    check_fault("mremap A: the half the shrink freed", b + 212 * MB, SEGV_MAPERR);
    check_fault("mremap A: the half it kept", b + 204 * MB, SEGV_ACCERR);
    char v = 0;
    int got = try_read(c + 7, &v);
    check(got == 0 && v == 0x3c, "mremap A: the commit kept its data (%#x)", (unsigned char) v);
    munmap(b, 256 * MB);

    // B: an RW mapping touched near both ends with a hole punched in it; grow
    // the part above the hole, which moves it.
    char *r = mmap(NULL, 256 * MB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (r == MAP_FAILED) {
        check(0, "mremap B: map 256M");
        return;
    }
    r[5] = 'x';
    check(munmap(r + 64 * MB, 16 * MB) == 0, "mremap B: munmap a hole");
    r[200 * MB] = 'y';
    q = mremap(r + 80 * MB, 176 * MB, 352 * MB, MREMAP_MAYMOVE);
    check(q != MAP_FAILED, "mremap B: grow the part above the hole to 352M (%s)",
          mremap_result(q));
    if (q != MAP_FAILED) {
        v = 0;
        got = try_read(q + 120 * MB, &v);
        check(got == 0 && v == 'y', "mremap B: its data moved (%#x)", (unsigned char) v);
        check_rw("mremap B: the moved, untouched top", q + 160 * MB, 16 * MB);
        check_rw("mremap B: the grown tail", q + 176 * MB, 176 * MB);
        check_fault("mremap B: where it was", r + 100 * MB, SEGV_MAPERR);
        munmap(q, 352 * MB);
    }
    v = 0;
    got = try_read(r + 5, &v);
    check(got == 0 && v == 'x', "mremap B: the part below the hole kept its data");
    munmap(r, 64 * MB);

    // C: MREMAP_FIXED of 32M that is partly faulted in and partly reserved.
    r = mmap(NULL, 256 * MB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (r == MAP_FAILED) {
        check(0, "mremap C: map 256M");
        return;
    }
    r[1] = 'a';
    check(munmap(r + 64 * MB, 16 * MB) == 0, "mremap C: munmap a hole");
    r[100 * MB] = 'z';
    char *hole = reserve(64 * MB);
    check(hole != NULL && munmap(hole, 64 * MB) == 0, "mremap C: make a destination hole");
    q = mremap(r + 96 * MB, 32 * MB, 32 * MB, MREMAP_MAYMOVE | MREMAP_FIXED, hole);
    check(q == hole, "mremap C: MREMAP_FIXED 32M into the hole (%s)", mremap_result(q));
    if (q == hole) {
        v = 0;
        got = try_read(q + 4 * MB, &v);
        check(got == 0 && v == 'z', "mremap C: its data moved (%#x)", (unsigned char) v);
        check_rw("mremap C: the moved, untouched part", q + 16 * MB, 16 * MB);
        check_fault("mremap C: where it was", r + 100 * MB, SEGV_MAPERR);
        check_rw("mremap C: the mapping above it", r + 128 * MB, 64 * MB);
        munmap(q, 32 * MB);
    }
    v = 0;
    got = try_read(r + 1, &v);
    check(got == 0 && v == 'a', "mremap C: the part below the hole kept its data");
    munmap(r, 64 * MB);
    munmap(r + 80 * MB, 176 * MB);

    // D: realloc's shape. A large mapping touched in its first chunk, grown to
    // twice its size by moving, moved whole with MREMAP_FIXED, and grown in place.
    char *p = mmap(NULL, 256 * MB, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {
        check(0, "mremap D: map 256M");
        return;
    }
    p[1 * MB] = 0x77;
    before = vm_rss_kb();
    q = mremap(p, 256 * MB, 512 * MB, MREMAP_MAYMOVE);
    after = vm_rss_kb();
    check(q != MAP_FAILED, "mremap D: grow a 256M mapping to 512M (%s)", mremap_result(q));
    if (q == MAP_FAILED) {
        munmap(p, 256 * MB);
        return;
    }
    check_rss_flat("mremap D: 256M grown to 512M", before, after);
    v = 0;
    got = try_read(q + 1 * MB, &v);
    check(got == 0 && v == 0x77, "mremap D: its data moved (%#x)", (unsigned char) v);

    // Into the bottom of a 768M reservation, whose top is then given back so
    // there is room to grow in place. Nothing may print in between: a first
    // printf can allocate, and could land in that room.
    char *area = reserve(768 * MB);
    before = vm_rss_kb();
    char *fixed = area != NULL
            ? mremap(q, 512 * MB, 512 * MB, MREMAP_MAYMOVE | MREMAP_FIXED, area) : MAP_FAILED;
    long mid = vm_rss_kb();
    int room = fixed == area && munmap(area + 512 * MB, 256 * MB) == 0;
    long before_grow = vm_rss_kb();
    char *grown = room ? mremap(area, 512 * MB, 768 * MB, 0) : MAP_FAILED;
    after = vm_rss_kb();
    check(area != NULL && fixed == area, "mremap D: MREMAP_FIXED it into a reservation (%s)",
          mremap_result(fixed));
    if (fixed == area)
        check_rss_flat("mremap D: moved whole with MREMAP_FIXED", before, mid);
    check(room, "mremap D: give back the reservation's top");
    check(grown == area, "mremap D: grow in place to 768M (%s)", mremap_result(grown));
    if (grown == area)
        check_rss_flat("mremap D: 512M grown in place to 768M", before_grow, after);
    char *last = fixed == area ? area : q;
    size_t last_len = grown == area ? 768 * MB : 512 * MB;
    v = 0;
    got = try_read(last + 1 * MB, &v);
    check(got == 0 && v == 0x77, "mremap D: data still there (%#x)", (unsigned char) v);
    check_rw("mremap D: moved, untouched", last + 128 * MB, 8 * MB);
    check_rw("mremap D: the grown tail", last + 256 * MB, last_len - 256 * MB);
    munmap(last, last_len);
    if (area != NULL && fixed != area)
        munmap(area, 768 * MB);
}

// mremap with old_len 0 asks for a second mapping of the same pages, which
// only a shared mapping may have. A large one is reserved, here faulted in
// only at its first page, and the alias must still share the pages.
static void mremap_alias(void) {
    char *sh = mmap(NULL, 128 * MB, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (sh == MAP_FAILED) {
        check(0, "mremap alias: map 128M shared");
        return;
    }
    sh[0] = 'x';
    char *alias = mremap(sh, 0, 16 * MB, MREMAP_MAYMOVE);
    check(alias != MAP_FAILED, "mremap alias: alias 16M of a shared mapping (%s)", mremap_result(alias));
    if (alias != MAP_FAILED) {
        char via_alias = 0, via_original = 0;
        sh[8 * MB] = 'y';
        alias[12 * MB] = 'z';
        int got = try_read(alias + 8 * MB, &via_alias);
        if (got == 0)
            got = try_read(sh + 12 * MB, &via_original);
        check(got == 0 && via_alias == 'y' && via_original == 'z' && alias[0] == 'x',
              "mremap alias: the alias and the original share their pages (%#x, %#x)",
              (unsigned char) via_alias, (unsigned char) via_original);
        munmap(alias, 16 * MB);
    }
    munmap(sh, 128 * MB);
    char *priv = mmap(NULL, 128 * MB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (priv != MAP_FAILED) {
        errno = 0;
        char *bad = mremap(priv, 0, 16 * MB, MREMAP_MAYMOVE);
        check(bad == MAP_FAILED && errno == EINVAL,
              "mremap alias: no alias of a private mapping (%s)", mremap_result(bad));
        if (bad != MAP_FAILED)
            munmap(bad, 16 * MB);
        munmap(priv, 128 * MB);
    }
    // An alias gives page tables only to what it covers: 16M of an untouched
    // 512M shared mapping, not the 512M.
    sh = mmap(NULL, 512 * MB, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (sh != MAP_FAILED) {
        long before = vm_rss_kb();
        char *alias16 = mremap(sh, 0, 16 * MB, MREMAP_MAYMOVE);
        long after = vm_rss_kb();
        check(alias16 != MAP_FAILED, "mremap alias: alias 16M of an untouched 512M shared mapping (%s)",
              mremap_result(alias16));
        if (alias16 != MAP_FAILED) {
            if (rss_is_entries)
                check(after - before >= (long) (16 * MB / 1024) && after - before < (long) (64 * MB / 1024),
                      "mremap alias: page tables for the 16M aliased, not the 512M mapping "
                      "(VmRSS %ld -> %ld kB)", before, after);
            sh[200 * MB] = 'q';
            char v = 0;
            int got = try_read(sh + 200 * MB, &v);
            check(got == 0 && v == 'q', "mremap alias: the rest of the mapping still works (%#x)",
                  (unsigned char) v);
            munmap(alias16, 16 * MB);
        }
        munmap(sh, 512 * MB);
    }
}

// maps and smaps must list the same regions, reservations included, and a
// reservation has nothing resident.
static size_t read_proc(const char *path, char *buf, size_t size) {
    int fd = open(path, O_RDONLY);
    size_t len = 0;
    if (fd < 0)
        return 0;
    for (;;) {
        ssize_t n = read(fd, buf + len, size - 1 - len);
        if (n <= 0)
            break;
        len += (size_t) n;
        if (len >= size - 1)
            break;
    }
    close(fd);
    buf[len] = '\0';
    return len;
}

// Regions overlapping [lo, hi), or all of them when hi is 0.
static int count_regions(const char *text, unsigned long lo, unsigned long hi) {
    int n = 0;
    for (const char *line = text; line != NULL && *line != '\0'; ) {
        unsigned long s, e;
        char perms[8];
        if (sscanf(line, "%lx-%lx %7s", &s, &e, perms) == 3 &&
                (hi == 0 || (e > lo && s < hi)))
            n++;
        line = strchr(line, '\n');
        if (line != NULL)
            line++;
    }
    return n;
}

// A field of the smaps entry for the region starting at `start`, or -1.
static long smaps_field(const char *text, unsigned long start, const char *field) {
    char head[32];
    snprintf(head, sizeof(head), "%08lx-", start);
    for (const char *line = text; line != NULL && *line != '\0'; ) {
        if (strncmp(line, head, strlen(head)) == 0) {
            const char *f = strstr(line, field);
            const char *next = strstr(line + 1, "VmFlags:");
            if (f == NULL || (next != NULL && f > next))
                return -1;
            return strtol(f + strlen(field), NULL, 10);
        }
        line = strchr(line, '\n');
        if (line != NULL)
            line++;
    }
    return -1;
}

static void maps_and_smaps_agree(void) {
    static char maps[1 << 18], smaps[1 << 21];
    char *h = reserve(512 * MB);
    if (h == NULL) {
        check(0, "smaps: reserve");
        return;
    }
    check(commit(h + 256 * MB, 4096, PROT_READ | PROT_WRITE), "smaps: commit one page");
    size_t nm = read_proc("/proc/self/maps", maps, sizeof(maps));
    size_t ns = read_proc("/proc/self/smaps", smaps, sizeof(smaps));
    check(nm != 0 && ns != 0 && nm < sizeof(maps) - 1 && ns < sizeof(smaps) - 1,
          "smaps: read maps (%zu bytes) and smaps (%zu bytes)", nm, ns);
    unsigned long lo = (unsigned long) h, hi = lo + 512 * MB;
    int heap_maps = count_regions(maps, lo, hi), heap_smaps = count_regions(smaps, lo, hi);
    check(heap_maps == 3 && heap_smaps == 3,
          "smaps: maps and smaps both list the heap's 3 regions (maps=%d smaps=%d)",
          heap_maps, heap_smaps);
    int all_maps = count_regions(maps, 0, 0), all_smaps = count_regions(smaps, 0, 0);
    check(all_maps == all_smaps, "smaps: maps and smaps list as many regions (%d, %d)",
          all_maps, all_smaps);
    long size = smaps_field(smaps, lo, "Size:");
    long rss = smaps_field(smaps, lo, "Rss:");
    check(size == (long) (256 * MB / 1024) && rss == 0,
          "smaps: the reserved part below the commit is 256M with nothing resident "
          "(Size %ld kB, Rss %ld kB)", size, rss);
    munmap(h, 512 * MB);
}

// Splits must not take the slots later large mappings need. A PROT_NONE heap
// takes one-page commits 4M apart, each strictly inside what is left of it, so
// each splits the rest, until a split is refused and the rest is materialised,
// which moves VmRSS. Large mappings made after that must still be reservations.
// When splits could take every slot, the table was full at that point: the
// refused split freed one slot, the next mapping took it back, and the one
// after that was mapped eagerly, paying the host memory a reservation saves.
static void splits_do_not_starve(void) {
    char *heap = reserve(512 * MB);
    if (heap == NULL) {
        check(0, "no starvation: reserve");
        return;
    }
    int splits = 0, refused_at = 0;
    long before = vm_rss_kb(), after = before;
    for (int i = 1; i <= SPLIT_COMMITS; i++) {
        long prev = vm_rss_kb();
        if (!commit(heap + (size_t) i * 4 * MB, 4096, PROT_READ | PROT_WRITE)) {
            check(0, "no starvation: commit %d", i);
            break;
        }
        after = vm_rss_kb();
        if (after - prev >= (long) (64 * MB / 1024)) {
            refused_at = i;
            break;
        }
        splits++;
    }
    if (rss_is_entries) {
        check(refused_at != 0,
              "no starvation: a split was refused within %d commits (VmRSS %ld -> %ld kB)",
              SPLIT_COMMITS, before, after);
        check(splits >= 16,
              "no starvation: %d commits split the heap before one was refused", splits);
    }
    int last = refused_at != 0 ? refused_at : SPLIT_COMMITS;
    check_rw("no starvation: first commit", heap + 4 * MB, 4096);
    if (last > 1)
        check_rw("no starvation: last commit", heap + (size_t) last * 4 * MB, 4096);
    check_fault("no starvation: PROT_NONE between commits", heap + 6 * MB, SEGV_ACCERR);
    check_fault("no starvation: PROT_NONE past the last commit",
                heap + (size_t) last * 4 * MB + 4096, SEGV_ACCERR);

    enum { LATER = 8 };
    char *later[LATER];
    int made = 0;
    before = vm_rss_kb();
    for (; made < LATER; made++) {
        later[made] = mmap(NULL, 64 * MB, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (later[made] == MAP_FAILED)
            break;
    }
    after = vm_rss_kb();
    check(made == LATER, "no starvation: %d later 64M mappings made (%d)", LATER, made);
    if (rss_is_entries)
        check(after - before < (long) (16 * MB / 1024),
              "no starvation: %d later 64M mappings stayed reservations (VmRSS %ld -> %ld kB)",
              made, before, after);
    if (made > 0)
        check_rw("no starvation: last later mapping", later[made - 1], 64 * MB);
    while (made > 0)
        munmap(later[--made], 64 * MB);
    munmap(heap, 512 * MB);
}

// With MEM_LAZY_SPLIT_LIMIT slots in use a split is refused, and each path
// materialises the reservation holding the range in full instead. VmRSS moving
// by what that materialises is what shows the refusal branch ran.
static void at_split_limit(void) {
    char *regions[FILL_REGIONS + 2];
    size_t sizes[FILL_REGIONS + 2];
    int n = 0;
    for (; n < FILL_REGIONS; n++) {
        sizes[n] = n < 3 ? 128 * MB : 64 * MB;
        if ((regions[n] = reserve(sizes[n])) == NULL)
            break;
    }
    if (n < FILL_REGIONS) {
        // An address space too small for ~2.7 GB of reservations, i.e. a
        // 32-bit guest: not a defect, and the rest of the test still ran.
        printf("mmap_lazy_split_commit: note: split-limit cases skipped (reserved %d of %d)\n",
               n, FILL_REGIONS);
        while (n > 0) {
            n--;
            munmap(regions[n], sizes[n]);
        }
        return;
    }

    // munmap of a hole: pt_unmap_always materialises the 128M and unmaps 16M.
    char *r = regions[0];
    long before = vm_rss_kb();
    check(munmap(r + 56 * MB, 16 * MB) == 0, "at the split limit: munmap a hole");
    long after = vm_rss_kb();
    if (rss_is_entries)
        check(after - before >= (long) (112 * MB / 1024),
              "at the split limit: the munmap materialised (VmRSS %ld -> %ld kB)", before, after);
    check_fault("at the split limit: the hole", r + 64 * MB, SEGV_MAPERR);
    check_fault("at the split limit: below the hole", r + 55 * MB, SEGV_ACCERR);
    check_fault("at the split limit: above the hole", r + 73 * MB, SEGV_ACCERR);
    sizes[n] = 64 * MB;
    regions[n++] = reserve(64 * MB);   // retake the slot that freed

    // A commit large enough to be a reservation: mem_lazy_reserve materialises
    // the 128M and declines, and the eager map replaces 64M of it. Growth by
    // only 64M would mean the commit went eager over a split that happened.
    r = regions[1];
    before = vm_rss_kb();
    check(commit(r + 32 * MB, 64 * MB, PROT_READ | PROT_WRITE), "at the split limit: 64M commit");
    after = vm_rss_kb();
    if (rss_is_entries)
        check(after - before >= (long) (128 * MB / 1024),
              "at the split limit: the commit materialised (VmRSS %ld -> %ld kB)", before, after);
    check_rw("at the split limit: 64M commit", r + 32 * MB, 64 * MB);
    check_fault("at the split limit: below the 64M commit", r + 32 * MB - 1, SEGV_ACCERR);
    check_fault("at the split limit: above the 64M commit", r + 96 * MB, SEGV_ACCERR);
    sizes[n] = 64 * MB;
    regions[n++] = reserve(64 * MB);

    // A small one: pt_map materialises the 128M and maps 1M over it.
    r = regions[2];
    before = vm_rss_kb();
    check(commit(r + 64 * MB, 1 * MB, PROT_READ | PROT_WRITE), "at the split limit: 1M commit");
    after = vm_rss_kb();
    if (rss_is_entries)
        check(after - before >= (long) (128 * MB / 1024),
              "at the split limit: the 1M commit materialised (VmRSS %ld -> %ld kB)", before, after);
    check_rw("at the split limit: 1M commit", r + 64 * MB, 1 * MB);
    check_fault("at the split limit: below the 1M commit", r + 64 * MB - 1, SEGV_ACCERR);
    check_fault("at the split limit: above the 1M commit", r + 65 * MB, SEGV_ACCERR);

    // mremap of the first 16M of an untouched 64M reservation. Moving it as a
    // reservation would add one, which the limit refuses, so it is materialised
    // -- the 16M moved, not the 64M it came from -- and its grown tail gets
    // entries rather than a reservation of its own.
    r = regions[3];
    before = vm_rss_kb();
    char *moved = mremap(r, 16 * MB, 32 * MB, MREMAP_MAYMOVE);
    after = vm_rss_kb();
    check(moved != MAP_FAILED, "at the split limit: mremap 16M of a reservation (%s)",
          moved == MAP_FAILED ? strerror(errno) : "ok");
    if (rss_is_entries)
        check(after - before >= (long) (16 * MB / 1024) && after - before < (long) (64 * MB / 1024),
              "at the split limit: the mremap materialised the range it moved, not its "
              "reservation (VmRSS %ld -> %ld kB)", before, after);
    if (moved != MAP_FAILED) {
        check_fault("at the split limit: the moved range", moved, SEGV_ACCERR);
        check_fault("at the split limit: its grown tail", moved + 32 * MB - 1, SEGV_ACCERR);
        check_fault("at the split limit: where it was", r + 8 * MB, SEGV_MAPERR);
        check_fault("at the split limit: the rest of its reservation", r + 16 * MB, SEGV_ACCERR);
        munmap(moved, 32 * MB);
    }

    // mlock of the first 4M of an untouched PROT_NONE reservation marks that
    // part locked by splitting it off, which the limit refuses, so the 4M is
    // populated and locked instead -- the 4M, not the reservation. Linux
    // cannot populate PROT_NONE and says ENOMEM.
    r = regions[4];
    before = vm_rss_kb();
    int locked = mlock(r, 4 * MB);
    int lock_err = errno;
    after = vm_rss_kb();
    check(locked == 0 || lock_err == ENOMEM,
          "at the split limit: mlock 4M of a PROT_NONE reservation (%s)",
          locked == 0 ? "ok" : strerror(lock_err));
    if (rss_is_entries && locked == 0)
        check(after - before >= (long) (4 * MB / 1024) && after - before < (long) (64 * MB / 1024),
              "at the split limit: the refused mark populated the 4M, not its reservation "
              "(VmRSS %ld -> %ld kB)", before, after);
    check_fault("at the split limit: the locked 4M", r + 1 * MB, SEGV_ACCERR);
    check_fault("at the split limit: the rest of that reservation", r + 32 * MB, SEGV_ACCERR);
    check(munlock(r, 4 * MB) == 0, "at the split limit: munlock the 4M");

    while (n > 0) {
        n--;
        if (regions[n] != NULL)
            munmap(regions[n], sizes[n]);
    }
}

static const char *call_result(int r, int err) {
    return r == 0 ? "ok" : strerror(err);
}

// One byte a page: every page is written, without writing every byte.
static void touch_pages(char *p, size_t len, char v) {
    for (size_t off = 0; off < len; off += 4096)
        p[off] = v;
}

// mlock and munlock of reserved memory: a split's remainders and a grown mremap
// tail. mlock populates what it locks, as Linux does, and only that.
static void mlock_reservations(void) {
    char *m = mmap(NULL, 512 * MB, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (m == MAP_FAILED) {
        check(0, "mlock: map 512M");
        return;
    }
    m[0] = 1;
    check(munmap(m + 64 * MB, 16 * MB) == 0, "mlock: munmap a hole");

    long before = vm_rss_kb();
    int r = mlock(m + 128 * MB, 4 * MB);
    int err = errno;
    long after = vm_rss_kb();
    check(r == 0, "mlock: 4M of a split remainder (%s)", call_result(r, err));
    if (r == 0 && rss_is_entries)
        check(after - before >= (long) (4 * MB / 1024) && after - before < (long) (64 * MB / 1024),
              "mlock: populated the 4M it locked, not its reservation (VmRSS %ld -> %ld kB)",
              before, after);
    check_rw("mlock: the locked 4M", m + 128 * MB, 4 * MB);
    r = munlock(m + 128 * MB, 4 * MB);
    check(r == 0, "munlock: the same 4M (%s)", call_result(r, errno));

    before = vm_rss_kb();
    r = munlock(m + 256 * MB, 128 * MB);
    err = errno;
    after = vm_rss_kb();
    check(r == 0, "munlock: 128M of a remainder nobody locked (%s)", call_result(r, err));
    check_rss_flat("munlock: 128M of a remainder", before, after);

    before = vm_rss_kb();
    r = mlock(m + 400 * MB, 4096);
    err = errno;
    after = vm_rss_kb();
    check(r == 0, "mlock: one page inside a remainder (%s)", call_result(r, err));
    check_rss_flat("mlock: one page inside a remainder", before, after);
    check_rw("mlock: the locked page", m + 400 * MB, 4096);
    check_rw("mlock: the page above it", m + 400 * MB + 4096, 4096);
    r = munlock(m + 400 * MB, 4096);
    check(r == 0, "munlock: the page (%s)", call_result(r, errno));

    // A range running into the hole is ENOMEM, and must not populate first.
    before = vm_rss_kb();
    errno = 0;
    r = mlock(m + 60 * MB, 8 * MB);
    err = errno;
    after = vm_rss_kb();
    check(r == -1 && err == ENOMEM, "mlock: a range running into the hole is ENOMEM (%s)",
          r == 0 ? "ok" : strerror(err));
    check_rss_flat("mlock: the refused range", before, after);
    munmap(m, 64 * MB);
    munmap(m + 80 * MB, 432 * MB);

    // A PROT_NONE remainder is locked but not populated. Linux cannot populate
    // it and says ENOMEM, having locked it anyway, and so does AOK, as for a
    // small PROT_NONE mapping (tests/manual/mlock_accounting).
    char *h = reserve(512 * MB);
    if (h != NULL) {
        check(commit(h + 256 * MB, 4096, PROT_READ | PROT_WRITE), "mlock: commit one page");
        before = vm_rss_kb();
        r = mlock(h + 300 * MB, 32 * MB);
        err = errno;
        after = vm_rss_kb();
        check(r == 0 || err == ENOMEM, "mlock: 32M of a PROT_NONE remainder (%s)",
              call_result(r, err));
        check_rss_flat("mlock: 32M of a PROT_NONE remainder", before, after);
        check_fault("mlock: the locked PROT_NONE remainder", h + 310 * MB, SEGV_ACCERR);
        r = munlock(h + 300 * MB, 32 * MB);
        check(r == 0, "munlock: the PROT_NONE remainder (%s)", call_result(r, errno));
        munmap(h, 512 * MB);
    }

    // The tail an mremap grows onto a small, fully touched mapping is a
    // reservation of its own.
    char *p = mmap(NULL, 32 * MB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        check(0, "mlock: map 32M");
        return;
    }
    touch_pages(p, 32 * MB, 1);
    char *q = mremap(p, 32 * MB, 128 * MB, MREMAP_MAYMOVE);
    check(q != MAP_FAILED, "mlock: grow a touched 32M mapping to 128M (%s)", mremap_result(q));
    if (q == MAP_FAILED) {
        munmap(p, 32 * MB);
        return;
    }
    before = vm_rss_kb();
    r = mlock(q + 100 * MB, 4 * MB);
    err = errno;
    after = vm_rss_kb();
    check(r == 0, "mlock: 4M of an mremap-grown tail (%s)", call_result(r, err));
    if (r == 0 && rss_is_entries)
        check(after - before >= (long) (4 * MB / 1024) && after - before < (long) (64 * MB / 1024),
              "mlock: populated the 4M of the tail it locked (VmRSS %ld -> %ld kB)", before, after);
    r = munlock(q + 100 * MB, 4 * MB);
    check(r == 0, "munlock: the same 4M of the tail (%s)", call_result(r, errno));
    r = mlock(q + 30 * MB, 4 * MB);
    check(r == 0, "mlock: 4M across the old end into the tail (%s)", call_result(r, errno));
    r = munlock(q + 30 * MB, 4 * MB);
    check(r == 0, "munlock: 4M across the old end into the tail (%s)", call_result(r, errno));
    munmap(q, 128 * MB);
}

#ifndef MCL_ONFAULT
#define MCL_ONFAULT 4
#endif

static int swap_evict_pid(pid_t pid) {
    FILE *ev = fopen("/proc/ish/swap_evict", "w");
    if (ev == NULL)
        return -1;
    fprintf(ev, "%d\n", (int) pid);
    return fclose(ev) == 0 ? 0 : -1;
}

// The sum of a smaps field over the regions overlapping [lo, hi).
static long smaps_sum(const char *text, unsigned long lo, unsigned long hi, const char *field) {
    long sum = 0;
    int in = 0;
    for (const char *line = text; line != NULL && *line != '\0'; ) {
        unsigned long s, e;
        char perms[8];
        if (sscanf(line, "%lx-%lx %7s", &s, &e, perms) == 3)
            in = e > lo && s < hi;
        else if (in && strncmp(line, field, strlen(field)) == 0)
            sum += strtol(line + strlen(field), NULL, 10);
        line = strchr(line, '\n');
        if (line != NULL)
            line++;
    }
    return sum;
}

// The child half of mlockall_reservations; returns failures_total.
static unsigned mlockall_child(void) {
    // MCL_CURRENT: the RW remainders are populated, the PROT_NONE reservation
    // is not.
    char *rw = mmap(NULL, 256 * MB, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    char *pn = reserve(256 * MB);
    if (rw == MAP_FAILED || pn == NULL) {
        check(0, "mlockall: map 256M RW and 256M PROT_NONE");
        return failures_total;
    }
    rw[0] = 1;
    check(munmap(rw + 64 * MB, 16 * MB) == 0, "mlockall: munmap a hole");
    long before = vm_rss_kb();
    int r = mlockall(MCL_CURRENT);
    int err = errno;
    long after = vm_rss_kb();
    if (r != 0 && (err == ENOMEM || err == EPERM) && geteuid() != 0) {
        printf("mmap_lazy_split_commit: note: mlockall needs privilege here (%s), "
               "not checked\n", strerror(err));
        return failures_total;
    }
    check(r == 0, "mlockall(MCL_CURRENT) (%s)", r == 0 ? "ok" : strerror(err));
    check(after - before >= (long) (200 * MB / 1024) && after - before < (long) (400 * MB / 1024),
          "mlockall(MCL_CURRENT): populated the 240M of RW remainders and not the 256M "
          "PROT_NONE reservation (VmRSS %ld -> %ld kB)", before, after);
    check_rw("mlockall(MCL_CURRENT): a remainder", rw + 200 * MB, 4 * MB);
    check_fault("mlockall(MCL_CURRENT): the PROT_NONE reservation", pn + 100 * MB, SEGV_ACCERR);
    r = munlockall();
    check(r == 0, "munlockall (%s)", call_result(r, errno));
    munmap(rw, 64 * MB);
    munmap(rw + 80 * MB, 176 * MB);
    munmap(pn, 256 * MB);

    // MCL_CURRENT|MCL_ONFAULT: nothing is populated, and what materialises
    // later is locked.
    rw = mmap(NULL, 256 * MB, PROT_READ | PROT_WRITE,
              MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (rw == MAP_FAILED) {
        check(0, "mlockall: map 256M RW again");
        return failures_total;
    }
    rw[0] = 1;
    check(munmap(rw + 64 * MB, 16 * MB) == 0, "mlockall: munmap a hole again");
    before = vm_rss_kb();
    r = mlockall(MCL_CURRENT | MCL_ONFAULT);
    err = errno;
    after = vm_rss_kb();
    if (r != 0 && err == EINVAL) {
        printf("mmap_lazy_split_commit: note: no MCL_ONFAULT here, not checked\n");
        munmap(rw, 64 * MB);
        munmap(rw + 80 * MB, 176 * MB);
        return failures_total;
    }
    check(r == 0, "mlockall(MCL_CURRENT|MCL_ONFAULT) (%s)", r == 0 ? "ok" : strerror(err));
    check(after - before < (long) (16 * MB / 1024),
          "mlockall(MCL_CURRENT|MCL_ONFAULT): populated nothing (VmRSS %ld -> %ld kB)", before, after);
    // Below the hole, away from the pages the swap check below reads: smaps
    // would list them in one region with anything unlocked beside them.
    before = vm_rss_kb();
    r = munlock(rw + 20 * MB, 8 * MB);
    err = errno;
    after = vm_rss_kb();
    check(r == 0, "munlock: 8M of a remainder mlockall locked (%s)", call_result(r, err));
    check(after - before < (long) (4 * MB / 1024),
          "munlock: 8M of a locked remainder populated nothing (VmRSS %ld -> %ld kB)", before, after);

    // Locked for real: with swap on, a forced eviction takes the pages of a
    // mapping made after the mlockall (no MCL_FUTURE, so unlocked) and must not
    // take the pages the locked remainder materialised after it. AOK only.
    // The control sits between PROT_NONE guards: smaps merges neighbouring
    // read-write regions, and a control placed in the hole beside the remainder
    // was listed as one region with it.
    char *locked = rw + 128 * MB;
    char *guard = reserve(18 * MB);
    char *ctl = guard == NULL ? MAP_FAILED
            : mmap(guard + 1 * MB, 16 * MB, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (ctl != MAP_FAILED) {
        for (size_t off = 0; off < 16 * MB; off += 4096) {
            locked[off] = (char) (0x5a ^ (off >> 12));
            ctl[off] = (char) (0xa5 ^ (off >> 12));
        }
        int evicted = 0;
        for (int sweep = 0; sweep < 3 && evicted == 0; sweep++) {
            if (swap_evict_pid(getpid()) != 0)
                break;
            static char smaps_text[1 << 21];
            if (read_proc("/proc/self/smaps", smaps_text, sizeof(smaps_text)) == 0)
                break;
            long ctl_swap = smaps_sum(smaps_text, (unsigned long) ctl,
                                      (unsigned long) ctl + 16 * MB, "Swap:");
            if (ctl_swap > 0) {
                long locked_swap = smaps_sum(smaps_text, (unsigned long) locked,
                                             (unsigned long) locked + 16 * MB, "Swap:");
                evicted = 1;
                check(locked_swap == 0,
                      "mlockall(MCL_CURRENT|MCL_ONFAULT): pages a locked remainder "
                      "materialised later stay out of swap (Swap %ld kB, unlocked "
                      "control %ld kB)", locked_swap, ctl_swap);
            }
        }
        if (!evicted)
            test_logf("mlockall: nothing evicted (swap off?), lock witness not checked\n");
        int same = 1;
        for (size_t off = 0; off < 16 * MB; off += 4096)
            same &= locked[off] == (char) (0x5a ^ (off >> 12)) && ctl[off] == (char) (0xa5 ^ (off >> 12));
        check(same, "mlockall: both mappings read back what was written");
    }
    if (guard != NULL)
        munmap(guard, 18 * MB);
    r = munlockall();
    check(r == 0, "munlockall again (%s)", call_result(r, errno));
    munmap(rw, 64 * MB);
    munmap(rw + 80 * MB, 176 * MB);
    return failures_total;
}

// mlockall locks and populates the whole address space, so it runs in a child.
static void mlockall_reservations(void) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        check(0, "mlockall: fork (%s)", strerror(errno));
        return;
    }
    if (pid == 0) {
        // failures_total came across the fork; count only the child's own.
        unsigned inherited = failures_total;
        unsigned fails = mlockall_child() - inherited;
        fflush(stdout);
        _exit(fails > 100 ? 100 : (int) fails);
    }
    int status;
    if (waitpid(pid, &status, 0) != pid) {
        check(0, "mlockall: wait (%s)", strerror(errno));
        return;
    }
    if (WIFEXITED(status))
        failures_total += (unsigned) WEXITSTATUS(status);
    else
        check(0, "mlockall: the child died (status %#x)", status);
}

static long futex_call(int *word, int op, int val, const struct timespec *t) {
#ifdef SYS_futex_time64
    if (t != NULL && sizeof t->tv_sec > sizeof(long))
        return syscall(SYS_futex_time64, word, op, val, t, NULL, 0);
#endif
    return syscall(SYS_futex, word, op, val, t, NULL, 0);
}

static double mono_seconds(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double) t.tv_sec + t.tv_nsec / 1e9;
}

struct futex_waiter {
    int *word;
    long ret;
    int err;
    double took;
};

// A shared (not FUTEX_PRIVATE_FLAG) wait for *word to stop being 0, of up to 4s.
static void *futex_wait_run(void *arg) {
    struct futex_waiter *w = arg;
    struct timespec timeout = { 4, 0 };
    double t0 = mono_seconds();
    w->ret = futex_call(w->word, FUTEX_WAIT, 0, &timeout);
    w->err = errno;
    w->took = mono_seconds() - t0;
    return NULL;
}

static void sleep_ms(long ms) {
    struct timespec ts = { ms / 1000, ms % 1000 * 1000000 };
    nanosleep(&ts, NULL);
}

// A shared futex in a remainder of a MAP_SHARED|MAP_ANONYMOUS mapping.
static void futex_in_reservations(void) {
    char *m = mmap(NULL, 128 * MB, PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (m == MAP_FAILED) {
        check(0, "futex: map 128M shared");
        return;
    }
    m[0] = 1;
    check(munmap(m + 16 * MB, 16 * MB) == 0, "futex: munmap a hole");

    // Two words at the same offset of two 2M chunks the remainder faulted in
    // separately. A wake for one must reach its own waiter, not the other.
    char *base = m + 32 * MB;
    int *w1 = (int *) (base + 4096), *w2 = (int *) (base + 2 * MB + 4096);
    *w1 = 0;
    *w2 = 0;
    int decided = 0;
    for (int attempt = 0; attempt < 4 && !decided; attempt++) {
        struct futex_waiter a = { .word = w2 }, b = { .word = w1 };
        pthread_t ta, tb;
        if (pthread_create(&ta, NULL, futex_wait_run, &a) != 0) {
            check(0, "futex: pthread_create");
            break;
        }
        sleep_ms(200);
        if (pthread_create(&tb, NULL, futex_wait_run, &b) != 0) {
            check(0, "futex: pthread_create");
            futex_call(w2, FUTEX_WAKE, 1, NULL);
            pthread_join(ta, NULL);
            break;
        }
        sleep_ms(300);
        long woke = futex_call(w1, FUTEX_WAKE, 1, NULL);
        pthread_join(tb, NULL);
        futex_call(w2, FUTEX_WAKE, 1, NULL);
        pthread_join(ta, NULL);
        if (woke == 0)
            continue;
        decided = 1;
        check(b.took < 2.0,
              "futex: a wake 2M away at the same chunk offset does not take the waiter "
              "(woke %ld; its own waiter %.2fs, the other %.2fs)", woke, b.took, a.took);
    }
    if (!decided)
        printf("mmap_lazy_split_commit: note: the chunk-offset futex waits never both "
               "blocked, not checked\n");

    // Wait on a page nobody has touched, then touch it and wake. Above the
    // chunks just faulted, so each attempt's page is still reserved. A try whose
    // wake found nobody queued, because the waiter had not blocked yet, says
    // nothing either way and is repeated on a fresh page.
    decided = 0;
    for (int attempt = 0; attempt < 4 && !decided; attempt++) {
        int *word = (int *) (m + 48 * MB + (size_t) attempt * 16 * MB + 8);
        struct futex_waiter w = { .word = word };
        pthread_t t;
        if (pthread_create(&t, NULL, futex_wait_run, &w) != 0) {
            check(0, "futex: pthread_create");
            break;
        }
        sleep_ms(300);
        *word = 1;
        long woke = futex_call(word, FUTEX_WAKE, 1, NULL);
        pthread_join(t, NULL);
        if (woke == 0 && w.ret != 0 && w.err == EAGAIN)
            continue;
        decided = 1;
        check(w.took < 2.0,
              "futex: a wait on an untouched page of a shared remainder sees the wake "
              "after the page is touched (woke %ld, wait %.2fs, %s)",
              woke, w.took, w.ret == 0 ? "ok" : strerror(w.err));
    }
    if (!decided)
        printf("mmap_lazy_split_commit: note: the untouched-page futex wait never blocked "
               "before its wake, not checked\n");

    // Across fork: a child waits in the remainder, the parent wakes it.
    int *word = (int *) (m + 100 * MB + 8);
    *word = 0;
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        struct timespec timeout = { 4, 0 };
        double t0 = mono_seconds();
        long r = futex_call(word, FUTEX_WAIT, 0, &timeout);
        double took = mono_seconds() - t0;
        _exit(r == 0 || took < 2.0 ? 0 : 1);
    }
    if (pid > 0) {
        long woke = 0;
        for (int i = 0; i < 20 && woke == 0; i++) {
            sleep_ms(100);
            woke = futex_call(word, FUTEX_WAKE, 1, NULL);
        }
        *word = 1;
        futex_call(word, FUTEX_WAKE, 1, NULL);
        int status;
        waitpid(pid, &status, 0);
        check(woke == 1 && WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "futex: a wake from the parent reaches a child waiting in the shared remainder "
              "(woke %ld, child status %#x)", woke, status);
    } else {
        check(0, "futex: fork (%s)", strerror(errno));
    }
    munmap(m, 16 * MB);
    munmap(m + 32 * MB, 96 * MB);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(120));

    char *probe = reserve(640 * MB);
    if (probe == NULL) {
        printf("mmap_lazy_split_commit: SKIP (cannot reserve 640MB)\n");
        return 0;
    }
    munmap(probe, 640 * MB);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);

    // The eager-path controls first: the 48M one is also what shows whether
    // VmRSS counts entries here.
    commit_inside(48 * MB);
    commit_inside(1 * MB);
    if (!rss_is_entries)
        printf("mmap_lazy_split_commit: note: VmRSS does not count untouched "
               "pages, so the split witnesses are not checked\n");
    commit_inside(64 * MB);
    commit_inside(128 * MB);
    commit_at_edges();
    commit_into_remainders();
    map_over_entries();
    mremap_reservations();
    mremap_alias();
    maps_and_smaps_agree();
    splits_do_not_starve();
    at_split_limit();
    mlock_reservations();
    mlockall_reservations();
    futex_in_reservations();

    return finish_suite("mmap_lazy_split_commit");
}
