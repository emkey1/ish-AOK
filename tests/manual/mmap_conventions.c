// Memory-mapping calls that answered without looking.
//
//   mincore reported every mapped page resident, so it said the whole address
//   space was already in RAM. Programs use it to decide what to bring in -- an
//   allocator sizing a madvise, a GC choosing pages to scan, a loader deciding
//   whether to prefault -- and were told there was nothing to do.
//
//   mlock and munlock returned 0 for any range at all, including one that was
//   not mapped. The usual reason to call mlock is keeping a secret out of
//   swap, and a caller that got its range wrong was told it had succeeded.
//   (The locking itself is still a no-op -- AOK cannot pin host pages, and a
//   lock is advisory against swap, which iOS manages itself. Claiming a range
//   EXISTS when it does not is a different kind of answer.)
//
//   MAP_FIXED means "here or nowhere". A request at address 0 fell past the
//   fixed-address handling into the pick-any-hole path, so it was answered
//   with a mapping somewhere else and reported as success -- the one thing
//   MAP_FIXED exists to rule out.
//
//   MAP_SHARED_VALIDATE is spelled as both the SHARED and PRIVATE bits
//   together, and was rejected as the contradiction it looks like. It is
//   MAP_SHARED with strict flag checking, so callers using the safer of the
//   two spellings could not map at all.
//
//   PROT_SEM asks for a mapping usable for atomics -- which every mapping
//   here already is. Linux accepts and ignores it everywhere.
//
//   SEEK_DATA and SEEK_HOLE were EINVAL. A filesystem is always allowed to
//   report that a file has no holes, and that is now the answer; EINVAL told
//   callers the interface did not exist, and the tools that copy sparsely
//   (cp --sparse, tar, rsync) act on that.
//
//   memfd_create reported EFAULT for a name that was merely too long, sending
//   the caller to look for a bad pointer it did not have.
//
// Measured against x86_64 glibc on Linux 6.12.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "test_common.h"

#ifndef MAP_SHARED_VALIDATE
#define MAP_SHARED_VALIDATE 0x03
#endif
#ifndef PROT_SEM
#define PROT_SEM 0x8
#endif
#ifndef SEEK_DATA
#define SEEK_DATA 3
#define SEEK_HOLE 4
#endif

#define PS 4096

static char base[80];

static void ck(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-58s got=%-10ld want=%ld\n", label, got, want);
}

static int mkfile(const char *name, int pages) {
    char path[160];
    snprintf(path, sizeof path, "%s/%s", base, name);
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    char buf[PS];
    memset(buf, 'A', sizeof buf);
    for (int i = 0; i < pages; i++)
        if (write(fd, buf, sizeof buf) != (ssize_t) sizeof buf) {
            close(fd);
            return -1;
        }
    return fd;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(90));
    snprintf(base, sizeof base, "/tmp/mmapconv-%d", (int) getpid());
    char cmd[160];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", base);
    if (system(cmd) < 0)
        failures_total++;
    ck("mkdir base", mkdir(base, 0755), 0);

    // ---- mincore reports real residency ----------------------------------
    {
        unsigned char vec[4];
        char *a = mmap(NULL, 4 * PS, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        ck("map four anonymous pages", a == MAP_FAILED ? 0 : 1, 1);
        if (a != MAP_FAILED) {
            memset(vec, 0xAA, sizeof vec);
            ck("mincore on a fresh mapping succeeds", mincore(a, 4 * PS, vec), 0);
            int resident = 0;
            for (int i = 0; i < 4; i++)
                resident += vec[i] & 1;
            // Nothing has been touched, so nothing is in memory yet. This is
            // the assertion that failed before: every page came back 1.
            ck("  and reports none of them resident", resident, 0);

            a[0] = 'x';
            memset(vec, 0xAA, sizeof vec);
            ck("mincore after touching page 0", mincore(a, 4 * PS, vec), 0);
            ck("  page 0 is resident", vec[0] & 1, 1);
            // The other three are deliberately NOT asserted. Residency is the
            // HOST's answer, and on arm64 macOS the host page is 16K -- four
            // guest pages to one host page -- so touching one really does make
            // all four resident. That is a true answer about a different page
            // size, not an over-report.
            munmap(a, 4 * PS);
        }
        // A range that is not mapped at all is ENOMEM, not a zero vector.
        char *hole = mmap(NULL, PS, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (hole != MAP_FAILED) {
            munmap(hole, PS);
            errno = 0;
            ck("mincore on an unmapped range is ENOMEM",
               mincore(hole, PS, vec) < 0 ? errno : 0, ENOMEM);
        }
    }

    // ---- mlock / munlock check the range ---------------------------------
    {
        char *a = mmap(NULL, PS, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (a != MAP_FAILED) {
            errno = 0;
            ck("mlock on a mapped range succeeds", mlock(a, PS), 0);
            errno = 0;
            ck("munlock on it succeeds", munlock(a, PS), 0);
            munmap(a, PS);
            // Now that it is gone, the same range is ENOMEM.
            errno = 0;
            ck("mlock on an unmapped range is ENOMEM", mlock(a, PS) < 0 ? errno : 0, ENOMEM);
            errno = 0;
            ck("munlock on it too", munlock(a, PS) < 0 ? errno : 0, ENOMEM);
        }
    }

    // ---- MAP_FIXED at address 0 ------------------------------------------
    {
        errno = 0;
        void *a = mmap((void *) 0, PS, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        // Either it maps exactly at 0 (a system that allows it) or it is
        // refused. What it must never do is succeed somewhere else, which is
        // what it used to do.
        ck("MAP_FIXED at 0 does not relocate", a == MAP_FAILED || a == (void *) 0 ? 1 : 0, 1);
        if (a == MAP_FAILED)
            ck("  and is refused with EPERM", errno, EPERM);
        else if (a != (void *) 0)
            munmap(a, PS);
    }

    // ---- MAP_SHARED_VALIDATE and PROT_SEM --------------------------------
    {
        int fd = mkfile("sv", 1);
        ck("stage a file", fd >= 0 ? 1 : 0, 1);
        if (fd >= 0) {
            errno = 0;
            char *a = mmap(NULL, PS, PROT_READ | PROT_WRITE, MAP_SHARED_VALIDATE, fd, 0);
            ck("mmap MAP_SHARED_VALIDATE succeeds", a == MAP_FAILED ? 0 : 1, 1);
            if (a != MAP_FAILED) {
                a[0] = 'S';
                msync(a, PS, MS_SYNC);
                char c = 0;
                ck("  and the mapping is really shared", pread(fd, &c, 1, 0) == 1 && c == 'S' ? 1 : 0, 1);
                munmap(a, PS);
            }
            close(fd);
        }
        errno = 0;
        char *b = mmap(NULL, PS, PROT_READ | PROT_WRITE | PROT_SEM,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        ck("mmap with PROT_SEM succeeds", b == MAP_FAILED ? 0 : 1, 1);
        if (b != MAP_FAILED) {
            // ...and the mapping is usable, i.e. PROT_SEM was ignored rather
            // than folded into the protection bits.
            b[0] = 'q';
            ck("  and the mapping works", b[0] == 'q' ? 1 : 0, 1);
            errno = 0;
            ck("mprotect with PROT_SEM succeeds", mprotect(b, PS, PROT_READ | PROT_SEM), 0);
            munmap(b, PS);
        }
    }

    // ---- SEEK_DATA / SEEK_HOLE -------------------------------------------
    {
        int fd = mkfile("sk", 2);
        ck("stage a two-page file", fd >= 0 ? 1 : 0, 1);
        if (fd >= 0) {
            errno = 0;
            ck("SEEK_DATA at 0 is 0", (long) lseek(fd, 0, SEEK_DATA), 0);
            errno = 0;
            // A fully-allocated file's only hole is the implicit one at EOF.
            ck("SEEK_HOLE at 0 is EOF", (long) lseek(fd, 0, SEEK_HOLE), 2 * PS);
            errno = 0;
            ck("SEEK_DATA in the middle is where you are", (long) lseek(fd, PS, SEEK_DATA), PS);
            // Past EOF is ENXIO for both -- there is no more data and no more
            // file to hold a hole.
            errno = 0;
            ck("SEEK_DATA past EOF is ENXIO",
               lseek(fd, 4 * PS, SEEK_DATA) < 0 ? errno : 0, ENXIO);
            errno = 0;
            ck("SEEK_HOLE past EOF is ENXIO",
               lseek(fd, 4 * PS, SEEK_HOLE) < 0 ? errno : 0, ENXIO);
            // ...and ordinary seeking is untouched.
            ck("SEEK_SET still works", (long) lseek(fd, PS, SEEK_SET), PS);
            ck("SEEK_END still works", (long) lseek(fd, 0, SEEK_END), 2 * PS);
            close(fd);
        }
    }

    // ---- memfd_create name length ----------------------------------------
    {
        char name[320];
        memset(name, 'n', sizeof name);
        name[299] = '\0';
        errno = 0;
        long r = syscall(SYS_memfd_create, name, 0u);
        ck("memfd_create with a 299-char name is EINVAL", r < 0 ? errno : 0, EINVAL);
        ck("  and specifically not EFAULT", r < 0 && errno == EFAULT ? 1 : 0, 0);
        if (r >= 0)
            close((int) r);
        // The longest name that fits still works, so the check is a length
        // check and not a blanket refusal.
        name[249] = '\0';
        errno = 0;
        r = syscall(SYS_memfd_create, name, 0u);
        ck("a 249-char name is accepted", r >= 0 ? 1 : 0, 1);
        if (r >= 0)
            close((int) r);
        // A genuinely bad pointer is still EFAULT.
        errno = 0;
        r = syscall(SYS_memfd_create, (void *) 1, 0u);
        ck("a bad name pointer is EFAULT", r < 0 ? errno : 0, EFAULT);
        if (r >= 0)
            close((int) r);
    }

    snprintf(cmd, sizeof cmd, "rm -rf '%s'", base);
    if (system(cmd) < 0)
        failures_total++;
    return finish_suite("mmap_conventions");
}
