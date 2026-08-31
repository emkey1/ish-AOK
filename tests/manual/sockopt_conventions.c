// Socket options whose ANSWER is the contract, not just their acceptance.
//
//   SO_RCVBUF/SO_SNDBUF: Linux stores twice what it is asked for -- the
//   second half is its own bookkeeping overhead -- and getsockopt reports the
//   doubled number. A program that sets 8192 and reads back 8192 concludes
//   the kernel truncated its request; every buffer-tuning loop is written
//   against the doubling. It also clamps: to net.core.{r,w}mem_max on the way
//   in, and to a floor on the way out, so a request of 0 is legal and yields
//   the minimum rather than an error. AOK passed the value straight to the
//   host and reported whatever came back.
//
//   IP_RETOPTS was accepted and discarded with no reader at all, so
//   getsockopt reported optlen 0 and left the caller's buffer untouched --
//   whatever uninitialised bytes were already there read back as the answer.
//
//   SO_PASSCRED was refused on any socket that was not AF_UNIX or netlink.
//   Linux accepts it everywhere and it simply means nothing where credentials
//   do not travel, so a library that sets it before deciding what kind of
//   socket it has saw an error Linux never gives.
//
//   An IPv6 option asked of a socket that is not IPv6 is not an unknown
//   option -- it is a level that socket does not have. Linux dispatches by
//   family, lands in no IPv6 handler, and returns EOPNOTSUPP; AOK recognised
//   the level regardless of family and said ENOPROTOOPT, telling a caller
//   probing for IPv6 support the wrong thing about why.
//
//   SO_ZEROCOPY and SO_TIMESTAMPING are refused here rather than accepted,
//   and that is deliberate -- see the section below.
//
// Measured against x86_64 glibc on Linux 6.12.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "test_common.h"

#define SO_PASSCRED_ 16
#define SO_TIMESTAMPING_ 37
#define SO_PEEK_OFF_ 42
#define SO_INCOMING_CPU_ 49
#define SO_ZEROCOPY_ 60
#define SO_BINDTODEVICE_ 25
#define SO_RCVBUFFORCE_ 33
#define IP_RETOPTS_ 7

// Linux's floors, after the doubling.
#define SOCK_MIN_RCVBUF 2304
#define SOCK_MIN_SNDBUF 4608

static int on_ish;

static void ck(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-58s got=%-10ld want=%ld\n", label, got, want);
}

static int seti(int fd, int level, int opt, int val) {
    errno = 0;
    return setsockopt(fd, level, opt, &val, sizeof val) == 0 ? 0 : errno;
}

// Returns the value, or -errno on failure (every option here is either
// non-negative or -1, so the two never collide in a way that matters).
static long geti(int fd, int level, int opt) {
    int v = 0x5A5A5A5A;
    socklen_t l = sizeof v;
    errno = 0;
    if (getsockopt(fd, level, opt, &v, &l) != 0)
        return -errno;
    if (l != sizeof v)
        return -1000 - (long) l;   // wrong optlen is its own failure
    return v;
}

static long read_sysctl(const char *path, long dflt) {
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return dflt;
    long v = dflt;
    if (fscanf(f, "%ld", &v) != 1)
        v = dflt;
    fclose(f);
    return v;
}

// Each system names its own loopback: "lo" on Linux, "lo0" on Darwin -- and
// the AOK guest presents the HOST's interface list, so it is "lo0" there.
// Asking for a name the system does not have is correctly ENODEV.
static const char *loopback_name(void) {
    if (if_nametoindex("lo") != 0)
        return "lo";
    if (if_nametoindex("lo0") != 0)
        return "lo0";
    return NULL;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));
    on_ish = access("/proc/ish", F_OK) == 0;

    // ---- SO_RCVBUF / SO_SNDBUF ------------------------------------------
    // The expectation is computed from the kernel's OWN advertised limit, so
    // this also checks that /proc/sys/net/core agrees with what setsockopt
    // enforces. A kernel that advertised one cap and applied another would
    // pass a hardcoded expectation and still be lying to every program that
    // reads the limit before setting the value.
    {
        long rmax = read_sysctl("/proc/sys/net/core/rmem_max", 212992);
        long wmax = read_sysctl("/proc/sys/net/core/wmem_max", 212992);
        test_logf("  (rmem_max=%ld wmem_max=%ld)\n", rmax, wmax);
        struct { long ask; } asks[] = { { 0 }, { 100 }, { 2048 }, { 8192 }, { 65536 },
                                        { 1048576 } };
        for (unsigned i = 0; i < sizeof asks / sizeof asks[0]; i++) {
            long a = asks[i].ask;
            char lab[80];
            {
                int s = socket(AF_INET, SOCK_STREAM, 0);
                long capped = a > rmax ? rmax : a;
                long want = capped * 2;
                if (want < SOCK_MIN_RCVBUF)
                    want = SOCK_MIN_RCVBUF;
                snprintf(lab, sizeof lab, "SO_RCVBUF %ld is accepted", a);
                ck(lab, seti(s, SOL_SOCKET, SO_RCVBUF, (int) a), 0);
                snprintf(lab, sizeof lab, "  and reads back doubled/clamped");
                ck(lab, geti(s, SOL_SOCKET, SO_RCVBUF), want);
                close(s);
            }
            {
                int s = socket(AF_INET, SOCK_STREAM, 0);
                long capped = a > wmax ? wmax : a;
                long want = capped * 2;
                if (want < SOCK_MIN_SNDBUF)
                    want = SOCK_MIN_SNDBUF;
                snprintf(lab, sizeof lab, "SO_SNDBUF %ld reads back doubled/clamped", a);
                ck(lab, seti(s, SOL_SOCKET, SO_SNDBUF, (int) a) == 0
                        ? geti(s, SOL_SOCKET, SO_SNDBUF) : -1, want);
                close(s);
            }
        }
        // SO_RCVBUFFORCE is the privileged form that skips the cap. Root
        // only, which is what the suite runs as; an unprivileged run would
        // get EPERM, which is a different (also correct) answer.
        if (geteuid() == 0) {
            int s = socket(AF_INET, SOCK_STREAM, 0);
            long ask = rmax * 4;
            ck("SO_RCVBUFFORCE is accepted as root",
               seti(s, SOL_SOCKET, SO_RCVBUFFORCE_, (int) ask), 0);
            ck("  and is NOT capped at rmem_max",
               geti(s, SOL_SOCKET, SO_RCVBUF) > rmax * 2 ? 1 : 0, 1);
            close(s);
        }
    }

    // ---- IP_RETOPTS round-trips ------------------------------------------
    {
        int s = socket(AF_INET, SOCK_STREAM, 0);
        ck("IP_RETOPTS=1 is accepted", seti(s, IPPROTO_IP, IP_RETOPTS_, 1), 0);
        ck("  and reads back 1", geti(s, IPPROTO_IP, IP_RETOPTS_), 1);
        ck("IP_RETOPTS=0", seti(s, IPPROTO_IP, IP_RETOPTS_, 0), 0);
        ck("  and reads back 0", geti(s, IPPROTO_IP, IP_RETOPTS_), 0);
        close(s);
    }

    // ---- SO_PASSCRED is a generic option --------------------------------
    {
        int s = socket(AF_INET, SOCK_STREAM, 0);
        ck("SO_PASSCRED on a TCP socket is accepted",
           seti(s, SOL_SOCKET, SO_PASSCRED_, 1), 0);
        ck("  and reads back 1", geti(s, SOL_SOCKET, SO_PASSCRED_), 1);
        close(s);
        // ...and still works where it actually means something.
        s = socket(AF_UNIX, SOCK_STREAM, 0);
        ck("SO_PASSCRED on a unix socket is accepted",
           seti(s, SOL_SOCKET, SO_PASSCRED_, 1), 0);
        ck("  and reads back 1", geti(s, SOL_SOCKET, SO_PASSCRED_), 1);
        close(s);
    }

    // ---- SO_INCOMING_CPU -------------------------------------------------
    {
        int s = socket(AF_INET, SOCK_STREAM, 0);
        // -1 is "no CPU has received on this socket", which is every socket
        // that has not received yet -- and, here, every socket.
        ck("SO_INCOMING_CPU defaults to -1", geti(s, SOL_SOCKET, SO_INCOMING_CPU_), -1);
        ck("  is settable", seti(s, SOL_SOCKET, SO_INCOMING_CPU_, 0), 0);
        ck("  and reads back what was set", geti(s, SOL_SOCKET, SO_INCOMING_CPU_), 0);
        close(s);
    }

    // ---- SO_PEEK_OFF, SO_ZEROCOPY, SO_TIMESTAMPING -----------------------
    // The values that ask for nothing are accepted everywhere; turning any of
    // them ON is where AOK deliberately parts company with Linux.
    {
        int s = socket(AF_INET, SOCK_STREAM, 0);
        ck("SO_PEEK_OFF defaults to -1", geti(s, SOL_SOCKET, SO_PEEK_OFF_), -1);
        ck("  -1 (disabled) is accepted", seti(s, SOL_SOCKET, SO_PEEK_OFF_, -1), 0);
        // Offset 0 means "peek from the front", which is the behaviour
        // already in place -- accepting it promises nothing.
        ck("  0 (peek from the front) is accepted", seti(s, SOL_SOCKET, SO_PEEK_OFF_, 0), 0);
        ck("  and reads back 0", geti(s, SOL_SOCKET, SO_PEEK_OFF_), 0);
        ck("SO_ZEROCOPY=0 is accepted", seti(s, SOL_SOCKET, SO_ZEROCOPY_, 0), 0);
        ck("  and reads back 0", geti(s, SOL_SOCKET, SO_ZEROCOPY_), 0);
        ck("SO_TIMESTAMPING=0 is accepted", seti(s, SOL_SOCKET, SO_TIMESTAMPING_, 0), 0);
        ck("  and reads back 0", geti(s, SOL_SOCKET, SO_TIMESTAMPING_), 0);
        close(s);
    }
    if (on_ish) {
        // Each of these three is a promise to deliver something later, and
        // AOK can deliver none of it. Refusing is not an oversight -- a
        // caller that succeeds here goes on to WAIT for what it was promised:
        //
        //   SO_ZEROCOPY makes send(MSG_ZEROCOPY) report completion on the
        //   error queue; without the notification the sender never learns its
        //   buffer is free and blocks on a completion that is never coming.
        //
        //   SO_TIMESTAMPING promises SCM_TIMESTAMPING control messages and
        //   transmit completions through the same error queue. Callers fall
        //   back to SO_TIMESTAMP, which does work here.
        //
        //   SO_PEEK_OFF makes recv(MSG_PEEK) start N bytes in. AOK peeks from
        //   the front, so a caller that set a positive offset would silently
        //   read the wrong bytes -- the one failure mode with no symptom.
        //
        // EOPNOTSUPP is what Linux itself gives for SO_ZEROCOPY on a family
        // that cannot do it, so callers already handle it. If any of these is
        // ever implemented, this section is what says so.
        int s = socket(AF_INET, SOCK_STREAM, 0);
        ck("turning SO_ZEROCOPY on is refused", seti(s, SOL_SOCKET, SO_ZEROCOPY_, 1), EOPNOTSUPP);
        ck("turning SO_TIMESTAMPING on is refused",
           seti(s, SOL_SOCKET, SO_TIMESTAMPING_, 1), EOPNOTSUPP);
        ck("a positive SO_PEEK_OFF is refused", seti(s, SOL_SOCKET, SO_PEEK_OFF_, 8), EOPNOTSUPP);
        // ...and refusing left the state alone.
        ck("  the offset is still 0/-1", geti(s, SOL_SOCKET, SO_PEEK_OFF_) <= 0 ? 1 : 0, 1);
        close(s);
    }

    // ---- an IPv6 option on a socket that is not IPv6 ---------------------
    {
        int s = socket(AF_INET, SOCK_STREAM, 0);
        ck("IPV6_V6ONLY on an AF_INET socket is EOPNOTSUPP",
           (int) -geti(s, IPPROTO_IPV6, IPV6_V6ONLY), EOPNOTSUPP);
        ck("an unknown IPv6 option there is too",
           (int) -geti(s, IPPROTO_IPV6, 9999), EOPNOTSUPP);
        close(s);
        // ...but a real IPv6 socket still answers.
        s = socket(AF_INET6, SOCK_STREAM, 0);
        if (s >= 0) {
            ck("IPV6_V6ONLY on an AF_INET6 socket works",
               geti(s, IPPROTO_IPV6, IPV6_V6ONLY) >= 0 ? 1 : 0, 1);
            close(s);
        }
    }

    // ---- SO_BINDTODEVICE round-trips -------------------------------------
    {
        const char *lo = loopback_name();
        if (lo == NULL) {
            printf("sockopt_conventions: NOTE no loopback interface; skipping SO_BINDTODEVICE\n");
        } else {
            int s = socket(AF_INET, SOCK_DGRAM, 0);
            errno = 0;
            int r = setsockopt(s, SOL_SOCKET, SO_BINDTODEVICE_, lo, strlen(lo) + 1);
            ck("SO_BINDTODEVICE to the loopback is accepted", r == 0 ? 0 : errno, 0);
            char buf[32];
            memset(buf, 0xAA, sizeof buf);
            socklen_t l = sizeof buf;
            errno = 0;
            r = getsockopt(s, SOL_SOCKET, SO_BINDTODEVICE_, buf, &l);
            ck("  and reads back the name", r == 0 && strcmp(buf, lo) == 0 ? 1 : 0, 1);
            ck("  with its length", r == 0 ? (long) l : -1, (long) strlen(lo) + 1);
            close(s);
            // A name no interface has is ENODEV -- the request was well
            // formed, the device just is not there.
            s = socket(AF_INET, SOCK_DGRAM, 0);
            errno = 0;
            r = setsockopt(s, SOL_SOCKET, SO_BINDTODEVICE_, "nosuchif0", 10);
            ck("a name no interface has is ENODEV", r == 0 ? 0 : errno, ENODEV);
            close(s);
        }
    }

    return finish_suite("sockopt_conventions");
}
