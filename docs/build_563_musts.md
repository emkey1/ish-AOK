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
| 2 | `list_remove` leaves a NULL node that a walk faults on | open: audit `list_null()` readers, then pick the class fix |
| 3 | Exec hands a task stale TLB entries | open: healed by a retry, cause not found |
| 4 | Native programs share the host C library's static state | open: basename and four others done in 559; ten more names and the tz cache left |
| 5 | Shortcuts Run Command: output over 256 KB, non-UTF-8 output, a reap timeout | open: fixes known to the line; proof needs a signed device build |
| 6 | `signal_process_wake_one` late on the A10X | open: decide whether it is the device or a wake path |
| 7 | Issues and the TODO's own state | open: #646 to close, the issue table stale, 27 done entries in the queue |
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

**Why.** A crash class with a device report behind it. `list_remove`
(util/list.h) sets a node's `next` and `prev` to NULL, `list_init` makes it
point at itself, and `list_for_each_entry` stops only on reaching the head,
with no NULL check: a removed node still reachable from a list faults the
walk at `0xfffffffffffffff8`.

**Established** (TODO, *Diagnosed, not fixed*): 94 bare `list_remove` calls
against 14 `list_remove_safe`; only the two `alive_pids_list` sites, the one
with the crash report, re-init after removing. The one-line class fix
(re-init instead of NULL) changes what `list_null()` means at every call
site, and `list_empty()` and `list_null()` disagree about a NULL node.

**Next step.** Audit every `list_null()` reader, then choose between the
one-line change and converting the remaining bare removes to a re-initing
form.

**Prove.** The audit's list in the commit; the full suite on five roots; a
test that walks a list after a remove, if one can be made to reach the
state without a race.

## 3. Exec hands a task stale TLB entries

**Why.** `concurrent_exec_tlb` (8-way fork+exec of cc1, 32 rounds) takes about
one unmapped guest-execution fault per run on the i386 CLI (36 in 40 runs,
2026-10-04). `jit_unmapped_guest_fault`'s flush-and-retry heals each one, and
on the CLI too since 1342669c8, but a heal is not a cure: something gives a
running task TLB entries for pages its new mm does not have.

**Established:** `tlb_refresh` skips the flush when `tlb->mmu` and its change
count match -- the #469 shape, a new mm at the old one's address with a
colliding count. On 2026-10-09 48 runs gave no unwinding heal, so the rate
has moved since 10-04, and one cause was closed then (TODO, *Stale TLB after
exec*).

**Next step.** Re-measure the rate at head. If it still happens, log the mmu
pointer and change count at each unwind and whether the task had just
exec'd.

**Prove.** Zero heals over the same 40 runs, with the printk counted, and
the cause named in the commit.

## 4. Native programs share the host C library's static state

**Why.** Native programs are threads of one host process, so a libc function
that keeps its answer in one static buffer is shared between them. 559
proved it with basename (an applet starting beside another ran under the
other's name) and made basename, dirname, strtok, gmtime and asctime per
thread.

**Established** (TODO queue, *what the basename fix left*): not done are
`inet_ntoa` (SmallCLUE calls it twice), `strsignal`, `strerror` for an
unknown number, `ether_ntoa`, `devname`, `getbsize`, `nl_langinfo`,
`tmpnam`, `rand`/`random` (one stream for every program), and
kernel/native_tz.c's one-zone cache (`ntz_zone`, `ntz_key`), which two
programs with different TZ values overwrite.

**Next step.** `nm -u` each native archive (libsmallclue, libzsh, libdash,
libopenssh, libgit2, libnextvi, libmotepad, libktop) for imports the shim
does not route; check each against Darwin's man page; route the unsafe ones
per thread. Fix the tz cache first: it is the one with a visible wrong
answer.

**Prove.** A `native_applet_race`-style test for the tz cache (two programs,
two zones, many rounds); the `nm -u` list in the commit, with each name's
verdict.

## 5. Shortcuts Run Command: output over 256 KB, non-UTF-8 output, a reap timeout

**Why.** Defects in a shipped feature, found on 2026-10-02 while documenting
it and not fixed since. Three of the seven matter to a user (TODO queue,
*Shortcuts Run Command: seven defects*):

- Output over 256 KB fails the action with "killed by signal 9" instead of
  returning it: `outcome.truncated` (GuestCommandRunner.m) is set and read
  nowhere. Check it first and return the output with a truncation note.
- Output that is not valid UTF-8 comes back as empty text, and a 256 KB cut
  through a multibyte character produces exactly that. Decode lossily.
- A reap that gives up after 3 s leaves exited=0 and termSignal=0
  (kernel/init.c), which counts as success even with Fail on Non-Zero Exit
  on.

The other four (root when no uid-1000 account exists, su's error text, a
failed headless boot cached until relaunch, resume without asking) are
wording or documented behaviour: fix the text where it is wrong.

**Prove.** Run from the Shortcuts app on a signed device build (an unsigned
simulator cannot resolve the intent's metadata): `seq 1 200000` (over
256 KB), `head -c 300000 /dev/urandom`, and a command that outlives the reap.

## 6. `signal_process_wake_one` late on the A10X

**Why.** The one failure in 562's device legs. On bip it fails 4 runs in 5
alone and once in each full leg: a restarted call ends 2506-2703 ms after the
start, against 2000 + 500 ms of slack. Every correctness check holds and it
passes with `ISH_TEST_WATCHDOG_SCALE=2`. The 560 tag fails the same way, so
it predates 562; the M4 passes.

**Next step.** Decide which it is: time the wake path on bip (thirteen parked
threads taking 0.5-0.7 s to run again), and compare with the same test's
timing on the SE 2 and the iPhone 12. If it is the device, the test's slack
should scale with the host, as `ISH_TEST_WATCHDOG_SCALE` already does for
watchdogs; if it is a wake path, fix that.

**Prove.** bip passes 5 of 5 alone and in a full leg, with whichever change
was made named.

## 7. Issues and the TODO's own state

- **#646** ("Install a Desktop" feedback): done for 562 and answered
  2026-10-16, "close when 562 ships". 562 shipped on 2026-10-19.
- **The TODO's issue table is stale.** It lists #577, #616, #620, #625, #628,
  #631, #639 and #640 as open; GitHub has them closed. #648 (an apt `W:`
  line on Devuan, answered 2026-10-17, waiting on the reporter) is not in it.
  Open on GitHub today: #540, #646, #647, #648.
- **The queue holds 27 entries marked Done or Settled.** The queue's own rule
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

(Empty.)
