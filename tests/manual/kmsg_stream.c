// The kernel log, as the two files every syslog daemon opens.
//
//   AOK has implemented the kernel log ring buffer since forever, and has
//   served it to syslog(2) all along -- but neither of the files userspace
//   actually reads existed. /dev/kmsg had a driver (fs/mem.c) and a device
//   number and no node in any root, and there was no /proc/kmsg at all. So
//   every syslog daemon failed on its very first step: busybox's klogd and
//   rsyslog's imklog open /dev/kmsg, and the older ones open /proc/kmsg
//   ("cannot open kernel log").
//
//   Creating the node is not enough on its own. A daemon's whole main loop is
//   a blocking read on that fd, and the driver returned 0 -- not "wait" -- as
//   soon as the reader caught up, which turns that loop into a spin. Both
//   files now block until there is a new message, honour O_NONBLOCK with
//   EAGAIN, and report poll readiness rather than claiming to be always
//   readable.
//
//   The reader's position also had to stop being an offset INTO the ring
//   buffer. The buffer overwrites when it fills, so its size stops growing --
//   a reader parked at the end would never have seen another byte once the
//   log had wrapped once, and a blocking reader would have waited forever.
//   Positions are counted against every byte ever logged instead.
//
// Measured against x86_64 glibc on Linux 6.12: /dev/kmsg is 0644 char 1:11
// and /proc/kmsg is a 0400 regular file; on both, a drained non-blocking read
// is EAGAIN and poll reports nothing ready.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <string.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include "test_common.h"

static void ck(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-54s got=%-8ld want=%ld\n", label, got, want);
}

// Linux gates /proc/kmsg on CAP_SYSLOG and the write side of /dev/kmsg on the
// node's 0644 mode, so an unprivileged caller legitimately gets EACCES for both
// (measured on 6.12). That is the caller's privilege, not the kernel behaviour
// this file is about, so report it as a skip. Anything else -- including EACCES
// while actually root -- is still a failure.
//
// It matters because the suite does not always run as root: the CLI harness
// does, an ssh session into the app does not, and an unprivileged run used to
// report two flat failures here that looked like a kmsg regression.
static int kmsg_open(const char *path, int flags, const char *label) {
    int fd = open(path, flags);
    if (fd >= 0) {
        ck(label, 1, 1);
        return fd;
    }
    if ((errno == EACCES || errno == EPERM) && geteuid() != 0) {
        test_logf("  %-54s SKIP (unprivileged: %s)\n", label, strerror(errno));
        return -1;
    }
    ck(label, 0, 1);
    return -1;
}

// Read until the fd is caught up, then answer what the NEXT read says. On
// Linux that is always EAGAIN; a 0 would be the spin this test exists for.
// Bounded so a kernel that never catches up fails instead of looping.
static int drain_then_read(int fd) {
    char buf[4096];
    for (int i = 0; i < 20000; i++) {
        errno = 0;
        ssize_t n = read(fd, buf, sizeof buf);
        if (n <= 0)
            return n < 0 ? -errno : 0;
    }
    return -EMFILE; // never caught up: distinct from any real answer
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));

    // ---- /dev/kmsg: the node exists, and is the node Linux has ------------
    {
        struct stat st;
        ck("/dev/kmsg exists", stat("/dev/kmsg", &st), 0);
        if (stat("/dev/kmsg", &st) == 0) {
            ck("  is a character device", S_ISCHR(st.st_mode) ? 1 : 0, 1);
            ck("  major is 1", (long) major(st.st_rdev), 1);
            ck("  minor is 11", (long) minor(st.st_rdev), 11);
            ck("  mode is 0644", (long) (st.st_mode & 07777), 0644);
        }
    }

    // ---- /proc/kmsg: present, and root-only as Linux has it ---------------
    {
        struct stat st;
        ck("/proc/kmsg exists", stat("/proc/kmsg", &st), 0);
        if (stat("/proc/kmsg", &st) == 0) {
            ck("  is a regular file", S_ISREG(st.st_mode) ? 1 : 0, 1);
            ck("  mode is 0400", (long) (st.st_mode & 07777), 0400);
        }
    }

    // ---- a zero-length read answers at once, and never blocks -------------
    //
    // POSIX: read() with a count of zero "returns zero and has no other
    // results". rsyslogd starts by doing exactly that to /proc/kmsg, and here
    // it stopped a device booting: the stream's read loop could only ever copy
    // zero bytes, read that as "nothing new", wait, wake immediately because
    // there WAS something new, and go round again -- inside kernel code with
    // no syscall boundary, so no signal could land and the task could not be
    // killed. init gave up on rsyslogd sixty seconds later.
    //
    // The two nodes answer differently, and both answers are Devuan's:
    // /proc/kmsg is a byte stream and has nothing to object to, while
    // /dev/kmsg hands back one whole record per read and calls a buffer too
    // small to hold one a bad argument.
    {
        // A watchdog well under the suite's, because the failure being
        // guarded against is an unkillable spin: a child that hangs here has
        // to be reaped by the alarm rather than waited for.
        int fd = open("/proc/kmsg", O_RDONLY | O_NONBLOCK);
        if (fd >= 0) {
            char one;
            ck("/proc/kmsg zero-length read returns 0", read(fd, &one, 0), 0);
            close(fd);
        }
        fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
        if (fd >= 0) {
            char one;
            ssize_t r = read(fd, &one, 0);
            ck("/dev/kmsg zero-length read is EINVAL", r < 0 ? -errno : r, -EINVAL);
            close(fd);
        }
    }

    // ---- a caught-up reader is told to wait, not handed a zero ------------
    // This is the whole reason the node could not simply be created: a daemon
    // reading in a loop would have burned a core on zero-length reads.
    {
        int fd = kmsg_open("/dev/kmsg", O_RDONLY | O_NONBLOCK,
                           "/dev/kmsg opens O_NONBLOCK");
        if (fd >= 0) {
            ck("  drained read is EAGAIN, never 0", drain_then_read(fd), -EAGAIN);
            struct pollfd p = { .fd = fd, .events = POLLIN };
            ck("  and poll reports nothing ready", poll(&p, 1, 0), 0);
            ck("  with no stray revents", (long) p.revents, 0);
            close(fd);
        }
    }
    {
        int fd = kmsg_open("/proc/kmsg", O_RDONLY | O_NONBLOCK,
                           "/proc/kmsg opens O_NONBLOCK");
        if (fd >= 0) {
            // The same check, and the one that caught /proc/kmsg ignoring
            // O_NONBLOCK entirely: its read path could not see the open flags,
            // so a caller that asked not to block blocked anyway and the test
            // hung here rather than failing.
            ck("  drained read is EAGAIN, never 0", drain_then_read(fd), -EAGAIN);
            struct pollfd p = { .fd = fd, .events = POLLIN };
            ck("  and poll reports nothing ready", poll(&p, 1, 0), 0);
            close(fd);
        }
    }

    // ---- a fresh open sees the log from the start ------------------------
    // Both files start a new reader at the oldest message still buffered, so
    // a daemon started after boot still gets the boot messages. A reader that
    // began at "now" would silently lose them.
    {
        int fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
        if (fd >= 0) {
            char buf[4096];
            errno = 0;
            ssize_t n = read(fd, buf, sizeof buf);
            // The emulator always logs its banner at startup, so there is
            // something there; an empty log would make this vacuous.
            ck("a fresh reader sees buffered messages", n > 0 ? 1 : 0, 1);
            close(fd);

            // ...and a second, independent reader sees the same thing rather
            // than finding it consumed.
            int fd2 = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
            if (fd2 >= 0) {
                errno = 0;
                ssize_t n2 = read(fd2, buf, sizeof buf);
                ck("  and a second reader sees it too", n2 > 0 ? 1 : 0, 1);
                close(fd2);
            }
        }
    }

    // ---- a privileged write lands in the log -----------------------------
    // `echo x > /dev/kmsg` is how boot scripts and initramfs hooks put a line
    // in dmesg, so accepting the write is not enough -- it has to come back
    // out of a reader, with the "<N>" priority marker stripped the way Linux
    // stores it.
    {
        int rd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
        int wr = kmsg_open("/dev/kmsg", O_WRONLY, "/dev/kmsg opens for writing");
        if (rd >= 0 && wr >= 0) {
            char buf[8192];
            while (read(rd, buf, sizeof buf) > 0)
                ;  // catch up first, so what we read back is only ours
            static const char line[] = "<6>kmsg_stream test marker\n";
            errno = 0;
            ssize_t n = write(wr, line, sizeof line - 1);
            ck("  the write is accepted", n == (ssize_t) (sizeof line - 1), 1);
            errno = 0;
            ssize_t got = read(rd, buf, sizeof buf - 1);
            ck("  and a reader sees it", got > 0 ? 1 : 0, 1);
            if (got > 0) {
                buf[got] = '\0';
                ck("  with the text intact",
                   strstr(buf, "kmsg_stream test marker") != NULL ? 1 : 0, 1);
                ck("  and the <N> priority marker stripped",
                   strstr(buf, "<6>") == NULL ? 1 : 0, 1);
            }
        }
        if (rd >= 0)
            close(rd);
        if (wr >= 0)
            close(wr);
    }

    return finish_suite("kmsg_stream");
}
