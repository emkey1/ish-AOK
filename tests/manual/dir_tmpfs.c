// Directory records, a faulting read, and two things tmpfs could not do.
//
//   Every getdents record is padded so the NEXT one starts aligned: Linux
//   rounds getdents64 to 8 and legacy getdents to sizeof(long). It matters
//   because the documented way to walk the buffer is to cast each record in
//   place and step by d_reclen. Unpadded records left every record after the
//   first at an address struct linux_dirent64 is not aligned for -- an
//   unaligned load on a strict-alignment target, and everywhere else a size
//   no reader computing lengths for itself would predict.
//
//   getdents64 into a buffer that faults partway through returned EFAULT and
//   threw away everything it had already copied, having ALREADY advanced the
//   directory past those entries. They were gone for good. Linux returns a
//   short but valid byte count and leaves the position exactly after the last
//   stored entry, so a follow-up call returns the rest.
//
//   tmpfs could not make a hard link -- link() returned EPERM, the errno for
//   "this filesystem cannot do links at all" -- although a tmpfs inode is
//   already refcounted and named by a separate dirent. Anything that creates
//   a temporary file and links it into place (dpkg, rename-by-link, maildir
//   delivery, GNU ln) failed on one.
//
//   tmpfs never touched atime, so a reader could not tell a file that had
//   just been read from one nobody had opened since boot -- the one thing
//   atime is for.
//
// Measured against x86_64 glibc on Linux 6.12.
#define _GNU_SOURCE
#include <dirent.h>
#include <stddef.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "test_common.h"

struct linux_dirent64_t {
    unsigned long long d_ino;
    long long d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[];
};

#define PS 4096

static char base[80];

static void ck(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-58s got=%-10ld want=%ld\n", label, got, want);
}

static void p(char *o, size_t n, const char *r) { snprintf(o, n, "%s/%s", base, r); }

static int make_entries(const char *dir, int count, const char *prefix) {
    if (mkdir(dir, 0755) != 0)
        return -1;
    for (int i = 0; i < count; i++) {
        char f[256];
        snprintf(f, sizeof f, "%s/%s%03d", dir, prefix, i);
        int t = open(f, O_WRONLY | O_CREAT, 0644);
        if (t < 0)
            return -1;
        close(t);
    }
    return 0;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(120));
    snprintf(base, sizeof base, "/tmp/dirtmp-%d", (int) getpid());
    char cmd[160];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", base);
    if (system(cmd) < 0)
        failures_total++;
    ck("mkdir base", mkdir(base, 0755), 0);

    // ---- d_reclen alignment ----------------------------------------------
    {
        char d[128];
        p(d, sizeof d, "rl");
        ck("stage a directory with varied name lengths", mkdir(d, 0755), 0);
        const char *names[] = { "a", "abcd", "abcdefghij", "a-twenty-char-name-x" };
        for (int i = 0; i < 4; i++) {
            char f[224];
            snprintf(f, sizeof f, "%s/%s", d, names[i]);
            int t = open(f, O_WRONLY | O_CREAT, 0644);
            if (t >= 0)
                close(t);
        }
        int fd = open(d, O_RDONLY | O_DIRECTORY);
        ck("open it", fd >= 0 ? 1 : 0, 1);
        if (fd >= 0) {
            char buf[4096];
            long n = syscall(SYS_getdents64, fd, buf, sizeof buf);
            ck("getdents64 returns entries", n > 0 ? 1 : 0, 1);
            int records = 0, misaligned = 0, offset_misaligned = 0;
            for (long off = 0; off < n; ) {
                struct linux_dirent64_t *e = (struct linux_dirent64_t *) (buf + off);
                if (e->d_reclen == 0)
                    break;
                records++;
                if (e->d_reclen % 8 != 0)
                    misaligned++;
                if (off % 8 != 0)
                    offset_misaligned++;
                // The name must still be there and NUL-terminated inside the
                // record, so the padding did not eat it.
                if (strlen(e->d_name) + 1 > e->d_reclen - offsetof(struct linux_dirent64_t, d_name))
                    misaligned++;
                off += e->d_reclen;
            }
            ck("  six records (4 files + . + ..)", records, 6);
            ck("  every d_reclen is a multiple of 8", misaligned, 0);
            ck("  so every record starts aligned", offset_misaligned, 0);
            close(fd);
        }
    }

    // ---- a fault partway through keeps what was stored --------------------
    {
        char d[128];
        p(d, sizeof d, "fb");
        ck("stage a directory with 40 entries", make_entries(d, 40, "e"), 0);
        // Two pages with the second unwritable, and a buffer straddling them.
        char *region = mmap(NULL, 2 * PS, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        ck("map a two-page region", region == MAP_FAILED ? 0 : 1, 1);
        if (region != MAP_FAILED) {
            ck("  make the second page unwritable", mprotect(region + PS, PS, PROT_NONE), 0);
            int fd = open(d, O_RDONLY | O_DIRECTORY);
            ck("  open the directory", fd >= 0 ? 1 : 0, 1);
            if (fd >= 0) {
                errno = 0;
                long n = syscall(SYS_getdents64, fd, region + PS - 200, PS);
                // A short count, not EFAULT: some entries fitted in the
                // writable tail before the fault.
                ck("  getdents64 across the boundary is short, not EFAULT",
                   n > 0 ? 1 : 0, 1);
                if (n > 0) {
                    ck("  and it did not exceed the writable part", n <= 200 ? 1 : 0, 1);
                    // The rest is still there: the position was left after the
                    // last STORED entry, not after the ones thrown away.
                    long n2 = syscall(SYS_getdents64, fd, region, 2048);
                    ck("  the remaining entries follow", n2 > 0 ? 1 : 0, 1);
                    // Count everything and confirm nothing vanished.
                    long total = 0;
                    for (long off = 0; off < n; ) {
                        struct linux_dirent64_t *e =
                            (struct linux_dirent64_t *) (region + PS - 200 + off);
                        if (e->d_reclen == 0) break;
                        total++;
                        off += e->d_reclen;
                    }
                    for (long off = 0; off < n2; ) {
                        struct linux_dirent64_t *e = (struct linux_dirent64_t *) (region + off);
                        if (e->d_reclen == 0) break;
                        total++;
                        off += e->d_reclen;
                    }
                    for (;;) {
                        long more = syscall(SYS_getdents64, fd, region, 2048);
                        if (more <= 0) break;
                        for (long off = 0; off < more; ) {
                            struct linux_dirent64_t *e = (struct linux_dirent64_t *) (region + off);
                            if (e->d_reclen == 0) break;
                            total++;
                            off += e->d_reclen;
                        }
                    }
                    ck("  and every entry arrived exactly once", total, 42);
                }
                close(fd);
            }
            munmap(region, 2 * PS);
        }
    }

    // ---- tmpfs --------------------------------------------------------------
    if (geteuid() != 0) {
        printf("dir_tmpfs: NOTE not root; skipping the tmpfs section\n");
    } else {
        char m[128];
        p(m, sizeof m, "tm");
        ck("mkdir a mount point", mkdir(m, 0755), 0);
        errno = 0;
        if (mount("none", m, "tmpfs", 0, NULL) != 0) {
            printf("dir_tmpfs: NOTE tmpfs mount refused (%s); skipping\n", strerror(errno));
        } else {
            char a[192], b[192];
            snprintf(a, sizeof a, "%s/a", m);
            snprintf(b, sizeof b, "%s/b", m);
            int fd = open(a, O_WRONLY | O_CREAT, 0644);
            ck("create a file on the tmpfs", fd >= 0 ? 1 : 0, 1);
            if (fd >= 0) {
                ck("  write to it", (long) write(fd, "hello", 5), 5);
                close(fd);
            }
            errno = 0;
            ck("hard link it", link(a, b), 0);
            struct stat sa, sb;
            ck("  stat the original", stat(a, &sa), 0);
            ck("  st_nlink is 2", (long) sa.st_nlink, 2);
            ck("  stat the link", stat(b, &sb), 0);
            ck("  same inode", sa.st_ino == sb.st_ino ? 1 : 0, 1);
            // ...and the content really is shared.
            fd = open(b, O_WRONLY | O_APPEND);
            if (fd >= 0) {
                ssize_t w = write(fd, "!", 1);
                (void) w;
                close(fd);
            }
            ck("  writing through the link grows the original",
               stat(a, &sa) == 0 ? (long) sa.st_size : -1, 6);
            // Removing one name leaves the other.
            ck("unlink one name", unlink(b), 0);
            ck("  the other survives", stat(a, &sa), 0);
            ck("  with st_nlink back to 1", (long) sa.st_nlink, 1);
            // Linking a DIRECTORY is EPERM everywhere.
            char dir[192], dl[192];
            snprintf(dir, sizeof dir, "%s/d", m);
            snprintf(dl, sizeof dl, "%s/dl", m);
            ck("mkdir on the tmpfs", mkdir(dir, 0755), 0);
            errno = 0;
            ck("  linking a directory is EPERM", link(dir, dl) < 0 ? errno : 0, EPERM);
            // ...and an existing target is EEXIST.
            errno = 0;
            ck("  linking onto an existing name is EEXIST", link(a, a) < 0 ? errno : 0, EEXIST);

            // atime advances on read, under relatime.
            struct stat before, after;
            ck("stat before reading", stat(a, &before), 0);
            sleep(2);
            fd = open(a, O_RDONLY);
            if (fd >= 0) {
                char c[8];
                ssize_t r = read(fd, c, sizeof c);
                (void) r;
                close(fd);
            }
            ck("stat after reading", stat(a, &after), 0);
            ck("  atime advanced", after.st_atime > before.st_atime ? 1 : 0, 1);
            ck("  and mtime did not", (long) after.st_mtime, (long) before.st_mtime);

            umount2(m, 2 /* MNT_DETACH */);
        }
    }

    snprintf(cmd, sizeof cmd, "rm -rf '%s'", base);
    if (system(cmd) < 0)
        failures_total++;
    return finish_suite("dir_tmpfs");
}
