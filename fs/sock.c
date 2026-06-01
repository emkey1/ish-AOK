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
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include "kernel/calls.h"
#include "kernel/task.h"
#include "fs/fd.h"
#include "fs/inode.h"
#include "fs/net_route.h"
#include "fs/path.h"
#include "fs/poll.h"
#include "fs/real.h"
#include "fs/sock.h"
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
#define NLM_F_ROOT_ 0x100
#define NLM_F_MATCH_ 0x200
#define NLM_F_DUMP_ (NLM_F_ROOT_ | NLM_F_MATCH_)

#define SOCK_DIAG_BY_FAMILY_ 20

#define SIOCGIFNAME_ 0x8910
#define SIOCGIFCONF_ 0x8912
#define SIOCGIFFLAGS_ 0x8913
#define SIOCGIFINDEX_ 0x8933

#define IFNAMSIZ_ 16

#define RTM_NEWLINK_ 16
#define RTM_GETLINK_ 18
#define RTM_NEWADDR_ 20
#define RTM_GETADDR_ 22
#define RTM_NEWROUTE_ 24
#define RTM_GETROUTE_ 26

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

struct ifreq_ {
    char ifr_name[IFNAMSIZ_];
    union {
        struct sockaddr_ addr;
        int16_t flags;
        int32_t ifindex;
        char pad[24];
    } ifr_ifru;
};

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

struct guest_ifreq_addr_ {
    char ifr_name[IFNAMSIZ_];
    struct sockaddr_in_ guest_addr;
};

struct guest_ifconf_ {
    int32_t guest_len;
    addr_t guest_buf;
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
    if (cursor->ifa_data != NULL) {
        const struct if_data *stats = (const struct if_data *) cursor->ifa_data;
        if (stats->ifi_mtu != 0)
            info->mtu = (uint32_t) stats->ifi_mtu;
    }
#if defined(__APPLE__)
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
    if (host_flags & IFF_RUNNING)
        linux_flags |= IFF_RUNNING_LINUX_;
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

static int netlink_append_route_link(struct fd *sock, const struct nlmsghdr_ *req_hdr,
        const char *ifname, unsigned ifflags, const struct netlink_link_info *info) {
    char payload[256] = {};
    size_t payload_len = sizeof(struct ifinfomsg_);
    struct ifinfomsg_ *msg = (struct ifinfomsg_ *) payload;
    msg->ifi_family = AF_UNSPEC;
    msg->ifi_type = (strcmp(ifname, "lo0") == 0 || strcmp(ifname, "lo") == 0)
        ? ARPHRD_LOOPBACK_
        : ARPHRD_ETHER_;
    msg->ifi_index = (int32_t) if_nametoindex(ifname);
    msg->ifi_flags = netlink_linux_if_flags(ifflags);
    msg->ifi_change = 0xffffffffu;

    int err = netlink_append_attr_raw(payload, sizeof(payload), &payload_len,
            IFLA_IFNAME_, ifname, strlen(ifname) + 1);
    if (err < 0)
        return err;
    err = netlink_append_attr_raw(payload, sizeof(payload), &payload_len,
            IFLA_MTU_, &info->mtu, sizeof(info->mtu));
    if (err < 0)
        return err;
    if (info->address_len != 0) {
        err = netlink_append_attr_raw(payload, sizeof(payload), &payload_len,
                IFLA_ADDRESS_, info->address, info->address_len);
        if (err < 0)
            return err;
    }
    if (info->broadcast_len != 0) {
        err = netlink_append_attr_raw(payload, sizeof(payload), &payload_len,
                IFLA_BROADCAST_, info->broadcast, info->broadcast_len);
        if (err < 0)
            return err;
    }
    err = netlink_append_attr_raw(payload, sizeof(payload), &payload_len,
            IFLA_TXQLEN_, &info->txqlen, sizeof(info->txqlen));
    if (err < 0)
        return err;
    err = netlink_append_attr_raw(payload, sizeof(payload), &payload_len,
            IFLA_OPERSTATE_, &info->operstate, sizeof(info->operstate));
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

    uint8_t family = netlink_route_request_family(payload, payload_len);
    int err = 0;
    for (const struct ifaddrs *cursor = addrs; cursor != NULL; cursor = cursor->ifa_next) {
        if (cursor->ifa_name == NULL)
            continue;
        if (family != AF_UNSPEC && family != AF_PACKET_ && family != 0)
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

static int netlink_append_route_addr(struct fd *sock, const struct nlmsghdr_ *req_hdr,
        const struct ifaddrs *ifa) {
    char payload[256] = {};
    size_t payload_len = sizeof(struct ifaddrmsg_);
    struct ifaddrmsg_ *msg = (struct ifaddrmsg_ *) payload;
    msg->ifa_family = netlink_guest_family_from_host(ifa->ifa_addr->sa_family);
    msg->ifa_prefixlen = netlink_prefixlen_from_sockaddr(ifa->ifa_netmask);
    msg->ifa_flags = 0;
    msg->ifa_scope = netlink_addr_scope(ifa);
    msg->ifa_index = if_nametoindex(ifa->ifa_name);

    if (ifa->ifa_addr->sa_family == AF_INET) {
        const struct in_addr *addr = &((const struct sockaddr_in *) ifa->ifa_addr)->sin_addr;
        int err = netlink_append_attr_raw(payload, sizeof(payload), &payload_len,
                IFA_LOCAL_, addr, sizeof(*addr));
        if (err < 0)
            return err;
        err = netlink_append_attr_raw(payload, sizeof(payload), &payload_len,
                IFA_ADDRESS_, addr, sizeof(*addr));
        if (err < 0)
            return err;
        if (ifa->ifa_dstaddr != NULL && (ifa->ifa_flags & IFF_POINTOPOINT)) {
            const struct in_addr *dst = &((const struct sockaddr_in *) ifa->ifa_dstaddr)->sin_addr;
            err = netlink_append_attr_raw(payload, sizeof(payload), &payload_len,
                    IFA_BROADCAST_, dst, sizeof(*dst));
            if (err < 0)
                return err;
        } else if (ifa->ifa_broadaddr != NULL && (ifa->ifa_flags & IFF_BROADCAST)) {
            const struct in_addr *bcast = &((const struct sockaddr_in *) ifa->ifa_broadaddr)->sin_addr;
            err = netlink_append_attr_raw(payload, sizeof(payload), &payload_len,
                    IFA_BROADCAST_, bcast, sizeof(*bcast));
            if (err < 0)
                return err;
        }
    } else if (ifa->ifa_addr->sa_family == AF_INET6) {
        const struct in6_addr *addr6 = &((const struct sockaddr_in6 *) ifa->ifa_addr)->sin6_addr;
        int err = netlink_append_attr_raw(payload, sizeof(payload), &payload_len,
                IFA_LOCAL_, addr6, sizeof(*addr6));
        if (err < 0)
            return err;
        err = netlink_append_attr_raw(payload, sizeof(payload), &payload_len,
                IFA_ADDRESS_, addr6, sizeof(*addr6));
        if (err < 0)
            return err;
    } else {
        return 0;
    }

    int err = netlink_append_attr_raw(payload, sizeof(payload), &payload_len,
            IFA_LABEL_, ifa->ifa_name, strlen(ifa->ifa_name) + 1);
    if (err < 0)
        return err;
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
            return netlink_append_done(sock, hdr->nlmsg_seq);
    }
}

struct diag_socket_entry {
    struct fd **fds;
    unsigned count;
    unsigned cap;
};

static uint32_t netlink_next_port_id(void);

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

static int sock_ifconf(struct guest_ifconf_ *ifconf) {
    if (ifconf->guest_len < 0)
        return _EINVAL;

    struct ifaddrs *addrs = NULL;
    if (getifaddrs(&addrs) != 0)
        return _EIO;

    size_t capacity = (size_t) ifconf->guest_len;
    size_t used = 0;
    size_t total = 0;
    int err = 0;

    for (const struct ifaddrs *cursor = addrs; cursor != NULL; cursor = cursor->ifa_next) {
        if (cursor->ifa_name == NULL || cursor->ifa_addr == NULL)
            continue;
        if (cursor->ifa_addr->sa_family != AF_INET)
            continue;

        struct guest_ifreq_addr_ entry = {};
        strncpy(entry.ifr_name, cursor->ifa_name, sizeof(entry.ifr_name) - 1);
        entry.guest_addr.sin_family = AF_INET_;
        entry.guest_addr.sin_port = 0;
        entry.guest_addr.sin_addr = ((const struct sockaddr_in *) cursor->ifa_addr)->sin_addr.s_addr;

        total += sizeof(entry);
        if (ifconf->guest_buf == 0 || used + sizeof(entry) > capacity)
            continue;
        if (user_write(ifconf->guest_buf + used, &entry, sizeof(entry))) {
            err = _EFAULT;
            break;
        }
        used += sizeof(entry);
    }

    freeifaddrs(addrs);
    if (err < 0)
        return err;

    if ((size_t) ifconf->guest_len >= total)
        ifconf->guest_len = (int32_t) total;
    else
        ifconf->guest_len = (int32_t) used;
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
    bool signal_pending = !!(current->pending & ~current->blocked);
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

static bool socket_blocking_syscall_begin(sigset_t *oldmask) {
    sigset_t sigusr1;
    sigemptyset(&sigusr1);
    sigaddset(&sigusr1, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &sigusr1, oldmask);

    if (sigunwind_start()) {
        pthread_sigmask(SIG_SETMASK, oldmask, NULL);
        errno = EINTR;
        return false;
    }

    if (socket_guest_signal_pending()) {
        sigunwind_end();
        pthread_sigmask(SIG_SETMASK, oldmask, NULL);
        errno = EINTR;
        return false;
    }

    pthread_sigmask(SIG_SETMASK, oldmask, NULL);
    return true;
}

static void socket_blocking_syscall_end(void) {
    sigunwind_end();
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
    if (so_error != 0)
        return true;

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
        if (real_error != 0)
            return err_map(real_error);
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

static int unix_socket_finish_peer(struct fd *sock, bool wait);

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
                protocol != NETLINK_KOBJECT_UEVENT_)
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

    int sock = socket(real_domain, real_type, protocol);
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

static struct fd *sock_getfd(fd_t sock_fd) {
    struct fd *sock = f_get(sock_fd);
    if (sock == NULL || sock->ops != &socket_fdops)
        return NULL;
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

static void netlink_reply_reset(struct fd *sock) {
    free(sock->socket.netlink_reply);
    sock->socket.netlink_reply = NULL;
    sock->socket.netlink_reply_len = 0;
    sock->socket.netlink_reply_off = 0;
}

static int netlink_reply_append(struct fd *sock, const void *data, size_t len) {
    size_t old_len = sock->socket.netlink_reply_len;
    char *new_reply = realloc(sock->socket.netlink_reply, old_len + len);
    if (new_reply == NULL)
        return _ENOMEM;
    memcpy(new_reply + old_len, data, len);
    sock->socket.netlink_reply = new_reply;
    sock->socket.netlink_reply_len = old_len + len;
    return 0;
}

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
    int err = netlink_reply_append(sock, &hdr, sizeof(hdr));
    if (err < 0)
        return err;
    if (payload_len != 0) {
        err = netlink_reply_append(sock, payload, payload_len);
        if (err < 0)
            return err;
    }
    size_t aligned_len = NLMSG_ALIGN(hdr.nlmsg_len);
    size_t pad_len = aligned_len - hdr.nlmsg_len;
    if (pad_len != 0) {
        static const char zeros[NLMSG_ALIGNTO] = {};
        err = netlink_reply_append(sock, zeros, pad_len);
        if (err < 0)
            return err;
    }
    return 0;
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
    return netlink_append_nlmsg(sock, NLMSG_DONE_, 0, seq, &status, sizeof(status));
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

    netlink_reply_reset(sock);
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
    size_t available = sock->socket.netlink_reply_len - sock->socket.netlink_reply_off;
    bool peek = (fake_flags & MSG_PEEK_) != 0;
    bool want_trunc_len = (fake_flags & MSG_TRUNC_) != 0;
    size_t capacity = diag_iov_capacity(msg->msg_iov, msg->msg_iovlen);
    if (sock->socket.netlink_reply_off >= sock->socket.netlink_reply_len)
        return _EAGAIN;
    if (capacity == 0) {
        if (want_trunc_len)
            return (int) available;
        return 0;
    }

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
        if (err < 0)
            return err;
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
        netlink_reply_reset(sock);
    if ((msg->msg_flags & MSG_TRUNC) && want_trunc_len)
        return (int) available;
    return (int) copied;
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

static int unix_socket_finish_peer(struct fd *sock, bool wait) {
    if (sock->socket.domain != AF_LOCAL_)
        return 0;

    if (sock->socket.unix_peer_pending) {
        int recv_flags = 0;
        if (!wait || (fd_getflags(sock) & O_NONBLOCK_))
            recv_flags |= MSG_DONTWAIT;
        else
            recv_flags |= MSG_WAITALL;

        while (sock->socket.unix_peer_off < sizeof(struct fd *)) {
            ssize_t res = 0;
            TASK_MAY_BLOCK {
                do {
                    errno = 0;
                    res = recv(sock->real_fd,
                               sock->socket.unix_peer_buf + sock->socket.unix_peer_off,
                               sizeof(struct fd *) - sock->socket.unix_peer_off,
                               recv_flags);
                } while (res < 0 && errno == EINTR);
            }
            if (res < 0)
                return errno_map();
            if (res == 0)
                return _ECONNRESET;
            sock->socket.unix_peer_off += res;
            if (!wait || (fd_getflags(sock) & O_NONBLOCK_))
                break;
        }
        if (sock->socket.unix_peer_off < sizeof(struct fd *))
            return _EAGAIN;

        struct fd *peer = NULL;
        memcpy(&peer, sock->socket.unix_peer_buf, sizeof(peer));
        lock(&peer_lock, 0);
        if (sock->socket.unix_peer == NULL && peer != NULL) {
            sock->socket.unix_peer = peer;
            peer->socket.unix_peer = sock;
            sock->socket.unix_peer_cred = peer->socket.unix_cred;
            sock->socket.unix_peer_cred_valid = true;
            peer->socket.unix_peer_cred = sock->socket.unix_cred;
            peer->socket.unix_peer_cred_valid = true;
            notify(&peer->socket.unix_got_peer);
        }
        sock->socket.unix_peer_pending = false;
        sock->socket.unix_peer_off = 0;
        unlock(&peer_lock);
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
    uint32_t socket_id;
    struct list links;
};
#define ABSTRACT_HASH_SIZE 1024
static struct list abstract_hash[ABSTRACT_HASH_SIZE];
static lock_t unix_abstract_lock = LOCK_INITIALIZER;

static int unix_abstract_get(const char *name, struct fd *bind_fd, uint32_t *socket_id) {
    uint32_t hash = str_hash(name);
    lock(&unix_abstract_lock, 0);
    struct unix_abstract *sock_tmp;
    struct unix_abstract *sock = NULL;
    struct list *bucket = &abstract_hash[hash % ABSTRACT_HASH_SIZE];
    if (list_null(bucket))
        list_init(bucket);
    list_for_each_entry(bucket, sock_tmp, links) {
        if (sock_tmp->hash == hash) {
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
        sock->refcount = 0;
        sock->hash = hash;
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

static void unix_abstract_release(struct unix_abstract *name) {
    lock(&unix_abstract_lock, 0);
    if (--name->refcount == 0) {
        list_remove(&name->links);
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
            }
            if (err < 0)
                return err;
            if (bind_fd != NULL) {
                bind_fd->socket.unix_name_len = path_size;
                memcpy(bind_fd->socket.unix_name, path, path_size);
            }

            struct sockaddr_un *real_addr_un = sockaddr;
            size_t path_len = snprintf(real_addr_un->sun_path, sizeof(real_addr_un->sun_path), "%s.%u", sock_tmp_prefix, socket_id);
            if (path_len >= sizeof(real_addr_un->sun_path))
                path_len = sizeof(real_addr_un->sun_path) - 1;
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

static int_t sys_bind_common(fd_t sock_fd, guest_addr_t sockaddr_addr, uint_t sockaddr_len) {
    STRACE("bind(%d, 0x%llx, %d)", sock_fd, (unsigned long long) sockaddr_addr, sockaddr_len);
    struct fd *sock = sock_getfd(sock_fd);
    if (sock == NULL)
        return _EBADF;
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

    err = bind(sock->real_fd, (void *) &sockaddr, sockaddr_len);
    if (err < 0) {
        inode_release_if_exist(sock->socket.unix_name_inode);
        if (sock->socket.unix_name_abstract != NULL)
            unix_abstract_release(sock->socket.unix_name_abstract);
        return errno_map();
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

static int_t sys_connect_common(fd_t sock_fd, guest_addr_t sockaddr_addr, uint_t sockaddr_len) {
    STRACE("connect(%d, 0x%llx, %d)", sock_fd, (unsigned long long) sockaddr_addr, sockaddr_len);
    struct fd *sock = sock_getfd(sock_fd);
    if (sock == NULL)
        return _EBADF;
    sock_debug_guest_sockaddr("connect", sock, sockaddr_addr, sockaddr_len);
    if (sock->socket.domain == AF_LOCAL_) {
        sock->socket.unix_devlog_sink = false;
        sock->socket.unix_initctl_sink = false;
        if (guest_sockaddr_is_devlog(sockaddr_addr, sockaddr_len)) {
            sock->socket.unix_devlog_sink = true;
            fill_cred(&sock->socket.unix_cred);
            sock_debug_event("connect-devlog", sock, 0, 0);
            return 0;
        }
        if (guest_sockaddr_is_initctl(sockaddr_addr, sockaddr_len)) {
            sock->socket.unix_initctl_sink = true;
            fill_cred(&sock->socket.unix_cred);
            sock_debug_event("connect-initctl", sock, 0, 0);
            return 0;
        }
    }
    struct sockaddr_max_ sockaddr;
    int err = sockaddr_read(sockaddr_addr, &sockaddr, &sockaddr_len);
    if (err < 0) {
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
    if (forced_nonblocking_connect)
        (void) fcntl(sock->real_fd, F_SETFL, saved_host_flags);
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
        if (getsockopt(sock->real_fd, SOL_SOCKET, SO_ERROR, &real_error, &real_error_len) == 0 &&
                real_error != 0) {
            int mapped_err = err_map(real_error);
            sock_trace("connect", sock, -1, mapped_err);
            return mapped_err;
        }
        // Darwin can report connect() success before TCP_CONNECTION_INFO
        // catches up. Do not turn a successful connect into ECONNRESET here;
        // later poll/send/recv paths already re-check real socket state.
        sock_trace_tcp_info("connect-postcheck", sock);
    }
#endif

    if (sock->socket.domain == AF_LOCAL_) {
        fill_cred(&sock->socket.unix_cred);
        assert(sock->socket.unix_peer == NULL);
        // Send a pointer to ourselves so the accept side can link unix_peer
        // later, but do not wait for that acknowledgement here. Linux connect()
        // completes once the transport connection exists; waiting for accept()
        // to run can wedge clients on daemons that accept asynchronously.
        (void) write(sock->real_fd, &sock, sizeof(struct fd *));
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
    struct fd *sock = sock_getfd(sock_fd);
    if (sock == NULL)
        return _EBADF;
    int err = listen(sock->real_fd, backlog);
    if (err < 0)
        return errno_map();
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
    struct fd *sock = sock_getfd(sock_fd);
    if (sock == NULL)
        return _EBADF;
    dword_t sockaddr_len = 0;
    if (sockaddr_addr != 0) {
        if (user_get(sockaddr_len_addr, sockaddr_len))
            return _EFAULT;
    }

    char sockaddr[sockaddr_len];
    int client =0;
    TASK_MAY_BLOCK {
        do {
            sockrestart_begin_listen_wait(sock);
            errno = 0;
            client = accept(sock->real_fd,
                            sockaddr_addr != 0 ? (void *) sockaddr : NULL,
                            sockaddr_addr != 0 ? &sockaddr_len : NULL);
            sockrestart_end_listen_wait(sock);
        } while (sockrestart_should_restart_listen_wait(0) && errno == EINTR);
    }
    if (client < 0)
        return errno_map();

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
        int peer_err = unix_socket_finish_peer(client_fd, !(fd_getflags(client_fd) & O_NONBLOCK_));
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
    int err = unix_socket_finish_peer(sock, true);
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
    struct fd *sock = sock_getfd(sock_fd);
    if (sock == NULL)
        return _EBADF;
    dword_t sockaddr_len;
    if (user_get(sockaddr_len_addr, sockaddr_len))
        return _EFAULT;
    char sockaddr[sockaddr_len];

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
        copy_unix_name(sockaddr, &sockaddr_len, sock);
        if (user_write(sockaddr_addr, sockaddr, sizeof(sockaddr)))
            return _EFAULT;
        if (user_put(sockaddr_len_addr, sockaddr_len))
            return _EFAULT;
        return 0;
    }

    int res = getsockname(sock->real_fd, (void *) sockaddr, &sockaddr_len);
    if (res < 0)
        return errno_map();

    int err = sockaddr_write(sockaddr_addr, sockaddr, sizeof(sockaddr), &sockaddr_len);
    if (err < 0)
        return err;
    if (user_put(sockaddr_len_addr, sockaddr_len))
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
    struct fd *sock = sock_getfd(sock_fd);
    if (sock == NULL)
        return _EBADF;
    dword_t sockaddr_len;
    if (user_get(sockaddr_len_addr, sockaddr_len))
        return _EFAULT;
    char sockaddr[sockaddr_len];

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
        int err = copy_unix_peer_name(sockaddr, &sockaddr_len, sock);
        if (err < 0)
            return err;
        if (user_write(sockaddr_addr, sockaddr, sizeof(sockaddr)))
            return _EFAULT;
        if (user_put(sockaddr_len_addr, sockaddr_len))
            return _EFAULT;
        return 0;
    }

    int res = getpeername(sock->real_fd, (void *) sockaddr, &sockaddr_len);
    if (res < 0)
        return errno_map();

    int err = sockaddr_write(sockaddr_addr, sockaddr, sizeof(sockaddr), &sockaddr_len);
    if (err < 0)
        return err;
    if (user_put(sockaddr_len_addr, sockaddr_len))
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
    int err = socketpair(real_domain, real_type, protocol, sockets);
#if defined(__APPLE__)
    if (err < 0 && unix_seqpacket_fallback_needed(domain, type, protocol, errno))
        err = socketpair(real_domain, SOCK_STREAM, protocol, sockets);
#endif
    if (err < 0)
        return errno_map();

    lock(&peer_lock, 0);
    int fake_sockets[2];
    err = fake_sockets[0] = sock_fd_create(sockets[0], domain, type, protocol);
    if (fake_sockets[0] < 0) {
        unlock(&peer_lock);
        goto close_sockets;
    }
    err = fake_sockets[1] = sock_fd_create(sockets[1], domain, type, protocol);
    if (fake_sockets[1] < 0) {
        unlock(&peer_lock);
        goto close_fake_0;
    }
    struct fd *sock1 = f_get(fake_sockets[0]);
    struct fd *sock2 = f_get(fake_sockets[1]);
    fill_cred(&sock1->socket.unix_cred);
    fill_cred(&sock2->socket.unix_cred);
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
    struct fd *sock = sock_getfd(sock_fd);
    if (sock == NULL)
        return _EBADF;
    char *buffer = malloc(len + 1);
    if (user_read(buffer_addr, buffer, len))
        return _EFAULT;
    buffer[len] = '\0';
    STRACE("sendto(%d, \"%.100s\", %d, %d, 0x%x, %d)", sock_fd, buffer, len, flags, sockaddr_addr, sockaddr_len);
    int real_flags = sock_flags_to_real(flags);
    int err = _EINVAL;
    if (real_flags < 0)
        goto error;
    if (sock_is_devlog_sink(sock) || sock_is_initctl_sink(sock) ||
            (sock->socket.domain == AF_LOCAL_ &&
             (guest_sockaddr_is_devlog(sockaddr_addr, sockaddr_len) ||
              guest_sockaddr_is_initctl(sockaddr_addr, sockaddr_len)))) {
        free(buffer);
        return len;
    }
    struct sockaddr_max_ sockaddr;
    if (sockaddr_addr) {
        err = sockaddr_read(sockaddr_addr, &sockaddr, &sockaddr_len);
        if (err < 0)
            goto error;
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

    ssize_t res = 0;
    TASK_MAY_BLOCK {
        do {
            errno = 0;
            res = sendto(sock->real_fd, buffer, len, real_flags,
                         sockaddr_addr ? (void *) &sockaddr : NULL, sockaddr_len);
        } while (res < 0 && socket_should_retry_io_eintr(sock, real_flags));
    }
    free(buffer);
    if (res < 0) {
        int mapped_err = errno_map();
        sock_trace("sendto", sock, -1, mapped_err);
        sock_debug_event("sendto", sock, -1, mapped_err);
        return mapped_err;
    }
    sock_trace("sendto", sock, res, 0);
    sock_debug_event("sendto", sock, res, 0);
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
    struct fd *sock = sock_getfd(sock_fd);
    if (sock == NULL)
        return _EBADF;
    int real_flags = sock_flags_to_real(flags);
    if (real_flags < 0)
        return _EINVAL;
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

    char *buffer = malloc(len);
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
    TASK_MAY_BLOCK {
        sigset_t oldmask;
        if (!socket_blocking_syscall_begin(&oldmask)) {
            res = -1;
        } else {
            do {
                errno = 0;
                res = recvfrom(sock->real_fd, buffer, len, real_flags,
                               sockaddr_addr != 0 ? (void *) sockaddr : NULL,
                               sockaddr_len_addr != 0 ? &sockaddr_len : NULL);
            } while (res < 0 && socket_should_retry_io_eintr(sock, real_flags));
            socket_blocking_syscall_end();
        }
    }
    if (res < 0) {
        free(buffer);
        int mapped_err = errno_map();
        sock_trace("recvfrom", sock, -1, mapped_err);
        sock_debug_event("recvfrom", sock, -1, mapped_err);
        return mapped_err;
    }

    if (res > 0 && user_write(buffer_addr, buffer, res)) {
        free(buffer);
        return _EFAULT;
    }
    free(buffer);
    if (sockaddr_addr != 0) {
        int err = sockaddr_write(sockaddr_addr, sockaddr, sizeof(sockaddr), &sockaddr_len);
        if (err < 0)
            return err;
    }
    if (sockaddr_len_addr != 0)
        if (user_put(sockaddr_len_addr, sockaddr_len))
            return _EFAULT;
    sock_trace("recvfrom", sock, res, 0);
    sock_debug_event("recvfrom", sock, res, 0);
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
    struct fd *sock = sock_getfd(sock_fd);
    if (sock == NULL)
        return _EBADF;
    int err = shutdown(sock->real_fd, how);
    if (err < 0)
        return errno_map();
    return 0;
}

static void sock_init_emulation_defaults(struct fd *fd) {
    strcpy(fd->socket.tcp_congestion, DEFAULT_TCP_CONGESTION);
    fd->socket.ipv6_recverr_fd = -1;
}

static int_t sys_setsockopt_guest_abi(fd_t sock_fd, dword_t level, dword_t option,
        guest_addr_t value_addr, dword_t value_len, enum guest_abi abi) {
    STRACE("setsockopt(%d, %d, %d, %#llx, %d)", sock_fd, level, option,
            (unsigned long long) value_addr, value_len);
    struct fd *sock = sock_getfd(sock_fd);
    if (sock == NULL)
        return _EBADF;
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
            if (ipv6_recverr_fd_get(sock) < 0)
                return errno_map();
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
        return _ENOPROTOOPT;
    }
    if (sock->socket.domain == AF_NETLINK_ && sock->real_fd < 0) {
        if (level == SOL_SOCKET_ &&
                (option == SO_RCVBUF_ || option == SO_SNDBUF_ ||
                 option == SO_SNDBUFFORCE_ || option == SO_RCVTIMEO_OLD_ ||
                 option == SO_SNDTIMEO_OLD_ || option == SO_RCVTIMEO_ ||
                 option == SO_SNDTIMEO_ || option == SO_ATTACH_FILTER_ ||
                 option == SO_DETACH_FILTER_)) {
            return 0;
        }
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
        if (option == SO_SNDBUFFORCE_) {
            option = SO_SNDBUF_;
        } else if (option == SO_RCVBUFFORCE_) {
            option = SO_RCVBUF_;
        } else if (sockopt_is_linux_soft_unsupported(level, option)) {
            return _ENOPROTOOPT;
        }
    }

    int real_opt = sock_opt_to_real(option, level);
    if (real_opt < 0)
        return sockopt_is_linux_soft_unsupported(level, option) ? _ENOPROTOOPT : _EINVAL;
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
    *result_len = src_len;
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
    struct fd *sock = sock_getfd(sock_fd);
    if (sock == NULL)
        return _EBADF;
    dword_t user_value_len;
    if (user_get(len_addr, user_value_len))
        return _EFAULT;
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
        int err = unix_socket_finish_peer(sock, true);
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
        dword_t acceptconn = 0;
        if (!(sock->socket.domain == AF_NETLINK_ && sock->real_fd < 0)) {
            int real_acceptconn = 0;
            socklen_t real_acceptconn_len = sizeof(real_acceptconn);
            int err = getsockopt(sock->real_fd, SOL_SOCKET, SO_ACCEPTCONN,
                    &real_acceptconn, &real_acceptconn_len);
            if (err < 0)
                return errno_map();
            acceptconn = real_acceptconn != 0;
        }
        sockopt_store_value(value, user_value_len, &value_len, &acceptconn, sizeof(acceptconn));
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
            return sockopt_is_linux_soft_unsupported(level, option) ? _ENOPROTOOPT : _EINVAL;
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
    size_t cmsg_space = guest_cmsg_space(abi, len - hdr_size);
    if (*offset + cmsg_space > capacity)
        return false;

    cmsg->len = len;
    *data = buffer + *offset + hdr_size;
    *data_len = len - hdr_size;
    *offset += cmsg_space;
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
    struct fd *sock = sock_getfd(sock_fd);
    if (sock == NULL)
        return _EBADF;

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

    if (sock_is_devlog_sink(sock) || sock_is_initctl_sink(sock) ||
            (sock->socket.domain == AF_LOCAL_ &&
             (guest_sockaddr_is_devlog(msg_fake.msg_name, msg_fake.msg_namelen) ||
              guest_sockaddr_is_initctl(msg_fake.msg_name, msg_fake.msg_namelen)))) {
        size_t total = 0;
        for (size_t i = 0; i < (size_t) msg.msg_iovlen; i++)
            total += msg_iov[i].iov_len;
        err = (int_t) total;
        goto out_free_iov;
    }

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
    char real_msg_control[CMSG_SPACE(sizeof(int))]; // only used if actually sending an fd
    if (sock->socket.domain == AF_LOCAL_ && msg_control != NULL &&
            msg_fake.msg_controllen >= guest_cmsg_hdr_size(abi)) {
        err = unix_socket_finish_peer(sock, !(real_flags & MSG_DONTWAIT));
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

    ssize_t send_res = 0;
    TASK_MAY_BLOCK {
        do {
            errno = 0;
            send_res = sendmsg(sock->real_fd, &msg, real_flags);
        } while (send_res < 0 && socket_should_retry_io_eintr(sock, real_flags));
    }
    if (send_res < 0) {
        err = errno_map();
        sock_trace("sendmsg", sock, -1, err);
        sock_debug_event("sendmsg", sock, -1, err);
        if (scm != NULL)
            printk("INFO: scm-send pid=%d real sendmsg FAILED: errno=%d err=%d sock_real=%d\n",
                   current ? current->pid : -1, errno, err, sock->real_fd);
        goto out_free_scm;
    }
    err = send_res;
    sock_trace("sendmsg", sock, err, 0);
    sock_debug_event("sendmsg", sock, err, 0);
    if (scm != NULL)
        printk("INFO: scm-send pid=%d real sendmsg OK: sent=%d sock_real=%d ctrl_len=%zu\n",
               current ? current->pid : -1, err, sock->real_fd, msg.msg_controllen);
#if defined(__APPLE__)
    sock_trace_tcp_info("sendmsg-after", sock);
#endif
    goto out_free_iov;

out_free_scm:
    if (scm != NULL) {
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
    struct fd *sock = sock_getfd(sock_fd);
    if (sock == NULL)
        return _EBADF;

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
    if (msg_fake.msg_controllen != 0) {
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
        int peer_err = unix_socket_finish_peer(sock, !(real_flags & MSG_DONTWAIT));
        if (peer_err < 0) {
            free_msghdr_iov(msg_iov, msg.msg_iovlen);
            free(msg_iov_fake);
            free(real_msg_control);
            if (msg_name != msg_name_stack)
                free(msg_name);
            return peer_err;
        }
    }

    ssize_t res = 0;
    TASK_MAY_BLOCK {
        sigset_t oldmask;
        if (!socket_blocking_syscall_begin(&oldmask)) {
            res = -1;
        } else {
            bool use_ipv6_errqueue =
                (flags & MSG_ERRQUEUE_) &&
                sock->socket.domain == AF_INET6_ &&
                sock->socket.type == SOCK_DGRAM_ &&
                sock->socket.ipv6_recverr;
            do {
                errno = 0;
                if (use_ipv6_errqueue)
                    res = recvmsg_ipv6_errqueue(sock, &msg, real_flags);
                else
                    res = recvmsg(sock->real_fd, &msg, real_flags);
            } while (res < 0 && socket_should_retry_io_eintr(sock, real_flags));
            socket_blocking_syscall_end();
        }
    }
    err = 0;
    if (res < 0) {
        err = errno_map();
        sock_trace("recvmsg", sock, -1, err);
        sock_debug_event("recvmsg", sock, -1, err);
    } else {
        sock_trace("recvmsg", sock, res, 0);
        sock_debug_event("recvmsg", sock, res, 0);
        if (sock_trace_enabled()) {
            printk("INFO: net recvmsg-flags pid=%d comm=%s real=%d flags=%#x namelen=%u controllen=%zu\n",
                   current->pid, current->comm, sock->real_fd, msg.msg_flags,
                   (unsigned) msg.msg_namelen, msg.msg_controllen);
        }
    }
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
    if (sock->socket.domain == AF_LOCAL_ && msg.msg_control != NULL)
        printk("INFO: scm-recv pid=%d sock_real=%d res=%zd real_ctrl_after=%zu have_rights=%d unix_peer=%p scm_empty=%d\n",
               current ? current->pid : -1, sock->real_fd, res, msg.msg_controllen,
               (int) have_rights, (void *) sock->socket.unix_peer,
               (int) list_empty(&sock->socket.unix_scm));
    bool want_passcred = sock->socket.domain == AF_LOCAL_ &&
        sock->socket.unix_passcred && res >= 0;
    struct scm *scm = NULL;
    if (have_rights) {
        int dummy_fd = ((int *) CMSG_DATA(rights_cmsg))[0];
        close(dummy_fd);

        lock(&sock->lock, 0);
        assert(!list_empty(&sock->socket.unix_scm));
        scm = list_first_entry(&sock->socket.unix_scm, struct scm, queue);
        list_remove(&scm->queue);
        unlock(&sock->lock);

        if (res < 0) {
            scm_free(scm);
            free(real_msg_control);
            if (msg_name != msg_name_stack)
                free(msg_name);
            return err;
        }
    }

    if (res >= 0 && msg_fake.msg_control != 0 && (cmsg != NULL || want_passcred)) {
        size_t guest_msg_control_len = 0;
        size_t required_msg_control = 0;
        struct ucred_ cred = {};
        bool have_passcred = want_passcred && unix_socket_get_peer_cred(sock, &cred);

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
        if (have_passcred)
            required_msg_control += guest_cmsg_space(abi, sizeof(cred));

        if (required_msg_control > guest_controllen_max) {
            msg_fake.msg_flags |= MSG_CTRUNC_;
        } else if (required_msg_control != 0) {
            uint8_t *guest_msg_control = calloc(1, required_msg_control);
            if (guest_msg_control == NULL) {
                if (scm != NULL)
                    scm_free(scm);
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
            if (have_passcred) {
                bool appended = guest_cmsg_append(abi, guest_msg_control, required_msg_control, &guest_msg_control_len,
                        SOL_SOCKET_, SCM_CREDENTIALS_, &cred, sizeof(cred));
                assert(appended);
            }
            if (user_write(msg_fake.msg_control, guest_msg_control, guest_msg_control_len)) {
                if (scm != NULL)
                    scm_free(scm);
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

    // by now the iovecs and scm have been freed so we can return
    if (res < 0) {
        free(real_msg_control);
        if (msg_name != msg_name_stack)
            free(msg_name);
        return err;
    }

    // msg_name (changed)
    if (msg.msg_name != 0) {
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
        }
    }
}

static int sock_poll(struct fd *fd) {
    if (sock_is_devlog_sink(fd))
        return POLL_WRITE;
    if (sock_is_initctl_sink(fd))
        return POLL_READ | POLL_WRITE | POLL_HUP;
    if (fd->real_fd < 0) {
        int types = POLL_WRITE;
        if (fd->socket.netlink_reply_off < fd->socket.netlink_reply_len)
            types |= POLL_READ;
        return types;
    }
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
    if (fd->real_fd < 0)
        return _EOPNOTSUPP;
    if (fd->socket.domain == AF_LOCAL_) {
        int err = unix_socket_finish_peer(fd, !(fd->flags & O_NONBLOCK_));
        if (err < 0)
            return err;
    }
    ssize_t res = 0;
    TASK_MAY_BLOCK {
        do {
            errno = 0;
            res = read(fd->real_fd, buf, size);
        } while (res < 0 && socket_should_retry_io_eintr(fd, 0));
    }
    if (res < 0) {
        int err = errno_map();
        sock_translate_err(fd, &err);
        sock_trace("read", fd, -1, err);
        return err;
    }
    sock_trace("read", fd, res, 0);
    return res;
}

static ssize_t sock_write(struct fd *fd, const void *buf, size_t size) {
    if (sock_is_devlog_sink(fd) || sock_is_initctl_sink(fd)) {
        return size;
    }
    if (fd->real_fd < 0)
        return _EOPNOTSUPP;
    sock_trace_write_preview(fd, buf, size);
    ssize_t res = 0;
    TASK_MAY_BLOCK {
        do {
            errno = 0;
            res = write(fd->real_fd, buf, size);
        } while (res < 0 && socket_should_retry_io_eintr(fd, 0));
    }
    if (res < 0) {
        int err = errno_map();
        sock_translate_err(fd, &err);
        sock_trace("write", fd, -1, err);
        return err;
    }
    sock_trace("write", fd, res, 0);
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
    if (fd->real_fd < 0) {
        if (cmd == FIONREAD_)
            *(dword_t *) arg = (dword_t) (fd->socket.netlink_reply_len - fd->socket.netlink_reply_off);
        else
            return _EINVAL;
        return 0;
    }
    return realfs_ioctl(fd, cmd, arg);
}

static ssize_t sock_ioctl_size(int cmd) {
    switch (cmd) {
        case SIOCGIFNAME_:
        case SIOCGIFCONF_:
        case SIOCGIFINDEX_:
        case SIOCGIFFLAGS_:
            return cmd == SIOCGIFCONF_ ? sizeof(struct guest_ifconf_) : sizeof(struct ifreq_);
        default:
            return realfs_ioctl_size(cmd);
    }
}

static int sock_getflags(struct fd *fd) {
    if (fd->real_fd < 0)
        return fd->flags;
    return realfs_getflags(fd);
}

static int sock_setflags(struct fd *fd, dword_t flags) {
    if (fd->real_fd < 0) {
        fd->flags = (fd->flags & ~(O_APPEND_ | O_NONBLOCK_)) | (flags & (O_APPEND_ | O_NONBLOCK_));
        return 0;
    }
    return realfs_setflags(fd, flags);
}

static int sock_close(struct fd *fd) {
    sockrestart_end_listen(fd);
    if (fd->socket.domain == AF_NETLINK_)
        netlink_reply_reset(fd);
    // FIXME next 3 lines should go in a function like release_unix_names
    inode_release_if_exist(fd->socket.unix_name_inode);
    if (fd->socket.unix_name_abstract != NULL)
        unix_abstract_release(fd->socket.unix_name_abstract);
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

#if defined(__GNUC__) && __GNUC__ >= 8
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
