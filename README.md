# iSH-AOK

iSH-AOK is a fork of [ish-app/ish](https://github.com/ish-app/ish) with local product, tooling, and platform changes for day-to-day development on this tree.

Testflight: https://testflight.apple.com/join/X1flyiqE

This fork is not just a rebrand. It carries fork-specific behavior, bundled roots, diagnostics work, File Provider integration, and support for four guest architectures. If you want upstream iSH, use `ish-app/ish`. If you are working in this repository, this README is the relevant one.

## What This Fork Adds

- Fork-specific app identity:
  - product name `iSH-AOK`
  - bundle root `app.ish.iSH-AOK`
- **Four guest architectures**, all JIT: `i386`, `amd64` (x86_64), `arm64` (aarch64), and `riscv64`.
- **Native programs**: bash, zsh and SmallCLUE's busybox-style toolbox — which carries OpenSSH (`ssh`, `scp`, `sftp`, `ssh-keygen`, `ssh-copy-id`) and the Nextvi editor — are compiled into the app as host code and dispatched from guest `execve` through `/AOK/native/<name>`. They are host functions on a guest task's thread, not guest binaries, so they run at full speed instead of being translated instruction by instruction.
- `/AOK`, a read-only in-app filesystem (`/AOK/docs`, `/AOK/tools`, `/AOK/tests`, `/AOK/native`) embedded at build time from `opt/AOK/` via `fs/aok-*.manifest` and `tools/gen-aokfs.py`.
- Bundled root filesystems in the app build (Alpine 3.23.3 and Devuan 6, `aarch64` only), plus downloadable images for `i386`, `x86_64` and `riscv64`.
- File Provider support for exposing guest files through iOS.
- **FUSE**: `/dev/fuse` and a `fuse` filesystem type (protocol 7.31), so guest `libfuse2`/`libfuse3` daemons mount and serve filesystems unmodified. No setuid `fusermount` is involved — the guest is already fake-root, so libfuse calls `mount(2)` directly. See `/AOK/docs/fuse.md`.
- **Apple Shortcuts actions** (iOS 16+): a headless "Run Command" action that executes a command in the guest under the native zsh and returns its output to the shortcut — the app never has to come to the foreground — plus "Open iSH-AOK" destinations with Siri phrases. See `/AOK/docs/shortcuts.md`.
- Optional accelerators: native replacement of hot libc routines, and crypto and pixman offload.
- Extra diagnostics and operational changes that are specific to this fork.

## Guest Architectures

All four guests are supported and run through the gadget JIT. None of them runs
natively: an `arm64` guest instruction on an ARM host is still one gadget
dispatch, exactly like a `riscv64` one. Being the host's own ISA family makes
each gadget's body cheaper, not free.

| guest | status |
|---|---|
| `i386` | the original guest, JIT only |
| `amd64` | supported, JIT |
| `arm64` | supported, JIT |
| `riscv64` | supported, JIT |

The per-guest regression suites pass on all four on device. Note that the
interpreters are legacy and are being retired: new work should target the JIT.

Relevant files:

- [jit/gen.c](jit/gen.c) instruction translation for every guest
- [jit/jit.c](jit/jit.c) block cache and dispatch
- [kernel/calls.c](kernel/calls.c) per-ABI syscall tables
- [docs/amd64_port_plan.md](docs/amd64_port_plan.md)
- [docs/aarch64_guest_plan.md](docs/aarch64_guest_plan.md)

## Performance

The engine is dispatch-bound at roughly 6.8 ns per gadget dispatch, so cost
tracks guest instruction count. Measured method and numbers are in
[docs/perf_benchmarks_2026_08.md](docs/perf_benchmarks_2026_08.md).

Instruction fusion and the return caches can be toggled at runtime for A/B
measurement, per guest:

```sh
cat /proc/ish/arm64_jit_fuse          # one "name on|off" line per family
echo retcache=0 > /proc/ish/arm64_jit_fuse
echo all=1 > /proc/ish/riscv64_jit_fuse
```

Nodes exist for `i386`, `amd64`, `arm64` and `riscv64`. The bits are consumed at
translation time, so a change affects newly compiled blocks; run each timed
measurement as its own process. [tests/manual/jit_fuse_ab.sh](tests/manual/jit_fuse_ab.sh)
automates an interleaved A/B and restores the mask when it exits.

## Optional Accelerators

All three are **off by default** and opt-in:

| feature | CLI | what it does |
|---|---|---|
| HLE | `ISH_HLE=1` | replaces hot libc routines (`memcpy`, `strlen`, `memcmp`, ...) with native code — **arm64 and riscv64 guests only** |
| Crypto | `ISH_CRYPTO_ACCEL=1` | AES-GCM and ChaCha20-Poly1305 offload |
| Pixman | `ISH_PIX_ACCEL=1` | pixman composite offload |

HLE matters most, and only for the arm64 and riscv64 guests — `jit/jit.c` gates
it on those two, so an i386 or amd64 guest never takes the path and `ISH_HLE=1`
silently does nothing there. Measured against the same build with it off, on a
memcpy/memset/memcmp/strlen loop: 1.23x at 256 B, 3.16x at 4 KB, 7.17x at 64 KB,
6.68x at 1 MB
([docs/performance-optimizations-2026-07.md](docs/performance-optimizations-2026-07.md)).
The work happens inside one native call rather than one dispatch per guest
instruction, so it helps data-movement-heavy code and is neutral where a
program's own arithmetic dominates. It is a pure fast path: an unrecognized libc
simply never matches and falls through to ordinary translation.
`ISH_HLE_STATS=1` prints per-function call counts.

## Repository Layout

- `app/`: iOS app, UI, root selection, diagnostics, File Provider integration.
- `emu/`: guest CPU state, memory, TLB, FPU/vector support.
- `kernel/`: syscall translation, process model, exec, signals, memory management.
- `fs/`: filesystem layer, fakefs, procfs, tmpfs, mounts.
- `jit/`: the gadget JIT and its per-guest translators.
- `tests/`: end-to-end tests and the guest-side regression suite.
- `tools/`: developer tools and host-side helpers.

## Clone

This repo uses submodules.

```bash
git clone --recurse-submodules git@github.com:emkey1/ish-AOK.git
cd ish-AOK
```

If you already cloned without submodules:

```bash
git submodule update --init --recursive
```

Note that `--recursive` includes `deps/bash`, which makes the default build a
GPLv3 one. See [Native bash and licensing](#native-bash-and-licensing) if you
intend to distribute the result.

## Build Requirements

For local development you will typically want:

- Xcode
- Python 3
- Meson
- Ninja
- Clang/LLVM toolchain
- sqlite3
- libarchive

On macOS, common setup is:

```bash
brew install meson ninja llvm libarchive
```

`sqlite3` is usually already present.

On Apple silicon, note that the build looks for `llvm`, `libarchive` and
`unicorn` under `/opt/homebrew` before `/usr/local`. If an old Intel Homebrew is
still installed, its x86_64 copies are not used.

## Build the iOS App

Open [iSH-AOK.xcodeproj](iSH-AOK.xcodeproj) in Xcode and build the `iSH` scheme.

Important fork-specific settings:

- Bundle IDs are driven by [app/iSH.xcconfig](app/iSH.xcconfig).
- `ROOT_BUNDLE_IDENTIFIER` defaults to `app.ish.iSH-AOK`.
- The project uses the fork-specific debug configuration `Debug-ApplePleaseFixFB19282108`.

Command-line build for a device:

```bash
xcodebuild \
  -project iSH-AOK.xcodeproj \
  -scheme iSH-AOK \
  -configuration Debug-ApplePleaseFixFB19282108 \
  -destination 'generic/platform=iOS' \
  -allowProvisioningUpdates build
```

The iOS build scripts copy the rootfs archives into the app bundle from the repo
root. If one is missing, the corresponding bundled root will not work.

## Build the Native CLI / Emulator

For emulator-side work, the Meson build is much faster than full Xcode runs.

```bash
meson setup build --buildtype=debugoptimized
ninja -C build
```

Use `--buildtype=debugoptimized`. Meson's default is `debug` (`-O0`), and an
`-O0` emulator does not merely run slower, it invalidates measurements taken on
it. A guest on such a build reports `" unoptimized"` in `uname -v`.

Run a guest:

```bash
./build/ish -f build/alpine /bin/login -f root
```

Create a filesystem from a rootfs tarball:

```bash
./build/tools/fakefsify alpine-minirootfs-*.tar.gz alpine
```

## Native Programs

A native program is host code compiled into the app. `execve` of a path under
`/AOK/native` dispatches to a function inside iSH-AOK rather than loading a
guest image, and the caller cannot tell the difference. `/AOK/native` holds one
entry per program in the registry (`kernel/native.c`) — `smallclue`, `motepad`,
`hx`, `rust-probe`, `bash`, `zsh`, `zsh-multio` — and everything else is a
symlink to one of those, the link name selecting the applet exactly as busybox
does:

| program | what it is |
|---|---|
| `/AOK/native/smallclue` | busybox-style multicall toolbox, applet chosen by `argv[0]` |
| `ssh`, `scp`, `sftp`, `ssh-keygen`, `ssh-copy-id` | OpenSSH, applets of SmallCLUE (built without OpenSSL) |
| `vi` | the Nextvi editor, an applet of SmallCLUE |
| `/AOK/native/motepad` | a modeless terminal text editor, the counterpart to Workspace's MotePad applet |
| `/AOK/native/hx` | [helix](https://helix-editor.com), a modal editor with syntax highlighting. MPL-2.0, so like bash it has a build switch (`-Dnative_helix`); its grammars live under `/AOK/native/libs` |
| `/AOK/native/rust-probe` | exercises the Rust-on-the-shim path that `hx` is built on; not a tool you have a use for |
| `/AOK/native/bash` | see [Native bash and licensing](#native-bash-and-licensing) |
| `/AOK/native/zsh` | see [Native zsh](#native-zsh) |

`/AOK/tools/native-links.sh` builds the symlink farm that puts the applets on
`PATH`, and `--shell bash|zsh|/path` switches the login shell; `--remove` undoes
both. In-app documentation is at `/AOK/docs/native-programs.md` (what they are)
and `/AOK/docs/native-setup.md` (how to set them up), sources under
[opt/AOK/docs/](opt/AOK/docs).

The hard part is not speed, it is that a native program must answer questions
about the *guest* rather than about the iPhone it is running on: environment,
identity, filesystem, `/etc/hosts` and `/etc/resolv.conf`, terminfo, locale and
rc-file locations are all routed to the rootfs by a shim compiled in ahead of
the system headers (`kernel/native_libc.c`). The governing question is not "is
this function pure?" but "can this function's answer differ between the host and
the guest?". `tools/check-native-libc.py` is the gate for it: run over the built
objects, it reports every host-libc symbol a native program references that is
not on an explicit allowlist. It is run deliberately rather than wired into the
build.

## Native bash and licensing

bash is compiled into the app as a native program. The win is interpretation,
not forking: an arithmetic loop runs roughly 16x faster than under the emulated
shell, while subshells and command substitutions land near parity, because a
native program cannot `fork` and re-launches itself instead. Numbers and method
are in [docs/bash_native_plan.md](docs/bash_native_plan.md). It also puts GPLv3
code in the binary: bash itself, its bundled readline, and GNU termcap.

That matters for App Store distribution. iSH-AOK is GPLv3 too, but
[LICENSE.IOS](LICENSE.IOS) is a promise from *this project's* copyright holders
not to enforce against the conflict between the GPL and Apple's terms. It
cannot bind the FSF, which holds bash's copyright and has had GPL software
removed from the App Store twice — [GNU
Go](https://www.theregister.com/2010/05/27/gnu_go_fsf_apple_itunes/) in 2010
and [VLC](https://www.fsf.org/blogs/licensing/vlc-enforcement) in 2011, on the
grounds that the store's Usage Rules impose "further restrictions" barred by
[GPL section
6](https://www.fsf.org/blogs/licensing/more-about-the-app-store-gpl-enforcement).
The FSF states that analysis applies to all GPL versions, not only v3.

So it is a build option:

```bash
meson setup build .                          # auto: on if deps/bash is present
meson setup build . -Dnative_bash=disabled   # no third-party GPL in the binary
meson setup build . -Dnative_bash=enabled    # fail if deps/bash is missing
```

Configure prints which one you got, under a `Licensing` heading. Check it
rather than assuming:

```
Licensing
  native bash: no -- no third-party GPL in the binary
```

`disabled` leaves bash, readline and termcap out of the archive entirely — 0
objects, verified with `ar t`. Users still get bash: the emulated `/bin/bash`
from the guest rootfs, which is the same mere-aggregation position as every
other GPL tool in Devuan or Alpine.

**Removing the applet-table entry in `kernel/native.c` is not sufficient.**
`meson.build` folds these archives in with `link_whole`, so the objects ship
whether or not anything references them — measured, 144 bash and 35 readline
objects remain with the registry entry deleted. Only the build option removes
them.

Nothing else in the binary is third-party GPL: SmallCLUE is MIT, OpenSSH and
libarchive are BSD, liblzma is public domain, and `deps/linux` is not compiled
into this target.

## Native zsh

zsh is compiled in as a third native program, reachable as `/AOK/native/zsh`,
and is **on by default** — `-Dnative_zsh=disabled` leaves it out. Unlike bash
there is no licensing question: zsh's licence is permissive and none of its
compiled C is GPL.

It is a working shell. ZLE — the line editor — works: prompt, echo, editing,
history keys, line wrapping, full terminal negotiation. So does `fork`, which
was the thing that did not. A native program is a C function on a guest task's
thread rather than a process, so `fork` cannot copy an address space; zsh
instead serialises its own state into a script and re-launches itself, the
design proven first on bash (`deps/zsh/Src/aok_fork.c`, `deps/bash/aok_fork.c`).
Command substitution, pipelines, subshells and background jobs all go through
that path:

```
% echo $(echo A); echo B | tr B C; (echo D); sleep 0.1 & wait; echo E
A
C
D
E
```

MULTIOS redirections use a companion native program, `zsh-multio`, because the
descriptors have to be held by something that is not the shell.

`/AOK/tools/native-links.sh --shell zsh` will make it the login shell.

119 differential cases ship in the guest at
`/AOK/tests/native_zsh_fork_state.sh`, with every expectation taken from what
real zsh prints rather than from what looked reasonable; 116 of them pass. The
two that fail are **process substitution** — `<(...)` and `>(...)` — and that is
a property of the rootfs rather than of the shell: it needs `/dev/fd`, which the
Alpine image does not provide, so it fails identically under the emulated
`/bin/bash` there and works under both shells on Devuan, where `/dev/fd` is a
symlink to `/proc/self/fd`. Two known gaps that *are* the shell's are recorded
under *Known gaps* in
[docs/release-notes-since-iSH-AOK_549.md](docs/release-notes-since-iSH-AOK_549.md):
a pattern is compiled at first use and cached in the parse tree with nothing
recording the options in force at the time, so a re-launched child can compile
it under different options than its parent did; and `pipestatus` under a multio
reports `1 0` where zsh reports `0 0`.

The tree at `deps/zsh` is a submodule of
[emkey1/zsh](https://github.com/emkey1/zsh) on branch `ish-aok`. It carries
zsh's *generated* sources — `config.h`, `Src/signames.c`, the per-module
`.mdh`/`.epro`/`.pro` — committed against upstream's `.gitignore`, because this
build compiles zsh with meson and never runs zsh's own `make`. So a checkout
builds with no configure step:

```bash
git submodule update --init deps/zsh
```

It is configured termcap-only with all modules linked statically. Both are
forced: the iOS SDK ships the curses `.tbd` stubs without `curses.h`/`term.h`,
and a native program cannot `dlopen` — where `--disable-dynamic` alone silently
maps `zsh/regex` to `link=no` and `[[ =~ ]]` then fails at runtime.

## Regression Tests

Host-side tests:

```bash
meson test -C build
```

`float80` skips on hosts whose `long double` is not the x87 80-bit format, which
includes Apple silicon: there is no reference to compare against there. It runs
in full on an x86_64 host.

The guest-side suite is the primary regression gate. It lives in
[tests/manual/](tests/manual) and is served read-only inside the guest at
`/AOK/tests`, with roughly 120 focused programs covering signals, futexes,
process lifecycle, the filesystem layer, the JIT, and per-architecture
instruction behavior. Each exits non-zero on failure and accepts `-v`.

Inside a guest:

```sh
sh /AOK/tests/setup-regressions.sh --install-deps --run   # build and run everything
sh /AOK/tests/setup-regressions.sh --only fs_conformance,futex_core --run
```

Adding a test means dropping the source in `tests/manual/` and listing it in
[fs/aok-tests.manifest](fs/aok-tests.manifest), which is what publishes it to
`/AOK/tests`, plus [tests/manual/setup-regressions.sh](tests/manual/setup-regressions.sh)
so it is built and run. A test missing from the manifest is silently absent on
device.

Three suites are the exception: `native_zsh_fork_state.sh` (119 cases),
`native_bash_fork_state.sh` (20) and `native_stdio_redirect.sh` are shell
scripts rather than C, so `setup-regressions.sh` neither builds nor lists them.
They ship via the manifest and are run directly from `/AOK/tests`, and each
needs the matching native program to be present.

## Working with Root Filesystems

Bundled in the app: Alpine 3.23.3 and Devuan 6 (excalibur), `aarch64` only. The
Xcode "Download Root" phase installs those two archives and deletes the i386 and
x86_64 ones from Resources, so they are the only roots present before any
download. The same two distros for `i386`, `x86_64` and `riscv64`, plus Arch,
are downloadable from within the app; the catalogue is
[deps/rootfs-manifest](deps/rootfs-manifest).

The root-selection UI and metadata handling live in:

- [app/Roots.m](app/Roots.m)
- [app/RootsTableViewController.m](app/RootsTableViewController.m)

Notes:

- The app records the guest ABI per imported root.
- Every installed root is also exposed read-write at `/AOK/roots/<name>` in the
  booted guest, so you can chroot into another architecture's userland.
- File Provider domains are synchronized for managed roots.

## Logging and Diagnostics

Logging is controlled by `ISH_LOG` in [app/iSH.xcconfig](app/iSH.xcconfig), or
`meson configure -Dlog=...` for the CLI build.

```xcconfig
ISH_LOG = verbose strace
```

Common channels: `strace` (syscall parameters and return values, the most
useful), `verbose`, and `instr` (every instruction, very slow).

Logger defaults are `nslog` on iPhone and the simulator, `dprintf` on macOS.

## File Provider

This fork includes an iOS File Provider extension so guest files can be surfaced
through the system file APIs.

- [app/FileProvider/FileProviderExtension.m](app/FileProvider/FileProviderExtension.m)
- [app/FileProvider/FileProviderEnumerator.m](app/FileProvider/FileProviderEnumerator.m)
- [app/FileProvider/FileProviderItem.m](app/FileProvider/FileProviderItem.m)

This is fork-specific functionality and part of the maintained product surface
here.

## Release Automation

[tools/release-aok.sh](tools/release-aok.sh) wraps the archive and export flow:

```bash
./tools/release-aok.sh preflight
./tools/release-aok.sh archive
./tools/release-aok.sh export latest /tmp/iSH-AOK-export
./tools/release-aok.sh upload-fastlane      # full TestFlight automation
```

`upload-fastlane` uses the existing `fastlane upload_build` lane and requires a
Ruby/Bundler/Fastlane setup plus signing and auth secrets.

Releases themselves are cut by bumping `CURRENT_PROJECT_VERSION`, adding
`docs/release-notes-since-iSH-AOK_<N>.md` and `docs/release-summary-iSH-AOK_<N>.md`,
and tagging that commit `builds/iSH-AOK_<N>`. The tag name is load-bearing:
`.github/workflows/build-release-ipa.yml` triggers on `builds/iSH-AOK_*`, so a
differently named tag produces no release build.

## Branches

- `working` is the default branch and the active integration branch. Bug fixes,
  feature work, and release candidates land here.
- `amd64`, `aarch64` and `riscv` were the original per-guest bring-up branches.
  That work is merged into `working`, which builds all four guests.

## Upstream Relationship

iSH-AOK is based on upstream iSH, but it is intentionally diverged.

That means:

- upstream README instructions may be incomplete or wrong for this fork
- branch names and build configurations differ
- bundled roots and operational behavior here are fork-specific
- the amd64, arm64 and riscv64 guests here should not be assumed to exist upstream

If you use the `gh` CLI in a clone that has an `upstream` remote, pass
`--repo emkey1/ish-AOK`. Without it `gh` resolves to `ish-app/ish` and will
answer about upstream's workflows, releases and tags instead of this fork's.

## Acknowledgments

The ARM64 guest work is motivated by, and in places adapted from,
[OpenMinis/ish-arm64](https://github.com/OpenMinis/ish-arm64), a GPLv3 fork of
`ish-app/ish` that added the same capability independently. See
[docs/CREDITS-aarch64.md](docs/CREDITS-aarch64.md) for file-level attribution.

## License

See:

- [LICENSE.md](LICENSE.md)
- [LICENSE.IOS](LICENSE.IOS)
