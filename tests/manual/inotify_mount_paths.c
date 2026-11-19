/*
 * inotify_mount_paths.c — regression lock for inotify event delivery on
 * NON-ROOT mounts, and for bind() of a unix socket raising IN_CREATE.
 *
 * AOK-iSH registers inotify watches by full normalized guest path
 * (sys_inotify_add_watch), but every notification call site in fs/generic.c
 * used to fire with the path AFTER find_mount_and_trim_path() had rewritten
 * it to be mount-relative. On the root mount the two are identical, which
 * hid the bug from every existing test; on any other mount (tmpfs /run,
 * /tmp, ...) events matched nothing and were silently dropped.
 *
 * Separately, bind() creating a unix-socket inode bypassed generic_mknodat
 * and raised no IN_CREATE at all, on any mount.
 *
 * Real-world victim: sd-bus's WATCH_BIND state. A daemon started before
 * dbus.socket exists (systemd-resolved, logind) parks watching /run/dbus
 * with inotify and retries its bus connection only when the creation of
 * system_bus_socket fires the event. Both halves of that event chain
 * (mkdir + bind, on the /run tmpfs) were dropped, so such daemons never
 * connected, never socket-activated dbus-broker, and ran nameless on the
 * bus -- every D-Bus call to them ate the full 25s/120s method-call timeout
 * (resolvectl query timing out while getent worked; 25s pam_systemd stalls).
 *
 * The test mounts a private tmpfs (falling back to a plain directory when
 * unprivileged, which still locks the bind/IN_CREATE half on the root
 * mount), watches it, performs creat/mkdir/bind/chmod/rename/unlink/rmdir,
 * and requires the corresponding events with the right names.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "test_common.h"

static char base[256];
static int mounted;

static void check(const char *label, long got, long exp) {
    int bad = got != exp;
    test_log_if(bad, "%s: got=%ld exp=%ld errno=%d (%s)\n",
                label, got, exp, errno, strerror(errno));
    if (bad)
        failf(label, (uint64_t) got, (uint64_t) errno, 0, (uint64_t) exp, 0, 0);
}

/* Drain events for up to 2s until all wanted (mask, name) pairs have shown
 * up; a single read batch may satisfy several. Returns a bitmask of which
 * were seen. Unrelated events are skipped. */
static unsigned expect_events(int ifd, size_t count,
        const uint32_t *masks, const char *const *names) {
    char buf[4096];
    unsigned seen = 0, all = (1u << count) - 1;
    while (seen != all) {
        struct pollfd pfd = { .fd = ifd, .events = POLLIN };
        if (poll(&pfd, 1, 2000) <= 0)
            break;
        ssize_t n = read(ifd, buf, sizeof buf);
        if (n <= 0)
            break;
        for (char *p = buf; p < buf + n;) {
            struct inotify_event *ev = (struct inotify_event *) p;
            test_logf("  event mask=%#x name=%s\n", ev->mask,
                      ev->len ? ev->name : "");
            for (size_t i = 0; i < count; i++) {
                if ((ev->mask & masks[i]) == masks[i] &&
                        (names[i] == NULL ||
                         (ev->len && strcmp(ev->name, names[i]) == 0)))
                    seen |= 1u << i;
            }
            p += sizeof(*ev) + ev->len;
        }
    }
    return seen;
}

static int expect_event(int ifd, uint32_t mask, const char *name) {
    return expect_events(ifd, 1, &mask, &name) == 1;
}

static void cleanup(void) {
    char p[512];
    snprintf(p, sizeof p, "%s/sock", base); unlink(p);
    snprintf(p, sizeof p, "%s/file2", base); unlink(p);
    snprintf(p, sizeof p, "%s/file", base); unlink(p);
    snprintf(p, sizeof p, "%s/dir", base); rmdir(p);
    if (mounted)
        umount(base);
    rmdir(base);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(30));

    snprintf(base, sizeof base, "/tmp/inmnt.%d", getpid());
    rmdir(base);
    check("mkdir_base", mkdir(base, 0755), 0);

    /* A separate mount is the point of the test: only there does the
     * mount-relative-path bug bite. Unprivileged fallback still exercises
     * the unix-bind IN_CREATE half. */
    if (mount("tmpfs", base, "tmpfs", 0, NULL) == 0)
        mounted = 1;
    test_logf("  tmpfs mounted: %d\n", mounted);

    int ifd = inotify_init1(IN_NONBLOCK);
    check("inotify_init", ifd >= 0, 1);
    int wd = inotify_add_watch(ifd, base,
            IN_CREATE | IN_DELETE | IN_MODIFY | IN_ATTRIB |
            IN_MOVED_FROM | IN_MOVED_TO);
    check("add_watch", wd >= 0, 1);

    char p[512], p2[512];

    /* creat -> IN_CREATE */
    snprintf(p, sizeof p, "%s/file", base);
    int fd = open(p, O_CREAT | O_WRONLY, 0644);
    check("creat", fd >= 0, 1);
    check("event_create_file", expect_event(ifd, IN_CREATE, "file"), 1);

    /* write -> IN_MODIFY */
    check("write", write(fd, "x", 1), 1);
    close(fd);
    check("event_modify_file", expect_event(ifd, IN_MODIFY, "file"), 1);

    /* chmod -> IN_ATTRIB */
    check("chmod", chmod(p, 0600), 0);
    check("event_attrib_file", expect_event(ifd, IN_ATTRIB, "file"), 1);

    /* mkdir -> IN_CREATE|IN_ISDIR */
    snprintf(p2, sizeof p2, "%s/dir", base);
    check("mkdir", mkdir(p2, 0755), 0);
    check("event_create_dir", expect_event(ifd, IN_CREATE | IN_ISDIR, "dir"), 1);

    /* bind() of a unix socket -> IN_CREATE (the sd-bus WATCH_BIND trigger) */
    int sfd = socket(AF_UNIX, SOCK_STREAM, 0);
    check("socket", sfd >= 0, 1);
    struct sockaddr_un sun = { .sun_family = AF_UNIX };
    snprintf(sun.sun_path, sizeof sun.sun_path, "%s/sock", base);
    check("bind", bind(sfd, (struct sockaddr *) &sun, sizeof sun), 0);
    check("event_create_sock", expect_event(ifd, IN_CREATE, "sock"), 1);
    close(sfd);

    /* rename -> IN_MOVED_FROM + IN_MOVED_TO */
    snprintf(p2, sizeof p2, "%s/file2", base);
    check("rename", rename(p, p2), 0);
    {
        const uint32_t move_masks[2] = { IN_MOVED_FROM, IN_MOVED_TO };
        const char *const move_names[2] = { "file", "file2" };
        unsigned seen = expect_events(ifd, 2, move_masks, move_names);
        check("event_moved_from", seen & 1, 1);
        check("event_moved_to", !!(seen & 2), 1);
    }

    /* unlink -> IN_DELETE */
    check("unlink", unlink(p2), 0);
    check("event_delete_file", expect_event(ifd, IN_DELETE, "file2"), 1);

    /* rmdir -> IN_DELETE|IN_ISDIR */
    snprintf(p2, sizeof p2, "%s/dir", base);
    check("rmdir", rmdir(p2), 0);
    check("event_delete_dir", expect_event(ifd, IN_DELETE | IN_ISDIR, "dir"), 1);

    close(ifd);
    cleanup();
    return finish_suite("inotify_mount_paths");
}
