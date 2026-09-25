#ifndef PATH_H
#define PATH_H

struct fd;
struct statbuf;

#define AT_PWD (struct fd *) -2

// Whether a symlink SPELLED AS THE FINAL COMPONENT is followed. Every other
// component is followed either way. NOFOLLOW still RESOLVES that component --
// it looks the name up and checks what it is -- it just stops at the link
// rather than chasing it. A caller that must not resolve it at all adds
// N_PARENT_ONLY below.
#define N_SYMLINK_FOLLOW 1
#define N_SYMLINK_NOFOLLOW 2
// Resolve an absolute path against the REAL filesystem root, ignoring the
// task's chroot. For re-resolving a stored, already-normalized guest path
// (procfd magic-link reopen, fsmount's detached-mount staging dir): those
// strings already carry any chroot prefix, so running them through the
// chroot-aware anchor again would double-apply it -- inside a chroot that
// meant ENOENT/EACCES for perfectly valid paths.
#define N_REALROOT 8
// Paired with N_PARENT_DIR_WRITE by the operations that CREATE a new name
// (mkdir, symlink, link, mknod). Linux looks the final component up before
// checking whether the parent may be written -- filename_create() returns
// -EEXIST for a name already there, and only vfs_mkdir/vfs_link then ask
// may_create() for permission -- so an existing target reports EEXIST, not
// EACCES. Not for unlink/rmdir/rename: there an existing target is the point
// of the call, so the permission check is what governs and must come first.
// Linux's MAXSYMLINKS: how many symlinks one path resolution may follow.
#define MAX_SYMLINKS 40

#define N_CREATE_EEXIST_FIRST 16
// The mirror image, for the operations that REMOVE a name (unlink, rmdir, and
// rename's SOURCE). Linux looks the final component up before asking
// may_delete() for permission, so a name that is not there reports ENOENT even
// when the parent is unwritable -- do_unlinkat() and do_rmdir() both bail on a
// negative dentry first. Without this, `rm -f /unwritable/nonexistent` answered
// EACCES where Linux answers success, because rm -f suppresses ENOENT and
// nothing else. Not for rename's DESTINATION: a missing destination is the
// ordinary case there, and the permission error is the right answer.
#define N_REMOVE_ENOENT_FIRST 64
// Refuse to traverse a symlink anywhere in the path, final component
// included, answering ELOOP instead of following it. openat2's
// RESOLVE_NO_SYMLINKS, which exists so a caller can open a path it does not
// control without a symlink in it redirecting the open somewhere else.
#define N_NO_SYMLINKS 32
// Require write+execute permission on the resolved parent directory of the
// final path component. Only correct for callers where the operation always
// creates or removes a directory entry regardless of whether the final
// component itself already exists (mkdir, rmdir, rename, symlink, link,
// unlink, mknod). generic_openat's O_CREAT case is NOT one of those -- it
// commonly targets an existing file where O_CREAT is passed defensively and
// only the target file's own permissions matter, not the parent's -- so it
// must not use this flag and instead performs its own check gated on whether
// it actually created a new entry. See generic_openat in fs/generic.c.
#define N_PARENT_DIR_WRITE 4
// A trailing slash is a request for a DIRECTORY, and these two say what the
// caller does about that. Both are decided after the parent has been walked
// and before the final component is looked at, which is where Linux decides
// them; see the comments at the use sites in fs/path.c.
//
// N_SLASH_NOT_A_DIR: the caller creates a name that is not a directory
// (mknod, mkfifo, symlink, link's new name, and a unix socket bind, which
// Linux routes through vfs_mknod too). A name spelled with a trailing slash
// that does not already exist is ENOENT. Not for mkdir, whose whole business
// is the directory a trailing slash asks for.
#define N_SLASH_NOT_A_DIR 128
// N_SLASH_EISDIR: open(O_CREAT). A trailing slash is EISDIR whether or not
// the name exists, and whatever kind of thing is there.
#define N_SLASH_EISDIR 256
// N_PARENT_ONLY: Linux's LOOKUP_PARENT. Walk everything the final component is
// reached THROUGH and stop: do not look the component itself up, do not follow
// a symlink there, and do not spend a trailing slash on it. The create and
// remove families resolve their name this way -- filename_create(),
// do_unlinkat(), do_rmdir(), do_renameat2() all call filename_parentat() --
// and then apply their OWN rule to the leftover slash, which is why those
// rules differ from each other and from a plain lookup's.
//
// Without this, N_SYMLINK_NOFOLLOW had to mean both things at once, and the
// resolution could only serve one of them: it skipped the final component
// entirely, so the must-be-a-directory check that spends a trailing slash
// never ran for ANY nofollow caller. lstat("file/") succeeded and
// unlink("file/") deleted the file. Running that check for every nofollow
// caller instead is the other wrong answer -- mkdir("dangling-link/") would
// follow the link and create its target where Linux says EEXIST.
#define N_PARENT_ONLY 512
// N_SLASH_UNLINK: unlink() on a name spelled with a trailing slash, which is
// its own answer again -- do_unlinkat()'s `slashes:` label: EISDIR for a
// directory, ENOENT for a name that is not there, ENOTDIR for anything else.
#define N_SLASH_UNLINK 1024
// N_DETACHED_OK: the walk may enter MOUNT_STAGING_DIR (kernel/fs.h), where
// detached mounts are parked. No path the guest spells may: on Linux nothing
// reaches a detached mount by name, only through a descriptor already in it.
// So this is for walks that ARE that descriptor -- fsmount() opening the
// mount it made, and a /proc/<pid>/{cwd,root,fd/N} link being followed, which
// Linux jumps through to the file itself -- and fs/path.c sets it for itself
// on a relative symlink inside a detached mount, whose target is re-walked
// from the top.
#define N_DETACHED_OK 2048

// Normalizes the path specified and writes the result into the out buffer.
//
// Normalization means:
//  - prepending the current or root directory
//  - converting multiple slashes into one
//  - resolving . and ..
//  - resolving symlinks, skipping the last path component if the follow_links
//    argument is true
// The result will always begin with a slash or be empty.
//
// If the normalized path plus the null terminator would be longer than
// MAX_PATH, _ENAMETOOLONG is returned. The out buffer is expected to be at
// least MAX_PATH in size.
//
// at is the file descriptor to use as a base to interpret relative paths. If
// at is AT_PWD, uses current->pwd (with appropriate locking).
int path_normalize(struct fd *at, const char *path, char *out, int flags);

// Is the final component of `path` "." or ".."? 1 for ".", 2 for "..", else 0.
//
// Linux answers this in filename_create()/do_unlinkat() BEFORE checking any
// permission on the parent, because such a component can never name an entry
// to create or remove. AOK checked the parent's write permission first, and
// the parent of a normalized "." is its GRANDparent -- so `ln -s /bin/sh .` in
// a user's own home reported EACCES (parent /home is root-owned) while root
// got the real EEXIST. GNU ln keys off exactly that EEXIST to retry as "link
// into this directory", so the command worked for root and failed for every
// normal user.
int path_final_dot(const char *path);

// The parent walk that has to happen BEFORE path_final_dot's answer is given:
// Linux's filename_create()/do_unlinkat()/do_rmdir() all run
// filename_parentat() first and only then look at the final component's type,
// so a parent that is missing, is a regular file, or cannot be searched
// reports its own error and this rule reports nothing. AOK answered the rule
// with nothing resolved at all, which made mknod("gone/.") EEXIST where Linux
// says ENOENT. Returns 0 if the walk arrived, else the error to report.
// Only meaningful for a path whose final component is "." or ".."; see the
// definition in fs/path.c.
int path_parent_walk(struct fd *at, const char *path_raw);

// Was `path` spelled with a trailing slash -- Linux's
// `nd->last.name[nd->last.len]`, the character sitting where the final
// component ends? The whole trailing-slash family of rules turns on it.
bool path_trailing_slash(const char *path);

// Look the FINAL component up the way a LOOKUP_PARENT caller does after its
// walk -- lookup_one_qstr_excl() -- and report what kind of thing is there: no
// symlink followed, no trailing slash spent, a dangling symlink counted as a
// name that IS there. Returns 0 with `stat` filled, the parent walk's error,
// or _ENOENT for a negative dentry. Only rename needs it, because its
// trailing-slash rule is the one that reads the type of a name OTHER than the
// one the slash is on; see generic_renameat.
int path_lookup_final(struct fd *at, const char *path_raw, struct statbuf *stat);

bool path_is_normalized(const char *path);

// Helper function for iterating through a normalized path.
//
// The *path pointer is advanced to point to the next /, and the next path
// component is copied to component. component must point to a buffer large
// enough to hold a string of MAX_NAME characters.
//
// If the next path component was successfully copied, returns true; otherwise
// returns false. If an error occurred, *err is set to the error code.
// Otherwise, the end of the path has been reached.
bool path_next_component(const char **path, char *component, int *err);

#endif
