/*
 * netlink_route.c — self-checking regression lock for AF_NETLINK rtnetlink
 * dumps issued through the bare read()/write() (and readv()/writev()) path.
 *
 * AOK-iSH emulates NETLINK_ROUTE sockets entirely in-process (real_fd < 0).
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
 * What waybar's network module reads to find the interface it shows (each of
 * the first two, missing, left it at "Disconnected" on a working network):
 *   - the default route carries the router as RTA_GATEWAY. waybar takes the
 *     default route to be the one with a gateway; AOK reported "default dev en0
 *     scope link" with none, while /proc/net/route flagged the same route G
 *     beside a zero gateway, which Linux never does
 *   - every link carries IFLA_CARRIER. waybar starts the link it found at
 *     carrier=false and only that attribute raises it
 *   - RTM_GETLINK without NLM_F_DUMP answers with the one link it names (no
 *     NLM_F_MULTI, no NLMSG_DONE), then the ACK; ENODEV for no such link and
 *     EINVAL for a request naming none. AOK answered with the whole dump, so
 *     `ip link show dev en0` printed lo0, and so did a name that did not exist
 *   - every route carries RTA_TABLE
 * Checked against Linux 6.12 (Devuan 6, x86_64): all of these hold there.
 *
 * Exits 0 and prints "netlink_route: PASS" on success.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <poll.h>
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

/* From <net/if.h>, which cannot be included here: both glibc and musl define
 * ifr_name there as a macro, and struct nl_ifreq below has a field by that
 * name. */
extern unsigned int if_nametoindex(const char *ifname);

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
#define RTM_NEWROUTE_ 24
#define RTM_GETROUTE_ 26
#define NLM_F_ACK_ 0x004
#define IFA_FLAGS_ 8
#define IFLA_IFNAME_ 3
#define IFLA_CARRIER_ 33
#define RTA_OIF_ 4
#define RTA_GATEWAY_ 5
#define RTA_MULTIPATH_ 9
#define RTA_TABLE_ 15
#define RT_TABLE_MAIN_ 254
#define RT_SCOPE_LINK_ 253
#define RTF_GATEWAY_ 0x2
#define IFF_UP_       0x1
#define IFF_RUNNING_  0x40
#define IFF_LOWER_UP_ 0x10000

struct nl_rtmsg {
    unsigned char rtm_family;
    unsigned char rtm_dst_len;
    unsigned char rtm_src_len;
    unsigned char rtm_tos;
    unsigned char rtm_table;
    unsigned char rtm_protocol;
    unsigned char rtm_scope;
    unsigned char rtm_type;
    uint32_t rtm_flags;
};

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
 * AOK-iSH dropped every link for any family except AF_UNSPEC/AF_PACKET, which
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
     * emits bridge ports only. AOK-iSH has no bridge devices, so the reply must
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

/* Attribute `type` of a message whose fixed header is `fixed` bytes, or NULL. */
static struct nl_rtattr *msg_attr(struct nl_hdr *nlh, size_t fixed, uint16_t type) {
    int len = (int) nlh->nlmsg_len - NL_HDRLEN - (int) NL_ALIGN(fixed);
    struct nl_rtattr *rta = (struct nl_rtattr *) ((char *) NL_DATA(nlh) + NL_ALIGN(fixed));
    while (len >= (int) sizeof(*rta) && rta->rta_len >= sizeof(*rta) &&
            (int) rta->rta_len <= len) {
        if ((rta->rta_type & 0x3fff) == type)
            return rta;
        len -= NL_ALIGN(rta->rta_len);
        rta = (struct nl_rtattr *) ((char *) rta + NL_ALIGN(rta->rta_len));
    }
    return NULL;
}
#define ATTR_DATA(rta) ((void *) ((char *) (rta) + NL_ALIGN(sizeof(struct nl_rtattr))))
#define ATTR_LEN(rta)  ((int) (rta)->rta_len - (int) sizeof(struct nl_rtattr))

static uint32_t attr_u32(struct nl_rtattr *rta) {
    uint32_t v = 0;
    if (ATTR_LEN(rta) >= (int) sizeof v)
        memcpy(&v, ATTR_DATA(rta), sizeof v);
    return v;
}

struct link_rec {
    int index;
    char name[16];
    uint32_t flags;
    int carrier; /* -1: no IFLA_CARRIER */
};

#define MAX_LINKS 128

/* Every link one RTM_GETLINK dump reports. */
static int collect_links(struct link_rec *links, int cap) {
    int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (fd < 0)
        return -1;
    if (send_dump_family(fd, IO_SEND, RTM_GETLINK_, 0x6000, 0) <= 0) {
        close(fd);
        return -1;
    }
    char buf[64 * 1024];
    int count = 0, done = 0;
    arm_alarm(10);
    while (!done && !alarm_fired) {
        ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n <= 0)
            break;
        struct nl_hdr *nlh = (struct nl_hdr *) buf;
        int len = (int) n;
        for (; NL_OK(nlh, len); nlh = NL_NEXT(nlh, len)) {
            if (nlh->nlmsg_type == NLMSG_DONE_ || nlh->nlmsg_type == NLMSG_ERROR_) {
                done = 1;
                break;
            }
            if (nlh->nlmsg_type != RTM_NEWLINK_ || count >= cap)
                continue;
            struct nl_ifinfomsg *ifi = NL_DATA(nlh);
            struct link_rec *l = &links[count++];
            memset(l, 0, sizeof *l);
            l->index = ifi->ifi_index;
            l->flags = ifi->ifi_flags;
            struct nl_rtattr *name = msg_attr(nlh, sizeof *ifi, IFLA_IFNAME_);
            if (name != NULL)
                snprintf(l->name, sizeof l->name, "%.*s", ATTR_LEN(name), (char *) ATTR_DATA(name));
            struct nl_rtattr *carrier = msg_attr(nlh, sizeof *ifi, IFLA_CARRIER_);
            l->carrier = carrier != NULL && ATTR_LEN(carrier) >= 1
                ? *(unsigned char *) ATTR_DATA(carrier) : -1;
        }
    }
    alarm(0);
    close(fd);
    return count;
}

/* Linux puts IFLA_CARRIER in every link message, and for a link that is up it
 * agrees with IFF_LOWER_UP: dev_get_flags() sets LOWER_UP from the carrier
 * while the device runs. A link that is down may report either. waybar's
 * network module starts the link it found at carrier=false, and only this
 * attribute raises it, so without it a working interface was "Disconnected". */
static void test_link_carrier(void) {
    struct link_rec links[MAX_LINKS];
    int n = collect_links(links, MAX_LINKS);
    check("carrier.have_links", n >= 1, 1);
    int missing = 0, disagree = 0;
    for (int i = 0; i < n; i++) {
        if (links[i].carrier < 0) {
            test_log_if(1, "  carrier: link %d (%s) has no IFLA_CARRIER\n",
                        links[i].index, links[i].name);
            missing++;
        } else if ((links[i].flags & IFF_UP_) &&
                links[i].carrier != !!(links[i].flags & IFF_LOWER_UP_)) {
            test_log_if(1, "  carrier: link %d (%s) is up, carrier=%d, flags=%#x\n",
                        links[i].index, links[i].name, links[i].carrier, links[i].flags);
            disagree++;
        }
    }
    check("carrier.every_link_has_it", missing, 0);
    check("carrier.agrees_with_lower_up", disagree, 0);
}

struct getlink_reply {
    int newlinks;  /* RTM_NEWLINK messages */
    int multi;     /* ...of them carrying NLM_F_MULTI */
    int index;     /* ifi_index of the last */
    char name[16]; /* IFLA_IFNAME of the last */
    int acks;      /* NLMSG_ERROR with error 0 */
    int error;     /* the last nonzero NLMSG_ERROR */
    int dones;     /* NLMSG_DONE */
    int other;     /* anything else */
    int wrong_seq; /* replies not carrying the request's nlmsg_seq */
};

/* One RTM_GETLINK request without NLM_F_DUMP, naming its link by index or, when
 * `name` is not NULL, by IFLA_IFNAME, and everything that comes back. The
 * kernel queues the whole answer before sendmsg returns, so once the first
 * reply is readable the rest is drained without waiting. Returns 1 if anything
 * came back, 0 if nothing did, -1 if the request could not be sent. */
static int getlink_one(int fd, uint32_t seq, int index, const char *name, int ack,
                       struct getlink_reply *r) {
    char req[128];
    memset(req, 0, sizeof req);
    memset(r, 0, sizeof *r);
    struct nl_hdr *nlh = (struct nl_hdr *) req;
    size_t len = NL_HDRLEN + NL_ALIGN(sizeof(struct nl_ifinfomsg));
    ((struct nl_ifinfomsg *) NL_DATA(nlh))->ifi_index = index;
    if (name != NULL) {
        struct nl_rtattr *rta = (struct nl_rtattr *) (req + len);
        size_t name_len = strlen(name) + 1;
        rta->rta_type = IFLA_IFNAME_;
        rta->rta_len = (uint16_t) (sizeof *rta + name_len);
        memcpy(rta + 1, name, name_len);
        len += NL_ALIGN(rta->rta_len);
    }
    nlh->nlmsg_len = (uint32_t) len;
    nlh->nlmsg_type = RTM_GETLINK_;
    nlh->nlmsg_flags = NLM_F_REQUEST_ | (ack ? NLM_F_ACK_ : 0);
    nlh->nlmsg_seq = seq;
    struct nl_sockaddr dst = { .nl_family = AF_NETLINK };
    if (sendto(fd, req, len, 0, (struct sockaddr *) &dst, sizeof dst) != (ssize_t) len)
        return -1;

    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    if (poll(&pfd, 1, 2000) != 1)
        return 0;
    char buf[16 * 1024];
    for (;;) {
        ssize_t n = recv(fd, buf, sizeof buf, MSG_DONTWAIT);
        if (n <= 0)
            break;
        struct nl_hdr *h = (struct nl_hdr *) buf;
        int left = (int) n;
        for (; NL_OK(h, left); h = NL_NEXT(h, left)) {
            if (h->nlmsg_seq != seq)
                r->wrong_seq++;
            if (h->nlmsg_type == RTM_NEWLINK_) {
                struct nl_ifinfomsg *ifi = NL_DATA(h);
                r->newlinks++;
                if (h->nlmsg_flags & NLM_F_MULTI_)
                    r->multi++;
                r->index = ifi->ifi_index;
                struct nl_rtattr *nm = msg_attr(h, sizeof *ifi, IFLA_IFNAME_);
                if (nm != NULL)
                    snprintf(r->name, sizeof r->name, "%.*s", ATTR_LEN(nm), (char *) ATTR_DATA(nm));
            } else if (h->nlmsg_type == NLMSG_ERROR_) {
                struct nl_err *e = NL_DATA(h);
                if (e->error == 0)
                    r->acks++;
                else
                    r->error = e->error;
            } else if (h->nlmsg_type == NLMSG_DONE_) {
                r->dones++;
            } else {
                r->other++;
            }
        }
    }
    return 1;
}

static void log_getlink(const char *what, const struct getlink_reply *r) {
    test_log_if(1, "  getlink %s: newlinks=%d multi=%d index=%d name=%s acks=%d error=%d "
                "dones=%d other=%d wrong_seq=%d\n", what, r->newlinks, r->multi, r->index,
                r->name, r->acks, r->error, r->dones, r->other, r->wrong_seq);
}

/* RTM_GETLINK without NLM_F_DUMP (rtnl_getlink): the one link the request names,
 * without NLM_F_MULTI or NLMSG_DONE, then the ACK when one was asked for. No such
 * link is ENODEV; naming none is EINVAL. waybar asks this way for the link its
 * default route leaves by, and `ip link show dev X` does too. AOK answered with
 * the whole dump, and iproute2 printed its first link, lo0, for any name. */
static void test_link_get(void) {
    struct link_rec links[MAX_LINKS];
    int n = collect_links(links, MAX_LINKS);
    check("getlink.have_links", n >= 1, 1);
    if (n < 1)
        return;
    int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    check("getlink.socket", fd >= 0, 1);
    if (fd < 0)
        return;

    struct getlink_reply r;
    int wrong = 0;
    for (int i = 0; i < n; i++) {
        int got = getlink_one(fd, 0x7000 + (uint32_t) i, links[i].index, NULL, 1, &r);
        if (got != 1 || r.newlinks != 1 || r.multi != 0 || r.index != links[i].index ||
                strcmp(r.name, links[i].name) != 0 || r.acks != 1 || r.error != 0 ||
                r.dones != 0 || r.other != 0 || r.wrong_seq != 0) {
            char what[48];
            snprintf(what, sizeof what, "index %d (%s) got=%d", links[i].index, links[i].name, got);
            log_getlink(what, &r);
            wrong++;
        }
    }
    check("getlink.by_index_that_link_then_ack", wrong, 0);

    int got = getlink_one(fd, 0x7100, 0, links[0].name, 1, &r);
    int ok = got == 1 && r.newlinks == 1 && r.multi == 0 && r.index == links[0].index &&
        r.acks == 1 && r.error == 0 && r.dones == 0 && r.other == 0;
    if (!ok)
        log_getlink("by name", &r);
    check("getlink.by_name_that_link_then_ack", ok, 1);

    got = getlink_one(fd, 0x7101, links[0].index, NULL, 0, &r);
    ok = got == 1 && r.newlinks == 1 && r.multi == 0 && r.acks == 0 && r.error == 0 &&
        r.dones == 0 && r.other == 0;
    if (!ok)
        log_getlink("without NLM_F_ACK", &r);
    check("getlink.no_ack_link_alone", ok, 1);

    got = getlink_one(fd, 0x7102, 2000000000, NULL, 1, &r);
    if (r.error != -ENODEV || r.newlinks != 0)
        log_getlink("missing index", &r);
    check("getlink.missing_index_enodev", r.error, -ENODEV);
    check("getlink.missing_index_no_link", r.newlinks, 0);

    got = getlink_one(fd, 0x7103, 0, "nosuchlink9", 1, &r);
    if (r.error != -ENODEV || r.newlinks != 0)
        log_getlink("missing name", &r);
    check("getlink.missing_name_enodev", r.error, -ENODEV);

    got = getlink_one(fd, 0x7104, 0, NULL, 1, &r);
    if (r.error != -EINVAL || r.newlinks != 0)
        log_getlink("no index or name", &r);
    check("getlink.nothing_named_einval", r.error, -EINVAL);
    close(fd);
}

/* /proc/net/route and the rtnetlink route dump describe the same table. On
 * Linux a route flagged G (RTF_GATEWAY) names a nonzero gateway; a default
 * route through a router is in the main table's dump with the router as
 * RTA_GATEWAY and its interface as RTA_OIF; a route through a router is never
 * link scope (fib_check_nh refuses one); and every route carries RTA_TABLE,
 * agreeing with rtm_table when the id fits in it.
 *
 * AOK flagged its default route G beside a zero gateway and reported it over
 * rtnetlink as "default dev en0 scope link", with no RTA_GATEWAY and no
 * RTA_TABLE. waybar's network module takes the default route to be the one
 * with a gateway, found none, and showed "Disconnected" on a working network. */
static void test_route_gateway(void) {
    FILE *f = fopen("/proc/net/route", "r");
    check("route.proc_open", f != NULL, 1);
    if (f == NULL)
        return;
    struct { char ifname[16]; uint32_t gateway; } proc_defaults[16];
    int n_proc = 0, flagged_without_gateway = 0;
    char line[256];
    while (fgets(line, sizeof line, f) != NULL) {
        char ifname[16];
        unsigned dest, gateway, flags, mask;
        int refcnt, use, metric;
        if (sscanf(line, "%15s %x %x %x %d %d %d %x", ifname, &dest, &gateway, &flags,
                   &refcnt, &use, &metric, &mask) != 8)
            continue;
        if ((flags & RTF_GATEWAY_) && gateway == 0) {
            test_log_if(1, "  route: /proc/net/route %s %08X is flagged G with gateway 0\n",
                        ifname, dest);
            flagged_without_gateway++;
        }
        if (dest == 0 && mask == 0 && (flags & RTF_GATEWAY_) && gateway != 0 && n_proc < 16) {
            snprintf(proc_defaults[n_proc].ifname, sizeof proc_defaults[n_proc].ifname, "%s", ifname);
            proc_defaults[n_proc].gateway = gateway;
            n_proc++;
        }
    }
    fclose(f);
    check("route.proc_g_flag_has_gateway", flagged_without_gateway, 0);

    int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    check("route.socket", fd >= 0, 1);
    if (fd < 0)
        return;
    check("route.dump_sent", send_dump_family(fd, IO_SEND, RTM_GETROUTE_, 0x7200, AF_INET) > 0, 1);
    struct { uint32_t gateway; uint32_t oif; } nl_defaults[32];
    int n_nl = 0, routes = 0, no_table = 0, table_mismatch = 0, gateway_link_scope = 0;
    int multipath_default = 0, done = 0;
    char buf[64 * 1024];
    arm_alarm(10);
    while (!done && !alarm_fired) {
        ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n <= 0)
            break;
        struct nl_hdr *h = (struct nl_hdr *) buf;
        int left = (int) n;
        for (; NL_OK(h, left); h = NL_NEXT(h, left)) {
            if (h->nlmsg_type == NLMSG_DONE_ || h->nlmsg_type == NLMSG_ERROR_) {
                done = 1;
                break;
            }
            if (h->nlmsg_type != RTM_NEWROUTE_)
                continue;
            routes++;
            struct nl_rtmsg *rtm = NL_DATA(h);
            unsigned table_id = rtm->rtm_table;
            struct nl_rtattr *table = msg_attr(h, sizeof *rtm, RTA_TABLE_);
            if (table == NULL) {
                test_log_if(no_table == 0, "  route: a route (dst_len %u, table %u) has no RTA_TABLE\n",
                            rtm->rtm_dst_len, rtm->rtm_table);
                no_table++;
            } else {
                table_id = attr_u32(table);
                if (table_id < 256 && table_id != rtm->rtm_table)
                    table_mismatch++;
            }
            struct nl_rtattr *gateway = msg_attr(h, sizeof *rtm, RTA_GATEWAY_);
            if (gateway != NULL && rtm->rtm_scope >= RT_SCOPE_LINK_) {
                test_log_if(1, "  route: a route through a gateway has scope %u\n", rtm->rtm_scope);
                gateway_link_scope++;
            }
            if (table_id == RT_TABLE_MAIN_ && rtm->rtm_dst_len == 0) {
                if (msg_attr(h, sizeof *rtm, RTA_MULTIPATH_) != NULL)
                    multipath_default++;
                struct nl_rtattr *oif = msg_attr(h, sizeof *rtm, RTA_OIF_);
                if (gateway != NULL && ATTR_LEN(gateway) == 4 && oif != NULL && n_nl < 32) {
                    nl_defaults[n_nl].gateway = attr_u32(gateway);
                    nl_defaults[n_nl].oif = attr_u32(oif);
                    n_nl++;
                }
            }
        }
    }
    alarm(0);
    close(fd);
    check("route.dump_terminated", done, 1);
    check("route.dump_has_routes", routes >= 1, 1);
    check("route.every_route_has_rta_table", no_table, 0);
    check("route.rta_table_matches_rtm_table", table_mismatch, 0);
    check("route.gateway_route_not_link_scope", gateway_link_scope, 0);

    /* A multipath default names its routers in RTA_MULTIPATH, not RTA_GATEWAY. */
    int unmatched = 0;
    for (int i = 0; i < n_proc && multipath_default == 0; i++) {
        uint32_t ifindex = if_nametoindex(proc_defaults[i].ifname);
        int found = 0;
        for (int j = 0; j < n_nl; j++)
            if (nl_defaults[j].gateway == proc_defaults[i].gateway && nl_defaults[j].oif == ifindex)
                found = 1;
        if (!found) {
            test_log_if(1, "  route: /proc/net/route default via %08X dev %s (index %u) is not "
                        "an rtnetlink default route with that RTA_GATEWAY and RTA_OIF\n",
                        proc_defaults[i].gateway, proc_defaults[i].ifname, ifindex);
            unmatched++;
        }
    }
    check("route.proc_default_gateway_in_rtnetlink", unmatched, 0);
    test_log_if(test_verbose, "  route: %d routes, %d /proc default gateways, %d rtnetlink\n",
                routes, n_proc, n_nl);
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

    /* What waybar's network module needs to find the interface it shows. */
    test_link_carrier();
    test_link_get();
    test_route_gateway();

    return finish_suite("netlink_route");
}
