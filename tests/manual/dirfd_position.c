// A directory's position is a cookie, and lseek has to report it.
//
//   The position of an open directory is whatever the last getdents said in
//   d_off. It is opaque -- on ext4 it is a hash, and the value here is a
//   host telldir cookie -- and the only thing it is good for is being handed
//   back through SEEK_SET. That round trip is how a directory walk is
//   resumed, and it is what seekdir(3) and telldir(3) are built on.
//
//   lseek(dirfd, 0, SEEK_CUR) fell through to the host lseek on the
//   underlying descriptor, which knows nothing about the directory stream and
//   answered with a byte offset (INT32_MAX in practice). A caller that saved
//   its place and restored it later got a number that meant nothing, and
//   restoring it moved the stream somewhere unrelated.
//
// Measured against x86_64 glibc on Linux 6.12 (ext4).
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "test_common.h"

struct ld64 {
    unsigned long long d_ino;
    long long d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[];
};

#define ENTRIES 12

static char base[80];

static void ck(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-58s got=%-10ld want=%ld\n", label, got, want);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));
    snprintf(base, sizeof base, "/tmp/dirpos-%d", (int) getpid());
    char cmd[160];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", base);
    if (system(cmd) < 0)
        failures_total++;
    ck("mkdir base", mkdir(base, 0755), 0);
    for (int i = 0; i < ENTRIES; i++) {
        char f[160];
        snprintf(f, sizeof f, "%s/f%02d", base, i);
        int t = open(f, O_WRONLY | O_CREAT, 0644);
        if (t >= 0)
            close(t);
    }

    int fd = open(base, O_RDONLY | O_DIRECTORY);
    ck("open the directory", fd >= 0 ? 1 : 0, 1);
    if (fd < 0)
        return finish_suite("dirfd_position");

    // A partial read, so there is a real position in the middle to report.
    char buf[512];
    long n = syscall(SYS_getdents64, fd, buf, 200);
    ck("a partial getdents64 returns entries", n > 0 ? 1 : 0, 1);
    long long last_off = -1;
    int first_batch = 0;
    for (long off = 0; off < n; ) {
        struct ld64 *e = (struct ld64 *) (buf + off);
        if (e->d_reclen == 0)
            break;
        last_off = e->d_off;
        first_batch++;
        off += e->d_reclen;
    }
    ck("  more than one, but not all of them",
       first_batch > 0 && first_batch < ENTRIES + 2 ? 1 : 0, 1);

    // The cookie the kernel reports has to be the one it just handed out.
    off_t cur = lseek(fd, 0, SEEK_CUR);
    ck("lseek(dirfd, 0, SEEK_CUR) is the last d_off", (long) cur, (long) last_off);
    // ...and specifically not a byte offset, which is what it used to be.
    ck("  and not a byte count", (long) cur == 200 ? 1 : 0, 0);

    // Feeding it back must resume exactly where the read stopped: every
    // remaining entry, none repeated.
    ck("seek back to it", (long) lseek(fd, cur, SEEK_SET), (long) cur);
    int rest = 0;
    for (;;) {
        long m = syscall(SYS_getdents64, fd, buf, sizeof buf);
        if (m <= 0)
            break;
        for (long off = 0; off < m; ) {
            struct ld64 *e = (struct ld64 *) (buf + off);
            if (e->d_reclen == 0)
                break;
            rest++;
            off += e->d_reclen;
        }
    }
    ck("the rest of the directory follows", first_batch + rest, ENTRIES + 2);

    // Rewinding to 0 restarts it, which is the other half of the contract.
    ck("seek to 0", (long) lseek(fd, 0, SEEK_SET), 0);
    int again = 0;
    for (;;) {
        long m = syscall(SYS_getdents64, fd, buf, sizeof buf);
        if (m <= 0)
            break;
        for (long off = 0; off < m; ) {
            struct ld64 *e = (struct ld64 *) (buf + off);
            if (e->d_reclen == 0)
                break;
            again++;
            off += e->d_reclen;
        }
    }
    ck("  and the whole directory reads again", again, ENTRIES + 2);
    // At the end, the reported position is the last entry's cookie and a
    // further read returns nothing.
    ck("a read at the end returns 0",
       (long) syscall(SYS_getdents64, fd, buf, sizeof buf), 0);

    close(fd);
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", base);
    if (system(cmd) < 0)
        failures_total++;
    return finish_suite("dirfd_position");
}
