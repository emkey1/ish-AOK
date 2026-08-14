# iSH-AOK

iSH-AOK is a fork of [ish-app/ish](https://github.com/ish-app/ish) with local product, tooling, and platform changes for day-to-day development on this tree.

Testflight: https://testflight.apple.com/join/X1flyiqE

This fork is not just a rebrand. It carries fork-specific behavior, bundled roots, diagnostics work, File Provider integration, and support for four guest architectures. If you want upstream iSH, use `ish-app/ish`. If you are working in this repository, this README is the relevant one.

## What This Fork Adds

- Fork-specific app identity:
  - product name `iSH-AOK`
  - bundle root `app.ish.iSH-AOK`
- **Four guest architectures**, all JIT: `i386`, `amd64` (x86_64), `arm64` (aarch64), and `riscv64`.
- Bundled root filesystems in the app build (Alpine 3.23.3 and Devuan 6, each for i386, x86_64 and aarch64), plus additional downloadable images including riscv64.
- File Provider support for exposing guest files through iOS.
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

The per-guest regression suites pass on all four. Note that the interpreters are
legacy and are being retired: new work should target the JIT.

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
| HLE | `ISH_HLE=1` | replaces hot libc routines (`memcpy`, `strlen`, `memcmp`, ...) with native code |
| Crypto | `ISH_CRYPTO_ACCEL=1` | AES-GCM and ChaCha20-Poly1305 offload |
| Pixman | `ISH_PIX_ACCEL=1` | pixman composite offload |

HLE matters most. On a loop dominated by the routines it replaces, it takes the
guest from roughly 250x slower than native to roughly 1.4x, because the work
happens inside one native call rather than one dispatch per guest instruction.
It is a pure fast path: an unrecognized libc simply never matches and falls
through to ordinary translation. `ISH_HLE_STATS=1` prints per-function call
counts.

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
  -scheme iSH \
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

## Working with Root Filesystems

Bundled in the app: Alpine 3.23.3 and Devuan 6 (excalibur), each for `i386`,
`x86_64` and `aarch64`. Further images, including `riscv64` and Arch, are
downloadable from within the app; the catalogue is
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
