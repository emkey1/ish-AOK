# Release Notes Since `builds/iSH-AOK_560`

41 commits. This build adds a fifth guest architecture, 64-bit
little-endian PowerPC (ppc64le), as an experimental download. It fixes a
data loss in the Files app: moving a large file out of a folder could empty
most of that folder. Install a Desktop shows its progress and can remove a
desktop, and Save Anyway works with a sandbox running. There are fixes for
561's one new crash and a handful of kernel corrections.

TestFlight's build 561 was 560's code, renumbered when it was uploaded, so
this is the first build after 560.

## AOK VM is on TestFlight

**AOK VM**, iSH-AOK's sibling app, is a full virtual machine for iPhone and
iPad: a fork of UTM's iOS SE edition, built around QEMU with the AOK gadget
engine. Where iSH-AOK runs Linux programs on its own kernel, AOK VM boots a
whole operating system with its own kernel. Join the beta at
**https://testflight.apple.com/join/wzDUq3rP**. AltStore, SideStore and
Sideloadly users can
[download the IPA](https://github.com/emkey1/aok-vm/releases/latest/download/AOK-VM.ipa).

## Before you update

**Saved sessions from 560 and 561 will not restore.** An image is tied to the
build that saved it. The guest boots fresh, and the resume picker marks such
an image "can't be restored".

**Each filesystem's database is upgraded the first time 562 boots it** (schema
version 10, which records the next inode number; see below). The upgrade
takes a moment and needs nothing from you. An older build can still open the
upgraded filesystem: it ignores the new field.

## A fifth guest: ppc64le (experimental)

iSH-AOK runs 64-bit little-endian PowerPC programs: a POWER8 (Power ISA 2.07)
with VMX and VSX, the baseline Alpine's `ppc64le` and Debian's `ppc64el` are
built for. Like the riscv64 guest it is gadgets only, with no C core.

- **Get it** from Settings → Filesystems → Community Distributions: **Alpine
  3.24.2 (ppc64le)**, marked Experimental. On a ppc64le root, `apk` works over
  HTTPS, and so do `gcc`, Python and `strace`.
- **What it reports** to software (`AT_HWCAP`, `AT_PLATFORM` `power8`,
  `/proc/cpuinfo`) is in `/AOK/docs/guest-cpus.md`.
- **The whole regression suite passes on it**, on the Mac and on two iPads (see
  Testing).
- **Not there yet:** speed work (no fused pairs or high-level emulation), the
  vector crypto and POWER9 (ISA 3.0) instructions — distributions built for
  POWER9, such as Ubuntu 24.04 and later, stop with `SIGILL` — and the
  saturation flag of the saturating vector instructions.
- Builds 556–561 don't list the download: they could not boot it.

## Files: moving a file out no longer empties its folder

A report from Discord: an image copied to `/root` on an Arch root was
decompressed there, the result was moved out with the Files app, and most of
`/root` was gone. Two faults made that possible, and 562 fixes both.

- **Files makes copies of whole folders**: opening one, or showing it under
  Recently Used in the Move sheet, copies everything in it, large files
  included. When such a copy came back, or when iOS cleared it to free space,
  iSH-AOK deleted the real folder and copied the snapshot in. A stale
  snapshot, or one cut short when the device ran out of space, replaced the
  folder. Now nothing is ever written back over a folder, a clearing writes
  nothing back, and an edit made in another app is saved into the same file,
  in place. If there isn't room for the edit, the old file stays as it was.
- **A deleted file's number went to the next new file.** Files remembers items
  by number for days. When the newest file was deleted, its number was handed
  to the next file or folder — at the next boot, or within seconds in the
  Files extension — so a later action on the old item reached the new one. A
  number is now never reused on a filesystem. In testing, `/run`, which every
  boot recreates, took the number of a file Files was still showing.
- Also: moving a file into a filesystem's top folder made it vanish from the
  guest (it was recorded under a relative name); a filesystem's own folder can
  no longer be deleted, renamed or moved from Files; and an item's location
  comes from the filesystem's database, not from the name its descriptor once
  had.

`/AOK/docs/files-app-integration.md` describes what Files can and can't do now.

## Issues answered

- **Install a Desktop shows its progress and can remove a desktop**
  ([#646](https://github.com/emkey1/ish-AOK/issues/646)). The install page
  has a header with the script's current step, a bar, and a count
  ("Installing 40 of 189 packages"). Swipe left on an installed desktop, or
  touch and hold it, to remove its own packages and what they pulled in that
  nothing else needs. If the removed desktop was the one that starts, the
  choice moves to one still installed.
- **#639's leftovers**: installing Wayfire asks which panel it should have,
  and the display's "not installed" messages point to Install a Desktop….
- **fastfetch, neofetch and btop**
  ([#647](https://github.com/emkey1/ish-AOK/issues/647)) are one `apk add` or
  `apt install` away on an Alpine or Devuan filesystem. Native versions are
  queued.

## Crash fixes

- **Save Image no longer closes the app.** From the Workspace image viewer's
  Share button, Save Image was refused by iOS, which then ended the app: the
  app had no description of why it adds to the photo library. It asks for
  permission now, in all nine languages. Seen once on 561.

## The app

- **Save Anyway, and the automatic save, work with a sandbox running.** Since
  559 a session with a process in a namespace of its own could not be saved,
  even with Save Anyway, so on Arch (whose services run sandboxed) no session
  could be saved at all. Save Anyway and the automatic save now leave such
  processes out, with their process group and children, the way they leave
  out a GPU program. A plain Save still asks first.
- **Filesystems shows each installed root's architecture**: "Devuan6_2
  (i386)". A name is only what the root was called when it was made. A root
  that never recorded its architecture is asked its own programs.
- **A filesystem is deleted entirely or not at all.** A delete that stopped
  part way used to leave a fragment that blocked the next install under that
  name. The root is now set aside first, and anything left is cleared at the
  next launch.
- **VoiceOver** hints for the LLM Chat toolbar and the Workspace applets say
  what each button does, and the About screen's diagnostics buttons are named
  Export and Refresh, in all nine languages (from the bot PRs #649, #650 and
  #651, corrected where they described something the button does not do).
- **Settings**: the Swap Size picker sits under Simulated Swap, where it
  belongs, not under Suspend to Disk.
- **ktop** names ppc64le processes in its ARCH column.

## Kernel and conformance

- **A process-CPU timer counts the process's CPU.** A POSIX timer on
  `CLOCK_PROCESS_CPUTIME_ID` ran on the CPU time of all of iSH-AOK, every
  guest process included, so it fired early whenever anything else was busy:
  a 1 ms timer counted about 950 expiries while its process used about 620 ms.
  It now fires on the process's own CPU, at most one sampling tick late and
  never early.
- **A process's accounting record is written before its parent can reap
  it**, as on Linux. A quick parent could find the record missing.
- **`execve` with a NULL `argv` runs the program with an empty one**, as on
  Linux, instead of failing with `EFAULT`.
- **Native zsh takes long commands.** `rm *` over a few thousand files failed
  with "argument list too long" (a subshell's command was limited to 128 KB),
  and a command of 100,000 words closed the app. Both work now; a single
  program's arguments are still limited as on Linux.

## Testing

The full regression suite on the five release roots — Alpine 3.24 i386,
amd64, arm64 and riscv64, and Devuan 6 arm64 with glibc — on this build:

| root | M5 Mac | M4 iPad |
|---|---|---|
| i386 | 452 passed | 457 passed |
| amd64 | 472 passed | 477 passed |
| arm64 (musl) | 391 passed | 396 passed |
| riscv64 | 383 passed | 386 passed |
| arm64 (Devuan, glibc) | 391 passed | 396 passed |

No failures on either host. On the iPad each root was booted as the running
root, not reached through a chroot. Its riscv64 leg ran on Alpine 3.23.3: that
iPad's 3.24.2 riscv64 root has no ssh server to start the run from. The ppc64le
root passed its suite on the Mac (362), the M4 iPad (366) and the A10X iPad
(366). The e2e suite passed 7 of 7 from a freshly built test filesystem.

The A10X iPad (iPadOS 17.7) also ran Devuan 6 arm64 (395 passed) and riscv64
(387 passed). Each had one failure, `signal_process_wake_one`: a restarted
call there ends 0.5–0.7 s after the moment it should, past the test's 0.5 s
allowance. Its correctness checks all pass, and it passes with the allowance
doubled. 560, built and run on the same iPad, fails the same way, so this
build did not cause it. The M4 passes it.

## Known issues

- **ppc64le is experimental**: see what is not there yet, above.
- **`signal_process_wake_one` is late on the A10X** (see Testing).
- **Sandboxes cannot reach the network** through a veth: there is no
  forwarding or bridge yet.
- **ssh sessions still drop when the device locks**
  ([#631](https://github.com/emkey1/ish-AOK/issues/631)): iOS takes a
  suspended app's connections.
- **A native program's own threads share one task** and are not stopped
  when the program ends.
- **Xfce draws no desktop backdrop**: icons on black.
- **On distribution roots, `who` misses the app terminal's own login.**
