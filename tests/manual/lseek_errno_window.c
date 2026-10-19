// A 64-bit result whose low word looks like -errno is still a result.
//
// kernel/calls.c's amd64 result write (which ppc64 shares) took any value
// with the upper 32 bits zero and a low word in 0xfffff001-0xffffffff for a
// 32-bit errno and sign-extended it, so an lseek landing up to 4095 bytes
// below 4 GiB came back as -1 with errno set to whatever the low word spelt
// (0xfffff800 is -2048: an errno no Linux defines). The rule was there for
// handlers that return a dword; those now widen their own errno.
//
// Every guest should give the offset back. The controls: an offset just
// outside the window (0xfffff000), one above 4 GiB, and a real 64-bit errno
// (a negative offset, EINVAL), so a pass is not a wrapper that never fails.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "test_common.h"

static void check(int cond, const char *what) {
    if (cond) {
        test_logf("ok: %s\n", what);
    } else {
        printf("FAIL: %s (errno=%d %s)\n", what, errno, strerror(errno));
        failures_total++;
    }
}

static void check_seek(int fd, off_t off, int whence, off_t want, const char *what) {
    errno = 0;
    off_t got = lseek(fd, off, whence);
    char label[160];
    snprintf(label, sizeof(label), "%s: lseek gives %#llx (got %#llx)", what,
             (unsigned long long) want, (unsigned long long) got);
    check(got == want, label);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));

    char path[] = "/tmp/lseek_errno_window.XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        check(0, "mkstemp");
        return finish_suite("lseek_errno_window");
    }
    unlink(path);

    // Controls first: outside the window, and above 4 GiB.
    check_seek(fd, (off_t) 0xfffff000, SEEK_SET, (off_t) 0xfffff000, "SEEK_SET just below the window");
    check_seek(fd, (off_t) 0x100000800LL, SEEK_SET, (off_t) 0x100000800LL, "SEEK_SET above 4 GiB");

    // Inside the window: 0xfffff001 is -4095, 0xfffff800 is -2048, 0xffffffff
    // is -1 (EPERM).
    check_seek(fd, (off_t) 0xfffff001, SEEK_SET, (off_t) 0xfffff001, "SEEK_SET to 4 GiB - 4095");
    check_seek(fd, (off_t) 0xfffff800, SEEK_SET, (off_t) 0xfffff800, "SEEK_SET to 4 GiB - 2048");
    check_seek(fd, (off_t) 0xffffffff, SEEK_SET, (off_t) 0xffffffff, "SEEK_SET to 4 GiB - 1");

    // The same offsets reached by SEEK_CUR and SEEK_END, which take a
    // different path through the handler.
    check_seek(fd, 0, SEEK_SET, 0, "SEEK_SET back to 0");
    check_seek(fd, (off_t) 0xfffffff0, SEEK_CUR, (off_t) 0xfffffff0, "SEEK_CUR into the window");
    if (ftruncate(fd, (off_t) 0xfffffffeLL) != 0) {
        test_logf("skip: ftruncate to 4 GiB - 2 (%s)\n", strerror(errno));
    } else {
        check_seek(fd, 0, SEEK_END, (off_t) 0xfffffffe, "SEEK_END of a 4 GiB - 2 file");
        check_seek(fd, -0x7ff, SEEK_END, (off_t) 0xfffff7ff, "SEEK_END - 0x7ff");
    }

    // A real error still reads as one.
    errno = 0;
    off_t bad = lseek(fd, -1, SEEK_SET);
    check(bad == -1 && errno == EINVAL, "a negative offset is EINVAL");

    close(fd);
    return finish_suite("lseek_errno_window");
}
