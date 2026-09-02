// /proc/<pid>/fd/N "magic link" opens must honor the CALLER's open flags,
// like Linux: opening /proc/self/fd/N is a fresh open of the target file
// with the flags passed to open(2), not a clone of the original
// descriptor's mode. AOK's procfd_openat (fs/generic.c) reopened the target
// with the ORIGINAL descriptor's flags instead, so upgrading an O_PATH fd to
// O_RDWR via /proc/self/fd -- systemd's fd_reopen(), used by xopenat_full
// (path=NULL) -- silently returned a read-only description: F_GETFL showed
// accmode 0, write() failed EBADF, ftruncate() failed EINVAL.
//
// Real-world casualty: systemd-machine-id-setup on a fresh Arch rootfs
// (whose /etc/machine-id ships as an empty 0444 file). chase() opens it
// O_PATH, fd_reopen upgrades to O_RDWR, then ftruncate(fd, 0) got EINVAL ->
// "Failed to truncate /etc/machine-id" -> no machine id -> dbus-broker
// exits 1 at startup, systemd restarts it every ~90s (each cycle pinning
// PID 1 inside bus_get_instance_id's synchronous Hello wait), and the boot
// froze at "Starting D-Bus System Message Bus..." with logins dead behind
// pam_systemd. First boot of any fresh Arch aarch64 install on device hit
// this; older test images had a committed machine-id and hid it.
//
// Also covered: fresh file position on procfd reopen (script-loader
// behavior), O_NOFOLLOW -> ELOOP on the magic link, O_TRUNC honored, and
// the deleted-file fallback (same accmode) still works. Deviation not
// asserted: reopening a DELETED file with a WIDER accmode than the original
// fd succeeds on Linux (inode reopen) but fails EACCES under AOK (no stable
// path to reopen; a loud error beats a silently read-only fd).
//
// Also passes on real Linux (mint oracle, verified).
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "test_common.h"

static void check(int cond, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (!cond) {
        printf("FAIL ");
        vprintf(fmt, ap);
        printf("\n");
        failures_total++;
    } else if (test_verbose) {
        printf("ok ");
        vprintf(fmt, ap);
        printf("\n");
    }
    va_end(ap);
}

static int procfd_open(int fd, int flags) {
    char buf[64];
    snprintf(buf, sizeof(buf), "/proc/self/fd/%d", fd);
    return open(buf, flags);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(30));
    const char *path = "/tmp/procfd_reopen_test";

    // 1. the machine-id-setup sequence: 0444 empty file, O_PATH open,
    // reopen O_RDWR via /proc/self/fd, ftruncate + write. Upgrading a 0444
    // file to O_RDWR needs CAP_DAC_OVERRIDE (the magic-link open re-checks
    // permissions against the caller), so use 0644 when unprivileged (the
    // mint oracle runs this as a plain user; the guest suite runs as root).
    mode_t mode = geteuid() == 0 ? 0444 : 0644;
    unlink(path);
    int fd = open(path, O_CREAT | O_EXCL | O_RDWR, mode);
    check(fd >= 0, "create %o (%s)", mode, strerror(errno));
    close(fd);
    int pfd = open(path, O_PATH | O_NOFOLLOW | O_CLOEXEC);
    check(pfd >= 0, "O_PATH open (%s)", strerror(errno));
    int rw = procfd_open(pfd, O_RDWR | O_CLOEXEC);
    check(rw >= 0, "procfd reopen O_RDWR (%s)", strerror(errno));
    int fl = fcntl(rw, F_GETFL);
    check((fl & O_ACCMODE) == O_RDWR, "reopened accmode is O_RDWR (%#x)", fl);
    check(ftruncate(rw, 0) == 0, "ftruncate(reopened, 0) (%s)", strerror(errno));
    check(write(rw, "0123456789abcdef0123456789abcdef\n", 33) == 33,
          "write(reopened) (%s)", strerror(errno));
    close(rw);
    close(pfd);

    // 2. fresh file position: original fd read to EOF, procfd reopen reads
    // from the start (shell script loader behavior)
    int rd = open(path, O_RDONLY);
    char buf[64];
    check(read(rd, buf, sizeof(buf)) == 33, "prime original position");
    int rd2 = procfd_open(rd, O_RDONLY);
    check(rd2 >= 0, "procfd reopen O_RDONLY (%s)", strerror(errno));
    ssize_t n = read(rd2, buf, sizeof(buf));
    check(n == 33, "reopened fd reads from position 0 (n=%zd)", n);
    close(rd2);

    // 3. O_NOFOLLOW on the magic link itself -> ELOOP (it is a symlink)
    errno = 0;
    int nf = procfd_open(rd, O_RDONLY | O_NOFOLLOW);
    check(nf < 0 && errno == ELOOP, "O_NOFOLLOW magic link -> ELOOP (ret=%d errno=%d)", nf, errno);
    if (nf >= 0) close(nf);

    // 4. O_TRUNC honored on reopen
    int tr = procfd_open(rd, O_RDWR | O_TRUNC);
    check(tr >= 0, "procfd reopen O_RDWR|O_TRUNC (%s)", strerror(errno));
    struct stat st;
    check(fstat(tr, &st) == 0 && st.st_size == 0, "O_TRUNC truncated (size=%lld)",
          (long long) st.st_size);
    close(tr);
    close(rd);

    // 5. deleted-file fallback: unlinked file, same-accmode reopen still
    // reads the content
    fd = open(path, O_RDWR);
    check(write(fd, "gone", 4) == 4, "write before unlink");
    unlink(path);
    int del = procfd_open(fd, O_RDONLY);
    check(del >= 0, "deleted-file procfd reopen O_RDONLY (%s)", strerror(errno));
    if (del >= 0) {
        n = pread(del, buf, sizeof(buf), 0);
        if (n < 0) // fallback fd may not support pread the same way; use read
            n = read(del, buf, sizeof(buf));
        check(n == 4 && memcmp(buf, "gone", 4) == 0,
              "deleted-file reopen reads content (n=%zd)", n);
        close(del);
    }
    close(fd);

    unlink(path);
    return finish_suite("procfd_reopen");
}
