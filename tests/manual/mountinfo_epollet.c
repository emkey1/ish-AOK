// /proc/self/mountinfo must generate an epoll EPOLLIN|EPOLLET edge on every
// mount-table change -- the exact contract libmount's kernel mount monitor
// (util-linux monitor.c) depends on: it registers the mountinfo fd with
// EPOLLIN|EPOLLET, drains the initial readiness event at setup, never reads
// the fd, and then treats each subsequent epoll event as "the mount table
// changed, rescan". systemd's drain_libmount() gates its ENTIRE mountinfo
// rescan on this: without an edge per change, mount_process_proc_self_mountinfo
// returns early and no mount unit can ever be observed as mounted --
// "tmp.mount: Mount process finished, but there is no mount", failing every
// mount unit in the boot transaction.
//
// Also locks in the plain poll() side: a proc file must report POLLIN
// (always readable), matching Linux.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include "test_common.h"

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(10));

    int mi = open("/proc/self/mountinfo", O_RDONLY | O_CLOEXEC);
    if (mi < 0) {
        perror("open mountinfo");
        return 1;
    }

    // Plain poll(): always readable, like any proc file on Linux.
    struct pollfd pfd = {.fd = mi, .events = POLLIN};
    int r = poll(&pfd, 1, 0);
    test_logf("poll(mountinfo) -> %d revents=%#x\n", r, pfd.revents);
    if (r != 1 || !(pfd.revents & POLLIN)) {
        printf("FAIL: poll(mountinfo, POLLIN) -> %d revents=%#x (want 1/POLLIN)\n",
               r, pfd.revents);
        failures_total++;
    }

    // libmount's kernel-monitor pattern, verbatim.
    int ep = epoll_create1(EPOLL_CLOEXEC);
    if (ep < 0) {
        perror("epoll_create1");
        return 1;
    }
    struct epoll_event ev = {.events = EPOLLIN | EPOLLET};
    if (epoll_ctl(ep, EPOLL_CTL_ADD, mi, &ev) < 0) {
        printf("FAIL: epoll_ctl(ADD mountinfo, EPOLLIN|EPOLLET) -> %s\n",
               strerror(errno));
        return 1;
    }
    // Drain initial events (monitor.c does exactly this loop after ADD).
    struct epoll_event out[1];
    int drained = 0;
    while (epoll_wait(ep, out, 1, 0) > 0)
        drained++;
    test_logf("initial drain consumed %d event(s)\n", drained);

    // Edge-triggered quiescence: with no mount change, no further events.
    r = epoll_wait(ep, out, 1, 0);
    test_log_if(r == 0, "quiescent after drain ok\n");
    if (r != 0) {
        printf("FAIL: epoll_wait after drain -> %d (want 0; EPOLLET must "
               "suppress re-delivery)\n", r);
        failures_total++;
    }

    // Change the mount table.
    char target[] = "/tmp/mountinfo_epollet.XXXXXX";
    if (mkdtemp(target) == NULL) {
        perror("mkdtemp");
        return 1;
    }
    // A mount needs privilege. The CLI harness runs as root, an ssh session
    // into the app does not, and an unprivileged run reported this as a flat
    // failure that read like a mount regression. Skip only for the privilege
    // errnos and only when actually unprivileged; anything else still fails.
    if (mount("tmpfs", target, "tmpfs", 0, "") != 0) {
        if ((errno == EPERM || errno == EACCES) && geteuid() != 0) {
            printf("mountinfo_epollet: SKIP (needs root to mount: %s)\n",
                   strerror(errno));
            rmdir(target);
            return 0;
        }
        printf("FAIL: mount(tmpfs) -> %s\n", strerror(errno));
        rmdir(target);
        return 1;
    }

    // The change must have generated a fresh edge.
    r = epoll_wait(ep, out, 1, 0);
    test_logf("epoll_wait after mount -> %d events=%#x\n", r,
              r > 0 ? out[0].events : 0);
    if (r != 1) {
        printf("FAIL: no epoll event after mount-table change (got %d) -- "
               "this is exactly what starves systemd's mountinfo rescan\n", r);
        failures_total++;
    }

    // And exactly one edge: consumed, quiescent again.
    r = epoll_wait(ep, out, 1, 0);
    test_log_if(r == 0, "single edge per change ok\n");
    if (r != 0) {
        printf("FAIL: second epoll_wait after one change -> %d (want 0)\n", r);
        failures_total++;
    }

    // An unmount is a change too.
    if (umount2(target, MNT_DETACH) == 0) {
        r = epoll_wait(ep, out, 1, 0);
        test_log_if(r == 1, "edge on umount ok\n");
        if (r != 1) {
            printf("FAIL: no epoll event after umount (got %d)\n", r);
            failures_total++;
        }
    }

    rmdir(target);
    close(ep);
    close(mi);

    return finish_suite("mountinfo_epollet");
}
