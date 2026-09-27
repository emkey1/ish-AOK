# Release Notes Since `builds/iSH-AOK_553`

148 commits. Two large things and a long tail: the guest can page memory out to
a swap file now, and the amd64 engine finished the job it started in 552 — every
instruction the JIT meets it compiles, with the interpreter removed from the
path entirely.

## Highlights

**Swap.** iOS gives an app a memory ceiling and kills it without warning when it
is crossed, so a guest that allocates too much has never received an `ENOMEM` —
the whole app disappears, terminals and all. There is now a pager: cold guest
pages are written to a file inside the app's own storage and faulted back on
demand, so a large, mostly idle working set can exist without spending real
memory on it.

It is **off by default and has to be turned on deliberately**, in Settings, with
a size you choose. Paging spends flash writes on your device, and that is not a
cost to impose on someone who did not ask for it.

The guest sees it the way it sees swap anywhere: `free`, `/proc/meminfo`,
`/proc/swaps`, `vmstat`'s `si`/`so` columns, and per-process residency that `top`
and `ps` report honestly. `swapon` and `swapoff` work on `/dev/aokswap0`, which
answers the block ioctls a real swap device answers. `/proc/ish/swap` shows what
the pager is actually doing, and says *why* if swap was refused rather than just
reporting `off`.

Three things in it are worth naming because they are what makes a pager fit to
run on somebody's phone rather than a server:

- **A write budget.** The pager stops evicting once it has written a fixed
  amount within 24 hours, so a runaway guest cannot quietly grind through your
  device's write endurance.
- **A thrash guard.** If pages come straight back after eviction, the background
  sweep backs off instead of churning.
- **A suspension gate**, and a release check that stops paging out when the host
  is not actually taking the pages back — measured, not assumed.

Eviction is a second-chance clock, so it takes cold pages rather than convenient
ones, and a frame's slot belongs to the frame and comes back when the frame does.

**The amd64 engine no longer falls back to an interpreter, at all.** In 552 the
gadget JIT handled the common path and handed anything awkward to the amd64
interpreter. This cycle closed that: the `0F 3A` group (98.7% of all block
fallbacks on its own), AVX, x87 and `FWAIT`, the locked read-modify-writes, and
finally the last two opcodes that de-JITted a whole block. A full guest
regression suite with the compile cache disabled — so gcc, and `as`, really run
for all ~200 test programs — now reports **zero interpreter fallbacks**, and the
interpreter's entry points have been deleted.

Two consequences beyond speed. `as` used to be routed to the interpreter by name
to contain crashes that were never root-caused; that bypass is gone, shown
unnecessary by a byte-identical differential on a nontrivial translation unit.
And amd64 single-step now runs on the JIT, so `PTRACE_SINGLESTEP` no longer needs
an interpreter that is not there.

**94 AVX conformance defects, fixed against real hardware.** A differential
against an x86_64 machine found them in code that both the amd64 and i386 guests
share, so both get the repair. Eight of them turned out to have a single root
cause worth stating: an invalid floating-point operation must produce the
*negative* indefinite QNaN, and AOK was producing the positive one. The x87 work
alongside it added `FPREM1`, made `FPREM` exact, and fixed the same NaN class.

**`binfmt_misc` is implemented.** The directory used to exist, accept writes and
discard them — which is worse than not having it, because `update-binfmts` and
`systemd-binfmt` believe the success they are handed. It now registers rules that
actually affect `execve`: magic or extension matching, the `P` flag that
preserves `argv[0]`, enable/disable/remove, and the delimiter convention where
the first character of the line is the separator.

## Fixes worth naming

**The memory guard refused a shell a megabyte, and that bricks the guest.** The
jetsam headroom guard exists so a runaway guest gets clean `ENOMEM`s instead of
iOS killing the app — but it ignored *how much* was being asked for. Under
pressure, a process denied 1 MB cannot start a process, cannot `exec`, and cannot
run the command that would fix it; every command after it failed until the app
restarted. The guard is size-aware now, and small growth is always allowed:
anonymous growth is a lazy reservation that costs nothing until the pages are
touched, and touching them is policed separately by the fault-path throttle this
cycle added.

**A host wait with no bound never recovered from a lost wake.** On Darwin the
`pthread_kill` wake-poke can be swallowed permanently, and a thread parked in an
unbounded wait then never woke. Every host wait is capped so it rechecks, and the
cap is not visible to the guest as a timeout.

**gdb could not tell its own breakpoints from stray signals on the x86 guests**
(GH #503). Every ptrace primitive was correct; what was wrong was one number.
Linux reports an `int3` as `SIGTRAP` with `si_code` `SI_KERNEL` and no address,
and gdb uses exactly that to decide a stop is its own software breakpoint — which
is what makes it rewind the PC and restore the clobbered instruction byte. AOK
sent `TRAP_BRKPT`, so gdb called the stop a random signal and resumed into the
middle of the instruction it had overwritten. `break; run; next` now works.
Architecture-scoped: arm64's `BRK` and riscv64's `EBREAK` correctly keep
`TRAP_BRKPT` and the faulting PC.

**riscv64 `PTRACE_SINGLESTEP` ran the tracee to completion and reported
success.** The riscv64 dispatch never looked at the single-step flag, so a step
was a continue — and returned 0 while doing it. Every debugger stepping operation
on that guest was broken with nothing anywhere saying so.

**A listening socket has to be rebuilt after iOS suspends us.** iOS kills the
listener silently; `sshd` stayed in `ps` with the socket at `TCP_CLOSE`, so it
looked alive and answered nothing. Sockets are now recorded when the app is
backgrounded rather than when suspension looks near, which is too late.

**A 64-bit guest's timed wait lost its nanoseconds.** `futex` with a timeout had
been rounding away the sub-second part on every 64-bit guest for two months. The
test that should have caught it asserted `ETIMEDOUT` rather than elapsed time.

**`splice` deadlocked against its own pipe** when the caller asked to drain its
full count — which is what `cat` on a pipe does.

**The Diagnostics pane.** It snapped to the top on every breadcrumb, had no Share
button in Workspace mode, and refreshed itself while open, destroying any
selection you were making. It is a snapshot now: it refreshes when you press
refresh.

**Other guest-visible repairs:** `/proc/meminfo` describes the guest rather than
the phone, and `/proc/swaps` names a real device instead of contradicting
`meminfo`; `/proc/<pid>/maps` stops merging across an unmapped hole; the
`termios2` ioctls no longer skip the background-process check and the `IXON`
release; a `#!` interpreter may itself be a `#!` script, four deep as on Linux; a
native program named on a `#!` line is treated as one; a zombie handed to a new
parent is announced to it; a subreaper's children are not themselves subreapers;
a process's CPU total no longer runs backwards across a thread's birth and death;
and a long `REP` is interruptible on every host and both engines.

**Native bash:** a subshell entered with a nonzero `$?` no longer costs a second
shell, a null command's subshell no longer re-launches itself forever, a
re-launched subshell no longer fires `DEBUG` on its own state script, and a
subshell no longer publishes its environment in `ps`.

## Testing

Eighteen new tests, each written against a measured divergence rather than a
theory. The ones worth naming:

- `swap_roundtrip` — 64 MB of a position-dependent pattern, forced out to swap
  and checked byte-for-byte on the way back. It refuses to pass unless something
  was actually evicted, because the read-back would otherwise sail through
  against a build where the pager never ran. Verified discriminating: one
  flipped byte in 64 MB is caught with its offset.

- `ptrace_trap_siginfo` — the per-architecture `si_code` and `si_addr` for the
  breakpoint instruction and for a single-step, plus `/proc/<pid>/mem`'s `EIO`
  for an unmapped address. Passes unmodified against real Linux, `-m32` and
  `-m64`.
- `riscv64/riscv64_singlestep` — the riscv64 counterpart of the arm64 test. The
  step *count* is the assertion: registers alone cannot distinguish a real step
  from a resume that stopped somewhere plausible.
- `x86/amd64_singlestep` — nothing covered amd64 single-step at all, so a
  regression there would have been silent.
- `wake_poke_lost` and `mem_guard_small_growth` — the two release-blocking
  memory bugs above.
- `jit_writer_starvation`, `futex_timeout_duration` (which asserts elapsed time,
  not `ETIMEDOUT`), `rusage_monotonic`, `reparent_zombie_notify`,
  `subreaper_not_inherited`, `fork_tgroup_reset`, `exec_shebang_interpreter`,
  `madvise_lazy_reservation`, `fs_remove_enoent_order`, `x86/fpu_state_span` and
  `x86/rep_interruptible`.
- An **unprivileged gate leg**, because every other leg runs as uid 0, where a
  parent directory is always writable and an ownership check always passes. It
  found two harness bugs immediately.

The release gate itself grew a **sixth leg**: `devuan-amd64`, glibc on the amd64
engine. One glibc leg on one architecture was half a fix — libc choice interacts
with the execution engine, each guest has its own JIT frontend, and
`jit_writer_starvation` was exactly that shape: reachable only from glibc on
amd64, and green on all five other legs.

## Also in this build

- `ktop` is dispatched natively, from the same source, and its meter text scales
  at a megabyte rather than at six digits.
- The app starts in the session shell, and Devuan's console has a way in.
- The swap and `binfmt_misc` documentation: chapter 13 of the book rewritten
  around the shape the memory defence actually has, chapter 15's "two formats"
  now three, and new `swap.md` and `binfmt-misc.md` guides shipping to
  `/AOK/docs`.
- Accessibility: the floating display-menu pip now says what tapping it does
  (from PR #582).

## Verified

The full guest suite on every root, counted by the gate's own scorer:

| leg | pass | fail |
|---|---|---|
| alpine-arm64 (musl) | 196 | 0 |
| alpine-i386 (musl) | 202 | 0 |
| alpine-amd64 (musl) | 204 | 0 |
| alpine-riscv64 (musl) | 191 | 0 |
| devuan-amd64 (glibc, amd64 engine) | 204 | 0 |
| M4 iPad Pro (device, glibc arm64) | 199 | 0 |
| A9 iPad (device, glibc arm64) | 197 | **1** |

The A9 iPad's one failure is `signal_child_burst`, and it is **not resolved**.
It reported 2 stuck trials of 25, which is exactly the test's failure threshold
— not the ~50% a build with the bug it hunts for shows, and not the 0% the M4
gives three runs in a row. There is a specific reason to suspect the test: each
trial spawns 24 backgrounded subshells that sleep a second and waits for them
under what was a fixed `alarm(10)`, the only watchdog in the suite that ignored
`ISH_TEST_WATCHDOG_SCALE`, so on the slowest hardware a slow trial and a wedged
one are indistinguishable to it. That watchdog is now scaled like every other
one, with the default unchanged. The deciding measurement — time a trial, then
re-run with a 60-second watchdog, since a genuinely wedged shell never finishes
however long you wait — was not taken, because the device became unavailable.
Recorded rather than waved through.

Worth noting for calibration: this is the first release in which the A9 iPad ran
the *full* suite. 548, the last release to name it, ran "the curated set" there.

Also run: the swap round trip on all five roots with swap enabled, and a
differential of every claim in the new `binfmt_misc` documentation against the
running implementation.

## Known gaps

- **`signal_child_burst` on the A9 iPad**, above. Unresolved.
- **`wayland_scm_shm`** failed once on an amd64 leg and has not reproduced in
  four attempts, one idle and three under load, and passed on all eight legs of
  this release's gate. Cause not established.
- **The Files extension does not work on macOS**, and cannot as written. The
  recurring `__FILEPROVIDER_BAD_EXTENSION__` crash in Apple's reports is
  entirely a "Designed for iPad" AOK on Apple Silicon — every one of the twenty
  logs is macOS 27, none is iOS or iPadOS. `FileProviderExtension` subclasses
  `NSFileProviderExtension`, which the SDK marks
  `API_UNAVAILABLE(macos, macCatalyst)`; macOS requires the replicated
  extension. Not a regression, and no iOS user is affected, but it is a real
  limitation and the fix is a protocol port rather than a superclass swap.
- **`swap_roundtrip` skips on an installed app.** The forced-eviction control it
  uses is gated to the CLI and Xcode launch paths on purpose, so the pager's
  round trip is verified there and not from a device build. On a device the
  pager is exercised only by real memory pressure.
- **The clock does not age without pressure.** kswapd's loop returns before
  sweeping when there is memory to spare, so nothing ages and nothing is
  reclaimed until the host is actually short — measured on an idle M4 iPad Pro
  with swap on: 4060 passes, 0 bytes reclaimed. That is the intended design, but
  it means the first pass under pressure has no aged candidates to take, which
  is why bounded reclaim retries at an age bar of zero.
- **Swap does not evict pages shared by a forked family.** Only memory reachable
  from a single address space is paged out today, so a fork-heavy workload
  benefits less than the totals suggest.
- **Swap does not survive a guest reboot.** The area is recreated empty, and
  `swapoff` tears it down rather than parking it.
