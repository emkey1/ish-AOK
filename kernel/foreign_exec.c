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
        // glibc reads its locales from /usr/lib/locale, which is THIS root's:
        // without them tmux, for one, refuses to start ("invalid LC_ALL,
        // LC_CTYPE or LANG"), even for C.UTF-8, which Debian ships as a file.
        char locales[MAX_PATH];
        if (snprintf(locales, sizeof(locales), "%s/usr/lib/locale", root) < (int) sizeof(locales) &&
                is_dir(locales))
            fx->locpath = strdup(locales);
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
    free(fx->locpath);
    *fx = (struct foreign_exec) {0};
}

// --------------------------------------------------------- the environment

static bool env_is(const char *entry, const char *var) {
    return strncmp(entry, var, strlen(var)) == 0;
}

// The variables a LIBS exec sets. Each has a marker beside it recording the
// value the exec chose, so a later exec can tell its own setting from the
// user's: ours is taken back out of a program that does not need it, the
// user's is always kept. LD_LIBRARY_PATH puts ours in front of the user's;
// for LOCPATH the user's own, if any, wins.
struct env_rule {
    const char *var, *marker;
    bool join;
};
static const struct env_rule env_rules[] = {
    {"LD_LIBRARY_PATH=", "AOK_FOREIGN_LDPATH=", true},
    {"LOCPATH=", "AOK_FOREIGN_LOCPATH=", false},
};
#define ENV_RULES (sizeof(env_rules) / sizeof(env_rules[0]))

static const char *rule_ours(const struct foreign_exec *fx, size_t i) {
    const char *ours = i == 0 ? fx->ldpath : fx->locpath;
    return ours != NULL && ours[0] != '\0' ? ours : NULL;
}

char *foreign_exec_env(const struct foreign_exec *fx, const char *envp, size_t envc,
                       size_t *count_out) {
    // What is there: each variable, and the marker an earlier LIBS exec left
    // to say which value was its own.
    const char *current[ENV_RULES] = {0}, *marker[ENV_RULES] = {0};
    bool touched = false;
    const char *p = envp;
    for (size_t i = 0; i < envc; i++) {
        for (size_t r = 0; r < ENV_RULES; r++) {
            if (env_is(p, env_rules[r].var))
                current[r] = p + strlen(env_rules[r].var);
            else if (env_is(p, env_rules[r].marker))
                marker[r] = p + strlen(env_rules[r].marker);
        }
        p += strlen(p) + 1;
    }
    for (size_t r = 0; r < ENV_RULES; r++)
        touched |= rule_ours(fx, r) != NULL || marker[r] != NULL;
    if (!touched)
        return NULL;

    // The value each should have, NULL for none, and whether it carries ours.
    char *value[ENV_RULES] = {0};
    bool marked[ENV_RULES] = {0};
    size_t size = (size_t) (p - envp) + 1;
    for (size_t r = 0; r < ENV_RULES; r++) {
        const char *ours = rule_ours(fx, r);
        // The user's part: for a joined path, what follows exactly the prefix
        // the marker recorded; otherwise the whole value, unless it is ours.
        const char *user = current[r];
        if (user != NULL && marker[r] != NULL) {
            size_t mlen = strlen(marker[r]);
            if (env_rules[r].join) {
                if (strncmp(user, marker[r], mlen) == 0 && (user[mlen] == '\0' || user[mlen] == ':'))
                    user = user[mlen] == ':' ? user + mlen + 1 : NULL;
            } else if (strcmp(user, marker[r]) == 0) {
                user = NULL;
            }
        }
        if (user != NULL && user[0] == '\0')
            user = NULL;
        if (ours != NULL && env_rules[r].join) {
            size_t n = strlen(ours) + (user ? strlen(user) + 1 : 0) + 1;
            value[r] = malloc(n);
            if (value[r] != NULL)
                snprintf(value[r], n, "%s%s%s", ours, user ? ":" : "", user ? user : "");
            marked[r] = true;
        } else if (user != NULL) {
            value[r] = strdup(user);
        } else if (ours != NULL) {
            value[r] = strdup(ours);
            marked[r] = true;
        }
        if ((ours != NULL || user != NULL) && value[r] == NULL)
            goto fail;
        if (value[r] != NULL)
            size += strlen(env_rules[r].var) + strlen(value[r]) + 1 +
                    (marked[r] ? strlen(env_rules[r].marker) + strlen(ours) + 1 : 0);
    }

    char *out = malloc(size);
    if (out == NULL)
        goto fail;
    size_t at = 0, count = 0;
    p = envp;
    for (size_t i = 0; i < envc; i++) {
        size_t len = strlen(p) + 1;
        bool drop = false;
        for (size_t r = 0; r < ENV_RULES; r++)
            drop |= env_is(p, env_rules[r].var) || env_is(p, env_rules[r].marker);
        if (!drop) {
            memcpy(out + at, p, len);
            at += len;
            count++;
        }
        p += len;
    }
    for (size_t r = 0; r < ENV_RULES; r++) {
        if (value[r] == NULL)
            continue;

        size_t avail = size > at ? size - at : 0;
        int n = snprintf(out + at, avail, "%s%s", env_rules[r].var, value[r]);
        if (n >= 0 && (size_t) n < avail)
            at += (size_t) n + 1;
        else
            at += avail;
        count++;
        // The marker only for a value this exec chose, and only its own part;
        // a value that is purely the user's is theirs to keep.
        if (marked[r]) {
            avail = size > at ? size - at : 0;
            n = snprintf(out + at, avail, "%s%s", env_rules[r].marker, rule_ours(fx, r));
            if (n >= 0 && (size_t) n < avail)
                at += (size_t) n + 1;
            else
                at += avail;
            count++;
        }
        free(value[r]);
    }
    out[at] = '\0';
    *count_out = count;
    return out;

fail:
    for (size_t r = 0; r < ENV_RULES; r++)
        free(value[r]);
    return NULL;
}
