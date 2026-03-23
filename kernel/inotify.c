#include <string.h>
#include "kernel/calls.h"
#include "kernel/fs.h"
#include "kernel/inotify.h"
#include "fs/path.h"
#include "fs/poll.h"

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
#define IN_ISDIR_ 0x40000000

struct inotify_watch {
    int_t wd;
    uint_t mask;
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
};

static struct fd_ops inotify_fdops;
static struct list inotify_instances = LIST_INITIALIZER(inotify_instances);
static lock_t inotify_instances_lock = LOCK_INITIALIZER;

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

static struct inotify_watch *inotify_find_watch(struct inotify_state *state, const char *path) {
    struct inotify_watch *watch;
    list_for_each_entry(&state->watches, watch, list) {
        if (strcmp(watch->path, path) == 0)
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

static dword_t inotify_name_len(const char *name) {
    if (name == NULL || *name == '\0')
        return 0;
    dword_t len = strlen(name) + 1;
    return (len + sizeof(dword_t) - 1) & ~(sizeof(dword_t) - 1);
}

static void inotify_parent_and_name(const char *path, char *parent, const char **name_out) {
    const char *slash = strrchr(path, '/');
    if (slash == NULL) {
        strcpy(parent, "/");
        *name_out = path;
        return;
    }
    if (slash == path) {
        strcpy(parent, "/");
        *name_out = slash + 1;
        return;
    }
    size_t parent_len = slash - path;
    memcpy(parent, path, parent_len);
    parent[parent_len] = '\0';
    *name_out = slash + 1;
}

static int inotify_queue_event_locked(struct fd *fd, int_t wd, dword_t mask, dword_t cookie, const char *name) {
    struct inotify_state *state = inotify_state_get(fd);
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
    notify(&fd->cond);
    return 0;
}

static void inotify_notify_exact_locked(struct inotify_state *state, const char *path, dword_t mask, dword_t cookie) {
    struct inotify_watch *watch = inotify_find_watch(state, path);
    if (watch == NULL)
        return;
    if ((watch->mask & mask) == 0)
        return;
    if (inotify_queue_event_locked(state->fd, watch->wd, mask, cookie, NULL) == 0)
        poll_wakeup(state->fd, POLL_READ);
}

static void inotify_notify_parent_locked(struct inotify_state *state, const char *path, dword_t mask, dword_t cookie) {
    char parent[MAX_PATH];
    const char *name;
    inotify_parent_and_name(path, parent, &name);
    struct inotify_watch *watch = inotify_find_watch(state, parent);
    if (watch == NULL)
        return;
    if ((watch->mask & mask) == 0)
        return;
    if (inotify_queue_event_locked(state->fd, watch->wd, mask, cookie, name) == 0)
        poll_wakeup(state->fd, POLL_READ);
}

static void inotify_for_each_instance(void (*cb)(struct inotify_state *, void *), void *ctx) {
    lock(&inotify_instances_lock, 0);
    struct inotify_state *state;
    list_for_each_entry(&inotify_instances, state, all) {
        lock(&state->fd->lock, 0);
        cb(state, ctx);
        unlock(&state->fd->lock);
    }
    unlock(&inotify_instances_lock);
}

struct inotify_path_event {
    const char *path;
    dword_t mask;
};

static void inotify_emit_exact_cb(struct inotify_state *state, void *ctx) {
    struct inotify_path_event *event = ctx;
    inotify_notify_exact_locked(state, event->path, event->mask, 0);
}

static void inotify_emit_parent_cb(struct inotify_state *state, void *ctx) {
    struct inotify_path_event *event = ctx;
    inotify_notify_parent_locked(state, event->path, event->mask, 0);
}

struct inotify_move_event {
    const char *old_path;
    const char *new_path;
    dword_t old_mask;
    dword_t new_mask;
    dword_t self_mask;
    dword_t cookie;
};

static void inotify_emit_move_cb(struct inotify_state *state, void *ctx) {
    struct inotify_move_event *event = ctx;
    inotify_notify_parent_locked(state, event->old_path, event->old_mask, event->cookie);
    inotify_notify_parent_locked(state, event->new_path, event->new_mask, event->cookie);
    struct inotify_watch *watch = inotify_find_watch(state, event->old_path);
    if (watch == NULL)
        return;
    if ((watch->mask & event->self_mask) == 0)
        return;
    free(watch->path);
    watch->path = strdup(event->new_path);
    if (watch->path == NULL)
        return;
    if (inotify_queue_event_locked(state->fd, watch->wd, event->self_mask, event->cookie, NULL) == 0)
        poll_wakeup(state->fd, POLL_READ);
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
    unlock(&inotify_instances_lock);
    return f_install(fd, flags);
}

int_t sys_inotify_init(void) {
    return sys_inotify_init1(0);
}

int_t sys_inotify_add_watch(fd_t fd_no, addr_t pathname_addr, uint_t mask) {
    char path_raw[MAX_PATH];
    if (user_read_string(pathname_addr, path_raw, sizeof(path_raw)))
        return _EFAULT;
    STRACE("inotify_add_watch(%d, \"%s\", %#x)", fd_no, path_raw, mask);

    char path[MAX_PATH];
    int err = path_normalize(AT_PWD, path_raw, path, N_SYMLINK_FOLLOW);
    if (err < 0)
        return err;

    struct statbuf stat;
    err = generic_statat(AT_PWD, path, &stat, 0);
    if (err < 0)
        return err;

    struct fd *fd;
    err = inotify_lookup_fd(fd_no, &fd);
    if (err < 0)
        return err;

    struct inotify_state *state = inotify_state_get(fd);
    lock(&fd->lock, 0);
    struct inotify_watch *watch = inotify_find_watch(state, path);
    if (watch != NULL) {
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

    struct inotify_state *state = inotify_state_get(fd);
    lock(&fd->lock, 0);
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

    struct inotify_state *state = inotify_state_get(fd);
    lock(&fd->lock, 0);
    while (list_empty(&state->events)) {
        if (fd->flags & O_NONBLOCK_) {
            unlock(&fd->lock);
            return _EAGAIN;
        }
        if (wait_for(&fd->cond, &fd->lock, NULL)) {
            unlock(&fd->lock);
            return _EINTR;
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
        free(event->name);
        free(event);
    }
    unlock(&fd->lock);
    return written;
}

static int inotify_poll(struct fd *fd) {
    struct inotify_state *state = inotify_state_get(fd);
    lock(&fd->lock, 0);
    int types = list_empty(&state->events) ? 0 : POLL_READ;
    unlock(&fd->lock);
    return types;
}

static int inotify_close(struct fd *fd) {
    struct inotify_state *state = inotify_state_get(fd);
    lock(&inotify_instances_lock, 0);
    list_remove(&state->all);
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
    free(state);
    fd->data = NULL;
    return 0;
}

static struct fd_ops inotify_fdops = {
    .read = inotify_read,
    .poll = inotify_poll,
    .close = inotify_close,
};

void inotify_notify_open(const char *path) {
    if (path == NULL || path[0] != '/')
        return;
    struct inotify_path_event event = {.path = path, .mask = IN_OPEN_};
    inotify_for_each_instance(inotify_emit_exact_cb, &event);
}

void inotify_notify_modify(const char *path) {
    if (path == NULL || path[0] != '/')
        return;
    struct inotify_path_event event = {.path = path, .mask = IN_MODIFY_};
    inotify_for_each_instance(inotify_emit_exact_cb, &event);
}

void inotify_notify_attrib(const char *path) {
    if (path == NULL || path[0] != '/')
        return;
    struct inotify_path_event event = {.path = path, .mask = IN_ATTRIB_};
    inotify_for_each_instance(inotify_emit_exact_cb, &event);
}

void inotify_notify_create(const char *path, bool is_dir) {
    if (path == NULL || path[0] != '/')
        return;
    struct inotify_path_event event = {
        .path = path,
        .mask = IN_CREATE_ | (is_dir ? IN_ISDIR_ : 0),
    };
    inotify_for_each_instance(inotify_emit_parent_cb, &event);
}

void inotify_notify_delete(const char *path, bool is_dir) {
    if (path == NULL || path[0] != '/')
        return;
    struct inotify_path_event parent = {
        .path = path,
        .mask = IN_DELETE_ | (is_dir ? IN_ISDIR_ : 0),
    };
    struct inotify_path_event exact = {
        .path = path,
        .mask = IN_DELETE_SELF_,
    };
    inotify_for_each_instance(inotify_emit_parent_cb, &parent);
    inotify_for_each_instance(inotify_emit_exact_cb, &exact);
}

void inotify_notify_move(const char *old_path, const char *new_path, bool is_dir) {
    if (old_path == NULL || new_path == NULL || old_path[0] != '/' || new_path[0] != '/')
        return;
    static _Atomic dword_t next_cookie = 1;
    struct inotify_move_event event = {
        .old_path = old_path,
        .new_path = new_path,
        .old_mask = IN_MOVED_FROM_ | (is_dir ? IN_ISDIR_ : 0),
        .new_mask = IN_MOVED_TO_ | (is_dir ? IN_ISDIR_ : 0),
        .self_mask = IN_MOVE_SELF_,
        .cookie = next_cookie++,
    };
    inotify_for_each_instance(inotify_emit_move_cb, &event);
}
