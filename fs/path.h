#ifndef PATH_H
#define PATH_H

#define AT_PWD (struct fd *) -2

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
