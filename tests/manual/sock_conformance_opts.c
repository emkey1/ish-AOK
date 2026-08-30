// Socket option and socket() argument conformance: ten things AOK reported
// differently from Linux, and one that could not be read back at all.
//
//   socket() and socketpair() answered EINVAL for three different failures --
//   an address family that does not exist, a type that is not a socket type,
//   and a protocol the family cannot speak. Linux gives EAFNOSUPPORT, EINVAL
//   and EPROTONOSUPPORT, and a program probing for what this kernel supports
//   cannot tell them apart otherwise. socket(AF_UNIX, SOCK_STREAM, 1) also
//   failed, and libraries do write that: Linux's unix_create accepts protocol
//   0 and PF_UNIX alike.
//
//   SO_PROTOCOL stored the raw protocol argument, so every socket made the
//   ordinary way -- socket(AF_INET, SOCK_STREAM, 0) -- reported 0, and a
//   caller using the option to find out what it had learned nothing.
//
//   SO_PRIORITY, SO_MARK, SO_BUSY_POLL, SO_NO_CHECK and SO_TIMESTAMPNS have no
//   Darwin knob and were refused with ENOPROTOOPT, a state real Linux never
//   produces. They are remembered and reported back now, the way tcp_syncnt
//   and friends already are; SO_MARK keeps Linux's CAP_NET_ADMIN rule, which
//   is EPERM and never EINVAL.
//
//   The six multicast options were settable and not readable: the set path
//   forwarded them to the host and there was no get path at all.
//
//   getsockopt with an unrecognised LEVEL gave ENOPROTOOPT where Linux gives
//   EOPNOTSUPP -- and Linux really is asymmetric, since setsockopt gives
//   ENOPROTOOPT for the same level. sock_level_to_real passes an unknown level
//   straight through, so "no mapping" could not mean "unknown level".
//
//   shutdown() on a LISTENING socket is 0 on Linux (inet_shutdown disconnects)
//   and ENOTCONN only for a socket that never had a peer. Darwin refuses both,
//   so the ordinary "stop accepting and wake every poller" idiom failed.
//
//   MSG_CMSG_CLOEXEC was ignored, so an fd received through SCM_RIGHTS arrived
//   without FD_CLOEXEC and leaked into the next exec. There is no race-free
//   way for the caller to repair that afterwards, which is why the flag exists.
//
// Measured against x86_64 glibc on Linux 6.12.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <net/if.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>

#include "test_common.h"

static void ck(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-50s got=%-9ld want=%ld\n", label, got, want);
}

static int mk(int type) { return socket(AF_INET, type, 0); }

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));

    test_logf("[117/166] errno for unknown options and levels\n");
    {
        int s = mk(SOCK_STREAM); int v = 1; socklen_t l = sizeof v;
        errno = 0; ck("  setsockopt(SOL_SOCKET, bogus opt)",
                      setsockopt(s, SOL_SOCKET, 12345, &v, sizeof v) < 0 ? errno : 0, ENOPROTOOPT);
        errno = 0; ck("  getsockopt(SOL_SOCKET, bogus opt)",
                      getsockopt(s, SOL_SOCKET, 12345, &v, &l) < 0 ? errno : 0, ENOPROTOOPT);
        errno = 0; ck("  setsockopt(bogus level)",
                      setsockopt(s, 9999, 1, &v, sizeof v) < 0 ? errno : 0, ENOPROTOOPT);
        errno = 0; ck("  getsockopt(bogus level)",
                      getsockopt(s, 9999, 1, &v, &l) < 0 ? errno : 0, EOPNOTSUPP);
        close(s);
    }

    test_logf("[110] a boolean option reads back as exactly 1\n");
    {
        int s = mk(SOCK_STREAM); int on = 1, v = -1; socklen_t l = sizeof v;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
        getsockopt(s, SOL_SOCKET, SO_REUSEADDR, &v, &l);
        ck("  SO_REUSEADDR reads back 1", v, 1);
        v = -1; setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof on);
        getsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &v, &l);
        ck("  SO_KEEPALIVE reads back 1", v, 1);
        v = -1; setsockopt(s, SOL_SOCKET, SO_BROADCAST, &on, sizeof on);
        getsockopt(s, SOL_SOCKET, SO_BROADCAST, &v, &l);
        ck("  SO_BROADCAST reads back 1", v, 1);
        close(s);
    }

    test_logf("[118] SOL_SOCKET options that should exist\n");
    {
        struct { int opt; const char *name; int val; } opts[] = {
            { SO_OOBINLINE, "SO_OOBINLINE", 1 },
            { SO_DONTROUTE, "SO_DONTROUTE", 1 },
            { SO_PRIORITY,  "SO_PRIORITY",  3 },
            { SO_TIMESTAMPNS, "SO_TIMESTAMPNS", 1 },
            { SO_TIMESTAMP, "SO_TIMESTAMP", 1 },
            { SO_RCVLOWAT,  "SO_RCVLOWAT",  2 },
            { SO_NO_CHECK,  "SO_NO_CHECK",  1 },
            { SO_BUSY_POLL, "SO_BUSY_POLL", 0 },
        };
        for (unsigned i = 0; i < sizeof opts / sizeof *opts; i++) {
            int s = mk(SOCK_DGRAM);
            char label[80];
            errno = 0;
            int r = setsockopt(s, SOL_SOCKET, opts[i].opt, &opts[i].val, sizeof(int));
            snprintf(label, sizeof label, "  set %s", opts[i].name);
            ck(label, r < 0 ? errno : 0, 0);
            int v = -1; socklen_t l = sizeof v;
            errno = 0;
            r = getsockopt(s, SOL_SOCKET, opts[i].opt, &v, &l);
            snprintf(label, sizeof label, "  get %s", opts[i].name);
            ck(label, r < 0 ? errno : 0, 0);
            close(s);
        }
    }

    test_logf("[114] socket() family/type/protocol errnos\n");
    {
        errno = 0; int s = socket(255, SOCK_STREAM, 0);
        ck("  a nonexistent family is EAFNOSUPPORT", s < 0 ? errno : 0, EAFNOSUPPORT);
        if (s >= 0) close(s);
        errno = 0; s = socket(AF_INET, SOCK_STREAM, 253);
        ck("  a bad protocol for the family is EPROTONOSUPPORT", s < 0 ? errno : 0, EPROTONOSUPPORT);
        if (s >= 0) close(s);
        errno = 0; s = socket(AF_INET, 99, 0);
        ck("  a malformed type is EINVAL/ESOCKTNOSUPPORT",
           s < 0 ? (errno == EINVAL || errno == ESOCKTNOSUPPORT) : 0, 1);
        if (s >= 0) close(s);
        errno = 0; s = socket(AF_UNIX, SOCK_STREAM, 1);
        ck("  AF_UNIX with protocol 1 still succeeds", s >= 0 ? 0 : errno, 0);
        if (s >= 0) close(s);
    }

    test_logf("[115] SO_PROTOCOL reports the resolved protocol\n");
    {
        int s = mk(SOCK_STREAM); int v = -1; socklen_t l = sizeof v;
        errno = 0;
        int r = getsockopt(s, SOL_SOCKET, SO_PROTOCOL, &v, &l);
        ck("  a default TCP socket reports IPPROTO_TCP", r == 0 ? v : -1, IPPROTO_TCP);
        close(s);
        s = mk(SOCK_DGRAM); v = -1; l = sizeof v;
        r = getsockopt(s, SOL_SOCKET, SO_PROTOCOL, &v, &l);
        ck("  a default UDP socket reports IPPROTO_UDP", r == 0 ? v : -1, IPPROTO_UDP);
        close(s);
        s = socket(AF_UNIX, SOCK_STREAM, 0); v = -1; l = sizeof v;
        r = getsockopt(s, SOL_SOCKET, SO_PROTOCOL, &v, &l);
        ck("  an AF_UNIX socket reports 0", r == 0 ? v : -1, 0);
        close(s);
    }

    test_logf("[116] multicast options round-trip\n");
    {
        int s = mk(SOCK_DGRAM);
        unsigned char ttl = 4; int loop = 0;
        errno = 0;
        ck("  set IP_MULTICAST_TTL",
           setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl) < 0 ? errno : 0, 0);
        int v = -1; socklen_t l = sizeof v;
        errno = 0;
        int r = getsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, &v, &l);
        ck("  get IP_MULTICAST_TTL", r < 0 ? errno : 0, 0);
        ck("    and it is what we set", r == 0 ? v : -1, 4);
        errno = 0;
        ck("  set IP_MULTICAST_LOOP",
           setsockopt(s, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof loop) < 0 ? errno : 0, 0);
        v = -1; l = sizeof v;
        errno = 0;
        r = getsockopt(s, IPPROTO_IP, IP_MULTICAST_LOOP, &v, &l);
        ck("  get IP_MULTICAST_LOOP", r < 0 ? errno : 0, 0);
        ck("    and it is what we set", r == 0 ? v : -1, 0);
        close(s);
    }

    test_logf("[54] shutdown on a listening socket\n");
    {
        int s = mk(SOCK_STREAM);
        struct sockaddr_in a; memset(&a, 0, sizeof a);
        a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(s, (struct sockaddr *) &a, sizeof a);
        listen(s, 4);
        errno = 0;
        ck("  a listening socket shuts down cleanly",
           shutdown(s, SHUT_RDWR) < 0 ? errno : 0, 0);
        close(s);
        int u = mk(SOCK_STREAM);
        errno = 0;
        ck("  but an unconnected one is ENOTCONN",
           shutdown(u, SHUT_RDWR) < 0 ? errno : 0, ENOTCONN);
        close(u);
    }

    test_logf("[55] MSG_CMSG_CLOEXEC on a received SCM_RIGHTS fd\n");
    {
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0) {
            int payload = open("/dev/null", O_RDONLY);
            char cbuf[CMSG_SPACE(sizeof(int))];
            struct iovec iov = { .iov_base = (void *) "x", .iov_len = 1 };
            struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1,
                                .msg_control = cbuf, .msg_controllen = sizeof cbuf };
            struct cmsghdr *c = CMSG_FIRSTHDR(&m);
            c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS;
            c->cmsg_len = CMSG_LEN(sizeof(int));
            memcpy(CMSG_DATA(c), &payload, sizeof(int));
            sendmsg(sv[0], &m, 0);
            char rbuf[1], rcbuf[CMSG_SPACE(sizeof(int))];
            struct iovec riov = { .iov_base = rbuf, .iov_len = 1 };
            struct msghdr rm = { .msg_iov = &riov, .msg_iovlen = 1,
                                 .msg_control = rcbuf, .msg_controllen = sizeof rcbuf };
            ssize_t n = recvmsg(sv[1], &rm, MSG_CMSG_CLOEXEC);
            int got = -1;
            if (n >= 0) {
                struct cmsghdr *rc = CMSG_FIRSTHDR(&rm);
                if (rc && rc->cmsg_type == SCM_RIGHTS)
                    memcpy(&got, CMSG_DATA(rc), sizeof got);
            }
            ck("  the fd arrived", got >= 0, 1);
            ck("  with FD_CLOEXEC set", got >= 0 ? (fcntl(got, F_GETFD) & FD_CLOEXEC) != 0 : 0, 1);
            if (got >= 0) close(got);
            close(payload); close(sv[0]); close(sv[1]);
        }
    }

    // ---- SO_BINDTODEVICE ---------------------------------------------------
    {
        // setsockopt refused it with ENOPROTOOPT while getsockopt reported
        // success with an empty name, so a program that bound to an interface
        // and then checked was told the bind had happened when it never did.
        // `ping -I <iface>` failed outright with "can't bind to interface".
        //
        // The loopback interface is not called the same thing everywhere --
        // "lo" on Linux, "lo0" on Darwin, which the guest sees through -- so
        // it is discovered rather than assumed.
        const char *dev = NULL;
        static const char *candidates[] = { "lo", "lo0" };
        for (unsigned i = 0; i < 2 && dev == NULL; i++)
            if (if_nametoindex(candidates[i]) != 0)
                dev = candidates[i];
        if (dev == NULL) {
            test_logf("  (no loopback interface found; skipping SO_BINDTODEVICE)\n");
        } else {
            int s = socket(AF_INET, SOCK_DGRAM, 0);
            char buf[32];
            memset(buf, 0xAA, sizeof buf);
            socklen_t l = sizeof buf;
            errno = 0;
            ck("SO_BINDTODEVICE on a never-bound socket succeeds",
               getsockopt(s, SOL_SOCKET, SO_BINDTODEVICE, buf, &l) < 0 ? errno : 0, 0);
            ck("  and reports an empty name", (long) l, 0);
            errno = 0;
            ck("  binding to the loopback interface succeeds",
               setsockopt(s, SOL_SOCKET, SO_BINDTODEVICE, dev, strlen(dev)) < 0 ? errno : 0, 0);
            memset(buf, 0xAA, sizeof buf);
            l = sizeof buf;
            errno = 0;
            ck("  and it reads back",
               getsockopt(s, SOL_SOCKET, SO_BINDTODEVICE, buf, &l) < 0 ? errno : 0, 0);
            ck("    with the name's length including its NUL",
               (long) l, (long) strlen(dev) + 1);
            ck("    and the name itself", strcmp(buf, dev) == 0, 1);
            errno = 0;
            ck("  an interface that does not exist is ENODEV",
               setsockopt(s, SOL_SOCKET, SO_BINDTODEVICE, "nosuchif0", 9) < 0 ? errno : 0,
               ENODEV);
            // ...and the socket is still usable, which is the point of binding it
            struct sockaddr_in a;
            memset(&a, 0, sizeof a);
            a.sin_family = AF_INET;
            a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            errno = 0;
            ck("  the bound socket still binds an address",
               bind(s, (struct sockaddr *) &a, sizeof a) < 0 ? errno : 0, 0);
            close(s);
        }
    }

    return finish_suite("sock_conformance_opts");
}
