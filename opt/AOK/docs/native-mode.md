# Native mode: iSH-AOK with no Linux distribution

Every other root you can install is a Linux distribution: Alpine, Devuan,
Arch, each with its own programs, all run by the emulator. **Native mode** is
the root without one. Its `/bin` and `/usr/bin` are the programs compiled into
iSH-AOK itself, [native programs](native-programs.md) that run as the iPad's
own code with nothing translated, and its login shell is native zsh.

Nothing is downloaded, and nothing is unpacked: the filesystem starts empty,
and iSH-AOK fills it in at every boot. What you get is a shell, about 140
everyday commands, ssh, git, curl, editors and a small service system, all
fast. What you do not get is a package manager. When you need something
native mode does not have, install a distribution beside it (see below).

## Choosing it

In **Filesystems** (Settings, or the Boot Images applet), under **Official
Distributions**, pick **iSH-AOK Native**. It is created on the spot, with no
download. To boot it, open it under Installed Filesystems and choose **Boot
From This Filesystem**. The first-launch picker offers it too, beside Alpine
and Devuan; nothing is chosen for you.

From a shell, with `manage-roots.sh`:

```sh
sudo sh /AOK/tools/manage-roots.sh install aoknative --default --exit-app
```

As with any root, which one boots is decided when the app starts; see
[roots.md](roots.md).

## The first start

A new native root has one account, `root`. The first time it boots, iSH-AOK
asks for the name of your everyday account, and optionally a password:

- **Create** makes it at uid 1000, in the groups `sudo` and `users`, with a
  home directory of its own and native zsh as its shell. With **Open
  Everything as Default User** switched on in Settings, Workspace terminals
  open as this user, the same as on a distribution root.
- **Skip (Root Only)** leaves root alone. You will not be asked again; delete
  `/etc/aok-native-user-skipped` to be asked at the next launch.

Nothing waits for your answer. The system boots while the question is on
screen, and the **Session Shell is always root**: you always have a root
prompt to fix things from, whatever happens to the accounts.

Without a password the account still opens (terminals use `login -f`), but
`sudo` needs one. Set it from the root Session Shell with `passwd <name>`.

## What is in it

Run `ls /usr/bin` for the exact list in your build. The main pieces:

| | |
|---|---|
| shells | `zsh` (the login shell, with completion), `sh` and `dash` |
| files and text | `ls cp mv rm ln mkdir cat less head tail sort uniq wc cut tr sed grep awk find xargs diff patch tar gzip`, and the rest of SmallCLUE: see [native-programs.md](native-programs.md) |
| network | `ssh scp sftp ssh-keygen ssh-copy-id`, `curl`, `wget`, `ping`, `host`, `nslookup` |
| git | `git`: init, clone and fetch over HTTPS, add, commit, status, diff, log, branch, checkout |
| editors | `vi`, `hx` (helix), `motepad`, and `md` to read Markdown -- these documents included |
| system | `ps top ktop kill df free dmesg mount umount chroot uname uptime` |
| terminal | `tput reset clear stty resize` |
| accounts | `login su sudo passwd id whoami` |
| services | `init`, `runit`, `sv`, `halt poweroff reboot` |

Every one of these is the native program: there is no emulated copy
underneath.

`uname -o` says `AOK/Linux`: the kernel is AOK's, and there is no C library
for a `GNU/` to name. On a distribution root it says `GNU/Linux` (glibc) or
`Linux` (musl), as there.

**Not here:** a package manager, compilers, Python, Perl, and the Wayland
desktop, which needs a distribution's compositor. `git` does not yet speak
`git@host:` (ssh) remotes; use the HTTPS URL. And `ps` shows a shell's
subshells by their re-launch command line, because a native shell cannot fork
(see [native-programs.md](native-programs.md)).

## How the filesystem is kept

iSH-AOK provisions a native root **at every boot**, so it follows the build:
an update that adds a command puts it on your `PATH` at the next launch, and
one that drops a command removes its link.

- `/bin` and `/sbin` are links to `usr/bin` and `usr/sbin`, as on current
  Debian. Scripts that name `/bin/echo` find it.
- **The links are iSH-AOK's.** Each command is a symlink into `/AOK/native`,
  and `/etc/aok-native.links` records the set the last boot made. A link that
  is missing is put back; a link the build no longer has is removed.
- **Anything of yours at one of those paths is left alone.** Replace
  `/usr/bin/date` with a script of your own and it stays. iSH-AOK only ever
  touches symlinks that point into `/AOK/native`.
- **`/etc` is yours.** `passwd`, `group`, `shadow`, `sudoers`, `shells`,
  `hosts`, `profile`, `zprofile`, `zshrc`, `rc` and `rc.shutdown` are written
  only when they are missing, so edit them freely. `/etc/os-release` and
  `/etc/aok-native.links` are iSH-AOK's and are rewritten.
- `/usr/share/terminfo` is the terminal database iSH-AOK carries, and
  `/usr/share/zoneinfo` is the iPad's own time-zone data, mounted read-only.
  `TZ` and `/etc/localtime` work as on a distribution, and the clock follows
  the device's zone.
- `/AOK/native/libs/zsh` holds zsh's completion system, so Tab completes
  commands and options.

`native-links.sh` has nothing to do here and says so: on a native root the
commands already are the native programs.

## Booting and services

`/sbin/init` is SmallCLUE's init. At boot it runs `/etc/rc` once, which:

1. runs every executable `/etc/rc.d/S??*` with `start`, in name order;
2. starts `runit` for `/etc/service` if anything is there;
3. runs `/etc/rc.local` if it is executable.

A boot script is any executable: there is a commented example at
`/etc/rc.d/S50example.disabled`. A long-running program belongs under
`/etc/service/<name>/run` instead, where `runit` keeps it going: it restarts
one that dies (at once if it had been up for ten seconds, and with a growing
delay if it keeps dying), and a file named `down` beside `run` keeps it
stopped until asked.

```sh
sv status myservice      # run: myservice: (pid 42) 310s
sv restart myservice
sv down myservice        # stop it, and keep it stopped
sv up myservice
```

`halt`, `poweroff` and `reboot` ask init to stop the system: it runs
`/etc/rc.shutdown` (which stops the services, then runs the `S??*` scripts with
`stop` in reverse order), stops every process and ends. The app then shows
**System Halted**: quit iSH-AOK and open it again to boot. iOS does not let an
app restart itself.

**Suspend and restore.** A native root can be [suspended](suspend.md) like any
other. On the way back, init does not run `/etc/rc` again and runit takes over
the services it was already running, so nothing starts twice.

## Reaching a distribution

Native mode and a distribution live side by side. Install one in Filesystems
(or `manage-roots.sh install`): every root you are not booted into appears
under `/AOK/roots/<name>`. They are mounted in the background as the app
starts, a second or so after boot begins, so a boot script that uses one should
wait for it (`while [ ! -d /AOK/roots/<name>/usr ]; do sleep 1; done`).

**Running one of its programs by name.** `/AOK/roots/Devuan6-arm64/usr/bin/tmux`
is a real program, but it was built against Devuan's libraries, and its first
step is to load Devuan's dynamic loader from `/lib/...` -- which in a native
root is not there. On Linux that exec simply fails, with a "no such file or
directory" about a file that plainly exists. iSH-AOK can do better, and
**Settings → Other Filesystems → Programs From Other Roots** chooses how
(it applies to the next program started; `/proc/ish/foreign_exec` reads and
sets it too):

| | |
|---|---|
| **Run Inside Their Root** (the default) | The program runs chrooted into its own root, as if you had used `mount-root.sh`: iSH-AOK binds `/proc`, `/sys`, `/dev`, `/run` and `/AOK/native` into it first. It sees that root's `/etc`, its data files, its `/tmp` and its home directories, and everything it starts does too. |
| **Use Their Libraries Here** | The program runs here, in the native root, borrowing only its loader, libraries and locales (through `LD_LIBRARY_PATH` and `LOCPATH`, which iSH-AOK removes again for programs that do not need them; a `LOCPATH` of your own wins). It sees your files and your home. Programs that need other data files of their own -- vim's runtime, Python's library -- will not find them. |
| **Off** | The Linux behaviour: it fails. |

It applies on any root, not only a native one, and only to programs that would
otherwise fail: a program from another root whose loader is missing here, or
whose path or `#!` interpreter only resolves inside its own root. A static
program, or a script whose interpreter exists here too (`#!/bin/sh`), runs as
it always did, in this root.

**A shell inside a root.** `mount-root.sh` works from a native root as it does
from any other, and binds `/AOK/native` in, so your native shell and tools are
there too:

```sh
sudo sh /AOK/tools/mount-root.sh Alpine3.24.2-aarch64           # a shell inside it
sudo sh /AOK/tools/mount-root.sh Alpine3.24.2-aarch64 -- apk add python3
```

Programs inside it are the distribution's own, run by the emulator.

A single static Linux program you bring yourself also runs, under emulation
like any guest program: put it in `/AOK/persist/bin`, which is on `PATH` (see
[persist.md](persist.md)).
