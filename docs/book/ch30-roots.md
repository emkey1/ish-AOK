# 30. Roots

Nobody uses iSH-AOK. People use Alpine, or Devuan, or Arch, and iSH-AOK is what
those are running on.

That is worth saying at the top of this chapter because it sets the stakes. The
emulator can be perfect and the product still unusable if installing a
distribution is hard, if switching between them loses work, or if the one you
have cannot be repaired from inside itself. Roots are where the engineering of
Parts II–V becomes something somebody can actually live in.

## 30.1 What a root is

A root is a fakefs (Chapter 17): a `data/` directory of escaped host files and a
`meta.db` holding every mode, owner and device number. Installing one means
running a rootfs tarball through `fakefsify`, which extracts through libarchive
and writes both halves.

The app can hold several at once, in any of the four guest architectures, and
records the guest ABI per root — which is how it knows to boot an x86_64 image
as an amd64 guest and an aarch64 image as an arm64 one.

They arrive three ways:

**Bundled.** Alpine 3.23.3 and Devuan 6 (excalibur), `aarch64` only. The Xcode
"Download Root" build phase installs those two archives into the app and deletes
the i386 and x86_64 ones from Resources, so those two are the only roots present
before anything is downloaded.

**From the catalogue.** The same two distributions for `i386`, `x86_64` and
`riscv64`, plus community images — Arch Linux (`x86_64` and `aarch64`) and
PSCAL + SmallCLUE — downloaded on demand into `/AOK/persist/roots` and imported
from there. The catalogue itself is `deps/rootfs-manifest`.

**Imported.** Any archive sitting in `/AOK/persist/roots`, whether it got there
by download or because somebody dropped a `.tar.xz` in through the Files app
(Chapter 31) or from the guest. Tap it and it becomes a named root.

## 30.2 The control surface is a file

The interesting design decision is that the Filesystems screen is not the root
manager. `/proc/ish/roots` is, and the screen is one of its front ends.

Reading it lists the installed roots, the catalogue, and the state of any job in
progress. Writing to it sends a command as `key=value` lines:

```sh
printf 'op=default\nname=Devuan6-x86_64\nrun\n' | sudo tee /proc/ish/roots
cat /proc/ish/roots
```

The `run` line is the part worth pausing on, because it is a correct solution to
a problem that only exists in this kind of interface:

> `run` is there because a write is a fragment, not a command: the shell's
> `printf` issues one `write(2)` per line, so the three lines above arrive as
> three separate writes. The knob accumulates them and acts when `run` turns up.

A procfs node cannot see "a command"; it sees writes, whenever the writer's
buffering decides to flush. Any protocol that assumes one write is one message
is broken by a shell, a pipe, or a slightly different `printf`. Accumulating
until an explicit commit is the fix — and it has a pleasant side effect:

```sh
exec 3> /AOK/../proc/ish/roots
echo op=remove >&3; echo name=Experiment >&3; echo confirm=yes >&3; echo run >&3
```

the command can be built up interactively, one line at a time, from a shell.

`/AOK/tools/manage-roots.sh` is a thin front end over that node, and it exists
for a specific reason: **the Filesystems screen is not always in reach.** You may
be at the far end of an `ssh` session, or scripting a device you are not looking
at. Chapter 28 argued that a setting reachable only by tapping is half a
setting; this is the same argument applied to an operation that takes minutes and
downloads a hundred megabytes.

## 30.3 Three properties that make it survivable

**Switching takes effect at the next launch.** Which root boots is read once,
when the app starts, so changing it disturbs nothing that is running. That is
also what makes the obvious paradox work: *you can install a root from inside the
root you are using.* `--exit-app` quits the app for you, through the same
shutdown a suspension takes (Chapter 28), so nothing is left half-written.

**Which means "the current root" is two things, and the screen says which.** The
root you are *running* is mounted at `/`, and the Filesystems screen draws its
row in green with a bold name and a filled dot reading "IN USE — mounted at / ·
can't be deleted". The root that boots *next* is a separate choice — what
`op=default` sets. Neither can be deleted or renamed: doing either to the running
root would move `/` out from under the live guest, and doing it to the default
would leave the next launch with nothing to boot. Keying the screen's marker off
the running root rather than the default one is the fix, not a decoration; the
guard against deleting `/` had been checking only the default, so choosing a
different one and then acting on the running root walked straight past it.
`/proc/ish/roots` reports only the second of the two — `root default=1` and
`default name=` — so which root actually booted is app-side knowledge today.

**An install keeps running if you walk away.** The download and unpack belong to
the app, not to the shell that asked for them. Ctrl-C, a dropped `ssh` session,
or a second invocation of `manage-roots.sh status` all find the same job still
going. A twenty-minute unpack tied to the lifetime of an ssh connection would be
unusable on a phone, where the connection is the least reliable component.

**It needs the app, and says so.** The command-line build has no root manager,
and `/proc/ish/roots` reports `job state=unavailable` there rather than
pretending. That is Chapter 40's capability-honesty rule in one field of one
file: a tool that reads the node learns the feature is absent instead of
watching a command silently do nothing.

## 30.4 A name is a `df` column

Root names are restricted to `[A-Za-z0-9._-]`, cannot begin with `.`, and cannot
contain spaces — because the name becomes the mount **source** string that guest
tools display.

Chapter 18 told the other half of that story. The root's source used to be its
directory name, which read well in `df` and named something no other file in the
guest had ever heard of, so nothing tied a mount to a device and `btop`'s disk
panel was empty. The root is `/dev/sda` now, defined once in `fs/real.h`.

The naming rule survives because a root name still appears in `/AOK/roots`, in
the Filesystems screen, and in the app's own bookkeeping — and a name with a
space in it breaks a shell script somewhere, always.

## 30.5 `/AOK/roots`, and chrooting across architectures

On boot, every installed root *other* than the booted one appears under
`/AOK/roots`, read-write.

```sh
chroot /AOK/roots/Devuan6-x86_64 /bin/bash
```

An arm64 guest, running an x86_64 userland, in the same session, with the
emulator switching guest ABIs per task (Chapter 7).

What it is actually for is more mundane and more useful than it sounds:
building something for another architecture, testing that a change behaves the
same on a different libc, and — most often — **repairing a root from outside
it**. A root whose `/etc` has been broken badly enough that it will not boot is
still just a directory to the root that can boot, which is a recovery story that
a single-root design does not have.

It works because there are no mount namespaces (Chapters 10 and 21). There is
one `/proc`, one `/sys` and one `/dev`, and they describe the single true system
from wherever you are standing.

`/AOK/tools/mount-root.sh` sets up the bind mounts a chroot needs — `/proc`,
`/sys`, `/dev`, `/run`, plus `/AOK/tools` and `/AOK/tests` from the booted root.
Chapter 9 met the consequence: a test running inside such a chroot sees the
*booted* root's `/proc` while standing in a different filesystem, which is why
`test_common.h` detects that situation and skips the tests that cross-check the
two.

## 30.6 One sharp edge

`fakefsify` converts an archive into a root. That is all it does, and the thing
it does not do catches everyone:

**There is no way to copy a host file into an existing root.**

Not from the host side, anyway — the root's contents are escaped names plus a
metadata database, so writing a file into it means writing both halves
consistently, and no tool does that from outside. The workaround for injecting a
host binary is to append it to the tarball first and re-run the conversion:

```sh
xz -dc foo.tar.xz > foo.tar
(cd stage && tar rf ../foo.tar ./usr)
./build/tools/fakefsify foo.tar build/newroot
```

Inside the guest, of course, it is just a file copy — which is the point of the
whole design, and also why the outside-in path never got built.

## 30.7 From a tarball to a system

A minirootfs is not a usable machine. It has a shell, a package manager and
almost nothing else: no timezone, no services, no editor configuration, no user
account, no `ssh` daemon.

`/AOK/tools/provision-ultimate-alpine.sh` and its Devuan and Arch siblings close
that gap. They are **idempotent** — safe to run repeatedly — and they set up a
generous CLI tool set, services on boot through the distribution's own init,
a timezone, shell niceties, and a dependency-free editor configuration.

One line in the Alpine script is a small window onto the whole project:

> chrony in iSH-aware monitoring mode (the guest clock is the host clock)

A time daemon's job is to discipline the system clock. Here there is no system
clock to discipline — the guest's time comes from the host, which iOS is already
keeping accurate. So `chronyd` is configured to *monitor* rather than *steer*,
which keeps every tool that expects a running time daemon happy without letting
it fight a clock it cannot move.

That is the same reasoning as Chapter 10's `nice` and `sched_policy`: implement
the observable contract, decline the part that has no meaning here, and be
explicit about which is which.

## 30.8 When a root goes wrong

Two failure stories, both instructive about where the boundaries are.

> **The bug that taught us this**
>
> A `pacman` operation on an Arch root produced a storm of warnings:
> `warning: could not get file information for usr/share/terminfo/x/...`.
>
> The message does *not* mean a `stat` returned an unusual errno.
> `calculate_removed_size()` in libalpm warns on **any** `llstat` failure with no
> errno test at all — plain `ENOENT` included — during the disk-space check
> rather than during extraction. So it means exactly "a file this installed
> package's list claims is missing". An hour went into chasing `ENOTSUP` and
> `EPERM` before that was read.
>
> Reproducing it needs no `pacman` at all: `xargs -a <filelist> ls -ld` inside
> the guest gives the identical count.
>
> The actual cause was fakefs case-twin damage from before the name escaping of
> Chapter 17 — `/usr/share/terminfo/{a,l,m,n,p,x}` merged into their uppercase
> twins. Which means the remedy is **reinstall, not migrate**: migration assumes
> the metadata was once right, and here the files are gone.

The second is shorter and is about age. Roots persist across app updates, so a
root created years ago is still in service — and an old Alpine image carries an
old musl, which behaves differently enough in places to send an investigation
sideways. When something behaves oddly on one root and not another, the age of
the image is a variable worth eliminating early.

## 30.9 What roots are, in the end

They are the reason the rest of this book matters, and they are also the part of
the product the project does not write.

Everything in `/AOK` (Chapter 21) exists *outside* every root precisely because
roots come and go: they are installed, broken, replaced, and switched between,
and the tools for doing that cannot live inside the thing being replaced. The
documentation, the tests, the native programs and the persistent storage sit one
level up for exactly the same reason a rescue disk is not on the disk being
rescued.

---

*Anchors:* [app/Roots.m](../../app/Roots.m),
[app/RootsTableViewController.m](../../app/RootsTableViewController.m),
[app/UpgradeRootViewController.m](../../app/UpgradeRootViewController.m),
[tools/fakefsify.c](../../tools/fakefsify.c), `deps/rootfs-manifest`,
[fs/proc/ish.c](../../fs/proc/ish.c) (`/proc/ish/roots`),
[opt/AOK/tools/manage-roots.sh](../../opt/AOK/tools/manage-roots.sh),
[opt/AOK/tools/mount-root.sh](../../opt/AOK/tools/mount-root.sh),
[opt/AOK/tools/provision-ultimate-alpine.sh](../../opt/AOK/tools/provision-ultimate-alpine.sh),
[opt/AOK/docs/roots.md](../../opt/AOK/docs/roots.md),
[fs/fake-path.h](../../fs/fake-path.h), [fs/real.h](../../fs/real.h).

*Story:* `warning: could not get file information for usr/share/terminfo/x/...` —
a message that means "this file is missing", not "this stat failed strangely",
and an hour spent on errno codes before anybody read libalpm.
