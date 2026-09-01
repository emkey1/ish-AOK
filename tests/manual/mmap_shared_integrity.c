// Three ways a MAP_SHARED mapping quietly stopped being shared.
//
//   A ptrace poke into a tracee's shared file mapping set P_COW along with
//   P_WRITE, so the page was copied and the tracee was switched to a private
//   duplicate. The poke never reached the file, and every store the TRACEE
//   itself made afterwards was lost as well -- with no error anywhere.
//
//   A PROT_NONE MAP_SHARED anonymous region is reserved with no host backing
//   and one shared descriptor that every mapper points at. The first side to
//   mprotect it accessible allocated a page for ITSELF, ending the sharing at
//   the exact moment the mapping first became usable. Two processes that
//   mprotected such a region after fork never saw each other's writes again --
//   while doing the mprotect BEFORE the fork worked, which is what made it
//   look like a fork bug rather than an mprotect one.
//
//   memfd F_SEAL_WRITE was granted while a shared mapping was live. That is
//   worse than not implementing sealing: F_GET_SEALS then reported a guarantee
//   nothing was keeping, stores through the live mapping still landed, and a
//   receiver who mapped the "sealed" fd afterwards saw the mutations. Linux
//   refuses with EBUSY -- for a read-only shared mapping too, since it can be
//   mprotected writable later, and NOT for a writable private one. That is the
//   opposite of what "writable" would suggest, so it is checked as a matrix.
//
// Measured against x86_64 glibc on Linux 6.12.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#include "test_common.h"

#ifndef F_ADD_SEALS
#define F_ADD_SEALS 1033
#define F_GET_SEALS 1034
#endif
#ifndef F_SEAL_WRITE
#define F_SEAL_WRITE 0x0008
#endif
#ifndef MFD_ALLOW_SEALING
#define MFD_ALLOW_SEALING 2U
#endif

static void ck(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-56s got=%-7ld want=%ld\n", label, got, want);
}

static int memfd(const char *name) {
    return (int) syscall(SYS_memfd_create, name, MFD_ALLOW_SEALING);
}

// Does a shared PROT_NONE anonymous region stay shared when each side
// mprotects it? Run both orderings relative to the fork -- the pre-fork one is
// the control that made this look like a fork bug.
static int shared_after_mprotect(int mprotect_before_fork) {
    char *m = mmap(NULL, 4096, PROT_NONE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED)
        return -1;
    if (mprotect_before_fork)
        mprotect(m, 4096, PROT_READ | PROT_WRITE);
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        munmap(m, 4096);
        return -1;
    }
    fflush(NULL);
    pid_t c = fork();
    if (c == 0) {
        close(pipefd[0]);
        if (!mprotect_before_fork)
            mprotect(m, 4096, PROT_READ | PROT_WRITE);
        memcpy(m, "KID!", 4);
        // Handshake rather than a sleep: the parent must read strictly after
        // the child's store, or a pass could be luck.
        ssize_t w = write(pipefd[1], "x", 1);
        _exit(w == 1 ? 0 : 1);
    }
    close(pipefd[1]);
    if (!mprotect_before_fork)
        mprotect(m, 4096, PROT_READ | PROT_WRITE);
    char sync_byte;
    int got = read(pipefd[0], &sync_byte, 1) == 1 && memcmp(m, "KID!", 4) == 0;
    int st;
    waitpid(c, &st, 0);
    close(pipefd[0]);
    munmap(m, 4096);
    return got;
}

// F_SEAL_WRITE with one mapping of the given kind live. Returns the errno, or
// 0 when the seal was granted.
static int seal_with_mapping(int prot, int flags) {
    int fd = memfd("sealmatrix");
    if (fd < 0)
        return -1;
    if (ftruncate(fd, 4096) != 0) {
        close(fd);
        return -1;
    }
    char *p = NULL;
    if (flags != 0) {
        p = mmap(NULL, 4096, prot, flags, fd, 0);
        if (p == MAP_FAILED) {
            close(fd);
            return -1;
        }
    }
    errno = 0;
    int r = fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE);
    int e = r < 0 ? errno : 0;
    if (p != NULL)
        munmap(p, 4096);
    close(fd);
    return e;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(90));

    // ---- a ptrace poke must not privatise a shared mapping -----------------
    {
        char path[128];
        snprintf(path, sizeof path, "/tmp/poke-check-%d.bin", (int) getpid());
        unlink(path);
        int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
        ck("poke: the scratch file opens", fd >= 0, 1);
        if (fd >= 0) {
            char filler[4096];
            memset(filler, 'A', sizeof filler);
            ck("  and can be filled", write(fd, filler, sizeof filler) == sizeof filler, 1);
            int pipefd[2];
            if (pipe(pipefd) == 0) {
                fflush(NULL);
                pid_t c = fork();
                if (c == 0) {
                    close(pipefd[0]);
                    ptrace(PTRACE_TRACEME, 0, NULL, NULL);
                    char *m = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
                    if (m == MAP_FAILED)
                        _exit(2);
                    if (write(pipefd[1], &m, sizeof m) != (ssize_t) sizeof m)
                        _exit(3);
                    raise(SIGSTOP);
                    memcpy(m + 16, "CHILDWR", 7);   // the tracee's OWN store, after the poke
                    msync(m, 4096, MS_SYNC);
                    _exit(0);
                }
                close(pipefd[1]);
                char *addr = NULL;
                ck("  the tracee reported its mapping",
                   read(pipefd[0], &addr, sizeof addr) == (ssize_t) sizeof addr, 1);
                int st;
                waitpid(c, &st, WUNTRACED);
                ck("  and stopped for us", WIFSTOPPED(st), 1);
                // PTRACE_POKEDATA moves exactly one machine word, so the
                // payload and the check both have to be sizeof(long) -- 4 on
                // a 32-bit guest, 8 elsewhere. A fixed 7-byte payload
                // overflowed `word` on i386 (UB, and gcc says so), poked only
                // "KOPE", and then compared 7 bytes that could never match.
                // Real Linux fails it identically under gcc -m32: the
                // assertion was unsatisfiable on any 32-bit platform, which is
                // a test bug and not a kernel one.
                static const char poke[] = "KOPEDATA";   // 8 bytes of payload
                long word = 0;
                memcpy(&word, poke, sizeof word);
                errno = 0;
                ck("  PTRACE_POKEDATA succeeds",
                   ptrace(PTRACE_POKEDATA, c, addr, (void *) word) < 0 ? errno : 0, 0);
                char buf[16] = { 0 };
                pread(fd, buf, sizeof word, 0);
                ck("  the poke reached the file",
                   memcmp(buf, poke, sizeof word) == 0, 1);
                ptrace(PTRACE_CONT, c, NULL, NULL);
                waitpid(c, &st, 0);
                ck("  the tracee ran to completion",
                   WIFEXITED(st) && WEXITSTATUS(st) == 0, 1);
                memset(buf, 0, sizeof buf);
                pread(fd, buf, 7, 16);
                ck("  and its own later store reached the file too",
                   memcmp(buf, "CHILDWR", 7) == 0, 1);
                close(pipefd[0]);
            }
            close(fd);
            unlink(path);
        }
    }

    // ---- a shared reservation stays shared once mprotected ------------------
    {
        ck("mprotect BEFORE the fork keeps it shared", shared_after_mprotect(1), 1);
        ck("mprotect AFTER the fork keeps it shared too", shared_after_mprotect(0), 1);
    }

    // ---- F_SEAL_WRITE while a mapping is live ------------------------------
    {
        // The matrix, because which mappings block it is not what the names
        // suggest: any SHARED mapping does, no PRIVATE one does.
        ck("seal: no mapping at all -> granted", seal_with_mapping(0, 0), 0);
        ck("seal: PROT_READ  MAP_SHARED  -> EBUSY",
           seal_with_mapping(PROT_READ, MAP_SHARED), EBUSY);
        ck("seal: PROT_WRITE MAP_SHARED  -> EBUSY",
           seal_with_mapping(PROT_READ | PROT_WRITE, MAP_SHARED), EBUSY);
        ck("seal: PROT_READ  MAP_PRIVATE -> granted",
           seal_with_mapping(PROT_READ, MAP_PRIVATE), 0);
        ck("seal: PROT_WRITE MAP_PRIVATE -> granted",
           seal_with_mapping(PROT_READ | PROT_WRITE, MAP_PRIVATE), 0);

        // ...and the seal is real once it is granted.
        int fd = memfd("sealreal");
        ck("seal: memfd_create succeeds", fd >= 0, 1);
        if (fd >= 0) {
            ftruncate(fd, 4096);
            char *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            ck("  a writable shared mapping succeeds", p != MAP_FAILED, 1);
            if (p != MAP_FAILED) {
                memcpy(p, "BEFORE", 6);
                errno = 0;
                ck("  F_SEAL_WRITE while it is live is EBUSY",
                   fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE) < 0 ? errno : 0, EBUSY);
                ck("  and F_GET_SEALS honestly reports none",
                   fcntl(fd, F_GET_SEALS), 0);
                munmap(p, 4096);
                errno = 0;
                ck("  once unmapped it is granted",
                   fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE) < 0 ? errno : 0, 0);
                ck("  and reported", fcntl(fd, F_GET_SEALS), F_SEAL_WRITE);
                errno = 0;
                ck("  pwrite is then refused",
                   pwrite(fd, "X", 1, 0) < 0 ? errno : 0, EPERM);
                errno = 0;
                char *q = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
                ck("  and a new writable shared mapping is refused",
                   q == MAP_FAILED ? errno : 0, EPERM);
                if (q != MAP_FAILED)
                    munmap(q, 4096);
                char *ro = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0);
                ck("  a read-only mapping still works", ro != MAP_FAILED, 1);
                if (ro != MAP_FAILED) {
                    ck("  and sees the sealed content", memcmp(ro, "BEFORE", 6) == 0, 1);
                    munmap(ro, 4096);
                }
            }
            close(fd);
        }
    }

    return finish_suite("mmap_shared_integrity");
}
