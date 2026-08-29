// Socket option values as Linux reports them.
//
// Two things AOK got wrong, both easy for ordinary code to trip over:
//
// BSD's getsockopt hands back the option's own bit out of so_options rather
// than a boolean, so an enabled SO_REUSEADDR read back as 4, SO_KEEPALIVE as
// 8, SO_BROADCAST as 32, SO_REUSEPORT as 512 and TCP_NODELAY as 4. Linux
// always reports exactly 0 or 1, and `if (val == 1)` is the common idiom.
//
// And most of the TCP tuning family was unmapped, so setting a keepalive
// interval -- which every Linux accepts -- failed with ENOPROTOOPT. Where
// Darwin has the knob it is now wired to it (TCP_CORK is its TCP_NOPUSH,
// TCP_KEEPIDLE its TCP_KEEPALIVE); where it does not, the value is kept and
// reported back, since an error no Linux ever returns is the worse lie.
//
// Every expectation was measured against x86_64 glibc on Linux 6.12.
#define _GNU_SOURCE
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include "test_common.h"

static void check(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-34s got=%-8ld want=%ld\n", label, got, want);
}

// Set 1 then 0; both must read back exactly as set.
static void boolean_opt(const char *name, int level, int opt) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { printf("  %s: SKIP (socket)\n", name); return; }
    char label[80];
    int v = 1;
    errno = 0;
    if (setsockopt(s, level, opt, &v, sizeof v) < 0) {
        snprintf(label, sizeof label, "%s set 1", name);
        failf(label, (uint64_t) errno, 0, 0, 0, 0, 0);
        close(s);
        return;
    }
    int got = -1;
    socklen_t len = sizeof got;
    if (getsockopt(s, level, opt, &got, &len) < 0) got = -1;
    snprintf(label, sizeof label, "%s enabled reads back", name);
    check(label, got, 1);

    v = 0;
    setsockopt(s, level, opt, &v, sizeof v);
    got = -1; len = sizeof got;
    if (getsockopt(s, level, opt, &got, &len) < 0) got = -1;
    snprintf(label, sizeof label, "%s disabled reads back", name);
    check(label, got, 0);
    close(s);
}

// Set a value, read it back unchanged.
static void value_opt(const char *name, int level, int opt, int val) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { printf("  %s: SKIP (socket)\n", name); return; }
    char label[80];
    errno = 0;
    if (setsockopt(s, level, opt, &val, sizeof val) < 0) {
        snprintf(label, sizeof label, "%s set", name);
        failf(label, (uint64_t) errno, 0, 0, 0, 0, 0);
        close(s);
        return;
    }
    int got = -1;
    socklen_t len = sizeof got;
    errno = 0;
    if (getsockopt(s, level, opt, &got, &len) < 0) {
        snprintf(label, sizeof label, "%s get", name);
        failf(label, (uint64_t) errno, 0, 0, 0, 0, 0);
        close(s);
        return;
    }
    snprintf(label, sizeof label, "%s round-trips", name);
    check(label, got, val);
    close(s);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));

    boolean_opt("SO_REUSEADDR", SOL_SOCKET, SO_REUSEADDR);
    boolean_opt("SO_KEEPALIVE", SOL_SOCKET, SO_KEEPALIVE);
    boolean_opt("SO_BROADCAST", SOL_SOCKET, SO_BROADCAST);
    boolean_opt("SO_OOBINLINE", SOL_SOCKET, SO_OOBINLINE);
    boolean_opt("SO_DONTROUTE", SOL_SOCKET, SO_DONTROUTE);
#ifdef SO_REUSEPORT
    boolean_opt("SO_REUSEPORT", SOL_SOCKET, SO_REUSEPORT);
#endif
    boolean_opt("TCP_NODELAY", IPPROTO_TCP, TCP_NODELAY);
    boolean_opt("TCP_CORK", IPPROTO_TCP, TCP_CORK);

    value_opt("TCP_MAXSEG", IPPROTO_TCP, TCP_MAXSEG, 1200);
    value_opt("TCP_KEEPIDLE", IPPROTO_TCP, TCP_KEEPIDLE, 120);
    value_opt("TCP_KEEPINTVL", IPPROTO_TCP, TCP_KEEPINTVL, 30);
    value_opt("TCP_KEEPCNT", IPPROTO_TCP, TCP_KEEPCNT, 5);
    value_opt("TCP_SYNCNT", IPPROTO_TCP, TCP_SYNCNT, 3);
    value_opt("TCP_LINGER2", IPPROTO_TCP, TCP_LINGER2, 30);
    value_opt("TCP_WINDOW_CLAMP", IPPROTO_TCP, TCP_WINDOW_CLAMP, 32768);
    value_opt("TCP_USER_TIMEOUT", IPPROTO_TCP, TCP_USER_TIMEOUT, 9000);
    value_opt("TCP_FASTOPEN", IPPROTO_TCP, TCP_FASTOPEN, 5);

    // TCP_SYNCNT's range is enforced: 1..127, anything else EINVAL.
    {
        int s = socket(AF_INET, SOCK_STREAM, 0);
        if (s >= 0) {
            int v = 0;
            errno = 0;
            check("TCP_SYNCNT 0 rejected", setsockopt(s, IPPROTO_TCP, TCP_SYNCNT, &v, sizeof v), -1);
            check("TCP_SYNCNT 0 errno", errno, EINVAL);
            v = 200;
            errno = 0;
            check("TCP_SYNCNT 200 rejected", setsockopt(s, IPPROTO_TCP, TCP_SYNCNT, &v, sizeof v), -1);
            check("TCP_SYNCNT 200 errno", errno, EINVAL);
            close(s);
        }
    }

    // TCP_CONGESTION reports the full TCP_CA_NAME_MAX buffer, NUL-padded.
    {
        int s = socket(AF_INET, SOCK_STREAM, 0);
        if (s >= 0) {
            char cc[32];
            memset(cc, 0xaa, sizeof cc);
            socklen_t len = 16;
            errno = 0;
            if (getsockopt(s, IPPROTO_TCP, TCP_CONGESTION, cc, &len) == 0) {
                check("TCP_CONGESTION optlen", (long) len, 16);
                check("TCP_CONGESTION is NUL-terminated", cc[strnlen(cc, 16)] == '\0', 1);
            } else {
                printf("  TCP_CONGESTION: SKIP (%s)\n", strerror(errno));
            }
            close(s);
        }
    }

    return finish_suite("sock_options");
}
