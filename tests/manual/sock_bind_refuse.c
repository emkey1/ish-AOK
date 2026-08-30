// A TCP port that is bound but not yet listening must REFUSE connections.
//
// Linux keeps a bound socket in the bound hash and only a listening one in the
// listening hash, so a SYN arriving for a bound-but-not-listening port gets an
// RST and the client's connect() returns ECONNREFUSED immediately. Darwin
// silently DROPS that SYN, so the client retries and times out about eight
// seconds later -- and iSH-AOK inherited it, because a guest socket is a host
// socket. Measured three ways: on the host, in the guest, and on a real device
// with an external client and a listening control.
//
// What caused it was holding the port on the host in that state, so it is no
// longer held: a plain TCP bind() is validated against a throwaway socket and
// then remembered rather than applied, and the real host bind happens at
// listen() or connect(). Everything the guest can observe about bind() has to
// stay identical, which is most of what this file checks:
//
//   getsockname answers before listen, and does not change at listen;
//   a port-0 bind still gets a real port at BIND time (so it is never
//   deferred -- the port is assigned by the bind, and getsockname must report
//   it straight away);
//   a second bind to the same port is still EADDRINUSE, which the host can no
//   longer report because it is no longer holding the port -- the conflict
//   scan had to learn about deferred binds;
//   SO_REUSEADDR, source-port binding and an actual client/server exchange all
//   still work.
//
// The getsockname half is why this is worth a test rather than a comment: the
// first version answered with the stored bytes directly, which are in HOST
// layout -- Darwin's sockaddr_in starts with a one-byte sin_len where Linux
// has a two-byte family -- so python's http.server read the host part of
// getsockname() as an int and died in socket.getfqdn().
#define _GNU_SOURCE
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>

#include "test_common.h"

static void ck(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-52s got=%-8ld want=%ld\n", label, got, want);
}

static int mk(void) { return socket(AF_INET, SOCK_STREAM, 0); }

static int bind_to(int s, int port) {
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return bind(s, (struct sockaddr *) &a, sizeof a);
}

static int local_port(int s) {
    struct sockaddr_in a;
    socklen_t l = sizeof a;
    if (getsockname(s, (struct sockaddr *) &a, &l) < 0)
        return -1;
    // The family has to survive the round trip too -- that is the half that
    // broke python, and a port alone would not have caught it.
    if (a.sin_family != AF_INET)
        return -2;
    return ntohs(a.sin_port);
}

// connect() to a port, reporting the errno and how long it took. The DURATION
// is the point: a refusal is instant, a swallowed SYN is seconds.
static int connect_errno(int port, double *ms_out) {
    struct timespec t0, t1;
    int s = mk();
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    errno = 0;
    int r = connect(s, (struct sockaddr *) &a, sizeof a);
    int e = r < 0 ? errno : 0;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    if (ms_out)
        *ms_out = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    close(s);
    return e;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(90));
    // Ports are picked from the pid so concurrent suite runs do not collide.
    int base = 19000 + (getpid() % 400) * 10;

    // ---- what a connection to each port state does ------------------------
    {
        double ms = 0;
        ck("an unbound port refuses", connect_errno(base, &ms), ECONNREFUSED);
        ck("  promptly", ms < 2000, 1);

        int l = mk();
        if (bind_to(l, base + 1) == 0 && listen(l, 5) == 0) {
            ck("a listening port connects", connect_errno(base + 1, NULL), 0);
        } else {
            printf("FAIL could not set up a listener: %s\n", strerror(errno));
            failures_total++;
        }
        close(l);

        // The case this file exists for.
        int b = mk();
        if (bind_to(b, base + 2) == 0) {
            ms = 0;
            ck("a BOUND but not listening port refuses",
               connect_errno(base + 2, &ms), ECONNREFUSED);
            test_logf("    (took %.1fms; the bug made this ~7800ms)\n", ms);
            ck("  promptly, not after a retry timeout", ms < 2000, 1);
        } else {
            printf("FAIL could not bind: %s\n", strerror(errno));
            failures_total++;
        }
        close(b);

        int c = mk();
        if (bind_to(c, base + 3) == 0 && listen(c, 5) == 0)
            close(c);
        ck("a closed listener refuses again", connect_errno(base + 3, NULL), ECONNREFUSED);
    }

    // ---- everything the guest can observe about bind() is unchanged -------
    {
        int s = mk();
        ck("bind to an explicit port", bind_to(s, base + 4) < 0 ? errno : 0, 0);
        ck("  getsockname reports it before listen", local_port(s), base + 4);
        ck("  and the same after listen", (listen(s, 5), local_port(s)), base + 4);
        close(s);
    }
    {
        int s = mk();
        ck("bind to port 0", bind_to(s, 0) < 0 ? errno : 0, 0);
        int p = local_port(s);
        ck("  a real port is assigned at BIND time", p > 0, 1);
        listen(s, 5);
        ck("  and listen does not change it", local_port(s), p);
        close(s);
    }
    {
        int a = mk(), b = mk();
        ck("first bind", bind_to(a, base + 5) < 0 ? errno : 0, 0);
        ck("  a second bind to it is EADDRINUSE",
           bind_to(b, base + 5) < 0 ? errno : 0, EADDRINUSE);
        listen(a, 5);
        ck("  still EADDRINUSE once the first listens",
           bind_to(b, base + 5) < 0 ? errno : 0, EADDRINUSE);
        close(a);
        close(b);
    }
    {
        int a = mk();
        int one = 1;
        setsockopt(a, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        ck("SO_REUSEADDR bind", bind_to(a, base + 6) < 0 ? errno : 0, 0);
        ck("  and listen", listen(a, 5) < 0 ? errno : 0, 0);
        close(a);
    }
    {
        // A bound socket that CONNECTS rather than listening: the bind is its
        // source address, so it has to be real by then.
        int srv = mk();
        bind_to(srv, base + 7);
        listen(srv, 5);
        int c = mk();
        ck("a client binds a source port", bind_to(c, base + 8) < 0 ? errno : 0, 0);
        struct sockaddr_in a;
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_port = htons(base + 7);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ck("  and connects out", connect(c, (struct sockaddr *) &a, sizeof a) < 0 ? errno : 0, 0);
        ck("  from the source port it asked for", local_port(c), base + 8);
        close(c);
        close(srv);
    }

    // ---- and a whole server actually works --------------------------------
    {
        int srv = mk();
        if (bind_to(srv, base + 9) == 0 && listen(srv, 5) == 0) {
            fflush(NULL);
            pid_t p = fork();
            if (p == 0) {
                int c = mk();
                struct sockaddr_in a;
                memset(&a, 0, sizeof a);
                a.sin_family = AF_INET;
                a.sin_port = htons(base + 9);
                a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                if (connect(c, (struct sockaddr *) &a, sizeof a) < 0)
                    _exit(1);
                if (write(c, "ping", 4) != 4)
                    _exit(2);
                char b[8] = { 0 };
                _exit(read(c, b, 4) == 4 && memcmp(b, "pong", 4) == 0 ? 0 : 3);
            }
            int cs = accept(srv, NULL, NULL);
            char b[8] = { 0 };
            if (cs >= 0) {
                if (read(cs, b, 4) != 4) {}
                if (write(cs, "pong", 4) != 4) {}
                close(cs);
            }
            int st;
            waitpid(p, &st, 0);
            ck("a full client/server exchange", WIFEXITED(st) && WEXITSTATUS(st) == 0, 1);
            ck("  and the server saw the request", memcmp(b, "ping", 4) == 0, 1);
        } else {
            printf("FAIL could not set up the server: %s\n", strerror(errno));
            failures_total++;
        }
        close(srv);
    }

    return finish_suite("sock_bind_refuse");
}
