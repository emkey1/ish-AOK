#include <string.h>
#include "kernel/calls.h"
#include "kernel/fs.h"
#include "kernel/inotify.h"
#include "fs/path.h"
#include "fs/poll.h"
#include "kernel/anonfd_ckpt.h"

#define IN_CLOEXEC_ O_CLOEXEC_
#define IN_NONBLOCK_ O_NONBLOCK_
#define IN_ACCESS_ 0x00000001
#define IN_MODIFY_ 0x00000002
#define IN_ATTRIB_ 0x00000004
#define IN_CLOSE_WRITE_ 0x00000008
#define IN_CLOSE_NOWRITE_ 0x00000010
#define IN_OPEN_ 0x00000020
#define IN_MOVED_FROM_ 0x00000040
#define IN_MOVED_TO_ 0x00000080
#define IN_CREATE_ 0x00000100
#define IN_DELETE_ 0x00000200
#define IN_DELETE_SELF_ 0x00000400
#define IN_MOVE_SELF_ 0x00000800
#define IN_IGNORED_ 0x00008000
#define IN_Q_OVERFLOW_ 0x00004000
#define IN_ONESHOT_ 0x80000000
#define IN_MASK_ADD_ 0x20000000
// fs.inotify.max_queued_events. An unread inotify fd used to grow the heap
// without any limit at all -- a watched directory under churn and a reader
// that stalls is enough, and nothing ever told the reader it had missed
// anything. Linux caps the queue and appends one synthetic overflow event
// meaning "rescan, I stopped keeping track".
// Declared in kernel/inotify.h so /proc/sys/fs/inotify reports this exact
// number rather than a second copy of it.
#define IN_ISDIR_ 0x40000000

// What a watch is on: the file at `path` on the mount whose ID is `mnt`. See
// inotify_resolve.
struct inotify_watch {
    int_t wd;
    uint_t mask;
    int mnt;
    char *path;
    struct list list;
};

struct inotify_event_ {
    int_t wd;
    dword_t mask;
    dword_t cookie;
    dword_t len;
};

struct inotify_event_node {
    struct inotify_event_ event;
    char *name;
    struct list list;
};

struct inotify_state {
    struct fd *fd;
    int_t next_wd;
    struct list watches;
    struct list events;
    struct list all;
    // How many events are queued, and whether the overflow marker has already
    // been appended -- one per overflow episode, as Linux does.
    unsigned queued;
    bool overflowed;
};

static struct fd_ops inotify_fdops;
static struct list inotify_instances = LIST_INITIALIZER(inotify_instances);
static lock_t inotify_instances_lock = LOCK_INITIALIZER;
// Shadows the list's emptiness so inotify_has_instances() -- which now gates
// the read, write AND close paths, i.e. most syscalls a busy guest makes --
// costs an atomic load instead of a mutex round trip. Only ever written under
// inotify_instances_lock, so it cannot disagree with the list.
static _Atomic unsigned inotify_instance_count = 0;

static struct inotify_state *inotify_state_get(struct fd *fd) {
    return fd->data;
}

static int inotify_lookup_fd(fd_t fd_no, struct fd **fd_out) {
    struct fd *fd = f_get(fd_no);
    if (fd == NULL)
        return _EBADF;
    if (fd->ops != &inotify_fdops)
        return _EINVAL;
    *fd_out = fd;
    return 0;
}

// Linux keeps a watch on the inode, so every name a file has reaches the same
// watches. A bind mount gives a directory another name: after `mount --bind
// /tmp/src /tmp/dst`, /tmp/dst/x IS /tmp/src/x. Watches used to be kept by the
// path string they were added with, and each event carried the one name it
// was made through, so a file made through /tmp/dst reached no watch on
// /tmp/src, and the other way round.
//
// So a watch, and every event, is keyed by what its path resolves to: the
// mount whose storage holds the file -- a bind resolves to the mount it shows
// (find_mount_and_trim_path) -- and the path on that mount. That is as near to
// the inode as a path gets here, and it behaves like one where names and
// files part company, as checked against Linux 6.12:
//   - a second add through another name finds the same watch and returns its
//     wd, so one instance watching both names is told each event once;
//   - a watch placed through a bind stays on the source after the umount;
//   - a watch on a directory that a mount then covers keeps watching the
//     covered directory, which nothing reaches any more.
// Rewriting an event onto every other name for the directory instead would
// have got the last two wrong. Hard links are still separate files here: a
// path does not say which names share an inode.
//
// A mount's ID is never reused, so a watch on a mount that is gone matches
// nothing again. `path` is a MAX_PATH buffer; takes mounts_lock.
static bool inotify_resolve(const char *guest_path, int *mnt, char *path) {
    if (strlen(guest_path) >= MAX_PATH || !path_is_normalized(guest_path))
        return false;
    strcpy(path, guest_path);
    struct mount *mount = find_mount_and_trim_path(path);
    if (mount == NULL)
        return false;
    *mnt = mount->id;
    mount_release(mount);
    // generic_getpath spells the root "/", which is "" on its mount.
    if (strcmp(path, "/") == 0)
        path[0] = '\0';
    return true;
}

static struct inotify_watch *inotify_find_watch(struct inotify_state *state, int mnt, const char *path) {
    struct inotify_watch *watch;
    list_for_each_entry(&state->watches, watch, list) {
        if (watch->mnt == mnt && strcmp(watch->path, path) == 0)
            return watch;
    }
    return NULL;
}

static struct inotify_watch *inotify_find_watch_by_wd(struct inotify_state *state, int_t wd) {
    struct inotify_watch *watch;
    list_for_each_entry(&state->watches, watch, list) {
        if (watch->wd == wd)
            return watch;
    }
    return NULL;
}

// Every event's name field is padded to a multiple of sizeof(struct
// inotify_event) -- 16 -- not to 4. Linux's roundup keeps each record
// 16-byte aligned so a reader can walk the buffer by casting each record in
// place, which is exactly how the documented read loop is written. Padding to
// 4 left records at addresses the struct is not aligned for, and made every
// event a different size from what a reader computing lengths for itself
// would predict.
static dword_t inotify_name_len(const char *name) {
    if (name == NULL || *name == '\0')
        return 0;
    dword_t len = strlen(name) + 1;
    dword_t align = sizeof(struct inotify_event_);
    return (len + align - 1) & ~(align - 1);
}

// The directory holding the file at `path` on its mount, and its name there.
// False for the mount's root (""), which Linux does not report to a parent
// either: a filesystem's root has none. A bind's root is not one of these --
// it resolves to the source directory, whose parent is told, as on Linux.
// The root of the root mount used to come out as "/" here, where a watch on
// / is "", so a watch on / never heard of anything in it.
static bool inotify_parent_and_name(const char *path, char *parent, const char **name_out) {
    const char *slash = strrchr(path, '/');
    if (slash == NULL)
        return false;
    size_t parent_len = slash - path;
    memcpy(parent, path, parent_len);
    parent[parent_len] = '\0';
    *name_out = slash + 1;
    return true;
}

static int inotify_queue_event_locked(struct fd *fd, int_t wd, dword_t mask, dword_t cookie, const char *name) {
    struct inotify_state *state = inotify_state_get(fd);

    // At the cap, drop this event and append IN_Q_OVERFLOW exactly once --
    // Linux's behaviour, and the only honest thing to do: the reader cannot be
    // told what it missed, only that it missed something and should rescan.
    // The marker carries wd = -1 and no name.
    if (state->queued >= INOTIFY_MAX_QUEUED_EVENTS) {
        if (state->overflowed)
            return 0;
        state->overflowed = true;
        wd = -1;
        mask = IN_Q_OVERFLOW_;
        cookie = 0;
        name = NULL;
    }

    struct inotify_event_node *event = malloc(sizeof(struct inotify_event_node));
    if (event == NULL)
        return _ENOMEM;
    event->name = NULL;
    if (name != NULL && *name != '\0') {
        event->name = strdup(name);
        if (event->name == NULL) {
            free(event);
            return _ENOMEM;
        }
    }
    event->event = (struct inotify_event_) {
        .wd = wd,
        .mask = mask,
        .cookie = cookie,
        .len = inotify_name_len(name),
    };
    list_add_tail(&state->events, &event->list);
    state->queued++;
    notify(&fd->cond);
    return 0;
}

// Deliver one event, and retire the watch if it was IN_ONESHOT.
//
// IN_ONESHOT means "tell me once, then forget me". It was ignored entirely, so
// the watch kept firing forever and never sent the IN_IGNORED that tells a
// reader the wd is dead -- a program that installed a one-shot watch and moved
// on kept receiving events for a wd it believed was gone.
static bool inotify_deliver_locked(struct inotify_state *state, struct inotify_watch *watch,
        dword_t mask, dword_t cookie, const char *name) {
    int_t wd = watch->wd;
    bool oneshot = (watch->mask & IN_ONESHOT_) != 0;
    if (oneshot) {
        list_remove(&watch->list);
        free(watch->path);
        free(watch);
    }
    bool ok = inotify_queue_event_locked(state->fd, wd, mask, cookie, name) == 0;
    if (oneshot)
        // IN_IGNORED is delivered whether or not the watch asked for it.
        inotify_queue_event_locked(state->fd, wd, IN_IGNORED_, 0, NULL);
    return ok;
}

static bool inotify_notify_exact_locked(struct inotify_state *state, int mnt, const char *path,
        dword_t mask, dword_t cookie) {
    struct inotify_watch *watch = inotify_find_watch(state, mnt, path);
    if (watch == NULL)
        return false;
    if ((watch->mask & mask) == 0)
        return false;
    return inotify_deliver_locked(state, watch, mask, cookie, NULL);
}

static bool inotify_notify_parent_locked(struct inotify_state *state, int mnt, const char *path,
        dword_t mask, dword_t cookie) {
    char parent[MAX_PATH];
    const char *name;
    if (!inotify_parent_and_name(path, parent, &name))
        return false;
    struct inotify_watch *watch = inotify_find_watch(state, mnt, parent);
    if (watch == NULL)
        return false;
    if ((watch->mask & mask) == 0)
        return false;
    return inotify_deliver_locked(state, watch, mask, cookie, name);
}

static struct fd **inotify_snapshot_instances(size_t *count_out) {
    struct fd **fds = NULL;
    size_t count = 0;

    lock(&inotify_instances_lock, 0);
    struct inotify_state *state;
    list_for_each_entry(&inotify_instances, state, all) {
        count++;
    }

    if (count != 0) {
        fds = malloc(sizeof(struct fd *) * count);
        if (fds != NULL) {
            size_t i = 0;
            list_for_each_entry(&inotify_instances, state, all) {
                // state->fd may be mid-teardown in a concurrent fd_close
                // (its refcount already hit 0, ops->close/free not yet
                // run) -- inotify_instances_lock alone doesn't rule that
                // out, since the generic refcount decrement in fd_close
                // knows nothing about this lock. A plain fd_retain here
                // would resurrect a dying fd; skip it instead.
                struct fd *retained = fd_retain_if_live(state->fd);
                if (retained != NULL)
                    fds[i++] = retained;
            }
            count = i;
        }
    }
    unlock(&inotify_instances_lock);

    *count_out = count;
    return fds;
}

static void inotify_for_each_instance(bool (*cb)(struct inotify_state *, void *), void *ctx) {
    size_t count = 0;
    struct fd **fds = inotify_snapshot_instances(&count);
    if (count != 0 && fds == NULL)
        return;

    for (size_t i = 0; i < count; i++) {
        struct fd *fd = fds[i];
        bool wake = false;
        lock(&fd->lock, 0);
        struct inotify_state *state = inotify_state_get(fd);
        if (state != NULL)
            wake = cb(state, ctx);
        unlock(&fd->lock);
        if (wake)
            poll_wakeup(fd, POLL_READ);
        fd_close(fd);
    }
    free(fds);
}

// An event on the file at `path` on the mount whose ID is `mnt`.
struct inotify_path_event {
    int mnt;
    const char *path;
    dword_t mask;
};

static bool inotify_emit_exact_cb(struct inotify_state *state, void *ctx) {
    struct inotify_path_event *event = ctx;
    return inotify_notify_exact_locked(state, event->mnt, event->path, event->mask, 0);
}

static bool inotify_emit_parent_cb(struct inotify_state *state, void *ctx) {
    struct inotify_path_event *event = ctx;
    return inotify_notify_parent_locked(state, event->mnt, event->path, event->mask, 0);
}

// Linux delivers IN_OPEN/IN_MODIFY/IN_ATTRIB both to a watch on the file
// itself (no name) and to a watch on its parent directory (name filled in).
// Emitting only the exact form meant a directory watch never saw child
// modify/attrib/open activity -- tail -F style watchers and anything
// watching a spool/config directory for content changes missed everything.
static bool inotify_emit_exact_and_parent_cb(struct inotify_state *state, void *ctx) {
    struct inotify_path_event *event = ctx;
    bool wake = inotify_notify_exact_locked(state, event->mnt, event->path, event->mask, 0);
    wake |= inotify_notify_parent_locked(state, event->mnt, event->path, event->mask, 0);
    return wake;
}

// A rename never crosses mounts (EXDEV), so both names are on `mnt`.
struct inotify_move_event {
    int mnt;
    const char *old_path;
    const char *new_path;
    dword_t old_mask;
    dword_t new_mask;
    dword_t self_mask;
    dword_t cookie;
};

// Watches are keyed by path on a mount here, where Linux keys them by inode.
// Renaming the watched file itself is handled below by rewriting its path -- but a
// rename of an ANCESTOR directory moves the inode just as surely, and left
// every watch underneath naming a path that no longer exists, silently deaf
// from then on. Editors and build tools rename directories routinely.
//
// So carry the subtree: rewrite the prefix of every watch living under the
// renamed directory. No event is emitted for them, matching Linux, where those
// inodes have not changed -- only the path by which they are reached has.
static void inotify_move_subtree_locked(struct inotify_state *state, int mnt,
        const char *old_path, const char *new_path) {
    size_t old_len = strlen(old_path);
    if (old_len == 0)
        return;
    struct inotify_watch *watch;
    list_for_each_entry(&state->watches, watch, list) {
        if (watch->path == NULL || watch->mnt != mnt)
            continue;
        if (strncmp(watch->path, old_path, old_len) != 0 || watch->path[old_len] != '/')
            continue;
        char rebuilt[MAX_PATH];
        int n = snprintf(rebuilt, sizeof(rebuilt), "%s%s", new_path, watch->path + old_len);
        if (n < 0 || (size_t) n >= sizeof(rebuilt))
            continue;   // would truncate: leave it rather than corrupt it
        char *copy = strdup(rebuilt);
        if (copy == NULL)
            continue;
        free(watch->path);
        watch->path = copy;
    }
}

static bool inotify_emit_move_cb(struct inotify_state *state, void *ctx) {
    struct inotify_move_event *event = ctx;
    bool wake = false;
    wake |= inotify_notify_parent_locked(state, event->mnt, event->old_path, event->old_mask,
            event->cookie);
    wake |= inotify_notify_parent_locked(state, event->mnt, event->new_path, event->new_mask,
            event->cookie);
    inotify_move_subtree_locked(state, event->mnt, event->old_path, event->new_path);
    struct inotify_watch *watch = inotify_find_watch(state, event->mnt, event->old_path);
    if (watch == NULL)
        return wake;
    // The watch follows the file whether or not it asked to be told: it was
    // left on the old name unless it wanted IN_MOVE_SELF, deaf from then on.
    char *new_path = strdup(event->new_path);
    if (new_path == NULL)
        return wake;
    free(watch->path);
    watch->path = new_path;
    if ((watch->mask & event->self_mask) == 0)
        return wake;
    // Cookie 0: the cookie exists to pair IN_MOVED_FROM with IN_MOVED_TO, and
    // inotify(7) gives IN_MOVE_SELF none. Verified against Linux 6.12, which
    // reports 0 here where AOK was passing the rename's cookie through.
    wake |= inotify_queue_event_locked(state->fd, watch->wd, event->self_mask, 0, NULL) == 0;
    return wake;
}

int_t sys_inotify_init1(int_t flags) {
    STRACE("inotify_init1(%#x)", flags);
    if (flags & ~(IN_CLOEXEC_ | IN_NONBLOCK_))
        return _EINVAL;

    struct inotify_state *state = malloc(sizeof(struct inotify_state));
    if (state == NULL)
        return _ENOMEM;
    *state = (struct inotify_state) {};
    state->next_wd = 1;
    list_init(&state->watches);
    list_init(&state->events);
    list_init(&state->all);

    struct fd *fd = adhoc_fd_create(&inotify_fdops);
    if (fd == NULL) {
        free(state);
        return _ENOMEM;
    }
    state->fd = fd;
    fd->data = state;
    lock(&inotify_instances_lock, 0);
    list_add_tail(&inotify_instances, &state->all);
    inotify_instance_count++;
    unlock(&inotify_instances_lock);
    return f_install(fd, flags);
}

int_t sys_inotify_init(void) {
    return sys_inotify_init1(0);
}

int_t sys_inotify_add_watch(fd_t fd_no, addr_t pathname_addr, uint_t mask) {
    return sys_inotify_add_watch_guest(fd_no, pathname_addr, mask);
}

int_t sys_inotify_add_watch_guest(fd_t fd_no, guest_addr_t pathname_addr, uint_t mask) {
    char path_raw[MAX_PATH];
    int path_err = user_read_path(pathname_addr, path_raw, sizeof(path_raw));
    if (path_err)
        return path_err;
    STRACE("inotify_add_watch(%d, \"%s\", %#x)", fd_no, path_raw, mask);

    char path[MAX_PATH];
    int err = path_normalize(AT_PWD, path_raw, path, N_SYMLINK_FOLLOW);
    if (err < 0)
        return err;

    struct statbuf stat;
    // `path` is already fully normalized (chroot prefix included), so it
    // must NOT go back through a chroot-aware resolver: generic_statat
    // re-normalizes, which double-applied the chroot prefix and failed
    // every inotify_add_watch inside a chroot with ENOENT. Stat through the
    // mount table directly instead. (This also inherently handles the ""
    // spelling of the filesystem root that path_normalize produces --
    // watching / is legal and is the first thing sd-bus's watch_bind does.)
    // That leaves `path` as the path on the mount holding the file, which,
    // with the mount's ID, is what the watch is kept by; see inotify_resolve.
    int mnt;
    {
        struct mount *mount = find_mount_and_trim_path(path);
        if (mount == NULL)
            return _ENOENT;
        err = mount->fs->stat(mount, path, &stat);
        mnt = mount->id;
        mount_release(mount);
        if (err < 0)
            return err;
    }

    // Watching an inode requires the same read permission open(2) needs --
    // Linux's inotify_find_inode() does path_permission(MAY_READ). Without it
    // an unprivileged process installed a watch on a directory it could not
    // open and harvested the filenames inside from the events, which carry the
    // child name. The stat above already has the mode/uid/gid.
    err = access_check(&stat, AC_R);
    if (err < 0)
        return err;

    struct fd *fd;
    err = inotify_lookup_fd(fd_no, &fd);
    if (err < 0)
        return err;

    lock(&fd->lock, 0);
    struct inotify_state *state = inotify_state_get(fd);
    if (state == NULL) {
        unlock(&fd->lock);
        return _EBADF;
    }
    // Found through any name of the file, bind or source: Linux returns the
    // same wd for a second add of one inode, so an instance watching both
    // names is told each event once.
    struct inotify_watch *watch = inotify_find_watch(state, mnt, path);
    if (watch != NULL) {
        // IN_MASK_ADD ORs into the existing mask rather than replacing it,
        // which is the whole reason the flag exists -- it was ignored, so the
        // second add silently discarded whatever the first one was watching
        // for. Without the flag, replacing is correct.
        if (mask & IN_MASK_ADD_)
            watch->mask |= mask & ~IN_MASK_ADD_;
        else
            watch->mask = mask;
        err = watch->wd;
        unlock(&fd->lock);
        return err;
    }

    watch = malloc(sizeof(struct inotify_watch));
    if (watch == NULL) {
        unlock(&fd->lock);
        return _ENOMEM;
    }
    watch->path = strdup(path);
    if (watch->path == NULL) {
        free(watch);
        unlock(&fd->lock);
        return _ENOMEM;
    }
    watch->wd = state->next_wd++;
    watch->mask = mask;
    watch->mnt = mnt;
    list_add_tail(&state->watches, &watch->list);
    err = watch->wd;
    unlock(&fd->lock);
    return err;
}

int_t sys_inotify_rm_watch(fd_t fd_no, int_t wd) {
    STRACE("inotify_rm_watch(%d, %d)", fd_no, wd);
    struct fd *fd;
    int err = inotify_lookup_fd(fd_no, &fd);
    if (err < 0)
        return err;

    lock(&fd->lock, 0);
    struct inotify_state *state = inotify_state_get(fd);
    if (state == NULL) {
        unlock(&fd->lock);
        return _EBADF;
    }
    struct inotify_watch *watch = inotify_find_watch_by_wd(state, wd);
    if (watch == NULL) {
        unlock(&fd->lock);
        return _EINVAL;
    }

    list_remove(&watch->list);
    err = inotify_queue_event_locked(fd, wd, IN_IGNORED_, 0, NULL);
    unlock(&fd->lock);
    free(watch->path);
    free(watch);
    poll_wakeup(fd, POLL_READ);
    return err < 0 ? err : 0;
}

static ssize_t inotify_read(struct fd *fd, void *buf, size_t bufsize) {
    if (bufsize < sizeof(struct inotify_event_))
        return _EINVAL;

    lock(&fd->lock, 0);
    struct inotify_state *state = inotify_state_get(fd);
    if (state == NULL) {
        unlock(&fd->lock);
        return _EBADF;
    }
    while (list_empty(&state->events)) {
        if (fd->flags & O_NONBLOCK_) {
            unlock(&fd->lock);
            return _EAGAIN;
        }
        if (wait_for_blocked(&fd->cond, &fd->lock, NULL)) {
            unlock(&fd->lock);
            return _EINTR;
        }
        // wait_for drops fd->lock while blocked; another thread may have
        // closed this fd (freeing state) in the meantime.
        state = inotify_state_get(fd);
        if (state == NULL) {
            unlock(&fd->lock);
            return _EBADF;
        }
    }

    size_t written = 0;
    while (!list_empty(&state->events) && bufsize - written >= sizeof(struct inotify_event_)) {
        struct inotify_event_node *event =
            list_first_entry(&state->events, struct inotify_event_node, list);
        size_t event_size = sizeof(event->event) + event->event.len;
        if (bufsize - written < event_size)
            break;
        memcpy((char *) buf + written, &event->event, sizeof(event->event));
        written += sizeof(event->event);
        if (event->event.len != 0) {
            memset((char *) buf + written, 0, event->event.len);
            memcpy((char *) buf + written, event->name, strlen(event->name) + 1);
            written += event->event.len;
        }
        list_remove(&event->list);
        if (state->queued > 0)
            state->queued--;
        free(event->name);
        free(event);
    }
    // Draining the queue ends the overflow episode: the reader has been told
    // to rescan, so the next overflow is a new one and gets its own marker.
    if (list_empty(&state->events))
        state->overflowed = false;
    unlock(&fd->lock);
    return written;
}

static int inotify_poll(struct fd *fd) {
    lock(&fd->lock, 0);
    struct inotify_state *state = inotify_state_get(fd);
    int types = (state != NULL && !list_empty(&state->events)) ? POLL_READ : 0;
    unlock(&fd->lock);
    return types;
}

static ssize_t inotify_ioctl_size(int cmd) {
    if (cmd == FIONREAD_)
        return sizeof(int_t);
    return -1;
}

// FIONREAD: the bytes a read with a big enough buffer would return right now,
// i.e. every queued record plus its padded name -- the IN_Q_OVERFLOW marker
// included, since it is a record too. Linux's inotify_ioctl() walks its queue
// the same way.
//
// There was no ioctl at all, so this was ENOTTY, and GLib treats that as fatal.
// Its inotify backend reads into a 4096-byte buffer and asks FIONREAD whenever
// a read comes back nearly full; the failure is a g_error(), which killed the
// monitoring program or left its "gmain" thread spinning at 100% CPU (Thunar,
// about 8 s after every launch). Any GLib program watching a directory reached
// it on the first burst of changes big enough to fill that buffer.
static int inotify_ioctl(struct fd *fd, int cmd, void *arg) {
    if (cmd != FIONREAD_)
        return _ENOTTY;
    lock(&fd->lock, 0);
    struct inotify_state *state = inotify_state_get(fd);
    if (state == NULL) {
        unlock(&fd->lock);
        return _EBADF;
    }
    // Bounded: the queue holds at most INOTIFY_MAX_QUEUED_EVENTS + 1 records
    // of at most 16 + 256 bytes each, a few MB, so an int cannot overflow.
    size_t bytes = 0;
    struct inotify_event_node *event;
    list_for_each_entry(&state->events, event, list) {
        bytes += sizeof(event->event) + event->event.len;
    }
    unlock(&fd->lock);
    *(int_t *) arg = (int_t) bytes;
    return 0;
}

static int inotify_close(struct fd *fd) {
    lock(&fd->lock, 0);
    struct inotify_state *state = inotify_state_get(fd);
    if (state == NULL) {
        unlock(&fd->lock);
        return 0;
    }
    fd->data = NULL;

    lock(&inotify_instances_lock, 0);
    // list_remove_safe is a no-op on an already-unlinked node, so only
    // decrement when this call is the one that unlinked it.
    if (!list_null(&state->all)) {
        list_remove(&state->all);
        inotify_instance_count--;
    }
    unlock(&inotify_instances_lock);

    struct inotify_watch *watch, *watch_tmp;
    list_for_each_entry_safe(&state->watches, watch, watch_tmp, list) {
        list_remove(&watch->list);
        free(watch->path);
        free(watch);
    }
    struct inotify_event_node *event, *event_tmp;
    list_for_each_entry_safe(&state->events, event, event_tmp, list) {
        list_remove(&event->list);
        free(event->name);
        free(event);
    }
    state->queued = 0;
    free(state);
    unlock(&fd->lock);
    return 0;
}

static struct fd_ops inotify_fdops = {
    .name = "inotify",
    .anon_inode_class = "inotify",
    .read = inotify_read,
    .poll = inotify_poll,
    .ioctl_size = inotify_ioctl_size,
    .ioctl = inotify_ioctl,
    .close = inotify_close,
};

bool inotify_has_instances(void) {
    return atomic_load_explicit(&inotify_instance_count, memory_order_relaxed) != 0;
}

// Resolve an event's path for delivery (inotify_resolve), into `buf`. Only
// when someone could be told: most callers do not check, and this is a walk
// of the mount table under mounts_lock.
static bool inotify_event_path(const char *path, int *mnt, char *buf) {
    if (path == NULL || path[0] != '/' || !inotify_has_instances())
        return false;
    return inotify_resolve(path, mnt, buf);
}

static void inotify_emit(const char *path, dword_t mask,
        bool (*cb)(struct inotify_state *, void *)) {
    char buf[MAX_PATH];
    struct inotify_path_event event = {.path = buf, .mask = mask};
    if (inotify_event_path(path, &event.mnt, buf))
        inotify_for_each_instance(cb, &event);
}

void inotify_notify_open(const char *path) {
    inotify_emit(path, IN_OPEN_, inotify_emit_exact_and_parent_cb);
}

void inotify_notify_modify(const char *path) {
    inotify_emit(path, IN_MODIFY_, inotify_emit_exact_and_parent_cb);
}

void inotify_notify_attrib(const char *path) {
    inotify_emit(path, IN_ATTRIB_, inotify_emit_exact_and_parent_cb);
}

void inotify_notify_create(const char *path, bool is_dir) {
    inotify_emit(path, IN_CREATE_ | (is_dir ? IN_ISDIR_ : 0), inotify_emit_parent_cb);
}

// A watch on a file that has just been deleted is dead: Linux reports
// IN_ATTRIB for the link-count change, then IN_DELETE_SELF, then retires the
// watch with IN_IGNORED. Watchers use IN_IGNORED to know the wd is gone and
// stop tracking it; without it they hold a stale wd forever and never re-arm
// on a recreated file, which is the usual "editor saved the file and my
// watcher went deaf" shape.
static bool inotify_emit_ignored_cb(struct inotify_state *state, void *ctx) {
    struct inotify_path_event *event = ctx;
    struct inotify_watch *watch = inotify_find_watch(state, event->mnt, event->path);
    if (watch == NULL)
        return false;
    int_t wd = watch->wd;
    list_remove(&watch->list);
    free(watch->path);
    free(watch);
    // IN_IGNORED is delivered whether or not the watch asked for it.
    return inotify_queue_event_locked(state->fd, wd, IN_IGNORED_, 0, NULL) == 0;
}

void inotify_notify_delete(const char *path, bool is_dir) {
    char buf[MAX_PATH];
    int mnt;
    if (!inotify_event_path(path, &mnt, buf))
        return;
    struct inotify_path_event parent = {
        .mnt = mnt,
        .path = buf,
        .mask = IN_DELETE_ | (is_dir ? IN_ISDIR_ : 0),
    };
    struct inotify_path_event attrib = {.mnt = mnt, .path = buf, .mask = IN_ATTRIB_};
    struct inotify_path_event exact = {.mnt = mnt, .path = buf, .mask = IN_DELETE_SELF_};
    struct inotify_path_event ignored = {.mnt = mnt, .path = buf, .mask = IN_IGNORED_};
    inotify_for_each_instance(inotify_emit_parent_cb, &parent);
    inotify_for_each_instance(inotify_emit_exact_cb, &attrib);
    inotify_for_each_instance(inotify_emit_exact_cb, &exact);
    inotify_for_each_instance(inotify_emit_ignored_cb, &ignored);
}

// close(2) on a descriptor that was open for writing is IN_CLOSE_WRITE, and
// IN_CLOSE_NOWRITE otherwise. This is the single most-used inotify event
// there is -- `inotifywait -e close_write`, entr, and every build watcher key
// on it to mean "the writer finished, the file is now consistent" -- and AOK
// emitted neither, so those tools simply never fired.
void inotify_notify_close(const char *path, bool was_writable) {
    inotify_emit(path, was_writable ? IN_CLOSE_WRITE_ : IN_CLOSE_NOWRITE_,
            inotify_emit_exact_and_parent_cb);
}

void inotify_notify_access(const char *path) {
    inotify_emit(path, IN_ACCESS_, inotify_emit_exact_and_parent_cb);
}

void inotify_notify_move(const char *old_path, const char *new_path, bool is_dir) {
    char old_buf[MAX_PATH], new_buf[MAX_PATH];
    int old_mnt, new_mnt;
    if (new_path == NULL || new_path[0] != '/' ||
            !inotify_event_path(old_path, &old_mnt, old_buf) ||
            !inotify_resolve(new_path, &new_mnt, new_buf) || old_mnt != new_mnt)
        return;
    static _Atomic dword_t next_cookie = 1;
    struct inotify_move_event event = {
        .mnt = old_mnt,
        .old_path = old_buf,
        .new_path = new_buf,
        .old_mask = IN_MOVED_FROM_ | (is_dir ? IN_ISDIR_ : 0),
        .new_mask = IN_MOVED_TO_ | (is_dir ? IN_ISDIR_ : 0),
        .self_mask = IN_MOVE_SELF_,
        .cookie = next_cookie++,
    };
    inotify_for_each_instance(inotify_emit_move_cb, &event);
}

// A small append-only buffer and its reader, for the checkpoint blob.
struct ckpt_blob { char *buf; size_t len, cap; bool failed; };
static void ckpt_blob_put(struct ckpt_blob *b, const void *p, size_t n) {
    if (b->failed || n == 0)
        return;
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 256;
        while (cap < b->len + n)
            cap *= 2;
        char *nb = realloc(b->buf, cap);
        if (nb == NULL) { b->failed = true; return; }
        b->buf = nb; b->cap = cap;
    }
    memcpy(b->buf + b->len, p, n);
    b->len += n;
}
struct ckpt_blob_rd { const char *p; size_t left; bool bad; };
static void ckpt_blob_get(struct ckpt_blob_rd *r, void *out, size_t n) {
    if (r->bad || n > r->left) { r->bad = true; memset(out, 0, n); return; }
    memcpy(out, r->p, n);
    r->p += n; r->left -= n;
}

// ---- checkpoint (kernel/anonfd_ckpt.h) ------------------------------------
//
//   int32 next_wd, uint32 overflowed, uint32 n_watches, uint32 n_events
//   n_watches x { int32 wd, uint32 mask, uint32 path_len, path }
//   n_events  x { int32 wd, uint32 mask, uint32 cookie, uint32 name_len, name }

bool inotify_fd_is(struct fd *fd) {
    return fd != NULL && fd->ops == &inotify_fdops;
}

char *inotify_ckpt_describe(struct fd *fd, size_t *len) {
    struct ckpt_blob b = {0};
    lock(&fd->lock, 0);
    struct inotify_state *state = inotify_state_get(fd);
    if (state == NULL) {
        unlock(&fd->lock);
        return NULL;
    }
    // A watch is saved as a guest path, since mount IDs are this boot's: its
    // path on its mount, under that mount's point. For a watch placed through
    // a bind that is the source's path, which suits a restore that brings
    // back tmpfs mounts but not binds. A watch whose mount is gone watches
    // nothing and is left out. mount_path_by_id takes mounts_lock under this
    // instance's lock, which is safe because no inotify lock is ever taken
    // under mounts_lock: an event resolves its path before visiting anyone.
    uint32_t nw = 0, ne = 0, kept = 0;
    struct inotify_watch *w;
    list_for_each_entry(&state->watches, w, list)
        nw++;
    char **paths = calloc(nw != 0 ? nw : 1, sizeof(*paths));
    if (paths == NULL) {
        unlock(&fd->lock);
        return NULL;
    }
    uint32_t i = 0;
    list_for_each_entry(&state->watches, w, list) {
        char guest[MAX_PATH];
        strcpy(guest, w->path);
        if (mount_path_by_id(w->mnt, guest)) {
            paths[i] = strdup(guest);
            if (paths[i] == NULL)
                b.failed = true;
            else
                kept++;
        }
        i++;
    }
    struct inotify_event_node *ev;
    list_for_each_entry(&state->events, ev, list)
        ne++;
    int32_t next_wd = state->next_wd;
    uint32_t overflowed = state->overflowed ? 1 : 0;
    ckpt_blob_put(&b, &next_wd, sizeof(next_wd));
    ckpt_blob_put(&b, &overflowed, sizeof(overflowed));
    ckpt_blob_put(&b, &kept, sizeof(kept));
    ckpt_blob_put(&b, &ne, sizeof(ne));
    i = 0;
    list_for_each_entry(&state->watches, w, list) {
        char *path = paths[i++];
        if (path == NULL)
            continue;
        int32_t wd = w->wd;
        uint32_t mask = w->mask, plen = (uint32_t) strlen(path);
        ckpt_blob_put(&b, &wd, sizeof(wd));
        ckpt_blob_put(&b, &mask, sizeof(mask));
        ckpt_blob_put(&b, &plen, sizeof(plen));
        ckpt_blob_put(&b, path, plen);
        free(path);
    }
    free(paths);
    list_for_each_entry(&state->events, ev, list) {
        int32_t wd = ev->event.wd;
        uint32_t mask = ev->event.mask, cookie = ev->event.cookie;
        uint32_t nlen = ev->name != NULL ? (uint32_t) strlen(ev->name) : 0;
        ckpt_blob_put(&b, &wd, sizeof(wd));
        ckpt_blob_put(&b, &mask, sizeof(mask));
        ckpt_blob_put(&b, &cookie, sizeof(cookie));
        ckpt_blob_put(&b, &nlen, sizeof(nlen));
        ckpt_blob_put(&b, ev->name, nlen);
    }
    unlock(&fd->lock);
    if (b.failed) {
        free(b.buf);
        return NULL;
    }
    *len = b.len;
    return b.buf;
}

struct fd *inotify_ckpt_new(const char *blob, size_t len) {
    struct ckpt_blob_rd r = {.p = blob, .left = len};
    int32_t next_wd;
    uint32_t overflowed, nw, ne;
    ckpt_blob_get(&r, &next_wd, sizeof(next_wd));
    ckpt_blob_get(&r, &overflowed, sizeof(overflowed));
    ckpt_blob_get(&r, &nw, sizeof(nw));
    ckpt_blob_get(&r, &ne, sizeof(ne));
    if (r.bad)
        return ERR_PTR(_EINVAL);

    struct inotify_state *state = malloc(sizeof(struct inotify_state));
    if (state == NULL)
        return ERR_PTR(_ENOMEM);
    *state = (struct inotify_state) {};
    state->next_wd = next_wd > 0 ? next_wd : 1;
    list_init(&state->watches);
    list_init(&state->events);
    list_init(&state->all);
    struct fd *fd = adhoc_fd_create(&inotify_fdops);
    if (fd == NULL) {
        free(state);
        return ERR_PTR(_ENOMEM);
    }
    state->fd = fd;
    fd->data = state;
    lock(&inotify_instances_lock, 0);
    list_add_tail(&inotify_instances, &state->all);
    inotify_instance_count++;
    unlock(&inotify_instances_lock);

    // The watches keep their numbers: a program keeps a map from watch
    // descriptor to what it asked to watch, and a watch that came back under
    // another number would deliver its events to the wrong entry.
    lock(&fd->lock, 0);
    for (uint32_t i = 0; i < nw && !r.bad; i++) {
        int32_t wd;
        uint32_t mask, plen;
        ckpt_blob_get(&r, &wd, sizeof(wd));
        ckpt_blob_get(&r, &mask, sizeof(mask));
        ckpt_blob_get(&r, &plen, sizeof(plen));
        if (r.bad || plen > r.left || plen >= MAX_PATH) {
            r.bad = true;
            break;
        }
        struct inotify_watch *w = malloc(sizeof(*w));
        char *path = malloc(plen + 1);
        if (w == NULL || path == NULL) {
            free(w);
            free(path);
            r.bad = true;
            break;
        }
        ckpt_blob_get(&r, path, plen);
        path[plen] = '\0';
        // Kept by what the path resolves to now, as when it was added. One
        // that resolves to nothing keeps its wd and matches no event: no path
        // resolves to mount ID 0.
        char key[MAX_PATH];
        int mnt = 0;
        if (!r.bad && inotify_resolve(path, &mnt, key)) {
            char *resolved = strdup(key);
            if (resolved != NULL) {
                free(path);
                path = resolved;
            } else {
                mnt = 0;
            }
        }
        w->wd = wd;
        w->mask = mask;
        w->mnt = mnt;
        w->path = path;
        list_add_tail(&state->watches, &w->list);
    }
    // Events already queued are still owed to the reader.
    for (uint32_t i = 0; i < ne && !r.bad; i++) {
        int32_t wd;
        uint32_t mask, cookie, nlen;
        ckpt_blob_get(&r, &wd, sizeof(wd));
        ckpt_blob_get(&r, &mask, sizeof(mask));
        ckpt_blob_get(&r, &cookie, sizeof(cookie));
        ckpt_blob_get(&r, &nlen, sizeof(nlen));
        if (r.bad || nlen > r.left || nlen >= MAX_PATH) {
            r.bad = true;
            break;
        }
        char name[MAX_PATH];
        ckpt_blob_get(&r, name, nlen);
        name[nlen] = '\0';
        inotify_queue_event_locked(fd, wd, mask, cookie, nlen != 0 ? name : NULL);
    }
    state->overflowed = overflowed != 0;
    unlock(&fd->lock);
    if (r.bad) {
        fd_close(fd);
        return ERR_PTR(_EINVAL);
    }
    return fd;
}
