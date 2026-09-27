# Release Notes Since `builds/iSH-AOK_532`

These notes summarize changes from `builds/iSH-AOK_532` intended for `builds/iSH-AOK_533`.

This is a focused release (89 commits). The headline is a **Workspace UI redesign** — a flat, ctwm-style "Modern" window manager with in-app virtual **Desktops** and a customizable **Launcher**, now the default — alongside the **64-bit (amd64) guest shedding its "experimental" framing**, all four guest roots bundled (and the on-device xz import fixed), the **MMX instruction set essentially completed** across both engines, and a handful of real crash/`SIGILL` fixes that unblock `mosh`, `chronyd`, `gpgv`, and glibc/Python 3.13 atomics.

## Highlights

- **Workspace redesign — "Modern" (ctwm-style) is now the default.** A flat, window-manager-style Workspace replaces the dock with a long-press / floating-pip **root menu**, adds in-app virtual **Desktops** (multiple desktops on a single app window — works on iPhone, no iOS multi-window required), a customizable **Launcher** of shortcuts (shell commands or built-in tools), and an **Icon Manager** window switcher. The keyboard now follows the focused window. Classic remains available in Appearance settings.
- **64-bit (amd64) guest no longer flagged "experimental."** The rootfs picker imports amd64 roots directly — no warning prompt — matching i386. (The separate engine-selection "Enable Experimental amd64 JIT" toggle is unchanged.)
- **All four guest roots are bundled, smaller, and import on device.** Alpine and Devuan, x86 and x86_64, all shipped as `xz`; the stale provisioned Alpine x86 image is replaced with a clean stock 3.23.3 minirootfs. A new minimal **Devuan 6 "Excalibur" minirootfs** (i386 + amd64) and provisioner are added. The on-device "filesystem archive could not be read" `xz`-import bug is fixed.
- **x86 instruction coverage filled out (both engines).** The MMX integer instruction set is essentially completed (unpack/pack, saturating add/sub, min/max, pavg, pmulhuw, pmaddwd, psadbw, pandn, MMX↔XMM transfers, emms), fixing `gpgv`/libgcrypt `SIGILL`s — plus amd64 `cvtsd2si`/`cvtss2si` (mosh) and 66-prefixed `movlpd`/`movhpd` stores (chronyd).
- **Two real crash fixes:** a guest-triggerable `clone()` use-after-free, and an unaligned i386 `lock cmpxchg8b` that `SIGSEGV`'d glibc / Python 3.13 64-bit atomics.
- **Kernel correctness for modern-distro boot.** `mount(2)` now handles bind mounts, `MS_MOVE`, and propagation/no-op flags (systemd); `clock_adjtime` lets `chronyd`/`ntpd` run monitor-only; the i386 table gained the new mount-API stubs — together silencing a stream of boot-console "missing syscall" spam.
- **Terminal/keyboard refinements:** opt-in "Maximize Screen Space," more accessory-bar keys (`-` `:` `!` `|`), an iPad soft-keyboard accessory-bar fix, and more compact default terminal sizes.
- **CLI now defaults to 4 emulated CPUs** so local/fakefs runs reproduce device multicore races; new `ISH_REAL_MNT` host-directory mount.

## User-Facing Changes

### Workspace — Modern Style, Desktops, and Launcher

**Modern style.** A new "Workspace Style" preference (Classic | Modern). Modern is a flat, ctwm-inspired skin — hairline window borders, a solid accent-washed focused title bar, flat controls, and a light/dark-aware flat desktop background — that applies live (from Settings, the menu, or guest `defaults`). **Modern is now the default** for new/unset installs; anyone who explicitly chose Classic keeps it. Focusing a window (tap to raise) now makes that window's terminal first responder, so the keyboard follows the active pane — a fix that also benefits Classic.

**Root menu.** In Modern the dock is hidden; a root menu replaces it, reachable by long-pressing the desktop, a floating menu pip (bottom-right), a two-finger long-press anywhere (even over a window), or each window's title-bar menu button. It routes the dock's full Terminal and Utilities menus plus New Terminal (a fresh shell each time), New System Console, Settings, New Workspace/Desktop, the Launcher, and the Icon Manager window switcher.

**In-app Desktops (virtual desktops).** Multiple virtual desktops within a single app window, switched by a two-finger horizontal swipe (with a brief "Desktop N / M" toast). Works on **every device, including iPhone** — no iOS multi-window / Split View required — and terminals keep running while their Desktop is hidden. A **Desktops applet** manages them: one row per Desktop (active highlighted, tap to jump), add/remove, and a per-Desktop lock (the first Desktop is permanently protected). "Desktops at Launch" (1–4) seeds how many open. Save → Restore is Desktop-aware: a multi-Desktop arrangement of tool windows and structure survives, and the Desktops/Launcher applets are global singletons pinned to the first Desktop.

**Launcher.** A new Launcher of user-defined shortcuts — each a name plus either a shell command (runs in a fresh terminal), an empty command (just opens a shell), or a `{token}` that opens a built-in tool (e.g. `{clock}`, `{settings}`, `{monitor}`, `{networks}`, `{llm}`). Available both as a root-menu sheet and as a persistent on-screen applet that doubles as the editor ("Edit Shortcuts," "Add Built-in…"). Edits live-sync across every open view; shortcuts and window placement persist in defaults and restore at their saved position.

**Sizing and polish.** The Desktops/Workspaces, Launcher, and Monitor applets now size themselves to their content or active style (e.g. Monitor sizes to ring vs bar gauges so the Battery/Storage row is no longer clipped). Numerous phone-specific placement/persistence fixes keep global applets on-screen across backgrounding and foregrounding instead of drifting off-screen.

### 64-bit (amd64) Guest

- Dropped the "experimental" framing from the amd64 rootfs UI: the picker subtitle, the import-confirmation prompt, the bundled-section footer, and the `ENOEXEC` boot-failure message. amd64 roots now import directly like i386, with the same actionable recovery message on failure. (The separate Settings "Enable Experimental amd64 JIT" engine toggle is left as-is.)
- amd64 `SIGILL` fixes that unblock real software — see CPU Emulation: `mosh` (`cvtsd2si`/`cvtss2si`), `chronyd` (`movlpd`/`movhpd`, `clock_adjtime`), `gpgv`/libgcrypt (MMX).

### Guest Roots and Images

- All four bundled roots (Alpine x86/x86_64, Devuan x86/x86_64) ship as `xz` and appear in the first-run picker; the bundled-root resolver now accepts any libarchive-supported extension instead of hardcoding `.tar.gz`. Net bundle is ~67 MB of rootfs / ~76 MB app, well under the 200 MB over-cellular download threshold.
- The bundled Alpine x86 image was a booted/provisioned 9.9 MB capture (apk index cache, OpenRC, strace, htop); it's replaced with the clean stock **Alpine 3.23.3** x86 minirootfs (SHA256-verified), and both Alpine images recompressed gz → xz.
- New minimal **Devuan 6 "Excalibur" minirootfs** (i386 + amd64), built reproducibly via `mmdebstrap --variant=minbase`, plus `provision-ultimate-devuan.sh` — the apt + sysvinit twin of the Alpine provisioner — now exposed at `/AOK/tools`.
- The Devuan minirootfs now bundles **busybox-static** (~2 MB) with the gap-applet tools symlinked into `/usr/bin` (`ps`, `top`, `free`, `watch`, `wget`, `less`, `killall`, …), so a bare Devuan root has working interactive/monitoring tools offline — before the provisioner runs `apt` — at parity with the busybox-based Alpine roots. Under usr-merge `dpkg` replaces each symlink with the real binary when its package installs, so busybox is a self-healing fallback rather than a shadow.
- **`xz` roots now import on device.** The iOS app's libarchive was built with `HAVE_LIBLZMA` but without `HAVE_LZMA_H`, so its `xz` reader compiled as a stub and every `.tar.xz` root failed with "The filesystem archive could not be read." Fixed by enabling the real `xz` reader (vendored public headers) and linking a **vendored, statically-built liblzma 5.8.3** (`deps/liblzma-static/`) into the app and FileProvider extension. (CLI tools link the macOS system libarchive, which has `xz`, so local `fakefsify`/`ish` tests passed and hid the original stub. The first on-device attempt linked the iOS *system* `liblzma.tbd`, which works in the simulator but is a private library — App Store validation rejects references to its non-public `lzma_*` symbols — so it was replaced with the static build, whose symbols are defined in-binary.)
- The cached-roots Settings header now shows where those archives live (`/AOK/persist/roots`).

### CPU Emulation (both engines)

- **MMX completion.** Implemented the remaining common 2-byte MMX integer ops on i386 and amd64 — `punpck{l,h}{bw,wd}`/`punpckhdq`, `packsswb`/`packuswb`/`packssdw`, saturating `padd`/`psub` (signed & unsigned, byte & word), `pminub`/`pmaxub`/`pminsw`/`pmaxsw`, `pavgb`/`pavgw`, `pmulhuw`, `pmaddwd`, `psadbw` — plus `pandn` (0F DF), the MMX store + `emms` gaps (0F 7F / 0F 7E / 0F 77), and the `movq2dq`/`movdq2q` MMX↔XMM transfers. These had `SIGILL`'d MMX code (e.g. `gpgv`/libgcrypt SHA). amd64 `pmaddwd` is handled via the `#UD`-handler SSE2/MMX emulator to sidestep a JIT block-chaining bug; `maskmovq` is intentionally omitted (a masked store essentially never emitted).
- **amd64 scalar/SSE.** `cvtsd2si`/`cvtss2si` (0F 2D — round-to-nearest-even, out-of-range/NaN → integer-indefinite), fixing a `mosh-client` `SIGILL`. The 66 prefix is now accepted on `movlpd`/`movhpd` (0F 13 / 0F 17) m64 stores (they store the same low/high qword as their `movlps`/`movhps` siblings), fixing a `chronyd` `SIGILL`.
- **i386 unaligned `lock cmpxchg8b`.** The aarch64 exclusive-load gadget requires 8-byte alignment, but x86 permits unaligned `LOCK` operands and the i386 ABI 4-aligns 64-bit fields, so glibc / Python 3.13 atomics routinely landed on a 4-aligned address and `SIGSEGV`'d. It now falls back to a software-atomic helper under the global atomic lock (cross-page / `#PF`-safe, only `ZF` affected). The aligned fast path is unchanged; amd64 `cmpxchg16b` keeps its mandatory 16-byte alignment.
- **Build fix.** Added the missing x86_64-host `lahf` JIT gadget so the x86_64 emulator slice links (arm64 device/sim builds were unaffected, which hid the gap).

### Kernel and Syscalls

- **`clone()` use-after-free fixed.** A non-`CLONE_THREAD` `clone()` that failed *after* linking the new thread group into the caller's session/pgroup lists (a bad `CLONE_SETTLS` or `CLONE_*_SETTID` pointer) freed the group without unlinking it — leaving a freed node in the live, pid-rooted lists, so the next traversal (process-group signal delivery, session-exit, `kill_pg`) corrupted freed memory. Guest-triggerable. The error path now unlinks the group like the normal exit path does. New `tests/manual/clone_error_cleanup.c` regression (the buggy build hangs on the corrupted lists; the fixed build survives 512 error-path clones).
- **`mount(2)` rebuilt into Linux-style operation dispatch:** bind mounts (`MS_BIND`, sharing the source mount's backing — works for realfs and fakefs, and is transitive), `MS_MOVE`, propagation-only changes (`MS_PRIVATE`/`SHARED`/`SLAVE`/`UNBINDABLE`) accepted as no-ops, and durability/atime/symlink option flags (`MS_REC`/`MS_SYNCHRONOUS`/…) stripped rather than rejecting the whole mount. Fixes `mount --bind` and the propagation changes systemd issues at boot; genuinely-unknown flags still return `EINVAL`. New `tests/manual/mount_flags.c` regression.
- **`clock_adjtime` (i386 343/405, amd64 305).** Implemented the read-only state query: `modes=0` returns a sane undisciplined state (default tick, `STA_UNSYNC`, `TIME_ERROR`) — bit-exact vs real Linux for a clock with no NTP discipline — so `chronyd`/`ntpd` start in monitor-only mode. Any actual adjustment (`modes != 0`) stays `EPERM` (iSH can't slew the iOS clock).
- **New mount-API stubs on i386** (`open_tree`/`move_mount`/`fsopen`/`fsconfig`/`fsmount`/`fspick`, 428–433) as silent `ENOSYS`, matching amd64, so util-linux falls back to `mount(2)`. With `clock_adjtime`, this silences a stream of boot-console "missing syscall" lines.
- Resolved two stale/false-alarm refcount FIXMEs (`fs/proc`, `fs/fd`) after a Tier-2 audit — comments only, no behavior change. Enriched the unimplemented-`clone`-flags, unsupported-`futex`, `pipe2`, and unknown-mount-flag FIXMEs to decode the feature and name the requesting program (logging only), to drive the "harvest device logs → decide what to implement" strategy.

### Terminal and Keyboard

- New opt-in **"Maximize Screen Space"** (External Keyboard settings): reclaims the home-indicator strip on notched iPhones when used with an external keyboard plus "Hide with external keyboard" — a tightened reimplementation of upstream `ish-app/ish#2754` without its storyboard/whitespace churn.
- Accessory bar: added `-` `:` `!` `|` keys (shown in landscape and on iPad; hidden on iPhone portrait for space), and the whole center cluster is now centered as a unit rather than pinned by the `.` / `/` gap.
- Fixed the accessory bar vanishing on iPad portrait with the on-screen keyboard (a short soft-keyboard frame was being mistaken for a hardware keyboard; now gated on an authoritative `GameController` check).
- New default-on preference to show/hide the on-terminal settings gear and switcher buttons. New phone terminal windows fill the usable desktop or open at compact, orientation-scaled default sizes, and the resizable minimum is roughly halved.
- Fixed a dead strip at the bottom of workspace **floating terminals** on iPhone (e.g. the Session Shell) that the cursor couldn't reach: the floating window is already placed clear of the keyboard, but the keyboard-avoidance inset was applied anyway (it didn't honor `embeddedInWorkspaceWindow`, and the narrower-than-screen window tripped the "undocked keyboard" fallback).

## Known Issues

- **`futex_core` "signal restart" is flaky on-device under load** (carried over from 531; ~75–85% on a busy device, both i386 and amd64; ~0% on a quiet device, not reproducible on the macOS CLI). A `FUTEX_WAKE` that races an `SA_RESTART` signal can be lost while the waiter is mid-restart, so the restarted wait runs to its timeout. Still **deferred**: it is real-software-immune (every libc synchronization primitive changes the futex word before waking, degrading a lost wake to a harmless re-check; real Linux passes because its syscall restart is microseconds).
- The 64-bit (amd64) guest, while no longer labeled experimental in the UI, is still newer than the i386 guest and remains the less-seasoned of the two.

## Maintainer Notes

- `builds/iSH-AOK_531` points to `a3fdfb6b`; `builds/iSH-AOK_532` points to `17bd4267`. `builds/iSH-AOK_533` should point to this release-notes commit (matching the established `docs: add iSH-AOK <N> release notes` pattern).
- `CURRENT_PROJECT_VERSION` bumped 532 → 533 in the four app configs (commit `f8189355`); the secondary target remains at 529, as in 531/532. `MARKETING_VERSION` is unchanged (`1.3`).
- Feature branches merged into `working` this cycle: `workspace-modern`, `maximize-screen-space`, `fork-clone-fix`, `mount-flags`, `clone-instrument`, `fd-proc-cleanup`.
- The emulator-level fixes (MMX completion, `cvtsd2si`/`cvtss2si`, `movlpd`/`movhpd`, unaligned `cmpxchg8b`, `clock_adjtime`, `mount`, the `clone()` UAF) were validated on an arm64 host bit-exact vs the Rosetta x86 and real-Intel "mint" Linux oracles per their commit messages; they need this app rebuild to reach the device. New guest self-checks: `mount_flags.c`, `clone_error_cleanup.c`.
- Most Workspace / iOS-app UI changes can't be compiled from the CLI here; they were static-verified and device-tested by the maintainer. The `[LAUNCHER-DIAG]` breadcrumbs added in `bf5f35b0` were removed in `91f214e9` once the Launcher foreground-displacement fix (`3689f3ce`) was confirmed on device — no diagnostic logging remains.
- **App Store packaging — vendored liblzma.** iOS's `liblzma` is a *private* library, so linking the system `liblzma.tbd` (the original on-device xz fix, `aaf5036b`) makes the App Store reject the upload: "references non-public symbols … `_lzma_code`, `_lzma_crc32`, `_lzma_alone_decoder`, `_lzma_raw_decoder`, `_lzma_properties_decode`, `_lzma_stream_decoder`, `_lzma_end`." Replaced with a vendored, statically-linked **liblzma 5.8.3** built from source under `deps/liblzma-static/` (decode-only, single-threaded; taken from the xz git tag `v5.8.3` — not the release tarball — to avoid the CVE-2024-3094 build artifacts). The `lzma_*` symbols are now defined in-binary. The static slices were cross-compiled here with the iOS SDK and symbol-verified (`nm`: self-contained, all seven flagged symbols defined `T`) for `ios-arm64` + `ios-arm64_x86_64-simulator`, packaged as `liblzma.xcframework`; rebuild via `deps/liblzma-static/build-liblzma-apple.sh` (see that dir's README). `libbz2`/`libz` stay as system links — they are public/allowed iOS libraries, which is why only `lzma` was flagged.
- Pre-tag sanity checks at this HEAD (`088ff1cc`): version bump consistent across the four app configs; working tree clean apart from this untracked notes file and a `.swp` (vim swap, not part of the release); the emulator core compiles (`ninja -C build ish` → "no work to do"); and the amd64 (`alpine64`) fakefs boots and runs (`uname -m` → `x86_64`, CPU flags present).
- Recommended at tag time: run the on-device `/AOK/tests` guest regression suite on **both** roots (i386 + amd64), as in 531.

## Commit Range

```
088ff1cc terminal: don't reserve keyboard space in a workspace floating terminal
6c9e820a build/devuan: bundle busybox-static with gap-applet symlinks; rebuild images
f42dff14 workspace: compress the Desktops applet on iPhone
64333e74 workspace: size the Monitor applet to its active gauge style
f97c206d workspace: auto-open the Desktops applet on iPhone (drop iPad-only gate)
91f214e9 workspace: remove temporary [LAUNCHER-DIAG] logging
074ba5fa kernel/time: clock_adjtime read-state for chronyd monitor-only
bf5f35b0 workspace: temporary [LAUNCHER-DIAG] logging to trace foreground displacement
c194b80f emu/amd64: accept 66-prefix movlpd/movhpd (0F 13/0F 17) m64 store
3689f3ce workspace: give the Launcher dock-style frame persistence
a482646f workspace: re-assert Launcher placement on foreground (like Desktops applet)
744d1b33 workspace: keep phone Desktops applet + Launcher across foreground
6f802ae9 emu/amd64: implement pmaddwd (0F F5) via the #UD-handler emulator
54630c67 kernel: i386 clock_adjtime + new-mount-API syscalls (stop boot spam)
8c955bff workspace: first Desktop is permanently protected (always-closed lock)
ffaa70c3 workspace: stop re-pinning the phone Desktops applet off-screen
ccb2fa99 jit: fix unaligned i386 lock cmpxchg8b (was SIGSEGV)
0bcc54c4 workspace: fix Desktops applet not opening on iPhone
76f8f4ff emu: implement the remaining 2-byte MMX integer ops
b5a58156 workspace: hide the delete x entirely on locked Desktops
5062e3d0 workspace: per-Desktop lock toggle + smaller delete x in the applet
0891340b emu: implement MMX pandn (0F DF) on i386 + amd64
47d13f75 emu/amd64: implement missing MMX store + emms instruction gaps
ed1562fb workspace: make the Desktops applet available on iPhone
4592eadc app: show cached-roots location in the section header
05d10177 fs: expose provision-ultimate-devuan.sh at /AOK/tools
f3cea09f app: stop flagging x86_64/amd64 roots as experimental
11fa93dd workspace: double default terminal width; default to Modern style
b1e17464 workspace: smaller orientation-aware default terminal size; halve minimum
aaf5036b app: enable libarchive xz support on iOS (system liblzma + vendored headers)
18ef9fbf app: bundle all four roots as xz and replace the bloated Alpine x86 image
bd10debe Add minimal Devuan 6 "Excalibur" minirootfs (i386 + amd64) and provisioner
5cd8076e jit: add the missing x86_64-host lahf gadget
be6cac4f workspace: actively reap duplicate global-tool windows
b14aed1a workspace: pin global applets to Desktop 0; ignore saved copies beyond it
10bb418c workspace: make the Desktops & Launcher applets true global singletons
43a9e57d workspace: Phase 4 (part) - delete dead scene-spawn launch-count helpers
d0be5c95 workspace: Desktops applet is now a real Desktop manager
ff307a6a workspace: fix Launcher duplicating/moving across Desktops
c0d5854c workspace: Launcher global across Desktops; rename applet to Desktops
ecf77d50 workspace: keep the Workspaces applet visible on every Desktop
d46b9806 workspace: launch count applies everywhere, labeled Desktops
fa60e841 workspace: in-app Desktops phase 3 (desktop-aware Save/Restore)
49fb1bd2 workspace: in-app Desktops phase 1 (data model + two-finger swipe switch)
cca7d2d5 workspace: trim visible (Split View) workspaces when lowering the count
7fd0ccbd workspace: Settings count for workspaces opened at launch
c9488a6c workspace: center Launcher card text
5f94afda workspace: Launcher can open built-in tools via {token} shortcuts
7bfc7471 workspace: honor saved layout at launch; name-only Launcher cards
9e724eae workspace: smaller Launcher cards; New Workspace in root menu
ceb34b50 workspace: size the Workspaces applet to the scene count
979a4a53 workspace: Settings in root menu; tighter Launcher with larger item text
e92a2873 app: fill phone terminal windows; toggle terminal settings/switcher buttons
6e812359 workspace: refine Launcher applet (empty command, auto-size, edit flow)
953b8799 workspace: add Launcher as a persistent on-screen applet
59c4d222 workspace: Modern auto-closes phantom workspace windows at launch
584d8b67 workspace: Modern folds Layout Manager into the Workspaces applet
d27cdda8 workspace: add a customizable Launcher to the root menu
703cd408 workspace: Modern hides the dock and drops New Workspace / Close Hidden Windows
3895fc12 workspace: Icon Manager is a real window switcher, not the Sessions panel
60bb6633 workspace: full dock-replacement root menu + per-window menu button + visible pip
d7790ad3 workspace: always-reachable Modern root menu (menu pip + two-finger long-press)
05b76cd9 workspace: surface the session switcher as Icon Manager in the root menu
5bbb9725 app: center the accessory key cluster as a whole
c728a27e app: don't mistake the iPad soft keyboard for a hardware one
5071675e app: add "|" to the space-gated accessory keys
0d86d0ae workspace: desktop root menu New Terminal opens a fresh shell each time
8b092c1a app: add "!" to the space-gated accessory keys
3c58d582 Merge branch 'maximize-screen-space' into working
40b802bd app: add "-" and ":" accessory-bar keys (landscape / iPad only)
8b2712a4 workspace: ctwm-style desktop root menu (Modern, long-press)
f690ffe8 workspace: light/dark Modern cards + focus the active window's terminal
e69dca6b app: opt-in "Maximize Screen Space" terminal layout option
115bffc0 workspace: make Modern style apply live and read clearly
d4461aec Merge workspace-modern: opt-in Modern (ctwm-style) Workspace style
afd1eec1 workspace: add opt-in Modern (ctwm-style) Workspace style alongside Classic
f8189355 build: bump project version to 533
0e4528b0 cli: default to 4 emulated CPUs; add ISH_REAL_MNT realfs dev-mount
0f6808af jit/amd64: implement cvtsd2si/cvtss2si (0F 2D), fixing mosh SIGILL
11d8a190 Merge fork-clone-fix: clone() error-path session/pgroup UAF fix + regression
58848c9b Merge mount-flags: bind mounts, MS_MOVE, propagation/no-op flag handling
ad342013 Merge clone-instrument: enrich clone/futex/pipe harvest FIXMEs with comm/pid
d86635f4 Merge fd-proc-cleanup: resolve false-alarm fd/proc refcount FIXMEs
3bfad08f kernel/clone: unlink the new thread-group on clone() error paths (UAF fix)
88b1643e fs/fd, fs/proc: resolve two stale/false-alarm refcount FIXMEs
6129832c fs/mount: name the requesting program in the unknown-flag FIXME
153b8e2e kernel/futex, fs/pipe: name the requesting program in unsupported-op FIXMEs
acea5fb4 kernel/clone: enrich the unimplemented-clone-flags FIXME for log harvesting
47825465 fs/mount: implement bind mounts, MS_MOVE, and propagation/no-op flags
```
