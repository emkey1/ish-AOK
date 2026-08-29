#ifndef SYS_SOCK_H
#define SYS_SOCK_H

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include "kernel/errno.h"
#include "fs/fd.h"
#include "misc.h"
#include "debug.h"

#ifndef IPV6_RECVHOPLIMIT
#define IPV6_RECVHOPLIMIT 37
#endif

#ifndef IPV6_HOPLIMIT
#define IPV6_HOPLIMIT 47
#endif

extern const struct fd_ops socket_fdops;

int_t sys_socketcall(dword_t call_num, addr_t args_addr);
int_t sys_socketcall_guest(dword_t call_num, guest_addr_t args_addr);

int_t sys_socket(dword_t domain, dword_t type, dword_t protocol);
int_t sys_bind(fd_t sock_fd, addr_t sockaddr_addr, uint_t sockaddr_len);
int_t sys_bind_guest(fd_t sock_fd, guest_addr_t sockaddr_addr, uint_t sockaddr_len);
int_t sys_connect(fd_t sock_fd, addr_t sockaddr_addr, uint_t sockaddr_len);
int_t sys_connect_guest(fd_t sock_fd, guest_addr_t sockaddr_addr, uint_t sockaddr_len);
int_t sys_listen(fd_t sock_fd, int_t backlog);
int_t sys_accept(fd_t sock_fd, addr_t sockaddr_addr, addr_t sockaddr_len_addr);
int_t sys_accept_guest(fd_t sock_fd, guest_addr_t sockaddr_addr, guest_addr_t sockaddr_len_addr);
int_t sys_accept4(fd_t sock_fd, addr_t sockaddr_addr, addr_t sockaddr_len_addr, int_t flags);
int_t sys_accept4_guest(fd_t sock_fd, guest_addr_t sockaddr_addr, guest_addr_t sockaddr_len_addr, int_t flags);
int_t sys_getsockname(fd_t sock_fd, addr_t sockaddr_addr, addr_t sockaddr_len_addr);
int_t sys_getsockname_guest(fd_t sock_fd, guest_addr_t sockaddr_addr, guest_addr_t sockaddr_len_addr);
int_t sys_getpeername(fd_t sock_fd, addr_t sockaddr_addr, addr_t sockaddr_len_addr);
int_t sys_getpeername_guest(fd_t sock_fd, guest_addr_t sockaddr_addr, guest_addr_t sockaddr_len_addr);
int_t sys_socketpair(dword_t domain, dword_t type, dword_t protocol, addr_t sockets_addr);
int_t sys_socketpair_guest(dword_t domain, dword_t type, dword_t protocol, guest_addr_t sockets_addr);
int_t sys_sendto(fd_t sock_fd, addr_t buffer_addr, dword_t len, dword_t flags, addr_t sockaddr_addr, dword_t sockaddr_len);
int_t sys_sendto_guest(fd_t sock_fd, guest_addr_t buffer_addr, dword_t len, dword_t flags, guest_addr_t sockaddr_addr, dword_t sockaddr_len);
int_t sys_recvfrom(fd_t sock_fd, addr_t buffer_addr, dword_t len, dword_t flags, addr_t sockaddr_addr, addr_t sockaddr_len_addr);
int_t sys_recvfrom_guest(fd_t sock_fd, guest_addr_t buffer_addr, dword_t len, dword_t flags, guest_addr_t sockaddr_addr, guest_addr_t sockaddr_len_addr);
int_t sys_shutdown(fd_t sock_fd, dword_t how);
int_t sys_setsockopt(fd_t sock_fd, dword_t level, dword_t option, addr_t value_addr, dword_t value_len);
int_t sys_setsockopt_guest(fd_t sock_fd, dword_t level, dword_t option, guest_addr_t value_addr, dword_t value_len);
int_t sys_setsockopt_amd64(fd_t sock_fd, dword_t level, dword_t option, addr_t value_addr, dword_t value_len);
int_t sys_setsockopt_amd64_guest(fd_t sock_fd, dword_t level, dword_t option, guest_addr_t value_addr, dword_t value_len);
int_t sys_getsockopt(fd_t sock_fd, dword_t level, dword_t option, addr_t value_addr, dword_t len_addr);
int_t sys_getsockopt_guest(fd_t sock_fd, dword_t level, dword_t option, guest_addr_t value_addr, guest_addr_t len_addr);
int_t sys_getsockopt_amd64(fd_t sock_fd, dword_t level, dword_t option, addr_t value_addr, dword_t len_addr);
int_t sys_getsockopt_amd64_guest(fd_t sock_fd, dword_t level, dword_t option, guest_addr_t value_addr, guest_addr_t len_addr);
int_t sys_sendmsg(fd_t sock_fd, addr_t msghdr_addr, int_t flags);
int_t sys_sendmsg_guest(fd_t sock_fd, guest_addr_t msghdr_addr, int_t flags);
int_t sys_sendmsg_amd64(fd_t sock_fd, addr_t msghdr_addr, int_t flags);
int_t sys_sendmsg_amd64_guest(fd_t sock_fd, guest_addr_t msghdr_addr, int_t flags);
int_t sys_recvmsg(fd_t sock_fd, addr_t msghdr_addr, int_t flags);
int_t sys_recvmsg_guest(fd_t sock_fd, guest_addr_t msghdr_addr, int_t flags);
int_t sys_recvmsg_amd64(fd_t sock_fd, addr_t msghdr_addr, int_t flags);
int_t sys_recvmsg_amd64_guest(fd_t sock_fd, guest_addr_t msghdr_addr, int_t flags);
int_t sys_recvmmsg(fd_t sock_fd, addr_t msgvec_addr, uint_t msgvec_len, int_t flags, addr_t timeout_addr);
int_t sys_recvmmsg_guest(fd_t sock_fd, guest_addr_t msgvec_addr, uint_t msgvec_len, int_t flags, guest_addr_t timeout_addr);
int_t sys_recvmmsg_amd64(fd_t sock_fd, addr_t msgvec_addr, uint_t msgvec_len, int_t flags, addr_t timeout_addr);
int_t sys_recvmmsg_amd64_guest(fd_t sock_fd, guest_addr_t msgvec_addr, uint_t msgvec_len, int_t flags, guest_addr_t timeout_addr);
int_t sys_recvmmsg_time64(fd_t sock_fd, addr_t msgvec_addr, uint_t msgvec_len, int_t flags, addr_t timeout_addr);
int_t sys_recvmmsg_time64_guest(fd_t sock_fd, guest_addr_t msgvec_addr, uint_t msgvec_len, int_t flags, guest_addr_t timeout_addr);
int_t sys_sendmmsg(fd_t sock_fd, addr_t msgvec_addr, uint_t msgvec_len, int_t flags);
int_t sys_sendmmsg_guest(fd_t sock_fd, guest_addr_t msgvec_addr, uint_t msgvec_len, int_t flags);
int_t sys_sendmmsg_amd64(fd_t sock_fd, addr_t msgvec_addr, uint_t msgvec_len, int_t flags);
int_t sys_sendmmsg_amd64_guest(fd_t sock_fd, guest_addr_t msgvec_addr, uint_t msgvec_len, int_t flags);

#define SOCKADDR_DATA_MAX 108

struct sockaddr_ {
    uint16_t family;
    char data[14];
};
struct sockaddr_max_ {
    uint16_t family;
    char data[SOCKADDR_DATA_MAX];
};

size_t sockaddr_size(void *p);
// result comes from malloc
struct sockaddr *sockaddr_to_real(void *p);

struct i386_msghdr_ {
    addr_t msg_name;
    uint_t msg_namelen;
    addr_t msg_iov;
    uint_t msg_iovlen;
    addr_t msg_control;
    uint_t msg_controllen;
    int_t msg_flags;
};
static_assert(sizeof(struct i386_msghdr_) == 28, "i386_msghdr size");

struct amd64_msghdr_ {
    qword_t msg_name;
    uint_t msg_namelen;
    uint_t __pad0;
    qword_t msg_iov;
    qword_t msg_iovlen;
    qword_t msg_control;
    qword_t msg_controllen;
    int_t msg_flags;
    uint_t __pad1;
};
static_assert(sizeof(struct amd64_msghdr_) == 56, "amd64_msghdr size");

struct msghdr_ {
    addr_t msg_name;
    uint_t msg_namelen;
    addr_t msg_iov;
    uint_t msg_iovlen;
    addr_t msg_control;
    uint_t msg_controllen;
    int_t msg_flags;
};

struct i386_cmsghdr_ {
    dword_t len;
    int_t level;
    int_t type;
};
static_assert(sizeof(struct i386_cmsghdr_) == 12, "i386_cmsghdr size");

struct amd64_cmsghdr_ {
    qword_t len;
    int_t level;
    int_t type;
};
static_assert(sizeof(struct amd64_cmsghdr_) == 16, "amd64_cmsghdr size");

struct cmsghdr_ {
    dword_t len;
    int_t level;
    int_t type;
    uint8_t data[];
};

struct sock_extended_err_ {
    uint32_t ee_errno;
    uint8_t ee_origin;
    uint8_t ee_type;
    uint8_t ee_code;
    uint8_t ee_pad;
    uint32_t ee_info;
    uint32_t ee_data;
};

#define SO_EE_ORIGIN_NONE_ 0
#define SO_EE_ORIGIN_LOCAL_ 1
#define SO_EE_ORIGIN_ICMP_ 2
#define SO_EE_ORIGIN_ICMP6_ 3

struct i386_mmsghdr_ {
    struct i386_msghdr_ hdr;
    uint_t len;
};
static_assert(sizeof(struct i386_mmsghdr_) == 32, "i386_mmsghdr size");

struct amd64_mmsghdr_ {
    struct amd64_msghdr_ hdr;
    uint_t len;
    uint_t __pad0;
};
static_assert(sizeof(struct amd64_mmsghdr_) == 64, "amd64_mmsghdr size");
#define SCM_RIGHTS_ 1
#define SCM_CREDENTIALS_ 2
// copied and ported from musl
#define CMSG_LEN_(cmsg) (((cmsg)->len + sizeof(dword_t) - 1) & ~(dword_t)(sizeof(dword_t) - 1))
#define CMSG_NEXT_(cmsg) ((uint8_t *)(cmsg) + CMSG_LEN_(cmsg))
#define CMSG_NXTHDR_(cmsg, mhdr_end) ((cmsg)->len < sizeof (struct cmsghdr_) || \
        CMSG_LEN_(cmsg) + sizeof(struct cmsghdr_) >= (size_t) (mhdr_end - (uint8_t *)(cmsg)) \
        ? NULL : (struct cmsghdr_ *)CMSG_NEXT_(cmsg))

struct scm {
    struct list queue;
    unsigned num_fds;
    struct fd *fds[];
};

#define PF_LOCAL_ 1
#define PF_INET_ 2
#define PF_BRIDGE_ 7
#define PF_NETLINK_ 16
#define PF_PACKET_ 17
#define PF_INET6_ 10
#define AF_LOCAL_ PF_LOCAL_
#define AF_INET_ PF_INET_
#define AF_BRIDGE_ PF_BRIDGE_
#define AF_NETLINK_ PF_NETLINK_
#define AF_PACKET_ PF_PACKET_
#define AF_INET6_ PF_INET6_
static inline int sock_family_to_real(int fake) {
    switch (fake) {
        case PF_LOCAL_: return PF_LOCAL;
        case PF_INET_: return PF_INET;
        case PF_NETLINK_: return PF_NETLINK_;
        case PF_INET6_: return PF_INET6;
    }
    return -1;
}
static inline int sock_family_from_real(int fake) {
    switch (fake) {
        case PF_LOCAL: return PF_LOCAL_;
        case PF_INET: return PF_INET_;
        case PF_NETLINK_: return PF_NETLINK_;
        case PF_INET6: return PF_INET6_;
    }
    return -1;
}

#define SOCK_STREAM_ 1
#define SOCK_DGRAM_ 2
#define SOCK_RAW_ 3
#define SOCK_SEQPACKET_ 5
#define SOCK_NONBLOCK_ 0x800
#define SOCK_CLOEXEC_ 0x80000

static inline int sock_type_to_real(int type, int protocol) {
    switch (type & 0xff) {
        case SOCK_STREAM_:
            if (protocol != 0 && protocol != IPPROTO_TCP)
                return -1;
            return SOCK_STREAM;
        case SOCK_DGRAM_:
            switch (protocol) {
                default:
                    return -1;
                case 0:
                case IPPROTO_UDP:
                case IPPROTO_ICMP:
                case IPPROTO_ICMPV6:
                    break;
            }
            return SOCK_DGRAM;
        case SOCK_RAW_:
            switch (protocol) {
                default:
                    return -1;
                case IPPROTO_RAW:
                case IPPROTO_UDP:
                case IPPROTO_ICMP:
                case IPPROTO_ICMPV6:
                    break;
            }
            return SOCK_DGRAM;
        case SOCK_SEQPACKET_:
            if (protocol != 0)
                return -1;
            return SOCK_SEQPACKET;
    }
    return -1;
}

#define MSG_OOB_ 0x1
#define MSG_PEEK_ 0x2
#define MSG_CTRUNC_  0x8
#define MSG_TRUNC_  0x20
#define MSG_DONTWAIT_ 0x40
#define MSG_EOR_    0x80
#define MSG_WAITALL_ 0x100
#define MSG_ERRQUEUE_ 0x2000
// Suppress SIGPIPE for this send only. Nothing is passed to the host: host
// SIGPIPE is already SIG_IGN process-wide (kernel/init.c), and Darwin has no
// MSG_NOSIGNAL anyway. What it controls is whether the GUEST gets the signal,
// which is decided when the host errno is mapped -- see errno_map_flags().
#define MSG_NOSIGNAL_ 0x4000

static inline int sock_flags_to_real(int fake) {
    int real = 0;
    if (fake & MSG_OOB_) real |= MSG_OOB;
    if (fake & MSG_PEEK_) real |= MSG_PEEK;
    if (fake & MSG_CTRUNC_) real |= MSG_CTRUNC;
    if (fake & MSG_TRUNC_) real |= MSG_TRUNC;
    if (fake & MSG_DONTWAIT_) real |= MSG_DONTWAIT;
    if (fake & MSG_EOR_) real |= MSG_EOR;
    if (fake & MSG_WAITALL_) real |= MSG_WAITALL;
    if (fake & ~(MSG_OOB_|MSG_PEEK_|MSG_CTRUNC_|MSG_TRUNC_|MSG_DONTWAIT_|MSG_EOR_|MSG_WAITALL_|MSG_ERRQUEUE_|MSG_NOSIGNAL_))
        TRACE("unimplemented socket flags %d\n", fake);
    return real;
}
static inline int sock_flags_from_real(int real) {
    int fake = 0;
    if (real & MSG_OOB) fake |= MSG_OOB_;
    if (real & MSG_PEEK) fake |= MSG_PEEK_;
    if (real & MSG_CTRUNC) fake |= MSG_CTRUNC_;
    if (real & MSG_TRUNC) fake |= MSG_TRUNC_;
    if (real & MSG_DONTWAIT) fake |= MSG_DONTWAIT_;
    if (real & MSG_EOR) fake |= MSG_EOR_;
    if (real & MSG_WAITALL) fake |= MSG_WAITALL_;
    if (real & ~(MSG_OOB|MSG_PEEK|MSG_CTRUNC|MSG_TRUNC|MSG_DONTWAIT|MSG_EOR|MSG_WAITALL))
        TRACE("unimplemented socket flags %d\n", real);
    return fake;
}

#define SOL_SOCKET_ 1
#define SOL_NETLINK_ 270

#define NETLINK_KOBJECT_UEVENT_ 15
#define NETLINK_SOCK_DIAG_ 4
#define NETLINK_ROUTE_ 0
#define NETLINK_AUDIT_ 9
#define NETLINK_GENERIC_ 16
#define NETLINK_ADD_MEMBERSHIP_ 1
#define NETLINK_DROP_MEMBERSHIP_ 2
#define NETLINK_PKTINFO_ 3
#define NETLINK_LIST_MEMBERSHIPS_ 9
#define NETLINK_CAP_ACK_ 10
#define NETLINK_EXT_ACK_ 11
#define NETLINK_GET_STRICT_CHK_ 12

#define SO_REUSEADDR_ 2
#define SO_TYPE_ 3
#define SO_ERROR_ 4
#define SO_BROADCAST_ 6
#define SO_SNDBUF_ 7
#define SO_RCVBUF_ 8
#define SO_KEEPALIVE_ 9
#define SO_LINGER_ 13
#define SO_REUSEPORT_ 15
#define SO_PASSCRED_ 16
#define SO_PEERCRED_ 17
#define SO_RCVLOWAT_ 18
#define SO_SNDLOWAT_ 19
#define SO_RCVTIMEO_OLD_ 20
#define SO_SNDTIMEO_OLD_ 21
#define SO_BINDTODEVICE_ 25
#define SO_BINDTOIFINDEX_ 62
#define SO_ATTACH_FILTER_ 26
#define SO_DETACH_FILTER_ 27
#define SO_TIMESTAMP_ 29
#define SO_ACCEPTCONN_ 30
#define SO_PEERSEC_ 31
#define SO_SNDBUFFORCE_ 32
#define SO_RCVBUFFORCE_ 33
#define SO_PASSSEC_ 34
#define SO_PROTOCOL_ 38
#define SO_DOMAIN_ 39
#define SO_PEERGROUPS_ 59
#define SO_RCVTIMEO_ 66
#define SO_SNDTIMEO_ 67
#define IP_TOS_ 1
#define IP_TTL_ 2
#define IP_HDRINCL_ 3
#define IP_RETOPTS_ 7
// Linux IP_PKTINFO (boolean toggle AND the cmsg type it delivers). Darwin
// splits nothing but numbers differently: IP_RECVPKTINFO/IP_PKTINFO are
// both 26 there; struct in_pktinfo layouts are identical (ifindex,
// spec_dst, addr -- 12 bytes).
#define IP_PKTINFO_ 8
#define IP_MTU_DISCOVER_ 10
#define IP_RECVERR_ 11
#define IP_RECVTTL_ 12
#define IP_MTU_ 14
#define IP_RECVTOS_ 13
#define TCP_NODELAY_ 1
#define TCP_DEFER_ACCEPT_ 9
#define TCP_INFO_ 11
#define TCP_CONGESTION_ 13
#define TCP_FASTOPEN_ 23
#define IPV6_MTU_DISCOVER_ 23
#define IPV6_MTU_ 24
#define IPV6_UNICAST_HOPS_ 16
#define IPV6_RECVERR_ 25
#define IPV6_RECVHOPLIMIT_ 51
#define IPV6_HOPLIMIT_ 52
#define IPV6_V6ONLY_ 26
#define IPV6_RECVPKTINFO_ 49
#define IPV6_TCLASS_ 67
#define ICMP6_FILTER_ 1

static inline int sock_opt_to_real(int fake, int level) {
    switch (level) {
        case SOL_SOCKET_: switch (fake) {
            case SO_REUSEADDR_: return SO_REUSEADDR;
            case SO_TYPE_: return SO_TYPE;
            case SO_ERROR_: return SO_ERROR;
            case SO_BROADCAST_: return SO_BROADCAST;
            case SO_KEEPALIVE_: return SO_KEEPALIVE;
            case SO_LINGER_: return SO_LINGER;
            case SO_SNDBUF_: return SO_SNDBUF;
            case SO_RCVBUF_: return SO_RCVBUF;
            case SO_SNDLOWAT_: return SO_SNDLOWAT;
            case SO_RCVLOWAT_: return SO_RCVLOWAT;
#ifdef SO_REUSEPORT
            case SO_REUSEPORT_: return SO_REUSEPORT;
#endif
            case SO_TIMESTAMP_: return SO_TIMESTAMP;
            case SO_ACCEPTCONN_: return SO_ACCEPTCONN;
            case SO_RCVTIMEO_OLD_:
            case SO_RCVTIMEO_: return SO_RCVTIMEO;
            case SO_SNDTIMEO_OLD_:
            case SO_SNDTIMEO_: return SO_SNDTIMEO;
        } break;
        case IPPROTO_TCP: switch (fake) {
            case TCP_NODELAY_: return TCP_NODELAY;
            case TCP_DEFER_ACCEPT_: return 0; // unimplemented
#if defined(__linux__)
            case TCP_INFO_: return TCP_INFO;
            case TCP_CONGESTION_: return TCP_CONGESTION;
#endif
        } break;
        case IPPROTO_IP: switch (fake) {
            case IP_TOS_: return IP_TOS;
            case IP_TTL_: return IP_TTL;
            case IP_HDRINCL_: return IP_HDRINCL;
            case IP_RETOPTS_: return IP_RETOPTS;
#ifdef IP_RECVPKTINFO
            // systemd-resolved's DNS stub listener sets this and treats
            // failure as fatal ("Failed to listen on UDP socket
            // 127.0.0.53:53").
            case IP_PKTINFO_: return IP_RECVPKTINFO;
#endif
            case IP_RECVTTL_: return IP_RECVTTL;
            case IP_RECVTOS_: return IP_RECVTOS;
        } break;
        case IPPROTO_IPV6: switch (fake) {
            case IPV6_UNICAST_HOPS_: return IPV6_UNICAST_HOPS;
            case IPV6_RECVHOPLIMIT_: return IPV6_RECVHOPLIMIT;
            case IPV6_TCLASS_: return IPV6_TCLASS;
            case IPV6_V6ONLY_: return IPV6_V6ONLY;
            // The IPv6 twin of IP_PKTINFO. systemd-resolved's LLMNR/mDNS and
            // DNS-stub IPv6 UDP sockets set this via socket_set_recvpktinfo()
            // and treat failure as fatal to link processing ("LLMNR-IPv6(UDP):
            // Failed to set common socket options"), which cascaded into
            // "Could not create manager" and took down all DNS. Darwin hides
            // its IPV6_RECVPKTINFO (61) behind __APPLE_USE_RFC_3542, which we
            // cannot define globally without shifting other IPV6_* meanings,
            // so use the raw value.
#ifdef IPV6_RECVPKTINFO
            case IPV6_RECVPKTINFO_: return IPV6_RECVPKTINFO;
#elif defined(__APPLE__)
            case IPV6_RECVPKTINFO_: return 61;
#endif
        } break;
    }
    return -1;
}

// Linux IP/IPv6 multicast option numbers (asm-generic; same on every Linux
// arch). Guest values -- translated to Darwin in the multicast handler.
#define IP_MULTICAST_IF_ 32
#define IP_MULTICAST_TTL_ 33
#define IP_MULTICAST_LOOP_ 34
#define IP_ADD_MEMBERSHIP_ 35
#define IP_DROP_MEMBERSHIP_ 36
#define IPV6_MULTICAST_IF_ 17
#define IPV6_MULTICAST_HOPS_ 18
#define IPV6_MULTICAST_LOOP_ 19
#define IPV6_ADD_MEMBERSHIP_ 20
#define IPV6_DROP_MEMBERSHIP_ 21

static inline int sock_level_to_real(int fake) {
    if (fake == SOL_SOCKET_)
        return SOL_SOCKET;
    return fake;
}

extern const char *sock_tmp_prefix;

struct tcp_info_ {
    uint8_t state;
    uint8_t ca_state;
    uint8_t retransmits;
    uint8_t probes;
    uint8_t backoff;
    uint8_t options;
    uint8_t snd_wscale:4, rcv_wscale:4;

    uint32_t rto;
    uint32_t ato;
    uint32_t snd_mss;
    uint32_t rcv_mss;

    uint32_t unacked;
    uint32_t sacked;
    uint32_t lost;
    uint32_t retrans;
    uint32_t fackets;

    uint32_t last_data_sent;
    uint32_t last_ack_sent;
    uint32_t last_data_recv;
    uint32_t last_ack_recv;

    uint32_t pmtu;
    uint32_t rcv_ssthresh;
    uint32_t rtt;
    uint32_t rttvar;
    uint32_t snd_ssthresh;
    uint32_t snd_cwnd;
    uint32_t advmss;
    uint32_t reordering;

    uint32_t rcv_rtt;
    uint32_t rcv_space;

    uint32_t total_retrans;
};

// Starts the background thread that watches the host's network interfaces/
// addresses for changes and delivers RTM_NEWLINK/RTM_DELLINK/RTM_NEWADDR/
// RTM_DELADDR notifications to any netlink socket subscribed to the
// relevant multicast group. Call once, early, from the process's single
// shared entry point (xX_main_Xx) -- not per-task, per-architecture, or
// per-engine; there is exactly one of these threads per iSH process.
void netlink_link_watch_start(void);

#endif
