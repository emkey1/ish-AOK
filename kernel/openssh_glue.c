// AOK's side of the seam for the ssh family (ssh, scp, sftp, ssh-keygen,
// ssh-copy-id).
//
// The programs themselves are OpenSSH, vendored at
// deps/smallclue/third-party/openssh and compiled in as HOST code like every
// other native program (kernel/native.h); the launcher that stands between
// them and SmallCLUE's applet table -- setjmp/longjmp around fatal() and
// exit(), so a native program cannot kill the app -- is
// deps/smallclue/src/openssh_app.c. Only two things are left for AOK:
//
//  1. Two globals OpenSSH's client sources were patched to share. scp.c,
//     sftp.c and sftp-client.c each carried their own file-static
//     `interrupted` and `showprogress`; with all three linked into one binary
//     they were #defined onto these instead, so a ^C during an scp is seen by
//     whichever transfer is actually running. Nobody defines them, so AOK
//     does.
//
//  2. The honest refusal when the tree is not there. meson.build passes
//     -DAOK_HAVE_OPENSSH exactly when it built the vendored tree, so this
//     file and libopenssh.a cannot disagree about whether ssh exists.
//
// WHAT THIS BUILD OF SSH CAN AND CANNOT DO. OpenSSH is configured
// --without-openssl --without-zlib, because AOK links no libcrypto and no
// zlib and the Xcode app targets link the meson archives by name with nothing
// else on the line. That leaves OpenSSH's own bundled crypto, which is
// ED25519 AND CHACHA20-POLY1305 ONLY: RSA and ECDSA keys do not work, and
// `ssh-keygen -t rsa` refuses. Someone will bring an existing id_rsa and be
// puzzled -- this is why.

#include "kernel/native_io.h"

#include <signal.h>

volatile sig_atomic_t pscal_openssh_interrupted = 0;
int pscal_openssh_showprogress = 1;

#ifndef AOK_HAVE_OPENSSH

// deps/smallclue/third-party/openssh is not populated (or not configured), so
// deps/smallclue/src/openssh_app.c is out of the build and the applet table's
// entries need definitions. Populate and configure it with:
//
//     cd deps/smallclue/third-party/openssh
//     ./configure --without-openssl --without-zlib --sysconfdir=/etc/ssh
//
// and then `meson setup --reconfigure build`, because whether the tree exists
// is decided at configure time and ninja alone will not notice it appear.
static int openssh_not_built(const char *what) {
    native_printf(2, "%s: not built into this iSH-AOK\n", what);
    return 127;
}

int smallclueRunSsh(int argc, char **argv);
int smallclueRunScp(int argc, char **argv);
int smallclueRunSftp(int argc, char **argv);
int smallclueRunSshKeygen(int argc, char **argv);
int smallclueRunSshCopyId(int argc, char **argv);

int smallclueRunSsh(int argc, char **argv) {
    (void) argc; (void) argv; return openssh_not_built("ssh");
}
int smallclueRunScp(int argc, char **argv) {
    (void) argc; (void) argv; return openssh_not_built("scp");
}
int smallclueRunSftp(int argc, char **argv) {
    (void) argc; (void) argv; return openssh_not_built("sftp");
}
int smallclueRunSshKeygen(int argc, char **argv) {
    (void) argc; (void) argv; return openssh_not_built("ssh-keygen");
}
int smallclueRunSshCopyId(int argc, char **argv) {
    (void) argc; (void) argv; return openssh_not_built("ssh-copy-id");
}

#endif  // !AOK_HAVE_OPENSSH
