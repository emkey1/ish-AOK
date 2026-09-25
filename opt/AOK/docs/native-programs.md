# Native programs: code that is part of the app

Most of what you run under iSH-AOK is a guest binary. The emulator reads its
instructions and translates them, block by block, into instructions the iPhone
can run. That translation is most of the cost of running anything here.

A **native program** skips it. Programs like `zsh`, `dash` and SmallCLUE's
toolbox are compiled into iSH-AOK itself, as ordinary arm64 code, and when the
guest `execve`s one, the emulator calls that code directly instead of loading a
guest image. There is no translation, because there is nothing to translate.

You reach them through `/AOK/native`:

```sh
ls /AOK/native
# bmm  bmt  dash  ktop  motepad  passwd  rust-probe  sh  smallclue  su  sudo  zsh  zsh-multio
```

What is actually there depends on how the build was configured, so read the
directory rather than this page. The shipped app build enables `hx` (helix,
`AOK_NATIVE_HELIX=YES` in `app/iSH.xcconfig`) but not `bash`
(`AOK_NATIVE_BASH=NO`, as of 556 — see "Native bash and licensing" in the
project `README.md`); a plain CLI/meson build defaults both `native_helix` and
`native_bash` to `disabled` unless you pass `-Dnative_helix=enabled` /
`-Dnative_bash=enabled`.

Everything else — `ssh`, `wc`, `vi` — is a symlink to
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
and `ps` shows an ordinary process. But no image is loaded, and that has one
visible consequence: `/proc/<pid>/exe` names the `/AOK/native` entry that was
exec'd, since there is no ELF file to name instead. In [ktop](ktop.md) the
COMMAND column shows the native program's real name and the ARCH column reports
the *host's* architecture, which is what that code actually is — `arm64` next
to a native `zsh` in an `x86` root.

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

The rule is checked rather than trusted: `tools/check-native-libc.py`, run over
the built objects, reports every host-libc symbol a native program references
that is not on an explicit allowlist. It is a gate someone runs, not something
wired into the build, and it covers the routed surface rather than proving there
is nothing left — the timezone, for one, is still the host's.

## When a program is not in this build

Native programs can be compiled out — `-Dnative_bash=disabled`, for instance,
which is how a build leaves bash's GPLv3 code out of the binary. When that
happens the registry entry is empty and **`/AOK/native/bash` simply does not
exist**, rather than existing and failing.

Not everything here has a switch. `smallclue`, `motepad`, `ktop` and the
`bmm`/`bmt` benchmarks are unconditional, because there is nothing to gate them
on: none drags in a toolchain or a licence question the way bash, zsh and helix
do. So a script may reasonably assume those and should check for the rest.

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
| `ssh`, `scp`, `sftp`, `ssh-keygen`, `ssh-copy-id` | OpenSSH, as applets of SmallCLUE — note it is built **without OpenSSL**, so the [crypto accelerator](crypto-accel.md) does not apply to it |
| `vi` | the Nextvi editor, an applet of SmallCLUE |
| `/AOK/native/bash` | bash 5.2. GPLv3, which is why it has a build switch at all — and why it is **removed in build 556**; your guest `/bin/bash` is unaffected |
| `/AOK/native/zsh` | zsh, with fork-by-relaunch; `zsh --version` for the exact one. The only native program that can describe its own state, so the only one a [suspend](suspend.md) brings back where it was |
| `/AOK/native/dash`, `/AOK/native/sh` | dash, the POSIX shell most scripts are written against. BSD-licensed. Its fork-by-relaunch hands the child the parse *tree* rather than the command text, so quoting cannot be lost on the way. Your `/bin/sh` is untouched — but see [native-setup.md](native-setup.md), since the link step is what makes a bare `sh` mean this shell |
| `/AOK/native/zsh-multio` | a helper for zsh's MULTIOS redirections, which need a process that is not the shell to hold the descriptors |
| `/AOK/native/motepad` | a modeless terminal text editor, the counterpart to Workspace's MotePad applet — see [motepad.md](motepad.md) |
| `/AOK/native/ktop` | the process viewer, with no build step — the same source that ships at `/AOK/tools/ktop`, compiled as host code. See [ktop.md](ktop.md) |
| `/AOK/native/hx` | [helix](https://helix-editor.com), a modal editor with syntax highlighting and multiple selections. MPL-2.0, so like bash it has a build switch; registered as `hx`, which is what helix calls itself. Its grammars and themes are served from `/AOK/native/libs` |
| `/AOK/native/rust-probe` | a probe that exercises the Rust-on-the-shim path, not a tool you have a use for. Present because the Rust support it checks is what `hx` is built on |
| `/AOK/native/bmm`, `/AOK/native/bmt` | the CPU and thread microbenchmarks, so the same workload can be timed with and without emulation — see [benchmarks.md](benchmarks.md) |
| `/AOK/native/su`, `/AOK/native/sudo`, `/AOK/native/passwd` | SmallCLUE's su, sudo and passwd, each shipped as its own **setuid-root** program rather than an applet of the multicall binary — see below |

SmallCLUE's applets are *smaller* implementations, not drop-in replacements for
the distro's. They cover the common cases and diverge on individual flags — the
kind of difference no audit of the sources finds, because the command is present
and works, just not with that one option. That is worth knowing before you put
them ahead of your distro's tools on `PATH`; see
[native-setup.md](native-setup.md), which is also where the escape hatches are.

## `su`, `sudo` and `passwd`: the only setuid-root native programs

`ls -l /AOK/native/sudo` shows `-rwsr-xr-x`; every other native program is
`-r-xr-xr-x`. That bit means a real gain of the host euid to root
(`fs/aok.c`), which is exactly why `/AOK/native/smallclue` itself can never
carry it: SmallCLUE is a multicall binary that picks its applet from `argv[0]`,
so a setuid copy of it would hand `smallclue sh` a root shell to anyone who
asked. `su`, `sudo` and `passwd` are three separate programs that supply their
own `argv[0]` at the call site and never take it from the caller
(`kernel/native.c`), so `/AOK/native/sudo` can only ever be `sudo`.

They authenticate against the **booted root's own** `/etc/shadow`, read
through the same shim every other guest-facing lookup uses, and check the
password with a real `$5$`/`$6$` SHA-crypt implementation
(`kernel/sha_crypt.c`) built on CommonCrypto, checked against Drepper's
published test vectors — Darwin's own `crypt()` is DES-only and cannot read a
modern shadow line at all, so this had to be built rather than borrowed. A
locked account (`*` or `!...`) is not a hash any password produces, so it
never authenticates by accident. `sudo` still consults `/etc/sudoers` the same
way the applet always did; the difference is that it can now actually reach
root when a rule allows it. `$1$`, `$2b$` and Debian 12's default `$y$`
(yescrypt) are not implemented and are refused rather than guessed at.

## Where this is not the fast path

The win is **interpretation**. A shell grinding through a long script — an
arithmetic loop is the extreme case, at roughly 16x — spends its time reading and
dispatching its own syntax, and that is exactly the work that stops being
translated.

The cost is **forking**. Every subshell, pipeline stage and command substitution
becomes a serialise-and-re-launch rather than a `fork`, so a script whose shape
is thousands of short subshells gains far less, and can be slower than you
expect. If you are measuring, measure the workload you actually care about:
extrapolating one number to "the shell is 16x faster" is a mistake this project
has already made once.
