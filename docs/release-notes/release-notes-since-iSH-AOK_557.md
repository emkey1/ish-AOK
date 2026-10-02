# Release Notes Since `builds/iSH-AOK_556`

250 commits. This build gives the guest a GPU. Vulkan programs, OpenGL through
zink and the Wayland desktop itself now draw on the iPad's graphics hardware,
and the desktop reaches the app without VNC. Around that: a desktop that
behaves with a hardware keyboard, a games installer, an LLM Chat that works on
files and runs agents, native tools that answer the way GNU's do, and a faster
JIT on every guest architecture.

## Before you update

**Saved sessions from 556 will not restore.** The suspend-to-disk format has
changed (checkpoint version 22 in 556, 23 now), and an image is tied to the
build that saved it in any case, so the guest boots fresh. The resume picker
now marks such an image "can't be restored". Finish anything that lives only
in a saved session before you update.

**`sh` can mean native dash beyond login shells.** `native-links.sh` now also
links into `/usr/local/bin`, which every distro's built-in `PATH` puts ahead
of `/bin`. So `ssh host cmd`, cron and init scripts get native `sh` (dash),
and package scripts get SmallCLUE's `sed`, `grep` and `awk`, now
GNU-compatible (see below). `#!/bin/sh` scripts are untouched. The Alpine,
Devuan and Arch provisioning scripts now run it for you. `--no-sh` keeps `sh`
out, `--remove` takes every link back, `NATIVE_LINKS=0` skips the step.

**Files in `/AOK/persist` and `/AOK/roots` show a new owner.** The host can
record only one owner there, so each file now shows as owned by whichever
guest user asks. Before, every user but root was "other", and uid 1000 could
not use a directory it had just made. The app no longer resets everything
there to 0666/0777 at launch, so a `chmod` stays (an ssh key can be 0600). A
`chown` to another user is `EPERM`, root included.

## GPU acceleration (#484)

`/dev/dri/renderD128` is a virtio-gpu render node, in every build. Stock Mesa
sees "Virtio-GPU Venus (Apple M4 GPU)", or the device's own GPU; behind it,
virglrenderer's Venus renderer runs inside the app on MoltenVK.

- **Vulkan, and OpenGL through zink, run on the GPU.** On the M4 iPad,
  glmark2 through zink scores 1299 against llvmpipe's 90, at 1/61 of the CPU
  per frame; vkcube runs at 54.8 fps against 29.1. zink here is OpenGL 2.1 /
  ES 2.0, so OpenGL programs stay on llvmpipe (4.5) unless you ask:
  `gpu-run <program>` runs one on the GPU, `setup-gpu.sh --gl-default` makes
  it the desktop's default.
- **`/AOK/tools/setup-gpu.sh`** installs Venus, zink and the Vulkan and Mesa
  tools on Devuan, Alpine or Arch, and checks the result. arm64 and x86_64
  roots have Venus; riscv64 and i386 distros ship none.
- **The Wayland desktop composites on the GPU by default**, falling back to
  software if it cannot (`ISH_DISPLAY_GPU=0` keeps software). Xwayland draws
  through zink too, so X11 OpenGL programs reach the GPU.
- **The desktop reaches the app without VNC.** The new native `wl-present`
  hands each changed frame to the app, which draws it with Metal, and carries
  pointer, keys, clipboard and resize back; wayvnc takes over if it stops. On
  the M4: up to 60 fps instead of wayvnc's 30, for less CPU.
- **The GPU survives the app going to the background.** Leaving the app with a
  GPU program running used to hang every GPU client until relaunch.
- **Older iPads.** The A10X (2017 iPad Pro) composites on the GPU and runs
  zink: Freedoom's timedemo runs at 79 fps, where in software it did not
  finish in 700 s. The A9 (5th-gen iPad) runs Venus and zink too.
- **Triangle fans no longer exhaust GPU memory.** Extreme Tux Racer's race
  took the app to 3.9 GB and on until iOS killed it at 6 GB, or stopped with
  "device lost". It now finishes at 1.0 GB. `/proc/ish/host_vm`, which found
  it, says where the app's own memory is, graphics memory included.

Known limits: suspend to disk cannot save a GPU program (see below), and on
the A10X results read back from Venus still vary from run to run.

## The Wayland desktop

- **Four desktops**: Ctrl+Alt+Left/Right (Shift takes the window along) and
  Ctrl+Alt+1-4.
- **Full screen and Workspace share one session.** Open Workspace parks it in
  a Workspace window; the corner menu's Wayland Full Screen moves it back.
- **Hardware keys go down and up as you press them**, lone modifiers
  included, so games read a held key (in Doom, Ctrl never fired). Arrows with
  Shift, Control and Option, and Control+Option with letters and digits, now
  arrive.
- **A tap no longer brings up the software keyboard**; Auto-Show or the menu's
  Show Keyboard does.
- **The app no longer freezes after you switch away and back** with a hardware
  keyboard; its keyboard handling could loop on the main thread.
- **The session ends when the compositor does**, so the Display applet no
  longer shows a dead desktop, and a stale session no longer blocks a new one.
- **#620:** root programs from the menu (Synaptic) ask for a password through
  sudo, since `pkexec` cannot work here, and everything the app starts gets a
  UTF-8 locale, which btop needs.
- **Freezes and failures fixed:** starting an X11 program, or Xwayland
  crashing, could freeze the desktop; fullscreen games leaked file descriptors
  and stopped after a few minutes; the boot's `/tmp` cleanup could take away
  the X display; a default user's session never got the Ctrl+Alt keys.

## Games

`/AOK/tools/setup-games.sh` installs Freedoom (Chocolate Doom) and Beneath a
Steel Sky (ScummVM) on Devuan, ready to play, plus Warzone 2100, Armagetron
Advanced, Chromium B.S.U., Blobby Volley 2, Neverball and Trigger Rally
(`--minimal` keeps to the first two), with sound, on the GPU where that works.
On the M4, Freedoom's timedemo runs at 180 fps on the GPU, under 35 in
software.

- On 2-CPU iPads Doom plays without music, and says so (`AOK_DOOM_MUSIC=1`
  keeps it): the timedemo went from 12.6 to 36.9 fps on a 5th-gen iPad.
- Chocolate Heretic, Hexen and Strife stay hidden until their data is
  installed. The menus honour `/usr/local` and `~/.local` desktop files.
- **Extreme Tux Racer is left out for now**: its pointer is out of step at the
  desktop's 2x scale, and two menu boxes draw solid. Installed by hand, it
  still runs through the games launcher.

## LLM Chat

- **API keys are now kept in the Keychain. This is a security fix:** any
  guest process could read them through `/proc/ish/defaults`.
- **Anthropic's Messages API** as a provider, with prompt caching.
- **File tools** (read, write, edit, list, glob, grep), each Allow, Ask or
  Deny, with rules for shell commands. Changes… (`/changes`) shows each edit
  as a diff with Revert; `/undo` reverts the newest.
- **MCP servers** as tool sources, remote or run in the guest (`/mcp`).
- **Background agents and sub-agents**, with a status panel (`/agents`).
  Replies stream, tool rounds included.
- **A working directory per chat**, with its `AGENTS.md`, a task list, and
  `/compact`. Apple's on-device model gets the conversation as turns, and no
  longer answers every earlier question again.

## The app and Workspace

- **Tabs in Terminal windows** (Cmd+T, Cmd+1-9), kept by layouts and suspend.
- **Saved Workspace layouts bring terminals back**; they restored everything
  else but not one. A resume with more than eight sessions restores them all.
- **Arrange the keyboard toolbar** (#609): Settings → Keyboard Toolbar,
  including your own keys that type text.
- **Delete a machine by swiping its row** (#575). Boot From This Filesystem
  asks whether to boot it next launch or quit now.
- **Output no longer cuts a selection down** to the part on screen (#617), and
  the terminal ends at the extra-keys row after coming back (#612).
- **Video Player** plays web video, HLS streams and `.m3u` playlists.
  **Browser** opens guest `.html`, `.svg` and `.pdf` from the File Manager,
  with twice the tab limit. **Equalizer**: 10 bands for Music (`ws-eq`).
- `/AOK/docs/keyboard-shortcuts.md` lists every hardware-keyboard shortcut.
  `ISH_BOOT_ROOT` boots a named root for one launch. ktop marks native
  programs "(n)".

## Suspend to disk

- **Saving with the Wayland desktop running** used to be refused. Save Anyway
  now leaves the desktop and GPU programs out as whole jobs and saves the
  rest; the background save does so without asking.
- A root running atop can be saved. chronyd and other local UDP services work
  after iOS suspends the app (chronyd used to spin a whole core).
- A program run from a memfd (runc) keeps its `/proc/<pid>/exe`, and a refused
  restore says why.

## Native programs

SmallCLUE's applets now match GNU coreutils 9.4, grep 3.11, findutils 4.9,
diffutils, sed 4.9, tar 1.35, gzip 1.13 and mawk 1.3.4, each checked against
the GNU tool's own output: `sed`, `grep`, `find`, `xargs`, `ls`, `cp`, `mv`,
`ln`, `rm`, `wc`, `head`, `tail`, `sort`, `date`, `stty`, `chmod`, `diff`,
`cmp`, `uniq`, `tr`, `nl`, `seq`, `touch`, `stat`, `realpath`, `readlink`,
`env`, `cat`, `rmdir`, `sum`, `dd`, `od`, `fold`, `tac`, `split`, `du`,
`gzip`, `tar` and `awk`. Underneath:

- Times use the guest's time zone (native `ls -l` could be an hour off) and
  keep their nanoseconds; `stty` settings take effect; device numbers and
  filesystem types read correctly.
- Output to a file or pipe is fully buffered: `seq 1 300000 > f` went from
  0.546 s to 0.024 s.
- `sum < file` no longer crashes the app, and `uname -m` reports the guest's
  architecture, not the iPad model (#622).
- **Native sudo** reads `@includedir` (a wheel member was refused), gives the
  target user's groups, and accepts `sudo NAME=value command`.

## Performance

- **A forked child reuses its parent's translated code.** Subshells take 14%
  less time on the M4, 20% on the A10X.
- **arm64:** glibc's `memcpy`, `memmove` and `memset` are one operation each
  (FEAT_MOPS: half the time for small copies on the A10X), and instructions no
  longer decode their operands every run (A10X `xz -6`: 27.5 s to 23.2 s).
- **riscv64:** common instruction pairs run as one (A10X `gzip -9` -13%,
  python3 -9%).
- **HLE is on by default**: up to 18% on musl and riscv64 programs, nothing
  measured slower. Your own setting still wins.
- **x86:** exact x87 fast paths (A10X amd64 `fadd` 79.7 ns to 29.8, `fsqrt`
  280 to 79), native i386 scalar SSE, i386 register arithmetic about twice as
  fast on the A10X.
- **Sparse reservations no longer cost page tables up front.** OpenCode's
  server used 1.08 GB of them, now 18 MB, halving its footprint.
- New for measuring: `/proc/ish/jit_timing`, `/proc/ish/hle`, `VmPTE`.

## Roots and the network

- **Alpine 3.24.2 is official** for x86, x86_64, aarch64 and riscv64, and the
  app bundles its aarch64 build in place of 3.23.3. Existing roots are
  unaffected.
- **DNS lookups no longer stall.** On Devuan every lookup waited out a 5 s
  timeout on the app's relay first (sshd came up 67 s after boot on a 5th-gen
  iPad). The relay now answers at `127.0.0.53`, in 200-310 ms.
- The Alpine setup scripts download every package before installing any, so a
  dropped download no longer gives "I/O error", and pick the fastest mirror.

## Kernel and emulator

**Programs that work now:** Python 3.14's `multiprocessing.Pool`, GNU
`tail -f`, `cp --sparse`/tar/rsync on tmpfs, `needrestart` and `lsof`
(" (deleted)" in `/proc` links), and a Go network scanner that crashed the
app. Codex CLI runs commands once `/AOK/fixes/codex` turns its sandbox off.

- **Security:** an unprivileged `open(f, O_PATH|O_WRONLY|O_TRUNC)` could empty
  a root-owned file. `O_PATH` now drops every other flag, as on Linux.
- **Files:** `pwritev2` honours its flags (on Alpine 3.24, `pwrite` to an
  `O_APPEND` file went to the end); `F_GETFL`, tmpfs `SEEK_DATA`/`SEEK_HOLE`,
  statfs, `getcwd` in a removed directory, and `/proc/mounts` in a chroot
  answer as Linux does.
- **Sockets and polling:** pipes and FIFOs report the other end leaving
  (`tail -f f | head -2` ends); local sockets match Linux on `recvmsg` names,
  listen backlogs and non-blocking `EINTR`; concurrent `epoll_ctl` no longer
  crashes the app.
- **x86:** misaligned `LOCK` operations are atomic, and no longer crash the
  app on i386; i386 `ENTER`, 16-bit branches, and `mmap` past 4 GiB work;
  i386 64-bit-time calls no longer fail at random with `EINVAL`; `#GP`, page
  faults and `FSQRT` report what hardware and Linux do (a misaligned
  `movaps`-class operand now faults with `SIGSEGV`, as on real hardware,
  where AOK used to let it through); `MOVNTPS`, `MOVNTPD`
  and `MOVNTI` exist.
- **Debugging:** `PTRACE_POKEUSER` and the debug registers follow Linux's
  rules, and single-stepping no longer skips a fused instruction.

## Removed

`/dev/aokgfx`, a PSCAL graphics channel nothing used; graphics go through the
GPU render node. A root booted since 2026-09-19 keeps the node (`ENXIO`).

## Issues you reported, closed in this build

- **[#484](https://github.com/emkey1/ish-AOK/issues/484)** — 3D acceleration.
- **[#575](https://github.com/emkey1/ish-AOK/issues/575)** — delete machines.
- **[#609](https://github.com/emkey1/ish-AOK/issues/609)** — keyboard toolbar.
- **[#612](https://github.com/emkey1/ish-AOK/issues/612)** — last row hidden.
- **[#617](https://github.com/emkey1/ish-AOK/issues/617)** — copy of a long
  selection.
- **[#622](https://github.com/emkey1/ish-AOK/issues/622)** — `uname -m`.

**[#620](https://github.com/emkey1/ish-AOK/issues/620)** stays open: Synaptic
starts and btop runs, but the reported display freeze on an x86_64 root has
not been reproduced (probably slowness under emulation).
**[#580](https://github.com/emkey1/ish-AOK/issues/580)** was confirmed fixed
by its reporter on 556.

Merged from bot pull requests: VoiceOver hints on applet toolbars and file
browsers ([#611](https://github.com/emkey1/ish-AOK/pull/611),
[#613](https://github.com/emkey1/ish-AOK/pull/613),
[#623](https://github.com/emkey1/ish-AOK/pull/623)), and a bounded `snprintf`
for `TERM=` ([#624](https://github.com/emkey1/ish-AOK/pull/624)).

## How this build was tested

The full guest regression suite ran on five roots on the Mac -- Alpine 3.23.3
i386, x86_64, aarch64 and riscv64, and Devuan 6 aarch64 (glibc) -- and again on
Devuan with the native tools first on `PATH`. The same five roots ran BOOTED on
an M4 iPad Pro, and Devuan 6 on a 5th-generation iPad (iPadOS 16). The e2e
suite passes on the Mac and on Linux CI. Every failure was re-run alone and
either fixed or traced to the test. Three are still open and not known to be
new in 557: a misaligned `LOCK` operation can starve another thread for a
moment on x86 guests on the M4 (1 run in 6); a thread CPU-time timer fires
early on the 5th-generation iPad; and #625 ("Browse Files…" in Filesystems on
iOS 27.0.1), which does not reproduce in the iOS 27 simulator.
