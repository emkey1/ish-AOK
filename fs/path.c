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

    if (strcmp(path, "") == 0)
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
                if (levels >= 5)
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

            // if there's a slash after this component, ensure that if it
            // exists, it's a directory and that we have execute perms on it
            if (*(p - 1) == '/') {
                struct statbuf stat;
                int err = mount->fs->stat(mount, possible_symlink, &stat);
                mount_release(mount);
                if (err >= 0) {
                    if (!S_ISDIR(stat.mode))
                        return _ENOTDIR;
                    err = access_check(&stat, AC_X);
                    if (err < 0)
                        return err;
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

int path_normalize(struct fd *at, const char *path, char *out, int flags) {
    // A genuinely-NULL dirfd (distinct from the AT_PWD sentinel (struct fd *)-2)
    // reaches here when an *at syscall is handed a bad/closed dirfd. Linux
    // returns EBADF; don't abort the whole emulator (stress-ng --dir passed one).
    if (at == NULL)
        return _EBADF;
    if (strcmp(path, "") == 0)
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
    if (path[0] == '/')
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
        size_t last_slash = 0;
        // Bolt: Optimize finding the last slash by scanning backward instead of forward.
        // Impact: Avoids unnecessary iterations on long paths, yielding O(1) performance in the best case.
        for (size_t i = len; i > 0; i--) {
            if (out[i - 1] == '/') {
                last_slash = i - 1;
                break;
            }
        }
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
        if (access_err < 0)
            return access_err;
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
