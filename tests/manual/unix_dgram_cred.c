// Per-datagram SCM_CREDENTIALS on AF_UNIX SOCK_DGRAM sockets. Linux attaches
// the SENDER's credentials to every unix datagram; a receiver with
// SO_PASSCRED set gets them as an SCM_CREDENTIALS cmsg carrying the sender's
// real pid. AOK only tracked the CONNECTED PEER's cred, which is undefined
// for many-senders-one-receiver datagram sockets -- exactly the shape of
// systemd's /run/systemd/notify. Without per-datagram creds, systemd dropped
// every sd_notify READY=1 ("Got notification datagram lacking valid
// credential information, ignoring"), so every Type=notify service
// (journald, udevd, userdbd, ...) hung in "activating" until its start
// timeout, stalling the whole boot. Now the sender's guest ucred travels
// in-band with each datagram on the emulator's host-side socket (struct
// unix_dgram_cred_hdr, fs/sock.c) and is stripped/delivered on receive.
//
// Covers: the exact systemd notify pattern (unbound dgram sender ->
// sendto(path) -> receiver recvmsg with SO_PASSCRED sees SCM_CREDENTIALS
// with the SENDER's pid -- from a forked child, so it provably differs from
// the receiver's); payload integrity (header stripped); the bare recvfrom
// surface (no cmsg, payload still clean); and a sender using plain write()
// on a connect()ed dgram socket. Also passes on real Linux.
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_common.h"

#ifndef SCM_CREDENTIALS
#define SCM_CREDENTIALS 2
#endif

static int recv_with_cred(int sock, char *buf, size_t buflen, struct ucred *cred_out,
        int *have_cred) {
    struct iovec iov = {.iov_base = buf, .iov_len = buflen};
    char control[256];
    struct msghdr msg = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = control,
        .msg_controllen = sizeof(control),
    };
    ssize_t n = recvmsg(sock, &msg, 0);
    if (n < 0)
        return -1;
    *have_cred = 0;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c != NULL; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_CREDENTIALS) {
            memcpy(cred_out, CMSG_DATA(c), sizeof(*cred_out));
            *have_cred = 1;
        }
    }
    return (int) n;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(10));

    char dir[] = "/tmp/unix_dgram_cred.XXXXXX";
    if (mkdtemp(dir) == NULL) {
        perror("mkdtemp");
        return 1;
    }
    char sockpath[300];
    snprintf(sockpath, sizeof(sockpath), "%s/notify", dir);

    int receiver = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (receiver < 0) { perror("socket"); return 1; }
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    strncpy(addr.sun_path, sockpath, sizeof(addr.sun_path) - 1);
    if (bind(receiver, (struct sockaddr *) &addr, sizeof(addr)) != 0) {
        perror("bind");
        return 1;
    }
    int one = 1;
    if (setsockopt(receiver, SOL_SOCKET, SO_PASSCRED, &one, sizeof(one)) != 0) {
        printf("FAIL: setsockopt(SO_PASSCRED) -> %s\n", strerror(errno));
        failures_total++;
    }

    // Child = the "service": unbound dgram socket, sendto(path) -- exactly
    // what sd_notify does. Its pid necessarily differs from the receiver's,
    // so a passing check proves per-datagram sender attribution, not a
    // reflected receiver/peer cred.
    pid_t child = fork();
    if (child < 0) { perror("fork"); return 1; }
    if (child == 0) {
        int s = socket(AF_UNIX, SOCK_DGRAM, 0);
        if (s < 0) _exit(1);
        const char *payload = "READY=1";
        if (sendto(s, payload, strlen(payload), 0,
                   (struct sockaddr *) &addr, sizeof(addr)) != (ssize_t) strlen(payload))
            _exit(2);
        // Second datagram via connect() + plain write(), the other send path.
        if (connect(s, (struct sockaddr *) &addr, sizeof(addr)) != 0)
            _exit(3);
        const char *payload2 = "STATUS=ok";
        if (write(s, payload2, strlen(payload2)) != (ssize_t) strlen(payload2))
            _exit(4);
        _exit(0);
    }
    int status = 0;
    waitpid(child, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("FAIL: sender child exited %d\n",
               WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        return 1;
    }

    // Datagram 1: sendto() path, received via recvmsg with cred cmsg.
    char buf[128] = "";
    struct ucred cred = {};
    int have_cred = 0;
    int n = recv_with_cred(receiver, buf, sizeof(buf) - 1, &cred, &have_cred);
    buf[n < 0 ? 0 : n] = '\0';
    test_logf("dgram 1: n=%d payload=\"%s\" have_cred=%d pid=%d (sender=%d, me=%d)\n",
              n, buf, have_cred, have_cred ? (int) cred.pid : -1,
              (int) child, (int) getpid());
    if (n != 7 || strcmp(buf, "READY=1") != 0) {
        printf("FAIL: dgram 1 payload mangled: n=%d \"%s\" (want \"READY=1\")\n", n, buf);
        failures_total++;
    }
    if (!have_cred) {
        printf("FAIL: dgram 1 arrived without SCM_CREDENTIALS -- exactly what made "
               "systemd drop every sd_notify READY=1\n");
        failures_total++;
    } else if (cred.pid != child) {
        printf("FAIL: dgram 1 cred pid=%d, want the SENDER's (%d), not mine (%d)\n",
               (int) cred.pid, (int) child, (int) getpid());
        failures_total++;
    }

    // Datagram 2: connect()+write() path, received via bare recvfrom (no
    // cmsg surface; the in-band header must still be stripped cleanly).
    char buf2[128] = "";
    ssize_t n2 = recvfrom(receiver, buf2, sizeof(buf2) - 1, 0, NULL, NULL);
    buf2[n2 < 0 ? 0 : n2] = '\0';
    test_logf("dgram 2: n=%zd payload=\"%s\"\n", n2, buf2);
    if (n2 != 9 || strcmp(buf2, "STATUS=ok") != 0) {
        printf("FAIL: dgram 2 payload mangled: n=%zd \"%s\" (want \"STATUS=ok\")\n",
               n2, buf2);
        failures_total++;
    }

    // Datagram 3: a small message received with MSG_TRUNC and a large buffer
    // must NOT come back with MSG_TRUNC set. AOK prepends a 16-byte in-band
    // cred header to every unix datagram; a naive implementation lets the
    // host's own MSG_TRUNC (computed against header+buffer, and on macOS
    // echoed from the input flag) leak through -- and systemd treats a
    // MSG_TRUNC on its notify socket as "Received notify message exceeded
    // maximum size", dropping every sd_notify READY=1 so Type=notify services
    // (journald, udevd, ...) never leave "activating" and boot wedges. Here
    // the payload obviously fits, so MSG_TRUNC must be clear.
    {
        int s = socket(AF_UNIX, SOCK_DGRAM, 0);
        if (s < 0) { perror("socket dgram3"); failures_total++; }
        else {
            const char *payload = "READY=1";
            if (sendto(s, payload, strlen(payload), 0,
                       (struct sockaddr *) &addr, sizeof(addr)) != (ssize_t) strlen(payload)) {
                perror("sendto dgram3");
                failures_total++;
            } else {
                char big[4096];
                struct iovec iov = {.iov_base = big, .iov_len = sizeof(big)};
                struct msghdr m = {.msg_iov = &iov, .msg_iovlen = 1};
                ssize_t n3 = recvmsg(receiver, &m, MSG_TRUNC);
                test_logf("dgram 3: n=%zd msg_flags=%#x\n", n3, m.msg_flags);
                if (n3 != 7) {
                    printf("FAIL: dgram 3 got n=%zd (want 7)\n", n3);
                    failures_total++;
                }
                if (m.msg_flags & MSG_TRUNC) {
                    printf("FAIL: dgram 3 spurious MSG_TRUNC on a %zd-byte message that fit a "
                           "4096 buffer -- this is what makes systemd drop sd_notify READY=1\n", n3);
                    failures_total++;
                }
            }
            close(s);
        }
    }

    close(receiver);
    unlink(sockpath);
    rmdir(dir);

    return finish_suite("unix_dgram_cred");
}
