# Release Notes Since `builds/iSH-AOK_557`

199 commits. This build adds a root with no Linux distribution in it: native
mode boots straight into iSH-AOK's own programs, with nothing to download. The
x86 guests now run every instruction as gadgets, advertise AVX, AVX2 and
AVX-512 (glibc rates them x86-64-v4), and run 7-Zip about five times faster on
amd64. riscv64 gains RVA23 and its vector unit, so Ubuntu 25.10 runs. Around
that: Wayfire and Xfce desktops, the app in nine languages with Chinese and
Japanese input at the cursor, and a `git` that works.

## Before you update

**Saved sessions from 557 will not restore.** The suspend-to-disk format
version is unchanged (23), but an image is tied to the build that saved it,
and this build's CPU state is laid out differently (the riscv64 vector
registers, the x86 `TSC_AUX`, MMX now held in the x87 registers). The guest
boots fresh, and the resume picker marks such an image "can't be restored".
Finish anything that lives only in a saved session before you update.

**Login shells that name `/AOK/native/bash` are repaired at boot.** sshd,
`login` and `su` refuse a user whose shell is missing, which reads as a bad
password or key. Each boot now points such a shell at the root's own program
of that name (a distro's `/bin/bash`), else native zsh, else `/bin/sh`, and
says so in the kernel log. Nothing else in `/etc/passwd` changes.

**The provisioning scripts make sudo passwordless by default.** The Alpine,
Devuan and Arch scripts create the login with no password, then gave sudo a
rule that asked for one, so the new account could never use it.
`SUDO_NOPASSWD` now defaults to yes; `SUDO_NOPASSWD=0` or answering `n` keeps
the password rule (then run `passwd <user>`). The scripts' sudoers fragments
are now `00-aok-sudo`, `00-wheel` and `00-aok-wheel`, so your own files sort
after them and win. Roots already provisioned change only if you re-run.

**x86 programs now take their AVX code paths.** CPUID advertises AVX, AVX2,
FMA, AVX-512 and more, so glibc, OpenSSL and anything else that checks picks
different routines than on 557. All of it was checked against hardware or
Intel's emulator, but it is the largest change in what the x86 guests run;
please report anything that behaves differently.

## Native mode: a root with no distribution

Pick **iSH-AOK Native** under Official Distributions in Filesystems. It is
created on the spot, with nothing to download or unpack: iSH-AOK fills the
empty filesystem at every boot and keeps it in step with the build. You get
native zsh as the login shell, native dash as `sh`, about 140 everyday
commands, ssh, git, curl and editors, all running as the iPad's own code.
There is no package manager, compiler, Python or Wayland desktop; for those,
install a distribution beside it. `/AOK/docs/native-mode.md` has the details.

- **The first boot asks for your everyday account**: a name and an optional
  password, made at uid 1000 in `sudo` and `users`. Skip leaves root only.
  The question never holds up the boot, and Open Everything as Default User
  then works as on a distribution root.
- **A real init.** SmallCLUE's `init` runs as pid 1, runs `/etc/rc`, and stays
  until `halt`, `poweroff` or `reboot`, which run `/etc/rc.shutdown`. Services
  in `/etc/service` run under `runit` and answer `sv status/restart/down/up`.
  After a suspend and restore, rc does not run again and runit takes back the
  services it had.
- **A familiar layout**: `/bin` and `/sbin` are links into `/usr` (scripts
  that name `/bin/echo` work), `login` at `/bin/login`, the terminfo entries,
  zsh's completion system (`compinit` gives 1299 completions), and the
  device's time zone through the host's zoneinfo. A file of your own at one of
  those paths, and your edits in `/etc`, are left alone.
- **New native commands**: `init`, `login`, `sv`, `tput`, `reset`, `free`,
  `printenv`, `mount` and `umount`, curl's `-I`, `-i`, `-f` and `-w` with
  curl's exit codes, and `wget -qO-`. `uname -o` says `AOK/Linux`.
- **Reaching a distribution**: `mount-root.sh` now binds `/AOK/native` and
  `/AOK/docs` into the root, so native zsh works inside a `chroot`, and
  `chroot` falls back to `/bin/sh` when `$SHELL` is missing there.

## Programs from other roots

Every installed root is at `/AOK/roots/<name>`, and running one of its
programs by path used to fail, as on Linux, because its loader was looked up
in the booted root. It now runs, from any root, native mode included.
Settings → Other Filesystems → Programs From Other Roots chooses how, from
the next exec:

- **Run Inside Their Root** (default): the program is chrooted into its own
  root, with `/proc`, `/sys`, `/dev`, `/run` and the native programs, docs and
  tools under `/AOK` bound in first, as `mount-root.sh` does. The working
  directory is kept if it is inside that root.
- **Use Their Libraries Here**: it runs in this root with its own loader,
  libraries and locales (Devuan's tmux refused to start without them); the
  variables are taken back out of the next program that does not need them,
  and your own `LD_LIBRARY_PATH` or `LOCPATH` wins.
- **Off**: as before.

The same applies to `#!` scripts whose interpreter is in another root, and to
paths that pass through an absolute symlink inside one (Alpine's `/bin/sh`).
Checked on the M4: Devuan's tmux 3.5a from a native root. Other roots are
mounted about a second after boot starts, so an rc script that runs earlier
may not find them yet.

## x86: every instruction a gadget, and AVX

Every x86 instruction the JIT runs is now a gadget, on both guests: the
integer ALU, the string instructions, SSE through SSE4.2, x87 (the
transcendentals included), VEX and EVEX, XSAVE, misaligned `LOCK`. The C
paths behind them are deleted, and many of them were wrong. What a user sees:

- **AVX, AVX2, FMA, F16C, AES-NI, PCLMULQDQ, MOVBE, AVX-VNNI and AVX-512**
  (F, DQ, BW, VL, CD, IFMA, VBMI, VBMI2, VNNI, BITALG, VPOPCNTDQ, BF16, GFNI,
  VAES, VPCLMULQDQ) are advertised, with XSAVE state carried through signal
  frames, ptrace (gdb reads it) and exec as Linux lays them out. glibc rates
  the guest x86-64-v4; its AVX2/EVEX string routines are faster here than the
  SSE2 ones (memcpy of 16 KB about 2x, memset 2-3x). AVX-512 was checked
  against Intel's SDE, since no AVX-512 hardware was at hand; AVX-512-FP16
  and AMX are not implemented.
- **x87 is exact to the hardware**, rounding, precision control, the tag
  word and unmasked exceptions (`SIGFPE`) included, and 1.3x (i386) to 1.7x
  (amd64) faster on a long-double loop. The transcendentals (`FSIN`, `FPTAN`,
  `FYL2X` and the rest) are correctly rounded, where the old C failed 12,083
  of 16,989 hardware cases; `FYL2X` went from 540 ns to 136.
- **The MMX registers are the x87 registers**, as on hardware, so `FXSAVE`,
  `FNSAVE` and signal handlers see MMX values where they belong, and x87 code
  after MMX code without `EMMS` sees what hardware shows it.
- **SSE floating point matches x86 bit for bit**: separate DAZ and FTZ,
  denormal flags, NaN order and sign, `MIN`/`MAX`, tininess after rounding,
  `ROUND*` honouring its immediate. The old paths differed in 4,288 of 41,664
  cases.
- **No longer `SIGILL`**: MMX shift-by-immediate, `PEXTRW`/`PINSRW` and
  `MASKMOVQ`/`MASKMOVDQU`, the SSSE3/SSE4 MMX forms, and an addr32 gather with
  a segment override. `RDTSCP` and `RDPID` exist and report the CPU `getcpu`
  does (HotSpot uses them). `SGDT`, `SIDT`, `SMSW`, `SLDT` and `STR` answer as
  Linux spoofs them on a UMIP processor, and `LAR`, `LSL`, `VERR`, `VERW`
  read Linux's descriptor table.
- **Faults and flags as on hardware**: a signal handler starts with DF and TF
  clear (after `std` in the interrupted code, a handler's `rep movsb` copied
  backwards); `POPF` setting TF traps; an addr32 (`0x67`) load through a
  pointer with high bits set no longer `SIGSEGV`s; `mmap(MAP_32BIT)` places
  below 2 GB; i386 memory `BT*` with a negative index no longer faults; the
  BCD adjusts match an AMD Ryzen on every input; an encoding x86 lacks is
  `#UD`.

## x86 speed

Build 557 against this one, the two apps installed alternately on each
device, medians of the runs. 7-Zip is `7z b 1 -mmt1 -md22` (single-thread
MIPS, higher is better); Python is a microbenchmark's `fib` and `dict` times
in ms (lower is better).

| device | guest | 7-Zip 557 → 558 | Python fib | Python dict |
|---|---|---|---|---|
| M4 iPad Pro | x86_64 | 165 → 817 (**5.0x**) | 338 → 87 | 3366 → 1064 |
| M4 iPad Pro | i386 | 349 → 599 (1.7x) | 182 → 119 | 1800 → 1264 |
| M4 iPad Pro | arm64 | 997 → 979 | 90 → 93 | 1058 → 1078 |
| M4 iPad Pro | riscv64 | 913 → 886 | 95 → 95 | 1297 → 1368 |
| A10X iPad Pro (2017) | x86_64 | 69 → 211 (**3.1x**) | 787 → 336 | 13716 → 6886 |
| A10X iPad Pro | i386 | 119 → 187 (1.6x) | 446 → 330 | 10319 → 8509 |
| A10X iPad Pro | arm64 | 318 → 323 | 226 → 226 | 5702 → 5623 |
| A10X iPad Pro | riscv64 | 290 → 288 | 253 → 264 | 5661 → 5692 |
| iPhone 12 | x86_64 | 110 → 418 (**3.8x**) | 574 → 175 | 4634 → 2051 |
| iPhone 12 | i386 | 220 → 341 (1.6x) | 389 → 280 | 3350 → 2448 |
| iPhone 12 | arm64 | 529 → 514 | 149 → 155 | 1351 → 1409 |
| iPhone 12 | riscv64 | 501 → 496 | 166 → 169 | 2112 → 2199 |

The x86 guests got this cycle's speed work; arm64 and riscv64 are where they
were. (Different rows use different distributions' 7-Zip, so compare builds
within a row, not rows with each other.)

**amd64.** On the M4, 7-Zip's single-thread benchmark (`7z b 1 -mmt1`) went
from 164 to about 820, past the i386 guest; a Python microbenchmark runs three
to four times faster (fib 310 ms to 84, dict 1235 to 376), now level with the
arm64 guest. The work: flags nobody reads are no longer computed, flags are
built without branches (+53% on its own), and registers, memory operands,
compares, branches and returns run on a register cache specialised per
register. A double-precision loop with `sqrtsd` ran 28x faster once that
instruction was translated (it fell back to the interpreter every iteration).

**i386.** 7-Zip on the M4 went from 355 to 595 (fused register ALU, moves,
scaled-index addresses and compare-and-branch), +33% and then +9% on the
A10X. Python is 12-29% faster on the M4 with SSE moves as gadgets, and
indirect calls take 59% less time. 32-bit programs on amd64 roots run on this
engine too. Small `REP MOVS` and `STOS` copies are about 30% quicker.

## riscv64: RVA23 and the vector unit

Ubuntu's riscv64 builds moved to the RVA23 baseline, which the guest did not
implement. It does now:

- **The scalar extensions**: Zba, Zbb, Zbs, Zicond, Zcb, Zfa, Zfhmin, the
  cache-block ops (`cbo.zero` zeroes; it was ignored), Zimop/Zcmop and Zawrs,
  with `riscv_hwprobe` and the `/proc/cpuinfo` isa line saying so.
- **V (RVV 1.0, VLEN 128, with Zvbb, Zvkt and Zvfhmin)**, every instruction
  a gadget, advertised through hwprobe and `AT_HWCAP`, carried in signal
  frames, ptrace and suspend images, with `PR_RISCV_V_SET_CONTROL` as Linux
  has it. OpenSSL 3.5 picks its vector ChaCha20 by itself: 108 MB/s on the
  M4 against 52 scalar, 44 against 14 on the A10X.
- **Ubuntu 25.10 riscv64 runs**: apt, Python 3.13 and gcc 15, on the Mac and
  the M4. Its coreutils (uutils, one binary under 115 hard links) needed
  `/proc/self/exe` to name the link that was run, which every root now gets.
- Misaligned `LR`, `SC` and AMOs are `SIGBUS` as on Linux (they ran
  non-atomically, or faulted the host on the A10X); `fmin`/`fmax` against a
  signalling NaN and out-of-range converts raise what RISC-V raises. All of
  this is checked against models written from the specification; a RISC-V
  board with V is on order to check it against hardware.

## arm64

No arm64 instruction runs in C any more. The atomics run as inline host
loops, on the A10X too; a misaligned atomic is `SIGBUS` (it retried forever).
`LD1`-`LD4`/`ST1`-`ST4` of every form leave every register unchanged when they
fault. SHA-512 and CRC32 on devices without them (the A10X and every pre-A13
device) and MOPS copies past 64 bytes are gadgets too.

## Wayland desktops

- **Wayfire and Xfce.** `/AOK/tools/setup-wayfire.sh` and `setup-xfce.sh`
  install them (the base desktop first, if needed), and `select-desktop.sh`
  chooses between labwc (the default), sway, Wayfire and Xfce. Wayfire
  composites on the GPU, needs a user session (it refuses root, as on Linux)
  and runs on Devuan 6 of any architecture (as arm64 where the root is not).
  `--panel` offers a faster wf-panel (2.1 s per menu open on the A10X,
  against 4.7), waybar, or both.
- **wayvnc 0.10.2 is bundled** for Devuan aarch64, x86_64 and riscv64 in the
  new `/AOK/bundled`. Devuan's 0.9.1 could die as the app took the desktop
  over, leaving it at "Connecting to compositor..." (5 of 15 starts on the
  M4); a wayvnc that dies is now restarted.
- **Software compositing is much faster at UI scale 2.** GL programs now pick
  8-bit buffers, which pixman has fast paths for, and the 2x stretch runs on
  the host: labwc on the A10X went from 1.6 to 19.8 fps on es2gears, and from
  580 to 10.7 ms of compositor CPU per frame. x86 guests get the stretch too.
- **The GPU on more roots**: Devuan riscv64 (checked on the A10X: Venus and
  zink, labwc composited on the GPU), and Devuan i386, which has the driver
  but is untested. `setup-gpu.sh` on Alpine installs the Vulkan loader Venus
  needed, says Alpine 3.23 has no Venus driver, and reports when OpenGL is in
  fact running in software (see Known issues).
- **Crashes and freezes fixed**: zink leaked three descriptors a frame until
  GTK programs crashed and the app went down accepting the next client (now
  about 50 held after minutes); wf-panel's menu crashed GTK 3 on a buffer
  released twice (5 crashes in 6 rounds, now 0 in 10); Qt programs and
  Chocolate Doom jumped to address 0 through the preloaded shims; native dash
  crashed the app 1-3 minutes into a Wayfire session.
- **Behaviour**: closing the Wayland window ends the whole desktop, for the
  default user too; the first terminal opens as a window at the real screen
  size, below the panel; X programs (Dillo) start from Wayfire's panel;
  terminal programs from the menus (btop) open in foot.

## Chinese and other languages

- **Typing Chinese, Japanese and other composed scripts**: the composition
  shows underlined at the cursor, and a hardware keyboard's candidate window
  sits beside it instead of in the corner. Korean types each syllable in
  place, as iOS gives it.
- **The app in nine languages**: Simplified and Traditional Chinese,
  Japanese, Korean, Spanish, French, German, Brazilian Portuguese and
  Russian, from about 2,000 strings that were all fixed English (App Store
  feedback asked for Chinese).
- **The guest in your language**: `/AOK/tools/setup-locale.sh` makes your
  device's first language the default for logins, zsh and the desktop
  (`zh-Hans` becomes `zh_CN.UTF-8`), generating it on Devuan and Arch and
  installing musl-locales on Alpine, from the new `/proc/ish/languages`.

## Native programs

- **git works.** Every earlier build's `git` said "libgit2 support is not
  enabled"; libgit2 is now built into the app (its licence permits that):
  init, add, commit, status, diff, log, branch, checkout and HTTPS clones
  (jq's 1954 commits in 13.7 s on the Mac). `git@host:` (ssh) remotes are not
  supported yet; use the HTTPS URL.
- **Login records**: native `login` and `init` write `/var/run/utmp` and
  `/var/log/wtmp` (logins, boots, runlevels, logouts, shutdowns) in the
  guest's own layout, and native `who` and `users` read them. Native zsh's
  `WATCH` read arm64 and riscv64 records as garbage from the second one on.
- **Patterns match as on Linux**: native programs used Darwin's `fnmatch`,
  which disagrees with glibc's on 2,340 of 14,960 cases, so `find -name 'a[b'`
  found nothing and `ls -I`, `grep --include`, `diff -x` and
  `tar --wildcards` failed on such names. Native dash's `${t#socket:[}`
  stripped nothing for the same reason.
- **Native zsh**: `kill %1` and `kill PID` reach its jobs again (they
  inherited the interactive shell's ignored TERM and QUIT). A program a
  native program `exec`s that traps a signal now lives on as the same pid
  until its trap finishes, instead of being reported dead at once.

## Kernel and conformance

**Programs that work now:** systemd on Arch Linux ARM boots again (557 stopped
at "Failed to start up manager"): on the Mac it reaches graphical.target in
7.4 s, and on the M4 it runs with no failed units, `systemctl`, `hostnamectl`
and `localectl` answering. Also Ubuntu 25.10's coreutils, and anything using
priority-inheritance mutexes.

- **PI futexes** (`FUTEX_LOCK_PI`, `LOCK_PI2`, `TRYLOCK_PI`, `UNLOCK_PI`) were
  `ENOSYS`, so glibc refused every `PTHREAD_PRIO_INHERIT` mutex. No priority
  is actually inherited.
- **For systemd**: cgroup2 membership is live (each failed oneshot waited
  90 s), `SO_PEERCRED` works right after `connect()` (every `systemctl` failed
  with "No data available"), `kernel.pid_max` takes up to 4194304, units with
  `PrivateNetwork=` run, and a closed console no longer leaves the kernel log
  locked, which silenced PID 1.
- **Files**: file times are signed 64-bit (`touch -d @-1` read back as
  4294967295); a `/proc` fd link out of a chroot leads to its file; `O_PATH`
  opens pipes, sockets and removed directories through `/proc`; FUSE mounts
  through `fsopen`/`fsmount` no longer hang; a library or program in `/AOK`
  can be mapped and run (`LD_PRELOAD` of one failed, exec of one killed the
  app).
- **Sockets**: at the descriptor limit `accept` is `EMFILE` with the
  connection left queued (it dropped the client, and could crash the app).
- **Terminals**: a pty hangup signals the session leader alone, as on Linux;
  AOK used to kill the foreground group too.
- **Processes**: a guest `execve` takes as much argument text as Linux's limit
  allows (2 MB with the default 8 MB stack) instead of 128 KB; `cp dir/*`
  over a few thousand names no longer fails "Argument list too long".
  `anon_inode` link names read as Linux spells them.

## Crash fixes

Apple's crash reports for 557 held two signatures, both fixed: native zsh
crashing under an amd64 guest (a signal the program had blocked was delivered
anyway, while zsh walked a job list it then freed), and reading
`/proc/sys/fs/binfmt_misc/register` once binfmt_misc is mounted (now
`EINVAL`, as on Linux). The A10X crashes above (`accept`, exec from `/AOK`,
native dash, the zink leak) are fixed too, as is an EVEX encoding that
aborted the app.

## The app

- **Resume picker**: with one saved session, Delete asks once, then boots
  fresh. **LLM Chat on iPhone**: the message box rides above the keyboard
  instead of behind it (TestFlight feedback).
- Every root's session falls back to native zsh and then `sh`, so a root with
  no working login or shell still gives a root prompt.
- **VoiceOver**: the scrollback find bar's buttons say what they do, and a
  file browser's `/` is read as "Root Directory", not "slash", in all ten
  languages (#633, #635).
- **Provisioning a root no longer needs a password for sudo** -- see
  *Before you update*.
- `/AOK/docs/shortcuts.md` documents every Shortcuts action in full;
  `/AOK/docs/guest-cpus.md` says what each guest CPU advertises.

Internal: Linux CI builds and passes again (the x86_64-host backend).

## Testing

The guest regression suite on the build being shipped:

| where | root | pass | fail |
|---|---|---|---|
| Mac (M5 Max) | Alpine 3.24 i386 | 430 | 0* |
| Mac | Alpine 3.24 x86_64 | 450 | 0* |
| Mac | Alpine 3.24 aarch64 | 364 | 0* |
| Mac | Alpine 3.24 riscv64 | 360 | 0* |
| Mac | Devuan 6 aarch64 (glibc) | 364 | 0* |
| Mac | Devuan 6 x86_64 (glibc) | 450 | 1*** |
| M4 iPad Pro (iPadOS 26) | Devuan 6 aarch64, booted | 370 | 0 |
| A10X iPad Pro (iPadOS 17) | Devuan 6 aarch64, booted | 369 | 1** |
| iPhone 12 (A14, iOS 26.3) | Devuan 6 aarch64, booted | 368 | 0 |
| iPhone 12 | Devuan 6 x86_64, booted | 453 | 1*** |

Each count leaves out 16-26 tests that skip by design on that root. \* One
networking test fails on that Mac because its firewall holds back inbound
connections to a freshly built binary; it passes on a Mac without one.
\*\* A thread CPU-time timer fires a few ms early on the A10X; build 557 does
the same there (see Known issues). \*\*\* The misaligned-`LOCK` test, which
catches the starvation listed under Known issues about one run in three, on
557 as well. `meson test e2e` (the i686 suite, with
qemu's x86 instruction test matching real hardware line for line) and Linux
CI pass.

The release pass itself found and fixed three things in this cycle's own
work before it shipped: gcc and Wayfire crashing on the A10X (a memset
instruction's gadget kept its fill pattern in a register a helper could
overwrite; only older chips showed it), native zsh hanging now and then on a
fast Mac (a child's exit signal taken just before the shell went to sleep
waiting for it), and guest `execve` refusing more than 128 KB of arguments.

## Known issues

- **ssh sessions drop when the screen goes idle**
  ([#631](https://github.com/emkey1/ish-AOK/issues/631)). iOS suspends the
  app and takes its connections with it; keeping the screen on keeps the
  session. Not worked on this cycle.
- **OpenGL stays in software on Alpine 3.24 and Arch.** Their Mesa (25.2 and
  later) refuses to start zink without a Vulkan feature the GPU path does not
  yet offer; Vulkan and the compositor still use the GPU. Wayfire, which
  draws only on the GPU, needs Devuan 6.
- **A misaligned `LOCK` operation can starve its process's other threads**
  for a while on x86 guests (carried over from 557). The fix that was
  measured cost 30-170x where every thread splits, so it was not kept.
- **Native zsh cannot run one command with more than 128 KB of arguments**
  (`rm *` over a few thousand files): it hands the words to its child as a
  single string. `sh`, bash and the tools themselves take the new 2 MB.
- **On the A10X (iPadOS 17), a `CLOCK_THREAD_CPUTIME_ID` timer can fire a few
  milliseconds of CPU early.** Not new in this build.
- **Xfce draws no desktop backdrop**: icons on black.
- **On distribution roots, `who` misses the app terminal's own login**: the
  guest's boot resets utmp a few seconds after login records it.
