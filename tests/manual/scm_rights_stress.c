// AF_UNIX SCM_RIGHTS fd-passing must never desync AOK's two-channel
// bookkeeping (a host "sentinel" fd carrying the message + an internal
// struct-scm queue carrying the real guest fds). A desync where the host
// sentinel arrives with no queued scm used to abort the WHOLE emulator via
// assert(!list_empty(&sock->socket.unix_scm)) in sys_recvmsg_guest_abi --
// hit mid-boot on amd64 Arch, where systemd/dbus pass fds heavily. This
// test hammers the fd-passing paths that stress that accounting:
//   - many fds in one message, many messages
//   - fd-carrying messages interleaved with plain-data messages
//   - bidirectional passing over one socketpair
//   - passing over a connect()/accept() stream pair
// and verifies every received fd actually refers to the same file (via
// a written sentinel byte), so a silently-dropped or mismatched fd fails
// loudly rather than passing vacuously. Also passes on real Linux.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_common.h"

// Make a fresh temp file whose first byte is `tag`, return its fd.
static int make_tagged_fd(char tag) {
    char path[] = "/tmp/scm_stress.XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0)
        return -1;
    unlink(path);
    if (write(fd, &tag, 1) != 1)
        return -1;
    return fd;
}

static int fd_tag(int fd) {
    char c = 0;
    if (pread(fd, &c, 1, 0) != 1)
        return -1;
    return (unsigned char) c;
}

static int send_fds(int sock, const int *fds, int n, const char *data, size_t datalen) {
    char cbuf[CMSG_SPACE(64 * sizeof(int))];
    memset(cbuf, 0, sizeof(cbuf));
    struct iovec iov = {.iov_base = (void *) data, .iov_len = datalen};
    struct msghdr msg = {
        .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = cbuf, .msg_controllen = CMSG_SPACE(n * sizeof(int)),
    };
    struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(n * sizeof(int));
    memcpy(CMSG_DATA(c), fds, n * sizeof(int));
    return sendmsg(sock, &msg, 0) < 0 ? -1 : 0;
}

// Receive one message; collect any passed fds into out_fds (cap out_cap),
// return count via *n_out. Returns bytes of data, or -1 on error.
static ssize_t recv_fds(int sock, char *data, size_t datalen, int *out_fds, int out_cap, int *n_out) {
    char cbuf[CMSG_SPACE(64 * sizeof(int))];
    struct iovec iov = {.iov_base = data, .iov_len = datalen};
    struct msghdr msg = {
        .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = cbuf, .msg_controllen = sizeof(cbuf),
    };
    ssize_t r = recvmsg(sock, &msg, 0);
    *n_out = 0;
    if (r < 0)
        return -1;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c != NULL; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            int cnt = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            int *p = (int *) CMSG_DATA(c);
            for (int i = 0; i < cnt && *n_out < out_cap; i++)
                out_fds[(*n_out)++] = p[i];
        }
    }
    return r;
}

// Round-trip `rounds` fd-messages over `sender`->`receiver`, each carrying
// `nfds` freshly-tagged fds, interleaved every other round with a plain
// data-only message (no fds). Verifies each received fd's tag.
static int exchange(int sender, int receiver, int rounds, int nfds) {
    for (int r = 0; r < rounds; r++) {
        if ((r & 1) == 0) {
            // fd-carrying message
            int fds[64];
            char tags[64];
            for (int i = 0; i < nfds; i++) {
                char tag = (char) (1 + ((r + i) % 250));
                tags[i] = tag;
                fds[i] = make_tagged_fd(tag);
                if (fds[i] < 0) { perror("make_tagged_fd"); return -1; }
            }
            if (send_fds(sender, fds, nfds, "F", 1) != 0) { perror("send_fds"); return -1; }
            for (int i = 0; i < nfds; i++) close(fds[i]);

            char data[8];
            int got[64], gn = 0;
            if (recv_fds(receiver, data, sizeof(data), got, 64, &gn) < 0) { perror("recv_fds"); return -1; }
            if (gn != nfds) {
                printf("FAIL: round %d got %d fds, want %d\n", r, gn, nfds);
                return -1;
            }
            for (int i = 0; i < gn; i++) {
                if (fd_tag(got[i]) != (unsigned char) tags[i]) {
                    printf("FAIL: round %d fd %d tag=%d want=%d (fd mismatch/desync)\n",
                           r, i, fd_tag(got[i]), (unsigned char) tags[i]);
                    return -1;
                }
                close(got[i]);
            }
        } else {
            // plain data message, no fds
            if (send(sender, "D", 1, 0) != 1) { perror("send"); return -1; }
            char data[8];
            int got[64], gn = 0;
            if (recv_fds(receiver, data, sizeof(data), got, 64, &gn) < 0) { perror("recv"); return -1; }
            if (gn != 0) {
                printf("FAIL: round %d plain-data message carried %d spurious fds\n", r, gn);
                return -1;
            }
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(30));

    // 1) socketpair, one direction, many fds per message, many rounds.
    int sp[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) != 0) { perror("socketpair"); return 1; }
    if (exchange(sp[0], sp[1], 200, 3) != 0) failures_total++;
    else test_logf("socketpair one-way: ok\n");

    // 2) socketpair, bidirectional in the same loop (both ends send + recv).
    for (int r = 0; r < 100 && failures_total == 0; r++) {
        int a = make_tagged_fd((char) (1 + (r % 250)));
        int b = make_tagged_fd((char) (1 + ((r + 125) % 250)));
        if (a < 0 || b < 0) { perror("mk"); failures_total++; break; }
        if (send_fds(sp[0], &a, 1, "F", 1) != 0 || send_fds(sp[1], &b, 1, "F", 1) != 0) {
            perror("send_fds bidi"); failures_total++; break;
        }
        close(a); close(b);
        char d[8]; int g0[4], g1[4], n0 = 0, n1 = 0;
        recv_fds(sp[1], d, sizeof(d), g0, 4, &n0);
        recv_fds(sp[0], d, sizeof(d), g1, 4, &n1);
        if (n0 != 1 || n1 != 1) {
            printf("FAIL: bidi round %d got n0=%d n1=%d (want 1,1)\n", r, n0, n1);
            failures_total++; break;
        }
        if (fd_tag(g0[0]) != (unsigned char)(1 + (r % 250)) ||
            fd_tag(g1[0]) != (unsigned char)(1 + ((r + 125) % 250))) {
            printf("FAIL: bidi round %d fd tag mismatch\n", r);
            failures_total++; break;
        }
        close(g0[0]); close(g1[0]);
    }
    if (failures_total == 0) test_logf("socketpair bidirectional: ok\n");
    close(sp[0]); close(sp[1]);

    // 3) connect()/accept() stream pair.
    char path[] = "/tmp/scm_stress_srv.XXXXXX";
    int tmpfd = mkstemp(path);
    if (tmpfd >= 0) { close(tmpfd); unlink(path); }
    int lsn = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    if (bind(lsn, (struct sockaddr *) &addr, sizeof(addr)) != 0 || listen(lsn, 4) != 0) {
        perror("bind/listen"); return 1;
    }
    int cli = socket(AF_UNIX, SOCK_STREAM, 0);
    if (connect(cli, (struct sockaddr *) &addr, sizeof(addr)) != 0) { perror("connect"); return 1; }
    int srv = accept(lsn, NULL, NULL);
    if (srv < 0) { perror("accept"); return 1; }
    if (exchange(cli, srv, 120, 2) != 0) failures_total++;
    else test_logf("connect/accept stream: ok\n");
    if (exchange(srv, cli, 120, 4) != 0) failures_total++;  // other direction
    else test_logf("connect/accept reverse: ok\n");
    close(cli); close(srv); close(lsn); unlink(path);

    // 4) connected DGRAM socketpair -- exercises the in-band cred-header
    // path (AOK prepends a sender-cred header to every unix datagram) while
    // ALSO passing SCM_RIGHTS, the combination most likely to desync the
    // fd-passing accounting. Interleave fd and plain-data datagrams.
    int dp[2];
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, dp) != 0) {
        perror("socketpair dgram"); return 1;
    }
    if (exchange(dp[0], dp[1], 160, 2) != 0) failures_total++;
    else test_logf("dgram socketpair fd-pass: ok\n");
    close(dp[0]); close(dp[1]);

    // 5) MSG_PEEK on a message carrying fds, then a real recv -- exactly what
    // dbus/systemd do (peek to size the message, then read it for real).
    // The peek must not consume the sender's message, and the following real
    // recv must still deliver its data. On Linux each peek also duplicates
    // the fds into the receiver; AOK need only avoid crashing and deliver the
    // data intact on the real recv.
    int pk[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pk) != 0) { perror("socketpair peek"); return 1; }
    int peek_ok = 1;
    for (int r = 0; r < 60; r++) {
        int f = make_tagged_fd((char) (1 + (r % 250)));
        if (f < 0) { perror("mk"); peek_ok = 0; break; }
        if (send_fds(pk[0], &f, 1, "P", 1) != 0) { perror("send_fds peek"); peek_ok = 0; break; }
        close(f);

        // Peek (may duplicate the fd on Linux; just close whatever comes back).
        char d[8]; int pf[4], pn = 0;
        struct iovec iov = {.iov_base = d, .iov_len = sizeof(d)};
        char cbuf[CMSG_SPACE(4 * sizeof(int))];
        struct msghdr pm = {.msg_iov = &iov, .msg_iovlen = 1,
                            .msg_control = cbuf, .msg_controllen = sizeof(cbuf)};
        ssize_t pr = recvmsg(pk[1], &pm, MSG_PEEK);
        if (pr < 0) { perror("recvmsg peek"); peek_ok = 0; break; }
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&pm); c != NULL; c = CMSG_NXTHDR(&pm, c))
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
                int cnt = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
                for (int i = 0; i < cnt; i++) { int gf; memcpy(&gf, CMSG_DATA(c) + i * sizeof(int), sizeof(int)); pf[pn++] = gf; }
            }
        for (int i = 0; i < pn; i++) close(pf[i]);

        // Real recv: both the data AND the fd must survive the peek.
        char d2[8]; int rf[4], rn = 0;
        ssize_t rr = recv_fds(pk[1], d2, sizeof(d2), rf, 4, &rn);
        if (rr != 1 || d2[0] != 'P') {
            printf("FAIL: peek round %d real recv got %zd bytes data='%c' (want 1,'P')\n",
                   r, rr, rr > 0 ? d2[0] : '?');
            peek_ok = 0; break;
        }
        if (rn != 1) {
            printf("FAIL: peek round %d real recv got %d fds (want 1) -- peek consumed the fd\n", r, rn);
            peek_ok = 0; break;
        }
        if (fd_tag(rf[0]) != (unsigned char) (1 + (r % 250))) {
            printf("FAIL: peek round %d real-recv fd tag=%d want=%d\n",
                   r, fd_tag(rf[0]), (unsigned char) (1 + (r % 250)));
            peek_ok = 0; break;
        }
        for (int i = 0; i < rn; i++) close(rf[i]);
    }
    if (!peek_ok) failures_total++;
    else test_logf("MSG_PEEK then recv with fds: ok\n");
    close(pk[0]); close(pk[1]);

    return finish_suite("scm_rights_stress");
}
