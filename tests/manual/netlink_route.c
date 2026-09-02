/*
 * netlink_route.c — self-checking regression lock for AF_NETLINK rtnetlink
 * dumps issued through the bare read()/write() (and readv()/writev()) path.
 *
 * iSH-AOK emulates NETLINK_ROUTE sockets entirely in-process (real_fd < 0).
 * The send/recv-family syscalls (sendto/sendmsg/recvfrom/recvmsg) special-case
 * AF_NETLINK and route to the in-process handler, but sock_write()/sock_read()
 * used to fall through to the `real_fd < 0` guard and return EOPNOTSUPP.
 *
 * BusyBox's `ip` (Alpine's default) issues its RTM_GET* dump request with
 * write() and drains the reply with read(), so `ip addr` failed with
 *     ip: write error: Not supported     (musl strerror for EOPNOTSUPP)
 * producing no output at all. This test reproduces that exact flow.
 *
 * Each method (write/read, writev/readv, send/recv) must:
 *   - have the request write succeed (return the request length, NOT EOPNOTSUPP)
 *   - deliver a reply stream terminated by NLMSG_DONE with no NLMSG_ERROR
 *   - contain at least one RTM_NEWLINK entry for the link dump (lo is universal)
 *
 * Dump conformance details systemd's sd-netlink depends on (each of these,
 * when missing, killed systemd-resolved -- and with it all hostname
 * resolution via nsswitch's "resolve [!UNAVAIL=return]"):
 *   - every part AND the terminating NLMSG_DONE carry NLM_F_MULTI
 *     (sd-netlink only ends a dump inside its `flags & NLM_F_MULTI` branch)
 *   - every RTM_NEWADDR carries the IFA_FLAGS u32 attribute (resolved's
 *     link_address_update_rtnl() read of it is required, ENODATA on miss)
 *   - no link reports IFF_RUNNING without IFF_LOWER_UP (Linux never does;
 *     resolved's link_relevant() insists on UP|LOWER_UP)
 *   - no link reports an empty flag word (Linux never does either; Darwin's
 *     6to4 tunnel stf0 is "flags=0<>" on macOS and iOS, and forwarding that
 *     zero SIGABRTs systemd-networkd -- see the comment at the check)
 *
 * Exits 0 and prints "netlink_route: PASS" on success.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <netinet/in.h>

#include "test_common.h"

/* Self-contained netlink/rtnetlink ABI (avoids a linux-headers dependency so
 * this builds under musl-only Alpine too). Values are kernel-ABI stable. */
#ifndef AF_NETLINK
#define AF_NETLINK 16
#endif
#ifndef NETLINK_ROUTE
#define NETLINK_ROUTE 0
#endif

struct nl_hdr {
    uint32_t nlmsg_len;
    uint16_t nlmsg_type;
    uint16_t nlmsg_flags;
    uint32_t nlmsg_seq;
    uint32_t nlmsg_pid;
};
struct nl_genmsg { unsigned char rtgen_family; };
struct nl_err { int error; struct nl_hdr msg; };
struct nl_sockaddr {
    uint16_t nl_family;
    uint16_t nl_pad;
    uint32_t nl_pid;
    uint32_t nl_groups;
};

#define NLMSG_ALIGNTO 4U
#define NL_ALIGN(len)  (((len) + NLMSG_ALIGNTO - 1) & ~(NLMSG_ALIGNTO - 1))
#define NL_HDRLEN      ((int) NL_ALIGN(sizeof(struct nl_hdr)))
#define NL_LENGTH(l)   ((l) + NL_HDRLEN)
#define NL_DATA(nlh)   ((void *) ((char *) (nlh) + NL_HDRLEN))
#define NL_OK(nlh, len) \
    ((len) >= (int) sizeof(struct nl_hdr) && \
     (nlh)->nlmsg_len >= sizeof(struct nl_hdr) && \
     (nlh)->nlmsg_len <= (unsigned) (len))
#define NL_NEXT(nlh, len) \
    ((len) -= NL_ALIGN((nlh)->nlmsg_len), \
     (struct nl_hdr *) ((char *) (nlh) + NL_ALIGN((nlh)->nlmsg_len)))

#define NLMSG_ERROR_ 2
#define NLMSG_DONE_  3
#define NLM_F_REQUEST_ 0x001
#define NLM_F_MULTI_   0x002
#define NLM_F_ROOT_    0x100
#define NLM_F_MATCH_   0x200
#define NLM_F_DUMP_    (NLM_F_ROOT_ | NLM_F_MATCH_)
#define RTM_NEWLINK_ 16
#define RTM_GETLINK_ 18
#define RTM_NEWADDR_ 20
#define RTM_GETADDR_ 22
#define IFA_FLAGS_ 8
#define IFF_RUNNING_  0x40
#define IFF_LOWER_UP_ 0x10000

struct nl_ifinfomsg {
    unsigned char ifi_family;
    unsigned char ifi_pad;
    uint16_t ifi_type;
    int32_t ifi_index;
    uint32_t ifi_flags;
    uint32_t ifi_change;
};
struct nl_ifaddrmsg {
    unsigned char ifa_family;
    unsigned char ifa_prefixlen;
    unsigned char ifa_flags;
    unsigned char ifa_scope;
    uint32_t ifa_index;
};
struct nl_rtattr {
    uint16_t rta_len;
    uint16_t rta_type;
};

/* Does the payload of an RTM_NEWADDR message carry attribute `type`? */
static int addr_msg_has_attr(struct nl_hdr *nlh, uint16_t type) {
    int len = (int) nlh->nlmsg_len - NL_HDRLEN - (int) NL_ALIGN(sizeof(struct nl_ifaddrmsg));
    struct nl_rtattr *rta = (struct nl_rtattr *)
        ((char *) NL_DATA(nlh) + NL_ALIGN(sizeof(struct nl_ifaddrmsg)));
    while (len >= (int) sizeof(*rta) && rta->rta_len >= sizeof(*rta) &&
            (int) rta->rta_len <= len) {
        if (rta->rta_type == type)
            return 1;
        len -= NL_ALIGN(rta->rta_len);
        rta = (struct nl_rtattr *) ((char *) rta + NL_ALIGN(rta->rta_len));
    }
    return 0;
}

/* Interface ioctls that BusyBox `ip addr` issues on an AF_INET socket. */
#define SIOCGIFNAME_   0x8910
#define SIOCGIFTXQLEN_ 0x8942
struct nl_ifreq {
    char ifr_name[16];
    union { int ival; char pad[24]; } u;
};

enum io_method { IO_RW, IO_VEC, IO_SEND };

static void check(const char *label, long got, long exp) {
    int bad = got != exp;
    test_log_if(bad, "%s: got=%ld exp=%ld\n", label, got, exp);
    if (bad)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) exp, 0, 0);
}

static volatile sig_atomic_t alarm_fired;
static void on_alarm(int s) { (void) s; alarm_fired = 1; }
static void arm_alarm(int sec) {
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_alarm; sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);
    alarm_fired = 0; alarm(sec);
}

static const char *method_name(enum io_method m) {
    return m == IO_RW ? "rw" : m == IO_VEC ? "vec" : "send";
}

/* Send one RTM_GET* dump request via the chosen io method; returns the
 * write/send result (request length on success, <0 on error). */
static ssize_t send_dump_family(int fd, enum io_method m, int type, uint32_t seq,
                                uint8_t family) {
    char req[NL_LENGTH(sizeof(struct nl_genmsg))];
    memset(req, 0, sizeof req);
    struct nl_hdr *nlh = (struct nl_hdr *) req;
    nlh->nlmsg_len = NL_LENGTH(sizeof(struct nl_genmsg));
    nlh->nlmsg_type = (uint16_t) type;
    nlh->nlmsg_flags = NLM_F_REQUEST_ | NLM_F_DUMP_;
    nlh->nlmsg_seq = seq;
    nlh->nlmsg_pid = 0;
    ((struct nl_genmsg *) NL_DATA(nlh))->rtgen_family = family;

    if (m == IO_RW)
        return write(fd, req, sizeof req);
    if (m == IO_VEC) {
        struct iovec iov = { .iov_base = req, .iov_len = sizeof req };
        return writev(fd, &iov, 1);
    }
    struct nl_sockaddr dst = { .nl_family = AF_NETLINK };
    return sendto(fd, req, sizeof req, 0, (struct sockaddr *) &dst, sizeof dst);
}

static ssize_t send_dump(int fd, enum io_method m, int type, uint32_t seq) {
    return send_dump_family(fd, m, type, seq, 0); /* AF_UNSPEC: all */
}

static ssize_t recv_batch(int fd, enum io_method m, char *buf, size_t cap) {
    if (m == IO_RW)
        return read(fd, buf, cap);
    if (m == IO_VEC) {
        struct iovec iov = { .iov_base = buf, .iov_len = cap };
        return readv(fd, &iov, 1);
    }
    return recv(fd, buf, cap, 0);
}

/* Run a full dump+drain over one io method and validate the reply stream. */
static void run_dump(enum io_method m, int req_type, int expect_new) {
    char lab[64];
    const char *mn = method_name(m);

    int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    snprintf(lab, sizeof lab, "%s.socket", mn);
    check(lab, fd >= 0, 1);
    if (fd < 0)
        return;

    errno = 0;
    ssize_t w = send_dump(fd, m, req_type, 0x1234);
    test_log_if(w < 0, "  %s.write errno=%d (%s)\n", mn, errno, strerror(errno));
    /* The core regression: the request write must succeed, not EOPNOTSUPP. */
    snprintf(lab, sizeof lab, "%s.write_ok", mn);
    check(lab, w > 0, 1);
    snprintf(lab, sizeof lab, "%s.write_not_eopnotsupp", mn);
    check(lab, !(w < 0 && errno == EOPNOTSUPP), 1);

    char buf[65536];
    int saw_done = 0, saw_err = 0, saw_new = 0, batches = 0;
    int done_multi = 0, parts_not_multi = 0;
    int addrs_missing_ifa_flags = 0, links_running_not_lower_up = 0;
    int links_zero_flags = 0;
    arm_alarm(5);
    for (int guard = 0; guard < 64 && !saw_done && !alarm_fired; guard++) {
        errno = 0;
        ssize_t n = recv_batch(fd, m, buf, sizeof buf);
        if (n < 0) {
            test_log_if(1, "  %s.read errno=%d (%s)\n", mn, errno, strerror(errno));
            break;
        }
        if (n == 0)
            break;
        batches++;
        struct nl_hdr *nlh = (struct nl_hdr *) buf;
        int len = (int) n;
        for (; NL_OK(nlh, len); nlh = NL_NEXT(nlh, len)) {
            if (nlh->nlmsg_type == NLMSG_DONE_) {
                saw_done = 1;
                /* sd-netlink only recognizes end-of-dump when the DONE
                 * carries NLM_F_MULTI like the rest of the dump; a bare DONE
                 * made every systemd link/addr enumeration return nothing. */
                done_multi = !!(nlh->nlmsg_flags & NLM_F_MULTI_);
                break;
            }
            if (nlh->nlmsg_type == NLMSG_ERROR_) {
                struct nl_err *e = NL_DATA(nlh);
                if (e->error != 0) saw_err = e->error;
            }
            if (nlh->nlmsg_type == expect_new) {
                saw_new++;
                if (!(nlh->nlmsg_flags & NLM_F_MULTI_))
                    parts_not_multi++;
                /* systemd-resolved requires the IFA_FLAGS u32 attribute on
                 * every address (its absence killed the daemon at startup
                 * with ENODATA / "Could not create manager"). */
                if (expect_new == RTM_NEWADDR_ && !addr_msg_has_attr(nlh, IFA_FLAGS_))
                    addrs_missing_ifa_flags++;
                /* Linux never reports carrier (RUNNING) without LOWER_UP;
                 * resolved's link_relevant() insists on UP|LOWER_UP. */
                if (expect_new == RTM_NEWLINK_) {
                    struct nl_ifinfomsg *ifi = NL_DATA(nlh);
                    if ((ifi->ifi_flags & IFF_RUNNING_) && !(ifi->ifi_flags & IFF_LOWER_UP_))
                        links_running_not_lower_up++;
                    /* No Linux netdev has an empty flag word -- every device
                     * type's setup sets something, so even a down device shows
                     * BROADCAST|MULTICAST or (tunnels, e.g. sit0) NOARP. Darwin
                     * does have flagless interfaces: `ifconfig stf0`, the 6to4
                     * tunnel present on macOS and iOS alike, prints "flags=0<>".
                     * Passing that zero through SIGABRTs systemd-networkd on
                     * sight: link_update_flags() early-returns when the incoming
                     * flags and operstate both equal what the freshly-zeroed
                     * Link already holds, so link_update_operstate() never runs,
                     * link->carrier_state stays at LINK_OPERSTATE_MISSING (a
                     * NULL hole in link_carrier_state_table[]), and link_save()'s
                     * assert(carrier_state) fires. Restart= then crash-loops it
                     * ("Failed to start Network Configuration", forever). */
                    if (ifi->ifi_flags == 0)
                        links_zero_flags++;
                }
            }
        }
    }
    alarm(0);

    test_log_if(test_verbose, "  %s: batches=%d entries=%d done=%d err=%d\n",
                mn, batches, saw_new, saw_done, saw_err);
    snprintf(lab, sizeof lab, "%s.reply_terminated", mn);
    check(lab, saw_done, 1);
    snprintf(lab, sizeof lab, "%s.done_has_multi", mn);
    check(lab, saw_done ? done_multi : 1, 1);
    snprintf(lab, sizeof lab, "%s.parts_have_multi", mn);
    check(lab, parts_not_multi, 0);
    snprintf(lab, sizeof lab, "%s.no_nlmsg_error", mn);
    check(lab, saw_err, 0);
    if (expect_new == RTM_NEWADDR_) {
        snprintf(lab, sizeof lab, "%s.addrs_have_ifa_flags", mn);
        check(lab, addrs_missing_ifa_flags, 0);
    }
    if (expect_new == RTM_NEWLINK_) {
        snprintf(lab, sizeof lab, "%s.running_implies_lower_up", mn);
        check(lab, links_running_not_lower_up, 0);
        snprintf(lab, sizeof lab, "%s.no_zero_flag_links", mn);
        check(lab, links_zero_flags, 0);
    }
    /* Every system has a loopback link, so a link dump must yield >= 1 entry. */
    if (expect_new == RTM_NEWLINK_) {
        snprintf(lab, sizeof lab, "%s.have_links", mn);
        check(lab, saw_new >= 1, 1);
    }
    close(fd);
}

/* BusyBox `ip addr` always probes SIOCGIFTXQLEN per interface (it does not read
 * the IFLA_TXQLEN netlink attribute), so an unhandled ioctl printed
 * "ip: ioctl 0x8942 failed: Not a tty" (ENOTTY) on every line. It must succeed
 * and return a tx queue length, not fall through to the realfs passthrough. */
static void test_ifr_txqlen(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    check("txqlen.socket", fd >= 0, 1);
    if (fd < 0)
        return;

    /* Name loopback (ifindex 1) via SIOCGIFNAME, then query its tx queue len. */
    struct nl_ifreq req;
    memset(&req, 0, sizeof req);
    req.u.ival = 1;
    errno = 0;
    int gn = ioctl(fd, SIOCGIFNAME_, &req);
    test_log_if(gn != 0, "  txqlen: SIOCGIFNAME(1) errno=%d (%s)\n", errno, strerror(errno));
    check("txqlen.getname", gn == 0, 1);
    if (gn != 0) { close(fd); return; }

    errno = 0;
    int rc = ioctl(fd, SIOCGIFTXQLEN_, &req);
    test_log_if(rc != 0, "  txqlen: SIOCGIFTXQLEN(%s) errno=%d (%s)\n",
                req.ifr_name, errno, strerror(errno));
    check("txqlen.ioctl_ok", rc == 0, 1);
    check("txqlen.not_enotty", !(rc != 0 && errno == ENOTTY), 1);
    if (rc == 0)
        check("txqlen.value_positive", req.u.ival > 0, 1);
    close(fd);
}

/* Count RTM_NEW* replies to one dump request made with an explicit family.
 * Returns the count, or -1 if the reply stream carried an NLMSG_ERROR. */
static int count_dump_family(int req_type, int expect_new, uint8_t family) {
    int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (fd < 0)
        return -1;
    if (send_dump_family(fd, IO_SEND, req_type, 0x5150 + family, family) <= 0) {
        close(fd);
        return -1;
    }

    char buf[64 * 1024];
    int seen = 0, done = 0, err = 0;
    arm_alarm(10);
    while (!done && !alarm_fired) {
        ssize_t n = recv_batch(fd, IO_SEND, buf, sizeof buf);
        if (n <= 0)
            break;
        struct nl_hdr *nlh = (struct nl_hdr *) buf;
        size_t len = (size_t) n;
        for (; NL_OK(nlh, len); nlh = NL_NEXT(nlh, len)) {
            if (nlh->nlmsg_type == NLMSG_DONE_) { done = 1; break; }
            if (nlh->nlmsg_type == NLMSG_ERROR_) { err = 1; done = 1; break; }
            if (nlh->nlmsg_type == expect_new)
                seen++;
        }
    }
    alarm(0);
    close(fd);
    return err ? -1 : seen;
}

/* An RTM_GETLINK dump must return the SAME link list no matter which family
 * the request names. Linux registers the link dump under PF_UNSPEC and every
 * other family falls through to it; only PF_BRIDGE has its own handler
 * (rtnl_bridge_getlink), which emits bridge ports only. Verified against real
 * Linux (iproute2 6.15.0, kernel 6.12): families 0/2/10/16/17 all return the
 * full list, family 7 returns none.
 *
 * iSH-AOK dropped every link for any family except AF_UNSPEC/AF_PACKET, which
 * is invisible to plain `ip addr` (it sends AF_UNSPEC) but makes `ip -4 addr`
 * and `ip -6 addr` print NOTHING: iproute2 dumps links first to build its
 * ifindex->name map, and an empty map means every address it then receives has
 * no interface to attach to and is silently dropped. The address dump itself
 * was always correct, so the addresses really were arriving and being thrown
 * away, which is why the failure looked like "no addresses" rather than an
 * error. */
static void test_link_dump_family_filter(void) {
    static const struct { uint8_t fam; const char *name; } families[] = {
        { 2,  "inet" }, { 10, "inet6" }, { 16, "netlink" }, { 17, "packet" },
    };

    int base = count_dump_family(RTM_GETLINK_, RTM_NEWLINK_, 0 /* AF_UNSPEC */);
    check("famlink.unspec_have_links", base >= 1, 1);
    if (base < 1)
        return;

    for (size_t i = 0; i < sizeof families / sizeof *families; i++) {
        char lab[64];
        int n = count_dump_family(RTM_GETLINK_, RTM_NEWLINK_, families[i].fam);
        test_log_if(n != base, "  famlink: AF_%s dump returned %d links, AF_UNSPEC returned %d\n",
                    families[i].name, n, base);
        snprintf(lab, sizeof lab, "famlink.%s_matches_unspec", families[i].name);
        check(lab, n, base);
    }

    /* AF_BRIDGE is the one family Linux answers differently: rtnl_bridge_getlink
     * emits bridge ports only. iSH-AOK has no bridge devices, so the reply must
     * be empty. On a real kernel that is only true when the host has no bridge
     * (a docker host has docker0), so assert the exact count under AOK and
     * merely a subset elsewhere -- otherwise this suite fails on any Linux box
     * that happens to run containers. */
    int bridge = count_dump_family(RTM_GETLINK_, RTM_NEWLINK_, 7);
    struct utsname uts;
    int under_ish = uname(&uts) == 0 && strstr(uts.release, "ish") != NULL;
    if (under_ish)
        check("famlink.bridge_empty", bridge, 0);
    else
        check("famlink.bridge_subset", bridge >= 0 && bridge <= base, 1);

    /* The address dump, unlike the link dump, IS filtered by family on Linux
     * (separate per-family handlers). Guard that the fix above did not turn the
     * address filter off too: an AF_INET address dump must not exceed the
     * unfiltered one, and both must be non-empty (every host has 127.0.0.1). */
    int addr_all = count_dump_family(RTM_GETADDR_, RTM_NEWADDR_, 0);
    int addr_v4 = count_dump_family(RTM_GETADDR_, RTM_NEWADDR_, 2);
    check("famaddr.unspec_have_addrs", addr_all >= 1, 1);
    check("famaddr.inet_have_addrs", addr_v4 >= 1, 1);
    check("famaddr.inet_subset_of_all", addr_v4 <= addr_all, 1);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    signal(SIGPIPE, SIG_IGN);

    /* The path the bug was reported on (`ip addr` -> RTM_GETADDR via write). */
    run_dump(IO_RW, RTM_GETADDR_, RTM_NEWADDR_);
    run_dump(IO_RW, RTM_GETLINK_, RTM_NEWLINK_);
    /* readv/writev collapse to the same sock_read/sock_write path. */
    run_dump(IO_VEC, RTM_GETLINK_, RTM_NEWLINK_);
    /* send/recv was already wired; assert parity as a guard. */
    run_dump(IO_SEND, RTM_GETLINK_, RTM_NEWLINK_);

    /* A family-qualified link dump must match the unqualified one (`ip -4 addr`). */
    test_link_dump_family_filter();

    /* The SIOCGIFTXQLEN ioctl `ip addr` issues per interface. */
    test_ifr_txqlen();

    return finish_suite("netlink_route");
}
