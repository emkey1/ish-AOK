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

// STAGED alongside the sc_exec* family in kernel/smallclue_shim.c: this is
// where a SmallCLUE applet's child becomes a real guest task
// (task_create_ -> copy_task -> do_execve -> task_start, with do_wait on the
// parent side; kernel/init.c's boot-command launcher is the working example
// of that shape). Until then it fails honestly rather than silently doing
// something host-side.
pid_t smallcluePlatformSpawn(const SmallclueSpawnRequest *request) {
    (void) request;
    errno = ENOSYS;
    return -1;
}
