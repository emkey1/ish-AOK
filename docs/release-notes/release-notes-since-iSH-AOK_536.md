# Release Notes Since `builds/iSH-AOK_536`

These notes summarize changes from `builds/iSH-AOK_536` intended for `builds/iSH-AOK_537`.

This is a focused bug-fix release. The headline is that **rtorrent / BitTorrent now works** — two independent bugs that each broke torrent clients (an arm64 floating-point instruction gap and a macOS/iOS `epoll` backend bug) are both fixed — alongside a correctness pass over the `POPCNT`/`TZCNT`/`LZCNT` bit-manipulation instructions across all engines, a crash guard, and a couple of app conveniences.

## Highlights

- **rtorrent / libtorrent works now.** Two separate bugs each killed it: on arm64, its MSE/Diffie-Hellman crypto path `SIGILL`'d on a scalar fixed-point float conversion; and on every host, a `kqueue`-backed `epoll` bug reported a spurious error on healthy sockets, closing BitTorrent peer connections within about a second. With both fixed, a peer connection that used to drop in under a second now stays open 50+ seconds and the download progresses.
- **`POPCNT`/`TZCNT`/`LZCNT` corrected across engines.** i386 `POPCNT` was completely unimplemented (`SIGILL` on any binary built with `-mpopcnt` or for a Nehalem+ baseline); `TZCNT`/`LZCNT` were wrongly treated as plain `BSF`/`BSR`, which give the wrong result and flags on a zero input. Fixed on i386, and the same `LZCNT` bug was fixed in the amd64 JIT bridge path too.
- **Crash fix:** a NULL-pointer guard in the memory refcount path.
- **App:** a persistent **Custom DNS Servers** override, **Send Feedback** now opens a GitHub issue (the old `mailto:` link failed silently without a configured Mail account), and a fixed crash in the About screen's LLM settings section.

## User-Facing Changes

### CPU Emulation

- **i386:** implemented `POPCNT` (`F3 0F B8`), and fixed `TZCNT`/`LZCNT` (`F3 0F BC/BD`) — they had been decoded only as aliases of `BSF`/`BSR`, which is wrong on a zero source operand (real `TZCNT`/`LZCNT` return the operand width and set `CF`, whereas `BSF`/`BSR` leave the destination undefined). amd64 already had `POPCNT`; this brings i386 to parity.
- **amd64:** fixed the same `LZCNT`-as-`BSR` bug in the JIT's `0F BC/BD` bridge helper — a second copy of the handler, separate from the interpreter path already fixed — confirmed against the differential oracle.
- **arm64:** implemented the scalar fixed-point float conversions `SCVTF`/`UCVTF`/`FCVTZS`/`FCVTZU` (with an immediate `#fbits`), both the 32-bit (S) and 64-bit (D) forms. These baseline ARMv8.0 instructions were falling through to "undefined" and `SIGILL`'d rtorrent's crypto path; the fix is validated bit-for-bit against the same instructions run natively on arm64 hardware.

### Networking / epoll (macOS/iOS backend)

- Fixed two bugs in the `kqueue`-backed `epoll` emulation that were killing otherwise-healthy sockets:
  - A single fd registered across multiple `kqueue` filters (read/write/except) could produce two separate `epoll_event` entries (e.g. a real `EPOLLIN` plus a bogus `EPOLLERR`) instead of Linux's single merged entry; events are now coalesced per fd before delivery.
  - `EVFILT_EXCEPT` was unconditionally reported as an error, but Darwin fires it on ordinary healthy sockets (`SO_ERROR` reads back 0); `EPOLLERR` is now reported only when `getsockopt(SO_ERROR)` confirms a real error.

### Stability

- Guard `mem_ref_cnt_get` against a NULL entry (crash fix).

### App

- Added a persistent **Custom DNS Servers** override.
- **Send Feedback** now opens the GitHub new-issue page instead of a `mailto:` link (which failed silently for users without Mail configured).
- Fixed a crash in the About screen when rendering the LLM settings section's header/footer.

## Known Issues

- Carried over from 536: `signal_poll` can still occasionally race under heavy concurrent load (long-standing, only reproduces under deliberate concurrency), and the futex `SA_RESTART` lost-wake issue remains open (deferred, real-software-immune, device-only repro).
- AArch64 remains the newest of the three guest engines and is still being hardened; expect rougher edges than the established i386/amd64 engines for a few more releases.

## Maintainer Notes

- `CURRENT_PROJECT_VERSION` is already at 537 (bumped in `40aae5a5`; the four app configs, secondary target frozen at 529 per existing convention). `builds/iSH-AOK_536` is an annotated tag; `builds/iSH-AOK_537` should be tagged the same way (annotated, message `iSH-AOK build 537`).
- The `LZCNT` bug lived in **two** independent code paths — the interpreter's `0F BC/BD` switch and the amd64 JIT's `amd64_jit_0f_rm` bridge helper — and needed fixing in both (`d690df04` + `1836635a`). Worth auditing any instruction with both an interpreter and a JIT-bridge handler for the same divergence.
- Release validation at tag time (local CLI, Apple-silicon host): clean build of `ish`; the amd64 and arm64 roots both boot and run busybox. No full i386 root is present locally, so i386's `POPCNT`/`TZCNT`/`LZCNT` change should be confirmed on-device or via the `tests/remote` differential harness. The x87 `float80` and `e2e` host-test failures observed locally are pre-existing/environmental (the latter only because this machine has no `gcc` in `PATH`), not regressions.
- Minor: `kernel/calls.c` emits `-Winitializer-overrides` warnings because io_uring syscalls 425–427 are initialized both individually and as a `[425 ... 427]` range; harmless (same `syscall_stub_silent` handler) but worth de-duplicating.

## Commit Range
```
40aae5a5 app: point Send Feedback at GitHub issues; bump build to 537
614296d7 Merge branch 'working' into aarch64
4fe517fc arm64 JIT: scalar UCVTF/SCVTF/FCVTZS/FCVTZU fixed-point convert (rtorrent SIGILL)
fd66a357 fs/poll: fix spurious EPOLLERR and split events on kqueue backend
1836635a emu/amd64: fix LZCNT-as-BSR-index bug in amd64 JIT bridge path too
d690df04 emu/i386: implement POPCNT, fix TZCNT/LZCNT aliasing to BSF/BSR
0bf90a43 emu/memory: guard mem_ref_cnt_get against NULL
35826ccf app: add persistent Custom DNS Servers override
1564cd8d app: guard viewForHeaderInSection/viewForFooterInSection for LLM section
```
