# Wayland in native mode: a desktop from a second root

Asked 2026-11-22. A native root (docs/native_mode_plan.md) has no
distribution, so it has no compositor and cannot install one. This gives it a
desktop without changing what it is: the booted root stays native, and the
desktop runs, emulated, out of a second installed root.

**Since 2026-11-22 the second root is the native root's "companion"**, named
`Devuan`, recorded in `/etc/aok-companion-root`, and prepared by
`setup-companion.sh`: apt works in it too
(opt/AOK/docs/native-companion.md). `setup-native-wayland.sh` runs that and
then installs the desktop. Where this note says "the Wayland root", "Wayland"
as a name, or `/etc/aok-wayland-root`, read those. The companion also asks for
`etc/hosts`, `etc/hostname`, `etc/resolv.conf` and `timezone` in its
`/etc/aok-binds`, a program started from a shared directory keeps it as its
cwd, and a script that lives in another root runs inside it.

It replaces the idea queued in TODO.md of shipping a self-contained Wayland
stack under /AOK/bundled. That would put the stack in the app for everyone;
this downloads it only for people who ask, and uses pieces that already exist.

## What the user does

```sh
sudo sh /AOK/tools/setup-native-wayland.sh      # once
```

or Settings -> Wayland Desktop -> Install, which runs the same thing. Then the
Wayland window works as it does on a distribution root.

## The pieces

**The root.** Devuan 6 arm64 from the catalogue (`devuan6arm64`: arm64 is the
fastest guest, and Devuan is the root the desktop scripts are tested on),
installed with manage-roots.sh and renamed `Wayland`. It is an ordinary,
visible root: Filesystems lists it, it can be booted, removed or renamed.
`--root NAME` uses a Devuan arm64 root that is already installed instead.

**Which root it is** is a file in the native root, `/etc/aok-wayland-root`,
holding the name. Rename the root and that file is what to edit.

**Getting in.** A program named by its path in another root already runs
chrooted there (kernel/foreign_exec.c, "Run Inside Their Root"), for any user,
with /proc, /sys, /dev, /run and /AOK/{native,docs,tools} bound in. So every
desktop script, started in a native root, begins by running itself again as
`/AOK/roots/Wayland/bin/sh /AOK/tools/<script>`. The app needs no change to
start a session: it still runs `sh /AOK/tools/start-wayland.sh`.

Measured (CLI, 2026-11-22): a native shell's `exec` of a program in another
root is a child it waits for, not a replacement, and a SIGHUP sent to that
native shell reaches the child. So the app's hang-up of the session's process
still ends the desktop, for root and through `su`.

It needs Programs From Other Roots to be Run Inside Their Root, the default.
The scripts check and say so otherwise.

**Home and /tmp.** New in the kernel: a root can ask for more of the booted
root to be bound into it when it is entered. `/etc/aok-binds` in that root
lists directories, one a line, from a fixed set (`home`, `tmp`, `mnt`,
`media`, `srv`). The Wayland root asks for `home` and `tmp`:

- `home`, so the desktop's terminal and file manager see the files you have,
  not an empty second home;
- `tmp`, so `/tmp/ish-display.ready`, which the app polls in the booted root,
  is the file the session writes. Nothing else has to cross the boundary.

mount-root.sh honours the same file.

**The same account.** With /home shared, a file's owner has to mean the same
person on both sides, so setup gives the Wayland root each native account it is
asked for with the same name, uid, gid, home and shell, and its shadow entry
and sudoers rule, so `sudo` inside the desktop asks the same question.

**DNS.** Setup copies the booted root's /etc/resolv.conf. On a device its first
server is AOK's relay, 127.0.0.53, which follows network changes and answers
in every root; the scripts refresh the copy whenever they run as root.

**Packages.** `setup-wayland.sh` runs inside the root, as on any Devuan. The
root was never booted, so setup adds `/usr/sbin/policy-rc.d` (exit 101), the
usual way to stop package scripts starting services in a chroot.

**In the desktop.** The terminal runs the account's shell, native zsh, since
/AOK/native is bound in; `/etc/profile.d/10-aok-native.sh` puts the native
programs on PATH after Devuan's. Graphical programs come from the Wayland
root: `sudo apt install ...` in the desktop's terminal, or
`mount-root.sh Wayland -- apt install ...` from outside.

## What changes

- `opt/AOK/tools/wayland-root.sh` (new, sourced): finds the root, checks it,
  re-runs the calling script inside it.
- `opt/AOK/tools/setup-native-wayland.sh` (new): installs and prepares the
  root, then runs setup-wayland.sh in it.
- start-wayland.sh, select-desktop.sh, setup-wayland.sh, setup-wayfire.sh,
  setup-xfce.sh, setup-wayland-extras.sh, setup-games.sh, setup-gpu.sh,
  setup-locale.sh: the re-run at the top. In a native root with no Wayland
  root, the setup scripts install one first; start-wayland.sh says how.
- kernel/foreign_exec.c: `/etc/aok-binds`.
- The app: the desktop setup page's wording in native mode.
- opt/AOK/docs: native-mode.md, wayland docs, roots.md.

## Costs, stated

- The desktop is emulated: compositor, panel and every graphical program.
  Only what comes from /AOK/native runs natively.
- A second root, a few hundred MB with a desktop in it.
- The GPU path (setup-gpu.sh) goes through the same re-run and /dev is bound,
  but it is not tested in this arrangement yet.

## Tests

- CLI: a native root with a Devuan arm64 desktop root at /AOK/roots/W
  (ISH_FAKE_MNT2, as native_mode.sh --foreign-exec does): setup with
  `--root W`, the account and binds, then a real session -- start-wayland.sh
  from the native side until the ready file appears, and a hang-up that ends
  it.
- Device: the whole of it on the A10X, install included.
