#ifndef FS_SOCK_CKPT_H
#define FS_SOCK_CKPT_H

// What a checkpoint records about a socket, and what it takes to build one
// again. Deliberately plain integers and bytes: this struct goes into the
// image verbatim, and kernel/checkpoint.c includes this header rather than
// fs/sock.h so that no part of the socket layer's system headers reaches it.
//
// The model is sockrestart's, not a photograph. A host socket cannot outlive
// the process that owned it -- and on iOS it does not even outlive a
// suspension, because the system tears connected sockets down while the app is
// frozen (see fs/sock.c's ENOTCONN->ECONNRESET translation). So what travels
// is enough to REBUILD, and a connection that cannot be rebuilt comes back
// hung up rather than costing the whole session.

#include <stdint.h>

struct fd;

#define SOCK_CKPT_ADDR_MAX 128

enum sock_ckpt_state {
    // Created, never bound and never connected: socket() again and that is all.
    SOCK_CKPT_FRESH = 0,
    // Bound to an address, not listening. Rebuilt by bind()ing it again.
    SOCK_CKPT_BOUND,
    // Listening, with the backlog listen() was given.
    SOCK_CKPT_LISTEN,
    // Netlink: AOK emulates these entirely (real_fd < 0), so there is no host
    // object to lose and the rebuild is exact.
    SOCK_CKPT_NETLINK,
    // Connected, or anything this version cannot rebuild. Comes back as a
    // descriptor whose peer is gone: reads give EOF, writes give EPIPE, and
    // poll reports POLLIN|POLLHUP so a select loop wakes, reads the EOF and
    // closes -- which is what programs already do when a peer disappears.
    SOCK_CKPT_HUNGUP,
    // An AF_LOCAL socket CONNECTED to another socket in the image -- a
    // socketpair, or either side of a connect/accept -- rebuilt as a connected
    // pair when both ends' records have been read. pair_cookie names the pair
    // (the same from either end) and pair_end which end this is. Hanging these
    // up was the rule for a peer outside the image, and it was wrong for one
    // inside: udevd's worker socketpair came back hung up, reported
    // EPOLLIN|EPOLLHUP for ever, and udevd spun on it. What each end had
    // queued to read travels ahead of its record (sock_ckpt_queued) and is
    // sent back into it from the other end (sock_ckpt_requeue).
    SOCK_CKPT_PAIR,
};

struct sock_ckpt_desc {
    uint32_t state;          // enum sock_ckpt_state
    uint32_t domain;         // GUEST numbers (AF_INET_ &c), not the host's
    uint32_t type;
    uint32_t protocol;
    uint32_t backlog;        // SOCK_CKPT_LISTEN
    uint32_t netlink_port_id;
    uint32_t netlink_groups;
    uint32_t nonblock;
    uint32_t addr_len;       // 0 when there is no address to put back
    // Two things, by domain, because they are never both needed:
    //
    //   AF_INET/AF_INET6 -- the bound address in HOST layout, as getsockname
    //     gave it (or as bind() was told, for a bind AOK has deferred). It
    //     only ever travels back to bind(), so it is never converted.
    //
    //   AF_UNIX -- the GUEST path the socket is bound to, exactly as the guest
    //     passed it to bind(), abstract names included (leading NUL, and
    //     addr_len is then the real length rather than a strlen). The host
    //     path is useless on the way back: it is an ishsock name allocated
    //     per run, so the rebuild replays the bind by guest path instead.
    uint8_t addr[SOCK_CKPT_ADDR_MAX];
    // SOCK_CKPT_PAIR only.
    uint64_t pair_cookie;
    uint32_t pair_end;
    // This end's own credentials and its peer's (SO_PEERCRED), which a pair
    // rebuilt during a restore would otherwise take from whoever rebuilt it.
    int32_t cred_pid;
    uint32_t cred_uid, cred_gid;
    int32_t peer_pid;
    uint32_t peer_uid, peer_gid;
    uint32_t peer_cred_valid;
};

// Fill *out from a socket that is frozen. Returns 0, or a negative errno if
// this descriptor is not a socket at all.
int sock_ckpt_describe(struct fd *sock, struct sock_ckpt_desc *out);
// SOCK_CKPT_PAIR: both ends of a connected pair, made from either end's
// description. Each end is then given its own name, credentials and flags by
// sock_ckpt_apply_pair_end as its own record is read.
int sock_ckpt_rebuild_pair(const struct sock_ckpt_desc *desc,
                           struct fd **end0, struct fd **end1);
void sock_ckpt_apply_pair_end(struct fd *sock, const struct sock_ckpt_desc *desc);
// SOCK_CKPT_PAIR: what is queued to be READ at this end, as a sequence of
// [uint32 length][bytes] messages -- one for a stream, one per datagram -- and
// on the far side, those messages put back by sending them from the peer end.
// The running original is left exactly as it was: a stream is peeked, and a
// datagram queue is drained and sent straight back from the peer.
char *sock_ckpt_queued(struct fd *sock, size_t *len);
int sock_ckpt_requeue(struct fd *from_peer, const char *blob, size_t len);

// Build the socket again. Returns a struct fd the caller owns and installs
// itself -- NOT installed in any descriptor table, because the restore puts it
// at a particular number in a particular process. NULL with *err set on
// failure; a socket that cannot be rebuilt comes back hung up rather than
// failing.
struct fd *sock_ckpt_rebuild(const struct sock_ckpt_desc *desc, int *err);

// Why the last sock_ckpt_rebuild could not put a bound or listening socket
// back as it was (it came back hung up instead), or NULL when it could.
// Cleared at the start of every rebuild.
const char *sock_ckpt_rebuild_failure(void);

const char *sock_ckpt_state_name(uint32_t state);

#endif
