#ifndef KERNEL_FOREIGN_EXEC_H
#define KERNEL_FOREIGN_EXEC_H

#include <stdbool.h>
#include <stddef.h>

// Running a program that belongs to another installed root -- one under
// /AOK/roots/<name> -- whose dynamic loader is not in the current root. Linux
// says ENOENT, and so does "off". See kernel/foreign_exec.c.
enum foreign_exec_mode {
    FOREIGN_EXEC_OFF = 0,
    FOREIGN_EXEC_ROOT = 1,   // run it chrooted into its own root
    FOREIGN_EXEC_LIBS = 2,   // run it here, with its root's loader and libraries
};

enum foreign_exec_mode foreign_exec_get_mode(void);
void foreign_exec_set_mode(enum foreign_exec_mode mode);
// "off", "root", "libs"; parse returns -1 for anything else.
const char *foreign_exec_mode_name(enum foreign_exec_mode mode);
int foreign_exec_parse_mode(const char *text, size_t len);

struct fd;
struct fs_info;

// What one exec did, so it can be undone if the exec then fails, and the
// library path the new image's environment needs.
struct foreign_exec {
    struct fd *old_root;   // FOREIGN_EXEC_ROOT: the root and cwd before
    struct fd *old_pwd;
    char *ldpath;          // FOREIGN_EXEC_LIBS: the LD_LIBRARY_PATH to give it
    char *locpath;         // FOREIGN_EXEC_LIBS: its root's locales, for LOCPATH
};

// The program in EXE needs INTERP, which is not in this root. If it lives in
// another root and the mode allows, open the loader the mode's way -- after
// chrooting the task into that root (ROOT), or from that root (LIBS) -- and
// return it. Otherwise ERR_PTR(_ENOENT), as before.
struct fd *foreign_exec_interp(struct fd *exe, const char *interp, struct foreign_exec *fx);

// A path to a program inside another root, /AOK/roots/<name>/rest, that did
// not open as given -- usually an absolute symlink inside that root
// (Alpine's /bin/sh -> /bin/busybox), which resolves against THIS root. True,
// with ROOT and REST split out, when the mode says to look again from inside
// <name>; false when it is off or the path is not of that shape.
bool foreign_exec_split(const char *path, char *root, size_t root_size, const char **rest);
// ROOT mode: bind what a program needs into ROOT and chroot the task into it,
// keeping its cwd when that is inside ROOT. Recorded in FX for undo.
int foreign_exec_enter_root(const char *root, struct foreign_exec *fx);
// LIBS mode: look a path up as if ROOT were /, just for the lookup. Begin
// returns the root to restore (or an ERR_PTR); end restores it.
struct fd *foreign_exec_lookup_root_begin(const char *root);
void foreign_exec_lookup_root_end(struct fd *old_root);

// The exec failed after foreign_exec_interp: put the task's root and cwd back.
void foreign_exec_undo(struct foreign_exec *fx);
// Release what FX holds, after success or failure.
void foreign_exec_done(struct foreign_exec *fx);

// The environment block for the new image: ENVP with LD_LIBRARY_PATH (and
// LOCPATH) set for a LIBS exec -- or, for any other ELF exec, with values an
// earlier LIBS exec set taken back out, so another root's libraries never reach a program that
// did not ask for them. Returns a malloc'd block of NUL-terminated strings
// (count in *count_out), or NULL when ENVP is fine as it is.
char *foreign_exec_env(const struct foreign_exec *fx, const char *envp, size_t envc,
                       size_t *count_out);

#endif
