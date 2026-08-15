// Everything smallclue needs from its host that AOK has to supply itself.
//
// Three groups:
//
//  1. Runtime hooks smallclue declares but expects the embedding program to
//     define (deps/smallclue-shim/core/build_info.h).
//  2. Entry points for the feature groups AOK deliberately does not compile --
//     OpenSSH (needs the vendored tree), the checksum applets (need OpenSSL,
//     which AOK does not link), and openrsync. Their applet-table entries
//     still reference these symbols, so each needs a definition that fails
//     honestly rather than a build that fails to link.
//  3. smallcluePlatformSpawn, which is how a smallclue applet starts another
//     program when there is no fork() to be had (deps/smallclue/src/spawn.h).

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/native_io.h"

extern char **environ;
#include "kernel/task.h"

// ---------------------------------------------------------------- 1. hooks

bool pscalRuntimeStderrIsInteractive(void) {
    // Used only to decide whether to emit progress/ANSI output. A guest fd is
    // not something the host can answer for, and guessing "yes" would corrupt
    // piped output, so the conservative answer is the right one.
    return false;
}

const char *pscal_program_version_string(void) {
    return "smallclue (iSH-AOK native)";
}

// ------------------------------------------------- 2. unbuilt feature groups

static int smallclue_not_built(const char *what) {
    native_printf(2, "%s: not built into this iSH-AOK\n", what);
    return 127;
}

int smallclueMd5sumCommand(int argc, char **argv);
int smallclueSha1sumCommand(int argc, char **argv);
int smallclueSha256sumCommand(int argc, char **argv);
int smallclueRunSsh(int argc, char **argv);
int smallclueRunScp(int argc, char **argv);
int smallclueRunSftp(int argc, char **argv);
int smallclueRunSshKeygen(int argc, char **argv);
int smallclueRunSshCopyId(int argc, char **argv);
int smallclueRunRsync(int argc, char **argv);
int pscal_openrsync_main(int argc, char **argv);

// The checksum applets live in checksum_app.c, which needs <openssl/evp.h>.
// AOK links no OpenSSL (its crypto accelerator is self-contained), so these
// are out until that changes.
int smallclueMd5sumCommand(int argc, char **argv) {
    (void) argc; (void) argv; return smallclue_not_built("md5sum");
}
int smallclueSha1sumCommand(int argc, char **argv) {
    (void) argc; (void) argv; return smallclue_not_built("sha1sum");
}
int smallclueSha256sumCommand(int argc, char **argv) {
    (void) argc; (void) argv; return smallclue_not_built("sha256sum");
}

// The ssh family needs smallclue's vendored OpenSSH tree, which is a heavy
// dependency for three applets and is deliberately skipped for now.
int smallclueRunSsh(int argc, char **argv) {
    (void) argc; (void) argv; return smallclue_not_built("ssh");
}
int smallclueRunScp(int argc, char **argv) {
    (void) argc; (void) argv; return smallclue_not_built("scp");
}
int smallclueRunSftp(int argc, char **argv) {
    (void) argc; (void) argv; return smallclue_not_built("sftp");
}
int smallclueRunSshKeygen(int argc, char **argv) {
    (void) argc; (void) argv; return smallclue_not_built("ssh-keygen");
}
int smallclueRunSshCopyId(int argc, char **argv) {
    (void) argc; (void) argv; return smallclue_not_built("ssh-copy-id");
}
int smallclueRunRsync(int argc, char **argv) {
    (void) argc; (void) argv; return smallclue_not_built("rsync");
}
int pscal_openrsync_main(int argc, char **argv) {
    (void) argc; (void) argv; return smallclue_not_built("openrsync");
}

// -------------------------------------------------------- 3. platform spawn

#include "../deps/smallclue/src/spawn.h"

// SmallCLUE's spawn helper (deps/smallclue/src/spawn.h) dispatches here when
// SMALLCLUE_PLATFORM_SPAWN is defined, which it is for this build: there is no
// fork() to be had inside one app process, so a child becomes a real AOK task
// instead (native_spawn, kernel/native_io.h).
//
// The attempt list is tried in order, mirroring the exec cascade the POSIX
// implementation runs in the child. Reporting -1 only when EVERY attempt fails
// keeps "never started" distinguishable from "ran and exited 127", which is
// the distinction spawn.h exists to preserve.
pid_t smallcluePlatformSpawn(const SmallclueSpawnRequest *request) {
    if (request == NULL || request->attempts == NULL || request->attempt_count == 0) {
        errno = EINVAL;
        return -1;
    }

    int last_err = ENOENT;
    for (size_t i = 0; i < request->attempt_count; i++) {
        const SmallclueSpawnAttempt *attempt = &request->attempts[i];
        if (attempt->file == NULL || attempt->argv == NULL)
            continue;

        char resolved[MAX_PATH];
        const char *path = attempt->file;
        if (attempt->search_path && strchr(path, '/') == NULL) {
            if (native_path_search(path, resolved, sizeof(resolved)) < 0) {
                last_err = ENOENT;
                continue;
            }
            path = resolved;
        }

        dword_t pid = 0;
        int err = native_spawn(path, attempt->argv, environ, &pid);
        if (err >= 0)
            return (pid_t) pid;
        last_err = err < 0 ? -err : err;
    }

    // setpgid_self is not honoured yet: the child is started already running,
    // so there is no point between fork and exec to place it in its own group.
    // timeout(1) is the caller that wants it, for killing a whole group.
    errno = last_err;
    return -1;
}
