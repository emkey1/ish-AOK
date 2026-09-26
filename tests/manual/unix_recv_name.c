// recvmsg and recvfrom on an AF_UNIX stream socket, given a buffer for the
// sender's address. The peer of a socketpair (or of any connection whose
// client never bound a name) has no address, and Linux answers with success
// and a length of 0.
//
// AOK answered EINVAL. The host returned no address, and sockaddr_write
// translated the family of a buffer the host had never written. Python passes
// a name buffer to every recvmsg, so SCM_RIGHTS never reached a Python
// receiver: Python 3.14's multiprocessing forkserver (the default start
// method from Alpine 3.24 on) died on its first fd hand-off, and so did
// multiprocessing.Pool. Seen on i386, amd64 and arm64 alike.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "test_common.h"

static int send_fd(int s, int fd) {
    char c = 'x';
    struct iovec iov = { &c, 1 };
    union { char buf[CMSG_SPACE(sizeof(int))]; struct cmsghdr align; } u;
    memset(&u, 0, sizeof u);
    struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1,
                        .msg_control = u.buf, .msg_controllen = sizeof u.buf };
    struct cmsghdr *h = CMSG_FIRSTHDR(&m);
    h->cmsg_level = SOL_SOCKET;
    h->cmsg_type = SCM_RIGHTS;
    h->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(h), &fd, sizeof fd);
    return sendmsg(s, &m, 0) == 1 ? 0 : -1;
}

// The call Python makes: a name buffer, room for one fd, MSG_CMSG_CLOEXEC.
static void test_recvmsg_scm_rights_with_name(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        printf("FAIL recvmsg_name: socketpair: %s\n", strerror(errno));
        failures_total++;
        return;
    }
    int fd = open("/dev/null", O_RDONLY);
    if (send_fd(sv[0], fd) != 0) {
        printf("FAIL recvmsg_name: sendmsg: %s\n", strerror(errno));
        failures_total++;
        return;
    }
    char c;
    struct iovec iov = { &c, 1 };
    struct sockaddr_storage name;
    memset(&name, 0xa5, sizeof name);
    union { char buf[CMSG_SPACE(sizeof(int))]; struct cmsghdr align; } u;
    memset(&u, 0, sizeof u);
    struct msghdr m = { .msg_name = &name, .msg_namelen = sizeof name,
                        .msg_iov = &iov, .msg_iovlen = 1,
                        .msg_control = u.buf, .msg_controllen = sizeof u.buf };
    ssize_t n = recvmsg(sv[1], &m, MSG_CMSG_CLOEXEC);
    test_logf("recvmsg_name: n=%zd errno=%d namelen=%u controllen=%zu\n",
              n, n < 0 ? errno : 0, (unsigned) m.msg_namelen, (size_t) m.msg_controllen);
    if (n != 1) {
        printf("FAIL recvmsg_name: recvmsg with a name buffer returned %zd (%s), want 1\n",
               n, n < 0 ? strerror(errno) : "short");
        failures_total++;
        return;
    }
    if (m.msg_namelen != 0) {
        printf("FAIL recvmsg_name: msg_namelen %u, want 0 for an unnamed peer\n",
               (unsigned) m.msg_namelen);
        failures_total++;
    }
    struct cmsghdr *h = CMSG_FIRSTHDR(&m);
    int got = -1;
    if (h != NULL && h->cmsg_level == SOL_SOCKET && h->cmsg_type == SCM_RIGHTS)
        memcpy(&got, CMSG_DATA(h), sizeof got);
    if (got < 0 || fcntl(got, F_GETFD) != FD_CLOEXEC) {
        printf("FAIL recvmsg_name: no close-on-exec fd came back (got %d)\n", got);
        failures_total++;
    }
    close(sv[0]);
    close(sv[1]);
}

static void test_recvfrom_with_name(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0 || write(sv[0], "y", 1) != 1) {
        printf("FAIL recvfrom_name: setup: %s\n", strerror(errno));
        failures_total++;
        return;
    }
    char c;
    struct sockaddr_storage from;
    socklen_t fromlen = sizeof from;
    ssize_t n = recvfrom(sv[1], &c, 1, 0, (struct sockaddr *) &from, &fromlen);
    test_logf("recvfrom_name: n=%zd errno=%d fromlen=%u\n", n, n < 0 ? errno : 0, (unsigned) fromlen);
    if (n != 1 || c != 'y') {
        printf("FAIL recvfrom_name: recvfrom with an address buffer returned %zd (%s), want 1\n",
               n, n < 0 ? strerror(errno) : "wrong byte");
        failures_total++;
    } else if (fromlen != 0) {
        printf("FAIL recvfrom_name: fromlen %u, want 0 for an unnamed peer\n", (unsigned) fromlen);
        failures_total++;
    }
    close(sv[0]);
    close(sv[1]);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    test_recvmsg_scm_rights_with_name();
    test_recvfrom_with_name();
    return finish_suite("unix_recv_name");
}
