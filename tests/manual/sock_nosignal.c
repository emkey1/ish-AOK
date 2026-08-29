// MSG_NOSIGNAL: send()/sendmsg() on a broken connection must return -1/EPIPE
// WITHOUT raising SIGPIPE in the caller, while the same call without the flag
// -- and a bare write(), which has nowhere to put the flag -- must still kill
// it. Linux decides this per-call, so it cannot be emulated by changing the
// signal disposition: a library doing send(..., MSG_NOSIGNAL) must not have to
// touch process-wide state its caller can observe.
//
// Verified against x86_64 glibc on Linux 6.12: all five cases below agree.
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include "test_common.h"

enum how { SEND, SENDMSG, WRITE };

// Runs in a forked child so the SIGPIPE death is observable rather than fatal.
static void child(enum how how, int nosignal) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
        _exit(90);
    close(sv[1]);                       // peer is gone: the next write is EPIPE
    int flags = nosignal ? MSG_NOSIGNAL : 0;
    ssize_t r;
    errno = 0;
    if (how == SEND) {
        r = send(sv[0], "x", 1, flags);
    } else if (how == SENDMSG) {
        struct iovec iov = { .iov_base = (void *) "x", .iov_len = 1 };
        struct msghdr m;
        memset(&m, 0, sizeof m);
        m.msg_iov = &iov;
        m.msg_iovlen = 1;
        r = sendmsg(sv[0], &m, flags);
    } else {
        r = write(sv[0], "x", 1);
    }
    // Reached only when no signal was delivered.
    _exit(r < 0 && errno == EPIPE ? 0 : 70);
}

static void check(const char *label, enum how how, int nosignal, int want_signal) {
    fflush(NULL);                       // don't let the child flush our buffer
    pid_t c = fork();
    if (c == 0)
        child(how, nosignal);
    if (c < 0) {
        failf(label, (uint64_t) -1, 0, 0, 0, 0, 0);
        return;
    }
    int st = 0;
    while (waitpid(c, &st, 0) < 0 && errno == EINTR)
        continue;
    int sig = WIFSIGNALED(st) ? WTERMSIG(st) : 0;
    int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    int ok = want_signal ? (sig == SIGPIPE) : (sig == 0 && code == 0);
    if (!ok)
        failf(label, (uint64_t) sig, (uint64_t) code, 0,
              (uint64_t) (want_signal ? SIGPIPE : 0), want_signal ? 0xffffffffu : 0, 0);
    test_logf("  %-22s sig=%d code=%d %s\n", label, sig, code, ok ? "ok" : "WRONG");
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));

    check("send MSG_NOSIGNAL",    SEND,    1, 0);
    check("send 0",               SEND,    0, 1);
    check("sendmsg MSG_NOSIGNAL", SENDMSG, 1, 0);
    check("sendmsg 0",            SENDMSG, 0, 1);
    check("write",                WRITE,   0, 1);

    return finish_suite("sock_nosignal");
}
