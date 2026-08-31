// inotify flags that were accepted and ignored, and a queue with no limit.
//
//   IN_MASK_ADD ORs the new events into an existing watch's mask -- that is
//   the entire reason the flag exists. It was ignored, so a second add
//   silently discarded whatever the first one was watching for.
//
//   IN_ONESHOT means "tell me once, then forget me". Also ignored: the watch
//   fired forever and never sent the IN_IGNORED that tells a reader the wd is
//   dead, so a program that installed a one-shot watch and moved on kept
//   getting events for a descriptor it believed was gone.
//
//   The event queue had no limit at all. A watched directory under churn and
//   a reader that stalls is enough to grow the heap without bound, and nothing
//   ever told the reader it had missed anything. Linux caps the queue at
//   fs.inotify.max_queued_events and appends one synthetic
//   {wd = -1, IN_Q_OVERFLOW} meaning "rescan, I stopped keeping track" -- the
//   only honest thing available, since the missed events cannot be recovered.
//
// The cap is asserted by behaviour, not by number: what matters is that the
// queue stops growing and the reader is told. Measured against x86_64 glibc on
// Linux 6.12, which drains exactly 16385 (16384 + the marker) where AOK used
// to drain all 40000.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sys/inotify.h>
#include <sys/stat.h>

#include "test_common.h"

static void ck(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-52s got=%-10ld want=%ld\n", label, got, want);
}

static char base[160];
static const char *P(const char *sub) {
    static char b[300];
    snprintf(b, sizeof b, "%s/%s", base, sub);
    return b;
}
static void touch(const char *p) {
    int f = open(p, O_RDWR | O_CREAT, 0644);
    if (f >= 0)
        close(f);
}

// Drain every queued event, returning the OR of the masks seen.
static uint32_t drain(int fd, int *count) {
    char buf[8192];
    uint32_t seen = 0;
    int n = 0;
    for (;;) {
        ssize_t r = read(fd, buf, sizeof buf);
        if (r <= 0)
            break;
        for (char *p = buf; p < buf + r; ) {
            struct inotify_event *e = (struct inotify_event *) p;
            seen |= e->mask;
            n++;
            p += sizeof(*e) + e->len;
        }
    }
    if (count != NULL)
        *count = n;
    return seen;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(180));
    snprintf(base, sizeof base, "/tmp/inotmq-%d", (int) getpid());
    { char c[400]; snprintf(c, sizeof c, "rm -rf '%s' && mkdir -p '%s'", base, base);
      if (system(c) < 0) { printf("FAIL could not make the scratch dir\n"); return 1; } }

    test_logf("[74] IN_MASK_ADD ORs into the existing mask\n");
    {
        int fd = inotify_init1(IN_NONBLOCK);
        touch(P("f"));
        int wd = inotify_add_watch(fd, P("f"), IN_MODIFY);
        int wd2 = inotify_add_watch(fd, P("f"), IN_ATTRIB | IN_MASK_ADD);
        ck("  the second add returns the same wd", wd2, wd);
        int f = open(P("f"), O_RDWR); if (write(f, "x", 1) != 1) {} close(f);
        chmod(P("f"), 0600);
        usleep(200000);
        uint32_t seen = drain(fd, NULL);
        test_logf("    seen mask = %#x\n", seen);
        ck("  IN_MODIFY still fires", (seen & IN_MODIFY) != 0, 1);
        ck("  and IN_ATTRIB too", (seen & IN_ATTRIB) != 0, 1);
        close(fd);
    }

    test_logf("[74] and WITHOUT IN_MASK_ADD the mask is replaced\n");
    {
        int fd = inotify_init1(IN_NONBLOCK);
        touch(P("g"));
        inotify_add_watch(fd, P("g"), IN_MODIFY);
        inotify_add_watch(fd, P("g"), IN_ATTRIB);          // no MASK_ADD: replaces
        int f = open(P("g"), O_RDWR); if (write(f, "x", 1) != 1) {} close(f);
        usleep(200000);
        uint32_t seen = drain(fd, NULL);
        ck("  IN_MODIFY no longer fires", (seen & IN_MODIFY) != 0, 0);
        close(fd);
    }

    test_logf("[135] IN_ONESHOT fires once, then IN_IGNORED, then is gone\n");
    {
        int fd = inotify_init1(IN_NONBLOCK);
        mkdir(P("d"), 0755);
        inotify_add_watch(fd, P("d"), IN_CREATE | IN_ONESHOT);
        touch(P("d/a"));
        touch(P("d/b"));
        usleep(200000);
        int n = 0;
        uint32_t seen = drain(fd, &n);
        test_logf("    %d event(s), mask %#x\n", n, seen);
        ck("  exactly two events (the create and IN_IGNORED)", n, 2);
        ck("  IN_CREATE fired", (seen & IN_CREATE) != 0, 1);
        ck("  IN_IGNORED followed", (seen & IN_IGNORED) != 0, 1);
        close(fd);
    }

    test_logf("[136] a read buffer too small for the head event is EINVAL\n");
    {
        int fd = inotify_init1(IN_NONBLOCK);
        mkdir(P("e"), 0755);
        inotify_add_watch(fd, P("e"), IN_CREATE);
        touch(P("e/somewhat-long-name"));
        usleep(200000);
        char tiny[8];
        errno = 0;
        ssize_t r = read(fd, tiny, sizeof tiny);
        ck("  read into a tiny buffer is EINVAL", r < 0 ? errno : 0, EINVAL);
        // ...and the event is still there
        char big[4096];
        errno = 0;
        r = read(fd, big, sizeof big);
        ck("  and the event was not consumed", r > 0, 1);
        close(fd);
    }

    test_logf("[138] the queue is bounded and reports IN_Q_OVERFLOW\n");
    {
        int fd = inotify_init1(IN_NONBLOCK);
        mkdir(P("q"), 0755);
        inotify_add_watch(fd, P("q"), IN_CREATE);
        // more events than any sane queue limit, never read
        for (int i = 0; i < 40000; i++) {
            char n[300]; snprintf(n, sizeof n, "%s/q/f%d", base, i);
            int f = open(n, O_RDWR|O_CREAT, 0644); if (f >= 0) close(f);
        }
        usleep(300000);
        int n = 0;
        uint32_t seen = drain(fd, &n);
        test_logf("    drained %d events, mask %#x\n", n, seen);
        ck("  IN_Q_OVERFLOW was delivered", (seen & IN_Q_OVERFLOW) != 0, 1);
        ck("  and the queue did not keep everything", n < 40000, 1);
        close(fd);
    }

    { char c[400]; snprintf(c, sizeof c, "rm -rf '%s'", base); if (system(c) < 0) {} }
    return finish_suite("inotify_mask_queue");
}
