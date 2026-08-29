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

    close(ifd);
    rmdir(dir);
    return finish_suite("inotify_events");
}
