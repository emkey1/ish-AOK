// mmap/mremap/membarrier argument handling, and three things AOK got wrong in
// ways a caller could not detect.
//
//   mremap with new_len == 0 unmapped every page of the mapping and returned
//   the old address as SUCCESS -- not a value in the errno window, so a caller
//   had no in-band way to learn it now held a pointer into a hole. Any size
//   computation that rounds or underflows to zero destroyed the mapping it
//   meant to resize. Linux returns EINVAL and leaves the mapping intact.
//
//   mremap with old_len == 0 is not a resize: it asks for a second mapping of
//   the same pages, which Linux allows only for a SHARED mapping. AOK checked
//   nothing, so the private case Linux rejects leaked a stray mapping on every
//   call, and the legitimate shared case "succeeded" while handing back a
//   fresh zero page instead of an alias -- silently losing the coherence that
//   was the whole point of asking.
//
//   mmap of /dev/zero returned ENODEV. It is the oldest portable way to get
//   anonymous memory and Linux has always supported it. /dev/null and /dev/full
//   are NOT mappable on Linux either, and are checked here so the fix does not
//   quietly widen.
//
//   membarrier advertised five commands next to a comment reading "lies", and
//   implemented a fence in the calling thread -- which is not what membarrier
//   provides. crossbeam-epoch, liburcu and .NET's FlushProcessWriteBuffers all
//   query the mask and skip their own fallback when the kernel claims support,
//   so claiming it disabled the code that would have worked. Unknown commands
//   and nonzero flags returned success, and PRIVATE_EXPEDITED worked without
//   the registration whose EPERM is how a runtime learns to register.
//
// Measured against x86_64 glibc on Linux 6.12. The one deliberate difference
// is the QUERY mask itself: Linux reports 0x3ff, AOK reports the subset it
// actually implements, so the mask is not asserted equal here.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#include "test_common.h"

// Spelled out rather than taken from the host headers, which do not have them
// on every platform this is compiled for.
#define MADV_REMOVE_       9
#define MADV_WIPEONFORK_  18
#define MADV_KEEPONFORK_  19

#define MEMBARRIER_CMD_QUERY                       0
#define MEMBARRIER_CMD_PRIVATE_EXPEDITED           (1 << 3)
#define MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED  (1 << 4)

static void ck(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-54s got=%-8ld want=%ld\n", label, got, want);
}

// Can this address be read without dying? Asked in a child, so that a wrong
// answer costs a fork rather than the whole suite.
static int readable(void *p) {
    fflush(NULL);
    pid_t c = fork();
    if (c == 0) {
        volatile char v = *(volatile char *) p;
        (void) v;
        _exit(0);
    }
    int st;
    if (waitpid(c, &st, 0) != c)
        return -1;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

// What byte does a forked child see at p[0]? Returned through the exit status,
// so the parent's own copy is never consulted.
static int child_byte(char *p) {
    fflush(NULL);
    pid_t c = fork();
    if (c == 0)
        _exit((unsigned char) p[0]);
    int st;
    if (waitpid(c, &st, 0) != c)
        return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static long mb(int cmd, int flags) {
    errno = 0;
    long v = syscall(SYS_membarrier, cmd, flags, 0);
    return v < 0 ? errno : 0;
}

int main(int argc, char **argv) {
    // Re-executed by the membarrier section below: report whether exec cleared
    // the registration, which Linux does and AOK now does.
    if (argc > 1 && strcmp(argv[1], "--exec-child") == 0)
        return mb(MEMBARRIER_CMD_PRIVATE_EXPEDITED, 0) == EPERM ? 0 : 1;

    test_init(argc, argv);
    alarm(test_watchdog_secs(90));

    // ---- mremap(new_len == 0) must not destroy the mapping ----------------
    {
        char *m = mmap(NULL, 8192, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m == MAP_FAILED) {
            printf("FAIL mmap: %s\n", strerror(errno));
            return finish_suite("mmap_validation");
        }
        memset(m, 'A', 8192);
        errno = 0;
        long r = syscall(SYS_mremap, m, (size_t) 8192, (size_t) 0, MREMAP_MAYMOVE, 0);
        ck("mremap(new_len=0) is EINVAL", r == -1 ? errno : 0, EINVAL);
        int alive = readable(m);
        ck("  the original mapping survives", alive, 1);
        // only safe to dereference here once we know it is still mapped
        ck("  with its contents intact",
           alive == 1 ? (m[0] == 'A' && m[8191] == 'A') : 0, 1);
        if (alive == 1)
            munmap(m, 8192);

        // A real shrink still works, which is what makes the case above
        // specific to zero rather than to shrinking.
        char *k = mmap(NULL, 8192, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        memset(k, 'S', 8192);
        errno = 0;
        char *shrunk = (char *) syscall(SYS_mremap, k, (size_t) 8192,
                                        (size_t) 4096, MREMAP_MAYMOVE, 0);
        ck("a genuine shrink still succeeds", shrunk != MAP_FAILED, 1);
        if (shrunk != MAP_FAILED) {
            ck("  and stays readable", readable(shrunk), 1);
            munmap(shrunk, 4096);
        }
    }

    // ---- mremap(old_len == 0): EINVAL private, a true alias when shared ----
    {
        char *m = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        memset(m, 'B', 4096);
        errno = 0;
        long r = syscall(SYS_mremap, m, (size_t) 0, (size_t) 4096, MREMAP_MAYMOVE, 0);
        ck("mremap(old_len=0) on a PRIVATE mapping is EINVAL",
           r == -1 ? errno : 0, EINVAL);
        munmap(m, 4096);

        char *sh = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        memset(sh, 'C', 4096);
        errno = 0;
        char *dup = (char *) syscall(SYS_mremap, sh, (size_t) 0,
                                     (size_t) 4096, MREMAP_MAYMOVE, 0);
        ck("mremap(old_len=0) on a SHARED mapping succeeds", dup != MAP_FAILED, 1);
        if (dup != MAP_FAILED) {
            int ok = readable(dup);
            ck("  the duplicate is mapped", ok, 1);
            ck("  and sees the original's data", ok == 1 ? dup[0] == 'C' : 0, 1);
            sh[0] = 'Z';
            ck("  and is a live alias, not a snapshot", ok == 1 ? dup[0] == 'Z' : 0, 1);
            dup[1] = 'Y';
            ck("  in both directions", ok == 1 ? sh[1] == 'Y' : 0, 1);
            if (ok == 1)
                munmap(dup, 4096);
        }
        munmap(sh, 4096);
    }

    // ---- /dev/zero is mappable; /dev/null and /dev/full are not -----------
    {
        int fd = open("/dev/zero", O_RDWR);
        ck("/dev/zero opens", fd >= 0, 1);
        if (fd >= 0) {
            for (int shared = 0; shared < 2; shared++) {
                errno = 0;
                char *p = mmap(NULL, 8192, PROT_READ | PROT_WRITE,
                               shared ? MAP_SHARED : MAP_PRIVATE, fd, 0);
                ck(shared ? "mmap(/dev/zero, MAP_SHARED)"
                          : "mmap(/dev/zero, MAP_PRIVATE)",
                   p != MAP_FAILED ? 0 : errno, 0);
                if (p != MAP_FAILED) {
                    ck("  reads as zeroes", p[0] == 0 && p[8191] == 0, 1);
                    p[0] = 'X';
                    ck("  and is writable", p[0] == 'X', 1);
                    munmap(p, 8192);
                }
            }
            close(fd);
        }
        static const char *unmappable[] = { "/dev/null", "/dev/full" };
        for (unsigned i = 0; i < 2; i++) {
            int f = open(unmappable[i], O_RDWR);
            if (f < 0)
                continue;
            errno = 0;
            char *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE, f, 0);
            char label[64];
            snprintf(label, sizeof label, "mmap(%s) is still ENODEV", unmappable[i]);
            ck(label, p == MAP_FAILED ? errno : 0, ENODEV);
            if (p != MAP_FAILED)
                munmap(p, 4096);
            close(f);
        }
    }

    // ---- membarrier validates, and PRIVATE_EXPEDITED needs registering ----
    {
        errno = 0;
        long mask = syscall(SYS_membarrier, MEMBARRIER_CMD_QUERY, 0, 0);
        ck("membarrier(QUERY) reports a mask", mask > 0, 1);
        test_logf("    mask = %#lx (Linux reports more; AOK claims what it implements)\n",
                  mask < 0 ? 0 : mask);
        // Whatever is advertised must be exactly what the switch accepts.
        ck("  and does not advertise PRIVATE_EXPEDITED without implementing it",
           (mask & MEMBARRIER_CMD_PRIVATE_EXPEDITED) != 0, 1);

        ck("an unknown command is EINVAL", mb(9999, 0), EINVAL);
        ck("nonzero flags are EINVAL", mb(MEMBARRIER_CMD_QUERY, 12345), EINVAL);
        ck("PRIVATE_EXPEDITED before registering is EPERM",
           mb(MEMBARRIER_CMD_PRIVATE_EXPEDITED, 0), EPERM);
        ck("REGISTER_PRIVATE_EXPEDITED succeeds",
           mb(MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED, 0), 0);
        ck("PRIVATE_EXPEDITED then succeeds",
           mb(MEMBARRIER_CMD_PRIVATE_EXPEDITED, 0), 0);

        // The registration is per-process: inherited across fork, cleared by
        // exec. A runtime that re-execs must be told to register again.
        fflush(NULL);
        pid_t c = fork();
        if (c == 0)
            _exit(mb(MEMBARRIER_CMD_PRIVATE_EXPEDITED, 0) == 0 ? 0 : 1);
        int st;
        waitpid(c, &st, 0);
        ck("the registration is inherited across fork",
           WIFEXITED(st) && WEXITSTATUS(st) == 0, 1);

        fflush(NULL);
        c = fork();
        if (c == 0) {
            execl(argv[0], argv[0], "--exec-child", (char *) NULL);
            _exit(127);
        }
        waitpid(c, &st, 0);
        ck("and is cleared by exec", WIFEXITED(st) && WEXITSTATUS(st) == 0, 1);
    }

    // ---- msync actually writes back ---------------------------------------
    {
        // Coherence was never the problem -- guest mappings share their host
        // pages, so a store is visible immediately. Durability was: no
        // writeback was ever issued, and the file's mtime never moved.
        //
        // One difference from Linux is deliberate and not asserted here:
        // Linux advances mtime on the first write FAULT, so it has already
        // moved before msync is called. AOK maps the host file directly, so
        // there is no fault to hook and the time moves when the writeback
        // happens. What matters -- that a writeback happens at all -- is what
        // this checks.
        char path[128];
        snprintf(path, sizeof path, "/tmp/msync-check-%d.bin", (int) getpid());
        unlink(path);
        int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
        ck("msync: the scratch file opens", fd >= 0, 1);
        if (fd >= 0) {
            ck("  and can be sized", ftruncate(fd, 4096) == 0, 1);
            struct stat before, after;
            fstat(fd, &before);
            sleep(2);                    // a 1-second mtime needs room to move
            char *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            ck("  and maps shared", p != MAP_FAILED, 1);
            if (p != MAP_FAILED) {
                memcpy(p, "HELLOMMAP", 9);
                errno = 0;
                ck("  msync(MS_SYNC) succeeds", msync(p, 4096, MS_SYNC), 0);
                fstat(fd, &after);
                ck("  and the file was written back (mtime moved)",
                   before.st_mtime != after.st_mtime, 1);
                char buf[16] = { 0 };
                pread(fd, buf, 9, 0);
                ck("  the content is there", strcmp(buf, "HELLOMMAP") == 0, 1);

                // Argument validation, which was already right and must stay.
                errno = 0;
                ck("  MS_SYNC|MS_ASYNC together is EINVAL",
                   msync(p, 4096, MS_SYNC | MS_ASYNC) < 0 ? errno : 0, EINVAL);
                errno = 0;
                ck("  an unknown flag is EINVAL",
                   msync(p, 4096, 0x1000) < 0 ? errno : 0, EINVAL);
                errno = 0;
                ck("  an unaligned address is EINVAL",
                   msync(p + 1, 4096, MS_SYNC) < 0 ? errno : 0, EINVAL);
                ck("  len 0 succeeds", msync(p, 0, MS_SYNC), 0);
                ck("  MS_ASYNC succeeds", msync(p, 4096, MS_ASYNC), 0);
                munmap(p, 4096);
            }
            close(fd);
            unlink(path);
        }
    }

    // ---- PROT_WRITE on a shared mapping of a read-only file ---------------
    {
        // mmap already refused this at map time; mprotect was the door it left
        // open. It returned 0 and the store that followed hit a read-only host
        // mapping and killed the process with SIGBUS, so a program that checks
        // the return -- as it should -- had no way to see it coming.
        char path[128];
        snprintf(path, sizeof path, "/tmp/prot-check-%d.bin", (int) getpid());
        unlink(path);
        int w = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
        ck("prot: the scratch file opens", w >= 0, 1);
        if (w >= 0) {
            char filler[4096];
            memset(filler, 'F', sizeof filler);
            ck("  and can be filled", write(w, filler, sizeof filler) == sizeof filler, 1);
            close(w);

            int ro = open(path, O_RDONLY);
            errno = 0;
            char *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, ro, 0);
            ck("mmap(PROT_WRITE, MAP_SHARED) of an O_RDONLY fd is EACCES",
               p == MAP_FAILED ? errno : 0, EACCES);
            if (p != MAP_FAILED)
                munmap(p, 4096);

            p = mmap(NULL, 4096, PROT_READ, MAP_SHARED, ro, 0);
            ck("mmap(PROT_READ, MAP_SHARED) of an O_RDONLY fd succeeds",
               p != MAP_FAILED, 1);
            if (p != MAP_FAILED) {
                errno = 0;
                ck("  mprotect(PROT_WRITE) on it is EACCES",
                   mprotect(p, 4096, PROT_READ | PROT_WRITE) < 0 ? errno : 0, EACCES);
                munmap(p, 4096);
            }

            // A private mapping of the same fd may be made writable: the write
            // is a copy, so the file is not at risk.
            p = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, ro, 0);
            if (p != MAP_FAILED) {
                errno = 0;
                ck("mprotect(PROT_WRITE) on a PRIVATE mapping is allowed",
                   mprotect(p, 4096, PROT_READ | PROT_WRITE) < 0 ? errno : 0, 0);
                p[0] = 'w';
                ck("  and the store works", p[0] == 'w', 1);
                munmap(p, 4096);
            }
            close(ro);

            // A writable fd, and shared anonymous memory, are both fine.
            int rw = open(path, O_RDWR);
            p = mmap(NULL, 4096, PROT_READ, MAP_SHARED, rw, 0);
            if (p != MAP_FAILED) {
                errno = 0;
                ck("mprotect(PROT_WRITE) on a SHARED map of an O_RDWR fd is allowed",
                   mprotect(p, 4096, PROT_READ | PROT_WRITE) < 0 ? errno : 0, 0);
                munmap(p, 4096);
            }
            close(rw);
            unlink(path);

            p = mmap(NULL, 4096, PROT_READ, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
            if (p != MAP_FAILED) {
                errno = 0;
                ck("mprotect(PROT_WRITE) on shared ANONYMOUS is allowed",
                   mprotect(p, 4096, PROT_READ | PROT_WRITE) < 0 ? errno : 0, 0);
                munmap(p, 4096);
            }
        }
    }

    // ---- MADV_WIPEONFORK and MADV_REMOVE ----------------------------------
    {
        // WIPEONFORK exists for a page holding something that must not cross a
        // fork -- a PRNG state, a key. It was accepted and ignored, so the
        // child inherited exactly what the caller asked to have withheld.
        char *m = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        ck("madv: a private anonymous page maps", m != MAP_FAILED, 1);
        if (m != MAP_FAILED) {
            memset(m, 'A', 4096);
            errno = 0;
            ck("madvise(MADV_WIPEONFORK) succeeds",
               madvise(m, 4096, MADV_WIPEONFORK_) < 0 ? errno : 0, 0);
            ck("  the child sees zeroes", child_byte(m), 0);
            ck("  the parent keeps its data", (unsigned char) m[0], 'A');
            ck("madvise(MADV_KEEPONFORK) succeeds",
               madvise(m, 4096, MADV_KEEPONFORK_) < 0 ? errno : 0, 0);
            ck("  and the child inherits again", child_byte(m), 'A');
            munmap(m, 4096);
        }
        char *sh = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        if (sh != MAP_FAILED) {
            errno = 0;
            ck("MADV_WIPEONFORK on a SHARED mapping is EINVAL",
               madvise(sh, 4096, MADV_WIPEONFORK_) < 0 ? errno : 0, EINVAL);
            // MADV_REMOVE punches a hole: zero for every mapper.
            memset(sh, 'A', 4096);
            errno = 0;
            ck("MADV_REMOVE on shared anonymous succeeds",
               madvise(sh, 4096, MADV_REMOVE_) < 0 ? errno : 0, 0);
            ck("  and the range reads back as zero", (unsigned char) sh[0], 0);
            munmap(sh, 4096);
        }
        char *pv = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (pv != MAP_FAILED) {
            memset(pv, 'A', 4096);
            errno = 0;
            ck("MADV_REMOVE on private anonymous is EINVAL",
               madvise(pv, 4096, MADV_REMOVE_) < 0 ? errno : 0, EINVAL);
            ck("  and leaves the data alone", (unsigned char) pv[0], 'A');
            munmap(pv, 4096);
        }
        // ...and on a shared FILE mapping the zeroes reach the file.
        char path[128];
        snprintf(path, sizeof path, "/tmp/madv-check-%d.bin", (int) getpid());
        unlink(path);
        int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            char filler[4096];
            memset(filler, 'Z', sizeof filler);
            if (write(fd, filler, sizeof filler) == sizeof filler) {
                char *fm = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
                ck("madv: a shared file mapping maps", fm != MAP_FAILED, 1);
                if (fm != MAP_FAILED) {
                    errno = 0;
                    ck("MADV_REMOVE on a shared FILE mapping succeeds",
                       madvise(fm, 4096, MADV_REMOVE_) < 0 ? errno : 0, 0);
                    ck("  the mapping reads back as zero", (unsigned char) fm[0], 0);
                    char one = 1;
                    pread(fd, &one, 1, 0);
                    ck("  and so does the file itself", (unsigned char) one, 0);
                    munmap(fm, 4096);
                }
            }
            close(fd);
            unlink(path);
        }
    }

    return finish_suite("mmap_validation");
}
