// splice(2) and vmsplice(2), and what tee(2) says when it cannot be done.
//
//   splice was an unconditional EINVAL and vmsplice and tee were ENOSYS, so
//   every caller of them -- and of the pipe-shaped fast paths libraries build
//   on splice -- fell back or failed. On arm64 and riscv64 the dispatcher made
//   it worse: splice was called with all six arguments replaced by zeros,
//   because it was a stub that read none of them.
//
//   What splice guarantees is which bytes move where, not that they move
//   without being copied. The zero-copy is why it exists on Linux and it is
//   not observable through the syscall, so AOK moves them through the same
//   buffered engine sendfile and copy_file_range use.
//
//   tee is different, and stays ENOSYS on purpose. It duplicates between two
//   pipes WITHOUT consuming the source, which is a statement about the pipe's
//   own buffer -- Linux takes another reference to the pages already in it.
//   AOK's pipes are host pipes and the buffer belongs to the host kernel,
//   which offers no way to read without consuming; emulating it by reading and
//   writing the bytes back would reorder anything else queued and deadlock
//   against a full pipe. ENOSYS is what a caller must already handle, since
//   tee is Linux 2.6.17+.
//
// Measured against x86_64 glibc on Linux 6.12.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

#include "test_common.h"

#define SPLICE_F_NONBLOCK_ 0x02

static char src_path[64], dst_path[64];

static void ck(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-56s got=%-8ld want=%ld\n", label, got, want);
}

static long rc_of(long r) { return r < 0 ? -errno : r; }

static long do_splice(int in, long long *in_off, int out, long long *out_off,
                      size_t len, unsigned flags) {
    errno = 0;
    return rc_of(syscall(SYS_splice, in, in_off, out, out_off, len, flags));
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));
    snprintf(src_path, sizeof src_path, "/tmp/splice-src-%d", (int) getpid());
    snprintf(dst_path, sizeof dst_path, "/tmp/splice-dst-%d", (int) getpid());
    {
        int fd = open(src_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        ck("stage the source file", fd >= 0, 1);
        if (fd >= 0) {
            ck("  write 16 bytes", (long) write(fd, "0123456789ABCDEF", 16), 16);
            close(fd);
        }
    }

    // ---- file -> pipe -> file, the whole point of the call -----------------
    {
        int in = open(src_path, O_RDONLY);
        int pf[2];
        ck("pipe", pipe(pf), 0);
        long moved = do_splice(in, NULL, pf[1], NULL, 8, 0);
        ck("splice(file -> pipe, 8)", moved, 8);
        // With no explicit offset the file's own position moves, exactly as a
        // read would have moved it.
        ck("  the source position advanced", (long) lseek(in, 0, SEEK_CUR), 8);

        int out = open(dst_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        // Only drain the pipe if the first splice put something in it: on a
        // kernel where splice does not work the pipe is empty, and reading it
        // anyway would hang the test instead of failing it.
        long back = moved == 8 ? do_splice(pf[0], NULL, out, NULL, 8, 0) : -1;
        ck("splice(pipe -> file, 8)", back, 8);
        close(out);

        char buf[32] = { 0 };
        int chk = open(dst_path, O_RDONLY);
        long n = chk >= 0 ? (long) read(chk, buf, sizeof buf - 1) : -1;
        if (chk >= 0)
            close(chk);
        ck("  the destination got 8 bytes", n, 8);
        // The bytes themselves, not just the count: a copy that moved the
        // right number of the wrong bytes would pass the count check.
        ck("  and they are the right ones", strcmp(buf, "01234567") == 0, 1);
        close(in);
        close(pf[0]);
        close(pf[1]);
    }

    // ---- an explicit offset reads from there and leaves the position alone --
    {
        int in = open(src_path, O_RDONLY);
        int pf[2];
        ck("pipe", pipe(pf), 0);
        long long off = 4;
        long moved = do_splice(in, &off, pf[1], NULL, 4, 0);
        ck("splice(file at offset 4 -> pipe, 4)", moved, 4);
        ck("  the offset argument advanced", (long) off, 8);
        ck("  and the file position did NOT move", (long) lseek(in, 0, SEEK_CUR), 0);
        char buf[16] = { 0 };
        // Same guard: an empty pipe must fail the check, not block on it.
        long got = moved == 4 ? (long) read(pf[0], buf, 4) : -1;
        ck("  the pipe has the bytes from offset 4", got, 4);
        ck("  which are \"4567\"", strncmp(buf, "4567", 4) == 0, 1);
        close(in);
        close(pf[0]);
        close(pf[1]);
    }

    // ---- what splice refuses ----------------------------------------------
    {
        int a = open(src_path, O_RDONLY);
        int b = open(dst_path, O_WRONLY | O_CREAT, 0644);
        int pf[2];
        ck("pipe", pipe(pf), 0);
        // Neither end a pipe: splice moves data THROUGH a pipe, and with
        // neither it does nothing read+write does not.
        ck("file -> file is EINVAL", do_splice(a, NULL, b, NULL, 4, 0), -EINVAL);
        // A pipe has no position, so naming an offset for one is a
        // contradiction -- ESPIPE, not EINVAL.
        long long off = 0;
        ck("an offset on the pipe end is ESPIPE", do_splice(pf[0], &off, pf[1], NULL, 4, 0), -ESPIPE);
        ck("a zero length is 0", do_splice(a, NULL, pf[1], NULL, 0, 0), 0);
        ck("an unknown flag is EINVAL", do_splice(a, NULL, pf[1], NULL, 4, 0x80), -EINVAL);
        close(a);
        close(b);
        close(pf[0]);
        close(pf[1]);
    }
    {
        // NONBLOCK is about the pipe: with nothing to read the answer is
        // EAGAIN, not a wait. Getting this wrong hangs rather than fails,
        // which is why it is worth its own case.
        int empty[2];
        ck("pipe", pipe(empty), 0);
        int out = open(dst_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        ck("splice from an empty pipe with NONBLOCK is EAGAIN",
           do_splice(empty[0], NULL, out, NULL, 4, SPLICE_F_NONBLOCK_), -EAGAIN);
        close(out);
        close(empty[0]);
        close(empty[1]);
    }

    // ---- vmsplice, both directions ----------------------------------------
    {
        int pf[2];
        ck("pipe", pipe(pf), 0);
        struct iovec iov[2] = { { (void *) "hello", 5 }, { (void *) "world", 5 } };
        errno = 0;
        long vs = rc_of(syscall(SYS_vmsplice, pf[1], iov, (unsigned long) 2, 0));
        ck("vmsplice into the write end gathers both iovs", vs, 10);
        char buf[32] = { 0 };
        long in_pipe = vs == 10 ? (long) read(pf[0], buf, sizeof buf - 1) : -1;
        ck("  the pipe holds 10 bytes", in_pipe, 10);
        ck("  in order", strcmp(buf, "helloworld") == 0, 1);

        // ...and out of the read end, scattered across the iovs.
        ck("  seed the pipe", (long) write(pf[1], "abcdef", 6), 6);
        char out1[4] = { 0 }, out2[4] = { 0 };
        struct iovec riov[2] = { { out1, 3 }, { out2, 3 } };
        errno = 0;
        ck("vmsplice out of the read end scatters",
           rc_of(syscall(SYS_vmsplice, pf[0], riov, (unsigned long) 2, 0)), 6);
        ck("  first iov got \"abc\"", strncmp(out1, "abc", 3) == 0, 1);
        ck("  second iov got \"def\"", strncmp(out2, "def", 3) == 0, 1);

        int fd = open(src_path, O_RDONLY);
        errno = 0;
        ck("vmsplice on something that is not a pipe is EBADF",
           rc_of(syscall(SYS_vmsplice, fd, iov, (unsigned long) 1, 0)), -EBADF);
        close(fd);
        close(pf[0]);
        close(pf[1]);
    }

    // ---- tee: implemented, or honestly absent ------------------------------
    {
        int pa[2], pb[2];
        ck("pipe a", pipe(pa), 0);
        ck("pipe b", pipe(pb), 0);
        ck("seed pipe a", (long) write(pa[1], "teetest", 7), 7);
        errno = 0;
        long r = rc_of(syscall(SYS_tee, pa[0], pb[1], (size_t) 7, 0));
        test_logf("  %-56s got=%ld\n", "tee answers", r);
        ck("tee either works or is ENOSYS", r == 7 || r == -ENOSYS, 1);
        if (r == 7) {
            // If it worked it must not have consumed: both pipes hold it.
            char x[16] = { 0 }, y[16] = { 0 };
            ck("  the destination got it", (long) read(pb[0], x, 7), 7);
            ck("  and the SOURCE still has it", (long) read(pa[0], y, 7), 7);
            ck("  unchanged", strncmp(y, "teetest", 7) == 0, 1);
        } else {
            // Refused: it must not have consumed either.
            char y[16] = { 0 };
            ck("  a refusal did not consume the source", (long) read(pa[0], y, 7), 7);
            ck("  which is unchanged", strncmp(y, "teetest", 7) == 0, 1);
        }
        close(pa[0]);
        close(pa[1]);
        close(pb[0]);
        close(pb[1]);
    }

    unlink(src_path);
    unlink(dst_path);
    return finish_suite("splice_vmsplice");
}
