# Filesystems, /AOK/roots, and chrooting between architectures

iSH-AOK can have several Linux root filesystems installed at once — Alpine
or Devuan, in i386, amd64, arm64, or riscv64 flavors — and can run
processes from more than one of them at the same time, in the same booted
session, via `chroot`. This document covers the on-device mechanics; the
in-app "Filesystems" screen is where you install, download, and delete
roots in the first place, and `/AOK/tools/manage-roots.sh` does the same
things from a shell.

## `manage-roots.sh`: installing and switching from the command line

The Filesystems screen is not always in reach: you may be on the far end of
an ssh session, or scripting a device you are not looking at.
`/AOK/tools/manage-roots.sh` drives the same catalog, the same importer and
the same "Default Root" setting.

```sh
sudo sh /AOK/tools/manage-roots.sh list        # installed, and which boots next
sudo sh /AOK/tools/manage-roots.sh available   # what you can install

# From the catalog, then make it the root that boots and quit the app
sudo sh /AOK/tools/manage-roots.sh install alpine3233arm64 --default --exit-app

# From an archive already on the device, or from a URL
sudo sh /AOK/tools/manage-roots.sh install /AOK/persist/roots/mine.tar.gz Mine
sudo sh /AOK/tools/manage-roots.sh install https://example.org/rootfs.tar.gz Experiment

sudo sh /AOK/tools/manage-roots.sh default Devuan6-x86_64
sudo sh /AOK/tools/manage-roots.sh remove Experiment --yes
```

Three things worth knowing:

- **Switching takes effect at the next app launch.** Which root boots is read
  once when the app starts, so nothing here disturbs the running guest. That is
  also why you can install a root from inside the root you are using.
  `--exit-app` quits the app for you (through the same shutdown a suspension
  takes, so nothing is left half-written), and the next launch comes up in the
  new root.
- **An install keeps running if you walk away.** The download and unpack belong
  to the app, not to the shell that asked for them. Ctrl-C, a dropped ssh
  session, or a second invocation of `manage-roots.sh status` all find the same
  job still going.
- **It needs the app.** The command-line build of iSH-AOK has no root manager, and
  says so rather than pretending: `/proc/ish/roots` reports
  `job state=unavailable` there.

The script is a thin front end over `/proc/ish/roots`, which is the whole
mechanism. Reading it lists the roots, the catalog and the current job. Writing
sends a command, one `key=value` per line, with `run` on its own line to commit
it. Every value runs to the end of its line, so paths and URLs need no quoting:

```sh
printf 'op=default\nname=Devuan6-x86_64\nrun\n' | sudo tee /proc/ish/roots
cat /proc/ish/roots
```

`run` is there because a write is a fragment, not a command: the shell's
`printf` issues one `write(2)` per line, so the three lines above arrive as
three separate writes. The knob accumulates them and acts when `run` turns up,
which also means you can build a command interactively:

```sh
exec 3> /proc/ish/roots
echo op=remove >&3; echo name=Experiment >&3; echo confirm=yes >&3; echo run >&3
```

## Installing roots (in-app)

The **Filesystems** screen — in app settings, and the same screen as the **Boot
Images** applet in Workspace — lists four groups:

- **Installed Filesystems** — the roots you already have, with the one that
  boots next marked. Swipe to delete one.
- **Root Cached Filesystems (`/AOK/persist/roots`)** — any root archives
  sitting in that shared, persistent folder, whether they got there via
  automatic download or because you (or the Files app) dropped a
  `.tar.xz`/`.tar.zst`/`.tar.gz`/etc. archive in yourself. Tap one to
  install it as a new named root.
- **Official Distributions** — Alpine 3.23.3 and Devuan 6 (excalibur), one row
  per distro with the architecture as a sub-choice. The `aarch64` images are
  bundled in the app; `i386`, `x86_64` and `riscv64` download on demand into
  `/AOK/persist/roots` and import from there.
- **Community Distributions** — PSCAL + SmallCLUE (arm64) and Arch Linux
  (`x86_64` and ARM `aarch64`). Contributed or experimental, without the same
  support guarantees as the official images.

Root names become the mount "source" string guest tools like `mount` and
`df` will show, so names are restricted to `[A-Za-z0-9._-]`, can't start
with `.`, and can't contain spaces.

## `/AOK/roots`: other roots, exposed read-write

On boot, every installed root *other than the one you're currently booted
into* is mounted read-write under `/AOK/roots/<name>` (backed by that
root's own SQLite-backed filesystem). This mounting happens in the
background after boot, so a root may take a moment to appear under
`/AOK/roots` right after app launch — this is deliberate: mounting several
roots synchronously at launch (each involving a SQLite connection and
possible schema check) risked tripping iOS's launch watchdog on a device
with many installed roots.

If a root is currently locked by something else (for example, the Files
app File Provider extension is mid-operation on it), its mount is silently
skipped for that boot — it'll appear on the next one once the lock clears.

`/AOK/roots` itself lives in the app's shared App Group container, exactly
like `/AOK/persist`, so it's the same location regardless of which root you
booted into.

## `mount-root.sh`: turning a root into a real chroot target

Being visible under `/AOK/roots/<name>` isn't enough to actually run
programs from another root the way a distro install would expect —
`/proc`, `/sys`, `/dev`, and `/dev/pts` still need to be bind-mounted in so
guest tools see the same view of the (single, shared) kernel that the
outer root sees. `/AOK/tools/mount-root.sh` does that bind-mounting and
then chroots you in.

Must be run as root (mounting and `chroot` both require it):

```sh
# Interactive shell inside a chroot (prefers /bin/bash, falls back to /bin/sh -l)
sh /AOK/tools/mount-root.sh Devuan6-x86_64

# Run one command non-interactively and return
sh /AOK/tools/mount-root.sh Devuan6-x86_64 -- uname -a

# Do it for every installed root under /AOK/roots
sh /AOK/tools/mount-root.sh all -- top -bn1

# Tear the bind mounts back down
sh /AOK/tools/mount-root.sh --unmount Devuan6-x86_64
sh /AOK/tools/mount-root.sh --unmount all
```

The script also bind-mounts `/AOK/tools` itself into the chroot, so
`mount-root.sh` and `ktop` stay reachable from inside it. Root names are
sanity-checked to reject `/`, `.`, and `..`.

Because there's only one real kernel underneath, a process started inside
a `mount-root.sh` chroot is a completely ordinary process from the outer
root's point of view — `ktop` or `ps` run outside the chroot will see it,
architecture and all. See [ktop.md](ktop.md) for the one caveat that runs
the other way (running `ktop` *from inside* a chroot).

## Bind mounts by hand

`mount-root.sh` does its bind-mounting for you, but `mount --bind` is available
directly and behaves the way Linux does:

```sh
mount --bind /some/dir  /mnt/point     # directory over directory
mount --bind /etc/hosts /tmp/hosts     # a single FILE over another file
umount /tmp/hosts                      # the original contents come back
```

Binding a file over a file is the case people most often assume is missing; it
works, and it is how a config file gets shadowed without touching the original.

The shapes have to match, exactly as on Linux — directory onto directory, or
non-directory onto non-directory. Mixing them fails with `ENOTDIR`, and a source
that does not exist fails with `ENOENT` rather than quietly creating a bind that
shadows the target with a dead path.

**`--rbind` is genuinely recursive.** A plain `--bind` copies only the one
filesystem at the source; `--rbind` replicates every mount underneath it at the
matching place under the new location:

```sh
mount --bind  /a /b     # anything mounted *under* /a is not visible under /b
mount --rbind /a /c     # it is under /c
```

Recursive propagation flags (`--make-rprivate` and friends) are accepted and do
nothing, which is the honest answer here: there are no mount namespaces to
propagate between.

That last point is the one real difference from a normal Linux box, and it is
worth keeping in mind — **a bind mount you create is visible to everything**,
including processes in other roots and other chroots, because there is a single
mount table underneath all of them (see [00-overview.md](00-overview.md)). A
bind is a system-wide change, not a private one, so unmount what you no longer
need.

## Provisioning scripts: turning a bare rootfs into a full terminal environment

A freshly-imported root is intentionally minimal. Three scripts under
`/AOK/tools` turn one into a comfortable, "full Linux feel" terminal
environment in one pass — matched packages, sudo, a themed shell, tmux, and
services that behave correctly under iSH-AOK's clock model:

```sh
sudo sh /AOK/tools/provision-ultimate-alpine.sh
sudo sh /AOK/tools/provision-ultimate-devuan.sh
sudo sh /AOK/tools/provision-ultimate-archlinux.sh   # experimental, like the root itself
```

All three are idempotent (safe to re-run) and interactively prompt for a
timezone and a target username unless you set `TZ_NAME` / `TARGET_USER`
(and optionally `NEW_HOSTNAME`, `SUDO_NOPASSWD`) in the environment first.
Each one:

- Installs a curated set of packages for a comfortable terminal: bash,
  vim/neovim, tmux, htop/btop, fzf, ripgrep, fd, bat, eza, git, a build
  toolchain, python3, common networking and compression tools, man pages,
  fastfetch, and more.
- Sets the timezone and a `C.UTF-8` locale, and generates a machine ID.
- Creates (or configures) the target user, adds them to the sudo group
  (`wheel` on Alpine, `sudo` on Devuan), and switches login shells to
  bash.
- Writes a MOTD, a colored prompt, an fzf/dircolors profile snippet, and a
  themed `tmux.conf` plus a dependency-free Neovim starter config
  (including OSC52 clipboard support on Neovim 0.10+).
- Configures `chrony` in **monitor-only mode** (`-x`). This matters
  specifically under iSH-AOK: the guest clock *is* the iOS host clock,
  which is already NTP-synced by iOS itself, so chrony must observe rather
  than try to step or slew it.
- Enables and starts the relevant boot services for the distro (OpenRC on
  Alpine: sshd, cronie, chronyd, syslog-ng; sysvinit on Devuan: ssh, cron,
  chrony, rsyslog). Arch has no *non-systemd* init — systemd itself runs as
  init when the app boots that root normally — so the Arch script starts sshd,
  syslog-ng, chronyd and crond directly in the background, and installs
  `start-aok-services` as the fallback for when the app drops you into a bare
  shell instead.

The Devuan script is the apt/dpkg + sysvinit counterpart of the Alpine
script; a comment block at the top of `provision-ultimate-devuan.sh`
documents the package-name differences between the two distros (e.g.
`procps-ng` → `procps`, `build-base` → `build-essential`, `cronie` →
`cron`, `bind-tools` → `bind9-dnsutils`).

## Known distro fixes: `/AOK/fixes`

`/AOK/fixes/devuan` (and its `/AOK/fixes/debian` symlink) ships a canned
fix for a known upstream bug: on current Devuan/Debian roots,
`/usr/sbin/pkcsslotd` starts and daemonizes correctly but never writes its
pidfile, so the stock init script reports failure via `start-stop-daemon`
even though the daemon is actually running. Apply it with:

```sh
sh /AOK/fixes/devuan/fix-pkcsslotd-init.sh
```

`/AOK/fixes/arch` does the same for Arch Linux ARM, where a stock root cannot
install packages at all until three things are dealt with — none of them an
emulator bug:

- **pacman's sandbox needs Landlock**, the Linux LSM, which AOK does not
  implement. pacman treats its absence as fatal rather than degrading
  (`restricting filesystem access failed because Landlock is not supported by
  the kernel!`), so the sandbox is switched off explicitly. AOK will not
  pretend to support it: a syscall claiming to have sandboxed something it did
  not is worse than one that says it cannot.
- **`/etc/resolv.conf` is a dangling symlink** to the file systemd-resolved
  would create. Nothing runs systemd here, so every mirror lookup fails with
  "Could not resolve host".
- **The keyring is empty**, so signed packages are refused.

```sh
sh /AOK/fixes/arch/fix-pacman.sh
```

Safe to re-run; each step checks whether it is already done. The keyring step
takes a few minutes and needs no network.
