// amd64 sendfile real copy (+ no SIGSYS on 64-bit count), and vhangup wired.
//
// sendfile (#40 amd64 / #187 i386) was a stub that first SIGSYS'd on amd64
// because its 64-bit size_t count tripped the legacy arg marshaller. It is now
// implemented natively (shared engine with copy_file_range) and actually copies
// bytes; this is systemd's copy fallback path. vhangup (#153 amd64 / #111 i386)
// had no table entry ("missing syscall"); it is now a success no-op stub that
// login uses to reset the controlling tty.
//
// Verifies sendfile with a NULL offset copies the data and advances positions
// (offset semantics are covered by copy_file_range.c's shared engine), that a
// file-to-pipe copy larger than the pipe capacity arrives complete and intact
// (the engine once dropped the read-but-unwritten tail of its bounce buffer on
// a short pipe write, so busybox cat/tar truncated any >64K pipe copy), that
// the same copy to a SOCKET comes back short and loses nothing across the loop
// (the engine also once drove its loop to the caller's full count, which is a
// deadlock the moment either end can block), and that vhangup returns cleanly.
// Arch-neutral.
#define _GNU_SOURCE
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include "test_common.h"

#ifndef SYS_sendfile
# ifdef __NR_sendfile
#  define SYS_sendfile __NR_sendfile
# elif defined(__x86_64__)
#  define SYS_sendfile 40
# else
#  define SYS_sendfile 187
# endif
#endif
#ifndef SYS_vhangup
# ifdef __NR_vhangup
#  define SYS_vhangup __NR_vhangup
# elif defined(__x86_64__)
#  define SYS_vhangup 153
# else
#  define SYS_vhangup 111
# endif
#endif

static const char MSG[] = "SENDFILE MOVES THESE BYTES";

static void test_sendfile(void) {
    size_t mlen = strlen(MSG);
    char buf[128];
    int s = open("/tmp/sf.src", O_RDWR | O_CREAT | O_TRUNC, 0600);
    int d = open("/tmp/sf.dst", O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (s < 0 || d < 0) { printf("FAIL: open: %s\n", strerror(errno)); failures_total++; goto out; }
    if (write(s, MSG, mlen) != (ssize_t) mlen) { printf("FAIL: write src\n"); failures_total++; goto out; }
    lseek(s, 0, SEEK_SET);

    errno = 0;
    // NULL offset: read from src's current position, write to dst, advance both.
    long r = syscall(SYS_sendfile, d, s, (void *) 0, (size_t) mlen);
    if (r != (long) mlen) {
        printf("FAIL: sendfile: r=%ld (%s), want %zu\n", r, r < 0 ? strerror(errno) : "", mlen);
        failures_total++; goto out;
    }
    ssize_t got = pread(d, buf, sizeof buf - 1, 0);
    if (got >= 0) buf[got] = '\0';
    if (got != (ssize_t) mlen || strcmp(buf, MSG) != 0) {
        printf("FAIL: sendfile dest content wrong: \"%s\"\n", buf); failures_total++;
    }
    if (lseek(s, 0, SEEK_CUR) != (off_t) mlen) { printf("FAIL: sendfile src pos not advanced\n"); failures_total++; }
    test_logf("sendfile copy ok (%ld bytes)\n", r);
out:
    if (s >= 0) close(s);
    if (d >= 0) close(d);
    unlink("/tmp/sf.src"); unlink("/tmp/sf.dst");
}

// File-to-pipe sendfile of much more than the pipe capacity, with the reader
// draining concurrently. Every byte must arrive, in order. Regression: the
// shared copy engine read a 64K chunk, wrote what the pipe would take, and
// dropped the rest -- the input position had already advanced past it -- so
// the caller's next sendfile() saw a false EOF and the stream truncated.
static void test_sendfile_to_pipe(void) {
    enum { SIZE = 300 * 1024 };
    const char *path = "/tmp/sf.pipe.src";
    int s = -1, pfd[2] = {-1, -1};
    pid_t child = -1;

    s = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (s < 0) { printf("FAIL: pipe-copy open: %s\n", strerror(errno)); failures_total++; goto out; }
    char block[4096];
    for (size_t off = 0; off < SIZE; off += sizeof block) {
        for (size_t i = 0; i < sizeof block; i++)
            block[i] = (char) ((off + i) * 31 >> 3);
        if (write(s, block, sizeof block) != (ssize_t) sizeof block) {
            printf("FAIL: pipe-copy fill: %s\n", strerror(errno)); failures_total++; goto out;
        }
    }
    lseek(s, 0, SEEK_SET);

    if (pipe(pfd) < 0) { printf("FAIL: pipe: %s\n", strerror(errno)); failures_total++; goto out; }
    child = fork();
    if (child < 0) { printf("FAIL: fork: %s\n", strerror(errno)); failures_total++; goto out; }
    if (child == 0) {
        // Reader: verify content and total independently of the writer.
        close(pfd[1]);
        size_t total = 0;
        char buf[8192];
        ssize_t n;
        while ((n = read(pfd[0], buf, sizeof buf)) > 0) {
            for (ssize_t i = 0; i < n; i++) {
                char want = (char) ((total + (size_t) i) * 31 >> 3);
                if (buf[i] != want) {
                    printf("FAIL: pipe-copy corrupt at byte %zu\n", total + (size_t) i);
                    _exit(1);
                }
            }
            total += (size_t) n;
        }
        _exit(total == SIZE ? 0 : (printf("FAIL: pipe-copy got %zu bytes, want %d\n", total, SIZE), 1));
    }
    close(pfd[0]); pfd[0] = -1;

    size_t sent = 0;
    while (sent < SIZE) {
        long r = syscall(SYS_sendfile, pfd[1], s, (void *) 0, (size_t) (SIZE - sent));
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) {
            printf("FAIL: pipe-copy sendfile: r=%ld (%s) after %zu bytes\n",
                   r, r < 0 ? strerror(errno) : "eof", sent);
            failures_total++; break;
        }
        sent += (size_t) r;
    }
    close(pfd[1]); pfd[1] = -1;

    int st;
    if (waitpid(child, &st, 0) != child || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        printf("FAIL: pipe-copy reader status %#x\n", st); failures_total++;
    } else if (sent == SIZE) {
        test_logf("sendfile to pipe ok (%zu bytes)\n", sent);
    }
    child = -1;
out:
    if (pfd[0] >= 0) close(pfd[0]);
    if (pfd[1] >= 0) close(pfd[1]);
    if (s >= 0) close(s);
    if (child > 0) waitpid(child, NULL, 0);
    unlink(path);
}

// The same copy with a SOCKET on the far end, which is what sendfile exists
// for. It matters separately from the pipe because the amount a socket will
// take in one go is its own business: the call comes back short, and a caller
// that believed one sendfile() moved everything it asked for would silently
// truncate. Linux returns short here too -- on a signal, on a non-blocking
// peer, or simply when the buffer is full -- so looping is the contract, and
// this pins that AOK's engine both returns short and loses nothing doing it.
static void test_sendfile_to_socket(void) {
    enum { SIZE = 300 * 1024 };
    const char *path = "/tmp/sf.sock.src";
    int s = -1, sv[2] = {-1, -1};
    pid_t child = -1;

    s = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (s < 0) { printf("FAIL: sock-copy open: %s\n", strerror(errno)); failures_total++; goto out; }
    char block[4096];
    for (size_t off = 0; off < SIZE; off += sizeof block) {
        for (size_t i = 0; i < sizeof block; i++)
            block[i] = (char) ((off + i) * 31 >> 3);
        if (write(s, block, sizeof block) != (ssize_t) sizeof block) {
            printf("FAIL: sock-copy fill: %s\n", strerror(errno)); failures_total++; goto out;
        }
    }
    lseek(s, 0, SEEK_SET);

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        printf("FAIL: socketpair: %s\n", strerror(errno)); failures_total++; goto out;
    }
    child = fork();
    if (child < 0) { printf("FAIL: fork: %s\n", strerror(errno)); failures_total++; goto out; }
    if (child == 0) {
        close(sv[1]);
        size_t total = 0;
        char buf[8192];
        ssize_t n;
        while ((n = read(sv[0], buf, sizeof buf)) > 0) {
            for (ssize_t i = 0; i < n; i++) {
                char want = (char) ((total + (size_t) i) * 31 >> 3);
                if (buf[i] != want) {
                    printf("FAIL: sock-copy corrupt at byte %zu\n", total + (size_t) i);
                    _exit(1);
                }
            }
            total += (size_t) n;
        }
        _exit(total == SIZE ? 0 : (printf("FAIL: sock-copy got %zu bytes, want %d\n", total, SIZE), 1));
    }
    close(sv[0]); sv[0] = -1;

    size_t sent = 0;
    while (sent < SIZE) {
        long r = syscall(SYS_sendfile, sv[1], s, (void *) 0, (size_t) (SIZE - sent));
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) {
            printf("FAIL: sock-copy sendfile: r=%ld (%s) after %zu bytes\n",
                   r, r < 0 ? strerror(errno) : "eof", sent);
            failures_total++; break;
        }
        sent += (size_t) r;
    }
    close(sv[1]); sv[1] = -1;

    int st;
    if (waitpid(child, &st, 0) != child || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        printf("FAIL: sock-copy reader status %#x\n", st); failures_total++;
    } else if (sent == SIZE) {
        test_logf("sendfile to socket ok (%zu bytes)\n", sent);
    }
    child = -1;
out:
    if (sv[0] >= 0) close(sv[0]);
    if (sv[1] >= 0) close(sv[1]);
    if (s >= 0) close(s);
    unlink(path);
}

static void test_vhangup(void) {
    errno = 0;
    long r = syscall(SYS_vhangup);
    if (r == 0)
        test_logf("vhangup -> 0 ok\n");
    else if (errno == EPERM)
        test_logf("vhangup -> -1 EPERM (acceptable)\n");
    else {
        printf("FAIL: vhangup -> %ld %s (want 0)\n", r, r < 0 ? strerror(errno) : "");
        failures_total++;
    }
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    test_sendfile();
    test_sendfile_to_pipe();
    test_sendfile_to_socket();
    test_vhangup();
    return finish_suite("sendfile_vhangup");
}
