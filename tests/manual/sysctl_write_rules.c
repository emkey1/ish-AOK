// What /proc/sys accepts, what it refuses, and what it admits to not having.
//
//   Writes to a scalar knob were never validated. strtol's return was stored
//   whatever it had parsed, so "notanumber" became 0 and "-5" was kept
//   verbatim, and the write reported the full byte count either way. A program
//   that sets a knob and reads it back to confirm got confirmation of a value
//   the kernel had invented -- which is worse than the write simply failing.
//
//   kernel/pid_max, kernel/threads-max, fs/file-max and fs/nr_open advertised
//   mode 0644 and then refused every write with EPERM, even for root, because
//   they had no update handler at all. `sysctl -p` aborts at the first failing
//   key, so one unwritable knob in /etc/sysctl.conf silently dropped every
//   setting after it.
//
//   /proc/sys/fs/binfmt_misc presented `register` and `status` with nothing
//   behind them: status read "enabled" always, a register write returned the
//   byte count and was discarded, and writing 0 to status reported success and
//   changed nothing. update-binfmts and systemd-binfmt both believe that, so a
//   guest configured to run foreign binaries looked configured and ran
//   nothing. Linux shows an EMPTY directory until the binfmt_misc filesystem
//   is mounted on it, which is the truthful state and the one those tools
//   already handle.
//
// Measured against x86_64 glibc on Linux 6.12. The test restores every value
// it changes: it runs as root on a real machine too.
#define _GNU_SOURCE
#include <errno.h>
#include <stdbool.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "test_common.h"

static void ck(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-56s got=%-8ld want=%ld\n", label, got, want);
}

// Returns the write's result: the byte count, or -errno.
static long sysctl_write(const char *path, const char *value) {
    int fd = open(path, O_WRONLY | O_TRUNC);
    if (fd < 0)
        return -errno;
    errno = 0;
    ssize_t n = write(fd, value, strlen(value));
    int e = errno;
    close(fd);
    return n < 0 ? -e : n;
}

static long sysctl_read(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -errno;
    char buf[64] = { 0 };
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0)
        return -EIO;
    return strtol(buf, NULL, 10);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));

    if (geteuid() != 0) {
        printf("sysctl_write_rules: SKIP (needs root to write /proc/sys)\n");
        return 0;
    }

    // ---- a scalar knob validates what it is given -------------------------
    {
        static const char *knob = "/proc/sys/net/core/somaxconn";
        long original = sysctl_read(knob);
        ck("somaxconn reads as a number", original > 0, 1);

        ck("a non-numeric write is EINVAL", sysctl_write(knob, "notanumber"), -EINVAL);
        ck("  and the value is unchanged", sysctl_read(knob), original);
        ck("a negative write is EINVAL", sysctl_write(knob, "-5"), -EINVAL);
        ck("  and the value is unchanged", sysctl_read(knob), original);
        // Trailing garbage after a valid number is not a valid number.
        ck("a trailing-garbage write is EINVAL", sysctl_write(knob, "12abc"), -EINVAL);
        ck("  and the value is unchanged", sysctl_read(knob), original);
        // A zero-length write is write(2)'s business, not the sysctl's: it
        // writes nothing and returns 0 without the handler ever being asked.
        ck("a zero-length write is 0, not an error", sysctl_write(knob, ""), 0);
        ck("  and the value is unchanged", sysctl_read(knob), original);

        // ...and a legal one goes through, including with the trailing newline
        // `echo` adds.
        ck("a legal write is accepted", sysctl_write(knob, "4096\n") > 0, 1);
        ck("  and takes effect", sysctl_read(knob), 4096);

        char restore[32];
        snprintf(restore, sizeof restore, "%ld\n", original);
        ck("restore the original", sysctl_write(knob, restore) > 0, 1);
        ck("  restored", sysctl_read(knob), original);
    }

    // ---- a knob that says 0644 accepts a write from root ------------------
    // The value has to come back changed: an update handler that returns 0 and
    // stores nothing is the same lie in a different place.
    {
        static const char *knob = "/proc/sys/fs/nr_open";
        struct stat st;
        ck("nr_open exists", stat(knob, &st), 0);
        ck("  mode is 0644", (long) (st.st_mode & 07777), 0644);
        long original = sysctl_read(knob);
        ck("  reads as a number", original > 0, 1);
        ck("root's write is accepted", sysctl_write(knob, "524288\n") > 0, 1);
        ck("  and takes effect", sysctl_read(knob), 524288);
        char restore[32];
        snprintf(restore, sizeof restore, "%ld\n", original);
        sysctl_write(knob, restore);
        ck("  restored", sysctl_read(knob), original);
    }
    {
        static const char *knob = "/proc/sys/kernel/threads-max";
        struct stat st;
        ck("threads-max exists", stat(knob, &st), 0);
        ck("  mode is 0644", (long) (st.st_mode & 07777), 0644);
        long original = sysctl_read(knob);
        ck("root's write is accepted", sysctl_write(knob, "60000\n") > 0, 1);
        ck("  and takes effect", sysctl_read(knob), 60000);
        char restore[32];
        snprintf(restore, sizeof restore, "%ld\n", original);
        sysctl_write(knob, restore);
        ck("  restored", sysctl_read(knob), original);
    }
    // pid_max is writable the same way, but its ceiling is the kernel's own pid
    // space -- 4194304 on Linux, and on AOK the compile-time size of the pid
    // table. So the value that round-trips is not portable; what is portable is
    // that a write of the CURRENT value succeeds, and that an absurd one is
    // EINVAL rather than EPERM.
    {
        static const char *knob = "/proc/sys/kernel/pid_max";
        struct stat st;
        ck("pid_max exists", stat(knob, &st), 0);
        ck("  mode is 0644", (long) (st.st_mode & 07777), 0644);
        long original = sysctl_read(knob);
        char same[32];
        snprintf(same, sizeof same, "%ld\n", original);
        ck("root's write of the current value is accepted", sysctl_write(knob, same) > 0, 1);
        ck("  and it is unchanged", sysctl_read(knob), original);
        ck("a write past any kernel's ceiling is EINVAL",
           sysctl_write(knob, "999999999\n"), -EINVAL);
        ck("  and it is still unchanged", sysctl_read(knob), original);
        // Below Linux's floor of 301.
        ck("a write below the floor is EINVAL", sysctl_write(knob, "2\n"), -EINVAL);
        ck("  and it is still unchanged", sysctl_read(knob), original);
    }

    // ---- binfmt_misc admits to not being mounted --------------------------
    // Linux's /proc/sys/fs/binfmt_misc is an empty directory until the
    // filesystem is mounted on it, so `status` and `register` must not exist.
    // On a machine that HAS mounted it they will, which is why this checks the
    // pair together rather than asserting absence outright.
    {
        struct stat st;
        ck("binfmt_misc is a directory", stat("/proc/sys/fs/binfmt_misc", &st) == 0 &&
           S_ISDIR(st.st_mode), 1);
        bool has_status = access("/proc/sys/fs/binfmt_misc/status", F_OK) == 0;
        bool has_register = access("/proc/sys/fs/binfmt_misc/register", F_OK) == 0;
        ck("status and register appear together or not at all",
           has_status == has_register, 1);
        if (has_status) {
            // Mounted: then it has to actually work -- writing 0 to status
            // must be visible on the next read. That is the assertion the old
            // hardcoded "enabled" failed.
            int fd = open("/proc/sys/fs/binfmt_misc/status", O_RDONLY);
            char buf[32] = { 0 };
            if (fd >= 0) {
                if (read(fd, buf, sizeof buf - 1) <= 0)
                    failures_total++;
                close(fd);
            }
            char *nl = strchr(buf, '\n');
            if (nl != NULL)
                *nl = '\0';
            bool was_enabled = strcmp(buf, "enabled") == 0;
            test_logf("  %-56s \"%s\"\n", "binfmt_misc is mounted; status reads", buf);
            if (was_enabled) {
                ck("  writing 0 to status is accepted",
                   sysctl_write("/proc/sys/fs/binfmt_misc/status", "0") > 0, 1);
                fd = open("/proc/sys/fs/binfmt_misc/status", O_RDONLY);
                memset(buf, 0, sizeof buf);
                if (fd >= 0) {
                    if (read(fd, buf, sizeof buf - 1) <= 0)
                        failures_total++;
                    close(fd);
                }
                nl = strchr(buf, '\n');
                if (nl != NULL)
                    *nl = '\0';
                ck("  ...and status really says disabled", strcmp(buf, "disabled") == 0, 1);
                sysctl_write("/proc/sys/fs/binfmt_misc/status", "1");
            }
        } else {
            test_logf("  %-56s\n", "binfmt_misc is not mounted, so it is empty");
        }
    }

    return finish_suite("sysctl_write_rules");
}
