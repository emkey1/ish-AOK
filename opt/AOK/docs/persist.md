# /AOK/persist: a host directory that survives everything

`/AOK/persist` is a real, writable, host-backed directory (not an emulated
SQLite filesystem) living in the app's shared App Group container. Unlike
every root filesystem you install, it is:

- **The same single mount regardless of which root you booted into.** It
  is not inside any root's own filesystem — booting a different root or
  chrooting elsewhere doesn't change what you see under `/AOK/persist`.
- **Survives root switches, app updates, and reinstalls**, because it lives
  in the App Group container rather than inside any particular root's data
  store or the app's own sandbox.
- **Directly host-backed**, so file access through it has no emulated-VFS
  translation overhead — useful for anything the app itself also wants to
  read quickly (audio playback, the LLM chat log, etc.).

If you want a place to keep something that should outlive "delete this
root and reinstall a fresh one," `/AOK/persist` is usually that place — but it
is host-backed, so it flattens Linux ownership and cannot hold device nodes.
For a cross-root tree that needs real filesystem semantics (uid/gid, modes,
device nodes, hardlinks), use `/AOK/fakefs` instead; it survives exactly the
same things. See [00-overview.md](00-overview.md) for where both sit.

## What already lives there

| Path | Used for |
|---|---|
| `/AOK/persist/roots/` | Cached/downloaded root filesystem archives (`.tar.xz` and similar), shared by every installed root. Populated by the Filesystems screen's download flow, or by dropping an archive in yourself (from the guest, or from the Files app). |
| `/AOK/persist/music/` | Default library folder for the in-app Music player applet. |
| `/AOK/persist/playlists/` | Saved playlists (JSON) for the Music player. |
| `/AOK/persist/llm-chat.json` | Chat history for the in-app LLM Chat assistant. |
| `/AOK/persist/llm-extracts/` | Saved extracted content from the LLM assistant. |
| `/AOK/persist/llm-prompts/` | User-editable prompt templates for the LLM assistant. |

| Path | Created empty for you to fill |
|---|---|
| `/AOK/persist/bin` | Your own programs. **First on the default `PATH`.** |
| `/AOK/persist/lib` | Shared libraries for them, if they are not static. |
| `/AOK/persist/etc` | Their configuration. |

The MotePad text editor also treats `/AOK/persist` as its default starting
directory whenever it's present, since it's the one location guaranteed
not to disappear under it.

## `bin`, `lib` and `etc`: programs that follow you between roots

`bin`, `lib` and `etc` are created empty on every launch, so they are simply
there to drop things into -- from the guest, or from the Files app. Nothing
lives in them unless you put it there.

`/AOK/persist/bin` comes **first** on the default `PATH`. That is safe in a way
it would not be for a directory full of replacement commands, because this one
is empty until you fill it: it shadows nothing by default, and anything it does
shadow is something you chose to put there.

What makes this more useful than a plain `~/bin` is that **it does not care
which root you booted**, for two separate reasons:

- `/AOK/persist` is one host-backed directory shared by every root, and it
  survives root switches, app updates and reinstalls.
- iSH-AOK picks the guest ABI from **each ELF's own header**, not from the
  root. An `aarch64` binary runs under an i386 Alpine root or a riscv64 one
  just as happily -- that is tested, not assumed.

So a **statically linked aarch64 binary** dropped in `/AOK/persist/bin` works
in every root you will ever install, and keeps working after you delete the one
you built it in. That is the combination worth knowing about.

    # from any arm64 root with a compiler, or cross-compiled on a Mac:
    zig cc -target aarch64-linux-musl -static -O2 -o /AOK/persist/bin/mytool mytool.c

## The `ws-*` Workspace launchers

`bin` also gets a small generated launcher per Workspace applet, rewritten on
every launch so they track the app:

    ws-motepad ws-filemanager ws-markdown ws-imageviewer ws-videoplayer
    ws-audio ws-browser ws-llm ws-filesystems ws-storage ws-monitor
    ws-networks ws-status ws-settings ws-themes ws-launcher ws-clock
    ws-info ws-diagnostics ws-sessions

Each opens that applet in a Workspace window, optionally on a file:

    ws-markdown README.md          # relative paths are resolved for you
    ws-filemanager /etc
    ws-audio                       # no file: just open the applet

They are shell scripts, not binaries, so they work under any root regardless of
architecture, and you can read one to see exactly what it does. They talk to
`/proc/ish/workspace`; outside a Workspace-hosted session they say so and exit
1 rather than failing obscurely.

The `ws-` prefix is deliberate. `bin` is first on `PATH`, and several applet
names collide with real commands -- `info` is GNU info, and `status`, `clock`
and `browser` are plausible names for anything you might install. A bare `info`
that opened a GUI panel is exactly the kind of shadowing worth avoiding. It
also makes the set discoverable: type `ws-` and press TAB.

They are only rewritten if the existing file still carries the generated-file
marker, so if you replace one with your own, yours is left alone.

**If it cannot be static**, put the libraries in `/AOK/persist/lib` and build
the binary with an rpath so it finds them by itself:

    zig cc -target aarch64-linux-musl -O2 -Wl,-rpath,/AOK/persist/lib \
        -o /AOK/persist/bin/mytool mytool.c

An rpath is used rather than a global `LD_LIBRARY_PATH` on purpose: that
variable would be inherited by every process in every root, and would send the
dynamic linker hunting through a directory of wrong-architecture libraries on
every single exec. The rpath binds the lookup to the one binary that needs it.

Two things this directory is host-backed and therefore cannot do: hold device
nodes, or preserve Linux uid/gid and modes. If you need those, use
`/AOK/fakefs`.

## Reaching them from a `PATH` iSH-AOK did not set

`/AOK/persist/bin` is first on the `PATH` — but that is the `PATH` **iSH-AOK
hands to the sessions it starts itself**. It covers the app's terminals and the
command-line build, and nothing else. An `ssh` login, a `su -`, a cron job, or a
service started by init takes its `PATH` from `/etc/profile` or from a
compiled-in default, and none of those has ever heard of `/AOK/persist/bin`.
Your program is right there and still works by full path, but `command -v`
cannot find it. That is the shape this problem takes, and it is easy to read as
"the program is broken" instead of "this login has a different `PATH`".

Linking into a directory every distro already has on `PATH` fixes all of them at
once:

```sh
sh /AOK/tools/persist-links.sh
```

That symlinks each executable in `/AOK/persist/bin` into `/usr/local/bin`.
Symlinks specifically, never hard links — `/AOK/persist` is its own filesystem,
so `ln` across the boundary fails with `EXDEV`.

`--list` changes nothing and prints exactly what would happen:

```sh
sh /AOK/tools/persist-links.sh --list
```

| option | effect |
|---|---|
| `--list` | show what would happen, change nothing |
| `--remove` | remove links that resolve into `/AOK/persist/bin` |
| `--force` | replace files that are not its own symlinks |
| `--help` | the above, from the script itself |

A trailing argument picks a different directory, if `/usr/local/bin` is not the
one on your `PATH`:

```sh
sh /AOK/tools/persist-links.sh /opt/bin
```

### Expect to re-run it

This is worth saying out loud, because it looks like breakage otherwise: the
links live in `/usr/local/bin`, which belongs to **the root you are in**, while
their targets live in `/AOK/persist`, which does not. Install a fresh root and
the links are gone while every program they pointed at is still exactly where
you left it. Re-running the script is the expected thing to do, not a sign that
something failed.

The same applies after you drop a new program into `bin`. The links are made
when you run the script, not watched — so a new program is reachable by full
path and from an app terminal immediately, and from an `ssh` login after the
next run.

### What it will not do

It will not replace a file it did not create unless you pass `--force`.
Shadowing a distro's own command from a directory this script does not own is
exactly the failure [`native-links.sh`](native-setup.md) had to be walked back
from, so the default is to leave the file alone and say so. A real run counts
them:

```
linked 1 into /usr/local/bin (1 already or skipped, 1 left in place)
  1 existing command(s) left alone; --force to replace, --list to see them
```

and `--list` names them:

```
  would NOT replace /usr/local/bin/tar (exists; --force to override)
```

`--remove` takes back only symlinks that actually resolve into
`/AOK/persist/bin`, so a real file of yours that happens to share a name with
one of your tools is never touched. `--list` applies to a removal too:
`--list --remove` names every link it would take back and takes back none.
Anything in `bin` that is not executable is skipped rather than linked, so a
config file or a note kept alongside your programs does not become a command.

This is a different job from [`native-links.sh`](native-setup.md), which links
the app's own [native programs](native-programs.md) and can shadow your distro's
tools with smaller ones. `persist-links.sh` only ever links things **you** put
in `/AOK/persist/bin`, so it shadows nothing you did not choose.

There's nothing special about the directory beyond being a plain writable
folder — feel free to keep your own dotfiles, scripts, or notes there if
you want them to survive a root reinstall.
