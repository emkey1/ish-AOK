// unix_seqpacket_msgs: AF_UNIX SOCK_SEQPACKET delivers messages, not bytes.
//
// The triage: "SOCK_SEQPACKET sockets lose message boundaries. Sends of 1, 2
// and 4 bytes arrive as one 7-byte read, and waiting for the second message
// hangs forever." The host has no SEQPACKET (macOS: EPROTONOSUPPORT; the iOS
// sandbox: EPERM), so AOK makes a host SOCK_STREAM and carried the messages
// as bytes. Before the fix 142 of these checks failed on AOK and none on
// Linux 6.12 (x86_64 and -m32 glibc): every receive merged messages, a short
// buffer neither truncated nor said so, MSG_PEEK and MSG_TRUNC and FIONREAD
// counted bytes, a 100000-byte message went out 8192 bytes at a time, a
// message too big for the send buffer was sent in part instead of refused
// with EMSGSIZE, a nonblocking send under backpressure split a message,
// SCM_RIGHTS arrived with the wrong message, and a SEQPACKET client
// connected to a SOCK_STREAM listener (Linux: EPROTOTYPE).
//
// Each case runs on a socketpair, on a path bind/listen/connect/accept and on
// an abstract name. Every receive that could wait for ever -- the triage's
// symptom -- has a 2-second SO_RCVTIMEO, so a lost boundary fails its checks
// instead of hanging the suite.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <time.h>

#include "test_common.h"

enum setup { S_PAIR, S_PATH, S_ABSTRACT, S_COUNT };
static const char *const setup_name[S_COUNT] = {"pair", "path", "abstract"};
static enum setup cur;
static int serial;

static void ck(const char *what, long got, long want) {
    char label[128];
    snprintf(label, sizeof label, "%s %s", setup_name[cur], what);
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-48s got=%#-8lx want=%#lx%s\n", label, got, want,
              got != want ? "   <-- FAIL" : "");
}

static void ck_bytes(const char *what, const void *got, const void *want, size_t n) {
    ck(what, memcmp(got, want, n) == 0, 1);
}

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static socklen_t make_addr(enum setup s, struct sockaddr_un *sun) {
    memset(sun, 0, sizeof *sun);
    sun->sun_family = AF_UNIX;
    if (s == S_ABSTRACT) {
        int n = snprintf(sun->sun_path + 1, sizeof sun->sun_path - 1,
                         "aok-seqpacket-%d-%d", (int) getpid(), serial++);
        return (socklen_t) (offsetof(struct sockaddr_un, sun_path) + 1 + n);
    }
    snprintf(sun->sun_path, sizeof sun->sun_path, "/tmp/aok-seqpacket.%d.%d",
             (int) getpid(), serial++);
    unlink(sun->sun_path);
    return (socklen_t) sizeof *sun;
}

// A receive that would wait for ever -- the triage's symptom -- times out
// instead, so the rest of the checks still run and say what they saw.
static int bound_waits(int sv[2]) {
    struct timeval tv = {.tv_sec = 2};
    for (int i = 0; i < 2; i++)
        setsockopt(sv[i], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    return 0;
}

// sv[0] sends, sv[1] receives.
static int make_conn(enum setup s, int sv[2]) {
    if (s == S_PAIR)
        return socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) < 0 ? -1 : bound_waits(sv);
    struct sockaddr_un sun;
    socklen_t len = make_addr(s, &sun);
    int l = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (l < 0 || bind(l, (struct sockaddr *) &sun, len) < 0 || listen(l, 4) < 0)
        return -1;
    int c = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (c < 0 || connect(c, (struct sockaddr *) &sun, len) < 0)
        return -1;
    int a = accept(l, NULL, NULL);
    close(l);
    if (s == S_PATH)
        unlink(sun.sun_path);
    if (a < 0)
        return -1;
    sv[0] = c;
    sv[1] = a;
    return bound_waits(sv);
}

static ssize_t recv_flags(int fd, void *buf, size_t len, int flags, int *msg_flags) {
    struct iovec iov = {.iov_base = buf, .iov_len = len};
    struct msghdr mh = {.msg_iov = &iov, .msg_iovlen = 1};
    ssize_t n = recvmsg(fd, &mh, flags);
    if (msg_flags != NULL)
        *msg_flags = mh.msg_flags;
    return n;
}

struct later {
    int fd;
    const char *msg;
    int delay_ms;
};

static void *send_later(void *arg) {
    struct later *l = arg;
    usleep(l->delay_ms * 1000);
    send(l->fd, l->msg, strlen(l->msg), 0);
    return NULL;
}

static void check_basic(enum setup s) {
    cur = s;
    int sv[2];
    ck("connect", make_conn(s, sv), 0);
    int type = -1;
    socklen_t tl = sizeof type;
    getsockopt(sv[0], SOL_SOCKET, SO_TYPE, &type, &tl);
    ck("so_type sender", type, SOCK_SEQPACKET);
    type = -1;
    getsockopt(sv[1], SOL_SOCKET, SO_TYPE, &type, &tl);
    ck("so_type receiver", type, SOCK_SEQPACKET);

    char buf[256];
    int mf;
    // The triage's case: 1, 2 and 4 bytes are three messages.
    ck("send 1", send(sv[0], "a", 1, 0), 1);
    ck("send 2", send(sv[0], "bb", 2, 0), 2);
    ck("send 4", send(sv[0], "cccc", 4, 0), 4);
    ck("recv msg1 len", recv_flags(sv[1], buf, sizeof buf, 0, &mf), 1);
    ck("recv msg1 flags", mf, 0);
    struct pollfd p = {.fd = sv[1], .events = POLLIN};
    ck("poll after msg1", poll(&p, 1, 500), 1);
    ck("recv msg2 len", recv(sv[1], buf, sizeof buf, 0), 2);
    ck_bytes("recv msg2 data", buf, "bb", 2);
    ck("recv msg3 len", recv(sv[1], buf, sizeof buf, 0), 4);
    ck_bytes("recv msg3 data", buf, "cccc", 4);
    errno = 0;
    ck("recv empty", recv(sv[1], buf, sizeof buf, MSG_DONTWAIT), -1);
    ck("recv empty errno", errno, EAGAIN);

    // A blocking recv waits for the next message and returns it alone.
    struct later l = {.fd = sv[0], .msg = "second", .delay_ms = 100};
    pthread_t t;
    pthread_create(&t, NULL, send_later, &l);
    double t0 = now_s();
    ssize_t n = recv(sv[1], buf, sizeof buf, 0);
    double dt = now_s() - t0;
    pthread_join(t, NULL);
    ck("blocking recv len", n, 6);
    ck_bytes("blocking recv data", buf, "second", 6);
    ck("blocking recv prompt", dt < 2.0, 1);

    // A short buffer truncates the message and the rest of it is gone.
    send(sv[0], "hello", 5, 0);
    send(sv[0], "xyz", 3, 0);
    memset(buf, 0, sizeof buf);
    ck("trunc recv len", recv_flags(sv[1], buf, 2, 0, &mf), 2);
    ck("trunc recv flags", mf & MSG_TRUNC, MSG_TRUNC);
    ck_bytes("trunc recv data", buf, "he", 2);
    ck("after trunc len", recv(sv[1], buf, sizeof buf, 0), 3);
    ck_bytes("after trunc data", buf, "xyz", 3);

    // MSG_TRUNC asks for the message's real length.
    send(sv[0], "hello", 5, 0);
    ck("MSG_TRUNC recv", recv_flags(sv[1], buf, 2, MSG_TRUNC, &mf), 5);
    ck("MSG_TRUNC recv flags", mf & MSG_TRUNC, MSG_TRUNC);

    // MSG_PEEK looks without consuming, one message at a time.
    send(sv[0], "peek1", 5, 0);
    send(sv[0], "peek22", 6, 0);
    ck("peek len", recv(sv[1], buf, sizeof buf, MSG_PEEK), 5);
    ck("peek short len", recv_flags(sv[1], buf, 2, MSG_PEEK, &mf), 2);
    ck("peek short flags", mf & MSG_TRUNC, MSG_TRUNC);
    ck("after peek len", recv(sv[1], buf, sizeof buf, 0), 5);
    ck_bytes("after peek data", buf, "peek1", 5);
    ck("peek2 len", recv(sv[1], buf, sizeof buf, 0), 6);

    // A zero-length message is a message.
    ck("send empty", send(sv[0], "", 0, 0), 0);
    send(sv[0], "after", 5, 0);
    ck("recv empty msg", recv(sv[1], buf, sizeof buf, 0), 0);
    ck("recv after empty", recv(sv[1], buf, sizeof buf, 0), 5);

    // Several iovecs make one message, going out and coming in.
    struct iovec out[3] = {{"ab", 2}, {"cd", 2}, {"ef", 2}};
    struct msghdr mh = {.msg_iov = out, .msg_iovlen = 3};
    ck("sendmsg iov", sendmsg(sv[0], &mh, 0), 6);
    ck("recv iov msg", recv(sv[1], buf, sizeof buf, 0), 6);
    ck_bytes("recv iov data", buf, "abcdef", 6);
    send(sv[0], "abcdef", 6, 0);
    send(sv[0], "next", 4, 0);
    char b1[2], b2[2];
    struct iovec in[2] = {{b1, 2}, {b2, 2}};
    struct msghdr rh = {.msg_iov = in, .msg_iovlen = 2};
    ck("recvmsg 2 iov", recvmsg(sv[1], &rh, 0), 4);
    ck("recvmsg 2 iov flags", rh.msg_flags & MSG_TRUNC, MSG_TRUNC);
    ck("after 2 iov", recv(sv[1], buf, sizeof buf, 0), 4);
    ck_bytes("after 2 iov data", buf, "next", 4);

    // read() and write() are messages too, and so are writev() and readv().
    ck("write 1", write(sv[0], "w1", 2), 2);
    ck("write 2", write(sv[0], "w22", 3), 3);
    ck("read 1", read(sv[1], buf, sizeof buf), 2);
    ck("read 2", read(sv[1], buf, sizeof buf), 3);
    struct iovec wv[2] = {{"xy", 2}, {"zw", 2}};
    ck("writev", writev(sv[0], wv, 2), 4);
    send(sv[0], "tail", 4, 0);
    char r1[3], r2[64];
    struct iovec rv[2] = {{r1, 3}, {r2, sizeof r2}};
    ck("readv", readv(sv[1], rv, 2), 4);
    ck("after readv", recv(sv[1], buf, sizeof buf, 0), 4);

    // MSG_WAITALL does not join messages.
    send(sv[0], "ab", 2, 0);
    send(sv[0], "cd", 2, 0);
    ck("waitall recv", recv(sv[1], buf, 4, MSG_WAITALL), 2);
    ck("waitall recv 2", recv(sv[1], buf, 4, MSG_WAITALL), 2);

    // What is queued: FIONREAD counts message bytes.
    send(sv[0], "abc", 3, 0);
    send(sv[0], "defgh", 5, 0);
    usleep(20000);
    int avail = -1;
    ck("fionread rc", ioctl(sv[1], FIONREAD, &avail), 0);
    ck("fionread", avail, 8);
    ck("drain 1", recv(sv[1], buf, sizeof buf, 0), 3);
    p.revents = 0;
    ck("poll with 1 left", poll(&p, 1, 0), 1);
    ck("drain 2", recv(sv[1], buf, sizeof buf, 0), 5);

    // A large message arrives whole.
    size_t big = 100000;
    char *bigbuf = malloc(big), *bigin = malloc(2 * big);
    for (size_t i = 0; i < big; i++)
        bigbuf[i] = (char) (i * 7 + 3);
    ck("send big", send(sv[0], bigbuf, big, 0), (long) big);
    ck("recv big", recv(sv[1], bigin, 2 * big, 0), (long) big);
    ck("recv big data", memcmp(bigin, bigbuf, big) == 0, 1);
    free(bigbuf);
    free(bigin);

    // recvmmsg and sendmmsg: one message per entry.
    struct mmsghdr mm[3];
    struct iovec mmiov[3] = {{"1", 1}, {"22", 2}, {"4444", 4}};
    memset(mm, 0, sizeof mm);
    for (int i = 0; i < 3; i++) {
        mm[i].msg_hdr.msg_iov = &mmiov[i];
        mm[i].msg_hdr.msg_iovlen = 1;
    }
    ck("sendmmsg", sendmmsg(sv[0], mm, 3, 0), 3);
    char mb[3][16];
    struct iovec mbiov[3] = {{mb[0], 16}, {mb[1], 16}, {mb[2], 16}};
    memset(mm, 0, sizeof mm);
    for (int i = 0; i < 3; i++) {
        mm[i].msg_hdr.msg_iov = &mbiov[i];
        mm[i].msg_hdr.msg_iovlen = 1;
    }
    usleep(20000);
    ck("recvmmsg", recvmmsg(sv[1], mm, 3, MSG_DONTWAIT, NULL), 3);
    ck("recvmmsg len 0", mm[0].msg_len, 1);
    ck("recvmmsg len 1", mm[1].msg_len, 2);
    ck("recvmmsg len 2", mm[2].msg_len, 4);

    // The peer's close: queued messages first, then end of file.
    send(sv[0], "last", 4, 0);
    close(sv[0]);
    usleep(20000);
    p.revents = 0;
    p.events = POLLIN | POLLRDHUP;
    poll(&p, 1, 0);
    ck("poll after close", p.revents, POLLIN | POLLRDHUP | POLLHUP);
    ck("recv last", recv(sv[1], buf, sizeof buf, 0), 4);
    ck("recv eof", recv(sv[1], buf, sizeof buf, 0), 0);
    close(sv[1]);
}

// A message that does not fit the send buffer is refused whole.
static void check_limits(enum setup s) {
    cur = s;
    int sv[2];
    if (make_conn(s, sv) < 0)
        return;
    size_t huge = 300000;
    char *h = calloc(1, huge);
    errno = 0;
    ck("send huge", send(sv[0], h, huge, MSG_DONTWAIT), -1);
    ck("send huge errno", errno, EMSGSIZE);
    free(h);

    // Backpressure never splits a message: every send that succeeds arrives
    // as exactly one whole message.
    fcntl(sv[0], F_SETFL, O_NONBLOCK);
    char msg[1000];
    int sent = 0;
    for (int i = 0; i < 100000; i++) {
        memset(msg, 'A' + (i % 26), sizeof msg);
        ssize_t n = send(sv[0], msg, sizeof msg, 0);
        if (n < 0) {
            ck("full send errno", errno, EAGAIN);
            break;
        }
        ck(i == 0 ? "full send whole" : "full send whole (later)", n, (long) sizeof msg);
        if (n != (ssize_t) sizeof msg)
            break;
        sent++;
    }
    int got = 0, bad = 0;
    char in[2000];
    for (;;) {
        ssize_t n = recv(sv[1], in, sizeof in, MSG_DONTWAIT);
        if (n < 0)
            break;
        if (n != (ssize_t) sizeof msg || in[0] != 'A' + (got % 26) || in[999] != in[0])
            bad++;
        got++;
    }
    test_logf("  %s: %d messages fit before EAGAIN\n", setup_name[s], sent);
    ck("full sent > 0", sent > 0, 1);
    ck("full received count", got, sent);
    ck("full received intact", bad, 0);
    close(sv[0]);
    close(sv[1]);
}

// fds travel with the message they were sent with, and credentials too.
static void check_scm(enum setup s) {
    cur = s;
    int sv[2];
    if (make_conn(s, sv) < 0)
        return;
    int pp[2];
    pipe(pp);
    send(sv[0], "m1", 2, 0);
    char cbuf[CMSG_SPACE(sizeof(int))];
    memset(cbuf, 0, sizeof cbuf);
    struct iovec iov = {"m2", 2};
    struct msghdr mh = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = cbuf,
                        .msg_controllen = sizeof cbuf};
    struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &pp[0], sizeof(int));
    ck("sendmsg rights", sendmsg(sv[0], &mh, 0), 2);
    send(sv[0], "m3", 2, 0);

    for (int i = 1; i <= 3; i++) {
        char buf[16], rc[CMSG_SPACE(sizeof(int) * 4)];
        struct iovec riov = {buf, sizeof buf};
        struct msghdr rh = {.msg_iov = &riov, .msg_iovlen = 1, .msg_control = rc,
                            .msg_controllen = sizeof rc};
        ssize_t n = recvmsg(sv[1], &rh, 0);
        char label[48];
        snprintf(label, sizeof label, "scm msg%d len", i);
        ck(label, n, 2);
        struct cmsghdr *rcm = CMSG_FIRSTHDR(&rh);
        snprintf(label, sizeof label, "scm msg%d has rights", i);
        ck(label, rcm != NULL && rcm->cmsg_type == SCM_RIGHTS, i == 2);
        if (i == 2 && rcm != NULL && rcm->cmsg_type == SCM_RIGHTS) {
            int got;
            memcpy(&got, CMSG_DATA(rcm), sizeof got);
            write(pp[1], "z", 1);
            char z = 0;
            ck("scm passed fd reads", read(got, &z, 1), 1);
            ck("scm passed fd data", z, 'z');
            close(got);
        }
    }

    int on = 1;
    setsockopt(sv[1], SOL_SOCKET, SO_PASSCRED, &on, sizeof on);
    send(sv[0], "cred", 4, 0);
    char buf[16], rc[CMSG_SPACE(sizeof(struct ucred))];
    struct iovec riov = {buf, sizeof buf};
    struct msghdr rh = {.msg_iov = &riov, .msg_iovlen = 1, .msg_control = rc,
                        .msg_controllen = sizeof rc};
    ck("cred msg len", recvmsg(sv[1], &rh, 0), 4);
    struct cmsghdr *rcm = CMSG_FIRSTHDR(&rh);
    ck("cred cmsg", rcm != NULL && rcm->cmsg_type == SCM_CREDENTIALS, 1);
    if (rcm != NULL && rcm->cmsg_type == SCM_CREDENTIALS) {
        struct ucred uc;
        memcpy(&uc, CMSG_DATA(rcm), sizeof uc);
        ck("cred pid", uc.pid, getpid());
        ck("cred uid", uc.uid, getuid());
    }
    close(pp[0]);
    close(pp[1]);
    close(sv[0]);
    close(sv[1]);
}

// shutdown(SHUT_WR) ends the stream of messages for the peer.
static void check_shutdown(enum setup s) {
    cur = s;
    int sv[2];
    if (make_conn(s, sv) < 0)
        return;
    char buf[16];
    send(sv[0], "one", 3, 0);
    send(sv[0], "two", 3, 0);
    shutdown(sv[0], SHUT_WR);
    ck("shut recv 1", recv(sv[1], buf, sizeof buf, 0), 3);
    ck("shut recv 2", recv(sv[1], buf, sizeof buf, 0), 3);
    ck("shut recv eof", recv(sv[1], buf, sizeof buf, 0), 0);
    close(sv[0]);
    close(sv[1]);
}

// Two senders and two receivers on one connection: every message still
// arrives whole, once. Each message is its own length's low byte, repeated,
// so one split or merged by a reader racing another shows up at once.
#define CONC_PER_WRITER 1500
struct conc {
    int fd;
    int seed;
    _Atomic int received;
    _Atomic int bad;
};

static void *conc_writer(void *arg) {
    struct conc *c = arg;
    char msg[300];
    unsigned x = (unsigned) c->seed;
    for (int i = 0; i < CONC_PER_WRITER; i++) {
        x = x * 1103515245u + 12345u;
        size_t len = 1 + (x >> 8) % sizeof msg;
        memset(msg, (int) (len & 0xff), len);
        if (send(c->fd, msg, len, 0) != (ssize_t) len)
            c->bad++;
    }
    return NULL;
}

static void *conc_reader(void *arg) {
    struct conc *c = arg;
    char buf[512];
    while (c->received < 2 * CONC_PER_WRITER) {
        ssize_t n = recv(c->fd, buf, sizeof buf, 0);
        // End of file once the writers are done (every message has at least
        // a byte), or the 2 s receive timeout if messages went missing.
        if (n <= 0)
            break;
        c->received++;
        if (n > 300) {
            c->bad++;
            continue;
        }
        for (ssize_t i = 0; i < n; i++) {
            if ((unsigned char) buf[i] != (unsigned char) (n & 0xff)) {
                c->bad++;
                break;
            }
        }
    }
    return NULL;
}

static void check_concurrent(enum setup s) {
    cur = s;
    int sv[2];
    if (make_conn(s, sv) < 0)
        return;
    struct conc wr[2] = {{.fd = sv[0], .seed = 1}, {.fd = sv[0], .seed = 2}};
    struct conc rd = {.fd = sv[1]};
    pthread_t w[2], r[2];
    for (int i = 0; i < 2; i++)
        pthread_create(&r[i], NULL, conc_reader, &rd);
    for (int i = 0; i < 2; i++)
        pthread_create(&w[i], NULL, conc_writer, &wr[i]);
    for (int i = 0; i < 2; i++)
        pthread_join(w[i], NULL);
    shutdown(sv[0], SHUT_WR);
    for (int i = 0; i < 2; i++)
        pthread_join(r[i], NULL);
    ck("concurrent send errors", wr[0].bad + wr[1].bad, 0);
    ck("concurrent received", rd.received, 2 * CONC_PER_WRITER);
    ck("concurrent intact", rd.bad, 0);
    close(sv[0]);
    close(sv[1]);
}

// A connection only joins sockets of one type.
static void check_type_mismatch(void) {
    cur = S_PATH;
    struct sockaddr_un sun;
    socklen_t len = make_addr(S_PATH, &sun);
    int l = socket(AF_UNIX, SOCK_STREAM, 0);
    bind(l, (struct sockaddr *) &sun, len);
    listen(l, 4);
    int c = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    errno = 0;
    ck("seqpacket to stream listener", connect(c, (struct sockaddr *) &sun, len), -1);
    ck("seqpacket to stream errno", errno, EPROTOTYPE);
    close(c);
    close(l);
    unlink(sun.sun_path);

    len = make_addr(S_PATH, &sun);
    l = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    bind(l, (struct sockaddr *) &sun, len);
    listen(l, 4);
    c = socket(AF_UNIX, SOCK_STREAM, 0);
    errno = 0;
    ck("stream to seqpacket listener", connect(c, (struct sockaddr *) &sun, len), -1);
    ck("stream to seqpacket errno", errno, EPROTOTYPE);
    close(c);
    close(l);
    unlink(sun.sun_path);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    setvbuf(stdout, NULL, _IOLBF, 0);
    alarm(test_watchdog_secs(120));
    signal(SIGPIPE, SIG_IGN);
    for (int s = 0; s < S_COUNT; s++) {
        check_basic(s);
        check_limits(s);
        check_scm(s);
        check_shutdown(s);
        check_concurrent(s);
    }
    check_type_mismatch();
    return finish_suite("unix_seqpacket_msgs");
}
