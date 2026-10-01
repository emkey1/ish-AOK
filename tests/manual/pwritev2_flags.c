// pwritev2(2) honours its flags.
//
// musl 1.2.6 (Alpine 3.24) issues every pwrite() as pwritev2(..., RWF_NOAPPEND)
// so that, as POSIX asks, it writes at the given offset even on an O_APPEND
// descriptor -- Linux's plain pwrite() appends there. AOK took `flags` as
// unused, so what such a pwrite() did came down to the host write path. Raw
// system calls throughout, so the libc in front cannot change the answer:
//   - flags 0 on O_APPEND: appends, ignoring the offset (Linux's pwrite rule)
//   - RWF_NOAPPEND on O_APPEND: writes at the offset
//   - offset -1: the file position, which advances (writev behaviour)
//   - RWF_APPEND on a plain descriptor: appends this one write
//   - RWF_DSYNC / RWF_SYNC: accepted
//   - an unknown flag: EOPNOTSUPP, the answer libcs fall back on
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>
#include "test_common.h"

#ifndef RWF_HIPRI
#define RWF_HIPRI    0x01
#define RWF_DSYNC    0x02
#define RWF_SYNC     0x04
#define RWF_NOWAIT   0x08
#define RWF_APPEND   0x10
#endif
#ifndef RWF_NOAPPEND
#define RWF_NOAPPEND 0x20
#endif

static int fails;
#define CHECK(cond, ...) do { if (cond) { test_logf("ok: " __VA_ARGS__); test_logf("\n"); } \
    else { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

// The raw call splits the offset into two longs on every ABI.
static long pwritev2_raw(int fd, const char *s, int64_t off, int flags) {
    struct iovec iov = { (void *) s, strlen(s) };
    unsigned long lo = (unsigned long) off;
    unsigned long hi = sizeof(long) == 4 ? (unsigned long) ((uint64_t) off >> 32) : 0;
    return syscall(SYS_pwritev2, fd, &iov, 1, lo, hi, flags);
}

static void contents(const char *path, char *buf, size_t cap) {
    int fd = open(path, O_RDONLY);
    ssize_t n = fd >= 0 ? read(fd, buf, cap - 1) : -1;
    buf[n > 0 ? n : 0] = '\0';
    if (fd >= 0)
        close(fd);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    char path[] = "/tmp/pwv2XXXXXX", buf[64];
    int seed = mkstemp(path);
    if (seed < 0 || write(seed, "0123456789", 10) != 10) {
        perror("setup");
        return 2;
    }
    close(seed);

    int fd = open(path, O_WRONLY | O_APPEND);
    long r = pwritev2_raw(fd, "ab", 0, 0);
    contents(path, buf, sizeof buf);
    CHECK(r == 2 && strcmp(buf, "0123456789ab") == 0,
          "flags 0 on O_APPEND appends (rc %ld, file \"%s\")", r, buf);

    r = pwritev2_raw(fd, "XY", 2, RWF_NOAPPEND);
    contents(path, buf, sizeof buf);
    if (r < 0 && errno == EOPNOTSUPP) {
        printf("FAIL: RWF_NOAPPEND refused with EOPNOTSUPP\n");
        fails++;
    } else {
        CHECK(r == 2 && strcmp(buf, "01XY456789ab") == 0,
              "RWF_NOAPPEND writes at the offset on O_APPEND (rc %ld, file \"%s\")", r, buf);
    }
    close(fd);

    fd = open(path, O_WRONLY);
    lseek(fd, 4, SEEK_SET);
    r = pwritev2_raw(fd, "pq", -1, 0);
    off_t pos = lseek(fd, 0, SEEK_CUR);
    contents(path, buf, sizeof buf);
    CHECK(r == 2 && pos == 6 && strncmp(buf, "01XYpq", 6) == 0,
          "offset -1 writes at the position and advances it (rc %ld, pos %lld, file \"%s\")",
          r, (long long) pos, buf);

    r = pwritev2_raw(fd, "Z", 0, RWF_APPEND);
    contents(path, buf, sizeof buf);
    CHECK(r == 1 && strcmp(buf, "01XYpq6789abZ") == 0 && buf[0] == '0',
          "RWF_APPEND on a plain descriptor appends (rc %ld, file \"%s\")", r, buf);

    r = pwritev2_raw(fd, "0", 0, RWF_DSYNC);
    CHECK(r == 1, "RWF_DSYNC accepted (rc %ld errno %d)", r, r < 0 ? errno : 0);
    r = pwritev2_raw(fd, "0", 0, RWF_SYNC);
    CHECK(r == 1, "RWF_SYNC accepted (rc %ld errno %d)", r, r < 0 ? errno : 0);

    errno = 0;
    r = pwritev2_raw(fd, "0", 0, 0x40000000);
    CHECK(r == -1 && errno == EOPNOTSUPP, "an unknown flag is EOPNOTSUPP (rc %ld errno %d)", r, errno);
    close(fd);
    unlink(path);

    printf("pwritev2_flags: %s\n", fails ? "FAIL" : "PASS");
    return fails != 0;
}
