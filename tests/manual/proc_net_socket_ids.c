// /proc/net/{tcp,tcp6,udp,udp6,unix} and NETLINK_SOCK_DIAG (what ss reads)
// must describe a socket by the numbers the rest of the system uses for it.
//
// Regression for a triage report: "/proc/net/tcp is wrong. Addresses are
// byte-swapped (127.0.0.1 reads as 1.0.0.127), and the socket IDs don't match
// the processes' open files. As a result lsof -i finds nothing and ss -p can't
// name processes, even as root." Four bugs behind it:
//
//   - Addresses were printed in host order. Linux prints the __be32 as it sits
//     in memory, read as a little-endian word: 127.0.0.1:22 is 0100007F:0016,
//     and ::1 is 00000000000000000000000001000000.
//   - The inode column was a kernel pointer, not the socket's inode -- the
//     number fstat reports as st_ino and /proc/<pid>/fd/N spells socket:[N].
//     lsof and ss join their tables to processes on exactly that number. The
//     sock_diag reply had the same pointer.
//   - The uid column was the READER's uid, not the socket owner's.
//   - A socket (and a pipe) belonged to its creator's real uid, where Linux
//     uses the filesystem uid: a setuid program's socket is its euid's.
//
// Plus what Linux lists at all: only sockets in its lookup tables -- a TCP
// socket while listening or connected, a UDP socket once it has a port -- the
// header and padding of each file, and a unix listener shown as one (the
// listings asked the host for SO_ACCEPTCONN, which Darwin does not have).
//
// Oracle: Linux 6.12 (camd), glibc x86_64 and -m32, as root and as a user.
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include "test_common.h"

#define TEST_NAME "proc_net_socket_ids"

// <linux/netlink.h> and <linux/sock_diag.h> are not on every root, so the
// handful of definitions used here are spelled out.
#ifndef AF_NETLINK
#define AF_NETLINK 16
#endif
#define T_NETLINK_SOCK_DIAG 4
#define T_SOCK_DIAG_BY_FAMILY 20
#define T_NLMSG_ERROR 2
#define T_NLMSG_DONE 3
#define T_NLM_F_REQUEST 0x1
#define T_NLM_F_DUMP 0x300

struct t_nlmsghdr {
    uint32_t len;
    uint16_t type;
    uint16_t flags;
    uint32_t seq;
    uint32_t pid;
};
struct t_sockaddr_nl {
    uint16_t family;
    uint16_t pad;
    uint32_t pid;
    uint32_t groups;
};
struct t_inet_diag_sockid {
    uint16_t sport;
    uint16_t dport;
    uint32_t src[4];
    uint32_t dst[4];
    uint32_t ifindex;
    uint32_t cookie[2];
};
struct t_inet_diag_req_v2 {
    uint8_t family;
    uint8_t protocol;
    uint8_t ext;
    uint8_t pad;
    uint32_t states;
    struct t_inet_diag_sockid id;
};
struct t_inet_diag_msg {
    uint8_t family;
    uint8_t state;
    uint8_t timer;
    uint8_t retrans;
    struct t_inet_diag_sockid id;
    uint32_t expires;
    uint32_t rqueue;
    uint32_t wqueue;
    uint32_t uid;
    uint32_t inode;
};
struct t_unix_diag_req {
    uint8_t family;
    uint8_t protocol;
    uint16_t pad;
    uint32_t states;
    uint32_t ino;
    uint32_t show;
    uint32_t cookie[2];
};
struct t_unix_diag_msg {
    uint8_t family;
    uint8_t type;
    uint8_t state;
    uint8_t pad;
    uint32_t ino;
    uint32_t cookie[2];
};

static void check(int cond, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (cond) {
        if (test_verbose) {
            printf("ok: ");
            vprintf(fmt, ap);
            printf("\n");
        }
    } else {
        printf("FAIL: ");
        vprintf(fmt, ap);
        printf("\n");
        failures_total++;
    }
    va_end(ap);
}

static unsigned long st_ino_of(int fd) {
    struct stat st;
    if (fstat(fd, &st) < 0)
        return 0;
    return (unsigned long) st.st_ino;
}

static unsigned st_uid_of(int fd) {
    struct stat st;
    if (fstat(fd, &st) < 0)
        return (unsigned) -1;
    return (unsigned) st.st_uid;
}

// The N in /proc/self/fd/<fd> -> socket:[N]; 0 if the link is not that shape.
static unsigned long link_ino_of(int fd) {
    char path[64], target[128];
    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    ssize_t n = readlink(path, target, sizeof(target) - 1);
    if (n < 0)
        return 0;
    target[n] = '\0';
    unsigned long ino = 0;
    if (sscanf(target, "socket:[%lu]", &ino) != 1)
        return 0;
    return ino;
}

static unsigned local_port(int fd) {
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    if (getsockname(fd, (struct sockaddr *) &ss, &len) < 0)
        return 0;
    if (ss.ss_family == AF_INET)
        return ntohs(((struct sockaddr_in *) &ss)->sin_port);
    if (ss.ss_family == AF_INET6)
        return ntohs(((struct sockaddr_in6 *) &ss)->sin6_port);
    return 0;
}

struct row {
    char local[33];
    unsigned lport;
    char remote[33];
    unsigned rport;
    unsigned state;
    unsigned uid;
    unsigned long inode;
};

// The row of /proc/net/<file> whose inode column is `inode`. Returns how many
// rows carry it (a socket must be listed once, or not at all).
static int find_row(const char *file, unsigned long inode, struct row *out) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/net/%s", file);
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return -1;
    char line[512];
    int found = 0;
    if (fgets(line, sizeof(line), f) == NULL) { // header
        fclose(f);
        return -1;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        struct row r = {};
        if (sscanf(line, " %*[^:]: %32[0-9A-Fa-f]:%x %32[0-9A-Fa-f]:%x %x %*x:%*x %*x:%*x %*x %u %*d %lu",
                   r.local, &r.lport, r.remote, &r.rport, &r.state, &r.uid, &r.inode) != 7)
            continue;
        if (r.inode != inode)
            continue;
        if (found++ == 0 && out != NULL)
            *out = r;
    }
    fclose(f);
    return found;
}

// The whole of one socket's row: listed once, local address, peer, state, uid.
static void check_row(const char *file, const char *what, int fd, const char *want_local,
        unsigned want_lport, const char *want_remote, unsigned want_rport, unsigned want_state) {
    unsigned long ino = st_ino_of(fd);
    unsigned long link = link_ino_of(fd);
    check(ino != 0 && link == ino,
          "%s: /proc/self/fd link socket:[%lu] == fstat st_ino %lu", what, link, ino);
    struct row r = {};
    int n = find_row(file, ino, &r);
    check(n == 1, "%s: listed exactly once in /proc/net/%s by inode %lu (found %d)",
          what, file, ino, n);
    if (n < 1)
        return;
    check(strcmp(r.local, want_local) == 0 && r.lport == want_lport,
          "%s: local %s:%04X, want %s:%04X", what, r.local, r.lport, want_local, want_lport);
    check(strcmp(r.remote, want_remote) == 0 && r.rport == want_rport,
          "%s: remote %s:%04X, want %s:%04X", what, r.remote, r.rport, want_remote, want_rport);
    check(r.state == want_state, "%s: state %02X, want %02X", what, r.state, want_state);
    unsigned owner = st_uid_of(fd);
    check(r.uid == owner, "%s: uid column %u == the socket's st_uid %u (reader euid %u)",
          what, r.uid, owner, (unsigned) geteuid());
}

static void check_unlisted(const char *file, const char *what, int fd) {
    int n = find_row(file, st_ino_of(fd), NULL);
    check(n == 0, "%s: not in /proc/net/%s, as on Linux (found %d)", what, file, n);
}

// Header text, trailing padding removed, and -- for the two IPv4 files, which
// Linux pads -- every line exactly `width` bytes including its newline.
static void check_layout(const char *file, const char *want_header, size_t width) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/net/%s", file);
    FILE *f = fopen(path, "r");
    check(f != NULL, "open %s", path);
    if (f == NULL)
        return;
    char line[512];
    int lineno = 0, bad_width = 0;
    size_t first_bad = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        size_t len = strlen(line);
        if (width != 0 && len != width && bad_width++ == 0)
            first_bad = len;
        if (lineno++ == 0) {
            while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == ' '))
                line[--len] = '\0';
            check(strcmp(line, want_header) == 0, "%s header is Linux's: got \"%s\"", path, line);
        }
    }
    fclose(f);
    if (width != 0)
        check(bad_width == 0, "%s: every line is %zu bytes, as Linux pads them (%d not, first %zu)",
              path, width, bad_width, first_bad);
}

static int listener(int family, const void *addr, socklen_t len, int type) {
    int fd = socket(family, type, 0);
    if (fd < 0)
        return -1;
    if (bind(fd, addr, len) < 0 || (type == SOCK_STREAM && listen(fd, 4) < 0)) {
        close(fd);
        return -1;
    }
    return fd;
}

// ---- NETLINK_SOCK_DIAG ------------------------------------------------------

// One dump request; calls `each` for every SOCK_DIAG_BY_FAMILY reply payload.
// Returns 0, or -errno from an NLMSG_ERROR reply or a socket call.
static int diag_dump(const void *req, size_t req_len,
        void (*each)(const void *payload, size_t len, void *ctx), void *ctx) {
    int nl = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, T_NETLINK_SOCK_DIAG);
    if (nl < 0)
        return -errno;
    struct {
        struct t_nlmsghdr h;
        char body[64];
    } msg = {};
    msg.h.len = sizeof(msg.h) + req_len;
    msg.h.type = T_SOCK_DIAG_BY_FAMILY;
    msg.h.flags = T_NLM_F_REQUEST | T_NLM_F_DUMP;
    msg.h.seq = 4242;
    memcpy(msg.body, req, req_len);
    struct t_sockaddr_nl kernel = {.family = AF_NETLINK};
    if (sendto(nl, &msg, msg.h.len, 0, (struct sockaddr *) &kernel, sizeof(kernel)) < 0) {
        int err = -errno;
        close(nl);
        return err;
    }
    static char buf[65536];
    for (;;) {
        ssize_t n = recv(nl, buf, sizeof(buf), 0);
        if (n < 0) {
            int err = -errno;
            close(nl);
            return err;
        }
        size_t off = 0;
        while (off + sizeof(struct t_nlmsghdr) <= (size_t) n) {
            struct t_nlmsghdr *h = (struct t_nlmsghdr *) (buf + off);
            if (h->len < sizeof(*h) || off + h->len > (size_t) n)
                break;
            if (h->type == T_NLMSG_DONE) {
                close(nl);
                return 0;
            }
            if (h->type == T_NLMSG_ERROR) {
                int32_t code;
                memcpy(&code, h + 1, sizeof(code));
                close(nl);
                return code;
            }
            if (h->type == T_SOCK_DIAG_BY_FAMILY)
                each(h + 1, h->len - sizeof(*h), ctx);
            off += (h->len + 3) & ~3u;
        }
    }
}

struct inet_want {
    unsigned long inode;
    int found;
    struct t_inet_diag_msg msg;
};

static void inet_each(const void *payload, size_t len, void *ctx) {
    struct inet_want *want = ctx;
    struct t_inet_diag_msg m;
    if (len < sizeof(m))
        return;
    memcpy(&m, payload, sizeof(m));
    if (m.inode == (uint32_t) want->inode && want->found++ == 0)
        want->msg = m;
}

// The sock_diag record of a TCP socket, found by its inode, carries its
// addresses in network order, its owner and its state.
static void check_diag_inet(const char *what, int family, int fd, const void *want_src,
        const void *want_dst, size_t addr_len, unsigned want_dport, unsigned want_state) {
    struct t_inet_diag_req_v2 req = {.family = (uint8_t) family, .protocol = IPPROTO_TCP,
                                     .states = 0xffffffffu};
    struct inet_want want = {.inode = st_ino_of(fd)};
    int err = diag_dump(&req, sizeof(req), inet_each, &want);
    check(err == 0, "%s: sock_diag TCP dump succeeds (%d %s)", what, err, strerror(-err));
    check(want.found == 1, "%s: sock_diag lists inode %lu exactly once (found %d)",
          what, want.inode, want.found);
    if (want.found < 1)
        return;
    struct t_inet_diag_msg *m = &want.msg;
    check(m->family == family, "%s: sock_diag family %u", what, m->family);
    check(ntohs(m->id.sport) == local_port(fd), "%s: sock_diag sport %u == %u",
          what, ntohs(m->id.sport), local_port(fd));
    check(memcmp(m->id.src, want_src, addr_len) == 0,
          "%s: sock_diag src is the local address in network order (%08x)", what, m->id.src[0]);
    check(ntohs(m->id.dport) == want_dport, "%s: sock_diag dport %u == %u",
          what, ntohs(m->id.dport), want_dport);
    check(memcmp(m->id.dst, want_dst, addr_len) == 0,
          "%s: sock_diag dst is the peer address in network order (%08x)", what, m->id.dst[0]);
    check(m->state == want_state, "%s: sock_diag state %u == %u", what, m->state, want_state);
    check(m->uid == st_uid_of(fd), "%s: sock_diag uid %u == the socket's st_uid %u",
          what, m->uid, st_uid_of(fd));
}

static void check_diag_inet_absent(const char *what, int fd) {
    struct t_inet_diag_req_v2 req = {.family = AF_INET, .protocol = IPPROTO_TCP,
                                     .states = 0xffffffffu};
    struct inet_want want = {.inode = st_ino_of(fd)};
    int err = diag_dump(&req, sizeof(req), inet_each, &want);
    check(err == 0 && want.found == 0, "%s: not in sock_diag, as on Linux (err %d, found %d)",
          what, err, want.found);
}

struct unix_want {
    unsigned long inode;
    int found;
    unsigned state;
};

static void unix_each(const void *payload, size_t len, void *ctx) {
    struct unix_want *want = ctx;
    struct t_unix_diag_msg m;
    if (len < sizeof(m))
        return;
    memcpy(&m, payload, sizeof(m));
    if (m.ino == (uint32_t) want->inode && want->found++ == 0)
        want->state = m.state;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));

    // ---- IPv4 -------------------------------------------------------------
    struct sockaddr_in lo = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    int tl = listener(AF_INET, &lo, sizeof(lo), SOCK_STREAM);
    check(tl >= 0, "TCP listener on 127.0.0.1 (%s)", strerror(errno));
    if (tl < 0)
        return finish_suite(TEST_NAME);
    unsigned port = local_port(tl);

    int client = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in to = lo;
    to.sin_port = htons(port);
    check(client >= 0 && connect(client, (struct sockaddr *) &to, sizeof(to)) == 0,
          "connect to the listener (%s)", strerror(errno));
    int server = accept(tl, NULL, NULL);
    check(server >= 0, "accept (%s)", strerror(errno));
    unsigned cport = local_port(client);

    int udp = listener(AF_INET, &lo, sizeof(lo), SOCK_DGRAM);
    check(udp >= 0, "UDP socket bound to 127.0.0.1 (%s)", strerror(errno));

    check_row("tcp", "tcp listener", tl, "0100007F", port, "00000000", 0, 0x0A);
    check_row("tcp", "tcp client", client, "0100007F", cport, "0100007F", port, 0x01);
    check_row("tcp", "tcp accepted", server, "0100007F", port, "0100007F", cport, 0x01);
    if (udp >= 0)
        check_row("udp", "udp bound", udp, "0100007F", local_port(udp), "00000000", 0, 0x07);

    // Only what is in Linux's lookup tables is listed: not a socket that was
    // only created, nor a TCP socket bound but not listening.
    int fresh_tcp = socket(AF_INET, SOCK_STREAM, 0);
    int fresh_udp = socket(AF_INET, SOCK_DGRAM, 0);
    int bound_tcp = socket(AF_INET, SOCK_STREAM, 0);
    check(bound_tcp >= 0 && bind(bound_tcp, (struct sockaddr *) &lo, sizeof(lo)) == 0,
          "bind a TCP socket without listening (%s)", strerror(errno));
    check_unlisted("tcp", "unbound tcp socket", fresh_tcp);
    check_unlisted("udp", "unbound udp socket", fresh_udp);
    check_unlisted("tcp", "bound, not listening, tcp socket", bound_tcp);

    check_layout("tcp", "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when "
                 "retrnsmt   uid  timeout inode", 150);
    check_layout("udp", "   sl  local_address rem_address   st tx_queue rx_queue tr tm->when "
                 "retrnsmt   uid  timeout inode ref pointer drops", 128);
    check_layout("tcp6", "  sl  local_address                         remote_address          "
                 "              st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode", 0);
    check_layout("udp6", "  sl  local_address                         remote_address          "
                 "              st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode "
                 "ref pointer drops", 0);

    // ---- sock_diag (ss) ---------------------------------------------------
    struct in_addr loopback = {.s_addr = htonl(INADDR_LOOPBACK)};
    struct in_addr any = {.s_addr = 0};
    check_diag_inet("diag listener", AF_INET, tl, &loopback, &any, sizeof(loopback), 0, 10);
    check_diag_inet("diag client", AF_INET, client, &loopback, &loopback, sizeof(loopback),
                    port, 1);
    check_diag_inet_absent("diag unbound tcp socket", fresh_tcp);

    // ---- IPv6 -------------------------------------------------------------
    struct sockaddr_in6 lo6 = {.sin6_family = AF_INET6, .sin6_addr = IN6ADDR_LOOPBACK_INIT};
    int tl6 = listener(AF_INET6, &lo6, sizeof(lo6), SOCK_STREAM);
    int udp6 = listener(AF_INET6, &lo6, sizeof(lo6), SOCK_DGRAM);
    if (tl6 < 0 || udp6 < 0) {
        printf("%s: note: no IPv6 loopback (%s); v6 rows not checked\n", TEST_NAME,
               strerror(errno));
    } else {
        const char *v6lo = "00000000000000000000000001000000";
        const char *v6any = "00000000000000000000000000000000";
        check_row("tcp6", "tcp6 listener", tl6, v6lo, local_port(tl6), v6any, 0, 0x0A);
        check_row("udp6", "udp6 bound", udp6, v6lo, local_port(udp6), v6any, 0, 0x07);
        struct in6_addr any6 = IN6ADDR_ANY_INIT;
        check_diag_inet("diag tcp6 listener", AF_INET6, tl6, &lo6.sin6_addr, &any6,
                        sizeof(any6), 0, 10);
    }

    // ---- AF_UNIX ----------------------------------------------------------
    char upath[108];
    snprintf(upath, sizeof(upath), "/tmp/pnsi-%d.sock", (int) getpid());
    unlink(upath);
    struct sockaddr_un un = {.sun_family = AF_UNIX};
    snprintf(un.sun_path, sizeof(un.sun_path), "%s", upath);
    int ul = listener(AF_UNIX, &un, sizeof(un), SOCK_STREAM);
    check(ul >= 0, "unix listener at %s (%s)", upath, strerror(errno));
    if (ul >= 0) {
        unsigned long ino = st_ino_of(ul);
        check(link_ino_of(ul) == ino, "unix listener: fd link inode == st_ino %lu", ino);
        FILE *f = fopen("/proc/net/unix", "r");
        int found = 0;
        unsigned flags = 0, st = 0;
        char line[512];
        while (f != NULL && fgets(line, sizeof(line), f) != NULL) {
            unsigned long row_ino = 0;
            unsigned row_flags = 0, row_st = 0;
            char path[256] = "";
            if (sscanf(line, "%*[^:]: %*x %*x %x %*x %x %lu %255s", &row_flags, &row_st,
                       &row_ino, path) >= 3 && row_ino == ino && strcmp(path, upath) == 0 &&
                    found++ == 0) {
                flags = row_flags;
                st = row_st;
            }
        }
        if (f != NULL)
            fclose(f);
        check(found == 1, "/proc/net/unix lists %s by inode %lu (found %d)", upath, ino, found);
        // __SO_ACCEPTCON, and SS_UNCONNECTED, as a listener is.
        check(found < 1 || (flags == 0x10000 && st == 1),
              "/proc/net/unix flags the listener: Flags %08X St %02X, want 00010000 01", flags, st);

        struct t_unix_diag_req ureq = {.family = AF_UNIX, .states = 0xffffffffu};
        struct unix_want uwant = {.inode = ino};
        int err = diag_dump(&ureq, sizeof(ureq), unix_each, &uwant);
        check(err == 0 && uwant.found == 1,
              "unix_diag lists the listener by inode %lu (err %d, found %d)", ino, err, uwant.found);
        check(uwant.found < 1 || uwant.state == 10, "unix_diag state %u is TCP_LISTEN (10)",
              uwant.state);
        close(ul);
        unlink(upath);
    }

    // ---- owner ------------------------------------------------------------
    // As root, the reader and the owner can be told apart. Unprivileged, every
    // socket is the reader's own, and the checks above already compared the
    // column against st_uid.
    if (geteuid() == 0) {
        // A chown of the socket moves the column (sockfs_setattr carries it to
        // sk_uid); root reading it still sees 65534, not 0.
        int owned = listener(AF_INET, &lo, sizeof(lo), SOCK_STREAM);
        check(owned >= 0 && fchown(owned, 65534, 65534) == 0, "fchown a socket (%s)",
              strerror(errno));
        if (owned >= 0) {
            check(st_uid_of(owned) == 65534, "fchown'd socket st_uid %u", st_uid_of(owned));
            check_row("tcp", "fchown'd listener", owned, "0100007F", local_port(owned),
                      "00000000", 0, 0x0A);
            close(owned);
        }

        // A socket made while only the EFFECTIVE uid is 65534 is 65534's:
        // Linux takes the filesystem uid. So is a pipe, mode 0600.
        if (setresuid((uid_t) -1, 65534, (uid_t) -1) == 0) {
            int as_user = listener(AF_INET, &lo, sizeof(lo), SOCK_STREAM);
            int pipefd[2] = {-1, -1};
            int pipe_ok = pipe(pipefd) == 0;
            struct stat pst = {};
            if (pipe_ok)
                fstat(pipefd[0], &pst);
            int restored = setresuid((uid_t) -1, 0, (uid_t) -1) == 0;
            check(restored, "setresuid back to euid 0 (%s)", strerror(errno));
            check(as_user >= 0, "listener made with euid 65534 (%s)", strerror(errno));
            if (as_user >= 0) {
                check(st_uid_of(as_user) == 65534,
                      "socket made with ruid 0, euid 65534 is owned by 65534 (st_uid %u)",
                      st_uid_of(as_user));
                check_row("tcp", "euid-65534 listener", as_user, "0100007F",
                          local_port(as_user), "00000000", 0, 0x0A);
                close(as_user);
            }
            check(pipe_ok && pst.st_uid == 65534,
                  "pipe made with ruid 0, euid 65534 is owned by 65534 (st_uid %u)",
                  (unsigned) pst.st_uid);
            check(pipe_ok && (pst.st_mode & 07777) == 0600, "pipe mode %o == 0600",
                  (unsigned) (pst.st_mode & 07777));
            if (pipe_ok) {
                close(pipefd[0]);
                close(pipefd[1]);
            }
            if (!restored)
                return finish_suite(TEST_NAME);
        } else {
            check(0, "setresuid(-1, 65534, -1) as root (%s)", strerror(errno));
        }
    } else {
        test_logf("not root: owner-change checks skipped\n");
    }

    return finish_suite(TEST_NAME);
}
