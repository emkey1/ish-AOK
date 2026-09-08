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
#include <sys/mman.h>
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

// /dev/kmsg is where a guest can read the emulator's own log. Open it and
// throw away everything already buffered, so a later read sees only what the
// calls below produced. Returns -1 if the log is not readable, which is not a
// failure -- on real Linux this needs CAP_SYSLOG, and the checks that use it
// are skipped rather than guessed at.
static int kmsg_open_drained(void) {
    int fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
    if (fd < 0)
        return -1;
    char junk[8192];
    while (read(fd, junk, sizeof junk) > 0)
        continue;
    return fd;
}

// Collect every record logged since the drain into one buffer. This reads the
// stream to EAGAIN, which is why it must happen ONCE and be searched many
// times rather than the other way round: a per-needle reader consumes the
// records the first needle did not match, so every check after the first one
// silently examines an empty stream and passes. (Measured -- with a per-needle
// reader, three of the four checks below could not fail even against a build
// that was logging the line they look for.) /dev/kmsg hands back one whole
// record per read.
static void kmsg_slurp(int fd, char *out, size_t outsize) {
    size_t used = 0;
    char rec[8192];
    ssize_t n;
    out[0] = '\0';
    while ((n = read(fd, rec, sizeof rec - 1)) > 0) {
        if (used + (size_t) n + 1 >= outsize)
            break;
        memcpy(out + used, rec, (size_t) n);
        used += (size_t) n;
        out[used] = '\0';
    }
}

static int log_has(const char *log, const char *kind, long nr) {
    char needle[64];
    // The trailing space matters: without it "syscall 75" also matches a
    // syscall 750.
    snprintf(needle, sizeof needle, "%s syscall %ld ", kind, nr);
    return strstr(log, needle) != NULL;
}

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

    // ---- the offset may live anywhere the guest can address ---------------
    //
    // Same call as above, with the offset variable in an mmap'd page instead of
    // on the stack. That is not a contrived place to keep one -- it is where a
    // heap allocation lands -- and on amd64 it was the difference between
    // working and being KILLED. amd64 routed splice through the legacy
    // marshalled table to sys_splice, the i386 entry point, whose offsets are
    // 32-bit addr_t; the marshaller refuses an argument that does not fit a
    // dword and delivers SIGSYS. A stack offset (0xffffec78 on this guest) fits
    // and passed, which is why the case above never caught it; an mmap'd one
    // (0x7ffffdfc7000) does not. sendfile and copy_file_range were already
    // routed natively for exactly this reason -- splice was the one left out.
    {
        int in = open(src_path, O_RDONLY);
        int pf[2];
        ck("pipe", pipe(pf), 0);
        long long *off = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        ck("an mmap'd page for the offset", off != MAP_FAILED, 1);
        if (off != MAP_FAILED) {
            *off = 4;
            long moved = do_splice(in, off, pf[1], NULL, 4, 0);
            ck("splice with the offset in mmap'd memory", moved, 4);
            ck("  the offset argument advanced", (long) *off, 8);
            char buf[16] = { 0 };
            long got = moved == 4 ? (long) read(pf[0], buf, 4) : -1;
            ck("  and the right bytes moved", got, 4);
            ck("  which are \"4567\"", strncmp(buf, "4567", 4) == 0, 1);
            munmap(off, 4096);
        }
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

    // ---- a short answer, rather than waiting for bytes nobody will send ----
    //
    // splice returns what it moved. Draining the full `count` instead is a hang
    // whenever the caller is the only one who could supply the rest -- and that
    // is not a corner case, it is how GNU cat copies. cat makes a pipe of its
    // own and bounces the input through it:
    //     pipe(p); splice(0, .., p[1], .., 65536); splice(p[0], .., 1, .., 65536)
    // The second call asks a pipe holding three bytes for 65536, and the only
    // writer of that pipe is cat, which will not write again until this call
    // returns. AOK's copy engine looped for the other 65533 forever: every
    // `echo hi | cat` and `cat <<EOF` in the system wedged, taking five of
    // bash's own regression tests (alias, builtins, comsub, comsub-eof,
    // comsub-posix) with them.
    {
        int pf[2];
        ck("pipe", pipe(pf), 0);
        ck("put three bytes in it", (long) write(pf[1], "abc", 3), 3);
        int out = open(dst_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        ck("open the destination", out >= 0, 1);
        // The write end stays OPEN across this call, exactly as cat holds it:
        // there is no EOF coming to end the wait, only a short return.
        ck("splice(pipe with 3 bytes -> file, asking 65536) answers 3",
           do_splice(pf[0], NULL, out, NULL, 65536, 0), 3);
        close(out);
        char buf[16] = { 0 };
        int chk = open(dst_path, O_RDONLY);
        ck("  the three bytes landed", chk >= 0 ? (long) read(chk, buf, sizeof buf - 1) : -1, 3);
        ck("  and are the right ones", strcmp(buf, "abc") == 0, 1);
        if (chk >= 0)
            close(chk);
        close(pf[0]);
        close(pf[1]);
    }

    // ---- and the same on the write side ------------------------------------
    //
    // The mirror image: splice a file into a pipe, asking for more than the
    // pipe can hold. This is splice's canonical idiom -- file into a pipe, then
    // pipe out to a socket, one thread doing both -- and the only reader of
    // that pipe is the caller, waiting for this call to return. Filling the
    // pipe and then waiting for room is the same deadlock from the other end.
    // The bytes that did not fit must go back to the file's position, not be
    // dropped: the caller resumes from where the short answer said it stopped.
    {
        int big = open(src_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        ck("stage a file larger than a pipe", big >= 0, 1);
        char meg[4096];
        memset(meg, 'z', sizeof meg);
        long staged = 0;
        for (int i = 0; i < 256 && big >= 0; i++) {   // 1 MiB
            long w = (long) write(big, meg, sizeof meg);
            if (w != (long) sizeof meg) break;
            staged += w;
        }
        ck("  1 MiB staged", staged, 1024 * 1024);
        if (big >= 0)
            close(big);

        int in = open(src_path, O_RDONLY);
        int pf[2];
        ck("pipe", pipe(pf), 0);
        // Seed the pipe first, so the room left in it is not a round number and
        // the copy has to stop PART WAY through a chunk it has already read.
        // That is the path that can lose data rather than merely hang: bytes
        // taken out of the file and not written have to go back.
        long seeded = (long) write(pf[1], meg, 4096);
        ck("  seed the pipe so it cannot take a whole chunk", seeded, 4096);
        long moved = do_splice(in, NULL, pf[1], NULL, 1024 * 1024, 0);
        ck("splice(1 MiB file -> pipe) returns what fit, not a hang",
           moved > 0 && moved < 1024 * 1024, 1);
        test_logf("  %-56s got=%ld\n", "  bytes it took", moved);
        // Nothing was taken out of the file that did not reach the pipe.
        ck("  the file position matches what it moved",
           (long) lseek(in, 0, SEEK_CUR), moved);
        char buf[8192];
        long drained = 0, n;
        while (drained < seeded + moved && (n = (long) read(pf[0], buf, sizeof buf)) > 0)
            drained += n;
        ck("  and the pipe holds the seed plus exactly that many", drained, seeded + moved);
        close(in);
        close(pf[0]);
        close(pf[1]);
    }

    // ---- tee: implemented, or honestly absent ------------------------------
    //
    // The FOUR-argument call below is itself a regression, so do not "tidy" it
    // into anything else. amd64's legacy marshaller classified tee as taking
    // six arguments -- the default -- and so validated r8/r9, which a
    // four-argument syscall() never writes. The verdict then came from whatever
    // the caller left in those registers: glibc reliably leaves a >4 GiB
    // address there, so this line SIGSYS-KILLED the process every time on a
    // Devuan root, while the same source passed on Alpine's musl. That is why
    // it read as a flake -- and why this suite has to be run on a glibc amd64
    // root as well as a musl one to see it at all.
    {
        int pa[2], pb[2];
        ck("pipe a", pipe(pa), 0);
        ck("pipe b", pipe(pb), 0);
        ck("seed pipe a", (long) write(pa[1], "teetest", 7), 7);
        errno = 0;
        long r = rc_of(syscall(SYS_tee, pa[0], pb[1], (size_t) 7, 0));
        test_logf("  %-56s got=%ld\n", "tee answers", r);
        ck("tee either works or is ENOSYS", r == 7 || r == -ENOSYS, 1);

        // The same call with the two registers past tee's fourth argument
        // carrying values that cannot fit a dword. A four-argument syscall()
        // never writes them, so on a real kernel they hold caller garbage and
        // are ignored; amd64's arity classifier defaulted to six and VALIDATED
        // them, turning whatever happened to be in r8/r9 into the verdict. The
        // plain four-argument call above cannot pin that -- it dies only when
        // the leftovers happen not to fit, which is why this looked like a
        // flake -- so state the values outright and make it deterministic.
        errno = 0;
        long rx = rc_of(syscall(SYS_tee, pa[0], pb[1], (size_t) 7, 0,
                                0x7fffffffffffULL, 0x7fffffffffffULL));
        ck("tee ignores what is past its fourth argument", rx == 7 || rx == -ENOSYS, 1);
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

    // ---- a working call must not announce itself as unimplemented ---------
    //
    // The arm64/riscv64 table named vmsplice and tee twice: once with their
    // real implementations near the top, and again as syscall_stub in a later
    // parity sweep -- and a later designated initializer wins. Neither call
    // actually broke, because handle_asm_generic_native_syscall answers 75/76/77
    // before the table is ever called, so nothing above this line could see it:
    // every transfer check still passed.
    //
    // But the table IS read for one thing first -- "is this entry a stub?" --
    // and that writes the kernel log. So each successful vmsplice printed
    // "arm64 stub syscall 75" at ERROR level, and on arm64/riscv64 the log
    // budget does not apply (it only covers abi < 2), so it printed on every
    // call, forever. A guest reading its own log was told a syscall that had
    // just moved its bytes did not exist.
    //
    // The needle carries THIS arch's syscall number, which is what the log
    // prints, so the check follows the test to i386, amd64, arm64 and riscv64
    // without knowing their numbering. The trailing space matters: without it
    // "syscall 75 " would also match a syscall 750.
    {
        int klog = kmsg_open_drained();
        if (klog < 0) {
            test_logf("  %-56s (skipped: /dev/kmsg not readable)\n",
                      "the kernel log does not call these stubs");
        } else {
            int pf[2];
            ck("pipe", pipe(pf), 0);
            struct iovec iov = { (void *) "logcheck", 8 };
            errno = 0;
            ck("vmsplice moves bytes", rc_of(syscall(SYS_vmsplice, pf[1], &iov, (unsigned long) 1, 0)), 8);
            char buf[16] = { 0 };
            ck("  which read back", (long) read(pf[0], buf, 8), 8);
            ck("  and are the right ones", strncmp(buf, "logcheck", 8) == 0, 1);

            int pb[2];
            ck("pipe", pipe(pb), 0);
            errno = 0;
            long t = rc_of(syscall(SYS_tee, pf[0], pb[1], (size_t) 4, 0));
            ck("tee either works or is ENOSYS", t == 4 || t == -ENOSYS, 1);

            static char klog_text[64 * 1024];
            kmsg_slurp(klog, klog_text, sizeof klog_text);
            ck("vmsplice is not logged as a stub",
               log_has(klog_text, "stub", (long) SYS_vmsplice), 0);
            ck("  nor as missing",
               log_has(klog_text, "missing", (long) SYS_vmsplice), 0);
            // tee's ENOSYS is sys_tee's own documented decision -- AOK pipes are
            // host pipes, which cannot be read without consuming -- and the
            // i386 and amd64 tables name sys_tee for exactly that reason. An
            // implemented refusal is not an absent table entry.
            ck("tee is not logged as a stub either",
               log_has(klog_text, "stub", (long) SYS_tee), 0);
            ck("  nor as missing",
               log_has(klog_text, "missing", (long) SYS_tee), 0);

            close(pf[0]);
            close(pf[1]);
            close(pb[0]);
            close(pb[1]);
            close(klog);
        }
    }

    unlink(src_path);
    unlink(dst_path);
    return finish_suite("splice_vmsplice");
}
