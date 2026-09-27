# Release Notes Since `builds/iSH-AOK_521`

These notes summarize changes from `builds/iSH-AOK_521` through the current `working` head, including `builds/iSH-AOK_522`.

## Highlights

- Added bundled root filesystem selection during initial setup.
  - `Devuan5(Debian12)` from `root.tar.gz`
  - `Alpine3.23.3` from `alpine-minirootfs-3.23.3-x86.tar.gz`
- Initial setup now chooses a more appropriate first window automatically.
  - Devuan defaults to `System Console`
  - Alpine defaults to `Session Shell`
- Added an in-app diagnostics screen and export flow to improve crash and hang triage.
- Improved Linux networking compatibility, especially for netlink-based tools.

## User-Facing Changes

### Root Filesystems and First Launch

- First launch now presents bundled filesystem choices instead of forcing a single default import.
- Bundled filesystems can also be imported later from `Settings -> Filesystems`.
- Bundled root display names were cleaned up for clarity.
- Root import now performs a free-space preflight before extraction.
- Out-of-space failures during extraction are handled more cleanly and reported more clearly.
- Failed imports clean up temporary extraction state more reliably.
- Switching the boot root from `Settings -> Filesystems` now uses the controlled app restart path instead of looking like a foreground crash.

### Diagnostics and Crash Triage

- Added a diagnostics screen inside the app.
- Added diagnostics export support.
- Added lifecycle breadcrumbs for launch, scene transitions, background/foreground flow, root import, and terminal/WebKit failures.
- Added MetricKit payload capture and presentation to help investigate anonymous crash and hang reports.

### Networking and Linux Compatibility

- `ip addr` now works instead of failing with `EOF on netlink` / `Dump terminated`.
- `ip route` and `netstat -rn` now use a normalized route view instead of dumping unusable Apple host routes.
- Added route normalization to prefer useful interfaces and suppress noisy Apple-internal ones where possible.
- Added enough rtnetlink support for practical route and address enumeration.
- Added Linux socket ioctls needed by networking tools, including support used by `ifconfig`.
- Fixed netlink error handling so ordinary Linux `EINVAL`/`EOPNOTSUPP` replies do not log as `unknown error -22`.
- `ip route` interface names now resolve correctly instead of falling back to `ifNN`.

### Alpine Compatibility

- Improved generated `/etc/resolv.conf` handling so unusable link-local IPv6 nameservers are not written blindly.
- If iOS reports no usable nameservers, the existing resolver file is preserved instead of being replaced with bad data.
- Unmanaged Alpine roots now normalize upstream `apk` repositories away from HTTPS where needed for current iSH-AOK behavior.
- Fixed `/proc/self/fd/N` reopening behavior so BusyBox trigger scripts and similar workflows no longer inherit the wrong file offset/state.

### Filesystem and procfs Fixes

- Fixed procfs File Provider enumeration issues.
- Fixed bogus procfs inode collisions that caused tools like `du` to report circular directory structures under `/proc/ish`.
- Fixed the follow-on proc entry assertion caused by nameless proc entries after the inode cleanup.

## Build and Project Changes

- Cleaned up several warnings across app and kernel code.
- Fixed build-script issues under newer Xcode releases.
- Improved Meson/Ninja integration and script output declarations.
- Stopped tracking bundled rootfs archives in git while keeping local build support intact.

## Maintainer / Internal Notes

- Added `amd64_port_plan.md`, a phased architecture plan for future x86_64 support work.
- Added netlink regression coverage under `tests/e2e/net_regress`.
- Added shared route synthesis code used by both `/proc/net/route` and rtnetlink.
- Added diagnostics storage and export plumbing used by the new in-app diagnostics screen.

## Commit Range

- `fef6351f` Fix procfs File Provider enumeration
- `bfe695f3` Add amd64 port architecture plan
- `714de0ef` Improve diagnostics, build scripts, and netlink compatibility
- `9b8f13b6` Improve rootfs setup and Linux compatibility
- `07e26e1a` Stop tracking bundled rootfs archives
- `0a1aa508` Refine initial root selection behavior
