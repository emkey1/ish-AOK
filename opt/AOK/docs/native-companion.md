# apt in native mode: the companion filesystem

A [native-mode](native-mode.md) root has no distribution, so it has no
package manager. It can keep a second filesystem that has one: its
**companion**, Devuan 6 arm64, installed beside it and named `Devuan`. apt
works there, what it installs runs from there, and the
[Wayland desktop](native-mode.md#a-desktop) lives there too. The booted root
stays native.

```sh
sudo apt install htop      # the first time, offers to install the companion
htop                       # in a new login
```

## What you get

- **`apt`, `apt-get`, `apt-cache`, `apt-mark`, `dpkg` and `dpkg-query`** work
  from the native root. They are the companion's own, run inside it.
- **What apt installs runs by name.** The companion's `bin` directories
  follow this root's on `PATH`, so `htop`, `python3` or `gcc` are found once
  they are installed. A login made before the companion existed does not
  have them on `PATH` yet: log in again.
- **Native programs win.** Where both roots have a command (`ls`, `grep`,
  `vim`), the native one runs, because this root's directories come first.
  The companion's is still there by its path:
  `/AOK/roots/Devuan/usr/bin/vim`.

## What a program from the companion sees

It runs **inside** the companion: its `/usr`, its `/etc`, its libraries. On
top of that, the companion's `/etc/aok-binds` asks for these from the native
root each time it is entered:

| Shared | What it means |
|---|---|
| `/home`, `/tmp` | your files, under the same paths |
| `/etc/hosts`, `/etc/hostname`, `/etc/resolv.conf` | the same system name, addresses and DNS |
| the time zone | its `/etc/localtime` names the zone this root's does |

and, as for any root entered this way, `/proc`, `/sys`, `/dev`, `/run` and
`/AOK/native`, `/AOK/docs`, `/AOK/tools`.

- **It starts in your directory** when that is under `/home` or `/tmp`:
  `python3 script.py` in `~/work` reads `~/work/script.py`. Anywhere else
  (`/etc`, `/usr/...`) it starts in the companion's `/`, because those
  directories are the companion's own in there.
- **Your account is the same**: same name, uid, gid, home, shell and
  password, so a file in the shared `/home` has one owner and `sudo` asks the
  same question.
- **A script that lives in the companion runs inside it**, with the
  companion's interpreter, whatever its `#!` line names.

## Costs

- **Everything apt installs is emulated.** Only what comes from `/AOK/native`
  runs natively.
- **Two `/etc`.** A program from the companion reads the companion's
  configuration, apart from the three files above. `/etc/ssh/ssh_config`
  there is not the one here.
- **Outside `/home` and `/tmp`, files are not shared.** A program from the
  companion that writes to `/var` or `/opt` writes to the companion's.
- **Services do not start.** The companion is never booted, so package
  scripts are told not to start daemons (`/usr/sbin/policy-rc.d`). Run one
  yourself, or from this root's [`/etc/rc.d`](native-mode.md).
- **Space**: the Devuan base is a few hundred MB before the first package.

## Setting it up yourself

`sudo apt ...` offers to do this the first time. By hand:

```sh
sudo sh /AOK/tools/setup-companion.sh               # install and prepare
sudo sh /AOK/tools/setup-companion.sh --root NAME   # use a Devuan arm64 filesystem you have
sudo sh /AOK/tools/setup-companion.sh --user LOGIN  # give another account the companion
```

It is safe to run again; what is done is kept.

- **It is an ordinary filesystem.** Filesystems lists it, and it can be
  renamed or removed there, or with `sudo sh /AOK/tools/manage-roots.sh
  remove Devuan --yes`. Its name is in `/etc/aok-companion-root` in the native
  root; after a rename, that file is what to edit, and after a removal,
  delete it.
- **What is shared in is there only for programs running inside it.** Seen
  from the native root, `/AOK/roots/Devuan/home` is the companion's own empty
  `/home`, not yours, so searching or deleting under `/AOK/roots/Devuan` from
  here cannot reach your files. (`mount-root.sh` is different: the binds it
  makes are ordinary ones, visible from everywhere until
  `mount-root.sh --unmount`.)
- **It needs Programs From Other Roots set to Run Inside Their Root**, the
  default ([roots.md](roots.md)). `apt` says so if it is not.
- **A shell inside it**: `sudo sh /AOK/tools/mount-root.sh Devuan`.

## What is yours to edit

In the native root, all written only when missing:

| File | What it does |
|---|---|
| `/usr/bin/apt`, `apt-get`, `apt-cache`, `apt-mark`, `dpkg`, `dpkg-query` | one line each: hand the command to the companion (`/AOK/tools/companion-run.sh`) |
| `/etc/profile.d/20-aok-companion.sh` | puts the companion's `bin` directories after this root's on `PATH` |
| `/etc/aok-companion-root` | the companion's name |

In the companion: `/etc/aok-binds`, the list of what is shared in. The
names it may hold are `home`, `tmp`, `mnt`, `media`, `srv`, `etc/hosts`,
`etc/hostname`, `etc/resolv.conf` and `timezone`; add `mnt` or `media` if you
want those shared too.
