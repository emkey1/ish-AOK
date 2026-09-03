# ktop

A small, dependency-free `htop`-style process viewer with one extra column:
**ARCH**, the CPU architecture (`arm64` / `x86_64` / `x86` / `riscv64`) of
each process's binary, read from its ELF header via `/proc/<pid>/exe`.
Natively-dispatched programs (`/AOK/native/*`) have no ELF image of their own,
so for those it reports the host's architecture -- which is what that code
really is, and makes the native processes stand out at a glance.

iSH-AOK can run i386, amd64, arm64 and riscv64 binaries side by side in the
same booted guest -- most usefully via `chroot`ing into another installed
root with [`/AOK/tools/mount-root.sh`](../mount-root.sh) -- and stock
`top`/`htop` have no way to show that mix. Interactive mode is htop-flavored:
colored per-CPU / memory / swap meter bars, a cursor-selectable scrolling
process list, sort hotkeys, and kill. Batch mode (`-b`) prints a plain
top-style table for scripting. No ncurses, no procps -- just libc, ANSI
escapes and `/proc`.

## Already built in: /AOK/native/ktop

ktop is also compiled into iSH-AOK from this same `ktop.c`, and runs as host
code with no build step:

    /AOK/native/ktop

Present on every build and every guest architecture. Everything below is still
the way to build it yourself, and nothing here has been replaced.

## Prebuilt binaries (aarch64) -- source tree only

`opt/AOK/tools/ktop/prebuilt/` in the iSH-AOK source repository holds
`ktop-aarch64-musl` (static, works on any aarch64 guest, musl or glibc) and
`ktop-aarch64-glibc` (dynamic, for glibc-based roots like Devuan/Debian), which
CI rebuilds whenever `ktop.c` changes.

They are NOT embedded in the app. `fs/aok-tools.manifest` ships only `ktop.c`,
`Makefile`, `build.sh` and this README, so `/AOK/tools/ktop/prebuilt` does not
exist on the device -- build from source below, on aarch64 too.

## Build

```sh
sh /AOK/tools/ktop/build.sh          # builds ./ktop under /tmp/ktop-build
sh /AOK/tools/ktop/build.sh install  # also installs to /usr/local/bin/ktop
```

Installing writes to `/usr/local/bin`, which is root-owned on a normal root, so
run that second form as root (`sudo sh /AOK/tools/ktop/build.sh install`) or
install the built binary yourself with
`sudo make -C /tmp/ktop-build install PREFIX=/usr/local`. Re-running either
form is fine: the work directory is refreshed from `/AOK` each time, so a ktop
fix reaches an already-installed copy by building again.

`/AOK` is a read-only mount, so the script copies the source to a writable
work directory (`$WORK_DIR`, default `/tmp/ktop-build`) before running `make`.
Or build it manually:

```sh
cp /AOK/tools/ktop/ktop.c /AOK/tools/ktop/Makefile /tmp/ktop-build/
make -C /tmp/ktop-build
```

## Usage

```
ktop [-b] [-n iterations] [-d seconds]

  -b            batch mode: no screen clearing, print each snapshot and
                exit after -n iterations (default 1 in batch mode)
  -n iterations number of snapshots before exiting (default: run until
                'q' is pressed, in interactive mode)
  -d seconds    delay between snapshots (default: 3)
```

Interactive keys:

| Key | Action |
|-----|--------|
| `Up`/`Down`/`PgUp`/`PgDn`/`Home`/`End` | move the cursor / scroll the list |
| `P` / `M` / `T` / `N` | sort by `%CPU` / `%MEM` / `TIME+` / `PID` |
| `c` | toggle full command line vs short name |
| `k` | send a signal to the selected process (Enter = TERM, Esc = cancel) |
| `q` | quit |

The cursor follows the selected process across refreshes and re-sorts, like
htop. Batch mode (`-b`) is meant for scripting, e.g.:

```sh
ktop -bn1                       # one snapshot, then exit
ktop -bn1 | head -20
```

## Example: seeing the arch mix across a chroot

Run `ktop` from the **outer, booted root** (i.e. a plain shell, not one
entered via `mount-root.sh`). Build it there once:

```sh
sh /AOK/tools/ktop/build.sh install
```

Then, from that same outer shell, `ktop -bn1` shows every process on the
system -- including ones running inside a `mount-root.sh` chroot into another
installed root -- each correctly labeled with its own architecture, since
`/proc` is a single, shared view of the one true kernel state regardless of
which root a process was `exec`'d from:

```sh
sudo /AOK/tools/mount-root.sh Devuan6-x86_64 -- some-long-running-thing &
ktop -bn1   # run from the outer shell, NOT inside the chroot above
```

**Known limitation:** run `ktop` *from inside* a `mount-root.sh` chroot and
it can only detect the architecture of processes reachable from that
chroot's own root -- anything outside it (including the outer root's own
processes) shows `?` in the ARCH column rather than failing or showing wrong
data. This mirrors a real gap in iSH-AOK's `/proc/<pid>/exe`: opening
another process's exe this way resolves through a symlink-target string
(unlike real Linux, which opens the underlying file directly regardless of
the caller's chroot), so a target outside the caller's chroot can't be
opened. `readlink` on that same path still works correctly and reports it as
unreachable, matching real Linux -- it's specifically *opening* it that's
restricted. Bottom line: for the full picture, run `ktop` from the outer,
un-chrooted shell.
