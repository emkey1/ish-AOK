#ifndef KERNEL_GUEST_SSHD_H
#define KERNEL_GUEST_SSHD_H

#include <stdbool.h>

// sshd for native mode: three static aarch64 guest binaries the app carries,
// unpacked into the root at boot. kernel/guest_sshd.c says why they are
// emulated guest ELFs and not native programs the way ssh, scp, sftp and
// ssh-keygen are.

// The bundled archive. fs/aok.c serves it at /AOK/tools/<name>, read from the
// app bundle on a device and from the repository under the CLI;
// tools/build-guest-sshd.sh makes it. Changing this name means changing the
// Resources phase in iSH-AOK.xcodeproj too, or a device finds nothing.
#define GUEST_SSHD_ARCHIVE_NAME "guest-sshd-aarch64.tar.lzma"
#define GUEST_SSHD_ARCHIVE_PATH "/AOK/tools/" GUEST_SSHD_ARCHIVE_NAME

// Holds the build timestamp of the installed copy, so a newer build replaces
// it and an unchanged one does not pay to decompress 21 MB at every boot.
#define GUEST_SSHD_STAMP "/etc/aok-sshd-build"

// Whether the user has asked for sshd. /etc/rc.d/S50sshd starts nothing
// without it; the app's Settings toggle writes and removes it. A plain file
// rather than a setting the kernel reads, so that it is inspectable from the
// guest and `touch` works as well as the toggle does.
#define GUEST_SSHD_ENABLED "/etc/aok-sshd-enabled"

// Unpack the archive into /usr/sbin and /usr/libexec when the installed copy
// is missing or older than this build; with force, always. Returns 0 when the
// binaries are in place, nothing-to-do included, or a negative errno: _ENOENT
// when this build carries no archive, _ENOSYS where the platform has no LZMA
// decoder (see host_lzma_decompress in platform/platform.h).
int guest_sshd_install(bool force);

// Whether all three binaries are installed and came from this build.
bool guest_sshd_installed(void);

// Whether this root has been asked to run sshd. This, not a preference in the
// app, is where the answer lives: it is a property of the filesystem, and a
// user who removes the file from a shell has turned sshd off as surely as the
// switch would.
bool guest_sshd_enabled(void);

// Write that setting into the booted root: GUEST_SSHD_ENABLED is created or
// removed, and when enabled the port goes into
// /etc/ssh/sshd_config.d/10-aok-port.conf, which /etc/ssh/sshd_config includes
// before its own defaults. Nothing starts or stops here: at the next boot,
// provisioning unpacks the binaries and then /etc/rc runs S50sshd, so one
// relaunch is enough.
//
// Everything goes through the guest VFS, so the CALLER must have a usable task
// current (+[AppDelegate pushUsableInitTaskAsCurrent:] in the app). Returns 0
// or a negative errno.
int guest_sshd_set_enabled(bool enabled, unsigned port);

#endif
