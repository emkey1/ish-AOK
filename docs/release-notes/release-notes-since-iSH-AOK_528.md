# Release Notes Since `builds/iSH-AOK_528`

These notes summarize changes from `builds/iSH-AOK_528` intended for `builds/iSH-AOK_529`.

## Highlights

- Fixed fakefs package-install failures caused by stale host-side temporary link artifacts during `dpkg` upgrades.
- Improved signal handling compatibility by no longer ignoring `SIGURG` by default.
- Fixed guest write-fault handling and related debugger/runtime crash paths.
- Added amd64 `cc1` diagnostics and safer fallback controls to narrow JIT-related failures.
- Unified app and File Provider build versions and avoided wrong-architecture Unicorn selection on Apple Silicon.

## User-Facing Changes

### Package Management and Filesystem

- Fixed a fakefs/realfs desynchronization case where stale host-side temporary paths could make `dpkg` fail while creating backup links during package upgrades.
- Added recovery logic in fakefs link handling so stale host-only temp artifacts are removed and retried automatically when fakefs has no corresponding inode entry.

### Runtime Compatibility and Stability

- Stopped ignoring `SIGURG` by default, improving compatibility with runtimes that install a real `SIGURG` handler, including newer Go toolchains.
- Fixed guest write-fault handling and related debugger crash paths.

### amd64 Diagnostics and Fallbacks

- Added amd64 `cc1` diagnostics and interpreter fallback support to narrow suspected JIT failure ranges.
- Added a runtime knob for amd64 `cc1` interpreter fallback and narrowed the suspect JIT ranges further.
- Improved Apple Silicon behavior by avoiding wrong-architecture Unicorn usage.

### Build and Release Plumbing

- Unified app and File Provider build versions.
- Updated the Fastlane bundle for current JWT security fixes.
- Kept disabled dpkg trace toggles in the shared Xcode scheme for future debugging.

## Maintainer Notes

- `builds/iSH-AOK_528` now points to commit `e379f10d`, the actual 528 release commit.
- `builds/iSH-AOK_529` points to commit `efd1f90d`, the current pre-Fastlane release tip.
- The subsequent Fastlane retargeting commit is intentionally not part of build 529.

## Commit Range

- `91b619a1` Fixed typo
- `dcb0f750` Fix guest write fault handling and related debugger issues
- `929f0197` Add amd64 cc1 diagnostics and interpreter fallback
- `6226263c` Add runtime knob for amd64 cc1 interpreter fallback
- `c7c86873` Narrow amd64 cc1 JIT suspect ranges
- `e3735527` Update fastlane bundle for jwt security fix
- `6ec20a31` Unify app and File Provider build versions
- `eb596467` Avoid wrong-arch Unicorn on Apple Silicon
- `0866428d` signal: stop ignoring SIGURG by default
- `b74537b7` fakefs: recover stale host links during link
- `efd1f90d` xcode: keep dpkg trace toggles in shared scheme
