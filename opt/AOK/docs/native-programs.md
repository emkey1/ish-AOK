# Native programs: code that is part of the app

Most of what you run under iSH-AOK is a guest binary. The emulator reads its
instructions and translates them, block by block, into instructions the iPhone
can run. That translation is most of the cost of running anything here.

A **native program** skips it. Programs like `bash`, `zsh` and SmallCLUE's
toolbox are compiled into iSH-AOK itself, as ordinary arm64 code, and when the
guest `execve`s one, the emulator calls that code directly instead of loading a
guest image. There is no translation, because there is nothing to translate.

You reach them through `/AOK/native`:

```sh
ls /AOK/native
# bash  smallclue  zsh  zsh-multio
```

Everything else — `ssh`, `wc`, `vi`, `rsync` — is a symlink to
`/AOK/native/smallclue`, which picks its applet from the name it was invoked
under, exactly the way busybox does. [native-setup.md](native-setup.md) covers
putting those symlinks in place.

## A native program is a function call, not a process

This is the one idea everything else follows from. When you run a guest binary,
the kernel builds a new address space and loads an ELF image into it. When you
run a native program, none of that happens: the emulator looks the name up in a
small registry (`kernel/native.c`) and calls a C function on the guest task's
own thread.

The task keeps its pid, its open files, its working directory, its environment
and its signal state — from the guest's point of view nothing unusual happened,
and `ps` shows an ordinary process. But there is no new memory image, and that
has one visible consequence: `/proc/<pid>/exe` still points at whatever guest
binary that task loaded last. In [ktop](ktop.md) the COMMAND column shows the
native program's real name while ARCH describes the previous image, which is why
you can see `x86` next to a native `zsh`.

It also explains `fork`. A native program cannot fork, because forking means
copying an address space and it does not have one of its own. The shells solve
this by writing their own state — variables, functions, options, traps — into a
script and re-launching themselves to read it back. That is why subshells and
command substitution work at all, and why they cost more here than the raw
speed-up might suggest: the win is in *interpretation*, not in forking.

## What it means for a program to "believe" it is in your Linux system

Speed is the easy half. The hard half is that a native program is host code, so
every question it asks is answered by the iPhone unless something intervenes.
Left alone, `getenv` would read the app's environment, `open` would see iOS's
filesystem, `uname` would report Darwin, and `getpwuid` would look up a user
that does not exist in your rootfs.

So the calls are intercepted. A shim (`kernel/native_libc.c`) is compiled in
ahead of the system headers and redirects libc by name, routing each call
through the same syscall path a guest program's `open` or `stat` would take. The
test for whether a given function needs that treatment is not "is it pure?" but:

> **can this function's answer differ between the host and the guest?**

`getenv`, `getpwuid`, `gethostbyname`, `setlocale`, `uname`, `sysctl`, `isatty`,
`tcgetattr` — every one of them answered about an iPhone until it was made to
answer about your rootfs. In practice this means a native program reads *your*
`/etc/passwd`, *your* `/etc/resolv.conf`, *your* terminfo, and writes to *your*
files, with your guest uid and your guest permissions.

A few things genuinely belong to the host and stay there — a native program's C
stack is a host stack, and its memory comes from the host allocator, because
those are not things the guest has an opinion about.

The rule is enforced rather than trusted: `tools/check-native-libc.py` fails the
build if a native program references a host libc symbol that is not on an
explicit allowlist.

## When a program is not in this build

Native programs can be compiled out — `-Dnative_bash=disabled`, for instance,
which is how a build leaves bash's GPLv3 code out of the binary. When that
happens the registry entry is empty and **`/AOK/native/bash` simply does not
exist**, rather than existing and failing.

The files that *do* exist are worth a look:

```sh
head -2 /AOK/native/bash
# #!/bin/sh
# # Placeholder for a program implemented natively inside iSH-AOK.
```

Each one is a tiny shell script. That text is unreachable in normal use, because
`execve` dispatches to the compiled function before the contents ever matter. If
you ever *see* that message, native dispatch did not happen — which makes it a
diagnostic rather than a program.

## What runs natively

| path | what it is |
|---|---|
| `/AOK/native/smallclue` | a busybox-style toolbox; the applet is chosen by the name it is invoked under |
| `ssh`, `scp`, `sftp`, `ssh-keygen` | OpenSSH, as applets of SmallCLUE — note it is built **without OpenSSL**, so the [crypto accelerator](crypto-accel.md) does not apply to it |
| `rsync` | openrsync, an applet of SmallCLUE |
| `vi` | the Nextvi editor, an applet of SmallCLUE |
| `/AOK/native/bash` | bash 5.2. GPLv3, which is why it has a build switch at all |
| `/AOK/native/zsh` | zsh. See the README for what works and what does not |
| `/AOK/native/zsh-multio` | a helper for zsh's MULTIOS redirections, which need a process that is not the shell to hold the descriptors |

SmallCLUE's applets are *smaller* implementations, not drop-in replacements for
the distro's. They cover the common cases and diverge on individual flags — the
kind of difference no audit of the sources finds, because the command is present
and works, just not with that one option. That is worth knowing before you put
them ahead of your distro's tools on `PATH`; see
[native-setup.md](native-setup.md), which is also where the escape hatches are.

## Where this is not the fast path

Native execution is a large win for anything that spends its time interpreting —
a shell running a long script, for example. It is neutral or slightly negative
for work dominated by a single tight loop of the program's own arithmetic, and
for anything that forks constantly, since each fork becomes a re-launch. If you
are measuring, measure the workload you actually care about rather than
extrapolating from one number.
