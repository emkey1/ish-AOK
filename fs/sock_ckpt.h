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
};

// Fill *out from a socket that is frozen. Returns 0, or a negative errno if
// this descriptor is not a socket at all.
int sock_ckpt_describe(struct fd *sock, struct sock_ckpt_desc *out);

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
