#ifndef FD_H
#define FD_H
#include <dirent.h>
#include <sys/stat.h>
#include "emu/memory.h"
#include "util/list.h"
#include "util/ro_locks.h"
#include "util/sync.h"
#include "util/bits.h"
#include "fs/stat.h"
#include "fs/proc.h"
#include "fs/sockrestart.h"

// FIXME almost everything that uses the structs in this file does so without any kind of sane locking

struct fd {
    atomic_uint refcount;
    unsigned flags;
    mode_t_ type; // just the S_IFMT part, it can't change
    const struct fd_ops *ops;
    struct list poll_fds;
    lock_t poll_lock;
    unsigned long offset;
    // fcntl(F_SETOWN)/fcntl(F_GETOWN): the pid (positive) or process group
    // (negative, -pgid) that would receive SIGIO/SIGURG for this fd. 0 means
    // no owner. Stored generically here (not under the socket union) because
    // F_SETOWN/F_GETOWN are valid on any fd type on Linux, not just sockets.
    pid_t_ owner;

    // fd data
    union {
        // tty
        struct {
            struct tty *tty;
            // links together fds pointing to the same tty
            // locked by the tty
            struct list tty_other_fds;
            // tty->hangup_gen as it was when this descriptor was opened.
            // Differing from the tty's current value means a hangup happened
            // after this open, and only then does this fd see EIO.
            unsigned tty_hangup_gen;
        };
        struct {
            struct poll *poll;
        } epollfd;
        struct {
            uint64_t val;
            bool semaphore; // EFD_SEMAPHORE: read returns 1 and decrements by 1
        } eventfd;
        // fifo (named pipe): links fds open on the same FIFO inode. The shared
        // buffer (struct fifo) lives on the inode; locked by the fifo's fds_lock.
        struct list fifo_other_fds;
        struct {
            struct timer *timer;
            uint64_t expirations;
        } timerfd;
        // O_PATH|O_NOFOLLOW fd referring to a symlink itself (see
        // generic_openat). Owns one reference on the mount and a malloc'd
        // mount-relative path; fd->mount stays NULL so fd_close doesn't run
        // the filesystem's close on an fd it never opened.
        struct {
            struct mount *mount;
            char *path;
        } opath_link;
        struct {
            int domain;
            int type;
            int protocol;

            // These are only used as strong references, to keep the inode
            // alive while there is a listener.
            struct inode_data *unix_name_inode;
            struct unix_abstract *unix_name_abstract;
            uint8_t unix_name_len;
            char unix_name[108];
            struct fd *unix_peer; // locked by peer_lock, for simplicity
            cond_t unix_got_peer;
            bool unix_peer_pending;
            size_t unix_peer_off;
            char unix_peer_buf[sizeof(struct fd *)];
            // Queue of struct scm for sending file descriptors
            // locked by fd->lock
            struct list unix_scm;
            struct ucred_ {
                pid_t_ pid;
                uid_t_ uid;
                uid_t_ gid;
            } unix_cred;
            struct ucred_ unix_peer_cred;
            bool unix_peer_cred_valid;
            bool unix_passcred;
            bool unix_devlog_sink;
            bool unix_initctl_sink;
            bool reuseaddr;
            bool reuseport;
            // euid at bind() time. SO_REUSEPORT lets several sockets share a
            // port, and Linux requires every member of the group to have the
            // same effective uid -- otherwise any user could join a root
            // daemon's port and take its connections. Only meaningful once
            // the socket is bound.
            uid_t_ bind_euid;
            bool listening; // listen() called: SO_ACCEPTCONN (Darwin can't report it)
            // The host description is kept nonblocking whenever a guest call
            // that may block has run on it, so no guest task can ever wedge
            // unkillably inside a host recvmsg/sendmsg (fs/sock.c
            // socket_force_host_nonblock; the guest's own O_NONBLOCK lives in
            // fd->flags and is what sock_getflags reports). Cached to keep the
            // I/O fast path off fcntl; cleared by anything that writes the host
            // flags behind our back, which makes the next call re-force.
            bool host_nonblock;
            // SO_ERROR is read-and-clear at the host level: the first getsockopt()
            // to observe a nonzero value resets it to 0 for every later reader.
            // iSH's own internal readiness probes (see socket_tcp_connect_write_ready
            // in fs/sock.c, called from every poll/epoll scan of a connecting socket)
            // query it purely as a boolean and would otherwise silently steal the
            // one authoritative read a guest's own getsockopt(SOL_SOCKET, SO_ERROR)
            // needs after a nonblocking connect() -- observed making a refused
            // loopback connect() report success. Stashed here the first time an
            // internal probe observes it nonzero; consumed (and cleared) by the
            // guest-facing getsockopt(SO_ERROR) handler in preference to a
            // since-reset host value of 0.
            int host_connect_error;
            // Set once the ENOTCONN->ECONNRESET translation in fs/sock.c has
            // fired: iOS killed this connected socket when the device slept and
            // it is never coming back. Without it the translation re-delivered
            // the same error on every call while poll went on reporting the fd
            // readable -- chronyd at 106% of one core, 47496 failing recvmmsg
            // in 12 seconds. A dead connection reports itself once.
            bool conn_dead;
            dword_t ip_mtu_discover;
            dword_t ipv6_mtu_discover;
            dword_t ipv6_mtu;
            bool ip_recverr;
            bool ipv6_recverr;
            int ipv6_recverr_fd;
            bool icmp6_filter_valid;
            uint32_t icmp6_filter[8];
            dword_t tcp_defer_accept;
            char tcp_congestion[16];
            // Linux accepts these unconditionally and BSD has no equivalent
            // knob. Returning ENOPROTOOPT is a state real Linux never produces
            // -- a program tuning a connection sees an error where every Linux
            // gives success -- so the value is kept and reported back, the same
            // way tcp_defer_accept above already is. They are advisory
            // (retry/timeout/window hints), so a host stack that does not act
            // on them degrades gracefully; TCP_USER_TIMEOUT is the one with
            // real teeth, and a connection simply keeps Darwin's own timeout.
            dword_t tcp_syncnt;
            dword_t tcp_linger2;
            dword_t tcp_window_clamp;
            dword_t tcp_user_timeout;
            dword_t tcp_quickack;
            dword_t tcp_maxseg;
            dword_t tcp_fastopen;
            bool tcp_quickack_set;

            // SOL_SOCKET options Darwin has no knob for, kept so the get
            // reports what the set was given (see fs/sock.h).
            dword_t so_priority;
            dword_t so_mark;
            dword_t so_busy_poll;
            bool so_no_check;
            bool so_timestampns;
            // SO_BINDTODEVICE: the interface name this socket was bound to,
            // empty when it never was. Reported back by getsockopt, which is
            // what a caller checks after setting it.
            char so_bindtodevice[16];

            // A TCP bind() that has NOT been handed to the host yet. Linux
            // refuses connections to a bound-but-not-listening socket (RST);
            // Darwin silently drops the SYN, so the client hangs for ~8s
            // instead of getting ECONNREFUSED. Holding the port is what causes
            // that, so it is not held until listen() or connect() needs it.
            // The address is kept here so getsockname can still answer.
            bool bind_deferred;
            // Raw bytes rather than struct sockaddr_max_: fs/fd.h does not
            // include fs/sock.h, and this only ever travels back to bind().
            char deferred_addr[128];
            uint_t deferred_addr_len;

            // Guest-loopback NAT (fs/sock.c inet_nat_*): when a guest
            // bind() asks for a loopback endpoint the host can't provide
            // (a 127.x.y.z alias macOS doesn't have, or a privileged
            // port), the host socket is silently re-bound to
            // 127.0.0.1:<ephemeral> and these carry the guest-visible
            // address so getsockname/getpeername and datagram source
            // addresses keep telling the guest what it expects.
            // All stored in network byte order.
            bool inet_nat_bound;
            uint32_t inet_nat_bound_addr;
            uint16_t inet_nat_bound_port;
            bool inet_nat_peer;
            uint32_t inet_nat_peer_addr;
            uint16_t inet_nat_peer_port;

            uint32_t netlink_port_id;
            uint32_t netlink_groups;
            char *netlink_reply;
            size_t netlink_reply_len;
            size_t netlink_reply_off;
            // Guards the three netlink_reply* fields above (and the
            // notification-append path below) against a background
            // notifier thread racing the guest thread's own sendmsg/
            // recvmsg/poll on this fd -- see netlink_notify_link_change in
            // fs/sock.c. Nothing else in this file touched these fields
            // from more than one thread before that feature existed, so
            // there was previously no lock here at all.
            lock_t netlink_reply_lock;
            // Membership in the process-wide list of netlink sockets
            // subscribed to at least one multicast group (netlink_groups
            // != 0), maintained in fs/sock.c. Only valid while
            // netlink_notify_registered is true.
            struct list netlink_notify_link;
            bool netlink_notify_registered;
            bool netlink_cap_ack;
            bool netlink_ext_ack;
            bool netlink_get_strict_chk;
            // SO_RCVBUF/SO_SNDBUF on a fake (real_fd < 0) netlink socket: no
            // real fd to ask the host kernel, so track what setsockopt was
            // given (Linux-kernel-style doubled) and hand it back verbatim on
            // getsockopt. See sock_init_emulation_defaults for the default.
            dword_t netlink_rcvbuf;
            dword_t netlink_sndbuf;
        } socket;

        // See app/Pasteboard.m
        struct {
            // UIPasteboard.changeCount
            uint64_t generation;
            // Buffer for written data
            void* buffer;
            // its capacity
            size_t buffer_cap;
            // length of actual data stored in the buffer
            size_t buffer_len;
        } clipboard;

        // can fit anything in here
        void *data;
    };
    // fs data
    union {
        struct {
            struct proc_entry entry;
            unsigned dir_index;
            struct proc_data data;
            // Open /proc/.../mountinfo fds, linked into the global
            // mountinfo-watch list (fs/proc.c) so mount-table changes can
            // poll_wakeup them (libmount's kernel mount monitor -- systemd --
            // registers mountinfo in epoll with EPOLLIN|EPOLLET and relies on
            // a new edge per mount change). Null links for other proc fds.
            struct list mountinfo_link;
        } proc;
        struct {
            // Open /dev/kmsg fds, linked into the global kernel-log watch
            // list (fs/mem.c) so a newly logged line can poll_wakeup them --
            // `dmesg --follow` and systemd-journald both epoll this rather
            // than sitting in a blocking read.
            struct list link;
        } kmsg;
        struct {
            int num;
        } devpts;
        struct {
            struct tmp_dirent *dirent;
            struct tmp_dirent *dir_pos;
            // readdir phase: 0 = emit ".", 1 = emit "..", 2 = children (dir_pos)
            unsigned dots_pos;
        } tmpfs;
        void *fs_data;
    };

    // fs/inode data
    struct mount *mount;
    int real_fd; // seeks on this fd require the lock TODO think about making a special lock just for that
    bool realfs_fifo_had_data;
    DIR *dir;
    struct inode_data *inode;
    ino_t fake_inode;
    struct statbuf stat; // for adhoc fs
    struct fd_sockrestart sockrestart; // argh

    // these are used for a variety of things related to the fd
    lock_t lock;
    cond_t cond;
};

typedef sdword_t fd_t;
#define AT_FDCWD_ -100

struct fd *fd_create(const struct fd_ops *ops);
struct fd *fd_retain(struct fd *fd);
// Like fd_retain, but for promoting a non-owning pointer found via a
// secondary lookup structure (e.g. a global registry keyed off fd->data)
// whose own lock does NOT participate in fd_close's refcount-reaches-zero
// decision. Plain fd_retain would happily resurrect an fd whose refcount
// has already hit 0 in a concurrent fd_close, racing its ops->close/free.
// Returns NULL (no reference taken) if the fd is already past that point.
struct fd *fd_retain_if_live(struct fd *fd);
int fd_close(struct fd *fd);

int fd_getflags(struct fd *fd);
int fd_setflags(struct fd *fd, int flags);

#define NAME_MAX 255
struct dir_entry {
    qword_t inode;
    byte_t type;
    char name[NAME_MAX + 1];
};

static inline byte_t dir_entry_type_for_mode(mode_t_ mode) {
    switch (mode & S_IFMT) {
        case S_IFREG: return DT_REG;
        case S_IFDIR: return DT_DIR;
        case S_IFLNK: return DT_LNK;
        case S_IFCHR: return DT_CHR;
        case S_IFBLK: return DT_BLK;
        case S_IFIFO: return DT_FIFO;
        case S_IFSOCK: return DT_SOCK;
        default: return DT_UNKNOWN;
    }
}

#define LSEEK_SET 0
#define LSEEK_CUR 1
#define LSEEK_END 2

struct fd_ops {
    // required for files
    // TODO make optional for non-files
    ssize_t (*read)(struct fd *fd, void *buf, size_t bufsize);
    ssize_t (*write)(struct fd *fd, const void *buf, size_t bufsize);
    ssize_t (*pread)(struct fd *fd, void *buf, size_t bufsize, off_t off);
    ssize_t (*pwrite)(struct fd *fd, const void *buf, size_t bufsize, off_t off);
    off_t_ (*lseek)(struct fd *fd, off_t_ off, int whence);

    // Reads a directory entry from the stream
    // required for directories
    int (*readdir)(struct fd *fd, struct dir_entry *entry);
    // Called before a sequence of readdir calls
    void (*readdir_begin)(struct fd *fd);
    // Called after a sequence of readdir calls
    void (*readdir_end)(struct fd *fd);
    // Return an opaque value representing the current point in the directory stream
    // optional, fd->offset will be used instead
    unsigned long (*telldir)(struct fd *fd);
    // Seek to the location represented by a pointer returned from telldir
    // optional, fd->offset will be used instead
    void (*seekdir)(struct fd *fd, unsigned long ptr);

    // map the file
    int (*mmap)(struct fd *fd, struct mem *mem, page_t start, pages_t pages, off_t offset, int prot, int flags);

    // returns a bitmask of operations that won't block
    int (*poll)(struct fd *fd);

    // returns the size needed for the output of ioctl, 0 if the arg is not a
    // pointer, -1 for invalid command
    ssize_t (*ioctl_size)(int cmd);
    // if ioctl_size returns non-zero, arg must point to ioctl_size valid bytes
    int (*ioctl)(struct fd *fd, int cmd, void *arg);

    int (*fsync)(struct fd *fd);
    int (*close)(struct fd *fd);

    // handle F_GETFL, i.e. return open flags for this fd
    int (*getflags)(struct fd *fd);
    // handle F_SETFL, i.e. set O_NONBLOCK
    int (*setflags)(struct fd *fd, dword_t arg);

    // For adhoc fds shown in /proc/<pid>/fd as "anon_inode:[<class>]"
    // (eventfd, eventpoll, signalfd, timerfd, inotify). NULL when the type is
    // taken from stat.mode instead (sockets -> socket:, pipes -> pipe:).
    const char *anon_inode_class;
};

struct fdtable {
    atomic_uint refcount;
    unsigned size;
    struct fd **files;
    bits_t *cloexec;
    lock_t lock;
};

struct fdtable *fdtable_new(int size);
struct fdtable *fdtable_retain(struct fdtable *table);
void fdtable_release(struct fdtable *table);
struct fdtable *fdtable_copy(struct fdtable *table);
int fdtable_unshare_current(void);
void fdtable_free(struct fdtable *table);
void fdtable_do_cloexec(struct fdtable *table);
struct fd *fdtable_get(struct fdtable *table, fd_t f);

struct fd *f_get(fd_t f);
// f_get, but NULL for an O_PATH descriptor -- I/O on one is EBADF on Linux.
// See the definition in kernel/fs.c.
struct fd *f_get_io(fd_t f);
// The *at() base directory for `path`, honouring the rule that an ABSOLUTE
// path makes dirfd irrelevant -- see the definition in kernel/fs.c for why
// that matters and what it broke. Declared here rather than duplicated because
// it already WAS duplicated, in kernel/fs.c and fs/stat.c, and the fix landed
// in one of them: everything passed except fstatat and statx, which is where
// modern glibc actually goes.
struct fd *at_fd_for_path(fd_t f, const char *path);
struct fd *f_get_retain(fd_t f);
// steals a reference to the fd, gives it to the table on success and destroys it on error
// flags is checked for O_CLOEXEC and O_NONBLOCK
fd_t f_install(struct fd *fd, int flags);
int f_close(fd_t f);

#endif
