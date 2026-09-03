# build 554 musts

Work deliberately deferred out of 553, with the diagnosis already done so
nobody has to re-derive it. Each entry says what is **established**, what the
**next step** is, and how to **prove** it afterwards.

Started 2026-09-02, during the 553 release run. Supersedes
`docs/historical/build_553_musts.md`, whose atomics item is closed — see the *Closed in
553* section at the bottom for what actually turned out to be wrong there,
because the diagnosis in that file was materially incomplete.

---

## strace and gdb kill the process they attach to

**Established.** Attaching to a **thread** (not a thread-group leader) with
either tool fails, and takes the target with it. On the test iPad, `strace -c -p
623` on chronyd ran for six seconds and then the process was gone from `ps`; a
second attach reported `PTRACE_SEIZE: No such process`. `gdb -p 653` on
rsyslogd's `in:imuxsock` thread hit gdb's own internal assertion:

    linux-nat.c:1027: internal-error: linux_nat_post_attach_wait:
    Assertion `pid == new_pid' failed.

gdb attaches and then waits; it requires the wait to report the pid it attached
to. AOK reported a different one. That is very likely the same root cause as
`strace`'s: this kernel makes threads children of their **creator** rather than
of the leader's parent (see the note of that name in
`docs/historical/build_553_musts.md`-era work and `kernel/task.c`), so a wait after
attaching to a non-leader thread resolves to the wrong task.

This is why the CPU-spin hunt below had to be settled from `/proc/<pid>/io`
counters instead: the two tools that would normally answer it destroy the
evidence.

**Next step.** A test that attaches to a non-leader thread with `PTRACE_ATTACH`,
then `waitpid(-1, ..., __WALL)`, and requires the returned pid to be the thread
attached to -- and requires the thread to still be alive afterwards. Then follow
it into `do_wait`.

**Prove it.** `gdb -p <tid>` on a live multithreaded guest process prints a
backtrace and detaches with the process still running. `strace -p <tid>` for ten
seconds leaves the target alive.

---

## A NULL dereference in the ptrace memory-write path

**Established.** A local crash report from 2026-09-02 (`ish` CLI, SIGSEGV,
`KERN_INVALID_ADDRESS at 0x0`) with this stack:

    _platform_memmove
    mem_ptr
    __user_write_task_mem
    user_write_task_ptrace
    sys_ptrace_guest
    handle_asm_generic_native_syscall

`mem_ptr` returned NULL and the caller memmove'd through it. A ptrace poke at an
address the tracee has not mapped is an ordinary thing for a debugger to do and
must return EIO or EFAULT, never take the emulator down -- and it takes down the
whole app, not just the guest process, because every guest task is a thread of
one host process.

Not attributed to a specific reproducer: the report was found during the 553
release run, on a machine where more than one session was working, and the
faulting thread was named `pk-10`. The stack is unambiguous regardless of who
provoked it, and it is in a path 553 did not touch.

Probably the same area as the entry above -- both are ptrace, and a kernel that
resolves the wrong task for a thread is a plausible source of a NULL `mem_ptr`
for the task it then writes to.

**Next step.** Check every `mem_ptr` return in `user_write_task_ptrace` /
`__user_write_task_mem` for a NULL test, and make the syscall return EIO the way
Linux does for an unmapped `PTRACE_POKEDATA`.

**Prove it.** A test that attaches to a child, pokes an address the child has
not mapped, and requires EIO with both processes still alive.

---

## mem_mapped_page_count walks a page table without the lock

**Established.** It is called with no `mem->lock` held and reads
`entries[i].data` out of a leaf array that another thread can be freeing --
`proc_mem_count_pages` says the lock-free-ness is intentional, and for /proc it
is: a stale count costs nothing there.

553 found out what it costs elsewhere. `task_maxrss_kb` folds the same count
into a **high-water mark**, so one torn read becomes the permanent answer:
`ru_maxrss` reported 2,819,362,696 KB (2.7 TB) for a process whose real peak was
about 4 MB, on device, for every process in one shell's subtree.

553 fixed the two things that made that observable: a forked child no longer
inherits its parent's peak (`kernel/task.c`, and Linux is the reference -- the
oracle reports a child peak below its parent's), and the latch now refuses a
sample larger than **physical memory**. Note the second one took two attempts.
The first bounded by `mm->mem.page_limit`, which for a 64-bit guest is the whole
address space -- so it accepted everything, rejected nothing, and the next
device run reported 2.7 TB again. A resident set cannot exceed the memory that
exists; that is the bound that works (7.28 GB of device RAM is ~1.9M pages
against a bad sample of ~695M, over by 364x). Verified 12/12 under fork load on
device, where it had been failing every run.

**The unsafe walk itself is unchanged**, and the bound is not airtight: a
garbage sample that happens to land UNDER physical memory still latches.

**A second source of a bad latch was found and closed on 2026-09-02**, while
giving native programs their own address space. `mm_new` allocated `struct mm`
with `malloc` and assigned only some of it -- `rss_pages_hwm` was never
initialized on any path, so a fresh address space started its high-water mark at
whatever was in that heap word. Nothing corrects it afterwards, by design: the
latch only ever goes up. It stayed quiet because malloc mostly hands back fresh
zero pages, which is luck rather than a guarantee, and it means the 553
diagnosis (a torn read of a leaf array) was not necessarily the whole story for
the 2.7 TB report. `mm_new` uses `calloc` now, which also fixes the uninitialized
`vdso` and the argv/env/auxv/stack bounds procfs reports -- those had been
covered up by `elf_exec` overwriting them, which a native exec does not do.

**Next step.** Take `mem->lock` for read around the walk, or restructure it to
be genuinely safe against a concurrent leaf free (hazard pointer, RCU-ish
grace period, or simply not freeing leaves). The reason it was not done in 553:
the fault path already holds `mem->lock`, so adding an acquisition on the
getrusage/exit paths is a lock-ordering change, and the last hour of a release
is the wrong time for one.

**Prove it.** A thread hammering mmap/munmap while another reads
`/proc/<pid>/statm` and `getrusage` in a loop, with no impossible counts and no
sanitizer report, for long enough to have hit the old race many times over.

---

## exec_de_thread's native half fails ~2 runs in 3, on the glibc root only

**Established.** Measured 2026-09-03 while gating an unrelated change, so the
numbers are from a clean A/B rather than one bad run. On
`build/devuan-arm64-test`, `exec_de_thread` fails its last case --

    FAIL native exec takes over the leader's identity got=0 expected=1

-- on **both** the changed and unchanged binaries, interleaved run-by-run so
machine drift cannot land on one arm: 10 PASS / 22 FAIL against 13 PASS /
19 FAIL over 32 runs each. On `build/alpine-arm64-test` it is 10 PASS / 0 FAIL
on both. So it is real, it is frequent, and it is **glibc-specific** -- exactly
the gap the fifth gate leg exists to close (see the header of
`tools/run-guest-gate.sh`: four architectures of one libc let four CPU-clock
bugs ship in a single release).

The case is the native half: a four-thread process whose NON-leader thread execs
`/AOK/native/smallclue` as `cat`, after which `/proc/<pid>/comm` must become
`cat` within 500 polls at 20 ms -- ten seconds. Failure means the exec'ing
thread never took over the leader's identity in that budget. It is not a load
flake: it fails just as often on an idle machine as under `--parallel`, and the
musl root passes it 10/10 in the same conditions.

**Next step.** Decide first whether de_thread never happened or merely took
longer than ten seconds -- print `comm` and the thread list at the timeout
rather than lengthening the budget, since a slow de_thread and a missing one are
different bugs. Then follow the surviving threads: the leader is supposed to be
reaped and its identity assumed, and glibc's pthread teardown differs from
musl's in exactly the place that would show up here.

**Prove it.** 30 interleaved runs on `build/devuan-arm64-test` with no failure,
and the musl root still at 10/10.

---

## tty_hangup_signal failed once on device, under suite load

**Established.** It failed in the device suite run for 553 -- "still alive 6s
after the hangup" -- and then passed **3 runs out of 3** standalone on the same
device minutes later, and passes on all five local legs. The test gives the
hangup a 6-second budget and the device was running the rest of a 188-test suite
at the time.

Recorded rather than dismissed, because "passes alone, fails in the suite" is
NOT by itself proof of a load flake -- that exact shape was a real bug once
before (GH #542, `ptrace_group_stop`). If it recurs, A/B the suspected cause in
one binary before re-running anything.

---

## POLLHUP without POLLIN on a closed socket

**Established.** Noticed while fixing the idle-poll spin, not chased. On a unix
socketpair whose peer has closed, `poll(POLLIN)` returns `revents=0x10`
(POLLHUP alone) under AOK and `0x11` (POLLIN|POLLHUP) on Linux 6.12 -- measured
against the oracle by `tests/manual/poll_idle_cpu.c`, which accepts either
because it is testing something else.

Linux sets POLLIN as well because a closed socket **is** readable: a read
returns 0 for EOF. A program that waits for POLLIN before reading, and treats
POLLHUP as merely informational, will not read the EOF it is being told about.

**Next step.** Find where sock_poll composes the hangup result and add POLL_READ
alongside POLL_HUP for a peer-closed socket. Check the half-close case
separately -- `fs/sock.c` already has careful reasoning about EPOLLRDHUP vs
EPOLLHUP that must not be disturbed.

**Prove it.** A test asserting `revents == POLLIN|POLLHUP` after the peer
closes, checked against the oracle first.

---

## Native programs appear in top; the Launcher applets do not

**Established.** Programs under `/AOK/native` -- native `bash`, `zsh`,
`smallclue`, `motepad` -- **do** appear in `ps`, `top` and `ktop` with correct
state and correct `%CPU`. Measured in an arm64 guest: a native `bash` spinning
showed at 99.5%, a native `zsh` alongside it at 0.0%, both with the right
`COMMAND`. So the guest-process half of "native apps should show up in
top/htop/btop/ktop" already works.

Half of that was the `comm` column only. The ARGUMENTS column was the parent's,
because /proc/<pid>/cmdline was read out of an address space the native exec had
never replaced -- `ps` showed a backgrounded `/AOK/native/smallclue sleep 3` as
`{smallclue} /bin/sh -c ...`, the shell's entire command line. Fixed 2026-09-02
alongside the native address-space work: the exec records the program's own argv
on the task and procfs answers from it, so the same process now reads
`/AOK/native/smallclue sleep 3`.

What does not appear is the **Launcher applets** -- File Manager, MotePad, LLM
Chat, Markdown, Clock, Music, Settings, Wayland. Those are not guest processes
at all: they are iOS UI running in the app, with no pid, no `/proc` entry and no
guest address space.

**Next step.** This is a design decision, not a bug. Showing them means
synthesizing `/proc/<pid>` entries for app-side work -- inventing pids that no
guest syscall can act on, so `kill` on one has to mean something or be refused.
Worth doing only if the goal is "the user can see what the app is doing", in
which case a distinct presentation (a separate section, or an `ARCH` value that
reads `applet`) is more honest than pretending they are processes.

**Prove it.** Whatever is decided, `kill -9` on such an entry must do something
defensible and must not corrupt the process table.

---

## An iosfs mount made through the new mount API does not persist

Carried forward unchanged from `docs/historical/build_553_musts.md`; nothing about it
changed in 553. The short version: iosfs keys its security-scoped bookmark on
`mount->point` at mount time, and a mount made through
`fsopen`/`fsconfig`/`fsmount`/`move_mount` is created at a private staging path
(`/.ish-fsmount/<n>`) and relocated later, so the key is wrong. 552 stopped
persisting staging-path keys, because persisting them resurrected a permanent
phantom mount on every launch; the cost is that such a mount no longer survives
a relaunch.

**Next step.** Re-key at relocation. `mount_relocate` (`fs/mount.c`) has the
mount and both paths; an optional `relocated(mount, old_point)` in
`struct fs_ops` (`kernel/fs.h`) lets iosfs move the bookmark to the real path.
Call it after unlocking `mounts_lock`. Note the bookmark is not merely
*mis-keyed* at mount time, it is not stored at all, so iosfs also needs a
non-persisted side table keyed by the staging path to move from -- or it must
re-derive the bookmark, which needs the security-scoped URL it only holds during
`iosfs_mount`.

**Prove it.** Mount an iCloud directory with a util-linux `mount(8)` new enough
to use the new API, relaunch the app, and require the mount back at the path the
user asked for -- with nothing under `/.ish-fsmount/` in `/proc/mounts` either
before or after. This needs a device and an app relaunch, which is why it did
not ride along with 553's kernel work.

---

## `lock not` and `lock neg` are SIGILL on an i386 guest

**Established.** The i386 `LOCK` opcode table in `emu/decode.h` has entries for
the ALU pairs, the `80/81/83` immediate group, the `0F` atomics, and `FE/FF`
inc/dec -- and **no group-3 entry at all**. So `lock notl (mem)` and
`lock negl (mem)` fall through to `default: UNDEFINED` and kill the guest with
SIGILL. Real Linux runs both, on both `-m32` and `-m64` (checked on the oracle).
Found while writing `tests/manual/x86/atomic_lock_contended.c`, which skips
those two forms on i386 for exactly this reason and says so.

`lock xchg` was missing from the same table and **was** fixed in 553, because it
decodes to the existing `XCHG`, whose gadget is already a real `ldaxr`/`stlxr`
pair. `not`/`neg` are not that cheap: they need new entries in
`do_op_size_atomic` (`jit/gadgets-aarch64/math.S`), whose `.ifin` structure
makes it mechanical but which is shared by every i386 atomic, so a mistake there
breaks all of them.

**Next step.** Add `not` and `neg` to the `.irp` list in `do_op_size_atomic`:
`not` is `mvn` with **no** flag changes at all, `neg` is `0 - operand` with the
full sub flag rule. Then add `case 0xf6`/`0xf7` with a group-3 switch to the
LOCK table.

**Prove it.** Un-skip the two forms in `atomic_lock_contended` (drop the
`HAVE_64BIT_OPS` guard around them) and require the i386 leg to pass, plus the
whole i386 atomics set, since the gadget macro is shared.

---

## RLIMIT_STACK is not pushed down for a third party

Carried forward unchanged from `docs/historical/build_553_musts.md`. `prlimit64` against
another process updates that process's limits without updating its address
space, so a lowered `RLIMIT_STACK` takes effect at its next exec rather than
immediately. Deliberate: reading another task's `->mm` needs `general_lock`, and
the stack stays bounded by the guard gap meanwhile, so the failure mode is
"bounded less tightly than asked", never unbounded. Still only worth doing if
something real depends on it.

---

## The amd64 JIT still bridges locked instructions

**Established.** 553 made every locked instruction on both x86 guests correct,
by giving the interpreter and the one JIT helper that handles them real host
atomics. It did **not** do the other half of the 553 doc's proposal: the amd64
JIT still compiles a locked instruction into a bridge to a C helper, where the
i386 JIT compiles atomics into `ldaxr`/`stlxr` gadgets.

That is now a throughput question only, and a second-order one. The bridge costs
a gadget dispatch, a `set_rip`, a C call, and a re-walk of prefixes and modrm
that the translator already decoded -- call it tens of nanoseconds against the
~5ns of the atomic itself. What it no longer costs is the global lock, which was
the expensive part: an atomic-heavy amd64 loop measured 6.75s against i386's
1.65s before the fix.

**Next step.** Only if a profile says so. The shape is a gadget that computes
the effective address, does `amd64_vwrite_prep` to resolve a writable host
pointer, checks natural alignment, and runs an `ldaxr`/`stlxr` loop -- falling
back to the existing bridge when unaligned. `lock cmpxchg` and `lock xadd`
first; those are what glibc's atomics and condvar machinery emit.

**Prove it.** An uncontended mutex loop on amd64 against the same on i386,
before and after. The correctness tests must stay green throughout:
`atomic_lock_contended` is the one that can see a regression here.

---

## Found and closed in 554

### The x87 state instructions checked four bytes and moved up to 512

The i386 JIT reaches FXSAVE, FXRSTOR, FNSAVE, FRSTOR, FNSTENV and FLDENV
through the generic memory-helper gadget. That gadget runs a TLB check of a
width the front end chooses, then hands the helper a host pointer -- and
`jit/gen.c` chose the width from the **helper's name suffix**, `fpu_save32` /
`fpu_fxsave32`, which is the x87 mode, not the operand size. So every one of
the six got a four-byte check in front of a 28-, 108- or 512-byte access.

Four bytes is precisely the check that passes when the real one would not:

* a 512-byte FXSAVE at page offset `0xf00` passed, got a pointer good for the
  256 bytes left in the page, and wrote 512 -- through the second page's
  `P_WRITE` bit, through its copy-on-write sharing, and, when that page was the
  last one mapped, off the end of the host region. A guest running that
  instruction **took the host process down with SIGSEGV**, which is what makes
  this a memory-safety bug rather than an emulation-accuracy one;
* where the four-byte check *did* say "page-straddling", the gadget staged four
  bytes into `jit_frame`'s crosspage buffer, let the helper write 108 or 512
  bytes into a **32-byte** buffer -- over `last_block` and `chain_budget` -- and
  then flushed four of them back to the guest. A crosspage `fnsave` landed 4 of
  its 108 bytes; the other 104 went into the JIT's own frame.

The load forms carried a second bug on top: FLDENV and FRSTOR were emitted as
*writes*, so they demanded write permission on memory they only read. Measured
on the unfixed build: both raise SIGSEGV from a `PROT_READ` page that the amd64
engine loads from happily.

The fix is to name the true width at the call site (`h_read_bits`/`h_write_bits`
in `jit/gen.c`), add the three widths to the helper-gadget size list on **both**
hosts, and grow the crosspage staging buffer to 512 bytes so the widest access
it can be asked to stage actually fits. `tests/manual/x86/fpu_state_span.c`
holds both x86 guests to the same answers; on the unfixed build its first check
fails and its second kills the host.

The lesson is the same one the atomics item below records, in a different
register: **a size that is right for the helper is not automatically right for
the access**, and nothing about the old spelling looked wrong -- `h_write(fpu_save, z)`
reads as if `z` were the operand size, and for the fifteen other x87 memory
helpers it is.

### Related: bts/btr/btc on the x86_64 host used read_prep

Found in the same sweep, and separate: `jit/gadgets-x86_64/bits.S` resolved the
memory destination of `bts`, `btr` and `btc` (and their `lock` forms) through
the **read** TLB entry, so those instructions skipped the `P_WRITE` check and
the copy-on-write break, and had no `write_done` -- a page-straddling one
modified the staging buffer and dropped it. The aarch64 copy of the same macro
has always used `write_prep`/`write_done`; this is now a straight port of it.
x86_64-host only, so it affects the Linux CLI and CI and not iOS.

---

### A pending signal failed a write that could not have blocked

The "native bash intermittently reports EINTR from a write" item, carried into
554 unverified. It reproduced at about one run in eight as

    /tmp/g.sh: line 4: echo: write error: Interrupted system call

and the suspicion recorded at the time -- SIGCHLD interrupting a pipe write,
with `native_syscall_args` restarting only on the two `_ERESTART` codes -- was
right about the signal and wrong about where the fault was. Two independent
kernel defects, either of which is enough on its own.

**One: `fs/real.c` decided about signals before attempting the transfer.**
`realfs_write()` called `realfs_wait_writable()` first, and that wait answers
EINTR the moment a guest signal is pending -- before a single byte is offered to
the fd. So three bytes going into an *empty* 64 KiB pipe, a write that cannot
block, were failed outright because a SIGCHLD happened to be queued. Linux only
lets a signal cut short an operation that genuinely blocks and has transferred
nothing; anything it can complete, it completes. `realfs_read()` had the same
shape. The fix is to attempt first and wait only on `EAGAIN` -- the fd is
already forced non-blocking a few lines above, so the attempt costs nothing and
answers the only question that matters. It also takes a `poll()` off the fast
path of every blocking read and write.

**Two: a native program's `SA_RESTART` was unreachable, by construction.** The
shim cannot hand the kernel a host function pointer, so `nlibc_set_disposition`
blocks the signal and leaves `SIG_DFL` in `sighand->action` -- flags and all.
`signal_should_restart_syscall()` then had nothing true to read and answered
"do not restart" for every native program, always. Worse, the two halves
disagreed with each other by construction: the wait side asks
`task_wake_blocked()`, which *subtracts* the shim's held set so a held signal
counts as pending, while the restart side asked the raw `blocked` set, where a
held signal is invisible. Pending enough to interrupt; blocked enough not to
restart. The shim now records the program's real `sa_flags` (and reports them
back through `oact`, which it also used to drop), publishes the restarting
subset as `struct task`'s `native_restart`, and the predicate uses
`task_wake_blocked()` like everyone else and consults `native_restart` for a
signal the shim holds.

The placeholder is read in three places, and all three had to learn about it:
`signal_should_restart_syscall()`, its `_nohand` twin, and
`signal_note_interrupted()`, which latches the answer at delivery time for a
wait that was actually asleep. The last two matter because a shim-held signal
always runs a handler whatever the placeholder claims -- a native program's own
`SIGTSTP` handler reads as `SIGNAL_STOP` there, and would have parked a `poll()`
that Linux interrupts.

One more thing falls out, and it is a place a restart would be wrong rather
than merely absent: a restart is only meaningful if the handler runs first, and
inside a host stdio callback delivery is deliberately deferred
(`nlibc_stdio_defer_fatal`), so re-issuing there would meet the same pending
signal and spin. `native_syscall_args` returns the EINTR instead, and
`realfs_guest_signal_pending()` does not manufacture one in the first place for
a deferred signal the program marked `SA_RESTART`. `fs/sock.c` is the other I/O
path reachable from inside a native stdio callback and gets the same rule --
though notably it never had the *first* defect: the socket code has always run
the host call non-blocking and waited only on `EAGAIN`, which is exactly the
shape `fs/real.c` has now been given. realfs was the outlier, not the pattern.

That deferral is also why the window was never the microsecond race it looked
like. Every syscall a native program makes checkpoints on entry and drains
pending signals -- except inside stdio, and bash's `echo` flushes and then
checks `ferror` from exactly there. The signal sits pending across the whole
write. That made it reproducible on demand rather than one run in eight:

    bash -c 'trap : USR1; kill -USR1 $$; echo x; echo y' | cat

prints two `write error` lines on the unfixed build and `x`/`y` on the fixed
one. `tests/manual/native_signal_write_eintr.sh` holds all of it, including the
half that must NOT change: a bash trap carries no `SA_RESTART`, so a *blocked*
read interrupted by one still has to come back and let the trap run. Every case
in that file was checked against real bash on x86_64 Linux 6.12 first, and the
same three shapes agree with it now.

The lesson is the disagreement rather than either bug: **two predicates that
answer "is a signal pending?" differently will eventually meet**, and the one
that interrupts and the one that restarts are exactly the pair that must not.

### `rm -f` on a name that is not there answered EACCES

Found while running the guest suite on the device over ssh, which is the first
time it had been run as anything other than **uid 0**. The suite died at the
first cache miss with no diagnostic whatsoever -- 28 lines of build log and
then nothing. `sh -x` put the last command at `rm -f` inside `cache_store`, a
best-effort cache write into a root-owned `/AOK/fakefs/regress-cache`; under
`set -e`, a nonzero `rm -f` ends the script.

`rm -f` on a nonexistent file is not supposed to be able to fail. It failed
because AOK checked the parent directory's write bit during path normalization,
*ahead of* looking up the final component -- so a name that was not there came
back EACCES, and `rm -f` suppresses ENOENT and nothing else.

Linux decides this by ORDER rather than by policy. Resolution happens first, so
`do_unlinkat()`/`do_rmdir()` reject a negative dentry before `may_delete()` is
ever consulted. Measured on 6.12, as uid 1000 against a root-owned 0755 parent:

| | Linux | AOK before |
|---|---|---|
| `unlink`/`rmdir`/`rename` of a MISSING name | ENOENT | EACCES |
| `unlink`/`rmdir`/`rename` of a PRESENT name | EACCES | EACCES |
| `mkdir`/`symlink`/`open O_CREAT` of a MISSING name | EACCES | EACCES |
| `mkdir`/`symlink` of a PRESENT name | EEXIST | EEXIST |

The create half was already right: `N_CREATE_EEXIST_FIRST` exists precisely
because `mkdir -p` needs EEXIST to beat EACCES. The remove half is its mirror
and was simply never written, and the flag's own comment asserted the opposite
-- "not for unlink/rmdir/rename: there an existing target is the point of the
call" -- which holds only when the target exists. `N_REMOVE_ENOENT_FIRST` now
carries the other case, on unlink, rmdir and rename's SOURCE (not its
destination, where a missing name is the ordinary case and EACCES is right).

### A socket was owned by root, whoever made it

The same device run reported `utimensat_fd` failing on `futimens: Operation not
permitted` for a socket. That is not a privilege artefact: `adhoc_fd_create`
zeroes the whole statbuf and the socket paths never set `uid`/`gid`, so every
socket in the system was recorded as uid 0 -- and `futimens` with explicit times
requires ownership, so an unprivileged process could not set the times on a
socket it had created itself.

`fs/pipe.c` has always assigned `current->uid`; sockets were missed. Measured on
6.12 as uid 1000, and the split is not obvious enough to have guessed:

    socket    fstat_uid=1000  futimens=OK
    pipe      fstat_uid=1000  futimens=OK
    eventfd   fstat_uid=0     futimens=EPERM
    epoll     fstat_uid=0     futimens=EPERM
    timerfd   fstat_uid=0     futimens=EPERM
    signalfd  fstat_uid=0     futimens=EPERM

sockfs and pipefs give the inode the creator's uid; the anon_inode family
shares one root-owned inode and really does answer EPERM. So the fix is exactly
sockets, and the test now asserts the whole split rather than the one line.

### What actually hid both of these: the harness only ever runs as root

Neither bug is subtle, and both had been reachable for as long as the code has
existed. They survived because every local leg of the gate runs the CLI as
**uid 0**, where a parent directory is always writable and an ownership check
always passes -- the entire question is unreachable. It took an ssh session
into the app, which lands as uid 1000, to make either observable.

`tests/manual/fs_remove_enoent_order.c` and the new block in `utimensat_fd.c`
both **drop privileges themselves** rather than relying on how they are invoked,
so the local gate now covers this class. That is the general lesson and it is
worth more than either fix: a permission-ordering test that runs as root tests
nothing, and a suite that only ever runs as root will keep growing this blind
spot.

Five tests also *failed* rather than skipping when unprivileged (`kmsg_stream`,
`mountinfo_epollet`, `epoll_nested`, `tmpfs_exec`, plus `utimensat_fd`'s real
bug). They now skip on EPERM/EACCES and only when actually unprivileged, so a
non-root run reports honestly instead of five false failures that read like a
regression.

---

---

## Closed in 553

`docs/historical/build_553_musts.md`'s amd64-atomics item is done, but its diagnosis was
incomplete in a way worth recording, because the same mistake is easy to repeat:
it asserted that "every eligibility predicate in the amd64 JIT front-end
requires the lock prefix to be absent". Every predicate *it listed* did. The one
block that emits `amd64_jit_mem_op` -- the only implementation of
`<alu> [mem], reg` the JIT has -- does not, and never did, so that whole family
was JIT-compiled with the prefix silently discarded and was not atomic against
anything, not even other guest threads. The doc's framing ("slow, and does not
interlock with the kernel") understated it into a performance problem.

Three further holes the doc did not mention, all found by writing a contended
test rather than by reading: `xchg reg, [mem]` livelocked; `lock inc/dec` and
`lock <alu> [mem], imm` each have a second copy in the main interpreter that
never took the lock; and i386's `lock adc`/`lock sbb` read the carry flag inside
their compare-exchange retry loop, which the arithmetic overwrites.

The lesson, and the reason `atomic_lock_contended` exists: a lost update is the
only symptom a broken atomic has, and no single-threaded test can produce one.
Every existing x86 atomics test passed throughout all of the above.

`ru_maxrss` no longer latches a garbage sample and no longer hands it to every
descendant; what remains of that one is the unsafe walk above.

`docs/book` now ships at `/AOK/docs/book` -- 53 files, nested appendices and
all -- so that entry is closed too. It needed no Xcode project change after all:
the app builds through a Meson legacy target, so wiring meson.build and
fs/aok.c was the whole job. The risk that deferred it was imaginary.

The idle-poll spin closed in 553 has the same shape and the same lesson. Every
functional poll/select test passed while a blocking `poll()` on a quiet socket
burned 97% of a host core, because the guest-visible answer was correct -- it
returned 0 after exactly its timeout. Only the COST was wrong, and nothing in
the suite measured cost. `tests/manual/poll_idle_cpu.c` now does.
