# build 554 musts

Work deliberately deferred out of 553, with the diagnosis already done so
nobody has to re-derive it. Each entry says what is **established**, what the
**next step** is, and how to **prove** it afterwards.

Started 2026-09-02, during the 553 release run. Supersedes
`docs/build_553_musts.md`, whose atomics item is closed — see the *Closed in
553* section at the bottom for what actually turned out to be wrong there,
because the diagnosis in that file was materially incomplete.

---

## Two daemons burn 100% CPU each, spinning in userspace

**Established.** On the test iPad (Devuan arm64, glibc), `rsyslogd`'s
`in:imuxsock` thread and `chronyd` each accumulate CPU at ~102% indefinitely.
This is not a reporting artifact: sampled twice five seconds apart,
`/proc/<pid>/stat` utime+stime grew by 511 and 510 jiffies against 5 seconds of
wall clock. Two of eight cores pegged for the whole uptime, and it survives
across sessions.

They are spinning in **userspace**, not on a syscall: `strace -c -p 623` for six
seconds recorded **two** syscalls total, both `clock_gettime`. Both processes
are reported `S` (sleeping) throughout, which is consistent -- they are not
blocked in the kernel, they are running.

The `clock_gettime`-only trace is the interesting part, and chronyd is a *clock*
daemon. `strace`'s own `-c` summary reported nonsense for those two calls
(79610536 seconds across 2 calls), which is worth understanding rather than
dismissing: it may be strace's arithmetic on a bad time value, or it may be the
value the guest actually returned.

Attaching strace to `chronyd` (pid 623) **killed it** -- the subsequent
`PTRACE_SEIZE` reported *No such process* and the pid was gone from `ps`. That
is a second defect, and possibly the more serious one.

**Next step.** Reproduce locally rather than on the device: a Devuan arm64 root
with chrony installed, under the CLI harness, where a host-side profiler can see
which guest code is looping. Compare what `clock_gettime` returns to the oracle
for the same clock id -- the 552 cycle's four `CLOCK_PROCESS_CPUTIME_ID` bugs
were all glibc-only and all invisible on Alpine roots, and this is the same
shape (glibc device, musl test roots). Separately, reproduce the strace-kills-
the-tracee case, which should be a small ptrace test.

**Prove it.** An idle Devuan guest whose `chronyd` and `rsyslogd` sit at ~0%
CPU, and a ptrace test that attaches to a live process and detaches without
killing it.

---

## Native programs appear in top; the Launcher applets do not

**Established.** Programs under `/AOK/native` -- native `bash`, `zsh`,
`smallclue`, `motepad` -- **do** appear in `ps`, `top` and `ktop` with correct
state and correct `%CPU`. Measured in an arm64 guest: a native `bash` spinning
showed at 99.5%, a native `zsh` alongside it at 0.0%, both with the right
`COMMAND`. So the guest-process half of "native apps should show up in
top/htop/btop/ktop" already works.

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

## `docs/book` is not shipped at /AOK/docs

**Established.** The book is 45 files and ~630 KB of Markdown under
`docs/book/` (plus eight appendices, four of them generated). `/AOK/docs` ships
21 files from `opt/AOK/docs/` via `fs/aok-docs.manifest`, and every one of them
is present and correctly listed -- the docs set itself has no gaps.

`tools/gen-aokfs.py` already handles manifest entries with slashes in them, and
`fs/aok.c` synthesizes the intermediate directory nodes, so the *mechanism*
supports `book/ch01-....md` served at `/AOK/docs/book/`. What it needs is a
manifest whose source directory is `docs/book/` rather than `opt/AOK/docs/`,
which means a second `custom_target` and a second generated `.inc` -- and the
matching change in the Xcode project, which is the build that actually ships
(`meson`/`ninja` is a convenience).

**Next step.** Add `fs/aok-book.manifest` with `docs/book` as its source dir and
`/docs/book` as its dest prefix, wire the new `.inc` through `meson.build` and
`fs/aok.c`, then add it to `iSH-AOK.xcodeproj`. Deferred out of 553 only because
it touches the pbxproj mid-release; there is nothing hard in it.

**Prove it.** `ls /AOK/docs/book` on a device build lists the chapters, and
`wc -c` on one of them matches the repo file byte for byte.

---

## An iosfs mount made through the new mount API does not persist

Carried forward unchanged from `docs/build_553_musts.md`; nothing about it
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

Carried forward unchanged from `docs/build_553_musts.md`. `prlimit64` against
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

## Closed in 553

`docs/build_553_musts.md`'s amd64-atomics item is done, but its diagnosis was
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
