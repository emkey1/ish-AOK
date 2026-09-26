# Release Notes Since `builds/iSH-AOK_555`

282 commits. This build is mostly about the guest being what it
claims to be. Security features that Linux programs rely on were accepted and
then ignored; now they are enforced. Timers now fire on time. And a long list
of emulator, filesystem and memory behaviour now matches what a real Linux
machine does.

## Before you update

**Saved sessions from 555 will not restore.** The suspend-to-disk image
format has changed (checkpoint version 5 in 555, 22 now), because the image now
carries a good deal more: seccomp filters, capabilities, the NX signal page,
timers and their deadlines, and a memfd's identity. A session saved by 555
cannot be resumed by 556, so the guest boots fresh.
Finish anything that lives only in a saved session before you update.

**Native bash is gone**, as 555 announced. `/AOK/native/bash` is no longer in
the build, so the app carries no GPLv3 code. Guest bash (`/bin/bash`,
`apt install bash`, `#!/bin/bash`) is untouched. Native zsh and native dash
remain.

## Security: what was accepted is now enforced

Several Linux security interfaces answered "yes" and did nothing. A program
that sandboxed itself believed it was confined, and was not. Each one now works
as it does on Linux:

- **seccomp.** Strict mode and BPF filters are enforced. OpenSSH's
  pre-authentication child, systemd's `SystemCallFilter=`, and the sandboxed
  helpers that apt and man-db use are now actually confined.
- **NX.** Memory mapped without `PROT_EXEC` no longer executes, on every
  engine. Code written to the stack, the heap or a data page faults with
  `SIGSEGV` (`SEGV_ACCERR`), as it does on Linux.
- **ASLR.** Each `exec` places the stack, heap, program, loader and libraries
  at random addresses. Set `ISH_RANDOMIZE_VA_SPACE=0` for reproducible layouts
  while debugging.
- **Other users' processes.** `/proc/<pid>` no longer shows another user's
  memory map, open files, cwd, root, exe or I/O counters. `PTRACE_ATTACH` and
  `PTRACE_SEIZE` now check credentials: before, any user could read and write a
  root process's memory. Because of this, ktop now reads each process's
  architecture from the new `/proc/ish/arch` (see below).
- **Resource limits.** `RLIMIT_CPU` (`SIGXCPU`, then `SIGKILL` at the hard
  limit), `RLIMIT_AS`, `RLIMIT_DATA` and a tmpfs `size=` were stored but never
  checked. They are now enforced. A stack limit that another process lowers
  applies immediately.
- **chgrp.** A file's owner may give it only a group the owner belongs to.
  Before, any user could `chgrp` a program to any group and `chmod g+s` it,
  which was a way to run as any group, including `shadow`.
- **Set-id programs under a tracer or `no_new_privs`** gain nothing, as on
  Linux. `AT_SECURE` and the saved and filesystem ids follow Linux's rules.

Also new: extended attributes and file capabilities (`setcap` works),
`rseq`, IPC namespaces, POSIX message queues, and BSD process accounting
(`atopacctd` has records to read). `setns(2)` works too: joining a UTS or IPC
namespace switches to it, and atop no longer logs "stub syscall 268" when it
starts. The kernel now reports version 5.10.0 rather than 5.20, a release that
never existed.

## Timers fire on time

Darwin coalesces host timers: a sleep may run late by up to a quarter of what
was asked for, capped at 5 ms. Every guest timer and timed wait inherited that
delay. On the M4 iPad, a 50 ms `nanosleep` woke 5 ms late and a 20 ms POSIX
timer 4 ms late; Linux is late by microseconds. Guest timers and timed waits
now ask the host for precise wake-ups. Measured on a Mac: timers about 25 µs
late, sleeps and waits 30–160 µs.

In the same area:
- a thread CPU-time timer samples nanoseconds, not 10 ms jiffies;
- `ITIMER_PROF` and `ITIMER_VIRTUAL` fire at the rate asked for;
- a late periodic timer counts the periods it missed, and stays on its grid;
- `timer_getoverrun` is correct.

Two bugs underneath lost wake-ups, and both are fixed:
- **Deaf threads.** On Darwin, `sigprocmask` sets the mask of every thread in
  the app. So each poll, socket or pipe wait that a signal interrupted left
  every host thread unable to hear its next wake-up.
- **A leaked Mach port per exiting thread.** A long enough session was killed
  by iOS at about 115,000 ports. `/proc/ish/host_ports` now reports the count.

## The emulator, and x86 in particular

- **i386:**
  - `lock not` and `lock neg` were `SIGILL`; they now run;
  - segment registers load and read as on 32-bit Linux, with separate TLS
    bases for FS and GS;
  - the 16-bit stack instructions move two bytes, and `PUSHA`/`POPA` fault as
    one instruction;
  - a fault at address 0 is reported as `SEGV_MAPERR` at 0.
- **amd64:**
  - segment registers, `IRETQ`, a GS base of its own (`ARCH_SET_GS` and
    `ARCH_GET_GS`), and `POPF` of `AC` and `ID`;
  - a `#GP` is reported the way Linux reports it;
  - `HLT`, `CLI`, `STI` and `int n` fault.
- **Self-modifying x86 code.** Code rewritten by a store now runs as
  rewritten, including a rewrite made through a second mapping of the same
  memory. That is what JITs such as HotSpot do.
- **CPUID** says family 6 and `CLFLUSH`, and `/proc/cpuinfo` is read from it.
- **Floating point:**
  - rounding modes and exception flags work on every guest architecture;
  - x87 gains `FPTAN`, `FDECSTP` and `FYL2XP1`;
  - denormals get their real exponent in `FSCALE`, `FXTRACT` and `FPREM`.
- **vDSO.** 64-bit guests now read the clock without a system call
  (`ISH_VDSO=0` turns this off).

## Memory and fakefs

- **Finding free address space costs per region, not per page.** Every mmap
  that does not use `MAP_FIXED` searched the whole address space, one page
  entry at a time. A single 2 GiB mapping made each later mmap read 28 MB.
  `dotnet --info` on an amd64 guest used to spend most of an hour here, and
  now takes under a minute.
- **`/proc/meminfo`** keeps counters for `Shmem`, `AnonPages` and `Mapped`.
  Before, it walked every page of every process under a lock on each read,
  which the .NET garbage collector does on every collection.
- **Memory reporting:**
  - `VmSize` counts the address space, `VmRSS` counts what is used, and the
    peaks are real peaks;
  - `mlock` shows up in `VmLck`, `smaps` and `maps`;
  - `mlock2(2)` and `MLOCK_ONFAULT` are supported.
- **Pages past the end of a file.** A system call that touches such a page,
  and a suspend-to-disk save that reads one, now fail cleanly. Before, they
  killed the app with `SIGBUS`. Apple's crash reports showed the save case on
  555.
- **fakefs** lookups stay fast under memory pressure. Apple's SQLite drops
  its page cache on every statement, and under pressure a lookup on a large
  root went from 40 µs to 91 µs. With the pages in the OS file cache, it
  measures 25 µs. Every change to an inode also moves its ctime now.

## Workspace, Wayland and the app

- **One Wayland desktop at a time.** A second `start-wayland.sh` used to
  silently end the desktop in use. That includes a second Wayland applet and
  the script typed into the desktop's own terminal. It now refuses with "a
  Wayland session is already running (pid N)" and changes nothing.
- **ws- launchers.** `ws-wayland`, `ws-desktops` and `ws-quickactions` are
  new. None of the `ws-` launchers could be found by name in the app's own
  terminals, because `/etc/profile` reset `PATH`. Each boot now installs
  `/etc/profile.d/10-aok-persist-bin.sh`, which puts `/AOK/persist/bin` on
  `PATH` for app terminals, ssh logins and `su -`.
- **`/proc/ish/arch`** lists each process's architecture (`aarch64`,
  `x86_64`, `i686`, `riscv64` or `native`), and anyone may read it. ktop's
  ARCH column uses it.
- **`/proc/ish/applets`** lists the open Workspace tool windows. They are app
  UI, not processes, so they do not appear in `ps` or `top`.
- **Suspend to disk:**
  - automatic saves (the ones iOS triggers) now reuse a single slot, so they
    no longer push out the sessions you chose to keep;
  - saved sessions can be deleted from the start screen;
  - a restore now brings back local sockets with their queues, named FIFOs,
    tmpfs contents, timers and queued signals, the guest clocks, threads and
    zombies.
- **DNS.** Settings → Custom DNS Servers has a third choice, **Don't
  Manage**. It leaves `/etc/resolv.conf` alone, for a root that runs its own
  resolver.
- **Roots.** The app downloads the root catalogue instead of shipping a
  frozen copy of it, so new roots appear without an app update.
- **Native `su`, `sudo` and `passwd`** are setuid-root programs of their own,
  and they check the guest's `/etc/shadow`.
- **iOS Files mounts** made with a new-API `mount(8)` (such as util-linux
  2.41) now survive a relaunch.

## Experimental: swap to an external drive

Swap can now go to a USB drive instead of the app's own storage
([#605](https://github.com/emkey1/ish-AOK/pull/605), contributed by
@KrisButIAmAnOSDev). **This is experimental, it is off by default, and it has
not been tested on a real drive.** The kernel side passes its round-trip test
on APFS, exFAT and FAT32 disk images. The folder picker, remembering the
drive across launches, and an actual USB drive on iPadOS have not been
exercised. Please report what happens if you try it. In particular, a slow
launch with a drive attached is worth reporting.

## Issues you reported, closed in this build

- **[#602](https://github.com/emkey1/ish-AOK/issues/602)** — debconf could
  not start its Dialog frontend on Devuan, so questions came out as a bare
  numbered list. It now uses dialog.
- **[#603](https://github.com/emkey1/ish-AOK/issues/603)** — after a paste,
  Enter did nothing until you tapped the terminal. A paste now gives the
  keyboard focus back to the terminal.
- **[#605](https://github.com/emkey1/ish-AOK/pull/605)** — swap to an external
  drive (experimental; see above).

Apple's crash reports for 555 held two signatures. Both are fixed in this
build: the suspend-to-disk `SIGBUS` above, and the remaining RunningBoard
`0xdead10cc` kills, where iOS suspended the app while it held a filesystem
lock (three gaps in the suspension guard, now closed).

Still open: **[#580](https://github.com/emkey1/ish-AOK/issues/580)** (window
controls in iPadOS windowed mode) has a further fix in this build that has
not yet been confirmed on a device that shows the problem.
