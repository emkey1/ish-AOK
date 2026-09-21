// checkpoint_sockpair.c -- local sockets across a checkpoint, the way udevd
// and dbus-daemon hold them. Driven by checkpoint_sockpair.sh, which sets
// ISH_FORCE_SEQPACKET_EPERM so the CLI refuses AF_UNIX SOCK_SEQPACKET the way
// iOS's sandbox does.
//
// What broke on device: udevd's control socket (SEQPACKET, listening) came back
// hung up, because the rebuild asked for SEQPACKET bare and got EPERM; and its
// worker socketpair came back hung up at both ends, because every connected
// socket was treated as having a peer outside the image. epoll then reported
// both readable and hung up, for ever, and udevd spun at most of a core.
//
// And what a pair has queued when the image is written travels with it, the
// way a pipe's leftover bytes do: datagrams as datagrams, in order, and a
// stream's bytes in both directions. The save reads the queues while the
// guest is frozen, and it must leave them as it found them -- the run that
// saved goes on, and the script checks that run too.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

static void check(const char *what, int ok, const char *detail) {
    printf("%s %s: %s\n", ok ? "OK" : "FAIL", what, detail);
    fflush(stdout);
}

// Fill one end of a pair until it will take no more, and say how much went in:
// a queue that was full when the image was written must fit when it is read.
static long fill(int fd, int dgram) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    char b[512];
    long total = 0;
    for (unsigned i = 0;; i++) {
        memset(b, 'a' + i % 26, sizeof(b));
        ssize_t n = send(fd, b, dgram ? 100 : sizeof(b), 0);
        if (n <= 0)
            break;
        total += n;
    }
    return total;
}
static long drain(int fd, int dgram, long *msgs, int *in_order) {
    char b[512];
    long total = 0;
    *msgs = 0;
    *in_order = 1;
    for (;;) {
        ssize_t n = recv(fd, b, sizeof(b), MSG_DONTWAIT);
        if (n <= 0)
            break;
        for (ssize_t k = 0; k < n; k++) {
            long pos = dgram ? *msgs : (total + k) / 512;
            if (b[k] != 'a' + pos % 26)
                *in_order = 0;
        }
        total += n;
        (*msgs)++;
    }
    return total;
}

static int listen_on(const char *path, int type) {
    int s = socket(AF_UNIX, type, 0);
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    strncpy(a.sun_path, path, sizeof(a.sun_path) - 1);
    unlink(path);
    if (bind(s, (struct sockaddr *) &a, sizeof(a)) || listen(s, 4))
        return -1;
    return s;
}
static int connect_to(const char *path, int type) {
    int s = socket(AF_UNIX, type, 0);
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    strncpy(a.sun_path, path, sizeof(a.sun_path) - 1);
    return connect(s, (struct sockaddr *) &a, sizeof(a)) == 0 ? s : -1;
}

int main(void) {
    int ctl = listen_on("/run/ckctl", SOCK_SEQPACKET);   // udevd's control socket
    int pair[2];
    socketpair(AF_UNIX, SOCK_DGRAM, 0, pair);           // udevd's worker_watch
    send(pair[1], "one", 3, 0);                         // queued at the save
    send(pair[1], "two!", 4, 0);
    int sp[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sp);
    write(sp[1], "still-here", 10);                     // both directions
    write(sp[0], "back", 4);
    fcntl(sp[0], F_SETFL, fcntl(sp[0], F_GETFL) | O_NONBLOCK);  // as dbus does
    int fs[2], fd2[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, fs);
    socketpair(AF_UNIX, SOCK_DGRAM, 0, fd2);
    long fs_in = fill(fs[1], 0), fd_in = fill(fd2[1], 1);
    // One whose owner asked for bigger buffers than the host's defaults, which
    // is what a rebuilt pair gets.
    int bs[2], big = 65536;
    socketpair(AF_UNIX, SOCK_STREAM, 0, bs);
    setsockopt(bs[1], SOL_SOCKET, SO_SNDBUF, &big, sizeof(big));
    setsockopt(bs[0], SOL_SOCKET, SO_RCVBUF, &big, sizeof(big));
    long bs_in = fill(bs[1], 0);
    int srv = listen_on("/run/cksrv", SOCK_STREAM);
    pid_t child = fork();
    if (child == 0) {
        int c = connect_to("/run/cksrv", SOCK_STREAM);
        for (int i = 0; i < 8; i++) sleep(1);           // checkpoint in here
        write(c, "ping", 4);
        char b[8] = {0};
        read(c, b, 4);
        printf("CHILD-GOT=[%s]\n", b);
        fflush(stdout);
        _exit(0);
    }
    int conn = accept(srv, NULL, NULL);
    for (int i = 0; i < 6; i++) sleep(1);               // checkpoint in here

    char d[160];
    // 1. The listener still listens.
    int cl = connect_to("/run/ckctl", SOCK_SEQPACKET);
    struct pollfd lp = {.fd = ctl, .events = POLLIN};
    int lpr = poll(&lp, 1, 2000);
    int acc = lpr > 0 ? accept(ctl, NULL, NULL) : -1;
    int acc_err = acc < 0 ? errno : 0;
    int sent = cl >= 0 ? (int) send(cl, "hello", 5, 0) : -1;
    char lb[8] = {0};
    int got = acc >= 0 ? (int) recv(acc, lb, sizeof(lb) - 1, 0) : -1;
    snprintf(d, sizeof(d), "connect=%d accept=%d errno=%d sent=%d got=%d [%s]",
             cl >= 0, acc >= 0, acc_err, sent, got, lb);
    check("LISTENER", cl >= 0 && acc >= 0 && got == 5 && strcmp(lb, "hello") == 0, d);

    // 2. What the pairs had queued: two datagrams, one at a time and in
    // order, then nothing; and a stream's bytes each way.
    char q1[8] = {0}, q2[8] = {0}, q3[8];
    int n1 = (int) recv(pair[0], q1, sizeof(q1) - 1, MSG_DONTWAIT);
    int n2 = (int) recv(pair[0], q2, sizeof(q2) - 1, MSG_DONTWAIT);
    int n3 = (int) recv(pair[0], q3, sizeof(q3), MSG_DONTWAIT);
    int e3 = n3 < 0 ? errno : 0;
    snprintf(d, sizeof(d), "[%s]=%d [%s]=%d then %d errno=%d", q1, n1, q2, n2, n3, e3);
    check("QUEUED-DGRAM", n1 == 3 && !strcmp(q1, "one") && n2 == 4 &&
          !strcmp(q2, "two!") && n3 < 0 && e3 == EAGAIN, d);
    char s0[16] = {0}, s1[16] = {0};
    int r0 = (int) recv(sp[0], s0, sizeof(s0) - 1, MSG_DONTWAIT);
    int r1 = (int) recv(sp[1], s1, sizeof(s1) - 1, MSG_DONTWAIT);
    snprintf(d, sizeof(d), "[%s]=%d [%s]=%d", s0, r0, s1, r1);
    check("QUEUED-STREAM", r0 == 10 && !strcmp(s0, "still-here") &&
          r1 == 4 && !strcmp(s1, "back"), d);

    // And pairs filled to the brim.
    long fm, dm;
    int fo, dord;
    long fs_out = drain(fs[0], 0, &fm, &fo);
    long fd_out = drain(fd2[0], 1, &dm, &dord);
    snprintf(d, sizeof(d), "stream %ld of %ld%s, dgram %ld of %ld in %ld%s",
             fs_out, fs_in, fo ? "" : " OUT-OF-ORDER", fd_out, fd_in, dm,
             dord ? "" : " OUT-OF-ORDER");
    check("QUEUED-FULL", fs_in > 0 && fs_out == fs_in && fo &&
          fd_in > 0 && fd_out == fd_in && dm == fd_in / 100 && dord, d);
    long bm;
    int bo;
    long bs_out = drain(bs[0], 0, &bm, &bo);
    snprintf(d, sizeof(d), "stream %ld of %ld%s", bs_out, bs_in, bo ? "" : " OUT-OF-ORDER");
    check("QUEUED-BIG", bs_in > fs_in && bs_out == bs_in && bo, d);

    // Each end keeps the guest's own O_NONBLOCK, which is not the host's: a
    // socket's host descriptor is non-blocking whatever the guest asked. A
    // non-blocking end that came back blocking hung dbus-daemon in recvmsg.
    int nb_sp0 = !!(fcntl(sp[0], F_GETFL) & O_NONBLOCK);
    int nb_sp1 = !!(fcntl(sp[1], F_GETFL) & O_NONBLOCK);
    int nb_ctl = !!(fcntl(ctl, F_GETFL) & O_NONBLOCK);
    alarm(4);                                           // a hang is the failure
    char e[4];
    int er = (int) recv(sp[0], e, sizeof(e), 0);
    int ee = er < 0 ? errno : 0;
    alarm(0);
    snprintf(d, sizeof(d), "nonblock sp0=%d sp1=%d listener=%d; empty recv=%d errno=%d",
             nb_sp0, nb_sp1, nb_ctl, er, ee);
    check("FLAGS", nb_sp0 && !nb_sp1 && !nb_ctl && er < 0 && ee == EAGAIN, d);

    // 3. The datagram pair: quiet until written to, then carries a datagram.
    struct pollfd pp = {.fd = pair[0], .events = POLLIN};
    int idle = poll(&pp, 1, 0);
    int idle_rev = pp.revents;
    send(pair[1], "dgram", 5, 0);
    char pb[8] = {0};
    pp.revents = 0;
    int ready = poll(&pp, 1, 2000);
    int pg = ready > 0 ? (int) recv(pair[0], pb, sizeof(pb) - 1, MSG_DONTWAIT) : -1;
    snprintf(d, sizeof(d), "idle-poll=%d revents=%#x then got=%d [%s]", idle, idle_rev, pg, pb);
    check("SOCKETPAIR", idle == 0 && pg == 5 && strcmp(pb, "dgram") == 0, d);

    // 4. The connection between two processes, both ways, with its peer's
    // credentials.
    char cb[8] = {0};
    int cg = (int) read(conn, cb, 4);
    write(conn, "pong", 4);
    struct ucred cr = {0};
    socklen_t crl = sizeof(cr);
    getsockopt(conn, SOL_SOCKET, SO_PEERCRED, &cr, &crl);
    int st = 0;
    waitpid(child, &st, 0);
    snprintf(d, sizeof(d), "got=%d [%s] peer-pid=%d want=%d", cg, cb, (int) cr.pid, (int) child);
    check("CONNECTION", cg == 4 && strcmp(cb, "ping") == 0 && cr.pid == child, d);
    return 0;
}
