// getsockopt's length contract, and the errno for an option that does not exist.
//
// Linux clamps an option's natural size to the caller's buffer and writes THAT
// back through optlen -- a short buffer truncates silently, it is not an error.
// Reporting the natural size instead tells the caller more bytes were written
// than its buffer could hold, which is how a caller ends up reading stack
// garbage as part of the value.
//
// And an option the level does not recognise is ENOPROTOOPT, not EINVAL:
// probing code treats the first as "not available, carry on" and the second as
// a hard error. Measured against x86_64 glibc on Linux 6.12.
#define _GNU_SOURCE
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "test_common.h"

static void check(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-38s got=%ld want=%ld\n", label, got, want);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        printf("sock_optlen: SKIP (no AF_INET socket: %s)\n", strerror(errno));
        return 0;
    }

    int v = 0;
    socklen_t len = sizeof(int) * 4;              // oversized: clamps to 4
    errno = 0;
    check("oversized buf rc", getsockopt(s, SOL_SOCKET, SO_TYPE, &v, &len), 0);
    check("oversized buf optlen", (long) len, (long) sizeof(int));
    check("oversized buf value", v, SOCK_STREAM);

    char small[4] = {0};
    len = 1;                                       // short: truncates to 1
    errno = 0;
    check("1-byte buf rc", getsockopt(s, SOL_SOCKET, SO_TYPE, small, &len), 0);
    check("1-byte buf optlen", (long) len, 1);

    len = 0;                                       // zero: writes nothing
    errno = 0;
    check("zero-len buf rc", getsockopt(s, SOL_SOCKET, SO_TYPE, &v, &len), 0);
    check("zero-len buf optlen", (long) len, 0);

    v = 12345; len = sizeof v; errno = 0;
    check("SO_ERROR rc", getsockopt(s, SOL_SOCKET, SO_ERROR, &v, &len), 0);
    check("SO_ERROR on a fresh socket", v, 0);
    check("SO_ERROR optlen", (long) len, (long) sizeof(int));

    v = 0; len = sizeof v; errno = 0;
    check("unknown option rc", getsockopt(s, SOL_SOCKET, 0x7ffe, &v, &len), -1);
    check("unknown option errno", errno, ENOPROTOOPT);

    errno = 0;
    check("NULL optlen rc", getsockopt(s, SOL_SOCKET, SO_TYPE, &v, NULL), -1);
    check("NULL optlen errno", errno, EFAULT);

    close(s);
    return finish_suite("sock_optlen");
}
