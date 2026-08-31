# 34. Two builds

iSH-AOK has two build systems, and only one of them matters.

That is not a criticism of the other. The meson build is faster, more
configurable, runs on Linux, and is where nearly all of the emulator work in
this book was done. But it produces a command-line binary, and the product is an
iOS application, and the application is built by Xcode.

The rule the project states for itself is blunt:

> **Xcode is the only build.** A meson/ninja result is a development convenience,
> not a deliverable. A feature not in the Xcode build is not done.

Three times that rule was learned by breaking it, which is why it is stated in
those terms.

## 34.1 What each build is for

**The meson build** produces `build/ish`, the standalone emulator, plus the
tools — `fakefsify`, `ptraceomatic`, `unicornomatic`, the VDSO builder. It runs
on macOS and on Linux. It builds in seconds rather than minutes, it can be
configured a dozen ways at once in separate directories, and it is what a
lockstep differential run or a `meson test` sweep uses.

**The Xcode build** produces the app: the emulator as a static library, plus
UIKit, plus the Swift intents, plus the File Provider extension, plus the
bundled rootfs archives, signed and packaged.

The two are not independent. Xcode does not compile the emulator itself — it
invokes `xcode-meson.sh` and `xcode-ninja.sh`, which configure and drive meson
under Xcode's environment, then links the resulting `libish.a`. So the emulator
sources have exactly one build description, and the app build consumes it.

That is the right architecture and it is also the source of every trap in this
chapter, because *what Xcode passes to meson* is a second configuration surface
that looks like it does not exist.

## 34.2 The three times "it works" was wrong

> **The bug that taught us this**
>
> Three separate features were declared working on the strength of a `ninja`
> build and then handed over in an Xcode build that did not have them.
>
> **`native_helix` had no Xcode knob at all**, so `xcode-meson.sh` never passed
> it, and the device build silently had no editor after it had been reported
> ready.
>
> **The `-force_load` for the Rust object only ever reached meson's own link
> line**, so the first device build failed to resolve the entry point. Chapter 25
> gave the general form of this: a meson `declare_dependency(link_args:)` reaches
> the CLI build and nothing else, because Xcode links `-lish` against the archive
> meson produced rather than linking *through* meson.
>
> And the third time was after adding the missing knob — and defaulting it
> wrongly, so the option existed and was off.
>
> The rule that came out of it is procedural rather than technical: **verify
> what the user runs.** Not a build with the same sources; the build they will
> install, in the shape they will install it.

## 34.3 The configuration matrix

`meson_options.txt` is the feature matrix for the emulator, and it is worth
reading as documentation of what this system can be:

| option | what it decides |
|---|---|
| `guest_archs` | which of `i386`, `amd64`, `arm64`, `riscv64` this build can execute |
| `engine` | `jit` — the interpreters are no longer selectable (Chapter 7) |
| `arm64_gret` | `dmb` or `ldar` dispatch (Chapter 6's 2.04x regression) |
| `native_bash` | GPLv3 in the binary or not (Chapter 26) |
| `native_zsh`, `native_helix`, `native_rust` | which native programs exist |
| `log`, `nolog`, `log_handler` | which channels are compiled in, and where they go |
| `jit_cpu_family`, `vdso_c_args`, `cargo_home`, `native_rust_target` | toolchain plumbing |

The Xcode side is thirteen `.xcconfig` files, which is more than it sounds like
because they compose: project-level, per-configuration (debug and release),
per-target (app, static library, CLI, App Store), and the fork's own identity
in `app/iSH.xcconfig` — `ROOT_BUNDLE_IDENTIFIER`, the product name, the `ISH_LOG`
channels.

One configuration name in that set is a small piece of history:
`Debug-ApplePleaseFixFB19282108`. It exists because of a filed radar, it is
named after the radar, and anybody who wonders why the debug configuration has
an odd name can search for the number.

## 34.4 `-O0` is not a slower build, it is a different one

The most consequential build flag in this project is the optimization level, and
not for the usual reasons.

```sh
meson setup build --buildtype=debugoptimized
```

Meson's default is `debug`, which is `-O0`. An `-O0` emulator does not merely
run slowly — **it invalidates every measurement taken on it**, and it does so
non-uniformly. Chapter 33 has the sharpest example: at `-O0` the host cipher
runs 7–17x slower and the crypto accelerator *loses* to the emulation it
replaces. A conclusion drawn there is not a slower version of the truth; it is a
different answer.

Because that is so easy to get wrong, the build tells the guest:

```
$ uname -a
Linux Mac.lan 5.20.66-ish_aok iSH-AOK built 2026-08-31 10:24Z unoptimized aarch64 Linux
```

`unoptimized` in `uname -v`. It costs a string and it means no benchmark result
can be quoted without the reader being able to check that condition.

## 34.5 Generated code

A surprising amount of this system is generated at build time, and each
generator solves a synchronization problem:

- **`tools/gen-aokfs.py`** turns manifests plus source files into the C string
  tables that back `/AOK` (Chapter 21).
- **`tools/gen-nlibc-renames.py`** reads the shim header and emits the
  symbol-rewriting flags for foreign objects (Chapter 23) — *read from the
  header*, so a hand-kept second list cannot drift.
- **`tools/gen-native-syscalls.py`** generates the native syscall plumbing.
- **`jit/offsets.c`** emits structure offsets for the assembly gadgets, which is
  what makes `struct cpu_state`'s layout part of the JIT's ABI (Chapter 5).
- **`hterm/bin/mkdist`** builds the terminal bundle, and Chapter 29 told what
  happened when it was not run.
- **The VDSO** is compiled and embedded by its own subproject.

The common shape: wherever two places have to agree, one of them is generated
from the other. That is the same reasoning as `/AOK/native` being served from
the registry rather than from a second list (Chapter 22).

## 34.6 Submodules, and a trap that eats worktrees

The tree carries nine submodules — `libapps` (hterm), `libarchive`,
`rootfs-manifest`, `smallclue`, `bash`, `zsh`, `helix`, `tokio`, `signal-hook` —
and a full clone needs `--recurse-submodules`.

Two things about them are worth knowing before they cost a day.

`--recursive` includes `deps/bash`, **which makes the default build a GPLv3
one** (Chapter 26). That is stated in the README because a clone command should
not silently decide a licensing question.

And: **never run `submodule update --init` from a git worktree.** Worktrees share
the main checkout's git directory, so a submodule update from one reaches into
the other's state. The safe pattern for a worktree that needs submodules is a
`clone --shared` instead.

## 34.7 The simulator is not one target

Building for the simulator exposed four stacked faults, each hidden by the one
before it, and three of them generalize past this project.

**Cargo's triple is not clang's.** Cargo says `aarch64-apple-ios-sim`; clang
wants `arm64-apple-ios15.0-simulator` — a different environment word, and the
version goes *before* the environment. The script had been building one from the
other by concatenation, which is valid for the device only by coincidence
(`aarch64-apple-ios` + `15.0` happens to parse). **Never build one triple from
another by string concatenation; translate per environment.**

**The simulator's `ARCHS` has two entries and the device's has one.**
`ARCHS_STANDARD` for `iphonesimulator` is `arm64 x86_64`, and the build scripts
fan out one meson directory per architecture. Anything written as
`[[ "$arch" == arm64 ]] && x=...` silently leaves the second slice unset — and an
unset `-Dnative_rust_target` is *not* inert: cargo then builds for the **host**,
and macOS objects reach an iOS link.

**`ONLY_ACTIVE_ARCH` does nothing under `xcodebuild` without `-destination`.** It
is forced to `NO`, so the `YES` in the debug xcconfig never applied — which is
why a command-line build fans out to both architectures when a developer expects
one.

The fourth was the entitlements, and it belongs to Chapter 37.

## 34.8 The gate that grades yesterday

One more trap, because it undermines a safety mechanism rather than a feature.

`tools/check-native-libc.py` (Chapter 23) reads the built **archives**. And
`ninja -C build ish` does not refresh them — it relinks the binary. So running
the gate after an incremental build of just the emulator grades whatever
`libish.a` was last written, which may be from before the change being checked.

A green result from a stale archive is worse than no result, because it is
indistinguishable from a real one. The rule is to run a bare `ninja -C build`
before the gate, and the wider lesson is that **any tool reading build artifacts
needs to know how those artifacts get refreshed** — which is not a question most
static-analysis tools invite you to ask.

## 34.9 Why two builds is the right number

It would be tidier to have one. It would also be worse.

The meson build makes the emulator testable *as a program*: `ptraceomatic` needs
to link the kernel into a differential harness (Chapter 9), the conductor needs
to run eight configurations of it (Chapter 9), and the Linux CI needs to compile
it with GCC (Chapter 35). None of that is possible through an iOS app target.

The Xcode build makes it a product. Signing, extensions, entitlements, Swift
intents, App Store packaging — all of it is Xcode's, and none of it is
meaningfully reproducible elsewhere.

The cost of having both is exactly the class of bug in Section 34.2: a
configuration surface between them that is easy to forget. The mitigation is not
architectural, it is procedural, and it is the same sentence the project keeps
having to repeat — **build what the user will run, and check it there.**

---

*Anchors:* [meson.build](../../meson.build), [meson_options.txt](../../meson_options.txt),
[xcode-meson.sh](../../xcode-meson.sh), [xcode-ninja.sh](../../xcode-ninja.sh),
[app/iSH.xcconfig](../../app/iSH.xcconfig), [app/Project.xcconfig](../../app/Project.xcconfig),
[app/XcodeDebug.xcconfig](../../app/XcodeDebug.xcconfig),
[iSH-AOK.xcodeproj](../../iSH-AOK.xcodeproj), [tools/](../../tools),
[jit/offsets.c](../../jit/offsets.c), [vdso/](../../vdso),
[.gitmodules](../../.gitmodules), [kernel/uname.c](../../kernel/uname.c),
[ci_scripts/ci_post_clone.sh](../../ci_scripts/ci_post_clone.sh).

*Story:* a device build with no editor in it — because `native_helix` had no
Xcode knob, so the option that turned it on existed only in the build nobody
ships.
