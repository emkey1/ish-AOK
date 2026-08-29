// The inotify event set file watchers actually key on.
//
// IN_CLOSE_WRITE is the one that matters most: `inotifywait -e close_write`,
// entr, and every build/reload watcher use it to mean "the writer finished,
// the file is consistent now". AOK emitted no close events at all, so those
// tools sat silent. IN_ACCESS was likewise never emitted, and a watch on a
// deleted file was never retired with IN_IGNORED -- so a watcher kept a stale
// wd forever and went deaf after the first save-by-rename.
//
// Each expectation is the exact event sequence Linux 6.12 produces, measured
// rather than assumed -- including the ordering, since AOK reported IN_OPEN
// before the IN_CREATE that caused it.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/inotify.h>
#include <sys/stat.h>

#include "test_common.h"

static char dir[128];
static int ifd;

static const char *maskname(uint32_t m) {
    static char b[256];
    b[0] = 0;
#define M(x) if (m & x) { if (b[0]) strcat(b, "|"); strcat(b, #x); }
    M(IN_ACCESS) M(IN_MODIFY) M(IN_ATTRIB) M(IN_CLOSE_WRITE) M(IN_CLOSE_NOWRITE)
    M(IN_OPEN) M(IN_MOVED_FROM) M(IN_MOVED_TO) M(IN_CREATE) M(IN_DELETE)
    M(IN_DELETE_SELF) M(IN_MOVE_SELF) M(IN_IGNORED) M(IN_ISDIR)
#undef M
    if (!b[0]) snprintf(b, sizeof b, "0x%x", m);
    return b;
}

// Drain the queue and compare the whole sequence, so ordering is covered too.
static void drain(const char *label, const char *want) {
    char buf[8192];
    char seen[1024] = {0};
    for (;;) {
        struct pollfd p = { ifd, POLLIN, 0 };
        if (poll(&p, 1, (int) (500 * test_watchdog_secs(1))) <= 0)
            break;
        ssize_t n = read(ifd, buf, sizeof buf);
        if (n <= 0)
            break;
        for (char *q = buf; q < buf + n; ) {
            struct inotify_event *e = (struct inotify_event *) q;
            char one[160];
            snprintf(one, sizeof one, "%s%s[%s]%s", seen[0] ? " " : "",
                     maskname(e->mask), e->len ? e->name : "", e->cookie ? "#c" : "");
            if (strlen(seen) + strlen(one) < sizeof seen - 1)
                strcat(seen, one);
            q += sizeof(struct inotify_event) + e->len;
        }
    }
    if (strcmp(seen, want) != 0) {
        printf("FAIL %s\n      got:  %s\n      want: %s\n", label,
               seen[0] ? seen : "(nothing)", want[0] ? want : "(nothing)");
        failf(label, 0, 0, 0, 0, 0, 0);
        return;
    }
    test_logf("  %-30s %s\n", label, seen[0] ? seen : "(nothing)");
}

// Consume whatever is queued without judging it -- for use after a block's
// own cleanup, whose unlinks and rmdirs raise events of their own.
static void drain_any(void) {
    char buf[8192];
    for (;;) {
        struct pollfd p = { ifd, POLLIN, 0 };
        if (poll(&p, 1, (int) (200 * test_watchdog_secs(1))) <= 0)
            break;
        if (read(ifd, buf, sizeof buf) <= 0)
            break;
    }
}

static void wr(const char *name, const char *data) {
    char p[256];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    int fd = open(p, O_WRONLY | O_CREAT, 0600);
    if (fd >= 0) {
        if (write(fd, data, strlen(data)) < 0) {}
        close(fd);
    }
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(120));
    snprintf(dir, sizeof dir, "/tmp/inot_ev.%d", (int) getpid());
    if (mkdir(dir, 0700) < 0) {
        printf("inotify_events: SKIP (cannot create %s)\n", dir);
        return 0;
    }
    ifd = inotify_init1(IN_NONBLOCK);
    if (ifd < 0) {
        printf("inotify_events: SKIP (inotify_init1: %s)\n", strerror(errno));
        rmdir(dir);
        return 0;
    }

    int wd = inotify_add_watch(ifd, dir, IN_ALL_EVENTS);
    if (wd < 0) {
        printf("inotify_events: SKIP (add_watch: %s)\n", strerror(errno));
        close(ifd); rmdir(dir);
        return 0;
    }
    // Watching the same path twice is one watch, and reports the same wd.
    int wd2 = inotify_add_watch(ifd, dir, IN_ALL_EVENTS);
    if (wd2 != wd)
        failf("duplicate add -> same wd", (uint64_t) wd2, 0, 0, (uint64_t) wd, 0, 0);

    drain("settle", "");
    wr("a.txt", "hello");
    drain("create+write+close",
          "IN_CREATE[a.txt] IN_OPEN[a.txt] IN_MODIFY[a.txt] IN_CLOSE_WRITE[a.txt]");

    {
        char p[256];
        snprintf(p, sizeof p, "%s/a.txt", dir);
        int fd = open(p, O_RDONLY);
        char c;
        if (fd >= 0) { if (read(fd, &c, 1) < 0) {} close(fd); }
    }
    drain("read-only open+close",
          "IN_OPEN[a.txt] IN_ACCESS[a.txt] IN_CLOSE_NOWRITE[a.txt]");

    {
        char o[256], n[256];
        snprintf(o, sizeof o, "%s/a.txt", dir);
        snprintf(n, sizeof n, "%s/b.txt", dir);
        if (rename(o, n) < 0) {}
    }
    drain("rename within dir", "IN_MOVED_FROM[a.txt]#c IN_MOVED_TO[b.txt]#c");

    { char p[256]; snprintf(p, sizeof p, "%s/b.txt", dir); chmod(p, 0644); }
    drain("chmod", "IN_ATTRIB[b.txt]");

    { char p[256]; snprintf(p, sizeof p, "%s/b.txt", dir); unlink(p); }
    drain("unlink", "IN_DELETE[b.txt]");

    if (inotify_rm_watch(ifd, wd) < 0)
        failf("inotify_rm_watch", (uint64_t) errno, 0, 0, 0, 0, 0);
    drain("rm_watch -> IN_IGNORED", "IN_IGNORED[]");

    // A watch on the file itself: deleting it retires the watch.
    wr("c.txt", "x");
    {
        char p[256];
        snprintf(p, sizeof p, "%s/c.txt", dir);
        if (inotify_add_watch(ifd, p, IN_ALL_EVENTS) < 0)
            failf("add_watch on a file", (uint64_t) errno, 0, 0, 0, 0, 0);
        drain("settle", "");
        unlink(p);
    }
    drain("unlink the watched file", "IN_ATTRIB[] IN_DELETE_SELF[] IN_IGNORED[]");

    errno = 0;
    if (!(inotify_rm_watch(ifd, 12345) < 0 && errno == EINVAL))
        failf("rm_watch(bogus wd) -> EINVAL", (uint64_t) errno, 0, 0, EINVAL, 0, 0);
    errno = 0;
    if (!(inotify_add_watch(ifd, "/tmp/inotify-events-no-such-path", IN_ALL_EVENTS) < 0 &&
            errno == ENOENT))
        failf("add_watch(missing) -> ENOENT", (uint64_t) errno, 0, 0, ENOENT, 0, 0);

    // Watch identity. Watches here are keyed by path where Linux keys them by
    // inode, so the cases where those two disagree are what is pinned: the
    // watched file renamed, an ANCESTOR directory renamed -- which used to
    // leave the watch naming a path that no longer existed, silently deaf --
    // and a fresh file at the old name, which must NOT inherit the watch.
    {
        char sub[256], sub2[256], f1[320], f2[320], moved[320];
        snprintf(sub, sizeof sub, "%s/sub", dir);
        snprintf(sub2, sizeof sub2, "%s/sub2", dir);
        if (mkdir(sub, 0700) == 0) {
            snprintf(f1, sizeof f1, "%s/f", sub);
            snprintf(f2, sizeof f2, "%s/renamed", sub);
            snprintf(moved, sizeof moved, "%s/renamed", sub2);
            int fd = open(f1, O_WRONLY | O_CREAT, 0600);
            if (fd >= 0) { if (write(fd, "x", 1) < 0) {} close(fd); }

            int fwd = inotify_add_watch(ifd, f1, IN_MODIFY | IN_MOVE_SELF | IN_ATTRIB);
            if (fwd < 0) {
                printf("  watch identity: SKIP (%s)\n", strerror(errno));
            } else {
                drain("settle", "");

                // The watched file is renamed: the watch goes with it.
                if (rename(f1, f2) == 0) {
                    // No cookie: that pairs MOVED_FROM with MOVED_TO, not the self event.
                    drain("rename the watched file", "IN_MOVE_SELF[]");
                    fd = open(f2, O_WRONLY | O_APPEND);
                    if (fd >= 0) { if (write(fd, "y", 1) < 0) {} close(fd); }
                    drain("modify it at its new name", "IN_MODIFY[]");

                    // Its PARENT is renamed. The inode has not moved -- only
                    // the path by which it is reached -- so no event, and the
                    // watch must keep working underneath the new name.
                    if (rename(sub, sub2) == 0) {
                        drain("rename the parent directory", "");
                        fd = open(moved, O_WRONLY | O_APPEND);
                        if (fd >= 0) { if (write(fd, "z", 1) < 0) {} close(fd); }
                        drain("modify it under its new parent", "IN_MODIFY[]");

                        // A brand-new file at the ORIGINAL path is a different
                        // inode and must not inherit the watch.
                        mkdir(sub, 0700);
                        fd = open(f1, O_WRONLY | O_CREAT, 0600);
                        if (fd >= 0) { if (write(fd, "w", 1) < 0) {} close(fd); }
                        drain("a new file at the old path is not watched", "");
                        unlink(f1);
                        rmdir(sub);
                        unlink(moved);
                        rmdir(sub2);
                    } else {
                        unlink(f2);
                        rmdir(sub);
                    }
                }
            }
        }
    }

    // link() reports two things on Linux: IN_ATTRIB on the inode whose link
    // count changed, and IN_CREATE in the directory that gained the name.
    // Neither was emitted at all.
    {
        char src[256], lnk[256];
        snprintf(src, sizeof src, "%s/linksrc", dir);
        snprintf(lnk, sizeof lnk, "%s/linkdst", dir);
        int fd = open(src, O_WRONLY | O_CREAT, 0600);
        if (fd >= 0) { if (write(fd, "x", 1) < 0) {} close(fd); }
        drain_any();          // the previous block's teardown
        int lwd = inotify_add_watch(ifd, src, IN_ATTRIB);
        if (lwd < 0) {
            printf("  link(): SKIP (%s)\n", strerror(errno));
        } else {
            drain("settle", "");
            if (link(src, lnk) == 0)
                drain("link() -> IN_ATTRIB on the inode", "IN_ATTRIB[]");
            else
                printf("  link(): SKIP (%s)\n", strerror(errno));
            inotify_rm_watch(ifd, lwd);
            drain_any();
        }
        unlink(lnk);
        unlink(src);
    }

    close(ifd);
    rmdir(dir);
    return finish_suite("inotify_events");
}
