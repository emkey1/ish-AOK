// zsh as a native program: the seam between iSH-AOK and it.
//
// The bash counterpart, kernel/bash_glue.c, is the model and is worth reading
// first -- it explains why a native program is a C function rather than a
// process. This file is smaller because zsh needs less coaxing at the entry
// point.
//
// The same WARNING that belongs on bash_glue.c belongs here: this file is
// AOK's own code, and tools/check-native-libc.py does not run over it. It does
// include kernel/native_libc.h, so the libc names below are routed the way
// zsh's are -- but anything added here has to be read twice, because a name
// the header does not rewrite reaches the HOST.
//
// WHAT IS NOT DONE, said plainly so nobody has to infer it from behaviour:
//
//  - SEQUENTIAL re-entry on ONE thread. zsh's globals are __thread now, so two
//    zshs on two guest tasks are independent -- which is what makes fork by
//    re-launch work (deps/zsh/Src/aok_fork.c). A second zsh on the SAME task
//    still inherits the first one's globals, because thread-local means
//    per-thread and that is one thread. bash answers this with
//    shell_reinitialize; zsh has no equivalent function and has not been given
//    one. The flag below makes the case visible instead of mysterious.

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "kernel/calls.h"
#include "kernel/native.h"
#include "kernel/native_io.h"
#include "kernel/native_libc.h"
#include "kernel/task.h"

// zsh's function path, found in the guest at run time rather than baked in.
//
// WHY. zsh's fpath is fixed at configure time, and deps/zsh is a tree
// configured for embedding, so Src/zshpaths.h says:
//
//     #define SITEFPATH_DIR "/usr/local/share/zsh/site-functions"
//     #define FPATH_DIR     "/usr/local/share/zsh/5.9.999.3-test/functions"
//
// No guest has either. The prefix is configure's default and the version is
// this git checkout's, so even a rootfs that did use /usr/local would not
// match. The effect is not subtle and is not a corner case -- it is every
// interactive user's first impression:
//
//     % autoload -Uz compinit && compinit
//     compinit: function definition file not found
//
// and the Tab key does nothing, ever.
//
// The functions themselves ARE in the guest; the distro's zsh package ships
// them. Where depends on the distro -- Debian and Devuan use
// /usr/share/zsh/functions/<Category>/..., others use
// /usr/share/zsh/<version>/functions -- so this LOOKS instead of assuming,
// which is also the only version that survives someone swapping rootfs.
//
// Through FPATH rather than a patch to zsh: Src/init.c builds the compiled
// list only when the environment does not already say (the FPATH_NEEDS_INIT
// block), so setting FPATH replaces it outright with no zsh change. And only
// when the user has not set one themselves -- their choice wins over ours.
//
// Every call below is routed by kernel/native_libc.h, which is the whole point:
// an unrouted opendir here would enumerate the MAC's filesystem and hand the
// guest a completion path pointing into iOS.

#define AOK_FPATH_MAX 8192

struct aok_fpath {
    char s[AOK_FPATH_MAX];
    size_t n;
};

static void aok_fpath_add(struct aok_fpath *b, const char *dir) {
    struct stat st;
    size_t len;

    if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode))
        return;
    len = strlen(dir);
    if (b->n + len + 2 > sizeof(b->s))
        return;
    if (b->n)
        b->s[b->n++] = ':';
    memcpy(b->s + b->n, dir, len);
    b->n += len;
    b->s[b->n] = '\0';
}

// The tree and everything under it. Debian puts completions two levels down
// (functions/Completion/Unix), and fpath has to name each directory -- zsh does
// not search recursively at lookup time.
static void aok_fpath_add_tree(struct aok_fpath *b, const char *root, int depth) {
    DIR *d;
    struct dirent *e;

    aok_fpath_add(b, root);
    if (depth <= 0 || !(d = opendir(root)))
        return;
    while ((e = readdir(d)) != NULL) {
        char sub[1024];
        struct stat st;

        if (e->d_name[0] == '.')   // skips . and .. as well as dotfiles
            continue;
        if (snprintf(sub, sizeof(sub), "%s/%s", root, e->d_name) >= (int) sizeof(sub))
            continue;
        if (stat(sub, &st) != 0 || !S_ISDIR(st.st_mode))
            continue;
        aok_fpath_add_tree(b, sub, depth - 1);
    }
    closedir(d);
}

static void aok_zsh_set_fpath(void) {
    struct aok_fpath b;
    struct stat st;
    DIR *d;
    struct dirent *e;

    if (getenv("FPATH"))
        return;

    b.n = 0;
    b.s[0] = '\0';
    aok_fpath_add(&b, "/usr/local/share/zsh/site-functions");
    aok_fpath_add(&b, "/usr/share/zsh/vendor-functions");
    aok_fpath_add(&b, "/usr/share/zsh/vendor-completions");

    if (stat("/usr/share/zsh/functions", &st) == 0 && S_ISDIR(st.st_mode)) {
        aok_fpath_add_tree(&b, "/usr/share/zsh/functions", 3);
    } else if ((d = opendir("/usr/share/zsh")) != NULL) {
        // The versioned layout. Every entry is tried rather than the newest
        // guessed at: a rootfs with two of them is odd but not broken, and
        // "5.9" does not sort against "5.10" the way a human would read it.
        while ((e = readdir(d)) != NULL) {
            char cand[512];

            if (e->d_name[0] == '.')
                continue;
            if (snprintf(cand, sizeof(cand), "/usr/share/zsh/%s/functions",
                         e->d_name) >= (int) sizeof(cand))
                continue;
            if (stat(cand, &st) == 0 && S_ISDIR(st.st_mode))
                aok_fpath_add_tree(&b, cand, 3);
        }
        closedir(d);
    }

    // Nothing found means no zsh functions are installed in this guest. Leave
    // FPATH unset rather than empty: zsh then falls back to its compiled list,
    // which is no worse, and an empty FPATH would look deliberate.
    if (b.n)
        setenv("FPATH", b.s, 1);
}

// zsh's entry point, Src/init.c. NOT Src/main.c, which is a three-line
// wrapper around this and would bring a second main() into the binary; the
// build leaves that file out rather than renaming it the way bash's -Dmain
// does.
//
// The signature differs from bash_main_entry's on purpose: zsh_main takes
// only (argc, argv) and reads the environment through `environ`, which
// kernel/native_libc.h rewrites into a per-task accessor. So the envp the
// dispatcher hands us is already where zsh will look for it, and passing it on
// would be the wrong shape as well as redundant.
int zsh_main(int argc, char **argv);

int native_zsh_main(int argc, char *const argv[], char *const envp[]) {
    (void) envp;   // see zsh_main above: zsh reads the routed `environ`

    // Re-entry on the SAME thread, which is the case the thread-local
    // conversion does not cover. Two zshs on two guest tasks each have their
    // own copy of zsh's globals; two zshs on one task share the one copy, and
    // the second finds the first's module registrations still in place --
    //
    //     zsh:1: name clash when adding hook `exit'
    //     zsh:1: command not found: print
    //
    // because zsh re-adds hooks that are already there and the builtin table
    // ends up in a state where `print` is not a builtin any more.
    //
    // __thread, not a plain static: as a plain static this fired on the FIRST
    // zsh of every subshell, since a re-launched child is a new task and every
    // one of them would have been told it was "the second zsh in one app
    // session". bash's flag is thread-local for the same reason.
    //
    // Said on the guest's OWN stderr rather than only through printk. printk
    // goes to AOK's log, which the person watching a terminal fill up with
    // "name clash" messages is not reading.
    static __thread bool zsh_has_run;
    if (zsh_has_run) {
        native_printf(2, "native zsh: this is the second zsh on this task; its "
                "globals are the first one's and it will misbehave.\n");
    }
    zsh_has_run = true;

    aok_zsh_set_fpath();

    int status = zsh_main(argc, (char **) argv);
    nlibc_flush_std();
    // zsh normally leaves through exit(), which nlibc_exit turns into the
    // guest task's exit -- so this is only reached when zsh_main RETURNS,
    // which is unusual enough to be worth a line in the log.
    printk("native zsh: returned %d\n", status);
    return status;
}

// ------------------------------------------------------- zsh's MULTIOS pump
//
// `echo hi > a > b` and `cat < in1 < in2` are MULTIOS: two or more
// redirections on one descriptor, which is zsh's default. zsh implements them
// by forking a child that is not a shell at all -- it is a byte pump between
// one internal pipe and several real descriptors, holding no shell state of
// any kind. See closemn() in deps/zsh/Src/exec.c, which is the only caller of
// this program and explains the other half of the arrangement.
//
// That fork is the one zsh fork with no re-launch equivalent. A re-launch
// (deps/zsh/Src/aok_fork.c) starts a fresh zsh and hands it this shell's
// serialised state, which is the right way to run a COMMAND and an absurd way
// to copy bytes. Nor can the pump be a host thread: the parent's very next act
// is to CLOSE the descriptors the pump is reading and writing, and a thread
// shares the task's fd table, so the close would take them out from under it.
// What is left is a separate guest TASK running a tiny program, which is this
// one. It is in kernel/native.c's registry like any other native program, so
// exec finds it at /AOK/native/zsh-multio, and the descriptors reach it by
// INHERITANCE -- a spawned guest task gets a copy of the spawner's fd table,
// which is what lets them be named on the command line as bare numbers:
//
//     zsh-multio tee <pipefd> <fd> [<fd> ...]   output: cmd > a > b
//     zsh-multio cat <pipefd> <fd> [<fd> ...]   input:  cmd < a < b
//
// Everything that is NOT one of those descriptors has already been closed by
// closemn's spawn file actions, which is closeallelse()'s job in the forked
// child and matters for the same reason: a pump still holding the write end of
// the pipe it is reading never sees EOF, and the shell waits forever.
//
// Before this existed the whole family of constructs was refused outright --
// and, until the refusal was moved ahead of the redirection loop, refused
// after truncating every target file.

// Upstream zsh's TCBUFSIZE, which lived in Src/exec.c and went with the fork
// it sized. The same number deliberately: the two loops below are a
// transcription of the ones in closemn's forked child, and nobody comparing
// them should have to wonder whether the buffer size is significant.
#define AOK_MULTIO_BUFSIZE 4092

// zsh's write_loop (Src/utils.c) without the zwarn. There is nowhere for a
// diagnostic to go: fd 2 is one of the descriptors closemn closed, and the
// forked child said nothing here either.
static int aok_multio_write_loop(int fd, const char *buf, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        buf += n;
        len -= (size_t) n;
    }
    return 0;
}

static bool aok_multio_fdarg(const char *s, int *out) {
    char *end;
    long v;

    if (s == NULL || *s == '\0')
        return false;
    errno = 0;
    v = strtol(s, &end, 10);
    if (*end != '\0' || errno != 0 || v < 0 || v > 0x7fffffff)
        return false;
    *out = (int) v;
    return true;
}

int native_zsh_multio_main(int argc, char *const argv[], char *const envp[]) {
    (void) envp;   // a byte pump reads no environment

    // A usage error here is an AOK bug, not a user error, and it cannot be
    // reported on stderr because the pump has none. printk puts it in the
    // app's log, which is where someone debugging closemn will be looking.
    if (argc < 4) {
        printk("zsh-multio: need a mode, a pipe fd and at least one target fd\n");
        return 1;
    }
    bool tee = strcmp(argv[1], "tee") == 0;
    if (!tee && strcmp(argv[1], "cat") != 0) {
        printk("zsh-multio: unknown mode '%s'\n", argv[1]);
        return 1;
    }

    int pipefd;
    if (!aok_multio_fdarg(argv[2], &pipefd)) {
        printk("zsh-multio: bad pipe fd '%s'\n", argv[2]);
        return 1;
    }

    int ct = argc - 3;
    int *fds = malloc((size_t) ct * sizeof(*fds));
    char *buf = malloc(AOK_MULTIO_BUFSIZE);
    if (fds == NULL || buf == NULL) {
        printk("zsh-multio: out of memory\n");
        free(fds);
        free(buf);
        return 1;
    }
    for (int i = 0; i < ct; i++) {
        if (!aok_multio_fdarg(argv[3 + i], &fds[i])) {
            printk("zsh-multio: bad target fd '%s'\n", argv[3 + i]);
            free(fds);
            free(buf);
            return 1;
        }
    }

    ssize_t len;
    if (tee) {
        // The tee process. One reader, several writers, and the inner `break`
        // on a failed write leaves only the write loop -- upstream keeps
        // pumping to the remaining targets, so `echo hi > /dev/full > b` still
        // fills b.
        while ((len = read(pipefd, buf, AOK_MULTIO_BUFSIZE)) != 0) {
            if (len < 0) {
                if (errno == EINTR)
                    continue;
                else
                    break;
            }
            for (int i = 0; i < ct; i++)
                if (aok_multio_write_loop(fds[i], buf, (size_t) len) < 0)
                    break;
        }
    } else {
        // The cat process: each source in turn, in the order the redirections
        // were written, because `cat < in1 < in2` is a concatenation and the
        // order is the answer.
        //
        // The EINTR guard is NOT the same as the tee side's, and the asymmetry
        // is upstream's and deliberate: a read interrupted on a TTY means the
        // user did something, and retrying would ignore it, so only a
        // non-tty read is resumed.
        for (int i = 0; i < ct; i++)
            while ((len = read(fds[i], buf, AOK_MULTIO_BUFSIZE)) != 0) {
                if (len < 0) {
                    if (errno == EINTR && !isatty(fds[i]))
                        continue;
                    else
                        break;
                }
                if (aok_multio_write_loop(pipefd, buf, (size_t) len) < 0)
                    break;
            }
    }

    free(fds);
    free(buf);
    // Upstream's child ends with _exit(0) whatever the pump managed to
    // transfer, and the status matters: closemn adds this pid to the job as an
    // AUXILIARY process, so a non-zero status here would be a failure the user
    // never asked about.
    return 0;
}
