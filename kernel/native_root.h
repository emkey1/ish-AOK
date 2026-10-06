#ifndef KERNEL_NATIVE_ROOT_H
#define KERNEL_NATIVE_ROOT_H

#include <stdbool.h>

// Native mode: a root with no Linux distribution in it, whose /bin and
// /usr/bin point into /AOK/native. See kernel/native_root.c and
// docs/native_mode_plan.md.

// The uid of the everyday account, and the one "Open Everything as Default
// User" switches to (ISHDefaultUserAccountUID in the app).
#define NATIVE_ROOT_USER_UID 1000

// Bring the root's skeleton up to date: directories, the links into
// /AOK/native, and the /etc files a root needs to be usable. Idempotent, and
// meant to run at every boot of a native root, after /AOK is mounted and
// before the first program starts. Files that are the user's (/etc/passwd,
// /etc/profile, /etc/rc, ...) are written only when missing; a link this
// function made is updated or removed as the build's programs change, and
// anything else at one of its paths is left alone. Returns 0, or a negative
// errno when the root could not be provisioned at all; individual paths that
// fail are skipped rather than ending the boot.
int native_root_provision(void);

// Just the directories (/bin, /etc, /tmp, /var, /run, ...), for the part of a
// boot that runs before /AOK is mounted and expects them: provisioning proper
// needs /AOK/native to link to. native_root_provision() does this too.
int native_root_make_dirs(void);

// Mount the host's own time zone database read-only at /usr/share/zoneinfo,
// unless the root already has one there. The files are TZif on every Apple
// platform, the same format a distro's tzdata installs, so /etc/localtime and
// TZ work exactly as on a distro root -- and they follow the system's tzdata
// updates instead of whatever a copy shipped with. Per boot: mounts are not
// remembered across launches.
int native_root_mount_zoneinfo(void);

// Make TASK -- the first process -- an arm64 one before anything runs. A task
// with no guest image is i386 by default (kernel/task.c), so a native root
// reported `uname -m` as i686 and handed native programs 32-bit struct layouts
// wherever the shim asks guest_abi_is_64bit(). Native programs ARE arm64 host
// code, and native syscalls are dispatched with asm-generic numbers whatever
// the ABI says, so arm64 is the honest answer. Children inherit it.
struct task;
void native_root_adopt_abi(struct task *task);

// Whether the root has an account at NATIVE_ROOT_USER_UID.
bool native_root_has_default_user(void);

// Whether the first-start prompt for that account was answered with Skip
// (/etc/aok-native-user-skipped exists).
bool native_root_default_user_skipped(void);
// Record that answer, so the prompt is not shown again.
int native_root_skip_default_user(void);

// Whether NAME is acceptable as a new account name: [a-z_][a-z0-9_-]{0,31},
// and not one /etc/passwd or /etc/group already has.
bool native_root_user_name_valid(const char *name);

// Create the everyday account: passwd, group and shadow lines at
// NATIVE_ROOT_USER_UID, membership of `users` and `sudo`, shell native zsh, and
// a 0700 home directory it owns. PASSWORD is the plain text to hash ($6$), or
// NULL/empty for no password (shadow field "!"; the account still opens through
// `login -f`). Returns 0 or a negative errno: _EINVAL for a bad name, _EEXIST
// when the name or the uid is taken.
int native_root_add_user(const char *name, const char *password);
// Every boot, every root: /etc/passwd shells naming a native program this
// build lacks are repointed. Returns how many changed, or an error.
int native_root_repair_login_shells(void);

#endif
