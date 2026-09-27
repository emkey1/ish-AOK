# Release Notes Since `builds/iSH-AOK_524`

These notes summarize changes from `builds/iSH-AOK_524` intended for `builds/iSH-AOK_525`.

## Highlights

- Advanced experimental amd64/x86_64 guest bring-up with broader JIT/gadget coverage and a more complete GCC/binutils smoke path.
- Fixed amd64 signal, thread, and interrupted-syscall restart behavior caught by the guest regression suite.
- Expanded amd64 atomic and assembler-workaround coverage for `xadd`, `cmpxchg`, `cmpxchg8b`, and common flag/logic paths.
- Improved Linux ptrace behavior across syscall stops, exec/clone events, detach/resume, SIGTRAP metadata, and wait wakeups.
- Added and refined workspace/browser UI features including workspace themes, switcher placement, memory-aware browser tabs, and phone layout behavior.
- Removed release-hostile debug autorun and gated remaining amd64 bring-up probes behind explicit trace environment variables.

## User-Facing Changes

### Experimental amd64 Support

- Added amd64 JIT frontend block caching.
- Expanded JIT helper coverage for memory operations, shifts, group-3 instructions, sign/zero extension, string operations, `xchg`, immediate `imul`, 16-bit stores, and common SSE opcodes.
- Added smoke-test fallback coverage so unsupported amd64 JIT cases fall back cleanly instead of silently misbehaving.
- Fixed amd64 thread signal delivery and interrupted syscall restart handling.
- Improved atomic regression coverage and fixed code paths needed by `atomic_xadd32`, `atomic_cmpxchg32`, `atomic_cmpxchg8b`, and `atomic_logic32`.
- Added GAS workaround support for immediate/register rewrite cases that currently trip Alpine binutils in the emulated environment.

### Compatibility and Stability

- Fixed ptrace syscall entry register reporting and syscall stop phase reset.
- Matched Linux-style ptrace exec stops and event siginfo behavior more closely.
- Added ptrace clone event stops, detach handling, resume signal handling, and wait4 wakeup fixes.
- Enabled `statx` for i386 userspace.
- Fixed select/poll registration failure handling and Darwin FIFO polling/rescan behavior.
- Added quiet syscall handling for `sync_file_range` and explicit unsupported handling for `seccomp` in both i386 and amd64 syscall tables.
- Fixed procfs and amd64 bring-up regressions from the previous build window.

### App and Workspace

- Added a browser workspace utility with configurable home button support.
- Added memory-aware browser tabs and refreshed browser tab limits dynamically.
- Added workspace themes, native-app theming, generated wallpaper application, and improved theme previews.
- Added and refined grouped workspace utilities, workspace switcher placement, dashboard sizing, dock behavior, and phone layout handling.
- Improved workspace scene activation, switcher persistence, and terminal/window placement behavior.
- Removed the debug terminal SSH autorun path.
- Deferred the Swift/App Shortcuts integration from this release after it changed the archive shape by embedding Swift runtime support and triggered Xcode Organizer's `Copy failed` export path.

## Testing and Developer Support

- Extended guest regression coverage for amd64 atomics, JIT helper fallbacks, signals, eventfd/futex restart paths, process lifecycle, and pthread synchronization.
- Added an Alpine 3.23.3 x86_64 root image with GCC for local amd64 regression testing.
- Gated noisy amd64 compiler, assembler, tty, htop, bash, apt/dpkg filesystem, poll/select, and socket probes behind explicit `ISH_TRACE_*` environment variables for release builds.
- Restored release packaging to the same compact Alpine i386 and x86_64 minirootfs archives used by `builds/iSH-AOK_524`.
- Restored standard release dSYM generation so Organizer receives a real symbol bundle instead of an empty archive `dSYMs` directory.
- Restored the app archive to the Objective-C-only shape used by `builds/iSH-AOK_524`; release archives no longer contain `SwiftSupport` or embedded `libswift*.dylib` files.

## Known Notes

- amd64/x86_64 guest support remains experimental.
- The Alpine binutils assembler workaround remains a compatibility shim for the current test root and should be revisited once the underlying GAS issue is no longer relevant.
- Re-archive after this change before exporting; older archives may still contain the oversized Devuan/full x86_64 test roots.
- Re-archive after the dSYM setting restoration before using Organizer; archives built with empty `dSYMs` can still hit Xcode's `Copy failed` path.
- Re-archive after the App Shortcuts rollback before using Organizer; archives containing `SwiftSupport` can still hit Xcode's `rsync -E` symbol-copy failure during validation/upload.

## Maintainer Notes

- `builds/iSH-AOK_524` points to commit `eaf1a830`.
- `builds/iSH-AOK_525` tags the release sanity cleanup after the range below.
- These notes cover committed history after `builds/iSH-AOK_524` plus the release sanity cleanup in the current worktree.
- CLI export command: `tools/export-ios-archive.sh /path/to/iSH.xcarchive /tmp/iSH-AOK-export`.

## Commit Range

- `0c3e0697` Fix interrupted syscall restart handling
- `7f8af2f5` Work around Xcode IPA symbol copy failure
- `2cc33a5f` Fix release archive root bundle
- `5778c072` Prepare iSH-AOK 525 release
- `eea40e4f` Fix amd64 thread signal regressions
- `2c8ec6c5` Remove debug terminal autorun
- `6af16115` Rewrite immediate push in GAS workaround
- `c5596127` Fix amd64 atomic logic flag capture
- `cfa2bcb7` Support amd64 cmpxchg8b regression
- `d6c88122` Avoid GAS cmpxchg mnemonic crash in regression
- `64e803b8` Cover remaining amd64 JIT smoke fallbacks
- `7427597a` Expand amd64 JIT helper coverage
- `66e9da40` Add amd64 JIT helpers for common SSE opcodes
- `8dacdb13` Ignore local editor configuration files
- `c493a9ec` Handle 16-bit amd64 JIT memory stores
- `c45afde4` Add amd64 JIT xchg and immediate imul helpers
- `1726e540` Add amd64 JIT string and sign-extension helpers
- `5b300d80` Add amd64 JIT shift and group3 helpers
- `98a094fb` Improve amd64 JIT helpers and regression harness
- `25b82743` Expand amd64 JIT memory helper coverage
- `89fc5708` Cache amd64 JIT frontend blocks
- `7d604130` Expand amd64 JIT helper coverage
- `fdfb7580` Add app shortcuts and advance amd64 bring-up
- `852cc58c` Improve runtime compatibility and debugging support
- `12cdf316` Keep workspace zoom restore frame-only
- `506322f2` Add workspace terminal titlebar zoom toggle
- `1f369b3a` Emit ptrace exit events for traced tasks
- `298bdbac` Implement ptrace detach and resume signals
- `bc53cdf0` Fix ptrace wait4 lost wakeups
- `1b6992ca` Fix ptrace SIGTRAP stop siginfo semantics
- `028f94ef` Use ptrace event stops for clone tracing
- `c389e0a6` Match Linux ptrace exec stop semantics
- `07532f3c` Stop ptrace before syscall restart rewind
- `4b9eaf4f` Fix ptrace event siginfo semantics
- `fc4886dc` Make ptrace syscall entry regs Linux-like
- `0da8d005` Fix ptrace syscall stop phase reset
- `7bd73b90` Enable statx for i386 userspace
- `5d2513e0` Rescan Darwin FIFO write polls periodically
- `97f6421b` Handle select poll registration failures
- `332201a2` Fix RAM tiering and Darwin FIFO polling
- `de49d2f9` Refresh browser tab cap dynamically
- `d53001ad` Fix browser RAM tier thresholds
- `7fb3b308` Fix browser tab strip controls
- `7de3b5b0` Add memory-aware browser tabs
- `aa652eeb` Remove dashboard header text
- `01501a02` Add configurable browser home button
- `e3fe19c4` Improve workspace theme contrast
- `487e2f45` Add browser workspace utility
- `daff75cd` Make dock position global across workspaces
- `8dd64299` Make shortcuts utility scroll correctly
- `291dbfc6` Trim dashboard window and remove mini mode
- `31c3c588` Separate dashboard and scene layouts
- `9f3d460e` Disable workspace scenes on phones
- `9f39a2cd` Sync switcher frame across workspace scenes
- `e4877739` Reapply switcher on scene activation
- `9f133cee` End editing before workspace scene switches
- `2662de9f` Stop saving switcher during scene switches
- `665f612b` Bypass generic init for workspace switcher
- `d36a1405` Exclude switcher from compact sizing
- `31effbac` Persist switcher only on user move
- `2ff84a8d` Use global workspaces switcher position
- `764f45c6` Reapply switcher frame on activation
- `5797f9bc` Unify workspaces window placement
- `144342e6` Persist switcher frame on drag
- `814568fa` Persist workspaces switcher placement
- `856cedda` Compact workspace switcher
- `6dee47ac` Fix process utility and switcher restore
- `05e30200` Add workspace switcher utility
- `477fb4bc` Add grouped workspace utilities
- `246102f0` Tune workspace defaults for phones
- `749180b0` Apply workspace wallpaper on startup
- `b677d860` Use direct workspace host for wallpaper apply
- `383554f6` Shorten clock util and reinforce wallpaper apply
- `906d35e6` Apply generated workspace wallpaper directly
- `6241131f` Fix workspace theme library taps
- `d7bdaec5` Compact workspace utility card geometry
- `03f62552` Tighten workspace utility typography and theme selection
- `a1ec14e7` Force compact workspace utility sizing
- `bd056a40` Compact workspace utilities and fix dock layout restore
- `22c79ec0` Fix workspace theme image previews
- `4f0fa92d` Polish workspace theme chrome and previews
- `6f62b381` Add dedicated workspace themes utility
- `4a533c04` Theme workspace native apps
- `394c775f` Refine workspace dashboard and dock behavior
- `b24f3e66` Fix procfs and amd64 bring-up regressions
