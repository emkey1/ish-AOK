// Epoll-inside-epoll readiness, the exact topology systemd boots with:
// sd-event's epoll registers libmount's mount-monitor epoll fd
// (level-triggered EPOLLIN), and that inner epoll's only member is
// /proc/self/mountinfo registered EPOLLIN|EPOLLET. Linux reports an epoll fd
// readable whenever its ready list is non-empty, so a mount-table edge inside
// the inner epoll must (a) wake a blocked epoll_wait on the OUTER epoll and
// (b) show the inner fd as readable in a later outer scan.
//
// AOK's epoll fds had no .poll callback and poll_wakeup did not cascade
// through an owning epoll, so the edge died inside the inner epoll: the
// outer epoll_wait slept through it, systemd's mountinfo rescan never ran,
// and every boot-time mount unit "protocol"-failed
// ("Failed with result 'protocol'" on tmp.mount; the tmpfs itself was
// mounted fine). Fixed by kernel/epoll.c epoll_poll + the poll_wakeup
// owner-fd cascade (fs/poll.c).
//
// Also passes on real Linux (mint oracle, verified).
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_common.h"

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(15));

    // Can we mount at all? Settled FIRST, before either epoll exists, for two
    // reasons. The parent commits to a 3-second blocking epoll_wait before it
    // can learn why the child's mount failed, so an unprivileged run reported
    // the missed edge, the child's failure AND the inner epoll's silence as
    // three separate failures -- which reads like a nested-epoll regression and
    // is only ever a missing capability. And probing here means the probe's own
    // mount/umount cannot leave an edge queued on an epoll that does not exist
    // yet, so there is nothing to drain afterwards -- draining a LEVEL-triggered
    // outer epoll never terminates, which is its own trap.
    //
    // Probed by trying rather than by testing for uid 0: the CLI harness runs as
    // root, an ssh session into the app does not, and privilege is not always
    // spelled uid.
    {
        char probe[] = "/tmp/epoll_nested_probe.XXXXXX";
        if (mkdtemp(probe) == NULL) {
            perror("mkdtemp probe");
            return 1;
        }
        int mounted = mount("tmpfs", probe, "tmpfs", 0, "") == 0;
        int why = errno;
        if (mounted)
            umount(probe);
        rmdir(probe);
        if (!mounted) {
            if (why == EPERM || why == EACCES) {
                printf("epoll_nested: SKIP (needs privilege to mount: %s)\n",
                       strerror(why));
                return 0;
            }
            printf("FAIL: mount(tmpfs) probe -> %s\n", strerror(why));
            return 1;
        }
    }

    int mi = open("/proc/self/mountinfo", O_RDONLY | O_CLOEXEC);
    if (mi < 0) {
        perror("open mountinfo");
        return 1;
    }

    // Inner epoll: libmount's monitor shape (mountinfo, EPOLLIN|EPOLLET).
    int inner = epoll_create1(EPOLL_CLOEXEC);
    // Outer epoll: sd-event's shape (the inner epoll fd, level EPOLLIN).
    int outer = epoll_create1(EPOLL_CLOEXEC);
    if (inner < 0 || outer < 0) {
        perror("epoll_create1");
        return 1;
    }
    struct epoll_event ev = {.events = EPOLLIN | EPOLLET};
    if (epoll_ctl(inner, EPOLL_CTL_ADD, mi, &ev) < 0) {
        printf("FAIL: inner ADD mountinfo -> %s\n", strerror(errno));
        return 1;
    }
    struct epoll_event oev = {.events = EPOLLIN};
    if (epoll_ctl(outer, EPOLL_CTL_ADD, inner, &oev) < 0) {
        printf("FAIL: outer ADD inner-epoll -> %s\n", strerror(errno));
        return 1;
    }

    struct epoll_event out[1];
    // Drain the initial readiness edge through the outer epoll: the inner
    // epoll holds mountinfo's setup edge, so the outer must show it readable.
    int r = epoll_wait(outer, out, 1, 0);
    test_logf("outer initial -> %d\n", r);
    if (r != 1) {
        printf("FAIL: outer epoll does not see inner's initial readiness "
               "(got %d, want 1)\n", r);
        failures_total++;
    }
    while (epoll_wait(inner, out, 1, 0) > 0)
        ;
    // Inner drained: level-triggered outer must now report not-ready.
    r = epoll_wait(outer, out, 1, 0);
    test_log_if(r == 0, "outer quiescent after inner drain ok\n");
    if (r != 0) {
        printf("FAIL: outer still ready after inner drained (got %d)\n", r);
        failures_total++;
    }

    // Change the mount table; the edge must propagate to a BLOCKED outer
    // epoll_wait. Fork: child mounts after a beat, parent blocks on outer.
    char target[] = "/tmp/epoll_nested.XXXXXX";
    if (mkdtemp(target) == NULL) {
        perror("mkdtemp");
        return 1;
    }
    pid_t child = fork();
    if (child == 0) {
        usleep(200000);
        if (mount("tmpfs", target, "tmpfs", 0, "") != 0)
            _exit(1);
        _exit(0);
    }
    r = epoll_wait(outer, out, 1, 3000);
    test_logf("outer after mount -> %d\n", r);
    if (r != 1) {
        printf("FAIL: blocked outer epoll_wait missed the mount edge "
               "(got %d, want 1) -- systemd's tmp.mount protocol failure\n", r);
        failures_total++;
    }
    int status = 0;
    waitpid(child, &status, 0);
    if (status != 0) {
        printf("FAIL: child mount failed (status %#x)\n", status);
        failures_total++;
    }

    // The inner epoll must hold exactly one edge for the change.
    r = epoll_wait(inner, out, 1, 0);
    test_log_if(r == 1, "inner delivers the mount edge ok\n");
    if (r != 1) {
        printf("FAIL: inner epoll has no event after mount (got %d)\n", r);
        failures_total++;
    }
    // Consumed: both levels quiescent again.
    r = epoll_wait(inner, out, 1, 0);
    int r2 = epoll_wait(outer, out, 1, 0);
    if (r != 0 || r2 != 0) {
        printf("FAIL: not quiescent after consuming edge (inner %d outer %d)\n",
               r, r2);
        failures_total++;
    }

    umount2(target, MNT_DETACH);
    rmdir(target);
    close(outer);
    close(inner);
    close(mi);

    return finish_suite("epoll_nested");
}
