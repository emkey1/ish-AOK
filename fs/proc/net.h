#include <stdint.h>

// One network interface, as both /proc/net/dev and /sys/class/net describe it.
//
// Shared deliberately. These two files have to agree about which interfaces
// exist and what their counters say, and the way to guarantee that is one
// source rather than two readers of getifaddrs() -- the same lesson
// /proc/diskstats and /proc/mounts taught by disagreeing about a device name
// until 2026-08-19.
#define NET_IFACE_NAME_MAX 32
struct net_iface_stats {
    char name[NET_IFACE_NAME_MAX];
    uint64_t rx_bytes, rx_packets, rx_errors, rx_dropped, multicast;
    uint64_t tx_bytes, tx_packets, tx_errors, tx_dropped, collisions;
    uint64_t mtu;
    unsigned int flags;     // IFF_* as the host reports them
    unsigned char mac[6];
    bool has_mac;
    bool has_stats;         // false when the host gave no counters for it
};

// Snapshot every interface. Fills at most `max` entries into `out` (which may
// be NULL to count only) and returns the total number of interfaces, which can
// exceed `max`.
int net_iface_snapshot(struct net_iface_stats *out, int max);
#include <stddef.h>
#include <stdbool.h>

//extern static ssize_t proc_show_dev(struct proc_entry * UNUSED(entry), char *buf);
extern bool proc_net_readdir(struct proc_entry * UNUSED(entry), unsigned long *index, struct proc_entry *next_entry);
//extern bool (*remove_user_default)(const char *name);
