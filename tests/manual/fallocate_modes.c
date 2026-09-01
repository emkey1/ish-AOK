// What fallocate's mode argument means.
//
//   It was UNUSED. Every fallocate did the same one thing -- extend the file
//   if the range ran past the end -- whatever mode said, and reported success.
//
//   So FALLOC_FL_PUNCH_HOLE, whose entire purpose is to zero a range, left the
//   old bytes exactly where they were and said it had worked. A caller
//   punching a hole to erase something still had it, and had been told
//   otherwise. And FALLOC_FL_KEEP_SIZE, whose entire meaning is "allocate but
//   do not change the size", grew the file: a preallocating writer found its
//   file the wrong length. Both are silent data bugs, not error-reporting
//   ones, which is what makes them worth a test.
//
//   Nothing was validated either: a negative offset, a zero length, an
//   unknown mode bit and a read-only fd were all accepted.
//
// Measured against x86_64 glibc on Linux 6.12 (ext4). Where a mode is
// filesystem-specific -- COLLAPSE_RANGE and INSERT_RANGE, which ext4 and xfs
// implement and most others refuse -- the test accepts either answer, because
// both are things a real kernel says.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <stdint.h>
#include "test_common.h"

#ifndef FALLOC_FL_KEEP_SIZE
#define FALLOC_FL_KEEP_SIZE      0x01
#endif
#ifndef FALLOC_FL_PUNCH_HOLE
#define FALLOC_FL_PUNCH_HOLE     0x02
#endif
#ifndef FALLOC_FL_COLLAPSE_RANGE
#define FALLOC_FL_COLLAPSE_RANGE 0x08
#endif
#ifndef FALLOC_FL_ZERO_RANGE
#define FALLOC_FL_ZERO_RANGE     0x10
#endif
#ifndef FALLOC_FL_INSERT_RANGE
#define FALLOC_FL_INSERT_RANGE   0x20
#endif

#define FILE_BYTES 8192

static char path[64];

static void ck(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-56s got=%-8ld want=%ld\n", label, got, want);
}

// A fresh FILE_BYTES of 'A'.
static int fresh(void) {
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    char *buf = malloc(FILE_BYTES);
    if (buf == NULL) {
        close(fd);
        return -1;
    }
    memset(buf, 'A', FILE_BYTES);
    ssize_t n = write(fd, buf, FILE_BYTES);
    free(buf);
    if (n != FILE_BYTES) {
        close(fd);
        return -1;
    }
    return fd;
}

// fallocate is SYSCALL_DEFINE4(fd, mode, loff_t, loff_t) on a 64-bit ABI, but
// a 32-bit one splits each loff_t into a (low, high) dword pair -- so the i386
// kernel entry takes SIX arguments, and AOK implements exactly that
// (kernel/fs.c's sys_fallocate). libc's syscall() reads six argument slots
// whichever ABI it is on, so passing four handed the kernel two words of stack
// garbage as the high halves of offset and length. Real Linux fails the same
// way under gcc -m32; this was never a kernel bug.
static long fallocate_raw(int fd, int mode, long long off, long long len) {
    errno = 0;
    long r;
#if defined(__i386__) || (defined(__SIZEOF_LONG__) && __SIZEOF_LONG__ == 4)
    r = syscall(SYS_fallocate, fd, mode,
                (long) (uint32_t) (uint64_t) off,  (long) (uint32_t) ((uint64_t) off >> 32),
                (long) (uint32_t) (uint64_t) len,  (long) (uint32_t) ((uint64_t) len >> 32));
#else
    r = syscall(SYS_fallocate, fd, mode, (long) off, (long) len);
#endif
    return r < 0 ? -errno : r;
}

static long file_size(int fd) {
    struct stat st;
    if (fstat(fd, &st) != 0)
        return -1;
    return (long) st.st_size;
}

// Is [off, off+len) all zeros?
static int range_is_zero(int fd, long off, long len) {
    char buf[512];
    long done = 0;
    while (done < len) {
        size_t want = (size_t) (len - done);
        if (want > sizeof buf)
            want = sizeof buf;
        ssize_t n = pread(fd, buf, want, off + done);
        if (n <= 0)
            return 0;
        for (ssize_t i = 0; i < n; i++)
            if (buf[i] != 0)
                return 0;
        done += n;
    }
    return 1;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));
    snprintf(path, sizeof path, "/tmp/fallocate-modes-%d", (int) getpid());

    // ---- the range and mode are checked before anything happens -----------
    {
        int fd = fresh();
        ck("staged the file", fd >= 0, 1);
        if (fd >= 0) {
            ck("a zero length is EINVAL", fallocate_raw(fd, 0, 0, 0), -EINVAL);
            ck("a negative length is EINVAL", fallocate_raw(fd, 0, 0, -1), -EINVAL);
            ck("a negative offset is EINVAL", fallocate_raw(fd, 0, -1, 4096), -EINVAL);
            ck("an unknown mode bit is EOPNOTSUPP", fallocate_raw(fd, 0x40, 0, 4096), -EOPNOTSUPP);
            // PUNCH_HOLE and ZERO_RANGE contradict each other, and a punch that
            // does not keep the size is not a punch. Both are EOPNOTSUPP on
            // Linux, not EINVAL -- an easy pair to get backwards.
            ck("PUNCH_HOLE without KEEP_SIZE is EOPNOTSUPP",
               fallocate_raw(fd, FALLOC_FL_PUNCH_HOLE, 4096, 4096), -EOPNOTSUPP);
            ck("PUNCH_HOLE|ZERO_RANGE is EOPNOTSUPP",
               fallocate_raw(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_ZERO_RANGE | FALLOC_FL_KEEP_SIZE,
                             4096, 4096), -EOPNOTSUPP);
            ck("  and the file is untouched", file_size(fd), FILE_BYTES);
            close(fd);
        }
    }

    // ---- the fd has to be one you could write ----------------------------
    {
        int fd = open(path, O_RDONLY);
        ck("an O_RDONLY fd is EBADF", fallocate_raw(fd, 0, 0, 4096), -EBADF);
        if (fd >= 0)
            close(fd);
        int pf[2];
        ck("pipe", pipe(pf), 0);
        ck("a pipe is ESPIPE", fallocate_raw(pf[1], 0, 0, 4096), -ESPIPE);
        close(pf[0]);
        close(pf[1]);
    }

    // ---- KEEP_SIZE means the size does not change -------------------------
    {
        int fd = fresh();
        ck("staged the file", fd >= 0, 1);
        if (fd >= 0) {
            ck("plain fallocate past EOF succeeds", fallocate_raw(fd, 0, FILE_BYTES, FILE_BYTES), 0);
            ck("  and the file grew", file_size(fd), 2 * FILE_BYTES);
            ck("  and the new region reads as zeros",
               range_is_zero(fd, FILE_BYTES, FILE_BYTES), 1);
            close(fd);
        }
        fd = fresh();
        if (fd >= 0) {
            ck("KEEP_SIZE past EOF succeeds",
               fallocate_raw(fd, FALLOC_FL_KEEP_SIZE, FILE_BYTES, FILE_BYTES), 0);
            ck("  and the size is UNCHANGED", file_size(fd), FILE_BYTES);
            close(fd);
        }
    }

    // ---- PUNCH_HOLE actually zeroes ---------------------------------------
    {
        int fd = fresh();
        ck("staged the file", fd >= 0, 1);
        if (fd >= 0) {
            ck("PUNCH_HOLE|KEEP_SIZE succeeds",
               fallocate_raw(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 4096, 4096), 0);
            ck("  the punched range reads as zeros", range_is_zero(fd, 4096, 4096), 1);
            ck("  the size is unchanged", file_size(fd), FILE_BYTES);
            // ...and only the punched range: a punch that zeroed the whole file
            // would pass the check above.
            char before[8] = { 0 };
            if (pread(fd, before, sizeof before, 0) != (ssize_t) sizeof before)
                failures_total++;
            ck("  and the bytes before it are untouched", before[0] == 'A', 1);
            // Entirely past the end is a no-op, not an extension.
            ck("PUNCH_HOLE past EOF succeeds",
               fallocate_raw(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                             FILE_BYTES, 4096), 0);
            ck("  and did not grow the file", file_size(fd), FILE_BYTES);
            close(fd);
        }
    }

    // ---- ZERO_RANGE zeroes, and may extend --------------------------------
    {
        int fd = fresh();
        if (fd >= 0) {
            ck("ZERO_RANGE inside the file succeeds",
               fallocate_raw(fd, FALLOC_FL_ZERO_RANGE, 4096, 4096), 0);
            ck("  the range reads as zeros", range_is_zero(fd, 4096, 4096), 1);
            ck("  the size is unchanged", file_size(fd), FILE_BYTES);
            close(fd);
        }
        fd = fresh();
        if (fd >= 0) {
            ck("ZERO_RANGE past EOF succeeds",
               fallocate_raw(fd, FALLOC_FL_ZERO_RANGE, FILE_BYTES, 4096), 0);
            ck("  and extended the file", file_size(fd), FILE_BYTES + 4096);
            ck("  with the new region zeroed", range_is_zero(fd, FILE_BYTES, 4096), 1);
            close(fd);
        }
    }

    // ---- the range-shifting modes ----------------------------------------
    // ext4 and xfs implement these; most filesystems answer EOPNOTSUPP, and
    // ext4 itself rejects some ranges with EINVAL. What must NOT happen is the
    // old behaviour: succeeding and doing a plain allocation instead, which
    // moves no data and leaves the caller believing it did.
    {
        int fd = fresh();
        if (fd >= 0) {
            long r = fallocate_raw(fd, FALLOC_FL_COLLAPSE_RANGE, 4096, 4096);
            test_logf("  %-56s got=%ld\n", "COLLAPSE_RANGE answers", r);
            ck("COLLAPSE_RANGE either works or is refused",
               r == 0 || r == -EOPNOTSUPP || r == -EINVAL, 1);
            if (r != 0)
                ck("  and a refusal left the size alone", file_size(fd), FILE_BYTES);
            // Keeping the size is meaningless for a mode that moves the
            // file's contents. Linux 6.12 answers EOPNOTSUPP here, not EINVAL
            // -- it treats the mode bits as an enum with KEEP_SIZE as the one
            // modifier, and EINVAL is reserved for the range.
            ck("COLLAPSE_RANGE|KEEP_SIZE is EOPNOTSUPP",
               fallocate_raw(fd, FALLOC_FL_COLLAPSE_RANGE | FALLOC_FL_KEEP_SIZE, 4096, 4096),
               -EOPNOTSUPP);
            ck("INSERT_RANGE|KEEP_SIZE is EOPNOTSUPP",
               fallocate_raw(fd, FALLOC_FL_INSERT_RANGE | FALLOC_FL_KEEP_SIZE, 4096, 4096),
               -EOPNOTSUPP);
            close(fd);
        }
    }

    unlink(path);
    return finish_suite("fallocate_modes");
}
