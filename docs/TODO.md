# iSH-AOK TODO

Open work: bugs that are diagnosed but not fixed, reported issues, and features
deferred on purpose. Each entry says what is already **established**, so nobody
re-derives it, and what the **next step** actually is.

Started 2026-08-19, after the 549 release run.

---

## Diagnosed, not fixed

### #523: yay's reported failure does not reproduce; an http2 flake does

Reproduced the environment on 2026-08-20 -- Arch Linux ARM aarch64, yay v13.0.1
built from AUR under emulation -- and ran `yay -S pandoc-bin` four times. **The
reported `context: signal: terminated` never appeared.** All four got through
the AUR fetch and downloaded sources.

Getting there needed five things fixed first, only one of them AOK's:

1. Landlock, 2. a dangling /etc/resolv.conf, 3. an empty keyring -- all three
   now shipped as `/AOK/fixes/arch`.
4. **`/dev/fd` was missing**, so bash process substitution was ENOENT and
   makepkg died at "Retrieving sources". Ours, and fixed (`59827f5ce`).
5. The minirootfs strips headers from 137 packages, so anything that compiles
   needs `pacman -S glibc linux-api-headers` first.

**What DOES reproduce, about one run in four:**

    request failed: Get "https://aur.archlinux.org/rpc?...":
        http2: client conn could not be established

yay recovers -- it falls back to git and carries on -- so it is not fatal, and
it is not what was reported. But it is a real intermittent failure of Go's
HTTP/2 client against a host that curl reaches every time, on both HTTP/2 and
HTTP/1.1. Ruled out already: not git (3 of 3 clones standalone), not TLS
generally (pacman syncs fine), not concurrency (9 simultaneous TLS operations
all succeeded).

**Measured 2026-08-20, and it is a latency tail, not a protocol bug.** TLS
handshake time to the same host, 15 samples each:

    host  (macOS)   min 0.09  median 0.21  max  1.15
    guest (AOK)     min 0.21  median 0.39  max 15.32

The median is about twice the host's -- unremarkable for emulation. The TAIL is
13x worse, and 15.3s is past Go's default TLSHandshakeTimeout of 10s, which is
exactly how "http2: client conn could not be established" arises. The same
stall explains the OpenSSL "unexpected eof while reading" seen from git.

**The CPU-count lie is not the cause**, though it was the obvious suspect:
AOK reports 4 of the host's 10 logical CPUs, and Go sizes GOMAXPROCS from it.
A Go HTTP/2 probe run at GOMAXPROCS 1, 4 and 8 (20 requests each) failed once
in 60, at GOMAXPROCS=8, with a handshake timeout -- the same tail, not a
scheduling effect.

**Next step.** Find what stalls a socket for seconds when the median is
sub-second. Nothing in the handshake is compute-heavy, so this is a wait that
is not being woken promptly rather than work that is slow -- which puts it in
the same neighbourhood as the poll/quiesce machinery. `curl` on its own shows
the tail too, so it reproduces without Go, without yay and without the AUR:
any repeated HTTPS handshake will do.

### `pread_stack_thread_race` SIGSEGVs on i386 -- FIXED 2026-08-22

**A dangling pointer to a dead stack frame, written one byte at a time by
another thread.** `kernel/futex.c:412` publishes the address of one of its own
STACK locals in `current->waiting_interrupt_flag` (`&wait.interrupted`) just
before calling `wait_for`, and `wait_for_internal` (util/sync.c:180) is the only
place that pointer is ever cleared. `wait_for`'s FIRST early return --
`consume_wait_interrupted() || is_signal_pending(lock)` -- skips
`wait_for_internal` entirely. So `futex_wait_masked` returns `_EINTR`, its frame
dies, and `task->waiting_interrupt_flag` goes on pointing into it. From then on
`wake_waiting_task` (kernel/signal.c:341) does

    __atomic_store_n(waiting_interrupt_flag, true, __ATOMIC_RELEASE);

from *another* thread, into dead stack, for the rest of that task's life.

**Why the crash value was always identical.** `interrupted` sits at offset 4 of
its eight-byte word, so the stale store always writes byte 4 of some aligned
word -- every dangling address measured ends in 4 mod 8. When the dead frame has
been reused by libpthread's `pthread_cond_wait` cleanup record, that word is the
record's `__next`, and setting byte 4 of it makes exactly **0x100000000**. The
thread dies later inside `pthread_exit` following it, on a frame with nothing of
ours on it. Not a coincidence, not a stray value: one byte at a fixed
sub-offset.

Fixed by clearing the pointer on that early return. `ISH_WAITFLAG_LEAK=1`
restores the old behaviour so the fix can be A/B'd on one binary, and
`ISH_WAITFLAG_TRACE=1` proves the knob actually toggles it -- an A/B whose two
arms behave identically looks exactly like an A/B whose subject does not matter.

**Proven by counting the bug, not by waiting for it.** The SIGSEGV is the
~1.3%-per-run tail of this: only a small fraction of stale stores happen to
land on a live cleanup record, so proving a fix that way needs hundreds of runs
per arm and a lot of patience with noise. `task->waiting_interrupt_flag` is only
ever set to `&wait.interrupted`, so the containing `struct futex_wait` is
recoverable and its magic says whether that object is still live --
`futex_wait_flag_is_live()`, checked at the store site in `wake_waiting_task`,
counts the defect directly:

    ISH_WAITFLAG_LEAK=1 (bug restored)   12 runs, 17 stale stores
    fix active                           12 runs,  0

Twelve runs an arm, perfectly separated. tier0 is green on both arches with the
fix in (113 passed, 0 failed, 4 environmental skips).

**What had to be corrected on the way, because each one cost time:**

- **"A nonzero `__cleanup_stack` is corruption by definition" is false.** Darwin's
  `pthread_cond_wait` pushes a cleanup record itself, so every thread parked in
  `wait_for`, `mem_quiesce_park` or `wrlock_acquire_locked` has one. A watcher
  that reported any nonzero value fired on **12 runs out of 12**, every time on
  a thread sitting innocently in `mem_quiesce_park`. The real invariant is NULL,
  or a pointer into this thread's own 4 MB stack.
- **`printk` goes to fd 555**, which an ordinary CLI run never opens -- and zsh
  cannot even parse `555>file` to attach it. Two separate "nothing fired"
  results in this hunt were a disconnected channel, not a zero, and one of them
  would have closed the case an hour earlier. Diagnostics in a CLI debugging run
  belong on stderr.
- **Nine `sig`-changed reports** that looked like the corruption enriching on
  exiting threads were the watcher racing a thread's own teardown. The tell was
  in the report itself: the victim was missing from its own registered-threads
  list, and the value differed every time, where all twelve genuine catches had
  it present with exactly 0x100000000.
- **Ten hardware-watchpoint traps** named `notify()` in the futex wake path. The
  handler now re-reads the owner's `__cleanup_stack` at trap time and all ten
  came back *record already popped* -- a stale watch address, legitimate stores.
- **A census of cross-thread stack writes** initially listed three JIT gadgets,
  which reads as a spectacular finding and was the masked watch window
  overshooting above the struct into a guest page. Filter by address, not by
  window geometry.
- **The instrument itself shipped a crash.** The counter that killed the
  leaked-record theory read a `__thread` variable from inside
  `sigusr1_handler`, and the first read of an uninstantiated one goes through
  dyld's `_tlv_get_addr`, which mallocs. A signal landing on a thread already
  inside malloc -- `pthread_exit` freeing its TSD -- then re-enters the malloc
  lock and aborts in `_os_unfair_lock_recursive_abort`. It looked safe because
  the handler's `thread_locals_ready()` guard is right there, but that guard
  only covers the variables `signal_thread_locals_init()` instantiates by hand.
  Any NEW `__thread` variable in that handler is a fresh landmine. Found from a
  stray `.ips` in the A/B lane, not from the test.
- **The leaked-cleanup-record theory** (a `siglongjmp` abandoning a
  `pthread_cond_wait` frame) is dead: a counter on `sigusr1_handler`'s
  `siglongjmp` reads zero, because this test never reaches a
  `sigunwind_start()` window at all.

**Measurements worth keeping.** The corruption is transient -- it ping-pongs
between the head and a record's `__next` as records are pushed and popped, and
only kills the thread if it reaches `pthread_exit` while the value is in the
head, which is why the crash is a flake rather than every run. Rate without
instrumentation was **3 in 200** on this machine, not the 1-in-8 the original
entry recorded. `ISH_MEM_QUARANTINE` is complete (emu/memory.c:823 is the only
`munmap` of guest page data), so its earlier refutation stands -- though note it
cannot see a kernel copyout, which returns EFAULT to a PROT_NONE page in
silence. libpthread legitimately writes OTHER threads' `struct _pthread` at
+16/+24 (the global thread list), measured at 200 runs out of 200; `+8` carries
none of that traffic.

**Two guards in kernel/futex.c that came out of this.** `futex_wait_is_live()`:
`struct futex_wait` is a stack local published on a shared queue, so every wake
path runs `pthread_cond_broadcast` over another task thread's stack -- a magic
set when it is built and cleared when it leaves the queue means a stale entry is
dequeued and reported rather than notified. It has never fired, which is the
answer that theory deserved. `ISH_FUTEX_HEAP_WAIT=1|2` is the A/B harness built
for it, with a decoy arm; the theory did not survive, but the harness is the
shape to copy.

**Knobs, all in kernel/task.c:**

- `ISH_PTHREAD_CANARY=1` -- register every task thread's `self` and watch
  `__cleanup_stack` and the cleanup chain's `__next` links, plus self-checks on
  the owning thread. On a violation it suspends every other thread with the mach
  APIs and dumps registers and a frame-pointer backtrace for each, then exits 66.
  The report prints the image slide, so `atos -o build/ish -l <load address>`
  symbolises it.
- `ISH_PTHREAD_WATCH=1|record|stack|census` -- arm64 hardware watchpoints via
  `thread_set_state(ARM_DEBUG_STATE64)`, which works in-process on this host.
  `record` watches the live cleanup record, `census` lists every instruction that
  stores into another task thread's stack.
- `ISH_PTHREAD_WATCH_SELFTEST=1` -- positive control. A watchpoint that was never
  applied looks exactly like one that was never hit. Run it before believing a
  quiet watch run; the same discipline is what caught the fd-555 problem above.

**Do not write a tier0 failure of this test off as "the known flake" any
more.** That reflex is what kept this one invisible.

<!-- superseded detail below retained deliberately -->
### Original entry: `pread_stack_thread_race` SIGSEGVs on i386, about 1 run in 8

Split out of the hang entry above once the hang was fixed and stopped hiding
it. **Pre-existing, and NOT caused by the jetsam fix**: measured on a binary
built before that change (1 in 8), and it survives the fix at about the same
rate.

The run dies with rc=139 and produces no output at all -- not even the test's
first line -- so it is dying early, and the empty output is itself a clue worth
starting from. amd64 has not shown it in 12 runs; only i386 so far.

Next step is the same one that settled the hang: catch one and look, rather
than reason from the symptom. `scratchpad/catch_hang.sh` in the working notes
is the shape to copy -- run in a loop, and when the exit status is 139 rather
than a timeout, get a core or attach before it goes.

**Do not write a tier0 failure of this test off as "the known flake" any
more.** That reflex is what kept this one invisible.

## Closed during the 550 cycle

### Typed characters arrived out of order -- FIXED 2026-08-23, and only h/j/k/l ever moved

Typing quickly into the terminal delivered characters transposed, always with a
later character jumping toward the front: `mkdir big` -> `kmdir big`,
`cd /etc/apk` -> `cd k/etc/ap`, `clear; touch "my file;rm.txt"` ->
`clear; htoucl "my fie;rm.txt"`. The shell genuinely received the wrong bytes,
so it was neither a rendering artifact nor dropped input.

**Every displaced character is in `hjkl`, and nothing else ever moves.** That is
the whole diagnosis. `app/TerminalView.m` registered those four as bare
`UIKeyCommand`s (added 2026-03-23 in `4c10b28af`) so that a held key would
repeat -- UIKit only repeats keys a key command has claimed. But claiming a key
also changes how it is delivered:

* a key command is dispatched straight off the key event -- measured **0.3 ms**
  after the press;
* every other printable character reaches `-insertText:` through UIKit's
  text-input pipeline -- measured **~5 ms** after the press, and further behind
  once that pipeline has a backlog.

Two delivery paths, nothing sequencing them. Each stream stayed internally
ordered and they interleaved wrongly, which is exactly the observed shape:
`abcdefghijklmnopqrst` reached the tty as `abcdhejkflgimnopqrst`, the sixteen
other letters still in order.

**Does it affect real users? Measured: no, not by typing.** Injecting a
`ghghghghgh` alternation at controlled rates, order held at 11 ms between
keystrokes and first broke at 5.6 ms -- about 90 keystrokes/second. It stayed
correct at 40-100 ms with the terminal under heavy render load (a `yes` flood),
because a busy main thread delays both paths together rather than spreading
them. The software keyboard, paste and autocorrect never enter the key-command
path at all, and key repeat is self-consistent. So the reordering needs a
machine at the keyboard: synthetic injection, a macro key, a scanner, anything
delivering faster than ~90 keys/s. It is still a defect -- the terminal has no
business reordering what it is handed -- and it was worth fixing on those terms,
not as a user-facing regression.

**The fix** (`app/TerminalView.m`) drops the four bare key-command registrations
so the letters travel the same in-order path as the rest of the alphabet, and
synthesises the held-key repeat from `-pressesBegan:`/`-pressesEnded:` instead,
at the delay and interval UIKit's own key-command repeat was measured using
(0.4 s, then 0.1 s). The first character of a hold now comes from the ordinary
text path, so only the repeats are synthetic.

**Verified in the Xcode Simulator build**, same device and same harness for
both sides, comparing the keys the app received against the bytes that reached
the tty: baseline reordered 5 runs out of 5 at 2-3 ms per key, the fix preserved
order 5 out of 5. End to end through the guest tty, all eight test lines --
including every string from the original report and `hjkl hjkl hjkl kjhl lkjh`
-- came back byte-exact. Held h/j/k/l still deliver 7 characters over ~1.3 s,
identical to the old behaviour, and an unregistered letter still delivers 1.

Two things worth knowing about the harness, both of which cost time here:

* `xcrun simctl`-style injection drops keys below about 3 ms spacing, and a
  dropped key looks exactly like a reordering failure in a naive string compare.
  Log what the app actually *received* alongside what it delivered; the diff
  between those two is the only honest signal.
* Another session on this machine was installing its own build onto the same
  booted simulator, which silently replaced the binary under test and wiped the
  guest root twice. Check the installed binary's hash before trusting a
  measurement, or boot a private device.

**Residual, not fixed:** arrows, Tab, Esc, function keys and the Ctrl chords are
still key commands, so they keep the same ~5 ms head start over typed text. The
hazard is unchanged from before the vi keys were added and is far rarer -- it
needs a special key inside the same 5 ms window -- but it is the same defect.
Fixing it properly means either owning key repeat for every key or buffering the
fast path behind the text path, neither of which this bug justified.

### The file browsers froze behind bulk transfers -- FIXED 2026-08-23

`GuestFileBridge` ran everything on one serial queue -- directory listings,
stat, statfs, mkdir, rename, delete, whole-file reads and writes, chunked
extraction, cross-backend copies -- so a large copy or extraction sat in front
of every short operation behind it. The file manager made that maximally
visible: `-setLoading:` (app/WorkspaceFileManager.m) holds
`userInteractionEnabled = NO` for the whole load, so the folder was not merely
stale, it was untappable. That is the mechanism behind the reports of the file
manager going "completely unresponsive trying to do anything with the folder,
including just viewing it".

**Fixed by separating latency classes, not by adding parallelism.** Two SERIAL
lanes, interactive and bulk. Concurrency was never the answer here: AOK's fakefs
metadata mutex already saturates under a single thread (78% duty under
`ISH_FAKEFS_LOCKSTATS`) and parallel metadata work measures slower. What was
wrong was that a `readdir` of twelve entries queued behind a 300 MB transfer.

Design and the full reasoning: [guest_file_bridge_lanes.md](guest_file_bridge_lanes.md).

**The ordering question, which is where a naive split goes wrong.** Callers do
write-then-reload (new folder, rename, duplicate, delete, then `-reload`) and
expect the listing to show the result. Reading every call site settled it: they
all issue the reload *from inside the mutation's completion block*, so the
happens-before is the completion's, not the queue's, and splitting cannot break
it. The `ioQueue` property that existed so callers could order their own work
behind pending operations had **no users**; it is gone from the header so nobody
can rebuild a dependency on a total order that no longer exists.

That was not left to caller discipline. The bridge keeps a table of the claims
held on each lane and enforces per-path ordering itself, under one invariant:
**the interactive lane never waits** -- an interactive operation that would have
to wait is re-routed onto the bulk lane behind the work it must follow, while
the bulk lane may block, because it is already slow. No cycle, so no deadlock.

**Three bugs found by testing it, all in the first draft of my own work:**

- The claim-conflict rule started as "equal, or one is an ancestor of the
  other". `/` is an ancestor of everything, so a copy into `/tmp/foo` conflicted
  with a listing of `/` -- any transfer anywhere would have pushed every listing
  of `/` onto the bulk lane, which is the exact blocking being removed. The rule
  is now: same path, a path and its parent directory, and (for a recursive
  delete alone) a subtree and anything inside it.
- `WorkspaceImageViewer`'s sibling scan had no generation guard, so once
  cancellation existed, a scan cancelled by the next `loadPath:` landed
  afterward and cleared the token that load had just stored.
- The cross-lane barrier is a `dispatch_sync` onto the interactive lane, which
  self-deadlocks when `ISH_BRIDGE_SINGLE_LANE` makes the two lanes the same
  queue -- and it duly did, the first time the control run reached section 4.
  The shipping path was never exposed to it (the barrier is only ever set for
  work heading to the bulk lane), but the knob is only useful if it runs, so the
  barrier is now skipped when the queues are identical, where the ordering it
  restores is already the queue's own.

**Also folded in: listings are cancellable.** They had no token at all, and
`ShellFileBrowser`'s `_loadGeneration` guard discarded the *answer*, not the
*work* -- dismissing a sheet over a big directory left the whole enumeration
running. `ISHGuestFileOperationToken` now covers listings, whole-file reads,
extraction, and the cross-store copy (which previously ran to completion even
when cancelled). Recursive delete is deliberately NOT cancellable: stopping a
tree walk halfway leaves a half-deleted tree and no way to say what survived.

**Measured, not asserted.** `ISH_BRIDGE_LANE_SELFTEST=<MiB>` runs a harness
against the live guest fs at launch, and `ISH_BRIDGE_SINGLE_LANE=1` collapses
the lanes back into one so the control is taken on the same hardware in the same
binary. iPhone 17 Pro simulator, 64 MiB payload, listing `/etc` with the bulk
lane saturated by continuous copies (mean 19-20ms each):

                                    under load            idle baseline
    one lane (the old behaviour)    mean 19.8ms worst 77.3ms    1.3 / 7.6ms
    two lanes                       mean  1.6ms worst  7.7ms    0.8 / 5.7ms

so listing latency under load drops about 12x on the mean and 10x on the worst
case, and lands within noise of the idle baseline. Under one lane the mean
listing costs almost exactly one copy, which is the head-of-line blocking stated
as a number; the worst is four copies, because a listing there queues behind
several. Section 1 correctly FAILS its own bar in the control run.

Ordering held in both configurations: 40 iterations of write-then-reload under
bulk load, 0 mismatches; 40 iterations of mutation-then-listing enqueued back to
back *without* chaining through the completion, 0 same-lane and 0 cross-lane
misses; 30 iterations of unchained write-then-copy, 0 stale reads. Cancelling a
900-entry listing mid-walk cut it from 9.5ms to 2.8ms, so the work stops rather
than the answer being thrown away.

The claim-conflict truth table is also checked directly, including the `/` case
above and the `/ab` vs `/abc` component-boundary trap.

And by hand in `ShellFileBrowser` on the simulator, against a 400 MB file, since
the harness drives the bridge and not the widget: Duplicate (a 400 MB bulk copy),
New Folder, Rename and Delete each showed their result in the reload that
followed. Note the simulator is far too fast for a wall-clock freeze demo -- it
writes 400 MB through fakefs at 2.6 GB/s -- which is why the numbers above come
from saturating the lane rather than from timing one copy.

**The extraction leak is closed -- FIXED 2026-08-23.** `-clearExtractionCache`
had no caller despite its own documentation saying it should run at app launch,
so every video played and every image shared out of a fakefs root left a
full-size copy in `NSTemporaryDirectory()/GuestFileBridgeExtractions` that no
later run could even find to reuse -- the cache keyed to those files is
in-memory, so it dies with the process while the files do not.
`-application:didFinishLaunchingWithOptions:` now calls it. Nothing can be
holding a URL from a previous run for the same reason the files were
unreachable, and the call is a `dispatch_async` onto the bulk lane with no path
claims, so it does not hold launch.

**Still open in this area.** Copy and move return cancellation tokens that no UI
drives yet.


### SmallCLUE's pager wedged apt -- FIXED 2026-08-22, and it was not the pager

`apt search maria` hung the whole app, every time, and removing
/usr/local/native-bin/less cured it. This entry used to say the trigger was
uncharacterised and name the pager's read-it-all spooling as the likely shape.
The spooling was real and is gone now, but it was not the trigger.

**The trigger was close-on-exec, and it was ours.** APT learns whether its pager
exec'd by handing the child a close-on-exec pipe and reading four bytes from it:
EOF means the exec happened and closed the write end; four bytes of errno mean
it did not. `kernel/exec.c`'s native dispatch (`/AOK/native/*`) returned from
`__do_execve` the moment it had recorded the program to run -- before
`fdtable_do_cloexec`, before the signal-handler reset, before `vfork_notify`.
The write end therefore survived into the pager, apt's read never saw EOF, and
apt sat on four bytes while the pager sat on the stdin apt had not begun
writing. That is both backtraces, exactly.

It also explains the observation that stopped the last attempt: `PAGER=<path>
apt search maria` COMPLETED. Anything that puts a real exec between apt and the
native pager -- a shell script, an interpreter -- closes the pipe on apt's
behalf, and the pager that runs after it is not the exec apt was watching.

**And how apt found the pager at all**, which the old entry called a clue and
could not resolve: `opt/AOK/tools/provision-ultimate-devuan.sh` exports
`PAGER=less`, a BARE name. apt resolves that on PATH, and native-links.sh puts
/usr/local/native-bin first on PATH through /etc/profile.d. A non-login ssh
session never sources /etc/profile.d -- which is why seven attempts there kept
getting /usr/bin/less and never reproduced it.

**Fixed in three places:**

1. `kernel/exec.c` -- `exec_apply_native_process_state()` applies the
   process-state half of an exec to a native program: close-on-exec, the
   signal-handler reset and altstack, did_exec/keepcaps, and `vfork_notify`.
   That last one is its own latent hang: a vfork parent was being released by
   the native program's EXIT rather than its exec, so it stayed blocked for the
   whole run -- and glibc's `posix_spawn` waits exactly that way.
2. `deps/smallclue` -- the pager streams. `pagerCollectLines` read the entire
   input before drawing a line; `pagerBufferFill` now reads only as far as the
   reader has scrolled, so the first screen appears at once and a stream that
   never ends is paged rather than waited on. Pipes are read a byte at a time
   for that reason, regular files in blocks. It also took real less's other
   rule with it -- "if the output is not a tty, less acts like cat" -- which
   this pager applied only when it had no controlling terminal at all, so
   `less file | head` inside a session used to draw a page down a pipe nobody
   was watching and then wait for a keystroke nobody would type.
3. `opt/AOK/tools/native-links.sh` -- `less` and `more` are linked again.

**Verified with the actual failing case rather than a model of it:** Devuan 6
amd64, `native-links.sh` run for real, /usr/local/native-bin first on PATH via
the snippet the script installs, `PAGER=less`, stdout a tty, `apt search maria`.
Before: no pager output at all, four keypresses ignored, killed at 140s. After:
the pager draws its first screen, Q exits it, `APT RC=0`, 83s.
`tests/manual/native_exec_cloexec.c` is the guard -- it runs apt's handshake
against a native exec, with an ordinary exec as a control so it still means
something on a Linux oracle, which has no native dispatch.

**Two things worth keeping.** Streaming alone would NOT have fixed this: apt
writes nothing until its read returns, so a streaming pager would still have had
an empty pipe to wait on. And the class is wider than the pager -- EVERY native
program was exec'ing without close-on-exec, so any parent using that handshake
would have hung on any of them.

### ptraceomatic's divergence at 4175 -- NOT AN EMULATOR BUG, FIXED 2026-08-22

The emulator was right and the tool was wrong, for the third time in this
tool's short working life. `edx: real 0x1, fake 0x800000` -- and 0x800000 is
8388608, which is exactly camd's `ulimit -s`. The correct answer was the one
the emulator had.

**What it actually was.** ptraceomatic does not let the real process execute
`int $0x80` at all. It intercepts the instruction, `pt_copy`s the emulated
process's output buffer into the tracee, injects the emulated return value into
eax, and steps the eip past it. That is what the big switch in `step_tracing`
is for. A syscall with no case in that switch still gets its **return value**
synced -- the code falls out of the switch into `regs.rax = cpu->eax` -- but
nothing writes the **memory** the syscall was supposed to fill. The real
process keeps whatever was already there.

Syscall 191, `ugetrlimit`, had no case. glibc's `__libc_early_init` calls
`getrlimit(RLIMIT_STACK, &rlim)` and then reads `rlim.rlim_cur` off the stack
four instructions later; the emulated process had 0x800000 written into it and
the real one still had uninitialised stack, which happened to be 1. `8b 54 24
14` was innocent, as the entry suspected -- but the write it was missing had
never been performed in the first place, rather than performed differently.

Both variants were missing, 191 and 76 (`old_getrlimit`), and both write a
`struct rlimit32_` through the address in ECX. Two lines fix it.

**The honest alternatives, ruled out rather than assumed away:**

- *auxv, initial stack, environment.* Ruled out by construction, not by
  argument: `prepare_tracee` calls `transplant_vdso` and then `pt_copy`s the
  emulator's entire initial stack page range into the tracee, and sets the
  tracee's esp to the emulator's. argv, envp and auxv in the real process are
  the emulator's own bytes. This was the entry's first suspect and it was the
  wrong one.
- *ASLR.* `start_tracee` sets ADDR_NO_RANDOMIZE, and the divergence reproduces
  at exactly instruction 4175 across two different binaries built minutes
  apart -- 0x8058b8b in one, 0x805f2cb in the other, same four bytes, same
  values. Nothing random survives that.
- *A syscall returning host-specific data the tool does not synchronise.* This
  is the right family, but the direction is the reverse of the guess. The
  problem was never that the real process saw a host value the emulator could
  not know. It was that the emulator's answer never reached the real process.
  The tool's design means host-specific results are *already* handled: watch
  `readlinkat` on /proc/self/exe, where both CPUs agree on the emulated answer
  because case 305 copies it across.

**Evidence, five independent lines.** The reading instruction is four
instructions after the `call __new_getrlimit` that fills the slot;
`__new_getrlimit` issues `mov $0xbf,%eax` = 191; 191 is absent from the switch;
the fake value equals the host's real RLIMIT_STACK and the real value does not;
and removing the case reproduces the divergence exactly while adding it back
removes it. A new `PTRACEOMATIC_TRACE_SYSCALLS=1` prints the same conclusion
directly -- `syscall 191 -> 0   [no memory sync]` -- which is the diagnostic
that would have found this in minutes, so it is now part of the tool.

**The second bug it was hiding.** With 4175 fixed the run got 416 instructions
further and stopped again, at `call *%gs:0x10` in `_dl_get_origin`, with esp
off by exactly 4. `step_tracing` runs the fake CPU, and on any interrupt other
than INT_DEBUG hands it to `handle_interrupt` -- then steps the real CPU
regardless. But a fault the emulator resolves itself, here an INT_GPF growing
the stack after `sub $0x101c,%esp`, leaves the faulting instruction
**un-retired**, to be retried on the next run. The real CPU had executed the
`call` and pushed a return address; the fake one had not moved. One instruction
out of phase, and the next compare blames whichever instruction is next. The
fix holds the real CPU back whenever the fake CPU takes a non-syscall interrupt
without advancing eip, bounded at 16 consecutive holds so a genuinely stuck
emulator reports instead of hanging.

**Where it leaves the tool.** A static i386 glibc binary now runs start to
finish with no divergence at all. Verified on four binaries, twice each, exit
codes matching a native run: an empty `main`, /tmp/hi, a witness printing
getrlimit's result, and a syscall-heavy one doing uname, readlink, a 1MB
malloc-and-touch, and file I/O. An edge-case binary calling 191 and 76 directly
matches native on all four paths including the NULL-buffer EFAULT and the
EINVAL that must leave the buffer untouched. Both compilers build it clean.

amd64 guests still die in setup with SIGSEGV -- unchanged by any of this, and
consistent with the comment in `prepare_tracee` saying vdso and stack sync for
amd64 is phase-2 work.

**The general lesson for this tool.** Only four syscalls in a whole start-up
were intercepted rather than executed: 258 `set_tid_address`, 311
`set_robust_list`, 386 `rseq`, and 191. The first three write no guest memory,
so exactly one of the four needed a case and exactly that one was missing. When
ptraceomatic next reports a memory divergence, run it with
PTRACEOMATIC_TRACE_SYSCALLS=1 and read the `[no memory sync]` lines before
reading the emulator.

### CI was red for the whole 550 cycle -- FIXED 2026-08-22

Found by release verification, not by anything failing locally. Every commit
since 549 had failed CI, on three jobs, for two unrelated reasons. Neither
shows up in an Xcode build, which is why neither was noticed.

**Linux (clang and gcc).** `kernel/native_libc.c` included `<sys/sysctl.h>`
unconditionally; glibc dropped it and musl never had it. A second one behind
it: `kernel/native_kqueue.c` included `<sys/event.h>`, kqueue being BSD-only.
Every other Darwin-only include in the non-app sources was already guarded, so
these were the only two gaps -- which is why the build stopped at exactly them.
Both are Darwin-only in substance: sysctl passthrough answers about the host,
and the kqueue front end exists only because a runtime built for Apple reaches
for kqueue. Linux gets ENOTSUP and four linkable stubs, not a second
implementation.

Consequence worth stating plainly: Linux CI is a second compiler, and GCC has
caught shipping bugs here that clang's `-w` hides. Nothing in this cycle went
through it until now.

**mac, and the release tag.** `the aarch64-apple-ios target may not be
installed`. None of the three workflows installed it. 549 shipped before Rust
was in the build; 550 is the first release that needs it, helix being Rust and
compiled in unconditionally. The release IPA workflow is not exercised until a
tag is pushed, so **pushing builds/iSH-AOK_550 would have failed to produce an
IPA** with nothing before it to warn us.

The trap is `native_rust = auto`. The runners ship cargo, so auto enables the
Rust program and the build only then discovers the target is missing. Detecting
a toolchain is not the same as being able to build for the platform.

Verified: CI green on 9fdeb44ca -- build-linux (clang), build-linux (gcc) with
its Unit tests and e2e steps, and build-mac -- plus Build Dev IPA, which is the
same shape as the release IPA job. The Linux fixes were iterated in an
ubuntu:24.04 container with `ninja -k 0`, which reports every portability error
in one pass instead of one per CI round trip.

### System Console gave no prompt -- FIXED 2026-08-21 (tty hangup recovery)

Reported as "the System Console fails to give the prompt". Reproduced in the
CLI by booting the guest's own init (`./build/ish -f ROOT /sbin/init 2` under a
pty): the pre-fix build never reaches a login prompt, the fixed one shows
`login:` on tty1 within a minute, from the same root state.

The full chain, all four steps confirmed:

1. bootlogd holds /dev/tty1 through boot and is stopped at the end of it. A
   session leader exiting hangs up its controlling terminal -- correct, and
   kernel/exit.c does it deliberately.
2. getty's EXISTING descriptors correctly go EIO, and it exits. Also correct.
3. init respawns getty -- and its FRESH open was still EIO. **This was the
   bug.** AOK modelled a hangup as one sticky bool on the tty, where Linux
   scopes it to the descriptors open at the time.
4. So getty died repeatedly until `init: Id "1" respawning too fast: disabled
   for 5 minutes`, and the console stayed dead. Restarting the app did not
   help, because bootlogd does the same thing every boot.

Fixed by per-descriptor hangup generations (fs/tty.c, tests/manual/
tty_hangup_reopen.c). Note bootlogd's TIOCCONS is already a deliberate no-op
here and is NOT involved -- worth saying, because it looks like the culprit in
a trace.

Two wrong turns, both plausible: "udev rewrote the tty nodes" (it had, to
root:tty 0620 -- but udev never ran on the boot that was broken), and "clear
the flag when no descriptors remain" (the app holds the console open, so the
list is never empty; that version passed a pty-based test and fixed nothing).

### System Console never gets a shell on Devuan 6 -- ROOT CAUSED AND FIXED 2026-08-21

Reported with a thread backtrace, then established by logging into the device
itself, which turned a plausible story into a measured one and found a kernel
bug underneath it.

**The console was never the broken part.** sysvinit runs the runlevel as
`l2:2:wait:/etc/init.d/rc 2`, and `wait` means init processes no later entry
until rc exits -- the `getty` lines are `respawn` entries after it. rc was
parked in `/etc/init.d/mariadb start`, which polls `mariadb-admin ping`
forever against a mariadbd that had already decided to abort but could not
exit. Killing that chain by hand made rc exit and six gettys appear within
seconds, which is the whole chain confirmed end to end.

**Why mariadbd could not exit -- the actual kernel bug.** Its `/proc` state
named it exactly:

    tid 990 (signal_handler, parked in sigwait)  SigPnd: 0
    tid 857 (main thread)                        SigPnd: 0x4000  (SIGTERM)
    both                                         SigBlk: 0x85007 (HUP INT QUIT PIPE TERM TSTP)
    neither                                      ShdPnd: 0

MariaDB signalled its own signal thread to make it exit. `kill(2)` is
PROCESS-directed on Linux: it goes to the thread group's shared queue, where
any thread not blocking it -- including one in `sigwait()` for exactly that
signal -- dequeues it. AOK's `do_kill` delivered it to the addressed task's
PRIVATE queue instead, so it landed on a thread that blocks it and that nobody
will ever dequeue from, while the sigwait-ing thread saw nothing.

That breaks the standard daemon shape -- block the signals in every thread,
one dedicated thread sigwaits them -- which MariaDB, PostgreSQL and most sysv
daemons use. `kill <pid>` against any of them was a no-op.

Fixed by `pick_process_directed_target()` in kernel/signal.c: `kill` now hands
the signal to a thread that can actually take it -- preferring one parked in
`sigwait()` for exactly that signal -- while `tkill`/`tgkill` still deliver to
the addressed thread's private queue, which is their purpose.
tests/manual/sigwait_kill.c reproduces it -- watchdog before, PASS after, on
all four ABIs.

**The first fix was wrong and tier0 caught it.** Routing all of `kill` through
`send_signal_to_group` looked like the obvious answer and hung
`signal_restart`, `signal_stop_cont` and `process_conformance`: that path does
not carry the stop/cont and default-ignore handling `send_signal()` has. The
lesson generalises -- the group path is for signals that are genuinely
group-wide (SIGCHLD from exit.c), not a drop-in for per-task delivery. Choosing
the target thread instead leaves every case that already worked on exactly the
task it used before, so the blast radius is only the case that was broken.

**Still open, and the reason mariadb is disabled on that device:** the crash
that started all of it, in the entry below.

### mariadbd SIGSEGV in ha_maria::drop_table -- FIXED 2026-08-21, and it was ours

Established on the M4 iPad, then reproduced locally and fixed. Not an Aria bug,
and not an AIO one either: **AOK returned EBADF for a legal openat.**

`openat(2)`, and POSIX for the whole *at() family: "If the pathname given in
pathname is absolute, then dirfd is ignored." Ignored, not merely unused -- it
is never looked at, so it may be -1, or closed. AOK validated it anyway. glibc,
canonicalising an Aria temp table, calls

    openat(-1, "/tmp", O_PATH|O_CLOEXEC|O_NOFOLLOW)

got EBADF where Linux hands back a descriptor, and Aria carried the failure
three frames -- surfacing first as `Got error 9 "Bad file descriptor" from
storage engine Aria` -- before dereferencing the NULL it had left behind. The
SIGSEGV therefore landed in ha_maria::drop_table, nowhere near the mistake,
which is why it read as a MariaDB bug for weeks and why its fault address is
recorded elsewhere in this file as an AIO nullptr.

**No MariaDB install on iSH-AOK had ever completed**: mysql_install_db died
leaving 3 of ~30 system tables. It now installs 88 and serves queries, verified
on the device.

Fixed by at_fd_for_path() in kernel/fs.c **and fs/stat.c** -- that file keeps
its own copy of at_fd, so the first fix passed everything except fstatat, and
only tests/manual/at_absolute_path.c noticed. statx lives in that same file,
which is the call modern glibc actually makes. The two helpers are still
duplicated; unifying them is worth doing.

Found with the fd-555 syscall log against a local Devuan root. strace ON THE
DEVICE would not follow the exec into the crashing binary and named nothing --
build the local repro instead of fighting it.

### AOK lost a connected UDP socket's error -- NO LONGER REPRODUCES 2026-08-20

Measured again on the same harness the entry was written against: **10 of 10,
five runs**, against the 14-of-20 recorded here. It does not reproduce with or
without any change of mine, so it is fixed and I cannot claim it.

**What fixed it.** `31261988b` ("a dead connection reports itself once, and AOK
stops eating the report") landed 2026-08-19 at 18:28, hours after this entry
was written, and introduced `sock_take_pending_error()` -- which the recv path
calls when a read returns nothing, precisely to recover an error AOK's own poll
probe consumed off the host first. That is this bug's mechanism, answered.

**One genuine gap remains from the same family, and is fixed here.** The
EVFILT_EXCEPT branch of fs/poll.c's `rpe_events` calls getsockopt(SO_ERROR) --
which read-and-CLEARS on the host -- and threw the value away, returning only
POLL_ERR. fd.h's contract for `host_connect_error` is that AOK's internal
probes stash what they consume so the guest-facing call still sees it, and the
two other probe sites do; this one did not.

It is not what the UDP test measures, and the entry should not pretend
otherwise: that test polls POLLIN only, so EVFILT_EXCEPT is never armed and the
change is inert for it. It bites a guest that asks for POLLERR/EPOLLERR --
which epoll consumers do, rtorrent and libtorrent among them, per the comment
at that very site -- and then finds its error gone.

### `pread_stack_thread_race` hangs -- FIXED 2026-08-21, and it was not a lock cycle

**The recorded diagnosis was wrong, and confidently so.** This entry said the
hang was an ordering cycle -- the mem read lock held across the jetsam lock --
and proposed two fixes costed against that. Neither would have worked. Sampling
a freshly wedged run showed:

- one thread in `mem_write_lock_with_pokes`, waiting for readers to drain;
- one frontend thread asleep in `_pthread_rwlock_lock_slow` on the jetsam lock,
  holding the mem read lock;
- every other thread parked in `mem_quiesce_park`, a futex, or nanosleep;
- **and no thread holding the jetsam write lock at all.**

There is no cycle. That reader is asleep on a lock NOBODY HOLDS -- the Darwin
psynch lost-wakeup pathology kernel/task.c already documents ("writers asleep
forever on a FREE lock"). `mem_write_lock_with_pokes` exists to fire SIGUSR1 at
exactly these threads to evict them, and a signal landing on a thread parked in
`_pthread_rwlock_lock_wait` is what loses the wakeup. The reader then never
returns, never drops the mem read lock, and everything parks behind the writer.

The earlier stack trace was real; the conclusion drawn from it was not. Two
threads blocked on a lock looks like contention until you check whether anyone
holds it.

**Fixed** by `jetsam_read_lock_polled` (jit/jit.c): the nine blocking
`pthread_rwlock_rdlock` sites poll with `tryrdlock` instead, so a lost wakeup
self-heals on the next attempt. This is the treatment the WRITE path already
had -- `jetsam_write_lock_timed` polls for the same reason, so a stuck reader
cannot wedge writers -- applied to the side that was still blocking.

Measured, same harness both sides:

    pre-fix   amd64  hung on run 1;  i386  2 hangs in 8
    post-fix  amd64  0 hangs in 12;  i386  0 hangs in 10

**It was masking a second, unrelated bug** -- see the entry below. Every tier0
failure of this test has been written off as "the known hang"; roughly one in
eight on i386 was not.

### Rust runs natively, async Rust included -- WORKING 2026-08-20

The question blocking python, node, ripgrep and helix -- can another
toolchain's objects be made to call `nlibc_open`? -- is answered, and no longer
with a C stand-in: real Rust, `rustc 1.98`, built as a staticlib, its libc
imports rewritten onto the shim by `tools/build-rust-native.sh`, running as
`/AOK/native/rust-probe`. Every question the prototype left open is settled
except one, and that one is now the whole remaining piece of work.

**What works, measured rather than reasoned about.** `std::fs` reads and
writes the guest. `env::args`/`env::vars` see the guest's. Threads work, and a
spawned worker can read guest files -- it is not a main-thread-only trick.
`std::process::Command` works with inherited, null and piped stdio, running
guest binaries. `isatty` and `TIOCGWINSZ` answer about the guest's tty (43x132
under a pty). `available_parallelism` reports AOK's core count, not the Mac's.

**tokio works too, as of the same day.** kernel/native_kqueue.c is a kqueue
front end over the guest's `ppoll`; the runtime builds, timers fire, an async
TCP round trip completes, and `tokio::process` spawns a child, reads its
output through the reactor and reaps it. Reproduce with

    AOK_RUST_FEATURES=tokio-probe ninja -C build ish

Four things had to be true, and three of them were bugs elsewhere:

- **EV_CLEAR is edge, ppoll is level.** A descriptor that has been reported
  ready is held back until it is next seen NOT ready, and is excluded from the
  blocking set meanwhile. Without that, tokio's driver marks readiness, loops
  straight back into kevent, and ppoll returns the same descriptor at once,
  forever.
- **A dup of a kqueue names the same queue.** mio's `Waker::new` calls
  `try_clone()`, so the waker fires EVFILT_USER through a different descriptor
  number than the poller waits on. Without the alias it got EBADF, which is
  what tokio reported as "Runtime::build failed".
- **SA_SIGINFO was refused**, on the grounds that there was no siginfo to hand
  host code. There was: `rt_sigtimedwait` writes one and the shim was passing
  NULL for it. signal_hook, and so tokio's process and signal drivers, needs
  the three-argument form.
- **The shim's handler table was per THREAD.** A native program's threads share
  one task, so the thread that installs a handler is not the one that reaches
  a checkpoint and delivers it; the deliverer saw an empty table and took
  nothing. Now per task.
- **The held set was one registration behind.** `nlibc_set_disposition`
  computed it from the handler table before the table was written. A shell
  installs several handlers and the next one papers over the last, so this
  never showed; tokio installs exactly one, the held set stayed empty, and a
  blocking poll in the guest was never interrupted for SIGCHLD.

**Two gaps named rather than closed.** `SO_NOSIGPIPE` is accepted and does
nothing -- Darwin's per-socket SIGPIPE suppression has no Linux counterpart,
and a write to a departed peer still raises SIGPIPE. And a SA_SIGINFO handler
is given a NULL `ucontext_t`: it describes interrupted machine state, and a
native program's handler is a plain call at a checkpoint rather than a frame
the kernel built.

**What the front end covers**, which is mio's use of kqueue rather than all of
it. Counted from mio 1.2.2's `sys/unix/selector/kqueue.rs` -- the rest of the
EVFILT_/NOTE_ names in that file are a Debug formatting table, not calls:

  filters   EVFILT_READ, EVFILT_WRITE, EVFILT_USER
  flags in  EV_ADD, EV_DELETE, EV_CLEAR, EV_RECEIPT, EV_ONESHOT, EV_DISPATCH
  flags out EV_ERROR, EV_EOF
  fflags    NOTE_TRIGGER, for the waker

Any other filter is refused with ENOTSUP rather than ignored, so a program
that wants EVFILT_VNODE or EVFILT_PROC finds out.

**Four silent host escapes were found on the way and closed**, all of the same
family -- a Darwin entry point the rename list did not name:

- `realpath$DARWIN_EXTSN`. `<stdlib.h>` makes `realpath()` emit the suffixed
  symbol, the list was keyed on the bare name, and `fs::canonicalize` resolved
  against the Mac's filesystem with every visible check passing. The generator
  now emits `$DARWIN_EXTSN`, `$INODE64`, `$UNIX2003` and `$NOCANCEL` variants.
- `getpwuid_r`. `getpwuid` was routed and the `_r` form was not, so Rust's
  `home_dir()` read the Mac's `/etc/passwd`.
- `_NSGetArgv`/`_NSGetEnviron`. Darwin has no linkable `environ`, so a runtime
  built for Apple reads its arguments and environment through these -- and got
  the iSH app's.
- `std::fs::copy`'s fast path: `fclonefileat`, `fcopyfile`, `copyfile_state_*`.
  Not read/write at all on Apple; it copied one host file to another.

`ioctl(fd, FIOCLEX)` was a fifth of a different kind -- it hit the `default:
ENOSYS`, and since Rust sets close-on-exec that way on every pipe it opens,
`Command::output()` failed before the spawn was reached. It looked exactly like
a broken spawn path, and the spawn path was fine.

**Still open, and unchanged by any of this.** The rewrite does not reach inside
a prebuilt dylib, and it does not catch a call made through `dlsym` -- Rust
imports no `dlsym`, so nothing escaped that way here, but a runtime that does
would need a different answer.

**Turning the tokio test on from Xcode.** `AOK_RUST_FEATURES` in
app/iSH.xcconfig, next to `ISH_LOG` and `ISH_GUEST_ARCHS`; set it to
`tokio-probe`. It reaches cargo through a meson option rather than only the
environment, so changing it reconfigures and rebuilds the crate on its own --
through the environment alone the flag could be flipped and yesterday's crate
would ship. Empty is the default, and has to stay that way: the tokio test
pulls in a dependency tree with no business in a shipping app.

**One loose end before release.** `rust-probe` is a diagnostic and it is in the
app whenever cargo is on the build machine (`native_rust` defaults to `auto`).
Either flip that default to `disabled` or drop the registry entry before the
550 tag.

**And the test that guards this.** tests/remote/native_job_control.py drives a
native shell through a real pty -- ^C, ^Z, fg, the job table, a trap firing
while the shell waits. tests/manual's two native-shell suites run a shell with
`-c` and read what it prints, which covers everything except the part that
needs a terminal, and that is exactly where the signal work above could have
broken something.

### A terminal MotePad, and a guest-to-app channel -- BOTH DONE 2026-08-21

Requested 2026-08-20 as two halves, "and only the first is small". Both landed.

**The channel** is `/proc/ish/workspace` (fs/proc/ish.c, app side in
WorkspaceViewController.m), which answers the question the guest could not ask
-- am I under Workspace, and what can you open? -- and takes `open <tool>
[path]` requests. Deliberately narrow: one verb, a tool allowlist decided in
the app, unknown tool or relative path refused synchronously so the caller gets
an errno rather than silence from a queue it cannot observe. It reports
"accepted", not "on screen": UIKit cannot be touched from a task thread, and
blocking a guest write on the UI queue would deadlock a terminal hosted BY that
UI.

**The launchers** are twenty `ws-*` scripts generated into /AOK/persist/bin at
launch (AppDelegate.m). Prefixed because that directory is first on PATH and
several tool identifiers collide with real commands -- `info` is GNU info.

**The editor** is kernel/native_motepad.c, a native program: modeless, gutter,
Ctrl-S/Q/F/G/K, UTF-8-aware motion, atomic save. Under Workspace it hands the
file to the applet; -t forces the terminal.

**The trap worth remembering, because it cost hours.** A native program built
as an ordinary libish source gets the HOST libc, not the shim: it compiles,
links and runs while writing files to the Mac and reading the Mac's stdin,
where it races the emulator's own reader on the same descriptor and loses every
other keystroke. It needs its own archive compiled with the native_libc.h
force-include, folded in with link_whole (meson.build). Nothing about the
failure points at the cause -- the visible symptom was dropped keys, which
looks exactly like a tty bug in AOK. What ruled that out was running native
bash through the same harness and watching it behave perfectly.

Still open, and small: no wcwidth, so a double-width glyph counts as one column
and can leave the cursor a cell off on such a line.

### eudev refuses to start -- FIXED 2026-08-21, and it was never about containers

The entry this replaces had two things wrong, both worth naming because both
would have sent the work in the wrong direction.

**It is not a container check.** eudev's init script line 126:

    # System processes and/or kernel threads are surrounded by brackets: [...]
    if ! ps --no-headers --format args ax | egrep -q '^\['; then
      log_warning_msg "eudev does not support containers, $NAME not started"

The test is "are any KERNEL THREADS visible". The word container appears only
in the message. AOK showed none, so the check fired.

**And udevd works.** The old entry hedged that starting it might be worse than
the warning, since udevd wants netlink uevents AOK does not generate. Started
by hand on the device it enumerated **30 devices**, and udevadm trigger and
settle both returned 0. There was nothing to protect anyone from.

Two things only a look at the device showed:

  - Started by hand it passed **by accident**: `[elogind-daemon]` has an empty
    cmdline, so ps brackets it and the egrep matches something that is not a
    kernel thread at all.
  - At boot it cannot. eudev is `Default-Start: S`; elogind is
    `Default-Start: 2 3 4 5`. Nothing that early has an empty cmdline, so the
    check fires -- which is exactly the reported symptom.

**Fixed** with a synthetic kernel thread: /proc/2 is `kthreadd`, with an empty
cmdline so ps renders it `[kthreadd]`, and pid 2 reserved in the allocator so
no real task can collide (40 spawned tasks got 85+, never 2). Only the files a
process listing reads are answered -- stat, cmdline, comm, status; everything
else still reports ESRCH, because there is no process there.

Deliberately ONE thread. AOK genuinely runs kernel-side threads (timer,
netlink watcher, JIT) doing kernel work for the guest, so showing one is not a
fiction -- but inventing a plausible crowd of `[ksoftirqd/0]` and friends would
claim more than is true. kthreadd is the honest minimum: on Linux it is the one
kernel thread that always exists.

**Still to do on the device**: eudev is not enabled at boot there
(`/etc/rc2.d` has no entry), so enabling it is a separate step from this fix.

### Linux native AIO -- IMPLEMENTED 2026-08-21

`apt install mariadb-server` used to leave the server crash-looping, because
the whole io_* family was `syscall_stub` and MariaDB 11.8's thread pool does
not fall back: `create_linux_aio()` returned nullptr. Now implemented in
kernel/aio.c for all four guest ABIs, with
tests/manual/aio_basic.c and aio_threads.c; see docs/aio_plan.md for the design
and, more usefully, for the three things implementation contradicted.

The drop-in workaround (`innodb_use_native_aio=0` under
/etc/mysql/mariadb.conf.d/) is no longer needed and was never shipped.

**Correction, 2026-08-21.** This entry used to claim the SIGSEGV in the
original report was that nullptr being dereferenced, and named the faulting
opcodes as "load the aio member". That was wrong. Resolving the same fault
address against the binary on the device gives
`_ZN8ha_maria10drop_tableEPKc+0x18` -- Aria's drop_table on a temp table, a
different bug that is still open (see the entry below). The io_* stub was real
and is fixed; it was simply not the thing that was crashing.

Two things deliberately NOT done, both cheap to add if something wants them:

  - **Asynchrony.** `io_submit` runs each iocb to completion before it returns.
    Every caller is correct with this -- the completion is simply already on
    the ring -- but a caller that submits a large read expecting to overlap it
    with other work does not get the overlap. Phase 2 in the plan is a worker
    per context; the structures do not change.
  - **IOCB_CMD_POLL**, which needs that asynchrony to mean anything. It is
    refused at submit with EINVAL, which is how a caller discovers it.

Still worth revisiting: nginx's `aio`, PostgreSQL's `io_method=aio` and RocksDB
all assume native AIO exists on Linux, and none has been tried yet.


### #558 npm segfault installing OpenClaw -- CLOSED BY THE REPORTER 2026-08-20

Reproduced the reporter's environment exactly rather than approximately, on the
M4 iPad: **Devuan GNU/Linux 6 (excalibur), aarch64, glibc 2.41, node v24.18.0,
npm v11.16.0** -- their distro, arch, libc and both version numbers. Devuan
packages only node 20, so they must have installed 24 themselves; matched with
the official arm64 build from nodejs.org.

On current code, `npm install -g openclaw` **added 309 packages in 5m, exit 0**,
and the result runs: `openclaw --version` reports 2026.7.1-2 (0790d9f) and
`--help` loads. The ladder they never answered passes too -- `node -e`, bare
`npm`, `npm --version`, `npm ping`, and a small install.

Also reproduced on an Alpine arm64 guest (musl, node 24.18.0, npm 11.17.0) for
a second data point: 309 packages in 14m, exit 0.

**What changed is not established, and one attractive answer was tested and
rejected.** They are on build 548, cut 2026-08-13, and reported on 2026-08-18;
ninety-six emulator commits have landed since. `49de7e671` (2026-08-19, the day
after the report) looked like the one -- a JIT use-after-free on a freed block's
ret_cache, fixed in all four frontends, and exactly the shape that would hit a
guest whose own JIT churns translated blocks the way V8 does. The dates were
suggestive enough to be worth testing rather than asserting.

It does not hold up. Built with that single commit reverted and ran the
identical install: **309 packages, exit 0** (20m rather than 14m). So the crash
does not come back when the fix is removed, and that commit cannot be credited.

What survives is the observation and not a cause: the failure does not
reproduce on current code in the reporter's exact environment, on Alpine arm64,
or with the suspect commit removed. Either something else among the 96 commits
fixed it, or it depends on a condition not matched here -- device memory
pressure, a different node install method, or their particular root.

Reported back on the issue with that evidence and asked them to retest on 549
or later. They replied "This Problem is Resolved thx" and closed it the same
day, so it was fixed somewhere in the 96 commits between 548 and now -- by
which one is still not established, and the one candidate worth suspecting was
tested and cleared.

### pidfd_epoll_deadlock was two bugs, not a flake -- FIXED 2026-08-21

Reported as "intermittently hangs and exits 137". It was both, and they are
unrelated: on `18df370e5`, 40 runs of the test gave **8 exits with 137 and no
hang**, and a later 24-run batch with only the first fix in gave **6 SIGKILLs
and 1 hang**. Neither is the fakefs connection-pool work -- both predate that
branch.

**137 was not a guest status.** cli_halt maps a signalled init to 128+signo, so
137 reads like the guest was SIGKILLed; the host `ish` process was. Every run
left a crash report in `~/Library/Logs/DiagnosticReports`, and all fourteen of
them were byte-for-byte the same stack:

    _os_unfair_lock_recursive_abort <- malloc <- _tlv_get_addr
      <- sigusr2_handler <- _sigtramp <- malloc <- _tlv_get_addr <- task_thread

A wake poke landed on a brand-new task thread while it was inside
`_tlv_get_addr` instantiating `current` -- the first `__thread` access on that
thread, which on Darwin malloc()s -- and the handler's own `__thread` read
re-entered malloc holding its lock. libplatform kills the process for that, and
because stdout to a pipe is block-buffered, the test's already-completed "PASS"
died in the buffer with it.

`task_start` blocks both wake signals across `pthread_create` precisely to
prevent this, and the mask really is inherited: instrumented, the creating
thread read 0x60000000 in 1000 of 1000 creations. The child did not. **18 of
1000 task threads entered `task_thread` with SIGUSR2 already missing from the
mask**, and others lost it later with none of our handlers having run on them
(verified with a per-thread ring buffer of handler entries). The same anomaly a
standalone 1600-thread reproduction of the identical pattern -- same attrs,
same pokes -- could not produce in three runs, and the same family as the
"Darwin swallowed the poke and left it blocked and pending" behaviour that
`signal_thread_unwedge_wake_sigs()` already exists to repair. **Not explained.**

So the fix does not rely on the mask. `signal_thread_locals_init()` now sets a
pthread TSD flag once the storage exists, and both handlers return immediately
unless it is set -- reading a TSD slot cannot allocate, and a thread that has
not run the init has no task and nothing to interrupt anyway. Every thread that
runs guest work now calls it: `task_run_current()` covers whichever thread runs
init, and `nlibc_thread_trampoline` covers native-program threads, which had
the same latent hole.

**The hang is an AB-BA cycle over three locks, and the pidfd path is innocent.**
`pidfd_notify_exit`'s half was already fixed with the trylock form; this comes
in through the SIGCHLD the same `do_exit` raises. Sampled live, with source
lines:

    A  do_exit          holds pids_lock   -> signalfd_wakeup_task -> fdtable_release -> files->lock
    B  close(2)         holds files->lock -> epoll_close -> poll_destroy -> poll->lock
    C  epoll_wait/ctl   holds poll->lock  -> pidfd_poll -> pids_lock

`signalfd_wakeup_task` is written not to block -- it trylocks the fd table and
gives up when busy -- but its give-up path called `fdtable_release()`, and that
took `table->lock` unconditionally, so it waited for the lock whose trylock had
just failed, with pids_lock still in hand. `refcount` is already atomic, so the
lock was never what made the decrement safe; it now guards only the final
teardown, which by definition runs with no other reference left.

Verified by A/B against a build with a 1.5 ms delay in `poll_destroy` to widen
the window: **2 hangs in 60 runs without the fix, 0 in 100 with it**. Unwidened,
252 runs of the real binary passed with no SIGKILL and no hang.

### `pidfd_open` refused a zombie -- FIXED 2026-08-20

sys_pidfd_open went through pid_get_task_ref, and pid_get_task filters zombies
out by design, so the call failed for a task that had exited and not been
reaped. Linux succeeds there -- an immediately-readable pidfd is how a pidfd
reports an exit at all -- and answers ESRCH only once the pid is gone.

It showed up as a flaky suite rather than a bug report: pidfd_epoll_deadlock
opens a pidfd on children that _exit at once, deliberately, so whenever a child
won that race the open returned ESRCH. About one run in three on x86_64.

pid_get_task_zombie_ref is the accessor it wanted, and it is safe against the
exit path for reasons already in pidfd.c -- pidfd_create flags its reference so
do_exit's exit_wait_needed() ignores it, and pidfd_poll already reported
POLL_READ for a zombie. tests/manual/pidfd_zombie.c pins it down with no race
in it (waitid(WNOWAIT) guarantees the zombie), and was verified to fail on both
arches with the fix reverted. `52f55c10d`.

### ptraceomatic did not run -- FIXED 2026-08-20 (GH #541)

Its divergence reports went to printk, which writes to fd 555 -- the emulator's
own log convention, which nobody redirects when running the tool by hand. The
write failed with EBADF, the report vanished, and `debugger` fired an int3, so
the tool that exists to say what differed died with "Trace/breakpoint trap" and
no output at all. Reports go to stderr now, the trap is opt-in via
PTRACEOMATIC_TRAP=1, and the report names the faulting instruction.

The issue's "tracee reaped during setup" was real too: start_tracee waited with
bare wait(), which reaps any child, so another child exiting at the wrong
moment was consumed instead and the WIFSTOPPED check read a status belonging to
something else.

Then it ran, and immediately reported two flags the architecture does not
define -- AF after a shift, and everything after DIV/IDIV. Both were gaps in
undefined_flags_mask rather than emulator bugs, which is exactly the kind of
false positive that would have made the restored tool useless. With them fixed
it reaches instruction 4175 before reporting something real. `796a9c179`.
That one turned out to be a third false positive of the same kind -- see
"ptraceomatic's divergence at 4175" above.

### `md`'s word boundaries come from the HTML, not from letter case -- FIXED 2026-08-20

The camelCase splitter behind the original "i SH-AOK" report was a guess
standing in for information the HTML converter had thrown away: dropping a tag
without putting anything in its place runs words together, so
"<td>Some</td><td>Text</td>" became "SomeText", and the answer had been to
insert a space wherever a lowercase letter met an uppercase one. Gating it to
fetched pages stopped it damaging local documents; it still damaged the fetched
ones, where macOS read as "mac OS" and GitHub as "Git Hub".

markdownHtmlTagKind classifies every tag reaching the converter's default case
-- inline, side by side, own line, own paragraph -- so the boundary comes from
the element, which is the only place it exists. <td>, <button>, <dt>/<dd>,
<nav> and the rest separate; <b>mac</b>OS stays "macOS".

That left the guessing with nothing to do, so it is gone along with its three
helpers, an 8 KB stack buffer and a copy of every line. It had been doing more
than letter case: splitting a digit from a letter ("utf8mb4"), splitting
"array[0]", and carrying hardcoded fixups for particular scraped sites.

**The limit, recorded so it is not filed again as a bug.** CSS can make an
inline element a block one and this does not read CSS, so GitHub's navigation
-- two <span>s inside one <a> -- still runs together as "GitHub CopilotWrite
better code with AI". Unfixable from the element name, and the better failure:
wrong about navigation furniture on some pages rather than wrong about macOS in
every document. Article text on the same page reads correctly throughout.
`6834e9e` in the fork.

### `md`: indented code blocks, and the syntax that showed through -- FIXED 2026-08-20

The indented-code-block gap filed earlier is closed, and the trap it was filed
over turned out to be real: four spaces under a bullet is that item's
continuation paragraph, not code. Detection is guarded on not being in a list
and on no paragraph being open, since an indented chunk cannot interrupt one.

Fixing it surfaced more of the same kind:

- **A list item's own wrapped lines were rendered as a separate block**, so the
  first line wrapped to the margin and the rest started again underneath --
  stray one-word lines like "    but". Items go through the paragraph buffer
  now and wrap once.
- **Width was measured in bytes.** "• " is three bytes and one column, and the
  list prefixes compensated by indenting continuations two spaces too far.
- **Every list rendered loose**, once items went through the paragraph buffer,
  until the trailing blank was made to come from a blank line in the source.
- **`has_blank_separator` was lying after a heading** -- it claims the output
  ends with a blank line and none was emitted, so two headings ran together.

And five inline constructs were printing their own markup: `\*` kept the
backslash and lost the asterisk; `&amp;` and numeric references rendered
literally; `![alt](url)` came out as "!alt [3]"; `<https://example.com>` kept
its brackets; and `[text][ref]` showed its brackets while every "[ref]: url"
definition was rendered as a paragraph, so a README keeping its links at the
bottom ended with a block of bare URLs. Hard breaks (two trailing spaces) are
honoured rather than reflowed away.

Checked against all 14 documents: every word of every source still present, no
colour-marker leaks in piped output, and the only over-wide lines are inside
code blocks. `11f07b4` in the fork.

### `md` rewrote the documents it rendered -- FIXED 2026-08-20

Reported against /AOK/docs, where "iSH-AOK" rendered as "i SH-AOK". Three bugs,
all of them the renderer editing text it should have been showing, and the
reported one was the least damaging of them.

- **Web-scrape heuristics ran on local files.** md cleans up pages fetched as
  HTML, and every one of those passes also ran on real .md files. camelCase
  splitting rewrote the whole vocabulary -- macOS to "mac OS", GitHub to "Git
  Hub", NSURLSession to "NSURL Session" -- and the CSS-selector detector
  DELETED whole sentences: two commas and a '.' not followed by a space was
  enough, so "small, synthetic, read-only filesystem (`aokfs`, see `fs/aok.c`)
  that" vanished from the overview with no marker where it had been. They are
  gated on the input having come out of the HTML converter now.
- **Code spans were not verbatim.** A backtick was skipped rather than opening
  a span, so `a*b*c` rendered as "abc".
- **Every '*' and '_' was deleted on sight**, matched or not: TZ_NAME to
  TZNAME, kernel/native_libc.c to kernel/nativelibc.c, "5 * 3" to "5  3".

Checked by rendering all 14 documents and diffing every word against the
sources: 66 words were missing before, and the remainder are table-cell
wrapping and '/'-joined tokens, verified present by hand. `787236a` in the
fork, `bdbd617e0` here.

### `md` has colour -- ADDED 2026-08-20

The reason it had none was a good one: the pager sanitises every line so a
fetched page cannot emit escape sequences. So the renderer emits a two-byte
private marker instead, and the pager expands markers into SGR only for buffers
it was told are markdown renders. A document carrying a stray marker can change
a colour and nothing else -- no cursor movement, no screen clear, no mode
switch. Markers are only emitted when something will expand them, so a
redirected run is plain text; NO_COLOR and a dumb TERM are honoured.

Two fixes fell out of it. **The pager's `raw_mode` had never worked**: it was
assigned before pagerCollectLines, which memsets the struct, so `less -r` has
been sanitising its output for as long as the option has existed. And **table
cells printed their backticks** while the same code span in a paragraph did
not, because cells never went through the inline pass. `87e628c` in the fork.

### The applets stubbed over a missing library -- FIXED 2026-08-20

tar, gzip, gunzip, zcat, curl, wget and `md <url>` all refused with "not built
into this iSH-AOK" or "networking support is unavailable in this build". Both
groups are real now, and neither was the build-system problem it looked like.

- **zlib was never missing.** It is public on iOS as well as macOS. The actual
  obstacle was that a system dylib compiled against the HOST libc does host
  I/O: `gzopen` on a guest path opened iOS's copy, invisibly correct wherever a
  path existed on both sides. deps/smallclue-shim/zlib.h reimplements the six
  gz* calls over the redirected open/read/write and leaves deflate/inflate --
  which touch nothing but the caller's buffers -- to zlib. `550bc653c`.
- **libcurl really is absent from the iOS SDK**, header and tbd alike.
  deps/smallclue-shim/curl/curl.h implements the seven functions core.c uses on
  NSURLSession. That is host networking, deliberately and with the cost written
  down: guest /etc/hosts and /etc/resolv.conf do not apply. `643c8ef7d`.
- **Enabling tar exposed a corruption in it.** SmallCLUE's tar treated GNU
  @LongLink and PAX headers as files, then read their data as the next header
  and abandoned the archive -- and those are what busybox tar writes for any
  path over 100 bytes. Fixed in the fork before tar could reach anyone's PATH.
  `07ed79c6e`.

native-links.sh needed no edit: its PROBED list already anticipated "tar and
gzip need zlib, curl and wget need libcurl" and runs each applet once to ask.
The link count went from 106 to 112 on its own.

**Verified on device** (ipp4-dev-arm64, an M4 iPad, 2026-08-20): tar, gzip,
gunzip, zcat, curl, wget and `md <url>` all work in the app, which settles the
`libz.tbd` link -- a build that had missed it could not have launched.

**Also confirmed by a headless build, 2026-08-20**, and the earlier claim here
that one was impossible was wrong. The project builds fine from the command
line; it has to be driven by SCHEME, not by target:

    xcodebuild -project iSH-AOK.xcodeproj -scheme iSH -configuration Release \
        -sdk iphonesimulator ARCHS=arm64 CODE_SIGNING_ALLOWED=NO build

`-target` disables the implicit dependency resolution that makes the Meson and
Ninja targets run before anything links against their archives, which is why
`libiSH-AOKApp` appeared to resolve SDKROOT to macOS and `iSH-AOK.FileProvider`
appeared to link `-lish_emu` too early. Both were artefacts of the wrong flag,
not project defects: iSH, ish-cli, iSH+Linux and Screenshots are all shared
schemes and 16 target dependencies are declared. The resulting app binary links
/usr/lib/libz.1.dylib with `_deflate`/`_inflate` bound to it.

**The device found one thing the CLI could not**: plain HTTP to a public host
failed in the app while HTTPS and localhost worked -- App Transport Security's
exact signature, confirmed by serving plain HTTP from the guest itself and
watching that succeed while `http://example.com` failed and the emulated curl
got 200 from the same device. app/Info.plist declared NSAllowsArbitraryLoads
AND NSAllowsLocalNetworking, and the presence of the latter makes the former be
ignored on iOS 10 and later. Removing it is what makes the declaration apply:
confirmed on the rebuilt device, where three plain-HTTP hosts now fetch, HTTPS
is unaffected, and localhost still works -- so nothing was lost with the key.
The shim also now reports the framework's own wording instead of mapping every
unrecognised NSError to "Failure when receiving data from the peer", which is
what made this look like a network fault for as long as it did.

### Terminal cell height -- FIXED and SEEN 2026-08-19

At font sizes under 16 a background highlight sat 1-2px proud of the Powerline
separator beside it. hterm sizes a cell as
`fontBoundingBoxAscent + fontBoundingBoxDescent` -- the font's MAXIMUM extent,
with room for accents and deep descenders a block glyph never uses. The
background fills that cell; `U+2588` only rises to about the em height, and the
leftover is the band.

Fixed with a `line-height` MULTIPLIER (not the reporter's second suggestion, a
baseline offset, which cannot fix a glyph shorter than its cell -- it only moves
the band from the top to the bottom). A multiplier rather than a formula because
how much of the em a patched font's blocks cover varies between Nerd Font
patches. Default 1 is the measured height, so nothing moves until someone asks.
Settable at `/proc/ish/defaults/line_height` and from a Line Height row under
Font Size in Appearance.

**Seen, on an iPhone 17 Pro simulator, at font size 12** -- five rows of `U+2588`
on a red background, three rows of blue background-only, and a line each of
descenders/accents and CJK:

| line-height | block-glyph band | `Agjpqy ÀÉÎÕÜ` and CJK |
|---|---|---|
| 1.00 (default) | thick red band between every row | intact |
| 0.85 | much thinner | intact |
| 0.75 | thinner still | **descenders clipping** |

So the useful range for that font is about 0.85-0.95, the band narrows rather
than vanishing, and clipping starts below ~0.8 -- which is exactly why this is a
knob with bounds and not a computed value. Background-only rows tiled solid at
every setting, confirming the diagnosis that it is the glyph and not the cell.

**What actually took the time, and the real bug behind it.** The JS was correct
from the start and had no effect for three builds, because
`app/terminal/term.html` loads `hterm/dist/js/hterm_all.js` -- a bundle that is
gitignored and that NOTHING in the build produced. The phase named "Compile
JavaScript" only asserted the file existed. Fixed in `61fc0f59a`; see that
commit for why the regeneration lives in `xcode-meson.sh` and not in the phase.

### SmallCLUE `dmesg` said it was unsupported -- FIXED 2026-08-19

The implementation was there all along, behind `#if defined(__linux__)`, and a
native program is compiled for the HOST -- so the test was false even though the
kernel the call would reach is Linux, and AOK's own. The same guest's
`/usr/bin/dmesg` printed the boot line perfectly while this said it could not.
The platform test was asking about the compiler's target when what matters is
which kernel answers.

`nlibc_klogctl` issues the guest's `syslog(2)`; `deps/smallclue` gained
`SMALLCLUE_HAVE_KLOGCTL` so the existing branch compiles; and dmesg left
`native-links.sh`'s EXCLUDED list, which now skips 27 applets rather than 28.
Output verified byte-identical to the oracle -- the same guest's `/bin/dmesg` --
for both `dmesg` and `dmesg -T`. `42c1536da`.

### eudev's "sysfs not mounted", and the 30-second sleep behind it -- FIXED 2026-08-19

`/sys/class` did not exist at all. Fixed together with the block-device naming
below, because they need the same thing: `/sys/class/block/sda`.

Supplying `/sys/class` alone would have made boot **worse**. `log_end_msg 1`
does not exit, so the script runs on to a guard that checks
`[ -e /sys/block -a ! -e /sys/class/block ]` and sleeps 30 seconds; an empty
`/sys/class` turns a cosmetic warning into half a minute on every boot. Both
arrive together and both guards now pass.

**The open question answered itself.** Run against a real Devuan root with eudev
actually installed, the init script no longer complains about sysfs, does not
sleep, and stops at its NEXT guard with "eudev does not support containers,
udevd not started ... (warning)", exit 0, in under a second. eudev decides
correctly on its own. On a root where that container check passes by accident
(the TODO's note about `[elogind-daemon]` being the one bracketed process), the
30-second guard is now passed too, so both paths are covered. `4fb8c0768`.

### btop's empty disk, net and io panels -- FIXED 2026-08-19 (in two goes)

**The first attempt fixed the wrong thing, and this is why.** The entry said
btop matches mounts to diskstats entries by device name, so the two files were
made to agree on `sda`. Verified: the invariant held, in every file, on all four
guest arches. Then it was installed on an iPad and both panels were still empty
-- because that invariant was never what btop reads.

Reading btop's own binary settles it in one command:

    strings /usr/bin/btop | grep -E "^/(proc|sys|etc)/"
    /etc/fstab          /etc/mtab          /sys/block/{}/stat
    /sys/class/net/     /statistics/       ...

`/proc/net/dev` does not appear ANYWHERE in it, and `/proc/diskstats` does not
either. Two separate causes, neither of them column alignment:

- **Disks come from `/etc/fstab`.** btop's `use_fstab` defaults to true, so its
  whole disk list is whatever fstab declares -- and a rootfs tarball has never
  heard of AOK's root. Alpine's ships `noauto` lines for a CD-ROM and a USB
  stick and nothing else. Measured both ways: with `use_fstab = false` the panel
  fills in; with the default, blank. `ensure_root_fstab_entry()` (kernel/init.c,
  called from both the CLI and the app at boot, like the /dev repair beside it)
  adds a root line when nothing declares "/", and only then.
- **Counters come from `/sys/class/net/<iface>/statistics/`.** AOK had no
  `/sys/class/net` at all, so every interface read zero -- which is exactly the
  "shows no traffic" seen on the iPad's tailscale interface. Now present, with
  statistics plus address/mtu/flags/operstate/carrier/type, built from the same
  snapshot `/proc/net/dev` uses so the two cannot disagree.

With btop's stock config, unmodified: disks show 926 GiB at 32% used, and the
net panel reads 2.88 MiB/s down / 124 KiB/s up against real traffic.

**The lesson, since it cost a round trip:** the invariant was verified and the
outcome was not. Running btop once would have caught it in a minute, and reading
its binary for the paths it opens would have caught it before any code changed.

**The name fix was still worth doing**, for everything that DOES read those
files. `/proc/diskstats` named "disk1" and `/proc/mounts` named the root
"alpine-arm64-test", so nothing tied a mount to a device. "disk1" was the host's name for a Mac
disk -- a host detail leaking into a guest -- and it contradicted the major 8,
minor 0 printed beside it, which IS sda in Linux's numbering.

The device is `sda` now, defined once in `fs/real.h` and used by
`/proc/diskstats`, `/sys/block` and `/sys/class/block`, and `/` reports
`/dev/sda`. That replaces a deliberate choice worth naming: `mount_root` used to
report the root's directory name there so df would not print the host path.
`/dev/sda` is the Linux answer and the one that makes tools work; busybox df
prints it correctly. `4fb8c0768`.

### Uptime and btime described the app process -- FIXED 2026-08-19

The decision the entry asked for: **the machine is the guest**. `boot_time` is
now set where pid 1 is created, the only event in AOK that means what a boot
means, instead of once per app process. `run_at_boot` still seeds it so nothing
reading the clock before init exists sees zero.

Two more found there. `sysinfo(2)` reported uptime in the wrong unit --
`uptime_ticks` is 100 Hz and `kernel/uname.c` handed it to a field measured in
SECONDS, so a 12-second-old guest read as "up 20 min" through busybox uptime and
anything else on `sysinfo(2)`. And the two platforms disagreed about that unit,
which is how it survived: darwin produced ticks, linux produced seconds taken
straight from the HOST's `sysinfo()`, a different and much older machine. Both
produce ticks from the guest's boot now. Measured after a `sleep(15)`: sysinfo
16 s, `/proc/uptime` 16.0 s.

Also gone: darwin's `get_uptime()` read `kern.boottime` into a local and never
used it -- and had it been used it would have reported when the DEVICE last
booted. `f23d92bdc`.

Confirmed end to end on the Linux build, where the old behaviour was worst: on a
host 685417 seconds into its uptime, a guest that had slept 6 seconds reports
`/proc/uptime` 7.0 and a btime 7 seconds back. GCC and clang both build it
clean; `platform/linux.c` needed a `<time.h>` it had been getting from nobody
(`d672344af`).

### The `fflush(NULL)`-adjacent socket bugs -- chronyd's spin -- FIXED 2026-08-19

Two bugs, one recorded and one not.

**The recorded one.** iOS kills connected sockets when the device sleeps, reads
return ENOTCONN, and `sock_translate_err` maps that to ECONNRESET -- on every
call, for ever, because the host keeps answering ENOTCONN while `sock_poll` went
on reporting the fd readable. The translation now records that the connection is
finished; after that, reads report end-of-file and poll reports
`POLL_ERR|POLL_HUP`.

**Correction to the plan that was written here.** `POLL_ERR|POLL_HUP` alone
would NOT have stopped it. `kernel/poll.c`'s `SELECT_READ` counts both as
readable, matching Linux, so a select-based loop like chronyd's still wakes. It
is the read returning EOF that ends the loop; both halves are needed.

**The unrecorded one, and the one that could be measured.** A TCP peer resetting
with SO_LINGER 0 makes `recv()` report ECONNRESET on the macOS host and on Linux
6.12 alike. An AOK guest agreed -- until it called `poll()` first, after which
the same `recv()` returned 0 bytes. AOK reads the host's SO_ERROR itself
(`socket_tcp_connect_write_ready`, on every `sock_poll` of a stream socket) and
SO_ERROR is read-and-clear, so AOK's own poll consumed the pending error and the
guest's read saw a clean end-of-file. The stash that already existed for this
was consulted only by `getsockopt(SO_ERROR)`; `read`, `recvfrom` and `recvmsg`
consult it now too.

**The secondary item in the old entry did not reproduce.** It said a connected
UDP socket on ICMP port-unreachable showed ECONNRESET where the host and Linux
show ECONNREFUSED. AOK reports ECONNREFUSED correctly. What it does do is lose
the error entirely about a third of the time -- filed above, on its own.

Guarded by `tests/manual/sock_conn_error.c`, which passes on real Linux 6.12 as
well as on AOK -- re-run against the oracle after it came back online, together
with `proc_field_layout`. `31261988b`.

### i386 `fakefs_type_race` killed the CLI build -- FIXED 2026-08-19

**The recorded bisect was measuring the wrong thing.** The entry said
`ISH_I386_NOCHAIN=1` made it pass and `ISH_I386_NOBACKCHAIN=1` did not, and
concluded "forward-edge block chaining". But `i386_jit_chaining_enabled()` is a
C-side gate on the *linking loop* only -- it does not touch a single line of the
assembly that writes `frame->last_block` -- and it appears in the crashing
expression itself:

```c
if (last_block != NULL && i386_jit_chaining_enabled() &&
        (last_block->jump_ip[0] != NULL || ...     // <- jit.c, the faulting line
```

`&&` short-circuits, so `NOCHAIN=1` was not preventing the corruption, it was
skipping the DEREFERENCE. Instrumenting `last_block` proved it: with
`NOCHAIN=1`, and the test "passing", the pointer was still being corrupted
1-3 times per run. Chaining was never involved.

**What it actually was.** `frame->last_block` was `0x4`, and `jump_ip` is at
offset `0x18`, giving the reported fault address `0x1c`. 4 and 8 are `4 + imm`
from `RET_NEAR(imm)` -- a **ret** gadget's pop count. The `ret` gadget's
return-cache path reads its candidate block pointer from the offset where a
**call** gadget keeps one, so the cache entry was pointing into a block that had
been freed and whose memory had been reused by a block with a ret gadget there.

The dangling entry survives because of a hole between the two staleness guards:

- `jit_entry_scratch_get()` purges `cache` and `ret_cache` when
  `jit_block_free_generation` has moved -- but it is called **before** the
  caller takes `jetsam_lock`, so it samples the counter too early;
- the frontend loop purges when `cleanup_seq` has moved -- but it seeds
  `last_block_cleanup_seq` **after** the lock, so a bump inside the window is
  already included and never seen.

A `jit_free_jetsam()` pass landing between the two reads is therefore invisible
to both, and the thread runs the whole entry with a `ret_cache` full of pointers
into freed blocks. Fixed by `jit_entry_scratch_refresh()`, called with the read
lock held -- at which point no free can be in flight, so the generation it reads
holds for the entry. One relaxed load when nothing was freed; no measurable
cost on a syscall-heavy or a find-heavy guest benchmark.

**All four frontends had it**, not just i386 -- the same three lines appear in
the arm64, riscv64 and amd64 entry paths, and all four are fixed. i386 is
simply where a test hit it: 3/3 crashes before, 6/6 clean after, and 9/9 across
`default` / `NOCHAIN` / `NOBACKCHAIN` with the instrumentation still in and
reporting zero corruption events.

`ISH_I386_NOCHAIN=1` is no longer needed as a workaround, and would not have
been a real one -- it left the corrupt pointer in place.

**Regression gate:** `python3 tests/remote/conductor.py tier0`, then
`./build/ish -f tests/remote/.work/tier0fs-i386 /bin/fakefs_type_race`. Capture
the status directly; piping to `tail` reports tail's status and hides a crash.

### `/proc/net/dev` printed nine columns a side -- FIXED 2026-08-19

`proc_show_dev()` passed eighteen arguments to a format string with sixteen
conversions: Linux folds four counters into its single `frame` column and four
more into `carrier`, and the port had given each half of those sums an argument
of its own. printf dropped the last two and every transmit column sat one place
left of its header -- multicast printed as tx_bytes, rx_bytes as tx_packets.
btop's network panel was empty and ifconfig showed a loopback that had received
259 MiB and sent 33 kB. Fixed in `104e5ff4d`.

**Why it shipped, and what else it was hiding.** `proc_printf()` carried no
`format` attribute, so no compiler ever checked a single procfs format string.
It has one now, and it immediately found three more:

- `/proc/<pid>/stat` printed six `addr_t` values through `%lu`. `addr_t` is 32
  bits, so the conversion read eight bytes where four were passed -- correct
  today on Darwin arm64 only because the adjacent stack happens to be zero.
- `/proc/<pid>/status` rendered the signal masks as `%08x` from a 64-bit
  `sigset_t_`; Linux's `render_sigset_t()` always emits all sixteen hex digits.
- `/proc/consoles` passed `console_major` twice and dropped `console_minor`.

**Guarded by `tests/manual/proc_field_layout.c`.** Counting fields does NOT
catch the `/proc/net/dev` bug -- the line still had sixteen of them -- so the
test pushes a megabyte through loopback and requires the column the header
calls tx_bytes to move by at least that much. Against the pre-fix kernel it
reports tx_bytes moving by 0 and tx_packets by 1054720, which is rx_bytes,
exactly one place over. Passes on real Linux (Debian 13, 6.12) too.

Not simplified, deliberately: the literal space after the name colon. It is
what Linux itself writes, not a workaround, and without it an 8-digit rx_bytes
glues onto the interface name and busybox ifconfig loses the device. The test
asserts it so nobody "tidies" it away.

### Lingering `ish` processes: the `fflush(NULL)` deadlock -- FIXED 2026-08-19

The entry that used to sit under *Diagnosed, not fixed* had the stack right and
the mechanism half right, and the fix it proposed would have traded the hang for
silent data loss. Recorded here because of that.

**What was actually observed**, with `lldb` on two live wedged processes rather
than inferred:

- The blocked frame is `cli_halt -> _fwalk -> sflush_locked -> flockfile ->
  __psynch_mutexwait`. `_fwalk` is `fflush(NULL)` walking every host stream.
- The stream it blocks on is **one of ours**: `_file = -1`, `_write =
  nlibc_file_write`, cookie = guest fd 1. That is the native-libc shim's
  `funopen()` wrapper for a native program's stdout (`nlibc_file_wrap`), living
  in libc's `usual[]` pool -- NOT `stdout`, and not some stream a native program
  opened for itself.
- The mutex's recorded owner tid was absent from the process's own live thread
  list, in both processes. Both had exactly two threads left, neither of them
  the owner.

So: a native program is host code on a guest task's thread. A task killed before
`nlibc_flush_std()` runs leaves its wrapped stdout live in libc's pool, and a
task killed INSIDE stdio leaves that stream's lock held. Darwin does not release
a mutex when its owner thread dies, so `fflush(NULL)` waits for a thread that no
longer exists. Confirmed independently by a 30-line host probe with no AOK in
it: a thread that exits holding a `flockfile` makes a later `fflush(NULL)` hang
for ever, and `ftrylockfile` refuses that lock rather than blocking on it.

**Why the proposed fix was wrong.** "Flush `stdout` and `stderr` by name" would
have stopped the hang, but `stdout` is `__sF[1]` -- not where a native program's
pending output is. The shim's wrappers are, and skipping them loses the tail of
every native program's output. What landed instead: flush the same set of
streams, each guarded by `ftrylockfile()`, and skip any whose lock cannot be
taken. A stream nobody can lock has nothing recoverable in it anyway. Verified
A/B on the real algorithm -- old hangs, new returns and still writes the owned
stream's bytes -- and by 360 runs of the reproducer under self-contention with
zero survivors.

**The same landmine was inside native programs, and that one reached users.**
`fflush` was on `check-native-libc.py`'s PURE list, justified as "every stream a
native program holds is one the shim made". True of `fflush(f)`, and not an
argument about `fflush(NULL)` at all -- that one flushes EVERY stream in the
process, meaning every other concurrently-running native program's stdout and
stderr as well, and it blocks on any lock a departed task's thread still holds.
smallclue's shell alone calls it 20 times, several around fork and pipeline
teardown. This is the third instance of that exact error shape, after `fileno`
and `getopt`: premise right, conclusion backwards. `fflush` is now routed to
`nlibc_fflush`, which reads NULL as "the streams this program owns" -- the
registry gained a per-thread owner tag to answer that -- and flushes them
without waiting. Unlike the `cli_halt` half, this one was reachable in the iOS
app, where it would have hung a user's shell rather than a test process.

`fclose` came with it, for a different reason. It was on PURE too, and closing a
stream really does reach nothing on the host -- but it has to drop the stream
from the shim's own registry, and leaving it to the host left a stale entry
behind for every `fopen`/`fclose` pair a native program ever made. Only
`nlibc_flush_std`'s teardown forgot one. The registry is keyed by `FILE*` and
libc reissues the same slot, so a later stream inherited the dead entry's answer
to `fileno()` -- the hazard `nlibc_stream_forget`'s own comment describes, with
nothing closing the loop. Now routed to `nlibc_fclose`, and `nlibc_freopen` and
`nlibc_pclose` go through it too.

Also fixed in passing: refreshing the archives the gate reads (`ninja ish` does
not, which is its own trap) exposed a real pre-existing failure --
`deps/zsh/Src/aok_fork.c` calls `pthread_get_stackaddr_np` and
`pthread_get_stacksize_np`. Those ask about the calling HOST thread's own stack,
which is the only correct answer for a stack-overflow guard, and the shim's own
`nlibc_stack_exhausted()` asks the same two questions. Added to PURE with that
reason; the gate is clean again, at 236 host symbols.

---

## Closed during the 549 cycle

Kept only because the entry was wrong in a way worth remembering.

### tmpfs asserts that can abort the whole app -- FIXED 2026-08-19

This entry said the remaining sites were "now unreachable via mknod". That was
wrong, and the cost of being wrong was high: `11edc1843` mapped only the type-0
case, so any OTHER invalid `S_IFMT` -- `0x3000`, say -- walked through
`generic_mknodat`, which rejects only DIR and LNK and gates only BLK and CHR on
superuser. The result was a tmpfs inode of no type at all, and `read`, `pread`,
`pwrite` and `ftruncate` each hit an assert and aborted the WHOLE app. Three
lines of C, no privilege required, on any mounted tmpfs. Reproduced, then fixed:

- `kernel/fs.c` now whitelists the five types Linux's `may_mknod` accepts and
  returns EINVAL otherwise, so such an inode cannot be created in the first
  place;
- the five reachable asserts in `fs/tmp.c` became errno returns anyway, because
  an assert reachable from a syscall argument is the wrong tool.

The one assert left (`tmpfs_init_regular_file`) is a creation-time invariant its
only caller satisfies by construction.

---

## Deferred on purpose

### External display / AirPlay -- GH #540

Work exists on the branch `worktree-external-display-540`:

    6156597ee app: mirror the Wayland display to an external display (GH #540)

**Deferred to a future release by the maintainer (2026-08-18): "the external
display work is flawed".** The commit is NOT merged and must not be swept into a
release by accident. Left on its branch deliberately.

---

## Native program candidates

Programs worth compiling in as native code (kernel/native.h), and the one
question that decides most of them.

**The dividing line is the shim, not the program.** kernel/native_libc.h works
by `#define`-ing libc names ahead of the system headers, so it redirects calls
in translation units AOK COMPILES. It does nothing to calls made from a
prebuilt dylib or from another toolchain's objects -- that is exactly what made
zlib's gz* family unusable until deps/smallclue-shim/zlib.h reimplemented it.
So candidates fall into two groups, and the second is a different project from
the first:

- **C sources AOK can compile itself.** bash, zsh, OpenSSH and nextvi are
  already here. Cost is the porting work the gate enumerates
  (`tools/check-native-libc.py --report <objects>`), which is finite and
  visible up front.
- **Anything built by a foreign toolchain** -- Rust, Go, or a vendored build
  system we do not drive. Their `open`/`read`/`write` resolve to the host's at
  link time and no `#define` reaches them.

  **Prototyped 2026-08-20, and it works with link flags alone.** Darwin's
  linker will alias an undefined import onto a symbol we define:

      clang -o prog shim.o foreign.o \
          -Wl,-alias,_nlibc_open,_open \
          -Wl,-alias,_nlibc_read,_read

  An object compiled with no knowledge of AOK then calls `nlibc_*` instead --
  verified against a control build of the same object, which read the host's
  real /etc/hosts while the aliased one read the stand-in guest VFS.
  `llvm-objcopy --redefine-sym` also works, including on static archives (what
  a Rust staticlib ships as), but it rewrites the objects and the alias needs
  nothing but the link line. Caveats and the one open decision are below.

### helix

Requested 2026-08-20. Modal editor, Rust, MPL-2.0 (confirm before any work --
the licence matters here the way GPLv3 does for bash, which is why
`-Dnative_bash` exists at all).

Second group, so it is behind the interposition question above. Beyond that:

- Rust std does its own syscalls, and helix does file I/O throughout -- there
  is no pure/impure split to exploit the way zlib's deflate/inflate allowed.
- LSP servers and formatters are spawned processes. `fork()` is ENOSYS for a
  native program, but exec/spawn works (see [[native-exec-standin]]), so this
  is probably not the blocker it first looks like.
- Tree-sitter grammars are built as loadable objects by default; a static
  grammar build would be needed.
- Size is tens of MB with grammars, against a binary that ships in an app.

AOK already has nextvi and micro native, so this is the "modern editor" slot
rather than a gap. **Next step** is the interposition prototype, not helix
itself -- pick the smallest Rust program that does one `open` and see whether
its objects can be made to call `nlibc_open`.

---

## Reported issues

### Bugs

| # | Title | Notes |
|---|---|---|
| [#482](https://github.com/emkey1/ish-AOK/issues/482) | Wayland applet does not resize properly | |
| [#485](https://github.com/emkey1/ish-AOK/issues/485) | Qt apps (Falkon) cannot connect to session bus | 6 comments |
| [#503](https://github.com/emkey1/ish-AOK/issues/503) | amd64: gdb next/step after a breakpoint crashes with SIGILL | ours |
| [#521](https://github.com/emkey1/ish-AOK/issues/521) | Buildroot `make` crashes on "checking for working sigaltstack" | body is a screenshot only |
| [#523](https://github.com/emkey1/ish-AOK/issues/523) | yay (AUR helper) fails on Arch ARM64 | **reported symptom does not reproduce** -- see *Diagnosed* above. Previously: **half fixed.** The crash was a poll.c fd use-after-free, fixed in `717e6d3d`. The *reported* symptom -- `yay -S pandoc-bin` dying with `context: signal: terminated` -- still reproduces and is not a crash: yay's Go runtime sends itself SIGTERM when its context is cancelled, most likely its own timeout firing because emulated syscalls are slower than its budget assumes. Not a re-test; a timeout question |
| [#527](https://github.com/emkey1/ish-AOK/issues/527) | pikaur fails on Arch ARM64 | blocked on `systemd-run` |
| [#541](https://github.com/emkey1/ish-AOK/issues/541) | ptraceomatic does not run: tracee reaped during setup | **fixed 2026-08-20** -- see *Closed during the 550 cycle* |
| [#542](https://github.com/emkey1/ish-AOK/issues/542) | JVM/HotSpot crashes on aarch64, "Field too big for insn" | reporter suspects upstream OpenJDK |
| -- | btop shows nothing in its disk, net and io sections | reported 2026-08-19; **fixed** -- see *Closed during the 550 cycle* |

### Feature requests

| # | Title |
|---|---|
| [#483](https://github.com/emkey1/ish-AOK/issues/483) | Fullscreen mode for the Wayland applet, dynamic resolution |
| [#484](https://github.com/emkey1/ish-AOK/issues/484) | 3D acceleration via virglrenderer |
| [#540](https://github.com/emkey1/ish-AOK/issues/540) | External display support (AirPlay) -- see *Deferred* above |
| [#556](https://github.com/emkey1/ish-AOK/issues/556) | Updated preset appearances |
| [#559](https://github.com/emkey1/ish-AOK/issues/559) | Feedback: own icons rather than iSH's, more OS images, QEMU |
| -- | Terminal cell height / line-height control -- see *Diagnosed* above |

---

## Build and test infrastructure

### Linux CI

**Green again as of 2026-08-19**, both arms of the `[clang, gcc]` matrix.

It had been red since 2026-08-10, which is BEFORE the 548 release -- `e4fe5116`,
the commit tagged 548, was itself red. Never a regression of the 549 cycle, and
it affected no shipped code: `build-mac` and `Build Dev IPA` were green
throughout.

Nearly all of it was one root cause: bash's, zsh's and OpenSSH's `config.h` are
each generated by running configure **on a Mac**, and the tree is compiled for
both platforms, so all three asserted Darwin facts that are false on glibc.
iconv lives inside libc on Linux; `<sys/sysctl.h>` and `<sys/filio.h>` are not
glibc headers; `st_atimespec`, `d_namlen` and `fpurge` are Darwin spellings;
`strtonum`, `timingsafe_bcmp`, `memset_s`, `<util.h>`, the `pw_class` family and
`sin_len` are BSD's. Every such macro is now behind `!__linux__`, so the shipping
build is bit-for-bit unmoved. The rest:

- `__thread` must FOLLOW the storage class for gcc -- 40 declarations, mostly in
  the vendored OpenSSH;
- `-D_GNU_SOURCE` project-wide, for `off64_t`, the `cookie_io` typedefs and
  `RUN_LVL`;
- the xattr port, which was a real port and not a config guard: Darwin's calls
  carry a position and an options word and Linux's do not, so the shim now
  declares the shape each platform actually has;
- `<rpc/types.h>`, which OpenSSH asks for and Debian hides in libtirpc -- one
  missing header accounted for 181 of the original 186 failures;
- the fused i386 ALU gadgets, which exist only in aarch64 assembly, so merely
  naming them was a link error on any x86_64 host;
- a duplicate `smallclueRunRsync`, which ld64 quietly tolerates and GNU ld does
  not.

One of the fixes was not a build fix at all. GCC rejected an assignment clang
waves through and turned up a live crash on iOS: `bash --rcfile FILE` and
`--init-file FILE` wrote through a NULL pointer, and native bash runs in-process,
so that is the app going down rather than a shell. See `deps/bash` `a097512`.

Verified by cloning the pushed branch fresh on Debian 13 and building it exactly
the way CI does, with each compiler: 0 failed targets, `float80` and
`riscv64_decode` pass, `e2e` passes.

### `time_conformance` fails only in a full-suite run

Seen 2026-08-20 in a full tier0 sweep: x86_64 reported `time_conformance: FAIL
failures=3`, and the same test passed three times out of three when run alone
immediately afterwards. It is a timing test, the full sweep loads the machine,
and nothing in that run touched clocks -- the only kernel change was
pidfd_open. Recorded rather than diagnosed: if it starts failing alone, it is a
real regression and this note is the date it was not one.

The conductor keeps no per-test log, so the three failing assertions were not
recoverable after the fact. Worth fixing if this recurs -- a failing test that
cannot say what it checked costs a re-run every time.

### Regression-suite observations

From the 4-arch on-device run, 2026-08-19 (aarch64 booted 118/118 clean; i386
110/6, x86_64 112/5, riscv64 103/5):

- **Five failures are identical on every chroot arm and absent from the booted
  arm**: `devtmpfs_mount`, `proc_pid_io`, `taskstats_genl`, `mount_stdev`,
  `fifo_open_creat_deadlock`.
- `mount_stdev` and `devtmpfs_mount` are proven chroot artifacts: they fail in
  an **aarch64** chroot, the same arch that passes them when booted. AOK has no
  mount namespaces, so `/proc` inside a chroot describes the booted root while
  `stat()` sees the chroot's.
- The other three pass when run **by hand** inside the same chroot, and failed
  when x86_64 ran **alone**, so it is the chroot plus the suite runner -- not
  contention and not architecture. Worth understanding before anyone reads them
  as product bugs.
- `mount-root.sh` bind-mounts `/AOK/tools` but not `/AOK/tests`, so
  `setup-regressions.sh` cannot find its sources inside a chroot without a
  manual bind. Small gap worth closing if this becomes routine.
