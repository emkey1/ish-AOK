// Everything smallclue needs from its host that AOK has to supply itself.
//
// Three groups:
//
//  1. Runtime hooks smallclue declares but expects the embedding program to
//     define (deps/smallclue-shim/core/build_info.h).
//  2. Entry points for the feature groups AOK deliberately does not compile --
//     the checksum applets (need OpenSSL, which AOK does not link) and
//     openrsync. Their applet-table entries still reference these symbols, so
//     each needs a definition that fails honestly rather than a build that
//     fails to link. The ssh family used to be on this list; it is built now,
//     and what is left of it lives in kernel/openssh_glue.c.
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

#include "kernel/native.h"
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

int smallclueRunRsync(int argc, char **argv);
int pscal_openrsync_main(int argc, char **argv);

// The checksum applets live in checksum_app.c, which needs <openssl/evp.h>.
// AOK links no OpenSSL, so these used to be refusals -- but the dependency was
// never really OpenSSL, it was a digest, and deps/smallclue-shim/openssl/evp.h
// now serves that #include out of CommonCrypto. checksum_app.c is compiled
// wherever that is possible, and only where it is not do the refusals below
// exist. -DAOK_HAVE_CHECKSUMS comes from meson.build, from the same variable
// that decides the archive, so exactly one of the two definitions is ever
// linked.
#ifndef AOK_HAVE_CHECKSUMS
int smallclueMd5sumCommand(int argc, char **argv);
int smallclueSha1sumCommand(int argc, char **argv);
int smallclueSha256sumCommand(int argc, char **argv);
int smallclueMd5sumCommand(int argc, char **argv) {
    (void) argc; (void) argv; return smallclue_not_built("md5sum");
}
int smallclueSha1sumCommand(int argc, char **argv) {
    (void) argc; (void) argv; return smallclue_not_built("sha1sum");
}
int smallclueSha256sumCommand(int argc, char **argv) {
    (void) argc; (void) argv; return smallclue_not_built("sha256sum");
}
#endif

// The ssh family (ssh/scp/sftp/ssh-keygen/ssh-copy-id) is real now, from the
// vendored OpenSSH tree; kernel/openssh_glue.c holds both its globals and the
// refusal for a build without the tree.
// No smallclueRunRsync stub here: deps/smallclue/src/openrsync_app.c defines
// the real one UNCONDITIONALLY, and that file is always in the build. Two
// definitions is a duplicate symbol -- GNU ld says so with --start-group, while
// ld64 never pulls the second archive member and stays quiet, which is why this
// only ever showed up on Linux. pscal_openrsync_main below IS ours alone:
// openrsync_app.c only declares it.
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

    // setpgid_self asks for what a forked child does between fork and exec.
    // There is no such window here, so it is described to native_spawn instead
    // and applied on the child's behalf before it starts (kernel/native_io.h).
    // timeout(1) is the caller that wants it, for killing a whole group.
    struct native_spawn_opts opts = {
        .pgid = request->setpgid_self ? 0 : NATIVE_SPAWN_PGID_INHERIT,
    };

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

        // native_env_vector(), NOT `environ`. This file is compiled WITHOUT
        // kernel/native_libc.h force-included -- it is AOK's own code, in
        // libish rather than in the SmallCLUE archive -- so a plain `environ`
        // here is the HOST process's, and children were being handed the Mac's
        // environment: HOME=/Users/mke, __CF_USER_TEXT_ENCODING, and none of
        // the guest's own exports.
        //
        // check-native-libc.py does not cover this file and should not: it is
        // meant to call the host libc. That makes the glue between AOK and a
        // native program the one place the gate cannot help, and so the one
        // place to be deliberate about which side of the seam a name comes
        // from.
        dword_t pid = 0;
        int err = native_spawn_opts(path, attempt->argv, native_env_vector(),
                &opts, &pid);
        if (err >= 0)
            return (pid_t) pid;
        last_err = err < 0 ? -err : err;
    }

    errno = last_err;
    return -1;
}

#ifndef ISH_HAVE_ZLIB
// tar/gzip/gunzip/zcat live in tar_app.c and gzip_app.c, which are excluded
// from the build (meson.build) on a host without zlib, so their applet-table
// entries need definitions here.
//
// Note what this is NOT about. For most of AOK's life these four were stubbed
// on a host that had zlib all along, because the problem was never the link
// line: zlib is a system dylib compiled against the HOST libc, so its
// gzopen/gzread/gzwrite call the host's open/read/write on whatever path or
// descriptor they are handed, and kernel/native_libc.h cannot reach inside a
// prebuilt dylib to redirect them. Gzipping a guest path under /tmp looked
// perfect while doing the entire operation on iOS's /tmp.
//
// deps/smallclue-shim/zlib.h answers that by reimplementing the six gz* calls
// over the redirected open/read/write and leaving compression to zlib's
// deflate/inflate, which touch nothing but the caller's buffers. So these
// stubs now mean only what they say -- no zlib on this host.
int smallclueTarCommand(int argc, char **argv);
int smallclueGzipCommand(int argc, char **argv);
int smallclueGunzipCommand(int argc, char **argv);
int smallclueZcatCommand(int argc, char **argv);

int smallclueTarCommand(int argc, char **argv) {
    (void) argc; (void) argv; return smallclue_not_built("tar");
}
int smallclueGzipCommand(int argc, char **argv) {
    (void) argc; (void) argv; return smallclue_not_built("gzip");
}
int smallclueGunzipCommand(int argc, char **argv) {
    (void) argc; (void) argv; return smallclue_not_built("gunzip");
}
int smallclueZcatCommand(int argc, char **argv) {
    (void) argc; (void) argv; return smallclue_not_built("zcat");
}
#endif
