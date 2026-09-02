// inaddr_any_iface: a listener bound to the wildcard address (0.0.0.0) must be
// reachable at EVERY IPv4 address the host currently has, and must stay
// reachable across changes to the host's interface set -- including at
// addresses that did not exist when the socket was bound.
//
// The reported failure (M4 iPad Pro, build 2026-08-08 17:40Z, aarch64 Devuan,
// OpenSSH 10.0p2): sshd bound 0.0.0.0:1022 at boot, and `ss -tlnp` kept showing
// it in LISTEN. Connections over WiFi were accepted; connections over the
// USB-C link-local address were answered with an immediate TCP RST, even though
// ICMP to that same address worked and ARP resolved it. Killing sshd and
// starting it again -- same interface, same address, nothing else changed --
// made the wired address work at once. The wired link's address had changed
// (169.254.203.216 -> 169.254.39.106) around an app restart, which is what
// suggested the trigger was an interface appearing, or changing address, after
// the guest socket was already bound.
//
// That proposed MECHANISM did not survive testing on the reporting device
// (2026-08-08, build 2026-08-08 17:40Z, M4 iPad Pro), and this comment records
// the negatives so the next person does not re-run them:
//
//   - Cable replugged with three wildcard listeners already parked (sshd:1022,
//     :1023, :9977). en3 vanished and returned. All three still served the
//     wired address, as did a freshly bound one.
//   - A wildcard listener bound while en3 was entirely ABSENT from the guest's
//     interface list still served en3 when the cable was plugged back in.
//   - fs/sock.c does not substitute an address on the bind path
//     (sockaddr_read_bind copies sin_addr through; sys_bind_common hands it
//     straight to the host bind), nothing snapshots the interface list at bind
//     time, and sys_listen reuses the same real_fd.
//   - inet_nat_bind_fallback, the one path that DOES substitute 127.0.0.1, is
//     not reached here: this iOS lets the app bind privileged ports, confirmed
//     by reaching a guest 0.0.0.0:1023 listener from another machine.
//
// In both device experiments the link-local address came back UNCHANGED, so the
// address-change half of the incident is still untested, as is the app-restart
// half (both device runs kept the app running throughout). Treat the phenomenon
// as real and unexplained; treat "wildcard sockets do not cover later
// interfaces" as tested and not reproduced.
//
// RST rather than a timeout is the fingerprint that separates this from the
// "sshd stalls" bug class it is easily mistaken for: a listener that is alive
// but wedged in accept(2) still completes the TCP handshake (the host kernel
// queues it) and only then hangs, so you get a connect that succeeds and a
// session that never starts. A RST means no listening PCB matched the SYN at
// all -- the socket is not covering that address.
//
// Two checks, because only one of them is self-contained:
//
//   coverage (always runs) -- bind the wildcard, then connect to every IPv4
//       address getifaddrs() currently reports. All must be accepted. This is a
//       true invariant on Linux and catches any path that silently substitutes
//       a narrower address for the wildcard: notably fs/sock.c's
//       inet_nat_bind_fallback, which on a host that refuses non-root binds to
//       ports < 1024 re-binds such a socket to 127.0.0.1:<ephemeral> and leaves
//       it loopback-only, with getsockname() still reporting the wildcard the
//       guest asked for, so nothing in the guest can tell. Run with --port in
//       the privileged range to exercise that path deliberately.
//
//   ordering (--watch N) -- hold that same socket and wait up to N seconds for
//       the host's address set to change, then re-probe every address,
//       including the new ones. This is the reported bug, and it cannot be
//       driven from inside the guest: it needs a real host interface event
//       (replug the cable, toggle WiFi, bring an interface up). Opt-in for that
//       reason, and skipped, not failed, if nothing changes before the timeout.
//
// The coverage check alone CANNOT catch the reported bug -- it binds fresh, so
// every address it probes existed before the bind, by construction. It is worth
// running anyway for the substitution class above, but do not read a pass as
// evidence about ordering.
//
// Portable to Darwin as well as Linux, deliberately: the same binary run
// natively on macOS answers whether a wildcard socket serving a
// later-added address is something AOK breaks or something it inherits.

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

#define BANNER "inaddr-any-ok\n"
#define MAX_ADDRS 64

struct addr_set {
    struct {
        uint32_t addr;             // network byte order
        char ifname[IF_NAMESIZE];
    } v[MAX_ADDRS];
    unsigned n;
};

static int listen_fd = -1;
static volatile sig_atomic_t acceptor_stop;
// 0 means "let the host pick an ephemeral port", read back with getsockname.
// A fixed port would collide when the release procedure runs the four
// per-architecture suites concurrently against one host.
static int probe_port;

// ---------------------------------------------------------------- acceptor

// Serves the wildcard listener for the life of the test. Kept deliberately
// simple: poll with a short timeout so the stop flag is honoured promptly
// without needing to interrupt a blocking accept().
static void *acceptor(void *arg) {
    (void) arg;
    while (!acceptor_stop) {
        struct pollfd pfd = {.fd = listen_fd, .events = POLLIN};
        int r = poll(&pfd, 1, 200);
        if (r <= 0)
            continue;
        int c = accept(listen_fd, NULL, NULL);
        if (c < 0)
            continue;
        (void) !write(c, BANNER, strlen(BANNER));
        close(c);
    }
    return NULL;
}

// ------------------------------------------------------------- enumeration

static void addr_set_collect(struct addr_set *out) {
    memset(out, 0, sizeof(*out));
    struct ifaddrs *addrs = NULL;
    if (getifaddrs(&addrs) != 0) {
        printf("FAIL getifaddrs: %s\n", strerror(errno));
        failures_total++;
        return;
    }
    for (const struct ifaddrs *c = addrs; c != NULL; c = c->ifa_next) {
        if (c->ifa_addr == NULL || c->ifa_addr->sa_family != AF_INET)
            continue;
        if (!(c->ifa_flags & IFF_UP))
            continue;
        uint32_t a = ((const struct sockaddr_in *) c->ifa_addr)->sin_addr.s_addr;
        if (a == htonl(INADDR_ANY))
            continue;
        int dup = 0;
        for (unsigned i = 0; i < out->n; i++)
            if (out->v[i].addr == a)
                dup = 1;
        if (dup || out->n >= MAX_ADDRS)
            continue;
        out->v[out->n].addr = a;
        snprintf(out->v[out->n].ifname, IF_NAMESIZE, "%s",
                 c->ifa_name != NULL ? c->ifa_name : "?");
        out->n++;
    }
    freeifaddrs(addrs);
}

static int addr_set_has(const struct addr_set *s, uint32_t a) {
    for (unsigned i = 0; i < s->n; i++)
        if (s->v[i].addr == a)
            return 1;
    return 0;
}

static const char *addr_str(uint32_t a, char *buf, size_t len) {
    struct in_addr in = {.s_addr = a};
    if (inet_ntop(AF_INET, &in, buf, (socklen_t) len) == NULL)
        snprintf(buf, len, "?");
    return buf;
}

// ------------------------------------------------------------------ probe

// Returns 0 if the wildcard listener accepted and served us at `a`, else -1.
// `why` receives a short description of the failure -- distinguishing a RST
// ("refused", no listening PCB matched) from a timeout (a listener exists but
// is not accepting), because those two point at completely different bugs.
static int probe_addr(uint32_t a, int port, int expect_banner, char *why, size_t why_len) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        snprintf(why, why_len, "socket: %s", strerror(errno));
        return -1;
    }
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons((uint16_t) port);
    sin.sin_addr.s_addr = a;

    int rc = connect(fd, (struct sockaddr *) &sin, sizeof(sin));
    if (rc < 0 && errno != EINPROGRESS) {
        snprintf(why, why_len, "connect: %s", strerror(errno));
        close(fd);
        return -1;
    }
    if (rc < 0) {
        struct pollfd pfd = {.fd = fd, .events = POLLOUT};
        int pr = poll(&pfd, 1, 4000);
        if (pr == 0) {
            snprintf(why, why_len, "timeout (listener present but not accepting?)");
            close(fd);
            return -1;
        }
        if (pr < 0) {
            snprintf(why, why_len, "poll: %s", strerror(errno));
            close(fd);
            return -1;
        }
        int soerr = 0;
        socklen_t elen = sizeof(soerr);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &elen) < 0)
            soerr = errno;
        if (soerr != 0) {
            snprintf(why, why_len, "%s%s", strerror(soerr),
                     soerr == ECONNREFUSED ? " (RST: no listener covers this address)" : "");
            close(fd);
            return -1;
        }
    }

    // In --probe-only mode we are talking to somebody else's server (sshd,
    // say), so a completed handshake is the whole result; what it says is its
    // business. Reaching the listener at all is exactly the question.
    if (!expect_banner) {
        close(fd);
        return 0;
    }

    char buf[64];
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    if (poll(&pfd, 1, 4000) != 1) {
        snprintf(why, why_len, "connected but no banner");
        close(fd);
        return -1;
    }
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        snprintf(why, why_len, "connected but read failed");
        return -1;
    }
    buf[n] = '\0';
    if (strcmp(buf, BANNER) != 0) {
        snprintf(why, why_len, "wrong banner");
        return -1;
    }
    return 0;
}

// Probes every address in `set`, counting failures. `phase` labels the output.
static int expect_banner = 1;

static unsigned probe_all(const struct addr_set *set, const struct addr_set *baseline,
                          const char *phase) {
    unsigned bad = 0;
    for (unsigned i = 0; i < set->n; i++) {
        char abuf[INET_ADDRSTRLEN], why[160];
        addr_str(set->v[i].addr, abuf, sizeof(abuf));
        int is_new = baseline != NULL && !addr_set_has(baseline, set->v[i].addr);
        if (probe_addr(set->v[i].addr, probe_port, expect_banner, why, sizeof(why)) == 0) {
            test_logf("  ok   %s (%s)%s\n", abuf, set->v[i].ifname,
                      is_new ? "  [appeared after bind]" : "");
        } else {
            printf("FAIL %s: %s:%d (%s)%s -- %s\n", phase, abuf, probe_port,
                   set->v[i].ifname,
                   is_new ? " [appeared after bind]" : "", why);
            bad++;
        }
    }
    return bad;
}

// -------------------------------------------------------------------- main

static void usage(void) {
    printf("usage: inaddr_any_iface [-v] [--port N] [--watch SECONDS]\n"
           "  --port N        port to bind (default: an ephemeral port chosen by\n"
           "                  the host; pass one <1024 to exercise the\n"
           "                  privileged-port bind-fallback path deliberately)\n"
           "  --watch SECONDS hold the socket and wait for the host address set\n"
           "                  to change, then re-probe. Needs a real interface\n"
           "                  event (replug the cable, toggle WiFi).\n"
           "  --probe-only N  do not bind anything; probe an ALREADY RUNNING\n"
           "                  listener on port N at every local address. Run this\n"
           "                  from inside the guest when a server looks wedged:\n"
           "                  it answers, without any client machine's routing in\n"
           "                  the path, whether the listener really covers every\n"
           "                  address the host has.\n");
}

int main(int argc, char **argv) {
    int watch_secs = 0;
    int probe_only = 0;
    // test_init() rejects unknown options, so parse ours first and hand it
    // only what it knows about.
    int passthru_argc = 1;
    char *passthru[8];
    passthru[0] = argv[0];
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            probe_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--watch") == 0 && i + 1 < argc) {
            watch_secs = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--probe-only") == 0 && i + 1 < argc) {
            probe_only = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0) {
            usage();
            return 0;
        } else if (passthru_argc < 8) {
            passthru[passthru_argc++] = argv[i];
        }
    }
    test_init(passthru_argc, passthru);

    alarm(test_watchdog_secs(60 + (unsigned) watch_secs));
    signal(SIGPIPE, SIG_IGN);

    // Diagnostic mode: somebody else's listener, probed from inside the guest.
    // This is what separates "the listener does not cover that address" from
    // "the client machine's packets never got here" -- the loop never leaves
    // the host, so no routing, ARP, or link-local ambiguity on any other
    // machine can contribute to the answer.
    if (probe_only > 0) {
        probe_port = probe_only;
        expect_banner = 0;
        struct addr_set now;
        addr_set_collect(&now);
        if (now.n == 0) {
            printf("SKIP inaddr_any_iface: host reports no IPv4 addresses\n");
            return 0;
        }
        test_logf("probing existing listener on port %d at %u local address(es)\n",
                  probe_port, now.n);
        failures_total += probe_all(&now, NULL, "probe-only");
        return finish_suite("inaddr_any_iface");
    }

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        printf("FAIL socket: %s\n", strerror(errno));
        return 1;
    }
    int one = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_ANY);
    sin.sin_port = htons((uint16_t) probe_port);
    if (bind(listen_fd, (struct sockaddr *) &sin, sizeof(sin)) < 0) {
        // A privileged port the host genuinely refuses to a non-root process
        // is the tester's own choice, not a defect -- skip rather than fail.
        // (Under AOK this branch is rare: the bind fallback usually absorbs
        // the refusal, which is precisely what the coverage probe then
        // exposes as loopback-only reachability.)
        if (probe_port != 0 && probe_port < 1024 && (errno == EACCES || errno == EPERM)) {
            printf("SKIP inaddr_any_iface: host refuses port %d to this user (%s)\n",
                   probe_port, strerror(errno));
            return 0;
        }
        printf("FAIL bind 0.0.0.0:%d: %s\n", probe_port, strerror(errno));
        return 1;
    }
    if (listen(listen_fd, 16) < 0) {
        printf("FAIL listen: %s\n", strerror(errno));
        return 1;
    }

    // What the guest believes it bound. Under AOK this is rewritten back from
    // the NAT table when a fallback fired, so it reports the wildcard either
    // way -- which is exactly why the coverage probe below is the only way to
    // tell a real wildcard bind from a loopback-only one.
    struct sockaddr_in bound;
    socklen_t blen = sizeof(bound);
    if (getsockname(listen_fd, (struct sockaddr *) &bound, &blen) < 0) {
        printf("FAIL getsockname: %s\n", strerror(errno));
        return 1;
    }
    if (probe_port == 0)
        probe_port = ntohs(bound.sin_port);
    {
        char abuf[INET_ADDRSTRLEN];
        test_logf("bound: %s:%d\n", addr_str(bound.sin_addr.s_addr, abuf, sizeof(abuf)),
                  ntohs(bound.sin_port));
    }

    pthread_t th;
    if (pthread_create(&th, NULL, acceptor, NULL) != 0) {
        printf("FAIL pthread_create: %s\n", strerror(errno));
        return 1;
    }

    struct addr_set baseline;
    addr_set_collect(&baseline);
    test_logf("host has %u IPv4 address(es) at bind time\n", baseline.n);
    if (baseline.n == 0) {
        printf("SKIP inaddr_any_iface: host reports no IPv4 addresses\n");
        acceptor_stop = 1;
        pthread_join(th, NULL);
        close(listen_fd);
        return 0;
    }

    test_logf("-- coverage: every address present at bind time\n");
    failures_total += probe_all(&baseline, NULL, "coverage");

    if (watch_secs > 0) {
        printf("waiting up to %ds for the host address set to change"
               " (replug the cable / toggle an interface now)...\n", watch_secs);
        fflush(stdout);
        struct addr_set now;
        int changed = 0;
        for (int elapsed = 0; elapsed < watch_secs && !changed; elapsed++) {
            sleep(1);
            addr_set_collect(&now);
            if (now.n != baseline.n) {
                changed = 1;
                break;
            }
            for (unsigned i = 0; i < now.n; i++)
                if (!addr_set_has(&baseline, now.v[i].addr))
                    changed = 1;
        }
        if (!changed) {
            printf("SKIP ordering: host address set never changed within %ds\n", watch_secs);
        } else {
            // Let the new address finish coming up (DAD, link negotiation)
            // before concluding anything from a refusal.
            sleep(3);
            addr_set_collect(&now);
            for (unsigned i = 0; i < baseline.n; i++) {
                char abuf[INET_ADDRSTRLEN];
                if (!addr_set_has(&now, baseline.v[i].addr))
                    test_logf("  gone: %s (%s)\n",
                              addr_str(baseline.v[i].addr, abuf, sizeof(abuf)),
                              baseline.v[i].ifname);
            }
            test_logf("-- ordering: same socket, address set changed\n");
            failures_total += probe_all(&now, &baseline, "ordering");
        }
    }

    acceptor_stop = 1;
    pthread_join(th, NULL);
    close(listen_fd);
    return finish_suite("inaddr_any_iface");
}
