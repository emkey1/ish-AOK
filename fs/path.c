#include <string.h>
#include <sys/stat.h>
#include "kernel/calls.h"
#include "fs/path.h"

static int __path_normalize(const char *root_path, const char *at_path, const char *path, char *out, int flags, int levels) {
    // you must choose one
    if (flags & N_SYMLINK_FOLLOW)
        assert(!(flags & N_SYMLINK_NOFOLLOW));
    else
        assert(flags & N_SYMLINK_NOFOLLOW);

    const char *p = path;
    char *o = out;
    *o = '\0';
    int n = MAX_PATH - 1;

    if (path[0] == '\0')
        return _ENOENT;

    if (at_path != NULL && strcmp(at_path, "/") != 0) {
        // Bolt: Hoist strlen() and replace strcpy/strlen/strlen with memcpy
        // to avoid multiple O(N) traversals of the same string.
        size_t at_path_len = strlen(at_path);
        memcpy(o, at_path, at_path_len + 1);
        n -= at_path_len;
        o += at_path_len;
    }

    while (*p == '/')
        p++;

    while (*p != '\0') {
        if (p[0] == '.') {
            if (p[1] == '\0' || p[1] == '/') {
                // single dot path component, ignore
                p++;
                while (*p == '/')
                    p++;
                continue;
            } else if (p[1] == '.' && (p[2] == '\0' || p[2] == '/')) {
                // double dot path component, delete the last component
                if (o != out) {
                    do {
                        o--;
                        n++;
                    } while (*o != '/');
                }
                p += 2;
                while (*p == '/')
                    p++;
                continue;
            }
        }

        // output a slash
        *o++ = '/'; n--;
        char *c = o;
        // copy up to a slash or null
        while (*p != '/' && *p != '\0' && --n > 0)
            *o++ = *p++;
        // eat any slashes
        while (*p == '/')
            p++;

        if (n == 0)
            return _ENAMETOOLONG;

        if ((flags & N_SYMLINK_FOLLOW) || *p != '\0') {
            // this buffer is used to store the path that we're readlinking, then
            // if it turns out to point to a symlink it's reused as the buffer
            // passed to the next path_normalize call
            char possible_symlink[MAX_PATH];
            *o = '\0';
            strcpy(possible_symlink, out);
            struct mount *mount = find_mount_and_trim_path(possible_symlink);
            if (mount == NULL)
                return _ENOENT;
            assert(path_is_normalized(possible_symlink));
            int res = _EINVAL;
            if (mount->fs->readlink)
                res = mount->fs->readlink(mount, possible_symlink, c, MAX_PATH - (c - out));
            if (res >= 0) {
                mount_release(mount);
                // RESOLVE_NO_SYMLINKS: the caller asked for a resolution with
                // no symlink in it at all, so finding one is the answer, not
                // something to follow.
                if (flags & N_NO_SYMLINKS)
                    return _ELOOP;
                // Linux's MAXSYMLINKS. Five was low enough that ordinary
                // /etc/alternatives-style chains hit ELOOP: `levels` counts
                // every link followed across the whole resolution, including
                // symlinked directory components, so a handful of them in a
                // path exhausted it. Measured: Linux resolves 8 fine, AOK
                // failed from 6.
                //
                // The recursion is one 4KB frame per level, so 40 costs about
                // 170KB against a 4MB task stack.
                if (levels >= MAX_SYMLINKS)
                    return _ELOOP;
                // readlink does not null terminate
                c[res] = '\0';
                // If the symlink target is absolute, it must be re-anchored at the
                // calling process's root (root_path -- e.g. a chroot), not the real
                // filesystem root. Previously this dropped the accumulated `out`
                // prefix (which carried the chroot anchor applied by path_normalize()
                // at the top-level call) and recursed with at_path=NULL, so an
                // absolute symlink target escaped the chroot and resolved against the
                // real root. E.g. inside `chroot /i386root`, a symlink /bin/uname ->
                // /bin/busybox would resolve to the real /bin/busybox instead of
                // /i386root/bin/busybox, causing execve() to fail with ENOENT (no
                // such absolute path in the real root) or resolve the wrong file.
                bool absolute_target = *c == '/';
                if (absolute_target)
                    memmove(out, c, strlen(c) + 1);
                char *expanded_path = possible_symlink;
                // Bolt: Optimize string concatenation by tracking lengths and using
                // memcpy instead of multiple strcat calls which cause O(N^2) behavior.
                size_t out_len = strlen(out);
                memcpy(expanded_path, out, out_len + 1);
                if (*p != '\0') {
                    size_t p_len = strlen(p);
                    if (out_len + 1 + p_len >= MAX_PATH)
                        return _ENAMETOOLONG;
                    expanded_path[out_len] = '/';
                    memcpy(expanded_path + out_len + 1, p, p_len + 1);
                }
                const char *next_at_path = absolute_target ? root_path : NULL;
                return __path_normalize(root_path, next_at_path, expanded_path, out, flags, levels + 1);
            }

            // A slash after this component means it must be a directory. It
            // means we need SEARCH permission on it only if there is something
            // after it to reach: a trailing slash asks "is this a directory",
            // not "let me traverse into it".
            //
            // p has already been advanced past the run of slashes, so the
            // final component of "dir/" satisfies this test too, and used to
            // collect an execute check Linux never applies -- stat("/root/")
            // was EACCES for an ordinary user where stat("/root") succeeded,
            // and `test -d /root/` and `ls -d /root/` failed with it.
            if (*(p - 1) == '/') {
                bool traversing = *p != '\0';
                struct statbuf stat;
                int err = mount->fs->stat(mount, possible_symlink, &stat);
                mount_release(mount);
                if (err >= 0) {
                    if (!S_ISDIR(stat.mode))
                        return _ENOTDIR;
                    if (traversing) {
                        err = access_check(&stat, AC_X);
                        if (err < 0)
                            return err;
                    }
                } else if (*p != '\0') {
                    // A non-final component must exist and be a directory. Don't
                    // silently skip a missing one, or a following ".." would pop
                    // it lexically -- e.g. "nonexistent/.." must be ENOENT, not
                    // its (existing) parent.
                    return err;
                }
            } else {
                mount_release(mount);
            }
        }
    }

    *o = '\0';
    assert(path_is_normalized(out));

    return 0;
}

int path_final_dot(const char *path) {
    size_t len = strlen(path);
    // Trailing slashes do not change which component is last: "foo/./" ends in
    // "." just as "foo/." does.
    while (len > 1 && path[len - 1] == '/')
        len--;
    size_t start = len;
    while (start > 0 && path[start - 1] != '/')
        start--;
    size_t n = len - start;
    if (n == 1 && path[start] == '.')
        return 1;
    if (n == 2 && path[start] == '.' && path[start + 1] == '.')
        return 2;
    return 0;
}

int path_normalize(struct fd *at, const char *path, char *out, int flags) {
    // A genuinely-NULL dirfd (distinct from the AT_PWD sentinel (struct fd *)-2)
    // reaches here when an *at syscall is handed a bad/closed dirfd. Linux
    // returns EBADF; don't abort the whole emulator (stress-ng --dir passed one).
    if (at == NULL)
        return _EBADF;
    if (path[0] == '\0')
        return _ENOENT;
    if (current == NULL || current->fs == NULL)
        return _ENOENT;

    // start with root or cwd, depending on whether it starts with a slash
    lock(&current->fs->lock, 0);
    struct fd *root = current->fs->root;
    // N_REALROOT: the caller's path is already fully normalized against the
    // real root (it may itself contain the chroot prefix); anchor absolute
    // resolution at the true root instead of the chroot.
    if (flags & N_REALROOT)
        root = NULL;
    bool absolute = path[0] == '/';
    if (absolute)
        at = root;
    else if (at == AT_PWD)
        at = current->fs->pwd;
    unlock(&current->fs->lock);
    char at_path[MAX_PATH];
    if (at != NULL) {
        int err = generic_getpath(at, at_path);
        if (err < 0)
            return err;
        // A non-path fd (socket, pipe, anon inode -- adhoc_getpath renders
        // these as "socket:[N]" etc.) can land here as the dirfd of any *at
        // syscall. Linux returns ENOTDIR when relative resolution is attempted
        // against such an fd; don't assert (this aborted the whole app when
        // stress-ng --sockabuse passed a socket fd to utimensat).
        if (!path_is_normalized(at_path))
            return _ENOTDIR;
        // The starting directory's OWN search bit is never a component of
        // `path`, so the component loop in __path_normalize never checks it --
        // it only checks what follows. Without this, openat(dirfd, "file")
        // read files inside a directory the caller had no search permission
        // on, and the same held for a cwd whose search bit was removed after
        // the chdir. The dirfd is reachable because O_PATH on a directory
        // correctly performs no permission check of its own, so the check has
        // to happen here, at use time -- an fd opened while permissions
        // allowed it must not keep working after they change.
        //
        // fstat on the fd rather than stat by path: the path lookup above is
        // already the expensive part on fakefs, and this adds no second one.
        // Only for a RELATIVE path. An absolute one starts at the process's
        // own root, and Linux does not require search permission on that --
        // it checks the components it descends into, which __path_normalize
        // already does. Checking it here cost an fstat on every absolute-path
        // resolution and measured ~9% on open/stat-heavy work, for a check
        // Linux does not perform. The case this exists to stop -- openat()
        // through a dirfd on a directory with no search permission, and a cwd
        // whose search bit was removed -- is exactly the relative case.
        if (!absolute) {
            struct statbuf at_stat;
            if (at->mount != NULL && at->mount->fs->fstat != NULL &&
                    at->mount->fs->fstat(at, &at_stat) >= 0) {
                // What KIND of thing it is comes before whether we may search
                // it. A regular file as the dirfd of an *at() call is ENOTDIR
                // on Linux, decided before any lookup; falling straight into
                // the search check reported EACCES instead, because a 0644
                // file has no execute bit -- a plausible-looking errno for
                // entirely the wrong reason, and one that sends a caller
                // looking at permissions rather than at the fd it passed.
                if (!S_ISDIR(at_stat.mode))
                    return _ENOTDIR;
                int perm_err = access_check(&at_stat, AC_X);
                if (perm_err < 0)
                    return perm_err;
            }
        }
    }
    // root_path anchors any *absolute symlink target* encountered while
    // resolving (see __path_normalize): it must always be the process's
    // chroot root, not necessarily `at_path` above (which is cwd-relative
    // when `path` didn't start with '/'), so an absolute symlink inside a
    // chroot (e.g. /bin/uname -> /bin/busybox under `chroot /i386root`)
    // re-resolves against /i386root instead of escaping to the real root.
    char root_path[MAX_PATH];
    if (root != NULL) {
        int err = generic_getpath(root, root_path);
        if (err < 0)
            return err;
        assert(path_is_normalized(root_path));
    }

    int err = __path_normalize(root != NULL ? root_path : NULL, at != NULL ? at_path : NULL, path, out, flags, 0);
    if (err < 0)
        return err;

    if (flags & N_PARENT_DIR_WRITE) {
        // out is fully resolved and normalized here (begins with '/' or is
        // empty, no ".", "..", or unresolved symlinks in the final
        // component -- see __path_normalize). Strip the final component to
        // get the parent directory, then require write+execute permission
        // on it, matching Linux's MAY_WRITE|MAY_EXEC check on the parent for
        // any operation that creates or removes a directory entry.
        char parent[MAX_PATH];
        size_t len = strlen(out);
        if (len == 0) {
            // out == "" means the target itself is the root directory, e.g.
            // mkdir("/") or rmdir("/"). There is no parent to check write
            // permission on; the caller's own existence/EBUSY-style checks
            // (mkdir -> EEXIST, rmdir -> EBUSY) already handle this target
            // correctly without our help, so just let it through here.
            return 0;
        }
        const char *slash = strrchr(out, '/');
        size_t last_slash = slash != NULL ? (size_t) (slash - out) : 0;
        if (last_slash == 0) {
            // parent is the filesystem root, e.g. creating "/foo". The
            // mount-relative representation of the root used throughout this
            // codebase is "" (see generic_getpath, fix_path), not "/" --
            // stat'ing "/" would strip to an empty relative path passed to
            // fstatat(), which is ENOENT without AT_EMPTY_PATH.
            parent[0] = '\0';
        } else {
            memcpy(parent, out, last_slash);
            parent[last_slash] = '\0';
        }

        struct mount *mount = find_mount_and_trim_path(parent);
        if (mount == NULL)
            return _ENOENT;
        struct statbuf stat;
        int stat_err = mount->fs->stat(mount, parent, &stat);
        mount_release(mount);
        if (stat_err < 0)
            return stat_err;
        int access_err = access_check(&stat, AC_W | AC_X);
        if (access_err < 0) {
            if (flags & (N_CREATE_EEXIST_FIRST | N_REMOVE_ENOENT_FIRST)) {
            // Linux looks the final component up BEFORE checking whether the
            // parent may be written: filename_create() returns -EEXIST for a
            // name that is already there, and only vfs_mkdir/vfs_link/etc.
            // then ask may_create() for permission. So a caller that cannot
            // write the parent still gets EEXIST, not EACCES, when the target
            // exists -- and `mkdir -p` depends on exactly that, since it calls
            // mkdir on every component and treats EEXIST as success. Reporting
            // EACCES here made `mkdir -p /tmp/foo` fail outright for any
            // unprivileged user, because "/" is not writable by them and /tmp
            // already exists.
            //
            // Nothing is granted by deferring: the caller's own existence
            // check reports EEXIST, and if the target does NOT exist the
            // permission error below still stands.
                //
                // The REMOVE family needs the same lookup for the opposite
                // reason: Linux's do_unlinkat()/do_rmdir() reject a negative
                // dentry before may_delete() ever runs, so a name that is not
                // there is ENOENT and not EACCES. `rm -f` suppresses ENOENT
                // and nothing else, which is why `rm -f /unwritable/gone`
                // failed here and succeeds on Linux -- and why one such rm in
                // a `set -e` script (tests/manual/setup-regressions.sh's cache
                // store) killed the whole run with no diagnostic.
                //
                // fs->stat is an lstat (AT_SYMLINK_NOFOLLOW), so a dangling
                // symlink still counts as a name that exists and is removable.
                bool target_exists = false;
                char out_copy[MAX_PATH];   // find_mount_and_trim_path mutates it
                strcpy(out_copy, out);
                struct mount *target_mount = find_mount_and_trim_path(out_copy);
                if (target_mount != NULL) {
                    struct statbuf target_stat;
                    int target_err = target_mount->fs->stat(target_mount, out_copy, &target_stat);
                    mount_release(target_mount);
                    target_exists = target_err == 0;
                }
                if ((flags & N_CREATE_EEXIST_FIRST) && target_exists)
                    return 0;           // caller's own check reports EEXIST
                if ((flags & N_REMOVE_ENOENT_FIRST) && !target_exists)
                    return _ENOENT;
            }
            // The target's existence does not rescue this caller: the
            // permission error stands.
            return access_err;
        }
    }

    return 0;
}


bool path_is_normalized(const char *path) {
    while (*path != '\0') {
        if (*path != '/')
            return false;
        path++;
        if (*path == '/')
            return false;
        while (*path != '/' && *path != '\0')
            path++;
    }
    return true;
}

bool path_next_component(const char **path, char *component, int *err) {
    const char *p = *path;
    if (*p == '\0')
        return false;

    assert(*p == '/');
    p++;
    char *c = component;
    while (*p != '/' && *p != '\0') {
        *c++ = *p++;
        if (c - component >= MAX_NAME) {
            *err = _ENAMETOOLONG;
            return false;
        }
    }
    *c = '\0';
    *path = p;
    return true;
}
