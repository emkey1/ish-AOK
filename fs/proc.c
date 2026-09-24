#include <string.h>
#include <sys/stat.h>
#include <pthread.h>
#include "kernel/calls.h"
#include "kernel/fs.h"
#include "fs/proc.h"
#include "fs/path.h"
#include "fs/poll.h"

static pthread_once_t proc_tree_once = PTHREAD_ONCE_INIT;

static void proc_init_tree_once(void) {
    proc_root_init();
}

static int proc_mount(struct mount *UNUSED(mount)) {
    pthread_once(&proc_tree_once, proc_init_tree_once);
    return 0;
}

static void proc_prepare_child_entry(struct proc_entry *parent, unsigned long index, struct proc_entry *child) {
    child->index = index;
    child->parent = parent->meta;
}

// /proc/<n> for any live task, thread or process, as Linux's proc_pid_lookup
// resolves it. The /proc listing names processes only (fs/proc/root.c), so a
// thread's directory can't be found by walking it. Zombies resolve too, as
// they are listed. Kernel threads have no task, and are left to the walk.
static bool proc_lookup_task_dir(const char *component, struct proc_entry *next_entry) {
    if (component[0] < '1' || component[0] > '9')
        return false;
    unsigned long id = 0;
    for (const char *c = component; *c != '\0'; c++) {
        if (*c < '0' || *c > '9' || id > MAX_PID)
            return false;
        id = id * 10 + (unsigned long) (*c - '0');
    }
    if (id > MAX_PID)
        return false;
    complex_lockt(&pids_lock, 0);
    bool exists = pid_get_task_zombie((dword_t) id) != NULL;
    unlock(&pids_lock);
    if (!exists)
        return false;
    *next_entry = (struct proc_entry) {&proc_pid, .pid = (pid_t_) id};
    return true;
}

bool proc_entry_may_read(struct proc_entry *entry) {
    struct task *task = pid_get_task_ref(entry->pid);
    if (task == NULL)
        return true;
    bool ok = task_ptrace_may_access(task, PTRACE_MODE_READ_ | PTRACE_MODE_FSCREDS_);
    task_ref_cnt_mod(task, -1);
    return ok;
}

static int proc_lookup(const char *path, struct proc_entry *entry) {
    entry->meta = &proc_root;
    char component[MAX_NAME + 1];
    int err = 0;
    while (path_next_component(&path, component, &err)) {
        if (!S_ISDIR(proc_entry_mode(entry))) {
            err = _ENOTDIR;
            break;
        }
        // Nothing inside /proc/<pid>/fd or fdinfo is anyone else's to look
        // up: Linux's proc_fd_permission refuses the directory itself, so
        // stat, readlink and open of /proc/<pid>/fd/N all fail EACCES.
        if (entry->meta->ptrace_read && !proc_entry_may_read(entry)) {
            err = _EACCES;
            break;
        }

        unsigned long index = 0;
        struct proc_entry next_entry = {0};
        char entry_name[MAX_NAME];
        if (entry->meta == &proc_root && proc_lookup_task_dir(component, &next_entry)) {
            proc_prepare_child_entry(entry, 0, &next_entry);
            goto found;
        }
        while (proc_dir_read(entry, &index, &next_entry)) {
            proc_prepare_child_entry(entry, index, &next_entry);

            proc_entry_getname(&next_entry, entry_name);
            if (strcmp(entry_name, component) == 0)
                goto found;
            proc_entry_cleanup(&next_entry);
        }
        err = _ENOENT;
        break;
found:
        proc_entry_cleanup(entry);
        *entry = next_entry;
    }
    if (err < 0)
        proc_entry_cleanup(entry);
    return err;
}

extern const struct fd_ops procfs_fdops;

// Open mountinfo fds, so mount-table changes can wake their pollers.
// libmount's kernel mount monitor (what systemd uses to notice every
// mount/umount) registers /proc/self/mountinfo in epoll with
// EPOLLIN|EPOLLET and expects the kernel to generate a fresh edge each
// time the mount table changes -- it never reads the fd, so without a
// poll_wakeup per change, systemd's drain_libmount() sees no events and
// skips its mountinfo rescan entirely ("Mount process finished, but
// there is no mount", failing every mount unit).
static struct list mountinfo_fds = LIST_INITIALIZER(mountinfo_fds);
static lock_t mountinfo_fds_lock = LOCK_INITIALIZER;

static struct fd *proc_open(struct mount *UNUSED(mount), const char *path, int UNUSED(flags), int UNUSED(mode)) {
    struct proc_entry entry = {0};
    int err = proc_lookup(path, &entry);
    if (err < 0)
        return ERR_PTR(err);
    if (entry.meta->ptrace_read && !proc_entry_may_read(&entry)) {
        proc_entry_cleanup(&entry);
        return ERR_PTR(_EACCES);
    }
    struct fd *fd = fd_create(&procfs_fdops);
    fd->proc.entry = entry;
    fd->proc.data.data = NULL;
    // Covers both /proc/mountinfo (fs/proc/root.c) and
    // /proc/<pid>/mountinfo (fs/proc/pid.c): distinct proc_dir_entry
    // structs sharing the same show callback.
    if (entry.meta->show == proc_show_mountinfo) {
        lock(&mountinfo_fds_lock, 0);
        list_add(&mountinfo_fds, &fd->proc.mountinfo_link);
        unlock(&mountinfo_fds_lock);
    }
    return fd;
}

void proc_mountinfo_notify_changed(void) {
    lock(&mountinfo_fds_lock, 0);
    struct fd *fd;
    list_for_each_entry(&mountinfo_fds, fd, proc.mountinfo_link)
        // Re-arms the edge for EPOLLET registrations (fs/poll.c clears the
        // fd's triggered_types) and wakes any blocked poll/epoll_wait.
        // proc_poll takes no locks, satisfying poll_wakeup's contract.
        poll_wakeup(fd, POLL_READ);
    unlock(&mountinfo_fds_lock);
}

static int proc_getpath(struct fd *fd, char *buf) {
    char *p = buf + MAX_PATH - 1;
    size_t n = 0;
    p[0] = '\0';
    struct proc_entry entry = fd->proc.entry;
    while (entry.meta != &proc_root) {
        if (entry.meta == NULL)
            return _ENOENT;
        char component[MAX_NAME];
        proc_entry_getname(&entry, component);
        size_t component_len = strlen(component) + 1; // plus one for the slash
        if ((size_t) (p - buf) < component_len)
            return _ENAMETOOLONG;
        p -= component_len;
        n += component_len;
        *p = '/';
        memcpy(p + 1, component, component_len - 1);
        entry.meta = entry.parent;
        entry.parent = entry.meta != NULL ? entry.meta->parent : NULL;
    }
    memmove(buf, p, n + 1); // plus one for the null
    return 0;
}

static int proc_stat(struct mount *UNUSED(mount), const char *path, struct statbuf *stat) {
    struct proc_entry entry = {0};
    extern time_t boot_time;
    int err = proc_lookup(path, &entry);
    if (err < 0)
        return err;
    int ret = proc_entry_stat(&entry, stat);
    stat->atime = (dword_t)time(NULL);
    stat->mtime = (dword_t)boot_time;
    stat->ctime = (dword_t)boot_time;
    proc_entry_cleanup(&entry);
    return ret;
}

static int proc_fstat(struct fd *fd, struct statbuf *stat) {
    return proc_entry_stat(&fd->proc.entry, stat);
}

static int proc_check_regular(struct fd *fd) {
    mode_t_ mode = proc_entry_mode(&fd->proc.entry);
    if (S_ISDIR(mode))
        return _EISDIR;
    assert(S_ISREG(mode));
    return 0;
}

static int proc_refresh_data(struct fd *fd) {
    int err = proc_check_regular(fd);
    if (err < 0)
        return err;

    struct proc_entry *entry = &fd->proc.entry;
    // pread/pwrite-backed entries (e.g. /proc/<pid>/mem) have no buffered
    // show() model: reads go straight through ->pread and there is nothing to
    // refresh. Calling through the NULL ->show pointer here crashed the
    // emulator -- a read() with no ->read op falls back to ->pread and then
    // advances the offset via ->lseek (proc_seek -> proc_refresh_data), which
    // stress-ng --procfs triggers by reading /proc/<pid>/mem.
    if (entry->meta->show == NULL)
        return 0;

    if (fd->proc.data.data == NULL) {
        fd->proc.data.capacity = 4096;
        fd->proc.data.data = malloc(fd->proc.data.capacity); // default size
    }
    fd->proc.data.size = 0;
    fd->proc.data_valid = false;
    err = entry->meta->show(entry, &fd->proc.data);
    if (err < 0)
        return err;
    fd->proc.data_valid = true;
    return 0;
}

// Linux renders a procfs file ONCE per read pass and serves the rest of the
// pass out of that one buffer: single_open's seq_file fills seq_file::buf at
// the read that starts at offset 0, and every later read of the pass copies
// from it (fs/seq_file.c, seq_read_iter). AOK used to re-run show() on every
// read(2) AND every lseek(2), so a reader taking a /proc file in small pieces
// spliced together as many different renderings as it made calls. That is
// invisible while the renderings have the same shape, and corrupts the file
// the moment a field earlier in it changes WIDTH: the tail shifts under the
// reader, and the byte at the offset it asks for next is the one it already
// has (a duplicate) or the one after (a loss). Observed in a Devuan guest,
// where dash's `read` builtin reads one byte at a time:
//
//     while read -r k v; do [ "$k" = btime ] && echo "BTIME=$v"; done </proc/stat
//
// printed BTIME=17990071649 for a btime of 1790071649 -- one duplicated
// digit, because a counter printed above btime grew a digit between two of
// those reads.
//
// So hold the rendering for the pass that is reading it, and re-render
// exactly where Linux re-renders, which is what keeps a poller that re-reads
// the same fd seeing new data:
//
//   * a read that starts at offset 0. seq_read_iter resets index and count
//     whenever ki_pos == 0, so the usual lseek(0)+read refresh loop -- and a
//     pread at 0 -- always gets a fresh rendering.
//   * a read that does not continue where the previous one stopped: after an
//     lseek, or a pread at an unrelated offset. seq_read_iter calls
//     traverse() for any position other than the one it left off at, and
//     traverse() re-runs show() from the start of the file.
//
// Anything else is the same pass still reading the same file, and gets the
// bytes that belong to the snapshot it started.
static int proc_data_for_read(struct fd *fd, off_t off) {
    int err = proc_check_regular(fd);
    if (err < 0)
        return err;
    if (fd->proc.data_valid && off != 0 && off == fd->proc.data_pos)
        return 0;
    return proc_refresh_data(fd);
}

static off_t_ proc_seek(struct fd *fd, off_t_ off, int whence) {
    int err = proc_check_regular(fd);
    if (err < 0)
        return err;

    // Only LSEEK_END needs the file's length, and a procfs file has none
    // until it has been rendered. Linux answers EINVAL for it outright
    // (seq_lseek handles SEEK_SET and SEEK_CUR only); AOK has always answered
    // it from the rendered size, and guests size buffers with it, so keep
    // answering -- just don't drag a render into the seeks that need no
    // length. The LSEEK_CUR that sys_read_buf issues after every read(2) is
    // one of those, and re-rendering there was half of the splicing above.
    if (whence == LSEEK_END && !fd->proc.data_valid) {
        err = proc_refresh_data(fd);
        if (err < 0)
            return err;
    }

    err = generic_seek(fd, off, whence, fd->proc.data.size);
    if (err < 0)
        return err;

    return fd->offset;
}

static ssize_t proc_pread(struct fd *fd, void *buf, size_t bufsize, off_t off) {
    if (fd->proc.entry.meta->pread) {
        struct proc_data data = {buf, bufsize, bufsize};
        return fd->proc.entry.meta->pread(&fd->proc.entry, &data, off, fd->flags);
    }
    
    // seq_read_iter returns before it even takes the seq_file's lock when
    // there is nothing to copy into, so an empty read renders nothing and
    // moves nothing. Rendering here would throw away the pass's snapshot for
    // a call that cannot consume a byte of it.
    if (bufsize == 0)
        return 0;

    int err = proc_data_for_read(fd, off);
    if (err < 0)
        return err;

    const char *data = fd->proc.data.data;
    assert(data != NULL);

    size_t remaining = 0;
    if (off >= 0 && (size_t) off <= fd->proc.data.size)
        remaining = fd->proc.data.size - (size_t) off;
    size_t n = bufsize;
    if (n > remaining)
        n = remaining;

    if (n > 0)
        memcpy(buf, data + off, n);
    // Where the next read has to start to be served from this same rendering.
    fd->proc.data_pos = off + (off_t) n;
    return n;
}

static void proc_buf_write(struct proc_data *buf, const void *data, size_t size, size_t off) {
    assert(off <= buf->size);
    size_t tidemark = off + size;
    if (tidemark > buf->capacity) {
        size_t new_capacity = buf->capacity + 1;
        while (tidemark > new_capacity)
            new_capacity *= 2;
        char *new_data = realloc(buf->data, new_capacity);
        if (new_data == NULL) {
            // just give up, error reporting is a pain
            return;
        }
        buf->data = new_data;
        buf->capacity = new_capacity;
    }
    assert(tidemark <= buf->capacity);
    memcpy(buf->data + off, data, size);
    if (tidemark > buf->size) {
        buf->size = tidemark;
    }
}

static ssize_t proc_pwrite(struct fd *fd, const void *buf, size_t bufsize, off_t off) {
    mode_t_ mode = proc_entry_mode(&fd->proc.entry);
    if (S_ISDIR(mode))
        return _EISDIR;
    assert(S_ISREG(mode));
    
    if (fd->proc.entry.meta->pwrite) {
        struct proc_data data = {(char *) buf, bufsize, bufsize};
        return fd->proc.entry.meta->pwrite(&fd->proc.entry, &data, off);
    }
    
    if (!fd->proc.entry.meta->update) {
        return _EPERM;
    }

    // A zero-length write is write(2)'s business, not the file's: it writes
    // nothing and returns 0, and Linux's proc_sys_call_handler short-circuits
    // before the handler is ever asked. Passing it through made every sysctl
    // answer EINVAL for `write(fd, buf, 0)`, which is not an error anywhere.
    if (bufsize == 0)
        return 0;

    struct proc_data data = {(char *)buf, bufsize, bufsize};
    int err = fd->proc.entry.meta->update(&fd->proc.entry, &data);
    if (err < 0)
        return err;

    // The write changed what this file says, so whatever rendering a read
    // pass on this same fd was holding is now history. Drop it rather than
    // let the next read continue out of it: an O_RDWR sysctl fd that writes
    // and reads back is the whole reason both ops exist on one description.
    fd->proc.data_valid = false;
    return bufsize;
}

static int proc_readdir(struct fd *fd, struct dir_entry *entry) {
    struct proc_entry proc_entry = {0};
    bool any_left = proc_dir_read(&fd->proc.entry, &fd->offset, &proc_entry);
    if (!any_left)
        return 0;
    proc_prepare_child_entry(&fd->proc.entry, fd->offset, &proc_entry);
    proc_entry_getname(&proc_entry, entry->name);
    entry->inode = proc_entry_inode(&proc_entry);
    entry->type = dir_entry_type_for_mode(proc_entry_mode(&proc_entry));
    proc_entry_cleanup(&proc_entry);
    return 1;
}

static int proc_close(struct fd *fd) {
    if (!list_null(&fd->proc.mountinfo_link)) {
        lock(&mountinfo_fds_lock, 0);
        list_remove(&fd->proc.mountinfo_link);
        unlock(&mountinfo_fds_lock);
    }
    if (fd->proc.data.data != NULL)
        free(fd->proc.data.data);
    proc_entry_cleanup(&fd->proc.entry);
    return 0;
}

// Regular proc files are always readable, matching Linux (poll on a proc
// file reports POLLIN immediately). This level state is also what makes
// the mountinfo EPOLLIN|EPOLLET edge machinery work: the edge-triggered
// mask (fs/poll.c triggered_types) suppresses re-delivery after the first
// event, and proc_mountinfo_notify_changed's poll_wakeup re-arms it per
// mount-table change.
static int proc_poll(struct fd *fd) {
    if (fd->proc.entry.meta->poll != NULL)
        return fd->proc.entry.meta->poll(&fd->proc.entry, (off_t) fd->offset);
    return POLL_READ;
}

const struct fd_ops procfs_fdops = {
    .name = "procfs",
    .pread = proc_pread,
    .pwrite = proc_pwrite,
    .lseek = proc_seek,
    .readdir = proc_readdir,
    .poll = proc_poll,
    .close = proc_close,
};

static ssize_t proc_readlink(struct mount *UNUSED(mount), const char *path, char *buf, size_t bufsize) {
    struct proc_entry entry = {0};
    int err = proc_lookup(path, &entry);
    if (err < 0)
        return err;
    if (!S_ISLNK(proc_entry_mode(&entry))) {
        proc_entry_cleanup(&entry);
        return _EINVAL;
    }
    // cwd, root, exe and the ns links: Linux's proc_fd_access_allowed.
    if (entry.meta->ptrace_read && !proc_entry_may_read(&entry)) {
        proc_entry_cleanup(&entry);
        return _EACCES;
    }

    char target[MAX_PATH + 1];
    err = entry.meta->readlink(&entry, target);
    proc_entry_cleanup(&entry);
    if (err < 0)
        return err;

    size_t target_len = strlen(target);
    if (bufsize > target_len)
        bufsize = target_len;
    memcpy(buf, target, bufsize);
    return bufsize;
}

static int proc_unlink(struct mount *UNUSED(mount), const char *path) {
    struct proc_entry entry = {0};
    int err = proc_lookup(path, &entry);
    if (err < 0)
        return err;
    err = _EPERM;
    if (entry.meta->unlink)
        err = entry.meta->unlink(&entry);
    proc_entry_cleanup(&entry);
    return err;
}

void proc_buf_append(struct proc_data *buf, const void *data, size_t size) {
    proc_buf_write(buf, data, size, buf->size);
}

void proc_printf(struct proc_data *buf, const char *format, ...) {
    char data[4096];
    va_list args;
    va_start(args, format);
    int printed = vsnprintf(data, sizeof(data), format, args);
    va_end(args);
    // vsnprintf reports what it WOULD have written, not what it did, and the
    // result went straight to proc_buf_append as a length. Two ways that reads
    // off the end of this stack buffer and copies the result into a file the
    // guest then reads: output longer than the buffer (printed > sizeof data),
    // and an encoding error (printed < 0, which as a size_t is SIZE_MAX).
    // Clamp to what is actually in the buffer.
    if (printed < 0)
        return;
    size_t size = (size_t) printed;
    if (size >= sizeof(data))
        size = sizeof(data) - 1;
    proc_buf_append(buf, data, size);
}

const struct fs_ops procfs = {
    .name = "proc", .magic = 0x9fa0,
    .mount = proc_mount,
    .open = proc_open,
    .getpath = proc_getpath,
    .stat = proc_stat,
    .fstat = proc_fstat,
    .readlink = proc_readlink,
    .unlink = proc_unlink,
};
