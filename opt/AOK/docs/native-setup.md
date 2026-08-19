# Setting up native programs

[native-programs.md](native-programs.md) explains what a native program is and
why it is fast. This page is the practical half: getting them onto your `PATH`,
making one your login shell, and backing out again.

Nothing here is required. `/AOK/native` is always present, and you can always
run a native program by its full path without setting anything up at all:

```sh
/AOK/native/bash --version
/AOK/native/smallclue wc -l /etc/passwd
```

What the setup does is let you type `wc` and `ssh` and get the native ones.

## The one command

```sh
sudo sh /AOK/tools/native-links.sh
```

That creates a symlink per applet in `/usr/local/native-bin`, puts that
directory first on your `PATH`, and switches your login shell to a native one.
On a current build it links about 105 applets and skips 28 it knows do not work.

Look before you leap — `--list` changes nothing and prints exactly what would
happen:

```sh
sh /AOK/tools/native-links.sh --list
```

The last lines are the summary worth reading:

```
would link 105, leave 0 in place, skip 28 excluded, 0 already linked, unlink 0 now-excluded
  would put /usr/local/native-bin first on PATH via /etc/profile.d/05-aok-native-bin.sh
```

**Do this once per root.** The links live in the root's own `/usr/local`, and
the PATH snippet in its `/etc/profile.d`, so a second root — or one you install
later — starts out without them. `/AOK/native` itself is the same everywhere,
since it is part of the app rather than of any root.

## Choosing the shell

By default the script switches the UID 1000 user's login shell to
`/AOK/native/bash` when that exists, and `/AOK/native/zsh` otherwise. Say so
explicitly with `--shell`:

```sh
sudo sh /AOK/tools/native-links.sh --shell zsh     # native zsh
sudo sh /AOK/tools/native-links.sh --shell bash    # native bash
sudo sh /AOK/tools/native-links.sh --shell /bin/ash   # an absolute path is taken as given
sudo sh /AOK/tools/native-links.sh --no-shell      # link the applets, leave the shell alone
```

The previous shell is recorded in `/etc/aok-native-shell.prev`, so `--remove`
can put it back.

To check what you are actually running right now, ask the shell for its own
name:

```sh
echo $0
# /AOK/native/bash
```

`/proc/self/exe` will *not* tell you — a native program has no guest image of
its own, so that link still points at the last guest binary the task loaded.

## Options

| option | effect |
|---|---|
| `--list` | show what would happen, change nothing |
| `--remove` | remove the links, restore the login shell, delete the PATH file |
| `--no-path` | create the links but leave `PATH` alone |
| `--no-shell` | leave the login shell alone |
| `--shell S` | `bash`, `zsh`, or an absolute path |
| `--all` | include applets that are known not to work in this build |
| `--force` | replace files that are not our own symlinks |
| `--help` | the above, from the script itself |

A trailing argument picks a different directory:

```sh
sudo sh /AOK/tools/native-links.sh /usr/local/bin
```

That puts the links ahead of your distro's own `/usr/local/bin` entries too,
which is more shadowing than the default, not less.

## Backing out

```sh
sudo sh /AOK/tools/native-links.sh --remove
```

That removes every link it owns, restores the login shell it saved, and deletes
`/etc/profile.d/05-aok-native-bin.sh`. It only touches symlinks that point into
`/AOK/native`, so anything of your own in the same directory is left alone. The
PATH change goes away at your next login.

For a single stubborn command, `--no-path` is the softer version: the links stay
usable by full path, and nothing is shadowed.

## When something behaves oddly

The applets are *smaller* implementations, not drop-in replacements, and the
places they diverge tend to be individual flags rather than whole commands. A
script that has always worked can fail on one option while the command itself is
plainly present and working — that is the shape this problem takes.

The excluded list is derived mechanically (`tools/native-applet-audit.py`)
rather than guessed, so applets that cannot work are not linked in the first
place. If one that *is* linked misbehaves:

```sh
command -v wc                  # which one am I actually getting?
/usr/local/native-bin/wc ...   # the native one, explicitly
/bin/wc ...                    # the distro's, explicitly
```

and if the answer is "the native one is wrong for my case", `--no-path` or
`--remove` are the fixes. There is no runtime switch that disables native
dispatch for a program that is compiled in; the links are the control.

Two things that are *not* faults:

- **ktop shows the wrong ARCH** next to a native program. Expected — see
  [native-programs.md](native-programs.md).
- **The crypto accelerator does nothing for native `ssh`.** Also expected: it is
  built without OpenSSL, so an OpenSSL provider cannot reach it. See
  [crypto-accel.md](crypto-accel.md).

## Re-run it after

- installing a new root (the links are per-root),
- a package upgrade that replaced something in your link directory,
- an app update that adds or removes native programs.

Re-running is safe: the script is idempotent, will not stack the PATH entry, and
`--list` will show you the delta first. It also removes links for applets that
have since been excluded, so an old link cannot go on shadowing a working
command forever.
