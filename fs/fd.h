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

    // Who opened this description, as much of Linux's f_cred as AOK has
    // anything to compare. One caller asks: linkat(fd, "", ..., AT_EMPTY_PATH)
    // names an inode with no path for it, and Linux allows that only to a
    // caller holding CAP_DAC_READ_SEARCH or still running on the very cred
    // object the open ran on -- so that handing somebody a descriptor does not
    // also hand them the right to give its inode a name they can reach
    // afterwards. Linux compares that object by pointer, which AOK has no
    // equivalent of; a fork gets a fresh one, CLONE_THREAD shares one, and any
    // credential change replaces one. The thread group plus the credential
    // values reproduce all three -- measured on Linux 6.12: a sibling thread
    // is allowed, a forked child is ENOENT, a setresuid that changes nothing
    // stays allowed. An execve is the fourth, and the one that moves no value
    // at all: a process that opens a descriptor and then execs ITSELF keeps
    // its pid, its thread group and every uid, and Linux still answers ENOENT.
    // task->exec_gen is carried here for exactly that case.
    struct {
        bool known; // false for a descriptor created with no task running
        pid_t_ tgid;
        unsigned exec_gen;
        uid_t_ uid, gid, euid, egid, suid, sgid, fsuid, fsgid;
    } open_creds;

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
        // /proc/<pid>/ns/<type>. Only an index into fs/proc/pid.c's
        // proc_ns_types, because AOK has exactly one namespace of each type
        // and the fd's whole identity is which type it names. Read by the
        // nsfs ioctls (NS_GET_NSTYPE and friends).
        struct {
            unsigned type_index;
        } nsfs;
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
            // Cookie this socket's connect() put on the wire, while it is
            // still unconsumed. Non-zero means "connected, but no accept has
            // linked unix_peer yet", which is when SCM_RIGHTS parcels have to
            // wait with the cookie rather than on a peer that does not exist.
            // Locked by unix_token_lock.
            uint64_t unix_peer_cookie;
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
            // This socket was rebuilt by a checkpoint restore and has no peer
            // and no address -- the host object it stands for died with the
            // process that owned it. The descriptor underneath is a
            // socketpair whose other end is already closed, so reads, writes
            // and poll all behave the way a vanished peer behaves; this flag
            // is what stops getsockname/getpeername from reporting THAT
            // socket's AF_UNIX identity instead of the one the guest knows it
            // by. See fs/sock_ckpt.h.
            bool ckpt_hungup;
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
            // SO_INCOMING_CPU reports which CPU last received on this
            // socket. Linux answers -1 until one has, and AOK never steers by
            // CPU, so -1 is the honest and permanent answer -- a state real
            // Linux produces for every socket that has not received yet. A
            // value the guest sets is kept and reported back, as Linux does
            // (there it is a hint for SO_REUSEPORT group selection, which a
            // stack is free to ignore).
            dword_t so_incoming_cpu;
            // SO_PEEK_OFF, but only ever a value that asks for nothing: -1
            // (disabled) or 0 (peek from the front, which is what AOK does).
            // A positive offset is refused rather than stored -- see
            // sys_setsockopt_guest_abi.
            dword_t so_peek_off;
            // IP_RETOPTS: whether to attach received IP options to messages.
            // Darwin cannot deliver them, but the flag itself round-trips --
            // ping reads it back after setting it.
            bool ip_retopts;
            // SO_RCVBUF/SO_SNDBUF as LINUX reports them, once the guest has
            // set one. Linux stores twice what you ask for (the second half
            // is its own bookkeeping overhead) with a floor and a cap, and
            // getsockopt returns that doubled number -- so a program that
            // sets 8192 and reads back 8192 concludes its request was
            // silently truncated. Untouched until a set arrives, so the
            // default keeps coming from the host and stays whatever the host
            // stack chose.
            dword_t so_rcvbuf;
            dword_t so_sndbuf;
            bool so_rcvbuf_set;
            bool so_sndbuf_set;
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
            // Datagram boundaries inside netlink_reply: each entry is the END
            // offset of one datagram, and bytes past the last entry are a
            // datagram still being built. A netlink socket is a datagram
            // socket, so one recvmsg returns ONE of these -- see
            // netlink_reply_seal_locked in fs/sock.c for why a dump's
            // NLMSG_DONE has to arrive in a datagram of its own.
            size_t *netlink_reply_bounds;
            size_t netlink_reply_nbounds;
            size_t netlink_reply_bounds_cap;
            // Guards the three netlink_reply* fields above (and the
            // notification-append path below) against a background
            // notifier thread racing the guest thread's own sendmsg/
            // recvmsg/poll on this fd -- see netlink_notify_link_change in
            // fs/sock.c. Nothing else in this file touched these fields
            // from more than one thread before that feature existed, so
            // there was previously no lock here at all.
            lock_t netlink_reply_lock;
            // A blocking receive with nothing queued waits here, under
            // netlink_reply_lock; netlink_append_nlmsg notifies it.
            cond_t netlink_reply_cond;
            // SO_RCVTIMEO/SO_SNDTIMEO: no host fd to hold them. `set` with a
            // zero value is Linux's zero-jiffy timeout, what a NEGATIVE
            // timeval gives: never wait. Unset is no timeout at all. Only the
            // receive side is ever consulted -- a netlink send never waits --
            // but both read back as set.
            struct timespec netlink_rcvtimeo;
            struct timespec netlink_sndtimeo;
            bool netlink_rcvtimeo_set;
            bool netlink_sndtimeo_set;
            // Membership in the process-wide list of netlink sockets
            // subscribed to at least one multicast group (netlink_groups
            // != 0), maintained in fs/sock.c. Only valid while
            // netlink_notify_registered is true.
            struct list netlink_notify_link;
            bool netlink_notify_registered;
            // Set by TASKSTATS_CMD_ATTR_REGISTER_CPUMASK. Rides the same
            // registry as the notify subscribers above -- registration and
            // lifetime are already correct there, and a second list would be a
            // second chance to leak a dead fd.
            bool netlink_taskstats_listener;
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
            // The sequence number of the record at fd->offset. Every record
            // /dev/kmsg emits carries one, and the reader just counts up: it
            // is recovered from the byte position only at open and lseek,
            // where the position can jump (kernel/log.c, ish_log_line_seek).
            uint64_t seq;
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
        struct {
            // The node, packed by fs/proc/root.c's sysfs_encode_node.
            void *node;
            // Read since it was opened. An attribute says "may have changed"
            // (POLLPRI|POLLERR) until then and not after, the way kernfs
            // compares the file's event count with the one its last read saw.
            bool read;
        } sysfs;
        void *fs_data;
    };

    // fs/inode data
    struct mount *mount;
    // The mount flags in force when this fd was opened. Not mount->flags: for
    // a bind, `mount` is the ORIGIN it aliases and carries the origin's flags,
    // while ro/nosuid/nodev/noexec belong to the bind. See
    // find_mount_and_trim_path_flags.
    int mount_flags;
    int real_fd; // seeks on this fd require the lock TODO think about making a special lock just for that
    bool realfs_fifo_had_data;
    // Whether the setuid/setgid strip on first write has already been done for
    // this descriptor. See file_remove_privs in kernel/fs.c.
    bool privs_checked;
    DIR *dir;
    struct inode_data *inode;
    ino_t fake_inode;
    struct statbuf stat; // for adhoc fs
    struct fd_sockrestart sockrestart; // argh

    // these are used for a variety of things related to the fd
    lock_t lock;
    cond_t cond;

    // Serializes getdents on this descriptor. A directory read is a
    // read-modify-write of the stream position -- tell, read, tell -- and two
    // threads sharing the fd interleaved it, so entries came back twice and
    // others were skipped entirely. Linux holds f_pos_lock across the whole
    // call for exactly this.
    //
    // Separate from `lock` above because tmpfs_readdir takes that one itself
    // to guard its own dir_pos, and this has to wrap the readdir call.
    lock_t dir_pos_lock;
};

typedef sdword_t fd_t;
#define AT_FDCWD_ -100

struct fd *fd_create(const struct fd_ops *ops);
// Is the caller still running on the credentials this descriptor was opened
// with? See open_creds above; linkat(AT_EMPTY_PATH) is the only caller.
bool fd_open_creds_match(struct fd *fd);
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
// name is sized for what a host directory can hold, not for the guest's
// NAME_MAX. APFS limits a name to 255 UTF-16 units, so a non-ASCII name runs
// to 765 bytes of UTF-8, and exFAT volumes are the same. Linux lists names
// like that: fs/readdir.c refuses only a name of PATH_MAX bytes or more, and
// FUSE allows 1024. 1024 is the size of Darwin's d_name, so realfs_readdir
// can pass every Darwin host name through whole.
#define DIR_ENTRY_NAME_SIZE 1024
struct dir_entry {
    qword_t inode;
    byte_t type;
    char name[DIR_ENTRY_NAME_SIZE];
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
// SEEK_DATA/SEEK_HOLE. A filesystem is always allowed to report that a file
// has no holes -- that is what a fully-allocated file looks like, and it is
// the answer the generic path gives: DATA is wherever you already are, and
// the only HOLE is the implicit one at EOF. Returning EINVAL instead told
// callers the file was not seekable that way at all, and tools that use them
// to copy sparsely (cp --sparse, tar, rsync, systemd-journald's compaction)
// fall back to a whole-file scan or fail outright.
#define LSEEK_DATA 3
#define LSEEK_HOLE 4

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
    // Fetch whatever ->mmap will need for [offset, offset+len) BEFORE the
    // address space is locked, and report any error that fetch produces.
    //
    // ->mmap itself runs under the address-space WRITE lock with the
    // process's other threads quiesced, so a filesystem whose bytes live
    // outside the kernel cannot go and get them there: it would freeze every
    // sibling thread for the duration, and deadlock outright when the thread
    // it is waiting on is one of them -- which is exactly the shape of a
    // program that mounts a FUSE filesystem and then maps a file on it.
    // Linux has no such problem because it faults pages in lazily; AOK maps
    // eagerly, so the fetch is hoisted out to here instead.
    //
    // Optional. NULL means ->mmap needs no preparation.
    int (*mmap_prepare)(struct fd *fd, off_t offset, size_t len);

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

    // Which family this is, for anything that has to reason about descriptors
    // by KIND rather than by behaviour. Set by every fd_ops in the tree.
    //
    // Added for the checkpoint inventory (/proc/ish/checkpoint), which has to
    // answer "what is actually open in a real session, and could it be brought
    // back" -- and the honest answer differs per family, from "re-open the path
    // and seek" to "cannot be restored, only rebuilt". Most fd_ops are static
    // to their own file, so a reader outside cannot compare pointers to
    // identify one; guessing from stat.mode conflates families that share a
    // mode. A name is the smallest thing that makes the question answerable.
    const char *name;

    // ->read and ->write make their own SA_RESTART decision: _ERESTART for an
    // interruption a handler may restart, _EINTR only for one it may not. The
    // read/write dispatchers (kernel/fs.c) then leave an _EINTR alone instead
    // of deciding again, which is all that makes a never-restarted wait
    // expressible through them.
    //
    // Set by sockets, where the answer depends on the wait: signal(7) never
    // restarts one with SO_RCVTIMEO/SO_SNDTIMEO armed, and only the op knows
    // whether the wait that was interrupted had a timeout.
    bool decides_restart;
};

struct fdtable {
    atomic_uint refcount;
    unsigned size;
    struct fd **files;
    bits_t *cloexec;
    lock_t lock;
};

// Re-record fd->open_creds from `current`. See fs/fd.c.
void fd_open_creds_stamp(struct fd *fd);

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
// Install at an exact number rather than the lowest free one, growing the
// table if needed. For kernel/checkpoint.c, where the number is part of what
// is being restored. Takes ownership of `fd` either way.
int fdtable_install_at(struct fdtable *table, fd_t f, struct fd *fd, bool cloexec);
int f_close(fd_t f);

// Write a FUSE file's shared mapping back to its daemon (fs/fuse.c). A no-op
// for every other kind of fd, so msync can call it for any file-backed
// mapping without knowing what is behind it.
int fuse_fd_msync_writeback(struct fd *fd);

#endif
