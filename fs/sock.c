#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#if defined(__APPLE__)
#include <net/if_dl.h>
#endif
#include <netinet/icmp6.h>
#include <netinet/ip6.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include "kernel/calls.h"
#include "kernel/inotify.h"
#include "kernel/task.h"
#include "fs/fd.h"
#include "fs/inode.h"
#include "fs/net_route.h"
#include "fs/path.h"
#include "fs/poll.h"
#include "fs/real.h"
#include "fs/sock.h"
#include "util/timer.h"
#include "debug.h"

#define SOCKET_TYPE_MASK 0xf

#define NLMSG_ALIGNTO 4
#define NLMSG_ALIGN(len) (((len) + NLMSG_ALIGNTO - 1) & ~(NLMSG_ALIGNTO - 1))
#define NLMSG_HDRLEN ((int) NLMSG_ALIGN(sizeof(struct nlmsghdr_)))
#define NLA_ALIGNTO 4
#define NLA_ALIGN(len) (((len) + NLA_ALIGNTO - 1) & ~(NLA_ALIGNTO - 1))

#define NLMSG_NOOP_ 1
#define NLMSG_ERROR_ 2
#define NLMSG_DONE_ 3

#define NLM_F_REQUEST_ 0x1
#define NLM_F_MULTI_ 0x2
#define NLM_F_ACK_ 0x4
#define NLM_F_ROOT_ 0x100
#define NLM_F_MATCH_ 0x200
#define NLM_F_DUMP_ (NLM_F_ROOT_ | NLM_F_MATCH_)

// NETLINK_AUDIT message types and status flags (linux/audit.h)
#define NLMSG_MIN_TYPE_ 0x10
#define AUDIT_GET_ 1000
#define AUDIT_SET_ 1001
#define AUDIT_LIST_ 1002 // removed legacy binary rule API; kernel: EOPNOTSUPP
#define AUDIT_ADD_ 1003
#define AUDIT_DEL_ 1004
#define AUDIT_USER_ 1005
#define AUDIT_ADD_RULE_ 1011
#define AUDIT_DEL_RULE_ 1012
#define AUDIT_LIST_RULES_ 1013
#define AUDIT_TRIM_ 1014
#define AUDIT_MAKE_EQUIV_ 1015
#define AUDIT_TTY_GET_ 1016
#define AUDIT_TTY_SET_ 1017
#define AUDIT_SET_FEATURE_ 1018
#define AUDIT_GET_FEATURE_ 1019
#define AUDIT_SIGNAL_INFO_ 1010
#define AUDIT_FIRST_USER_MSG_ 1100
#define AUDIT_LAST_USER_MSG_ 1199
#define AUDIT_FIRST_USER_MSG2_ 2100
#define AUDIT_LAST_USER_MSG2_ 2999

#define AUDIT_STATUS_ENABLED_ 0x0001
#define AUDIT_STATUS_FAILURE_ 0x0002
#define AUDIT_STATUS_PID_ 0x0004
#define AUDIT_STATUS_RATE_LIMIT_ 0x0008
#define AUDIT_STATUS_BACKLOG_LIMIT_ 0x0010

// struct audit_status, current (v11-field) layout; older libaudits copy a
// prefix, newer ones the whole thing, both driven by the reply length.
struct audit_status_ {
    uint32_t mask;
    uint32_t enabled;
    uint32_t failure;
    uint32_t pid;
    uint32_t rate_limit;
    uint32_t backlog_limit;
    uint32_t lost;
    uint32_t backlog;
    uint32_t feature_bitmap;
    uint32_t backlog_wait_time;
    uint32_t backlog_wait_time_actual;
};

struct audit_features_ {
    uint32_t vers;
    uint32_t mask;
    uint32_t features;
    uint32_t lock;
};

#define SOCK_DIAG_BY_FAMILY_ 20

#define SIOCGIFNAME_ 0x8910
#define SIOCGIFCONF_ 0x8912
#define SIOCGIFFLAGS_ 0x8913
#define SIOCGIFADDR_ 0x8915
#define SIOCGIFDSTADDR_ 0x8917
#define SIOCGIFBRDADDR_ 0x8919
#define SIOCGIFNETMASK_ 0x891b
#define SIOCGIFMETRIC_ 0x891d
#define SIOCGIFMTU_ 0x8921
#define SIOCGIFINDEX_ 0x8933
#define SIOCGIFTXQLEN_ 0x8942
#define SIOCGIFHWADDR_ 0x8927

#define IFNAMSIZ_ 16

#define RTM_NEWLINK_ 16
#define RTM_DELLINK_ 17
#define RTM_GETLINK_ 18
#define RTM_NEWADDR_ 20
#define RTM_DELADDR_ 21
#define RTM_GETADDR_ 22
#define RTM_NEWROUTE_ 24
#define RTM_GETROUTE_ 26

#define RTNLGRP_LINK_ 1
#define RTNLGRP_IPV4_IFADDR_ 5
#define RTNLGRP_IPV6_IFADDR_ 9

#define IFLA_ADDRESS_ 1
#define IFLA_BROADCAST_ 2
#define IFLA_IFNAME_ 3
#define IFLA_MTU_ 4
#define IFLA_TXQLEN_ 13
#define IFLA_OPERSTATE_ 16

#define IFA_ADDRESS_ 1
#define IFA_LOCAL_ 2
#define IFA_LABEL_ 3
#define IFA_BROADCAST_ 4
#define IFA_FLAGS_ 8
#define IFA_F_PERMANENT_ 0x80

#define RTA_DST_ 1
#define RTA_OIF_ 4
#define RTA_GATEWAY_ 5
#define RTA_PREFSRC_ 7

#define RT_SCOPE_UNIVERSE_ 0
#define RT_SCOPE_LINK_ 253
#define RT_SCOPE_HOST_ 254

#define RT_TABLE_MAIN_ 254
#define RTPROT_KERNEL_ 2
#define RTPROT_BOOT_ 3
#define RTN_UNICAST_ 1

#define ARPHRD_ETHER_ 1
#define ARPHRD_LOOPBACK_ 772

#define IF_OPER_UNKNOWN_ 0
#define IF_OPER_UP_ 6

#define IFF_UP_LINUX_ 0x1
#define IFF_BROADCAST_LINUX_ 0x2
#define IFF_LOOPBACK_LINUX_ 0x8
#define IFF_POINTOPOINT_LINUX_ 0x10
#define IFF_RUNNING_LINUX_ 0x40
#define IFF_NOARP_LINUX_ 0x80
#define IFF_PROMISC_LINUX_ 0x100
#define IFF_MULTICAST_LINUX_ 0x1000
#define IFF_LOWER_UP_LINUX_ 0x10000

#define TCPF_ALL_ 0xFFF
#define UDIAG_SHOW_NAME_ (1 << 0)

#if defined(__APPLE__)
#define DARWIN_TCPS_ESTABLISHED 4
#endif

struct sockaddr_nl_ {
    uint16_t nl_family;
    uint16_t nl_pad;
    uint32_t nl_pid;
    uint32_t nl_groups;
};

/* glibc <net/if.h> defines ifr_name as a macro (ifr_ifrn.ifrn_name) that would
   mangle the field name in iSH's guest-ABI struct below; neutralize it. */
#undef ifr_name
struct ifreq_ {
    char ifr_name[IFNAMSIZ_];
    union {
        struct sockaddr_ addr;
        int16_t flags;
        int32_t ifindex;
        int32_t qlen;
        int32_t mtu;
        int32_t metric;
        char pad[24];
    } ifr_ifru;
};

/* Real Linux sizeof(struct ifreq): 32 on a 32-bit guest (i386), 40 on a
   64-bit guest (amd64/arm64/riscv64) -- struct ifmap's "unsigned long"
   fields double in size, growing the ifr_ifru union. All the fields iSH
   actually reads/writes (ifr_name, ifr_flags, ifr_ifindex, ifr_qlen) sit
   within the first 20 bytes, so struct ifreq_ above (which mirrors the
   64-bit layout) is reused for both; only the copy size to/from guest
   memory needs to vary. Copying the 64-bit size for a 32-bit guest would
   write 8 bytes past its ifreq buffer into whatever guest memory follows. */
#define IFREQ_SIZE_32_ 32
#define IFREQ_SIZE_64_ (sizeof(struct ifreq_))

struct sockaddr_in_ {
    uint16_t sin_family;
    uint16_t sin_port;
    uint32_t sin_addr;
    uint8_t sin_zero[8];
};

struct sockaddr_in6_ {
    uint16_t sin6_family;
    uint16_t sin6_port;
    uint32_t sin6_flowinfo;
    struct in6_addr sin6_addr;
    uint32_t sin6_scope_id;
};

/* struct ifreq's union is sized to fit struct ifmap, whose "unsigned long"
   fields are 8 bytes on a 64-bit guest vs 4 on a 32-bit guest -- so
   sizeof(struct ifreq) is 40 on amd64/arm64/riscv64 but only 32 on i386.
   SIOCGIFCONF's ifc_buf is an array of these, so the entry stride must
   match the guest's real ifreq size or busybox/net-tools reads every
   entry past the first at the wrong offset (and miscomputes the device
   count from ifc_len / sizeof(struct ifreq)). */
struct guest_ifreq_addr32_ {
    char ifr_name[IFNAMSIZ_];
    struct sockaddr_in_ guest_addr;
};

struct guest_ifreq_addr64_ {
    char ifr_name[IFNAMSIZ_];
    struct sockaddr_in_ guest_addr;
    uint8_t __pad[8];
};

/* struct ifconf's layout depends on the guest pointer size: on a 32-bit
   guest (i386) it's a packed 8 bytes (len + 4-byte ptr), but on a 64-bit
   guest (amd64/arm64/riscv64) the ifc_buf pointer is 8-byte aligned, so
   there are 4 bytes of padding between ifc_len and ifc_buf, making the
   struct 16 bytes. Using the wrong layout for a 64-bit guest reads the
   padding as the buffer pointer (always 0), silently dropping every
   ifreq entry and leaving busybox/net-tools ifconfig's buffer zeroed --
   producing an empty ifr_name and "Device not found". */
struct guest_ifconf32_ {
    int32_t guest_len;
    uint32_t guest_buf;
};

struct guest_ifconf64_ {
    int32_t guest_len;
    int32_t __pad;
    guest_addr_t guest_buf;
};

struct nlmsghdr_ {
    uint32_t nlmsg_len;
    uint16_t nlmsg_type;
    uint16_t nlmsg_flags;
    uint32_t nlmsg_seq;
    uint32_t nlmsg_pid;
};

struct nlmsgerr_ {
    int32_t error;
    struct nlmsghdr_ msg;
};

struct nlattr_ {
    uint16_t nla_len;
    uint16_t nla_type;
};

struct ifinfomsg_ {
    uint8_t ifi_family;
    uint8_t __ifi_pad;
    uint16_t ifi_type;
    int32_t ifi_index;
    uint32_t ifi_flags;
    uint32_t ifi_change;
};

struct ifaddrmsg_ {
    uint8_t ifa_family;
    uint8_t ifa_prefixlen;
    uint8_t ifa_flags;
    uint8_t ifa_scope;
    uint32_t ifa_index;
};

struct rtgenmsg_ {
    uint8_t rtgen_family;
};

struct rtmsg_ {
    uint8_t rtm_family;
    uint8_t rtm_dst_len;
    uint8_t rtm_src_len;
    uint8_t rtm_tos;
    uint8_t rtm_table;
    uint8_t rtm_protocol;
    uint8_t rtm_scope;
    uint8_t rtm_type;
    uint32_t rtm_flags;
};

static int netlink_append_nlmsg(struct fd *sock, uint16_t type, uint16_t flags,
        uint32_t seq, const void *payload, size_t payload_len);
static int netlink_append_error(struct fd *sock, uint32_t seq,
        const struct nlmsghdr_ *req, int err_code);
static int netlink_append_done(struct fd *sock, uint32_t seq);
static void netlink_notify_register(struct fd *sock);
static void netlink_notify_unregister(struct fd *sock);

struct inet_diag_sockid_ {
    uint16_t idiag_sport;
    uint16_t idiag_dport;
    uint32_t idiag_src[4];
    uint32_t idiag_dst[4];
    uint32_t idiag_if;
    uint32_t idiag_cookie[2];
};

struct inet_diag_req_v2_ {
    uint8_t sdiag_family;
    uint8_t sdiag_protocol;
    uint8_t idiag_ext;
    uint8_t pad;
    uint32_t idiag_states;
    struct inet_diag_sockid_ id;
};

struct inet_diag_msg_ {
    uint8_t idiag_family;
    uint8_t idiag_state;
    uint8_t idiag_timer;
    uint8_t idiag_retrans;
    struct inet_diag_sockid_ id;
    uint32_t idiag_expires;
    uint32_t idiag_rqueue;
    uint32_t idiag_wqueue;
    uint32_t idiag_uid;
    uint32_t idiag_inode;
};

struct unix_diag_req_ {
    uint8_t sdiag_family;
    uint8_t sdiag_protocol;
    uint16_t pad;
    uint32_t udiag_states;
    uint32_t udiag_ino;
    uint32_t udiag_show;
    uint32_t udiag_cookie[2];
};

struct unix_diag_msg_ {
    uint8_t udiag_family;
    uint8_t udiag_type;
    uint8_t udiag_state;
    uint8_t pad;
    uint32_t udiag_ino;
    uint32_t udiag_cookie[2];
};

// Generic netlink (NETLINK_GENERIC): just enough of the genl controller to
// resolve the TASKSTATS family, plus TASKSTATS_CMD_GET itself. This is
// iotop's only per-pid data source (it hard-exits if genl taskstats is
// unavailable); pidstat and others use it too. Constants match
// <linux/genetlink.h> and <linux/taskstats.h>.
#define GENL_ID_CTRL_ 0x10
#define CTRL_CMD_NEWFAMILY_ 1
#define CTRL_CMD_GETFAMILY_ 3
#define CTRL_ATTR_FAMILY_ID_ 1
#define CTRL_ATTR_FAMILY_NAME_ 2
#define CTRL_ATTR_VERSION_ 3
#define CTRL_ATTR_HDRSIZE_ 4
#define CTRL_ATTR_MAXATTR_ 5

// Real kernels allocate taskstats' family id dynamically (>= GENL_START_ALLOC
// = 0x13); userspace must resolve it by name, so any fixed value here is fine.
#define GENL_FAMILY_TASKSTATS_ 0x16
#define TASKSTATS_GENL_NAME_ "TASKSTATS"
#define TASKSTATS_GENL_VERSION_ 0x1

#define TASKSTATS_CMD_GET_ 1
#define TASKSTATS_CMD_NEW_ 2
#define TASKSTATS_CMD_ATTR_PID_ 1
#define TASKSTATS_CMD_ATTR_TGID_ 2
#define TASKSTATS_TYPE_PID_ 1
#define TASKSTATS_TYPE_TGID_ 2
#define TASKSTATS_TYPE_STATS_ 3
#define TASKSTATS_TYPE_AGGR_PID_ 4
#define TASKSTATS_TYPE_AGGR_TGID_ 5

struct genlmsghdr_ {
    uint8_t cmd;
    uint8_t version;
    uint16_t reserved;
};
#define GENL_HDRLEN_ NLMSG_ALIGN(sizeof(struct genlmsghdr_))

// struct taskstats at TASKSTATS_VERSION 8 (ends at freepages_delay_total),
// the newest version that contains nothing we can't at least plausibly fill.
// The struct is append-only across versions and readers are told which
// version they got via ->version, so tools bundling newer headers (iotop
// ships v14/v15) read the common prefix. The explicit aligned(8) attributes
// mirror the uapi header and make the layout identical on i386 and x86_64
// guests and the host.
#define TASKSTATS_VERSION_ 8
#define TS_COMM_LEN_ 32
struct taskstats_ {
    uint16_t version;
    uint32_t ac_exitcode;
    uint8_t ac_flag;
    uint8_t ac_nice;
    uint64_t cpu_count __attribute__((aligned(8)));
    uint64_t cpu_delay_total;
    uint64_t blkio_count;
    uint64_t blkio_delay_total;
    uint64_t swapin_count;
    uint64_t swapin_delay_total;
    uint64_t cpu_run_real_total;
    uint64_t cpu_run_virtual_total;
    char ac_comm[TS_COMM_LEN_];
    uint8_t ac_sched __attribute__((aligned(8)));
    uint8_t ac_pad[3];
    uint32_t ac_uid __attribute__((aligned(8)));
    uint32_t ac_gid;
    uint32_t ac_pid;
    uint32_t ac_ppid;
    uint32_t ac_btime;
    uint64_t ac_etime __attribute__((aligned(8)));
    uint64_t ac_utime;
    uint64_t ac_stime;
    uint64_t ac_minflt;
    uint64_t ac_majflt;
    uint64_t coremem;
    uint64_t virtmem;
    uint64_t hiwater_rss;
    uint64_t hiwater_vm;
    uint64_t read_char;
    uint64_t write_char;
    uint64_t read_syscalls;
    uint64_t write_syscalls;
    uint64_t read_bytes;
    uint64_t write_bytes;
    uint64_t cancelled_write_bytes;
    uint64_t nvcsw;
    uint64_t nivcsw;
    uint64_t ac_utimescaled;
    uint64_t ac_stimescaled;
    uint64_t cpu_scaled_run_real_total;
    uint64_t freepages_count;
    uint64_t freepages_delay_total;
};
_Static_assert(sizeof(struct taskstats_) == 328, "taskstats v8 must be 328 bytes");

static int netlink_append_attr_raw(char *buf, size_t cap, size_t *len_io, uint16_t type,
        const void *data, size_t data_len) {
    size_t attr_len = sizeof(struct nlattr_) + data_len;
    size_t aligned_len = NLA_ALIGN(attr_len);
    if (*len_io + aligned_len > cap)
        return _ENOBUFS;
    struct nlattr_ *attr = (struct nlattr_ *) (buf + *len_io);
    attr->nla_len = (uint16_t) attr_len;
    attr->nla_type = type;
    memcpy(attr + 1, data, data_len);
    memset((char *) attr + attr_len, 0, aligned_len - attr_len);
    *len_io += aligned_len;
    return 0;
}

struct netlink_link_info {
    uint32_t mtu;
    uint32_t txqlen;
    uint8_t operstate;
    uint8_t address[16];
    uint8_t broadcast[16];
    size_t address_len;
    size_t broadcast_len;
};

static void netlink_fill_link_info(const struct ifaddrs *addrs, const struct ifaddrs *cursor,
        struct netlink_link_info *info) {
    info->mtu = 1500;
    info->txqlen = 1000;
    info->operstate = (cursor->ifa_flags & IFF_RUNNING) ? IF_OPER_UP_ : IF_OPER_UNKNOWN_;
#if defined(__APPLE__)
    if (cursor->ifa_data != NULL) {
        const struct if_data *stats = (const struct if_data *) cursor->ifa_data;
        if (stats->ifi_mtu != 0)
            info->mtu = (uint32_t) stats->ifi_mtu;
    }
    for (const struct ifaddrs *entry = addrs; entry != NULL; entry = entry->ifa_next) {
        if (entry->ifa_name == NULL || entry->ifa_addr == NULL)
            continue;
        if (strcmp(entry->ifa_name, cursor->ifa_name) != 0)
            continue;
        if (entry->ifa_addr->sa_family != AF_LINK)
            continue;
        const struct sockaddr_dl *sdl = (const struct sockaddr_dl *) entry->ifa_addr;
        if (sdl->sdl_alen == 0)
            continue;
        size_t addr_len = sdl->sdl_alen;
        if (addr_len > sizeof(info->address))
            addr_len = sizeof(info->address);
        memcpy(info->address, LLADDR(sdl), addr_len);
        info->address_len = addr_len;
        if ((cursor->ifa_flags & IFF_BROADCAST) && addr_len == 6) {
            memset(info->broadcast, 0xff, addr_len);
            info->broadcast_len = addr_len;
        }
        break;
    }
#endif
}

static bool netlink_addr_is_link_local(const struct sockaddr *sa) {
    if (sa == NULL)
        return false;
    if (sa->sa_family == AF_INET) {
        const uint8_t *bytes = (const uint8_t *) &((const struct sockaddr_in *) sa)->sin_addr;
        return bytes[0] == 169 && bytes[1] == 254;
    }
    if (sa->sa_family == AF_INET6) {
        const uint8_t *bytes = (const uint8_t *) &((const struct sockaddr_in6 *) sa)->sin6_addr;
        return bytes[0] == 0xfe && (bytes[1] & 0xc0) == 0x80;
    }
    return false;
}

static uint8_t netlink_addr_scope(const struct ifaddrs *ifa) {
    if (ifa->ifa_flags & IFF_LOOPBACK)
        return RT_SCOPE_HOST_;
    if (netlink_addr_is_link_local(ifa->ifa_addr))
        return RT_SCOPE_LINK_;
    return RT_SCOPE_UNIVERSE_;
}

static uint8_t netlink_route_request_family(const void *payload, size_t payload_len) {
    if (payload == NULL || payload_len < sizeof(struct rtgenmsg_))
        return 0;
    return ((const struct rtgenmsg_ *) payload)->rtgen_family;
}

static uint32_t netlink_linux_if_flags(unsigned host_flags) {
    uint32_t linux_flags = 0;
    if (host_flags & IFF_UP)
        linux_flags |= IFF_UP_LINUX_;
    if (host_flags & IFF_BROADCAST)
        linux_flags |= IFF_BROADCAST_LINUX_;
    if (host_flags & IFF_LOOPBACK)
        linux_flags |= IFF_LOOPBACK_LINUX_;
    if (host_flags & IFF_POINTOPOINT)
        linux_flags |= IFF_POINTOPOINT_LINUX_;
    if (host_flags & IFF_RUNNING) {
        // Linux splits "administratively up" (IFF_UP) from "carrier present"
        // (IFF_LOWER_UP, netdevice-level L1 state); Darwin folds both into
        // IFF_RUNNING. systemd-resolved's link_relevant() demands
        // IFF_UP|IFF_LOWER_UP before it will consider a link usable --
        // without LOWER_UP it sees no relevant links, declares
        // io.systemd.Resolve.NetworkDown, and refuses EVERY DNS query (the
        // upstream servers and UDP path never even get tried), which turned
        // all hostname resolution off for the whole Arch guest via
        // nsswitch's "resolve [!UNAVAIL=return]" hard stop.
        linux_flags |= IFF_RUNNING_LINUX_ | IFF_LOWER_UP_LINUX_;
    }
#ifdef IFF_NOARP
    if (host_flags & IFF_NOARP)
        linux_flags |= IFF_NOARP_LINUX_;
#endif
#ifdef IFF_PROMISC
    if (host_flags & IFF_PROMISC)
        linux_flags |= IFF_PROMISC_LINUX_;
#endif
#ifdef IFF_MULTICAST
    if (host_flags & IFF_MULTICAST)
        linux_flags |= IFF_MULTICAST_LINUX_;
#endif
    // A Linux netdev never has an empty flag word: every device type's setup
    // routine sets something, so even a fully-down device carries at least
    // BROADCAST|MULTICAST (ethernet) or NOARP (tunnels). Darwin does have
    // flagless interfaces -- `ifconfig stf0` (the 6to4 tunnel, present on
    // macOS AND iOS) prints literally "flags=0<>" -- and passing that zero
    // through synthesizes a link that cannot exist on Linux.
    //
    // systemd-networkd then SIGABRTs the moment it sees one. link_update_flags()
    // (networkd-link.c) early-returns when the incoming flags and operstate both
    // match what the Link already holds; a freshly allocated Link is zeroed, so
    // flags==0 plus operstate==IF_OPER_UNKNOWN(0) compares equal on the very
    // first RTM_NEWLINK and the function returns BEFORE calling
    // link_update_operstate(). link->carrier_state is left at 0
    // (LINK_OPERSTATE_MISSING), which is one of the two holes in
    // link_carrier_state_table[], so link_carrier_state_to_string() returns NULL
    // and link_save()'s assert(carrier_state) aborts -- taking down networkd on
    // every restart ("Failed to start Network Configuration" in a tight loop).
    //
    // NOARP is the honest floor: it asserts only that the link does no ARP,
    // which is trivially true of one advertising neither broadcast nor
    // multicast, and it is exactly what Linux reports for its own counterpart
    // of this device (`sit0` is "<NOARP>").
    if (linux_flags == 0)
        linux_flags = IFF_NOARP_LINUX_;
    return linux_flags;
}

static uint8_t netlink_guest_family_from_host(sa_family_t family) {
    switch (family) {
        case AF_INET:
            return AF_INET_;
        case AF_INET6:
            return AF_INET6_;
        default:
            return 0;
    }
}

static uint8_t netlink_prefixlen_from_sockaddr(const struct sockaddr *sa) {
    if (sa == NULL)
        return 0;
    const uint8_t *bytes = NULL;
    size_t len = 0;
    if (sa->sa_family == AF_INET) {
        bytes = (const uint8_t *) &((const struct sockaddr_in *) sa)->sin_addr;
        len = sizeof(((const struct sockaddr_in *) sa)->sin_addr);
    } else if (sa->sa_family == AF_INET6) {
        bytes = (const uint8_t *) &((const struct sockaddr_in6 *) sa)->sin6_addr;
        len = sizeof(((const struct sockaddr_in6 *) sa)->sin6_addr);
    } else {
        return 0;
    }
    uint8_t prefix = 0;
    for (size_t i = 0; i < len; i++)
        prefix += (uint8_t) __builtin_popcount(bytes[i]);
    return prefix;
}

// Fills payload/payload_len with one RTM_NEWLINK-shaped ifinfomsg_+attrs
// record for ifname. Shared by the dump path (netlink_append_route_link,
// tagged with the requesting socket's seq and NLM_F_MULTI) and the
// multicast-notification path (netlink_notify_route_link, untagged and
// unflagged like a real spontaneous kernel notification) -- see
// netlink_notify_link_change.
static int netlink_build_link_payload(char *payload, size_t cap, size_t *payload_len_out,
        const char *ifname, unsigned ifflags, const struct netlink_link_info *info) {
    size_t payload_len = sizeof(struct ifinfomsg_);
    memset(payload, 0, cap);
    struct ifinfomsg_ *msg = (struct ifinfomsg_ *) payload;
    msg->ifi_family = AF_UNSPEC;
    msg->ifi_type = (strcmp(ifname, "lo0") == 0 || strcmp(ifname, "lo") == 0)
        ? ARPHRD_LOOPBACK_
        : ARPHRD_ETHER_;
    msg->ifi_index = (int32_t) if_nametoindex(ifname);
    msg->ifi_flags = netlink_linux_if_flags(ifflags);
    msg->ifi_change = 0xffffffffu;

    int err = netlink_append_attr_raw(payload, cap, &payload_len,
            IFLA_IFNAME_, ifname, strlen(ifname) + 1);
    if (err < 0)
        return err;
    err = netlink_append_attr_raw(payload, cap, &payload_len,
            IFLA_MTU_, &info->mtu, sizeof(info->mtu));
    if (err < 0)
        return err;
    if (info->address_len != 0) {
        err = netlink_append_attr_raw(payload, cap, &payload_len,
                IFLA_ADDRESS_, info->address, info->address_len);
        if (err < 0)
            return err;
    }
    if (info->broadcast_len != 0) {
        err = netlink_append_attr_raw(payload, cap, &payload_len,
                IFLA_BROADCAST_, info->broadcast, info->broadcast_len);
        if (err < 0)
            return err;
    }
    err = netlink_append_attr_raw(payload, cap, &payload_len,
            IFLA_TXQLEN_, &info->txqlen, sizeof(info->txqlen));
    if (err < 0)
        return err;
    err = netlink_append_attr_raw(payload, cap, &payload_len,
            IFLA_OPERSTATE_, &info->operstate, sizeof(info->operstate));
    if (err < 0)
        return err;
    *payload_len_out = payload_len;
    return 0;
}

static int netlink_append_route_link(struct fd *sock, const struct nlmsghdr_ *req_hdr,
        const char *ifname, unsigned ifflags, const struct netlink_link_info *info) {
    char payload[256];
    size_t payload_len;
    int err = netlink_build_link_payload(payload, sizeof(payload), &payload_len,
            ifname, ifflags, info);
    if (err < 0)
        return err;
    return netlink_append_nlmsg(sock, RTM_NEWLINK_, NLM_F_MULTI_,
            req_hdr->nlmsg_seq, payload, payload_len);
}

static int netlink_append_route_links(struct fd *sock, const struct nlmsghdr_ *req_hdr,
        const void *payload, size_t payload_len) {
    struct ifaddrs *addrs = NULL;
    if (getifaddrs(&addrs) != 0)
        return _EIO;

    // Linux does NOT filter a link dump by the request's ifi_family: the
    // RTM_GETLINK dump handler is registered under PF_UNSPEC and every other
    // family falls back to it, so AF_INET/AF_INET6/AF_PACKET/AF_NETLINK all
    // return the complete link list. Only PF_BRIDGE has its own handler
    // (rtnl_bridge_getlink), which emits bridge ports only -- none here.
    //
    // We used to drop every link for any family other than AF_UNSPEC/AF_PACKET,
    // which made `ip -4 addr` and `ip -6 addr` print NOTHING: iproute2 asks for
    // the links first (with ifi_family set to the -4/-6 family), builds its
    // ifindex->name map from the reply, then asks for the addresses. The
    // address dump was already correct and filtered properly, but with an empty
    // link map iproute2 has no name to attach an address to and silently drops
    // every one of them. Plain `ip addr` sends AF_UNSPEC and was unaffected,
    // which is why this hid: the family-qualified forms are the only casualties.
    uint8_t family = netlink_route_request_family(payload, payload_len);
    int err = 0;
    for (const struct ifaddrs *cursor = addrs; cursor != NULL; cursor = cursor->ifa_next) {
        if (cursor->ifa_name == NULL)
            continue;
        if (family == AF_BRIDGE_)
            continue;
        bool seen = false;
        for (const struct ifaddrs *prev = addrs; prev != cursor; prev = prev->ifa_next) {
            if (prev->ifa_name != NULL && strcmp(prev->ifa_name, cursor->ifa_name) == 0) {
                seen = true;
                break;
            }
        }
        if (seen)
            continue;
        struct netlink_link_info info = {};
        netlink_fill_link_info(addrs, cursor, &info);
        err = netlink_append_route_link(sock, req_hdr, cursor->ifa_name, cursor->ifa_flags, &info);
        if (err < 0)
            break;
    }
    freeifaddrs(addrs);
    if (err >= 0)
        err = netlink_append_done(sock, req_hdr->nlmsg_seq);
    return err;
}

// Fills payload/payload_len with one RTM_NEWADDR-shaped ifaddrmsg_+attrs
// record for ifa. *payload_len_out == 0 on return with err == 0 means
// "nothing to send" (an address family this emulation doesn't represent),
// not an error -- mirrors netlink_append_route_addr's pre-existing
// "return 0" no-op for that case. Shared by the dump path
// (netlink_append_route_addr) and the notification path
// (netlink_notify_route_addr) -- see netlink_build_link_payload's comment.
static int netlink_build_addr_payload(char *payload, size_t cap, size_t *payload_len_out,
        const struct ifaddrs *ifa) {
    size_t payload_len = sizeof(struct ifaddrmsg_);
    memset(payload, 0, cap);
    struct ifaddrmsg_ *msg = (struct ifaddrmsg_ *) payload;
    msg->ifa_family = netlink_guest_family_from_host(ifa->ifa_addr->sa_family);
    msg->ifa_prefixlen = netlink_prefixlen_from_sockaddr(ifa->ifa_netmask);
    msg->ifa_flags = 0;
    msg->ifa_scope = netlink_addr_scope(ifa);
    msg->ifa_index = if_nametoindex(ifa->ifa_name);

    if (ifa->ifa_addr->sa_family == AF_INET) {
        const struct in_addr *addr = &((const struct sockaddr_in *) ifa->ifa_addr)->sin_addr;
        int err = netlink_append_attr_raw(payload, cap, &payload_len,
                IFA_LOCAL_, addr, sizeof(*addr));
        if (err < 0)
            return err;
        err = netlink_append_attr_raw(payload, cap, &payload_len,
                IFA_ADDRESS_, addr, sizeof(*addr));
        if (err < 0)
            return err;
        if (ifa->ifa_dstaddr != NULL && (ifa->ifa_flags & IFF_POINTOPOINT)) {
            const struct in_addr *dst = &((const struct sockaddr_in *) ifa->ifa_dstaddr)->sin_addr;
            err = netlink_append_attr_raw(payload, cap, &payload_len,
                    IFA_BROADCAST_, dst, sizeof(*dst));
            if (err < 0)
                return err;
        } else if (ifa->ifa_broadaddr != NULL && (ifa->ifa_flags & IFF_BROADCAST)) {
            const struct in_addr *bcast = &((const struct sockaddr_in *) ifa->ifa_broadaddr)->sin_addr;
            err = netlink_append_attr_raw(payload, cap, &payload_len,
                    IFA_BROADCAST_, bcast, sizeof(*bcast));
            if (err < 0)
                return err;
        }
    } else if (ifa->ifa_addr->sa_family == AF_INET6) {
        const struct in6_addr *addr6 = &((const struct sockaddr_in6 *) ifa->ifa_addr)->sin6_addr;
        int err = netlink_append_attr_raw(payload, cap, &payload_len,
                IFA_LOCAL_, addr6, sizeof(*addr6));
        if (err < 0)
            return err;
        err = netlink_append_attr_raw(payload, cap, &payload_len,
                IFA_ADDRESS_, addr6, sizeof(*addr6));
        if (err < 0)
            return err;
    } else {
        *payload_len_out = 0;
        return 0;
    }

    int err = netlink_append_attr_raw(payload, cap, &payload_len,
            IFA_LABEL_, ifa->ifa_name, strlen(ifa->ifa_name) + 1);
    if (err < 0)
        return err;
    // IFA_FLAGS is the u32 extension of ifaddrmsg's u8 ifa_flags field
    // (Linux 3.14+) and modern userspace treats it as mandatory:
    // systemd-resolved's link_address_update_rtnl() does a REQUIRED
    // sd_netlink_message_read_u32(m, IFA_FLAGS, ...) and propagates the
    // -ENODATA on miss all the way out of address coldplug, killing the
    // whole daemon at startup ("Could not create manager: No data
    // available"). All our addresses are host-configured and static, so
    // IFA_F_PERMANENT is the honest value (and carries none of the
    // DEPRECATED/TENTATIVE bits that would make resolved ignore the
    // address).
    uint32_t ifa_flags32 = IFA_F_PERMANENT_;
    err = netlink_append_attr_raw(payload, cap, &payload_len,
            IFA_FLAGS_, &ifa_flags32, sizeof(ifa_flags32));
    if (err < 0)
        return err;
    msg->ifa_flags = (uint8_t) ifa_flags32;
    *payload_len_out = payload_len;
    return 0;
}

static int netlink_append_route_addr(struct fd *sock, const struct nlmsghdr_ *req_hdr,
        const struct ifaddrs *ifa) {
    char payload[256];
    size_t payload_len = 0;
    int err = netlink_build_addr_payload(payload, sizeof(payload), &payload_len, ifa);
    if (err < 0)
        return err;
    if (payload_len == 0)
        return 0;
    return netlink_append_nlmsg(sock, RTM_NEWADDR_, NLM_F_MULTI_,
            req_hdr->nlmsg_seq, payload, payload_len);
}

static int netlink_append_route_addrs(struct fd *sock, const struct nlmsghdr_ *req_hdr,
        const void *payload, size_t payload_len) {
    struct ifaddrs *addrs = NULL;
    if (getifaddrs(&addrs) != 0)
        return _EIO;

    uint8_t family = netlink_route_request_family(payload, payload_len);
    int err = 0;
    for (const struct ifaddrs *cursor = addrs; cursor != NULL; cursor = cursor->ifa_next) {
        if (cursor->ifa_name == NULL || cursor->ifa_addr == NULL)
            continue;
        if (cursor->ifa_addr->sa_family != AF_INET && cursor->ifa_addr->sa_family != AF_INET6)
            continue;
        if (family != AF_UNSPEC && family != 0 &&
                netlink_guest_family_from_host(cursor->ifa_addr->sa_family) != family)
            continue;
        err = netlink_append_route_addr(sock, req_hdr, cursor);
        if (err < 0)
            break;
    }
    freeifaddrs(addrs);
    if (err >= 0)
        err = netlink_append_done(sock, req_hdr->nlmsg_seq);
    return err;
}

// ---------------------------------------------------------------------
// Multicast link/address-change notifications for RTNLGRP_LINK/
// RTNLGRP_IPV4_IFADDR/RTNLGRP_IPV6_IFADDR subscribers.
//
// iSH has no real kernel to generate these spontaneously, so a dedicated
// background thread (netlink_link_watch_thread, started once from main.c)
// polls getifaddrs() periodically, diffs against the previous snapshot,
// and for each interface/address that appeared or disappeared, appends an
// RTM_NEWLINK/RTM_DELLINK/RTM_NEWADDR/RTM_DELADDR message (seq=0, no
// NLM_F_MULTI -- exactly what a real spontaneous kernel notification looks
// like, as opposed to a dump reply) to every currently-open netlink socket
// whose subscribed groups match, then wakes any poller blocked on it.
//
// This exists specifically because subscribing to these groups and then
// waiting for a notification that never arrives is what makes sd-netlink
// managers (systemd-resolved, systemd's own PID 1 netlink setup) hang
// forever once NETLINK_PKTINFO/SO_BINDTOIFINDEX are accepted -- see the
// comments on those two option handlers in sys_setsockopt_guest_abi.
// ---------------------------------------------------------------------

static int netlink_notify_route_link(struct fd *sock, const char *ifname, unsigned ifflags,
        const struct netlink_link_info *info, bool is_new) {
    char payload[256];
    size_t payload_len;
    int err = netlink_build_link_payload(payload, sizeof(payload), &payload_len,
            ifname, ifflags, info);
    if (err < 0)
        return err;
    return netlink_append_nlmsg(sock, is_new ? RTM_NEWLINK_ : RTM_DELLINK_, 0, 0, payload, payload_len);
}

static int netlink_notify_route_addr(struct fd *sock, const struct ifaddrs *ifa, bool is_new) {
    char payload[256];
    size_t payload_len = 0;
    int err = netlink_build_addr_payload(payload, sizeof(payload), &payload_len, ifa);
    if (err < 0)
        return err;
    if (payload_len == 0)
        return 0;
    return netlink_append_nlmsg(sock, is_new ? RTM_NEWADDR_ : RTM_DELADDR_, 0, 0, payload, payload_len);
}

// Process-wide registry of every currently-open AF_NETLINK socket, so the
// watcher thread has something to fan notifications out to. Membership is
// unconditional (every netlink socket is registered at creation and
// unregistered at close, regardless of its current subscribed groups) --
// the group-match filter is applied per-notification at delivery time
// instead, which keeps registration/deregistration free of any need to
// track group-membership transitions in setsockopt/bind.
static lock_t netlink_notify_registry_lock = LOCK_INITIALIZER;
static struct list netlink_notify_registry = LIST_INITIALIZER(netlink_notify_registry);

static void netlink_notify_register(struct fd *sock) {
    lock(&netlink_notify_registry_lock, 0);
    if (!sock->socket.netlink_notify_registered) {
        list_add(&netlink_notify_registry, &sock->socket.netlink_notify_link);
        sock->socket.netlink_notify_registered = true;
    }
    unlock(&netlink_notify_registry_lock);
}

static void netlink_notify_unregister(struct fd *sock) {
    lock(&netlink_notify_registry_lock, 0);
    if (sock->socket.netlink_notify_registered) {
        list_remove(&sock->socket.netlink_notify_link);
        sock->socket.netlink_notify_registered = false;
    }
    unlock(&netlink_notify_registry_lock);
}

// Delivers one notification (built via the callback) to every registered
// netlink socket whose netlink_groups has `group`'s bit set. Takes a
// temporary fd_retain_if_live() reference per candidate so a concurrent
// sock_close() can't free the fd out from under this thread; releases it
// (via fd_close, ordinary refcounted release, not a real close since this
// is not the last reference while the guest still holds its own) before
// moving to the next one. poll_wakeup() is called with no lock of ours
// held -- netlink_append_nlmsg takes and releases netlink_reply_lock
// internally, so by the time we call poll_wakeup() here we hold nothing
// that fd's own sock_poll could need, avoiding the lock-order hazard
// documented on poll_wakeup itself (see fs/poll.c).
#define NETLINK_NOTIFY_MAX_CANDIDATES 64

// Snapshots every registered fd subscribed to `group` into `candidates`,
// each held with a temporary fd_retain_if_live() reference (so a
// concurrent sock_close() can't free it out from under the caller), and
// returns how many. The registry lock's critical section stays short --
// callers deliver outside it and release each reference with fd_close()
// when done. Silently drops candidates past NETLINK_NOTIFY_MAX_CANDIDATES
// (no real deployment should ever have anywhere near that many concurrent
// netlink subscribers; dropping the excess is far better than an unbounded
// stack array or blocking allocation from this thread).
static size_t netlink_notify_snapshot_candidates(uint32_t group,
        struct fd *candidates[NETLINK_NOTIFY_MAX_CANDIDATES]) {
    uint32_t bit = 1u << (group - 1);
    size_t candidate_count = 0;
    lock(&netlink_notify_registry_lock, 0);
    struct fd *sock;
    list_for_each_entry(&netlink_notify_registry, sock, socket.netlink_notify_link) {
        if (!(sock->socket.netlink_groups & bit))
            continue;
        struct fd *retained = fd_retain_if_live(sock);
        if (retained == NULL)
            continue;
        if (candidate_count < NETLINK_NOTIFY_MAX_CANDIDATES)
            candidates[candidate_count++] = retained;
        else
            fd_close(retained);
    }
    unlock(&netlink_notify_registry_lock);
    return candidate_count;
}

// poll_wakeup() is called with no lock of ours held -- netlink_append_nlmsg
// (called from the two *_route_link/_route_addr builders below) takes and
// releases netlink_reply_lock internally, so by the time poll_wakeup() runs
// here this thread holds nothing that fd's own sock_poll could need,
// avoiding the lock-order hazard documented on poll_wakeup itself (fs/poll.c).
static void netlink_notify_deliver_link(uint32_t group, const char *ifname, unsigned ifflags,
        const struct netlink_link_info *info, bool is_new) {
    struct fd *candidates[NETLINK_NOTIFY_MAX_CANDIDATES];
    size_t count = netlink_notify_snapshot_candidates(group, candidates);
    for (size_t i = 0; i < count; i++) {
        netlink_notify_route_link(candidates[i], ifname, ifflags, info, is_new);
        poll_wakeup(candidates[i], POLL_READ);
        fd_close(candidates[i]);
    }
}

static void netlink_notify_deliver_addr(uint32_t group, const struct ifaddrs *ifa, bool is_new) {
    struct fd *candidates[NETLINK_NOTIFY_MAX_CANDIDATES];
    size_t count = netlink_notify_snapshot_candidates(group, candidates);
    for (size_t i = 0; i < count; i++) {
        netlink_notify_route_addr(candidates[i], ifa, is_new);
        poll_wakeup(candidates[i], POLL_READ);
        fd_close(candidates[i]);
    }
}

// ---------------------------------------------------------------------
// Host network-state polling: periodically snapshots getifaddrs(), diffs
// against the previous snapshot, and delivers a notification for each
// interface/address that appeared or disappeared. Bounded arrays (no
// dynamic allocation per interface/address) sized generously past any
// real iSH deployment's interface count; entries past the bound are
// silently dropped from watching, same tradeoff as
// NETLINK_NOTIFY_MAX_CANDIDATES above.
// ---------------------------------------------------------------------

#define NETLINK_WATCH_MAX_LINKS 32
#define NETLINK_WATCH_MAX_ADDRS 64

struct netlink_watch_link {
    char name[IFNAMSIZ_];
    unsigned flags;
    struct netlink_link_info info;
};

struct netlink_watch_addr {
    char ifname[IFNAMSIZ_];
    unsigned ifflags;
    struct sockaddr_storage addr;
    struct sockaddr_storage netmask;
    struct sockaddr_storage dstaddr;
    struct sockaddr_storage broadaddr;
    bool has_dstaddr;
    bool has_broadaddr;
};

struct netlink_watch_snapshot {
    struct netlink_watch_link links[NETLINK_WATCH_MAX_LINKS];
    size_t link_count;
    struct netlink_watch_addr addrs[NETLINK_WATCH_MAX_ADDRS];
    size_t addr_count;
};

static void netlink_watch_capture(struct netlink_watch_snapshot *snap) {
    memset(snap, 0, sizeof(*snap));
    struct ifaddrs *addrs = NULL;
    if (getifaddrs(&addrs) != 0)
        return;
    for (const struct ifaddrs *cursor = addrs; cursor != NULL; cursor = cursor->ifa_next) {
        if (cursor->ifa_name == NULL)
            continue;
        bool seen_link = false;
        for (size_t i = 0; i < snap->link_count; i++) {
            if (strcmp(snap->links[i].name, cursor->ifa_name) == 0) {
                seen_link = true;
                break;
            }
        }
        if (!seen_link && snap->link_count < NETLINK_WATCH_MAX_LINKS) {
            struct netlink_watch_link *link = &snap->links[snap->link_count++];
            strncpy(link->name, cursor->ifa_name, sizeof(link->name) - 1);
            link->flags = cursor->ifa_flags;
            netlink_fill_link_info(addrs, cursor, &link->info);
        }

        if (cursor->ifa_addr == NULL ||
                (cursor->ifa_addr->sa_family != AF_INET && cursor->ifa_addr->sa_family != AF_INET6) ||
                snap->addr_count >= NETLINK_WATCH_MAX_ADDRS)
            continue;
        struct netlink_watch_addr *a = &snap->addrs[snap->addr_count++];
        strncpy(a->ifname, cursor->ifa_name, sizeof(a->ifname) - 1);
        a->ifflags = cursor->ifa_flags;
        size_t slen = cursor->ifa_addr->sa_family == AF_INET
            ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);
        memcpy(&a->addr, cursor->ifa_addr, slen);
        if (cursor->ifa_netmask != NULL)
            memcpy(&a->netmask, cursor->ifa_netmask, slen);
        if ((cursor->ifa_flags & IFF_POINTOPOINT) && cursor->ifa_dstaddr != NULL) {
            memcpy(&a->dstaddr, cursor->ifa_dstaddr, slen);
            a->has_dstaddr = true;
        } else if ((cursor->ifa_flags & IFF_BROADCAST) && cursor->ifa_broadaddr != NULL) {
            memcpy(&a->broadaddr, cursor->ifa_broadaddr, slen);
            a->has_broadaddr = true;
        }
    }
    freeifaddrs(addrs);
}

static bool netlink_watch_link_eq(const struct netlink_watch_link *a, const struct netlink_watch_link *b) {
    return strcmp(a->name, b->name) == 0 && a->flags == b->flags &&
        memcmp(&a->info, &b->info, sizeof(a->info)) == 0;
}

static bool netlink_watch_addr_eq(const struct netlink_watch_addr *a, const struct netlink_watch_addr *b) {
    return strcmp(a->ifname, b->ifname) == 0 && memcmp(&a->addr, &b->addr, sizeof(a->addr)) == 0;
}

// Reconstructs a self-contained struct ifaddrs from a captured snapshot
// entry, so the same netlink_notify_route_addr/netlink_build_addr_payload
// path used for dumps also serves DEL notifications, whose real interface
// state (needed to build the payload) is already gone from the host by the
// time we notice it's missing.
static void netlink_watch_addr_to_ifaddrs(const struct netlink_watch_addr *a, struct ifaddrs *out) {
    memset(out, 0, sizeof(*out));
    out->ifa_name = (char *) a->ifname;
    out->ifa_flags = a->ifflags;
    out->ifa_addr = (struct sockaddr *) &a->addr;
    out->ifa_netmask = (struct sockaddr *) &a->netmask;
    if (a->has_dstaddr)
        out->ifa_dstaddr = (struct sockaddr *) &a->dstaddr;
    if (a->has_broadaddr)
        out->ifa_broadaddr = (struct sockaddr *) &a->broadaddr;
}

static void netlink_watch_diff_and_notify(const struct netlink_watch_snapshot *old,
        const struct netlink_watch_snapshot *cur) {
    for (size_t i = 0; i < old->link_count; i++) {
        bool still_present = false;
        for (size_t j = 0; j < cur->link_count; j++) {
            if (netlink_watch_link_eq(&old->links[i], &cur->links[j])) {
                still_present = true;
                break;
            }
        }
        if (!still_present)
            netlink_notify_deliver_link(RTNLGRP_LINK_, old->links[i].name,
                    old->links[i].flags, &old->links[i].info, false);
    }
    for (size_t j = 0; j < cur->link_count; j++) {
        bool existed_before = false;
        for (size_t i = 0; i < old->link_count; i++) {
            if (netlink_watch_link_eq(&old->links[i], &cur->links[j])) {
                existed_before = true;
                break;
            }
        }
        if (!existed_before)
            netlink_notify_deliver_link(RTNLGRP_LINK_, cur->links[j].name,
                    cur->links[j].flags, &cur->links[j].info, true);
    }

    for (size_t i = 0; i < old->addr_count; i++) {
        bool still_present = false;
        for (size_t j = 0; j < cur->addr_count; j++) {
            if (netlink_watch_addr_eq(&old->addrs[i], &cur->addrs[j])) {
                still_present = true;
                break;
            }
        }
        if (!still_present) {
            struct ifaddrs fake;
            netlink_watch_addr_to_ifaddrs(&old->addrs[i], &fake);
            uint32_t group = old->addrs[i].addr.ss_family == AF_INET6
                ? RTNLGRP_IPV6_IFADDR_ : RTNLGRP_IPV4_IFADDR_;
            netlink_notify_deliver_addr(group, &fake, false);
        }
    }
    for (size_t j = 0; j < cur->addr_count; j++) {
        bool existed_before = false;
        for (size_t i = 0; i < old->addr_count; i++) {
            if (netlink_watch_addr_eq(&old->addrs[i], &cur->addrs[j])) {
                existed_before = true;
                break;
            }
        }
        if (!existed_before) {
            struct ifaddrs fake;
            netlink_watch_addr_to_ifaddrs(&cur->addrs[j], &fake);
            uint32_t group = cur->addrs[j].addr.ss_family == AF_INET6
                ? RTNLGRP_IPV6_IFADDR_ : RTNLGRP_IPV4_IFADDR_;
            netlink_notify_deliver_addr(group, &fake, true);
        }
    }
}

// One dedicated thread for the whole process (started once from
// xX_main_Xx, see fs/sock.h). Heap-allocated snapshots: each is tens of KB
// (32 links + 64 addrs, mostly sockaddr_storage), too large to keep on a
// thread stack twice over comfortably. Sleeps in a single bounded interval
// (not the multi-chunk pattern util/timer.c uses for arbitrary/huge
// durations) since this interval is a small fixed constant, not
// guest-controlled.
static void *netlink_link_watch_thread(void *unused) {
    (void) unused;
    struct netlink_watch_snapshot *prev = malloc(sizeof(*prev));
    struct netlink_watch_snapshot *cur = malloc(sizeof(*cur));
    if (prev == NULL || cur == NULL) {
        free(prev);
        free(cur);
        return NULL;
    }
    netlink_watch_capture(prev);
    while (true) {
        struct timespec interval = {.tv_sec = 5, .tv_nsec = 0};
        nanosleep(&interval, NULL);
        netlink_watch_capture(cur);
        netlink_watch_diff_and_notify(prev, cur);
        struct netlink_watch_snapshot *tmp = prev;
        prev = cur;
        cur = tmp;
    }
    return NULL;
}

void netlink_link_watch_start(void) {
    pthread_t thread;
    if (pthread_create(&thread, NULL, netlink_link_watch_thread, NULL) == 0)
        pthread_detach(thread);
}

static int netlink_append_route_route(struct fd *sock, const struct nlmsghdr_ *req_hdr,
        const struct host_route_entry *route) {
    char payload[256] = {};
    size_t payload_len = sizeof(struct rtmsg_);
    struct rtmsg_ *msg = (struct rtmsg_ *) payload;
    msg->rtm_family = AF_INET_;
    msg->rtm_dst_len = route->prefix_len;
    msg->rtm_src_len = 0;
    msg->rtm_tos = 0;
    msg->rtm_table = RT_TABLE_MAIN_;
    msg->rtm_protocol = route->protocol == HOST_ROUTE_PROTOCOL_BOOT ? RTPROT_BOOT_ : RTPROT_KERNEL_;
    msg->rtm_scope = route->scope == HOST_ROUTE_SCOPE_HOST ? RT_SCOPE_HOST_ :
        route->scope == HOST_ROUTE_SCOPE_LINK ? RT_SCOPE_LINK_ : RT_SCOPE_UNIVERSE_;
    msg->rtm_type = RTN_UNICAST_;
    msg->rtm_flags = 0;

    int err = 0;
    if (route->prefix_len != 0) {
        err = netlink_append_attr_raw(payload, sizeof(payload), &payload_len,
                RTA_DST_, &route->destination_be, sizeof(route->destination_be));
        if (err < 0)
            return err;
    }
    err = netlink_append_attr_raw(payload, sizeof(payload), &payload_len,
            RTA_OIF_, &route->ifindex, sizeof(route->ifindex));
    if (err < 0)
        return err;
    if (route->gateway_be != 0) {
        err = netlink_append_attr_raw(payload, sizeof(payload), &payload_len,
                RTA_GATEWAY_, &route->gateway_be, sizeof(route->gateway_be));
        if (err < 0)
            return err;
    }
    if (route->prefsrc_be != 0) {
        err = netlink_append_attr_raw(payload, sizeof(payload), &payload_len,
                RTA_PREFSRC_, &route->prefsrc_be, sizeof(route->prefsrc_be));
        if (err < 0)
            return err;
    }

    return netlink_append_nlmsg(sock, RTM_NEWROUTE_, NLM_F_MULTI_,
            req_hdr->nlmsg_seq, payload, payload_len);
}

static int netlink_append_route_routes(struct fd *sock, const struct nlmsghdr_ *req_hdr,
        const void *payload, size_t payload_len) {
    uint8_t family = netlink_route_request_family(payload, payload_len);
    if (family != AF_UNSPEC && family != 0 && family != AF_INET_)
        return netlink_append_done(sock, req_hdr->nlmsg_seq);

    struct host_route_table routes = {};
    if (host_route_table_collect(&routes) != 0)
        return _EIO;

    int err = 0;
    for (size_t i = 0; i < routes.count; i++) {
        err = netlink_append_route_route(sock, req_hdr, &routes.entries[i]);
        if (err < 0)
            break;
    }
    host_route_table_free(&routes);
    if (err >= 0)
        err = netlink_append_done(sock, req_hdr->nlmsg_seq);
    return err;
}

static int netlink_handle_route_request(struct fd *sock, const struct nlmsghdr_ *hdr,
        const void *payload, size_t payload_len) {
    switch (hdr->nlmsg_type) {
        case RTM_GETLINK_:
            return netlink_append_route_links(sock, hdr, payload, payload_len);
        case RTM_GETADDR_:
            return netlink_append_route_addrs(sock, hdr, payload, payload_len);
        case RTM_GETROUTE_:
            return netlink_append_route_routes(sock, hdr, payload, payload_len);
        default:
            // A bare NLMSG_DONE here is wrong for anything but a multi-part
            // dump terminator -- it doesn't match what a caller doing a
            // synchronous request/ack call (e.g. sd_netlink_call()-style,
            // matching on NLMSG_ERROR for its nlmsg_seq) is waiting for, so
            // that caller's recvmsg() never sees a satisfying reply and
            // blocks forever. Real Linux only replies at all when NLM_F_ACK
            // is set on the request; when it does, an accepted-but-
            // unimplemented request (RTM_NEWLINK/RTM_SETLINK/RTM_DELADDR/...
            // -- iSH doesn't actually configure anything) gets an
            // NLMSG_ERROR with error=0, the real ack, not EOPNOTSUPP: a
            // hard failure here would make callers that treat "the kernel
            // rejected my request" as fatal (as opposed to "iSH silently
            // no-ops it") abort setup entirely.
            if (hdr->nlmsg_flags & NLM_F_ACK_)
                return netlink_append_error(sock, hdr->nlmsg_seq, hdr, 0);
            return 0;
    }
}

struct diag_socket_entry {
    struct fd **fds;
    unsigned count;
    unsigned cap;
};

static uint32_t netlink_next_port_id(void);
// on iOS, when the device goes to sleep, all connected sockets are killed;
// reads/writes then return ENOTCONN, a POSIX violation this remaps to
// ECONNRESET. Defined near sock_read/sock_write; forward-declared here so the
// recvfrom/sendto/recvmsg/sendmsg paths above them can use it too.
static void sock_translate_err(struct fd *fd, int *err);
static int sock_take_pending_error(struct fd *fd);

const struct fd_ops socket_fdops;

static lock_t peer_lock = LOCK_INITIALIZER;

#define DEFAULT_TCP_CONGESTION "cubic"

static void sock_init_emulation_defaults(struct fd *fd);
static bool sockopt_is_linux_soft_unsupported(dword_t level, dword_t option);
static ssize_t sock_ioctl_size(int cmd);

static bool sock_trace_comm(const char *comm) {
    if (comm == NULL)
        return false;
    return strcmp(comm, "apk") == 0 ||
        strcmp(comm, "wget") == 0 ||
        strcmp(comm, "curl") == 0 ||
        strcmp(comm, "ping") == 0 ||
        strcmp(comm, "cat") == 0 ||
        strcmp(comm, "grep") == 0 ||
        strcmp(comm, "which") == 0 ||
        strcmp(comm, "install") == 0 ||
        strncmp(comm, "deboots", 7) == 0 ||
        strncmp(comm, "debootstrap", 11) == 0 ||
        strncmp(comm, "update-ca-certi", 15) == 0;
}

static bool sock_debug_comm(const char *comm) {
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("ISH_TRACE_SOCK_DEBUG") != NULL ? 1 : 0;
    if (!enabled)
        return false;
    if (comm == NULL)
        return false;
    return strcmp(comm, "apt") == 0 ||
        strcmp(comm, "apt-get") == 0 ||
        strncmp(comm, "http", 4) == 0;
}

static void sock_debug_event(const char *op, struct fd *sock, ssize_t result, int mapped_err) {
    if (current == NULL || !sock_debug_comm(current->comm))
        return;
    fprintf(stderr,
            "ish-sock:%s pid=%d comm=%s guest_domain=%d guest_type=%d protocol=%d real=%d result=%zd err=%d\n",
            op, current->pid, current->comm,
            sock != NULL ? sock->socket.domain : -1,
            sock != NULL ? sock->socket.type : -1,
            sock != NULL ? sock->socket.protocol : -1,
            sock != NULL ? sock->real_fd : -1,
            result, mapped_err);
}

static void sock_debug_guest_sockaddr(const char *op, struct fd *sock,
        guest_addr_t sockaddr_addr, uint_t sockaddr_len) {
    if (current == NULL || !sock_debug_comm(current->comm))
        return;
    if (sockaddr_addr == 0 || sockaddr_len < offsetof(struct sockaddr_, data) ||
            sockaddr_len > sizeof(struct sockaddr_max_)) {
        fprintf(stderr,
                "ish-sock:%s-addr pid=%d comm=%s real=%d guest_domain=%d guest_type=%d addr=%#llx len=%u detail=<none>\n",
                op, current->pid, current->comm,
                sock != NULL ? sock->real_fd : -1,
                sock != NULL ? sock->socket.domain : -1,
                sock != NULL ? sock->socket.type : -1,
                (unsigned long long) sockaddr_addr, sockaddr_len);
        return;
    }

    struct sockaddr_max_ fake_addr = {};
    if (user_read(sockaddr_addr, &fake_addr, sockaddr_len)) {
        fprintf(stderr,
                "ish-sock:%s-addr pid=%d comm=%s real=%d guest_domain=%d guest_type=%d addr=%#llx len=%u detail=<fault>\n",
                op, current->pid, current->comm,
                sock != NULL ? sock->real_fd : -1,
                sock != NULL ? sock->socket.domain : -1,
                sock != NULL ? sock->socket.type : -1,
                (unsigned long long) sockaddr_addr, sockaddr_len);
        return;
    }

    char detail[SOCKADDR_DATA_MAX + 32];
    if (fake_addr.family == AF_LOCAL_) {
        size_t path_size = sockaddr_len - offsetof(struct sockaddr_, data);
        if (path_size == 0) {
            snprintf(detail, sizeof(detail), "unix:<empty>");
        } else if (fake_addr.data[0] == '\0') {
            size_t copy = path_size - 1;
            if (copy > SOCKADDR_DATA_MAX)
                copy = SOCKADDR_DATA_MAX;
            char path[SOCKADDR_DATA_MAX + 1];
            memcpy(path, fake_addr.data + 1, copy);
            path[copy] = '\0';
            snprintf(detail, sizeof(detail), "unix-abstract:%s", path);
        } else {
            size_t copy = path_size;
            if (copy > SOCKADDR_DATA_MAX)
                copy = SOCKADDR_DATA_MAX;
            char path[SOCKADDR_DATA_MAX + 1];
            memcpy(path, fake_addr.data, copy);
            path[copy] = '\0';
            snprintf(detail, sizeof(detail), "unix:%s", path);
        }
    } else if (fake_addr.family == AF_INET_) {
        struct sockaddr_in_ *addr4 = (struct sockaddr_in_ *) &fake_addr;
        struct in_addr real_addr = {.s_addr = addr4->sin_addr};
        char host[INET_ADDRSTRLEN] = "";
        inet_ntop(AF_INET, &real_addr, host, sizeof(host));
        snprintf(detail, sizeof(detail), "inet:%s:%u", host, ntohs(addr4->sin_port));
    } else if (fake_addr.family == AF_INET6_) {
        snprintf(detail, sizeof(detail), "inet6");
    } else if (fake_addr.family == AF_NETLINK_) {
        struct sockaddr_nl_ *addr_nl = (struct sockaddr_nl_ *) &fake_addr;
        snprintf(detail, sizeof(detail), "netlink:pid=%u groups=%#x",
                addr_nl->nl_pid, addr_nl->nl_groups);
    } else {
        snprintf(detail, sizeof(detail), "family=%u", fake_addr.family);
    }

    fprintf(stderr,
            "ish-sock:%s-addr pid=%d comm=%s real=%d guest_domain=%d guest_type=%d addr=%#llx len=%u detail=%s\n",
            op, current->pid, current->comm,
            sock != NULL ? sock->real_fd : -1,
            sock != NULL ? sock->socket.domain : -1,
            sock != NULL ? sock->socket.type : -1,
            (unsigned long long) sockaddr_addr, sockaddr_len, detail);
}

static bool sock_trace_enabled(void) {
    if (current == NULL)
        return false;
    return sock_trace_comm(current->comm) && false;
}

static bool sock_is_x11_unix_socket(struct fd *sock) {
    if (sock == NULL || sock->socket.domain != AF_LOCAL_)
        return false;
    size_t name_len = sock->socket.unix_name_len;
    if (name_len == 0)
        return false;
    const char *name = sock->socket.unix_name;
    if (name[0] == '\0') {
        name++;
        name_len--;
    }
    static const char x11_prefix[] = "/tmp/.X11-unix/X";
    return name_len >= sizeof(x11_prefix) - 1 &&
        memcmp(name, x11_prefix, sizeof(x11_prefix) - 1) == 0;
}

static void sock_x11_event(const char *op, struct fd *sock, ssize_t result, int err, size_t requested) {
    if (!sock_is_x11_unix_socket(sock))
        return;
    size_t name_len = sock->socket.unix_name_len;
    const char *name = sock->socket.unix_name;
    if (name_len != 0 && name[0] == '\0') {
        name++;
        name_len--;
    }
    if (name_len > 107)
        name_len = 107;
    printk("INFO: x11sock %s pid=%d comm=%s real=%d requested=%zu result=%zd err=%d flags=%#x peer_pending=%d name=%.*s\n",
           op, current ? current->pid : -1, current ? current->comm : "?",
           sock->real_fd, requested, result, err, sock->flags,
           (int) sock->socket.unix_peer_pending, (int) name_len, name);
}

static size_t sock_iov_requested(const struct iovec *iov, size_t iovlen) {
    size_t total = 0;
    for (size_t i = 0; i < iovlen; i++)
        total += iov[i].iov_len;
    return total;
}

static bool sock_is_devlog_sink(const struct fd *sock) {
    return sock != NULL && sock->socket.domain == AF_LOCAL_ &&
        sock->socket.unix_devlog_sink;
}

static bool sock_is_initctl_sink(const struct fd *sock) {
    return sock != NULL && sock->socket.domain == AF_LOCAL_ &&
        sock->socket.unix_initctl_sink;
}

static int sock_ifreq_name_from_index(struct ifreq_ *ifreq) {
    unsigned ifindex = (unsigned) ifreq->ifr_ifru.ifindex;
    if (ifindex == 0)
        return _ENODEV;
    struct if_nameindex *list = if_nameindex();
    if (list == NULL)
        return _EIO;
    int err = _ENODEV;
    for (struct if_nameindex *entry = list; entry->if_index != 0 || entry->if_name != NULL; entry++) {
        if (entry->if_index != ifindex || entry->if_name == NULL)
            continue;
        memset(ifreq->ifr_name, 0, sizeof(ifreq->ifr_name));
        strncpy(ifreq->ifr_name, entry->if_name, sizeof(ifreq->ifr_name) - 1);
        err = 0;
        break;
    }
    if_freenameindex(list);
    return err;
}

static int sock_ifreq_index_from_name(struct ifreq_ *ifreq) {
    if (ifreq->ifr_name[0] == '\0')
        return _ENODEV;
    unsigned ifindex = if_nametoindex(ifreq->ifr_name);
    if (ifindex == 0)
        return _ENODEV;
    ifreq->ifr_ifru.ifindex = (int32_t) ifindex;
    return 0;
}

static int sock_ifreq_flags_from_name(struct ifreq_ *ifreq) {
    if (ifreq->ifr_name[0] == '\0')
        return _ENODEV;
    struct ifaddrs *addrs = NULL;
    if (getifaddrs(&addrs) != 0)
        return _EIO;
    int err = _ENODEV;
    for (const struct ifaddrs *cursor = addrs; cursor != NULL; cursor = cursor->ifa_next) {
        if (cursor->ifa_name == NULL)
            continue;
        if (strncmp(cursor->ifa_name, ifreq->ifr_name, sizeof(ifreq->ifr_name)) != 0)
            continue;
        ifreq->ifr_ifru.flags = (int16_t) netlink_linux_if_flags(cursor->ifa_flags);
        err = 0;
        break;
    }
    freeifaddrs(addrs);
    return err;
}

static int sock_ifreq_txqlen_from_name(struct ifreq_ *ifreq) {
    if (ifreq->ifr_name[0] == '\0')
        return _ENODEV;
    if (if_nametoindex(ifreq->ifr_name) == 0)
        return _ENODEV;
    // iSH does not model per-interface queueing. BusyBox's `ip addr` always
    // probes SIOCGIFTXQLEN rather than reading the IFLA_TXQLEN we already put
    // in the RTM_NEWLINK dump, so without this it spams "ioctl 0x8942 failed:
    // Not a tty" (ENOTTY from the realfs passthrough). Report the conventional
    // Linux default, matching netlink_fill_link_info()'s txqlen.
    ifreq->ifr_ifru.qlen = 1000;
    return 0;
}

static int sock_ifreq_mtu_from_name(struct ifreq_ *ifreq) {
    if (ifreq->ifr_name[0] == '\0')
        return _ENODEV;
    struct ifaddrs *addrs = NULL;
    if (getifaddrs(&addrs) != 0)
        return _EIO;
    int err = _ENODEV;
    int32_t mtu = 1500;
    for (const struct ifaddrs *cursor = addrs; cursor != NULL; cursor = cursor->ifa_next) {
        if (cursor->ifa_name == NULL)
            continue;
        if (strncmp(cursor->ifa_name, ifreq->ifr_name, sizeof(ifreq->ifr_name)) != 0)
            continue;
        err = 0;
#if defined(__APPLE__)
        if (cursor->ifa_data != NULL) {
            const struct if_data *stats = (const struct if_data *) cursor->ifa_data;
            if (stats->ifi_mtu != 0)
                mtu = (int32_t) stats->ifi_mtu;
        }
#endif
        break;
    }
    freeifaddrs(addrs);
    if (err < 0)
        return err;
    ifreq->ifr_ifru.mtu = mtu;
    return 0;
}

static int sock_ifreq_metric_from_name(struct ifreq_ *ifreq) {
    if (ifreq->ifr_name[0] == '\0')
        return _ENODEV;
    if (if_nametoindex(ifreq->ifr_name) == 0)
        return _ENODEV;
    // iSH does not model per-route metrics; 0 is the Linux default net-tools/
    // busybox ifconfig display when a device has none configured.
    ifreq->ifr_ifru.metric = 0;
    return 0;
}

static int sock_ifreq_hwaddr_from_name(struct ifreq_ *ifreq) {
    if (ifreq->ifr_name[0] == '\0')
        return _ENODEV;
    struct ifaddrs *addrs = NULL;
    if (getifaddrs(&addrs) != 0)
        return _EIO;
    int err = _ENODEV;
    bool is_loopback = false;
    for (const struct ifaddrs *cursor = addrs; cursor != NULL; cursor = cursor->ifa_next) {
        if (cursor->ifa_name == NULL)
            continue;
        if (strncmp(cursor->ifa_name, ifreq->ifr_name, sizeof(ifreq->ifr_name)) != 0)
            continue;
        err = 0;
        if (cursor->ifa_flags & IFF_LOOPBACK)
            is_loopback = true;
    }
    if (err < 0) {
        freeifaddrs(addrs);
        return err;
    }

    memset(&ifreq->ifr_ifru.addr, 0, sizeof(ifreq->ifr_ifru.addr));
    ifreq->ifr_ifru.addr.family = is_loopback ? ARPHRD_LOOPBACK_ : ARPHRD_ETHER_;
#if defined(__APPLE__)
    // A loopback interface has no link-layer address; only look for one on
    // AF_LINK entries here, mirroring netlink_fill_link_info()'s IFLA_ADDRESS.
    for (const struct ifaddrs *cursor = addrs; cursor != NULL; cursor = cursor->ifa_next) {
        if (cursor->ifa_name == NULL || cursor->ifa_addr == NULL)
            continue;
        if (strcmp(cursor->ifa_name, ifreq->ifr_name) != 0)
            continue;
        if (cursor->ifa_addr->sa_family != AF_LINK)
            continue;
        const struct sockaddr_dl *sdl = (const struct sockaddr_dl *) cursor->ifa_addr;
        if (sdl->sdl_alen == 0 || sdl->sdl_alen > sizeof(ifreq->ifr_ifru.addr.data))
            continue;
        memcpy(ifreq->ifr_ifru.addr.data, LLADDR(sdl), sdl->sdl_alen);
        break;
    }
#endif
    freeifaddrs(addrs);
    return 0;
}

enum sock_ifreq_addr_field_ {
    SOCK_IFREQ_ADDR_,
    SOCK_IFREQ_DSTADDR_,
    SOCK_IFREQ_BRDADDR_,
    SOCK_IFREQ_NETMASK_,
};

// SIOCGIFADDR/DSTADDR/BRDADDR/NETMASK all read a struct sockaddr_in out of the
// same ifr_ifru.addr slot; only which host-side address they report differs.
// A device that exists but has no IPv4 configured (in_dev is NULL on real
// Linux) fails with EADDRNOTAVAIL rather than ENODEV; a device with IPv4 but
// not point-to-point/broadcast reports 0.0.0.0 for dst/brd, matching Linux.
static int sock_ifreq_addr_field_from_name(struct ifreq_ *ifreq, enum sock_ifreq_addr_field_ field) {
    if (ifreq->ifr_name[0] == '\0')
        return _ENODEV;
    struct ifaddrs *addrs = NULL;
    if (getifaddrs(&addrs) != 0)
        return _EIO;
    bool device_found = false;
    bool inet_found = false;
    uint32_t result = 0;
    for (const struct ifaddrs *cursor = addrs; cursor != NULL; cursor = cursor->ifa_next) {
        if (cursor->ifa_name == NULL)
            continue;
        if (strncmp(cursor->ifa_name, ifreq->ifr_name, sizeof(ifreq->ifr_name)) != 0)
            continue;
        device_found = true;
        if (cursor->ifa_addr == NULL || cursor->ifa_addr->sa_family != AF_INET)
            continue;
        inet_found = true;
        const struct sockaddr *want = NULL;
        switch (field) {
            case SOCK_IFREQ_ADDR_:
                want = cursor->ifa_addr;
                break;
            case SOCK_IFREQ_DSTADDR_:
                if ((cursor->ifa_flags & IFF_POINTOPOINT) && cursor->ifa_dstaddr != NULL)
                    want = cursor->ifa_dstaddr;
                break;
            case SOCK_IFREQ_BRDADDR_:
                if ((cursor->ifa_flags & IFF_BROADCAST) && cursor->ifa_broadaddr != NULL)
                    want = cursor->ifa_broadaddr;
                break;
            case SOCK_IFREQ_NETMASK_:
                want = cursor->ifa_netmask;
                break;
        }
        if (want != NULL && want->sa_family == AF_INET)
            result = ((const struct sockaddr_in *) want)->sin_addr.s_addr;
        break;
    }
    freeifaddrs(addrs);
    if (!device_found)
        return _ENODEV;
    if (!inet_found)
        return _EADDRNOTAVAIL;

    struct sockaddr_in_ *sin = (struct sockaddr_in_ *) &ifreq->ifr_ifru.addr;
    memset(sin, 0, sizeof(*sin));
    sin->sin_family = AF_INET_;
    sin->sin_addr = result;
    return 0;
}

static int sock_ifconf(void *arg) {
    bool is_64bit = task_is_64bit(current);
    struct guest_ifconf32_ *ifconf32 = arg;
    struct guest_ifconf64_ *ifconf64 = arg;
    int32_t guest_len = is_64bit ? ifconf64->guest_len : ifconf32->guest_len;
    guest_addr_t guest_buf = is_64bit ? ifconf64->guest_buf : ifconf32->guest_buf;

    if (guest_len < 0)
        return _EINVAL;

    struct ifaddrs *addrs = NULL;
    if (getifaddrs(&addrs) != 0)
        return _EIO;

    size_t capacity = (size_t) guest_len;
    size_t used = 0;
    size_t total = 0;
    size_t entry_size = is_64bit ? sizeof(struct guest_ifreq_addr64_) : sizeof(struct guest_ifreq_addr32_);
    int err = 0;

    for (const struct ifaddrs *cursor = addrs; cursor != NULL; cursor = cursor->ifa_next) {
        if (cursor->ifa_name == NULL || cursor->ifa_addr == NULL)
            continue;
        if (cursor->ifa_addr->sa_family != AF_INET)
            continue;

        struct guest_ifreq_addr64_ entry = {};
        strncpy(entry.ifr_name, cursor->ifa_name, sizeof(entry.ifr_name) - 1);
        entry.guest_addr.sin_family = AF_INET_;
        entry.guest_addr.sin_port = 0;
        entry.guest_addr.sin_addr = ((const struct sockaddr_in *) cursor->ifa_addr)->sin_addr.s_addr;

        total += entry_size;
        if (guest_buf == 0 || used + entry_size > capacity)
            continue;
        if (user_write(guest_buf + used, &entry, entry_size)) {
            err = _EFAULT;
            break;
        }
        used += entry_size;
    }

    freeifaddrs(addrs);
    if (err < 0)
        return err;

    int32_t result_len = (size_t) guest_len >= total ? (int32_t) total : (int32_t) used;
    if (is_64bit)
        ifconf64->guest_len = result_len;
    else
        ifconf32->guest_len = result_len;
    return 0;
}

static bool guest_sockaddr_is_devlog(guest_addr_t sockaddr_addr, uint_t sockaddr_len) {
    if (sockaddr_addr == 0)
        return false;
    if (sockaddr_len < offsetof(struct sockaddr_, data))
        return false;
    if (sockaddr_len > sizeof(struct sockaddr_max_))
        return false;

    struct sockaddr_max_ fake_addr = {};
    if (user_read(sockaddr_addr, &fake_addr, sockaddr_len))
        return false;
    if (fake_addr.family != AF_LOCAL_)
        return false;

    size_t path_size = sockaddr_len - offsetof(struct sockaddr_, data);
    if (path_size == 0 || fake_addr.data[0] == '\0')
        return false;

    static const char devlog_path[] = "/dev/log";
    size_t guest_len = strnlen(fake_addr.data, path_size);
    return guest_len == strlen(devlog_path) &&
        memcmp(fake_addr.data, devlog_path, strlen(devlog_path)) == 0;
}

static bool guest_sockaddr_is_initctl(guest_addr_t sockaddr_addr, uint_t sockaddr_len) {
    if (sockaddr_addr == 0)
        return false;
    if (sockaddr_len < offsetof(struct sockaddr_, data))
        return false;
    if (sockaddr_len > sizeof(struct sockaddr_max_))
        return false;

    struct sockaddr_max_ fake_addr = {};
    if (user_read(sockaddr_addr, &fake_addr, sockaddr_len))
        return false;
    if (fake_addr.family != AF_LOCAL_)
        return false;

    size_t path_size = sockaddr_len - offsetof(struct sockaddr_, data);
    if (path_size == 0 || fake_addr.data[0] == '\0')
        return false;

    static const char run_initctl_path[] = "/run/initctl";
    static const char dev_initctl_path[] = "/dev/initctl";
    size_t guest_len = strnlen(fake_addr.data, path_size);
    return (guest_len == strlen(run_initctl_path) &&
            memcmp(fake_addr.data, run_initctl_path, strlen(run_initctl_path)) == 0) ||
        (guest_len == strlen(dev_initctl_path) &&
            memcmp(fake_addr.data, dev_initctl_path, strlen(dev_initctl_path)) == 0);
}

static bool guest_sockaddr_is_abstract_local(guest_addr_t sockaddr_addr, uint_t sockaddr_len) {
    if (sockaddr_addr == 0)
        return false;
    if (sockaddr_len < offsetof(struct sockaddr_, data))
        return false;
    if (sockaddr_len > sizeof(struct sockaddr_max_))
        return false;

    struct sockaddr_max_ fake_addr = {};
    if (user_read(sockaddr_addr, &fake_addr, sockaddr_len))
        return false;
    if (fake_addr.family != AF_LOCAL_)
        return false;

    size_t path_size = sockaddr_len - offsetof(struct sockaddr_, data);
    return path_size != 0 && fake_addr.data[0] == '\0';
}

#if defined(__APPLE__)
static void sock_trace_tcp_info(const char *label, struct fd *sock) {
    if (!sock_trace_enabled() || sock == NULL || sock->real_fd < 0)
        return;
    if (sock->socket.type != SOCK_STREAM_)
        return;
    if (sock->socket.domain != AF_INET_ && sock->socket.domain != AF_INET6_)
        return;

    struct tcp_connection_info info = {};
    socklen_t info_len = sizeof(info);
    if (getsockopt(sock->real_fd, IPPROTO_TCP, TCP_CONNECTION_INFO, &info, &info_len) < 0)
        return;

    int so_error = 0;
    socklen_t so_error_len = sizeof(so_error);
    if (getsockopt(sock->real_fd, SOL_SOCKET, SO_ERROR, &so_error, &so_error_len) < 0)
        so_error = -1;
    // This read-and-clears the host's SO_ERROR same as socket_tcp_connect_
    // write_ready; cache it too so an strace-enabled run doesn't itself steal
    // the one authoritative read a guest getsockopt(SO_ERROR) needs.
    else if (so_error != 0 && sock->socket.host_connect_error == 0)
        sock->socket.host_connect_error = so_error;

    printk("INFO: net tcp %s pid=%d comm=%s real=%d state=%u options=%#x flags=%#x so_error=%d snd_sbbytes=%u snd_cwnd=%u snd_wnd=%u rcv_wnd=%u rtt=%u srtt=%u txbytes=%llu rxbytes=%llu retrans=%llu\n",
           label, current->pid, current->comm, sock->real_fd,
           info.tcpi_state, info.tcpi_options, info.tcpi_flags, so_error,
           info.tcpi_snd_sbbytes, info.tcpi_snd_cwnd,
           info.tcpi_snd_wnd, info.tcpi_rcv_wnd,
           info.tcpi_rttcur, info.tcpi_srtt,
           (unsigned long long) info.tcpi_txbytes,
           (unsigned long long) info.tcpi_rxbytes,
           (unsigned long long) info.tcpi_txretransmitbytes);
}
#endif

static bool socket_guest_signal_pending(void) {
    lock(&current->sighand->lock, 0);
    bool signal_pending = !!((current->pending | current->sighand->pending) & ~task_wake_blocked(current));
    unlock(&current->sighand->lock);
    return signal_pending;
}

static bool socket_should_retry_io_eintr(struct fd *sock, int real_flags) {
    if (errno != EINTR)
        return false;
    if (fd_getflags(sock) & O_NONBLOCK_)
        return false;
#ifdef MSG_DONTWAIT
    if (real_flags & MSG_DONTWAIT)
        return false;
#endif
    return !socket_guest_signal_pending();
}

static bool socket_call_is_blocking(struct fd *sock, int real_flags) {
    if (fd_getflags(sock) & O_NONBLOCK_)
        return false;
#ifdef MSG_DONTWAIT
    if (real_flags & MSG_DONTWAIT)
        return false;
#endif
    return true;
}

static bool socket_should_retry_io_eagain(struct fd *sock, int real_flags) {
    if (errno != EAGAIN && errno != EWOULDBLOCK)
        return false;
    return socket_call_is_blocking(sock, real_flags);
}

static bool socket_should_map_unix_eperm_to_eagain(struct fd *sock, int real_flags) {
    if (errno != EPERM)
        return false;
    if (sock->socket.domain != AF_LOCAL_)
        return false;
    return !socket_call_is_blocking(sock, real_flags);
}

// Must be a macro, not a function: it plants the sigsetjmp unwind point (via
// sigunwind_start) that sigusr1_handler siglongjmps back to while the CALLER
// is blocked in the host syscall. As a function, that jump targeted this
// helper's dead stack frame -- it "worked" often enough to look fine, but the
// local oldmask it then restored with SIG_SETMASK had been clobbered by the
// blocking call's frames, so the thread's host signal mask became garbage.
// When the garbage included SIGUSR1, the thread went permanently deaf to
// task_poke, and its next blocking socket call hung unkillably (stress-ng's
// network sweep wedged in recvfrom with SIGALRM+SIGCHLD pending and unblocked).
// Same rule as sigunwind_start itself: see util/sync.h.
//
// The explicit SIG_UNBLOCK before entering the syscall mirrors fs/poll.c:
// guest sigprocmask only changes the guest-level mask, so make sure the host
// mask can never silently suppress the poke.
#define socket_blocking_syscall_begin(oldmask_p) ({ \
    bool __sbsb_ok; \
    sigset_t __sbsb_sigusr1; \
    sigemptyset(&__sbsb_sigusr1); \
    sigaddset(&__sbsb_sigusr1, SIGUSR1); \
    pthread_sigmask(SIG_BLOCK, &__sbsb_sigusr1, (oldmask_p)); \
    if (sigunwind_start()) { \
        pthread_sigmask(SIG_SETMASK, (oldmask_p), NULL); \
        errno = EINTR; \
        __sbsb_ok = false; \
    } else if (socket_guest_signal_pending()) { \
        sigunwind_end(); \
        pthread_sigmask(SIG_SETMASK, (oldmask_p), NULL); \
        errno = EINTR; \
        __sbsb_ok = false; \
    } else { \
        pthread_sigmask(SIG_SETMASK, (oldmask_p), NULL); \
        pthread_sigmask(SIG_UNBLOCK, &__sbsb_sigusr1, NULL); \
        __sbsb_ok = true; \
    } \
    __sbsb_ok; \
})

static void socket_blocking_syscall_end(void) {
    sigunwind_end();
}

// Keep the host description nonblocking so no guest task can ever park inside
// a host recvmsg/sendmsg/read/write, where nothing can reach it: a SIGUSR1
// poke aimed at a thread already blocked in a host wait is simply never
// delivered (see the long comment in socket_wait_ready). Every blocking-mode
// socket call therefore runs the host call nonblocking and does its waiting in
// socket_wait_ready(), which sleeps in a poll() that a signal can tear apart.
//
// The flag is a property of the open file description, so this deliberately
// diverges from the guest's O_NONBLOCK; fd->flags carries the guest's own and
// sock_getflags() reports that one, so the divergence is invisible. Same
// treatment sys_accept4_common gives listening sockets. Cached because this
// sits on the I/O fast path: the cache is cleared by sock_setflags() and by
// sys_connect_common()'s host-flag restore, the two places that write the host
// flags behind our back, so the next call re-forces.
static void socket_force_host_nonblock(struct fd *sock) {
    if (sock->socket.host_nonblock || sock->real_fd < 0)
        return;
    int host_fl = fcntl(sock->real_fd, F_GETFL, 0);
    if (host_fl < 0)
        return;
    if (!(host_fl & O_NONBLOCK) &&
            fcntl(sock->real_fd, F_SETFL, host_fl | O_NONBLOCK) < 0)
        return;
    sock->socket.host_nonblock = true;
}

// SO_RCVTIMEO/SO_SNDTIMEO used to be enforced by the host kernel, because the
// host call was the blocking one. Now that the wait is emulated, so is the
// timeout. Linux's is a budget for time spent *waiting* within one syscall
// rather than a per-wait limit, so the caller owns this state and passes the
// same one to every wait the call makes: a MSG_WAITALL recv that wakes ten
// times still gets a single deadline. Resolved lazily on the first wait, which
// keeps the getsockopt off the path of a call that never blocks.
struct socket_io_wait {
    bool resolved;
    bool has_deadline;
    struct timespec deadline;
};

static void socket_io_wait_resolve(struct socket_io_wait *wait, struct fd *sock, short events) {
    wait->resolved = true;
    wait->has_deadline = false;
    if (sock->real_fd < 0)
        return;
    struct timeval timeo = {0, 0};
    socklen_t timeo_len = sizeof(timeo);
    if (getsockopt(sock->real_fd, SOL_SOCKET,
                   (events & POLLOUT) ? SO_SNDTIMEO : SO_RCVTIMEO,
                   &timeo, &timeo_len) < 0)
        return;
    if (timeo.tv_sec == 0 && timeo.tv_usec == 0)
        return;
    wait->deadline = timespec_now(CLOCK_MONOTONIC);
    wait->deadline.tv_sec += timeo.tv_sec;
    wait->deadline.tv_nsec += (long) timeo.tv_usec * 1000;
    if (wait->deadline.tv_nsec >= 1000000000L) {
        wait->deadline.tv_sec++;
        wait->deadline.tv_nsec -= 1000000000L;
    }
    wait->has_deadline = true;
}

// The one place a guest socket call sleeps. `wait` carries the SO_RCVTIMEO/
// SO_SNDTIMEO budget for the whole syscall; pass NULL from iSH's own internal
// handshakes, which must not inherit a guest's timeout.
//
// A SIGUSR1 poke alone is NOT enough to wake this sleep. SIGUSR1 does not
// queue and doubles as the TLB/quiesce shootdown poke, and a poke aimed at a
// thread already blocked inside a host wait is simply never delivered:
// measured with N tasks parked in recv(), all SIGKILLed, every pthread_kill
// returned 0 to a distinct, correct, started thread, each parked with SIGUSR1
// unblocked and its sigunwind point armed, yet only the first one or two ever
// ran the handler and the rest slept on with SIGKILL pending -- permanently
// deaf, since further kills only re-sent the same lost poke. Parallelism is
// the trigger: one task at a time always wakes, so a sequential test passes
// vacuously. fs/poll.c defends against this with the non-lossy notify pipe
// published below, and fs/real.c with a 100ms timeout; a periodic wakeup is
// the wrong trade here, since a blocked recv() on an idle connection is the
// common case and should cost nothing until something happens.
static int socket_wait_ready(struct fd *sock, short events, struct socket_io_wait *wait) {
    if (wait != NULL && !wait->resolved)
        socket_io_wait_resolve(wait, sock, events);

    // Created lazily, and only here: a call that never has to sleep must not
    // pay for a pipe it will never wait on.
    int notify_pipe[2] = {-1, -1};
    int err = 0;
    for (;;) {
        int timeout = -1;
        if (wait != NULL && wait->has_deadline) {
            struct timespec now = timespec_now(CLOCK_MONOTONIC);
            long long remaining_ms =
                (long long) (wait->deadline.tv_sec - now.tv_sec) * 1000 +
                (wait->deadline.tv_nsec - now.tv_nsec) / 1000000;
            if (remaining_ms <= 0) {
                err = _EAGAIN; // timeout expired, matching Linux
                break;
            }
            timeout = remaining_ms > INT_MAX ? INT_MAX : (int) remaining_ms;
        }
        if (notify_pipe[0] < 0 && pipe(notify_pipe) == 0) {
            fcntl(notify_pipe[0], F_SETFL, O_NONBLOCK);
            fcntl(notify_pipe[1], F_SETFL, O_NONBLOCK);
            // Same lock deliver_signal_unlocked_locked reads the fd under, so
            // it can never observe a closed or reused one (cleared below
            // before close).
            lock(&current->sighand->lock, 0);
            current->poll_notify_fd = notify_pipe[1];
            unlock(&current->sighand->lock);
        }
        struct pollfd pfd[2];
        int npfd = 1;
        pfd[0] = (struct pollfd) {
            .fd = sock->real_fd,
            .events = events | POLLERR | POLLHUP,
        };
        if (notify_pipe[0] >= 0) {
            pfd[1] = (struct pollfd) {.fd = notify_pipe[0], .events = POLLIN};
            npfd = 2;
        }
        sigset_t oldmask;
        int wait_res, wait_errno;
        if (!socket_blocking_syscall_begin(&oldmask)) {
            wait_res = -1;
            wait_errno = EINTR;
        } else {
            errno = 0;
            wait_res = poll(pfd, npfd, timeout);
            wait_errno = errno;
            socket_blocking_syscall_end();
        }
        // Drain the notify pipe so one poke does not leave every later poll
        // instantly readable and spin this loop. The wake needs no
        // interpretation of its own: what reports the signal is the pending
        // check below, and what reports readiness is the socket's revents.
        if (npfd == 2 && wait_res > 0 && (pfd[1].revents & POLLIN)) {
            char drain[64];
            while (read(notify_pipe[0], drain, sizeof drain) > 0)
                continue;
        }
        if (wait_res > 0 && pfd[0].revents != 0)
            break; // ready (or error/hangup): the caller retries the I/O
        if (wait_res == 0) {
            if (wait != NULL && wait->has_deadline) {
                err = _EAGAIN;
                break;
            }
            continue; // no deadline was armed, so this cannot happen; re-poll
        }
        if (wait_res < 0 && wait_errno != EINTR) {
            errno = wait_errno;
            err = errno_map();
            break;
        }
        // EINTR, or woken by the notify pipe alone. A real pending guest
        // signal MUST surface as EINTR so the guest can run its handler; a
        // purely spurious poke (a TLB-shootdown SIGUSR1, or a notify for a
        // signal this task has blocked) just re-enters the wait.
        if (socket_guest_signal_pending()) {
            // SA_RESTART: the blocking socket calls are restartable, EXCEPT
            // when SO_RCVTIMEO/SO_SNDTIMEO is armed -- signal(7) puts a
            // timed socket wait in the never-restarted list, because a
            // restart would silently extend a timeout the guest asked for.
            // A NULL `wait` is one of iSH's own internal handshakes, which
            // has no guest syscall to restart.
            err = (wait != NULL && !wait->has_deadline)
                    ? signal_restart_or_eintr(_EINTR) : _EINTR;
            break;
        }
    }
    // Unpublish before closing so a concurrent deliver_signal (which reads the
    // fd under sighand->lock) can never poke a closed or reused fd. Done
    // before any errno the caller cares about is restored: lock() clobbers it.
    if (notify_pipe[0] >= 0) {
        lock(&current->sighand->lock, 0);
        current->poll_notify_fd = -1;
        unlock(&current->sighand->lock);
        close(notify_pipe[0]);
        close(notify_pipe[1]);
    }
    // Every caller's error path distinguishes an already-mapped negative code
    // from a raw host failure by testing errno == 0, so leave it that way: the
    // codes returned here are mapped (or synthesized, for a deadline that
    // expired), and the poll/drain/close/lock above all clobber errno.
    errno = 0;
    return err;
}

// Advance a scratch copy of an iovec array past `consumed` bytes. The copy is
// mandatory: free_msghdr_iov() frees each iov_base, and an advanced base is an
// interior pointer.
static void socket_iov_advance(struct iovec *iov, int *iovlen, size_t consumed) {
    int i = 0;
    while (consumed > 0 && i < *iovlen) {
        size_t chunk = iov[i].iov_len < consumed ? iov[i].iov_len : consumed;
        iov[i].iov_base = (char *) iov[i].iov_base + chunk;
        iov[i].iov_len -= chunk;
        consumed -= chunk;
        if (iov[i].iov_len == 0)
            i++;
    }
    *iovlen -= i;
    if (i > 0 && *iovlen > 0)
        memmove(iov, iov + i, (size_t) *iovlen * sizeof(*iov));
}

// Only a byte stream can deliver half a request. Datagram and seqpacket sends
// are all-or-nothing and MSG_WAITALL is a no-op on them, so they only ever
// retry on EAGAIN.
static bool socket_is_stream(struct fd *sock) {
    return sock->socket.type == SOCK_STREAM_;
}

#if defined(__APPLE__)
static bool socket_tcp_connect_established(struct fd *sock) {
    if (sock == NULL)
        return true;
    if (sock->socket.type != SOCK_STREAM_)
        return true;
    if (sock->socket.domain != AF_INET_ && sock->socket.domain != AF_INET6_)
        return true;

    struct sockaddr_storage peer = {};
    socklen_t peer_len = sizeof(peer);
    if (getpeername(sock->real_fd, (struct sockaddr *) &peer, &peer_len) == 0)
        return true;

    struct tcp_connection_info info = {};
    socklen_t info_len = sizeof(info);
    if (getsockopt(sock->real_fd, IPPROTO_TCP, TCP_CONNECTION_INFO, &info, &info_len) < 0)
        return true;
    return info.tcpi_state >= DARWIN_TCPS_ESTABLISHED;
}

static bool socket_tcp_connect_write_ready(struct fd *sock) {
    if (sock == NULL)
        return true;
    if (sock->socket.type != SOCK_STREAM_)
        return true;
    if (sock->socket.domain != AF_INET_ && sock->socket.domain != AF_INET6_)
        return true;

    int so_error = 0;
    socklen_t so_error_len = sizeof(so_error);
    if (getsockopt(sock->real_fd, SOL_SOCKET, SO_ERROR, &so_error, &so_error_len) < 0)
        return true;
    if (so_error != 0) {
        // This read just cleared the host's SO_ERROR (read-and-clear
        // semantics); stash it so a later guest getsockopt(SO_ERROR) still
        // sees it instead of a silently-reset 0. See the fd.h comment on
        // host_connect_error.
        if (sock->socket.host_connect_error == 0)
            sock->socket.host_connect_error = so_error;
        return true;
    }

    return socket_tcp_connect_established(sock);
}
#endif

static int socket_finish_blocking_connect(struct fd *sock) {
    struct pollfd pfd = {
        .fd = sock->real_fd,
        .events = POLLOUT,
    };
    for (;;) {
        errno = 0;
        int wait_res = poll(&pfd, 1, -1);
        if (wait_res < 0) {
            if (errno == EINTR && !socket_guest_signal_pending())
                continue;
            return errno_map();
        }
        if (wait_res == 0)
            continue;

        int real_error = 0;
        socklen_t real_error_len = sizeof(real_error);
        if (getsockopt(sock->real_fd, SOL_SOCKET, SO_ERROR, &real_error, &real_error_len) < 0)
            return errno_map();
#if defined(__APPLE__)
        // A concurrent poll/epoll scan on another thread (socket_tcp_connect_
        // write_ready, called from every sock_poll) may have already read and
        // cleared SO_ERROR before this getsockopt() got here.
        if (real_error == 0 && sock->socket.host_connect_error != 0)
            real_error = sock->socket.host_connect_error;
#endif
        if (real_error != 0) {
            sock->socket.host_connect_error = 0;
            return err_map(real_error);
        }
#if defined(__APPLE__)
        if (!socket_tcp_connect_established(sock))
            continue;
#endif
        return 0;
    }
}

static void sock_trace(const char *op, struct fd *sock, ssize_t result, int err_code) {
    if (!sock_trace_enabled())
        return;
    printk("INFO: net %s pid=%d comm=%s guest_domain=%d guest_type=%d protocol=%d real=%d result=%zd err=%d\n",
           op, current->pid, current->comm, sock->socket.domain, sock->socket.type,
           sock->socket.protocol, sock->real_fd, result, err_code);
}

static void sock_trace_sockaddr(const char *label, int real_fd) {
    if (!sock_trace_enabled() || real_fd < 0)
        return;

    struct sockaddr_storage addr = {};
    socklen_t len = sizeof(addr);
    int rc = strcmp(label, "peer") == 0
        ? getpeername(real_fd, (struct sockaddr *) &addr, &len)
        : getsockname(real_fd, (struct sockaddr *) &addr, &len);
    if (rc < 0) {
        printk("INFO: net %s real=%d unavailable errno=%d\n", label, real_fd, errno);
        return;
    }

    char host[INET6_ADDRSTRLEN];
    host[0] = '\0';
    unsigned port = 0;
    switch (addr.ss_family) {
        case AF_INET: {
            struct sockaddr_in *addr4 = (struct sockaddr_in *) &addr;
            if (inet_ntop(AF_INET, &addr4->sin_addr, host, sizeof(host)) == NULL)
                snprintf(host, sizeof(host), "<inet4-err>");
            port = ntohs(addr4->sin_port);
            break;
        }
        case AF_INET6: {
            struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *) &addr;
            if (inet_ntop(AF_INET6, &addr6->sin6_addr, host, sizeof(host)) == NULL)
                snprintf(host, sizeof(host), "<inet6-err>");
            port = ntohs(addr6->sin6_port);
            break;
        }
        default:
            snprintf(host, sizeof(host), "<family-%d>", addr.ss_family);
            break;
    }

    printk("INFO: net %s real=%d family=%d host=%s port=%u len=%u\n",
           label, real_fd, addr.ss_family, host, port, (unsigned) len);
}

static void sock_trace_write_preview(struct fd *sock, const void *buf, size_t size) {
    if (!sock_trace_enabled() || sock == NULL || sock->real_fd < 0)
        return;
    if (sock->socket.type != SOCK_STREAM_)
        return;

    size_t limit = size;
    if (limit > 160)
        limit = 160;

    char preview[limit * 2 + 1];
    size_t out = 0;
    const unsigned char *bytes = buf;
    for (size_t i = 0; i < limit && out + 2 < sizeof(preview); i++) {
        unsigned char c = bytes[i];
        if (c == '\r') {
            preview[out++] = '\\';
            preview[out++] = 'r';
        } else if (c == '\n') {
            preview[out++] = '\\';
            preview[out++] = 'n';
        } else if (c >= 32 && c <= 126) {
            preview[out++] = (char) c;
        } else {
            preview[out++] = '.';
        }
    }
    preview[out] = '\0';
    printk("INFO: net write-preview real=%d size=%zu text=\"%s\"\n",
           sock->real_fd, size, preview);
}

static void sock_trace_iov_preview(struct fd *sock, const struct iovec *iov, size_t iovlen) {
    if (!sock_trace_enabled() || sock == NULL || sock->real_fd < 0)
        return;
    if (sock->socket.type != SOCK_STREAM_)
        return;

    size_t total = 0;
    for (size_t i = 0; i < iovlen; i++)
        total += iov[i].iov_len;

    char preview[321];
    size_t out = 0;
    for (size_t i = 0; i < iovlen && out + 2 < sizeof(preview); i++) {
        const unsigned char *bytes = iov[i].iov_base;
        for (size_t j = 0; j < iov[i].iov_len && out + 2 < sizeof(preview); j++) {
            unsigned char c = bytes[j];
            if (c == '\r') {
                preview[out++] = '\\';
                preview[out++] = 'r';
            } else if (c == '\n') {
                preview[out++] = '\\';
                preview[out++] = 'n';
            } else if (c >= 32 && c <= 126) {
                preview[out++] = (char) c;
            } else {
                preview[out++] = '.';
            }
            if (out >= 160)
                break;
        }
        if (out >= 160)
            break;
    }
    preview[out] = '\0';
    printk("INFO: net write-preview real=%d size=%zu text=\"%s\"\n",
           sock->real_fd, total, preview);
}

static int unix_socket_finish_peer(struct fd *sock);

static fd_t sock_fd_create(int sock_fd, int domain, int type, int protocol) {
    struct fd *fd = adhoc_fd_create(&socket_fdops);
    if (fd == NULL)
        return _ENOMEM;
    fd->stat.mode = S_IFSOCK | 0666;
    fd->real_fd = sock_fd;
    fd->socket.domain = domain;
    fd->socket.type = type & SOCKET_TYPE_MASK;
    fd->socket.protocol = protocol;
    sock_init_emulation_defaults(fd);
    if (domain == AF_LOCAL_) {
        cond_init(&fd->socket.unix_got_peer);
        list_init(&fd->socket.unix_scm);
    }
    sock_debug_event("fd-create", fd, 0, 0);
    return f_install(fd, type & ~SOCKET_TYPE_MASK);
}

// Test hook: pretend the host denied AF_UNIX SOCK_SEQPACKET the way the iOS
// app sandbox does (EPERM), so the STREAM-fallback path can be exercised on
// the CLI where the native type otherwise succeeds. Set
// ISH_FORCE_SEQPACKET_EPERM=1 in the environment to enable.
static bool seqpacket_denied_by_host(int domain, int type, int protocol) {
    static int forced = -1;
    if (forced < 0)
        forced = getenv("ISH_FORCE_SEQPACKET_EPERM") != NULL;
    return forced && domain == AF_LOCAL_ &&
        (type & SOCKET_TYPE_MASK) == SOCK_SEQPACKET_ && protocol == 0;
}

static bool unix_seqpacket_fallback_needed(int domain, int type, int protocol, int err) {
    if (domain != AF_LOCAL_)
        return false;
    if ((type & SOCKET_TYPE_MASK) != SOCK_SEQPACKET_)
        return false;
    if (protocol != 0)
        return false;
    switch (err) {
        case EPROTONOSUPPORT:
        case EPROTOTYPE:
        case ESOCKTNOSUPPORT:
        case EOPNOTSUPP:
        // iOS's app sandbox (unlike an unsandboxed macOS process) denies
        // AF_UNIX SOCK_SEQPACKET creation outright with EPERM, even though
        // the Darwin kernel itself supports the socket type -- seen as
        // systemd-udevd fatally failing to create /run/udev/control on
        // device ("Failed to create socket: Operation not permitted"),
        // never hitting this fallback since EPERM wasn't in the allowlist.
        case EPERM:
            return true;
        default:
            return false;
    }
}

int_t sys_socket(dword_t domain, dword_t type, dword_t protocol) {
    STRACE("socket(%d, %d, %d)", domain, type, protocol);
    if (domain == AF_NETLINK_) {
        int socket_type = type & SOCKET_TYPE_MASK;
        if (protocol != NETLINK_ROUTE_ &&
                protocol != NETLINK_SOCK_DIAG_ &&
                protocol != NETLINK_KOBJECT_UEVENT_ &&
                protocol != NETLINK_AUDIT_ &&
                protocol != NETLINK_GENERIC_)
            return _EPROTONOSUPPORT;
        if (socket_type != SOCK_RAW_ && socket_type != SOCK_DGRAM_)
            return _EINVAL;
        struct fd *fd = adhoc_fd_create(&socket_fdops);
        if (fd == NULL)
            return _ENOMEM;
        fd->stat.mode = S_IFSOCK | 0666;
        fd->real_fd = -1;
        fd->socket.domain = domain;
        fd->socket.type = socket_type;
        fd->socket.protocol = protocol;
        sock_init_emulation_defaults(fd);
        fd->socket.netlink_port_id = netlink_next_port_id();
        fd->fake_inode = fd->socket.netlink_port_id;
        netlink_notify_register(fd);
        return f_install(fd, type & ~SOCKET_TYPE_MASK);
    }
    int real_domain = sock_family_to_real(domain);
    if (real_domain < 0)
        return _EINVAL;
    int real_type = sock_type_to_real(type, protocol);
    if (real_type < 0)
        return _EINVAL;

    // this hack makes mtr work
    if (type == SOCK_RAW_ && protocol == IPPROTO_RAW)
        protocol = IPPROTO_ICMP;

    int sock;
    if (seqpacket_denied_by_host(domain, type, protocol)) {
        sock = -1;
        errno = EPERM;
    } else {
        sock = socket(real_domain, real_type, protocol);
    }
#if defined(__APPLE__)
    if (sock < 0 && unix_seqpacket_fallback_needed(domain, type, protocol, errno))
        sock = socket(real_domain, SOCK_STREAM, protocol);
#endif
    if (sock < 0)
        return errno_map();
    if (sock_debug_comm(current != NULL ? current->comm : NULL))
        fprintf(stderr, "ish-sock:socket-host pid=%d comm=%s domain=%d type=%d protocol=%d real=%d\n",
                current != NULL ? current->pid : -1,
                current != NULL ? current->comm : "?",
                domain, type, protocol, sock);

#ifdef __APPLE__
    if (domain == AF_INET_ && type == SOCK_DGRAM_) {
        // in some cases, such as ICMP, datagram sockets on mac can default to
        // including the IP header like raw sockets
        int one = 1;
        setsockopt(sock, IPPROTO_IP, IP_STRIPHDR, &one, sizeof(one));
    }
#endif

    fd_t f = sock_fd_create(sock, domain, type, protocol);
    if (f < 0)
        close(sock);
    return f;
}

static void inode_release_if_exist(struct inode_data *inode) {
    if (inode != NULL)
        inode_release(inode);
}

// On failure returns NULL and sets *err to _EBADF (fd doesn't exist) or
// _ENOTSOCK (fd exists but isn't a socket), matching Linux's distinction.
static struct fd *sock_getfd(fd_t sock_fd, int_t *err) {
    struct fd *sock = f_get(sock_fd);
    if (sock == NULL) {
        *err = _EBADF;
        return NULL;
    }
    if (sock->ops != &socket_fdops) {
        *err = _ENOTSOCK;
        return NULL;
    }
    return sock;
}

static uint32_t netlink_next_port_id(void) {
    static uint32_t next_port_id = 0x10000;
    static lock_t next_port_id_lock = LOCK_INITIALIZER;
    lock(&next_port_id_lock, 0);
    uint32_t port_id = ++next_port_id;
    unlock(&next_port_id_lock);
    return port_id;
}

// netlink_reply* is read/written by the owning guest thread's own sendmsg/
// recvmsg/poll calls, and (once a socket is subscribed to a multicast group)
// also by the background link-change notifier thread appending spontaneous
// messages -- see netlink_notify_link_change. sock->socket.netlink_reply_lock
// guards all of it; the _locked variants below assume the caller already
// holds it, the public ones take/release it themselves. Never call a public
// variant while already holding the lock (plain non-recursive mutex).

static void netlink_reply_reset_locked(struct fd *sock) {
    free(sock->socket.netlink_reply);
    sock->socket.netlink_reply = NULL;
    sock->socket.netlink_reply_len = 0;
    sock->socket.netlink_reply_off = 0;
}

static void netlink_reply_reset(struct fd *sock) {
    lock(&sock->socket.netlink_reply_lock, 0);
    netlink_reply_reset_locked(sock);
    unlock(&sock->socket.netlink_reply_lock);
}

static int netlink_reply_append_locked(struct fd *sock, const void *data, size_t len) {
    size_t old_len = sock->socket.netlink_reply_len;
    char *new_reply = realloc(sock->socket.netlink_reply, old_len + len);
    if (new_reply == NULL)
        return _ENOMEM;
    memcpy(new_reply + old_len, data, len);
    sock->socket.netlink_reply = new_reply;
    sock->socket.netlink_reply_len = old_len + len;
    return 0;
}

// Appends one complete nlmsg (header+payload+padding) atomically under the
// lock, so a background notification can never land its header, get
// interleaved with a guest thread's own append, and corrupt message framing
// -- a fully self-contained nlmsg record is the smallest unit that's safe to
// interleave with others in the reply stream.
static int netlink_append_nlmsg(struct fd *sock, uint16_t type, uint16_t flags,
        uint32_t seq, const void *payload, size_t payload_len) {
    struct nlmsghdr_ hdr = {
        .nlmsg_len = NLMSG_HDRLEN + payload_len,
        .nlmsg_type = type,
        .nlmsg_flags = flags,
        .nlmsg_seq = seq,
        // Linux route netlink replies are tagged with the destination socket's
        // port ID, and glibc getifaddrs() filters on this value.
        .nlmsg_pid = sock->socket.netlink_port_id,
    };
    lock(&sock->socket.netlink_reply_lock, 0);
    int err = netlink_reply_append_locked(sock, &hdr, sizeof(hdr));
    if (err < 0)
        goto out;
    if (payload_len != 0) {
        err = netlink_reply_append_locked(sock, payload, payload_len);
        if (err < 0)
            goto out;
    }
    size_t aligned_len = NLMSG_ALIGN(hdr.nlmsg_len);
    size_t pad_len = aligned_len - hdr.nlmsg_len;
    if (pad_len != 0) {
        static const char zeros[NLMSG_ALIGNTO] = {};
        err = netlink_reply_append_locked(sock, zeros, pad_len);
    }
out:
    unlock(&sock->socket.netlink_reply_lock);
    return err;
}

static int netlink_append_error(struct fd *sock, uint32_t seq,
        const struct nlmsghdr_ *req, int err_code) {
    struct nlmsgerr_ err = {
        .error = err_code,
        .msg = req ? *req : (struct nlmsghdr_) {},
    };
    return netlink_append_nlmsg(sock, NLMSG_ERROR_, 0, seq, &err, sizeof(err));
}

static int netlink_append_done(struct fd *sock, uint32_t seq) {
    int32_t status = 0;
    // The DONE terminating a dump MUST carry NLM_F_MULTI, like every other
    // part: sd-netlink only recognizes end-of-dump inside its
    // `nlmsg_flags & NLM_F_MULTI` branch (netlink-socket.c), so a bare DONE
    // left the accumulated parts in rqueue_partial_by_serial forever and
    // every sd-netlink link/addr enumeration silently returned NOTHING.
    // That is why systemd-resolved held zero Link objects and answered every
    // query with io.systemd.Resolve.NetworkDown (killing ALL hostname
    // resolution via nsswitch's "resolve [!UNAVAIL=return]"), and why
    // networkctl printed "0 links listed" -- while busybox ip, which doesn't
    // care about the DONE flags, parsed the same bytes fine.
    return netlink_append_nlmsg(sock, NLMSG_DONE_, NLM_F_MULTI_, seq, &status, sizeof(status));
}

static int diag_socket_push(struct diag_socket_entry *entries, struct fd *fd) {
    for (unsigned i = 0; i < entries->count; i++) {
        if (entries->fds[i] == fd)
            return 0;
    }
    if (entries->count == entries->cap) {
        unsigned new_cap = entries->cap ? entries->cap * 2 : 16;
        struct fd **new_fds = realloc(entries->fds, sizeof(*new_fds) * new_cap);
        if (new_fds == NULL)
            return _ENOMEM;
        entries->fds = new_fds;
        entries->cap = new_cap;
    }
    entries->fds[entries->count++] = fd_retain(fd);
    return 0;
}

static struct fdtable *diag_task_files_retain(struct task *task) {
    struct fdtable *files = NULL;
    lock(&task->general_lock, 0);
    if (task->files != NULL)
        files = fdtable_retain(task->files);
    unlock(&task->general_lock);
    return files;
}

static void diag_socket_release(struct diag_socket_entry *entries) {
    for (unsigned i = 0; i < entries->count; i++)
        fd_close(entries->fds[i]);
    free(entries->fds);
}

static int diag_collect_sockets(struct diag_socket_entry *entries, int domain, int type) {
    struct task_snapshot snapshot = {};
    int err = task_snapshot_collect(&snapshot, false);
    if (err < 0)
        return err;

    for (unsigned i = 0; i < snapshot.count; i++) {
        struct task *task = snapshot.tasks[i];
        if (task == NULL)
            continue;
        struct fdtable *files = diag_task_files_retain(task);
        if (files == NULL)
            continue;
        lock(&files->lock, 0);
        for (fd_t fd_no = 0; (unsigned) fd_no < files->size; fd_no++) {
            struct fd *fd = fdtable_get(files, fd_no);
            if (fd == NULL || fd->ops != &socket_fdops)
                continue;
            if (fd->socket.domain != domain)
                continue;
            if (type >= 0 && fd->socket.type != type)
                continue;
            if (domain != AF_LOCAL_ && fd->real_fd < 0)
                continue;
            err = diag_socket_push(entries, fd);
            if (err < 0)
                break;
        }
        unlock(&files->lock);
        fdtable_release(files);
        if (err < 0)
            break;
    }
    task_snapshot_release(&snapshot);
    return err;
}

static unsigned long diag_socket_inode(const struct fd *fd) {
    if (fd->inode != NULL)
        return (unsigned long) fd->inode;
    if (fd->fake_inode != 0)
        return (unsigned long) fd->fake_inode;
    return (unsigned long) (uintptr_t) fd;
}

static int diag_recv_q(struct fd *fd) {
    int bytes = 0;
    if (fd->real_fd >= 0 && ioctl(fd->real_fd, FIONREAD, &bytes) == 0 && bytes > 0)
        return bytes;
    return 0;
}

struct inet_bind_info {
    sa_family_t family;
    uint16_t port;
    bool wildcard;
    uint32_t scope_id;
    union {
        struct in_addr v4;
        struct in6_addr v6;
    } addr;
};

static bool inet_bind_info_from_sockaddr(const struct sockaddr *sa, struct inet_bind_info *info) {
    if (sa == NULL || info == NULL)
        return false;
    memset(info, 0, sizeof(*info));
    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *sin = (const struct sockaddr_in *) sa;
        info->family = AF_INET;
        info->port = sin->sin_port;
        info->addr.v4 = sin->sin_addr;
        info->wildcard = sin->sin_addr.s_addr == htonl(INADDR_ANY);
        return info->port != 0;
    }
    if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *) sa;
        info->family = AF_INET6;
        info->port = sin6->sin6_port;
        info->addr.v6 = sin6->sin6_addr;
        info->scope_id = sin6->sin6_scope_id;
        info->wildcard = IN6_IS_ADDR_UNSPECIFIED(&sin6->sin6_addr);
        return info->port != 0;
    }
    return false;
}

static bool inet_bind_addr_overlaps(const struct inet_bind_info *a, const struct inet_bind_info *b) {
    if (a->family != b->family || a->port != b->port)
        return false;
    if (a->wildcard || b->wildcard)
        return true;
    if (a->family == AF_INET)
        return a->addr.v4.s_addr == b->addr.v4.s_addr;
    if (a->family == AF_INET6)
        return memcmp(&a->addr.v6, &b->addr.v6, sizeof(a->addr.v6)) == 0 &&
            a->scope_id == b->scope_id;
    return false;
}

static bool sock_bound_inet_conflicts(struct fd *sock, const struct inet_bind_info *candidate) {
    struct task_snapshot snapshot = {};
    bool conflict = false;
    if (task_snapshot_collect(&snapshot, false) < 0)
        return false;

    for (unsigned i = 0; i < snapshot.count && !conflict; i++) {
        struct task *task = snapshot.tasks[i];
        if (task == NULL)
            continue;
        struct fdtable *files = diag_task_files_retain(task);
        if (files == NULL)
            continue;
        lock(&files->lock, 0);
        for (fd_t fd_no = 0; (unsigned) fd_no < files->size; fd_no++) {
            struct fd *other = fdtable_get(files, fd_no);
            if (other == NULL || other == sock || other->ops != &socket_fdops)
                continue;
            if (other->socket.domain != sock->socket.domain ||
                    other->socket.type != sock->socket.type ||
                    other->real_fd < 0) {
                continue;
            }
            // Only an actual listener occupies the bind slot for a given
            // address:port. An established/accepted connection's local
            // address happens to match its listener's (getsockname() on an
            // accepted socket returns the same local port), so without this
            // check any live connection -- e.g. the very ssh session used to
            // restart sshd -- would falsely block a fresh bind() on that port.
            if (!other->socket.listening)
                continue;
            struct sockaddr_storage other_addr = {};
            socklen_t other_addr_len = sizeof(other_addr);
            if (getsockname(other->real_fd, (struct sockaddr *) &other_addr, &other_addr_len) < 0)
                continue;
            struct inet_bind_info other_info = {};
            if (!inet_bind_info_from_sockaddr((const struct sockaddr *) &other_addr, &other_info))
                continue;
            if (!inet_bind_addr_overlaps(candidate, &other_info))
                continue;
            // Linux has required identical socket euid across an
            // SO_REUSEPORT group since v3.9, precisely so an unprivileged user
            // cannot join a root daemon's port and take its connections. We
            // checked only that both had the flag set.
            if (sock->socket.reuseport && other->socket.reuseport &&
                    sock->socket.bind_euid == other->socket.bind_euid)
                continue;
            conflict = true;
            break;
        }
        unlock(&files->lock);
        fdtable_release(files);
    }
    task_snapshot_release(&snapshot);
    return conflict;
}

static int diag_tcp_state(struct fd *fd) {
#if defined(__APPLE__)
    struct tcp_connection_info conn_info;
    socklen_t conn_info_size = sizeof(conn_info);
    if (getsockopt(fd->real_fd, IPPROTO_TCP, TCP_CONNECTION_INFO, &conn_info, &conn_info_size) == 0) {
        static const uint8_t tcp_state_table[] = {
            7, 10, 2, 3, 1, 8, 4, 11, 9, 5, 6,
        };
        if (conn_info.tcpi_state < sizeof(tcp_state_table))
            return tcp_state_table[conn_info.tcpi_state];
    }
#endif
    int acceptconn = 0;
    socklen_t len = sizeof(acceptconn);
    if (getsockopt(fd->real_fd, SOL_SOCKET, SO_ACCEPTCONN, &acceptconn, &len) == 0 && acceptconn)
        return 10;

    struct sockaddr_storage peer;
    len = sizeof(peer);
    if (getpeername(fd->real_fd, (struct sockaddr *) &peer, &len) == 0)
        return 1;
    return 7;
}

static int diag_unix_state(struct fd *fd) {
    int acceptconn = 0;
    socklen_t len = sizeof(acceptconn);
    if (fd->real_fd >= 0 &&
            getsockopt(fd->real_fd, SOL_SOCKET, SO_ACCEPTCONN, &acceptconn, &len) == 0 &&
            acceptconn)
        return 10;
    if (fd->socket.unix_peer != NULL)
        return 1;
    return 7;
}

static int diag_copy_to_iov(struct iovec *iov, size_t iovlen, size_t offset,
        const void *src, size_t len) {
    const char *src_bytes = src;
    for (size_t i = 0; i < iovlen && len != 0; i++) {
        if (offset >= iov[i].iov_len) {
            offset -= iov[i].iov_len;
            continue;
        }
        size_t chunk = iov[i].iov_len - offset;
        if (chunk > len)
            chunk = len;
        memcpy((char *) iov[i].iov_base + offset, src_bytes, chunk);
        src_bytes += chunk;
        len -= chunk;
        offset = 0;
    }
    return len == 0 ? 0 : _EINVAL;
}

static size_t diag_iov_capacity(const struct iovec *iov, size_t iovlen) {
    size_t len = 0;
    for (size_t i = 0; i < iovlen; i++)
        len += iov[i].iov_len;
    return len;
}

static uint32_t unix_socket_next_id(void) {
    static uint32_t next_id = 0;
    static lock_t next_id_lock = LOCK_INITIALIZER;
    lock(&next_id_lock, 0);
    uint32_t id = ++next_id;
    unlock(&next_id_lock);
    return id;
}

static int netlink_append_inet_diag(struct fd *sock, const struct nlmsghdr_ *req_hdr,
        const struct inet_diag_req_v2_ *req) {
    int type = -1;
    if (req->sdiag_protocol == IPPROTO_TCP)
        type = SOCK_STREAM_;
    else if (req->sdiag_protocol == IPPROTO_UDP)
        type = SOCK_DGRAM_;
    else
        return netlink_append_error(sock, req_hdr->nlmsg_seq, req_hdr, _EOPNOTSUPP);

    struct diag_socket_entry entries = {};
    int err = diag_collect_sockets(&entries, req->sdiag_family, type);
    if (err < 0)
        return err;

    for (unsigned i = 0; i < entries.count; i++) {
        struct fd *fd = entries.fds[i];
        struct sockaddr_storage local = {};
        struct sockaddr_storage peer = {};
        socklen_t local_len = sizeof(local);
        socklen_t peer_len = sizeof(peer);
        if (getsockname(fd->real_fd, (struct sockaddr *) &local, &local_len) < 0)
            continue;

        bool has_peer = getpeername(fd->real_fd, (struct sockaddr *) &peer, &peer_len) == 0;
        int state = type == SOCK_STREAM_ ? diag_tcp_state(fd) : (has_peer ? 1 : 7);
        if (req->idiag_states != 0 && !(req->idiag_states & (1u << state)))
            continue;

        struct inet_diag_msg_ msg = {};
        msg.idiag_family = req->sdiag_family;
        msg.idiag_state = state;
        msg.idiag_uid = current->euid;
        msg.idiag_inode = diag_socket_inode(fd);
        msg.idiag_rqueue = diag_recv_q(fd);
        msg.id.idiag_cookie[0] = 0xffffffffu;
        msg.id.idiag_cookie[1] = 0xffffffffu;

        if (req->sdiag_family == AF_INET_ && local.ss_family == AF_INET) {
            struct sockaddr_in *local4 = (struct sockaddr_in *) &local;
            msg.id.idiag_sport = local4->sin_port;
            msg.id.idiag_src[0] = local4->sin_addr.s_addr;
            if (has_peer && peer.ss_family == AF_INET) {
                struct sockaddr_in *peer4 = (struct sockaddr_in *) &peer;
                msg.id.idiag_dport = peer4->sin_port;
                msg.id.idiag_dst[0] = peer4->sin_addr.s_addr;
            }
        } else if (req->sdiag_family == AF_INET6_ && local.ss_family == AF_INET6) {
            struct sockaddr_in6 *local6 = (struct sockaddr_in6 *) &local;
            memcpy(msg.id.idiag_src, &local6->sin6_addr, sizeof(local6->sin6_addr));
            msg.id.idiag_sport = local6->sin6_port;
            if (has_peer && peer.ss_family == AF_INET6) {
                struct sockaddr_in6 *peer6 = (struct sockaddr_in6 *) &peer;
                memcpy(msg.id.idiag_dst, &peer6->sin6_addr, sizeof(peer6->sin6_addr));
                msg.id.idiag_dport = peer6->sin6_port;
            }
        } else {
            continue;
        }

        err = netlink_append_nlmsg(sock, SOCK_DIAG_BY_FAMILY_, NLM_F_MULTI_,
                req_hdr->nlmsg_seq, &msg, sizeof(msg));
        if (err < 0)
            break;
    }

    if (err >= 0)
        err = netlink_append_done(sock, req_hdr->nlmsg_seq);
    diag_socket_release(&entries);
    return err;
}

static int netlink_append_unix_diag(struct fd *sock, const struct nlmsghdr_ *req_hdr,
        const struct unix_diag_req_ *req) {
    struct diag_socket_entry entries = {};
    int err = diag_collect_sockets(&entries, AF_LOCAL_, -1);
    if (err < 0)
        return err;

    for (unsigned i = 0; i < entries.count; i++) {
        struct fd *fd = entries.fds[i];
        struct unix_diag_msg_ msg = {};
        msg.udiag_family = AF_LOCAL_;
        msg.udiag_type = fd->socket.type;
        msg.udiag_state = diag_unix_state(fd);
        msg.udiag_ino = diag_socket_inode(fd);
        msg.udiag_cookie[0] = 0xffffffffu;
        msg.udiag_cookie[1] = 0xffffffffu;
        if (req->udiag_states != 0 && !(req->udiag_states & (1u << msg.udiag_state)))
            continue;

        err = netlink_append_nlmsg(sock, SOCK_DIAG_BY_FAMILY_, NLM_F_MULTI_,
                req_hdr->nlmsg_seq, &msg, sizeof(msg));
        if (err < 0)
            break;
    }

    if (err >= 0)
        err = netlink_append_done(sock, req_hdr->nlmsg_seq);
    diag_socket_release(&entries);
    return err;
}

static int netlink_handle_diag_request(struct fd *sock, const struct nlmsghdr_ *hdr,
        const void *payload, size_t payload_len) {
    if (hdr->nlmsg_type != SOCK_DIAG_BY_FAMILY_)
        return netlink_append_error(sock, hdr->nlmsg_seq, hdr, _EOPNOTSUPP);

    if (payload_len < 1)
        return netlink_append_error(sock, hdr->nlmsg_seq, hdr, _EINVAL);

    uint8_t family = *(const uint8_t *) payload;
    if (family == AF_INET_ || family == AF_INET6_) {
        if (payload_len < sizeof(struct inet_diag_req_v2_))
            return netlink_append_error(sock, hdr->nlmsg_seq, hdr, _EINVAL);
        return netlink_append_inet_diag(sock, hdr, payload);
    }
    if (family == AF_LOCAL_) {
        if (payload_len < sizeof(struct unix_diag_req_))
            return netlink_append_error(sock, hdr->nlmsg_seq, hdr, _EINVAL);
        return netlink_append_unix_diag(sock, hdr, payload);
    }
    return netlink_append_error(sock, hdr->nlmsg_seq, hdr, _EOPNOTSUPP);
}

// Find a top-level netlink attribute by type (NLA_F_NESTED/NET_BYTEORDER
// flags masked off, like the kernel's nla_find).
static const struct nlattr_ *netlink_attr_find(const void *attrs, size_t len, uint16_t type) {
    size_t off = 0;
    while (off + sizeof(struct nlattr_) <= len) {
        const struct nlattr_ *attr = (const struct nlattr_ *) ((const char *) attrs + off);
        if (attr->nla_len < sizeof(*attr) || off + attr->nla_len > len)
            break;
        if ((attr->nla_type & 0x3fff) == type)
            return attr;
        off += NLA_ALIGN(attr->nla_len);
    }
    return NULL;
}

// Compare a string attribute's payload against str, tolerating a missing
// terminating NUL (like the kernel's nla_strcmp).
static bool netlink_attr_streq(const struct nlattr_ *attr, const char *str) {
    size_t data_len = attr->nla_len - sizeof(*attr);
    const char *data = (const char *) (attr + 1);
    size_t str_len = strlen(str);
    if (data_len > 0 && data[data_len - 1] == '\0')
        data_len--;
    return data_len == str_len && memcmp(data, str, str_len) == 0;
}

static void netlink_taskstats_from_io(struct taskstats_ *ts, struct task_io_counters *io) {
    ts->read_char = atomic_load_explicit(&io->rchar, memory_order_relaxed);
    ts->write_char = atomic_load_explicit(&io->wchar, memory_order_relaxed);
    ts->read_syscalls = atomic_load_explicit(&io->syscr, memory_order_relaxed);
    ts->write_syscalls = atomic_load_explicit(&io->syscw, memory_order_relaxed);
    ts->read_bytes = atomic_load_explicit(&io->read_bytes, memory_order_relaxed);
    ts->write_bytes = atomic_load_explicit(&io->write_bytes, memory_order_relaxed);
    ts->cancelled_write_bytes = atomic_load_explicit(&io->cancelled_write_bytes, memory_order_relaxed);
    ts->blkio_count = atomic_load_explicit(&io->blkio_count, memory_order_relaxed);
    ts->blkio_delay_total = atomic_load_explicit(&io->blkio_delay_ns, memory_order_relaxed);
}

static int netlink_taskstats_fill(pid_t_ id, bool tgid, struct taskstats_ *ts) {
    memset(ts, 0, sizeof(*ts));
    complex_lockt(&pids_lock, 0);
    struct task *task = pid_get_task(id);
    if (task == NULL) {
        unlock(&pids_lock);
        return _ESRCH;
    }
    struct task_io_counters io = {};
    if (tgid) {
        struct tgroup *group = task->group;
        task_io_counters_add(&io, &group->io_dead);
        struct task *thread;
        list_for_each_entry(&group->threads, thread, group_links)
            task_io_counters_add(&io, &thread->io);
        ts->ac_pid = group->leader->pid;
    } else {
        task_io_counters_add(&io, &task->io);
        ts->ac_pid = task->pid;
    }
    ts->version = TASKSTATS_VERSION_;
    // Parent PROCESS: Linux's bacct_add_tsk uses task_tgid_nr_ns(real_parent).
    // See sys_getppid() for why ->pid is wrong here.
    ts->ac_ppid = task->parent != NULL ? task->parent->tgid : 0;
    ts->ac_uid = task->uid;
    ts->ac_gid = task->gid;
    // comm changes only at exec/prctl and is fixed-size; an unlocked copy can
    // at worst be torn between two names, never overrun (ac_comm is larger
    // and pre-zeroed).
    memcpy(ts->ac_comm, task->comm, sizeof(task->comm));
    unlock(&pids_lock);
    netlink_taskstats_from_io(ts, &io);
    return 0;
}

static int netlink_append_taskstats(struct fd *sock, const struct nlmsghdr_ *hdr,
        bool tgid, uint32_t id, const struct taskstats_ *ts) {
    char payload[GENL_HDRLEN_ + NLA_ALIGN(sizeof(struct nlattr_)) * 3 + 8 + sizeof(*ts) + 16] = {};
    struct genlmsghdr_ *genl = (struct genlmsghdr_ *) payload;
    genl->cmd = TASKSTATS_CMD_NEW_;
    genl->version = TASKSTATS_GENL_VERSION_;
    size_t len = GENL_HDRLEN_;

    // Nested TASKSTATS_TYPE_AGGR_{PID,TGID} containing {PID/TGID, STATS}.
    // No NLA_F_NESTED flag: the kernel builds this with nla_nest_start_noflag
    // and iotop switches on the raw nla_type.
    struct nlattr_ *aggr = (struct nlattr_ *) (payload + len);
    size_t aggr_start = len;
    len += sizeof(*aggr);
    int err = netlink_append_attr_raw(payload, sizeof(payload), &len,
            tgid ? TASKSTATS_TYPE_TGID_ : TASKSTATS_TYPE_PID_, &id, sizeof(id));
    if (err < 0)
        return err;
    err = netlink_append_attr_raw(payload, sizeof(payload), &len,
            TASKSTATS_TYPE_STATS_, ts, sizeof(*ts));
    if (err < 0)
        return err;
    aggr->nla_type = tgid ? TASKSTATS_TYPE_AGGR_TGID_ : TASKSTATS_TYPE_AGGR_PID_;
    aggr->nla_len = (uint16_t) (len - aggr_start);
    return netlink_append_nlmsg(sock, GENL_FAMILY_TASKSTATS_, 0, hdr->nlmsg_seq, payload, len);
}

static int netlink_handle_generic_request(struct fd *sock, const struct nlmsghdr_ *hdr,
        const void *payload, size_t payload_len) {
    if (payload_len < sizeof(struct genlmsghdr_))
        return netlink_append_error(sock, hdr->nlmsg_seq, hdr, _EINVAL);
    const struct genlmsghdr_ *genl = (const struct genlmsghdr_ *) payload;
    const void *attrs = (const char *) payload + GENL_HDRLEN_;
    size_t attrs_len = payload_len - GENL_HDRLEN_;

    if (hdr->nlmsg_type == GENL_ID_CTRL_) {
        if (genl->cmd != CTRL_CMD_GETFAMILY_)
            return netlink_append_error(sock, hdr->nlmsg_seq, hdr, _EOPNOTSUPP);
        const struct nlattr_ *name = netlink_attr_find(attrs, attrs_len, CTRL_ATTR_FAMILY_NAME_);
        if (name == NULL)
            return netlink_append_error(sock, hdr->nlmsg_seq, hdr, _EINVAL);
        // Family names are capped at GENL_NAMSIZ (16); real kernels fail
        // policy validation with EINVAL before the lookup even happens.
        if (name->nla_len - sizeof(*name) > 16)
            return netlink_append_error(sock, hdr->nlmsg_seq, hdr, _EINVAL);
        // TASKSTATS is the only family we implement; anything else resolves
        // like an unregistered family so callers fall back cleanly.
        if (!netlink_attr_streq(name, TASKSTATS_GENL_NAME_))
            return netlink_append_error(sock, hdr->nlmsg_seq, hdr, _ENOENT);

        char reply[GENL_HDRLEN_ + 5 * 8 + 16] = {};
        struct genlmsghdr_ *rgenl = (struct genlmsghdr_ *) reply;
        rgenl->cmd = CTRL_CMD_NEWFAMILY_;
        rgenl->version = 2;
        size_t len = GENL_HDRLEN_;
        uint16_t family_id = GENL_FAMILY_TASKSTATS_;
        uint32_t version = TASKSTATS_GENL_VERSION_;
        uint32_t hdrsize = 0;
        uint32_t maxattr = TASKSTATS_CMD_ATTR_TGID_;
        // Attribute order matters and must match the kernel's ctrl_fill_info
        // (NAME first, then ID): iotop doesn't scan attrs by type, it grabs
        // the second attribute and expects it to be CTRL_ATTR_FAMILY_ID.
        int err = netlink_append_attr_raw(reply, sizeof(reply), &len,
                CTRL_ATTR_FAMILY_NAME_, TASKSTATS_GENL_NAME_, sizeof(TASKSTATS_GENL_NAME_));
        if (err >= 0)
            err = netlink_append_attr_raw(reply, sizeof(reply), &len,
                    CTRL_ATTR_FAMILY_ID_, &family_id, sizeof(family_id));
        if (err >= 0)
            err = netlink_append_attr_raw(reply, sizeof(reply), &len,
                    CTRL_ATTR_VERSION_, &version, sizeof(version));
        if (err >= 0)
            err = netlink_append_attr_raw(reply, sizeof(reply), &len,
                    CTRL_ATTR_HDRSIZE_, &hdrsize, sizeof(hdrsize));
        if (err >= 0)
            err = netlink_append_attr_raw(reply, sizeof(reply), &len,
                    CTRL_ATTR_MAXATTR_, &maxattr, sizeof(maxattr));
        if (err < 0)
            return err;
        return netlink_append_nlmsg(sock, GENL_ID_CTRL_, 0, hdr->nlmsg_seq, reply, len);
    }

    if (hdr->nlmsg_type == GENL_FAMILY_TASKSTATS_) {
        // The kernel registers taskstats ops with GENL_ADMIN_PERM
        // (CAP_NET_ADMIN) since CVE-2011-2494; genl_rcv_msg fails them with
        // EPERM for unprivileged callers.
        if (current->euid != 0)
            return netlink_append_error(sock, hdr->nlmsg_seq, hdr, _EPERM);
        if (genl->cmd != TASKSTATS_CMD_GET_)
            return netlink_append_error(sock, hdr->nlmsg_seq, hdr, _EOPNOTSUPP);
        bool tgid = false;
        const struct nlattr_ *id_attr = netlink_attr_find(attrs, attrs_len, TASKSTATS_CMD_ATTR_PID_);
        if (id_attr == NULL) {
            id_attr = netlink_attr_find(attrs, attrs_len, TASKSTATS_CMD_ATTR_TGID_);
            tgid = true;
        }
        if (id_attr == NULL || id_attr->nla_len < sizeof(*id_attr) + sizeof(uint32_t))
            return netlink_append_error(sock, hdr->nlmsg_seq, hdr, _EINVAL);
        uint32_t id;
        memcpy(&id, id_attr + 1, sizeof(id));
        struct taskstats_ ts;
        int err = netlink_taskstats_fill((pid_t_) id, tgid, &ts);
        if (err < 0)
            return netlink_append_error(sock, hdr->nlmsg_seq, hdr, err);
        return netlink_append_taskstats(sock, hdr, tgid, id, &ts);
    }

    // Unknown family id: same as sending to an unregistered genl family.
    return netlink_append_error(sock, hdr->nlmsg_seq, hdr, _ENOENT);
}

// Kernel-global audit status, like the real audit subsystem's. Starts (and
// effectively stays) disabled: AUDIT_SET is accepted and echoed back by
// AUDIT_GET so an auditd that registers itself sees coherent state, but no
// records are ever generated or delivered -- exactly how a CONFIG_AUDIT=y
// kernel behaves before/without an enabled audit daemon.
static struct audit_status_ audit_state;
static pthread_mutex_t audit_state_lock = PTHREAD_MUTEX_INITIALIZER;

// Minimal NETLINK_AUDIT, mirroring kernel audit_receive_msg with auditing
// off. The consumers that matter are libaudit clients (PAM, login/su,
// shadow, sshd, dbus): audit_open() must succeed, and audit_send() --
// which sets NLM_F_ACK and blocks in check_ack() -- must get a real ACK,
// with user messages (AUDIT_USER*) silently accepted and dropped the same
// way Linux drops them when audit_enabled is off.
static int netlink_handle_audit_request(struct fd *sock, const struct nlmsghdr_ *hdr,
        const void *payload, size_t payload_len) {
    uint16_t type = hdr->nlmsg_type;
    // netlink_rcv_skb skips control messages (< NLMSG_MIN_TYPE) silently.
    if (type < NLMSG_MIN_TYPE_)
        return 0;

    bool user_msg = type == AUDIT_USER_ ||
        (type >= AUDIT_FIRST_USER_MSG_ && type <= AUDIT_LAST_USER_MSG_) ||
        (type >= AUDIT_FIRST_USER_MSG2_ && type <= AUDIT_LAST_USER_MSG2_);
    switch (type) {
        case AUDIT_LIST_:
        case AUDIT_ADD_:
        case AUDIT_DEL_:
            // Removed legacy binary rule API; kernel audit_netlink_ok says
            // EOPNOTSUPP before even checking permissions.
            return netlink_append_error(sock, hdr->nlmsg_seq, hdr, _EOPNOTSUPP);
        case AUDIT_GET_:
        case AUDIT_SET_:
        case AUDIT_GET_FEATURE_:
        case AUDIT_SET_FEATURE_:
        case AUDIT_ADD_RULE_:
        case AUDIT_DEL_RULE_:
        case AUDIT_LIST_RULES_:
        case AUDIT_TRIM_:
        case AUDIT_MAKE_EQUIV_:
        case AUDIT_TTY_GET_:
        case AUDIT_TTY_SET_:
        case AUDIT_SIGNAL_INFO_:
            break;
        default:
            if (!user_msg)
                // audit_netlink_ok's default: bad message type.
                return netlink_append_error(sock, hdr->nlmsg_seq, hdr, _EINVAL);
            break;
    }
    // Control needs CAP_AUDIT_CONTROL, user messages CAP_AUDIT_WRITE;
    // approximate both as euid==0 (same policy as the taskstats handler).
    if (current->euid != 0)
        return netlink_append_error(sock, hdr->nlmsg_seq, hdr, _EPERM);

    int err = 0;
    if (type == AUDIT_GET_) {
        pthread_mutex_lock(&audit_state_lock);
        struct audit_status_ s = audit_state;
        pthread_mutex_unlock(&audit_state_lock);
        s.mask = 0;
        err = netlink_append_nlmsg(sock, AUDIT_GET_, 0, hdr->nlmsg_seq, &s, sizeof(s));
    } else if (type == AUDIT_SET_) {
        // Kernel copies min(sizeof, data_len) and applies mask-gated fields.
        struct audit_status_ s = {};
        memcpy(&s, payload, payload_len < sizeof(s) ? payload_len : sizeof(s));
        pthread_mutex_lock(&audit_state_lock);
        if (s.mask & AUDIT_STATUS_ENABLED_)
            audit_state.enabled = s.enabled;
        if (s.mask & AUDIT_STATUS_FAILURE_)
            audit_state.failure = s.failure;
        if (s.mask & AUDIT_STATUS_PID_)
            audit_state.pid = s.pid;
        if (s.mask & AUDIT_STATUS_RATE_LIMIT_)
            audit_state.rate_limit = s.rate_limit;
        if (s.mask & AUDIT_STATUS_BACKLOG_LIMIT_)
            audit_state.backlog_limit = s.backlog_limit;
        pthread_mutex_unlock(&audit_state_lock);
    } else if (type == AUDIT_GET_FEATURE_) {
        struct audit_features_ f = { .vers = 1 };
        err = netlink_append_nlmsg(sock, AUDIT_GET_FEATURE_, 0, hdr->nlmsg_seq, &f, sizeof(f));
    } else if (user_msg) {
        // Accepted and dropped: Linux does the same for user messages when
        // audit_enabled is off (audit_receive_msg returns 0 without logging).
    } else {
        // Remaining control ops (rules, tty, signal-info, ...) belong to a
        // rule engine we don't have; fail them the way an old kernel fails
        // ops it doesn't implement rather than pretending they worked.
        return netlink_append_error(sock, hdr->nlmsg_seq, hdr, _EOPNOTSUPP);
    }
    if (err < 0)
        return err;
    if (hdr->nlmsg_flags & NLM_F_ACK_)
        return netlink_append_error(sock, hdr->nlmsg_seq, hdr, 0);
    return 0;
}

static int netlink_handle_sendmsg(struct fd *sock, const struct msghdr *msg) {
    int iovlen = msg->msg_iovlen;
    if (iovlen < 0)
        return _EINVAL;
    size_t req_len = 0;
    for (int i = 0; i < iovlen; i++)
        req_len += msg->msg_iov[i].iov_len;
    char *req = malloc(req_len);
    if (req == NULL)
        return _ENOMEM;
    size_t offset = 0;
    for (int i = 0; i < iovlen; i++) {
        memcpy(req + offset, msg->msg_iov[i].iov_base, msg->msg_iov[i].iov_len);
        offset += msg->msg_iov[i].iov_len;
    }

    // Do NOT reset the reply buffer here. A real netlink socket queues
    // replies per-request; a sender that pipelines several requests before
    // reading any acks (sd-netlink's loopback_setup fires RTM_NEWADDR
    // 127.0.0.1, RTM_NEWADDR ::1, RTM_SETLINK IFF_UP back-to-back, then
    // collects the three acks in seq order) must find ALL of them. The old
    // reset-on-every-sendmsg discarded the unread acks of every request but
    // the last, so systemd waited forever for the first ack -- observed as
    // PID 1 permanently polling its rtnetlink fd before spawning a single
    // child. Replies accumulate; recvmsg's drain path already resets the
    // buffer once everything is consumed.
    int err = 0;
    offset = 0;
    while (offset + sizeof(struct nlmsghdr_) <= req_len) {
        struct nlmsghdr_ *hdr = (struct nlmsghdr_ *) (req + offset);
        if (hdr->nlmsg_len < sizeof(*hdr) || offset + hdr->nlmsg_len > req_len) {
            err = netlink_append_error(sock, 0, hdr, _EINVAL);
            break;
        }
        const void *payload = req + offset + NLMSG_HDRLEN;
        size_t payload_len = hdr->nlmsg_len - NLMSG_HDRLEN;
        if (sock->socket.protocol == NETLINK_ROUTE_)
            err = netlink_handle_route_request(sock, hdr, payload, payload_len);
        else if (sock->socket.protocol == NETLINK_GENERIC_)
            err = netlink_handle_generic_request(sock, hdr, payload, payload_len);
        else if (sock->socket.protocol == NETLINK_AUDIT_)
            err = netlink_handle_audit_request(sock, hdr, payload, payload_len);
        else
            err = netlink_handle_diag_request(sock, hdr, payload, payload_len);
        if (err < 0)
            break;
        offset += NLMSG_ALIGN(hdr->nlmsg_len);
    }
    free(req);
    return err < 0 ? err : (int) req_len;
}

static int netlink_handle_recvmsg(struct fd *sock, struct msghdr *msg, int fake_flags) {
    lock(&sock->socket.netlink_reply_lock, 0);
    size_t available = sock->socket.netlink_reply_len - sock->socket.netlink_reply_off;
    bool peek = (fake_flags & MSG_PEEK_) != 0;
    bool want_trunc_len = (fake_flags & MSG_TRUNC_) != 0;
    size_t capacity = diag_iov_capacity(msg->msg_iov, msg->msg_iovlen);
    int ret;
    if (sock->socket.netlink_reply_off >= sock->socket.netlink_reply_len) {
        ret = _EAGAIN;
        goto out;
    }
    // No capacity==0 early-out here: a zero-length read must still fall
    // through to the loop below, which truncate-AND-CONSUMES the first
    // message (Linux datagram semantics: a recv always eats the datagram,
    // however small the buffer), and must still fill msg_name. sd-netlink
    // depends on both: it peeks with an empty iov to size the datagram
    // while checking the SENDER address -- an early-out that skipped the
    // msg_name fill handed it uninitialized garbage for nl_pid, it
    // declared the message "from untrusted PORT, ignoring", and its drop
    // (another empty-iov recvmsg, flags=0) then consumed nothing, leaving
    // the same message at the head of the queue forever: resolved spun
    // peek/drop/peek at 100% CPU until its start timeout.

    size_t copied = 0;
    size_t reply_off = sock->socket.netlink_reply_off;
    while (reply_off + sizeof(struct nlmsghdr_) <= sock->socket.netlink_reply_len) {
        struct nlmsghdr_ *hdr = (struct nlmsghdr_ *)
            (sock->socket.netlink_reply + reply_off);
        size_t msg_len = NLMSG_ALIGN(hdr->nlmsg_len);
        if (copied != 0 && copied + msg_len > capacity)
            break;
        if (copied == 0 && msg_len > capacity) {
            msg->msg_flags |= MSG_TRUNC;
            msg_len = capacity;
        }
        int err = diag_copy_to_iov(msg->msg_iov, msg->msg_iovlen, copied,
                sock->socket.netlink_reply + reply_off, msg_len);
        if (err < 0) {
            ret = err;
            goto out;
        }
        copied += msg_len;
        reply_off += NLMSG_ALIGN(hdr->nlmsg_len);
        if (msg_len < NLMSG_ALIGN(hdr->nlmsg_len))
            break;
        if (copied == capacity)
            break;
    }

    if (msg->msg_name != NULL) {
        struct sockaddr_nl_ name = {
            .nl_family = AF_NETLINK_,
            .nl_pid = 0,
            .nl_groups = 0,
        };
        size_t copy_len = msg->msg_namelen;
        if (copy_len > sizeof(name))
            copy_len = sizeof(name);
        if (copy_len != 0)
            memcpy(msg->msg_name, &name, copy_len);
        msg->msg_namelen = sizeof(name);
    }
    if (!peek)
        sock->socket.netlink_reply_off = reply_off;
    if (!peek && sock->socket.netlink_reply_off >= sock->socket.netlink_reply_len)
        netlink_reply_reset_locked(sock);
    if ((msg->msg_flags & MSG_TRUNC) && want_trunc_len)
        ret = (int) available;
    else
        ret = (int) copied;
out:
    unlock(&sock->socket.netlink_reply_lock);
    if (getenv("ISH_NETLINK_DIAG") != NULL)
        printk("NLDIAG: recvmsg pid=%d flags=%#x cap=%zu avail=%zu ret=%d trunc=%d\n",
               current->pid, fake_flags, capacity, available, ret,
               !!(msg->msg_flags & MSG_TRUNC));
    return ret;
}

static int netlink_sockaddr_write(guest_addr_t sockaddr_addr, const void *sockaddr, uint_t *sockaddr_len) {
    uint_t actual_len = sizeof(struct sockaddr_nl_);
    uint_t copy_len = *sockaddr_len;
    if (copy_len > actual_len)
        copy_len = actual_len;
    if (copy_len != 0)
        if (user_write(sockaddr_addr, sockaddr, copy_len))
            return _EFAULT;
    *sockaddr_len = actual_len;
    return 0;
}

static int unix_socket_get(const char *path_raw, struct fd *bind_fd, uint32_t *socket_id) {
    char path[MAX_PATH];
    int err = path_normalize(AT_PWD, path_raw, path, N_SYMLINK_FOLLOW);
    if (err < 0)
        return err;
    char guest_path[MAX_PATH]; // pre-trim path for inotify; see generic_openat
    strcpy(guest_path, path);
    struct mount *mount = find_mount_and_trim_path(path);
    if (mount == NULL)
        return _ENOENT;
    struct statbuf stat;
    err = mount->fs->stat(mount, path, &stat);

    // If bind was called, there are some funny semantics.
    if (bind_fd != NULL) {
        // If the file exists, fail.
        if (err == 0) {
            err = _EADDRINUSE;
            goto out;
        }
        // If the file can't be found, try to create it as a socket.
        if (err < 0) {
            mode_t_ mode = 0777;
            struct fs_info *fs = current->fs;
            lock(&fs->lock, 0);
            mode &= ~fs->umask;
            unlock(&fs->lock);
            err = mount->fs->mknod(mount, path, S_IFSOCK | mode, 0);
            if (err < 0)
                goto out;
            // bind() creating the socket inode is a filesystem create like
            // any other and must raise IN_CREATE (generic_mknodat does the
            // same for guest mknod). sd-bus depends on it: a daemon started
            // before dbus.socket exists parks in WATCH_BIND state watching
            // /run/dbus with inotify and only retries its connection when
            // the bind of system_bus_socket fires the event. Without it,
            // early-starting bus clients (systemd-resolved, logind) never
            // connected, never triggered dbus-broker's socket activation,
            // and ran nameless forever -- every D-Bus call to them ate the
            // full 25s/120s method-call timeout.
            inotify_notify_create(guest_path, false);
            err = mount->fs->stat(mount, path, &stat);
            if (err < 0)
                goto out;
        }
    }

    // If something other than bind was called, just do the obvious thing and
    // fail if stat failed.
    if (bind_fd == NULL && err < 0)
        goto out;

    if (!S_ISSOCK(stat.mode)) {
        err = _ENOTSOCK;
        goto out;
    }

    // Connecting to a unix socket needs write permission on the socket file --
    // Linux's unix_find_other() calls inode_permission(MAY_WRITE) at exactly
    // this point. Without it the socket's own mode meant nothing: mode 0700,
    // 0666 and even 0000 all connected for any uid, so every daemon whose
    // access control IS its socket mode (0660 root:docker and friends) was
    // open to the whole guest. Only bind is exempt -- it is creating the
    // socket, not reaching one that already exists.
    if (bind_fd == NULL) {
        err = access_check(&stat, AC_W);
        if (err < 0)
            goto out;
    }

    // Look up the socket ID for the inode number.
    struct inode_data *inode = inode_get(mount, stat.inode);
    lock(&inode->lock, 0);
    if (inode->socket_id == 0)
        inode->socket_id = unix_socket_next_id();
    unlock(&inode->lock);
    *socket_id = inode->socket_id;

    mount_release(mount);
    if (bind_fd != NULL)
        bind_fd->socket.unix_name_inode = inode;
    else
        inode_release(inode);
    return 0;

out:
    mount_release(mount);
    return err;
}

// ---- unix peer-token registry ----
//
// The connect side of a guest AF_UNIX stream sends an 8-byte token as the
// first bytes on the wire so the accept side can link socket.unix_peer (for
// SO_PEERCRED and friends). This used to be the sender's raw `struct fd *`,
// and the accept side dereferenced whatever 8 bytes it read. Two fatal
// flaws, both observed as emulator SIGBUS crashes during fresh Arch systemd
// boots (three identical crash reports in unix_socket_finish_peer, each
// "pointer" decoding to a CLOCK_MONOTONIC timestamp matching the moment of
// the message -- i.e. real application payload interpreted as a pointer):
//
//   1. a stream whose first bytes are NOT a token -- whatever guest path
//      produces one -- got those bytes dereferenced as a host pointer;
//   2. even a genuine token had no lifetime guarantee: the sender could be
//      closed and freed before the acceptor consumed the token.
//
// Now the wire carries a random 64-bit cookie and this registry maps live
// cookies to their sender fds. The acceptor PEEKs the first 8 bytes,
// validates them against the registry, and only consumes them on a match;
// unknown bytes are left in the stream untouched and the socket simply has
// no unix_peer (SO_PEERCRED degrades the same way it already does for
// datagram sockets). Entries die with their sender (sock_close), so a
// dangling cookie can never resolve to a freed fd. unix_token_lock nests
// OUTSIDE peer_lock: the consume path holds it across lookup+link so a
// concurrent sender close (which unregisters under the same lock, then
// unlinks under peer_lock) either wins entirely -- lookup misses, no link --
// or blocks until the link is made and then tears it down normally.
struct unix_token {
    uint64_t cookie;
    struct fd *sender;
    struct list tokens;
};
static lock_t unix_token_lock = LOCK_INITIALIZER;
static struct list unix_tokens = LIST_INITIALIZER(unix_tokens);

static struct unix_token *unix_token_find_locked(uint64_t cookie) {
    struct unix_token *token;
    list_for_each_entry(&unix_tokens, token, tokens) {
        if (token->cookie == cookie)
            return token;
    }
    return NULL;
}

// Register a fresh token for `sock` and return its cookie (never 0).
static uint64_t unix_token_register(struct fd *sock) {
    struct unix_token *token = malloc(sizeof(*token));
    if (token == NULL)
        return 0;
    lock(&unix_token_lock, 0);
    do {
        arc4random_buf(&token->cookie, sizeof(token->cookie));
    } while (token->cookie == 0 || unix_token_find_locked(token->cookie) != NULL);
    token->sender = sock;
    list_add(&unix_tokens, &token->tokens);
    uint64_t cookie = token->cookie;
    unlock(&unix_token_lock);
    return cookie;
}

// Tombstone every token registered by `sock` (unconsumed sends). Called
// from sock_close, so a cookie can never resolve to a freed fd. The entry
// itself must SURVIVE as a tombstone (sender=NULL): its 8 cookie bytes are
// already on the wire, and the acceptor must still recognize and consume
// them -- just without linking a peer -- or the guest would read 8 bytes of
// garbage ahead of its real data. Tombstones of connections that never get
// accepted are capped; evicting an ancient one merely re-opens the
// stray-8-bytes corner for a connection nobody accepted in ages.
#define UNIX_TOKEN_TOMBSTONE_MAX 1024
static unsigned unix_token_tombstones = 0;

static void unix_token_unregister_sender(struct fd *sock) {
    lock(&unix_token_lock, 0);
    struct unix_token *token, *tmp;
    list_for_each_entry_safe(&unix_tokens, token, tmp, tokens) {
        if (token->sender == sock) {
            token->sender = NULL;
            unix_token_tombstones++;
        }
    }
    if (unix_token_tombstones > UNIX_TOKEN_TOMBSTONE_MAX) {
        list_for_each_entry_safe(&unix_tokens, token, tmp, tokens) {
            if (unix_token_tombstones <= UNIX_TOKEN_TOMBSTONE_MAX)
                break;
            if (token->sender == NULL) {
                list_remove(&token->tokens);
                free(token);
                unix_token_tombstones--;
            }
        }
    }
    unlock(&unix_token_lock);
}

static int unix_socket_send_peer_token(struct fd *sock) {
    uint64_t cookie = unix_token_register(sock);
    if (cookie == 0)
        return _ENOMEM;
    size_t sent = 0;
    const char *buf = (const char *) &cookie;
    socket_force_host_nonblock(sock);
    while (sent < sizeof(cookie)) {
        ssize_t res = 0;
        TASK_MAY_BLOCK {
            while (1) {
                errno = 0;
                res = write(sock->real_fd, buf + sent, sizeof(cookie) - sent);
                if (res >= 0)
                    break;
                if (socket_should_retry_io_eintr(sock, 0))
                    continue;
                if ((errno == EAGAIN || errno == EWOULDBLOCK) &&
                        socket_call_is_blocking(sock, 0)) {
                    // NULL: iSH's own handshake, not a guest call, so it must
                    // not inherit the guest's SO_SNDTIMEO.
                    int wait_err = socket_wait_ready(sock, POLLOUT, NULL);
                    if (wait_err < 0) {
                        res = wait_err;
                        break;
                    }
                    continue;
                }
                break;
            }
        }
        if (res < 0) {
            unix_token_unregister_sender(sock);
            return res > -4096 && res < 0 ? (int) res : errno_map();
        }
        if (res == 0) {
            unix_token_unregister_sender(sock);
            return _EPIPE;
        }
        sent += (size_t) res;
    }
    return 0;
}

static int unix_socket_finish_peer(struct fd *sock) {
    if (sock->socket.domain != AF_LOCAL_)
        return 0;

    if (sock->socket.unix_peer_pending) {
        // PEEK at the first 8 bytes without consuming them: only a cookie
        // that resolves in the token registry may be taken off the wire.
        // MSG_PEEK always reads from the front of the stream, so no partial
        // -read offset bookkeeping is needed (or possible).
        uint64_t cookie = 0;
        ssize_t res = 0;
        socket_force_host_nonblock(sock);
        for (;;) {
            TASK_MAY_BLOCK {
                while (1) {
                    errno = 0;
                    res = recv(sock->real_fd, &cookie, sizeof(cookie), MSG_PEEK);
                    if (res >= 0)
                        break;
                    if (errno == EINTR) {
                        if (socket_guest_signal_pending())
                            break;
                        continue;
                    }
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        int wait_err = socket_wait_ready(sock, POLLIN, NULL);
                        if (wait_err < 0) {
                            res = wait_err;
                            break;
                        }
                        continue;
                    }
                    break;
                }
            }
            if (res < 0)
                return res > -4096 && res < 0 ? (int) res : errno_map();
            if (res == 0)
                return _ECONNRESET;
            if ((size_t) res >= sizeof(cookie))
                break;
            // Fewer than 8 bytes available yet: wait for more, then re-peek.
            int wait_err = socket_wait_ready(sock, POLLIN, NULL);
            if (wait_err < 0)
                return wait_err;
        }

        // Hold unix_token_lock across lookup AND linking: a concurrent
        // sender close unregisters under this lock before unlinking under
        // peer_lock, so it either wins entirely (lookup misses, no link) or
        // waits for the link and then tears it down normally.
        lock(&unix_token_lock, 0);
        struct unix_token *token = unix_token_find_locked(cookie);
        if (token == NULL) {
            unlock(&unix_token_lock);
            // Not a token: a stream whose first bytes are application data
            // (or whose sender already closed). Leave the bytes for the
            // guest to read; the socket just has no linked unix_peer.
            printk("INFO: unix accept without peer token (comm=%s first-bytes=%#llx)\n",
                   current != NULL ? current->comm : "?",
                   (unsigned long long) cookie);
            sock->socket.unix_peer_pending = false;
            return 0;
        }
        struct fd *peer = token->sender; // NULL: tombstone (sender closed);
                                         // consume the bytes, link nothing
        if (peer == NULL)
            unix_token_tombstones--;
        list_remove(&token->tokens);
        free(token);

        // The cookie is validated and its sender is live (its close would
        // have unregistered it); now actually consume the 8 peeked bytes.
        size_t got = 0;
        while (got < sizeof(cookie)) {
            errno = 0;
            res = recv(sock->real_fd, (char *) &cookie + got, sizeof(cookie) - got, 0);
            if (res < 0 && errno == EINTR)
                continue;
            if (res <= 0)
                break; // peeked bytes vanished: connection died underneath
            got += (size_t) res;
        }

        lock(&peer_lock, 0);
        if (peer != NULL && got == sizeof(cookie) && sock->socket.unix_peer == NULL) {
            sock->socket.unix_peer = peer;
            peer->socket.unix_peer = sock;
            sock->socket.unix_peer_cred = peer->socket.unix_cred;
            sock->socket.unix_peer_cred_valid = true;
            peer->socket.unix_peer_cred = sock->socket.unix_cred;
            peer->socket.unix_peer_cred_valid = true;
            notify(&peer->socket.unix_got_peer);
        }
        sock->socket.unix_peer_pending = false;
        unlock(&peer_lock);
        unlock(&unix_token_lock);
        return 0;
    }

    // Once there is no pending peer token left to consume, ordinary AF_UNIX
    // I/O must keep following the underlying socket state. Returning ENOTCONN
    // here breaks clients that read EOF or continue using the socket after the
    // peer has already closed.
    return 0;
}

// Dan Bernstein's simple and decently effective hash function
static uint32_t str_hash(const char *str) {
    uint32_t hash = 5381;
    for (int i = 0; str[i] != '\0'; i++) {
        hash = 33 * hash ^ str[i];
    }
    return hash;
}

// The abstract socket namespace is a lot simpler than it sounds: if the first
// byte of the path is a null byte, then it gets looked up in this hashtable
// instead of the filesystem.

struct unix_abstract {
    unsigned refcount;
    uint32_t hash;
    size_t name_len;
    char *name;
    uint32_t socket_id;
    struct list links;
};
#define ABSTRACT_HASH_SIZE 1024
static struct list abstract_hash[ABSTRACT_HASH_SIZE];
static lock_t unix_abstract_lock = LOCK_INITIALIZER;

static int unix_abstract_get(const char *name, struct fd *bind_fd, uint32_t *socket_id) {
    uint32_t hash = str_hash(name);
    size_t name_len = strlen(name);
    lock(&unix_abstract_lock, 0);
    struct unix_abstract *sock_tmp;
    struct unix_abstract *sock = NULL;
    struct list *bucket = &abstract_hash[hash % ABSTRACT_HASH_SIZE];
    if (list_null(bucket))
        list_init(bucket);
    list_for_each_entry(bucket, sock_tmp, links) {
        if (sock_tmp->hash == hash &&
                sock_tmp->name_len == name_len &&
                memcmp(sock_tmp->name, name, name_len) == 0) {
            sock = sock_tmp;
            break;
        }
    }

    if (bind_fd != NULL && sock != NULL) {
        unlock(&unix_abstract_lock);
        return _EEXIST;
    }
    if (bind_fd == NULL && sock == NULL) {
        unlock(&unix_abstract_lock);
        return _ENOENT;
    }

    if (sock == NULL) {
        sock = malloc(sizeof(struct unix_abstract));
        if (sock == NULL) {
            unlock(&unix_abstract_lock);
            return _ENOMEM;
        }
        sock->name = strdup(name);
        if (sock->name == NULL) {
            free(sock);
            unlock(&unix_abstract_lock);
            return _ENOMEM;
        }
        sock->refcount = 0;
        sock->hash = hash;
        sock->name_len = name_len;
        sock->socket_id = unix_socket_next_id();
        list_add(bucket, &sock->links);
    }

    sock->refcount++;
    unlock(&unix_abstract_lock);
    *socket_id = sock->socket_id;
    if (bind_fd != NULL)
        bind_fd->socket.unix_name_abstract = sock;
    return 0;
}

static bool unix_socket_should_fallback_x11_path(const char *name) {
    static const char x11_prefix[] = "/tmp/.X11-unix/";
    return strncmp(name, x11_prefix, strlen(x11_prefix)) == 0;
}

static void unix_abstract_release(struct unix_abstract *name) {
    lock(&unix_abstract_lock, 0);
    if (--name->refcount == 0) {
        list_remove(&name->links);
        free(name->name);
        free(name);
    }
    unlock(&unix_abstract_lock);
}

const char *sock_tmp_prefix = "/tmp/ishsock";

static int sockaddr_read_bind(guest_addr_t sockaddr_addr, void *sockaddr, uint_t *sockaddr_len, struct fd *bind_fd) {
    // Make sure we can read things without overflowing buffers
    if (*sockaddr_len < 2)
        return _EINVAL;
    uint16_t guest_family;
    if (user_read(sockaddr_addr, &guest_family, sizeof(guest_family)))
        return _EFAULT;
    int real_family = sock_family_to_real(guest_family);

    switch (real_family) {
        case PF_INET: {
            if (*sockaddr_len < sizeof(struct sockaddr_in_))
                return _EINVAL;
            struct sockaddr_in_ guest_addr;
            if (user_read(sockaddr_addr, &guest_addr, sizeof(guest_addr)))
                return _EFAULT;
            {
                struct sockaddr_in *real_addr = (struct sockaddr_in *) sockaddr;
                memset(real_addr, 0, sizeof(*real_addr));
#ifdef __APPLE__
                real_addr->sin_len = sizeof(*real_addr);
#endif
                real_addr->sin_family = PF_INET;
                real_addr->sin_port = guest_addr.sin_port;
                real_addr->sin_addr.s_addr = guest_addr.sin_addr;
            }
            *sockaddr_len = sizeof(struct sockaddr_in);
            break;
        }
        case PF_INET6: {
            if (*sockaddr_len < sizeof(struct sockaddr_in6_))
                return _EINVAL;
            struct sockaddr_in6_ guest_addr;
            if (user_read(sockaddr_addr, &guest_addr, sizeof(guest_addr)))
                return _EFAULT;
            {
                struct sockaddr_in6 *real_addr = (struct sockaddr_in6 *) sockaddr;
                memset(real_addr, 0, sizeof(*real_addr));
#ifdef __APPLE__
                real_addr->sin6_len = sizeof(*real_addr);
#endif
                real_addr->sin6_family = PF_INET6;
                real_addr->sin6_port = guest_addr.sin6_port;
                real_addr->sin6_flowinfo = guest_addr.sin6_flowinfo;
                real_addr->sin6_addr = guest_addr.sin6_addr;
                real_addr->sin6_scope_id = guest_addr.sin6_scope_id;
            }
            *sockaddr_len = sizeof(struct sockaddr_in6);
            break;
        }
        case PF_NETLINK_:
            if (*sockaddr_len < sizeof(struct sockaddr_nl_))
                return _EINVAL;
            if (user_read(sockaddr_addr, sockaddr, sizeof(struct sockaddr_nl_)))
                return _EFAULT;
            *sockaddr_len = sizeof(struct sockaddr_nl_);
            break;

        case PF_LOCAL: {
            if (*sockaddr_len > sizeof(struct sockaddr_max_))
                return _EINVAL;
            if (user_read(sockaddr_addr, sockaddr, *sockaddr_len))
                return _EFAULT;
            struct sockaddr_ *fake_addr = sockaddr;
            // First pull out the path, being careful to not overflow anything.
            char path[SOCKADDR_DATA_MAX + 1];
            size_t path_size = *sockaddr_len - offsetof(struct sockaddr_, data);
            memcpy(path, fake_addr->data, path_size);
            path[path_size] = '\0';

            uint32_t socket_id;
            int err;
            if (path_size == 0) {
                return _ENOENT;
            } else if (path[0] != '\0') {
                STRACE(" unix socket %s", path);
                err = unix_socket_get(path, bind_fd, &socket_id);
            } else {
                STRACE(" unix abstract socket %s", path + 1);
                err = unix_abstract_get(path + 1, bind_fd, &socket_id);
                if (err == _ENOENT && bind_fd == NULL &&
                        unix_socket_should_fallback_x11_path(path + 1)) {
                    STRACE(" unix abstract fallback to path %s", path + 1);
                    err = unix_socket_get(path + 1, bind_fd, &socket_id);
                }
            }
            if (err < 0)
                return err;
            if (bind_fd != NULL) {
                bind_fd->socket.unix_name_len = path_size;
                memcpy(bind_fd->socket.unix_name, path, path_size);
            }

            struct sockaddr_un *real_addr_un = sockaddr;
            size_t path_len = snprintf(real_addr_un->sun_path, sizeof(real_addr_un->sun_path), "%s.%u", sock_tmp_prefix, socket_id);
            if (path_len >= sizeof(real_addr_un->sun_path)) {
                return _ENAMETOOLONG;
            }
#ifdef __APPLE__
            real_addr_un->sun_len = offsetof(struct sockaddr_un, sun_path) + path_len;
#endif
            real_addr_un->sun_family = PF_LOCAL;
            // The call to real bind will fail if the backing socket already
            // exists from a previous run or something. We already checked that
            // the fake file doesn't exist in unix_socket_get, so try a simple
            // solution.
            if (bind_fd != NULL)
                unlink(real_addr_un->sun_path);
            *sockaddr_len = offsetof(struct sockaddr_un, sun_path) + path_len;
            break;
        }
        default:
            return _EINVAL;
    }
    return 0;
}

static int sockaddr_read(guest_addr_t sockaddr_addr, void *sockaddr, uint_t *sockaddr_len) {
    struct inode_data *inode = NULL;
    int err = sockaddr_read_bind(sockaddr_addr, sockaddr, sockaddr_len, NULL);
    inode_release_if_exist(inode);
    if (err < 0)
        return err;
    // As a *destination* (connect/sendto/sendmsg -- everything except bind,
    // which uses sockaddr_read_bind directly), the wildcard address means
    // "this host" on Linux: 0.0.0.0 behaves like 127.0.0.1 and :: like ::1.
    // Darwin instead fails such sends with EHOSTUNREACH. stress-ng --udp's
    // client targets 0.0.0.0, so every send died instantly and the server
    // side sat in recvfrom forever.
    struct sockaddr *real_addr = sockaddr;
    if (real_addr->sa_family == PF_INET) {
        struct sockaddr_in *sin = (struct sockaddr_in *) real_addr;
        if (sin->sin_addr.s_addr == htonl(INADDR_ANY))
            sin->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    } else if (real_addr->sa_family == PF_INET6) {
        struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *) real_addr;
        if (IN6_IS_ADDR_UNSPECIFIED(&sin6->sin6_addr))
            sin6->sin6_addr = in6addr_loopback;
    }
    return err;
}

static int ipv6_recverr_fd_get(struct fd *sock);

static int sockaddr_write(guest_addr_t sockaddr_addr, void *sockaddr, uint_t buffer_len, uint_t *sockaddr_len) {
    struct sockaddr *real_addr = sockaddr;
    struct sockaddr_ *fake_addr = sockaddr;
    fake_addr->family = sock_family_from_real(real_addr->sa_family);
    switch (fake_addr->family) {
        case PF_LOCAL_: {
            // Most callers of sockaddr_write use it to return a peer name, and
            // since we don't know the peer name in this case, just return the
            // default peer name, which is the null address.
            static struct sockaddr_ unix_domain_null = {.family = PF_LOCAL_};
            sockaddr = &unix_domain_null;
            *sockaddr_len = sizeof(unix_domain_null);
            break;
        }
        case PF_INET_:
        case PF_INET6_:
            break;
        case PF_NETLINK_:
            break;
        default:
            return _EINVAL;
    }

    if (buffer_len > *sockaddr_len)
        buffer_len = *sockaddr_len;
    // The address is supposed to be truncated if the specified length is too
    // short, instead of returning an error.
    if (user_write(sockaddr_addr, sockaddr, buffer_len))
        return _EFAULT;
    return 0;
}

// ---------------------------------------------------------------------
// Guest-loopback NAT.
//
// iSH's guest has no network namespace of its own -- every guest socket
// is a host socket. That breaks two things a Linux userland takes for
// granted about loopback: any 127.x.y.z address works (macOS only has
// 127.0.0.1 unless root adds aliases), and root can bind ports < 1024
// (iSH never runs as root on macOS or iOS). systemd-resolved's DNS stub
// listener needs BOTH (127.0.0.53:53) and treats bind failure as fatal.
//
// When a guest AF_INET bind fails with EADDRNOTAVAIL/EACCES/EPERM on a
// loopback (or wildcard-with-privileged-port) endpoint, the host socket
// is re-bound to 127.0.0.1 with an ephemeral port and the guest-visible
// endpoint is recorded in a process-wide table. Guest-to-guest loopback
// traffic is then translated at the edges: connect/sendto/sendmsg
// destinations matching a recorded guest endpoint are rewritten to the
// real host endpoint, and recvfrom/recvmsg source addresses (plus
// getsockname/getpeername) are rewritten back, so e.g. glibc's stub
// resolver -- which checks that a DNS reply's source address is exactly
// the 127.0.0.53:53 it queried -- sees what it expects. Traffic from
// outside the emulator can't reach these endpoints either way, so the
// translation only ever has to be guest-consistent, not host-visible.
// ---------------------------------------------------------------------

struct inet_nat_entry {
    struct list list;
    uint32_t guest_addr;   // network byte order; INADDR_ANY = wildcard
    uint16_t guest_port;   // network byte order
    uint16_t host_port;    // network byte order
    int type;              // SOCK_STREAM_ / SOCK_DGRAM_
    struct fd *owner;
};

static lock_t inet_nat_lock = LOCK_INITIALIZER;
static struct list inet_nat_table = LIST_INITIALIZER(inet_nat_table);

static bool inet_addr_is_loopback(uint32_t addr_be) {
    return (ntohl(addr_be) >> 24) == 127;
}

// Try to recover a failed AF_INET bind by re-binding to
// 127.0.0.1:<ephemeral> and recording the guest-visible endpoint.
// Returns 0 on success, negative errno to report the original failure.
static int inet_nat_bind_fallback(struct fd *sock, struct sockaddr_in *sin, int orig_err) {
    if (sin->sin_family != AF_INET)
        return orig_err;
    bool loopback = inet_addr_is_loopback(sin->sin_addr.s_addr);
    bool wildcard = sin->sin_addr.s_addr == htonl(INADDR_ANY);
    bool priv_port = sin->sin_port != 0 && ntohs(sin->sin_port) < 1024;
    if (!(loopback || (wildcard && priv_port)))
        return orig_err;

    uint32_t guest_addr = sin->sin_addr.s_addr;
    uint16_t guest_port = sin->sin_port;

    struct sockaddr_in host_sin = *sin;
    if (loopback)
        host_sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    host_sin.sin_port = 0;
    if (bind(sock->real_fd, (struct sockaddr *) &host_sin, sizeof(host_sin)) < 0)
        return orig_err;
    socklen_t host_len = sizeof(host_sin);
    if (getsockname(sock->real_fd, (struct sockaddr *) &host_sin, &host_len) < 0)
        return orig_err;

    struct inet_nat_entry *entry = malloc(sizeof(*entry));
    if (entry == NULL)
        return _ENOMEM;
    entry->guest_addr = guest_addr;
    entry->guest_port = guest_port;
    entry->host_port = host_sin.sin_port;
    entry->type = sock->socket.type;
    entry->owner = sock;
    lock(&inet_nat_lock, 0);
    list_add_tail(&inet_nat_table, &entry->list);
    unlock(&inet_nat_lock);

    sock->socket.inet_nat_bound = true;
    sock->socket.inet_nat_bound_addr = guest_addr;
    sock->socket.inet_nat_bound_port = guest_port;
    STRACE(" [nat: guest %#x:%u -> host 127.0.0.1:%u]",
            ntohl(guest_addr), ntohs(guest_port), ntohs(host_sin.sin_port));
    // The loopback-alias case (127.x.y.z -> 127.0.0.1) is a transparent,
    // harmless substitution -- same reachability either way. A wildcard
    // bind on a privileged port is not: the guest asked to be reachable
    // from the whole network (e.g. sshd on :22) and, because iSH never
    // runs as root, silently got a loopback-only listener instead, with
    // no error to notice. Surfacing that distinction is the whole point
    // of this warning; ports >=1024 don't hit this path at all, since a
    // non-root process can really bind those on a wildcard address.
    if (wildcard && priv_port) {
        printk("WARNING: %d(%s) bound 0.0.0.0:%u but iSH-AOK can't bind privileged "
               "ports as non-root -- this socket is loopback-only, NOT reachable "
               "from the network. Use a port >=1024 for anything meant to accept "
               "external connections.\n",
               current->pid, current->comm, ntohs(guest_port));
    }
    return 0;
}

// Rewrites a destination the guest addressed at a NAT'd guest endpoint to
// the real host endpoint. Returns true if rewritten.
static bool inet_nat_rewrite_dest(struct fd *sock, void *sockaddr) {
    struct sockaddr_in *sin = sockaddr;
    if (sin->sin_family != AF_INET || !inet_addr_is_loopback(sin->sin_addr.s_addr))
        return false;
    bool rewritten = false;
    lock(&inet_nat_lock, 0);
    struct inet_nat_entry *entry;
    // Exact-address entries take priority over wildcard (INADDR_ANY) ones.
    struct inet_nat_entry *wildcard_match = NULL;
    list_for_each_entry(&inet_nat_table, entry, list) {
        if (entry->type != sock->socket.type || entry->guest_port != sin->sin_port)
            continue;
        if (entry->guest_addr == sin->sin_addr.s_addr) {
            sin->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            sin->sin_port = entry->host_port;
            rewritten = true;
            break;
        }
        if (entry->guest_addr == htonl(INADDR_ANY) && wildcard_match == NULL)
            wildcard_match = entry;
    }
    if (!rewritten && wildcard_match != NULL) {
        sin->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sin->sin_port = wildcard_match->host_port;
        rewritten = true;
    }
    unlock(&inet_nat_lock);
    return rewritten;
}

// Rewrites a received source address (127.0.0.1:<host port> of a NAT'd
// socket) back to the guest endpoint the sender is known as.
static void inet_nat_rewrite_src(struct fd *sock, void *sockaddr) {
    struct sockaddr_in *sin = sockaddr;
    if (sin->sin_family != AF_INET || sin->sin_addr.s_addr != htonl(INADDR_LOOPBACK))
        return;
    lock(&inet_nat_lock, 0);
    struct inet_nat_entry *entry;
    list_for_each_entry(&inet_nat_table, entry, list) {
        if (entry->type != sock->socket.type || entry->host_port != sin->sin_port)
            continue;
        if (entry->guest_addr != htonl(INADDR_ANY))
            sin->sin_addr.s_addr = entry->guest_addr;
        sin->sin_port = entry->guest_port;
        break;
    }
    unlock(&inet_nat_lock);
}

static void inet_nat_remove_owner(struct fd *fd) {
    lock(&inet_nat_lock, 0);
    struct inet_nat_entry *entry, *tmp;
    list_for_each_entry_safe(&inet_nat_table, entry, tmp, list) {
        if (entry->owner == fd) {
            list_remove(&entry->list);
            free(entry);
        }
    }
    unlock(&inet_nat_lock);
}

// Releases whatever unix bind-name (inode-backed or abstract) `fd` currently
// holds and clears both fields, so a later re-release (e.g. a failed rebind
// followed by fd close) can't double-release the same name.
static void release_unix_names(struct fd *fd) {
    inode_release_if_exist(fd->socket.unix_name_inode);
    fd->socket.unix_name_inode = NULL;
    if (fd->socket.unix_name_abstract != NULL) {
        unix_abstract_release(fd->socket.unix_name_abstract);
        fd->socket.unix_name_abstract = NULL;
    }
}

static int_t sys_bind_common(fd_t sock_fd, guest_addr_t sockaddr_addr, uint_t sockaddr_len) {
    STRACE("bind(%d, 0x%llx, %d)", sock_fd, (unsigned long long) sockaddr_addr, sockaddr_len);
    int_t sock_err;
    struct fd *sock = sock_getfd(sock_fd, &sock_err);
    if (sock == NULL)
        return sock_err;
    struct sockaddr_max_ sockaddr;
    struct inode_data *inode = NULL;
    int err = sockaddr_read_bind(sockaddr_addr, &sockaddr, &sockaddr_len, sock);
    if (err < 0)
        return err;

    if (sock->socket.domain == AF_NETLINK_) {
        struct sockaddr_nl_ *addr = (struct sockaddr_nl_ *) &sockaddr;
        if (addr->nl_family != AF_NETLINK_)
            return _EINVAL;
        sock->socket.netlink_groups = addr->nl_groups;
        if (addr->nl_pid != 0)
            sock->socket.netlink_port_id = addr->nl_pid;
        return 0;
    }

    // CAP_NET_BIND_SERVICE: ports below 1024 are privileged on Linux. We had no
    // check at all, so uid 1000 could bind 127.0.0.1:80 or :53 and serve
    // guest-local traffic there -- and because the NAT fallback below rescues a
    // privileged-port bind the host refuses, it worked even though the host
    // itself would never allow it. Checked before the host bind so the fallback
    // only ever runs for a caller Linux would have permitted.
    if (sock->socket.domain == AF_INET_ || sock->socket.domain == AF_INET6_) {
        struct inet_bind_info want = {};
        if (inet_bind_info_from_sockaddr((const struct sockaddr *) &sockaddr, &want) &&
                want.port != 0 && ntohs(want.port) < 1024 &&
                !current_capable(CAP_NET_BIND_SERVICE_))
            return _EACCES;
    }
    // Remember who bound this socket, for the SO_REUSEPORT same-uid rule.
    sock->socket.bind_euid = current->euid;

#if defined(__APPLE__)
    if ((sock->socket.domain == AF_INET_ || sock->socket.domain == AF_INET6_) &&
            sock->socket.type == SOCK_STREAM_) {
        struct inet_bind_info candidate = {};
        if (inet_bind_info_from_sockaddr((const struct sockaddr *) &sockaddr, &candidate) &&
                sock_bound_inet_conflicts(sock, &candidate)) {
            return _EADDRINUSE;
        }
    }
#endif

    err = bind(sock->real_fd, (void *) &sockaddr, sockaddr_len);
    if (err < 0) {
        int mapped_err = errno_map();
        if (sock->socket.domain == AF_INET_ &&
                (errno == EADDRNOTAVAIL || errno == EACCES || errno == EPERM)) {
            // Loopback endpoints the host can't provide (a 127.x alias,
            // or a privileged port -- iSH never runs as root) get NAT'd
            // to 127.0.0.1:<ephemeral>; see inet_nat_bind_fallback.
            int nat_err = inet_nat_bind_fallback(sock,
                    (struct sockaddr_in *) &sockaddr, mapped_err);
            if (nat_err == 0) {
                sock->socket.unix_name_inode = inode;
                return 0;
            }
            mapped_err = nat_err;
        }
        release_unix_names(sock);
        return mapped_err;
    }
    sock->socket.unix_name_inode = inode;
    return 0;
}

int_t sys_bind(fd_t sock_fd, addr_t sockaddr_addr, uint_t sockaddr_len) {
    return sys_bind_common(sock_fd, sockaddr_addr, sockaddr_len);
}

int_t sys_bind_guest(fd_t sock_fd, guest_addr_t sockaddr_addr, uint_t sockaddr_len) {
    return sys_bind_common(sock_fd, sockaddr_addr, sockaddr_len);
}

static void fill_cred(struct ucred_ *cred) {
    cred->pid = current->pid;
    cred->uid = current->euid;
    cred->gid = current->egid;
}

// Per-datagram sender credentials for AF_LOCAL SOCK_DGRAM sockets: every
// guest datagram travels on the host socket with this header prepended
// (added by every send path, stripped by every recv path), so the sender's
// guest ucred arrives WITH the datagram -- no side-channel queues, no
// ordering races, MSG_PEEK-safe by construction. Linux attaches the
// SENDER's credentials to each unix datagram; the connected-peer cred this
// codebase already tracked (unix_peer_cred) is only correct for stream/
// connected pairs, not for many-senders-one-receiver datagram sockets like
// systemd's /run/systemd/notify. Without per-datagram creds, systemd drops
// every sd_notify READY=1 ("Got notification datagram lacking valid
// credential information, ignoring"), so every Type=notify service
// (journald, udevd, userdbd, ...) hangs in "activating" until its start
// timeout. All ends of a guest unix dgram socket live inside this emulator
// (the host-side socket files are private to it), so the wire format is
// ours to define.
struct unix_dgram_cred_hdr {
    uint32_t magic;
    struct ucred_ cred;
    // SCM_RIGHTS on a unix DGRAM socket travels in-band: a nonzero cookie
    // references a struct scm parcel in the unix_dgram_scm registry (next
    // to scm_free). The stream transport's peer-queue can't work here --
    // dgram sendmsg with msg_name has no unix_peer, which made every such
    // send fail EPIPE. systemd leans on exactly this: sd_notify FDSTORE
    // pushes (logind's session leader pidfds, udevd's inotify fd, ...) go
    // over the /run/systemd/notify DGRAM socket, and the EPIPE surfaced as
    // "Failed to push leader pidfd for session 'c1', ignoring: Broken
    // pipe" with session scopes never getting their PIDFD tracking.
    uint64_t scm_cookie;
};
#define UNIX_DGRAM_CRED_MAGIC 0x1D6CC12D

// Defined next to scm_free below; needed by the bare recvfrom path above
// them to discard a consumed datagram's fd parcel.
static void scm_free(struct scm *scm);
static struct scm *unix_dgram_scm_take(uint64_t cookie);

static bool sock_is_unix_dgram(struct fd *sock) {
    return sock->socket.domain == AF_LOCAL_ && sock->socket.type == SOCK_DGRAM_ &&
        sock->real_fd >= 0;
}

static void unix_dgram_cred_hdr_fill(struct unix_dgram_cred_hdr *hdr) {
    hdr->magic = UNIX_DGRAM_CRED_MAGIC;
    fill_cred(&hdr->cred);
    hdr->scm_cookie = 0;
}

// /dev/log and /run|/dev/initctl have a built-in fallback sink so guests can
// log (or send initctl messages) even when no daemon is running. But if a real
// daemon (e.g. syslog-ng) has bound the socket, we must connect to it for real
// and deliver messages; only fall back to the built-in sink when nothing is
// listening. This marks the socket as a sink; callers use it when the real
// connect/send fails with no listener.
static bool sock_devlog_initctl_fallback(struct fd *sock, bool devlog_target, bool initctl_target) {
    if (devlog_target) {
        sock->socket.unix_devlog_sink = true;
        fill_cred(&sock->socket.unix_cred);
        sock_debug_event("connect-devlog-fallback", sock, 0, 0);
        return true;
    }
    if (initctl_target) {
        sock->socket.unix_initctl_sink = true;
        fill_cred(&sock->socket.unix_cred);
        sock_debug_event("connect-initctl-fallback", sock, 0, 0);
        return true;
    }
    return false;
}

static int_t sys_connect_common(fd_t sock_fd, guest_addr_t sockaddr_addr, uint_t sockaddr_len) {
    STRACE("connect(%d, 0x%llx, %d)", sock_fd, (unsigned long long) sockaddr_addr, sockaddr_len);
    int_t sock_err;
    struct fd *sock = sock_getfd(sock_fd, &sock_err);
    if (sock == NULL)
        return sock_err;
    sock_debug_guest_sockaddr("connect", sock, sockaddr_addr, sockaddr_len);
    // Remember whether this is a /dev/log or initctl target; try a real connect
    // first and fall back to the built-in sink only if nothing is bound there.
    bool connect_devlog_target = false;
    bool connect_initctl_target = false;
    if (sock->socket.domain == AF_LOCAL_) {
        sock->socket.unix_devlog_sink = false;
        sock->socket.unix_initctl_sink = false;
        connect_devlog_target = guest_sockaddr_is_devlog(sockaddr_addr, sockaddr_len);
        connect_initctl_target = !connect_devlog_target &&
            guest_sockaddr_is_initctl(sockaddr_addr, sockaddr_len);
    }
    struct sockaddr_max_ sockaddr;
    int err = sockaddr_read(sockaddr_addr, &sockaddr, &sockaddr_len);
    if (err < 0) {
        // No real socket is bound at /dev/log or initctl: fall back to the
        // built-in sink so logging/initctl still succeed without a daemon.
        if (sock_devlog_initctl_fallback(sock, connect_devlog_target, connect_initctl_target))
            return 0;
        // Linux reports connect() to a missing abstract UNIX socket as
        // ECONNREFUSED, which util-linux agetty expects for its Plymouth probe.
        if (err == _ENOENT && sock->socket.domain == AF_LOCAL_ &&
                guest_sockaddr_is_abstract_local(sockaddr_addr, sockaddr_len))
            return _ECONNREFUSED;
        sock_debug_event("connect-parse-fail", sock, -1, err);
        return err;
    }

    if (sock->socket.domain == AF_NETLINK_) {
        struct sockaddr_nl_ *addr = (struct sockaddr_nl_ *) &sockaddr;
        if (addr->nl_family != AF_NETLINK_)
            return _EINVAL;
        if (addr->nl_pid != 0)
            return _ECONNREFUSED;
        sock->socket.netlink_groups = addr->nl_groups;
        sock_debug_event("connect-netlink", sock, 0, 0);
        return 0;
    }

    if (sock->socket.domain == AF_INET_) {
        struct sockaddr_in guest_dest = *(struct sockaddr_in *) &sockaddr;
        if (inet_nat_rewrite_dest(sock, &sockaddr)) {
            // Remember the endpoint the guest THINKS it connected to, so
            // getpeername reports it instead of the NAT'd host one.
            sock->socket.inet_nat_peer = true;
            sock->socket.inet_nat_peer_addr = guest_dest.sin_addr.s_addr;
            sock->socket.inet_nat_peer_port = guest_dest.sin_port;
        }
    }

    if (sock_trace_enabled()) {
        int host_flags = sock->real_fd >= 0 ? fcntl(sock->real_fd, F_GETFL, 0) : -1;
        printk("INFO: net connect enter pid=%d comm=%s guest_domain=%d guest_type=%d real=%d family=%d addrlen=%u guest_flags=%#x host_flags=%#x\n",
               current->pid, current->comm, sock->socket.domain, sock->socket.type,
               sock->real_fd, ((struct sockaddr_ *) &sockaddr)->family, sockaddr_len,
               fd_getflags(sock), host_flags);
    }

    int saved_host_flags = -1;
    bool forced_nonblocking_connect = false;
    if (!(fd_getflags(sock) & O_NONBLOCK_) &&
            sock->socket.type == SOCK_STREAM_ &&
            (sock->socket.domain == AF_INET_ || sock->socket.domain == AF_INET6_)) {
        // Avoid wedging inside the host kernel on a blocking TCP connect().
        // Start the connection in nonblocking mode, then restore the host fd
        // flags immediately and finish the wait in userspace below.
        saved_host_flags = fcntl(sock->real_fd, F_GETFL, 0);
        if (saved_host_flags >= 0 && !(saved_host_flags & O_NONBLOCK) &&
                fcntl(sock->real_fd, F_SETFL, saved_host_flags | O_NONBLOCK) == 0) {
            forced_nonblocking_connect = true;
        }
    }

    err = connect(sock->real_fd, (void *) &sockaddr, sockaddr_len);
    if (forced_nonblocking_connect) {
        (void) fcntl(sock->real_fd, F_SETFL, saved_host_flags);
        // This restore only runs when the host flags had no O_NONBLOCK to
        // begin with, so it cannot be undoing socket_force_host_nonblock() --
        // but it is a host-flag write, so drop the cache rather than reason
        // about it from a distance.
        sock->socket.host_nonblock = false;
    }
    if (err < 0) {
        int mapped_err = errno_map();
        if ((mapped_err == _EINPROGRESS || mapped_err == _EALREADY) &&
                !(fd_getflags(sock) & O_NONBLOCK_)) {
            if (sock_trace_enabled()) {
                printk("INFO: net connect wait pid=%d comm=%s real=%d blocking=1\n",
                       current->pid, current->comm, sock->real_fd);
            }
            mapped_err = socket_finish_blocking_connect(sock);
            if (mapped_err < 0) {
                sock_trace("connect", sock, -1, mapped_err);
                return mapped_err;
            }
            err = 0;
        } else {
            // A bound-but-not-listening (or absent) /dev/log or initctl socket
            // falls back to the built-in sink rather than failing the connect.
            if (sock_devlog_initctl_fallback(sock, connect_devlog_target, connect_initctl_target))
                return 0;
            sock_trace("connect", sock, -1, mapped_err);
            return mapped_err;
        }
    }

#if defined(__APPLE__)
    if (!(fd_getflags(sock) & O_NONBLOCK_) &&
            (sock->socket.domain == AF_INET_ || sock->socket.domain == AF_INET6_) &&
            sock->socket.type == SOCK_STREAM_ &&
            !socket_tcp_connect_established(sock)) {
        int real_error = 0;
        socklen_t real_error_len = sizeof(real_error);
        if (getsockopt(sock->real_fd, SOL_SOCKET, SO_ERROR, &real_error, &real_error_len) == 0) {
            // A concurrent poll/epoll scan on another thread may have already
            // read-and-cleared SO_ERROR; fall back to what it cached.
            if (real_error == 0 && sock->socket.host_connect_error != 0)
                real_error = sock->socket.host_connect_error;
            if (real_error != 0) {
                sock->socket.host_connect_error = 0;
                int mapped_err = err_map(real_error);
                sock_trace("connect", sock, -1, mapped_err);
                return mapped_err;
            }
        }
        // Darwin can report connect() success before TCP_CONNECTION_INFO
        // catches up. Do not turn a successful connect into ECONNRESET here;
        // later poll/send/recv paths already re-check real socket state.
        sock_trace_tcp_info("connect-postcheck", sock);
    }
#endif

    if (sock->socket.domain == AF_LOCAL_) {
        fill_cred(&sock->socket.unix_cred);
        // A real connect(2) succeeds again on an AF_UNIX SOCK_DGRAM socket
        // (reconnecting to a new peer, or dissolving the association) --
        // unlike SOCK_STREAM, where a second connect on an already-connected
        // socket fails with EISCONN before ever reaching this point. unix_peer
        // can therefore legitimately already be set here; break the stale
        // mutual link instead of asserting (observed: this crashed the whole
        // process on a legitimate reconnect during a stress-ng sweep).
        if (sock->socket.unix_peer != NULL) {
            lock(&peer_lock, 0);
            struct fd *old_peer = sock->socket.unix_peer;
            if (old_peer != NULL)
                old_peer->socket.unix_peer = NULL;
            sock->socket.unix_peer = NULL;
            unlock(&peer_lock);
        }
        // Send a pointer to ourselves so the accept side can link unix_peer
        // later, but do not wait for that acknowledgement here. Linux connect()
        // completes once the transport connection exists; waiting for accept()
        // to run can wedge clients on daemons that accept asynchronously.
        //
        // Only connection-oriented sockets do this: a datagram socket has no
        // accept() on the peer to consume the token, so writing it would inject
        // a spurious 8-byte datagram ahead of the application's data (e.g. into
        // a syslogd reading /dev/log). SO_PEERCRED already tolerates the
        // resulting NULL unix_peer for datagram sockets.
        if (sock->socket.type == SOCK_STREAM_ || sock->socket.type == SOCK_SEQPACKET_) {
            int peer_err = unix_socket_send_peer_token(sock);
            if (peer_err < 0) {
                sock_trace("connect", sock, -1, peer_err);
                sock_debug_event("connect-peer-token", sock, -1, peer_err);
                return peer_err;
            }
        }
    }

    sock_trace("connect", sock, err, 0);
    sock_trace_sockaddr("local", sock->real_fd);
    sock_trace_sockaddr("peer", sock->real_fd);
    sock_debug_event("connect", sock, err, 0);
    return err;
}

int_t sys_connect(fd_t sock_fd, addr_t sockaddr_addr, uint_t sockaddr_len) {
    return sys_connect_common(sock_fd, sockaddr_addr, sockaddr_len);
}

int_t sys_connect_guest(fd_t sock_fd, guest_addr_t sockaddr_addr, uint_t sockaddr_len) {
    return sys_connect_common(sock_fd, sockaddr_addr, sockaddr_len);
}

int_t sys_listen(fd_t sock_fd, int_t backlog) {
    STRACE("listen(%d, %d)", sock_fd, backlog);
    int_t sock_err;
    struct fd *sock = sock_getfd(sock_fd, &sock_err);
    if (sock == NULL)
        return sock_err;
    int err = listen(sock->real_fd, backlog);
    if (err < 0)
        return errno_map();
    sock->socket.listening = true;
    sock->sockrestart.backlog = backlog;
    sockrestart_begin_listen(sock);
    return err;
}

int_t sys_accept(fd_t sock_fd, addr_t sockaddr_addr, addr_t sockaddr_len_addr) {
    return sys_accept4(sock_fd, sockaddr_addr, sockaddr_len_addr, 0);
}

int_t sys_accept_guest(fd_t sock_fd, guest_addr_t sockaddr_addr, guest_addr_t sockaddr_len_addr) {
    return sys_accept4_guest(sock_fd, sockaddr_addr, sockaddr_len_addr, 0);
}

static int_t sys_accept4_common(fd_t sock_fd, guest_addr_t sockaddr_addr, guest_addr_t sockaddr_len_addr, int_t flags) {
    STRACE("accept4(%d, 0x%llx, 0x%llx, %#x)", sock_fd,
            (unsigned long long) sockaddr_addr,
            (unsigned long long) sockaddr_len_addr, flags);
    if (flags & ~(O_CLOEXEC_ | O_NONBLOCK_))
        return _EINVAL;
    int_t sock_err;
    struct fd *sock = sock_getfd(sock_fd, &sock_err);
    if (sock == NULL)
        return sock_err;
    dword_t sockaddr_len = 0;
    if (sockaddr_addr != 0) {
        if (user_get(sockaddr_len_addr, sockaddr_len))
            return _EFAULT;
    }

    char sockaddr[sockaddr_len];
    int client =0;
    // Hoisted out of the wait block below: the SA_RESTART decision on the
    // error path needs it (a timed accept is never restarted).
    bool has_deadline = false;
    if (!sock->socket.listening) {
        // Not a listening socket: the host accept() fails immediately
        // (EINVAL/EOPNOTSUPP) and can never block, so take the direct path.
        // The poll()-based wait below would otherwise hang here: an empty
        // non-listening socket is not readable, but Linux returns the error
        // right away.
        errno = 0;
        client = accept(sock->real_fd,
                        sockaddr_addr != 0 ? (void *) sockaddr : NULL,
                        sockaddr_addr != 0 ? &sockaddr_len : NULL);
        if (client < 0)
            return errno_map();
    } else TASK_MAY_BLOCK {
        // Never block inside the host accept(). Darwin's accept() ignores
        // SO_RCVTIMEO (Linux honors it, per accept(2)), and systemd's
        // userdb/homed workers rely on exactly that: a BLOCKING listener with
        // a receive timeout, where the periodic EAGAIN tick drives the whole
        // worker lifecycle (idle exit, retirement, respawn). Blocked-forever
        // workers wedged systemd-userdbd on device. Instead, keep the host
        // listener nonblocking and emulate the wait with poll(): guest
        // O_NONBLOCK -> immediate EAGAIN; SO_RCVTIMEO armed -> EAGAIN at the
        // deadline; plain blocking -> wait in poll (still interruptible by
        // the SIGUSR1 poke). This also gives Linux thundering-herd semantics:
        // when several tasks accept from one listener, the losers of a race
        // get EAGAIN/poll-again instead of re-blocking in the host kernel.
        //
        // O_NONBLOCK is a property of the open file description in both
        // worlds, and only listeners reach this path, so forcing the host
        // flag on permanently is safe and shared-fd-race-free (unlike a
        // set/restore dance racing sibling accepts on the same description).
        int host_fl = fcntl(sock->real_fd, F_GETFL, 0);
        if (host_fl >= 0 && !(host_fl & O_NONBLOCK))
            (void) fcntl(sock->real_fd, F_SETFL, host_fl | O_NONBLOCK);

        bool guest_nonblock = fd_getflags(sock) & O_NONBLOCK_;
        // The receive timeout lives on the (shared) host description; reading
        // it back at accept time needs no new guest-side state and naturally
        // tracks fork/exec fd inheritance.
        struct timeval rcvtimeo = {0, 0};
        socklen_t rcvtimeo_len = sizeof(rcvtimeo);
        if (getsockopt(sock->real_fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, &rcvtimeo_len) < 0)
            rcvtimeo = (struct timeval) {0, 0};
        has_deadline = !guest_nonblock && (rcvtimeo.tv_sec != 0 || rcvtimeo.tv_usec != 0);
        struct timespec deadline;
        if (has_deadline) {
            deadline = timespec_now(CLOCK_MONOTONIC);
            deadline.tv_sec += rcvtimeo.tv_sec;
            deadline.tv_nsec += (long) rcvtimeo.tv_usec * 1000;
            if (deadline.tv_nsec >= 1000000000L) {
                deadline.tv_sec++;
                deadline.tv_nsec -= 1000000000L;
            }
        }

        // A SIGUSR1 poke alone is NOT enough to wake this sleep. SIGUSR1 does
        // not queue and doubles as the TLB/quiesce shootdown poke, and a poke
        // aimed at a thread already blocked inside the host poll() is simply
        // never delivered: measured with N tasks parked in accept(2) and all
        // SIGKILLed, every pthread_kill returned 0 to a distinct, correct,
        // started thread, yet only the first one or two ever ran the handler
        // and the rest slept on with SIGKILL pending. fs/poll.c already
        // defends against this with the non-lossy notify pipe published below,
        // and fs/real.c with a 100ms timeout. Publishing the pipe lets
        // deliver_signal's existing poll_notify_poke() tear this wait out even
        // when the poke is lost, with no periodic wakeups, so an idle listener
        // still sleeps indefinitely.
        //
        // The rest of the bug class -- the recv and send paths, which blocked
        // directly in the host recvmsg/sendmsg, and socket_wait_ready()'s
        // infinite poll -- got the same treatment afterwards: the host
        // description is kept nonblocking (socket_force_host_nonblock) so all
        // of them wait in socket_wait_ready, which publishes this same pipe.
        // See socket_kill.c.
        //
        // Created lazily: a guest-O_NONBLOCK accept breaks at the first EAGAIN
        // below and must not pay for a pipe it will never wait on.
        int notify_pipe[2] = {-1, -1};

        // sockrestart_end_listen_wait() and friends take locks that can
        // clobber errno, so the errno to report is carried in fail_errno and
        // restored just before errno_map() below.
        int fail_errno = 0;
        bool retry;
        do {
            retry = false;
            sockrestart_begin_listen_wait(sock);
            errno = 0;
            client = accept(sock->real_fd,
                            sockaddr_addr != 0 ? (void *) sockaddr : NULL,
                            sockaddr_addr != 0 ? &sockaddr_len : NULL);
            int accept_errno = errno;
            sockrestart_end_listen_wait(sock);
            if (client >= 0)
                break;
            fail_errno = accept_errno;
            if (accept_errno == EAGAIN || accept_errno == EWOULDBLOCK) {
                if (guest_nonblock)
                    break; // EAGAIN through to the guest
                int poll_timeout = -1;
                if (has_deadline) {
                    struct timespec now = timespec_now(CLOCK_MONOTONIC);
                    long long remaining_ms =
                        (long long) (deadline.tv_sec - now.tv_sec) * 1000 +
                        (deadline.tv_nsec - now.tv_nsec) / 1000000;
                    if (remaining_ms <= 0)
                        break; // timeout expired: EAGAIN, matching Linux
                    poll_timeout = remaining_ms > INT_MAX ? INT_MAX : (int) remaining_ms;
                }
                if (notify_pipe[0] < 0 && pipe(notify_pipe) == 0) {
                    fcntl(notify_pipe[0], F_SETFL, O_NONBLOCK);
                    fcntl(notify_pipe[1], F_SETFL, O_NONBLOCK);
                    // Same lock deliver_signal_unlocked_locked reads the fd
                    // under, so it can never observe a closed or reused one
                    // (cleared below before close).
                    lock(&current->sighand->lock, 0);
                    current->poll_notify_fd = notify_pipe[1];
                    unlock(&current->sighand->lock);
                }
                struct pollfd pfd[2];
                int npfd = 1;
                pfd[0] = (struct pollfd) { .fd = sock->real_fd, .events = POLLIN };
                if (notify_pipe[0] >= 0) {
                    pfd[1] = (struct pollfd) { .fd = notify_pipe[0], .events = POLLIN };
                    npfd = 2;
                }
                sockrestart_begin_listen_wait(sock);
                // Sleep under the same discipline as socket_wait_ready() and
                // fs/poll.c: block SIGUSR1, arm the sigunwind point, re-check
                // for an already-pending guest signal, then force-unblock and
                // wait. This was the one blocking wait in the tree that just
                // called the host poll() bare, which is wrong twice over.
                // Nothing guaranteed SIGUSR1 was unblocked in the host mask
                // (guest sigprocmask only moves the guest mask -- the reason
                // poll.c:746 and socket_blocking_syscall_begin force-unblock),
                // and with no unwind point armed the wake depended entirely on
                // the host poll() returning EINTR. Worse, the pending check and
                // the sleep were not atomic: a poke landing between the
                // accept() EAGAIN above and this poll() ran a handler that did
                // nothing, and the task then slept forever with SIGKILL already
                // pending -- permanently deaf, since further kills only re-sent
                // the same lost poke. `nc -l -p N` survived kill -9 this way
                // (only an incoming connection could still wake it). Same bug
                // class as the raw host write in fs/real.c's realfs_wait_writable.
                sigset_t oldmask;
                int nready, poll_errno;
                if (!socket_blocking_syscall_begin(&oldmask)) {
                    nready = -1;
                    poll_errno = EINTR;
                } else {
                    errno = 0;
                    nready = poll(pfd, npfd, poll_timeout);
                    poll_errno = errno;
                    socket_blocking_syscall_end();
                }
                sockrestart_end_listen_wait(sock);
                // Drain the notify pipe so a single poke does not leave the
                // next poll instantly readable and spin the retry loop. The
                // wake itself needs no interpretation: the retry re-runs the
                // accept and then socket_blocking_syscall_begin's pre-sleep
                // pending check, which is what actually reports the signal.
                if (npfd == 2 && nready > 0 && (pfd[1].revents & POLLIN)) {
                    char drain[64];
                    while (read(notify_pipe[0], drain, sizeof drain) > 0)
                        continue;
                }
                bool resumed = sockrestart_should_restart_listen_wait(1);
                if (nready < 0 && poll_errno == EINTR &&
                        !resumed && socket_guest_signal_pending()) {
                    // A real pending guest signal MUST surface as EINTR so the
                    // guest can run its handler; a purely spurious poke (e.g.
                    // a TLB-shootdown SIGUSR1) or an iOS suspend/resume
                    // sockrestart punt just re-enters the wait. Mirrors
                    // socket_wait_ready()/fs/poll.c; see signal_restart.c and
                    // the stress-ng --syscall one-shot SIGALRM regression for
                    // why a forced restart with a signal pending is wrong.
                    fail_errno = EINTR;
                    break;
                }
                // Readable, timeout-race, punt, or spurious wake: try the
                // accept again; the deadline check above bounds the loop.
                retry = true;
            } else if (accept_errno == EINTR) {
                bool resumed = sockrestart_should_restart_listen_wait(1);
                retry = resumed || !socket_guest_signal_pending();
            }
        } while (retry);
        // Unpublish before closing so a concurrent deliver_signal (which reads
        // the fd under sighand->lock) can never poke a closed or reused fd.
        // Done before restoring fail_errno: lock() can clobber errno.
        if (notify_pipe[0] >= 0) {
            lock(&current->sighand->lock, 0);
            current->poll_notify_fd = -1;
            unlock(&current->sighand->lock);
            close(notify_pipe[0]);
            close(notify_pipe[1]);
        }
        if (client < 0)
            errno = fail_errno;
    }
    if (client < 0) {
        // SA_RESTART: accept() is restartable, but not with SO_RCVTIMEO armed
        // (signal(7)) -- restarting would silently extend the guest's timeout.
        int mapped = errno_map();
        if (mapped == _EINTR && !has_deadline)
            mapped = signal_restart_or_eintr(mapped);
        return mapped;
    }

    // BSD accepted sockets inherit O_NONBLOCK from the listener (which is
    // now kept permanently nonblocking above); Linux accepted sockets start
    // with fresh flags. Strip it -- accept4's own SOCK_NONBLOCK is applied
    // via f_install in sock_fd_create below.
    int client_fl = fcntl(client, F_GETFL, 0);
    if (client_fl >= 0 && (client_fl & O_NONBLOCK))
        (void) fcntl(client, F_SETFL, client_fl & ~O_NONBLOCK);

    if (sockaddr_addr != 0) {
        int err = sockaddr_write(sockaddr_addr, sockaddr, sizeof(sockaddr), &sockaddr_len);
        if (err < 0)
            return client;
        if (user_put(sockaddr_len_addr, sockaddr_len))
            return _EFAULT;
    }

    fd_t client_f = sock_fd_create(client,
            sock->socket.domain, sock->socket.type | flags, sock->socket.protocol);
    if (client_f < 0)
        close(client);

    if (sock->socket.domain == AF_LOCAL_) {
        struct fd *client_fd = f_get(client_f);
        fill_cred(&client_fd->socket.unix_cred);
        client_fd->socket.unix_name_len = sock->socket.unix_name_len;
        memcpy(client_fd->socket.unix_name, sock->socket.unix_name, sock->socket.unix_name_len);
        client_fd->socket.unix_peer_pending = true;
        client_fd->socket.unix_peer_off = 0;
        int peer_err = unix_socket_finish_peer(client_fd);
        if (peer_err < 0 && peer_err != _EAGAIN)
            STRACE("accept4(%d) deferred unix peer link err=%d", sock_fd, peer_err);
    }

    return client_f;
}

int_t sys_accept4(fd_t sock_fd, addr_t sockaddr_addr, addr_t sockaddr_len_addr, int_t flags) {
    return sys_accept4_common(sock_fd, sockaddr_addr, sockaddr_len_addr, flags);
}

int_t sys_accept4_guest(fd_t sock_fd, guest_addr_t sockaddr_addr, guest_addr_t sockaddr_len_addr, int_t flags) {
    return sys_accept4_common(sock_fd, sockaddr_addr, sockaddr_len_addr, flags);
}

static void copy_unix_name(char *sockaddr, dword_t *sockaddr_len, struct fd *sock) {
    struct sockaddr_ *fake_addr = (void *) sockaddr;
    fake_addr->family = PF_LOCAL_;

    size_t data_len = *sockaddr_len - offsetof(struct sockaddr_, data);
    size_t name_len = sock->socket.unix_name_len;
    if (name_len > data_len)
        name_len = data_len;
    memset(fake_addr->data, 0, data_len);
    memcpy(fake_addr->data, sock->socket.unix_name, name_len);
    *sockaddr_len = offsetof(struct sockaddr_, data) + name_len;
}

static int copy_unix_peer_name(char *sockaddr, dword_t *sockaddr_len, struct fd *sock) {
    int err = unix_socket_finish_peer(sock);
    if (err < 0 && err != _ENOTCONN)
        return err;

    lock(&peer_lock, 0);
    struct fd *peer = sock->socket.unix_peer;
    if (peer != NULL)
        copy_unix_name(sockaddr, sockaddr_len, peer);
    unlock(&peer_lock);

    return peer == NULL ? _ENOTCONN : 0;
}

static int_t sys_getsockname_common(fd_t sock_fd, guest_addr_t sockaddr_addr, guest_addr_t sockaddr_len_addr) {
    STRACE("getsockname(%d, 0x%llx, 0x%llx)", sock_fd,
            (unsigned long long) sockaddr_addr,
            (unsigned long long) sockaddr_len_addr);
    int_t sock_err;
    struct fd *sock = sock_getfd(sock_fd, &sock_err);
    if (sock == NULL)
        return sock_err;
    dword_t sockaddr_len;
    if (user_get(sockaddr_len_addr, sockaddr_len))
        return _EFAULT;
    // A fixed-size local scratch buffer, deliberately NOT sized by the
    // guest-requested sockaddr_len (which real callers may legitimately pass
    // as 0, or anything smaller than sizeof(sa_family_t), to probe the
    // required length -- getsockname/getpeername truncate on a too-small
    // buffer rather than erroring). copy_unix_name and sockaddr_write below
    // both read/write a family field at the front of this buffer before any
    // truncation is applied; sizing it by the untrusted guest length made
    // that an uninitialized-read (generic path) or, for copy_unix_name, an
    // unsigned-underflow-driven out-of-bounds memset/memcpy (PF_LOCAL path)
    // whenever the guest asked for a very small buffer.
    char sockaddr[sizeof(struct sockaddr_storage)];

    if (sock->socket.domain == AF_NETLINK_) {
        if (sockaddr_len < sizeof(struct sockaddr_nl_))
            return _EINVAL;
        struct sockaddr_nl_ *addr = (struct sockaddr_nl_ *) sockaddr;
        *addr = (struct sockaddr_nl_) {
            .nl_family = AF_NETLINK_,
            .nl_pid = sock->socket.netlink_port_id,
            .nl_groups = sock->socket.netlink_groups,
        };
        sockaddr_len = sizeof(*addr);
        if (user_write(sockaddr_addr, sockaddr, sockaddr_len))
            return _EFAULT;
        if (user_put(sockaddr_len_addr, sockaddr_len))
            return _EFAULT;
        return 0;
    }

    // if this is a unix socket, return the same string passed to bind
    if (sock->socket.domain == PF_LOCAL_) {
        dword_t real_len = sizeof(sockaddr);
        copy_unix_name(sockaddr, &real_len, sock);
        dword_t write_len = real_len < sockaddr_len ? real_len : sockaddr_len;
        if (user_write(sockaddr_addr, sockaddr, write_len))
            return _EFAULT;
        if (user_put(sockaddr_len_addr, real_len))
            return _EFAULT;
        return 0;
    }

    dword_t real_len = sizeof(sockaddr);
    int res = getsockname(sock->real_fd, (void *) sockaddr, &real_len);
    if (res < 0)
        return errno_map();

    if (sock->socket.inet_nat_bound) {
        // Report the guest-visible endpoint, not the NAT'd host one --
        // see inet_nat_bind_fallback.
        struct sockaddr_in *sin = (struct sockaddr_in *) sockaddr;
        if (sin->sin_family == AF_INET) {
            sin->sin_addr.s_addr = sock->socket.inet_nat_bound_addr;
            sin->sin_port = sock->socket.inet_nat_bound_port;
        }
    }

    int err = sockaddr_write(sockaddr_addr, sockaddr, sockaddr_len, &real_len);
    if (err < 0)
        return err;
    if (user_put(sockaddr_len_addr, real_len))
        return _EFAULT;
    return res;
}

int_t sys_getsockname(fd_t sock_fd, addr_t sockaddr_addr, addr_t sockaddr_len_addr) {
    return sys_getsockname_common(sock_fd, sockaddr_addr, sockaddr_len_addr);
}

int_t sys_getsockname_guest(fd_t sock_fd, guest_addr_t sockaddr_addr, guest_addr_t sockaddr_len_addr) {
    return sys_getsockname_common(sock_fd, sockaddr_addr, sockaddr_len_addr);
}

static int_t sys_getpeername_common(fd_t sock_fd, guest_addr_t sockaddr_addr, guest_addr_t sockaddr_len_addr) {
    STRACE("getpeername(%d, 0x%llx, 0x%llx)", sock_fd,
            (unsigned long long) sockaddr_addr,
            (unsigned long long) sockaddr_len_addr);
    int_t sock_err;
    struct fd *sock = sock_getfd(sock_fd, &sock_err);
    if (sock == NULL)
        return sock_err;
    dword_t sockaddr_len;
    if (user_get(sockaddr_len_addr, sockaddr_len))
        return _EFAULT;
    // See the matching comment in sys_getsockname_common: this must be a
    // fixed-size buffer, not one sized by the untrusted guest-requested
    // length. stress-ng's --sock stressor's getpeername() calls with a
    // small/zero-sized buffer were tripping exactly this: copy_unix_name and
    // sockaddr_write read a family field at the front of the buffer before
    // any truncation happened, which was uninitialized (or, for
    // copy_unix_name's unsigned length subtraction, drove an out-of-bounds
    // write) whenever the guest's requested length was too small to hold it.
    char sockaddr[sizeof(struct sockaddr_storage)];

    if (sock->socket.domain == AF_NETLINK_) {
        if (sockaddr_len < sizeof(struct sockaddr_nl_))
            return _EINVAL;
        struct sockaddr_nl_ sockaddr = {
            .nl_family = AF_NETLINK_,
            .nl_pid = 0,
            .nl_groups = 0,
        };
        dword_t out_len = sizeof(sockaddr);
        if (user_write(sockaddr_addr, &sockaddr, sizeof(sockaddr)))
            return _EFAULT;
        if (user_put(sockaddr_len_addr, out_len))
            return _EFAULT;
        return 0;
    }

    if (sock->socket.domain == PF_LOCAL_) {
        dword_t real_len = sizeof(sockaddr);
        int err = copy_unix_peer_name(sockaddr, &real_len, sock);
        if (err < 0)
            return err;
        dword_t write_len = real_len < sockaddr_len ? real_len : sockaddr_len;
        if (user_write(sockaddr_addr, sockaddr, write_len))
            return _EFAULT;
        if (user_put(sockaddr_len_addr, real_len))
            return _EFAULT;
        return 0;
    }

    dword_t real_len = sizeof(sockaddr);
    int res = getpeername(sock->real_fd, (void *) sockaddr, &real_len);
    if (res < 0) {
        // The host call above always uses our own valid, full-size local
        // buffer -- never the guest's requested addrlen -- so a host EINVAL
        // here can't be a bad-addrlen complaint (Linux's only documented
        // EINVAL case for this syscall). It's a Darwin-specific quirk: a TCP
        // socket that's still logically connected from Linux's point of view
        // (e.g. after a local shutdown(), or occasionally when the peer's
        // connection state changes concurrently -- stress-ng's --sock
        // stressor hits this once per several thousand connect/getpeername
        // iterations) but that Darwin's getpeername refuses with EINVAL
        // where Linux would either still succeed or report ENOTCONN. Map it
        // to ENOTCONN -- at minimum a real Linux errno for this call, and
        // exactly right for the concurrent-disconnect race.
        if (errno == EINVAL)
            return _ENOTCONN;
        return errno_map();
    }

    if (sock->socket.inet_nat_peer) {
        // Report the endpoint the guest connected to, not the NAT'd host
        // one it was silently rewritten to -- see inet_nat_rewrite_dest.
        struct sockaddr_in *sin = (struct sockaddr_in *) sockaddr;
        if (sin->sin_family == AF_INET) {
            sin->sin_addr.s_addr = sock->socket.inet_nat_peer_addr;
            sin->sin_port = sock->socket.inet_nat_peer_port;
        }
    }

    int err = sockaddr_write(sockaddr_addr, sockaddr, sockaddr_len, &real_len);
    if (err < 0)
        return err;
    if (user_put(sockaddr_len_addr, real_len))
        return _EFAULT;
    return res;
}

int_t sys_getpeername(fd_t sock_fd, addr_t sockaddr_addr, addr_t sockaddr_len_addr) {
    return sys_getpeername_common(sock_fd, sockaddr_addr, sockaddr_len_addr);
}

int_t sys_getpeername_guest(fd_t sock_fd, guest_addr_t sockaddr_addr, guest_addr_t sockaddr_len_addr) {
    return sys_getpeername_common(sock_fd, sockaddr_addr, sockaddr_len_addr);
}

static int_t sys_socketpair_common(dword_t domain, dword_t type, dword_t protocol, guest_addr_t sockets_addr) {
    STRACE("socketpair(%d, %d, %d, 0x%llx)", domain, type, protocol,
            (unsigned long long) sockets_addr);
    int real_domain = sock_family_to_real(domain);
    if (real_domain < 0)
        return _EINVAL;
    int real_type = sock_type_to_real(type, protocol);
    if (real_type < 0)
        return _EINVAL;

    int sockets[2];
    int err;
    if (seqpacket_denied_by_host(domain, type, protocol)) {
        err = -1;
        errno = EPERM;
    } else {
        err = socketpair(real_domain, real_type, protocol, sockets);
    }
#if defined(__APPLE__)
    if (err < 0 && unix_seqpacket_fallback_needed(domain, type, protocol, errno))
        err = socketpair(real_domain, SOCK_STREAM, protocol, sockets);
#endif
    if (err < 0)
        return errno_map();

    // Do NOT hold peer_lock across sock_fd_create: its failure path
    // (f_install with the fd table full) calls fd_close -> sock_close, which
    // takes peer_lock itself -- a self-deadlock on this non-recursive lock.
    // stress-ng --sockpair exhausts fds by design and hit this every run,
    // freezing the whole process (every other socketpair/sock_close/kill
    // piled up behind the dead lock holder). The host socketpair() above is
    // already fully connected, so the only window this opens is iSH's own
    // unix_peer metadata lagging until the lock below -- harmless next to a
    // guaranteed deadlock.
    int fake_sockets[2];
    err = fake_sockets[0] = sock_fd_create(sockets[0], domain, type, protocol);
    if (fake_sockets[0] < 0)
        goto close_sockets;
    err = fake_sockets[1] = sock_fd_create(sockets[1], domain, type, protocol);
    if (fake_sockets[1] < 0)
        goto close_fake_0;
    struct fd *sock1 = f_get(fake_sockets[0]);
    struct fd *sock2 = f_get(fake_sockets[1]);
    fill_cred(&sock1->socket.unix_cred);
    fill_cred(&sock2->socket.unix_cred);
    lock(&peer_lock, 0);
    sock1->socket.unix_peer = sock2;
    sock2->socket.unix_peer = sock1;
    sock1->socket.unix_peer_cred = sock2->socket.unix_cred;
    sock2->socket.unix_peer_cred = sock1->socket.unix_cred;
    sock1->socket.unix_peer_cred_valid = true;
    sock2->socket.unix_peer_cred_valid = true;
    unlock(&peer_lock);

    err = _EFAULT;
    if (user_put(sockets_addr, fake_sockets))
        goto close_fake_1;

    STRACE(" [%d, %d]", fake_sockets[0], fake_sockets[1]);
    return 0;

close_fake_1:
    sys_close(fake_sockets[1]);
close_fake_0:
    sys_close(fake_sockets[0]);
close_sockets:
    close(sockets[0]);
    close(sockets[1]);
    return err;
}

int_t sys_socketpair(dword_t domain, dword_t type, dword_t protocol, addr_t sockets_addr) {
    return sys_socketpair_common(domain, type, protocol, sockets_addr);
}

int_t sys_socketpair_guest(dword_t domain, dword_t type, dword_t protocol, guest_addr_t sockets_addr) {
    return sys_socketpair_common(domain, type, protocol, sockets_addr);
}

static int_t sys_sendto_common(fd_t sock_fd, guest_addr_t buffer_addr, dword_t len, dword_t flags,
        guest_addr_t sockaddr_addr, dword_t sockaddr_len) {
    int_t sock_err;
    struct fd *sock = sock_getfd(sock_fd, &sock_err);
    if (sock == NULL)
        return sock_err;
    if (sock->socket.domain == AF_LOCAL_) {
        int peer_err = unix_socket_finish_peer(sock);
        if (peer_err < 0)
            return peer_err;
    }
    char *buffer = malloc(len + 1);
    if (user_read(buffer_addr, buffer, len))
        return _EFAULT;
    buffer[len] = '\0';
    STRACE("sendto(%d, \"%.100s\", %d, %d, 0x%x, %d)", sock_fd, buffer, len, flags, sockaddr_addr, sockaddr_len);
    int real_flags = sock_flags_to_real(flags);
    int err = _EINVAL;
    if (real_flags < 0)
        goto error;
    // A socket that fell back to the built-in /dev/log or initctl sink (no real
    // listener at connect time) discards its writes.
    if (sock_is_devlog_sink(sock) || sock_is_initctl_sink(sock)) {
        free(buffer);
        return len;
    }
    // A connectionless sendto() to /dev/log or initctl delivers for real if a
    // daemon is bound there, and otherwise falls back to discarding.
    bool sendto_devlog_fallback = sock->socket.domain == AF_LOCAL_ &&
        (guest_sockaddr_is_devlog(sockaddr_addr, sockaddr_len) ||
         guest_sockaddr_is_initctl(sockaddr_addr, sockaddr_len));
    struct sockaddr_max_ sockaddr;
    if (sockaddr_addr) {
        err = sockaddr_read(sockaddr_addr, &sockaddr, &sockaddr_len);
        if (err < 0) {
            if (sendto_devlog_fallback) {
                free(buffer);
                return len;
            }
            goto error;
        }
        if (sock->socket.domain == AF_INET_)
            inet_nat_rewrite_dest(sock, &sockaddr);
    }

    if (sock->socket.domain == AF_NETLINK_) {
        struct iovec iov = {
            .iov_base = buffer,
            .iov_len = len,
        };
        struct msghdr msg = {
            .msg_name = sockaddr_addr ? (void *) &sockaddr : NULL,
            .msg_namelen = sockaddr_len,
            .msg_iov = &iov,
            .msg_iovlen = 1,
        };
        err = netlink_handle_sendmsg(sock, &msg);
        free(buffer);
        return err;
    }

    // AF_LOCAL datagrams carry the sender's guest creds in-band; see
    // struct unix_dgram_cred_hdr.
    bool unix_dgram = sock_is_unix_dgram(sock);
    struct unix_dgram_cred_hdr dgram_hdr;
    struct iovec dgram_iov[2];
    struct msghdr dgram_msg = {};
    if (unix_dgram) {
        unix_dgram_cred_hdr_fill(&dgram_hdr);
        dgram_iov[0] = (struct iovec) {.iov_base = &dgram_hdr, .iov_len = sizeof(dgram_hdr)};
        dgram_iov[1] = (struct iovec) {.iov_base = buffer, .iov_len = len};
        dgram_msg.msg_iov = dgram_iov;
        dgram_msg.msg_iovlen = 2;
        if (sockaddr_addr != 0) {
            dgram_msg.msg_name = (void *) &sockaddr;
            dgram_msg.msg_namelen = sockaddr_len;
        }
    }
    ssize_t res = 0;
    // A blocking send on a byte stream does not return until the whole request
    // is queued. The host used to run that loop for us; now that the host call
    // is nonblocking so a fatal signal can reach this task, the loop is ours.
    // Only streams: a datagram send is all-or-nothing.
    bool send_all = socket_call_is_blocking(sock, real_flags) && socket_is_stream(sock);
    size_t sent = 0;
    struct socket_io_wait wait = {};
    socket_force_host_nonblock(sock);
    TASK_MAY_BLOCK {
        while (1) {
            errno = 0;
            if (unix_dgram) {
                res = sendmsg(sock->real_fd, &dgram_msg, real_flags);
            } else if (sockaddr_addr == 0) {
                res = send(sock->real_fd, buffer + sent, len - sent, real_flags);
            } else {
                res = sendto(sock->real_fd, buffer + sent, len - sent, real_flags,
                             (void *) &sockaddr, sockaddr_len);
            }
            if (res >= 0) {
                if (!send_all)
                    break;
                sent += (size_t) res;
                if (res == 0 || sent >= len) {
                    res = (ssize_t) sent;
                    break;
                }
                int wait_err = socket_wait_ready(sock, POLLOUT, &wait);
                if (wait_err < 0) {
                    res = (ssize_t) sent; // interrupted mid-send: partial count
                    break;
                }
                continue;
            }
            if (socket_should_retry_io_eintr(sock, real_flags))
                continue;
            if (socket_should_retry_io_eagain(sock, real_flags)) {
                int wait_err = socket_wait_ready(sock, POLLOUT, &wait);
                if (wait_err < 0) {
                    if (sent > 0) {
                        res = (ssize_t) sent;
                        break;
                    }
                    res = wait_err;
                    errno = 0;
                    break;
                }
                continue;
            }
            // Linux reports the bytes that did get through and leaves the
            // error to be rediscovered by the next call.
            if (sent > 0)
                res = (ssize_t) sent;
            break;
        }
    }
    free(buffer);
    if (res < 0) {
        if (res > -4096 && res < 0 && errno == 0)
            return res;
        if (socket_should_map_unix_eperm_to_eagain(sock, real_flags)) {
            sock_trace("sendto", sock, -1, _EAGAIN);
            sock_debug_event("sendto", sock, -1, _EAGAIN);
            sock_x11_event("sendto-eagain", sock, -1, _EAGAIN, len);
            return _EAGAIN;
        }
        // MSG_NOSIGNAL: EPIPE without the guest's SIGPIPE.
        int mapped_err = errno_map_flags(flags & MSG_NOSIGNAL_);
        sock_translate_err(sock, &mapped_err);
        // Linux returns ENOTCONN for a send() on an unconnected AF_UNIX
        // datagram socket with no destination address; Darwin returns
        // EDESTADDRREQ. (For AF_INET both kernels use EDESTADDRREQ.)
        if (sock->socket.domain == AF_LOCAL_ && sockaddr_addr == 0 &&
                mapped_err == _EDESTADDRREQ)
            mapped_err = _ENOTCONN;
        // A /dev/log or initctl path whose socket exists but has no live reader
        // (e.g. a stale socket left by a dead daemon) falls back to discarding.
        if (sendto_devlog_fallback &&
                (mapped_err == _ECONNREFUSED || mapped_err == _ENOTCONN || mapped_err == _ENOENT))
            return len;
        sock_trace("sendto", sock, -1, mapped_err);
        sock_debug_event("sendto", sock, -1, mapped_err);
        if (mapped_err == _EAGAIN)
            sock_x11_event("sendto-eagain", sock, -1, mapped_err, len);
        else
            sock_x11_event("sendto-err", sock, -1, mapped_err, len);
        return mapped_err;
    }
    // The in-band cred header is emulator plumbing, not payload.
    if (unix_dgram && res >= (ssize_t) sizeof(dgram_hdr))
        res -= sizeof(dgram_hdr);
    sock_trace("sendto", sock, res, 0);
    sock_debug_event("sendto", sock, res, 0);
    if ((size_t) res != len)
        sock_x11_event("sendto-short", sock, res, 0, len);
    return res;

error:
    free(buffer);
    return err;
}

int_t sys_sendto(fd_t sock_fd, addr_t buffer_addr, dword_t len, dword_t flags, addr_t sockaddr_addr, dword_t sockaddr_len) {
    return sys_sendto_common(sock_fd, buffer_addr, len, flags, sockaddr_addr, sockaddr_len);
}

int_t sys_sendto_guest(fd_t sock_fd, guest_addr_t buffer_addr, dword_t len, dword_t flags, guest_addr_t sockaddr_addr, dword_t sockaddr_len) {
    return sys_sendto_common(sock_fd, buffer_addr, len, flags, sockaddr_addr, sockaddr_len);
}

static int_t sys_recvfrom_common(fd_t sock_fd, guest_addr_t buffer_addr, dword_t len, dword_t flags,
        guest_addr_t sockaddr_addr, guest_addr_t sockaddr_len_addr) {
    STRACE("recvfrom(%d, 0x%llx, %d, %d, 0x%llx, 0x%llx)", sock_fd,
            (unsigned long long) buffer_addr, len, flags,
            (unsigned long long) sockaddr_addr,
            (unsigned long long) sockaddr_len_addr);
    int_t sock_err;
    struct fd *sock = sock_getfd(sock_fd, &sock_err);
    if (sock == NULL)
        return sock_err;
    if (sock->socket.domain == AF_LOCAL_) {
        int peer_err = unix_socket_finish_peer(sock);
        if (peer_err < 0)
            return peer_err;
    }
    int real_flags = sock_flags_to_real(flags);
    if (real_flags < 0)
        return _EINVAL;
    // MSG_TRUNC on a message-based (datagram/seqpacket/raw) socket must return
    // the REAL datagram length, even though only `len` bytes are copied. Darwin
    // ignores MSG_TRUNC as a recv input flag, so we measure the true length by
    // receiving into a buffer large enough to hold the whole datagram and then
    // copy back only what the caller asked for. (Netlink has its own MSG_TRUNC
    // path.)
    bool dgram_trunc = (flags & MSG_TRUNC_) &&
        sock->socket.type != SOCK_STREAM_ && sock->socket.domain != AF_NETLINK_;
    size_t recv_cap = len;
    if (dgram_trunc && recv_cap < 65536)
        recv_cap = 65536;
    // AF_LOCAL datagrams arrive with an in-band cred header (see struct
    // unix_dgram_cred_hdr); receive into extra headroom and strip it below.
    bool unix_dgram = sock_is_unix_dgram(sock);
    if (unix_dgram)
        recv_cap += sizeof(struct unix_dgram_cred_hdr);
    uint_t sockaddr_len = 0;
    if (sockaddr_len_addr != 0)
        if (user_get(sockaddr_len_addr, sockaddr_len))
            return _EFAULT;
    if (sock_is_initctl_sink(sock)) {
        uint_t zero = 0;
        if (sockaddr_len_addr != 0)
            if (user_put(sockaddr_len_addr, zero))
                return _EFAULT;
        return 0;
    }

    char *buffer = malloc(recv_cap);
    char sockaddr[sockaddr_len];
    if (sock->socket.domain == AF_NETLINK_) {
        struct iovec iov = {
            .iov_base = buffer,
            .iov_len = len,
        };
        struct msghdr msg = {
            .msg_name = sockaddr_addr != 0 ? (void *) sockaddr : NULL,
            .msg_namelen = sockaddr_len,
            .msg_iov = &iov,
            .msg_iovlen = 1,
        };
        ssize_t netlink_res = netlink_handle_recvmsg(sock, &msg, flags);
        if (netlink_res < 0) {
            free(buffer);
            return (int_t) netlink_res;
        }
        if (netlink_res > 0 && user_write(buffer_addr, buffer, netlink_res)) {
            free(buffer);
            return _EFAULT;
        }
        free(buffer);
        if (sockaddr_addr != 0) {
            int err = netlink_sockaddr_write(sockaddr_addr, sockaddr, &msg.msg_namelen);
            if (err < 0)
                return err;
            sockaddr_len = msg.msg_namelen;
        }
        if (sockaddr_len_addr != 0)
            if (user_put(sockaddr_len_addr, sockaddr_len))
                return _EFAULT;
        return (int_t) netlink_res;
    }
    ssize_t res = 0;
    // MSG_WAITALL must not be handed to a nonblocking host socket: Darwin
    // returns EAGAIN rather than a short read when it cannot fill the buffer
    // (measured), so passing it through would spin forever on a stream that
    // never delivers the whole request. Accumulate here instead. Streams in
    // blocking mode only, which is everywhere Linux honors the flag at all.
    bool waitall = (real_flags & MSG_WAITALL) &&
        socket_call_is_blocking(sock, real_flags) && socket_is_stream(sock);
    bool waitall_peek = waitall && (real_flags & MSG_PEEK);
    int host_flags = waitall ? (real_flags & ~MSG_WAITALL) : real_flags;
    size_t got = 0;
    struct socket_io_wait wait = {};
    socket_force_host_nonblock(sock);
    TASK_MAY_BLOCK {
        while (1) {
            sigset_t oldmask;
            if (!socket_blocking_syscall_begin(&oldmask)) {
                res = errno_map();
                errno = 0;
                break;
            }
            errno = 0;
            if (sockaddr_addr == 0 && sockaddr_len_addr == 0) {
                res = recv(sock->real_fd, buffer + got, recv_cap - got, host_flags);
            } else {
                res = recvfrom(sock->real_fd, buffer + got, recv_cap - got, host_flags,
                               sockaddr_addr != 0 ? (void *) sockaddr : NULL,
                               sockaddr_len_addr != 0 ? &sockaddr_len : NULL);
            }
            socket_blocking_syscall_end();
            if (res >= 0) {
                if (!waitall)
                    break;
                if (waitall_peek) {
                    // A peek always reads from the front of the stream, so
                    // there is nothing to accumulate: re-peek the whole
                    // request until it is all there (or the peer hangs up).
                    if (res == 0 || (size_t) res >= len)
                        break;
                } else {
                    got += (size_t) res;
                    if (res == 0 || got >= len) {
                        res = (ssize_t) got;
                        break;
                    }
                }
                int wait_err = socket_wait_ready(sock, POLLIN, &wait);
                if (wait_err < 0) {
                    // Interrupted or timed out part way: hand back what
                    // arrived. Only an empty result may report the error.
                    if (!waitall_peek)
                        res = (ssize_t) got;
                    break;
                }
                continue;
            }
            if (socket_should_retry_io_eintr(sock, real_flags))
                continue;
            if (socket_should_retry_io_eagain(sock, real_flags)) {
                int wait_err = socket_wait_ready(sock, POLLIN, &wait);
                if (wait_err < 0) {
                    if (got > 0) {
                        res = (ssize_t) got;
                        break;
                    }
                    res = wait_err;
                    errno = 0;
                    break;
                }
                continue;
            }
            if (got > 0)
                res = (ssize_t) got;
            break;
        }
    }
    if (res <= 0) {
        // A pending socket error outranks both EOF and EAGAIN, and AOK's own
        // poll probe may already have taken it off the host. See
        // sock_take_pending_error.
        int pending = sock_take_pending_error(sock);
        if (pending != 0) {
            free(buffer);
            sock_trace("recvfrom", sock, -1, pending);
            sock_debug_event("recvfrom", sock, -1, pending);
            return pending;
        }
    }
    if (res < 0) {
        free(buffer);
        if (res > -4096 && res < 0 && errno == 0)
            return res;
        if (socket_should_map_unix_eperm_to_eagain(sock, real_flags)) {
            sock_trace("recvfrom", sock, -1, _EAGAIN);
            sock_debug_event("recvfrom", sock, -1, _EAGAIN);
            sock_x11_event("recvfrom-eagain", sock, -1, _EAGAIN, len);
            return _EAGAIN;
        }
        int mapped_err = errno_map();
        bool was_dead = sock->socket.conn_dead;
        sock_translate_err(sock, &mapped_err);
        // Already reported. A connection that is gone reads end-of-file from
        // here on, exactly as a reset one does on Linux and on the host.
        if (was_dead && mapped_err == _ECONNRESET) {
            sock_trace("recvfrom", sock, 0, 0);
            return 0;
        }
        sock_trace("recvfrom", sock, -1, mapped_err);
        sock_debug_event("recvfrom", sock, -1, mapped_err);
        if (mapped_err == _EAGAIN)
            sock_x11_event("recvfrom-eagain", sock, -1, mapped_err, len);
        else
            sock_x11_event("recvfrom-err", sock, -1, mapped_err, len);
        return mapped_err;
    }

    // Strip the in-band cred header off AF_LOCAL datagrams (this bare
    // recvfrom/recv surface has nowhere to deliver creds; recvmsg does).
    if (unix_dgram && res >= (ssize_t) sizeof(struct unix_dgram_cred_hdr)) {
        struct unix_dgram_cred_hdr stripped_hdr;
        memcpy(&stripped_hdr, buffer, sizeof(stripped_hdr));
        if (stripped_hdr.magic == UNIX_DGRAM_CRED_MAGIC) {
            res -= sizeof(stripped_hdr);
            memmove(buffer, buffer + sizeof(stripped_hdr), res);
            // A datagram consumed without a msghdr discards its SCM_RIGHTS
            // parcel (Linux closes the fds in that case); reclaim it so the
            // fds don't linger until the registry TTL.
            if (stripped_hdr.scm_cookie != 0) {
                struct scm *dropped = unix_dgram_scm_take(stripped_hdr.scm_cookie);
                if (dropped != NULL)
                    scm_free(dropped);
            }
        }
    }
    // With MSG_TRUNC on a datagram the real length (res) can exceed the user
    // buffer; copy only what fits but report the true length below.
    size_t copy_len = (size_t) res < len ? (size_t) res : len;
    if (copy_len > 0 && user_write(buffer_addr, buffer, copy_len)) {
        free(buffer);
        return _EFAULT;
    }
    free(buffer);
    if (sockaddr_addr != 0) {
        if (sock->socket.domain == AF_INET_)
            inet_nat_rewrite_src(sock, &sockaddr);
        int err = sockaddr_write(sockaddr_addr, sockaddr, sizeof(sockaddr), &sockaddr_len);
        if (err < 0)
            return err;
    }
    if (sockaddr_len_addr != 0)
        if (user_put(sockaddr_len_addr, sockaddr_len))
            return _EFAULT;
    sock_trace("recvfrom", sock, res, 0);
    sock_debug_event("recvfrom", sock, res, 0);
    if (res == 0)
        sock_x11_event("recvfrom-eof", sock, 0, 0, len);
    return res;
}

int_t sys_recvfrom(fd_t sock_fd, addr_t buffer_addr, dword_t len, dword_t flags, addr_t sockaddr_addr, addr_t sockaddr_len_addr) {
    return sys_recvfrom_common(sock_fd, buffer_addr, len, flags, sockaddr_addr, sockaddr_len_addr);
}

int_t sys_recvfrom_guest(fd_t sock_fd, guest_addr_t buffer_addr, dword_t len, dword_t flags, guest_addr_t sockaddr_addr, guest_addr_t sockaddr_len_addr) {
    return sys_recvfrom_common(sock_fd, buffer_addr, len, flags, sockaddr_addr, sockaddr_len_addr);
}

int_t sys_send(fd_t sock_fd, addr_t buf, dword_t len, int_t flags) {
    return sys_sendto(sock_fd, buf, len, flags, 0, 0);
}

int_t sys_recv(fd_t sock_fd, addr_t buf, dword_t len, int_t flags) {
    return sys_recvfrom(sock_fd, buf, len, flags, 0, 0);
}

int_t sys_shutdown(fd_t sock_fd, dword_t how) {
    STRACE("shutdown(%d, %d)", sock_fd, how);
    int_t sock_err;
    struct fd *sock = sock_getfd(sock_fd, &sock_err);
    if (sock == NULL)
        return sock_err;
    int err = shutdown(sock->real_fd, how);
    if (err < 0)
        return errno_map();
    return 0;
}

static void sock_init_emulation_defaults(struct fd *fd) {
    strcpy(fd->socket.tcp_congestion, DEFAULT_TCP_CONGESTION);
    fd->socket.ipv6_recverr_fd = -1;
    // Linux's net.core.rmem_default/wmem_default default.
    fd->socket.netlink_rcvbuf = 212992;
    fd->socket.netlink_sndbuf = 212992;
    lock_init(&fd->socket.netlink_reply_lock, "netlink_reply\0");
    fd->socket.netlink_notify_registered = false;
}

static int_t sys_setsockopt_guest_abi(fd_t sock_fd, dword_t level, dword_t option,
        guest_addr_t value_addr, dword_t value_len, enum guest_abi abi) {
    STRACE("setsockopt(%d, %d, %d, %#llx, %d)", sock_fd, level, option,
            (unsigned long long) value_addr, value_len);
    int_t sock_err;
    struct fd *sock = sock_getfd(sock_fd, &sock_err);
    if (sock == NULL)
        return sock_err;
    // value_len is the guest's raw setsockopt(2) optlen argument -- entirely
    // guest-controlled, with no upper bound of its own. No real socket option
    // is anywhere close to this size; reject before the VLA below turns an
    // absurd optlen (observed: 0xffffffff from a stress-ng fuzz) into an
    // unbounded stack allocation that blows through the guard page and
    // crashes the whole process.
    if (value_len > 4096)
        return _EINVAL;
    char value[value_len];
    if (user_read(value_addr, value, value_len))
        return _EFAULT;

    if (level == IPPROTO_ICMPV6 && option == ICMP6_FILTER_) {
        if (value_len != sizeof(sock->socket.icmp6_filter))
            return _EINVAL;
        if (sock->socket.type != SOCK_RAW_ || sock->socket.protocol != IPPROTO_ICMPV6)
            return _ENOPROTOOPT;
        memcpy(sock->socket.icmp6_filter, value, sizeof(sock->socket.icmp6_filter));
        sock->socket.icmp6_filter_valid = true;
        return 0;
    }
    if (level == IPPROTO_IP && option == IP_MTU_DISCOVER_) {
        if (value_len < sizeof(dword_t))
            return _EINVAL;
        sock->socket.ip_mtu_discover = *(dword_t *) value;
        return 0;
    }
    if (level == IPPROTO_IPV6 && option == IPV6_MTU_DISCOVER_) {
        if (value_len < sizeof(dword_t))
            return _EINVAL;
        sock->socket.ipv6_mtu_discover = *(dword_t *) value;
        return 0;
    }
    if (level == IPPROTO_IPV6 && option == IPV6_MTU_) {
        if (value_len < sizeof(dword_t))
            return _EINVAL;
        sock->socket.ipv6_mtu = *(dword_t *) value;
        return 0;
    }
    if (level == IPPROTO_IP && option == IP_RECVERR_) {
        if (value_len < sizeof(dword_t))
            return _EINVAL;
        sock->socket.ip_recverr = (*(dword_t *) value) != 0;
        return 0;
    }
    if (level == IPPROTO_IPV6 && option == IPV6_RECVERR_) {
        if (value_len < sizeof(dword_t))
            return _EINVAL;
        sock->socket.ipv6_recverr = (*(dword_t *) value) != 0;
        if (sock->socket.ipv6_recverr) {
            // Best-effort: the helper ICMPv6 socket only exists to capture
            // error packets from set-time on. Linux never fails this
            // setsockopt on an ordinary socket, and environments without
            // unprivileged ICMPv6 sockets (e.g. CI runners) must still be
            // able to set the flag; recvmsg's errqueue path retries the
            // open lazily and degrades to EOPNOTSUPP without it.
            (void) ipv6_recverr_fd_get(sock);
        } else if (sock->socket.ipv6_recverr_fd >= 0) {
            close(sock->socket.ipv6_recverr_fd);
            sock->socket.ipv6_recverr_fd = -1;
        }
        return 0;
    }
    if (level == IPPROTO_IP && option == IP_RETOPTS_) {
        // Linux ping probes this on IPv4 sockets. Darwin raw sockets do not
        // provide a compatible implementation, and the option is not required
        // for basic echo functionality.
        return 0;
    }
    if (level == IPPROTO_TCP && option == TCP_CONGESTION_) {
        size_t congestion_len = strnlen(value, value_len);
        if (congestion_len == 0 || congestion_len >= sizeof(sock->socket.tcp_congestion))
            return _EINVAL;
        memcpy(sock->socket.tcp_congestion, value, congestion_len);
        sock->socket.tcp_congestion[congestion_len] = '\0';
        return 0;
    }
    if (level == IPPROTO_TCP && option == TCP_DEFER_ACCEPT_) {
        if (value_len < sizeof(dword_t))
            return _EINVAL;
        sock->socket.tcp_defer_accept = *(dword_t *) value;
        return 0;
    }
    if (level == IPPROTO_TCP && option == TCP_FASTOPEN_) {
        if (value_len < sizeof(dword_t))
            return _EINVAL;
        return 0;
    }

    if (level == SOL_SOCKET_ && option == SO_PASSCRED_) {
        if (value_len < sizeof(dword_t))
            return _EINVAL;
        if (sock->socket.domain == AF_NETLINK_) {
            sock->socket.unix_passcred = (*(dword_t *) value) != 0;
            return 0;
        }
        if (sock->socket.domain != AF_LOCAL_)
            return _ENOPROTOOPT;
        sock->socket.unix_passcred = (*(dword_t *) value) != 0;
        return 0;
    }
    if (sock->socket.domain == AF_NETLINK_ && sock->real_fd < 0 && level == SOL_NETLINK_) {
        if (option == NETLINK_ADD_MEMBERSHIP_ || option == NETLINK_DROP_MEMBERSHIP_) {
            if (value_len < sizeof(dword_t))
                return _EINVAL;
            dword_t group = *(dword_t *) value;
            if (group == 0 || group > 32)
                return _EINVAL;
            uint32_t bit = 1u << (group - 1);
            if (option == NETLINK_ADD_MEMBERSHIP_)
                sock->socket.netlink_groups |= bit;
            else
                sock->socket.netlink_groups &= ~bit;
            return 0;
        }
        if (option == NETLINK_CAP_ACK_ || option == NETLINK_EXT_ACK_ || option == NETLINK_GET_STRICT_CHK_) {
            if (value_len < sizeof(dword_t))
                return _EINVAL;
            bool enabled = (*(dword_t *) value) != 0;
            if (option == NETLINK_CAP_ACK_)
                sock->socket.netlink_cap_ack = enabled;
            else if (option == NETLINK_EXT_ACK_)
                sock->socket.netlink_ext_ack = enabled;
            else
                sock->socket.netlink_get_strict_chk = enabled;
            return 0;
        }
        if (option == NETLINK_PKTINFO_) {
            // sd-netlink sets this unconditionally when creating an
            // rtnetlink manager socket and treats setsockopt failure as
            // fatal ("Could not create manager: Protocol not available"),
            // which sent systemd-resolved into a forever-retry loop that
            // blocked the rest of boot on slow (amd64-on-arm64) hosts.
            // Accepting it is safe now for two reasons, both load-bearing:
            // (1) netlink_handle_sendmsg no longer discards unread acks of
            // pipelined requests, which was the ACTUAL cause of the PID 1
            // boot hang two earlier attempts at accepting this option ran
            // into (loopback_setup fires three requests back-to-back and
            // collects the acks in seq order; we were destroying all but
            // the last), and (2) netlink_link_watch_thread delivers real
            // multicast notifications to subscribed sockets, so a manager
            // that goes on to wait for link/address change events isn't
            // waiting on something structurally impossible. No PKTINFO
            // cmsg is attached to messages (nothing needs it yet); the
            // option itself is tracked-and-ignored like NETLINK_CAP_ACK.
            return 0;
        }
        return _ENOPROTOOPT;
    }
    if (sock->socket.domain == AF_NETLINK_ && sock->real_fd < 0) {
        // No real fd to ask the host kernel for these, so track what was
        // requested (Linux's kernel doubles SO_RCVBUF/SO_SNDBUF/the FORCE
        // variants) and hand it back on getsockopt.
        if (level == SOL_SOCKET_ &&
                (option == SO_RCVBUF_ || option == SO_RCVBUFFORCE_ ||
                 option == SO_SNDBUF_ || option == SO_SNDBUFFORCE_)) {
            if (value_len < sizeof(dword_t))
                return _EINVAL;
            dword_t requested = *(dword_t *) value;
            dword_t doubled = requested * 2;
            if (option == SO_RCVBUF_ || option == SO_RCVBUFFORCE_)
                sock->socket.netlink_rcvbuf = doubled;
            else
                sock->socket.netlink_sndbuf = doubled;
            return 0;
        }
        // A fake socket is never actually put on the wire, so REUSEADDR/PORT
        // are meaningless here -- but systemd sets SO_REUSEADDR unconditionally
        // on every listening socket it creates, including its kobject-uevent
        // netlink socket, before this option list existed to catch it. That
        // fell through to a real setsockopt(-1, ...) call and got EBADF
        // ("Failed to create listening socket (kobject-uevent 1): Bad file
        // descriptor"), so systemd-udevd.service (and therefore udev as a
        // whole) never started.
        if (level == SOL_SOCKET_ &&
                (option == SO_RCVTIMEO_OLD_ || option == SO_SNDTIMEO_OLD_ ||
                 option == SO_RCVTIMEO_ || option == SO_SNDTIMEO_ ||
                 option == SO_ATTACH_FILTER_ || option == SO_DETACH_FILTER_ ||
                 option == SO_REUSEADDR_ || option == SO_REUSEPORT_ ||
                 // The other half of what sd-netlink sets unconditionally
                 // on an rtnetlink manager socket (binds it to the
                 // loopback ifindex) -- see the NETLINK_PKTINFO_ comment
                 // in the SOL_NETLINK block for why accepting these is
                 // safe now. A fake socket isn't on any real interface;
                 // there's nothing to bind, so no-op.
                 option == SO_BINDTODEVICE_ || option == SO_BINDTOIFINDEX_)) {
            return 0;
        }
    }
    if (level == SOL_SOCKET_ && option == SO_BINDTOIFINDEX_ && sock->real_fd >= 0 &&
            (sock->socket.domain == AF_INET_ || sock->socket.domain == AF_INET6_)) {
        // Linux SO_BINDTOIFINDEX restricts a socket to one interface.
        // Darwin's real equivalent is IP_BOUND_IF (IPPROTO_IP, per-socket
        // ifindex). systemd-resolved binds its DNS stub sockets to the
        // loopback ifindex this way and treats failure as fatal. The
        // guest's ifindexes come from the host's if_nametoindex (that's
        // what the netlink link dumps report), so the value maps 1:1.
        // If the host refuses (unknown index for a synthesized guest
        // view), fall back to success: every NAT'd guest endpoint already
        // lives on host loopback, so the restriction this asks for is
        // already structurally true.
        if (value_len < sizeof(dword_t))
            return _EINVAL;
#ifdef IP_BOUND_IF
        dword_t ifindex = *(dword_t *) value;
        setsockopt(sock->real_fd, IPPROTO_IP, IP_BOUND_IF, &ifindex, sizeof(ifindex));
#endif
        return 0;
    }
    if (level == SOL_SOCKET_ && (option == SO_RCVTIMEO_OLD_ || option == SO_SNDTIMEO_OLD_)) {
        if (value_len < guest_timeval_size(abi))
            return _EINVAL;
        struct timeval guest_timeout;
        if (read_guest_timeval_abi(abi, value_addr, &guest_timeout))
            return _EFAULT;
        struct timeval host_timeout = {
            .tv_sec = guest_timeout.tv_sec,
            .tv_usec = guest_timeout.tv_usec,
        };
        int err = setsockopt(sock->real_fd, SOL_SOCKET,
                option == SO_RCVTIMEO_OLD_ ? SO_RCVTIMEO : SO_SNDTIMEO,
                &host_timeout, sizeof(host_timeout));
        if (err < 0)
            return errno_map();
        return 0;
    }
    if (level == SOL_SOCKET_) {
        if (option == SO_REUSEADDR_) {
            if (value_len < sizeof(dword_t))
                return _EINVAL;
            sock->socket.reuseaddr = (*(dword_t *) value) != 0;
        } else if (option == SO_REUSEPORT_) {
            if (value_len < sizeof(dword_t))
                return _EINVAL;
            sock->socket.reuseport = (*(dword_t *) value) != 0;
        }
        if (option == SO_SNDBUFFORCE_) {
            option = SO_SNDBUF_;
        } else if (option == SO_RCVBUFFORCE_) {
            option = SO_RCVBUF_;
        } else if (sockopt_is_linux_soft_unsupported(level, option)) {
            return _ENOPROTOOPT;
        }
    }

    // IP/IPv6 multicast options. These need ABI translation Darwin and Linux
    // disagree on: Linux IP_MULTICAST_TTL/LOOP take an int, Darwin a u_char;
    // Linux IP_ADD_MEMBERSHIP takes struct ip_mreqn (with an imr_ifindex
    // field) while Darwin only has struct ip_mreq. They matter because
    // systemd-resolved allocates an LLMNR/mDNS multicast scope for EVERY link
    // during coldplug, and a setsockopt failure there doesn't just disable
    // LLMNR -- it propagates up as "Failed to process RTNL link message",
    // leaving resolved with zero usable links so it exits ("Could not create
    // manager: No data available") and takes ALL name resolution down with
    // it (nsswitch "resolve [!UNAVAIL=return]"). Actual LAN multicast can't
    // work under iSH regardless, so accept these best-effort: translate what
    // Darwin understands, and never fail the guest call on the host's
    // rejection -- unicast DNS, the part that matters, doesn't depend on it.
    if (level == IPPROTO_IP &&
            (option == IP_MULTICAST_TTL_ || option == IP_MULTICAST_LOOP_)) {
        // Linux accepts either a single byte or an int for these two options
        // (a documented historic quirk of ip_setsockopt: optlen >= 1 reads one
        // byte, optlen >= sizeof(int) reads an int). avahi-daemon passes a
        // uint8_t TTL; rejecting the 1-byte form made its IPv4 mDNS socket
        // setup fail ("IP_MULTICAST_TTL failed: Invalid argument") and dropped
        // it to IPv6-only mode.
        if (value_len < 1)
            return _EINVAL;
        unsigned char host_val = value_len >= sizeof(dword_t)
            ? (unsigned char) *(dword_t *) value
            : *(unsigned char *) value;
        int real_opt = option == IP_MULTICAST_TTL_ ? IP_MULTICAST_TTL : IP_MULTICAST_LOOP;
        (void) setsockopt(sock->real_fd, IPPROTO_IP, real_opt, &host_val, sizeof(host_val));
        return 0;
    }
    if (level == IPPROTO_IPV6 &&
            (option == IPV6_MULTICAST_HOPS_ || option == IPV6_MULTICAST_LOOP_)) {
        if (value_len < sizeof(dword_t))
            return _EINVAL;
        int host_val = *(dword_t *) value;
        int real_opt = option == IPV6_MULTICAST_HOPS_ ? IPV6_MULTICAST_HOPS : IPV6_MULTICAST_LOOP;
        (void) setsockopt(sock->real_fd, IPPROTO_IPV6, real_opt, &host_val, sizeof(host_val));
        return 0;
    }
    // Group membership: this used to be an unconditional no-op (see git
    // blame), which kept systemd-resolved's per-link LLMNR/mDNS scope setup
    // from failing and cascading into total DNS breakage -- but it also
    // meant NO guest program's multicast group join ever reached the real
    // host socket, so anything that needs to RECEIVE multicast (avahi/mDNS,
    // SSDP/UPnP discovery, ...) silently got nothing. Actually join now.
    //
    // Linux's IP_ADD_MEMBERSHIP/IP_DROP_MEMBERSHIP accept two struct shapes
    // keyed off optlen: the plain 8-byte ip_mreq (imr_multiaddr +
    // imr_interface, both struct in_addr) or the 12-byte ip_mreqn (same two
    // fields plus a trailing imr_ifindex). Darwin's ip_mreq is always the
    // 8-byte shape with no ifindex field, so both guest shapes translate by
    // just taking their first 8 bytes; a guest that selected the interface
    // by ifindex (imr_interface == INADDR_ANY, imr_ifindex != 0) degrades to
    // Darwin picking a default multicast-capable interface, matching this
    // function's existing best-effort stance elsewhere.
    if (level == IPPROTO_IP &&
            (option == IP_ADD_MEMBERSHIP_ || option == IP_DROP_MEMBERSHIP_)) {
        if (value_len != 8 && value_len != 12)
            return _EINVAL;
        struct ip_mreq host_mreq;
        memcpy(&host_mreq.imr_multiaddr, value, sizeof(host_mreq.imr_multiaddr));
        memcpy(&host_mreq.imr_interface, value + 4, sizeof(host_mreq.imr_interface));
        int real_opt = option == IP_ADD_MEMBERSHIP_ ? IP_ADD_MEMBERSHIP : IP_DROP_MEMBERSHIP;
        if (setsockopt(sock->real_fd, IPPROTO_IP, real_opt, &host_mreq, sizeof(host_mreq)) < 0)
            return errno_map();
        return 0;
    }
    // Linux's ipv6_mreq (in6_addr + int ipv6mr_ifindex) and Darwin's
    // (in6_addr + unsigned int ipv6mr_interface) share an identical 20-byte
    // layout on every guest ABI this project supports (all little-endian,
    // no struct padding before either trailing 4-byte field), so this one
    // needs no field-by-field translation -- unlike the IPv4 case above,
    // where Darwin lacks the ifindex field entirely.
    if (level == IPPROTO_IPV6 &&
            (option == IPV6_ADD_MEMBERSHIP_ || option == IPV6_DROP_MEMBERSHIP_)) {
        if (value_len != sizeof(struct ipv6_mreq))
            return _EINVAL;
        struct ipv6_mreq host_mreq;
        memcpy(&host_mreq, value, sizeof(host_mreq));
        int real_opt = option == IPV6_ADD_MEMBERSHIP_ ? IPV6_JOIN_GROUP : IPV6_LEAVE_GROUP;
        if (setsockopt(sock->real_fd, IPPROTO_IPV6, real_opt, &host_mreq, sizeof(host_mreq)) < 0)
            return errno_map();
        return 0;
    }
    // Outbound-interface selection (not membership): Linux specifies this by
    // ip_mreqn.imr_ifindex or a local address, but the guest's view of
    // "which interface" doesn't correspond to anything meaningful in iSH's
    // single shared-host-interface model. Remain a no-op and let the OS pick
    // a default interface for outbound multicast sends.
    if ((level == IPPROTO_IP && option == IP_MULTICAST_IF_) ||
            (level == IPPROTO_IPV6 && option == IPV6_MULTICAST_IF_)) {
        return 0;
    }

    int real_opt = sock_opt_to_real(option, level);
    if (real_opt < 0)
        // Linux reports an option the level does not recognise as
        // ENOPROTOOPT; EINVAL is for a malformed argument. Probing code
        // treats ENOPROTOOPT as "not available, carry on" and EINVAL as a
        // hard error, so the distinction is load-bearing. (The soft-unsupported
        // list this replaces was the same fix applied one option at a time.)
        return _ENOPROTOOPT;
    int real_level = sock_level_to_real(level);
    if (real_level < 0)
        return _EINVAL;

    if (real_opt == 0)
        return _ENOPROTOOPT;

    int err = setsockopt(sock->real_fd, real_level, real_opt, value, value_len);
    if (err < 0)
        return errno_map();
    return 0;
}

int_t sys_setsockopt_guest(fd_t sock_fd, dword_t level, dword_t option, guest_addr_t value_addr, dword_t value_len) {
    return sys_setsockopt_guest_abi(sock_fd, level, option, value_addr, value_len, GUEST_ABI_I386);
}

int_t sys_setsockopt(fd_t sock_fd, dword_t level, dword_t option, addr_t value_addr, dword_t value_len) {
    return sys_setsockopt_guest(sock_fd, level, option, value_addr, value_len);
}

int_t sys_setsockopt_amd64(fd_t sock_fd, dword_t level, dword_t option, addr_t value_addr, dword_t value_len) {
    return sys_setsockopt_guest_abi(sock_fd, level, option, value_addr, value_len, GUEST_ABI_AMD64);
}

int_t sys_setsockopt_amd64_guest(fd_t sock_fd, dword_t level, dword_t option, guest_addr_t value_addr, dword_t value_len) {
    return sys_setsockopt_guest_abi(sock_fd, level, option, value_addr, value_len, GUEST_ABI_AMD64);
}

static void sockopt_store_value(void *dst, dword_t dst_len, dword_t *result_len,
        const void *src, dword_t src_len) {
    size_t copy_len = dst_len < src_len ? dst_len : src_len;
    if (copy_len != 0)
        memcpy(dst, src, copy_len);
    // Linux clamps the option's natural size to the caller's buffer and writes
    // THAT back through optlen -- a short buffer truncates silently, it is not
    // an error. Reporting the natural size instead told the caller more bytes
    // had been written than the buffer could hold.
    *result_len = (dword_t) copy_len;
}

static bool sockopt_is_linux_soft_unsupported(dword_t level, dword_t option) {
    if (level != SOL_SOCKET_)
        return false;
    switch (option) {
        case SO_BINDTODEVICE_:
        case SO_PEERSEC_:
        case SO_PASSSEC_:
        case SO_PEERGROUPS_:
            return true;
    }
    return false;
}

static int_t sys_getsockopt_guest_abi(fd_t sock_fd, dword_t level, dword_t option,
        guest_addr_t value_addr, guest_addr_t len_addr, enum guest_abi abi) {
    STRACE("getsockopt(%d, %d, %d, %#llx, %#llx)", sock_fd, level, option,
            (unsigned long long) value_addr, (unsigned long long) len_addr);
    int_t sock_err;
    struct fd *sock = sock_getfd(sock_fd, &sock_err);
    if (sock == NULL)
        return sock_err;
    dword_t user_value_len;
    if (user_get(len_addr, user_value_len))
        return _EFAULT;
    // Same unbounded-VLA hazard as sys_setsockopt_guest_abi: user_value_len
    // comes from guest memory at len_addr, entirely guest-controlled.
    if (user_value_len > 4096)
        return _EINVAL;
    char value[user_value_len != 0 ? user_value_len : 1];
    if (user_value_len != 0 && user_read(value_addr, value, user_value_len))
        return _EFAULT;
    dword_t value_len = user_value_len;

    if (level == SOL_SOCKET_ && (option == SO_DOMAIN_ || option == SO_TYPE_ || option == SO_PROTOCOL_)) {
        dword_t value_p;
        if (option == SO_DOMAIN_)
            value_p = sock->socket.domain;
        else if (option == SO_TYPE_)
            value_p = sock->socket.type;
        else if (option == SO_PROTOCOL_)
            value_p = sock->socket.protocol;
        sockopt_store_value(value, user_value_len, &value_len, &value_p, sizeof(value_p));
    } else if (level == SOL_SOCKET_ && option == SO_PEERCRED_) {
        struct ucred_ cred;
        int err = unix_socket_finish_peer(sock);
        if (err < 0 && err != _ENOTCONN)
            return err;
        lock(&peer_lock, 0);
        if (sock->socket.domain != AF_LOCAL_) {
            cred.pid = 0;
            cred.uid = cred.gid = -1;
        } else if (sock->socket.unix_peer_cred_valid) {
            cred = sock->socket.unix_peer_cred;
        } else if (sock->socket.unix_peer != NULL) {
            cred = sock->socket.unix_peer->socket.unix_cred;
        } else {
            cred.pid = 0;
            cred.uid = cred.gid = -1;
        }
        unlock(&peer_lock);
        sockopt_store_value(value, user_value_len, &value_len, &cred, sizeof(cred));
    } else if (level == SOL_SOCKET_ && option == SO_PASSCRED_) {
        dword_t passcred;
        if (sock->socket.domain == AF_NETLINK_) {
            passcred = sock->socket.unix_passcred;
        } else if (sock->socket.domain != AF_LOCAL_) {
            return _ENOPROTOOPT;
        } else {
            passcred = sock->socket.unix_passcred;
        }
        sockopt_store_value(value, user_value_len, &value_len, &passcred, sizeof(passcred));
    } else if (level == SOL_SOCKET_ && option == SO_ACCEPTCONN_) {
        // Report our own tracked listen() state. Darwin's getsockopt does not
        // support querying SO_ACCEPTCONN (it returns ENOPROTOOPT), so passing
        // it through leaked that error where Linux reports 0/1.
        dword_t acceptconn = sock->socket.listening ? 1 : 0;
        sockopt_store_value(value, user_value_len, &value_len, &acceptconn, sizeof(acceptconn));
    } else if (sock->socket.domain == AF_NETLINK_ && sock->real_fd < 0 && level == SOL_SOCKET_ &&
            (option == SO_RCVBUF_ || option == SO_SNDBUF_)) {
        // Mirrors the setsockopt tracking above -- no real fd to ask the host
        // for these on a fake netlink socket.
        dword_t bufsize = option == SO_RCVBUF_ ? sock->socket.netlink_rcvbuf : sock->socket.netlink_sndbuf;
        sockopt_store_value(value, user_value_len, &value_len, &bufsize, sizeof(bufsize));
    } else if (sock->socket.domain == AF_NETLINK_ && sock->real_fd < 0 && level == SOL_NETLINK_) {
        if (option == NETLINK_CAP_ACK_ || option == NETLINK_EXT_ACK_ || option == NETLINK_GET_STRICT_CHK_) {
            dword_t enabled =
                option == NETLINK_CAP_ACK_ ? sock->socket.netlink_cap_ack :
                option == NETLINK_EXT_ACK_ ? sock->socket.netlink_ext_ack :
                sock->socket.netlink_get_strict_chk;
            sockopt_store_value(value, user_value_len, &value_len, &enabled, sizeof(enabled));
        } else if (option == NETLINK_LIST_MEMBERSHIPS_) {
            dword_t memberships[32];
            dword_t count = 0;
            for (dword_t group = 1; group <= 32; group++) {
                uint32_t bit = 1u << (group - 1);
                if (sock->socket.netlink_groups & bit)
                    memberships[count++] = group;
            }
            sockopt_store_value(value, user_value_len, &value_len, memberships, count * sizeof(dword_t));
        } else {
            return _ENOPROTOOPT;
        }
    } else if (level == SOL_SOCKET_ && option == SO_BINDTODEVICE_) {
        // Linux reports an unbound socket as success with an empty interface
        // name (optlen = 0), and nothing under iSH can bind a socket to a
        // device, so every socket is unbound. Returning ENOPROTOOPT here made
        // OpenSSH's sys_get_rdomain() VRF probe log "cannot determine VRF for
        // fd=N : Protocol not available" on every ssh session. setsockopt
        // still rejects it via the soft-unsupported list below.
        value_len = 0;
    } else if (sockopt_is_linux_soft_unsupported(level, option)) {
        return _ENOPROTOOPT;
    } else if (level == SOL_SOCKET_ && (option == SO_RCVTIMEO_OLD_ || option == SO_SNDTIMEO_OLD_)) {
        struct timeval host_timeout;
        socklen_t host_timeout_len = sizeof(host_timeout);
        int err = getsockopt(sock->real_fd, SOL_SOCKET,
                option == SO_RCVTIMEO_OLD_ ? SO_RCVTIMEO : SO_SNDTIMEO,
                &host_timeout, &host_timeout_len);
        if (err < 0)
            return errno_map();
        if (abi == GUEST_ABI_AMD64) {
            struct amd64_timeval_ guest_timeout = {
                .sec = host_timeout.tv_sec,
                .usec = host_timeout.tv_usec,
            };
            sockopt_store_value(value, user_value_len, &value_len, &guest_timeout, sizeof(guest_timeout));
        } else {
            struct timeval_ guest_timeout = {
                .sec = host_timeout.tv_sec,
                .usec = host_timeout.tv_usec,
            };
            sockopt_store_value(value, user_value_len, &value_len, &guest_timeout, sizeof(guest_timeout));
        }
    } else if (level == IPPROTO_ICMPV6 && option == ICMP6_FILTER_) {
        if (sock->socket.type != SOCK_RAW_ || sock->socket.protocol != IPPROTO_ICMPV6)
            return _ENOPROTOOPT;
        sockopt_store_value(value, user_value_len, &value_len,
                sock->socket.icmp6_filter, sizeof(sock->socket.icmp6_filter));
    } else if (level == IPPROTO_IP && option == IP_MTU_DISCOVER_) {
        dword_t mtu_discover = sock->socket.ip_mtu_discover;
        sockopt_store_value(value, user_value_len, &value_len, &mtu_discover, sizeof(mtu_discover));
    } else if (level == IPPROTO_IPV6 && option == IPV6_MTU_DISCOVER_) {
        dword_t mtu_discover = sock->socket.ipv6_mtu_discover;
        sockopt_store_value(value, user_value_len, &value_len, &mtu_discover, sizeof(mtu_discover));
    } else if (level == IPPROTO_IPV6 && option == IPV6_MTU_) {
        dword_t mtu = sock->socket.ipv6_mtu;
        sockopt_store_value(value, user_value_len, &value_len, &mtu, sizeof(mtu));
    } else if (level == IPPROTO_IP && option == IP_MTU_) {
        // Linux-only read of the connected socket's current path MTU
        // (ENOTCONN when unconnected); Darwin has no equivalent, and letting
        // it fall through to EINVAL broke systemd-resolved: its
        // dns_scope_emit_one() tolerates ENOTCONN from socket_get_mtu() but
        // treats any other errno as fatal for the send attempt, so every
        // UDP DNS transaction died with "Failed to read socket MTU: Invalid
        // argument" and rotated to the next server until MaxAttemptsReached
        // -- no query ever hit the wire. No PMTU data is available from the
        // host, so report the conventional Ethernet 1500 (only used as an
        // upper clamp by callers deciding UDP-vs-TCP).
        struct sockaddr_storage peer;
        socklen_t peer_len = sizeof(peer);
        if (getpeername(sock->real_fd, (struct sockaddr *) &peer, &peer_len) < 0)
            return _ENOTCONN;
        dword_t mtu = 1500;
        sockopt_store_value(value, user_value_len, &value_len, &mtu, sizeof(mtu));
    } else if (level == IPPROTO_IP && option == IP_RECVERR_) {
        dword_t recverr = sock->socket.ip_recverr;
        sockopt_store_value(value, user_value_len, &value_len, &recverr, sizeof(recverr));
    } else if (level == IPPROTO_IPV6 && option == IPV6_RECVERR_) {
        dword_t recverr = sock->socket.ipv6_recverr;
        sockopt_store_value(value, user_value_len, &value_len, &recverr, sizeof(recverr));
    } else if (level == SOL_SOCKET_ && option == SO_ERROR_) {
        dword_t socket_error;
        if (sock->socket.domain == AF_NETLINK_ && sock->real_fd < 0) {
            socket_error = 0;
        } else {
            int real_error;
            socklen_t real_error_len = sizeof(real_error);
            int err = getsockopt(sock->real_fd, SOL_SOCKET, SO_ERROR, &real_error, &real_error_len);
            if (err < 0)
                return errno_map();
            // SO_ERROR is read-and-clear at the host level: an internal
            // readiness probe (socket_tcp_connect_write_ready, run on every
            // poll/epoll scan of this fd) may have already observed and
            // cleared it before this guest query got here. Fall back to the
            // cached value so a genuine connect failure isn't reported as
            // success. See the fd.h comment on host_connect_error.
            if (real_error == 0 && sock->socket.host_connect_error != 0)
                real_error = sock->socket.host_connect_error;
            sock->socket.host_connect_error = 0;
            socket_error = real_error == 0 ? 0 : -err_map(real_error);
        }
        sockopt_store_value(value, user_value_len, &value_len, &socket_error, sizeof(socket_error));
    } else if (level == IPPROTO_TCP && option == TCP_DEFER_ACCEPT_) {
        dword_t defer_accept = sock->socket.tcp_defer_accept;
        sockopt_store_value(value, user_value_len, &value_len, &defer_accept, sizeof(defer_accept));
    } else if (level == IPPROTO_TCP && option == TCP_FASTOPEN_) {
        dword_t fastopen = 0;
        sockopt_store_value(value, user_value_len, &value_len, &fastopen, sizeof(fastopen));
    } else if (level == IPPROTO_TCP && option == TCP_CONGESTION_) {
        sockopt_store_value(value, user_value_len, &value_len,
                sock->socket.tcp_congestion, strlen(sock->socket.tcp_congestion));
#if defined(__APPLE__)
    } else if (level == IPPROTO_TCP && option == TCP_INFO_) {
        // This one's fun. On Linux, the struct is not ABI dependent, so no
        // special handling is needed. On Darwin, the struct is completely
        // different and has a different sockopt name.
        struct tcp_connection_info conn_info;
        socklen_t conn_info_size = sizeof(conn_info);
        int err = getsockopt(sock->real_fd, IPPROTO_TCP, TCP_CONNECTION_INFO, &conn_info, &conn_info_size);
        if (err < 0)
            return errno_map();

        // The possible keys for this table are in netinet/tcp_fsm.h, but that
        // header isn't available on iOS, only macOS.
        static const uint8_t tcp_state_table[] = {
            7, // TCPS_CLOSED
            10, // TCPS_LISTEN
            2, // TCPS_SYN_SENT
            3, // TCPS_SYN_RECEIVED
            1, // TCPS_ESTABLISHED
            8, // TCPS_CLOSE_WAIT
            4, // TCPS_FIN_WAIT_1
            11, // TCPS_CLOSING
            9, // TCPS_LAST_ACK
            5, // TCPS_FIN_WAIT_2
            6, // TCPS_TIME_WAIT
        };
        struct tcp_info_ info = {
            .state = tcp_state_table[conn_info.tcpi_state],
            .options = conn_info.tcpi_options,
            .snd_wscale = conn_info.tcpi_snd_wscale,
            .rcv_wscale = conn_info.tcpi_rcv_wscale,

            .rto = conn_info.tcpi_rto * 1000,
            .snd_mss = conn_info.tcpi_maxseg,

            .rtt = conn_info.tcpi_srtt * 1000,
            .rttvar = conn_info.tcpi_rttvar * 1000,
            .snd_ssthresh = conn_info.tcpi_snd_ssthresh,
            .snd_cwnd = conn_info.tcpi_snd_cwnd / conn_info.tcpi_maxseg,

            // https://lkml.org/lkml/2017/4/24/923
            .total_retrans = conn_info.tcpi_txretransmitpackets,
        };
        sockopt_store_value(value, user_value_len, &value_len, &info, sizeof(info));
#endif
    } else {
        int real_opt = sock_opt_to_real(option, level);
        if (real_opt < 0)
            // Linux reports an option the level does not recognise as
        // ENOPROTOOPT; EINVAL is for a malformed argument. Probing code
        // treats ENOPROTOOPT as "not available, carry on" and EINVAL as a
        // hard error, so the distinction is load-bearing. (The soft-unsupported
        // list this replaces was the same fix applied one option at a time.)
        return _ENOPROTOOPT;
        int real_level = sock_level_to_real(level);
        if (real_level < 0)
            return _EINVAL;

        socklen_t host_value_len = user_value_len;
        int err = getsockopt(sock->real_fd, real_level, real_opt, value, &host_value_len);
        if (err < 0)
            return errno_map();
        value_len = host_value_len;
    }

    if (user_put(len_addr, value_len))
        return _EFAULT;
    dword_t copy_value_len = user_value_len < value_len ? user_value_len : value_len;
    if (copy_value_len != 0 && user_write(value_addr, value, copy_value_len))
        return _EFAULT;
    return 0;
}

int_t sys_getsockopt_guest(fd_t sock_fd, dword_t level, dword_t option, guest_addr_t value_addr, guest_addr_t len_addr) {
    return sys_getsockopt_guest_abi(sock_fd, level, option, value_addr, len_addr, GUEST_ABI_I386);
}

int_t sys_getsockopt(fd_t sock_fd, dword_t level, dword_t option, addr_t value_addr, dword_t len_addr) {
    return sys_getsockopt_guest(sock_fd, level, option, value_addr, len_addr);
}

int_t sys_getsockopt_amd64(fd_t sock_fd, dword_t level, dword_t option, addr_t value_addr, dword_t len_addr) {
    return sys_getsockopt_guest_abi(sock_fd, level, option, value_addr, len_addr, GUEST_ABI_AMD64);
}

int_t sys_getsockopt_amd64_guest(fd_t sock_fd, dword_t level, dword_t option, guest_addr_t value_addr, guest_addr_t len_addr) {
    return sys_getsockopt_guest_abi(sock_fd, level, option, value_addr, len_addr, GUEST_ABI_AMD64);
}

static void scm_free(struct scm *scm) {
    for (unsigned i = 0; i < scm->num_fds; i++)
        fd_close(scm->fds[i]);
    free(scm);
}

// ---- unix DGRAM SCM_RIGHTS registry ----
//
// Parcels of guest fds in flight on unix DGRAM sockets, keyed by the random
// cookie carried in the datagram's unix_dgram_cred_hdr (see that struct's
// comment). The sender registers, the receiver takes on the real (non-PEEK)
// read. A datagram that is dropped, discarded via plain read()/recvfrom(),
// or truncated never takes its parcel, so entries also expire: anything
// older than UNIX_DGRAM_SCM_TTL_SECS is released on the next registry
// operation (fd refs dropped via scm_free). In-emulator dgram delivery is
// effectively instant, so the TTL only bounds the leak from genuinely lost
// datagrams.
struct unix_dgram_scm {
    uint64_t cookie;
    struct scm *scm;
    time_t added;
    struct list list;
};
#define UNIX_DGRAM_SCM_TTL_SECS 60
static lock_t unix_dgram_scm_lock = LOCK_INITIALIZER;
static struct list unix_dgram_scms = LIST_INITIALIZER(unix_dgram_scms);

static void unix_dgram_scm_gc_locked(void) {
    time_t now = time(NULL);
    struct unix_dgram_scm *entry, *tmp;
    list_for_each_entry_safe(&unix_dgram_scms, entry, tmp, list) {
        if (now - entry->added < UNIX_DGRAM_SCM_TTL_SECS)
            continue;
        list_remove(&entry->list);
        scm_free(entry->scm);
        free(entry);
    }
}

// Takes ownership of `scm` on success; returns its cookie (never 0), or 0
// on allocation failure (caller keeps ownership).
static uint64_t unix_dgram_scm_register(struct scm *scm) {
    struct unix_dgram_scm *entry = malloc(sizeof(*entry));
    if (entry == NULL)
        return 0;
    lock(&unix_dgram_scm_lock, 0);
    unix_dgram_scm_gc_locked();
    do {
        arc4random_buf(&entry->cookie, sizeof(entry->cookie));
    } while (entry->cookie == 0);
    entry->scm = scm;
    entry->added = time(NULL);
    list_add_tail(&unix_dgram_scms, &entry->list);
    uint64_t cookie = entry->cookie;
    unlock(&unix_dgram_scm_lock);
    return cookie;
}

// Removes and returns the parcel for `cookie` (caller owns it and must
// scm_free after use), or NULL if it was never registered or already
// taken/expired.
static struct scm *unix_dgram_scm_take(uint64_t cookie) {
    if (cookie == 0)
        return NULL;
    struct scm *scm = NULL;
    lock(&unix_dgram_scm_lock, 0);
    unix_dgram_scm_gc_locked();
    struct unix_dgram_scm *entry;
    list_for_each_entry(&unix_dgram_scms, entry, list) {
        if (entry->cookie == cookie) {
            list_remove(&entry->list);
            scm = entry->scm;
            free(entry);
            break;
        }
    }
    unlock(&unix_dgram_scm_lock);
    return scm;
}

struct guest_msghdr_marshaled {
    guest_addr_t msg_name;
    uint_t msg_namelen;
    guest_addr_t msg_iov;
    uint_t msg_iovlen;
    guest_addr_t msg_control;
    uint_t msg_controllen;
    int_t msg_flags;
};

struct guest_cmsghdr_marshaled {
    size_t len;
    int_t level;
    int_t type;
};

static size_t guest_mmsghdr_size(enum guest_abi abi) {
    return abi == GUEST_ABI_AMD64 ? sizeof(struct amd64_mmsghdr_) : sizeof(struct i386_mmsghdr_);
}

static size_t guest_mmsghdr_len_offset(enum guest_abi abi) {
    return abi == GUEST_ABI_AMD64 ? offsetof(struct amd64_mmsghdr_, len) : offsetof(struct i386_mmsghdr_, len);
}

static bool guest_msghdr_addr_valid(enum guest_abi abi, qword_t addr) {
    return guest_abi_addr_valid(abi, addr);
}

static int read_guest_msghdr(guest_addr_t msghdr_addr, enum guest_abi abi, struct guest_msghdr_marshaled *msg) {
    if (abi == GUEST_ABI_AMD64) {
        struct amd64_msghdr_ raw;
        if (user_read(msghdr_addr, &raw, sizeof(raw)))
            return _EFAULT;
        if (!guest_msghdr_addr_valid(abi, raw.msg_name) ||
                !guest_msghdr_addr_valid(abi, raw.msg_iov) ||
                !guest_msghdr_addr_valid(abi, raw.msg_control) ||
                raw.msg_namelen > UINT32_MAX ||
                raw.msg_iovlen > UINT32_MAX ||
                raw.msg_controllen > UINT32_MAX)
            return _EINVAL;
        *msg = (struct guest_msghdr_marshaled) {
            .msg_name = raw.msg_name,
            .msg_namelen = raw.msg_namelen,
            .msg_iov = raw.msg_iov,
            .msg_iovlen = (uint_t) raw.msg_iovlen,
            .msg_control = raw.msg_control,
            .msg_controllen = (uint_t) raw.msg_controllen,
            .msg_flags = raw.msg_flags,
        };
        return 0;
    }

    struct i386_msghdr_ raw;
    if (user_read(msghdr_addr, &raw, sizeof(raw)))
        return _EFAULT;
    *msg = (struct guest_msghdr_marshaled) {
        .msg_name = raw.msg_name,
        .msg_namelen = raw.msg_namelen,
        .msg_iov = raw.msg_iov,
        .msg_iovlen = raw.msg_iovlen,
        .msg_control = raw.msg_control,
        .msg_controllen = raw.msg_controllen,
        .msg_flags = raw.msg_flags,
    };
    return 0;
}

static int write_guest_msghdr(guest_addr_t msghdr_addr, enum guest_abi abi, const struct guest_msghdr_marshaled *msg) {
    if (abi == GUEST_ABI_AMD64) {
        struct amd64_msghdr_ raw = {
            .msg_name = msg->msg_name,
            .msg_namelen = msg->msg_namelen,
            .msg_iov = msg->msg_iov,
            .msg_iovlen = msg->msg_iovlen,
            .msg_control = msg->msg_control,
            .msg_controllen = msg->msg_controllen,
            .msg_flags = msg->msg_flags,
        };
        return user_write(msghdr_addr, &raw, sizeof(raw)) ? _EFAULT : 0;
    }

    struct i386_msghdr_ raw = {
        .msg_name = msg->msg_name,
        .msg_namelen = msg->msg_namelen,
        .msg_iov = msg->msg_iov,
        .msg_iovlen = msg->msg_iovlen,
        .msg_control = msg->msg_control,
        .msg_controllen = msg->msg_controllen,
        .msg_flags = msg->msg_flags,
    };
    return user_write(msghdr_addr, &raw, sizeof(raw)) ? _EFAULT : 0;
}

static size_t guest_cmsg_hdr_size(enum guest_abi abi) {
    return abi == GUEST_ABI_AMD64 ? sizeof(struct amd64_cmsghdr_) : sizeof(struct i386_cmsghdr_);
}

static size_t guest_cmsg_space(enum guest_abi abi, size_t data_len) {
    size_t align = guest_abi_desc(abi).word_size;
    size_t cmsg_len = guest_cmsg_hdr_size(abi) + data_len;
    return (cmsg_len + align - 1) & ~(align - 1);
}

static bool guest_cmsg_parse(enum guest_abi abi, const uint8_t *buffer, size_t capacity,
        size_t *offset, struct guest_cmsghdr_marshaled *cmsg, const uint8_t **data,
        size_t *data_len) {
    size_t hdr_size = guest_cmsg_hdr_size(abi);
    if (*offset + hdr_size > capacity)
        return false;

    size_t len;
    if (abi == GUEST_ABI_AMD64) {
        struct amd64_cmsghdr_ raw;
        memcpy(&raw, buffer + *offset, sizeof(raw));
        len = raw.len;
        cmsg->level = raw.level;
        cmsg->type = raw.type;
    } else {
        struct i386_cmsghdr_ raw;
        memcpy(&raw, buffer + *offset, sizeof(raw));
        len = raw.len;
        cmsg->level = raw.level;
        cmsg->type = raw.type;
    }

    if (len < hdr_size)
        return false;
    // Linux permits the last cmsg to end unpadded at exactly cmsg_len
    // (libwayland sends msg_controllen = CMSG_LEN, not CMSG_SPACE, so a
    // single-fd SCM_RIGHTS is 4 bytes short of the aligned size). Bounds-check
    // the unpadded length; the aligned advance may step past capacity, which
    // just terminates the caller's loop.
    if (*offset + len > capacity)
        return false;

    cmsg->len = len;
    *data = buffer + *offset + hdr_size;
    *data_len = len - hdr_size;
    *offset += guest_cmsg_space(abi, len - hdr_size);
    return true;
}

static bool guest_cmsg_append(enum guest_abi abi, uint8_t *buffer, size_t capacity, size_t *used,
        int_t level, int_t type, const void *data, size_t data_len) {
    size_t hdr_size = guest_cmsg_hdr_size(abi);
    size_t cmsg_len = hdr_size + data_len;
    size_t cmsg_space = guest_cmsg_space(abi, data_len);
    if (*used + cmsg_space > capacity)
        return false;

    if (abi == GUEST_ABI_AMD64) {
        struct amd64_cmsghdr_ raw = {
            .len = cmsg_len,
            .level = level,
            .type = type,
        };
        memcpy(buffer + *used, &raw, sizeof(raw));
    } else {
        struct i386_cmsghdr_ raw = {
            .len = cmsg_len,
            .level = level,
            .type = type,
        };
        memcpy(buffer + *used, &raw, sizeof(raw));
    }
    memcpy(buffer + *used + hdr_size, data, data_len);
    memset(buffer + *used + cmsg_len, 0, cmsg_space - cmsg_len);
    *used += cmsg_space;
    return true;
}

static struct cmsghdr *host_cmsg_first(struct msghdr *msg) {
    if (msg->msg_control == NULL || msg->msg_controllen < sizeof(struct cmsghdr))
        return NULL;
    struct cmsghdr *cmsg = (struct cmsghdr *) msg->msg_control;
    if (cmsg->cmsg_len < CMSG_LEN(0) || cmsg->cmsg_len > msg->msg_controllen)
        return NULL;
    return cmsg;
}

static struct cmsghdr *host_cmsg_next(struct msghdr *msg, struct cmsghdr *cmsg) {
    if (msg->msg_control == NULL || cmsg == NULL)
        return NULL;
    uint8_t *base = (uint8_t *) msg->msg_control;
    uint8_t *end = base + msg->msg_controllen;
    uint8_t *cur = (uint8_t *) cmsg;
    if (cur < base || cur + sizeof(struct cmsghdr) > end)
        return NULL;
    if (cmsg->cmsg_len < CMSG_LEN(0))
        return NULL;
    size_t data_len = cmsg->cmsg_len - CMSG_LEN(0);
    size_t step = CMSG_SPACE(data_len);
    if (step == 0)
        return NULL;
    uint8_t *next = cur + step;
    if (next + sizeof(struct cmsghdr) > end)
        return NULL;
    struct cmsghdr *next_cmsg = (struct cmsghdr *) next;
    if (next_cmsg->cmsg_len < CMSG_LEN(0) || next + next_cmsg->cmsg_len > end)
        return NULL;
    return next_cmsg;
}

static int sock_cmsg_level_to_fake(int level) {
    if (level == SOL_SOCKET)
        return SOL_SOCKET_;
    return level;
}

static int sock_cmsg_type_to_fake(int level, int type) {
    if (level == IPPROTO_IP) {
        switch (type) {
            case IP_TTL: return IP_TTL_;
            case IP_RECVTTL: return IP_TTL_;
            case IP_TOS: return IP_TOS_;
            case IP_RECVERR_: return IP_RECVERR_;
#ifdef IP_PKTINFO
            // Darwin numbers this 26; Linux delivers it as cmsg type 8
            // (IP_PKTINFO_). Identical 12-byte in_pktinfo payload, no
            // payload translation needed.
            case IP_PKTINFO: return IP_PKTINFO_;
#endif
        }
    } else if (level == IPPROTO_IPV6) {
        switch (type) {
            case IPV6_HOPLIMIT: return IPV6_HOPLIMIT_;
            case IPV6_TCLASS: return IPV6_TCLASS_;
            case IPV6_RECVERR_: return IPV6_RECVERR_;
        }
    }
    return -1;
}

static bool sock_cmsg_translate_payload(int level, int type, const void *data, size_t data_len,
        const void **fake_data, size_t *fake_data_len, int *fake_level, int *fake_type,
        uint8_t *scratch, size_t scratch_cap) {
    *fake_level = sock_cmsg_level_to_fake(level);
    *fake_type = sock_cmsg_type_to_fake(level, type);
    if (*fake_type < 0)
        return false;

    *fake_data = data;
    *fake_data_len = data_len;

    if (level == IPPROTO_IP && type == IP_RECVTTL) {
        if (data_len < sizeof(uint8_t))
            return false;
        if (scratch_cap < sizeof(int))
            return false;
        *(int *) scratch = *(const uint8_t *) data;
        *fake_data = scratch;
        *fake_data_len = sizeof(int);
        return true;
    }

    if (level == IPPROTO_IPV6 && type == IPV6_RECVERR_) {
        size_t need = sizeof(struct sock_extended_err_) + sizeof(struct sockaddr_in6_);
        if (data_len < sizeof(struct sock_extended_err_) + sizeof(struct sockaddr_in6))
            return false;
        if (scratch_cap < need)
            return false;

        struct sock_extended_err_ *guest_err = (struct sock_extended_err_ *) scratch;
        memcpy(guest_err, data, sizeof(*guest_err));

        const struct sockaddr_in6 *host_offender =
            (const struct sockaddr_in6 *) ((const uint8_t *) data + sizeof(*guest_err));
        struct sockaddr_in6_ *guest_offender =
            (struct sockaddr_in6_ *) (scratch + sizeof(*guest_err));
        memset(guest_offender, 0, sizeof(*guest_offender));
        guest_offender->sin6_family = AF_INET6_;
        guest_offender->sin6_port = host_offender->sin6_port;
        guest_offender->sin6_flowinfo = host_offender->sin6_flowinfo;
        guest_offender->sin6_addr = host_offender->sin6_addr;
        guest_offender->sin6_scope_id = host_offender->sin6_scope_id;

        *fake_data = scratch;
        *fake_data_len = need;
    }
    return true;
}

static void free_msghdr_iov(struct iovec *iov, size_t iovlen) {
    if (iov == NULL)
        return;
    for (size_t i = 0; i < iovlen; i++)
        free(iov[i].iov_base);
    free(iov);
}

static bool unix_socket_get_peer_cred(struct fd *sock, struct ucred_ *cred) {
    bool have_cred = false;
    lock(&peer_lock, 0);
    if (sock->socket.unix_peer_cred_valid) {
        *cred = sock->socket.unix_peer_cred;
        have_cred = true;
    } else if (sock->socket.unix_peer != NULL) {
        *cred = sock->socket.unix_peer->socket.unix_cred;
        have_cred = true;
    }
    unlock(&peer_lock);
    return have_cred;
}

static int_t sys_sendmsg_guest_abi(fd_t sock_fd, guest_addr_t msghdr_addr, int_t flags,
        enum guest_abi abi) {
    int err;
    STRACE("sendmsg(%d, %#llx, %d)", sock_fd, (unsigned long long) msghdr_addr, flags);
    int_t sock_err;
    struct fd *sock = sock_getfd(sock_fd, &sock_err);
    if (sock == NULL)
        return sock_err;

    struct msghdr msg = {};
    struct guest_msghdr_marshaled msg_fake;
    err = read_guest_msghdr(msghdr_addr, abi, &msg_fake);
    if (err < 0)
        return err;

    // msg_name
    struct sockaddr_max_ msg_name;
    if (msg_fake.msg_name != 0) {
        int err = sockaddr_read(msg_fake.msg_name, &msg_name, &msg_fake.msg_namelen);
        if (err < 0)
            return err;
        if (sock->socket.domain == AF_INET_)
            inet_nat_rewrite_dest(sock, &msg_name);
        msg.msg_name = &msg_name;
        msg.msg_namelen = msg_fake.msg_namelen;
    } else {
        msg.msg_name = NULL;
    }

    // msg_iovec
    struct guest_iovec_ *msg_iov_fake = user_read_iovecs_abi(current, abi, msg_fake.msg_iov, msg_fake.msg_iovlen);
    if (IS_ERR(msg_iov_fake))
        return PTR_ERR(msg_iov_fake);
    struct iovec *msg_iov = NULL;
    if (msg_fake.msg_iovlen != 0) {
        msg_iov = calloc(msg_fake.msg_iovlen, sizeof(*msg_iov));
        if (msg_iov == NULL) {
            free(msg_iov_fake);
            return _ENOMEM;
        }
    }
    msg.msg_iov = msg_iov;
    msg.msg_iovlen = msg_fake.msg_iovlen;
    for (size_t i = 0; i < (size_t) msg.msg_iovlen; i++) {
        msg_iov[i].iov_len = msg_iov_fake[i].len;
        msg_iov[i].iov_base = malloc(msg_iov_fake[i].len);
        err = _EFAULT;
        if (user_read(msg_iov_fake[i].base, msg_iov[i].iov_base, msg_iov_fake[i].len))
            goto out_free_iov;
    }

    // A socket that fell back to the built-in /dev/log or initctl sink (no real
    // listener at connect time) discards its writes.
    if (sock_is_devlog_sink(sock) || sock_is_initctl_sink(sock)) {
        size_t total = 0;
        for (size_t i = 0; i < (size_t) msg.msg_iovlen; i++)
            total += msg_iov[i].iov_len;
        err = (int_t) total;
        goto out_free_iov;
    }
    // A connectionless sendmsg() to /dev/log or initctl delivers for real if a
    // daemon is bound there, and otherwise falls back to discarding (handled at
    // the send-error path below).
    bool sendmsg_devlog_fallback = sock->socket.domain == AF_LOCAL_ &&
        (guest_sockaddr_is_devlog(msg_fake.msg_name, msg_fake.msg_namelen) ||
         guest_sockaddr_is_initctl(msg_fake.msg_name, msg_fake.msg_namelen));

    if (sock->socket.domain == AF_NETLINK_) {
        err = netlink_handle_sendmsg(sock, &msg);
        goto out_free_iov;
    }

    // msg_control
    uint8_t msg_control_buf[2048];
    uint8_t *msg_control = NULL;
    if (msg_fake.msg_control != 0) {
        if (msg_fake.msg_controllen > sizeof(msg_control_buf)) {
            err = _EINVAL;
            goto out_free_iov;
        }
        msg_control = msg_control_buf;
        err = _EFAULT;
        if (user_read(msg_fake.msg_control, msg_control, msg_fake.msg_controllen))
            goto out_free_iov;
    }
    msg.msg_control = NULL;
    msg.msg_controllen = 0;

    int real_flags = sock_flags_to_real(flags);
    if (real_flags < 0) {
        err = _EINVAL;
        goto out_free_iov;
    }

    struct scm *scm = NULL;
    uint64_t dgram_scm_cookie = 0; // nonzero once a dgram parcel is registered
    char real_msg_control[CMSG_SPACE(sizeof(int))]; // only used if actually sending an fd
    if (sock->socket.domain == AF_LOCAL_ && msg_control != NULL &&
            msg_fake.msg_controllen >= guest_cmsg_hdr_size(abi)) {
        err = unix_socket_finish_peer(sock);
        if (err < 0)
            goto out_free_iov;
        // figure out how many file descriptors we're sending
        unsigned num_fds = 0;
        struct ucred_ sender_cred = {};
        fill_cred(&sender_cred);
        size_t cmsg_off = 0;
        while (cmsg_off < msg_fake.msg_controllen) {
            struct guest_cmsghdr_marshaled cmsg;
            const uint8_t *cmsg_data;
            size_t data_len;
            if (!guest_cmsg_parse(abi, msg_control, msg_fake.msg_controllen, &cmsg_off, &cmsg, &cmsg_data, &data_len)) {
                err = _EINVAL;
                goto out_free_iov;
            }
            if (cmsg.level != SOL_SOCKET_)
                continue;
            if (cmsg.type == SCM_RIGHTS_) {
                if (data_len % sizeof(fd_t) != 0)
                    goto out_inval;
                num_fds += data_len / sizeof(fd_t);
            } else if (cmsg.type == SCM_CREDENTIALS_) {
                if (data_len != sizeof(struct ucred_))
                    goto out_inval;
                const struct ucred_ *cred = (const struct ucred_ *) cmsg_data;
                if (cred->pid != sender_cred.pid ||
                        cred->uid != sender_cred.uid ||
                        cred->gid != sender_cred.gid)
                    goto out_perm;
            } else {
                goto out_inval;
            }
        }
        if (num_fds > 253) // *magic*
            goto out_inval;

        if (num_fds > 0) {
            // The dgram transport carries the parcel by cookie in the
            // in-band header instead (registered below, after the fds are
            // collected); only the stream transport uses the sentinel-fd +
            // peer-queue scheme.
            if (!sock_is_unix_dgram(sock)) {
            // send one (1) real fd and put the rest in a struct scm
            static int real_fd = -1;
            if (real_fd == -1) {
                real_fd = open(".", O_RDONLY);
                if (real_fd < 0)
                    ERRNO_DIE("no");
            }
            msg.msg_control = real_msg_control;
            msg.msg_controllen = sizeof(real_msg_control);
            struct cmsghdr *real_cmsg = CMSG_FIRSTHDR(&msg);
            real_cmsg->cmsg_level = SOL_SOCKET;
            real_cmsg->cmsg_type = SCM_RIGHTS;
            real_cmsg->cmsg_len = CMSG_LEN(sizeof(real_fd));
            memcpy(CMSG_DATA(real_cmsg), &real_fd, sizeof(real_fd));
            }

            scm = malloc(sizeof(struct scm) + num_fds * sizeof(struct fd *));
            list_init(&scm->queue);
            scm->num_fds = num_fds;
            unsigned fd_i = 0;
            cmsg_off = 0;
            while (cmsg_off < msg_fake.msg_controllen) {
                struct guest_cmsghdr_marshaled cmsg;
                const uint8_t *cmsg_data;
                size_t data_len;
                if (!guest_cmsg_parse(abi, msg_control, msg_fake.msg_controllen, &cmsg_off, &cmsg, &cmsg_data, &data_len)) {
                    err = _EINVAL;
                    goto out_free_scm;
                }
                if (cmsg.level != SOL_SOCKET_ || cmsg.type != SCM_RIGHTS_)
                    continue;
                const fd_t *fds = (const fd_t *) cmsg_data;
                for (unsigned i = 0; i < data_len / sizeof(fd_t); i++) {
                    STRACE(" sending fd %d", fds[i]);
                    scm->fds[fd_i++] = fd_retain(f_get(fds[i]));
                }
            }
            if (sock_is_unix_dgram(sock)) {
                // Datagram transport: park the parcel in the registry and
                // ship its cookie in the in-band header (filled below).
                // There may legitimately be no unix_peer (sendmsg with
                // msg_name, many-senders-one-receiver sockets like
                // /run/systemd/notify).
                dgram_scm_cookie = unix_dgram_scm_register(scm);
                if (dgram_scm_cookie == 0) {
                    err = _ENOMEM;
                    goto out_free_scm;
                }
            } else {
            lock(&peer_lock, 0);
            struct fd *peer = sock->socket.unix_peer;
            if (peer == NULL) {
                printk("INFO: scm-send pid=%d EPIPE: unix_peer is NULL on sock real_fd=%d\n",
                       current ? current->pid : -1, sock->real_fd);
                unlock(&peer_lock);
                err = _EPIPE;
                goto out_free_scm;
            }
            printk("INFO: scm-send pid=%d num_fds=%u sock_real=%d peer_real=%d real_ctrl_len=%zu\n",
                   current ? current->pid : -1, num_fds, sock->real_fd, peer->real_fd,
                   msg.msg_controllen);
            lock(&peer->lock, 0);
            list_add_tail(&peer->socket.unix_scm, &scm->queue);
            unlock(&peer->lock);
            unlock(&peer_lock);
            }
        }
    }

    msg.msg_flags = sock_flags_to_real(msg_fake.msg_flags);
    err = _EINVAL;
    if (msg.msg_flags < 0)
        goto out_free_scm;

    sock_trace_sockaddr("local", sock->real_fd);
    sock_trace_sockaddr("peer", sock->real_fd);
    sock_trace_iov_preview(sock, msg.msg_iov, msg.msg_iovlen);
#if defined(__APPLE__)
    sock_trace_tcp_info("sendmsg-before", sock);
#endif

    size_t requested = sock_iov_requested(msg.msg_iov, msg.msg_iovlen);
    // AF_LOCAL datagrams carry the sender's guest creds in-band (see struct
    // unix_dgram_cred_hdr). Use a shadow msghdr with a prepended header iov
    // so the original msg (and its cleanup paths) stay untouched.
    bool unix_dgram_send = sock_is_unix_dgram(sock);
    struct unix_dgram_cred_hdr dgram_hdr;
    struct iovec *dgram_iov = NULL;
    struct msghdr host_send_msg = msg;
    if (unix_dgram_send) {
        unix_dgram_cred_hdr_fill(&dgram_hdr);
        dgram_hdr.scm_cookie = dgram_scm_cookie;
        dgram_iov = malloc((msg.msg_iovlen + 1) * sizeof(struct iovec));
        if (dgram_iov == NULL) {
            err = _ENOMEM;
            goto out_free_scm;
        }
        dgram_iov[0] = (struct iovec) {.iov_base = &dgram_hdr, .iov_len = sizeof(dgram_hdr)};
        memcpy(&dgram_iov[1], msg.msg_iov, msg.msg_iovlen * sizeof(struct iovec));
        host_send_msg.msg_iov = dgram_iov;
        host_send_msg.msg_iovlen = msg.msg_iovlen + 1;
    }
    ssize_t send_res = 0;
    // See the sendto path: a blocking stream send has to transmit the whole
    // request, and with the host call now nonblocking that loop is ours.
    bool send_all = !unix_dgram_send &&
        socket_call_is_blocking(sock, real_flags) && socket_is_stream(sock);
    size_t send_total = 0;
    struct iovec *resume_iov = NULL;
    int resume_iovlen = 0;
    struct msghdr resume_msg = {};
    struct socket_io_wait wait = {};
    socket_force_host_nonblock(sock);
    TASK_MAY_BLOCK {
        while (1) {
            errno = 0;
            struct msghdr *host_msg = unix_dgram_send ? &host_send_msg : &msg;
            if (resume_iov != NULL)
                host_msg = &resume_msg;
            send_res = sendmsg(sock->real_fd, host_msg, real_flags);
            if (send_res >= 0) {
                if (!send_all)
                    break;
                send_total += (size_t) send_res;
                if (send_res == 0 || send_total >= requested) {
                    send_res = (ssize_t) send_total;
                    break;
                }
                if (resume_iov == NULL) {
                    resume_iov = malloc((size_t) msg.msg_iovlen * sizeof(*resume_iov));
                    if (resume_iov == NULL) {
                        send_res = (ssize_t) send_total;
                        break;
                    }
                    memcpy(resume_iov, msg.msg_iov, (size_t) msg.msg_iovlen * sizeof(*resume_iov));
                    resume_iovlen = (int) msg.msg_iovlen;
                    resume_msg = msg;
                    // Any control data went out with the bytes already sent;
                    // repeating it on the continuation would hand the peer a
                    // second copy of the SCM_RIGHTS parcel.
                    resume_msg.msg_control = NULL;
                    resume_msg.msg_controllen = 0;
                    socket_iov_advance(resume_iov, &resume_iovlen, send_total);
                } else {
                    socket_iov_advance(resume_iov, &resume_iovlen, (size_t) send_res);
                }
                resume_msg.msg_iov = resume_iov;
                resume_msg.msg_iovlen = resume_iovlen;
                int wait_err = socket_wait_ready(sock, POLLOUT, &wait);
                if (wait_err < 0) {
                    send_res = (ssize_t) send_total;
                    break;
                }
                continue;
            }
            if (socket_should_retry_io_eintr(sock, real_flags))
                continue;
            if (socket_should_retry_io_eagain(sock, real_flags)) {
                int wait_err = socket_wait_ready(sock, POLLOUT, &wait);
                if (wait_err < 0) {
                    send_res = send_total > 0 ? (ssize_t) send_total : wait_err;
                    break;
                }
                continue;
            }
            if (send_total > 0)
                send_res = (ssize_t) send_total;
            break;
        }
    }
    free(resume_iov);
    free(dgram_iov);
    if (unix_dgram_send && send_res >= (ssize_t) sizeof(dgram_hdr))
        send_res -= sizeof(dgram_hdr);
    if (send_res < 0) {
        if (send_res > -4096 && send_res < 0 && errno == 0) {
            err = (int) send_res;
            goto out_free_scm;
        }
        if (socket_should_map_unix_eperm_to_eagain(sock, real_flags)) {
            err = _EAGAIN;
            sock_trace("sendmsg", sock, -1, err);
            sock_debug_event("sendmsg", sock, -1, err);
            sock_x11_event("sendmsg-eagain", sock, -1, err, requested);
            goto out_free_scm;
        }
        err = errno_map_flags(flags & MSG_NOSIGNAL_);   // MSG_NOSIGNAL
        // A /dev/log or initctl path whose socket exists but has no live reader
        // falls back to discarding rather than failing the send.
        if (sendmsg_devlog_fallback &&
                (err == _ECONNREFUSED || err == _ENOTCONN || err == _ENOENT)) {
            err = (int_t) requested;
            goto out_free_scm;
        }
        sock_translate_err(sock, &err);
        sock_trace("sendmsg", sock, -1, err);
        sock_debug_event("sendmsg", sock, -1, err);
        if (err == _EAGAIN)
            sock_x11_event("sendmsg-eagain", sock, -1, err, requested);
        else
            sock_x11_event("sendmsg-err", sock, -1, err, requested);
        if (scm != NULL)
            printk("INFO: scm-send pid=%d real sendmsg FAILED: errno=%d err=%d sock_real=%d\n",
                   current ? current->pid : -1, errno, err, sock->real_fd);
        goto out_free_scm;
    }
    err = send_res;
    sock_trace("sendmsg", sock, err, 0);
    sock_debug_event("sendmsg", sock, err, 0);
    if ((size_t) send_res != requested)
        sock_x11_event("sendmsg-short", sock, send_res, 0, requested);
    if (scm != NULL)
        printk("INFO: scm-send pid=%d real sendmsg OK: sent=%d sock_real=%d ctrl_len=%zu\n",
               current ? current->pid : -1, err, sock->real_fd, msg.msg_controllen);
#if defined(__APPLE__)
    sock_trace_tcp_info("sendmsg-after", sock);
#endif
    goto out_free_iov;

out_free_scm:
    if (scm != NULL) {
        if (dgram_scm_cookie != 0) {
            // The failed datagram never delivered its cookie; reclaim the
            // parcel from the registry (take may return NULL only if it
            // already expired -- then the GC freed it for us).
            if (unix_dgram_scm_take(dgram_scm_cookie) != NULL)
                scm_free(scm);
        } else {
            lock(&peer_lock, 0);
            struct fd *peer = sock->socket.unix_peer;
            if (peer != NULL) {
                lock(&peer->lock, 0);
                list_remove_safe(&scm->queue);
                unlock(&peer->lock);
            }
            unlock(&peer_lock);
            scm_free(scm);
        }
    }
    goto out_free_iov;
out_perm:
    err = _EPERM;
    goto out_free_iov;
out_inval:
    err = _EINVAL;
out_free_iov:
    free_msghdr_iov(msg_iov, msg.msg_iovlen);
    free(msg_iov_fake);
    return err;
}

int_t sys_sendmsg_guest(fd_t sock_fd, guest_addr_t msghdr_addr, int_t flags) {
    return sys_sendmsg_guest_abi(sock_fd, msghdr_addr, flags, GUEST_ABI_I386);
}

int_t sys_sendmsg(fd_t sock_fd, addr_t msghdr_addr, int_t flags) {
    return sys_sendmsg_guest(sock_fd, msghdr_addr, flags);
}

int_t sys_sendmsg_amd64(fd_t sock_fd, addr_t msghdr_addr, int_t flags) {
    return sys_sendmsg_guest_abi(sock_fd, msghdr_addr, flags, GUEST_ABI_AMD64);
}

int_t sys_sendmsg_amd64_guest(fd_t sock_fd, guest_addr_t msghdr_addr, int_t flags) {
    return sys_sendmsg_guest_abi(sock_fd, msghdr_addr, flags, GUEST_ABI_AMD64);
}

static int ipv6_recverr_errno_from_icmp6(uint8_t type, uint8_t code) {
    switch (type) {
        case ICMP6_DST_UNREACH:
            return code == ICMP6_DST_UNREACH_NOPORT ? ECONNREFUSED : EHOSTUNREACH;
        case ICMP6_PACKET_TOO_BIG:
            return EMSGSIZE;
        case ICMP6_TIME_EXCEEDED:
            return EHOSTUNREACH;
        case ICMP6_PARAM_PROB:
            return EPROTO;
        default:
            return EHOSTUNREACH;
    }
}

static int ipv6_recverr_fd_get(struct fd *sock) {
    if (sock->socket.ipv6_recverr_fd >= 0)
        return sock->socket.ipv6_recverr_fd;
    int fd = socket(AF_INET6, SOCK_DGRAM, IPPROTO_ICMPV6);
    if (fd < 0)
        return -1;
    sock->socket.ipv6_recverr_fd = fd;
    return fd;
}

static bool ipv6_recverr_matches_socket(struct fd *sock, const struct ip6_hdr *ip6,
        const struct udphdr *udp) {
    struct sockaddr_in6 peer = {};
    socklen_t peer_len = sizeof(peer);
    if (getpeername(sock->real_fd, (struct sockaddr *) &peer, &peer_len) < 0)
        return false;
    if (peer_len < sizeof(peer))
        return false;
    if (memcmp(&ip6->ip6_dst, &peer.sin6_addr, sizeof(peer.sin6_addr)) != 0)
        return false;
    if (udp->uh_dport != peer.sin6_port)
        return false;

    struct sockaddr_in6 local = {};
    socklen_t local_len = sizeof(local);
    if (getsockname(sock->real_fd, (struct sockaddr *) &local, &local_len) == 0 &&
            local_len >= sizeof(local)) {
        if (local.sin6_port != 0 && udp->uh_sport != local.sin6_port)
            return false;
    }
    return true;
}

static ssize_t recvmsg_ipv6_errqueue(struct fd *sock, struct msghdr *msg, int real_flags) {
    int errfd = ipv6_recverr_fd_get(sock);
    if (errfd < 0) {
        errno = EOPNOTSUPP;
        return -1;
    }

    int recv_flags = real_flags & MSG_DONTWAIT;
    while (1) {
        uint8_t packet[2048];
        struct sockaddr_in6 from = {};
        struct iovec iov = {.iov_base = packet, .iov_len = sizeof(packet)};
        struct msghdr raw = {
            .msg_name = &from,
            .msg_namelen = sizeof(from),
            .msg_iov = &iov,
            .msg_iovlen = 1,
        };
        ssize_t n = recvmsg(errfd, &raw, recv_flags);
        if (n < 0)
            return -1;
        if ((size_t) n < sizeof(struct icmp6_hdr) + sizeof(struct ip6_hdr) + sizeof(struct udphdr))
            continue;

        const struct icmp6_hdr *icmp6 = (const struct icmp6_hdr *) packet;
        switch (icmp6->icmp6_type) {
            case ICMP6_DST_UNREACH:
            case ICMP6_PACKET_TOO_BIG:
            case ICMP6_TIME_EXCEEDED:
            case ICMP6_PARAM_PROB:
                break;
            default:
                continue;
        }

        const uint8_t *quoted = packet + sizeof(struct icmp6_hdr);
        size_t quoted_len = (size_t) n - sizeof(struct icmp6_hdr);
        if (quoted_len < sizeof(struct ip6_hdr) + sizeof(struct udphdr))
            continue;

        const struct ip6_hdr *inner_ip6 = (const struct ip6_hdr *) quoted;
        if (inner_ip6->ip6_nxt != IPPROTO_UDP)
            continue;
        const struct udphdr *inner_udp =
            (const struct udphdr *) (quoted + sizeof(struct ip6_hdr));
        if (!ipv6_recverr_matches_socket(sock, inner_ip6, inner_udp))
            continue;

        if (msg->msg_name != NULL) {
            size_t copy_len = msg->msg_namelen < sizeof(from) ? msg->msg_namelen : sizeof(from);
            memcpy(msg->msg_name, &from, copy_len);
            msg->msg_namelen = sizeof(from);
        }

        if (msg->msg_control != NULL &&
                msg->msg_controllen >= CMSG_SPACE(sizeof(struct sock_extended_err_) + sizeof(struct sockaddr_in6))) {
            struct cmsghdr *cmsg = (struct cmsghdr *) msg->msg_control;
            cmsg->cmsg_level = IPPROTO_IPV6;
            cmsg->cmsg_type = IPV6_RECVERR_;
            cmsg->cmsg_len = CMSG_LEN(sizeof(struct sock_extended_err_) + sizeof(struct sockaddr_in6));

            struct sock_extended_err_ serr = {
                .ee_errno = ipv6_recverr_errno_from_icmp6(icmp6->icmp6_type, icmp6->icmp6_code),
                .ee_origin = SO_EE_ORIGIN_ICMP6_,
                .ee_type = icmp6->icmp6_type,
                .ee_code = icmp6->icmp6_code,
                .ee_info = icmp6->icmp6_type == ICMP6_PACKET_TOO_BIG ? ntohl(icmp6->icmp6_mtu) : 0,
                .ee_data = 0,
            };
            memcpy(CMSG_DATA(cmsg), &serr, sizeof(serr));
            memcpy((uint8_t *) CMSG_DATA(cmsg) + sizeof(serr), &from, sizeof(from));
            msg->msg_controllen = CMSG_SPACE(sizeof(struct sock_extended_err_) + sizeof(struct sockaddr_in6));
        } else {
            msg->msg_controllen = 0;
            msg->msg_flags |= MSG_CTRUNC;
        }

        size_t payload_len = quoted_len;
        size_t remaining = payload_len;
        const uint8_t *src = quoted;
        for (size_t i = 0; i < (size_t) msg->msg_iovlen && remaining > 0; i++) {
            size_t chunk = msg->msg_iov[i].iov_len;
            if (chunk > remaining)
                chunk = remaining;
            memcpy(msg->msg_iov[i].iov_base, src, chunk);
            src += chunk;
            remaining -= chunk;
        }
        msg->msg_flags &= ~MSG_TRUNC;
        if (remaining > 0)
            msg->msg_flags |= MSG_TRUNC;
        return payload_len;
    }
}

static int_t sys_recvmsg_guest_abi(fd_t sock_fd, guest_addr_t msghdr_addr, int_t flags,
        enum guest_abi abi) {
    STRACE("recvmsg(%d, %#llx, %d)", sock_fd, (unsigned long long) msghdr_addr, flags);
    int_t sock_err;
    struct fd *sock = sock_getfd(sock_fd, &sock_err);
    if (sock == NULL)
        return sock_err;

    struct msghdr msg = {};
    struct guest_msghdr_marshaled msg_fake;
    int err = read_guest_msghdr(msghdr_addr, abi, &msg_fake);
    if (err < 0)
        return err;

    int real_flags = sock_flags_to_real(flags);
    if (real_flags < 0)
        return _EINVAL;

    // msg_iovec (no initial content)
    struct guest_iovec_ *msg_iov_fake = user_read_iovecs_abi(current, abi, msg_fake.msg_iov, msg_fake.msg_iovlen);
    if (IS_ERR(msg_iov_fake))
        return PTR_ERR(msg_iov_fake);
    struct iovec *msg_iov = NULL;
    if (msg_fake.msg_iovlen != 0) {
        msg_iov = calloc(msg_fake.msg_iovlen, sizeof(*msg_iov));
        if (msg_iov == NULL) {
            free(msg_iov_fake);
            return _ENOMEM;
        }
    }

    // msg_name
    char msg_name_stack[128];
    char *msg_name = msg_name_stack;
    if (msg_fake.msg_namelen > sizeof(msg_name_stack)) {
        msg_name = malloc(msg_fake.msg_namelen);
        if (msg_name == NULL) {
            free(msg_iov_fake);
            free(msg_iov);
            return _ENOMEM;
        }
    }
    if (msg_fake.msg_name != 0) {
        msg.msg_name = msg_name;
        msg.msg_namelen = msg_fake.msg_namelen;
    } else {
        msg.msg_name = NULL;
        msg.msg_namelen = 0;
    }

    uint_t guest_controllen_max = msg_fake.msg_controllen;
    char local_rights_msg_control[CMSG_SPACE(sizeof(int))] = {};
    void *real_msg_control = NULL;
    // On MSG_PEEK over an AF_LOCAL socket, do NOT request ancillary data from
    // the host. macOS duplicates the SCM_RIGHTS sentinel fd on a peek and that
    // disturbs the control data still queued for the message, so the following
    // real recvmsg delivered a corrupt/mismatched fd (reproduced: peek then
    // read yields the sentinel/directory fd -> EBADF on the guest's first
    // read). Peeking only the data leaves our scm queue and the host's
    // ancillary pristine; the fds arrive correctly on the real read, which is
    // exactly what peek-to-size callers (dbus, systemd) do.
    bool peek_af_local = (flags & MSG_PEEK_) && sock->socket.domain == AF_LOCAL_;
    if (msg_fake.msg_controllen != 0 && !peek_af_local) {
        size_t real_msg_controllen = guest_controllen_max;
        if (sock->socket.domain == AF_LOCAL_ && real_msg_controllen < sizeof(local_rights_msg_control))
            real_msg_controllen = sizeof(local_rights_msg_control);
        real_msg_control = calloc(1, real_msg_controllen);
        if (real_msg_control == NULL) {
            free(msg_iov_fake);
            free(msg_iov);
            if (msg_name != msg_name_stack)
                free(msg_name);
            return _ENOMEM;
        }
        msg.msg_control = real_msg_control;
        msg.msg_controllen = real_msg_controllen;
    } else {
        msg.msg_control = NULL;
        msg.msg_controllen = 0;
    }
    msg.msg_iov = msg_iov;
    msg.msg_iovlen = msg_fake.msg_iovlen;
    for (size_t i = 0; i < (size_t) msg.msg_iovlen; i++) {
        msg_iov[i].iov_len = msg_iov_fake[i].len;
        msg_iov[i].iov_base = malloc(msg_iov_fake[i].len);
    }

    if (sock_is_initctl_sink(sock)) {
        msg_fake.msg_namelen = 0;
        msg_fake.msg_controllen = 0;
        msg_fake.msg_flags = 0;
        free_msghdr_iov(msg_iov, msg.msg_iovlen);
        free(msg_iov_fake);
        free(real_msg_control);
        if (msg_name != msg_name_stack)
            free(msg_name);
        err = write_guest_msghdr(msghdr_addr, abi, &msg_fake);
        if (err < 0)
            return err;
        return 0;
    }

    if (sock->socket.domain == AF_NETLINK_) {
        ssize_t res = netlink_handle_recvmsg(sock, &msg, flags);
        if (res >= 0) {
            size_t n = (size_t) res;
            for (size_t i = 0; i < (size_t) msg.msg_iovlen; i++) {
                size_t chunk_size = msg_iov[i].iov_len;
                if (chunk_size > n)
                    chunk_size = n;
                if (chunk_size != 0)
                    if (user_write(msg_iov_fake[i].base, msg_iov[i].iov_base, chunk_size)) {
                        free_msghdr_iov(msg_iov, msg.msg_iovlen);
                        free(msg_iov_fake);
                        free(real_msg_control);
                        if (msg_name != msg_name_stack)
                            free(msg_name);
                        return _EFAULT;
                    }
                n -= chunk_size;
            }
        }
        free_msghdr_iov(msg_iov, msg.msg_iovlen);
        free(msg_iov_fake);
        free(real_msg_control);
        if (res < 0) {
            if (msg_name != msg_name_stack)
                free(msg_name);
            return res;
        }
        if (msg.msg_name != NULL) {
            int err = netlink_sockaddr_write(msg_fake.msg_name, msg.msg_name, &msg.msg_namelen);
            if (err < 0) {
                if (msg_name != msg_name_stack)
                    free(msg_name);
                return err;
            }
        }
        msg_fake.msg_namelen = msg.msg_namelen;
        msg_fake.msg_controllen = 0;
        msg_fake.msg_flags = sock_flags_from_real(msg.msg_flags);
        if (msg_name != msg_name_stack)
            free(msg_name);
        err = write_guest_msghdr(msghdr_addr, abi, &msg_fake);
        if (err < 0)
            return err;
        return res;
    }

    if (sock->socket.domain == AF_LOCAL_) {
        int peer_err = unix_socket_finish_peer(sock);
        if (peer_err < 0) {
            free_msghdr_iov(msg_iov, msg.msg_iovlen);
            free(msg_iov_fake);
            free(real_msg_control);
            if (msg_name != msg_name_stack)
                free(msg_name);
            return peer_err;
        }
    }

    // AF_LOCAL datagrams arrive with an in-band cred header (see struct
    // unix_dgram_cred_hdr): receive through a shadow msghdr whose first iov
    // lands the header in a scratch struct, leaving the caller's iovs to
    // receive only real payload. The original msg (and its cleanup paths)
    // stay untouched; mutated fields are copied back after the call.
    bool unix_dgram = sock_is_unix_dgram(sock);
    struct unix_dgram_cred_hdr dgram_hdr = {};
    struct iovec *dgram_iov = NULL;
    struct msghdr host_recv_msg = msg;
    if (unix_dgram) {
        dgram_iov = malloc((msg.msg_iovlen + 1) * sizeof(struct iovec));
        if (dgram_iov == NULL) {
            free_msghdr_iov(msg_iov, msg.msg_iovlen);
            free(msg_iov_fake);
            free(real_msg_control);
            if (msg_name != msg_name_stack)
                free(msg_name);
            return _ENOMEM;
        }
        dgram_iov[0] = (struct iovec) {.iov_base = &dgram_hdr, .iov_len = sizeof(dgram_hdr)};
        memcpy(&dgram_iov[1], msg.msg_iov, msg.msg_iovlen * sizeof(struct iovec));
        host_recv_msg.msg_iov = dgram_iov;
        host_recv_msg.msg_iovlen = msg.msg_iovlen + 1;
    }
    struct msghdr *host_msg = unix_dgram ? &host_recv_msg : &msg;
    ssize_t res = 0;
    // See the recvfrom path for why MSG_WAITALL cannot reach a nonblocking
    // host socket. Same emulation, over the iovec array instead of a buffer.
    size_t recv_requested = sock_iov_requested(msg.msg_iov, msg.msg_iovlen);
    bool waitall = (real_flags & MSG_WAITALL) && !unix_dgram &&
        socket_call_is_blocking(sock, real_flags) && socket_is_stream(sock);
    bool waitall_peek = waitall && (real_flags & MSG_PEEK);
    int host_flags = waitall ? (real_flags & ~MSG_WAITALL) : real_flags;
    size_t got = 0;
    struct iovec *resume_iov = NULL;
    int resume_iovlen = 0;
    struct msghdr resume_msg = {};
    struct socket_io_wait wait = {};
    socket_force_host_nonblock(sock);
    TASK_MAY_BLOCK {
        bool use_ipv6_errqueue =
            (flags & MSG_ERRQUEUE_) &&
            sock->socket.domain == AF_INET6_ &&
            sock->socket.type == SOCK_DGRAM_ &&
            sock->socket.ipv6_recverr;
        while (1) {
            sigset_t oldmask;
            if (!socket_blocking_syscall_begin(&oldmask)) {
                res = errno_map();
                errno = 0;
                break;
            }
            errno = 0;
            struct msghdr *attempt = resume_iov != NULL ? &resume_msg : host_msg;
            if (use_ipv6_errqueue)
                res = recvmsg_ipv6_errqueue(sock, attempt, host_flags);
            else
                res = recvmsg(sock->real_fd, attempt, host_flags);
            socket_blocking_syscall_end();
            if (res >= 0) {
                if (!waitall)
                    break;
                if (waitall_peek) {
                    // Nothing to accumulate: a peek re-reads from the front.
                    if (res == 0 || (size_t) res >= recv_requested)
                        break;
                } else {
                    got += (size_t) res;
                    if (res == 0 || got >= recv_requested) {
                        res = (ssize_t) got;
                        break;
                    }
                    if (resume_iov == NULL) {
                        resume_iov = malloc((size_t) msg.msg_iovlen * sizeof(*resume_iov));
                        if (resume_iov == NULL) {
                            res = (ssize_t) got;
                            break;
                        }
                        memcpy(resume_iov, msg.msg_iov,
                               (size_t) msg.msg_iovlen * sizeof(*resume_iov));
                        resume_iovlen = (int) msg.msg_iovlen;
                        resume_msg = *host_msg;
                        // The name and control buffers were filled by the
                        // first chunk; a continuation must not overwrite them.
                        resume_msg.msg_name = NULL;
                        resume_msg.msg_namelen = 0;
                        resume_msg.msg_control = NULL;
                        resume_msg.msg_controllen = 0;
                        socket_iov_advance(resume_iov, &resume_iovlen, got);
                    } else {
                        socket_iov_advance(resume_iov, &resume_iovlen, (size_t) res);
                    }
                    resume_msg.msg_iov = resume_iov;
                    resume_msg.msg_iovlen = resume_iovlen;
                }
                int wait_err = socket_wait_ready(sock, POLLIN, &wait);
                if (wait_err < 0) {
                    if (!waitall_peek)
                        res = (ssize_t) got;
                    break;
                }
                continue;
            }
            if (socket_should_retry_io_eintr(sock, real_flags))
                continue;
            if (socket_should_retry_io_eagain(sock, real_flags)) {
                int wait_err = socket_wait_ready(sock, POLLIN, &wait);
                if (wait_err < 0) {
                    res = got > 0 ? (ssize_t) got : wait_err;
                    errno = 0;
                    break;
                }
                continue;
            }
            if (got > 0)
                res = (ssize_t) got;
            break;
        }
    }
    free(resume_iov);
    bool have_dgram_cred = false;
    struct scm *dgram_scm = NULL;
    if (unix_dgram) {
        // Copy back the fields the kernel mutates in the msghdr it was given.
        msg.msg_namelen = host_recv_msg.msg_namelen;
        msg.msg_controllen = host_recv_msg.msg_controllen;
        msg.msg_flags = host_recv_msg.msg_flags;
        free(dgram_iov);
        if (res >= (ssize_t) sizeof(dgram_hdr) && dgram_hdr.magic == UNIX_DGRAM_CRED_MAGIC) {
            res -= sizeof(dgram_hdr);
            have_dgram_cred = true;
            // In-band SCM_RIGHTS parcel (see unix_dgram_cred_hdr). Consume
            // only on a real read: like the stream transport, a MSG_PEEK
            // delivers the data without the fds, and the later real read
            // still finds the parcel registered.
            if (dgram_hdr.scm_cookie != 0 && !(flags & MSG_PEEK_))
                dgram_scm = unix_dgram_scm_take(dgram_hdr.scm_cookie);
        }
        // Recompute MSG_TRUNC from the guest's point of view. The 16-byte
        // in-band cred header consumes one host iov, so the host's own
        // MSG_TRUNC reflects truncation against (header + caller buffers),
        // not the caller's buffers alone -- and on macOS it comes back set
        // even for a datagram that fit (an input MSG_TRUNC the host echoes).
        // A spurious MSG_TRUNC on the notify socket is FATAL: systemd treats
        // it as "Received notify message exceeded maximum size" and drops
        // every sd_notify READY=1, so journald/udevd/etc. never leave
        // "activating" and boot wedges before basic.target. Set MSG_TRUNC iff
        // the delivered payload actually exceeded the caller's total buffer.
        if (res >= 0) {
            size_t caller_cap = 0;
            for (size_t i = 0; i < (size_t) msg.msg_iovlen; i++)
                caller_cap += msg.msg_iov[i].iov_len;
            if ((size_t) res > caller_cap)
                msg.msg_flags |= MSG_TRUNC;
            else
                msg.msg_flags &= ~MSG_TRUNC;
        }
    }
    size_t requested = sock_iov_requested(msg.msg_iov, msg.msg_iovlen);
    err = 0;
    if (res <= 0) {
        // A pending socket error outranks both EOF and EAGAIN, and AOK's own
        // poll probe may already have taken it off the host. See
        // sock_take_pending_error.
        int pending = sock_take_pending_error(sock);
        if (pending != 0) {
            res = -1;
            err = pending;
            sock_trace("recvmsg", sock, -1, err);
            sock_debug_event("recvmsg", sock, -1, err);
            goto out_recvmsg_done;
        }
    }
    if (res < 0) {
        if (res > -4096 && res < 0 && errno == 0)
            err = (int) res;
        else if (socket_should_map_unix_eperm_to_eagain(sock, real_flags))
            err = _EAGAIN;
        else
            err = errno_map();
        bool was_dead = sock->socket.conn_dead;
        sock_translate_err(sock, &err);
        // Already reported; a connection that is gone reads EOF from here on.
        if (was_dead && err == _ECONNRESET) {
            res = 0;
            err = 0;
            sock_trace("recvmsg", sock, 0, 0);
            goto out_recvmsg_done;
        }
        sock_trace("recvmsg", sock, -1, err);
        sock_debug_event("recvmsg", sock, -1, err);
        if (err == _EAGAIN)
            sock_x11_event("recvmsg-eagain", sock, -1, err, requested);
        else
            sock_x11_event("recvmsg-err", sock, -1, err, requested);
    } else {
        sock_trace("recvmsg", sock, res, 0);
        sock_debug_event("recvmsg", sock, res, 0);
        if (res == 0)
            sock_x11_event("recvmsg-eof", sock, 0, 0, requested);
        if (sock_trace_enabled()) {
            printk("INFO: net recvmsg-flags pid=%d comm=%s real=%d flags=%#x namelen=%u controllen=%zu\n",
                   current->pid, current->comm, sock->real_fd, msg.msg_flags,
                   (unsigned) msg.msg_namelen, msg.msg_controllen);
        }
    }
out_recvmsg_done:
    // don't return err quite yet, there are outstanding mallocs
    msg_fake.msg_flags = sock_flags_from_real(msg.msg_flags);

    // msg_iovec (changed)
    // copy as many bytes as were received, according to the return value
    size_t n = res;
    if (res < 0)
        n = 0;
    for (size_t i = 0; i < (size_t) msg.msg_iovlen; i++) {
        size_t chunk_size = msg_iov[i].iov_len;
        if (chunk_size > n)
            chunk_size = n;
        if (chunk_size > 0)
            if (user_write(msg_iov_fake[i].base, msg_iov[i].iov_base, chunk_size))
                goto out_recvmsg_fault;
        n -= chunk_size;
    }
    free_msghdr_iov(msg_iov, msg.msg_iovlen);
    free(msg_iov_fake);

    // msg_control (changed)
    msg_fake.msg_controllen = 0;
    struct cmsghdr *cmsg = host_cmsg_first(&msg);
    struct cmsghdr *rights_cmsg = NULL;
    bool have_rights = false;
    for (struct cmsghdr *iter = cmsg; iter != NULL; iter = host_cmsg_next(&msg, iter)) {
        if (sock->socket.domain == AF_LOCAL_ &&
                iter->cmsg_level == SOL_SOCKET && iter->cmsg_type == SCM_RIGHTS) {
            have_rights = true;
            rights_cmsg = iter;
            break;
        }
    }
    bool want_passcred = sock->socket.domain == AF_LOCAL_ &&
        sock->socket.unix_passcred && res >= 0;
    struct scm *scm = NULL;
    if (have_rights) {
        int dummy_fd = ((int *) CMSG_DATA(rights_cmsg))[0];
        close(dummy_fd);

        lock(&sock->lock, 0);
        if (list_empty(&sock->socket.unix_scm)) {
            // Defensive: the host delivered our SCM_RIGHTS sentinel fd but no
            // struct scm is queued on this socket -- the two channels (the
            // host-side sentinel carrying the message and our own scm queue
            // carrying the real guest fds) have desynced. Never abort the
            // emulator over it: an assert here SIGABRT'd all of iSH mid-boot
            // on amd64 Arch (the desync came from MSG_PEEK duplicating the
            // sentinel -- now suppressed above). Deliver the message without
            // the fds instead of crashing.
            unlock(&sock->lock);
            have_rights = false;
        } else {
            scm = list_first_entry(&sock->socket.unix_scm, struct scm, queue);
            list_remove(&scm->queue);
            unlock(&sock->lock);
        }

        if (scm != NULL && res < 0) {
            scm_free(scm);
            free(real_msg_control);
            if (msg_name != msg_name_stack)
                free(msg_name);
            return err;
        }
    }

    if (res >= 0 && msg_fake.msg_control != 0 &&
            (cmsg != NULL || want_passcred || dgram_scm != NULL)) {
        size_t guest_msg_control_len = 0;
        size_t required_msg_control = 0;
        struct ucred_ cred = {};
        // Prefer the per-datagram sender cred that traveled with this exact
        // datagram (many-senders-one-receiver sockets like systemd's notify
        // socket have no meaningful connected-peer cred); fall back to the
        // connected peer's for stream/connected sockets.
        bool have_passcred = false;
        if (want_passcred) {
            if (have_dgram_cred) {
                cred = dgram_hdr.cred;
                have_passcred = true;
            } else {
                have_passcred = unix_socket_get_peer_cred(sock, &cred);
            }
        }

        for (struct cmsghdr *iter = cmsg; iter != NULL; iter = host_cmsg_next(&msg, iter)) {
            if (iter->cmsg_len < CMSG_LEN(0))
                continue;
            size_t data_len = iter->cmsg_len - CMSG_LEN(0);
            if (sock->socket.domain == AF_LOCAL_ &&
                    iter->cmsg_level == SOL_SOCKET && iter->cmsg_type == SCM_RIGHTS) {
                if (have_rights)
                    required_msg_control += guest_cmsg_space(abi, sizeof(fd_t) * scm->num_fds);
                continue;
            }
            const void *fake_data;
            size_t fake_data_len;
            int fake_level;
            int fake_type;
            uint8_t scratch[sizeof(struct sock_extended_err_) + sizeof(struct sockaddr_in6_)];
            if (!sock_cmsg_translate_payload(iter->cmsg_level, iter->cmsg_type,
                        CMSG_DATA(iter), data_len, &fake_data, &fake_data_len,
                        &fake_level, &fake_type, scratch, sizeof(scratch)))
                continue;
            required_msg_control += guest_cmsg_space(abi, fake_data_len);
            (void) fake_level;
        }
        if (dgram_scm != NULL)
            required_msg_control += guest_cmsg_space(abi, sizeof(fd_t) * dgram_scm->num_fds);
        if (have_passcred)
            required_msg_control += guest_cmsg_space(abi, sizeof(cred));

        if (required_msg_control > guest_controllen_max) {
            msg_fake.msg_flags |= MSG_CTRUNC_;
        } else if (required_msg_control != 0) {
            uint8_t *guest_msg_control = calloc(1, required_msg_control);
            if (guest_msg_control == NULL) {
                if (scm != NULL)
                    scm_free(scm);
                if (dgram_scm != NULL)
                    scm_free(dgram_scm);
                free(real_msg_control);
                if (msg_name != msg_name_stack)
                    free(msg_name);
                return _ENOMEM;
            }
            for (struct cmsghdr *iter = cmsg; iter != NULL; iter = host_cmsg_next(&msg, iter)) {
                if (iter->cmsg_len < CMSG_LEN(0))
                    continue;
                size_t data_len = iter->cmsg_len - CMSG_LEN(0);
                if (sock->socket.domain == AF_LOCAL_ &&
                        iter->cmsg_level == SOL_SOCKET && iter->cmsg_type == SCM_RIGHTS) {
                    if (!have_rights)
                        continue;
                    fd_t fds[scm->num_fds];
                    for (unsigned i = 0; i < scm->num_fds; i++) {
                        fd_retain(scm->fds[i]); // f_install takes ownership; scm_free releases separately
                        fds[i] = f_install(scm->fds[i], 0);
                        STRACE(" receiving fd %d", fds[i]);
                    }
                    bool appended = guest_cmsg_append(abi, guest_msg_control, required_msg_control, &guest_msg_control_len,
                            SOL_SOCKET_, SCM_RIGHTS_, fds, sizeof(fd_t) * scm->num_fds);
                    assert(appended);
                    continue;
                }
                int fake_level = sock_cmsg_level_to_fake(iter->cmsg_level);
                const void *fake_data;
                size_t fake_data_len;
                int fake_type;
                uint8_t scratch[sizeof(struct sock_extended_err_) + sizeof(struct sockaddr_in6_)];
                if (!sock_cmsg_translate_payload(iter->cmsg_level, iter->cmsg_type,
                            CMSG_DATA(iter), data_len, &fake_data, &fake_data_len,
                            &fake_level, &fake_type, scratch, sizeof(scratch)))
                    continue;
                bool appended = guest_cmsg_append(abi, guest_msg_control, required_msg_control, &guest_msg_control_len,
                        fake_level, fake_type, fake_data, fake_data_len);
                assert(appended);
            }
            if (dgram_scm != NULL) {
                // In-band dgram parcel (no host cmsg carries it; the cookie
                // in the datagram header did).
                fd_t fds[dgram_scm->num_fds];
                for (unsigned i = 0; i < dgram_scm->num_fds; i++) {
                    fd_retain(dgram_scm->fds[i]); // f_install takes ownership; scm_free releases separately
                    fds[i] = f_install(dgram_scm->fds[i], 0);
                    STRACE(" receiving dgram fd %d", fds[i]);
                }
                bool appended = guest_cmsg_append(abi, guest_msg_control, required_msg_control, &guest_msg_control_len,
                        SOL_SOCKET_, SCM_RIGHTS_, fds, sizeof(fd_t) * dgram_scm->num_fds);
                assert(appended);
            }
            if (have_passcred) {
                bool appended = guest_cmsg_append(abi, guest_msg_control, required_msg_control, &guest_msg_control_len,
                        SOL_SOCKET_, SCM_CREDENTIALS_, &cred, sizeof(cred));
                assert(appended);
            }
            if (user_write(msg_fake.msg_control, guest_msg_control, guest_msg_control_len)) {
                if (scm != NULL)
                    scm_free(scm);
                if (dgram_scm != NULL)
                    scm_free(dgram_scm);
                free(guest_msg_control);
                free(real_msg_control);
                if (msg_name != msg_name_stack)
                    free(msg_name);
                return _EFAULT;
            }
            free(guest_msg_control);
            msg_fake.msg_controllen = guest_msg_control_len;
        }
    }
    if (scm != NULL)
        scm_free(scm);
    // Taken but undeliverable (no/too-small guest control buffer, CTRUNC,
    // error): the parcel's fds are released, matching Linux discarding
    // SCM_RIGHTS the receiver made no room for.
    if (dgram_scm != NULL)
        scm_free(dgram_scm);

    // by now the iovecs and scm have been freed so we can return
    if (res < 0) {
        free(real_msg_control);
        if (msg_name != msg_name_stack)
            free(msg_name);
        return err;
    }

    // msg_name (changed)
    if (msg.msg_name != 0) {
        if (sock->socket.domain == AF_INET_)
            inet_nat_rewrite_src(sock, msg.msg_name);
        int err = sockaddr_write(msg_fake.msg_name, msg.msg_name, msg_fake.msg_namelen, &msg.msg_namelen);
        if (err < 0) {
            free(real_msg_control);
            if (msg_name != msg_name_stack)
                free(msg_name);
            return err;
        }
    }
    msg_fake.msg_namelen = msg.msg_namelen;

    free(real_msg_control);
    if (msg_name != msg_name_stack)
        free(msg_name);
    err = write_guest_msghdr(msghdr_addr, abi, &msg_fake);
    if (err < 0)
        return err;
    return res;

out_recvmsg_fault:
    free_msghdr_iov(msg_iov, msg.msg_iovlen);
    free(msg_iov_fake);
    free(real_msg_control);
    if (msg_name != msg_name_stack)
        free(msg_name);
    return _EFAULT;
}

int_t sys_recvmsg_guest(fd_t sock_fd, guest_addr_t msghdr_addr, int_t flags) {
    return sys_recvmsg_guest_abi(sock_fd, msghdr_addr, flags, GUEST_ABI_I386);
}

int_t sys_recvmsg(fd_t sock_fd, addr_t msghdr_addr, int_t flags) {
    return sys_recvmsg_guest(sock_fd, msghdr_addr, flags);
}

int_t sys_recvmsg_amd64(fd_t sock_fd, addr_t msghdr_addr, int_t flags) {
    return sys_recvmsg_guest_abi(sock_fd, msghdr_addr, flags, GUEST_ABI_AMD64);
}

int_t sys_recvmsg_amd64_guest(fd_t sock_fd, guest_addr_t msghdr_addr, int_t flags) {
    return sys_recvmsg_guest_abi(sock_fd, msghdr_addr, flags, GUEST_ABI_AMD64);
}

static int_t sys_recvmmsg_guest_abi(fd_t sock_fd, guest_addr_t msg_vec, uint_t vec_len, int_t flags,
        guest_addr_t UNUSED(timeout_addr), enum guest_abi abi) {
    int num_received = 0;
    int recv_flags = flags;
    size_t msg_stride = guest_mmsghdr_size(abi);
    size_t msg_len_offset = guest_mmsghdr_len_offset(abi);
    for (unsigned i = 0; i < vec_len; i++) {
        guest_addr_t msghdr = msg_vec + i * msg_stride;
        int_t res = sys_recvmsg_guest_abi(sock_fd, msghdr, recv_flags, abi);
        if (res >= 0) {
            guest_addr_t msg_len_addr = msghdr + msg_len_offset;
            if (user_put(msg_len_addr, res))
                res = _EFAULT;
        }
        if (res < 0) {
            if (num_received > 0)
                break;
            return res;
        }
        num_received++;
        recv_flags |= MSG_DONTWAIT_;
    }
    return num_received;
}

int_t sys_recvmmsg_guest(fd_t sock_fd, guest_addr_t msg_vec, uint_t vec_len, int_t flags, guest_addr_t timeout_addr) {
    return sys_recvmmsg_guest_abi(sock_fd, msg_vec, vec_len, flags, timeout_addr, GUEST_ABI_I386);
}

int_t sys_recvmmsg(fd_t sock_fd, addr_t msg_vec, uint_t vec_len, int_t flags, addr_t timeout_addr) {
    return sys_recvmmsg_guest(sock_fd, msg_vec, vec_len, flags, timeout_addr);
}

int_t sys_recvmmsg_time64_guest(fd_t sock_fd, guest_addr_t msg_vec, uint_t vec_len, int_t flags, guest_addr_t timeout_addr) {
    return sys_recvmmsg_guest(sock_fd, msg_vec, vec_len, flags, timeout_addr);
}

int_t sys_recvmmsg_time64(fd_t sock_fd, addr_t msg_vec, uint_t vec_len, int_t flags, addr_t timeout_addr) {
    return sys_recvmmsg_time64_guest(sock_fd, msg_vec, vec_len, flags, timeout_addr);
}

int_t sys_recvmmsg_amd64(fd_t sock_fd, addr_t msg_vec, uint_t vec_len, int_t flags, addr_t timeout_addr) {
    return sys_recvmmsg_guest_abi(sock_fd, msg_vec, vec_len, flags, timeout_addr, GUEST_ABI_AMD64);
}

int_t sys_recvmmsg_amd64_guest(fd_t sock_fd, guest_addr_t msg_vec, uint_t vec_len, int_t flags, guest_addr_t timeout_addr) {
    return sys_recvmmsg_guest_abi(sock_fd, msg_vec, vec_len, flags, timeout_addr, GUEST_ABI_AMD64);
}

static int_t sys_sendmmsg_guest_abi(fd_t sock_fd, guest_addr_t msg_vec, uint_t vec_len, int_t flags,
        enum guest_abi abi) {
    int num_sent = 0;
    size_t msg_stride = guest_mmsghdr_size(abi);
    size_t msg_len_offset = guest_mmsghdr_len_offset(abi);
    for (unsigned i = 0; i < vec_len; i++) {
        guest_addr_t msghdr = msg_vec + i * msg_stride;
        int_t res = sys_sendmsg_guest_abi(sock_fd, msghdr, flags, abi);
        if (res >= 0) {
            guest_addr_t msg_len_addr = msghdr + msg_len_offset;
            if (user_put(msg_len_addr, res))
                res = _EFAULT;
        }
        if (res < 0) {
            // From the man page:
            // If an error occurs after at least one message has been sent, the
            // call succeeds, and returns the number of messages sent.  The
            // error code is lost.
            if (num_sent > 0)
                break;
            return res;
        }
        num_sent++;
        if (res == 0) {
            // This means the socket is non-blocking and can't be written to anymore.
            break;
        }
    }
    return num_sent;
}

int_t sys_sendmmsg_guest(fd_t sock_fd, guest_addr_t msg_vec, uint_t vec_len, int_t flags) {
    return sys_sendmmsg_guest_abi(sock_fd, msg_vec, vec_len, flags, GUEST_ABI_I386);
}

int_t sys_sendmmsg(fd_t sock_fd, addr_t msg_vec, uint_t vec_len, int_t flags) {
    return sys_sendmmsg_guest(sock_fd, msg_vec, vec_len, flags);
}

int_t sys_sendmmsg_amd64(fd_t sock_fd, addr_t msg_vec, uint_t vec_len, int_t flags) {
    return sys_sendmmsg_guest_abi(sock_fd, msg_vec, vec_len, flags, GUEST_ABI_AMD64);
}

int_t sys_sendmmsg_amd64_guest(fd_t sock_fd, guest_addr_t msg_vec, uint_t vec_len, int_t flags) {
    return sys_sendmmsg_guest_abi(sock_fd, msg_vec, vec_len, flags, GUEST_ABI_AMD64);
}

static void sock_translate_err(struct fd *fd, int *err) {
    // on ios, when the device goes to sleep, all connected sockets are killed.
    // reads/writes return ENOTCONN, which I'm pretty sure is a violation of
    // posix. so instead, detect this and return ECONNRESET.
    if (*err == _ENOTCONN) {
        struct sockaddr addr;
        socklen_t len = sizeof(addr);
        if (getpeername(fd->real_fd, &addr, &len) < 0 && errno == EINVAL) {
            *err = _ECONNRESET;
            // ...and remember it. The host keeps answering ENOTCONN for as
            // long as the fd exists, so this used to re-manufacture the same
            // ECONNRESET on every call while sock_poll went on reporting the
            // fd readable: poll says ready, recv says error, nothing changes.
            // chronyd sat at 106% of one core doing that -- 23748 pselect6 and
            // 47496 failing recvmmsg in 12 seconds of strace, on two fds. A
            // reset connection reports itself ONCE on every other system.
            fd->socket.conn_dead = true;
        }
    }
}

// A socket error that has already been taken off the host, if any.
//
// Linux hands a pending socket error to whichever call asks first and then
// clears it. On Darwin that error lives in SO_ERROR, which is read-and-clear --
// and AOK reads it ITSELF: socket_tcp_connect_write_ready() runs on every
// sock_poll() of a stream socket. So by the time the guest's recv() arrives the
// host has nothing left to report and the read looks like a clean end-of-file.
//
// Measured rather than reasoned: a TCP peer resetting with SO_LINGER 0 makes
// recv() report ECONNRESET on the macOS host and on Linux 6.12 alike, and an
// AOK guest agreed -- until the guest called poll() first, after which the same
// recv() returned 0 bytes. "The peer reset me" and "the peer closed cleanly"
// are different answers, and every guest that polls before reading was getting
// the wrong one.
//
// getsockopt(SO_ERROR) already consulted this stash (see the fd.h comment on
// host_connect_error). The reads have to as well, for the same reason.
static int sock_take_pending_error(struct fd *fd) {
    int err = fd->socket.host_connect_error;
    if (err == 0)
        return 0;
    fd->socket.host_connect_error = 0;
    return err_map(err);
}

static int sock_poll(struct fd *fd) {
    if (sock_is_devlog_sink(fd))
        return POLL_WRITE;
    if (sock_is_initctl_sink(fd))
        return POLL_READ | POLL_WRITE | POLL_HUP;
    if (fd->real_fd < 0) {
        int types = POLL_WRITE;
        // netlink_reply_lock (and the fields it guards) only exists in the
        // "socket" arm of fd->'s union -- other real_fd<0 fd kinds (e.g.
        // Pasteboard) alias the same bytes for unrelated fields, so this
        // must stay gated on the domain check.
        if (fd->socket.domain == AF_NETLINK_) {
            lock(&fd->socket.netlink_reply_lock, 0);
            if (fd->socket.netlink_reply_off < fd->socket.netlink_reply_len)
                types |= POLL_READ;
            unlock(&fd->socket.netlink_reply_lock);
        }
        return types;
    }
    // The connection is gone for good -- iOS killed it while the device slept,
    // and sock_translate_err has already said so once. Report what Linux shows
    // for a dead connection, and specifically NOT POLL_READ: a poll loop that
    // is told "readable" and then handed an error by every recv is exactly the
    // 100%-CPU spin this flag exists to end. A reader still wakes, because
    // select counts POLL_HUP and POLL_ERR as readable (kernel/poll.c's
    // SELECT_READ, matching Linux), and gets the end-of-file the read paths
    // now return.
    if (fd->socket.conn_dead)
        return POLL_ERR | POLL_HUP;
    int types = realfs_poll(fd);
#if defined(__APPLE__)
    if (types & POLL_WRITE)
        sock_trace_tcp_info("poll-write", fd);
    if ((types & POLL_WRITE) && !socket_tcp_connect_write_ready(fd))
        types &= ~POLL_WRITE;
#endif
    if (fd->socket.ipv6_recverr && fd->socket.ipv6_recverr_fd >= 0) {
        struct pollfd err_pfd = {
            .fd = fd->socket.ipv6_recverr_fd,
            .events = POLLIN,
        };
        if (poll(&err_pfd, 1, 0) > 0 && (err_pfd.revents & POLLIN))
            types |= POLLERR;
    }
    return types;
}

static ssize_t sock_read(struct fd *fd, void *buf, size_t size) {
    if (sock_is_initctl_sink(fd))
        return 0;
    if (fd->socket.domain == AF_NETLINK_) {
        // Mirror recvfrom()/recvmsg(): deliver the buffered netlink reply to a
        // bare read()/readv() too. Returns _EAGAIN when no reply is queued, the
        // same as the recvfrom() path.
        struct iovec iov = {
            .iov_base = buf,
            .iov_len = size,
        };
        struct msghdr msg = {
            .msg_iov = &iov,
            .msg_iovlen = 1,
        };
        return netlink_handle_recvmsg(fd, &msg, 0);
    }
    if (fd->real_fd < 0)
        return _EOPNOTSUPP;
    if (fd->socket.domain == AF_LOCAL_) {
        int err = unix_socket_finish_peer(fd);
        if (err < 0)
            return err;
    }
    // AF_LOCAL datagrams arrive with an in-band cred header (see struct
    // unix_dgram_cred_hdr): land it in a scratch struct via a 2-iov recvmsg
    // so the caller's buffer receives only real payload.
    bool unix_dgram = sock_is_unix_dgram(fd);
    struct unix_dgram_cred_hdr dgram_hdr = {};
    struct iovec dgram_iov[2] = {
        {.iov_base = &dgram_hdr, .iov_len = sizeof(dgram_hdr)},
        {.iov_base = buf, .iov_len = size},
    };
    struct msghdr dgram_msg = {.msg_iov = dgram_iov, .msg_iovlen = 2};
    ssize_t res = 0;
    struct socket_io_wait wait = {};
    socket_force_host_nonblock(fd);
    TASK_MAY_BLOCK {
        while (1) {
            errno = 0;
            if (unix_dgram)
                res = recvmsg(fd->real_fd, &dgram_msg, 0);
            else
                res = read(fd->real_fd, buf, size);
            if (res >= 0)
                break;
            if (socket_should_retry_io_eintr(fd, 0))
                continue;
            if ((errno == EAGAIN || errno == EWOULDBLOCK) &&
                    socket_call_is_blocking(fd, 0)) {
                int wait_err = socket_wait_ready(fd, POLLIN, &wait);
                if (wait_err < 0) {
                    res = wait_err;
                    errno = 0;
                    goto out_read;
                }
                continue;
            }
            break;
        }
    }
out_read:
    if (res <= 0) {
        // A pending socket error outranks both EOF and EAGAIN, and AOK's own
        // poll probe may already have taken it off the host. See
        // sock_take_pending_error.
        int pending = sock_take_pending_error(fd);
        if (pending != 0) {
            sock_trace("read", fd, -1, pending);
            return pending;
        }
    }
    if (res < 0) {
        if (res > -4096 && res < 0 && errno == 0)
            return res;
        if (socket_should_map_unix_eperm_to_eagain(fd, 0)) {
            sock_x11_event("read-eagain", fd, -1, _EAGAIN, size);
            return _EAGAIN;
        }
        int err = errno_map();
        bool was_dead = fd->socket.conn_dead;
        sock_translate_err(fd, &err);
        // Already reported; a connection that is gone reads EOF from here on.
        if (was_dead && err == _ECONNRESET) {
            sock_trace("read", fd, 0, 0);
            return 0;
        }
        sock_trace("read", fd, -1, err);
        if (err == _EAGAIN)
            sock_x11_event("read-eagain", fd, -1, err, size);
        else
            sock_x11_event("read-err", fd, -1, err, size);
        return err;
    }
    if (unix_dgram && res >= (ssize_t) sizeof(dgram_hdr) &&
            dgram_hdr.magic == UNIX_DGRAM_CRED_MAGIC) {
        res -= sizeof(dgram_hdr);
        // See the recvfrom path: plain read() discards the parcel.
        if (dgram_hdr.scm_cookie != 0) {
            struct scm *dropped = unix_dgram_scm_take(dgram_hdr.scm_cookie);
            if (dropped != NULL)
                scm_free(dropped);
        }
    }
    sock_trace("read", fd, res, 0);
    if (res == 0)
        sock_x11_event("read-eof", fd, 0, 0, size);
    return res;
}

static ssize_t sock_write(struct fd *fd, const void *buf, size_t size) {
    if (sock_is_devlog_sink(fd) || sock_is_initctl_sink(fd)) {
        return size;
    }
    if (fd->socket.domain == AF_NETLINK_) {
        // Netlink sockets are emulated in-process (real_fd < 0), so route a bare
        // write()/writev() through the same request handler as sendto()/sendmsg().
        // BusyBox's `ip` (and other tools) issue the rtnetlink dump request with
        // write() rather than send(); without this they hit the real_fd < 0 guard
        // below and get EOPNOTSUPP ("ip: write error: Not supported").
        struct iovec iov = {
            .iov_base = (void *) buf,
            .iov_len = size,
        };
        struct msghdr msg = {
            .msg_iov = &iov,
            .msg_iovlen = 1,
        };
        return netlink_handle_sendmsg(fd, &msg);
    }
    if (fd->real_fd < 0)
        return _EOPNOTSUPP;
    sock_trace_write_preview(fd, buf, size);
    // AF_LOCAL datagrams carry the sender's guest creds in-band; see
    // struct unix_dgram_cred_hdr.
    bool unix_dgram = sock_is_unix_dgram(fd);
    struct unix_dgram_cred_hdr dgram_hdr;
    struct iovec dgram_iov[2];
    struct msghdr dgram_msg = {};
    if (unix_dgram) {
        unix_dgram_cred_hdr_fill(&dgram_hdr);
        dgram_iov[0] = (struct iovec) {.iov_base = &dgram_hdr, .iov_len = sizeof(dgram_hdr)};
        dgram_iov[1] = (struct iovec) {.iov_base = (void *) buf, .iov_len = size};
        dgram_msg.msg_iov = dgram_iov;
        dgram_msg.msg_iovlen = 2;
    }
    ssize_t res = 0;
    // Plain write(2) on a socket is how most guests actually send, and a
    // blocking one must not come back short. See the sendto path: the host
    // call is nonblocking now so a fatal signal can reach a parked task, which
    // makes finishing the request our job.
    bool send_all = !unix_dgram && socket_call_is_blocking(fd, 0) && socket_is_stream(fd);
    size_t sent = 0;
    struct socket_io_wait wait = {};
    socket_force_host_nonblock(fd);
    TASK_MAY_BLOCK {
        while (1) {
            errno = 0;
            if (unix_dgram)
                res = sendmsg(fd->real_fd, &dgram_msg, 0);
            else
                res = write(fd->real_fd, (const char *) buf + sent, size - sent);
            if (res >= 0) {
                if (!send_all)
                    break;
                sent += (size_t) res;
                if (res == 0 || sent >= size) {
                    res = (ssize_t) sent;
                    break;
                }
                int wait_err = socket_wait_ready(fd, POLLOUT, &wait);
                if (wait_err < 0) {
                    res = (ssize_t) sent;
                    break;
                }
                continue;
            }
            if (socket_should_retry_io_eintr(fd, 0))
                continue;
            if ((errno == EAGAIN || errno == EWOULDBLOCK) &&
                    socket_call_is_blocking(fd, 0)) {
                int wait_err = socket_wait_ready(fd, POLLOUT, &wait);
                if (wait_err < 0) {
                    res = sent > 0 ? (ssize_t) sent : wait_err;
                    goto out_write;
                }
                continue;
            }
            if (sent > 0)
                res = (ssize_t) sent;
            break;
        }
    }
out_write:
    if (res < 0) {
        if (res > -4096 && res < 0 && errno == 0)
            return res;
        if (socket_should_map_unix_eperm_to_eagain(fd, 0)) {
            sock_trace("write", fd, -1, _EAGAIN);
            sock_x11_event("write-eagain", fd, -1, _EAGAIN, size);
            return _EAGAIN;
        }
        int err = errno_map();
        sock_translate_err(fd, &err);
        sock_trace("write", fd, -1, err);
        if (err == _EAGAIN)
            sock_x11_event("write-eagain", fd, -1, err, size);
        else
            sock_x11_event("write-err", fd, -1, err, size);
        return err;
    }
    if (unix_dgram && res >= (ssize_t) sizeof(dgram_hdr))
        res -= sizeof(dgram_hdr);
    sock_trace("write", fd, res, 0);
    if ((size_t) res != size)
        sock_x11_event("write-short", fd, res, 0, size);
    return res;
}

static int sock_ioctl(struct fd *fd, int cmd, void *arg) {
    if (cmd == SIOCGIFNAME_)
        return sock_ifreq_name_from_index(arg);
    if (cmd == SIOCGIFCONF_)
        return sock_ifconf(arg);
    if (cmd == SIOCGIFINDEX_)
        return sock_ifreq_index_from_name(arg);
    if (cmd == SIOCGIFFLAGS_)
        return sock_ifreq_flags_from_name(arg);
    if (cmd == SIOCGIFTXQLEN_)
        return sock_ifreq_txqlen_from_name(arg);
    if (cmd == SIOCGIFMTU_)
        return sock_ifreq_mtu_from_name(arg);
    if (cmd == SIOCGIFMETRIC_)
        return sock_ifreq_metric_from_name(arg);
    if (cmd == SIOCGIFHWADDR_)
        return sock_ifreq_hwaddr_from_name(arg);
    if (cmd == SIOCGIFADDR_)
        return sock_ifreq_addr_field_from_name(arg, SOCK_IFREQ_ADDR_);
    if (cmd == SIOCGIFDSTADDR_)
        return sock_ifreq_addr_field_from_name(arg, SOCK_IFREQ_DSTADDR_);
    if (cmd == SIOCGIFBRDADDR_)
        return sock_ifreq_addr_field_from_name(arg, SOCK_IFREQ_BRDADDR_);
    if (cmd == SIOCGIFNETMASK_)
        return sock_ifreq_addr_field_from_name(arg, SOCK_IFREQ_NETMASK_);
    if (fd->real_fd < 0) {
        if (cmd == FIONREAD_)
            *(dword_t *) arg = (dword_t) (fd->socket.netlink_reply_len - fd->socket.netlink_reply_off);
        else if (cmd == SIOCOUTQ_)
            // Emulated (netlink) sockets never queue outgoing bytes.
            *(dword_t *) arg = 0;
        else
            return _EINVAL;
        return 0;
    }
    if (cmd == SIOCOUTQ_) {
        // Linux SIOCOUTQ: bytes still queued in the socket's send buffer.
        // dbus-broker calls this from socket_dispatch_write() whenever it has
        // pending output and treats ANY failure as fatal (error_origin ->
        // main-loop exit 1), so returning ENOTTY here killed the system bus
        // the first time a connection's output backed up -- which is exactly
        // what logind's session-scope creation traffic does on login.
#if defined(__APPLE__)
        int queued = 0;
        socklen_t len = sizeof(queued);
        if (getsockopt(fd->real_fd, SOL_SOCKET, SO_NWRITE, &queued, &len) < 0)
            return errno_map();
        *(dword_t *) arg = (dword_t) queued;
#else
        int queued = 0;
        if (ioctl(fd->real_fd, TIOCOUTQ, &queued) < 0)
            return errno_map();
        *(dword_t *) arg = (dword_t) queued;
#endif
        return 0;
    }
    return realfs_ioctl(fd, cmd, arg);
}

static ssize_t sock_ioctl_size(int cmd) {
    switch (cmd) {
        case SIOCOUTQ_:
            return sizeof(dword_t);
        case SIOCGIFNAME_:
        case SIOCGIFCONF_:
        case SIOCGIFINDEX_:
        case SIOCGIFFLAGS_:
        case SIOCGIFTXQLEN_:
        case SIOCGIFMTU_:
        case SIOCGIFMETRIC_:
        case SIOCGIFHWADDR_:
        case SIOCGIFADDR_:
        case SIOCGIFDSTADDR_:
        case SIOCGIFBRDADDR_:
        case SIOCGIFNETMASK_:
            if (cmd != SIOCGIFCONF_)
                return task_is_64bit(current) ? IFREQ_SIZE_64_ : IFREQ_SIZE_32_;
            return task_is_64bit(current) ? sizeof(struct guest_ifconf64_) : sizeof(struct guest_ifconf32_);
        default:
            return realfs_ioctl_size(cmd);
    }
}

static int sock_getflags(struct fd *fd) {
    if (fd->real_fd < 0)
        return fd->flags;
    int flags = realfs_getflags(fd);
    if (flags < 0)
        return flags;
    // The host O_NONBLOCK can deliberately diverge from the guest's:
    // sys_accept4_common keeps listening sockets host-nonblocking forever so
    // a guest accept can never wedge inside the host accept(). fd->flags
    // carries the guest's own O_NONBLOCK for the open file description
    // (maintained by realfs_setflags via f_install/F_SETFL/FIONBIO), so
    // report that one, not the host's.
    return (flags & ~O_NONBLOCK_) | (fd->flags & O_NONBLOCK_);
}

static int sock_setflags(struct fd *fd, dword_t flags) {
    if (fd->real_fd < 0) {
        fd->flags = (fd->flags & ~(O_APPEND_ | O_NONBLOCK_)) | (flags & (O_APPEND_ | O_NONBLOCK_));
        return 0;
    }
    // The single choke point for guest F_SETFL and FIONBIO (kernel/fs.c
    // set_nonblock routes through fd_setflags), and realfs_setflags writes the
    // guest's flags straight onto the host description -- which drops the
    // O_NONBLOCK socket_force_host_nonblock() put there. Drop the cache with
    // it so the next call that can block re-forces.
    fd->socket.host_nonblock = false;
    return realfs_setflags(fd, flags);
}

static int sock_close(struct fd *fd) {
    sockrestart_end_listen(fd);
    if (fd->socket.domain == AF_NETLINK_) {
        netlink_notify_unregister(fd);
        netlink_reply_reset(fd);
    }
    if (fd->socket.domain == AF_INET_ && fd->socket.inet_nat_bound)
        inet_nat_remove_owner(fd);
    release_unix_names(fd);
    // Kill any unconsumed peer tokens BEFORE breaking peer links: a cookie
    // must never resolve to a freed fd, and unix_token_lock (taken first
    // here and by the consuming side across lookup+link) is what makes the
    // consume-vs-close race safe.
    if (fd->socket.domain == AF_LOCAL_)
        unix_token_unregister_sender(fd);
    lock(&peer_lock, 0);
    struct fd *peer = fd->socket.unix_peer;
    if (peer != NULL)
        peer->socket.unix_peer = NULL;
    unlock(&peer_lock);
    if (fd->socket.domain == AF_LOCAL_) {
        lock(&fd->lock, 0);
        struct scm *scm, *tmp;
        list_for_each_entry_safe(&fd->socket.unix_scm, scm, tmp, queue) {
            list_remove(&scm->queue);
            scm_free(scm);
        }
        unlock(&fd->lock);
    }
    if (fd->socket.ipv6_recverr_fd >= 0) {
        close(fd->socket.ipv6_recverr_fd);
        fd->socket.ipv6_recverr_fd = -1;
    }
    if (fd->real_fd < 0)
        return 0;
    return realfs_close(fd);
}

const struct fd_ops socket_fdops = {
    .read = sock_read,
    .write = sock_write,
    .close = sock_close,
    .poll = sock_poll,
    .getflags = sock_getflags,
    .setflags = sock_setflags,
    .ioctl_size = sock_ioctl_size,
    .ioctl = sock_ioctl,
};

#if (defined(__GNUC__) && __GNUC__ >= 8) || defined(__clang__)
#pragma GCC diagnostic ignored "-Wcast-function-type"
#endif
#if defined(__clang__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
static struct socket_call {
    syscall_t func;
    int args;
} socket_calls[] = {
    {NULL},
    {(syscall_t) sys_socket, 3},
    {(syscall_t) sys_bind, 3},
    {(syscall_t) sys_connect, 3},
    {(syscall_t) sys_listen, 2},
    {(syscall_t) sys_accept, 3},
    {(syscall_t) sys_getsockname, 3},
    {(syscall_t) sys_getpeername, 3},
    {(syscall_t) sys_socketpair, 4},
    {(syscall_t) sys_send, 4}, // send
    {(syscall_t) sys_recv, 4}, // recv
    {(syscall_t) sys_sendto, 6},
    {(syscall_t) sys_recvfrom, 6},
    {(syscall_t) sys_shutdown, 2},
    {(syscall_t) sys_setsockopt, 5},
    {(syscall_t) sys_getsockopt, 5},
    {(syscall_t) sys_sendmsg, 3},
    {(syscall_t) sys_recvmsg, 3},
    {(syscall_t) sys_accept4, 4},
    {(syscall_t) sys_recvmmsg, 5},
    {(syscall_t) sys_sendmmsg, 4},
};
// #define SYS_SOCKET    1        /* sys_socket(2)        */
// #define SYS_BIND    2        /* sys_bind(2)            */
// #define SYS_CONNECT    3        /* sys_connect(2)        */
// #define SYS_LISTEN    4        /* sys_listen(2)        */
// #define SYS_ACCEPT    5        /* sys_accept(2)        */
// #define SYS_GETSOCKNAME    6        /* sys_getsockname(2)        */
// #define SYS_GETPEERNAME    7        /* sys_getpeername(2)        */
// #define SYS_SOCKETPAIR    8        /* sys_socketpair(2)        */
// #define SYS_SEND    9        /* sys_send(2)            */
// #define SYS_RECV    10        /* sys_recv(2)            */
// #define SYS_SENDTO    11        /* sys_sendto(2)        */
// #define SYS_RECVFROM    12        /* sys_recvfrom(2)        */
// #define SYS_SHUTDOWN    13        /* sys_shutdown(2)        */
// #define SYS_SETSOCKOPT    14        /* sys_setsockopt(2)        */
// #define SYS_GETSOCKOPT    15        /* sys_getsockopt(2)        */
// #define SYS_SENDMSG    16        /* sys_sendmsg(2)        */
// #define SYS_RECVMSG    17        /* sys_recvmsg(2)        */
// #define SYS_ACCEPT4    18        /* sys_accept4(2)        */
// #define SYS_RECVMMSG    19        /* sys_recvmmsg(2)        */
// #define SYS_SENDMMSG    20        /* sys_sendmmsg(2)        */

int_t sys_socketcall_guest(dword_t call_num, guest_addr_t args_addr) {
    STRACE("%d ", call_num);
    if (call_num < 1 || call_num >= sizeof(socket_calls)/sizeof(socket_calls[0]))
        return _EINVAL;
    struct socket_call call = socket_calls[call_num];
    if (call.func == NULL) {
        FIXME("socketcall %d (%s:%d)", call_num, current->comm, current->pid);
        return _ENOSYS;
    }

    dword_t args[6];
    if (user_read(args_addr, args, sizeof(dword_t) * call.args))
        return _EFAULT;
    int_t result = call.func(args[0], args[1], args[2], args[3], args[4], args[5]);
    return result;
}

int_t sys_socketcall(dword_t call_num, addr_t args_addr) {
    return sys_socketcall_guest(call_num, args_addr);
}
