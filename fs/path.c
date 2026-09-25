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
    // N_PARENT_ONLY never resolves the final component, so there is nothing
    // for N_SYMLINK_FOLLOW to follow there; asking for both is a caller that
    // has not decided which walk it wants.
    assert(!((flags & N_PARENT_ONLY) && (flags & N_SYMLINK_FOLLOW)));

    const char *p = path;
    char *o = out;
    *o = '\0';
    int n = MAX_PATH - 1;

    if (path[0] == '\0')
        return _ENOENT;

    // The process's root, when it is not the real one: `..` stops there.
    size_t root_len = root_path != NULL && strcmp(root_path, "/") != 0 ? strlen(root_path) : 0;

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
                // double dot path component, delete the last component --
                // unless the walk is standing on the process's root, where
                // `..` is the root itself (Linux's follow_dotdot stops at
                // nd->root). Popping it lexically let every chrooted process
                // out: after chroot("/jail"), chdir("/"), a plain chdir("..")
                // landed in the real /, and so did "/..", openat(root_fd,
                // ".."), and a symlink to "../..". A cwd left outside the
                // root by chroot-without-chdir walks `..` freely, as on
                // Linux; only reaching the root stops it.
                bool at_root = root_len != 0 && (size_t) (o - out) == root_len &&
                    memcmp(out, root_path, root_len) == 0;
                // Nor above the root of a detached mount, which has no parent
                // to climb to: Linux's follow_dotdot finds nothing mounted
                // above it and stays. The walk is standing on one exactly when
                // what it has so far is a staging point (kernel/fs.h).
                size_t staging = mount_staging_point_len(out, (size_t) (o - out));
                if (staging != 0 && staging == (size_t) (o - out))
                    at_root = true;
                if (o != out && !at_root) {
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

        // MOUNT_STAGING_DIR, at the top of the real root, is where detached
        // mounts are parked (kernel/fs.h). A walk that starts in one never
        // spells it -- its at_path is copied in above, not walked -- so this
        // is a walk trying to get IN, and only N_DETACHED_OK's may. For them
        // the directory itself is not looked up: it exists on no filesystem.
        // For everyone else anything below it is not there, so no name the
        // guest spells reaches a mount Linux would give no name at all.
        // Inside a chroot the same spelling is an ordinary name: `out` then
        // starts with the chroot's path.
        const size_t staging_dir_len = sizeof(MOUNT_STAGING_DIR) - 1;
        if (c == out + 1 && (size_t) (o - c) == staging_dir_len - 1 &&
                memcmp(c, &MOUNT_STAGING_DIR[1], staging_dir_len - 1) == 0) {
            if ((flags & N_DETACHED_OK) && *p != '\0')
                continue;
        } else if (c == out + staging_dir_len + 1 &&
                memcmp(out, MOUNT_STAGING_DIR "/", staging_dir_len + 1) == 0) {
            if (!(flags & N_DETACHED_OK))
                return _ENOENT;
        }

        // N_SLASH_EISDIR: open(O_CREAT) on a name spelled with a trailing
        // slash. Linux answers EISDIR from open_last_lookups() --
        // `if (unlikely(nd->last.name[nd->last.len])) return ERR_PTR(-EISDIR)`
        // -- which sits after the parent walk and BEFORE the final lookup, so
        // it does not matter whether the name exists, what kind of thing it
        // is, or whether the parent may be written. This is that spot: the
        // components before this one have already been resolved and checked,
        // and nothing has looked at this one yet.
        //
        // It has to be here rather than in the caller because the checks that
        // would otherwise run on the final component answer first and answer
        // differently: `open("file/", O_CREAT|O_WRONLY)` collected ENOTDIR
        // from the block below, and `open("dir/", O_CREAT|O_RDONLY)` simply
        // succeeded.
        if (*p == '\0' && *(p - 1) == '/' && (flags & N_SLASH_EISDIR))
            return _EISDIR;

        // Which components get looked up at all. Everything before the last
        // one always does. The LAST one does when the caller asked to follow
        // a final symlink -- and also when it was spelled with a trailing
        // slash, because Linux's lookup_last() turns that slash into
        // LOOKUP_FOLLOW | LOOKUP_DIRECTORY:
        //
        //     if (nd->last_type == LAST_NORM && nd->last.name[nd->last.len])
        //             nd->flags |= LOOKUP_FOLLOW | LOOKUP_DIRECTORY;
        //
        // so "link/" follows the link and requires a directory even for an
        // lstat, a readlink or an open(O_NOFOLLOW) -- measured on Linux 6.12:
        // lstat("link-to-dir/") reports the DIRECTORY, readlink("link-to-dir/")
        // is EINVAL, and open("link-to-dir/", O_NOFOLLOW) succeeds. AOK skipped
        // this whole block for every nofollow caller, so the slash was ignored
        // outright and lstat("file/") succeeded.
        //
        // lookup_last() is reached only from path_lookupat(), never from
        // path_parentat() -- which is exactly N_PARENT_ONLY, whose callers
        // spend the slash with their own rules instead.
        if ((flags & N_SYMLINK_FOLLOW) || *p != '\0' ||
                (*(p - 1) == '/' && !(flags & N_PARENT_ONLY))) {
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
            // Asked before the link's text lands on top of this component:
            // is the directory holding it inside a detached mount? And is it
            // a procfs link, which Linux follows by jumping to the file it
            // names (nd_jump_link) rather than by walking its text?
            size_t parent_len = (size_t) (c - 1 - out);
            bool in_staging = mount_staging_point_len(out, parent_len) != 0;
            bool magic = mount->fs == &procfs;
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
                } else if (*(p - 1) == '/') {
                    // A FINAL symlink component spelled with a trailing slash.
                    // Nothing follows it, so the branch above does not run and
                    // the slash -- which says "and this is a directory" -- was
                    // simply dropped: the recursion below then resolved a path
                    // that had never been spelled with one, and
                    // open("dangling-link-to-a-file/") opened the file.
                    //
                    // Linux keeps it because the slash belongs to the NAME,
                    // not to the walk: trailing_slashes turns into
                    // LOOKUP_DIRECTORY, which survives the symlink being
                    // followed and is spent on whatever it lands on. Carrying
                    // it into the expanded path spends it the same way, at the
                    // must-be-a-directory block below, one level down --
                    // measured on Linux 6.12, open("link-to-file/") is ENOTDIR
                    // and open("dangling-link/") is ENOENT, where the same two
                    // without the slash succeed and report ENOENT
                    // respectively. A link to a directory is unaffected, which
                    // is the case everything actually relies on.
                    if (out_len + 1 >= MAX_PATH)
                        return _ENAMETOOLONG;
                    expanded_path[out_len] = '/';
                    expanded_path[out_len + 1] = '\0';
                }
                const char *next_at_path = absolute_target ? root_path : NULL;
                // The target is walked again from the top, through
                // MOUNT_STAGING_DIR when it is in a detached mount (see the
                // entry rule above). A relative target in one is: `..` cannot
                // leave the mount, so it stays there. An absolute one starts
                // from the process's root, like any other, and may not get
                // back in -- except through a procfs link, whose text is the
                // path of a descriptor the reader was allowed to see.
                int next_flags = flags & ~N_DETACHED_OK;
                if (magic || (!absolute_target && in_staging))
                    next_flags |= N_DETACHED_OK;
                return __path_normalize(root_path, next_at_path, expanded_path, out, next_flags, levels + 1);
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

// Walk everything the final component is reached THROUGH, the way Linux's
// filename_parentat() does, and report only what that walk had to say: 0 if it
// arrived, else its error (a missing parent's ENOENT, a parent that is a
// regular file's ENOTDIR, an unsearchable one's EACCES).
//
// For a path whose final component is "." or ".." -- the only caller, see
// path_final_dot() -- normalizing the whole path IS that walk and nothing
// more: such a component names no entry, and __path_normalize consumes it
// lexically (skip, or pop the last component) without ever looking it up. The
// components before it are the parent, and each is checked exactly as Linux
// checks it: it must exist, be a directory, and be searchable.
//
// No N_PARENT_DIR_WRITE and no create flags, deliberately. This is the walk,
// not the operation: Linux answers the final-"." rule between the two, so the
// parent's write permission must not be consulted yet.
//
// N_PARENT_ONLY is what makes it a walk rather than a lookup, and it matters
// for a caller whose path is not dotty after all -- a trailing slash on the
// final component must stay unspent here, so the caller's own rule gets to
// answer it.
int path_parent_walk(struct fd *at, const char *path_raw) {
    char scratch[MAX_PATH];
    return path_normalize(at, path_raw, scratch, N_SYMLINK_NOFOLLOW | N_PARENT_ONLY);
}

// Linux's `nd->last.name[nd->last.len]`: the character where the final
// component ends, which is '/' exactly when the name was spelled with a
// trailing slash. path_normalize() rejects "" before any caller gets here.
bool path_trailing_slash(const char *path) {
    size_t len = strlen(path);
    return len > 0 && path[len - 1] == '/';
}

// What does the already-normalized path `normalized` name? An lstat (fs->stat
// is AT_SYMLINK_NOFOLLOW), so a dangling symlink counts as a name that is
// there -- which is what a lookup of the final component answers, and Linux
// decides EEXIST from exactly that.
static bool path_target_lstat(const char *normalized, struct statbuf *stat) {
    char copy[MAX_PATH];       // find_mount_and_trim_path mutates its argument
    strcpy(copy, normalized);
    struct mount *mount = find_mount_and_trim_path(copy);
    if (mount == NULL)
        return false;
    int err = mount->fs->stat(mount, copy, stat);
    mount_release(mount);
    return err == 0;
}

// Does it name anything at all?
static bool path_target_exists(const char *normalized) {
    struct statbuf stat;
    return path_target_lstat(normalized, &stat);
}

int path_lookup_final(struct fd *at, const char *path_raw, struct statbuf *stat) {
    char path[MAX_PATH];
    int err = path_normalize(at, path_raw, path, N_SYMLINK_NOFOLLOW | N_PARENT_ONLY);
    if (err < 0)
        return err;
    if (!path_target_lstat(path, stat))
        return _ENOENT;
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
    // For an absolute path `at` IS the root and at_path already holds this
    // answer; on fakefs every getpath is a database query, and asking twice
    // was ~30% of the CPU an lstat("/etc") cost (26 us -> 18.5 us).
    char root_path[MAX_PATH];
    if (root != NULL && at == root) {
        strcpy(root_path, at_path);
    } else if (root != NULL) {
        int err = generic_getpath(root, root_path);
        if (err < 0)
            return err;
        assert(path_is_normalized(root_path));
    }

    int err = __path_normalize(root != NULL ? root_path : NULL, at != NULL ? at_path : NULL, path, out, flags, 0);
    if (err < 0)
        return err;

    // N_SLASH_NOT_A_DIR: the caller creates a name that is not a directory,
    // and the caller's name was spelled with a trailing slash, which asks for
    // one. Linux's filename_create() suppresses LOOKUP_CREATE for such a name
    // (`if (last.name[last.len] && !want_dir) create_flags = 0`) and then, on
    // the negative dentry that comes back, answers ENOENT -- "you had / on
    // the end, you've been asking for (non-existent) directory".
    //
    // The order matters as much as the errno. This sits after
    // __path_normalize, so every error the parent walk can raise (a missing
    // parent's ENOENT, a non-directory parent's ENOTDIR, an unsearchable
    // one's EACCES) still answers first, exactly as filename_parentat()'s do.
    // And it sits before N_PARENT_DIR_WRITE below, because Linux only asks
    // may_create() for the parent's write permission afterwards, inside
    // vfs_mknod/vfs_symlink/vfs_link -- so an unwritable parent reports this
    // ENOENT, not EACCES.
    //
    // A name that DOES exist is left alone: the lookup gives a positive
    // dentry and filename_create() answers EEXIST before ever reaching the
    // trailing-slash test, whatever kind of thing is there -- file,
    // directory, or dangling symlink. The caller's own existence check
    // reports that, the same way N_CREATE_EEXIST_FIRST relies on.
    //
    // Without this, mknod("d/fifo/"), mkfifo, symlink and link all created
    // the name WITHOUT the slash and reported success, so a guest could not
    // tell a request for a directory from a request for a fifo.
    if ((flags & N_SLASH_NOT_A_DIR) && path_trailing_slash(path) &&
            !path_target_exists(out))
        return _ENOENT;

    // N_SLASH_UNLINK: unlink() on a name spelled with a trailing slash. Linux
    // has a whole label for it, and it answers three different things:
    //
    //     slashes:
    //             if (d_is_negative(dentry))      error = -ENOENT;
    //             else if (d_is_dir(dentry))      error = -EISDIR;
    //             else                            error = -ENOTDIR;
    //
    // reached from do_unlinkat() on `if (last.name[last.len] || ...)`. The
    // dentry is the one a LOOKUP_PARENT walk's own lookup produced, so no
    // symlink was followed to get it: unlink("link-to-a-dir/") is ENOTDIR, not
    // EISDIR, because the LINK is what the name names.
    //
    // Same seat as N_SLASH_NOT_A_DIR above, and for the same reason: after
    // __path_normalize, so the parent walk's errors still answer first
    // (unlink("unsearchable/f/") is EACCES), and before N_PARENT_DIR_WRITE,
    // because may_delete() only runs inside vfs_unlink() afterwards -- so
    // unlink("unwritable/f/") is ENOTDIR and not EACCES. Measured.
    //
    // Without it the slash was ignored outright and unlink("file/") DELETED
    // the file, which is the one outcome a caller cannot undo.
    if ((flags & N_SLASH_UNLINK) && path_trailing_slash(path)) {
        struct statbuf stat;
        if (!path_target_lstat(out, &stat))
            return _ENOENT;
        return S_ISDIR(stat.mode) ? _EISDIR : _ENOTDIR;
    }

    if (flags & N_PARENT_DIR_WRITE) {
        // out is fully resolved and normalized here (begins with '/' or is
        // empty, no ".", "..", or unresolved symlinks in the final
        // component -- see __path_normalize). Strip the final component to
        // get the parent directory, then require write+execute permission
        // on it, matching Linux's MAY_WRITE|MAY_EXEC check on the parent for
        // any operation that creates or removes a directory entry.
        char parent[MAX_PATH];
        size_t len = strlen(out);
        // The root of a detached mount is a root too: `.` from a cwd there.
        if (len != 0 && mount_staging_point_len(out, len) == len)
            return 0;
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
                // path_target_exists is an lstat, so a dangling symlink still
                // counts as a name that exists and is removable.
                bool target_exists = path_target_exists(out);
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
