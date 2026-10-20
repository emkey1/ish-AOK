# build 563 musts

Work that must be done, or explicitly decided, before 563 is tagged. Each
entry says what is **established**, what the **next step** is, and how to
**prove** it afterwards.

Started 2026-10-19, after `2b230f849` (build: 562). Supersedes
[docs/build_559_musts.md](build_559_musts.md); 560, 561 and 562 had none. The
detail behind each entry is in [docs/TODO.md](TODO.md)'s queue; this file is
the list and its state.

**DRAFT for the maintainer to edit.** The selection below is a proposal
drawn from the queue on 2026-10-19: what to keep, cut or add is yours.

**Status, 2026-10-19:**

| § | item | state |
|---|---|---|
| 1 | A 64-bit result read as -errno; a call through NULL reported as SI_KERNEL | **DONE** (15a1fb543): both fixed, i386's NULL call too; suite clean on five roots, ppc64 checked |
| 2 | `list_remove` leaves a NULL node that a walk faults on | **DONE, a different fix** (2026-10-19): the audit ruled out re-initing; walks now treat a never-initialised head as empty, which is what crashed |
| 3 | Exec hands a task stale TLB entries | **DONE: it never did** (2026-10-19): the heals were an x87 80-bit load reading 16 bytes for 10, off the end of a host page; gone from Apple hosts since the 10-06 gadget port, fixed for x86_64 hosts now |
| 4 | Native programs share the host C library's static state | **DONE** (2026-10-20): the tz cache was wrong across roots and after a change; an `nm -u` audit found five more kinds of shared state, all per thread now |
| 5 | Shortcuts Run Command: output over 256 KB, non-UTF-8 output, a reap timeout | **DONE** (2026-10-20, de2f52f93): the lost exit status was every root with a real init, not a timeout; the action itself not yet run from Shortcuts |
| 6 | `signal_process_wake_one` late on the A10X | **DONE: the device** (2026-10-20): the test ran 14 scenario processes on 2 CPUs; batched by CPU count it passes 8 of 8 there |
| 7 | Issues and the TODO's own state | open: #646 to close, the issue table stale, 34 done entries in the queue |
| 8 | Decisions due at the start of a release | open: #540, the two unlanded branches |

Order: crash classes first (2, 3), then correctness native programs and
Shortcuts users see (4, 5), then the release gate's one device failure (6).
7 and 8 are small and best done early.

---

## 1. A 64-bit result read as -errno; a call through NULL reported as SI_KERNEL

**Done 2026-10-19 (15a1fb543).**

- *The -errno result.* amd64's result write (which ppc64 shares) took a
  result with the upper 32 bits zero and a low word in
  0xfffff001-0xffffffff for a 32-bit errno: an lseek landing up to 4095
  bytes below 4 GiB returned -1. The 115 handlers that return a dword now
  widen their own errno (`amd64_syscall_result_dword`), and the qword form
  takes its value as given. The split was found by the compiler: each call
  site's argument type checked with `_Generic`, not read by eye.
- *The NULL call.* arm64's and riscv64's interrupt gadgets mark an INT_GPF as
  a reported address, as ppc64's did. i386 had the same defect, which the
  queue entry did not list: its fetch faults shared the `interrupt` gadget
  with PORT_IO and PRIV, real #GPs that must stay SI_KERNEL, so gen.c's
  SEGFAULT now emits a `fetch_fault` gadget on both hosts.
- **Proved:** `lseek_errno_window` (6 failures on amd64 and ppc64 before) and
  `null_call_siginfo` (SI_KERNEL on arm64, riscv64 and i386 before), each
  with controls; MBP5 suite on five roots, 385-474 passes, 0 failures; the
  ppc64 root on MBP5 (`build-ppc563`); the Xcode device build carries the
  gadget. The x86 #GP tests (gpf_siginfo, port_io_gpf, priv_gp,
  null_deref_siginfo, x86_ud_siginfo) are unchanged.
- *Not run:* a device leg.

## 2. `list_remove` leaves a NULL node that a walk faults on

**Done 2026-10-19, and not with the one-line fix the queue proposed.**

- *What crashed.* Not a removed node: the device crash behind this entry was
  `alive_pids_list`, a bare global, walked before the guest booted -- a head
  that was never initialised, whose `next` is NULL. `list_empty()` has always
  called such a head empty; the walk macros faulted on it.
- *The audit.* 27 `list_null()` readers. 17 are heads set up lazily (jit's
  block hashes, the inode hash, net_emul_binds, the abstract-socket hash),
  untouched by `list_remove`. 10 are nodes whose owner asks "has it been
  removed?": the futex waiter learns it was woken that way (kernel/futex.c,
  three sites, one of them lock-free), and fifo close, a fuse request's
  interrupt, kmsg and proc close, inotify's instance count, tmpfs's readdir
  resume and sockrestart's re-add gate decide on it. A re-initing
  `list_remove` makes every one of those read a removed node as still
  linked: a woken futex waiter would sleep on, and sockrestart would never
  re-add a listener.
- *Why not re-init anyway, with a new predicate.* A walk whose cursor is on a
  node another thread removed would then spin on that node for ever with its
  lock held -- the shape of the sockrestart hang (a node linked to itself) --
  where NULL faults at once, at a signature the crash notes already name.
  Linux poisons removed nodes for the same reason.
- *The fix.* util/list.h's four walk macros start at `list_walk_start()`: the
  first node, or the head itself when its `next` is NULL, so a walk over a
  never-initialised head runs no iterations. One `csel` per walk.
  `list_remove` is unchanged, with the reasons above in a comment. The
  file-scope heads without an initializer (inodes_hash, mmap_cache_hash,
  net_emul_binds, abstract_hash, futex_hash) were already covered by a
  constructor or a lazy init; the walk guard covers the heads in structs that
  grep cannot find.
- **Proved:** `meson test list` (util/list-test.c): every walk form over a
  zeroed global and a calloc'd head, a populated control, and the
  removed-node contract the readers rely on. Built against the old header it
  dies of SIGSEGV. MBP5 suite on five roots at 306c895ed: 385-474 passes,
  0 failures; the Xcode device build has it.

## 3. Exec hands a task stale TLB entries

**Done 2026-10-19: exec never left stale TLB entries.** The heals were a
different bug, already fixed on Apple hosts and fixed today for x86_64 ones.

- *Re-measured at head* (4d5fb4f57, MBP5, i386 root, printk on fd 555 with
  the descriptor limit raised -- under ssh it is 256 there, and fd 555 then
  fails to open, so the first "no heals" was the instrument): 40 runs of
  `concurrent_exec_tlb` alone and 20 of four at once, **0 heals**.
- *Control:* 1342669c8, where 36 runs in 40 healed on 2026-10-04, built from a
  scratch clone, on a copy of the same root: **1 run in 40** on MBP5, nine
  unwinds for one cc1 at one host address, 0x10decc000 -- the retry cap, so
  eight flushes did not cure it. Not a race, then.
- *The cause:* cc1's `fld tbyte [ebp-0x64]` (db 6d 9c), operand at
  0xfffe3ff4, page offset 0xff4. The x87 m80 load then went through a C
  helper, `fpu_ldm80(cpu, float80 *f)`, which copied `*f` whole: 16 bytes
  (`ldr x1, [x8]; ldr x2, [x8, #8]` in the control binary). The read-prep
  gadget staged a copy only for an operand whose 10 bytes cross, so at
  offsets 0xff1-0xff6 the helper read 1-6 bytes of the next host page; the
  operand at host 0x10decbff4 makes the second load fault at exactly
  0x10decc000. The x87 gadget port (2026-10-06) took aarch64 hosts off that
  helper, which is why the heals stopped before dc5d53ae6, whose change-count
  fix was real but was never what they were.
- *Still live on x86_64 hosts* (Linux CI, camd), which run every x87 m80 load
  through `fpu_ldm80`. It copies the operand's 10 bytes now.
- **Proved:** `meson test fpu_m80` (emu/fpu-m80-test.c) calls the helpers
  against a host page whose successor is PROT_NONE: before the fix it faults
  at exactly offsets 0xff1-0xff6 (0x3ff1-0x3ff6 with a Mac's 16 KB pages), on
  the Mac with clang and on camd with GCC; after, it passes on both. A guest
  test cannot reach this -- the host page after a guest page's backing is not
  the guest's to choose -- so `x86/x87_page_edge` pins the guest-visible
  contract instead: FLD/FSTP and FBLD/FBSTP m80, FLDENV/FNSTENV and
  FRSTOR/FNSAVE at every offset up to a page end, loads beside PROT_NONE,
  stores leaving the next page alone, with a crossing load that must fault as
  control; 113 checks pass on i386 and amd64. MBP5 suite at 7e485a22a: five
  roots, 385-475 passes, 0 failures; the Xcode device build has it.
- *Corrected:* jit/jit.c's comment that named this crash as the stale-TLB
  race. *Found on the way:* FBLD/FBSTP are SIGILL on an x86_64 host (added to
  the TODO's x86_64-host entry).

## 4. Native programs share the host C library's static state

**Done 2026-10-20.**

- *The time zone* (958e93787). Not the two-TZ case the queue named -- that
  was right, if slow (two programs evicted each other's zone, reparsing under
  a lock all native programs share). The wrong answer was the once-a-second
  shortcut, which was global: for a second after any program looked, every
  program with no TZ took its zone. A native `date` in a chroot whose
  /etc/localtime is BBB printed another chroot's AAA, 150 times in 150, and a
  date straight after /etc/localtime was replaced printed the old zone. Zones
  are cached by identity now, four at once (device, inode and mtime; a TZ
  value with the root it resolves in), and the shortcut holds only for the
  same program, second and TZ. `native_tz_roots` (root): the old code fails
  4 checks, the new passes.
- *The audit.* `nm -u` over every native archive (smallclue, zsh, dash,
  openssh, git2, nextvi, motepad, ktop, wl-present), less everything the
  build defines: 285 names reach the host's libc. Each candidate was checked
  on the host by calling it from two threads. Shared: `inet_ntoa` (one
  buffer; SmallCLUE's traceroute, zsh/tcp), `rand`/`srand` (one stream;
  zsh's `$RANDOM`), the conversion state behind `mbrtowc`/`mbrlen`/`wcrtomb`
  with a NULL state and behind `mbtowc`/`wctomb` (a half-read character was
  finished by another thread's byte), and `signgam`, which zsh/mathfunc's
  gamma reads straight after `lgamma`. Per thread already or harmless:
  `strsignal` (per thread on Darwin), `nl_langinfo` (read-only locale data),
  `erand48`, `regerror`, errno. The queue's other names -- `strerror`,
  `ether_ntoa`, `devname`, `getbsize`, `tmpnam`, `random` -- do not reach the
  host at all.
- *The fix.* All of the shared ones are per thread in kernel/native_libc.c
  (`rand_r` is Darwin's own generator: 100,000 draws agree, so a seeded
  sequence is unchanged), and a new image starts them as a new process
  would (`nlibc_image_start`, beside the FPU reset in kernel/native.c).
  After: 275 names reach the host, none of these; check-native-libc clean.
- **Proved:** `native_libc_state`: one native zsh's `RANDOM=42` and 3000
  draws alone, then four at once, each of which must print it exactly. The
  old build fails all four copies in each of three runs (diverging from the
  first draw); the new passes 6 runs in 6 on arm64 and amd64 roots. The
  conversion state and signgam have no test a program can drive reliably;
  they rest on the two-thread host probes. MBP5 suite at 06e977258 (which
  carries the tz fix too): five roots, 387-477 passes, 0 failures, both new
  tests passing on each; the Xcode device build and GCC on camd build it.

## 5. Shortcuts Run Command: output over 256 KB, non-UTF-8 output, a reap timeout

**Done 2026-10-20 (de2f52f93).**

- *Over 256 KB.* The capture killed the command at the cap, so the action
  failed with "killed by signal 9". It now reads and drops the rest to EOF:
  the command runs to its end with its real status, and the action returns
  the first 256 KB and a line `[iSH-AOK: output truncated at 256 KB]`.
- *Not UTF-8.* One bad byte returned the whole output as empty text. Each
  ill-formed sequence is now a U+FFFD (maximal subparts, as Swift's
  `String(decoding:)`; Foundation's lossy detection dropped everything after
  the first bad byte), and a truncated output first loses a split character.
- *The exit status.* Bigger than "a reap that gives up": the command is a
  child of the guest's pid 1, and on any root with a real init, init usually
  reaps it first. The capture's wait then found nothing, and "Fail on
  Non-Zero Exit" passed failing commands. Exit-status watches
  (kernel/exit.c): the capture marks its child before it runs, and whoever
  reaps it records the status, in a slot under a generation. Still none
  (no reap within 3 s of SIGKILL) is `status_unknown`, which fails the
  action when Fail on Non-Zero Exit is on. The LLM tool and the desktop
  installer, which print their status into the output for the same reason,
  get the status this way too.
- **Proved:** main.c's capture harness, which now takes a cap and can run
  after pid 1 starts. 300 KB then `exit 3`: before, signal 9; after, exit 3,
  262144 bytes, and the command's last step ran. Under a pid 1 that reaps
  every child at once, `exit 7`: without the watch 5 runs in 10 lost the
  status, with it 10 in 10 (and a SIGTERM death reads 15). The decoder
  against ten byte strings, five compared with Swift's. The Xcode device
  build has it; MBP5 suite at de2f52f93: five roots, 387-477 passes, 0
  failures. *Not run:* the action from the
  Shortcuts app, which needs a signed device build.
- *Left* (TODO): (3) root when no uid-1000 account exists, (4) su's error
  text, (6) a cached boot failure's text, and `_EAGAIN` (waiting for the
  session choice) reported as "could not boot". All wording, in nine
  languages.

## 6. `signal_process_wake_one` late on the A10X

**Done 2026-10-20: the device, and the test's own load on it.**

- *What was late.* 562's bip leg: "lost the race, read (round 3):
  nanosleep = 0 at 2506ms, want it to run to ~2000ms" -- a waiter nothing
  signalled, a plain 2 s sleep ending half a second late. Not a lost wake.
- *Where the time went* (this build on bip, verbose, three runs). The wake
  itself is prompt: in each scenario run on its own, the first of its 13
  waiters ends 1-2 ms after 2000. Lateness grows with how many threads want
  a CPU at once: one process alone, at most ~75 ms; the racing scenarios,
  which ran side by side -- 14 processes, ~180 waiters released together, on
  a guest with 2 CPUs (`nproc` on bip) -- up to 700 ms, in every kind of
  wait (nanosleep, ppoll, futex, sigsuspend).
- *A/B on bip:* the test as it was failed 4 runs in 5 (and 3 in 3 verbose);
  the same scenarios one at a time passed 5 of 5.
- *The fix* (tests/manual/signal_process_wake_one.c): the racing scenarios
  run side by side as before, but no more at once than there are online
  CPUs. Same scenarios, same checks, same slack. bip 8 of 8 (20 s a run);
  the M4 (6 CPUs) 5 of 5 before and after (11 s, now 13 s); the Mac CLI
  (4) passes (17 s). No kernel change.
- *Ruled out on the way:* a `taskpolicy -b` run on MBP5 made
  `clock_nanosleep` late by up to 140 ms while `nanosleep` stayed on time;
  that is background QoS coalescing timers, which no device run has, not
  the A10X's behaviour.

## 7. Issues and the TODO's own state

- **#646** ("Install a Desktop" feedback): done for 562 and answered
  2026-10-16, "close when 562 ships". 562 shipped on 2026-10-19.
- **The TODO's issue table is stale.** It lists #577, #616, #620, #625, #628,
  #631, #639 and #640 as open; GitHub has them closed. #648 (an apt `W:`
  line on Devuan, answered 2026-10-17, waiting on the reporter) is not in it.
  Open on GitHub today: #540, #646, #647, #648.
- **The queue holds 34 entries whose headline says Done, Fixed or Settled.** The queue's own rule
  is that a landed item's bullet is deleted, the commit being the record.
  Delete them, after checking that none carries an open "Not done" tail that
  should become its own bullet.

Closing an issue and commenting on one are the maintainer's to approve.

## 8. Decisions due at the start of a release

- **#540, external display.** Built, reverted for 546, parked; the note on it
  says to raise it early in a release, not late. Decide whether 563 is the
  release for it. The work is on `worktree-external-display-540` (6156597ee).
- **`claude/jovial-dhawan-96001a`** (6c0568e83): the #523 "Go heap-growth
  stall" diagnosis, which the TODO's #523 entry does not contain. Land the
  text or drop the branch.

---

## Wanted if there is room

Not musts. Each is in docs/TODO.md's queue.

- **riscv64 against real silicon.** The RVV board ordered 2026-10-06 should be
  usable about now: run the riscv64 tests natively on it, and turn the
  model-based expectations into hardware known-answer tests (the
  tools/gen-x87-test.py pattern).
- **559 §5's transition steps**, handed to 560 and not taken since: the chain
  budget in a host register, the last block stored only on the way out, and
  the cost of `dmb ishld` against an acquire load. Each touches the lost-poke
  history (c5692489) and wants its own A/B on the M4 and the A10X.
- **ppc64 speed**: no fused pairs, HLE or specialised gadgets yet; measure 7z
  and the Python microbench against riscv64 first.
- **A native-dash background job holds its caller's ssh session open** (bip,
  2026-10-02): suspect descriptors past 0-2 that the fork-handoff child
  inherits.
- **Distro roots lose the app terminal's utmp record** to the guest's early
  boot, so `who` misses the app's own login.
- **Waiting, what is left**: the slow path of a wait that really sleeps, the
  futex wake path (the condvar round trip is 2x native on the Mac), and the
  waits not yet given the spin (poll/epoll_wait, accept, wait4).
- **GH #647**: fastfetch/btop as native programs; nothing promised.

## Done beside the musts

- **kernel/native.c builds without warnings** (4d5fb4f57). Ten in every build
  since e459ac19f: nine positional entries in `native_programs[]` that left
  out `setuid_root` (every entry now names `.name` and `.main`, the lesson of
  the native zsh setuid accident), and the dead seam-probe placeholder that
  stood in for SmallCLUE, with its four applets. Suite clean on five roots.
