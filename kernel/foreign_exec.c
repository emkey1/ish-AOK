// Running a program that belongs to another installed root.
//
// Every root other than the booted one is mounted at /AOK/roots/<name>, so its
// programs are files you can name: /AOK/roots/Devuan6-arm64/usr/bin/tmux. A
// static one simply runs. A dynamic one names its loader by an absolute path
// (/lib/ld-linux-aarch64.so.1) that means "in the root I came from" -- and
// looked up from here, in a native-mode root (docs/native_mode_plan.md) or a
// different distribution, it is not there, so Linux's exec fails with ENOENT
// and the shell says "no such file or directory" about a file that plainly
// exists. That is what mode OFF still does.
//
// The two other modes, a runtime choice (/proc/ish/foreign_exec, the app's
// Settings), because each is right for different programs:
//
//   ROOT  the program runs INSIDE its root: the task is chrooted into
//         /AOK/roots/<name> before the loader is opened, with /proc, /sys,
//         /dev, /dev/pts, /run, /AOK/native, /AOK/docs and /AOK/tools bound
//         into it first, as mount-root.sh binds them. It finds its own /etc,
//         its /usr/share data, its libraries -- everything -- and so does
//         anything it starts. It sees that root's home and /tmp, not these.
//   LIBS  the program runs HERE, in this root's filesystem, with only its
//         loader and libraries taken from its root: the loader is opened from
//         there, and LD_LIBRARY_PATH names that root's library directories. It
//         sees this root's files, home and /tmp; a program that needs data
//         files of its own (vim's runtime, Python's library) will not find
//         them.
//
// Only a program that would otherwise have failed is touched: one whose
// loader is missing here, and which lives in another root.

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fs/fd.h"
#include "fs/path.h"
#include "kernel/errno.h"
#include "kernel/foreign_exec.h"
#include "kernel/fs.h"
#include "kernel/task.h"

#define ROOTS_PREFIX "/AOK/roots/"
#define LDPATH_VAR "LD_LIBRARY_PATH="
#define MARKER_VAR "AOK_FOREIGN_LDPATH="

static _Atomic int foreign_exec_mode = FOREIGN_EXEC_ROOT;

enum foreign_exec_mode foreign_exec_get_mode(void) {
    return (enum foreign_exec_mode) atomic_load(&foreign_exec_mode);
}

void foreign_exec_set_mode(enum foreign_exec_mode mode) {
    atomic_store(&foreign_exec_mode, (int) mode);
}

const char *foreign_exec_mode_name(enum foreign_exec_mode mode) {
    switch (mode) {
        case FOREIGN_EXEC_OFF: return "off";
        case FOREIGN_EXEC_ROOT: return "root";
        case FOREIGN_EXEC_LIBS: return "libs";
    }
    return "off";
}

int foreign_exec_parse_mode(const char *text, size_t len) {
    while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == ' '))
        len--;
    static const struct { const char *name; enum foreign_exec_mode mode; } names[] = {
        {"off", FOREIGN_EXEC_OFF}, {"0", FOREIGN_EXEC_OFF},
        {"root", FOREIGN_EXEC_ROOT}, {"chroot", FOREIGN_EXEC_ROOT},
        {"libs", FOREIGN_EXEC_LIBS}, {"libraries", FOREIGN_EXEC_LIBS},
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (strlen(names[i].name) == len && strncmp(names[i].name, text, len) == 0)
            return (int) names[i].mode;
    return -1;
}

// The root EXE lives in, as "/AOK/roots/<name>", or false if it is not in one.
static bool foreign_root_of(struct fd *exe, char *root, size_t size) {
    char path[MAX_PATH];
    if (generic_getpath(exe, path) < 0)
        return false;
    size_t plen = strlen(ROOTS_PREFIX);
    if (strncmp(path, ROOTS_PREFIX, plen) != 0)
        return false;
    const char *slash = strchr(path + plen, '/');
    if (slash == NULL || slash == path + plen)
        return false;
    size_t len = (size_t) (slash - path);
    if (len >= size)
        return false;
    memcpy(root, path, len);
    root[len] = '\0';
    return true;
}

static bool is_dir(const char *path) {
    struct statbuf stat;
    return generic_statat(AT_PWD, path, &stat, 0) == 0 && S_ISDIR(stat.mode);
}

// What mount-root.sh binds into a root, in the order that lets the later
// ones land inside the earlier (dev before dev/pts).
static const char *const root_binds[] = {
    "proc", "sys", "dev", "dev/pts", "run", "AOK/native", "AOK/docs", "AOK/tools",
};

static void bind_into_root(const char *root) {
    // The mount points may need making -- /AOK is not in a distribution's
    // tarball -- and the program's user may not be allowed to make them in a
    // root-owned tree. The kernel is doing this, not the program, so the
    // directories are made with the filesystem identity of root.
    uid_t_ fsuid = current->fsuid, fsgid = current->fsgid;
    current->fsuid = 0;
    current->fsgid = 0;
    for (size_t i = 0; i < sizeof(root_binds) / sizeof(root_binds[0]); i++) {
        char source[MAX_PATH], point[MAX_PATH];
        snprintf(source, sizeof(source), "/%s", root_binds[i]);
        if (!is_dir(source))
            continue;
        // Each component in turn: mkdir -p.
        snprintf(point, sizeof(point), "%s/%s", root, root_binds[i]);
        for (char *p = point + strlen(root) + 1; *p != '\0'; p++) {
            if (*p == '/') {
                *p = '\0';
                generic_mkdirat(AT_PWD, point, 0755);
                *p = '/';
            }
        }
        generic_mkdirat(AT_PWD, point, 0755);
        if (!mount_exists_at_point(point))
            mount_bind_dir(source, point);
    }
    current->fsuid = fsuid;
    current->fsgid = fsgid;
}

// The library directories of the root at ROOT, as its loader would search
// them: the usual ones and Debian's multiarch ones, those that exist.
static char *library_path(const char *root) {
    static const char *const dirs[] = {
        "/usr/local/lib", "/lib", "/usr/lib", "/lib64", "/usr/lib64",
        "/lib/aarch64-linux-gnu", "/usr/lib/aarch64-linux-gnu",
        "/lib/x86_64-linux-gnu", "/usr/lib/x86_64-linux-gnu",
        "/lib/i386-linux-gnu", "/usr/lib/i386-linux-gnu",
        "/lib/riscv64-linux-gnu", "/usr/lib/riscv64-linux-gnu",
    };
    size_t cap = 1, used = 0;
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++)
        cap += strlen(root) + strlen(dirs[i]) + 1;
    char *out = malloc(cap);
    if (out == NULL)
        return NULL;
    out[0] = '\0';
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        char dir[MAX_PATH];
        snprintf(dir, sizeof(dir), "%s%s", root, dirs[i]);
        if (!is_dir(dir))
            continue;
        used += (size_t) snprintf(out + used, cap - used, "%s%s", used ? ":" : "", dir);
    }
    return out;
}

struct fd *foreign_exec_interp(struct fd *exe, const char *interp, struct foreign_exec *fx) {
    enum foreign_exec_mode mode = foreign_exec_get_mode();
    char root[MAX_PATH];
    if (mode == FOREIGN_EXEC_OFF || interp == NULL || interp[0] != '/' ||
            !foreign_root_of(exe, root, sizeof(root)))
        return ERR_PTR(_ENOENT);

    if (mode == FOREIGN_EXEC_LIBS) {
        char path[MAX_PATH];
        if (snprintf(path, sizeof(path), "%s%s", root, interp) >= (int) sizeof(path))
            return ERR_PTR(_ENAMETOOLONG);
        struct fd *fd = generic_open(path, O_RDONLY_, 0);
        if (IS_ERR(fd))
            return fd;
        fx->ldpath = library_path(root);
        return fd;
    }

    // ROOT: become a task of that root -- unless the exec already did, to
    // open the program itself (foreign_exec_split's path).
    if (fx->old_root == NULL) {
        int err = foreign_exec_enter_root(root, fx);
        if (err < 0)
            return ERR_PTR(err);
    }
    struct fd *fd = generic_open(interp, O_RDONLY_, 0);
    if (IS_ERR(fd))
        foreign_exec_undo(fx);
    return fd;
}

int foreign_exec_enter_root(const char *root, struct foreign_exec *fx) {
    bind_into_root(root);
    struct fd *new_root = fs_open_dir(root);
    if (IS_ERR(new_root))
        return (int) PTR_ERR(new_root);
    // The cwd stays where it is when it is inside the root -- it reads as the
    // same directory from in there -- and becomes the root's / otherwise.
    char cwd[MAX_PATH];
    bool cwd_inside = false;
    lock(&current->fs->lock, 0);
    struct fd *pwd = current->fs->pwd;
    unlock(&current->fs->lock);
    if (pwd != NULL && generic_getpath(pwd, cwd) >= 0) {
        size_t rlen = strlen(root);
        cwd_inside = strncmp(cwd, root, rlen) == 0 && (cwd[rlen] == '\0' || cwd[rlen] == '/');
    }
    struct fd *new_pwd = NULL;
    if (!cwd_inside) {
        new_pwd = fs_open_dir(root);
        if (IS_ERR(new_pwd)) {
            fd_close(new_root);
            return (int) PTR_ERR(new_pwd);
        }
    }

    lock(&current->fs->lock, 0);
    fx->old_root = current->fs->root;
    current->fs->root = new_root;
    if (new_pwd != NULL) {
        fx->old_pwd = current->fs->pwd;
        current->fs->pwd = new_pwd;
    }
    unlock(&current->fs->lock);
    return 0;
}

bool foreign_exec_split(const char *path, char *root, size_t root_size, const char **rest) {
    if (foreign_exec_get_mode() == FOREIGN_EXEC_OFF)
        return false;
    size_t plen = strlen(ROOTS_PREFIX);
    if (strncmp(path, ROOTS_PREFIX, plen) != 0)
        return false;
    const char *slash = strchr(path + plen, '/');
    if (slash == NULL || slash == path + plen || (size_t) (slash - path) >= root_size)
        return false;
    memcpy(root, path, (size_t) (slash - path));
    root[slash - path] = '\0';
    *rest = slash;
    return true;
}

struct fd *foreign_exec_lookup_root_begin(const char *root) {
    struct fd *dir = fs_open_dir(root);
    if (IS_ERR(dir))
        return dir;
    lock(&current->fs->lock, 0);
    struct fd *old = current->fs->root;
    current->fs->root = dir;
    unlock(&current->fs->lock);
    return old;
}

void foreign_exec_lookup_root_end(struct fd *old_root) {
    lock(&current->fs->lock, 0);
    struct fd *dir = current->fs->root;
    current->fs->root = old_root;
    unlock(&current->fs->lock);
    fd_close(dir);
}

void foreign_exec_undo(struct foreign_exec *fx) {
    if (fx->old_root == NULL)
        return;
    lock(&current->fs->lock, 0);
    struct fd *root = current->fs->root;
    current->fs->root = fx->old_root;
    struct fd *pwd = NULL;
    if (fx->old_pwd != NULL) {
        pwd = current->fs->pwd;
        current->fs->pwd = fx->old_pwd;
    }
    unlock(&current->fs->lock);
    fd_close(root);
    if (pwd != NULL)
        fd_close(pwd);
    fx->old_root = NULL;
    fx->old_pwd = NULL;
}

void foreign_exec_done(struct foreign_exec *fx) {
    if (fx->old_root != NULL)
        fd_close(fx->old_root);
    if (fx->old_pwd != NULL)
        fd_close(fx->old_pwd);
    free(fx->ldpath);
    *fx = (struct foreign_exec) {0};
}

// --------------------------------------------------------- the environment

static bool env_is(const char *entry, const char *var) {
    return strncmp(entry, var, strlen(var)) == 0;
}

char *foreign_exec_env(const struct foreign_exec *fx, const char *envp, size_t envc,
                       size_t *count_out) {
    // What is there: LD_LIBRARY_PATH, and the marker an earlier LIBS exec left
    // to say which value was its own.
    const char *ldpath = NULL, *marker = NULL;
    const char *p = envp;
    for (size_t i = 0; i < envc; i++) {
        if (env_is(p, LDPATH_VAR))
            ldpath = p + strlen(LDPATH_VAR);
        else if (env_is(p, MARKER_VAR))
            marker = p + strlen(MARKER_VAR);
        p += strlen(p) + 1;
    }
    if (fx->ldpath == NULL && marker == NULL)
        return NULL;

    // The value LD_LIBRARY_PATH should have, NULL for none: ours in front of
    // whatever the user had. The marker holds exactly the prefix an earlier
    // LIBS exec put there, so what follows it is the user's own.
    const char *user = ldpath;
    if (user != NULL && marker != NULL) {
        size_t mlen = strlen(marker);
        if (strncmp(user, marker, mlen) == 0 && (user[mlen] == '\0' || user[mlen] == ':'))
            user = user[mlen] == ':' ? user + mlen + 1 : NULL;
    }
    if (user != NULL && user[0] == '\0')
        user = NULL;
    char *value = NULL;
    if (fx->ldpath != NULL && fx->ldpath[0] != '\0') {
        size_t n = strlen(fx->ldpath) + (user ? strlen(user) + 1 : 0) + 1;
        value = malloc(n);
        if (value == NULL)
            return NULL;
        snprintf(value, n, "%s%s%s", fx->ldpath, user ? ":" : "", user ? user : "");
    } else if (user != NULL) {
        value = strdup(user);
        if (value == NULL)
            return NULL;
    }

    size_t size = (size_t) (p - envp) + 1;
    if (value != NULL)
        size += 2 * (strlen(value) + strlen(MARKER_VAR) + 2);
    char *out = malloc(size);
    if (out == NULL) {
        free(value);
        return NULL;
    }
    size_t at = 0, count = 0;
    p = envp;
    for (size_t i = 0; i < envc; i++) {
        size_t len = strlen(p) + 1;
        if (!env_is(p, LDPATH_VAR) && !env_is(p, MARKER_VAR)) {
            memcpy(out + at, p, len);
            at += len;
            count++;
        }
        p += len;
    }
    if (value != NULL) {
        at += (size_t) sprintf(out + at, "%s%s", LDPATH_VAR, value) + 1;
        count++;
        // The marker only for a path this exec chose, and only its own part;
        // a value that is purely the user's is theirs to keep.
        if (fx->ldpath != NULL && fx->ldpath[0] != '\0') {
            at += (size_t) sprintf(out + at, "%s%s", MARKER_VAR, fx->ldpath) + 1;
            count++;
        }
    }
    out[at] = '\0';
    free(value);
    *count_out = count;
    return out;
}
