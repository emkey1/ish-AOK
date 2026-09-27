# Release Notes Since `builds/iSH-AOK_535`

These notes summarize changes from `builds/iSH-AOK_535` intended for `builds/iSH-AOK_536`.

This is a **follow-on release** to 535's arm64 guest launch: it closes out the arm64-specific issues found in first real usage (mount(8), login/keyctl, a bundled Devuan root, a slow-password-hash footgun) and adds a Launcher shortcut-grouping feature to Workspace, plus arm64 `ptrace` register support.

## Highlights

- **New: Devuan 6 (excalibur) arm64 root**, completing arm64 parity with the existing bundled Devuan i386/amd64 roots and the Alpine arm64 root shipped in 535.
- **Fixed: `mount(8)` on arm64.** util-linux's new-mount-API probe (`open_tree`/`move_mount`/`fsopen`/`fsconfig`/`fsmount`/`fspick`) was missing from arm64's syscall table, spamming `ERROR: ... arm64 missing syscall 428` and blocking `mount --bind`/plain `mount` on the newly-bundled Devuan arm64 root.
- **Fixed: `login` on arm64.** `pam_keyinit`'s `keyctl(KEYCTL_JOIN_SESSION_KEYRING, ...)` call was a loud `ENOSYS` stub instead of the real implementation i386/amd64 already have, logging an error on every login.
- **Fixed: slow password-hash on Devuan roots.** Devuan/Debian defaults new password hashes to `yescrypt`, deliberately memory-hard and close to worst-case for a JIT-emulated guest — measured ~1.15s of extra latency per password login on fast Apple Silicon (root-caused from an "ssh-copy-id never finishes" report on an older A10X iPad, where it plausibly stretches into minutes). All three Devuan images (i386/amd64/arm64) now default to `sha512crypt` instead.
- **New: Launcher shortcuts can be grouped.** Workspace's Launcher applet now supports nested groups/folders of shortcuts (e.g. a "Remote Login" group holding several host launchers), with drag-and-drop reorder/nest/un-nest and matching drill-down in the dock's quick-launch popup.
- **New: arm64 `ptrace` register support.** `strace -f` and other ptrace-based tracers now read/write correct arm64 GPR/SP/PC/PSTATE and V-register state instead of a wrong-architecture layout.

## User-Facing Changes

### AArch64 Guest Architecture

- Bundled a **Devuan 6 (excalibur) arm64** minirootfs, matching the shape of the existing Devuan i386/amd64 entries; visible as a normal root-import choice. `tools/build-devuan-minirootfs.sh` gained arm64 as a supported build target (runs natively on an arm64 build host, no qemu binfmt needed).
- Fixed `mount(8)` failing on the new Devuan arm64 root: the six new-mount-API syscalls (428-433) are now silent-`ENOSYS` stubs like i386/amd64 already have, so util-linux falls back to legacy `mount(2)` cleanly. (The straightforward fix — just populating the table — reintroduced a worse bug first: a non-NULL table entry routes through default 6-arg marshalling, which validates every argument fits a 32-bit dword; `open_tree`'s ordinary 64-bit filename pointer failed that check and turned a silent `ENOSYS` into a `SIGSYS` kill. Classifying these six as 0-arg, since the stubs never read any argument, fixed it for real.)
- Fixed `login` failing (`keyctl` `ENOSYS`): arm64's syscall table now wires in the real `sys_keyctl` (matching i386/amd64), with the same 64-bit-pointer-vs-32-bit-dword-check pitfall as the mount fix applied to its own argument classification.
- Fixed the "missing/stub syscall" diagnostic log printing meaningless i386-shaped register fields (`eip`/`eax`/`ebx`/`ecx`/`edx`) for arm64 tasks; arm64 now gets its own branch printing `pc`/`x0`-`x3`. (Drive-by: the amd64 branch was also mislabeling `cpu->eip` as `rip=`; now correctly `cpu->amd64_rip`.)
- Added a head-to-head i386/amd64/arm64 performance benchmark (`guest_architecture_benchmarks.md`): arm64 is 9-11x faster than the x86 guests on a compute-bound loop on this arm64 build host (same-ISA dispatch plus 535's loop-chaining work), while thread lifecycle cost is flat across all three architectures (kernel-dominated, not guest-specific).
- **New:** arm64 `ptrace` `GETREGSET`/`SETREGSET` now correctly reports and accepts `NT_PRSTATUS` (GPRs/SP/PC/PSTATE), `NT_PRFPREG` (V-registers/FPSR/FPCR), and `NT_ARM_SYSTEM_CALL` for arm64 tracees, instead of a wrong-architecture register layout. Lets `strace -f` and similar tooling correctly follow forked/cloned arm64 guest processes.

### Password Hashing

- All three bundled Devuan images (i386/amd64/arm64) now default new password hashes to `sha512crypt` instead of `yescrypt`. `sha512crypt` is thousands of rounds of pure ALU work with no deliberate memory-hardness, so it doesn't pay the emulator's address-translation tax the way `yescrypt`'s memory-hard scratch-buffer accesses do. A `SLOW_HASH=1` build flag keeps `yescrypt` available for anyone who wants the stronger (but much slower under emulation) default. Alpine already defaulted to `sha512crypt` and needed no change.

### Workspace — Launcher Shortcut Groups (new)

- Launcher shortcuts can now hold `children` recursively — a shortcut like "Remote Login" can group multiple host launchers instead of flattening everything into one list.
- The Launcher applet gained a native table view with drag-and-drop reordering, drag-to-nest (drop a shortcut onto a group to move it inside), and drag-to-un-nest (drop onto the "‹ Back" row to pop it out to the parent level).
- Edit/Done mode toggle and long-press context menus for rename/delete/add-shortcut/add-group.
- Breadcrumb-style "‹ Back" navigation for drilling into groups, mirrored in the dock's quick-launch popup.
- New built-in "Session Shell" and "System Console" shortcut types, alongside the existing tool-token shortcuts.

## Known Issues

- Same open items as 535: `signal_poll` can still occasionally race under heavy concurrent load (long-standing, reproduces only under deliberate concurrency); the futex `SA_RESTART` lost-wake issue remains open (deferred, real-software-immune, device-only repro).
- AArch64 remains new and under active hardening — this release closes out issues found in first real usage, but expect rougher edges than the established i386/amd64 engines for a few more releases.

## Maintainer Notes

- The `open_tree`/`keyctl` arg-count classification bug (64-bit pointer argument failing the default 6-arg legacy marshalling's 32-bit dword-fit check) is now a two-for-two pattern on arm64 — worth checking any future silent-`ENOSYS`-stub addition against it up front rather than rediscovering it per-syscall.
- Both the Launcher-groups and arm64-ptrace changes were reviewed in this session (diff read + correctness check) before committing; no blocking issues found. The float80 x87 test failures observed locally are pre-existing/known (tracked separately, unrelated files) — not a regression from this release's changes. The e2e test suite failed locally only because this machine has no `gcc` in `PATH`; that's an environment gap, not a code issue.
- Version bumped to 536 in `iSH-AOK.xcodeproj/project.pbxproj` (4 app configs; secondary target frozen at 529 per existing convention).

## Commit Range

```
aa66b629 build: bump project version to 536
b4152478 arm64 guest: real ptrace GETREGSET/SETREGSET support
314e1b81 workspace: nested groups for Launcher shortcuts
b21f3c8f devuan roots: default to sha512crypt, not yescrypt (all archs)
e799cac8 arm64 guest: wire up real keyctl instead of ENOSYS stub
aab65172 arm64 guest: new mount-API syscall stubs; fix garbage register dump
cff3f30a docs: add guest architecture performance benchmarks
0bd76699 app: bundle a Devuan 6 (excalibur) arm64 minirootfs
```
