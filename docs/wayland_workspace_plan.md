# Wayland in Workspace: Tier 1 + Tier 2 Plan

Goal: arbitrary Wayland window managers and apps running inside a Workspace applet.
Architecture: guest-side headless wlroots compositor + wayvnc, displayed by an
in-app noVNC client hosted in a WKWebView applet, connected over localhost.

Tier definitions (from the feasibility discussion):
- Tier 1: the Wayland stack runs headless in the guest, viewable via any VNC client.
  Zero (or near-zero) app changes; emulator conformance fixes as they surface.
- Tier 2: a Workspace "Display" applet embeds the VNC client so the whole thing
  lives in the app. No compositor is written; we stay a dumb pixel pipe.

Why this is feasible now: wl_shm needs memfd + MAP_SHARED mmap (fixed:
tmpfs/memfd host-file backing, eb6493cc + memfd fix), fd passing over Unix
sockets via SCM_RIGHTS (present in fs/sock.c), epoll/timerfd/signalfd
(regression-tested), and pixman's SSE2 paths now run natively.

---

## Phase 0: Tier 1 on the CLI harness (the gate)

Everything else is scheduled behind this. Run on `build/ish` with the
`devuan-x86_64-cur` fakefs (boots clean, verified 2026-07-10).

0.1 Package availability probe (IN PROGRESS, background apt-get update).
    Want: cage, wayvnc, foot, labwc from Devuan 6/excalibur. All exist in
    Debian 12+ so expectation is yes for amd64.

0.2 Install minimal stack in the fakefs:
    apt-get install -y cage wayvnc foot   (labwc later; cage is the minimal
    kiosk compositor and matches the v1 applet UX of one fullscreen app).

0.3 Headless launch attempt:
      export XDG_RUNTIME_DIR=/tmp/xdg && mkdir -p $XDG_RUNTIME_DIR && chmod 700 $XDG_RUNTIME_DIR
      WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 cage -- foot &
      wayvnc 0.0.0.0 5900
    Notes: headless backend needs no DRM/udev/seatd, which is exactly what iSH
    can offer. Guest sockets are host passthrough, so wayvnc's listener is
    reachable at localhost:5900 on the Mac.

0.4 Validate from the host: connect with a VNC client (macOS Screen Sharing or
    vncdotool for scripted screenshots). Success = foot's prompt renders and
    keystrokes echo. Scripted capture gives an A/B artifact for later perf work.

0.5 Bug-hunt loop (the expected schedule owner): every failure gets the
    iSH-log-driven treatment (`meson configure -Dlog=strace`, compare against
    mint's Lima VM as oracle). Likely suspects, in rough order:
    - memfd/shm MAP_SHARED coherence between UNRELATED processes (fd passed via
      SCM_RIGHTS then mmap'd both sides). Fork-based coherence is tested;
      pass-an-fd-then-mmap is not. If broken, fix in fs/tmpfs or memfd backing.
    - XKB keymap fd: compositor sends a keymap fd the client mmaps MAP_PRIVATE.
    - epoll edge-trigger corner cases in wlroots' event loop.
    - clock_nanosleep/timerfd precision for frame timing.
    - inotify (wayvnc/compositor config watching): may need a graceful stub or
      real support depending on what breaks. Per project policy, implement
      rather than stub unless genuinely harmless.
    Each fix lands with a focused guest-side regression test in tests/manual/
    where practical (pattern: fcntl_ofd.c, tmpfs_mmap.c).

0.6 Deliverables: a reproducible bring-up script checked into tests/manual or
    /AOK/docs (setup-wayland.sh: installs packages, launches the stack), plus
    whatever emulator fixes landed. Perf note: record wall-clock for foot
    startup and rough interactive feel at 1024x768.

Exit criterion: foot (or busybox sh in foot) usable over VNC from the Mac,
sustained for minutes without wedge, on the CLI harness.

## Phase 1: Tier 1 on device (M4 iPad, amd64 Devuan)

1.1 Copy/install the same stack into the device's Devuan amd64 root (ssh rig).
1.2 Run the bring-up script on device; connect a VNC client from the Mac to the
    iPad's IP (the app can already hold listening sockets; ssh on 1022 proves it).
1.3 Measure: frame latency and CPU while typing in foot, and while running a
    curses app (htop). This sets expectations for applet UX and decides the
    wayvnc encoding choice (raw vs tight/zrle; over localhost raw is likely
    cheapest since encode cost dominates bandwidth).
1.4 Optional breadth check: labwc + two clients, verify window management works
    (the "arbitrary WMs" claim), XWayland explicitly out of scope for v1.

Exit criterion: same as phase 0 but on device, with a written perf note.

## Phase 2: Tier 2 applet (app work, can start once phase 0 exits)

2.1 Display client decision: noVNC (JS, in WKWebView) over native RFB.
    Rationale: WKWebView applets are an established pattern in this app
    (hterm terminal, MotePad), noVNC is mature and handles RFB decode, input
    mapping, and canvas scaling. Native RFB decoding is a rewrite we don't need
    for v1. License: noVNC is MPL-2.0; vendor it with attribution alongside the
    existing third-party JS (hterm), file-level MPL obligations are compatible
    with bundling.

2.2 WebSocket bridge (the one real piece of new native code):
    noVNC speaks WebSocket; wayvnc speaks raw RFB over TCP. Bridge in the app:
    a small Network.framework NWListener with the WebSocket protocol option on
    an ephemeral localhost port, proxying byte streams to localhost:5900.
    ~150-250 lines of ObjC. Alternative rejected: websockify in the guest
    (drags in a Python runtime, slower, more moving parts).

2.3 New Workspace applet ("Display", working name):
    - WKWebView loading the vendored noVNC bundle from app resources,
      configured to connect to ws://127.0.0.1:<bridge-port>.
    - Launch flow: applet start triggers a guest session running
      /AOK/tools/start-wayland.sh (packaged via the existing /AOK docs/tools
      generation), which brings up cage|labwc + wayvnc and exits with a clear
      error to the applet if packages are missing, pointing at the setup script.
    - Lifecycle: applet close tears down the guest session (SIGTERM to the
      session's pgroup, same pattern as existing applet-owned sessions);
      reconnect button for when the compositor is left running.
    - ATS: ws:// to 127.0.0.1 requires NSAllowsLocalNetworking (check the
      existing Info.plist; hterm may have needed it already).

2.4 Input and keyboard:
    - Touch: noVNC's default touch-to-mouse mapping to start; two-finger scroll
      and long-press-right-click come free.
    - Hardware/software keyboard: reuse the key-capture approach from the
      terminal applet (first-responder handling is a known landmine; see the
      workspace wedge notes, keep the scoped-first-responder-restore behavior).

2.5 Resolution: v1 launches the headless output at a fixed size passed to the
    bring-up script (default matched to the applet view's pixel size at launch,
    rounded); noVNC scales to fit. Live resize via wlr-output-management is a
    v2 item, noted but not built.

2.6 Settings UI: minimal, native UIKit (per standing preference): choice of
    compositor command (cage <app> vs labwc), output size, and a "keep running
    in background" toggle if jetsam behavior allows.

2.7 Packaging: the guest bits are packages the user installs (setup script), we
    do not bundle a compositor in the rootfs for v1. Ship
    /AOK/tools/setup-wayland.sh (apt/apk aware: Devuan first, Alpine follow-up).

Exit criterion: launch applet -> guest stack starts -> foot visible and
interactive inside Workspace on the M4 iPad, survives applet close/reopen and
app background/foreground.

## Phase 3: Device validation, polish, ship

3.1 On-device test cycle (Xcode install + eyeballing needs the user's hands,
    same cadence as MotePad).
3.2 Perf pass informed by phase 1 numbers: wayvnc encoding choice, noVNC
    render settings, and whether the JIT sweep already in flight helps compositing
    (pixman blit-heavy).
3.3 Regression coverage: guest-side wayland_smoke script in tests/manual
    (headless compositor + wayvnc handshake + one shm buffer round-trip),
    runnable in the local fakefs harness.
3.4 Docs + release notes entry (release process per reference: N+1 notes file,
    version bumps in 4 configs).

## Explicitly out of scope (v1)

- XWayland (doubles the syscall surface; revisit after v1 sticks).
- GPU/dmabuf anything (no path; clients must fall back to wl_shm, most do).
- Tier 3 (shared-memory zero-copy display device) and tier 4 (host-side
  compositor): the applet is deliberately a dumb pixel pipe so these can
  replace the transport later without changing the applet UX.
- Audio, clipboard sync, multi-output.

## Risk register

| Risk | Likelihood | Mitigation |
|------|-----------|------------|
| shm coherence across unrelated processes broken | medium | phase 0.5 first suspect; fix in memfd/tmpfs backing, add regression test |
| wlroots event loop trips epoll/timerfd gap | medium | strace-driven fix, mint oracle |
| Unusable perf on device | low-medium | phase 1 measures before applet work; raw encoding; smaller default output |
| WKWebView WS-to-localhost blocked (ATS) | low | NSAllowsLocalNetworking; native bridge already on localhost |
| First-responder/keyboard wedge in applet | medium | known bug class, reuse terminal applet handling |
| noVNC MPL-2.0 vendoring | low | attribution + unmodified files, matches hterm precedent |

## Sequencing and estimate

Phase 0 is serial and owns the risk (best case 1 session, expected 2-4 with
fixes). Phase 1 is half a session of rig time. Phase 2 is 1-2 sessions of app
code and can overlap the tail of phase 1. Phase 3 is gated on the user's device
cycle. Expected total: about a week of working sessions to a demoable applet,
2-3 sessions in the clean-run best case.
