/*
 * netlink_audit.c — regression lock for minimal NETLINK_AUDIT support.
 *
 * AOK-Workstation's AF_NETLINK socket() used to allow-list only ROUTE/SOCK_DIAG/
 * KOBJECT_UEVENT/GENERIC, so libaudit's audit_open() — a bare
 * socket(AF_NETLINK, SOCK_RAW, NETLINK_AUDIT) — failed with EPROTONOSUPPORT.
 * Every libaudit consumer (PAM, shadow's login/su, sshd, dbus) probes the
 * audit subsystem this way; most treat EPROTONOSUPPORT as "no audit in
 * kernel" and continue, but some paths surface it as
 *     "Failed to connect to audit daemon: Protocol not supported"
 * (observed on-device during a stress-ng sweep; issue #468).
 *
 * The implemented behavior mirrors a CONFIG_AUDIT=y Linux kernel with
 * auditing disabled and no rule engine:
 *   - audit_open() succeeds
 *   - AUDIT_GET returns a struct audit_status reply (enabled=0 initially)
 *     plus an ACK when NLM_F_ACK is set (libaudit always sets it and blocks
 *     in check_ack() waiting for one — a missing ACK hangs the caller)
 *   - user messages (AUDIT_USER, 1100-1199, 2100-2999) are accepted,
 *     dropped, and ACKed with success, exactly like Linux drops user
 *     messages when audit_enabled is off
 *   - AUDIT_SET applies mask-gated fields (an auditd registering its pid
 *     sees it echoed by the next AUDIT_GET)
 *   - unknown message types fail with EINVAL (audit_netlink_ok's default),
 *     removed-legacy AUDIT_LIST/ADD/DEL with EOPNOTSUPP
 *
 * Exits 0 and prints "netlink_audit: PASS" on success.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

#include "test_common.h"

/* Self-contained netlink/audit ABI (kernel-stable values; avoids a
 * linux-headers dependency so this builds under musl-only Alpine too). */
#ifndef AF_NETLINK
#define AF_NETLINK 16
#endif
#define NETLINK_AUDIT_PROTO 9

#define NLMSG_ERROR_T 2
#define NLM_F_REQUEST_F 0x1
#define NLM_F_ACK_F 0x4

#define AUDIT_GET_T 1000
#define AUDIT_SET_T 1001
#define AUDIT_LIST_T 1002
#define AUDIT_USER_LOGIN_T 1112 /* in the AUDIT_FIRST..LAST_USER_MSG range */
#define AUDIT_STATUS_RATE_LIMIT_F 0x0008

struct nl_hdr {
    uint32_t nlmsg_len;
    uint16_t nlmsg_type;
    uint16_t nlmsg_flags;
    uint32_t nlmsg_seq;
    uint32_t nlmsg_pid;
};
struct nl_err { int error; struct nl_hdr msg; };
struct nl_sockaddr {
    uint16_t nl_family;
    uint16_t nl_pad;
    uint32_t nl_pid;
    uint32_t nl_groups;
};
struct audit_status_msg {
    uint32_t mask;
    uint32_t enabled;
    uint32_t failure;
    uint32_t pid;
    uint32_t rate_limit;
    uint32_t backlog_limit;
    uint32_t lost;
    uint32_t backlog;
    uint32_t feature_bitmap;
    uint32_t backlog_wait_time;
    uint32_t backlog_wait_time_actual;
};

#define NL_ALIGN(len) (((len) + 3) & ~3u)
#define NL_HDRLEN ((int) NL_ALIGN(sizeof(struct nl_hdr)))

struct audit_msg_buf {
    struct nl_hdr nlh;
    char data[2048];
};

/* libaudit audit_send(): NLM_F_REQUEST|NLM_F_ACK on everything. */
static int audit_send(int fd, uint16_t type, const void *data, size_t size, uint32_t seq) {
    struct audit_msg_buf req;
    memset(&req, 0, sizeof(req));
    req.nlh.nlmsg_len = NL_HDRLEN + size;
    req.nlh.nlmsg_type = type;
    req.nlh.nlmsg_flags = NLM_F_REQUEST_F | NLM_F_ACK_F;
    req.nlh.nlmsg_seq = seq;
    if (size != 0)
        memcpy(req.data, data, size);
    struct nl_sockaddr addr = { .nl_family = AF_NETLINK };
    ssize_t n = sendto(fd, &req, req.nlh.nlmsg_len, 0,
            (struct sockaddr *) &addr, sizeof(addr));
    return n == (ssize_t) req.nlh.nlmsg_len ? 0 : -1;
}

static ssize_t audit_recv(int fd, struct audit_msg_buf *rep, int peek) {
    return recvfrom(fd, rep, sizeof(*rep),
            MSG_DONTWAIT | (peek ? MSG_PEEK : 0), NULL, NULL);
}

static int wait_readable(int fd) {
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    return poll(&pfd, 1, 2000) == 1 ? 0 : -1;
}

/* libaudit check_ack(): peek; if the head message is NLMSG_ERROR consume it
 * and report its code, otherwise leave the data reply queued and report 0. */
static int check_ack(int fd) {
    if (wait_readable(fd) < 0)
        return -9999; /* distinguishable "no ACK ever arrived" — the hang case */
    struct audit_msg_buf rep;
    if (audit_recv(fd, &rep, 1) < 0)
        return -errno;
    if (rep.nlh.nlmsg_type == NLMSG_ERROR_T) {
        audit_recv(fd, &rep, 0);
        struct nl_err *err = (struct nl_err *) rep.data;
        return err->error;
    }
    return 0;
}

int main(int argc, char **argv) {
    test_init(argc, argv);

    /* audit_open() */
    int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_AUDIT_PROTO);
    if (fd < 0) {
        failf("audit_open", (uint64_t) errno, 0, 0, 0, 0, 0);
        return finish_suite("netlink_audit");
    }
    test_logf("audit_open: fd=%d\n", fd);

    /* audit_open() succeeding is the regression this suite locks (the netlink
     * protocol allow-list used to reject NETLINK_AUDIT with EPROTONOSUPPORT),
     * and it needs no privilege -- so it is checked above, before this guard.
     *
     * The message exchange below does need privilege: audit_netlink_ok()
     * gates control messages (AUDIT_GET/SET) on CAP_AUDIT_CONTROL and user
     * messages on CAP_AUDIT_WRITE, returning EPERM otherwise, and AOK-Workstation's
     * own handler approximates both as euid==0. Skip rather than fail so an
     * unprivileged run stays green -- the on-device suite runs as uid 1000.
     * Same convention as ambient_caps.c / chroot_getcwd.c. */
    if (geteuid() != 0) {
        printf("netlink_audit: SKIP (not privileged: euid=%d)\n", (int) geteuid());
        close(fd);
        return 0;
    }

    /* AUDIT_GET: data reply carrying audit_status, then the requested ACK */
    if (audit_send(fd, AUDIT_GET_T, NULL, 0, 1) < 0)
        failf("send AUDIT_GET", (uint64_t) errno, 0, 0, 0, 0, 0);
    int rc = check_ack(fd);
    if (rc != 0)
        failf("AUDIT_GET check_ack", (uint64_t) (int64_t) rc, 0, 0, 0, 0, 0);
    struct audit_msg_buf rep;
    struct audit_status_msg orig_status = {};
    if (wait_readable(fd) < 0 || audit_recv(fd, &rep, 0) < 0 ||
            rep.nlh.nlmsg_type != AUDIT_GET_T ||
            rep.nlh.nlmsg_len < NL_HDRLEN + sizeof(struct audit_status_msg)) {
        failf("AUDIT_GET reply", rep.nlh.nlmsg_type, rep.nlh.nlmsg_len, 0,
                AUDIT_GET_T, NL_HDRLEN + sizeof(struct audit_status_msg), 0);
    } else {
        memcpy(&orig_status, rep.data, sizeof(orig_status));
        /* No fixed expectation on enabled/pid: on real Linux (or after some
         * other guest program AUDIT_SETs) they can legitimately be nonzero. */
        test_logf("AUDIT_GET: enabled=%u pid=%u rate_limit=%u\n",
                orig_status.enabled, orig_status.pid, orig_status.rate_limit);
    }
    /* drain the trailing ACK the GET also queued */
    while (audit_recv(fd, &rep, 0) >= 0)
        ;

    /* user message — the PAM/login/su/sshd path. Must be ACKed with success;
     * a missing ACK leaves the sender blocked in check_ack() forever. */
    const char msg[] = "op=login acct=\"root\" exe=\"/bin/login\" res=success";
    if (audit_send(fd, AUDIT_USER_LOGIN_T, msg, sizeof(msg), 2) < 0)
        failf("send AUDIT_USER_LOGIN", (uint64_t) errno, 0, 0, 0, 0, 0);
    rc = check_ack(fd);
    if (rc != 0)
        failf("AUDIT_USER_LOGIN ack", (uint64_t) (int64_t) rc, 0, 0, 0, 0, 0);
    else
        test_logf("AUDIT_USER_LOGIN: acked\n");

    /* AUDIT_SET: mask-gated field application, echoed by the next GET.
     * Uses rate_limit (a benign tunable, restored below) rather than
     * AUDIT_STATUS_PID — registering this test as the system's auditd
     * would be disruptive if this ever runs against a real kernel. */
    uint32_t want_rate = orig_status.rate_limit == 1234 ? 4321 : 1234;
    struct audit_status_msg set = { .mask = AUDIT_STATUS_RATE_LIMIT_F, .rate_limit = want_rate };
    if (audit_send(fd, AUDIT_SET_T, &set, sizeof(set), 3) < 0)
        failf("send AUDIT_SET", (uint64_t) errno, 0, 0, 0, 0, 0);
    rc = check_ack(fd);
    if (rc != 0)
        failf("AUDIT_SET ack", (uint64_t) (int64_t) rc, 0, 0, 0, 0, 0);
    if (audit_send(fd, AUDIT_GET_T, NULL, 0, 4) < 0)
        failf("send AUDIT_GET 2", (uint64_t) errno, 0, 0, 0, 0, 0);
    rc = check_ack(fd);
    if (rc != 0)
        failf("AUDIT_GET 2 check_ack", (uint64_t) (int64_t) rc, 0, 0, 0, 0, 0);
    if (wait_readable(fd) == 0 && audit_recv(fd, &rep, 0) >= 0 &&
            rep.nlh.nlmsg_type == AUDIT_GET_T) {
        struct audit_status_msg st;
        memcpy(&st, rep.data, sizeof(st));
        if (st.rate_limit != want_rate)
            failf("AUDIT_SET rate_limit echo", st.rate_limit, 0, 0, want_rate, 0, 0);
        else
            test_logf("AUDIT_SET: rate_limit %u echoed by GET\n", st.rate_limit);
    } else {
        failf("AUDIT_GET 2 reply", 0, 0, 0, 0, 0, 0);
    }
    while (audit_recv(fd, &rep, 0) >= 0)
        ;
    /* restore the original rate limit */
    set.rate_limit = orig_status.rate_limit;
    if (audit_send(fd, AUDIT_SET_T, &set, sizeof(set), 5) == 0)
        check_ack(fd);
    while (audit_recv(fd, &rep, 0) >= 0)
        ;

    /* unknown message type: audit_netlink_ok's default, EINVAL */
    if (audit_send(fd, 1099, NULL, 0, 5) < 0)
        failf("send unknown type", (uint64_t) errno, 0, 0, 0, 0, 0);
    rc = check_ack(fd);
    if (rc != -EINVAL)
        failf("unknown type", (uint64_t) (int64_t) rc, 0, 0, (uint64_t) (int64_t) -EINVAL, 0, 0);
    else
        test_logf("unknown type: EINVAL\n");

    /* removed legacy binary rule API: EOPNOTSUPP before permission checks */
    if (audit_send(fd, AUDIT_LIST_T, NULL, 0, 6) < 0)
        failf("send AUDIT_LIST", (uint64_t) errno, 0, 0, 0, 0, 0);
    rc = check_ack(fd);
    if (rc != -EOPNOTSUPP)
        failf("AUDIT_LIST", (uint64_t) (int64_t) rc, 0, 0, (uint64_t) (int64_t) -EOPNOTSUPP, 0, 0);
    else
        test_logf("AUDIT_LIST: EOPNOTSUPP\n");

    close(fd);
    return finish_suite("netlink_audit");
}
