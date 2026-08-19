# ktop: an htop-style process viewer that understands multiple guest architectures

`ktop` is a small, dependency-free process viewer, styled after `htop` but
written from scratch in plain C99 against `/proc` — no ncurses, no procps.
Source: `/AOK/tools/ktop/ktop.c` (also `Makefile`, `build.sh`,
`README.md`).

## Why iSH-AOK needs its own

iSH-AOK can run i386, amd64, arm64, and riscv64 binaries side by side in
one booted session — most usefully by chrooting into another installed
root with [`mount-root.sh`](roots.md). Stock `top`/`htop` have no way to
show that architecture mix. `ktop`'s one real differentiator versus a
normal `top` clone is an **ARCH** column, read directly from each
process's ELF header via `/proc/<pid>/exe`.

`ktop` is purely `/proc`-based — it doesn't use taskstats or netlink
sockets (that's a separate mechanism, used by `iotop`-style tools; see the
regression test `taskstats_genl.c` if you're curious about that one).

## Building and running it

`/AOK` is a read-only mount, so `ktop` can't be built in place — copy its source
out first. The bundled `build.sh` does that for you, on every guest architecture
(arm64, x86_64, x86, riscv64):

```sh
sh /AOK/tools/ktop/build.sh              # build only -> /tmp/ktop-build/ktop
sh /AOK/tools/ktop/build.sh install      # also installs to /usr/local/bin/ktop
```

Prebuilt aarch64 binaries (`ktop-aarch64-musl`, static, and `ktop-aarch64-glibc`,
dynamic, for glibc roots like Devuan/Debian) live in the iSH-AOK source
repository under `opt/AOK/tools/ktop/prebuilt/`, and CI rebuilds them whenever
`ktop.c` changes — but they are **not** embedded in the app, so there is no
`/AOK/tools/ktop/prebuilt` on the device. Build from source here, on aarch64 too.

Or by hand: copy `ktop.c` and `Makefile` to a writable directory and run
`make` (`make install PREFIX=/usr/local` to install; `make clean` to
clean up).

Run it interactively with no arguments, or in batch mode for scripting:

```sh
ktop            # interactive, refreshing display
ktop -bn1        # one batch snapshot, plain text, good for logs/scripts
```

## What it shows

Interactive mode: colored per-CPU meter bars (two per row), a memory
meter, a swap meter (only if swap exists), load average, uptime, and a
scrollable process table with columns PID, USER, PR, NI, VIRT, RES, S
(state), **ARCH**, %CPU, %MEM, TIME+, and COMMAND.

Batch mode (`-bn1` and friends) prints the same columns as a plain table,
suitable for piping or logging — that is the interactive column set without `S`
and `TIME+`.

A command too wide for the terminal is cut and marked with a trailing `+`, the
way `top` does, so a shortened name is never mistaken for the real one, and the
cut always falls on a UTF-8 character boundary rather than mid-character. In
batch mode this applies only when stdout is a terminal: redirect or pipe
`ktop -bn1` and the full command is recorded uncut.

### Interactive keys

| Key | Action |
|---|---|
| Up / Down / PgUp / PgDn / Home / End | move cursor / scroll |
| `1` | collapse the per-CPU meter bars into a single average-load meter, and back |
| `P` | sort by %CPU |
| `M` | sort by %MEM |
| `T` | sort by TIME+ |
| `N` | sort by PID |
| `c` | toggle full command line vs. short name |
| `k` | send a signal to the selected process (Enter = SIGTERM, Esc = cancel) |
| `q` | quit |

Cursor selection follows the highlighted process across refreshes and
re-sorts, the same way it does in `htop`.

## Run it from the outer root, not from inside a chroot

Because there's only one real kernel underneath every root and chroot,
running `ktop -bn1` from your **outer, booted** root shows every process
system-wide, correctly labeled by architecture, including anything running
inside a `mount-root.sh` chroot.

The reverse doesn't fully work: run `ktop` *from inside* a chroot, and it
can only resolve the architecture of processes reachable from that
chroot's own root. Anything outside the chroot — including the outer
root's own processes — shows `?` in the ARCH column. This is because
`/proc/<pid>/exe` resolves to a symlink-target string that can't be
`open()`ed from outside the calling process's chroot (unlike real Linux,
where `readlink` on the same path still works fine, and does here too —
it's specifically `open()` that's restricted). If you want full
system-wide architecture labeling, run `ktop` from the root you originally
booted into.
