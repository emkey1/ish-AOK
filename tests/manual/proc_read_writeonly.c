// proc_read_writeonly.c -- reading binfmt_misc's write-only `register` is EINVAL, not an app abort
//
// /proc/sys/fs/binfmt_misc/register takes writes and nothing else. Linux gives
// it a write op and no read op: root's O_RDONLY open succeeds (its 0200 does
// not stop root), and read(2) is EINVAL -- for a zero-byte read too, since
// vfs_read refuses a file it cannot read before it looks at the count.
// iSH-AOK rendered it with no show() at all, left the read buffer unallocated
// and hit assert(data != NULL) in proc_pread: the whole app aborted. That
// assert is build 557's Organizer crash on iPhone14,5, and this entry is the
// only regular procfs file with neither show nor pread, so the only way in.
//
// The control is `status` beside it, which must read "enabled". Unprivileged,
// the open is EACCES, as on Linux. binfmt_misc is mounted for the run if it was
// not already, and unmounted after.
//
// Oracle: Linux's fs/binfmt_misc.c (bm_register_operations: .write and
// .llseek only) and fs/read_write.c (vfs_read's FMODE_CAN_READ check).
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <unistd.h>

#include "test_common.h"

#define MISC "/proc/sys/fs/binfmt_misc"

static void ck(bool ok, const char *what) {
    test_log_if(!ok, "%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        failures_total++;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    TEST_SKIP_IF_FOREIGN_PROC("proc_read_writeonly");

    bool mounted_here = false;
    if (access(MISC "/register", F_OK) != 0) {
        if (geteuid() != 0 || mount("binfmt_misc", MISC, "binfmt_misc", 0, NULL) != 0) {
            printf("proc_read_writeonly: SKIP (binfmt_misc not mounted, and cannot mount it: %s)\n",
                   geteuid() != 0 ? "not root" : strerror(errno));
            return 0;
        }
        mounted_here = true;
    }

    char buf[64];
    int fd = open(MISC "/status", O_RDONLY);
    ssize_t n = fd >= 0 ? read(fd, buf, sizeof buf - 1) : -1;
    if (fd >= 0)
        close(fd);
    ck(n > 0 && strncmp(buf, "enabled", 7) == 0, "control: status reads \"enabled\"");

    fd = open(MISC "/register", O_RDONLY);
    if (geteuid() != 0) {
        ck(fd < 0 && errno == EACCES, "unprivileged open of register is EACCES");
    } else {
        ck(fd >= 0, "root opens register O_RDONLY");
        if (fd >= 0) {
            errno = 0;
            n = read(fd, buf, sizeof buf);
            ck(n == -1 && errno == EINVAL, "read of register is EINVAL");
            test_logf("  read returned %zd errno %d\n", n, errno);
            errno = 0;
            n = read(fd, buf, 0);
            ck(n == -1 && errno == EINVAL, "zero-byte read of register is EINVAL");
            errno = 0;
            n = pread(fd, buf, sizeof buf, 0);
            ck(n == -1 && errno == EINVAL, "pread of register is EINVAL");
            close(fd);
        }
    }
    if (fd >= 0 && geteuid() != 0)
        close(fd);

    if (mounted_here)
        umount2(MISC, 0);
    return finish_suite("proc_read_writeonly");
}
