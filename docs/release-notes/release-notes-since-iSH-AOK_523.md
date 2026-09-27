# Release Notes Since `builds/iSH-AOK_523`

These notes summarize committed changes from `builds/iSH-AOK_523` through `builds/iSH-AOK_524`.

## Highlights

- Added broad experimental amd64/x86_64 guest support, including a new amd64 interpreter path, ABI-aware syscall dispatch, amd64 ELF loading, and bundled x86_64 Alpine rootfs support.
- Expanded amd64 Linux compatibility across memory management, signals, futexes, ptrace, sockets, `/proc`, and core syscall marshalling.
- Added a guest-visible `/AOK/tests` regression suite plus new host-side/manual test coverage for futexes, signals, atomics, process lifecycle, and thread synchronization.
- Improved root selection, startup behavior, File Provider integration, workspace startup controls, and terminal/session handling in the app.
- Hardened task lifetime, procfs access, JIT fault handling, and shared-memory/process bookkeeping to reduce crash and teardown issues.

## User-Facing Changes

### Experimental amd64 Support

- Added experimental amd64 syscall entry and ABI scaffolding.
- Added amd64 ELF loader support and widened guest address handling.
- Added bundled x86_64 Alpine rootfs support, labeled as test-only for bring-up.
- Added substantial amd64 instruction coverage across integer, SSE, MMX, x87, string, bit-test, compare/exchange, and control-flow operations.
- Added amd64-native handling for key syscalls including `arch_prctl`, `set_tid_address`, `open*`, `rt_sigprocmask`, `getrandom`, `getdents64`, and several memory-management calls.

### Compatibility and Stability

- Improved amd64 signal delivery, signal frame layout, signal return handling, altstack compatibility, and trap context bookkeeping.
- Added amd64 ptrace register, floating-point state, and regset support.
- Fixed a number of amd64 `mmap`, `mprotect`, PIE load-bias, TLB, and high-address user-copy issues.
- Improved futex and shared-memory behavior, including guest-width-safe `shmctl` handling.
- Improved procfs compatibility and task retention so tools such as `htop` and procfd consumers behave more reliably.

### App and Filesystem Behavior

- Refined startup root selection and startup-mode handling.
- Improved terminal session handling and reduced session/workspace startup edge cases.
- Updated File Provider behavior and version alignment.
- Improved scene startup and terminal recovery behavior.

## Testing and Developer Support

- Added a guest regression suite under `/AOK/tests`.
- Added new manual test programs for futexes, atomics, realtime signals, altstack, restart behavior, process lifecycle, and pthread synchronization.
- Added setup scripts and Meson wiring for the new manual regression coverage.
- Expanded amd64 bring-up diagnostics and tracing to support compiler, shell, cargo, rustc, and memory-fault investigation during development.

## Bundled Roots and Documentation

- Added bundled Alpine 3.23.3 x86 and x86_64 root images.
- Updated README documentation for the current fork and startup/rootfs behavior.
- Added updated amd64 bring-up planning and architecture notes.

## Maintainer Notes

- `builds/iSH-AOK_524` points to commit `eaf1a830`.
- These notes cover committed history only. Current local uncommitted changes in the worktree are not included in the tag.

## Commit Range

- `e5ee66b1` Add workspace monitor and startup controls
- `6c91c1aa` Add startup filesystem chooser and release notes
- `f55fb225` Add guest ABI scaffolding and sparse MM core
- `8face218` Fix htop procfs compatibility and scene startup
- `164c4e85` Stabilize JIT faults and procfs task memory access
- `e638c805` Add bundled x86_64 Alpine rootfs option
- `5faba212` Wire experimental amd64 exec and syscall entry
- `0b5502c7` Add amd64 interpreter fallback path
- `cc1b85cf` Add Alpine root images
- `14eb4b0e` Add guest regression suite and signal fixes
- `45fbba8d` Expand guest regression coverage and fix signal/futex paths
- `9586d067` Improve amd64 stability and terminal session handling
- `733a37ea` Fix amd64 cc1 bring-up issues
- `eaf1a830` Add amd64 compiler pipeline tracing
