# Release Notes Since `builds/iSH-AOK_554`

Two large things: a session can survive the app being killed, and there is a
third shell — a small, fast, permissively-licensed one — compiled in.

## Highlights

**Suspend to disk.** iOS ends this app routinely — memory pressure, or a swipe
away — and until now that always lost the session. It can now save the whole
guest to a file when the app is backgrounded and bring it back on the next
launch: same device, same root, same build, back where you were.

Every process, its memory, its open files and its place in the process tree. A
shell blocked in `wait()` and the child it is waiting for come back and carry
on. A pipeline comes back with the bytes still in it. A file comes back at the
offset it was read to, and two processes that shared a descriptor still share
it. A zsh session comes back with its variables, functions and aliases, because
a native shell is *asked to describe itself* rather than photographed — there is
no serialising a host C stack. The native programs that cannot describe
themselves do not block the save: they are re-launched from their command line,
which for a shell at a prompt is the same thing, and the save says which ones
those were.

You come back to the terminal you were looking at, not to a new shell beside
your old one. A terminal cannot be restored — the one you had belonged to an app
process that no longer exists — so the resume makes a fresh one for each session
in the image and re-attaches that session to it: same controlling terminal, same
foreground job, same line settings, same hostname, same job table. In the app,
the window you resume into is the session you suspended.

**It is off by default**, in Settings, for the same reason swap is: it spends
your storage, and a session it cannot describe is one it will not save. When
that happens it says so — `cat /proc/ish/checkpoint` in the guest reports what
the last attempt did and, if it refused, which process and why — and the next
launch simply boots, which is the behaviour you get with the switch off. The
guest is never harmed by the attempt: a checkpoint is a copy, and the machine is
stopped only for as long as it takes to write one.

Three limits worth naming rather than leaving to be discovered. An image from a
different build is refused outright, because the register file travels as bytes
and reinterpreting one would be worse than declining it. A descriptor with no
rule for rebuilding it is refused by number — though regular files,
directories, terminals, pipes, sockets and the standard streams all have one.
And a native program that is not making any system calls cannot be stopped,
because there is nowhere to stop it: a shell waiting on a read is fine, a native
program in a tight compute loop is not.

What is *not* a refusal any more is a native program that simply cannot write
its own state down. Only `/AOK/native/zsh` can; bash, dash and the editors come
back re-launched from their command line instead, and the save reports which.
That used to refuse, and it was the wrong trade — the alternative to a degraded
restore is not a perfect one, it is no restore at all, because iOS kills the app
either way.

The guest can also take one for itself:

    echo save /path/to/image    > /proc/ish/checkpoint   # a copy; carry on
    echo suspend                > /proc/ish/checkpoint   # save and stop

**Native dash.** `/AOK/native/dash` and `/AOK/native/sh` are dash compiled into
the app and run as host code — a POSIX shell that starts in a fraction of the
time an emulated one does, which matters when a script forks one per line. It is
BSD-licensed and it is the shell most scripts are actually written against.

Your `/bin/sh` is untouched. You reach dash at its own path, or as plain `sh`
once `native-links.sh` has put `/usr/local/native-bin` first on your `PATH` —
which is worth knowing, because that is the step that makes `sh` mean this
shell rather than your distro's.

Its subshells work the way bash's and zsh's do here — a fork becomes a
re-launch, because a native program is a function call and not a process — but
with a difference worth having: dash hands its child the parse TREE rather than
the command's text, so quoting cannot be lost on the way. Measured against the
emulated `/bin/dash` over twenty-five constructs — nesting, `$$`, pipeline exit
status, subshell isolation, inherited functions and aliases, traps, background
jobs, a 51 KB here-document — the output is byte-for-byte identical, and faster:
500 command substitutions in 0.32s against 1.00s emulated.

## Notice: native bash is going away in 556

`/AOK/native/bash` will be removed in the next build. bash is GPLv3, and the
App Store's terms and the GPL have a history that has already cost two
well-known apps their listing — VLC in 2011 and GNU Go in 2010, both over the
same clause. Shipping a GPLv3 interpreter compiled into the binary is not a
risk worth carrying for a shell that now has two alternatives.

Nothing else changes. **Guest bash is untouched** — `apt install bash`,
`/bin/bash`, every script with `#!/bin/bash` in it, all exactly as before. What
goes is the natively-compiled copy at `/AOK/native/bash`.

If your login shell in `/etc/passwd` is `/AOK/native/bash`, this build moves it
to the guest bash for you, and to `/AOK/native/zsh` if there is none. Native zsh
is the interactive shell this project recommends, and the one that comes back
from a suspend with your session still in it.

## Compressed memory is no longer lazy

The pager only reclaimed when the guest was already short of memory, which for
a RAM-only compression pool is exactly the wrong rule — there is no file and no
flash cost to ration, so filling the pool with cold pages costs nothing and buys
headroom before it is needed. Measured on a device: 239 MB of recovered headroom
became 433 MB.

This is the **compression-on, swap-off** shape specifically. Turn a swap file on
as well and the old rule still applies, deliberately: evicting then reaches
flash, so there is a cost to ration again and the pager goes back to waiting
until the guest is actually short. If you have both on and you are wondering why
`/proc/ish/zswap` reports nothing stored on an idle device, that is why, and
`/proc/ish/swap` will say `kswapd running, N passes, 0 sweeps` to confirm it.

## And a long tail

- Two symbols that were silently one: dash's `$SHLVL` counter and zsh's shared
  a variable, as did their line counters, and dash's arithmetic `yylval` shared
  one with bash's. A tentative definition merges with a real one instead of
  colliding, so the build had never said a word about it.
- BSD `sigsetmask` reached the host rather than the guest, so dash's `wait`
  builtin worked once per shell and then blocked for ever.
- `cpu_poke` dereferenced a null pointer for any task whose program is native,
  which took the whole app down with it.
- Both ends of a pipe now share one inode, the way Linux reports them, so `lsof`
  and process-tree viewers can tell that two descriptors are two ends of one
  pipe; and the write end reports itself write-only rather than read-only.
- The guest test gate no longer skips anything. Every test it has, it runs.
- An unprivileged process may lock 8 MiB of memory, as it may on Linux.
  `RLIMIT_MEMLOCK` was 64 KiB -- the historical kernel default, which no distro
  has shipped in years -- so anything calling `mlock` on more than a page or
  two got ENOMEM unless it was running as root.

## Issues you reported, closed in this build

- **[#587](https://github.com/emkey1/ish-AOK/issues/587)** — `reboot` took the
  app down with it. The guest's init was killed, and a dead init will not adopt
  children, so there was nothing left to run. It now halts and says so.
- **[#572](https://github.com/emkey1/ish-AOK/issues/572)** — Gradle could not
  determine a usable IP. Two separate bugs: `SIOCGIFCONF` ignored a NULL-buffer
  size query the way OpenJDK asks it, and then the Gradle daemon died at startup
  because a `MAP_FIXED` commit inside the JVM's large `PROT_NONE` heap
  reservation faulted on every page.
- **[#485](https://github.com/emkey1/ish-AOK/issues/485)** — Qt applications
  such as Falkon could not reach a session bus. The desktop session has its own
  now.
- **[#482](https://github.com/emkey1/ish-AOK/issues/482)** and
  **[#483](https://github.com/emkey1/ish-AOK/issues/483)** — the Wayland desktop
  is sized from the window showing it, and resizing no longer crashes wayvnc.
- **[#580](https://github.com/emkey1/ish-AOK/issues/580)** — terminal content
  kept clear of the iPadOS window controls in windowed mode.
- **[#579](https://github.com/emkey1/ish-AOK/issues/579)** — a trackpad
  selection no longer leaves the terminal without keyboard focus.

Still open and worth naming, because it was misfiled rather than fixed:
**[#575](https://github.com/emkey1/ish-AOK/issues/575)** ("unable to delete
machines") is a reachability bug, not a missing button — the delete exists on
the machine's detail screen, but a swipe in the machines list does nothing at
all. The row action is not in this build.
