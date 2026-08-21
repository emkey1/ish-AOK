/*
 * scm_rights_pidfd.c — regression lock for passing host-backed-less fds
 * (pidfd, and adhoc fds generally) over unix sockets via SCM_RIGHTS, on
 * socketpair() pairs, connect/accept pairs, and on long-lived connections
 * where the fd is sent well after the initial handshake traffic.
 *
 * Real-world victim: systemd-logind pushes the session leader's PIDFD to
 * PID 1 (StartTransientUnit for the session scope) over its long-lived bus
 * and varlink connections. On the device this failed with EPIPE ("Failed
 * to push leader pidfd for session 'c1', ignoring: Broken pipe"), the
 * session scope creation came back "Connection reset by peer", and
 * dbus-broker exited 1 -- leaving resolved/logind nameless on the bus and
 * every D-Bus call to them timing out.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>

#include "test_common.h"

// Alpine 3.11's kernel headers (the i386 root the e2e suite builds against)
// predate pidfd_open, so SYS_pidfd_open is undeclared there -- a hard error
// that stops setup-regressions.sh, which builds every test before running
// any. Every other pidfd test already carries this guard; this one did not.
#ifndef SYS_pidfd_open
# ifdef __NR_pidfd_open
#  define SYS_pidfd_open __NR_pidfd_open
# else
#  define SYS_pidfd_open 434 // "common" number on i386 and amd64
# endif
#endif

static void check(const char *label, long got, long exp) {
    int bad = got != exp;
    test_log_if(bad, "%s: got=%ld exp=%ld errno=%d (%s)\n",
                label, got, exp, errno, strerror(errno));
    if (bad)
        failf(label, (uint64_t) got, (uint64_t) errno, 0, (uint64_t) exp, 0, 0);
}

static int send_one_fd(int sock, int fd) {
    char dummy = 'x';
    struct iovec iov = { .iov_base = &dummy, .iov_len = 1 };
    char ctrl[CMSG_SPACE(sizeof(int))];
    memset(ctrl, 0, sizeof(ctrl));
    struct msghdr mh = {
        .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = ctrl, .msg_controllen = sizeof(ctrl),
    };
    struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof(int));
    return sendmsg(sock, &mh, 0) < 0 ? -errno : 0;
}

static int recv_one_fd(int sock) {
    char dummy;
    struct iovec iov = { .iov_base = &dummy, .iov_len = 1 };
    char ctrl[CMSG_SPACE(sizeof(int))];
    struct msghdr mh = {
        .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = ctrl, .msg_controllen = sizeof(ctrl),
    };
    if (recvmsg(sock, &mh, 0) < 0)
        return -errno;
    struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
    if (c == NULL || c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS)
        return -9999; /* no fd attached */
    int fd;
    memcpy(&fd, CMSG_DATA(c), sizeof(int));
    return fd;
}

/* The received fd must still BE a pidfd: poll/waitid-able. Cheap check:
 * fstat succeeds and pidfd_send_signal(sig=0) accepts it. */
static void check_received_pidfd(const char *label, int fd) {
    char lab[64];
    snprintf(lab, sizeof lab, "%s.fd_valid", label);
    check(lab, fd >= 0, 1);
    if (fd < 0)
        return;
    struct stat st;
    snprintf(lab, sizeof lab, "%s.fstat", label);
    check(lab, fstat(fd, &st), 0);
    snprintf(lab, sizeof lab, "%s.send_signal0", label);
    check(lab, syscall(SYS_pidfd_send_signal, fd, 0, NULL, 0), 0);
    close(fd);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    signal(SIGPIPE, SIG_IGN);
    alarm(test_watchdog_secs(30));

    int pidfd = (int) syscall(SYS_pidfd_open, getpid(), 0);
    check("pidfd_open", pidfd >= 0, 1);
    if (pidfd < 0)
        return finish_suite("scm_rights_pidfd");

    /* socketpair */
    int sp[2];
    check("socketpair", socketpair(AF_UNIX, SOCK_STREAM, 0, sp), 0);
    check("sp.send", send_one_fd(sp[0], pidfd), 0);
    check_received_pidfd("sp", recv_one_fd(sp[1]));

    /* connect/accept pair, both directions */
    struct sockaddr_un sun = { .sun_family = AF_UNIX };
    snprintf(sun.sun_path, sizeof sun.sun_path, "/tmp/scmp.%d.sock", getpid());
    unlink(sun.sun_path);
    int ls = socket(AF_UNIX, SOCK_STREAM, 0);
    check("listen.socket", ls >= 0, 1);
    check("listen.bind", bind(ls, (struct sockaddr *) &sun, sizeof sun), 0);
    check("listen.listen", listen(ls, 8), 0);
    int cs = socket(AF_UNIX, SOCK_STREAM, 0);
    check("client.connect", connect(cs, (struct sockaddr *) &sun, sizeof sun), 0);
    int as = accept(ls, NULL, NULL);
    check("server.accept", as >= 0, 1);

    check("pair.send_c2s", send_one_fd(cs, pidfd), 0);
    check_received_pidfd("pair.c2s", recv_one_fd(as));
    check("pair.send_s2c", send_one_fd(as, pidfd), 0);
    check_received_pidfd("pair.s2c", recv_one_fd(cs));

    /* Long-lived connection: exchange plain traffic first, then push the
     * fd, in both directions (logind's pattern). */
    char buf[16];
    for (int i = 0; i < 5; i++) {
        check("late.ping", write(cs, "ping", 4), 4);
        check("late.ping_read", read(as, buf, sizeof buf), 4);
        check("late.pong", write(as, "pong", 4), 4);
        check("late.pong_read", read(cs, buf, sizeof buf), 4);
    }
    check("late.send_s2c", send_one_fd(as, pidfd), 0);
    check_received_pidfd("late.s2c", recv_one_fd(cs));
    check("late.send_c2s", send_one_fd(cs, pidfd), 0);
    check_received_pidfd("late.c2s", recv_one_fd(as));

    close(cs);
    close(as);
    close(ls);
    close(sp[0]);
    close(sp[1]);

    /* DGRAM with msg_name and no connect() -- the sd_notify FDSTORE
     * pattern (logind pushing session-leader pidfds, udevd its inotify
     * fd, to PID 1's notify socket). Failed with EPIPE when SCM_RIGHTS
     * required a connected unix_peer. */
    struct sockaddr_un dsun = { .sun_family = AF_UNIX };
    snprintf(dsun.sun_path, sizeof dsun.sun_path, "/tmp/scmpd.%d.sock", getpid());
    unlink(dsun.sun_path);
    int dr = socket(AF_UNIX, SOCK_DGRAM, 0);
    check("dgram.rsocket", dr >= 0, 1);
    check("dgram.bind", bind(dr, (struct sockaddr *) &dsun, sizeof dsun), 0);
    int ds = socket(AF_UNIX, SOCK_DGRAM, 0);
    check("dgram.ssocket", ds >= 0, 1);
    {
        char dummy = 'x';
        struct iovec iov = { .iov_base = &dummy, .iov_len = 1 };
        char ctrl[CMSG_SPACE(sizeof(int))];
        memset(ctrl, 0, sizeof(ctrl));
        struct msghdr mh = {
            .msg_name = &dsun, .msg_namelen = sizeof(dsun),
            .msg_iov = &iov, .msg_iovlen = 1,
            .msg_control = ctrl, .msg_controllen = sizeof(ctrl),
        };
        struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &pidfd, sizeof(int));
        errno = 0;
        check("dgram.sendmsg_unconnected", sendmsg(ds, &mh, 0) < 0 ? -errno : 0, 0);
    }
    check_received_pidfd("dgram", recv_one_fd(dr));

    /* And a plain-data datagram after the fd-carrying one still arrives
     * intact (wire framing must stay consistent). */
    check("dgram.plain_send",
          sendto(ds, "hello", 5, 0, (struct sockaddr *) &dsun, sizeof dsun), 5);
    char pbuf[16];
    check("dgram.plain_recv", recv(dr, pbuf, sizeof pbuf, 0), 5);
    check("dgram.plain_data", memcmp(pbuf, "hello", 5), 0);

    close(ds);
    close(dr);
    close(pidfd);
    unlink(sun.sun_path);
    unlink(dsun.sun_path);
    return finish_suite("scm_rights_pidfd");
}
