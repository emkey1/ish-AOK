#include <sys/stat.h>
#include <arpa/inet.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif
#include <sys/ioctl.h>
#include <inttypes.h>
#include <string.h>
#include "kernel/calls.h"
#include "kernel/task.h"
#include "fs/proc.h"
#include "fs/fd.h"
#include "fs/net_route.h"
#include "fs/sock.h"
#include "platform/platform.h"
#include "fs/proc/net.h"

#import <ifaddrs.h>
#import <netinet/in.h>
#import <netinet/tcp.h>
#import <sys/socket.h>
#import <unistd.h>
#if defined(__APPLE__)
#import <net/if_var.h>
#import <net/if_dl.h>
#else
#include <netpacket/packet.h>
#endif
#import <net/if.h>

#if !defined(__APPLE__)
/* iSH's /proc/net code was written against the BSD struct if_data + AF_LINK that
   macOS getifaddrs() returns. On Linux, getifaddrs() hands back a
   struct rtnl_link_stats on AF_PACKET entries; map the field names so the same
   code reads real Linux interface stats. */
#include <linux/if_link.h>
#define AF_LINK AF_PACKET
#define if_data rtnl_link_stats
#define ifi_ibytes rx_bytes
#define ifi_ipackets rx_packets
#define ifi_ierrors rx_errors
#define ifi_iqdrops rx_dropped
#define ifi_imcasts multicast
#define ifi_obytes tx_bytes
#define ifi_opackets tx_packets
#define ifi_oerrors tx_errors
#define ifi_collisions collisions
#endif

// Every interface the host has, with the counters both /proc/net/dev and
// /sys/class/net report. See the comment on struct net_iface_stats for why they
// share this rather than each calling getifaddrs().
// More interfaces than any host plausibly has; a machine with more simply
// reports the first 64 rather than growing an unbounded stack buffer.
#define PROC_NET_DEV_MAX_IFACES 64

int net_iface_snapshot(struct net_iface_stats *out, int max) {
    struct ifaddrs *addrs;
    if (getifaddrs(&addrs) != 0)
        return 0;
    // Darwin hands the MTU over in if_data; Linux's rtnl_link_stats has no such
    // field, and reporting 0 for /sys/class/net/<iface>/mtu is a wrong answer
    // rather than a missing one (lo is 65536 there). Ask for it, once, over one
    // socket reused for every interface.
    int mtu_sock = out != NULL ? socket(AF_INET, SOCK_DGRAM, 0) : -1;
    int count = 0;
    for (const struct ifaddrs *cursor = addrs; cursor != NULL; cursor = cursor->ifa_next) {
        if (cursor->ifa_addr == NULL || cursor->ifa_addr->sa_family != AF_LINK)
            continue;
        int slot = count++;
        if (out == NULL || slot >= max)
            continue;
        struct net_iface_stats *iface = &out[slot];
        memset(iface, 0, sizeof(*iface));
        snprintf(iface->name, sizeof(iface->name), "%s",
                 cursor->ifa_name != NULL ? cursor->ifa_name : "");
        iface->flags = cursor->ifa_flags;

        const struct if_data *stats = (const struct if_data *) cursor->ifa_data;
        if (stats != NULL) {
            iface->has_stats = true;
            iface->rx_bytes   = stats->ifi_ibytes;
            iface->rx_packets = stats->ifi_ipackets;
            iface->rx_errors  = stats->ifi_ierrors;
            iface->rx_dropped = stats->ifi_iqdrops;
            iface->multicast  = stats->ifi_imcasts;
            iface->tx_bytes   = stats->ifi_obytes;
            iface->tx_packets = stats->ifi_opackets;
            iface->tx_errors  = stats->ifi_oerrors;
            iface->collisions = stats->ifi_collisions;
            // tx_dropped has no counterpart in either shape; Linux's own
            // /proc/net/dev prints it, so report the zero rather than omit it.
#if defined(__APPLE__)
            iface->mtu = stats->ifi_mtu;
#endif
        }
        if (iface->mtu == 0 && mtu_sock >= 0) {
            struct ifreq req;
            memset(&req, 0, sizeof(req));
            snprintf(req.ifr_name, sizeof(req.ifr_name), "%s", iface->name);
            if (ioctl(mtu_sock, SIOCGIFMTU, &req) == 0)
                iface->mtu = (unsigned) req.ifr_mtu;
        }

        // The hardware address, for /sys/class/net/<iface>/address. The link
        // sockaddr differs between the two platforms; everything above this
        // point was already shimmed by name, but this one is a struct shape.
#if defined(__APPLE__)
        const struct sockaddr_dl *dl = (const struct sockaddr_dl *) cursor->ifa_addr;
        if (dl->sdl_alen == sizeof(iface->mac)) {
            memcpy(iface->mac, LLADDR(dl), sizeof(iface->mac));
            iface->has_mac = true;
        }
#else
        const struct sockaddr_ll *ll = (const struct sockaddr_ll *) cursor->ifa_addr;
        if (ll->sll_halen == sizeof(iface->mac)) {
            memcpy(iface->mac, ll->sll_addr, sizeof(iface->mac));
            iface->has_mac = true;
        }
#endif
    }
    if (mtu_sock >= 0)
        close(mtu_sock);
    freeifaddrs(addrs);
    return count;
}

// Partially cribbed from https://github.com/ish-app/ish/pull/315/commits/4a3d96b4ed81470216534d299b921ba3c09ba03f#diff-8c3246e6b14ecb993cb4bf40b3d502a201566f225e339aa09cff57871f0d6351

#pragma mark - /proc/net

/*
 00000000000000000000000000000001 01 80 10 80       lo
 */
static int proc_show_if_inet6(struct proc_entry * UNUSED(entry), struct proc_data *buf) {
    struct ifaddrs *addrs;
    bool success = (getifaddrs(&addrs) == 0);
    if (success) {
        for (const struct ifaddrs *cursor = addrs; cursor != NULL; cursor = cursor->ifa_next) {
            if (cursor->ifa_name == NULL || cursor->ifa_addr == NULL)
                continue;
            if (cursor->ifa_addr->sa_family != AF_INET6)
                continue;
            const struct in6_addr *addr6 = &((const struct sockaddr_in6 *) cursor->ifa_addr)->sin6_addr;

            uint8_t prefix_len = 128;
            if (cursor->ifa_netmask != NULL && cursor->ifa_netmask->sa_family == AF_INET6) {
                const uint8_t *mask_bytes = (const uint8_t *) &((const struct sockaddr_in6 *) cursor->ifa_netmask)->sin6_addr;
                prefix_len = 0;
                for (int i = 0; i < 16; i++)
                    prefix_len += (uint8_t) __builtin_popcount(mask_bytes[i]);
            }

            // Scope classification matches the byte ranges iSH already uses
            // for IPv4/IPv6 route scope elsewhere (see netlink_addr_is_link_local
            // in fs/sock.c): fe80::/10 link-local, fec0::/10 (deprecated)
            // site-local, ::1 host-local, everything else global. These are the
            // nibble-shifted values real /proc/net/if_inet6 emits (not the raw
            // IPV6_ADDR_SCOPE_* route constants), matching the sample above.
            uint8_t scope;
            static const uint8_t loopback_bytes[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
            if (memcmp(addr6->s6_addr, loopback_bytes, 16) == 0)
                scope = 0x10;
            else if (addr6->s6_addr[0] == 0xfe && (addr6->s6_addr[1] & 0xc0) == 0x80)
                scope = 0x20;
            else if (addr6->s6_addr[0] == 0xfe && (addr6->s6_addr[1] & 0xc0) == 0xc0)
                scope = 0x40;
            else
                scope = 0x00;

            unsigned ifindex = if_nametoindex(cursor->ifa_name);

            char addr_hex[33];
            for (int i = 0; i < 16; i++)
                snprintf(addr_hex + i * 2, 3, "%02x", addr6->s6_addr[i]);

            proc_printf(buf, "%s %02x %02x %02x 80       %s\n",
                    addr_hex, ifindex, prefix_len, scope, cursor->ifa_name);
        }
        freeifaddrs(addrs);
    }

    return 0;
}

static int proc_show_arp(struct proc_entry * UNUSED(entry), struct proc_data *buf) {
    /*
     IP address       HW type     Flags       HW address            Mask     Device
     10.211.55.1      0x1         0x2         00:1c:42:00:00:18     *        eth0
     */
    proc_printf(buf, "IP address       HW type     Flags       HW address            Mask     Device\n");
    proc_printf(buf, "192.168.1.1      0x1         0x2         00:BE:EF:CA:FE:00     *        en0\n");
    return 0;
}

static int proc_show_raw(struct proc_entry *UNUSED(entry), struct proc_data *UNUSED(buf)) {
    return 0;
}

static int proc_show_raw6(struct proc_entry *UNUSED(entry), struct proc_data *UNUSED(buf)) {
    return 0;
}

// The socket tables below all list the same thing -- every socket some
// process has open -- so they share one walk with sock_diag (ss), and take
// each socket's inode number and owner from the same two accessors. See
// sock_snapshot_collect, sock_inode and sock_uid in fs/sock.c.

// An address word as Linux prints one: a __be32 exactly as it sits in memory,
// read as a machine word and printed %08X. Every architecture a guest can be
// is little-endian, so 127.0.0.1 -- bytes 7f 00 00 01 -- reads as 0x0100007F,
// and "0100007F" is the spelling every reader decodes: lsof and netstat put
// the parsed word straight back into an in_addr. This printed the address in
// HOST order (ntohl) instead, 7F000001, which each of them then read back as
// 1.0.0.127. IPv6 is four such words (s6_addr32[0..3]), so ::1 prints as
// 00000000000000000000000001000000. Built from the bytes, so the host's own
// byte order cannot leak in either.
static uint32_t proc_net_addr_word(const uint8_t *bytes) {
    return (uint32_t) bytes[0] | (uint32_t) bytes[1] << 8 |
        (uint32_t) bytes[2] << 16 | (uint32_t) bytes[3] << 24;
}

// Fills words[0..3] and *port from a host sockaddr of the family asked for;
// false for any other. IPv4 uses words[0] alone.
static bool proc_net_addr(const struct sockaddr_storage *addr, bool v6,
        uint32_t words[4], uint16_t *port) {
    if (v6) {
        if (addr->ss_family != AF_INET6)
            return false;
        const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *) addr;
        for (int i = 0; i < 4; i++)
            words[i] = proc_net_addr_word(&in6->sin6_addr.s6_addr[i * 4]);
        *port = ntohs(in6->sin6_port);
    } else {
        if (addr->ss_family != AF_INET)
            return false;
        const struct sockaddr_in *in = (const struct sockaddr_in *) addr;
        words[0] = proc_net_addr_word((const uint8_t *) &in->sin_addr.s_addr);
        *port = ntohs(in->sin_port);
    }
    return true;
}

// Linux pads each line of the two IPv4 files, header included, with spaces to
// a fixed width before the newline (seq_setwidth + seq_pad): 149 columns for
// tcp (TMPSZ - 1 in net/ipv4/tcp_ipv4.c), 127 for udp. The IPv6 files are not
// padded. Measured on 6.12: every line of /proc/net/tcp is 150 bytes.
#define PROC_NET_TCP4_WIDTH 149
#define PROC_NET_UDP4_WIDTH 127

static int proc_show_inet_sockets(struct proc_data *buf, int domain, int type) {
    struct sock_snapshot sockets = {};
    int err = sock_snapshot_collect(&sockets, domain, type);
    if (err < 0) {
        sock_snapshot_release(&sockets);
        return err;
    }
    bool v6 = domain == AF_INET6_;
    bool tcp = type == SOCK_STREAM_;
    int width = v6 ? 0 : tcp ? PROC_NET_TCP4_WIDTH : PROC_NET_UDP4_WIDTH;

    // The header is how a reader decides the FORMAT, so the v6 files need the
    // v6 one -- `remote_address' rather than `rem_address', and address columns
    // wide enough for 128 bits. Emitting the v4 header for all four files made
    // lsof refuse both v6 tables outright:
    //
    //     lsof: WARNING: unsupported format: /proc/net/tcp6
    //     lsof: WARNING: unsupported format: /proc/net/udp6
    //
    // Taken verbatim from the kernel rather than from memory: tcp4_seq_show,
    // udp4_seq_show (whose `sl' is indented one more, for its %5d bucket
    // column), tcp6_seq_show, and IPV6_SEQ_DGRAM_HEADER in
    // include/net/transp_v6.h. The datagram headers add the three trailing
    // columns their rows carry.
    const char *header;
    if (!v6)
        header = tcp
            ? "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt"
              "   uid  timeout inode"
            : "   sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt"
              "   uid  timeout inode ref pointer drops";
    else
        header = tcp
            ? "  sl  local_address                         remote_address                        "
              "st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode"
            : "  sl  local_address                         remote_address                        "
              "st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode ref pointer drops";
    proc_printf(buf, "%-*s\n", width, header);

    unsigned row = 0;
    for (unsigned i = 0; i < sockets.count; i++) {
        struct fd *fd = sockets.fds[i];
        struct sockaddr_storage local = {};
        struct sockaddr_storage peer = {};
        socklen_t local_len = sizeof(local);
        socklen_t peer_len = sizeof(peer);
        if (getsockname(fd->real_fd, (struct sockaddr *) &local, &local_len) < 0)
            continue;
        uint32_t local_addr[4] = {};
        uint32_t peer_addr[4] = {};
        uint16_t local_port = 0;
        uint16_t peer_port = 0;
        if (!proc_net_addr(&local, v6, local_addr, &local_port))
            continue;
        bool has_peer = getpeername(fd->real_fd, (struct sockaddr *) &peer, &peer_len) == 0 &&
            proc_net_addr(&peer, v6, peer_addr, &peer_port);
        int state = tcp ? sock_tcp_state(fd) : (has_peer ? 0x01 : 0x07);

        if (!sock_inet_is_listed(type, state, (const struct sockaddr *) &local))
            continue;

        char line[256];
        int n;
        char addrs[160];
        if (v6)
            snprintf(addrs, sizeof(addrs), "%08X%08X%08X%08X:%04X %08X%08X%08X%08X:%04X",
                    local_addr[0], local_addr[1], local_addr[2], local_addr[3], local_port,
                    peer_addr[0], peer_addr[1], peer_addr[2], peer_addr[3], peer_port);
        else
            snprintf(addrs, sizeof(addrs), "%08X:%04X %08X:%04X",
                    local_addr[0], local_port, peer_addr[0], peer_port);
        // What follows the inode differs: TCP's refcount, socket pointer, rto,
        // ato, quick/pingpong, cwnd and ssthresh against UDP's refcount,
        // pointer and drop count. The pointer is %pK, which prints as zeros to
        // a reader without CAP_SYSLOG.
        if (tcp)
            n = snprintf(line, sizeof(line), "%4u: %s %02X %08X:%08X %02X:%08lX %08X %5u %8d %lu "
                    "%d %016x %lu %lu %u %u %d",
                    row, addrs, state, 0, sock_recv_queue(fd), 0, 0ul, 0, (unsigned) sock_uid(fd),
                    0, sock_inode(fd), 1, 0, 100ul, 0ul, 0, 10, state == 0x0A ? 0 : -1);
        else
            n = snprintf(line, sizeof(line), "%5u: %s %02X %08X:%08X %02X:%08lX %08X %5u %8d %lu "
                    "%d %016x %u",
                    row, addrs, state, 0, sock_recv_queue(fd), 0, 0ul, 0, (unsigned) sock_uid(fd),
                    0, sock_inode(fd), 2, 0, 0);
        if (n < 0 || (size_t) n >= sizeof(line))
            continue;
        proc_printf(buf, "%-*s\n", width, line);
        row++;
    }
    sock_snapshot_release(&sockets);
    return 0;
}

static int proc_show_tcp(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    return proc_show_inet_sockets(buf, AF_INET_, SOCK_STREAM_);
}

static int proc_show_tcp6(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    return proc_show_inet_sockets(buf, AF_INET6_, SOCK_STREAM_);
}

static int proc_show_udp(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    return proc_show_inet_sockets(buf, AF_INET_, SOCK_DGRAM_);
}

static int proc_show_udp6(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    return proc_show_inet_sockets(buf, AF_INET6_, SOCK_DGRAM_);
}

static int proc_show_unix(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    struct sock_snapshot sockets = {};
    int err = sock_snapshot_collect(&sockets, AF_LOCAL_, -1);
    if (err < 0) {
        sock_snapshot_release(&sockets);
        return err;
    }

    proc_printf(buf, "Num       RefCount Protocol Flags    Type St Inode Path\n");
    for (unsigned i = 0; i < sockets.count; i++) {
        struct fd *fd = sockets.fds[i];
        unsigned type = fd->socket.type & 0xf;
        unsigned st = 1;
        unsigned flags = 0;
        // __SO_ACCEPTCON for a listener, from listen()'s own record: asking
        // the host for SO_ACCEPTCONN fails on Darwin, which has none, so no
        // listener was ever flagged and lsof called each one UNCONNECTED.
        if (fd->socket.listening) {
            flags |= 0x00010000;
            st = 1;
        } else if (fd->socket.unix_peer != NULL) {
            st = 3;
        }

        char path[sizeof(fd->socket.unix_name) * 2 + 2];
        path[0] = '\0';
        if (fd->socket.unix_name_len != 0) {
            if (fd->socket.unix_name[0] == '\0') {
                path[0] = '@';
                size_t copy_len = fd->socket.unix_name_len - 1;
                if (copy_len > sizeof(path) - 2)
                    copy_len = sizeof(path) - 2;
                memcpy(path + 1, fd->socket.unix_name + 1, copy_len);
                path[copy_len + 1] = '\0';
            } else {
                size_t copy_len = fd->socket.unix_name_len;
                if (copy_len > sizeof(path) - 1)
                    copy_len = sizeof(path) - 1;
                memcpy(path, fd->socket.unix_name, copy_len);
                path[copy_len] = '\0';
            }
        }

        // unix_seq_show's "%pK: %08X %08X %08X %04X %02X %5lu".
        proc_printf(buf, "%08x: %08X %08X %08X %04X %02X %5lu",
                i, 0u, 0u, flags, type, st, sock_inode(fd));
        if (path[0] != '\0')
            proc_printf(buf, " %s", path);
        proc_printf(buf, "\n");
    }
    sock_snapshot_release(&sockets);
    return 0;
}

static int proc_show_route(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    proc_printf(buf, "Iface    Destination    Gateway     Flags    RefCnt    Use    Metric    Mask        MTU    Window    IRTT \n");
    struct host_route_table routes = {};
    if (host_route_table_collect(&routes) == 0) {
        for (size_t i = 0; i < routes.count; i++) {
            const struct host_route_entry *route = &routes.entries[i];
            // The whole name, as Linux prints it: a precision here cut
            // pdp_ip0 or bridge100 to six characters, naming no interface.
            proc_printf(buf, "%-6s  %08X  %08X  %04X  %d  %d  %d  %08X  %u  %d  %d\n",
                    route->ifname,
                    route->destination_be,
                    route->gateway_be,
                    route->proc_flags,
                    0, 0, 0,
                    route->mask_be,
                    route->mtu,
                    0, 0);
        }
        host_route_table_free(&routes);
    }
    return 0;
}

static int proc_show_dev(struct proc_entry * UNUSED(entry), struct proc_data *buf) {
    proc_printf(buf, "Inter-|   Receive                            "
                 "                    |  Transmit\n"
                 " face |bytes    packets errs drop fifo frame "
                 "compressed multicast|bytes    packets errs "
                 "drop fifo colls carrier compressed\n");

    // The same snapshot /sys/class/net is built from, so the two cannot
    // disagree about which interfaces exist or what their counters say.
    struct net_iface_stats ifaces[PROC_NET_DEV_MAX_IFACES];
    int count = net_iface_snapshot(ifaces, PROC_NET_DEV_MAX_IFACES);
    if (count > PROC_NET_DEV_MAX_IFACES)
        count = PROC_NET_DEV_MAX_IFACES;

    for (int i = 0; i < count; i++) {
        const struct net_iface_stats *iface = &ifaces[i];
        /* Linux's dev_seq_printf_stats() always emits a literal space after the
           name colon ("%6s: %7llu ..."), which guarantees a separator even when
           rx_bytes is 8+ digits. Without it, busybox/net-tools ifconfig's
           whitespace tokenizer glues the byte count onto the interface name
           (e.g. "lo0:3020696576"), so the parsed name is wrong/empty and the
           follow-up SIOCGIFFLAGS lookup fails with ENODEV -> "Device not found".

           Sixteen conversions take sixteen arguments: eight receive columns then
           eight transmit ones. Linux sums four of its own counters into the
           single "frame" column and four more into "carrier"; neither platform's
           getifaddrs has an equivalent, so both are reported as zero. Giving each
           half of those sums an argument of its own is what once shifted every
           transmit column one place left. */
        proc_printf(buf, "%6s: %7llu %7llu %4llu %4llu %4llu %5llu %10llu %9llu "
                         "%8llu %7llu %4llu %4llu %4llu %5llu %7llu %10llu\n",
                     iface->name,
                     (unsigned long long) iface->rx_bytes,
                     (unsigned long long) iface->rx_packets,
                     (unsigned long long) iface->rx_errors,
                     (unsigned long long) iface->rx_dropped,
                     0ULL,                                  // rx_fifo_errors
                     0ULL,                                  // frame
                     0ULL,                                  // rx_compressed
                     (unsigned long long) iface->multicast,
                     (unsigned long long) iface->tx_bytes,
                     (unsigned long long) iface->tx_packets,
                     (unsigned long long) iface->tx_errors,
                     (unsigned long long) iface->tx_dropped,
                     0ULL,                                  // tx_fifo_errors
                     (unsigned long long) iface->collisions,
                     0ULL,                                  // carrier
                     0ULL);                                 // tx_compressed
    }
    return 0;
}

#define PROC_NET_LEN sizeof(proc_net_entries) / sizeofproc_net_entries
/*
dr-xr-xr-x 5 root root 0 Jun  5 10:55 dev_snmp6
dr-xr-xr-x 3 root root 0 Jun  5 10:55 ipconfig
dr-xr-xr-x 3 root root 0 Jun  5 10:55 netfilter
dr-xr-xr-x 4 root root 0 Jun  5 10:55 nfsfs
dr-xr-xr-x 8 root root 0 Jun  5 10:55 rpc
dr-xr-xr-x 5 root root 0 Jun  5 10:55 stat
dr-xr-xr-x 3 root root 0 Jun  5 10:55 vlan
*/
static bool net_show_net_snmp6(struct proc_entry *UNUSED(entry), unsigned long *UNUSED(index), struct proc_entry *UNUSED(next_entry)) {
    return 0;
}

static bool net_show_ipconfig(struct proc_entry *UNUSED(entry), unsigned long *UNUSED(index), struct proc_entry *UNUSED(next_entry)) {
    return 0;
}

static bool net_show_netfilter(struct proc_entry *UNUSED(entry), unsigned long *UNUSED(index), struct proc_entry *UNUSED(next_entry)) {
    return 0;
}

static bool net_show_nfsfs(struct proc_entry *UNUSED(entry), unsigned long *UNUSED(index), struct proc_entry *UNUSED(next_entry)) {
    return 0;
}

static bool net_show_rpc(struct proc_entry *UNUSED(entry), unsigned long *UNUSED(index), struct proc_entry *UNUSED(next_entry)) {
    return 0;
}

static bool net_show_stat(struct proc_entry *UNUSED(entry), unsigned long *UNUSED(index), struct proc_entry *UNUSED(next_entry)) {
    return 0;
}

static bool net_show_vlan(struct proc_entry *UNUSED(entry), unsigned long *UNUSED(index), struct proc_entry *UNUSED(next_entry)) {
    return 0;
}

struct proc_children proc_net_children = PROC_CHILDREN({
    {"arp", .show = proc_show_arp},
    {"ipconfig", S_IFDIR, .readdir = net_show_ipconfig},
    {"net_snmp6", S_IFDIR, .readdir = net_show_net_snmp6},
    {"netfilter", S_IFDIR, .readdir = net_show_netfilter},
    {"nfsfs", S_IFDIR, .readdir = net_show_nfsfs},
    {"raw", .show = proc_show_raw},
    {"raw6", .show = proc_show_raw6},
    {"rpc", S_IFDIR, .readdir = net_show_rpc},
    {"stat", S_IFDIR, .readdir = net_show_stat},
    {"tcp", .show = proc_show_tcp},
    {"tcp6", .show = proc_show_tcp6},
    {"udp", .show = proc_show_udp},
    {"udp6", .show = proc_show_udp6},
    {"unix", .show = proc_show_unix},
    {"vlan", S_IFDIR, .readdir = net_show_vlan},
    //{"defaults",  .show = proc_show_dev},
    {"dev", .show = proc_show_dev},
    {"route", .show = proc_show_route },
    {"if_inet6", .show = proc_show_if_inet6 },
});

void proc_net_init(struct proc_dir_entry *root_entry) {
    if (root_entry == NULL)
        return;
    proc_set_children_parent(&proc_net_children, root_entry);
}
