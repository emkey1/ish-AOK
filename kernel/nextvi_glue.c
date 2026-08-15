// Running Nextvi more than once in one process.
//
// Nextvi is the editor behind `vi` (deps/smallclue/third-party/nextvi). It is
// an ordinary Unix program: it runs once, and the process exits. Inside AOK it
// does not -- `vi` is a native program (kernel/native.h), so the editor is a C
// function called on a thread of the app's own process, and typing `vi` a
// second time calls it again with every global exactly as the first call left
// them.
//
// The symptom is the one reported from a device: the first `vi` works, and
// every one after it draws the file and exits instantly. `xquit` is what does
// it. It is the editor's "stop now" flag, `:q` leaves it at 1, and vi()'s loop
// is `while (!xquit)` -- so the second run never enters the loop at all, and
// returns abs(1)-1, which is success. Nothing is printed and the status is 0,
// which is why it looks like nothing happened rather than like a failure.
//
// So the build renames Nextvi's entry point to nextvi_editor_run (meson.build)
// and this supplies the nextvi_main_entry that SmallCLUE actually calls,
// putting back the state a fresh process would have had.
//
// Why here rather than in Nextvi
// ------------------------------
// Nextvi has a nextvi_reset_state() hook meant for exactly this, and it is an
// empty function -- SmallCLUE's nextvi_app.c even carries a comment saying it
// is called from inside the entry point, which it is not. Filling it in is the
// better long-term home, and it is the only home that can reach the file-scope
// statics in vi.c's translation unit. But the requirement is AOK's: no other
// build of Nextvi runs it twice. Keeping the workaround on this side means the
// submodule stays at an unmodified upstream commit.
//
// What is reset, and why only this much
// -------------------------------------
// Everything here is per-invocation by construction -- state that a run
// establishes for itself and that means nothing to the next one. Deliberately
// NOT a sweep of Nextvi's ~97 externally visible globals: most are
// configuration, or heap pointers whose owners would then leak or dangle, and
// resetting one of those would trade a visible bug for a silent one.
//
// xquit alone accounts for the reported failure; the input cursors and the
// macro-execution state are here because leftover keystrokes from one run
// being consumed by the next is the same class of bug, and each of these is
// zero in a fresh process.
//
// Not reset, and worth knowing: Nextvi allocates `ibuf` on every entry without
// freeing the previous one, so each `vi` leaks its input buffer. That is a
// Nextvi bug rather than something this can paper over.

int nextvi_editor_run(int argc, char **argv);

// Nextvi's own globals, declared rather than included: vi.h is written to be
// read inside that translation unit and drags the rest of the editor with it.
extern int xquit;                                  // "stop now"; ex.c
extern unsigned int ibuf_pos, ibuf_cnt, icmd_pos;  // input cursors; term.c
extern unsigned int texec, tn;                     // macro execution; term.c

int nextvi_main_entry(int argc, char **argv) {
    xquit = 0;
    ibuf_pos = 0;
    ibuf_cnt = 0;
    icmd_pos = 0;
    texec = 0;
    tn = 0;
    return nextvi_editor_run(argc, argv);
}

// ------------------------------------------------------- the shell escape
//
// `!!` and `:!cmd` run a command with pipes on its stdin and stdout, which
// Nextvi does with fork, dup2 and execvp. The fork is the problem: a native
// program is one thread of the app's process, and fork() cannot return twice
// into C that is not being duplicated -- so upstream Nextvi reports "fork
// failed" for the shell escape, which is what it does inside AOK today.
//
// A Nextvi whose term.c carries the NEXTVI_PLATFORM_SPAWN seam calls this
// instead, and it is the same work done from the parent side rather than in a
// child that cannot exist (kernel/native_io.h). With it, `!!tr a-z A-Z` filters
// the line exactly as a host build of the same Nextvi does.
//
// This is AOK's half of that interface and is compiled whether or not the
// submodule currently has the other half -- meson.build detects that and says
// so at configure time, rather than this quietly deciding for itself.
//
// pipe_in and pipe_out are the two-element arrays pipe() filled in, or NULL
// where Nextvi did not want that direction. The child reads from pipe_in[0]
// and writes to pipe_out[1]; the other end of each is the editor's, and the
// child must not keep it or the editor never sees EOF. stdout and stderr both
// go to pipe_out[1], as the forked child did.

#include <errno.h>
#include <string.h>

#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/native.h"
#include "kernel/native_io.h"

int nextvi_platform_spawn(char **argv, const int *pipe_in, const int *pipe_out) {
    if (argv == NULL || argv[0] == NULL) {
        errno = EINVAL;
        return -1;
    }

    // Written in the order the forked child ran them: redirect first, then drop
    // both ends of both pipes. Closing before the dup2s would take away the
    // descriptor being copied.
    struct native_spawn_action actions[7];
    size_t n = 0;
    if (pipe_in != NULL)
        actions[n++] = (struct native_spawn_action) {
            .kind = NATIVE_SPAWN_DUP2, .fd = 0, .from = pipe_in[0] };
    if (pipe_out != NULL) {
        actions[n++] = (struct native_spawn_action) {
            .kind = NATIVE_SPAWN_DUP2, .fd = 1, .from = pipe_out[1] };
        actions[n++] = (struct native_spawn_action) {
            .kind = NATIVE_SPAWN_DUP2, .fd = 2, .from = pipe_out[1] };
    }
    if (pipe_in != NULL) {
        actions[n++] = (struct native_spawn_action) {
            .kind = NATIVE_SPAWN_CLOSE, .fd = pipe_in[0] };
        actions[n++] = (struct native_spawn_action) {
            .kind = NATIVE_SPAWN_CLOSE, .fd = pipe_in[1] };
    }
    if (pipe_out != NULL) {
        actions[n++] = (struct native_spawn_action) {
            .kind = NATIVE_SPAWN_CLOSE, .fd = pipe_out[0] };
        actions[n++] = (struct native_spawn_action) {
            .kind = NATIVE_SPAWN_CLOSE, .fd = pipe_out[1] };
    }
    struct native_spawn_opts opts = {
        .pgid = NATIVE_SPAWN_PGID_INHERIT,
        .actions = actions,
        .action_count = n,
    };

    // execvp semantics, against the GUEST's PATH -- the host's would find the
    // Mac's binaries.
    char resolved[MAX_PATH];
    const char *path = argv[0];
    if (strchr(path, '/') == NULL) {
        if (native_path_search(path, resolved, sizeof(resolved)) < 0) {
            errno = ENOENT;
            return -1;
        }
        path = resolved;
    }

    dword_t pid = 0;
    int err = native_spawn_opts(path, argv, native_env_vector(), &opts, &pid);
    if (err < 0) {
        errno = err == _ENOENT ? ENOENT : (err == _ENOMEM ? ENOMEM : EIO);
        return -1;
    }
    return (int) pid;
}
