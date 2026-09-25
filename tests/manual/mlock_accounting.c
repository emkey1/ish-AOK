// mlock(2) as the process can see it: VmLck in /proc/self/status, Locked and
// "lo" in smaps, and the region boundaries in maps -- and the lock following
// its mapping wherever Linux's VM_LOCKED does.
//
// AOK's lock was real (a locked page is never paged out) but invisible: VmLck
// and Locked were printed as 0 whatever was locked, maps did not split where a
// lock began, and the lock fell off a page whenever the page was replaced or
// the mapping grew or moved -- stack growth, mremap, a move, an alias. And
// mlockall(MCL_FUTURE) was recorded and never applied, MAP_LOCKED ignored.
//
// Every expectation below was MEASURED on Linux 6.12 (camd, x86_64 and -m32,
// as root and as an ordinary user) before it was asserted here:
//  - mlock of 4 pages of a private r-x file mapping raises VmLck by 16 kB and
//    makes the region "lo" with Locked 16 kB; a forced write
//    (/proc/self/mem) into one of them changes neither, nor the maps line;
//  - mlock of the middle page of a 3-page mapping prints 3 maps lines (a file
//    one at offsets 0, 0x1000, 0x2000); munlock joins them back into 1;
//  - mlock rounds an unaligned start down; a range with a hole locks up to
//    the hole and fails ENOMEM; a PROT_NONE page makes it ENOMEM with the
//    whole range locked and populated only up to that page; a wrapping range
//    is EINVAL; [vdso] and [vvar] are never locked;
//  - MADV_DONTNEED, FREE, COLD, PAGEOUT and REMOVE on a locked page are
//    EINVAL, and the page keeps its contents;
//  - a fork's child holds no locks, and a locked page written after the fork
//    stays locked in the parent;
//  - mremap keeps the lock when it grows a mapping (in place or moving it,
//    populating the growth), moves or shrinks it, and an alias of a locked
//    shared mapping is locked too; one across a lock boundary is EFAULT;
//  - mprotect that makes a locked PROT_NONE mapping writable populates it;
//  - mlockall(MCL_CURRENT) locks everything but the special mappings, VmLck
//    is the sum of the "lo" regions, and a locked stack grows locked;
//  - under MCL_FUTURE a new anonymous, file or PROT_NONE mapping, a brk and a
//    shmat are locked and (but PROT_NONE) populated; MCL_ONFAULT leaves them
//    unpopulated; an older mapping grown by mremap is not locked; a later
//    mlockall(MCL_CURRENT) or munlockall ends it; a fork's child does not
//    inherit it; MAP_LOCKED locks one mapping;
//  - (AOK only, with swap on) locked pages stay out of swap, the growth of a
//    locked mapping and an MCL_FUTURE mapping included;
//  - RLIMIT_MEMLOCK: 0 makes mlock, mlockall and MAP_LOCKED EPERM; over the
//    limit mlock is ENOMEM (but re-locking locked pages is free), mlockall
//    (MCL_CURRENT) ENOMEM, and mmap, mremap growth or an alias of a locked
//    mapping EAGAIN, a brk a failure; a locked stack growing past it SIGSEGV.
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/shm.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_common.h"

_Static_assert(sizeof(off_t) == 8, "the /proc/self/mem offsets need a 64-bit off_t");

// Older headers (the i386 root's musl 1.1.24) predate these.
#ifndef MCL_ONFAULT
#define MCL_ONFAULT 4
#endif
#ifndef MADV_COLD
#define MADV_COLD 20
#endif
#ifndef MADV_PAGEOUT
#define MADV_PAGEOUT 21
#endif
#ifndef MADV_FREE
#define MADV_FREE 8
#endif

#define PG 4096ul
#define MB (1024ul * 1024)

static void check(const char *what, int ok) {
    if (!ok) {
        printf("FAIL %s\n", what);
        failures_total++;
    } else {
        test_logf("ok   %s\n", what);
    }
}

// ---- /proc, read without allocating ----------------------------------------
//
// Under mlockall(MCL_FUTURE) every mapping the C library makes is locked too,
// so a stdio read of /proc that mallocs a buffer changes the number it is
// reading. Everything here goes through read(2) into static memory.

static char procbuf[1 << 20];

static size_t read_proc(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    size_t n = 0;
    ssize_t r;
    while (n < sizeof procbuf - 1 && (r = read(fd, procbuf + n, sizeof procbuf - 1 - n)) > 0)
        n += (size_t) r;
    close(fd);
    procbuf[n] = '\0';
    return n;
}

// A kB field of /proc/self/status; -1 when absent.
static long status_kb(const char *key) {
    if (read_proc("/proc/self/status") == 0)
        return -1;
    size_t n = strlen(key);
    for (char *line = procbuf; line != NULL && *line != '\0'; ) {
        if (strncmp(line, key, n) == 0 && line[n] == ':')
            return strtol(line + n + 1, NULL, 10);
        line = strchr(line, '\n');
        if (line != NULL)
            line++;
    }
    return -1;
}

static long vmlck(void) {
    return status_kb("VmLck");
}

// A maps/smaps region header: "start-end perms offset dev inode [path]".
static int parse_header(const char *line, unsigned long *start, unsigned long *end,
                        unsigned long *offset, const char **name) {
    char *e;
    if (!((*line >= '0' && *line <= '9') || (*line >= 'a' && *line <= 'f')))
        return 0;
    *start = strtoul(line, &e, 16);
    if (*e != '-')
        return 0;
    *end = strtoul(e + 1, &e, 16);
    if (*e != ' ')
        return 0;
    const char *p = e + 1;
    p = strchr(p, ' ');         // past the permissions
    if (p == NULL)
        return 0;
    *offset = strtoul(p + 1, &e, 16);
    // The path, if any, starts after the inode.
    p = e;
    for (int field = 0; field < 2 && p != NULL; field++) {
        while (*p == ' ')
            p++;
        p = strchr(p, ' ');
    }
    if (p != NULL)
        while (*p == ' ')
            p++;
    *name = p != NULL && *p != '\n' && *p != '\0' ? p : "";
    return 1;
}

struct region {
    int found;
    unsigned long start, end, offset;
    long rss, pss, locked, size;
    int lo;
    char name[48];
};

static void region_field(struct region *r, const char *line) {
    if (strncmp(line, "Rss:", 4) == 0)
        r->rss = strtol(line + 4, NULL, 10);
    else if (strncmp(line, "Pss:", 4) == 0)
        r->pss = strtol(line + 4, NULL, 10);
    else if (strncmp(line, "Locked:", 7) == 0)
        r->locked = strtol(line + 7, NULL, 10);
    else if (strncmp(line, "Size:", 5) == 0)
        r->size = strtol(line + 5, NULL, 10);
    else if (strncmp(line, "VmFlags:", 8) == 0) {
        const char *nl = strchr(line, '\n');
        size_t len = nl != NULL ? (size_t) (nl - line) : strlen(line);
        for (size_t i = 8; i + 2 <= len; i++)
            if (line[i - 1] == ' ' && line[i] == 'l' && line[i + 1] == 'o' &&
                    (i + 2 == len || line[i + 2] == ' '))
                r->lo = 1;
    }
}

static void region_name(struct region *r, const char *name) {
    size_t i = 0;
    while (name[i] != '\0' && name[i] != '\n' && i < sizeof r->name - 1) {
        r->name[i] = name[i];
        i++;
    }
    r->name[i] = '\0';
}

// The smaps region holding `addr`.
static struct region smaps_region(const void *addr) {
    struct region r = {0};
    unsigned long a = (unsigned long) (uintptr_t) addr;
    read_proc("/proc/self/smaps");
    int in = 0;
    for (char *line = procbuf; line != NULL && *line != '\0'; ) {
        unsigned long s, e, off;
        const char *name;
        if (parse_header(line, &s, &e, &off, &name)) {
            in = s <= a && a < e;
            if (in) {
                r.found = 1;
                r.start = s;
                r.end = e;
                r.offset = off;
                region_name(&r, name);
            }
        } else if (in) {
            region_field(&r, line);
        }
        line = strchr(line, '\n');
        if (line != NULL)
            line++;
    }
    return r;
}

// The maps lines overlapping [addr, addr + len): how many, and the file
// offsets of the first `max`.
static int maps_lines(const void *addr, size_t len, unsigned long *offsets, int max) {
    unsigned long lo = (unsigned long) (uintptr_t) addr, hi = lo + len;
    read_proc("/proc/self/maps");
    int n = 0;
    for (char *line = procbuf; line != NULL && *line != '\0'; ) {
        unsigned long s, e, off;
        const char *name;
        if (parse_header(line, &s, &e, &off, &name) && s < hi && lo < e) {
            if (n < max && offsets != NULL)
                offsets[n] = off;
            n++;
        }
        line = strchr(line, '\n');
        if (line != NULL)
            line++;
    }
    return n;
}

// Linux's special mappings, which mlock and mlockall pass over: named in
// brackets, bar the two ordinary ones.
static int special_name(const char *name) {
    return name[0] == '[' && strcmp(name, "[stack]") != 0 && strcmp(name, "[heap]") != 0;
}

// ---- helpers ---------------------------------------------------------------

static int privileged(void) {
    return geteuid() == 0;
}

static int tmp_file(int pages) {
    char path[] = "/tmp/mlock_accounting.XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0)
        return -1;
    unlink(path);
    char buf[PG];
    for (int i = 0; i < pages; i++) {
        memset(buf, 0x10 + i, sizeof buf);
        if (write(fd, buf, PG) != (ssize_t) PG) {
            close(fd);
            return -1;
        }
    }
    return fd;
}

// `pages` of address space between two PROT_NONE guard pages, so nothing the
// test does not control merges with the pages in /proc -- neighbouring
// mappings with equal flags are one region, on Linux and AOK both. Returns
// the first page inside.
static char *guarded(size_t pages, int prot) {
    char *g = mmap(NULL, (pages + 2) * PG, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (g == MAP_FAILED)
        return NULL;
    char *p = mmap(g + PG, pages * PG, prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

static void unguard(char *p, size_t pages) {
    munmap(p - PG, (pages + 2) * PG);
}

// Run `fn` in a child and fold its failures in: for anything that locks for
// the whole process (mlockall), changes its limits or credentials, or means to
// die.
static void in_child(const char *what, void (*fn)(void)) {
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        failures_total = 0;
        fn();
        fflush(NULL);
        _exit(failures_total > 100 ? 100 : (int) failures_total);
    }
    int status;
    char label[160];
    if (pid < 0 || waitpid(pid, &status, 0) != pid) {
        snprintf(label, sizeof label, "%s: the child ran", what);
        check(label, 0);
        return;
    }
    if (WIFSIGNALED(status)) {
        snprintf(label, sizeof label, "%s: the child died of signal %d", what, WTERMSIG(status));
        check(label, 0);
        return;
    }
    failures_total += (unsigned) WEXITSTATUS(status);
}

// Recursion that touches a page a frame, for stack growth. The frame's page is
// read after the call returns, so no compiler can fold the frames away.
__attribute__((noinline)) static int deep(int depth) {
    volatile char pad[PG];
    pad[0] = (char) depth;
    pad[PG - 1] = 1;
    if (depth > 0)
        return deep(depth - 1) + pad[0];
    return pad[0];
}

// ---- the lock and its reports ----------------------------------------------

static void file_mapping_and_forced_write(int fd) {
    char label[200];
    long l0 = vmlck();
    char *m = mmap(NULL, 4 * PG, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 0);
    check("file: mmap", m != MAP_FAILED);
    if (m == MAP_FAILED)
        return;
    check("file: mlock 4 pages of r-x private file mapping", mlock(m, 4 * PG) == 0);
    long l1 = vmlck();
    snprintf(label, sizeof label, "file: VmLck rises by 16 kB (%ld -> %ld)", l0, l1);
    check(label, l1 - l0 == 16);
    struct region r = smaps_region(m);
    snprintf(label, sizeof label, "file: the region is \"lo\", Locked 16 kB (lo %d Locked %ld Rss %ld)",
             r.lo, r.locked, r.rss);
    check(label, r.found && r.lo && r.locked == 16 && r.rss == 16);

    int mfd = open("/proc/self/mem", O_RDWR);
    char v = 0x77;
    ssize_t w = mfd < 0 ? -1 : pwrite(mfd, &v, 1, (off_t) (uintptr_t) (m + PG));
    if (mfd >= 0)
        close(mfd);
    check("file: a forced write into the second page takes", w == 1 && m[PG] == 0x77);
    long l2 = vmlck();
    snprintf(label, sizeof label, "file: VmLck unchanged by the forced write (%ld -> %ld)", l1, l2);
    check(label, l2 == l1);
    r = smaps_region(m);
    snprintf(label, sizeof label, "file: still one \"lo\" region of 16 kB, Locked 16 kB "
             "(lo %d Locked %ld %#lx-%#lx)", r.lo, r.locked, r.start, r.end);
    check(label, r.lo && r.locked == 16 && r.end - r.start == 4 * PG &&
          r.start == (unsigned long) (uintptr_t) m);
    unsigned long offs[4];
    check("file: one maps line after the forced write", maps_lines(m, 4 * PG, offs, 4) == 1);

    check("file: munlock", munlock(m, 4 * PG) == 0);
    long l3 = vmlck();
    snprintf(label, sizeof label, "file: munlock brings VmLck back (%ld)", l3);
    check(label, l3 == l0);
    r = smaps_region(m);
    check("file: munlocked region is not \"lo\" and Locked 0", !r.lo && r.locked == 0);
    munmap(m, 4 * PG);
}

static void maps_split_at_lock(int fd) {
    char label[200];
    unsigned long offs[4] = {0};
    char *m = mmap(NULL, 3 * PG, PROT_READ, MAP_PRIVATE, fd, 0);
    check("split: mmap 3 file pages", m != MAP_FAILED);
    if (m != MAP_FAILED) {
        long l0 = vmlck();
        check("split: mlock the middle file page", mlock(m + PG, PG) == 0);
        int n = maps_lines(m, 3 * PG, offs, 4);
        snprintf(label, sizeof label, "split: 3 maps lines at offsets 0 0x1000 0x2000 "
                 "(%d lines, %#lx %#lx %#lx)", n, offs[0], offs[1], offs[2]);
        check(label, n == 3 && offs[0] == 0 && offs[1] == PG && offs[2] == 2 * PG);
        check("split: only the middle is \"lo\"",
              !smaps_region(m).lo && smaps_region(m + PG).lo && !smaps_region(m + 2 * PG).lo);
        check("split: VmLck +4 kB", vmlck() - l0 == 4);
        check("split: munlock", munlock(m + PG, PG) == 0);
        check("split: joined back into one maps line", maps_lines(m, 3 * PG, NULL, 0) == 1);
        munmap(m, 3 * PG);
    }

    char *a = guarded(3, PROT_READ | PROT_WRITE);
    check("split: 3 anonymous pages", a != NULL);
    if (a == NULL)
        return;
    check("split: mlock the middle anonymous page", mlock(a + PG, PG) == 0);
    int n = maps_lines(a, 3 * PG, NULL, 0);
    snprintf(label, sizeof label, "split: anonymous, 3 maps lines (%d)", n);
    check(label, n == 3);
    struct region r = smaps_region(a + PG);
    snprintf(label, sizeof label, "split: the locked page is its own region, Locked 4 kB "
             "(%#lx-%#lx Locked %ld)", r.start, r.end, r.locked);
    check(label, r.lo && r.end - r.start == PG && r.locked == 4);
    // Not a line count: Linux merges these back or not by its VMA rules (a
    // plain mmap does; this one, mapped into its guards, stays in three).
    check("split: anonymous munlock", munlock(a + PG, PG) == 0);
    check("split: anonymous, nothing \"lo\" after it",
          !smaps_region(a).lo && !smaps_region(a + PG).lo && !smaps_region(a + 2 * PG).lo);
    unguard(a, 3);
}

static void mlock_ranges(void) {
    char label[200];
    long l0 = vmlck();

    // An unaligned start is rounded down, the length grown to cover it.
    char *a = guarded(3, PROT_READ | PROT_WRITE);
    check("ranges: 3 pages", a != NULL);
    if (a == NULL)
        return;
    int r = mlock(a + 100, PG);
    snprintf(label, sizeof label, "ranges: mlock(p + 100, 4096) locks two pages (r %d VmLck %+ld)",
             r, vmlck() - l0);
    check(label, r == 0 && vmlck() - l0 == 8);
    r = munlock(a + 100, 10);
    snprintf(label, sizeof label, "ranges: munlock(p + 100, 10) unlocks the first (r %d VmLck %+ld)",
             r, vmlck() - l0);
    check(label, r == 0 && vmlck() - l0 == 4);
    check("ranges: mlock of 0 bytes", mlock(a, 0) == 0 && vmlck() - l0 == 4);
    r = mlock(a + PG, 2 * PG);
    check("ranges: re-locking a locked page does not count it twice",
          r == 0 && vmlck() - l0 == 8);
    check("ranges: munlock all 3", munlock(a, 3 * PG) == 0 && vmlck() == l0);

    // A hole: the pages before it are locked, and it is ENOMEM.
    check("ranges: punch a hole in the middle", munmap(a + PG, PG) == 0);
    errno = 0;
    r = mlock(a, 3 * PG);
    int err = errno;
    snprintf(label, sizeof label, "ranges: mlock over a hole is ENOMEM and locks the page before it "
             "(r %d errno %d VmLck %+ld)", r, err, vmlck() - l0);
    check(label, r == -1 && err == ENOMEM && vmlck() - l0 == 4);
    check("ranges: that page is \"lo\", the one after the hole not",
          smaps_region(a).lo && !smaps_region(a + 2 * PG).lo);
    errno = 0;
    r = munlock(a, 3 * PG);
    err = errno;
    snprintf(label, sizeof label, "ranges: munlock over the hole is ENOMEM and unlocks it "
             "(r %d errno %d VmLck %+ld)", r, err, vmlck() - l0);
    check(label, r == -1 && err == ENOMEM && vmlck() == l0);
    errno = 0;
    r = mlock(a + PG, 2 * PG);
    err = errno;
    snprintf(label, sizeof label, "ranges: mlock starting in a hole is ENOMEM and locks nothing "
             "(r %d errno %d VmLck %+ld)", r, err, vmlck() - l0);
    check(label, r == -1 && err == ENOMEM && vmlck() == l0);
    unguard(a, 3);

    if (sizeof(void *) == 8) {
        errno = 0;
        r = mlock((void *) (uintptr_t) -PG, 2 * PG);
        err = errno;
        snprintf(label, sizeof label, "ranges: a range that wraps is EINVAL (r %d errno %d)", r, err);
        check(label, r == -1 && err == EINVAL);
        // The LENGTH wraps in Linux's page rounding, to nothing at all.
        char *p = guarded(1, PROT_READ | PROT_WRITE);
        if (p != NULL) {
            errno = 0;
            r = mlock(p, SIZE_MAX);
            err = errno;
            snprintf(label, sizeof label, "ranges: mlock(p, SIZE_MAX) rounds to nothing and is 0 "
                     "(r %d errno %d VmLck %+ld)", r, err, vmlck() - l0);
            check(label, r == 0 && vmlck() == l0);
            unguard(p, 1);
        }
    }

    // [rw, none, rw]: all locked, ENOMEM, and only the first populated.
    char *g = guarded(3, PROT_NONE);
    check("ranges: 3 guarded PROT_NONE pages", g != NULL);
    if (g != NULL) {
        check("ranges: make the first and third read-write",
              mprotect(g, PG, PROT_READ | PROT_WRITE) == 0 &&
              mprotect(g + 2 * PG, PG, PROT_READ | PROT_WRITE) == 0);
        errno = 0;
        r = mlock(g, 3 * PG);
        err = errno;
        struct region first = smaps_region(g), third = smaps_region(g + 2 * PG);
        snprintf(label, sizeof label, "ranges: mlock of [rw, none, rw] is ENOMEM, locks all three, "
                 "populates the first only (r %d errno %d VmLck %+ld Rss %ld/%ld)",
                 r, err, vmlck() - l0, first.rss, third.rss);
        check(label, r == -1 && err == ENOMEM && vmlck() - l0 == 12 &&
              first.rss == 4 && third.rss == 0 && first.lo && third.lo &&
              smaps_region(g + PG).lo);
        check("ranges: munlock them", munlock(g, 3 * PG) == 0 && vmlck() == l0);
        unguard(g, 3);
    }

    // The special mappings are passed over, and it is not an error.
    read_proc("/proc/self/maps");
    unsigned long specials[4][2];
    int nspecial = 0;
    for (char *line = procbuf; line != NULL && *line != '\0' && nspecial < 4; ) {
        unsigned long s, e, off;
        const char *name;
        if (parse_header(line, &s, &e, &off, &name) &&
                (strncmp(name, "[vdso]", 6) == 0 || strncmp(name, "[vvar]", 6) == 0)) {
            specials[nspecial][0] = s;
            specials[nspecial][1] = e;
            nspecial++;
        }
        line = strchr(line, '\n');
        if (line != NULL)
            line++;
    }
    for (int i = 0; i < nspecial; i++) {
        void *s = (void *) (uintptr_t) specials[i][0];
        errno = 0;
        r = mlock(s, specials[i][1] - specials[i][0]);
        err = errno;
        struct region sr = smaps_region(s);
        snprintf(label, sizeof label, "ranges: mlock of %s is 0 and locks nothing "
                 "(r %d errno %d VmLck %+ld lo %d)", sr.name, r, err, vmlck() - l0, sr.lo);
        check(label, r == 0 && vmlck() == l0 && !sr.lo);
    }
    test_logf("ranges: %d special mappings checked\n", nspecial);

    // munmap and a MAP_FIXED over locked pages take them out of VmLck.
    a = guarded(4, PROT_READ | PROT_WRITE);
    if (a != NULL) {
        check("ranges: mlock 4 more", mlock(a, 4 * PG) == 0 && vmlck() - l0 == 16);
        char *f = mmap(a + PG, 2 * PG, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        snprintf(label, sizeof label, "ranges: a MAP_FIXED over two of them takes 8 kB off VmLck "
                 "(VmLck %+ld)", vmlck() - l0);
        check(label, f == a + PG && vmlck() - l0 == 8);
        check("ranges: munmap the last takes it off too", munmap(a + 3 * PG, PG) == 0 &&
              vmlck() - l0 == 4);
        unguard(a, 4);
        check("ranges: and back to where it was", vmlck() == l0);
    }
}

static void madvise_refuses_locked(void) {
    char label[200];
    char *a = guarded(2, PROT_READ | PROT_WRITE);
    check("madvise: 2 pages", a != NULL);
    if (a == NULL)
        return;
    a[0] = 5;
    a[PG] = 6;
    check("madvise: mlock them", mlock(a, 2 * PG) == 0);
    static const struct { int advice; const char *name; } refused[] = {
        {MADV_DONTNEED, "MADV_DONTNEED"}, {MADV_FREE, "MADV_FREE"},
        {MADV_COLD, "MADV_COLD"}, {MADV_PAGEOUT, "MADV_PAGEOUT"},
    };
    for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++) {
        errno = 0;
        int r = madvise(a, 2 * PG, refused[i].advice);
        int err = errno;
        snprintf(label, sizeof label, "madvise: %s on a locked page is EINVAL, and the page "
                 "keeps its bytes (r %d errno %d bytes %d %d)", refused[i].name, r, err, a[0], a[PG]);
        check(label, r == -1 && err == EINVAL && a[0] == 5 && a[PG] == 6);
    }
    check("madvise: MADV_WILLNEED is still fine", madvise(a, 2 * PG, MADV_WILLNEED) == 0);
    // An unlocked page ahead of a locked one: it is discarded, then the locked
    // one refused, with its bytes.
    munlock(a, PG);
    errno = 0;
    int r = madvise(a, 2 * PG, MADV_DONTNEED);
    int err = errno;
    snprintf(label, sizeof label, "madvise: MADV_DONTNEED over [unlocked, locked] discards the "
             "first and refuses the second (r %d errno %d bytes %d %d)", r, err, a[0], a[PG]);
    check(label, r == -1 && err == EINVAL && a[0] == 0 && a[PG] == 6);
    munlock(a, 2 * PG);
    check("madvise: unlocked, MADV_DONTNEED discards",
          madvise(a, 2 * PG, MADV_DONTNEED) == 0 && a[PG] == 0);
    unguard(a, 2);

    char *s = mmap(NULL, PG, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    check("madvise: a shared page", s != MAP_FAILED);
    if (s == MAP_FAILED)
        return;
    s[0] = 9;
    mlock(s, PG);
    errno = 0;
    r = madvise(s, PG, MADV_REMOVE);
    err = errno;
    snprintf(label, sizeof label, "madvise: MADV_REMOVE on a locked shared page is EINVAL "
             "(r %d errno %d byte %d)", r, err, s[0]);
    check(label, r == -1 && err == EINVAL && s[0] == 9);
    munlock(s, PG);
    check("madvise: unlocked, MADV_REMOVE punches it", madvise(s, PG, MADV_REMOVE) == 0 && s[0] == 0);
    munmap(s, PG);
}

static void fork_and_cow(void) {
    char label[200];
    char *a = guarded(4, PROT_READ | PROT_WRITE);
    check("fork: 4 pages", a != NULL);
    if (a == NULL)
        return;
    memset(a, 1, 4 * PG);
    long l0 = vmlck();
    check("fork: mlock them", mlock(a, 4 * PG) == 0 && vmlck() - l0 == 16);
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        check("fork: pipe", 0);
        return;
    }
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        // The child holds no locks: VmLck 0, the copy not "lo".
        long child = vmlck();
        struct region r = smaps_region(a);
        a[PG] = 3;      // a copy of its own
        long after = vmlck();
        char result[3] = {(char) (child == 0), (char) (!r.lo && r.locked == 0), (char) (after == 0)};
        if (write(pipefd[1], result, sizeof result) != (ssize_t) sizeof result)
            _exit(2);
        _exit(0);
    }
    close(pipefd[1]);
    char result[3] = {0};
    ssize_t got = read(pipefd[0], result, sizeof result);
    close(pipefd[0]);
    waitpid(pid, NULL, 0);
    check("fork: the child's VmLck is 0", got == 3 && result[0]);
    check("fork: the child's copy of the locked mapping is not \"lo\"", got == 3 && result[1]);
    check("fork: the child's own copy-on-write leaves it 0", got == 3 && result[2]);
    a[0] = 2;
    a[PG] = 2;
    long l1 = vmlck();
    struct region r = smaps_region(a);
    snprintf(label, sizeof label, "fork: the parent's pages stay locked when written after the fork "
             "(VmLck %+ld lo %d Locked %ld)", l1 - l0, r.lo, r.locked);
    check(label, l1 - l0 == 16 && r.lo && r.locked == 16);
    check("fork: still one maps line", maps_lines(a, 4 * PG, NULL, 0) == 1);
    munlock(a, 4 * PG);
    unguard(a, 4);
}

static void mremap_carries_lock(void) {
    char label[200];
    long l0 = vmlck();
    // Room above for an in-place growth: 2 pages at the bottom of 8 free ones.
    char *room = mmap(NULL, 8 * PG, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check("mremap: reserve room", room != MAP_FAILED);
    if (room == MAP_FAILED)
        return;
    munmap(room, 8 * PG);
    char *m = mmap(room, 2 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    check("mremap: 2 pages", m == room);
    check("mremap: mlock them", mlock(m, 2 * PG) == 0 && vmlck() - l0 == 8);
    char *g = mremap(m, 2 * PG, 4 * PG, 0);
    struct region r = g == MAP_FAILED ? (struct region) {0} : smaps_region(g);
    snprintf(label, sizeof label, "mremap: grown in place 2 -> 4, all locked and resident "
             "(VmLck %+ld lo %d Locked %ld Rss %ld size %ld)", vmlck() - l0, r.lo, r.locked, r.rss,
             r.size);
    check(label, g == m && vmlck() - l0 == 16 && r.lo && r.locked == 16 && r.rss == 16 &&
          r.end - r.start == 4 * PG);
    if (g == MAP_FAILED)
        return;
    char *blocker = mmap(g + 4 * PG, PG, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    char *moved = mremap(g, 4 * PG, 6 * PG, MREMAP_MAYMOVE);
    r = moved == MAP_FAILED ? (struct region) {0} : smaps_region(moved);
    snprintf(label, sizeof label, "mremap: moved and grown 4 -> 6, all locked and resident "
             "(VmLck %+ld lo %d Locked %ld Rss %ld)", vmlck() - l0, r.lo, r.locked, r.rss);
    check(label, moved != MAP_FAILED && moved != g && vmlck() - l0 == 24 && r.lo &&
          r.locked == 24 && r.rss == 24 && r.end - r.start == 6 * PG);
    if (blocker != MAP_FAILED)
        munmap(blocker, PG);
    if (moved == MAP_FAILED)
        return;
    char *dst = mmap(NULL, 6 * PG, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    char *fixed = mremap(moved, 6 * PG, 6 * PG, MREMAP_MAYMOVE | MREMAP_FIXED, dst);
    r = fixed == MAP_FAILED ? (struct region) {0} : smaps_region(fixed);
    snprintf(label, sizeof label, "mremap: MREMAP_FIXED keeps the lock (VmLck %+ld lo %d Locked %ld)",
             vmlck() - l0, r.lo, r.locked);
    check(label, fixed == dst && vmlck() - l0 == 24 && r.lo && r.locked == 24);
    if (fixed == MAP_FAILED)
        return;
    char *shrunk = mremap(fixed, 6 * PG, 3 * PG, 0);
    snprintf(label, sizeof label, "mremap: a shrink takes its pages off VmLck (VmLck %+ld)",
             vmlck() - l0);
    check(label, shrunk == fixed && vmlck() - l0 == 12 && smaps_region(shrunk).lo);
    munmap(fixed, 3 * PG);
    check("mremap: munmap, back to where it was", vmlck() == l0);

    // Across a lock boundary it is two of Linux's VMAs: EFAULT.
    char *a = guarded(3, PROT_READ | PROT_WRITE);
    if (a != NULL) {
        mlock(a + PG, PG);
        errno = 0;
        char *x = mremap(a, 3 * PG, 5 * PG, MREMAP_MAYMOVE);
        int err = errno;
        snprintf(label, sizeof label, "mremap: across a lock boundary is EFAULT (%s errno %d)",
                 x == MAP_FAILED ? "failed" : "moved", err);
        check(label, x == MAP_FAILED && err == EFAULT);
        if (x != MAP_FAILED)
            munmap(x, 5 * PG);
        else
            unguard(a, 3);
        munlockall();
    }

    // An alias of a locked shared mapping is locked, and counted again.
    char *s = mmap(NULL, 2 * PG, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    check("mremap: 2 shared pages", s != MAP_FAILED);
    if (s == MAP_FAILED)
        return;
    long l1 = vmlck();
    check("mremap: mlock them", mlock(s, 2 * PG) == 0 && vmlck() - l1 == 8);
    char *alias = mremap(s, 0, 2 * PG, MREMAP_MAYMOVE);
    r = alias == MAP_FAILED ? (struct region) {0} : smaps_region(alias);
    snprintf(label, sizeof label, "mremap: an alias of them is locked (VmLck %+ld lo %d)",
             vmlck() - l1, r.lo);
    check(label, alias != MAP_FAILED && vmlck() - l1 == 16 && r.lo);
    if (alias != MAP_FAILED) {
        s[0] = 0x44;
        check("mremap: and is the same memory", alias[0] == 0x44);
        munmap(alias, 2 * PG);
    }
    munmap(s, 2 * PG);
    check("mremap: all gone again", vmlck() == l0);
}

static void mprotect_populates_locked(void) {
    char label[200];
    char *p = guarded(2, PROT_NONE);
    check("mprotect: 2 PROT_NONE pages", p != NULL);
    if (p == NULL)
        return;
    long l0 = vmlck();
    errno = 0;
    int r = mlock(p, 2 * PG);
    int err = errno;
    struct region reg = smaps_region(p);
    snprintf(label, sizeof label, "mprotect: mlock of PROT_NONE is ENOMEM, but locks it "
             "(r %d errno %d VmLck %+ld lo %d Locked %ld)", r, err, vmlck() - l0, reg.lo, reg.locked);
    check(label, r == -1 && err == ENOMEM && vmlck() - l0 == 8 && reg.lo && reg.locked == 0);
    check("mprotect: made readable", mprotect(p, 2 * PG, PROT_READ) == 0);
    reg = smaps_region(p);
    snprintf(label, sizeof label, "mprotect: readable, still locked and not populated "
             "(lo %d Rss %ld Locked %ld)", reg.lo, reg.rss, reg.locked);
    check(label, reg.lo && reg.rss == 0 && reg.locked == 0 && vmlck() - l0 == 8);
    check("mprotect: made writable", mprotect(p, 2 * PG, PROT_READ | PROT_WRITE) == 0);
    reg = smaps_region(p);
    snprintf(label, sizeof label, "mprotect: writable, populated (lo %d Rss %ld Locked %ld)",
             reg.lo, reg.rss, reg.locked);
    check(label, reg.lo && reg.rss == 8 && reg.locked == 8 && vmlck() - l0 == 8);
    p[0] = 1;
    check("mprotect: and usable", p[0] == 1);
    munlock(p, 2 * PG);
    unguard(p, 2);
}

static void map_locked_flag(int fd) {
    char label[200];
    long l0 = vmlck(), r0 = status_kb("VmRSS");
    char *m = mmap(NULL, 4 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_LOCKED, -1, 0);
    struct region r = m == MAP_FAILED ? (struct region) {0} : smaps_region(m);
    snprintf(label, sizeof label, "MAP_LOCKED: locked and populated (VmLck %+ld VmRSS %+ld lo %d)",
             vmlck() - l0, status_kb("VmRSS") - r0, r.lo);
    check(label, m != MAP_FAILED && vmlck() - l0 == 16 && status_kb("VmRSS") - r0 >= 16 && r.lo);
    if (m != MAP_FAILED)
        munmap(m, 4 * PG);
    char *f = mmap(NULL, 2 * PG, PROT_READ, MAP_PRIVATE | MAP_LOCKED, fd, 0);
    check("MAP_LOCKED: a file mapping too", f != MAP_FAILED && vmlck() - l0 == 8 &&
          smaps_region(f).lo);
    if (f != MAP_FAILED)
        munmap(f, 2 * PG);
    check("MAP_LOCKED: munmapped, back", vmlck() == l0);
}

// ---- mlockall ----------------------------------------------------------------

// VmLck is the sum of the "lo" regions, and every region but a special one is
// "lo" after mlockall(MCL_CURRENT).
static void check_all_locked(const char *when) {
    char label[240];
    long lck = vmlck();
    read_proc("/proc/self/smaps");
    long sum = 0;
    int unlocked = 0, special_locked = 0;
    char first_unlocked[64] = "";
    struct region r = {0};
    int have = 0;
    int droppable = 0;
    for (char *line = procbuf; ; ) {
        unsigned long s, e, off;
        const char *name;
        int header = line != NULL && *line != '\0' && parse_header(line, &s, &e, &off, &name);
        if ((header || line == NULL || *line == '\0') && have) {
            if (r.lo)
                sum += (long) ((r.end - r.start) / 1024);
            if (special_name(r.name))
                special_locked += r.lo;
            else if (!r.lo && !droppable) {
                if (unlocked++ == 0)
                    snprintf(first_unlocked, sizeof first_unlocked, "%#lx %s", r.start, r.name);
            }
            have = 0;
        }
        if (line == NULL || *line == '\0')
            break;
        if (header) {
            r = (struct region) {.found = 1, .start = s, .end = e};
            region_name(&r, name);
            droppable = 0;
            have = 1;
        } else if (have) {
            region_field(&r, line);
            // Linux's VM_DROPPABLE (glibc's getrandom state) is never locked.
            if (strncmp(line, "VmFlags:", 8) == 0 && strstr(line, " dp") != NULL)
                droppable = 1;
        }
        line = strchr(line, '\n');
        if (line != NULL)
            line++;
    }
    snprintf(label, sizeof label, "%s: every region but the special ones is \"lo\" "
             "(%d not, first %s)", when, unlocked, first_unlocked);
    check(label, unlocked == 0);
    snprintf(label, sizeof label, "%s: no special mapping is \"lo\" (%d are)", when, special_locked);
    check(label, special_locked == 0);
    snprintf(label, sizeof label, "%s: VmLck is the sum of the \"lo\" regions (%ld kB, sum %ld kB)",
             when, lck, sum);
    check(label, lck == sum);
}

static void mlockall_current_child(void) {
    char label[200];
    struct rlimit rl;
    getrlimit(RLIMIT_MEMLOCK, &rl);
    long size = status_kb("VmSize");
    errno = 0;
    int r = mlockall(MCL_CURRENT);
    int err = errno;
    if (!privileged() && rl.rlim_cur != RLIM_INFINITY && (unsigned long) size * 1024 > rl.rlim_cur) {
        snprintf(label, sizeof label, "mlockall: VmSize %ld kB over RLIMIT_MEMLOCK is ENOMEM "
                 "(r %d errno %d)", size, r, err);
        check(label, r == -1 && err == ENOMEM);
        return;
    }
    snprintf(label, sizeof label, "mlockall(MCL_CURRENT) (r %d errno %d)", r, err);
    check(label, r == 0);
    if (r != 0)
        return;
    check_all_locked("mlockall");
    struct region st0 = {0};
    read_proc("/proc/self/maps");
    long l0 = vmlck(), v0 = status_kb("VmSize");
    int d = deep(200);
    long l1 = vmlck(), v1 = status_kb("VmSize");
    int lines = 0;
    read_proc("/proc/self/maps");
    for (char *line = procbuf; line != NULL && *line != '\0'; ) {
        unsigned long s, e, off;
        const char *name;
        if (parse_header(line, &s, &e, &off, &name) && strncmp(name, "[stack]", 7) == 0) {
            lines++;
            st0.start = s;
        }
        line = strchr(line, '\n');
        if (line != NULL)
            line++;
    }
    struct region st = smaps_region((void *) (uintptr_t) st0.start);
    snprintf(label, sizeof label, "mlockall: the stack grows locked (deep %d, VmLck %+ld, VmSize %+ld, "
             "%d [stack] lines, lo %d Locked %ld Rss %ld)", d, l1 - l0, v1 - v0, lines, st.lo,
             st.locked, st.rss);
    // Locked is the region's Pss, and Pss is not all of Rss here on AOK: this
    // child shares the stack pages it has not written with its parent, which
    // is waiting for it. Linux's populate write-faults a locked private
    // writable page, which copies it; AOK's leaves it shared.
    check(label, l1 - l0 >= 400 && l1 - l0 == v1 - v0 && lines == 1 && st.lo &&
          st.locked > 0 && st.locked <= st.rss);
    check_all_locked("mlockall, after stack growth");
    check("munlockall", munlockall() == 0);
    snprintf(label, sizeof label, "munlockall: VmLck 0 (%ld)", vmlck());
    check(label, vmlck() == 0);
}

static void mlockall_future_child(void) {
    char label[200];
    int fd = tmp_file(4);
    char *old = guarded(2, PROT_READ | PROT_WRITE);  // made before MCL_FUTURE
    int shmid = shmget(IPC_PRIVATE, 4 * PG, IPC_CREAT | 0600);
    long l0 = vmlck();
    check("future: mlockall(MCL_FUTURE)", mlockall(MCL_FUTURE) == 0);
    check("future: locks nothing yet", vmlck() == l0);

    char *m = mmap(NULL, 4 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    struct region r = m == MAP_FAILED ? (struct region) {0} : smaps_region(m);
    snprintf(label, sizeof label, "future: a new anonymous mapping is locked and populated "
             "(VmLck %+ld lo %d Rss %ld Locked %ld)", vmlck() - l0, r.lo, r.rss, r.locked);
    check(label, m != MAP_FAILED && vmlck() - l0 == 16 && r.lo &&
          r.rss == (long) ((r.end - r.start) / 1024) && r.locked == r.rss);
    long l1 = vmlck();
    char *f = fd < 0 ? MAP_FAILED : mmap(NULL, 2 * PG, PROT_READ, MAP_PRIVATE, fd, 0);
    snprintf(label, sizeof label, "future: a file mapping (VmLck %+ld)", vmlck() - l1);
    check(label, f != MAP_FAILED && vmlck() - l1 == 8 && smaps_region(f).lo);
    long l2 = vmlck();
    char *n = mmap(NULL, 2 * PG, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    r = n == MAP_FAILED ? (struct region) {0} : smaps_region(n);
    snprintf(label, sizeof label, "future: a PROT_NONE mapping is locked, not resident "
             "(VmLck %+ld lo %d Locked %ld)", vmlck() - l2, r.lo, r.locked);
    check(label, n != MAP_FAILED && vmlck() - l2 == 8 && r.lo && r.locked == 0);

    long l3 = vmlck();
    long b0 = syscall(SYS_brk, 0);
    long want = ((b0 + (long) PG - 1) & ~(long) (PG - 1)) + 8 * (long) PG;
    long b1 = syscall(SYS_brk, want);
    long l4 = vmlck();
    syscall(SYS_brk, b0);
    long l5 = vmlck();
    snprintf(label, sizeof label, "future: brk grows locked and gives it back "
             "(brk %#lx -> %#lx, VmLck %+ld then %+ld)", b0, b1, l4 - l3, l5 - l3);
    check(label, b1 == want && l4 - l3 == 32 && l5 == l3);

    char *shm = shmid < 0 ? (void *) -1 : shmat(shmid, NULL, 0);
    long l6 = vmlck();
    r = shm == (void *) -1 ? (struct region) {0} : smaps_region(shm);
    snprintf(label, sizeof label, "future: shmat is locked and populated (VmLck %+ld lo %d Rss %ld)",
             l6 - l5, r.lo, r.rss);
    check(label, shm != (void *) -1 && l6 - l5 == 16 && r.lo && r.rss == 16);
    if (shm != (void *) -1)
        shmdt(shm);
    if (shmid >= 0)
        shmctl(shmid, IPC_RMID, NULL);
    check("future: shmdt takes it back", vmlck() == l5);

    if (old != NULL) {
        long l7 = vmlck();
        char *grown = mremap(old, 2 * PG, 4 * PG, MREMAP_MAYMOVE);
        r = grown == MAP_FAILED ? (struct region) {0} : smaps_region(grown);
        snprintf(label, sizeof label, "future: a mapping from before it grown by mremap stays "
                 "unlocked (VmLck %+ld lo %d)", vmlck() - l7, r.lo);
        check(label, grown != MAP_FAILED && vmlck() == l7 && !r.lo);
    }

    // A fork's child does not inherit it.
    int pipefd[2];
    if (pipe(pipefd) == 0) {
        fflush(NULL);
        pid_t pid = fork();
        if (pid == 0) {
            long c0 = vmlck();
            char *cm = mmap(NULL, 4 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            char ok = c0 == 0 && cm != MAP_FAILED && vmlck() == 0;
            if (write(pipefd[1], &ok, 1) != 1)
                _exit(2);
            _exit(0);
        }
        close(pipefd[1]);
        char ok = 0;
        ssize_t got = read(pipefd[0], &ok, 1);
        close(pipefd[0]);
        waitpid(pid, NULL, 0);
        check("future: a fork's child holds no locks and maps unlocked", got == 1 && ok);
        long l8 = vmlck();
        char *pm = mmap(NULL, PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        check("future: while the parent still maps locked", pm != MAP_FAILED && vmlck() - l8 == 4);
    }

    // MCL_ONFAULT: locked, not populated -- a large one, which AOK keeps
    // reserved, included.
    check("future: munlockall", munlockall() == 0 && vmlck() == 0);
    check("future: mlockall(MCL_FUTURE|MCL_ONFAULT)", mlockall(MCL_FUTURE | MCL_ONFAULT) == 0);
    long r0 = status_kb("VmRSS");
    char *o = mmap(NULL, 4 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    r = o == MAP_FAILED ? (struct region) {0} : smaps_region(o);
    snprintf(label, sizeof label, "future|onfault: locked, not populated (VmLck %ld VmRSS %+ld lo %d)",
             vmlck(), status_kb("VmRSS") - r0, r.lo);
    check(label, o != MAP_FAILED && vmlck() == 16 && status_kb("VmRSS") - r0 < 16 && r.lo);
    struct rlimit rl;
    getrlimit(RLIMIT_MEMLOCK, &rl);
    int unlimited = privileged() || rl.rlim_cur == RLIM_INFINITY;
    long l9 = vmlck();
    errno = 0;
    char *big = mmap(NULL, 64 * MB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    int err = errno;
    if (unlimited) {
        long l10 = vmlck();
        snprintf(label, sizeof label, "future|onfault: 64M is locked whole, not populated "
                 "(VmLck %+ld VmRSS %+ld)", l10 - l9, status_kb("VmRSS") - r0);
        check(label, big != MAP_FAILED && l10 - l9 == 65536 && status_kb("VmRSS") - r0 < 4096);
        if (big != MAP_FAILED) {
            big[32 * MB] = 1;
            snprintf(label, sizeof label, "future|onfault: a touch inside it leaves VmLck alone "
                     "(VmLck %+ld)", vmlck() - l9);
            check(label, vmlck() - l9 == 65536 && smaps_region(big + 32 * MB).lo &&
                  smaps_region(big + 64 * MB - 1).lo);
            munmap(big, 64 * MB);
            check("future|onfault: munmap gives it back", vmlck() == l9);
        }
    } else {
        snprintf(label, sizeof label, "future|onfault: 64M over RLIMIT_MEMLOCK is EAGAIN (%s errno %d)",
                 big == MAP_FAILED ? "failed" : "mapped", err);
        check(label, big == MAP_FAILED && err == EAGAIN);
    }

    // A later mlockall(MCL_CURRENT) ends MCL_FUTURE, and so does munlockall.
    munlockall();
    mlockall(MCL_FUTURE);
    if (mlockall(MCL_CURRENT) == 0) {
        long l11 = vmlck();
        char *c = mmap(NULL, 4 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        check("future: mlockall(MCL_CURRENT) ends it", c != MAP_FAILED && vmlck() == l11);
    }
    munlockall();
    mlockall(MCL_FUTURE);
    munlockall();
    char *u = mmap(NULL, 4 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check("future: munlockall ends it", u != MAP_FAILED && vmlck() == 0);
}

// ---- the lock keeps pages out of AOK's swap -----------------------------------
//
// AOK only: with swap on (ISH_GUEST_SWAP_MB), /proc/ish/swap_evict pages a
// process out on demand. Pages locked through the paths this test is about --
// the tail an mremap grows onto a locked mapping, and an MCL_FUTURE mapping --
// stay resident while an unlocked control made beside them goes to swap.
// Anywhere else (Linux, or swap off) there is nothing to evict and nothing is
// checked.

static int swap_evict_self(void) {
    int fd = open("/proc/ish/swap_evict", O_WRONLY);
    if (fd < 0)
        return -1;
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%d\n", (int) getpid());
    int ok = write(fd, buf, (size_t) n) == n;
    close(fd);
    return ok ? 0 : -1;
}

// smaps' Swap, summed over the regions overlapping [p, p + len).
static long swap_kb(const char *p, size_t len) {
    unsigned long lo = (unsigned long) (uintptr_t) p, hi = lo + len;
    long sum = 0;
    int in = 0;
    for (char *line = procbuf; line != NULL && *line != '\0'; ) {
        unsigned long s, e, off;
        const char *name;
        if (parse_header(line, &s, &e, &off, &name))
            in = s < hi && lo < e;
        else if (in && strncmp(line, "Swap:", 5) == 0)
            sum += strtol(line + 5, NULL, 10);
        line = strchr(line, '\n');
        if (line != NULL)
            line++;
    }
    return sum;
}

static void swap_witness_child(void) {
    char label[200];
    if (access("/proc/ish/swap_evict", W_OK) != 0) {
        test_logf("swap: no /proc/ish/swap_evict to write here, not checked\n");
        return;
    }
    const size_t pages = 256;
    char *ctl = guarded(pages, PROT_READ | PROT_WRITE);
    char *grow = guarded(pages / 2, PROT_READ | PROT_WRITE);
    if (ctl == NULL || grow == NULL || mlock(grow, pages / 2 * PG) != 0) {
        check("swap: set up the control and a locked mapping", 0);
        return;
    }
    char *grown = mremap(grow, pages / 2 * PG, pages * PG, MREMAP_MAYMOVE);
    char *fut = MAP_FAILED;
    if (grown != MAP_FAILED && mlockall(MCL_FUTURE) == 0)
        fut = mmap(NULL, pages * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (grown == MAP_FAILED || fut == MAP_FAILED) {
        check("swap: grow the locked mapping and make an MCL_FUTURE one", 0);
        return;
    }
    for (size_t i = 0; i < pages; i++) {
        ctl[i * PG] = (char) (i + 1);
        grown[i * PG] = (char) (i + 2);
        fut[i * PG] = (char) (i + 3);
    }
    long ctl_swap = 0;
    for (int sweep = 0; sweep < 3 && ctl_swap == 0; sweep++) {
        if (swap_evict_self() != 0)
            break;
        read_proc("/proc/self/smaps");
        ctl_swap = swap_kb(ctl, pages * PG);
    }
    if (ctl_swap == 0) {
        test_logf("swap: nothing evicted (swap off?), not checked\n");
        return;
    }
    long grown_swap = swap_kb(grown, pages * PG), fut_swap = swap_kb(fut, pages * PG);
    snprintf(label, sizeof label, "swap: an mremap-grown locked mapping and an MCL_FUTURE one stay "
             "out of swap (Swap %ld and %ld kB, unlocked control %ld kB)", grown_swap, fut_swap,
             ctl_swap);
    check(label, grown_swap == 0 && fut_swap == 0);
    int same = 1;
    for (size_t i = 0; i < pages; i++)
        same &= ctl[i * PG] == (char) (i + 1) && grown[i * PG] == (char) (i + 2) &&
                fut[i * PG] == (char) (i + 3);
    check("swap: all three read back what was written", same);
}

// ---- RLIMIT_MEMLOCK ----------------------------------------------------------

static int drop_privilege(void) {
    if (geteuid() != 0)
        return 1;
    return setgid(65534) == 0 && setuid(65534) == 0;
}

// The soft limit, which is what Linux charges against; the hard one stays, so
// an unprivileged process can raise it again.
static int set_memlock(rlim_t bytes) {
    struct rlimit rl;
    if (getrlimit(RLIMIT_MEMLOCK, &rl) != 0 || (rl.rlim_max != RLIM_INFINITY && bytes > rl.rlim_max))
        return 0;
    rl.rlim_cur = bytes;
    return setrlimit(RLIMIT_MEMLOCK, &rl) == 0;
}

static void memlock_limits_child(void) {
    char label[200];
    int fd = tmp_file(8);
    if (!drop_privilege() || fd < 0 || !set_memlock(0)) {
        check("limits: set up an unprivileged process with RLIMIT_MEMLOCK 0", 0);
        return;
    }
    char *p = mmap(NULL, PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    errno = 0;
    int r = mlock(p, PG);
    int err = errno;
    snprintf(label, sizeof label, "limits: at 0, mlock is EPERM (r %d errno %d)", r, err);
    check(label, r == -1 && err == EPERM);
    check("limits: at 0, munlock is 0", munlock(p, PG) == 0);
    errno = 0;
    r = mlockall(MCL_FUTURE);
    err = errno;
    snprintf(label, sizeof label, "limits: at 0, mlockall(MCL_FUTURE) is EPERM (r %d errno %d)", r, err);
    check(label, r == -1 && err == EPERM);
    errno = 0;
    r = mlockall(MCL_CURRENT);
    err = errno;
    snprintf(label, sizeof label, "limits: at 0, mlockall(MCL_CURRENT) is EPERM (r %d errno %d)", r, err);
    check(label, r == -1 && err == EPERM);
    // A file mapping: musl turns an anonymous mapping's EPERM into ENOMEM.
    errno = 0;
    char *ml = mmap(NULL, PG, PROT_READ, MAP_PRIVATE | MAP_LOCKED, fd, 0);
    err = errno;
    snprintf(label, sizeof label, "limits: at 0, MAP_LOCKED is EPERM (%s errno %d)",
             ml == MAP_FAILED ? "failed" : "mapped", err);
    check(label, ml == MAP_FAILED && err == EPERM);
    check("limits: at 0, munlockall is 0", munlockall() == 0);

    if (!set_memlock(4 * PG)) {
        check("limits: RLIMIT_MEMLOCK 4 pages", 0);
        return;
    }
    char *m = guarded(3, PROT_READ | PROT_WRITE);
    char *n = guarded(2, PROT_READ | PROT_WRITE);
    if (m == NULL || n == NULL) {
        check("limits: map 3 and 2 pages", 0);
        return;
    }
    check("limits: mlock 3 of 4", mlock(m, 3 * PG) == 0 && vmlck() == 12);
    r = mlock(m, 3 * PG);
    snprintf(label, sizeof label, "limits: locking the same 3 again is not charged twice "
             "(r %d VmLck %ld)", r, vmlck());
    check(label, r == 0 && vmlck() == 12);
    errno = 0;
    r = mlock(n, 2 * PG);
    err = errno;
    snprintf(label, sizeof label, "limits: 2 more is ENOMEM (r %d errno %d VmLck %ld)", r, err, vmlck());
    check(label, r == -1 && err == ENOMEM && vmlck() == 12);
    check("limits: 1 more, to the limit", mlock(n, PG) == 0 && vmlck() == 16);
    munlock(n, PG);
    errno = 0;
    char *g = mremap(m, 3 * PG, 5 * PG, MREMAP_MAYMOVE);
    err = errno;
    snprintf(label, sizeof label, "limits: mremap growing a locked mapping past it is EAGAIN "
             "(%s errno %d VmLck %ld)", g == MAP_FAILED ? "failed" : "grew", err, vmlck());
    check(label, g == MAP_FAILED && err == EAGAIN && vmlck() == 12);
    g = mremap(m, 3 * PG, 4 * PG, MREMAP_MAYMOVE);
    snprintf(label, sizeof label, "limits: growing it to the limit is fine (VmLck %ld)", vmlck());
    check(label, g != MAP_FAILED && vmlck() == 16);
    munlockall();
    errno = 0;
    r = mlockall(MCL_CURRENT);
    err = errno;
    snprintf(label, sizeof label, "limits: mlockall(MCL_CURRENT) of more than the limit is ENOMEM "
             "(r %d errno %d VmLck %ld)", r, err, vmlck());
    check(label, r == -1 && err == ENOMEM && vmlck() == 0);

    check("limits: mlockall(MCL_FUTURE) under a nonzero limit", mlockall(MCL_FUTURE) == 0);
    char *f1 = mmap(NULL, 3 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check("limits: a 3-page mapping fits", f1 != MAP_FAILED && vmlck() == 12);
    errno = 0;
    char *f2 = mmap(NULL, 2 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    err = errno;
    snprintf(label, sizeof label, "limits: 2 more pages is EAGAIN (%s errno %d VmLck %ld)",
             f2 == MAP_FAILED ? "failed" : "mapped", err, vmlck());
    check(label, f2 == MAP_FAILED && err == EAGAIN && vmlck() == 12);
    long b0 = syscall(SYS_brk, 0);
    long b1 = syscall(SYS_brk, ((b0 + (long) PG - 1) & ~(long) (PG - 1)) + 3 * (long) PG);
    snprintf(label, sizeof label, "limits: a brk past it does not move the break (%#lx -> %#lx)",
             b0, b1);
    check(label, b1 == b0 && vmlck() == 12);
    munlockall();
    errno = 0;
    char *f3 = mmap(NULL, 5 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_LOCKED, -1, 0);
    err = errno;
    snprintf(label, sizeof label, "limits: MAP_LOCKED of 5 pages is EAGAIN (%s errno %d)",
             f3 == MAP_FAILED ? "failed" : "mapped", err);
    check(label, f3 == MAP_FAILED && err == EAGAIN && vmlck() == 0);

    // MREMAP_FIXED unmaps its destination first: locked pages there come off
    // the count before the growth is charged, so this fits at the limit.
    char *src = guarded(2, PROT_READ | PROT_WRITE);
    char *dst = guarded(4, PROT_READ | PROT_WRITE);
    if (src != NULL && dst != NULL && mlock(src, 2 * PG) == 0 && mlock(dst, 2 * PG) == 0) {
        long before = vmlck();
        errno = 0;
        char *moved = mremap(src, 2 * PG, 4 * PG, MREMAP_MAYMOVE | MREMAP_FIXED, dst);
        err = errno;
        snprintf(label, sizeof label, "limits: MREMAP_FIXED growing onto locked pages at the limit "
                 "fits (%s errno %d VmLck %ld -> %ld)", moved == MAP_FAILED ? "failed" : "moved",
                 err, before, vmlck());
        check(label, before == 16 && moved == dst && vmlck() == 16);
    } else {
        check("limits: set up MREMAP_FIXED onto locked pages", 0);
    }
    munlockall();

    if (!set_memlock(3 * PG)) {
        check("limits: RLIMIT_MEMLOCK 3 pages", 0);
        return;
    }
    char *s = mmap(NULL, 2 * PG, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    check("limits: lock 2 shared pages", s != MAP_FAILED && mlock(s, 2 * PG) == 0 && vmlck() == 8);
    errno = 0;
    char *a2 = mremap(s, 0, 2 * PG, MREMAP_MAYMOVE);
    err = errno;
    snprintf(label, sizeof label, "limits: a 2-page alias past it is EAGAIN (%s errno %d)",
             a2 == MAP_FAILED ? "failed" : "mapped", err);
    check(label, a2 == MAP_FAILED && err == EAGAIN && vmlck() == 8);
    char *a1 = mremap(s, 0, PG, MREMAP_MAYMOVE);
    check("limits: a 1-page alias fits", a1 != MAP_FAILED && vmlck() == 12);
}

// The process that locked its stack grows it: within the limit, then past it.
static void locked_stack_child(void) {
    if (!drop_privilege()) {
        _exit(10);
    }
    // Half a megabyte of room over what mlockall locks. Linux starts a stack
    // with 128 kB below it already mapped, so the control recursion goes
    // deeper than that to be sure the stack grows at all.
    long size = status_kb("VmSize");
    if (size <= 0 || !set_memlock((rlim_t) (size + 512) * 1024) || mlockall(MCL_CURRENT) != 0)
        _exit(11);
    long l0 = vmlck();
    deep(64);
    if (vmlck() <= l0)
        _exit(12);      // the control: the stack grew, locked, within the limit
    // Past RLIMIT_MEMLOCK: a SIGSEGV, as for any stack growth refused.
    deep(1024);
    _exit(13);
}

static void locked_stack_limit(void) {
    char label[160];
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0)
        locked_stack_child();
    int status = 0;
    waitpid(pid, &status, 0);
    snprintf(label, sizeof label, "stack: a locked stack growing past RLIMIT_MEMLOCK is SIGSEGV "
             "(exit %d signal %d)", WIFEXITED(status) ? WEXITSTATUS(status) : -1,
             WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    check(label, WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    // No stdio buffer malloc'd later, under MCL_FUTURE.
    static char outbuf[1 << 16];
    setvbuf(stdout, outbuf, _IOLBF, sizeof outbuf);
    long base = vmlck();
    check("VmLck is in /proc/self/status, and 0 before anything is locked", base == 0);
    int fd = tmp_file(8);
    check("a scratch file", fd >= 0);
    if (fd < 0)
        return finish_suite("mlock_accounting");
    file_mapping_and_forced_write(fd);
    maps_split_at_lock(fd);
    mlock_ranges();
    madvise_refuses_locked();
    fork_and_cow();
    mremap_carries_lock();
    mprotect_populates_locked();
    map_locked_flag(fd);
    in_child("mlockall(MCL_CURRENT)", mlockall_current_child);
    in_child("mlockall(MCL_FUTURE)", mlockall_future_child);
    in_child("RLIMIT_MEMLOCK", memlock_limits_child);
    in_child("swap", swap_witness_child);
    locked_stack_limit();
    char label[80];
    snprintf(label, sizeof label, "nothing left locked (VmLck %ld)", vmlck());
    check(label, vmlck() == 0);
    return finish_suite("mlock_accounting");
}
