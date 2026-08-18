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

#import <ifaddrs.h>
#import <netinet/in.h>
#import <netinet/tcp.h>
#import <sys/socket.h>
#import <unistd.h>
#if defined(__APPLE__)
#import <net/if_var.h>
#endif

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

struct proc_net_socket_entry {
    struct fd **fds;
    unsigned count;
    unsigned cap;
};

static int proc_net_socket_push(struct proc_net_socket_entry *entries, struct fd *fd) {
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

static struct fdtable *proc_net_task_files_retain(struct task *task) {
    struct fdtable *files = NULL;
    // trylock, not lock: task_snapshot_collect() only checks task->exiting at
    // snapshot time, so a task in this snapshot can start exiting before this
    // loop reaches it. do_exit() then holds general_lock while it waits for
    // exit_wait_needed() to clear -- which won't happen until this snapshot's
    // own reference on the task is dropped by task_snapshot_release(), at the
    // end of the caller's loop. A blocking lock() here would wait forever for
    // a lock that can't be released until this very call returns -- the same
    // deadlock class fixed in fs/proc/root.c's collect_mem_page_stats(), and
    // the same trylock-and-skip fallback proc_entry_stat() already uses: a
    // task we can't immediately lock is mid-exit and about to disappear, so
    // treating it as having no open files for this snapshot is harmless.
    if (trylock(&task->general_lock) != 0)
        return NULL;
    if (task->files != NULL)
        files = fdtable_retain(task->files);
    unlock(&task->general_lock);
    return files;
}

static void proc_net_socket_release(struct proc_net_socket_entry *entries) {
    for (unsigned i = 0; i < entries->count; i++)
        fd_close(entries->fds[i]);
    free(entries->fds);
}

static int proc_net_collect_sockets(struct proc_net_socket_entry *entries, int domain, int type) {
    struct task_snapshot snapshot = {};
    int err = task_snapshot_collect(&snapshot, false);
    if (err < 0)
        return err;

    for (unsigned i = 0; i < snapshot.count; i++) {
        struct task *task = snapshot.tasks[i];
        if (task == NULL)
            continue;
        struct fdtable *files = proc_net_task_files_retain(task);
        if (files == NULL)
            continue;
        lock(&files->lock, 0);
        for (fd_t fd_no = 0; (unsigned) fd_no < files->size; fd_no++) {
            struct fd *fd = fdtable_get(files, fd_no);
            if (fd == NULL || fd->ops != &socket_fdops)
                continue;
            if (fd->socket.domain != domain || fd->socket.type != type)
                continue;
            if (fd->real_fd < 0)
                continue;
            err = proc_net_socket_push(entries, fd);
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

static int proc_net_collect_sockets_any_type(struct proc_net_socket_entry *entries, int domain) {
    struct task_snapshot snapshot = {};
    int err = task_snapshot_collect(&snapshot, false);
    if (err < 0)
        return err;

    for (unsigned i = 0; i < snapshot.count; i++) {
        struct task *task = snapshot.tasks[i];
        if (task == NULL)
            continue;
        struct fdtable *files = proc_net_task_files_retain(task);
        if (files == NULL)
            continue;
        lock(&files->lock, 0);
        for (fd_t fd_no = 0; (unsigned) fd_no < files->size; fd_no++) {
            struct fd *fd = fdtable_get(files, fd_no);
            if (fd == NULL || fd->ops != &socket_fdops)
                continue;
            if (fd->socket.domain != domain)
                continue;
            err = proc_net_socket_push(entries, fd);
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

static unsigned long proc_net_inode(const struct fd *fd) {
    if (fd->inode != NULL)
        return (unsigned long) fd->inode;
    if (fd->fake_inode != 0)
        return (unsigned long) fd->fake_inode;
    return (unsigned long) (uintptr_t) fd;
}

static uint32_t proc_net_ipv4_addr(const struct sockaddr_in *addr) {
    return ntohl(addr->sin_addr.s_addr);
}

static void proc_net_ipv6_addr(const struct sockaddr_in6 *addr, char *buf, size_t buf_len) {
    uint32_t words[4];
    memcpy(words, addr->sin6_addr.s6_addr, sizeof(words));
    snprintf(buf, buf_len, "%08X%08X%08X%08X",
            ntohl(words[0]), ntohl(words[1]), ntohl(words[2]), ntohl(words[3]));
}

static int proc_net_tcp_state(struct fd *fd) {
#if defined(__APPLE__)
    struct tcp_connection_info conn_info;
    socklen_t conn_info_size = sizeof(conn_info);
    if (getsockopt(fd->real_fd, IPPROTO_TCP, TCP_CONNECTION_INFO, &conn_info, &conn_info_size) == 0) {
        static const uint8_t tcp_state_table[] = {
            7,  // TCPS_CLOSED
            10, // TCPS_LISTEN
            2,  // TCPS_SYN_SENT
            3,  // TCPS_SYN_RECEIVED
            1,  // TCPS_ESTABLISHED
            8,  // TCPS_CLOSE_WAIT
            4,  // TCPS_FIN_WAIT_1
            11, // TCPS_CLOSING
            9,  // TCPS_LAST_ACK
            5,  // TCPS_FIN_WAIT_2
            6,  // TCPS_TIME_WAIT
        };
        if (conn_info.tcpi_state < sizeof(tcp_state_table))
            return tcp_state_table[conn_info.tcpi_state];
    }
#endif
    int acceptconn = 0;
    socklen_t len = sizeof(acceptconn);
    if (getsockopt(fd->real_fd, SOL_SOCKET, SO_ACCEPTCONN, &acceptconn, &len) == 0 && acceptconn)
        return 0x0A;

    struct sockaddr_storage peer;
    len = sizeof(peer);
    if (getpeername(fd->real_fd, (struct sockaddr *) &peer, &len) == 0)
        return 0x01;
    return 0x07;
}

static int proc_net_recv_q(struct fd *fd) {
    int bytes = 0;
    if (ioctl(fd->real_fd, FIONREAD, &bytes) < 0)
        return 0;
    return bytes < 0 ? 0 : bytes;
}

static int proc_show_inet_sockets(struct proc_data *buf, int domain, int type) {
    struct proc_net_socket_entry entries = {};
    int err = proc_net_collect_sockets(&entries, domain, type);
    if (err < 0) {
        proc_net_socket_release(&entries);
        return err;
    }

    // The header is how a reader decides the FORMAT, so the v6 files need the
    // v6 one -- `remote_address' rather than `rem_address', and address columns
    // wide enough for 128 bits. Emitting the v4 header for all four files made
    // lsof refuse both v6 tables outright:
    //
    //     lsof: WARNING: unsupported format: /proc/net/tcp6
    //     lsof: WARNING: unsupported format: /proc/net/udp6
    //
    // The ROWS were already right -- see the %32s below -- so only the banner
    // was lying about them.
    //
    // Taken verbatim from the kernel rather than from memory: tcp6_seq_show in
    // net/ipv6/tcp_ipv6.c, and IPV6_SEQ_DGRAM_HEADER in include/net/transp_v6.h
    // for the datagram variant, which differs only by the three trailing
    // columns. The v4 datagram header gains those same three, which our rows
    // have always emitted.
    if (domain == AF_INET_)
        proc_printf(buf, "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode%s\n",
                type == SOCK_STREAM_ ? "" : " ref pointer drops");
    else
        proc_printf(buf, "  sl  "
                "local_address                         "
                "remote_address                        "
                "st tx_queue rx_queue tr tm->when retrnsmt"
                "   uid  timeout inode%s\n",
                type == SOCK_STREAM_ ? "" : " ref pointer drops");
    for (unsigned i = 0; i < entries.count; i++) {
        struct fd *fd = entries.fds[i];
        struct sockaddr_storage local = {};
        struct sockaddr_storage peer = {};
        socklen_t local_len = sizeof(local);
        socklen_t peer_len = sizeof(peer);
        if (getsockname(fd->real_fd, (struct sockaddr *) &local, &local_len) < 0)
            continue;

        bool has_peer = getpeername(fd->real_fd, (struct sockaddr *) &peer, &peer_len) == 0;
        int state = (type == SOCK_STREAM_) ? proc_net_tcp_state(fd) : (has_peer ? 0x01 : 0x07);
        int recv_q = proc_net_recv_q(fd);
        unsigned uid = current_uid(current);
        unsigned long inode = proc_net_inode(fd);

        if (domain == AF_INET_) {
            if (local.ss_family != AF_INET)
                continue;
            struct sockaddr_in *local4 = (struct sockaddr_in *) &local;
            uint32_t local_addr = proc_net_ipv4_addr(local4);
            uint16_t local_port = ntohs(local4->sin_port);
            uint32_t peer_addr = 0;
            uint16_t peer_port = 0;
            if (has_peer && peer.ss_family == AF_INET) {
                struct sockaddr_in *peer4 = (struct sockaddr_in *) &peer;
                peer_addr = proc_net_ipv4_addr(peer4);
                peer_port = ntohs(peer4->sin_port);
            }
            proc_printf(buf,
                    "%4u: %08X:%04X %08X:%04X %02X %08X:%08X %02X:%08X %08X %5u %8u %lu 1 0000000000000000 100 0 0 10 0\n",
                    i, local_addr, local_port, peer_addr, peer_port, state,
                    0, recv_q, 0, 0, 0, uid, 0u, inode);
        } else {
            if (local.ss_family != AF_INET6)
                continue;
            char local_addr[33];
            char peer_addr[33];
            struct sockaddr_in6 *local6 = (struct sockaddr_in6 *) &local;
            proc_net_ipv6_addr(local6, local_addr, sizeof(local_addr));
            uint16_t local_port = ntohs(local6->sin6_port);
            memset(peer_addr, '0', sizeof(peer_addr) - 1);
            peer_addr[sizeof(peer_addr) - 1] = '\0';
            uint16_t peer_port = 0;
            if (has_peer && peer.ss_family == AF_INET6) {
                struct sockaddr_in6 *peer6 = (struct sockaddr_in6 *) &peer;
                proc_net_ipv6_addr(peer6, peer_addr, sizeof(peer_addr));
                peer_port = ntohs(peer6->sin6_port);
            }
            proc_printf(buf,
                    "%4u: %32s:%04X %32s:%04X %02X %08X:%08X %02X:%08X %08X %5u %8u %lu 1 0000000000000000 100 0 0 10 0\n",
                    i, local_addr, local_port, peer_addr, peer_port, state,
                    0, recv_q, 0, 0, 0, uid, 0u, inode);
        }
    }
    proc_net_socket_release(&entries);
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
    struct proc_net_socket_entry entries = {};
    int err = proc_net_collect_sockets_any_type(&entries, AF_LOCAL_);
    if (err < 0) {
        proc_net_socket_release(&entries);
        return err;
    }

    proc_printf(buf, "Num       RefCount Protocol Flags    Type St Inode Path\n");
    for (unsigned i = 0; i < entries.count; i++) {
        struct fd *fd = entries.fds[i];
        unsigned long inode = proc_net_inode(fd);
        unsigned type = fd->socket.type & 0xf;
        unsigned st = 1;
        unsigned flags = 0;
        int acceptconn = 0;
        socklen_t len = sizeof(acceptconn);
        if (getsockopt(fd->real_fd, SOL_SOCKET, SO_ACCEPTCONN, &acceptconn, &len) == 0 && acceptconn) {
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

        proc_printf(buf, "%08x: %08x %08x %08x %04x %02x %lu",
                i, 0u, 0u, flags, type, st, inode);
        if (path[0] != '\0')
            proc_printf(buf, " %s", path);
        proc_printf(buf, "\n");
    }
    proc_net_socket_release(&entries);
    return 0;
}


static int proc_show_route(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    proc_printf(buf, "Iface    Destination    Gateway     Flags    RefCnt    Use    Metric    Mask        MTU    Window    IRTT \n");
    struct host_route_table routes = {};
    if (host_route_table_collect(&routes) == 0) {
        for (size_t i = 0; i < routes.count; i++) {
            const struct host_route_entry *route = &routes.entries[i];
            proc_printf(buf, "%-6.6s  %08X  %08X  %04X  %d  %d  %d  %08X  %u  %d  %d\n",
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

    struct ifaddrs *addrs;
    bool success = (getifaddrs(&addrs) == 0);
    if (success) {
        const struct ifaddrs *cursor = addrs;
        while (cursor != NULL) {
            if (cursor->ifa_addr->sa_family == AF_LINK) {
              /*
               Inter-|   Receive                                                |  Transmit
                 face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed
                    lo:    4214      37    0    0    0     0          0         0     4214      37    0    0    0     0       0          0
                  eth0:  410911    6589    0    0    0     0          0       116   264679    4078    0    0    0     0       0          0
                 wlan0: 3014178    3267    0    0    0     0          0         2   126045    1205    0    0    0     0       0          0
               */
                const struct if_data *stats = (struct if_data *)cursor->ifa_data;
                
                if (stats != NULL) {
                    /* Linux's dev_seq_printf_stats() always emits a literal
                       space after the name colon ("%6s: %7llu ..."), which
                       guarantees a separator even when rx_bytes is 8+ digits.
                       Without it, busybox/net-tools ifconfig's whitespace
                       tokenizer glues the byte count onto the interface
                       name (e.g. "lo0:3020696576"), so the parsed name is
                       wrong/empty and the follow-up SIOCGIFFLAGS lookup
                       fails with ENODEV -> "Device not found". */
                    proc_printf(buf, "%6s: %7lu %7lu %4lu %4lu %4lu %5lu %10lu %9lu %8lu %7lu %4lu %4lu %4lu %5lu %7lu %10lu\n",
                                 cursor->ifa_name,
                                 (unsigned long)stats->ifi_ibytes,   // stats->rx_bytes,
                                 (unsigned long)stats->ifi_ipackets,   // stats->rx_packets,
                                 (unsigned long)stats->ifi_ierrors,  // stats->rx_errors,
                                 (unsigned long)stats->ifi_iqdrops,  // stats->rx_dropped + stats->rx_missed_errors,
                                 (unsigned long)0,  // stats->rx_fifo_errors,
                                 (unsigned long)0,  // stats->rx_length_errors + stats->rx_over_errors +
                                 (unsigned long)0,  // stats->rx_crc_errors + stats->rx_frame_errors,
                                 (unsigned long)0,  // stats->rx_compressed,
                                 (unsigned long)stats->ifi_imcasts,  // stats->multicast,
                                 (unsigned long)stats->ifi_obytes,  // stats->tx_bytes,
                                 (unsigned long)stats->ifi_opackets,  // stats->tx_packets,
                                 (unsigned long)stats->ifi_oerrors,  // stats->tx_errors,
                                 (unsigned long)0,  // stats->tx_dropped,
                                 (unsigned long)0,  // stats->tx_fifo_errors,
                                 (unsigned long)stats->ifi_collisions,  // stats->collisions,
                                 (unsigned long)stats->ifi_ierrors + stats->ifi_oerrors,  // stats->tx_carrier_errors + stats->tx_aborted_errors +
                                 (unsigned long)0,  // stats->tx_window_errors + stats->tx_heartbeat_errors,
                                 (unsigned long)0
                                );  // stats->tx_compressed);
                } else {
                    proc_printf(buf, "%6.6s:%8lu %7lu %4lu %4lu %4lu %5lu %10lu %9lu %8lu %7lu %4lu %4lu %4lu %5lu %7lu %10lu\n",
                                 cursor->ifa_name,
                                 (unsigned long long)0,
                                 (unsigned long long)0,
                                 (unsigned long long)0,
                                 (unsigned long long)0,
                                 (unsigned long long)0,
                                 (unsigned long long)0,
                                 (unsigned long long)0,
                                 (unsigned long long)0,
                                 (unsigned long long)0,
                                 (unsigned long long)0,
                                 (unsigned long long)0,
                                 (unsigned long long)0,
                                 (unsigned long long)0,
                                 (unsigned long long)0,
                                 (unsigned long long)0,
                                 (unsigned long long)0);
                }
            }
            cursor = cursor->ifa_next;
        }
        freeifaddrs(addrs);
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
